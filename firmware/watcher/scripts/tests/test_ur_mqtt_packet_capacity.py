"""回放 SDK 真实读包函数：旧 2 KiB 配置拒绝长回复，AI 配置完整接收并保留上限。

读包路径改为"跨调用续读 + 错位标志"后（见 test_ur_mqtt_rx_stream_resync.py），
超缓冲报文的处理也变了：不再按剩余长度排空字节（错位流里那是垃圾大值），而是丢弃该报文
并置位流错位标志；因此本用例在每个容量档位之间复位读包状态（等价于重建连接）。
"""
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SDK = ROOT / "components/unitedrhino_iot_c_sdk"
COMMON_SOURCE = (SDK / "sdk_src/mqtt_client_common.c").read_text()
MQTT_CLIENT_HEADER = (SDK / "sdk_src/internal_inc/mqtt_client.h").read_text()


def _function(source, signature):
    """按签名切出完整函数体（含右花括号），供编译执行。"""
    start = source.index(signature)
    end = source.index("\n}\n", start) + len("\n}\n")
    return source[start:end]


def _header_enum(header, name):
    """切出内部头文件里的枚举定义（报文类型/读包状态），保持与生产定义一致。"""
    return re.search(rf"typedef enum [^{{]*\{{[^}}]*\}} {name};", header).group(0)


class MqttPacketCapacityTest(unittest.TestCase):
    """同一生产函数分别编译默认和固件容量，复现现场 2512 字节失败。"""

    def test_long_reply_and_capacity_boundary(self):
        """旧配置失败、新配置接收长回复；最大容量及以上明确拒绝且不排空字节流。"""
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
        harness = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include "unitedrhino_iot_export_variables.h"
#define IOT_FUNC_ENTRY ((void)0)
#define IOT_FUNC_EXIT_RC(x) { return (x); }
#define POINTER_SANITY_CHECK(p,r) if(!(p)) return (r)
#define Log_e(...) ((void)0)
#define Log_w(...) ((void)0)
#define UNITEDRHINO_RET_SUCCESS 0
#define UNITEDRHINO_ERR_INVAL -1002
#define UNITEDRHINO_ERR_SSL_NOTHING_TO_READ -709
#define UNITEDRHINO_ERR_TCP_NOTHING_TO_READ -609
#define UNITEDRHINO_ERR_MQTT_NOTHING_TO_READ -109
#define UNITEDRHINO_ERR_BUF_TOO_SHORT -119
#define UNITEDRHINO_ERR_MQTT_RX_STREAM_DESYNC -123
#define MAX_NO_OF_REMAINING_LENGTH_BYTES 4
#define UNITEDRHINO_IOT_MQTT_MAX_REMAIN_WAIT_MS 2000
/* 包内读取预算下限（生产值 500，由 test_ur_signaling_recovery_contract 守护）；缺它
 * _read_mqtt_packet 无法编译，本用例会退化成编译失败而非容量断言。 */
#define UNITEDRHINO_IOT_MQTT_MIN_PACKET_READ_WINDOW_MS 500
#define UNITEDRHINO_IOT_MQTT_MAX_HALF_PACKET_STALL_MS 20000
#define MQTT_HEADER_TYPE_MASK 0xf0
#define MQTT_HEADER_TYPE_SHIFT 4
@@PACKET_TYPES@@
@@RX_STATES@@
struct Timer {};
static int left_ms(Timer*) { return 100; }
static uint32_t HAL_GetTimeMs() { return 1000; }

/* 模拟 HAL_TCP_Read：按注入的报文字节流交付，读满请求长度才算成功。 */
struct Net {
    unsigned char data[40960];
    size_t size = 0;
    size_t offset = 0;
    int read(Net* self, unsigned char* out, size_t n, uint32_t, size_t* read_len) {
        const size_t available = self->size - self->offset;
        const size_t give = n < available ? n : available;
        std::memcpy(out, self->data + self->offset, give);
        self->offset += give;
        *read_len = give;
        return give == n ? 0 : -604;
    }
};
struct UnitedRhino_IoT_Client {
    Net network_stack;
    size_t read_buf_size = UNITEDRHINO_IOT_MQTT_RX_BUF_LEN;
    unsigned char read_buf[UNITEDRHINO_IOT_MQTT_RX_BUF_LEN];
    uint32_t sentinel = 0x12345678;
    uint8_t rx_state = 0;
    uint8_t rx_stream_broken = 0;
    uint8_t rx_rem_len_bytes = 0;
    uint8_t rx_payload_offset = 0;
    uint32_t rx_rem_len = 0;
    uint32_t rx_payload_read = 0;
    uint32_t rx_progress_ms = 0;
};
@@HELPERS@@

/* build_publish 注入一个 PUBLISH 报文（固定头首字节 + 规范剩余长度编码 + 填充负载）。 */
static size_t build_publish(Net& net, uint32_t rem_len) {
    net.size = 0;
    net.offset = 0;
    net.data[net.size++] = 0x30; /* PUBLISH(3)<<4 */
    net.size += mqtt_write_packet_rem_len(net.data + net.size, rem_len);
    for (uint32_t i = 0; i < rem_len; ++i) {
        net.data[net.size++] = (unsigned char)(0x10 + i);
    }
    return net.size;
}
'''
        harness = harness.replace("@@HELPERS@@", helpers)
        harness = harness.replace(
            "@@PACKET_TYPES@@", _header_enum(MQTT_CLIENT_HEADER, "MessageTypes")
        )
        harness = harness.replace(
            "@@RX_STATES@@", _header_enum(MQTT_CLIENT_HEADER, "UrMqttRxState")
        )
        harness += r'''
int main() {
#ifdef EXPECT_AI
    static_assert(UNITEDRHINO_IOT_MQTT_RX_BUF_LEN == 16384);
#else
    static_assert(UNITEDRHINO_IOT_MQTT_RX_BUF_LEN == 2048);
#endif
    UnitedRhino_IoT_Client client;
    Timer timer; uint8_t type = 0;
    for (uint32_t n : {2512u, 8000u, 16000u, 16384u, 20000u}) {
        unsigned char length_field[MAX_NO_OF_REMAINING_LENGTH_BYTES] = {0};
        const uint32_t encoded = mqtt_write_packet_rem_len(length_field, n);
        const bool fits = (1 + encoded + n) <= client.read_buf_size;

        const size_t total = build_publish(client.network_stack, n);
        const int result = _read_mqtt_packet(&client, &timer, &type);
        assert(total == 1 + encoded + n);
        assert(result == (fits ? 0 : -119));
        assert(client.sentinel == 0x12345678);
        if (fits) {
            assert(type == 3);
            assert(client.network_stack.offset == total);
            assert(std::memcmp(client.read_buf, client.network_stack.data, total) == 0);
        } else {
            /* 超缓冲报文只消费固定头字段：不再按（可能是垃圾的）剩余长度排空字节流。 */
            assert(client.rx_stream_broken == 1);
            assert(client.network_stack.offset <= 1 + MAX_NO_OF_REMAINING_LENGTH_BYTES);
            /* 置位错位标志后必须返回错位错误，且不再读 socket。 */
            const size_t reads_before = client.network_stack.offset;
            const int marked = _read_mqtt_packet(&client, &timer, &type);
            assert(marked == -123);
            assert(client.network_stack.offset == reads_before);
        }
        /* 新连接复位读包状态，等价于重建连接后继续下一档位。 */
        reset_mqtt_rx_state(&client);
    }
    std::printf("ok: capacity boundaries\n");
}
'''
        with tempfile.TemporaryDirectory() as temp:
            for override in (False, True):
                binary = str(Path(temp) / ("ai" if override else "default"))
                command = ["g++", "-std=c++17", "-I" + str(SDK / "include")]
                if override:
                    command += ["-DEXPECT_AI=1", "-include", str(ROOT / "main/unitedrhino/ur_mqtt_buffer_config.h")]
                compiled = subprocess.run(command + ["-x", "c++", "-", "-o", binary],
                                          input=harness, text=True, capture_output=True)
                self.assertEqual(compiled.returncode, 0, compiled.stderr)
                subprocess.run([binary], check=True, capture_output=True)
        cmake = (ROOT / "main/CMakeLists.txt").read_text()
        self.assertIn("target_compile_options(${ur_ai_sdk_lib} PRIVATE", cmake)
        self.assertIn("unitedrhino/ur_mqtt_buffer_config.h", cmake)
