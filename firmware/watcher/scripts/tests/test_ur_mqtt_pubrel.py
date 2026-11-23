"""编译执行生产 PUBREL 收尾与包内读取预算，复现 QoS2 收尾缺失与 MQTT 流错位。

两个历史缺陷都发生在 `mqtt_client_common.c`，现场不可复现但成因明确：

- PUBREL 只打 "Packet type PUBREL is currently NOT handled!" 错误日志、不回 PUBCOMP，
  平台侧 QoS2 交付永远收不到终结，按重试节奏反复投递（真机可见 3~7 条连续日志）；
- Yield 窗口 500→50ms 后包内读取预算跟着缩到 ~2050ms，UDP 洪流造成 TCP 饥饿超时，
  HAL_TCP_Read 读完部分负载返回 -604，SDK 丢弃包内剩余字节，MQTT 流永久错位——错位后
  0x60~0x6F 的 JSON 文本字节恰好等于 MQTT 类型 6，被解析成 PUBREL 假包。

本文件从生产源码切出真实函数编译执行（仅替换发送出口、互斥、日志与定时器等宿主边界），
并内置负向对照：把最小窗口宏改回 Yield 窗口值必须复现 -604 失败。
"""

import re
import subprocess
import tempfile
import unittest
from pathlib import Path

WATCHER_ROOT = Path(__file__).resolve().parents[2]
SDK = WATCHER_ROOT / "components/unitedrhino_iot_c_sdk"
COMMON_SOURCE = (SDK / "sdk_src/mqtt_client_common.c").read_text(encoding="utf-8")
PUBLISH_SOURCE = (SDK / "sdk_src/mqtt_client_publish.c").read_text(encoding="utf-8")
MQTT_CLIENT_HEADER = (SDK / "sdk_src/internal_inc/mqtt_client.h").read_text(encoding="utf-8")
EXPORT_MQTT_HEADER = (SDK / "include/exports/unitedrhino_iot_export_mqtt.h").read_text(
    encoding="utf-8"
)
DEVICE_SOURCE = (WATCHER_ROOT / "main/unitedrhino/ur_device.c").read_text(encoding="utf-8")

# 契约下限：包内读取预算不得低于旧 500ms 窗口量级（与 MAX_REMAIN_WAIT 相加）。
MIN_PACKET_READ_WINDOW_FLOOR_MS = 500
# 复现真机场景：UDP 洪流把报文后半段的 TCP 交付推迟到 2.3 秒。
TAIL_STARVATION_MS = 2300


def _function(source, signature):
    """按签名切出完整函数体（含右花括号），供编译执行或语义位置断言。"""
    start = source.index(signature)
    end = source.index("\n}\n", start) + len("\n}\n")
    return source[start:end]


def _numeric_define(source, name):
    """读取生产宏的数值（兼容带括号写法），保证断言跟随实际配置。"""
    match = re.search(rf"^#define\s+{name}\s+\(?(\d+)\)?\b", source, re.M)
    if match is None:
        raise AssertionError(f"源码中未找到宏 {name}")
    return int(match.group(1))


class MqttPubrelTest(unittest.TestCase):
    """PUBCOMP 收尾与读取预算解耦，断言失败即代表现场缺陷回归。"""

    def _compile_and_run(self, harness, extra_flags=(), expect_success=True):
        """编译并运行夹具；负向对照要求进程必须失败。"""
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / "pubrel_case")
            built = subprocess.run(
                ["g++", "-std=c++17", "-x", "c++", "-", "-o", binary, *extra_flags],
                input=harness,
                text=True,
                capture_output=True,
            )
            self.assertEqual(built.returncode, 0, built.stderr)
            result = subprocess.run([binary], capture_output=True, text=True)
        if expect_success:
            self.assertEqual(result.returncode, 0, result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, "负向对照被编译执行后仍通过")
        return result

    def _pubrel_harness(self):
        """生产 _handle_pubrel_packet + 收发工具；发送出口替换为记录器。"""
        helpers = "".join(
            _function(source, signature)
            for source, signature in (
                (COMMON_SOURCE, "unsigned char mqtt_read_char("),
                (COMMON_SOURCE, "uint16_t mqtt_read_uint16_t("),
                (COMMON_SOURCE, "static int _decode_packet_rem_len_from_buf_read("),
                (COMMON_SOURCE, "int mqtt_read_packet_rem_len_form_buf("),
                (COMMON_SOURCE, "void mqtt_write_char("),
                (COMMON_SOURCE, "void mqtt_write_uint_16("),
                (COMMON_SOURCE, "size_t mqtt_write_packet_rem_len("),
                (COMMON_SOURCE, "int mqtt_init_packet_header("),
                (COMMON_SOURCE, "int deserialize_ack_packet("),
                (PUBLISH_SOURCE, "int serialize_pub_ack_packet("),
                (COMMON_SOURCE, "static int _handle_pubrel_packet("),
            )
        )
        packet_types = re.search(
            r"typedef enum msgTypes \{.*?\} MessageTypes;", MQTT_CLIENT_HEADER, re.S
        ).group(0)
        header_masks = "\n".join(
            re.findall(r"^#define MQTT_HEADER_\w+[^\n]*$", MQTT_CLIENT_HEADER, re.M)
        )
        qos_enum = re.search(r"typedef enum _QoS \{.*?\} QoS;", EXPORT_MQTT_HEADER, re.S).group(0)
        harness = r'''
// 生产 PUBREL 收尾路径：仅替换 Host 边界（发送出口、互斥、日志、定时器）。
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define IOT_FUNC_ENTRY ((void)0)
/* 与 SDK 一致：宏体是语句块，调用点允许不带分号。 */
#define IOT_FUNC_EXIT_RC(value) { return value; }
#define POINTER_SANITY_CHECK(pointer, rc) if (!(pointer)) return (rc)
#define Log_e(...) ((void)0)
#define Log_w(...) ((void)0)
#define Log_i(...) ((void)0)
#define HAL_MutexLock(lock) ((void)0)
#define HAL_MutexUnlock(lock) ((void)0)

#define UNITEDRHINO_RET_SUCCESS 0
#define UNITEDRHINO_ERR_INVAL -1002
#define UNITEDRHINO_ERR_FAILURE -1001
#define UNITEDRHINO_ERR_BUF_TOO_SHORT -119
#define UNITEDRHINO_ERR_MQTT_PACKET_READ -110
#define UNITEDRHINO_ERR_MQTT_UNKNOWN -104
#define MAX_NO_OF_REMAINING_LENGTH_BYTES 4

@@PACKET_TYPES@@
@@QOS_ENUM@@

struct Timer {};
static int expired(Timer*) { return 0; }
static void countdown_ms(Timer*, unsigned int) {}

#define UR_TEST_BUF_LEN 256
/* write_buf/read_buf 按生产结构（定长数组），write_buf_size 为 0 即"无写缓冲"。 */
struct UnitedRhino_IoT_Client {
    unsigned char read_buf[UR_TEST_BUF_LEN];
    size_t read_buf_size;
    unsigned char write_buf[UR_TEST_BUF_LEN];
    size_t write_buf_size;
    void* lock_write_buf;
};

static void reset_client(UnitedRhino_IoT_Client* client) {
    std::memset(client, 0, sizeof(*client));
    client->read_buf_size = UR_TEST_BUF_LEN;
    client->write_buf_size = UR_TEST_BUF_LEN;
}

static int sent_count = 0;
static uint32_t sent_len = 0;
static unsigned char sent_packet[UR_TEST_BUF_LEN];
static void reset_sent() {
    sent_count = 0;
    sent_len = 0;
    std::memset(sent_packet, 0, sizeof(sent_packet));
}
static int send_mqtt_packet(UnitedRhino_IoT_Client* client, uint32_t len, Timer*) {
    ++sent_count;
    sent_len = len;
    if (len <= UR_TEST_BUF_LEN) {
        std::memcpy(sent_packet, client->write_buf, len);
    }
    return 0;
}

static void check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::exit(1);
    }
}

@@HELPERS@@

int main() {
    Timer timer;

    /* 场景 a：合法 PUBREL（packet id 0x1234）必须写出 PUBCOMP 收尾。 */
    {
        UnitedRhino_IoT_Client client;
        reset_client(&client);
        client.read_buf[0] = 0x62; /* PUBREL(6)<<4 | QoS1 位 */
        client.read_buf[1] = 0x02; /* 剩余长度 2 */
        client.read_buf[2] = 0x12;
        client.read_buf[3] = 0x34;
        reset_sent();
        const int rc = _handle_pubrel_packet(&client, &timer);
        check(rc == UNITEDRHINO_RET_SUCCESS, "合法 PUBREL 必须返回成功");
        check(sent_count == 1, "合法 PUBREL 必须写出 PUBCOMP 回执");
        check(sent_len == 4, "PUBCOMP 报文长度必须是 4");
        check(sent_packet[0] == 0x70, "回执首字节必须是 PUBCOMP(0x70)");
        check(sent_packet[1] == 0x02, "PUBCOMP 剩余长度必须是 2");
        check(sent_packet[2] == 0x12 && sent_packet[3] == 0x34, "PUBCOMP 必须带回原 packet id");
    }

    /* 场景 b1：剩余长度放不下 packet id 的截断包——只告警，不写回执、不返回失败。 */
    {
        UnitedRhino_IoT_Client client;
        reset_client(&client);
        client.read_buf[0] = 0x62;
        client.read_buf[1] = 0x01;
        client.read_buf[2] = 0x12;
        reset_sent();
        const int rc = _handle_pubrel_packet(&client, &timer);
        check(rc == UNITEDRHINO_RET_SUCCESS, "截断 PUBREL 必须只告警不返回失败");
        check(sent_count == 0, "解析失败的 PUBREL 不得写出回执");
    }

    /* 场景 b2：读缓冲连固定头都不够（流错位现场形态）——同样安全忽略。 */
    {
        UnitedRhino_IoT_Client client;
        reset_client(&client);
        client.read_buf_size = 3;
        client.read_buf[0] = 0x62;
        client.read_buf[1] = 0x02;
        client.read_buf[2] = 0x12;
        reset_sent();
        const int rc = _handle_pubrel_packet(&client, &timer);
        check(rc == UNITEDRHINO_RET_SUCCESS, "读缓冲不足时不得返回失败");
        check(sent_count == 0, "读缓冲不足时不得写出回执");
    }

    /* 场景 b3：流错位后的假包（0x6C 'l' 被当成 PUBREL）——收尾确认不得把连接判死。 */
    {
        UnitedRhino_IoT_Client client;
        reset_client(&client);
        client.read_buf_size = 8;
        std::memcpy(client.read_buf, "lexer", 5);
        reset_sent();
        const int rc = _handle_pubrel_packet(&client, &timer);
        check(rc == UNITEDRHINO_RET_SUCCESS, "假 PUBREL 不得升级为连接级失败");
    }

    /* 场景 c：无写缓冲（write_buf_size=0）——序列化失败必须安全忽略且不崩。 */
    {
        UnitedRhino_IoT_Client client;
        reset_client(&client);
        client.write_buf_size = 0;
        client.read_buf[0] = 0x62;
        client.read_buf[1] = 0x02;
        client.read_buf[2] = 0x12;
        client.read_buf[3] = 0x34;
        reset_sent();
        const int rc = _handle_pubrel_packet(&client, &timer);
        check(rc == UNITEDRHINO_RET_SUCCESS, "无写缓冲时必须安全忽略");
        check(sent_count == 0, "无写缓冲时不得写出回执");
    }

    std::printf("ok: pubrel scenarios\n");
    return 0;
}
'''
        return harness.replace("@@PACKET_TYPES@@", header_masks + "\n" + packet_types).replace(
            "@@QOS_ENUM@@", qos_enum
        ).replace("@@HELPERS@@", helpers)

    def _read_budget_harness(self):
        """生产读包路径 + 网络读取层：模拟前半段可读、后半段 TCP 饥饿。"""
        yield_window_ms = _numeric_define(DEVICE_SOURCE, "UR_MQTT_YIELD_MS")
        min_window_ms = _numeric_define(
            COMMON_SOURCE, "UNITEDRHINO_IOT_MQTT_MIN_PACKET_READ_WINDOW_MS"
        )
        max_remain_wait_ms = _numeric_define(
            COMMON_SOURCE, "UNITEDRHINO_IOT_MQTT_MAX_REMAIN_WAIT_MS"
        )
        max_rem_len_bytes = _numeric_define(COMMON_SOURCE, "MAX_NO_OF_REMAINING_LENGTH_BYTES")
        max_half_packet_stall_ms = _numeric_define(
            COMMON_SOURCE, "UNITEDRHINO_IOT_MQTT_MAX_HALF_PACKET_STALL_MS"
        )
        helpers = "".join(
            _function(COMMON_SOURCE, signature)
            for signature in (
                "size_t mqtt_write_packet_rem_len(",
                "static uint32_t _packet_read_window_ms(",
                "static int _mark_rx_stream_broken(",
                "void reset_mqtt_rx_state(",
                "static int _read_rem_len_field(",
                "static int _read_payload_field(",
                "static int _read_mqtt_packet(",
            )
        )
        packet_types = re.search(
            r"typedef enum \{?[^}]*RESERVED[^}]*\} MessageTypes;", MQTT_CLIENT_HEADER
        ).group(0)
        rx_states = re.search(
            r"typedef enum \{[^}]*UR_MQTT_RX_IDLE[^}]*\} UrMqttRxState;", MQTT_CLIENT_HEADER
        ).group(0)
        harness = r'''
// 生产读包路径：仅替换网络层与时钟；宏值取自生产源码。
#include <cstdint>
#include <cstdio>
#include <cstring>

#define IOT_FUNC_ENTRY ((void)0)
/* 与 SDK 一致：宏体是语句块，调用点允许不带分号。 */
#define IOT_FUNC_EXIT_RC(value) { return value; }
#define POINTER_SANITY_CHECK(pointer, rc) if (!(pointer)) return (rc)
#define Log_e(...) ((void)0)
#define Log_w(...) ((void)0)
#define Log_i(...) ((void)0)

#define UNITEDRHINO_RET_SUCCESS 0
#define UNITEDRHINO_ERR_INVAL -1002
#define UNITEDRHINO_ERR_SSL_NOTHING_TO_READ -709
#define UNITEDRHINO_ERR_TCP_NOTHING_TO_READ -609
#define UNITEDRHINO_ERR_MQTT_NOTHING_TO_READ -109
#define UNITEDRHINO_ERR_FAILURE -1001
#define UNITEDRHINO_ERR_MQTT_PACKET_READ -110
#define UNITEDRHINO_ERR_BUF_TOO_SHORT -119
#define UNITEDRHINO_ERR_MQTT_RX_STREAM_DESYNC -123
#define MQTT_HEADER_TYPE_MASK 0xf0
#define MQTT_HEADER_TYPE_SHIFT 4

@@PACKET_TYPES@@
@@RX_STATES@@

#define UNITEDRHINO_IOT_MQTT_MAX_REMAIN_WAIT_MS (@@MAX_REMAIN_WAIT@@)
#define MAX_NO_OF_REMAINING_LENGTH_BYTES (@@MAX_REM_LEN_BYTES@@)
/* 最小窗口取自生产宏（占位值由测试注入）：断言跟随实际配置，不复制常量。 */
#define UNITEDRHINO_IOT_MQTT_MIN_PACKET_READ_WINDOW_MS (@@MIN_WINDOW@@)
#define UNITEDRHINO_IOT_MQTT_MAX_HALF_PACKET_STALL_MS (@@MAX_HALF_STALL@@)

/* 负向对照：把最小窗口改回 Yield 窗口值（真实回归形态）。
 * 旧实现下这会丢掉半个负载并永久错位；新实现必须仍然逐字节对齐（只是多读几轮）。 */
#ifdef TEST_FORCE_MIN_PACKET_READ_WINDOW_MS
#undef UNITEDRHINO_IOT_MQTT_MIN_PACKET_READ_WINDOW_MS
#define UNITEDRHINO_IOT_MQTT_MIN_PACKET_READ_WINDOW_MS (TEST_FORCE_MIN_PACKET_READ_WINDOW_MS)
#endif

constexpr int kYieldWindowMs = @@YIELD_WINDOW@@;
constexpr int kContractBudgetMs = @@CONTRACT_BUDGET@@;

struct Timer {};
static int left_ms(Timer*) { return kYieldWindowMs; }

/* 可控时钟：半包停滞判定依赖它，测试按需推进（每次读包调用 +25ms）。 */
static uint32_t g_now_ms = 500000;
static uint32_t HAL_GetTimeMs() { return g_now_ms; }

/* 模拟 HAL_TCP_Read：报文前半段已在内核缓冲，后半段在 starvation_ms 后才到达；
   预算覆盖不了饥饿时按已到达部分返回 -604（部分字节已写入缓冲，上层不得丢弃——
   旧实现正是在这里丢字节，让 MQTT 流永久错位）。 */
struct Net {
    static constexpr size_t kTotal = 42;       /* 1 头 + 1 剩余长度 + 40 负载 */
    static constexpr size_t kStallOffset = 22; /* 负载后半段起点 */
    unsigned char stream[kTotal];
    size_t offset = 0;
    int starvation_ms = @@STARVATION@@;
    int budgets[16] = {0};
    int budget_count = 0;
    bool tail_delivered = false;

    int read(Net* self, unsigned char* out, size_t n, int timeout_ms, size_t* read_len) {
        if (self->budget_count < 16) {
            self->budgets[self->budget_count++] = timeout_ms;
        }
        const size_t remaining = kTotal - self->offset;
        const size_t want = n < remaining ? n : remaining;
        const size_t deliverable =
            timeout_ms >= self->starvation_ms
                ? remaining
                : (self->offset < kStallOffset ? kStallOffset - self->offset : 0);
        const size_t give = want < deliverable ? want : deliverable;
        std::memcpy(out, self->stream + self->offset, give);
        self->offset += give;
        *read_len = give;
        if (self->offset > kStallOffset) {
            self->tail_delivered = true;
        }
        return give == n ? 0 : -604;
    }
};

struct UnitedRhino_IoT_Client {
    Net network_stack;
    unsigned char read_buf[64];
    size_t read_buf_size = sizeof(read_buf);
    uint8_t rx_state = 0;
    uint8_t rx_stream_broken = 0;
    uint8_t rx_rem_len_bytes = 0;
    uint8_t rx_payload_offset = 0;
    uint32_t rx_rem_len = 0;
    uint32_t rx_payload_read = 0;
    uint32_t rx_progress_ms = 0;
};

@@HELPERS@@

int main() {
    UnitedRhino_IoT_Client client;
    Net& net = client.network_stack;
    for (size_t i = 0; i < Net::kTotal; ++i) {
        net.stream[i] = static_cast<unsigned char>(0x40 + i);
    }
    net.stream[0] = 0x30; /* PUBLISH(3)<<4 */
    net.stream[1] = 40;   /* 剩余长度 = 负载长度 */

    Timer timer;
    uint8_t packet_type = 0;
    int rc = -1;
    int calls = 0;
    /* 续读契约：单次调用未读全时报错并在下一次调用继续，绝不丢弃已消费字节。 */
    while (calls < 32) {
        rc = _read_mqtt_packet(&client, &timer, &packet_type);
        ++calls;
        g_now_ms += 25;
        if (rc == UNITEDRHINO_RET_SUCCESS) {
            break;
        }
        if (calls == 8) {
            net.starvation_ms = 0; /* 洪流退去：剩余字节可立即交付 */
        }
    }
    if (rc != UNITEDRHINO_RET_SUCCESS) {
        std::fprintf(stderr, "FAIL: 包内读取最终返回 %d（错位或无法续读）\n", rc);
        return 1;
    }
    if (packet_type != 3) {
        std::fprintf(stderr, "FAIL: 读到完整报文后类型应为 PUBLISH，实际 %d\n", packet_type);
        return 1;
    }
    if (client.rx_stream_broken != 0) {
        std::fprintf(stderr, "FAIL: 饥饿导致的半包不得判定为流错位\n");
        return 1;
    }
    if (!net.tail_delivered) {
        std::fprintf(stderr, "FAIL: 报文后半段没有被读入\n");
        return 1;
    }
    if (net.offset != Net::kTotal) {
        std::fprintf(stderr, "FAIL: 报文消费字节数 %zu != %zu（多读或少读即包边界错位）\n",
                     net.offset, Net::kTotal);
        return 1;
    }
    if (std::memcmp(client.read_buf, net.stream, Net::kTotal) != 0) {
        std::fprintf(stderr, "FAIL: 读缓冲与完整报文不一致\n");
        return 1;
    }
    if (net.budget_count < 3) {
        std::fprintf(stderr, "FAIL: 包内读取点数量异常 %d\n", net.budget_count);
        return 1;
    }
    if (net.budgets[0] != kYieldWindowMs) {
        std::fprintf(stderr, "FAIL: 首字节读应按 Yield 窗口 %dms，实际 %dms\n", kYieldWindowMs,
                     net.budgets[0]);
        return 1;
    }
    std::printf("calls %d budgets", calls);
    for (int i = 0; i < net.budget_count; ++i) {
        std::printf(" %d", net.budgets[i]);
    }
    std::printf("\n");
    return 0;
}
'''
        harness = (
            harness.replace("@@MAX_REMAIN_WAIT@@", str(max_remain_wait_ms))
            .replace("@@MAX_REM_LEN_BYTES@@", str(max_rem_len_bytes))
            .replace("@@MIN_WINDOW@@", str(min_window_ms))
            .replace("@@MAX_HALF_STALL@@", str(max_half_packet_stall_ms))
            .replace("@@YIELD_WINDOW@@", str(yield_window_ms))
            .replace("@@CONTRACT_BUDGET@@", str(MIN_PACKET_READ_WINDOW_FLOOR_MS + max_remain_wait_ms))
            .replace("@@STARVATION@@", str(TAIL_STARVATION_MS))
            .replace("@@HELPERS@@", helpers)
            .replace("@@PACKET_TYPES@@", packet_types)
            .replace("@@RX_STATES@@", rx_states)
        )
        return harness, yield_window_ms, max_remain_wait_ms

    def test_pubrel_handler_replies_pubcomp_and_never_fails(self):
        """合法 PUBREL 回 PUBCOMP；截断/假包只告警返回成功，无写缓冲不崩。"""
        self._compile_and_run(self._pubrel_harness())

    def test_packet_read_budget_survives_tail_starvation(self):
        """Yield 窗口下报文后半段迟到 2.3 秒，包内预算仍必须按 ≥500ms 计并完整读完。"""
        harness, yield_window_ms, max_remain_wait_ms = self._read_budget_harness()
        result = self._compile_and_run(harness)
        budgets = [int(value) for value in result.stdout.split("budgets", 1)[1].split()]
        self.assertEqual(budgets[0], yield_window_ms, "首字节读仍走 Yield 窗口")
        for budget in budgets[1:]:
            self.assertGreaterEqual(
                budget, MIN_PACKET_READ_WINDOW_FLOOR_MS + max_remain_wait_ms
            )

    def test_window_reverted_to_yield_window_keeps_stream_aligned(self):
        """负向对照：预算被压回 Yield 窗口时，续读仍必须逐字节对齐（不得复现流错位）。"""
        harness, yield_window_ms, max_remain_wait_ms = self._read_budget_harness()
        result = self._compile_and_run(
            harness,
            extra_flags=[f"-DTEST_FORCE_MIN_PACKET_READ_WINDOW_MS={yield_window_ms}"],
        )
        budgets = [int(value) for value in result.stdout.split("budgets", 1)[1].split()]
        squeezed = min(budgets[1:])
        self.assertEqual(
            squeezed,
            yield_window_ms + max_remain_wait_ms,
            "负向对照必须真的把包内预算压回 Yield 窗口量级",
        )


if __name__ == "__main__":
    unittest.main()
