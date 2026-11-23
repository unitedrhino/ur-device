"""回放真实主动重连分支及重试尾段，防止自愈重建误等网络故障退避。"""
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def block(source, signature):
    """提取完整花括号块，保留生产分支的控制流。"""
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


class ForcedReconnectDelayTest(unittest.TestCase):
    """用虚拟时钟检查主动重建及时执行、真正失败仍退避且不误清身份。"""

    def test_forced_reconnect_skips_failure_backoff(self):
        source = (ROOT / 'main/unitedrhino/ur_device.c').read_text()
        loop = block(source, 'static void ur_device_task(')
        declarations = loop[loop.index('int final_error ='):loop.index('        while (true) {',
                                                                     loop.index('int final_error ='))]
        force = block(loop, 'if (s_force_mqtt_reconnect)')
        start = loop.index('        ur_destroy_mqtt_client();\n        auth_failures =')
        tail = loop[start:loop.rindex('\n    }')]
        code = r'''
#include <cassert>
#include <cstddef>
#include <cstdint>
#define ESP_LOGW(...) ((void)0)
#define pdMS_TO_TICKS(value) (value)
enum { UNITEDRHINO_RET_SUCCESS=0, UNITEDRHINO_ERR_MQTT_ATTEMPTING_RECONNECT=-5,
       UR_AUTH_FAILURE_LIMIT=3, UR_NETWORK_ERROR_AUTH=1, UR_NETWORK_ROLLBACK=2 };
static bool s_force_mqtt_reconnect=false, s_connected=true;
static int destroyed=0, delays=0, elapsed=0, cleared=0, attempts=0;
void ur_network_disconnected() {}
void ur_destroy_mqtt_client() { ++destroyed; }
bool ur_is_auth_error(int) { return false; }
bool ur_network_protect_credentials() { return false; }
int ur_network_fail(int) { return 0; }
void esp_restart() { assert(false); }
void ur_bridge_clear_device_secret() { ++cleared; }
uint32_t ur_next_retry_delay(size_t* index) { ++*index; return 30000; }
void vTaskDelay(int duration) { ++delays; elapsed+=duration; }
// 两个片段之间保留生产清理点；硬件调用替为计数器，不复制重试策略。
void iteration(bool forced) {
    s_force_mqtt_reconnect=forced; s_connected=true;
    size_t retry_index=0; int auth_failures=0; attempts=0;
    while (true) {
        if (++attempts==2) return;
        DECLARATIONS
        do {
            FORCE
            final_error=-99;
        } while(false);
        TAIL
    }
}
int main() {
    iteration(true);
    assert(attempts==2 && !s_force_mqtt_reconnect && !s_connected);
    assert(elapsed==0 && delays==0);
    assert(destroyed==1 && cleared==0);
    destroyed=delays=elapsed=0;
    iteration(false);
    assert(attempts==2 && elapsed==30000 && delays==1);
    assert(destroyed==1 && cleared==0);
}
'''.replace('DECLARATIONS', declarations).replace('FORCE', force).replace('TAIL', tail)
        with tempfile.TemporaryDirectory(prefix='watcher-forced-reconnect-') as directory:
            binary = str(Path(directory) / 'forced-reconnect')
            built = subprocess.run(['g++', '-std=c++17', '-x', 'c++', '-', '-o', binary],
                                   input=code, text=True, capture_output=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            result = subprocess.run([binary], text=True, capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
