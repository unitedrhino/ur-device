"""执行生产 MQTT 上报和注册入口，防止换网接线只在替身状态机里正确。"""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class NetworkMqttTest(unittest.TestCase):
    """不访问无线网络，检查真实函数是否调用正确桥接门禁。"""

    def test_action_ack_precedes_physical_confirmation(self):
        """运行真实 action 消费函数，发送失败或请求拒绝不得打开确认页。"""
        source = (ROOT / "main/unitedrhino/ur_device.c").read_text()
        action = source[source.index("static void ur_drain_action_queue(void)"):
                        source.index("/* ur_drain_photo_results")]
        code = r'''
#include <cassert>
#include <cstring>
#include <cstdint>
#define ESP_LOGI(...) ((void)0)
#define pdMS_TO_TICKS(x) (x)
enum {pdTRUE=1, UR_NETWORK_ERROR_NETWORK=5};
struct ur_action_item_t { char token[128], action_id[32], content[129], request_id[17]; };
static void* s_action_queue=reinterpret_cast<void*>(1);
static bool queued=true, s_report_requested=false, publish_ok=true;
static int prepare_code=200, reply_count=0, present_count=0, cancel_count=0;
int xQueueReceive(void*,ur_action_item_t* item,int) {
 if(!queued) return 0; queued=false;
 std::strcpy(item->action_id,"configureNetwork"); std::strcpy(item->token,"message-token");
 std::strcpy(item->request_id,"0123456789abcdef"); return 1;
}
int ur_bridge_prepare_network_change(const char* id) { assert(std::strlen(id)==16); return prepare_code; }
bool ur_publish_action_reply(const char* action,const char* token,int code,const char*,const char*,bool accepted) {
 assert(std::strcmp(action,"configureNetwork")==0 && std::strcmp(token,"message-token")==0);
 assert(code==prepare_code && accepted==(code==200)); ++reply_count; return publish_ok;
}
void ur_bridge_present_network_change(const char*) { assert(reply_count==1 && publish_ok); ++present_count; }
void ur_network_cancel_request(const char*,uint8_t error) { assert(error==5); ++cancel_count; }
int ur_bridge_start_photo_action(const char*) { return 423; }
void ur_bridge_show_text(const char*) {}
void vTaskDelay(unsigned) {}
void esp_restart() { assert(false); }
// ACTION
int main() {
 ur_drain_action_queue(); assert(present_count==1 && cancel_count==0 && s_report_requested);
 queued=true; publish_ok=false; reply_count=present_count=cancel_count=0;
 ur_drain_action_queue(); assert(present_count==0 && cancel_count==1);
 queued=true; publish_ok=true; prepare_code=423; reply_count=present_count=cancel_count=0;
 ur_drain_action_queue(); assert(present_count==0 && cancel_count==0 && reply_count==1);
}
'''.replace("// ACTION", action)
        compiler = shutil.which("g++") or shutil.which("clang++")
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / "network-action-test")
            subprocess.run([compiler, "-std=c++17", "-x", "c++", "-", "-o", binary],
                           input=code, text=True, check=True)
            for _ in range(10):
                subprocess.run([binary], check=True, capture_output=True)

    def test_publish_success_and_registration_guard(self):
        """换网不增加云端状态属性或回执依赖，缺密钥仍不得动态注册。"""
        source = (ROOT / "main/unitedrhino/ur_device.c").read_text()
        report = source[source.index("static void ur_report_all_properties(void)"):
                        source.index("/* ur_report_device_info")]
        register = source[source.index("static void ur_ensure_registered("):
                          source.index("/* ur_poll_ota_status")]
        code = r'''
// 生产函数所依赖的 SDK 边界替身，不重写函数内部判断。
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define HAL_Snprintf std::snprintf
#define pdMS_TO_TICKS(x) (x)
#define CONFIG_BOARD_TYPE_SENSECAP_WATCHER 1
struct ur_network_status_t { char request_id[17]{}; unsigned stage=0, error=0; bool blocked=false; };
constexpr unsigned UR_NETWORK_ERROR_STORAGE=6;
void ur_network_status(ur_network_status_t* status) { *status={"0123456789abcdef",9,0,false}; }
enum {UNITEDRHINO_RET_SUCCESS=0, UR_NETWORK_ERROR_AUTH=4, UR_NETWORK_ROLLBACK=3};
struct DeviceInfo { char device_secret[16]{}; };
static int published=0, publish_result=0, registered=0, failures=0, failure_result=3;
static bool protect=false;
static std::string reported_token;
static void* s_mqtt_client=nullptr;
void ur_bridge_get_battery(int* value, bool* charging) { *value=50; *charging=false; }
int ur_bridge_get_volume() { return 35; }
int ur_bridge_get_brightness() { return 40; }
int ur_bridge_get_rssi() { return -50; }
void ur_make_token(char* token, size_t size, const char*) { std::snprintf(token,size,"current"); }
int ur_publish_topic(void*,const char*,const char* payload,int) {
 assert(!std::strstr(payload,"networkConfigStatus"));
 assert(std::strstr(payload,"\"Battery\":50") && std::strstr(payload,"\"Rssi\":-50"));
 ++published; return publish_result;
}
void ur_network_report_sent(const char* token) { reported_token=token; }
void HAL_GetDevInfo(DeviceInfo* info) { *info=DeviceInfo{}; }
bool ur_network_protect_credentials() { return protect; }
int ur_network_fail(int error) { assert(error==4); ++failures; return failure_result; }
[[noreturn]] void esp_restart() { throw 1; }
[[noreturn]] void vTaskDelay(unsigned) { throw 2; }
int IOT_DynReg_Device(DeviceInfo*) { ++registered; return 0; }
int HAL_SetDevInfo(DeviceInfo*) { return 0; }
uint32_t ur_next_retry_delay(size_t*) { return 1000; }
// REPORT
// REGISTER
int main() {
 publish_result=-1; ur_report_all_properties(); assert(published==1 && reported_token.empty());
 publish_result=0; ur_report_all_properties(); assert(published==2 && reported_token.empty());
 DeviceInfo info; size_t retry=3;
 protect=true;
 try { ur_ensure_registered(&info,&retry); assert(false); } catch(int value) { assert(value==1); }
 assert(registered==0 && failures==1);
 failure_result=4;
 try { ur_ensure_registered(&info,&retry); assert(false); } catch(int value) { assert(value==2); }
 assert(registered==0 && failures==2);
 protect=false; ur_ensure_registered(&info,&retry); assert(registered==1 && retry==0);
}
'''.replace("// REPORT", report).replace("// REGISTER", register)
        compiler = shutil.which("g++") or shutil.which("clang++")
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / "mqtt-network-test")
            subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            "-x", "c++", "-", "-o", binary], input=code, text=True, check=True)
            for _ in range(10):
                subprocess.run([binary], check=True, capture_output=True)
