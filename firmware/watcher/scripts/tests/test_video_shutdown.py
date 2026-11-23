"""回放生产停止函数：编码耗时超过1.5秒时不能提前释放工作线程正在使用的资源。"""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class VideoShutdownTest(unittest.TestCase):
    """用确定性的1800ms编码退出模拟捕获真实Stop超时释放导致的UAF。"""

    def test_actual_stop_waits_for_worker_before_free(self):
        production = (ROOT / "main/signaling/ur_signaling_video.cc").read_text()
        stop = production.split("void UrSignalingVideo::Stop() {", 1)[1].split(
            "bool UrSignalingVideo::Running()", 1)[0]
        source = r'''
#include <atomic>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <vector>
using TickType_t = int;
bool encoder_in_use=true, released=false, camera_stopped=false;
int elapsed_ms=0;
void vTaskDelay(int);
#define pdMS_TO_TICKS(x) (x)
template<class... T> void log_all(T...) {}
#define ESP_LOGI(...) log_all(__VA_ARGS__)
constexpr const char* kTag="video";
void jpeg_dec_close(void*) { assert(!encoder_in_use); }
void heap_caps_free(void*) { assert(!encoder_in_use); }
struct Camera { void StopStream(){camera_stopped=true;} void SetPreviewEnabled(bool){} };
struct Encoder {
    void close(Encoder*) { assert(!encoder_in_use); }
    void del(Encoder*) { assert(!encoder_in_use); released=true; }
};
struct UrSignalingVideo {
    std::atomic<bool> running_{true}; std::atomic<void*> task_{reinterpret_cast<void*>(1)};
    Camera* camera_=nullptr; Encoder* encoder_=nullptr;
    std::mutex frames_mutex_; std::deque<std::vector<uint8_t>> frames_;
    std::vector<uint8_t> i420_,encoded_,sps_pps_,jpeg_;
    void* jpeg_decoder_=nullptr; uint8_t* jpeg_rgb_=nullptr; bool compressed_stream_=false;
    std::function<const uint8_t*()> frame_source_;
    void Stop();
};
UrSignalingVideo* owner=nullptr;
void vTaskDelay(int ticks) {
    assert(!released);
    elapsed_ms+=ticks;
    if(elapsed_ms>=1800) { encoder_in_use=false; owner->task_.store(nullptr); }
}
void UrSignalingVideo::Stop() {
''' + stop + r'''
int main() {
    UrSignalingVideo video; Camera camera; Encoder encoder;
    owner=&video; video.camera_=&camera; video.encoder_=&encoder;
    video.i420_.resize(100); video.encoded_.resize(100);
    video.Stop();
    assert(released && camera_stopped && !encoder_in_use);
    assert(video.encoder_==nullptr && video.task_.load()==nullptr && elapsed_ms>=1800);
    video.Stop();
}
'''
        compiler = shutil.which("g++") or shutil.which("clang++")
        self.assertIsNotNone(compiler)
        output = ROOT.parents[1] / ".temp"
        output.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=output) as directory:
            binary = Path(directory) / "shutdown"
            compiled = subprocess.run([
                compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=undefined", "-x", "c++", "-", "-o", str(binary),
            ], input=source, text=True, capture_output=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            completed = subprocess.run([str(binary)], capture_output=True)
            self.assertEqual(completed.returncode, 0, completed.stderr.decode(errors="replace"))
