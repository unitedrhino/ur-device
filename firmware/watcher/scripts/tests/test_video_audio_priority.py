"""执行真实视频循环，覆盖编码过载时仍须让出CPU，防止低优先级音频AFE与Idle饥饿。"""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class VideoAudioPriorityTest(unittest.TestCase):
    """虚拟编码器只替换硬件边界，采集、组帧、排队与调度分支使用生产循环。"""

    def test_actual_overloaded_encoder_still_yields(self):
        production = (ROOT / "main/signaling/ur_signaling_video.cc").read_text()
        run = production.split("void UrSignalingVideo::Run() {", 1)[1].split(
            "// AcquireFrame 只消费", 1)[0]
        source = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <functional>
#include <mutex>
#include <vector>
using TickType_t = int;
int64_t clock_us = 0;
int64_t encode_us = 300000;
std::vector<int> sleeps;
int64_t esp_timer_get_time() { return clock_us; }
void vTaskDelay(int ticks) { sleeps.push_back(ticks); clock_us += ticks*1000; }
#define pdMS_TO_TICKS(x) (x)
template<class... T> void log_all(T...) {}
#define ESP_LOGI(...) log_all(__VA_ARGS__)
#define ESP_LOGW(...) log_all(__VA_ARGS__)
constexpr const char* kTag="video";
constexpr size_t kQueueLimit=8;
constexpr int ESP_H264_ERR_OK=0, ESP_H264_FRAME_TYPE_IDR=1, ESP_H264_FRAME_TYPE_I=2;
struct Raw { uint8_t* buffer=nullptr; size_t len=0; };
struct esp_h264_enc_in_frame_t { Raw raw_data; unsigned pts=0; };
struct esp_h264_enc_out_frame_t { Raw raw_data; size_t length=0; int frame_type=0; };
struct Encoder {
    int process(Encoder*, esp_h264_enc_in_frame_t*, esp_h264_enc_out_frame_t* out) {
        clock_us += encode_us; out->length=1; out->raw_data.buffer[0]=5; return 0;
    }
};
struct Camera {
    bool* running; int calls=0;
    bool Capture() { if (++calls>2) { *running=false; return false; } return true; }
    int FrameWidth() { return 16; } int FrameHeight() { return 16; }
};
struct UrSignalingVideo {
    bool running_=true;
    bool compressed_stream_=false; int frame_interval_ms_=100;
    std::atomic<bool> request_idr_{false}, audio_priority_{false};
    Camera* camera_=nullptr; Encoder* encoder_=nullptr;
    std::function<const uint8_t*()> frame_source_;
    std::vector<uint8_t> i420_=std::vector<uint8_t>(384), encoded_=std::vector<uint8_t>(384);
    unsigned pts_=0;
    std::mutex frames_mutex_;
    std::deque<std::vector<uint8_t>> frames_;
    bool RebuildEncoder() { return false; }
    bool AcquireFrame(const uint8_t*& pixels,int& width,int& height) {
        if (!camera_->Capture()) return false;
        pixels=frame_source_(); width=16; height=16; return true;
    }
    void ConvertFrame(const uint8_t*,int,int) {}
    void LogNaluTypes(const uint8_t*,size_t,bool) {}
    void StashSpsPps(const uint8_t*,size_t) {}
    void PrependSpsPps(std::vector<uint8_t>&) {}
    void Run();
};
void UrSignalingVideo::Run() {
''' + run + r'''
int main() {
    for (const auto work : {300000,35000,-35000}) {
        clock_us=0; encode_us=std::abs(work); sleeps.clear();
        UrSignalingVideo video; Encoder encoder; video.audio_priority_.store(work<0);
        Camera camera{&video.running_};
        uint8_t pixels[512]={}; video.frame_source_=[&]{return pixels;};
        video.camera_=&camera; video.encoder_=&encoder;
        video.Run();
        assert(video.frames_.size()==2);
        assert(sleeps.size()==3 && sleeps[0]>0 && sleeps[1]>0 && sleeps[2]==1000);
        if (work==35000) assert(sleeps[0]==65 && sleeps[1]==65);
        if (work<0) assert(sleeps[0]==965 && sleeps[1]==965);
    }
}
'''
        compiler = shutil.which("g++") or shutil.which("clang++")
        self.assertIsNotNone(compiler)
        output = ROOT.parents[1] / ".temp"
        output.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=output) as directory:
            binary = Path(directory) / "priority"
            compiled = subprocess.run([
                compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=undefined", "-x", "c++", "-", "-o", str(binary),
            ], input=source, text=True, capture_output=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            completed = subprocess.run([str(binary)], capture_output=True)
            self.assertEqual(completed.returncode, 0, completed.stderr.decode(errors="replace"))
