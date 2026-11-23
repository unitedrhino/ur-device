"""执行真实PCM提交与AEC参考路径，验证原生16k不调用转换且保留24k回退。"""
from pathlib import Path
import subprocess
import tempfile
import unittest
from .test_call_audio_decode import method

ROOT = Path(__file__).resolve().parents[2]


class SignalingNativeRateTest(unittest.TestCase):
    """同一生产方法分别连接16k和24k边界，不用包数代替PCM内容检查。"""

    def test_native_and_legacy_pcm_reference(self):
        source = (ROOT / 'main/audio/audio_service.cc').read_text()
        submit = method(source, 'bool AudioService::PushSignalingPcm(')
        reference = method(source, 'if (audio_engine_ != nullptr && !signaling_task.pcm.empty())')
        code = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>
#define ESP_LOGW(...) ((void)0)
using esp_ae_sample_t=uint8_t*;
constexpr int ESP_AE_ERR_OK=0,ESP_AUDIO_SAMPLE_RATE_16K=16000,kAudioTaskTypeDecodeToPlaybackQueue=1;
struct Config {int in,out,channels;};using esp_ae_rate_cvt_cfg_t=Config;
#define RATE_CVT_CFG(in,out,channels) (Config{in,out,channels})
static int opens=0,processes=0;
int esp_ae_rate_cvt_open(Config* cfg,void** out){++opens;*out=reinterpret_cast<void*>(cfg->out==24000?1:2);return 0;}
int esp_ae_rate_cvt_get_max_out_sample_num(void* h,uint32_t,uint32_t* out){*out=h==reinterpret_cast<void*>(1)?480:320;return 0;}
int esp_ae_rate_cvt_process(void*,uint8_t*,uint32_t,uint8_t* out,uint32_t* count){++processes;std::fill_n(reinterpret_cast<int16_t*>(out),*count,7);return 0;}
struct AudioTask {int type;uint32_t timestamp=0,playback_id=0,media_position_ms=0,signaling_generation=0;std::vector<int16_t> pcm;};
struct Queue {std::deque<AudioTask> data;bool full(){return data.size()>=8;}void push_back(AudioTask task){data.push_back(std::move(task));}};
struct Codec {int rate;int output_sample_rate(){return rate;}};
struct Engine {std::vector<int16_t> received;void FeedReference(std::vector<int16_t> pcm){received=std::move(pcm);}};
struct AudioService {
 std::mutex audio_queue_mutex_,signaling_output_resampler_mutex_,signaling_reference_resampler_mutex_;
 std::condition_variable audio_queue_cv_;std::atomic<bool> service_stopped_{false};
 Queue signaling_playback_queue_;Codec* codec_;Engine* audio_engine_;
 uint32_t signaling_playback_generation_=1;
 void* signaling_output_resampler_=nullptr;void* signaling_reference_resampler_=nullptr;
 bool PushSignalingPcm(std::vector<int16_t>&&,uint32_t);
 void Reference(AudioTask& signaling_task){REFERENCE}
};
SUBMIT
int main(){
 for(int rate:{16000,24000}){
  opens=processes=0;Codec codec{rate};Engine engine;AudioService audio;audio.codec_=&codec;audio.audio_engine_=&engine;
  auto original=std::vector<int16_t>(320,7);
  assert(audio.PushSignalingPcm(std::vector<int16_t>(original),78));
  auto& frame=audio.signaling_playback_queue_.data.front();
  assert(frame.timestamp==78 && frame.pcm.size()==static_cast<unsigned>(rate/50));
  audio.Reference(frame);assert(engine.received==original);
  assert(opens==(rate==16000?0:2) && processes==(rate==16000?0:2));
 }
}
'''.replace('REFERENCE', reference).replace('SUBMIT', submit)
        with tempfile.TemporaryDirectory(prefix='watcher-native-rate-') as directory:
            binary = str(Path(directory) / 'native')
            built = subprocess.run(['g++', '-std=c++17', '-pthread', '-Wall', '-Wextra',
                                    '-x', 'c++', '-', '-o', binary], input=code,
                                   text=True, capture_output=True, timeout=30)
            self.assertEqual(built.returncode, 0, built.stderr)
            result = subprocess.run([binary], text=True, capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
