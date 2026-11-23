"""回放信令主循环等待策略：人际媒体结果可提前唤醒，同时保留调度让出和AI原节拍。"""
from pathlib import Path
import subprocess
import tempfile
import unittest
from .test_call_audio_decode import method

ROOT = Path(__file__).resolve().parents[2]


class CallEventWakeTest(unittest.TestCase):
    """仅替代FreeRTOS等待，执行生产等待策略并检查两条主循环入口。"""

    def test_media_notification_and_default_polling(self):
        source = (ROOT / 'main/signaling/ur_signaling_protocol.cc').read_text()
        signature = 'void SignalingProtocol::WaitForNextPoll()'
        actual = (method(source, signature) if signature in source else
                  signature + ' { vTaskDelay(kTaskPollPeriod); }')
        code = r'''
#include <cassert>
#include <cstdint>
using TickType_t=unsigned;
constexpr TickType_t kTaskPollPeriod=10;
constexpr bool pdTRUE=true;
unsigned slept=0,waits=0;bool notification=false;
void vTaskDelay(unsigned ticks){assert(ticks>0);slept+=ticks;}
unsigned ulTaskNotifyTake(bool clear,unsigned timeout){assert(clear&&timeout==10);++waits;return notification?1:0;}
struct UrSignalingCall{bool busy=false;static UrSignalingCall& Instance(){static UrSignalingCall c;return c;}bool Busy()const{return busy;}};
struct SignalingProtocol{bool call_appearance_ready_=false;void WaitForNextPoll();};
ACTUAL
int main(){SignalingProtocol p;
 p.WaitForNextPoll();assert(slept==10&&waits==0);
 slept=waits=0;p.call_appearance_ready_=true;
 p.WaitForNextPoll();assert(slept==10&&waits==0);
 slept=waits=0;UrSignalingCall::Instance().busy=true;notification=true;
 p.WaitForNextPoll();
#if CONFIG_UR_AI_SIGNALING_WEBRTC
 assert(waits==1&&slept==1&&"media ready still waits a fixed ten milliseconds");
 slept=waits=0;notification=false;p.WaitForNextPoll();assert(waits==1&&slept==0);
#else
 assert(waits==0&&slept==10);
#endif
}
'''.replace('ACTUAL', actual)
        for enabled in (0, 1):
            with self.subTest(webrtc=enabled), tempfile.TemporaryDirectory(prefix='watcher-wake-') as directory:
                binary = str(Path(directory) / 'wake')
                built = subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                        '-DCONFIG_UR_AI_SIGNALING_WEBRTC=' + str(enabled),
                                        '-x', 'c++', '-', '-o', binary], input=code,
                                       text=True, capture_output=True, timeout=30)
                self.assertEqual(built.returncode, 0, built.stderr)
                result = subprocess.run([binary], text=True, capture_output=True, timeout=5)
                self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(source.count('WaitForNextPoll();'), 2)
