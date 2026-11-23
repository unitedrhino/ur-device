"""验证Watcher的ROM格式门禁，畸形尺寸和截断段不能进入替代解码器。"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

# 使用真实帧头解析函数；不替身解析结果，ASan/UBSan检查所有截断位置及变异输入。
PROGRAM = r'''
#include <cstdint>
#include <vector>
#include "watcher_call_jpeg.h"

// header 是标准240×240、8位三分量、4:2:0的SOF0，图像正文由真正解码器另行校验。
static std::vector<uint8_t> header() {
    return {0xff,0xd8,0xff,0xc0,0,17,8,0,240,0,240,3,1,0x22,0,2,0x11,1,3,0x11,1};
}

// accepted 将测试宽高回读给调用方；失败时仅检查拒绝，避免依赖未承诺的输出值。
static bool accepted(const std::vector<uint8_t>& bytes, int expected_width=240, int expected_height=240) {
    int width=0,height=0;
    // 双入口结果独立回读，避免互相覆盖掩盖指针入口差异。
    int pointer_width=0,pointer_height=0;
    const bool vector_ok=WatcherRomJpegDimensions(bytes,width,height);
    const bool pointer_ok=WatcherRomJpegDimensions(bytes.data(),bytes.size(),pointer_width,pointer_height);
    return vector_ok && pointer_ok && width==expected_width && height==expected_height &&
           width==pointer_width && height==pointer_height;
}

int main() {
    int null_width=0,null_height=0;
    if(WatcherRomJpegDimensions(nullptr,20,null_width,null_height))return 12;
    const auto original=header();
    if(!accepted(original))return 1;
    for(uint8_t sampling:{0x11,0x21,0x22}) {
        auto bytes=original;bytes[13]=sampling;if(!accepted(bytes))return 2;
    }
    for(size_t length=0;length<original.size();++length) {
        std::vector<uint8_t> bytes(original.begin(),original.begin()+length);
        if(accepted(bytes))return 3;
    }
    for(int dimension:{0,1,240,241,65535}) {
        for(int start:{7,9}) {
            auto bytes=original;bytes[start]=dimension>>8;bytes[start+1]=dimension&255;
            const bool valid=dimension>=1 && dimension<=240;
            const bool actual=accepted(bytes,start==9?dimension:240,start==7?dimension:240);
            if(actual!=valid)return 4;
        }
    }
    // 不适用ROM的合法格式回退原解码器，不扩大替代路径的支持范围。
    for(int at:{3,5,6,11,12,13,15,16,18,19}) {
        auto bytes=original;
        bytes[at]=(at==3)?0xc2:0xff;
        if(accepted(bytes))return 5;
    }
    auto grayscale=original;grayscale[5]=11;grayscale[11]=1;grayscale.resize(15);
    if(accepted(grayscale))return 6;
    auto app=original;app.insert(app.begin()+2,{0xff,0xe1,0,6,1,2,3,4});
    if(!accepted(app))return 7;
    auto bad_length=app;bad_length[4]=0xff;bad_length[5]=0xff;
    if(accepted(bad_length))return 8;
    auto fill=original;fill.insert(fill.begin()+2,0xff);
    if(!accepted(fill))return 9;
    auto scan=original;scan[3]=0xda;if(accepted(scan))return 10;
    // 确定性变异覆盖段长、标记和尺寸组合；成功时输出始终受240×240约束。
    uint32_t random=0x633412ab;
    for(int iteration=0;iteration<100000;++iteration) {
        auto bytes=original;
        random=random*1664525u+1013904223u;
        const auto at=random%bytes.size();
        bytes[at]=random>>24;
        if(iteration%3==0)bytes.resize(random%(bytes.size()+1));
        int width=0,height=0;
        if(WatcherRomJpegDimensions(bytes,width,height) &&
           (width<=0||height<=0||width>240||height>240))return 11;
    }
    return 0;
}
'''


class WatcherJpegHeaderTest(unittest.TestCase):
    """覆盖可用彩色格式、回退、越界尺寸及网络截断的实际内存安全边界。"""

    def test_real_header_with_sanitizers(self):
        """编译生产函数并执行固定用例与变异，失败保留编译器或进程退出码。"""
        temp_root = ROOT.parents[1] / '.temp'
        temp_root.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=temp_root, prefix='watcher-jpeg-header-') as tmp:
            root = Path(tmp)
            source = root / 'main.cc'
            source.write_text(PROGRAM)
            executable = root / 'header'
            subprocess.run([
                'g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                '-I' + str(ROOT / 'main/signaling'), str(source), '-o', str(executable),
            ], check=True)
            subprocess.run([str(executable)], check=True, timeout=15)


    def test_camera_decoder_ownership_and_reopen(self):
        """执行实际模式切换、关闭及重建，覆盖互斥所有权和部分创建失败后的可重试性。"""
        text = (ROOT / 'main/boards/sensecap-watcher/sscma_camera.cc').read_text()
        functions = []
        for signature in ['bool SscmaCamera::EnsureJpegDecoder()',
                          'void SscmaCamera::StartJpegStream()',
                          'void SscmaCamera::StopStream()']:
            start = text.index(signature)
            at = text.index('{', start)
            depth = 1
            end = at + 1
            while depth:
                depth += (text[end] == '{') - (text[end] == '}')
                end += 1
            functions.append(text[start:end])
        program = r'''#include <atomic>
#include <cassert>
#include <cstdint>
#include <mutex>
#include <thread>
// SDK边界只计数分配与归还，模式切换和失败处理来自维护源码。
using jpeg_dec_handle_t=void*;
struct jpeg_dec_config_t {int output_type;int rotate;};
constexpr int JPEG_PIXEL_FORMAT_RGB565_LE=1,JPEG_ROTATE_0D=0,JPEG_ERR_OK=0;
std::mutex* decoder_mutex=nullptr; // decoder_mutex 是实际相机共用的互斥边界。
int opens=0,closes=0,breaks=0; // 计数验证重复启动及关闭不会重复释放。
bool partial_failure=false; // partial_failure 模拟SDK部分建立后返回错误。
// assert_locked 用另一线程确认调用方持有所有权锁，避免同线程重复取锁。
void assert_locked(){
 bool available=false; // 探针只写一次，join后读取。
 std::thread probe([&]{if(decoder_mutex->try_lock()){available=true;decoder_mutex->unlock();}});
 probe.join();assert(!available);
}
// jpeg_dec_open 仅模拟SDK分配边界，保留部分建立后失败的非空句柄。
int jpeg_dec_open(jpeg_dec_config_t* config,jpeg_dec_handle_t* decoder){
 assert_locked();assert(config->output_type==JPEG_PIXEL_FORMAT_RGB565_LE&&config->rotate==0);
 *decoder=reinterpret_cast<void*>(static_cast<uintptr_t>(0x100+(++opens)));
 return partial_failure?-1:JPEG_ERR_OK;
}
// jpeg_dec_close 核对持锁并计数，检查重复释放与失败清理。
void jpeg_dec_close(jpeg_dec_handle_t decoder){assert_locked();assert(decoder);++closes;}
// sscma_client_break 只计数协处理器停流，不替代生产模式切换。
void sscma_client_break(void*){++breaks;}
#define ESP_LOGI(...) do{}while(false)
#define TAG "camera"
struct SscmaCamera {
 void* sscma_client_handle_=reinterpret_cast<void*>(1); // 只替换协处理器通信句柄。
 jpeg_dec_handle_t jpeg_dec_=reinterpret_cast<void*>(2); // 模拟构造时已有的RGB上下文。
 std::mutex stream_mutex_; // 与生产相机相同的解码器互斥边界。
 std::atomic<bool> streaming_{false},jpeg_only_{false}; // 独立观测流和JPEG模式。
 size_t stream_jpeg_size_=42; // 旧帧长度必须在切入JPEG时丢弃。
 int starts=0;bool fail_start=false; // 硬件启动次数及可控失败。
 bool EnsureJpegDecoder();void StartJpegStream();void StopStream();
 // StartStream 仅替换协处理器启动边界，StartJpegStream仍执行真实代码。
 void StartStream(){++starts;streaming_.store(!fail_start);}
};
FUNCTIONS
int main(){
 SscmaCamera camera;decoder_mutex=&camera.stream_mutex_;
 camera.StartJpegStream();assert(camera.streaming_&&camera.jpeg_only_&&!camera.jpeg_dec_);
 assert(camera.stream_jpeg_size_==0&&closes==1&&opens==0&&camera.starts==1);
 {std::lock_guard<std::mutex> lock(camera.stream_mutex_);assert(!camera.EnsureJpegDecoder());}
 assert(opens==0);
 camera.StartJpegStream();assert(closes==1&&camera.starts==1);
 camera.StopStream();assert(!camera.streaming_&&!camera.jpeg_only_&&breaks==1);
 {std::lock_guard<std::mutex> lock(camera.stream_mutex_);
  assert(camera.EnsureJpegDecoder());assert(camera.EnsureJpegDecoder());}
 assert(opens==1&&camera.jpeg_dec_);
 camera.StartJpegStream();assert(closes==2&&!camera.jpeg_dec_);
 camera.StopStream();camera.fail_start=true;camera.StartJpegStream();
 assert(!camera.streaming_&&!camera.jpeg_only_);
 partial_failure=true;
 {std::lock_guard<std::mutex> lock(camera.stream_mutex_);
  assert(!camera.EnsureJpegDecoder());assert(!camera.jpeg_dec_);}
 assert(opens==2&&closes==3);
 partial_failure=false;
 {std::lock_guard<std::mutex> lock(camera.stream_mutex_);
  assert(camera.EnsureJpegDecoder());jpeg_dec_close(camera.jpeg_dec_);camera.jpeg_dec_=nullptr;}
 assert(opens==3&&closes==4);
}
'''.replace('FUNCTIONS', '\n'.join(functions))
        temp_root = ROOT.parents[1] / '.temp'
        temp_root.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=temp_root, prefix='watcher-camera-owner-') as tmp:
            source = Path(tmp) / 'main.cc'
            source.write_text(program)
            executable = Path(tmp) / 'camera'
            subprocess.run(['g++', '-std=c++20', '-pthread', '-Wall', '-Wextra', '-Werror',
                            str(source), '-o', str(executable)], check=True)
            subprocess.run([str(executable)], check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()
