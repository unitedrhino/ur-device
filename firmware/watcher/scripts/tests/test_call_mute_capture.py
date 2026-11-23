"""回放实际静音恢复分支与真实PCM桥，验证静音样本不重播且反向播放不被清空。"""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest
from .test_call_audio_decode import method

ROOT = Path(__file__).resolve().parents[2]
RUNTIME = ROOT.parent / 'device-runtime'


class CallMuteCaptureTest(unittest.TestCase):
    """只替代硬件来源、编码作废和控制出口，保持生产组帧及恢复逻辑。"""

    def test_resume_discards_all_muted_capture_layers_and_preserves_playback(self):
        source = (ROOT / 'main/signaling/ur_signaling_call.cc').read_text()
        restart = (method(source, 'bool RestartCapture()') if 'bool RestartCapture()' in source
                   else 'bool RestartCapture(){return true;}')
        case = source[source.index('        case kUiToggleMic:'):
                      source.index('        case kUiToggleCamera:')]
        pcm = (ROOT / 'main/signaling/ur_signaling_pcm.cc').read_text()
        actual_pcm = '\n'.join(method(pcm, name) for name in [
            'bool UrSignalingPcm::begin_capture(', 'void UrSignalingPcm::end_capture(',
            'void UrSignalingPcm::DrainPendingCapture(', 'PcmResult UrSignalingPcm::read_frame('])
        code = r'''
#include <cassert>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>
#include "ur/device/buffered_pcm.hpp"
#define CONFIG_UR_SOFTWARE_AEC 1
#define ESP_LOGW(...) ((void)0)
struct AudioSource {
 std::deque<std::vector<int16_t>> chunks;
 void EnableSignalingCapture(bool active){if(!active)chunks.clear();}
 bool PopSignalingPcm(std::vector<int16_t>&out){if(chunks.empty())return false;out=std::move(chunks.front());chunks.pop_front();return true;}
};
namespace ur::device {
enum class HumanCallState{Connected};
struct UrSignalingPcm {
 AudioSource&audio_;std::mutex capture_mutex_;BufferedPcm buffered_{{16000,1,20}};
 std::string input_id_;std::deque<int16_t>capture_buffer_;
 explicit UrSignalingPcm(AudioSource&a):audio_(a){}
 bool begin_capture(std::string_view);void end_capture();void DrainPendingCapture();
 PcmResult read_frame(std::vector<int16_t>&);
};
PCM
}
using namespace ur::device;
struct Owner {UrSignalingPcm*pcm_;bool media_active_=true;};
struct PeerMedia {
 Owner&owner_;int resets=0;std::vector<int>ready_{11};
 void ResetCallEncoding(){++resets;ready_.clear();}
 RESTART
};
struct Client {
 int controls=0;std::string track;bool paused=false;
 bool track_control_pending(){return false;}
 void control_track(const char*t,bool p,uint64_t){++controls;track=t;paused=p;}
};
struct Ui {
 enum{kUiToggleMic};Client*client_;PeerMedia*media_;bool mic_paused_=true,voice_call_=false;
 void NotifyTrackState(){}
 void Toggle(){const auto state=HumanCallState::Connected;uint64_t now=1;switch(kUiToggleMic){CASE}}
};
int main(){
 for(bool voice:{false,true}){
  AudioSource source;UrSignalingPcm pcm(source);assert(pcm.begin_capture("mic-1"));
  assert(pcm.buffered_.captured("mic-1",std::vector<int16_t>(320,10)));
  pcm.capture_buffer_.assign(192,30);source.chunks.assign(48,std::vector<int16_t>(512,20));
  assert(pcm.buffered_.begin_playback("reply",1));assert(pcm.buffered_.write_frame(std::vector<int16_t>(320,2),1));
  Owner owner{&pcm};PeerMedia media{owner};Client client;Ui ui{&client,&media,true,voice};ui.Toggle();
  assert(!ui.mic_paused_&&client.controls==1&&!client.paused);
  assert(client.track==(voice?"voice":"mic-1"));
  assert(source.chunks.empty()&&pcm.capture_buffer_.empty()&&"muted AFE/partial samples survived resume");
  assert(media.resets==1&&media.ready_.empty());
  assert(pcm.buffered_.pending_playback_frames()==1&&"resume cleared the independent speaker stream");
  source.chunks.push_back(std::vector<int16_t>(512,777));std::vector<int16_t>frame;
  assert(pcm.read_frame(frame)==PcmResult::Frame&&frame==std::vector<int16_t>(320,777));
  // 恢复失败必须保持静音；不能向平台宣告实际没有开始的采集。
  ui.Toggle();assert(ui.mic_paused_&&client.controls==2);
  owner.media_active_=false;ui.Toggle();assert(ui.mic_paused_&&client.controls==2);
 }
}
'''.replace('PCM', actual_pcm).replace('RESTART', restart).replace('CASE', case)
        with tempfile.TemporaryDirectory(prefix='watcher-mute-resume-') as directory:
            binary = str(Path(directory) / 'mute')
            flags = (['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
                     if os.environ.get('CALL_TEST_SANITIZE') else [])
            built = subprocess.run(['g++', *flags, '-std=c++17', '-pthread', '-Wall', '-Wextra',
                                    '-I', str(RUNTIME / 'include'), '-x', 'c++', '-',
                                    str(RUNTIME / 'core/buffered_pcm.cpp'), '-o', binary],
                                   input=code, text=True, capture_output=True, timeout=30)
            self.assertEqual(built.returncode, 0, built.stderr)
            result = subprocess.run([binary], text=True, capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
