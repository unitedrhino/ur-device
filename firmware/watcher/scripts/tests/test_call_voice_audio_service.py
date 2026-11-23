"""执行音频服务试验入口，验证空闲约束与识别停止顺序。"""
from pathlib import Path
import subprocess
import tempfile
import unittest
from .test_call_audio_decode import method

ROOT = Path(__file__).resolve().parents[2]


class CallVoiceAudioServiceTest(unittest.TestCase):
    """替换模型和任务边界，不模拟真实 AFE 或语音识别。"""

    def test_capture_only_begins_in_idle_wake_mode(self):
        source = (ROOT / 'main/audio/audio_service.cc').read_text()
        actual = '\n'.join(method(source, sig) for sig in (
            'bool AudioService::BeginCallVoiceCapture(',
            'void AudioService::StopCallVoiceCapture(',
            'bool AudioService::IsCallVoiceCaptureBusy(',
        ))
        code = r'''
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <new>
#include <utility>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
void* heap_caps_malloc(std::size_t n,int){return ::operator new(n);}
void heap_caps_free(void* p){::operator delete(p);}
struct srmodel_list_t{};
struct AfeAudioEngine{bool armed=false;std::uint32_t token=0;void SetCallVoiceCapture(bool value,std::uint32_t gen){armed=value;token=gen;}};
struct CallVoiceWorker {
 using Completion=std::function<void(int)>;bool busy=false;int stops=0;std::uint32_t generation=0;
 bool Initialize(){return true;}
 bool Begin(srmodel_list_t*,std::uint32_t g,Completion){if(busy)return false;busy=true;generation=g;return true;}
 void RequestStop(){++stops;}
 bool Busy()const{return busy;}
};
struct AudioService {
 std::atomic<bool> service_stopped_{false};bool audio_engine_initialized_=true,wake=true,processing=false;
 srmodel_list_t models;srmodel_list_t* models_list_=&models;
 std::unique_ptr<AfeAudioEngine> audio_engine_=std::make_unique<AfeAudioEngine>();
 std::atomic<CallVoiceWorker*> call_voice_worker_{nullptr};bool call_voice_capture_armed_=false;
 bool IsWakeWordRunning()const{return wake;}bool IsAudioProcessorRunning()const{return processing;}
 bool BeginCallVoiceCapture(std::uint32_t,CallVoiceWorker::Completion);
 void StopCallVoiceCapture();bool IsCallVoiceCaptureBusy()const;
};
''' + actual + r'''
int main(){
 AudioService a;auto done=[](int){};
 a.wake=false;assert(!a.BeginCallVoiceCapture(1,done));assert(!a.call_voice_worker_.load());
 a.wake=true;a.processing=true;assert(!a.BeginCallVoiceCapture(1,done));
 a.processing=false;a.service_stopped_=true;assert(!a.BeginCallVoiceCapture(1,done));
 a.service_stopped_=false;assert(a.BeginCallVoiceCapture(17,done));
 auto* worker=a.call_voice_worker_.load();assert(worker&&worker->generation==17);
 assert(a.audio_engine_->armed&&a.audio_engine_->token==17&&a.IsCallVoiceCaptureBusy());
 assert(!a.BeginCallVoiceCapture(18,done));assert(a.audio_engine_->token==17);
 a.StopCallVoiceCapture();assert(!a.audio_engine_->armed&&worker->stops==1);
 a.StopCallVoiceCapture();assert(worker->stops==1);
 worker->busy=false;assert(!a.IsCallVoiceCaptureBusy());
 worker->~CallVoiceWorker();heap_caps_free(worker);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / 'service')
            result = subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                     '-x', 'c++', '-', '-o', binary], input=code,
                                    text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([binary], check=True)

    def test_mono_injection_matches_hardware_reference_and_releases_queue_lock(self):
        """执行真实注入分支，验证M／R顺序和AFE回调可再次领取队列锁；不模拟声学效果。"""
        source = (ROOT / 'main/audio/audio_service.cc').read_text()
        marker = source.index('/* 调试注入优先替代麦克风读取')
        start = source.index('            {', marker)
        actual = source[start:source.index('#endif', start)]
        code = r'''
#include <cassert>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>
// Codec 仅替换硬件能力查询，注入分支始终来自维护源码。
struct Codec {
 bool reference=false; // reference 表示是否存在独立回采声道。
 int channels=1; // channels 表示实际输入交错通道数。
 bool input_reference(){return reference;}
 int input_channels(){return channels;}
};
// Engine 在另一个线程尝试领取队列锁，安全验证Feed没有被队列锁包围。
struct Engine {
 std::mutex* queue_mutex=nullptr; // queue_mutex 指向采集队列的真实互斥边界。
 std::vector<std::vector<int16_t>> frames; // frames 保存实际交付给AFE的采样。
 void Feed(std::vector<int16_t>&& data){
  bool available=false; // available 由探针线程写入，join后读取。
  std::thread probe([&]{
   if(queue_mutex->try_lock()){available=true;queue_mutex->unlock();}
  });
  probe.join();assert(available);frames.push_back(std::move(data));
 }
};
// Service 只替换队列和设备边界，Tick保留生产领取及声道适配代码。
struct Service {
 std::mutex audio_queue_mutex_; // audio_queue_mutex_ 保护注入队列。
 bool signaling_capture_enabled_=false; // signaling_capture_enabled_ 控制调试受理。
 std::deque<std::vector<int16_t>> signaling_mic_inject_queue_; // 队列保留单声道合同。
 Codec codec;Codec* codec_=&codec; // codec_ 提供本轮板级输入格式。
 Engine engine;Engine* audio_engine_=&engine; // audio_engine_ 接收真实分支的输出。
 Service(){engine.queue_mutex=&audio_queue_mutex_;}
 void Tick(){
  std::vector<int16_t> data; // data 与实际采集任务相同，暂存硬件M／R交错块。
  do {
ACTUAL
  }while(false);
 }
};
int main(){
 Service service; // service 模拟同一个采集任务的连续领取。
 service.signaling_mic_inject_queue_.push_back({1,-2,32767,-32768});
 service.Tick();assert(service.engine.frames.empty());
 assert(service.signaling_mic_inject_queue_.size()==1);
 service.signaling_capture_enabled_=true;service.Tick();
 assert((service.engine.frames.back()==std::vector<int16_t>{1,-2,32767,-32768}));
 service.codec.reference=true;service.codec.channels=2;
 service.signaling_mic_inject_queue_.push_back({7,-8});service.Tick();
 assert((service.engine.frames.back()==std::vector<int16_t>{7,0,-8,0}));
 service.codec.reference=false;
 service.signaling_mic_inject_queue_.push_back({3,4});service.Tick();
 assert((service.engine.frames.back()==std::vector<int16_t>{3,4}));
 service.Tick();assert(service.engine.frames.size()==3);
}
'''.replace('ACTUAL', actual)
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / 'mono-reference-injection')
            result = subprocess.run(['c++', '-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror',
                                     '-x', 'c++', '-', '-o', binary], input=code,
                                    text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([binary], check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()
