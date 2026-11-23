"""执行真实异步解码及播放方法，覆盖阻塞、取消代次、背压、排空和关闭。"""
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def method(source, signature):
    """提取生产函数，算法和队列控制流只保留一份。"""
    start = source.index(signature)
    return source[start:source.index('\n}\n', start) + 3]


class AsyncDecodeTest(unittest.TestCase):
    """用主机线程替换RTOS任务，只模拟驱动和阻塞的算法端口。"""

    def test_cancel_backpressure_and_close_during_decode(self):
        source = (ROOT / 'main/signaling/ur_signaling_webrtc_media.cc').read_text()
        header = (ROOT / 'main/signaling/ur_signaling_webrtc_media.h').read_text()
        start = header.index('    // DecodeRequest')
        fields = header[start:header.index('#endif', start)]
        constants = source.split('namespace {', 1)[1].split('}  // namespace', 1)[0]
        methods = '\n'.join(method(source, name) for name in [
            'bool WebRtcVoiceMedia::start_playback(', 'void WebRtcVoiceMedia::stop_playback(',
            'void WebRtcVoiceMedia::PumpPlayback()', 'bool WebRtcVoiceMedia::EnsureDecoderTask()',
            'void WebRtcVoiceMedia::StopDecoderTask()', 'void WebRtcVoiceMedia::ResetDecodeQueue()',
            'bool WebRtcVoiceMedia::DecoderIdle()', 'void WebRtcVoiceMedia::DecoderTask()',
            'void WebRtcVoiceMedia::PumpDecodedPlayback()',
        ])
        code = r'''
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>
using namespace std::chrono_literals;
#define CONFIG_UR_SOFTWARE_AEC 1
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
constexpr int pdPASS=1,portMAX_DELAY=-1,MALLOC_CAP_SPIRAM=1,MALLOC_CAP_8BIT=2;
using TaskHandle_t=std::thread*;
struct Semaphore { std::mutex mutex; std::condition_variable cv; bool done=false; };
using SemaphoreHandle_t=Semaphore*;
static int semaphores=0;
static bool fail_task=false;
static std::vector<std::unique_ptr<std::thread>> tasks;
SemaphoreHandle_t xSemaphoreCreateBinary() { ++semaphores; return new Semaphore; }
void vSemaphoreDelete(SemaphoreHandle_t semaphore) { --semaphores; delete semaphore; }
void xSemaphoreGive(SemaphoreHandle_t semaphore) {
    std::lock_guard<std::mutex> lock(semaphore->mutex); semaphore->done=true; semaphore->cv.notify_all();
}
void xSemaphoreTake(SemaphoreHandle_t semaphore,int) {
    std::unique_lock<std::mutex> lock(semaphore->mutex); semaphore->cv.wait(lock,[&]{return semaphore->done;});
}
int xTaskCreateWithCaps(void (*entry)(void*),const char*,int,void* context,int,TaskHandle_t* out,int) {
    if (fail_task) return 0;
    auto task=std::make_unique<std::thread>([=]{entry(context);});
    *out=task.get(); tasks.push_back(std::move(task)); return pdPASS;
}
void vTaskDeleteWithCaps(void*) {}
static std::atomic<int64_t> clock_us{0};
int64_t esp_timer_get_time() { return clock_us.load(); }
CONSTANTS
struct Pcm {
    bool active=false,blocked=false;
    uint64_t epoch=0;
    std::vector<int> submitted;
    std::deque<int> hardware;
    const std::thread::id owner=std::this_thread::get_id();
    bool begin_playback(std::string_view,uint64_t value) {
        if (active) return false; active=true; epoch=value; return true;
    }
    void end_playback(uint64_t value) { if (value==epoch) {active=false; hardware.clear();} }
    bool playback_backpressured(uint64_t) const { return blocked; }
    bool write_frame(const std::vector<int16_t>& pcm,uint64_t value) {
        assert(std::this_thread::get_id()==owner && value==epoch && active && !blocked);
        submitted.push_back(pcm.front()); hardware.push_back(pcm.front()); return true;
    }
    bool playback_empty(uint64_t) const { return hardware.empty(); }
};
struct Codec {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered=false,release=false;
    bool decode(const std::vector<uint8_t>& encoded,std::vector<int16_t>& pcm) {
        if (encoded.front()==2) {
            std::unique_lock<std::mutex> lock(mutex); entered=true; cv.notify_all();
            cv.wait(lock,[this]{return release;});
        }
        pcm.assign(320,encoded.front()); return true;
    }
};
struct WebRtcVoiceMedia {
    enum class PeerState { Connected };
    std::atomic<PeerState> state_{PeerState::Connected};
    Pcm pcm_; Codec codec_;
    bool playback_running_=false,playback_prefilled_=false;
    int64_t playback_first_frame_us_=-1;
    uint64_t playback_epoch_=0,drained_epoch_value_=0;
    unsigned playback_frames_=0;
    std::deque<std::vector<uint8_t>> downlink_;
    FIELDS
    bool start_playback(std::string_view,uint64_t);
    void stop_playback(uint64_t);
    void PumpPlayback();
};
METHODS
int main() {
    WebRtcVoiceMedia media;
    assert(media.start_playback("old",1));
    for (int i=1;i<=6;++i) media.downlink_.push_back({uint8_t(i)});
    const auto before=std::chrono::steady_clock::now(); media.PumpPlayback();
    assert(std::chrono::steady_clock::now()-before<100ms);
    {
        std::unique_lock<std::mutex> lock(media.codec_.mutex);
        assert(media.codec_.cv.wait_for(lock,1s,[&]{return media.codec_.entered;}));
    }
    for (int i=0;i<10;++i) media.PumpPlayback();
    assert(media.playback_frames_==1 && media.downlink_.empty());
    media.pcm_.hardware.clear(); media.PumpPlayback();
    assert(!media.DecoderIdle() && media.drained_epoch_value_==0);
    media.stop_playback(1);
    media.pcm_.submitted.clear();
    assert(media.start_playback("new",2));
    const auto generation=media.decode_generation_;
    media.stop_playback(1);
    assert(media.playback_running_ && media.decode_generation_==generation);
    for (int i=30;i<38;++i) media.downlink_.push_back({uint8_t(i)});
    media.pcm_.blocked=true; media.PumpPlayback();
    {
        std::lock_guard<std::mutex> lock(media.codec_.mutex);
        media.codec_.release=true; media.codec_.cv.notify_all();
    }
    for (int i=0;i<40;++i) { clock_us+=10000; media.PumpPlayback(); std::this_thread::sleep_for(1ms); }
    {
        std::lock_guard<std::mutex> lock(media.decoder_mutex_);
        assert(media.decode_pending_.size()<=kDecodeQueueLimit);
        assert(media.decoded_ready_.size()==kDecodeQueueLimit);
    }
    assert(media.pcm_.submitted.empty() && media.drained_epoch_value_==0);
    media.pcm_.blocked=false;
    for (int i=0;i<100 && media.pcm_.submitted.size()<8;++i) {
        clock_us+=10000; media.PumpPlayback(); std::this_thread::sleep_for(1ms);
    }
    assert((media.pcm_.submitted==std::vector<int>{30,31,32,33,34,35,36,37}));
    assert(media.drained_epoch_value_==0);
    media.pcm_.hardware.clear(); media.PumpPlayback();
    assert(media.DecoderIdle() && media.drained_epoch_value_==2);
    media.StopDecoderTask(); media.StopDecoderTask();
    assert(media.decoder_task_==nullptr && semaphores==0);
    for (auto& task:tasks) task->join(); tasks.clear();
    fail_task=true;
    WebRtcVoiceMedia failed;
    assert(!failed.start_playback("failed",1));
    assert(!failed.pcm_.active && failed.decoder_task_==nullptr && semaphores==0);
    fail_task=false;
    WebRtcVoiceMedia closing;
    assert(closing.start_playback("closing",3));
    for (int i=1;i<=6;++i) closing.downlink_.push_back({uint8_t(i)});
    closing.PumpPlayback();
    {
        std::unique_lock<std::mutex> lock(closing.codec_.mutex);
        assert(closing.codec_.cv.wait_for(lock,1s,[&]{return closing.codec_.entered;}));
    }
    std::atomic<bool> stopped{false};
    std::thread close([&]{closing.StopDecoderTask(); stopped=true;});
    std::this_thread::sleep_for(20ms); assert(!stopped);
    {
        std::lock_guard<std::mutex> lock(closing.codec_.mutex);
        closing.codec_.release=true; closing.codec_.cv.notify_all();
    }
    close.join();
    assert(stopped && closing.decoder_task_==nullptr && semaphores==0);
    for (auto& task:tasks) task->join(); tasks.clear();
}
'''.replace('CONSTANTS', constants).replace('FIELDS', fields).replace('METHODS', methods)
        with tempfile.TemporaryDirectory(prefix='watcher-async-decode-') as directory:
            binary = str(Path(directory) / 'async-decode')
            built = subprocess.run(['g++', '-std=c++17', '-pthread', '-x', 'c++', '-', '-o', binary],
                                   input=code, text=True, capture_output=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            result = subprocess.run([binary], text=True, capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
