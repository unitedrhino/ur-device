"""回放生产 MQTT 读包路径，验证读包中途失败不会让字节流错位。

真机证据（TASK-143 R1 验收日志）：
- `/tmp/watcher-voice-turn-1792059228.log` 477471ms 出现 `MQTT yield transient error -604`，
  1.2 秒后开始 `deserialize_ack_packet failure! ack_code = 0x64/0x2d/0x37` 与
  `_read_string_with_len: stringLen exceed`，483441ms 命中
  `MQTT Recv buffer not enough: 16384 < 557312` → `MQTT yield failed: -119` → 会话被判死；
- `/tmp/watcher-media-recovery-1792062180.log` 181541ms 同样先 -604，183131ms 起错位解析，
  184891ms 以 `MQTT yield failed: -118`、`runtime failure: mqttDisconnected` 收尾。

成因是旧读包实现的三条"丢弃已消费字节后继续读"路径：负载部分读（-604）、剩余长度字段中途读失败、
按垃圾长度排空超缓冲报文。本文件从生产源码切出真实函数编译执行（仅替换网络层、时钟、日志等宿主
边界），用故障注入断言新契约：读包进度跨调用保留、读全才返回成功、不可恢复时置错位标志并停止解析。
"""

import re
import subprocess
import tempfile
import unittest
from pathlib import Path

WATCHER_ROOT = Path(__file__).resolve().parents[2]
SDK = WATCHER_ROOT / "components/unitedrhino_iot_c_sdk"
COMMON_SOURCE = (SDK / "sdk_src/mqtt_client_common.c").read_text(encoding="utf-8")
CONNECT_SOURCE = (SDK / "sdk_src/mqtt_client_connect.c").read_text(encoding="utf-8")
MQTT_CLIENT_HEADER = (SDK / "sdk_src/internal_inc/mqtt_client.h").read_text(encoding="utf-8")
DEVICE_SOURCE = (WATCHER_ROOT / "main/unitedrhino/ur_device.c").read_text(encoding="utf-8")

# 真机现场命中的垃圾长度字段（错位后按错误偏移解出的"剩余长度"）。
FIELD_GARBAGE_REM_LEN = 71911397
FIELD_GARBAGE_REM_LEN_SMALL = 557312

RX_STREAM_DESYNC = -123
BUF_TOO_SHORT = -119
MQTT_NOTHING_TO_READ = -109
TCP_NOTHING_TO_READ = -609
TCP_READ_TIMEOUT = -604

RX_IDLE = 0
RX_LENGTH = 1
RX_PAYLOAD = 2


def _function(source, signature):
    """按签名切出完整函数体（含右花括号），供编译执行。"""
    start = source.index(signature)
    end = source.index("\n}\n", start) + len("\n}\n")
    return source[start:end]


def _numeric_define(source, name):
    """读取生产宏的数值（兼容带括号写法），保证断言跟随实际配置。"""
    match = re.search(rf"^#define\s+{name}\s+\(?(\d+)\)?\b", source, re.M)
    if match is None:
        raise AssertionError(f"源码中未找到宏 {name}")
    return int(match.group(1))


class MqttRxStreamResyncTest(unittest.TestCase):
    """读包路径的续读与错位判定：失败注入下字节流必须保持对齐。"""

    def _compile_and_run(self, main_body):
        """编译生产读包路径 + 夹具 main，返回进程结果与读包函数源码。"""
        helper = "".join(
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
            r"typedef enum msgTypes \{.*?\} MessageTypes;", MQTT_CLIENT_HEADER, re.S
        ).group(0)
        rx_states = re.search(
            r"typedef enum \{[^}]*UR_MQTT_RX_IDLE[^}]*\} UrMqttRxState;", MQTT_CLIENT_HEADER
        ).group(0)
        header_masks = "\n".join(
            re.findall(r"^#define MQTT_HEADER_\w+[^\n]*$", MQTT_CLIENT_HEADER, re.M)
        )
        harness = r'''
// 生产 MQTT 读包路径：仅替换网络层、时钟、日志与内存等宿主边界。
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#define IOT_FUNC_ENTRY ((void)0)
/* 与 SDK 一致：宏体是语句块，调用点允许不带分号。 */
#define IOT_FUNC_EXIT_RC(value) { return value; }
#define POINTER_SANITY_CHECK(pointer, rc) if (!(pointer)) return (rc)
#define Log_e(...) ((void)0)
#define Log_w(...) ((void)0)
#define Log_i(...) ((void)0)

#define UNITEDRHINO_RET_SUCCESS 0
#define UNITEDRHINO_ERR_INVAL -1002
#define UNITEDRHINO_ERR_FAILURE -1001
#define UNITEDRHINO_ERR_MQTT_PACKET_READ -110
#define UNITEDRHINO_ERR_MQTT_NOTHING_TO_READ (@@MQTT_NOTHING@@)
#define UNITEDRHINO_ERR_TCP_NOTHING_TO_READ (@@TCP_NOTHING@@)
#define UNITEDRHINO_ERR_SSL_NOTHING_TO_READ -709
#define UNITEDRHINO_ERR_TCP_READ_TIMEOUT (@@TCP_TIMEOUT@@)
#define UNITEDRHINO_ERR_BUF_TOO_SHORT (@@BUF_TOO_SHORT@@)
#define UNITEDRHINO_ERR_MQTT_RX_STREAM_DESYNC (@@RX_DESYNC@@)

#define MAX_NO_OF_REMAINING_LENGTH_BYTES (@@MAX_REM_LEN_BYTES@@)
#define UNITEDRHINO_IOT_MQTT_MAX_REMAIN_WAIT_MS (@@MAX_REMAIN_WAIT@@)
#define UNITEDRHINO_IOT_MQTT_MIN_PACKET_READ_WINDOW_MS (@@MIN_WINDOW@@)
#define UNITEDRHINO_IOT_MQTT_MAX_HALF_PACKET_STALL_MS (@@MAX_STALL@@)

@@HEADER_MASKS@@
@@PACKET_TYPES@@
@@RX_STATES@@

struct Timer {};
constexpr int kYieldWindowMs = @@YIELD_WINDOW@@;
static int left_ms(Timer*) { return kYieldWindowMs; }

/* 可控时钟：半包停滞判定依赖它，测试按需推进。 */
static uint32_t g_now_ms = 100000;
static uint32_t HAL_GetTimeMs() { return g_now_ms; }

#define UR_TEST_BUF_LEN 512

/* 模拟 platform/HAL_TCP_lwip.c 的 HAL_TCP_Read：只有读满请求长度才返回成功，
 * 否则返回 -604/-609，且 *read_len 可能 > 0（已交付的部分字节已写入缓冲）。
 * 旧读包实现正是在这里丢弃已交付字节，造成 MQTT 字节流永久错位。 */
struct Net {
    static constexpr size_t kCapacity = 16384;
    unsigned char data[kCapacity] = {0};
    size_t size = 0;        /* 已注入字节数 */
    size_t offset = 0;      /* 已被读走的字节数 */
    size_t chunk = kCapacity;  /* 单次 read 最多交付的字节数（小于请求长度即部分读） */
    bool stall_all = false;    /* 强制"无数据可读"：读超时且无字节交付 */
    long long stall_at = -1;   /* 读到该偏移时进入无数据状态（模拟洪流饿死 TCP） */
    int reads = 0;             /* 网络读次数：用于断言错位后不再读 socket */

    bool stalled() const { return stall_all || (stall_at >= 0 && offset >= (size_t)stall_at); }

    int read(Net* self, unsigned char* out, size_t n, uint32_t /*timeout_ms*/, size_t* read_len) {
        ++self->reads;
        if (self->stalled() || self->offset >= self->size) {
            *read_len = 0;
            return @@TCP_NOTHING@@;
        }
        const size_t available = self->size - self->offset;
        const size_t want = n < available ? n : available;
        const size_t give = want < self->chunk ? want : self->chunk;
        std::memcpy(out, self->data + self->offset, give);
        self->offset += give;
        *read_len = give;
        return give == n ? UNITEDRHINO_RET_SUCCESS : @@TCP_TIMEOUT@@;
    }
};

struct UnitedRhino_IoT_Client {
    Net network_stack;
    unsigned char read_buf[UR_TEST_BUF_LEN];
    size_t read_buf_size = UR_TEST_BUF_LEN;
    uint8_t rx_state = 0;
    uint8_t rx_stream_broken = 0;
    uint8_t rx_rem_len_bytes = 0;
    uint8_t rx_payload_offset = 0;
    uint32_t rx_rem_len = 0;
    uint32_t rx_payload_read = 0;
    uint32_t rx_progress_ms = 0;
};

@@HELPERS@@

/* build_packet 追加一个报文：固定头首字节 + 规范的剩余长度编码 + 填充负载。
 * 垃圾长度（真机 71MB/557KB）不填充负载，只保留固定头——读包方不得按该长度读/排空。 */
static size_t build_packet(Net& net, unsigned char first, uint32_t rem_len, unsigned char seed) {
    net.data[net.size++] = first;
    net.size += mqtt_write_packet_rem_len(net.data + net.size, rem_len);
    for (uint32_t i = 0; i < rem_len && net.size < Net::kCapacity; ++i) {
        net.data[net.size++] = (unsigned char)(seed + i);
    }
    return net.size;
}

static void check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::exit(1);
    }
}

@@MAIN@@
'''
        harness = (
            harness.replace("@@MAX_REM_LEN_BYTES@@", str(_numeric_define(COMMON_SOURCE, "MAX_NO_OF_REMAINING_LENGTH_BYTES")))
            .replace("@@MAX_REMAIN_WAIT@@", str(_numeric_define(COMMON_SOURCE, "UNITEDRHINO_IOT_MQTT_MAX_REMAIN_WAIT_MS")))
            .replace("@@MIN_WINDOW@@", str(_numeric_define(COMMON_SOURCE, "UNITEDRHINO_IOT_MQTT_MIN_PACKET_READ_WINDOW_MS")))
            .replace("@@MAX_STALL@@", str(_numeric_define(COMMON_SOURCE, "UNITEDRHINO_IOT_MQTT_MAX_HALF_PACKET_STALL_MS")))
            .replace("@@YIELD_WINDOW@@", str(_numeric_define(DEVICE_SOURCE, "UR_MQTT_YIELD_MS")))
            .replace("@@MQTT_NOTHING@@", str(MQTT_NOTHING_TO_READ))
            .replace("@@TCP_NOTHING@@", str(TCP_NOTHING_TO_READ))
            .replace("@@TCP_TIMEOUT@@", str(TCP_READ_TIMEOUT))
            .replace("@@BUF_TOO_SHORT@@", str(BUF_TOO_SHORT))
            .replace("@@RX_DESYNC@@", str(RX_STREAM_DESYNC))
            .replace("@@HEADER_MASKS@@", header_masks)
            .replace("@@PACKET_TYPES@@", packet_types)
            .replace("@@RX_STATES@@", rx_states)
            .replace("@@HELPERS@@", helper)
            .replace("@@MAIN@@", main_body)
        )
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / "rx_resync_case")
            built = subprocess.run(
                ["g++", "-std=c++17", "-x", "c++", "-", "-o", binary],
                input=harness,
                text=True,
                capture_output=True,
            )
            self.assertEqual(built.returncode, 0, built.stderr)
            return subprocess.run([binary], capture_output=True, text=True)

    def test_partial_reads_resume_without_byte_loss(self):
        """部分读（-604）与长度字段饥饿后，续读必须读全同一报文且后续包边界不变。"""
        main_body = r'''
int main() {
    Timer timer;
    uint8_t type = 0;
    int rc = 0;

    /* 场景 a：负载部分读。单次最多交付 16 字节，HAL 语义下必然返回 -604。 */
    UnitedRhino_IoT_Client client;
    Net& net = client.network_stack;
    const size_t first_len = build_packet(net, 0x30, 40, 0x41);   /* PUBLISH, 负载 40 */
    const size_t both_len = build_packet(net, 0x30, 6, 0x61);    /* 紧随其后的小报文 */
    net.chunk = 16;

    int calls = 0;
    do {
        rc = _read_mqtt_packet(&client, &timer, &type);
        ++calls;
        g_now_ms += 25;
        check(calls < 32, "部分读下必须能在有限次调用内读全报文");
    } while (rc != UNITEDRHINO_RET_SUCCESS);

    check(calls >= 2, "单次最多 16 字节时不可能一次读完 42 字节报文");
    check(net.offset == first_len, "必须恰好消费第一个报文的全部字节（不得多读或少读）");
    check(std::memcmp(client.read_buf, net.data, first_len) == 0,
          "读缓冲必须与第一个报文逐字节一致（丢字节即旧实现错位）");
    check(type == 3, "完整报文类型应为 PUBLISH");
    check(client.rx_payload_read == 40, "负载进度必须与剩余长度一致");

    /* 场景 b：紧接着读第二个报文，证明第一个报文之后包边界没有偏移。 */
    net.chunk = Net::kCapacity;
    rc = _read_mqtt_packet(&client, &timer, &type);
    check(rc == UNITEDRHINO_RET_SUCCESS, "第二个报文必须完整读出");
    check(type == 3, "第二个报文类型应为 PUBLISH");
    check(net.offset == both_len, "两个报文合计必须恰好消费其全部字节");
    check(std::memcmp(client.read_buf, net.data + first_len, both_len - first_len) == 0,
          "第二个报文内容必须正确（错位后这里会读到半个包）");

    /* 场景 c：剩余长度字段中途饥饿（0 字节交付）——状态保留，恢复后继续解出同一长度。 */
    UnitedRhino_IoT_Client stalled;
    Net& stalled_net = stalled.network_stack;
    const size_t payload_len = 300;   /* 剩余长度字段占 2 字节 */
    const size_t stalled_len = build_packet(stalled_net, 0x30, payload_len, 0x11);
    stalled_net.stall_at = 1;         /* 固定头首字节之后立刻饿死 */

    rc = _read_mqtt_packet(&stalled, &timer, &type);
    check(rc == UNITEDRHINO_ERR_MQTT_NOTHING_TO_READ, "饥饿应由网络层映射为 NOTHING_TO_READ");
    check(stalled_net.offset == 1, "只允许消费固定头首字节");
    check(stalled.rx_state == 1 && stalled.rx_rem_len_bytes == 0,
          "长度字段未读时状态必须保留在 UR_MQTT_RX_LENGTH");
    check(stalled.rx_stream_broken == 0, "饥饿不是流错位，不得置位错位标志");

    g_now_ms += 25;
    stalled_net.stall_at = -1;        /* 洪流退去，数据恢复 */
    rc = _read_mqtt_packet(&stalled, &timer, &type);
    check(rc == UNITEDRHINO_RET_SUCCESS, "恢复后必须读出完整报文");
    check(stalled_net.offset == stalled_len, "必须恰好消费该报文全部字节");
    check(std::memcmp(stalled.read_buf, stalled_net.data, stalled_len) == 0,
          "读缓冲必须与报文一致（长度字段续读不得丢字节）");
    check(type == 3, "续读完成后类型应为 PUBLISH");

    std::printf("ok: partial reads resume without byte loss\n");
    return 0;
}
'''
        result = self._compile_and_run(main_body)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_unrecoverable_stream_faults_mark_broken_and_stop_reading(self):
        """垃圾长度、保留类型、半包停滞都必须置位错位标志，并且不再读 socket。"""
        main_body = r'''
int main() {
    Timer timer;
    uint8_t type = 0;
    int rc = 0;

    /* 场景 a：错位后解出的垃圾大长度（真机 71911397、557312）不得按长度排空字节。 */
    for (uint32_t garbage : {@@GARBAGE_BIG@@u, @@GARBAGE_SMALL@@u}) {
        UnitedRhino_IoT_Client client;
        Net& net = client.network_stack;
        build_packet(net, 0x30, garbage, 0x77);
        const int reads_before = net.reads;

        rc = _read_mqtt_packet(&client, &timer, &type);
        check(rc == @@BUF_TOO_SHORT@@, "超缓冲报文必须返回 BUF_TOO_SHORT");
        check(client.rx_stream_broken == 1, "超缓冲报文必须置位流错位标志");
        check(net.reads - reads_before <= 6,
              "不得按垃圾长度排空字节（旧实现会把后续所有合法包一起吃掉）");
        check(net.offset <= 6, "只允许消费固定头字段");

        const int reads_marked = net.reads;
        rc = _read_mqtt_packet(&client, &timer, &type);
        check(rc == @@RX_DESYNC@@, "置位后读包必须返回流错位错误");
        check(net.reads == reads_marked, "置位后不得再读 socket");
    }

    /* 场景 b：类型 0/15 是 MQTT 保留值——错位后的假包形态，不得拿去解析。 */
    for (unsigned char first : {0x00u, 0xF0u}) {
        UnitedRhino_IoT_Client client;
        Net& net = client.network_stack;
        build_packet(net, first, 8, 0x21);
        const int reads_before = net.reads;

        rc = _read_mqtt_packet(&client, &timer, &type);
        check(rc == @@RX_DESYNC@@, "保留类型必须返回流错位错误");
        check(client.rx_stream_broken == 1, "保留类型必须置位流错位标志");
        check(net.offset == 1, "只允许消费固定头首字节");

        const int reads_marked = net.reads;
        rc = _read_mqtt_packet(&client, &timer, &type);
        check(rc == @@RX_DESYNC@@ && net.reads == reads_marked,
              "置位后不得再读 socket 或继续解析");
    }

    /* 场景 c：半包停滞（零进展）超过上限——链路已不可用，必须主动判错而不是无限挂着半包。 */
    {
        UnitedRhino_IoT_Client client;
        Net& net = client.network_stack;
        build_packet(net, 0x30, 40, 0x31);
        net.stall_at = 2;   /* 头 + 长度字段读到后饿死 */

        rc = _read_mqtt_packet(&client, &timer, &type);
        check(rc == @@TCP_NOTHING@@ || rc == UNITEDRHINO_ERR_MQTT_NOTHING_TO_READ,
              "半包饥饿本次调用应按无数据返回");
        check(client.rx_state == @@RX_PAYLOAD@@, "半包状态必须保留在 UR_MQTT_RX_PAYLOAD");
        check(client.rx_stream_broken == 0, "停止前不得置位错位标志");

        g_now_ms += @@MAX_STALL@@ + 1;
        const int reads_marked = net.reads;
        rc = _read_mqtt_packet(&client, &timer, &type);
        check(rc == @@RX_DESYNC@@, "半包零进展超上限必须返回流错位错误");
        check(client.rx_stream_broken == 1, "半包停滞必须置位流错位标志");
        check(net.reads == reads_marked, "判定停滞时不得再读 socket");
    }

    /* 场景 d：新连接复位读包进度与错位标志（重连后必须能重新读包）。 */
    {
        UnitedRhino_IoT_Client client;
        Net& net = client.network_stack;
        client.rx_stream_broken = 1;
        client.rx_state = @@RX_PAYLOAD@@;
        client.rx_rem_len = 1234;
        client.rx_rem_len_bytes = 3;
        client.rx_payload_offset = 4;
        client.rx_payload_read = 77;
        client.rx_progress_ms = 55;

        reset_mqtt_rx_state(&client);
        check(client.rx_stream_broken == 0 && client.rx_state == @@RX_IDLE@@ &&
                  client.rx_rem_len == 0 && client.rx_rem_len_bytes == 0 &&
                  client.rx_payload_offset == 0 && client.rx_payload_read == 0 &&
                  client.rx_progress_ms == 0,
              "复位必须清空读包进度与错位标志");

        const size_t packet_len = build_packet(net, 0x40, 8, 0x51);   /* PUBACK */
        rc = _read_mqtt_packet(&client, &timer, &type);
        check(rc == UNITEDRHINO_RET_SUCCESS && type == 4, "复位后必须能正常读包");
        check(net.offset == packet_len, "复位后读包必须消费完整报文");
    }

    std::printf("ok: unrecoverable stream faults mark broken\n");
    return 0;
}
'''
        main_body = (
            main_body.replace("@@GARBAGE_BIG@@", str(FIELD_GARBAGE_REM_LEN))
            .replace("@@GARBAGE_SMALL@@", str(FIELD_GARBAGE_REM_LEN_SMALL))
            .replace("@@BUF_TOO_SHORT@@", str(BUF_TOO_SHORT))
            .replace("@@RX_DESYNC@@", str(RX_STREAM_DESYNC))
            .replace("@@TCP_NOTHING@@", str(TCP_NOTHING_TO_READ))
            .replace("@@RX_PAYLOAD@@", str(RX_PAYLOAD))
            .replace("@@RX_IDLE@@", str(RX_IDLE))
            .replace("@@MAX_STALL@@", str(_numeric_define(COMMON_SOURCE, "UNITEDRHINO_IOT_MQTT_MAX_HALF_PACKET_STALL_MS")))
        )
        result = self._compile_and_run(main_body)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_randomized_starvation_keeps_packet_sequence(self):
        """随机分块与随机饥饿下，读包顺序/内容必须与注入序列逐包一致（等价于不丢字节）。"""
        main_body = r'''
/* 简易线性同余发生器：固定种子，失败可复现。 */
static uint32_t g_seed = 20261015u;
static uint32_t next_rand() {
    g_seed = g_seed * 1103515245u + 12345u;
    return (g_seed >> 8) & 0x7fffffffu;
}

int main() {
    Timer timer;
    uint8_t type = 0;

    UnitedRhino_IoT_Client client;
    Net& net = client.network_stack;

    /* 注入 40 个合法类型报文（长度覆盖 0、1、127/128 边界与长负载）。 */
    const uint32_t kPackets = 40;
    uint32_t lengths[kPackets] = {0};
    unsigned char types[kPackets] = {0};
    size_t ends[kPackets] = {0};
    for (uint32_t i = 0; i < kPackets; ++i) {
        unsigned char packet_type = (unsigned char)(1 + next_rand() % 14);
        uint32_t rem_len = next_rand() % 300;
        if (i % 7 == 0) rem_len = 127;
        if (i % 11 == 0) rem_len = 128;
        if (i % 13 == 0) rem_len = 0;
        types[i] = packet_type;
        lengths[i] = rem_len;
        build_packet(net, (unsigned char)(packet_type << 4), rem_len, (unsigned char)(0x20 + i));
        ends[i] = net.size;
    }

    /* 读包：每次调用前随机切换"单次交付上限"，并以 1/3 概率制造一次饥饿窗口。 */
    size_t consumed = 0;
    for (uint32_t i = 0; i < kPackets; ++i) {
        net.chunk = (size_t)(1 + next_rand() % 64);
        int rc = 0;
        int calls = 0;
        while (true) {
            rc = _read_mqtt_packet(&client, &timer, &type);
            ++calls;
            check(calls < 4096, "续读不得无限循环");
            g_now_ms += 20;
            if (rc == UNITEDRHINO_RET_SUCCESS) break;
            check(rc != @@RX_DESYNC@@, "合法报文序列不得被判定为流错位");
            if (next_rand() % 3 == 0) {
                net.stall_at = (long long)net.offset;   /* 随机饥饿：暂停交付 */
                rc = _read_mqtt_packet(&client, &timer, &type);
                if (!(rc == UNITEDRHINO_ERR_MQTT_NOTHING_TO_READ ||
                      rc == @@TCP_NOTHING@@ || rc == @@TCP_TIMEOUT@@)) {
                    std::fprintf(stderr,
                                 "FAIL: 饥饿窗口 rc=%d rx_state=%u broken=%u payload=%u/%u\n",
                                 rc, client.rx_state, client.rx_stream_broken,
                                 client.rx_payload_read, client.rx_rem_len);
                    return 1;
                }
                net.stall_at = -1;
            }
        }
        check(type == types[i], "报文类型必须与注入序列一致（错位即在此暴露）");
        check(net.offset == ends[i], "消费字节数必须逐包对齐");
        check(std::memcmp(client.read_buf, net.data + consumed, ends[i] - consumed) == 0,
              "报文内容必须逐字节一致");
        consumed = ends[i];
    }

    check(client.rx_stream_broken == 0, "整轮随机压力后连接不得被判定为流错位");
    check(net.offset == net.size, "全部注入字节必须恰好被消费");
    std::printf("ok: randomized starvation keeps packet sequence\n");
    return 0;
}
'''
        main_body = (
            main_body.replace("@@RX_DESYNC@@", str(RX_STREAM_DESYNC))
            .replace("@@TCP_NOTHING@@", str(TCP_NOTHING_TO_READ))
            .replace("@@TCP_TIMEOUT@@", str(TCP_READ_TIMEOUT))
        )
        result = self._compile_and_run(main_body)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_connect_resets_state_and_drain_loop_is_gone(self):
        """更正路径复位契约：建连复位读包状态，且旧的按垃圾长度排空已删除。"""
        self.assertIn("reset_mqtt_rx_state(pClient);", CONNECT_SOURCE)
        connect_body = _function(CONNECT_SOURCE, "static int _mqtt_connect(")
        network_connect = connect_body.index("network_stack.connect")
        self.assertLess(
            network_connect,
            connect_body.index("reset_mqtt_rx_state(pClient);"),
            "复位必须发生在 TCP/TLS 建连成功之后（否则 CONNACK 读包会被错位标志挡住）",
        )
        body = _function(COMMON_SOURCE, "static int _read_mqtt_packet(")
        self.assertNotIn("total_bytes_read", body, "按垃圾长度排空的旧循环必须删除")
        self.assertIn("UNITEDRHINO_ERR_MQTT_RX_STREAM_DESYNC", body)
        # 旧实现把长度字段读取失败直接上抛（-1001/-110）并继续下一次读包，必须已改为续读。
        self.assertNotIn("_decode_packet_rem_len_with_net_read", COMMON_SOURCE)


if __name__ == "__main__":
    unittest.main()
