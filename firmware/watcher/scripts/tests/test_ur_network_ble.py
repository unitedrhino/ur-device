"""编译真实 BLE 入口，核验换网与首次配网分流，避免仅测试抽象事务。"""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def function(source, name):
    """以花括号边界抽取定义，保持生产函数完整用于主机编译。"""
    pos = source.index(name + "(")
    start = source.rfind("\n", 0, pos) + 1
    body = source.index("{", pos)
    depth = 1
    end = body + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class NetworkBleTest(unittest.TestCase):
    """仅替换硬件/SDK 边界；真实 GATT 仍需 BlueZ 验收。"""

    def test_rebinding_commits_only_after_sdk_success(self):
        """实际绑定入口覆盖 SDK 失败、断网、读取失败、提交失败和成功。"""
        source = (ROOT / "main/unitedrhino/ur_ble_provision.c").read_text()
        code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "ur_network.h"
typedef struct { struct { char ssid[32],password[64]; } sta; } wifi_config_t;
enum { ESP_OK=0, WIFI_IF_STA=0 };
static bool s_rebind_mode=false, sdk_ok=true, sg_gotip_flag=true;
static int config_error=0, commit_result=UR_NETWORK_COMMITTED, commits=0, failures=0;
bool ur_ble_provision_start(uint32_t timeout) { assert(timeout==180000 && s_rebind_mode); return sdk_ok; }
int esp_wifi_get_config(int type,wifi_config_t* config) { (void)type; strcpy(config->sta.ssid,"candidate"); return config_error; }
int ur_network_commit_connected(const char* ssid) { assert(strcmp(ssid,"candidate")==0); ++commits; return commit_result; }
int ur_network_fail(uint8_t error) { assert(error==UR_NETWORK_ERROR_NETWORK); ++failures; return UR_NETWORK_ROLLBACK; }
// FUNCTION
int main(void) {
 for (int scenario=0;scenario<5;++scenario) {
  sdk_ok=scenario!=0; sg_gotip_flag=scenario!=1; config_error=scenario==2;
  commit_result=scenario==3?UR_NETWORK_FAULT:UR_NETWORK_COMMITTED;
  commits=failures=0;
  assert(ur_ble_rebind_start(180000)==(scenario==4));
  assert(!s_rebind_mode && commits==(scenario>=3) && failures==(scenario!=4));
 }
}
'''.replace("// FUNCTION", function(source, "ur_ble_rebind_start"))
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / "rebind-test")
            subprocess.run(["gcc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                            f"-I{ROOT / 'main/unitedrhino'}", "-x", "c", "-", "-o", binary],
                           input=code, text=True, check=True)
            for _ in range(10):
                subprocess.run([binary], check=True, capture_output=True)

    def test_disconnect_reason_preserves_retry_policy(self):
        """真实事件函数保留数值原因，不泄漏凭据、不改变候选重连及旧配置。"""
        source = (ROOT / "main/unitedrhino/ur_ble_provision.c").read_text()
        code = r'''
/* 替换 SDK 边界，执行实际事件处理函数；不复制断线分支。 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
typedef int esp_event_base_t;
typedef int esp_err_t;
typedef int esp_sntp_config_t;
typedef struct { uint8_t reason; char ssid[32]; } wifi_event_sta_disconnected_t;
typedef struct { struct { char ssid[32]; } sta; } wifi_config_t;
typedef struct { struct { int ip; } ip_info; } ip_event_got_ip_t;
enum { WIFI_EVENT=1, WIFI_EVENT_STA_DISCONNECTED=2, IP_EVENT=3,
 IP_EVENT_STA_GOT_IP=4, UR_CHANGE_IP=1, UR_CHANGE_DISCONNECTED=2,
 WIFI_IF_STA=0, BLE_WIFI_MODE_STA=1, BLE_WIFI_STATE_OTHER=1,
 ESP_OK=0, ESP_ERR_INVALID_STATE=1, ESP_COEX_PREFER_BALANCE=0 };
static atomic_bool s_change_mode=true, s_connect_requested=true;
static bool sg_gotip_flag=true, s_provisioning=false, s_sntp_started=false;
static int s_change_events=0, flags=UR_CHANGE_IP, connects=0, config_reads=0;
static char last_log[256];
#define ESP_LOGW(tag, ...) snprintf(last_log,sizeof(last_log),__VA_ARGS__)
#define ESP_LOGI(...) ((void)0)
#define ESP_NETIF_SNTP_DEFAULT_CONFIG(host) 0
void xEventGroupClearBits(int group,int bits) { (void)group; flags &= ~bits; }
void xEventGroupSetBits(int group,int bits) { (void)group; flags |= bits; }
int esp_wifi_connect(void) { ++connects; return 0; }
void esp_wifi_get_config(int iface,wifi_config_t *config) {
 (void)iface; (void)config; ++config_reads;
}
void ble_event_report_wifi_connect(int mode,int state,uint8_t len,const char *ssid) {
 (void)mode; (void)state; (void)len; (void)ssid;
}
const char *esp_err_to_name(int error) { (void)error; return "error"; }
void esp_coex_preference_set(int mode) { (void)mode; }
int esp_netif_sntp_init(esp_sntp_config_t *config) { (void)config; return 0; }
// FUNCTION
int main(void) {
 wifi_event_sta_disconnected_t event={.reason=201,.ssid="sensitive-network"};
 for (int repeat=0;repeat<10;++repeat) {
   connects=0; config_reads=0; sg_gotip_flag=true; flags=UR_CHANGE_IP;
   atomic_store(&s_connect_requested,true);
   ur_wifi_event_handler(NULL,WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,&event);
   assert(strstr(last_log,"reason=201") && !strstr(last_log,event.ssid));
   assert(connects==1 && config_reads==0 && !sg_gotip_flag);
   assert(flags==UR_CHANGE_DISCONNECTED);
   atomic_store(&s_connect_requested,false);
   ur_wifi_event_handler(NULL,WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,NULL);
   assert(strstr(last_log,"reason=-1") && connects==1 && config_reads==0);
 }
 return 0;
}
'''.replace("// FUNCTION", function(source, "ur_wifi_event_handler"))
        # 被忽略的 INFO 参数不应使 SDK 边界替身产生未使用变量告警。
        compiler = shutil.which("gcc") or shutil.which("clang")
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / "disconnect-test")
            subprocess.run([compiler, "-std=gnu11", "-x", "c", "-", "-o", binary],
                           input=code, text=True, capture_output=True, check=True)
            subprocess.run([binary], capture_output=True, check=True, timeout=10)

    def test_candidate_isolation_and_capability_ten_times(self):
        """正式列表隔离、迟到候选拒绝、解绑拒绝、15 字节能力通知和首次绑定兼容。"""
        source = (ROOT / "main/unitedrhino/ur_ble_provision.c").read_text()
        functions = "\n".join(function(source, name) for name in (
            "ur_change_report_result", "ur_change_queue_candidate", "ur_change_apply_candidate",
            "ble_combo_wifi_info_set", "ble_combo_wifi_connect", "unbind_dev_handle", "ur_change_notify"))
        code = r'''
/* SDK 边界替身记录副作用，不复制配网分流逻辑。 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#include "ur_network.h"
typedef int esp_err_t;
typedef int ble_qiot_ret_status_t;
typedef struct { struct { uint8_t ssid[32], password[64]; struct { int authmode; } threshold;
 struct { bool capable,required; } pmf_cfg; } sta; } wifi_config_t;
enum {ESP_OK=0, WIFI_AUTH_WPA2_PSK=2, WIFI_AUTH_OPEN=0, WIFI_IF_STA=0,
 WIFI_CONFIG_SUCCESS=1, BLE_QIOT_RS_ERR=-1, ESP_COEX_PREFER_WIFI=1,
 BLE_WIFI_MODE_STA=1, BLE_WIFI_STATE_CONNECT=0, BLE_WIFI_STATE_OTHER=1};
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_ERROR_CHECK(x) assert((x)==0)
static atomic_bool s_change_mode=false, s_connect_requested=false;
static atomic_int s_candidate_mailbox=0;
static atomic_bool s_candidate_ready=false;
static char s_candidate_ssid[33], s_candidate_password[65];
static bool s_trial_mode=false, s_rebind_mode=false, accept=true, protected=false, connected=true;
static int saved=0, accepted=0, configured=0, connects=0, cleared=0, restarted=0, notified=0;
static wifi_config_t last_config;
static bool sg_gotip_flag=false;
static int reported_state=-1, reported_length=-1;
static int credential_reply=-1, credential_replies=0, configure_error=0;
static ur_network_status_t status={.request_id="0123456789abcdef",.stage=3};
static uint8_t packet[15];
bool ur_network_accept(const char* ssid,const char* password) {
 assert(strcmp(ssid,"candidate")==0 && password[0]==0); ++accepted; return accept;
}
void ur_network_status(ur_network_status_t* out) { *out=status; }
bool ur_network_protect_credentials(void) { return protected; }
int ble_event_report_wifi_info(int result) { credential_reply=result; ++credential_replies; return result; }
int esp_wifi_get_config(int type,wifi_config_t* config) { (void)type; *config=last_config; return 0; }
int ble_event_report_wifi_connect(int mode,int state,uint8_t length,const char* ssid) {
 assert(mode==1); reported_state=state; reported_length=length;
 if (state==0) assert(length==9 && memcmp(ssid,"candidate",9)==0);
 return 0;
}
int esp_wifi_set_config(int type,const wifi_config_t* config) {
 (void)type; ++configured; last_config=*config; return configure_error;
}
void ur_bridge_save_wifi(const char* ssid,const char* password) { (void)ssid;(void)password;++saved; }
void set_bt_combo_config_state(int value) { (void)value; }
int esp_coex_preference_set(int preference) { (void)preference; return 0; }
int esp_wifi_connect(void) { ++connects; return 0; }
void ur_bridge_clear_device_info(void) { ++cleared; }
void esp_restart(void) { ++restarted; }
bool ble_is_connected(void) { return connected; }
int ble_send_notify(uint8_t* bytes,uint8_t length) {
 assert(length==15); memcpy(packet,bytes,15); ++notified; return 0;
}
// FUNCTIONS
int main(void) {
 atomic_store(&s_change_mode,true);
 assert(ble_combo_wifi_info_set("candidate",9,"",0)==0);
 // 回调返回不代表已保存：BTC_TASK 不得进入 NVS 或驱动配置，也不提前发送成功通知。
 assert(saved==0 && accepted==0 && configured==0 && credential_replies==0);
 assert(ble_combo_wifi_connect()==-1 && connects==0);
 assert(ble_combo_wifi_info_set("candidate",9,"",0)==1 && accepted==0);
 ur_change_apply_candidate();
 assert(credential_reply==0 && atomic_load(&s_candidate_mailbox)==0);
 assert(s_candidate_ssid[0]==0 && s_candidate_password[0]==0);
 assert(saved==0 && accepted==1 && configured==1);
 assert(last_config.sta.threshold.authmode==WIFI_AUTH_OPEN);
 assert(ble_combo_wifi_connect()==0 && connects==1);
 // 获得 IP 但未保存不能报告成功；保存后重复 E3 只读回报，不再次关联或写 Flash。
 sg_gotip_flag=true;
 ur_change_report_result(); assert(reported_state==1 && reported_length==0);
 status.stage=7;
 assert(ble_combo_wifi_connect()==0 && connects==1 && reported_state==0);
 sg_gotip_flag=false;
 assert(ble_combo_wifi_connect()==0 && connects==1 && reported_state==1);
 status.blocked=true; sg_gotip_flag=true;
 ur_change_report_result(); assert(reported_state==1);
 status.blocked=false; status.stage=3;
 assert(ble_combo_wifi_info_set("candidate",9,"",0)==1 && accepted==1 && saved==0);
 unbind_dev_handle(); assert(cleared==0 && restarted==0);
 ur_change_notify(); assert(notified==1 && packet[0]==0xe5 && packet[2]==12);
 assert(packet[3]==2 && packet[4]==1 && packet[5]==3);
 const uint8_t id[8]={1,0x23,0x45,0x67,0x89,0xab,0xcd,0xef};
 assert(memcmp(packet+7,id,8)==0);
 connected=false; ur_change_notify(); assert(notified==1);
 atomic_store(&s_connect_requested,false); accept=false;
 assert(ble_combo_wifi_info_set("candidate",9,"",0)==0);
 ur_change_apply_candidate();
 assert(credential_reply==1 && configured==1 && !atomic_load(&s_candidate_ready));
 // 驱动配置失败同样不能让 E3 连接旧配置；凭据副本仍必须清除。
 accept=true; configure_error=-1;
 assert(ble_combo_wifi_info_set("candidate",9,"",0)==0);
 ur_change_apply_candidate();
 assert(credential_reply==1 && !atomic_load(&s_candidate_ready));
 assert(ble_combo_wifi_connect()==-1 && connects==1);
 assert(s_candidate_ssid[0]==0 && s_candidate_password[0]==0);
 configure_error=0;
 status.stage=2; assert(ble_combo_wifi_connect()==-1 && connects==1);
 s_trial_mode=true; assert(ble_combo_wifi_info_set("candidate",9,"",0)==1);
 atomic_store(&s_change_mode,false); s_trial_mode=false;
 assert(ble_combo_wifi_info_set("candidate",9,"",0)==0 && saved==1);
 /* 重新绑定同样隔离候选，SDK 绑定结束前不写正式列表；开始连接后拒绝覆盖。 */
 s_rebind_mode=true; atomic_store(&s_connect_requested,false); accept=true;
 assert(ble_combo_wifi_info_set("candidate",9,"",0)==0);
 ur_change_apply_candidate();
 assert(saved==1 && atomic_load(&s_candidate_ready));
 assert(ble_combo_wifi_connect()==0);
 assert(ble_combo_wifi_info_set("candidate",9,"",0)==1 && saved==1);
 s_rebind_mode=false;
 protected=true; unbind_dev_handle(); assert(cleared==0);
 protected=false; unbind_dev_handle(); assert(cleared==1 && restarted==1);
 return 0;
}
'''.replace("// FUNCTIONS", functions)
        compiler = shutil.which("gcc") or shutil.which("clang")
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / "ble-network-test")
            subprocess.run([compiler, "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                            f"-I{ROOT / 'main/unitedrhino'}", "-x", "c", "-", "-o", binary],
                           input=code, text=True, check=True)
            for _ in range(10):
                subprocess.run([binary], check=True, capture_output=True)
