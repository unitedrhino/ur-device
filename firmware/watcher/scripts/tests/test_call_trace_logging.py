"""验证实际人际通话统计日志分级及异常报告，不触碰媒体帧。"""
from pathlib import Path
import subprocess
import tempfile
import unittest
from .test_call_audio_decode import method

ROOT = Path(__file__).resolve().parents[2]

class CallTraceLoggingTest(unittest.TestCase):
    """Watcher UDP 常规统计静默，其他模式保留信息日志，异常仍告警。"""
    def test_actual_trace_levels_and_failure_reporting(self):
        source = (ROOT / 'main/signaling/ur_signaling_call.cc').read_text()
        actual = method(source, 'void TraceAudio()')
        harness = r'''
#include <cassert>
#include <cstdint>
#include <deque>
#include <mutex>
#include <cstdarg>
enum esp_log_level_t { ESP_LOG_WARN=2, ESP_LOG_INFO=3, ESP_LOG_DEBUG=4 };
constexpr const char* kTag="call";
int info=0, debug=0, warn=0;
void emit(esp_log_level_t level,const char*,const char*,...){if(level==ESP_LOG_INFO)++info;else if(level==ESP_LOG_DEBUG)++debug;else if(level==ESP_LOG_WARN)++warn;}
#define ESP_LOGI(tag,format,...) emit(ESP_LOG_INFO,tag,format,##__VA_ARGS__)
#define ESP_LOGW(tag,format,...) emit(ESP_LOG_WARN,tag,format,##__VA_ARGS__)
#define ESP_LOG_LEVEL_LOCAL(level,tag,format,...) emit(level,tag,format,##__VA_ARGS__)
uint64_t clock_us=6000000;uint64_t esp_timer_get_time(){return clock_us;}
int xPortGetCoreID(){return 0;}int xTaskGetCoreID(void*){return 0;}
unsigned uxTaskGetStackHighWaterMark(void*){return 100;}
struct Pcm {unsigned completed_playback_frames()const{return 1;}unsigned pending_playback_frames()const{return 0;}};
struct Bridge {Pcm pcm;const Pcm& underlying()const{return pcm;}};
struct Owner {bool media_active_=true;Bridge* pcm_=nullptr;};
struct Media {
 explicit Media(Owner& owner):owner_(owner){}
 Owner& owner_; bool udp_mode_=true; uint64_t audio_trace_at_=1000;std::mutex decode_trace_mutex_;
 unsigned capture_reads_=1, encode_attempts_=1, encode_failures_=0, decode_attempts_=1, decode_failures_=0;
 uint64_t capture_read_us_=10,encode_time_us_=10,decode_time_us_=10;
 int last_encode_core_=0,last_decode_core_=0,audio_up_=1,audio_up_fail_=0,audio_trace_reported_failures_=0;
 std::deque<int> downlink_;unsigned playback_submits_=0,audio_sends_=0,udp_poll_calls_=0,udp_receive_calls_=0,udp_empty_calls_=0,udp_decrypt_calls_=0;
 uint64_t playback_submit_us_=0,audio_send_us_=0,udp_poll_us_=0,udp_poll_max_us_=0,udp_poll_gap_max_us_=0,udp_receive_us_=0,udp_receive_max_us_=0,udp_empty_us_=0,udp_decrypt_us_=0;
 std::mutex call_encode_mutex_,call_decode_mutex_;bool call_capture_active_=false,call_encode_busy_=false,call_decode_busy_=false;std::deque<int> call_encode_ready_,call_decode_waiting_,call_decode_ready_;void* call_encode_task_=nullptr;unsigned playback_rejected_=0;
 ACTUAL
};
int main(){Bridge bridge;Owner owner{true,&bridge};Media m{owner};m.TraceAudio();
#if CONFIG_UR_SOFTWARE_AEC && !CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
 const int udp_lines=7, rtc_lines=5;
#else
 const int udp_lines=5, rtc_lines=3;
#endif
#if CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
 assert(info==0 && debug==udp_lines && warn==0);
#else
 assert(info==udp_lines && debug==0 && warn==0);
#endif
 info=debug=warn=0;clock_us+=5000000;m.udp_mode_=false;m.TraceAudio();assert(info==rtc_lines && debug==0 && warn==0);
#if CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
 info=debug=warn=0;clock_us+=5000000;m.udp_mode_=true;m.encode_failures_=1;m.audio_up_fail_=1;m.TraceAudio();assert(warn==1 && info==0);
 info=debug=warn=0;clock_us+=5000000;m.TraceAudio();assert(warn==0);
 info=debug=warn=0;clock_us+=5000000;m.decode_failures_=1;m.TraceAudio();assert(warn==1);
 info=debug=warn=0;clock_us+=5000000;m.audio_up_fail_=2;m.TraceAudio();assert(warn==1);
#endif
 info=debug=warn=0;clock_us+=5000000;owner.media_active_=false;m.TraceAudio();assert(info==0 && debug==0 && warn==0);
}
'''.replace('ACTUAL', actual)
        temp_root = ROOT.parents[1] / '.temp'
        temp_root.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=temp_root) as directory:
            binary = Path(directory) / 'trace-logging'
            for watcher in (0, 1):
                for aec in (0, 1):
                    result = subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        f'-DCONFIG_UR_SOFTWARE_AEC={aec}',
                        f'-DCONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER={watcher}',
                        '-x', 'c++', '-', '-o', str(binary)], input=harness, text=True, capture_output=True)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    result = subprocess.run([str(binary)], capture_output=True)
                    self.assertEqual(result.returncode, 0, result.stderr.decode(errors='replace'))

if __name__ == '__main__':
    unittest.main()
