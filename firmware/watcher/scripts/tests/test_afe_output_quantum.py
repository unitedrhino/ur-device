"""回放真实AFE聚帧，核对人际原生块交付、默认60ms恢复和跨块采样顺序。"""
from pathlib import Path
import subprocess
import tempfile
import unittest
from .test_call_audio_decode import method

ROOT = Path(__file__).resolve().parents[2]


class AfeOutputQuantumTest(unittest.TestCase):
    """替换AFE硬件结果，保持生产聚帧、复位与回调逻辑。"""

    def test_low_latency_grouping_and_restore(self):
        source = (ROOT / 'main/audio/engines/afe_audio_engine.cc').read_text()
        setter = ('void AfeAudioEngine::EnableLowLatencyOutput(bool enable) { low_latency_output_.store(enable); }'
                  if 'void AfeAudioEngine::EnableLowLatencyOutput(' not in source
                  else method(source, 'void AfeAudioEngine::EnableLowLatencyOutput('))
        actual = '\n'.join([setter, method(source, 'void AfeAudioEngine::HandleVoiceResult('),
                            method(source, 'void AfeAudioEngine::OutputRawAudio(')])
        code = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <vector>
constexpr int VAD_SPEECH=1,VAD_SILENCE=0;
struct afe_fetch_result_t {int vad_state=0;int data_size=0;int16_t*data=nullptr;};
struct Codec {int input_channels(){return 1;}};
struct AfeAudioEngine {
 std::atomic<bool> low_latency_output_{false},output_reset_pending_{false};
 int frame_samples_=960;bool is_speaking_=false;Codec codec;Codec*codec_=&codec;
 std::vector<int16_t>output_buffer_;
 std::function<void(bool)>vad_state_change_callback_;
 std::function<void(std::vector<int16_t>&&)>output_callback_;
 void EnableLowLatencyOutput(bool);
 void HandleVoiceResult(const afe_fetch_result_t*);
 void OutputRawAudio(const std::vector<int16_t>&);
};
ACTUAL
int main(){
 for(bool raw:{false,true}){
  AfeAudioEngine engine;std::vector<std::vector<int16_t>>out;
  engine.output_callback_=[&](std::vector<int16_t>&&frame){out.push_back(std::move(frame));};
  auto feed=[&](int begin){std::vector<int16_t>pcm(512);for(int i=0;i<512;++i)pcm[i]=begin+i;
   if(raw)engine.OutputRawAudio(pcm);else{afe_fetch_result_t f{0,1024,pcm.data()};engine.HandleVoiceResult(&f);}};
  engine.EnableLowLatencyOutput(true);feed(0);
  assert(out.size()==1&&out[0].size()==512&&"native AFE output was split or delayed by extra aggregation");
  feed(512);assert(out.size()==2);
  for(int i=0;i<1024;++i)assert(out[i/512][i%512]==i);
  // 重复启用保持原生块连续，不重复拆分或申请额外320样本帧。
  engine.EnableLowLatencyOutput(true);feed(1024);assert(out.size()==3);
  for(int i=1024;i<1536;++i)assert(out[i/512][i%512]==i);
  // 退出仅在输出任务清尾；默认60ms恢复，旧人际尾样本不进入下一段。
  engine.EnableLowLatencyOutput(false);out.clear();feed(2000);assert(out.empty());
  feed(2512);assert(out.size()==1&&out[0].size()==960);
  for(int i=0;i<960;++i)assert(out[0][i]==2000+i);
 }
}
'''.replace('ACTUAL', actual)
        with tempfile.TemporaryDirectory(prefix='watcher-afe-quantum-') as directory:
            binary = str(Path(directory) / 'quantum')
            built = subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                    '-x', 'c++', '-', '-o', binary], input=code,
                                   text=True, capture_output=True, timeout=30)
            self.assertEqual(built.returncode, 0, built.stderr)
            result = subprocess.run([binary], text=True, capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_capture_close_rejects_inflight_short_legacy_frame(self):
        source = (ROOT / 'main/audio/audio_service.cc').read_text()
        callback = method(source, 'audio_engine_->OnOutput([this]') + ');'
        code = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <vector>
#define CONFIG_UR_AI_SIGNALING 1
#define OPUS_FRAME_DURATION_MS 60
constexpr int kAudioTaskTypeEncodeToSendQueue=0;
struct Queue:std::deque<std::vector<int16_t>> {bool full()const{return size()==48;}};
struct Notify{void notify_one(){}};
struct Engine {std::function<void(std::vector<int16_t>&&)>cb;void OnOutput(decltype(cb) f){cb=f;}};
struct Service {
 Engine engine;Engine*audio_engine_=&engine;std::mutex audio_queue_mutex_;
 bool signaling_capture_enabled_=true,signaling_mic_muted_=false;Queue signaling_capture_queue_;
 std::atomic<unsigned>signaling_output_samples_{0},signaling_drop_samples_{0};Notify signaling_capture_cv_;
 std::vector<size_t>legacy;
 void PushTaskToEncodeQueue(int,std::vector<int16_t>data){legacy.push_back(data.size());}
 void Install(){CALLBACK}
};
int main(){Service s;s.Install();s.engine.cb(std::vector<int16_t>(512,1));
 assert(s.signaling_capture_queue_.size()==1&&s.legacy.empty());
 s.signaling_capture_enabled_=false;s.engine.cb(std::vector<int16_t>(512,2));
 assert(s.legacy.empty()&&"inflight human native chunk escaped into legacy 60ms encoder");
 s.engine.cb(std::vector<int16_t>(960,3));assert(s.legacy==std::vector<size_t>{960});
}
'''.replace('CALLBACK', callback)
        with tempfile.TemporaryDirectory(prefix='watcher-short-close-') as directory:
            binary = str(Path(directory) / 'close')
            built = subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                    '-x', 'c++', '-', '-o', binary], input=code,
                                   text=True, capture_output=True, timeout=30)
            self.assertEqual(built.returncode, 0, built.stderr)
            result = subprocess.run([binary], text=True, capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
