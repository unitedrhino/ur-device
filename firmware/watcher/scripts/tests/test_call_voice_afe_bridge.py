"""执行 AFE 试验采样桥，确保旧 fetch 不会流入后续来电。"""
from pathlib import Path
import subprocess
import tempfile
import unittest
from .test_call_audio_decode import method

ROOT = Path(__file__).resolve().parents[2]


class CallVoiceAfeBridgeTest(unittest.TestCase):
    """替换 AFE 设备边界，保留生产代码的采样代次和回调。"""

    def test_old_fetch_and_disabled_capture_are_rejected(self):
        source = (ROOT / 'main/audio/engines/afe_audio_engine.cc').read_text()
        actual = '\n'.join(method(source, sig) for sig in (
            'void AfeAudioEngine::SetCallVoiceFrameHook(',
            'void AfeAudioEngine::SetCallVoiceCapture(',
            'void AfeAudioEngine::ForwardCallVoiceFrame(',
            'void AfeAudioEngine::ApplyPendingReset(',
        ))
        code = r'''
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>
#include <utility>
#define CONFIG_UR_SPARKBOT_VOICE_MODEL_TEST 1
struct Afe {int resets=0;std::function<void()> during_reset;void reset_buffer(void*){++resets;if(during_reset)during_reset();}};
struct AfeAudioEngine {
 std::atomic<bool> call_voice_enabled_{false},reset_pending_{false};
 std::atomic<std::uint32_t> control_generation_{0},call_voice_epoch_{0},call_voice_reset_epoch_{0},call_voice_generation_{0};
 std::function<void(const int16_t*,std::size_t,std::uint32_t)> call_voice_frame_hook_;
 std::mutex input_buffer_mutex_;std::vector<int> input_buffer_;Afe storage;Afe* afe_iface_=&storage;void* afe_data_=this;bool is_speaking_=true;
 void SetCallVoiceFrameHook(std::function<void(const int16_t*,std::size_t,std::uint32_t)>);
 void SetCallVoiceCapture(bool,std::uint32_t);
 void ForwardCallVoiceFrame(const int16_t*,std::size_t,std::uint32_t);void ApplyPendingReset();
};
''' + actual + r'''
int main(){
 AfeAudioEngine a;int calls=0;std::uint32_t token=0;int16_t frame[]={1,2};
 a.SetCallVoiceFrameHook([&](const int16_t* p,std::size_t n,std::uint32_t t){assert(p==frame&&n==2);++calls;token=t;});
 a.ForwardCallVoiceFrame(frame,2,0);assert(calls==0);
 a.SetCallVoiceCapture(true,77);auto old=a.control_generation_.load();assert(a.reset_pending_);
 a.ForwardCallVoiceFrame(frame,2,old-1);assert(calls==0);
 a.ForwardCallVoiceFrame(frame,2,old);assert(calls==0); // 未完成 AFE 清理不能读取旧音频。
 a.ApplyPendingReset();assert(a.storage.resets==1);
 a.ForwardCallVoiceFrame(frame,2,old);assert(calls==1&&token==77);
 a.SetCallVoiceCapture(false,0);a.ForwardCallVoiceFrame(frame,2,old);assert(calls==1);
 a.SetCallVoiceCapture(true,88);auto current=a.control_generation_.load();
 a.ForwardCallVoiceFrame(frame,2,old);assert(calls==1);
 a.ForwardCallVoiceFrame(nullptr,2,current);a.ForwardCallVoiceFrame(frame,0,current);assert(calls==1);
 a.ForwardCallVoiceFrame(frame,2,current);assert(calls==1);
 a.ApplyPendingReset();assert(a.storage.resets==2);
 a.ForwardCallVoiceFrame(frame,2,current);assert(calls==2&&token==88);
 // 清理期间新来电启用：前一次清理不能被错记为覆盖新代次。
 a.SetCallVoiceCapture(true,99);
 bool rearmed=false;a.storage.during_reset=[&]{if(!rearmed){rearmed=true;a.SetCallVoiceCapture(true,100);}};
 a.ApplyPendingReset();auto newest=a.control_generation_.load();
 a.ForwardCallVoiceFrame(frame,2,newest);assert(calls==2);
 a.ApplyPendingReset();a.ForwardCallVoiceFrame(frame,2,newest);assert(calls==3&&token==100);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / 'bridge')
            result = subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                     '-x', 'c++', '-', '-o', binary], input=code,
                                    text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([binary], check=True)


if __name__ == '__main__':
    unittest.main()
