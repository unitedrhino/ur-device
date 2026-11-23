"""执行固件生产回调和重订阅分支，复现 SDK 吞掉重连返回值后的 AI 永久离线。"""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class ReconnectTest(unittest.TestCase):
    """回调是事实来源，普通成功返回值不能丢失恢复请求。"""

    def test_reconnect_event_survives_successful_yield(self):
        """覆盖正常/丢失返回码、恢复失败、重复循环及断线清理。"""
        source = (ROOT / "main/unitedrhino/ur_device.c").read_text()
        start = source.index("static void ur_mqtt_event_handler(")
        end = source.index("/* ur_setup_connect_params", start)
        callback = source[start:end]
        marker = source.index("/* SDK 默认 clean_session=1", source.index("int final_error ="))
        start = source.rfind("            if (", 0, marker)
        end = source.index("            if (s_report_info_requested)", marker)
        restore = source[start:end]
        pending = "static bool s_resubscribe_pending = false;" if "s_resubscribe_pending" in source else ""
        code = r'''
#include <cassert>
#include <cstddef>
#include <cstdint>
#define CONFIG_UR_AI 1
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
enum {MQTT_EVENT_RECONNECT=1, MQTT_EVENT_DISCONNECT=2,
 UNITEDRHINO_RET_SUCCESS=0, UNITEDRHINO_RET_MQTT_RECONNECTED=1, UNITEDRHINO_ERR_FAILURE=-1,
 MQTT_EVENT_SUBCRIBE_SUCCESS=3, MQTT_EVENT_SUBCRIBE_NACK=4, MQTT_EVENT_SUBCRIBE_TIMEOUT=5};
struct MQTTEventMsg { int event_type; void* msg=nullptr; };
static int s_subscribe_packet_id=0, s_subscribe_result=1;
static bool s_connected=true, s_report_requested=false, s_report_info_requested=false, ai=true;
static int subscriptions=0, subscribe_result=0;
static void *s_mqtt_client=nullptr, *ur_unbind_callback=nullptr;
void ur_notify_ai_connection(bool ready) { ai=ready; }
void ur_network_disconnected() {}
int ur_subscribe_down_topics(void*) { ++subscriptions; return subscribe_result; }
void IOT_Unbind_Device_ByCloud(void*,void*,void*) {}
// PENDING
// CALLBACK
int poll(int result) { int final_error=0; do {
// RESTORE
} while(false); return final_error; }
int main() {
 MQTTEventMsg down{MQTT_EVENT_DISCONNECT}, up{MQTT_EVENT_RECONNECT};
 for(int code : {0,1}) {
   ur_mqtt_event_handler(nullptr,nullptr,&down); assert(!ai);
   ur_mqtt_event_handler(nullptr,nullptr,&up); assert(!s_connected);
   int old=subscriptions; assert(poll(code)==0);
   assert(ai && s_connected && subscriptions==old+1);
   assert(s_report_requested && s_report_info_requested);
   poll(0); assert(subscriptions==old+1);
 }
 ur_mqtt_event_handler(nullptr,nullptr,&down);
 ur_mqtt_event_handler(nullptr,nullptr,&up); subscribe_result=-1;
 assert(poll(0)==-1); assert(!ai && !s_connected);
}
'''.replace("#include <cassert>", "#include <cassert>\n#include <initializer_list>")
        code = code.replace("// PENDING", pending).replace("// CALLBACK", callback).replace("// RESTORE", restore)
        with tempfile.TemporaryDirectory() as temp:
            binary = str(Path(temp) / "reconnect")
            subprocess.run([shutil.which("g++"), "-std=c++17", "-x", "c++", "-", "-o", binary],
                           input=code, text=True, check=True)
            subprocess.run([binary], check=True, capture_output=True)
