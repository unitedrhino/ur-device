"""回放真实播放交接和AEC参考段，验证慢重采样不占共享队列或阻塞独立参考实例。"""
from pathlib import Path
import subprocess
import tempfile
import unittest
from .test_call_audio_decode import method

ROOT = Path(__file__).resolve().parents[2]


class SignalingOutputProgressTest(unittest.TestCase):
    """硬件重采样可控阻塞，生产锁策略和票据读取保持原函数。"""

    def test_done_and_reference_progress_while_submit_resamples(self):
        source = (ROOT / 'main/audio/audio_service.cc').read_text()
        methods = '\n'.join(method(source, signature) for signature in [
            'bool AudioService::PushSignalingPcm(', 'bool AudioService::PopSignalingPlaybackDone(',
            'void AudioService::ResetSignalingPlayback(',
        ])
        reference = method(source, 'if (audio_engine_ != nullptr && !signaling_task.pcm.empty())')
        code = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <future>
#include <mutex>
#include <thread>
#include <vector>
using namespace std::chrono_literals;
#define ESP_LOGW(...) ((void)0)
using esp_ae_sample_t=uint8_t*;
constexpr int ESP_AE_ERR_OK=0,ESP_AUDIO_SAMPLE_RATE_16K=16000,kAudioTaskTypeDecodeToPlaybackQueue=1;
struct Config {int in,out,channels;};
using esp_ae_rate_cvt_cfg_t=Config;
#define RATE_CVT_CFG(in,out,channels) (Config{in,out,channels})
// Gate只替代算法执行时间，句柄1/2分别是两个独立重采样器。
static std::promise<void> entered,release;
static auto released=release.get_future().share();
int esp_ae_rate_cvt_open(Config*,void**){return ESP_AE_ERR_OK;}
int esp_ae_rate_cvt_get_max_out_sample_num(void* handle,uint32_t,uint32_t* out){*out=handle==reinterpret_cast<void*>(1)?480:320;return ESP_AE_ERR_OK;}
int esp_ae_rate_cvt_process(void* handle,uint8_t*,uint32_t,uint8_t* out,uint32_t* count){
    static bool entered_once=false;
    if(handle==reinterpret_cast<void*>(1)){if(!entered_once){entered_once=true;entered.set_value();}released.wait();}
    std::fill_n(reinterpret_cast<int16_t*>(out),*count,3);return ESP_AE_ERR_OK;
}
template<class T> struct Queue {
    std::deque<T> values;
    bool full()const{return values.size()>=8;}
    bool empty()const{return values.empty();}
    T& front(){return values.front();}
    void pop_front(){values.pop_front();}
    void push_back(T value){values.push_back(std::move(value));}
    void clear(){values.clear();}
};
struct AudioTask {int type;uint32_t timestamp=0,playback_id=0,media_position_ms=0,signaling_generation=0;std::vector<int16_t> pcm;};
struct Codec {int output_sample_rate(){return 24000;}};
struct Engine {void FeedReference(std::vector<int16_t> pcm){assert(pcm.size()==320);}};
struct AudioService {
    std::mutex audio_queue_mutex_,signaling_output_resampler_mutex_,signaling_reference_resampler_mutex_;
    std::condition_variable audio_queue_cv_;
    std::atomic<bool> service_stopped_{false};
    Queue<AudioTask> signaling_playback_queue_;Queue<uint32_t> signaling_done_queue_;
    uint32_t signaling_playback_generation_=1;bool signaling_output_in_flight_=false;
    Codec codec;Codec* codec_=&codec;Engine engine;Engine* audio_engine_=&engine;
    void* signaling_output_resampler_=reinterpret_cast<void*>(1);
    void* signaling_reference_resampler_=reinterpret_cast<void*>(2);
    bool PushSignalingPcm(std::vector<int16_t>&&,uint32_t);
    bool PopSignalingPlaybackDone(uint32_t&);
    void ResetSignalingPlayback();
    void Reference(){AudioTask signaling_task;signaling_task.pcm.assign(480,1);REFERENCE}
};
METHODS
int main(){
    AudioService audio;audio.signaling_done_queue_.push_back(77);
    auto began=entered.get_future();
    auto producer=std::async(std::launch::async,[&]{return audio.PushSignalingPcm(std::vector<int16_t>(320,1),78);});
    assert(began.wait_for(1s)==std::future_status::ready);
    auto done=std::async(std::launch::async,[&]{uint32_t ticket=0;return audio.PopSignalingPlaybackDone(ticket)&&ticket==77;});
    auto reference=std::async(std::launch::async,[&]{audio.Reference();return true;});
    const bool queue_progress=done.wait_for(200ms)==std::future_status::ready;
    const bool ref_progress=reference.wait_for(200ms)==std::future_status::ready;
    release.set_value();
    assert(producer.get());assert(done.get());assert(reference.get());
    assert(queue_progress && "playback completion waited for unrelated resampling");
    assert(ref_progress && "AEC reference waited for independent downlink resampler");
    assert(audio.signaling_playback_queue_.front().pcm.size()==480);
    assert(audio.signaling_playback_queue_.front().timestamp==78);
    audio.service_stopped_=true;
    assert(!audio.PushSignalingPcm(std::vector<int16_t>(320,1),79));

    // 新会话不应看到旧的待播帧或完成票据，提交帧必须携带新代次。
    AudioService reset;
    reset.signaling_playback_queue_.push_back(AudioTask{});
    reset.signaling_done_queue_.push_back(66);
    reset.ResetSignalingPlayback();
    assert(reset.signaling_playback_queue_.empty() && reset.signaling_done_queue_.empty());
    assert(reset.signaling_playback_generation_==2);
    assert(reset.PushSignalingPcm(std::vector<int16_t>(320,1),80));
    assert(reset.signaling_playback_queue_.front().signaling_generation==2);
}
'''.replace('METHODS', methods).replace('REFERENCE', reference)
        with tempfile.TemporaryDirectory(prefix='watcher-output-progress-') as directory:
            binary = str(Path(directory) / 'progress')
            built = subprocess.run(['g++', '-std=c++17', '-pthread', '-Wall', '-Wextra',
                                    '-x', 'c++', '-', '-o', binary], input=code,
                                   text=True, capture_output=True, timeout=30)
            self.assertEqual(built.returncode, 0, built.stderr)
            result = subprocess.run([binary], text=True, capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_slow_diagnostics_do_not_hold_playback_queue(self):
        """实际播放尾段的慢日志不能扣住采集锁，也不能延后已写出帧的完成票据。"""
        source = (ROOT / 'main/audio/audio_service.cc').read_text()
        start = source.index('            const auto duplex_reference_done =')
        end = source.index('            continue;', start)
        tail = '\n'.join(line for line in source[start:end].splitlines()
                         if not line.lstrip().startswith('#'))
        code = r'''
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <future>
#include <mutex>
using namespace std::chrono_literals;
static std::promise<void> logging, release;
static auto released=release.get_future().share();
void slow_log(){logging.set_value();released.wait();}
#define ESP_LOGI(...) slow_log()
int64_t esp_timer_get_time(){return 2000001;}
int xPortGetCoreID(){return 1;}
int xTaskGetCoreID(void*){return 1;}
template<class T> struct Queue:std::deque<T>{bool full()const{return this->size()>=8;}};
struct AudioService {
    std::mutex audio_queue_mutex_;std::condition_variable audio_queue_cv_;
    Queue<uint32_t> signaling_done_queue_;Queue<int> signaling_playback_queue_;
    uint32_t signaling_playback_generation_=1;bool signaling_output_in_flight_=false;
    bool output_in_flight_=true;
    void Tail(){
        std::unique_lock<std::mutex> lock(audio_queue_mutex_,std::defer_lock);
        struct {uint32_t timestamp=42,signaling_generation=1;} signaling_task;
        const int64_t duplex_write_at=1,duplex_reference_at=1001;
        TAIL
    }
};
int main(){
    AudioService audio;auto began=logging.get_future();
    auto output=std::async(std::launch::async,[&]{audio.Tail();});
    assert(began.wait_for(1s)==std::future_status::ready);
    auto producer=std::async(std::launch::async,[&]{
        std::lock_guard<std::mutex> lock(audio.audio_queue_mutex_);
        return !audio.output_in_flight_ && audio.signaling_done_queue_.size()==1 &&
               audio.signaling_done_queue_.front()==42;
    });
    bool progressed=producer.wait_for(200ms)==std::future_status::ready;
    release.set_value();output.get();bool completed=producer.get();
    assert(progressed && "slow diagnostics held the shared capture/playback mutex");
    assert(completed && "completed I2S frame was not published before diagnostics");
}
'''.replace('TAIL', tail)
        with tempfile.TemporaryDirectory(prefix='watcher-output-log-') as directory:
            binary = str(Path(directory) / 'log-progress')
            built = subprocess.run(['g++', '-std=c++17', '-pthread', '-Wall', '-Wextra',
                                    '-x', 'c++', '-', '-o', binary], input=code,
                                   text=True, capture_output=True, timeout=30)
            self.assertEqual(built.returncode, 0, built.stderr)
            result = subprocess.run([binary], text=True, capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
