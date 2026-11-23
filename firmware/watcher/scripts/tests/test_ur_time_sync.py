"""执行实际启动校时函数，覆盖主时间源无响应、有界回退及已有有效时钟。"""
from pathlib import Path
import subprocess
import tempfile
import unittest
from .test_call_audio_decode import method

ROOT = Path(__file__).resolve().parents[2]


class TimeSyncFallbackTest(unittest.TestCase):
    """只替换时钟和SNTP驱动，启动顺序与时间预算使用生产函数。"""
    def test_primary_unavailable_uses_bounded_fallback(self):
        actual = method((ROOT / 'main/unitedrhino/ur_device.c').read_text(),
                        'static void ur_wait_time_sync(void)')
        code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <time.h>
#define UR_TIME_SYNC_TIMEOUT_SECONDS 15
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 1
#define ESP_ERR_TIMEOUT 3
#define pdMS_TO_TICKS(x) (x)
typedef int esp_err_t;
typedef struct {const char* server;} esp_sntp_config_t;
#define ESP_NETIF_SNTP_DEFAULT_CONFIG(name) ((esp_sntp_config_t){name})
// 夹具记录实际驱动初始化与睡眠；成功时间源在首次等待后返回真实年份范围。
static time_t now_value;
static const char *target, *selected;
static int starts, stops, seconds_waited;
static bool fail_first_init, already_started, missing_semaphore;
static time_t fake_time(time_t *out){if(out)*out=now_value;return now_value;}
#define time fake_time
static esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *config){++starts;if(already_started)return ESP_ERR_INVALID_STATE;if(fail_first_init&&starts==1){selected=NULL;return 2;}selected=config->server;return ESP_OK;}
static void esp_netif_sntp_deinit(void){++stops;selected=NULL;}
static void vTaskDelay(int ms){assert(ms==1000);++seconds_waited;if(target&&selected&&!strcmp(target,selected))now_value=1793190000;}
// 模拟 SNTP 的同步完成事件，已有合理年份也必须等待真实校时结果。
static esp_err_t esp_netif_sntp_sync_wait(int ticks){if(missing_semaphore)return ESP_ERR_INVALID_STATE;vTaskDelay(ticks);return target&&selected&&!strcmp(target,selected)?ESP_OK:ESP_ERR_TIMEOUT;}
ACTUAL
static void reset(const char *server){target=server;selected=NULL;now_value=0;fail_first_init=already_started=missing_semaphore=false;starts=stops=seconds_waited=0;}
int main(void){
 reset("ntp.aliyun.com");ur_wait_time_sync();assert(now_value>1735689600);assert(starts==2&&stops==1);assert(seconds_waited<=15);
 reset("ntp.aliyun.com");fail_first_init=true;ur_wait_time_sync();assert(now_value>1735689600&&starts==2&&stops==0&&seconds_waited<=15);
 reset("pool.ntp.org");ur_wait_time_sync();assert(starts==1&&stops==0&&seconds_waited==1);
 reset(NULL);ur_wait_time_sync();assert(now_value==0&&seconds_waited==15);assert(starts==3&&stops==2);
 reset("pool.ntp.org");now_value=1793190200;ur_wait_time_sync();assert(now_value==1793190000&&starts==1&&seconds_waited==1);
 reset(NULL);now_value=1793190200;ur_wait_time_sync();assert(now_value==1793190200&&seconds_waited==15);
 reset("ntp.aliyun.com");already_started=true;selected=target;now_value=1793190200;ur_wait_time_sync();assert(now_value==1793190000&&starts==1&&stops==0);
 reset(NULL);already_started=true;now_value=1793190200;ur_wait_time_sync();assert(starts==1&&stops==0&&seconds_waited==15);
 reset(NULL);already_started=true;missing_semaphore=true;ur_wait_time_sync();assert(starts==1&&stops==0&&seconds_waited==15);
 return 0;
}
'''.replace('ACTUAL', actual)
        directory = ROOT.parents[1] / '.temp'
        directory.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=directory) as temp:
            binary = str(Path(temp) / 'time-sync')
            built = subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-x', 'c', '-', '-o', binary],
                                   input=code, text=True, capture_output=True, timeout=30)
            self.assertEqual(built.returncode, 0, built.stderr)
            result = subprocess.run([binary], capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr.decode(errors='replace'))
