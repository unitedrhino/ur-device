"""回放生产JPEG取帧与颜色转换，验证压缩源不走高开销单拍且异常输入不能越界。"""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class VideoCompressedFrameTest(unittest.TestCase):
    """硬件边界返回确定性图像，实际取帧校验和动态分辨率转换全部使用生产函数。"""

    def test_actual_acquire_and_convert_boundaries(self):
        production = (ROOT / "main/signaling/ur_signaling_video.cc").read_text()
        acquire = production.split("bool UrSignalingVideo::AcquireFrame(", 1)[1].split(
            "void UrSignalingVideo::RequestIDR()", 1)[0]
        convert = production.split("void UrSignalingVideo::ConvertFrame(", 1)[1].split(
            "}  // namespace ur::device", 1)[0]
        source = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <functional>
#include <vector>
constexpr int JPEG_ERR_OK=0;
constexpr size_t kJpegRgbBytes=240*240*2;
int parse_calls=0, decode_calls=0, required_bytes=kJpegRgbBytes;
bool malformed=false;
struct jpeg_dec_io_t { uint8_t* inbuf=nullptr; int inbuf_len=0,inbuf_remain=0; uint8_t* outbuf=nullptr; };
struct jpeg_dec_header_info_t { int width=240,height=240; };
int jpeg_dec_parse_header(void*,jpeg_dec_io_t* io,jpeg_dec_header_info_t* header) {
    ++parse_calls; io->inbuf_remain=io->inbuf_len-2;
    if(malformed) header->width=640;
    return 0;
}
int jpeg_dec_get_outbuf_len(void*,int* bytes) { *bytes=required_bytes; return 0; }
int jpeg_dec_process(void*,jpeg_dec_io_t* io) {
    ++decode_calls; assert(io->inbuf_len==6);
    for(size_t i=0;i<kJpegRgbBytes;i+=2) {io->outbuf[i]=0; io->outbuf[i+1]=0xF8;}
    return 0;
}
struct Camera {
    bool fresh=false; size_t bytes=8; int captures=0; uint8_t rgb[8]={};
    bool TakeStreamJPEG(std::vector<uint8_t>& jpeg,uint32_t& sequence) {
        if(!fresh) return false;
        fresh=false; ++sequence; jpeg.assign(bytes,0); return true;
    }
    bool Capture() {++captures;return true;}
    int FrameWidth(){return 2;} int FrameHeight(){return 2;}
};
struct UrSignalingVideo {
    Camera* camera_=nullptr; bool compressed_stream_=true;
    void* jpeg_decoder_=nullptr; uint8_t* jpeg_rgb_=nullptr;
    std::vector<uint8_t> jpeg_,i420_; uint32_t jpeg_sequence_=0;
    std::function<const uint8_t*()> frame_source_;
    int output_width_=160,output_height_=120;
    bool AcquireFrame(const uint8_t*&,int&,int&);
    void ConvertFrame(const uint8_t*,int,int);
};
bool UrSignalingVideo::AcquireFrame(''' + acquire + r'''
void UrSignalingVideo::ConvertFrame(''' + convert + r'''
int main() {
    Camera camera; UrSignalingVideo video; video.camera_=&camera;
    std::vector<uint8_t> rgb(kJpegRgbBytes); video.jpeg_rgb_=rgb.data();
    const uint8_t* pixels=nullptr; int width=0,height=0;
    assert(!video.AcquireFrame(pixels,width,height) && parse_calls==0 && decode_calls==0);
    camera.fresh=true;
    assert(video.AcquireFrame(pixels,width,height) && width==240 && height==240);
    assert(pixels==rgb.data() && decode_calls==1 && camera.captures==0);
    assert(!video.AcquireFrame(pixels,width,height) && decode_calls==1);
    camera.fresh=true; malformed=true;
    assert(!video.AcquireFrame(pixels,width,height) && decode_calls==1);
    camera.fresh=true; malformed=false; required_bytes=kJpegRgbBytes+1;
    assert(!video.AcquireFrame(pixels,width,height) && decode_calls==1);
    camera.fresh=true; camera.bytes=48*1024+1;
    assert(!video.AcquireFrame(pixels,width,height) && decode_calls==1);
    for(const int out_width:{160,320}) {
        video.output_width_=out_width; video.output_height_=out_width*3/4;
        const size_t plane=video.output_width_*video.output_height_;
        video.i420_.assign(plane*3/2,0);
        video.ConvertFrame(rgb.data(),240,240);
        for(size_t i=0;i<plane;++i) assert(video.i420_[i]==74);
        for(size_t i=plane;i<plane*5/4;++i) assert(video.i420_[i]==86);
        for(size_t i=plane*5/4;i<plane*3/2;++i) assert(video.i420_[i]==252);
    }
    video.compressed_stream_=false; video.frame_source_=[&]{return camera.rgb;};
    assert(video.AcquireFrame(pixels,width,height) && width==2 && height==2 && camera.captures==1);
}
'''
        compiler = shutil.which("g++") or shutil.which("clang++")
        self.assertIsNotNone(compiler)
        output = ROOT.parents[1] / ".temp"
        output.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=output) as directory:
            binary = Path(directory) / "compressed"
            compiled = subprocess.run([
                compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=undefined,bounds", "-x", "c++", "-", "-o", str(binary),
            ], input=source, text=True, capture_output=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            completed = subprocess.run([str(binary)], capture_output=True)
            self.assertEqual(completed.returncode, 0, completed.stderr.decode(errors="replace"))
