"""编译并运行完整生产换网 worker，以可控 RTOS/Wi-Fi 边界回放事件顺序，不代替真机。"""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_ur_network_ble import function

ROOT = Path(__file__).resolve().parents[2]


class NetworkWorkerTest(unittest.TestCase):
    """完整循环必须证实成功窗口与失败副作用，不只断言源码包含某函数名。"""

    def test_worker_events_and_result_window_ten_times(self):
        """执行成功、频繁事件、保存失败、AP 无效、配网超时及蓝牙初始化失败。"""
        worker = function((ROOT / "main/unitedrhino/ur_ble_provision.c").read_text(), "ur_change_worker")
        code = r'''
/* RTOS 与硬件边界替身；生产 worker 负责所有分支、计时和副作用顺序。 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#include <setjmp.h>
#include "ur_network.h"
typedef uint32_t TickType_t;
typedef unsigned EventBits_t;
typedef int esp_err_t;
typedef struct { struct { uint8_t ssid[32], password[64]; struct { int authmode; } threshold;
 struct { bool capable; } pmf_cfg; } sta; } wifi_config_t;
typedef struct { uint8_t ssid[33]; } wifi_ap_record_t;
enum { eLOG_INFO, WIFI_IF_STA, WIFI_AUTH_WPA2_PSK, WIFI_AUTH_OPEN,
 ESP_BLUEDROID_STATUS_ENABLED=8, ESP_OK=0, UR_CHANGE_IP=1, UR_CHANGE_DISCONNECTED=2 };
#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(x) (x)
static bool s_trial_mode=false, sg_gotip_flag=false;
static atomic_bool s_connect_requested=false;
static void (*s_ip_callback)(bool,void*)=NULL;
static void* s_ip_context=NULL;
static int s_change_events=1;
static jmp_buf stopped;
static unsigned scenario=0, now=0, calls=0, commits=0, reports=0, failures=0;
static unsigned released=0, disconnects=0, restarts=0, deleted=0, commit_at=0;
static unsigned notifications=0;
static unsigned candidate_polls=0; /* 每次唤醒都消费信箱，凭据写入不能依赖 IP 事件。 */
static bool persisted=false, faulted=false;
void IOT_Log_Set_Level(int level) { (void)level; }
void wifi_init_sta(void) {}
void ur_change_apply_candidate(void) { ++candidate_polls; }
void ble_qiot_service_init(void) {}
int esp_bluedroid_get_status(void) { return scenario==5 ? 0 : 8; }
bool ur_network_candidate(char ssid[33],char password[65]) { (void)ssid;(void)password;return false; }
int esp_wifi_set_config(int type,const wifi_config_t* config) { (void)type;(void)config;return 0; }
int esp_wifi_connect(void) { return 0; }
void esp_wifi_disconnect(void) { ++disconnects; }
bool ble_is_connected(void) { return true; }
void ur_change_notify(void) { ++notifications; }
void ur_ble_provision_deinit_bt(void) { ++released; }
void vTaskDelay(unsigned delay) { now+=delay; }
TickType_t xTaskGetTickCount(void) { return now; }
void esp_restart(void) { ++restarts; longjmp(stopped,1); }
void vTaskDelete(void* task) { (void)task; ++deleted; longjmp(stopped,1); }
int ur_network_fail(uint8_t error) {
 assert(error==UR_NETWORK_ERROR_NETWORK || error==UR_NETWORK_ERROR_STORAGE);
 ++failures; return UR_NETWORK_ROLLBACK;
}
int esp_wifi_sta_get_ap_info(wifi_ap_record_t* ap) {
 if(scenario==3) return -1;
 memcpy(ap->ssid,"candidate",10);return 0;
}
int ur_network_commit_connected(const char* ssid) {
 assert(strcmp(ssid,"candidate")==0);
 assert(sg_gotip_flag && atomic_load(&s_connect_requested));
 ++commits;
 if(scenario==2) { faulted=true; return UR_NETWORK_FAULT; }
 persisted=true;commit_at=now;return UR_NETWORK_COMMITTED;
}
int ur_change_report_result(void) {
 ++reports;
 if(scenario==0 || scenario==1) assert(persisted && commits==1);
 else assert(!persisted);
 return 0;
}
int ur_network_tick(void) {
 if(faulted) return UR_NETWORK_FAULT;
 if(scenario==4 && now>=180000) return UR_NETWORK_ROLLBACK;
 return UR_NETWORK_CONTINUE;
}
EventBits_t xEventGroupWaitBits(int group,int bits,int clear,int all,unsigned timeout) {
 assert(group==1 && bits==3 && clear==1 && all==0 && timeout==250);
 ++calls; now+=scenario==1 ? 1 : 250;
 assert(calls<200000 && now<200000);
 if(scenario==4) return 0;
 sg_gotip_flag=true;
 atomic_store(&s_connect_requested,true);
 return UR_CHANGE_IP;
}
// WORKER
int main(void) {
 for(scenario=0;scenario<6;++scenario) {
  now=calls=commits=reports=failures=released=disconnects=restarts=deleted=commit_at=0;
  notifications=0;
  candidate_polls=0;
  persisted=faulted=sg_gotip_flag=false;
  atomic_store(&s_connect_requested,false);
  if(!setjmp(stopped)) ur_change_worker(NULL);
  assert(candidate_polls==calls);
  if(scenario<=1) {
   assert(commits==1 && reports>0 && released==1 && restarts==1 && deleted==0);
   assert(now-commit_at>=30000 && now-commit_at<30250);
   assert(reports<=31 && notifications<=30);
   assert(!atomic_load(&s_connect_requested));
  } else if(scenario==2) {
   assert(commits==1 && reports==1 && released==1 && disconnects==1 && deleted==1 && restarts==0);
   assert(!atomic_load(&s_connect_requested));
  } else if(scenario==3) {
   assert(commits==0 && reports==1 && failures==1 && restarts==1);
  } else if(scenario==4) {
   assert(commits==0 && reports==0 && now==180000 && restarts==1);
  } else {
   assert(calls==0 && commits==0 && failures==1 && restarts==1);
  }
 }
}
'''.replace("// WORKER", worker)
        compiler = shutil.which("gcc") or shutil.which("clang")
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory(prefix="watcher-network-worker-") as directory:
            binary = str(Path(directory) / "worker")
            subprocess.run([compiler, "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                            f"-I{ROOT / 'main/unitedrhino'}", "-x", "c", "-", "-o", binary],
                           input=code, text=True, check=True)
            for _ in range(10):
                result = subprocess.run([binary], capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stderr)
