"""执行实际 SSCMA 流交付函数，验证低分辨率尺寸、双缓冲与解码输出边界。"""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]  # 读取生产函数，硬件与 JPEG 库在边界处替换。


class SscmaStreamFrameTest(unittest.TestCase):
    """错误帧不得解码越界或发布，240 与 416 帧均按实际尺寸交付。"""

    def test_actual_stream_delivery_and_output_capacity(self):
        source = (ROOT / "main/boards/sensecap-watcher/sscma_camera.cc").read_text()
        start = source.index("void SscmaCamera::HandleStreamImage(")
        method = source[start:source.index("\nbool SscmaCamera::Capture()", start)]
        buffers_start = source.index("bool SscmaCamera::EnsureStreamBuffers()")
        method += source[buffers_start:source.index("/* HandleStreamImage", buffers_start)]
        stream_start = source.index("void SscmaCamera::StartStream()")
        method += source[stream_start:source.index("// StopStream", stream_start)]
        header = (ROOT / "main/boards/sensecap-watcher/sscma_camera.h").read_text()
        fields_start = header.index("    // StreamFrameTiming")
        fields = header[fields_start:header.index("    void HandleStreamImage(", fields_start)]
        code = r'''
#include <atomic>
#include <algorithm>
#include <cstdio>
#include <vector>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <initializer_list>
constexpr int ESP_OK=0, JPEG_ERR_OK=0, IMG_JPEG_BUF_SIZE=48*1024;
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
struct sscma_client_reply_t {};
struct Header { uint16_t width=0,height=0; };
struct Io { uint8_t* inbuf=nullptr; int inbuf_len=0,inbuf_remain=0; uint8_t* outbuf=nullptr; };
int frame_width=240, frame_height=240, output_bytes=240*240*2;
int decode_calls=0, parse_result=0, length_result=0, decode_result=0;
constexpr int MALLOC_CAP_SPIRAM=1,MALLOC_CAP_8BIT=2;
void* heap_caps_aligned_alloc(size_t alignment,size_t size,int) { return std::aligned_alloc(alignment,size); }
bool fail_timing_alloc=false;
int timing_allocations=0;
void* heap_caps_calloc(size_t count,size_t size,int) {
    ++timing_allocations; return fail_timing_alloc? nullptr:calloc(count,size);
}
void heap_caps_free(void* p) { free(p); }
int64_t fake_time=1234;
int64_t esp_timer_get_time() { return fake_time; }
int sscma_utils_fetch_image_from_reply(const sscma_client_reply_t*,char** data,int* size) {
    *data=static_cast<char*>(malloc(1)); **data='a'; *size=1; fake_time+=11; return 0;
}
bool base64_result=true;
namespace watcher {
bool DecodeImageBase64(uint8_t*,size_t,size_t* length,const unsigned char*,size_t) {
    *length=base64_result?3:0; fake_time+=23; return base64_result;
}
}
int jpeg_dec_parse_header(void*,Io* io,Header* header) {
    header->width=frame_width; header->height=frame_height; io->inbuf_remain=1;
    fake_time+=5; return parse_result;
}
int jpeg_dec_get_outbuf_len(void*,int* bytes) { *bytes=output_bytes; fake_time+=3; return length_result; }
int jpeg_dec_process(void*,Io*) { ++decode_calls; fake_time+=101; return decode_result; }
struct Display {
    int calls=0,width=0,height=0; const uint8_t* pixels=nullptr; int64_t received=0;
    void SetStreamPreview(const uint8_t* data,uint16_t w,uint16_t h,int64_t timestamp) {
        ++calls; pixels=data; width=w; height=h; received=timestamp; fake_time+=7;
    }
};
Display display;
struct Board {
    static Board& GetInstance() { static Board instance; return instance; }
    Display* GetDisplay() { return &display; }
};
using esp_err_t=int;
int sensor_option=-1, sensor_result=0, invoke_calls=0,sample_calls=0,start_result=0;
int sscma_client_break(void*) { return 0; }
int sscma_client_set_model(void*,int id) { assert(id==4); return 0; }
int sscma_client_set_sensor(void*,int id,int option,bool enabled) {
    assert(id==1 && enabled); sensor_option=option; return sensor_result;
}
int sscma_client_invoke(void*,int count,bool filter,bool show) {
    assert(count==-1 && !filter && show); ++invoke_calls; return start_result;
}
int sscma_client_sample(void*,int count) { assert(count==-1); ++sample_calls; return start_result; }
struct Trace { void MarkFrame(int64_t) {} void Print(bool) {} };
struct SscmaCamera {
    Trace* transport_trace_=nullptr;
    std::mutex stream_mutex_;
// TIMING_FIELDS
    void PrintStreamPerformance(bool);
    std::atomic<bool> streaming_{false};
    std::atomic<bool> jpeg_only_{false};
    size_t stream_jpeg_size_=0;
    void* sscma_client_handle_=this;
    uint8_t jpeg_memory_[16]{},frame_memory_[2][16]{};
    struct { uint8_t* buf; } jpeg_data_{jpeg_memory_};
    void* jpeg_dec_=this;
    Io io_; Header header_;
    Io* jpeg_io_=&io_; Header* jpeg_out_=&header_;
    uint8_t* stream_frame_[2]={nullptr,nullptr};
    ~SscmaCamera() { for (auto* buffer:stream_frame_) std::free(buffer); heap_caps_free(stream_timings_); }
    std::atomic<int> stream_write_{0}; std::atomic<uint32_t> stream_seq_{0};
    uint16_t stream_w_=99,stream_h_=99;
    bool preview_enabled_=true;
    bool EnsureStreamBuffers();
    void HandleStreamImage(const sscma_client_reply_t*);
    void StartStream();
};
// METHOD
int main() {
    for (bool local_preview : {true,false}) {
        SscmaCamera camera;
        camera.preview_enabled_=local_preview;
        sensor_option=-1; invoke_calls=sample_calls=0;
        camera.StartStream();
        assert(sensor_option==(local_preview ? 0:1) && camera.streaming_.load());
        assert(sample_calls==(local_preview ? 1:0) && invoke_calls==(local_preview ? 0:1));
        camera.StartStream();
        assert(invoke_calls+sample_calls==1);
    }
    {
        SscmaCamera camera; sensor_result=-1; invoke_calls=0;
        camera.StartStream();
        assert(!camera.streaming_.load() && invoke_calls==0);
        sensor_result=0;
    }
    for (bool local_preview : {true,false}) {
        SscmaCamera camera; camera.preview_enabled_=local_preview; start_result=-1;
        camera.StartStream();
        assert(!camera.streaming_.load());
    }
    start_result=0;
    sscma_client_reply_t reply;
    {
        SscmaCamera camera;
        camera.jpeg_only_.store(true); camera.preview_enabled_=false;
        sensor_option=-1; invoke_calls=sample_calls=0;
        camera.StartStream();
        assert(sensor_option==0 && sample_calls==1 && invoke_calls==0);
        display={}; decode_calls=0;
        camera.HandleStreamImage(&reply);
        assert(camera.stream_jpeg_size_==3 && camera.stream_seq_.load()==1);
        assert(decode_calls==0 && display.calls==0);
        base64_result=false;
        camera.HandleStreamImage(&reply);
        assert(camera.stream_jpeg_size_==0 && camera.stream_seq_.load()==1);
        base64_result=true;
        frame_width=frame_height=416;
        camera.HandleStreamImage(&reply);
        assert(camera.stream_jpeg_size_==0 && camera.stream_seq_.load()==1);
        frame_width=frame_height=240;
    }
    for (int dimension : {240,416}) {
        frame_width=frame_height=dimension; output_bytes=dimension*dimension*2;
        display={}; decode_calls=0;
        SscmaCamera camera;
        camera.PrintStreamPerformance(true);
        fake_time=1234;
        camera.HandleStreamImage(&reply);
        assert(camera.stream_timing_count_==1);
        const auto first=camera.stream_timings_[0];
        assert(first.extract_us==11 && first.mutex_us==0 && first.prepare_us==0);
        assert(first.base64_us==23 && first.header_us==8 && first.jpeg_us==101);
        assert(first.publish_us==0 && first.display_submit_us==7 && first.callback_us==150);
        assert(first.interval_us==0 && first.base64_bytes==1 && first.jpeg_bytes==3);
        assert(decode_calls==1 && camera.stream_seq_.load()==1);
        assert(camera.stream_write_.load()==1 && display.pixels==camera.stream_frame_[1]);
        assert(display.calls==1 && display.width==dimension && display.height==dimension);
        assert(display.received==1234);
        camera.EnsureStreamBuffers();
        assert(camera.stream_w_==dimension && camera.stream_h_==dimension);
        camera.preview_enabled_=false;
        camera.HandleStreamImage(&reply);
        assert(camera.stream_seq_.load()==2 && camera.stream_write_.load()==0 && display.calls==1);
        assert(camera.stream_timings_[1].display_submit_us==0);
        const int allocations=timing_allocations;
        camera.PrintStreamPerformance(true);
        for (uint32_t sequence=1; sequence<=201; ++sequence) {
            auto sample=first;sample.sequence=sequence;
            camera.RecordStreamTiming(sample,sequence*1000);
        }
        assert(camera.stream_timing_count_==128 && camera.stream_timing_write_==201%128);
        const auto latest=camera.stream_timings_[(camera.stream_timing_write_+127)%128];
        assert(latest.sequence==201 && latest.interval_us==1000);
        camera.PrintStreamPerformance(false);
        camera.PrintStreamPerformance(true);
        assert(camera.stream_timing_count_==0 && camera.stream_previous_received_us_==0);
        assert(timing_allocations==allocations);
    }
    // 尺寸、解码容量和错误返回分别阻断发布；只允许合法大小进入解码器。
    for (int failure=0;failure<8;++failure) {
        frame_width=frame_height=240; output_bytes=240*240*2;
        parse_result=length_result=decode_result=0;
        if (failure==0) frame_width=640;
        if (failure==1) frame_height=0;
        if (failure==2) output_bytes=416*416*2+1;
        if (failure==3) output_bytes=0;
        if (failure==4) length_result=-1;
        if (failure==5) parse_result=-1;
        if (failure==6) decode_result=-1;
        base64_result=failure!=7;
        display={}; decode_calls=0;
        SscmaCamera camera;
        camera.PrintStreamPerformance(true);
        camera.HandleStreamImage(&reply);
        assert(camera.stream_timing_count_==0);
        assert(decode_calls==(failure==6 ? 1:0));
        assert(camera.stream_seq_.load()==0 && camera.stream_write_.load()==0);
        assert(camera.stream_w_==99 && display.calls==0);
    }
    frame_width=frame_height=240;output_bytes=240*240*2;
    parse_result=length_result=decode_result=0; base64_result=true;
    {
        SscmaCamera camera;const int allocated=timing_allocations;
        camera.HandleStreamImage(&reply);
        assert(camera.stream_timings_==nullptr && timing_allocations==allocated);
    }
    {
        SscmaCamera camera;fail_timing_alloc=true;
        camera.PrintStreamPerformance(true);
        assert(camera.stream_timings_==nullptr);
        camera.HandleStreamImage(&reply);
        assert(camera.stream_seq_.load()==1 && camera.stream_timing_count_==0);
        fail_timing_alloc=false;
    }
}
'''.replace("// METHOD", method).replace("// TIMING_FIELDS", fields)
        compiler = shutil.which("g++") or shutil.which("clang++")
        self.assertIsNotNone(compiler)
        output = ROOT.parents[1] / ".temp"
        output.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=output) as directory:
            binary = str(Path(directory) / "sscma-stream-frame")
            compiled = subprocess.run(
                [compiler, "-x", "c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-", "-o", binary],
                input=code, text=True, capture_output=True,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            result = subprocess.run([binary], capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
            self.assertIn(b"BEGIN count=128 enabled=1 reset=0", result.stdout)
