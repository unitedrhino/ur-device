"""通过真实 JPEG 工作线程验证低 DMA 水位保护与恢复边界。"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

# 宿主替身只模拟系统 API 与 JPEG 硬件，执行的线程和状态逻辑来自生产头文件。
STUBS = {
    'sdkconfig.h': r'''#define CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO 1
#define CONFIG_BOARD_TYPE_ESP_SPARKBOT 0
''',
    'esp_heap_caps.h': r'''#pragma once
#include <atomic>
#include <cstdlib>
#include <cstddef>
extern std::atomic<size_t> test_dma;
inline std::atomic<size_t> test_dma_free{32768}; // 总DMA余量独立于最大连续块，用于交叉边界。
#define MALLOC_CAP_DMA 1
#define MALLOC_CAP_SPIRAM 2
#define MALLOC_CAP_8BIT 4
inline size_t heap_caps_get_largest_free_block(int){return test_dma.load();}
inline size_t heap_caps_get_free_size(int){return test_dma_free.load();}
inline void* heap_caps_aligned_alloc(size_t, size_t n, int){return std::malloc(n);}
inline void heap_caps_free(void* p){std::free(p);}
''',
    'esp_timer.h': r'''#pragma once
#include <chrono>
#include <cstdint>
#include <atomic>
extern std::atomic<int64_t> test_clock_us;
inline int64_t esp_timer_get_time(){if(auto now=test_clock_us.load())return now;return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
''',
    'esp_log.h': r'''#pragma once
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
''',
    'freertos/idf_additions.h': r'''#pragma once
#include <chrono>
#include <thread>
using TaskHandle_t=void*;
#define pdPASS 1
#define pdMS_TO_TICKS(x) (x)
inline void vTaskDelay(int ms){std::this_thread::sleep_for(std::chrono::milliseconds(ms));}
inline void vTaskDeleteWithCaps(void*){}
inline int xTaskCreatePinnedToCoreWithCaps(void (*fn)(void*),const char*,int,void* arg,int,TaskHandle_t* out,int,int){*out=reinterpret_cast<void*>(1);std::thread([=]{fn(arg);}).detach();return pdPASS;}
''',
    'esp_jpeg_dec.h': r'''#pragma once
#include <cstdint>
#include "esp_heap_caps.h"
inline std::atomic<int> test_jpeg_opens{0},test_jpeg_closes{0},test_jpeg_processes{0};
inline std::atomic<bool> test_jpeg_pressure{false},test_jpeg_parse_error{false};
using jpeg_dec_handle_t=void*;
struct jpeg_dec_config_t{int output_type=0;int rotate=0;};
struct jpeg_dec_io_t{uint8_t* inbuf=nullptr;int inbuf_len=0;uint8_t* outbuf=nullptr;};
struct jpeg_dec_header_info_t{int width=0;int height=0;};
#define JPEG_ERR_OK 0
#define JPEG_PIXEL_FORMAT_RGB565_LE 0
#define JPEG_ROTATE_0D 0
#define DEFAULT_JPEG_DEC_CONFIG() jpeg_dec_config_t{}
inline int jpeg_dec_open(jpeg_dec_config_t*,jpeg_dec_handle_t* p){++test_jpeg_opens;*p=(void*)1;if(test_jpeg_pressure.load())test_dma.store(7936);return 0;}
inline int jpeg_dec_parse_header(jpeg_dec_handle_t,jpeg_dec_io_t*,jpeg_dec_header_info_t* h){h->width=240;h->height=240;return test_jpeg_parse_error.load()?1:0;}
inline int jpeg_dec_get_outbuf_len(jpeg_dec_handle_t,int* n){*n=240*240*2;return 0;}
inline int jpeg_dec_process(jpeg_dec_handle_t,jpeg_dec_io_t*){++test_jpeg_processes;return 0;}
inline void jpeg_dec_close(jpeg_dec_handle_t){if(test_jpeg_pressure.load())test_dma.store(20000);++test_jpeg_closes;}
''',
    'jpeg_decoder.h': r'''#pragma once
#include <cstddef>
#include <cstdint>
#include <atomic>
#define ESP_OK 0
#define JPEG_IMAGE_FORMAT_RGB565 0
#define JPEG_IMAGE_SCALE_0 0
inline std::atomic<int> test_rom_decodes{0};
struct esp_jpeg_image_cfg_t{uint8_t* indata=nullptr;size_t indata_size=0;uint8_t* outbuf=nullptr;size_t outbuf_size=0;int out_format=0,out_scale=0;struct{void* working_buffer=nullptr;size_t working_buffer_size=0;} advanced;};
struct esp_jpeg_image_output_t{int width=0,height=0;size_t output_len=0;};
inline int esp_jpeg_decode(esp_jpeg_image_cfg_t* c,esp_jpeg_image_output_t* out){
 if(!c->advanced.working_buffer||c->advanced.working_buffer_size!=4096)return 1;
 out->width=(c->indata[9]<<8)|c->indata[10];out->height=(c->indata[7]<<8)|c->indata[8];out->output_len=out->width*out->height*2;++test_rom_decodes;return 0;
}
''',
    'ur/device/udp_av.hpp': r'''#pragma once
#include <cstddef>
#include <cstdint>
namespace ur::device {struct UdpAV{static constexpr size_t kMaxJPEG=49152;};struct RemoteVideoGate{bool completed(uint64_t,uint64_t,int){return true;}};}
''',
}

PROGRAM = r'''
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include "ur_signaling_call_jpeg.h"
std::atomic<size_t> test_dma{20000};
std::atomic<int64_t> test_clock_us{0};  // 仅截止边界场景使用可控时钟，避免真实睡眠抖动。
using namespace std::chrono_literals;
static bool show(UrCallJpegOutput& out,std::atomic<int>& frames,int target,int loops=100){
 for(int i=0;i<loops&&frames.load()<target;i++){out.Submit({1,2,3});std::this_thread::sleep_for(10ms);}return frames.load()>=target;
}
static int transient(){
 test_dma.store(20000);std::atomic<int> shown{0},cleared{0};UrCallJpegOutput out;
 if(!out.Start([&](const uint8_t* p,int,int){if(p)++shown;else ++cleared;}))return 2;
 if(!show(out,shown,1))return 3;test_dma.store(6912);std::this_thread::sleep_for(100ms);
#if CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
 // 确认已经暂停解码，但100ms内的有效画面不能被低DMA立即清掉。
 if(!out.WaitingForDma()||cleared.load()!=0)return 20;
#endif
 test_dma.store(20000);int first=shown.load();bool recovered=show(out,shown,first+1);bool disabled=out.Disabled();
 const int cleared_before_stop=cleared.load();  // Stop本身会清屏，恢复过程的清屏次数须提前记录。
 out.Stop();
#if CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
 if(!recovered||disabled||cleared_before_stop!=0){std::printf("transient failed recovered=%d disabled=%d cleared=%d\n",recovered,disabled,cleared.load());return 1;}
#else
 (void)cleared_before_stop;
 if(recovered||!disabled)return 4;
#endif
 return 0;
}
static int persistent(){
 test_dma.store(20000);std::atomic<int> shown{0},cleared{0};UrCallJpegOutput out;
 if(!out.Start([&](const uint8_t* p,int,int){if(p)++shown;else ++cleared;}))return 5;
 if(!show(out,shown,1))return 6;test_dma.store(6912);
#if CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
 // 画面500ms过期后必须清除；此时还未到1秒的永久停显期限。
 std::this_thread::sleep_for(650ms);
 if(cleared.load()==0||out.Disabled()||!out.WaitingForDma())return 21;
#endif
 for(int i=0;i<250&&!out.Disabled();i++)std::this_thread::sleep_for(10ms);
 bool disabled=out.Disabled();test_dma.store(20000);int first=shown.load();bool recovered=show(out,shown,first+1,50);out.Stop();
 if(!disabled||recovered){std::printf("persistent failed disabled=%d recovered=%d\n",disabled,recovered);return 7;}return 0;
}
static int explicit_protection(){
 test_dma.store(20000);std::atomic<int> shown{0};UrCallJpegOutput out;
 if(!out.Start([&](const uint8_t* p,int,int){if(p)++shown;}))return 8;
 if(!show(out,shown,1))return 9;out.ProtectAudio();int first=shown.load();bool recovered=show(out,shown,first+1,50);bool disabled=out.Disabled();out.Stop();return disabled&&!recovered?0:10;
}
static int stop_or_protect_while_waiting(bool protect){
#if CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
 test_dma.store(20000);std::atomic<int> shown{0};UrCallJpegOutput out;
 if(!out.Start([&](const uint8_t* p,int,int){if(p)++shown;}))return 11;
 if(!show(out,shown,1))return 12;test_dma.store(6912);
 for(int i=0;i<30&&!out.WaitingForDma();i++)std::this_thread::sleep_for(10ms);
 if(!out.WaitingForDma())return 13;
 if(protect){out.ProtectAudio();test_dma.store(20000);std::this_thread::sleep_for(300ms);
  if(!out.Disabled()||out.WaitingForDma())return 14;}
 out.Stop();test_dma.store(20000);
#endif
 return 0;
}
static int unstable_then_recover(){
#if CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
 test_dma.store(20000);std::atomic<int> shown{0};UrCallJpegOutput out;
 if(!out.Start([&](const uint8_t* p,int,int){if(p)++shown;}))return 15;
 if(!show(out,shown,1))return 16;test_dma.store(6912);
 for(int i=0;i<30&&!out.WaitingForDma();i++)std::this_thread::sleep_for(10ms);
 if(!out.WaitingForDma())return 17;
 test_dma.store(20000);std::this_thread::sleep_for(15ms);test_dma.store(6912);
 std::this_thread::sleep_for(30ms);test_dma.store(20000);
 std::this_thread::sleep_for(15ms);if(!out.WaitingForDma())return 18;
 int before=shown.load();bool recovered=show(out,shown,before+1,80);bool disabled=out.Disabled();out.Stop();
 if(!recovered||disabled)return 19;
#endif
 return 0;
}
// healthy_at_deadline 验证截止时已恢复仍可继续稳定观察；返回0表示通过，非0定位失败阶段。
static int healthy_at_deadline(){
#if CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
 test_clock_us.store(10000000);test_dma.store(20000);
 std::atomic<int> shown{0};  // 只统计实际成功交付的画面，提交队列不能代替呈现。
 UrCallJpegOutput out;  // 真实生产工作线程，系统时钟和DMA水位使用宿主替身。
 if(!out.Start([&](const uint8_t* p,int,int){if(p)++shown;}))return 22;
 if(!show(out,shown,1))return 23;test_dma.store(6912);
 for(int i=0;i<30&&!out.WaitingForDma();i++)std::this_thread::sleep_for(10ms);
 if(!out.WaitingForDma())return 24;
 test_dma.store(20000);test_clock_us.fetch_add(1000000);
 const int before=shown.load();  // 已存在画面不算恢复，必须有新的成功交付。
 for(int i=0;i<80&&shown.load()==before;i++){
  test_clock_us.fetch_add(10000);out.Submit({1,2,3});std::this_thread::sleep_for(10ms);
 }
 const bool recovered=shown.load()>before,disabled=out.Disabled();  // 同时核验恢复和不可逆保护状态。
 out.Stop();test_clock_us.store(0);
 if(!recovered||disabled)return 25;
#endif
 return 0;
}
int main(){int a=transient();if(a)return a;int b=persistent();if(b)return b;
 int c=explicit_protection();if(c)return c;int d=stop_or_protect_while_waiting(false);
 if(d)return d;int e=stop_or_protect_while_waiting(true);if(e)return e;
 int f=unstable_then_recover();return f?f:healthy_at_deadline();}
'''

# 执行真实ROM分支与回退所有权；SDK替身仅模拟分配后水位下降及解析失败。
ROM_PROGRAM = r'''
#include <atomic>
#include <chrono>
#include <thread>
#include "ur_signaling_call_jpeg.h"
std::atomic<size_t> test_dma{20000};
std::atomic<int64_t> test_clock_us{0};
using namespace std::chrono_literals;
static bool closed(int expected){
 for(int i=0;i<100&&test_jpeg_closes.load()<expected;++i)std::this_thread::sleep_for(5ms);
 return test_jpeg_closes.load()==expected;
}
int main(){
 std::atomic<int> frames{0};UrCallJpegOutput output;
 if(!output.Start([&](const uint8_t* p,int,int){if(p)++frames;}))return 1;
 // 回退申请后低于8KB时，不能开始解码或保留上下文拖住后续彩色帧。
 test_jpeg_pressure.store(true);output.Submit({1,2,3});
 if(!closed(1)||test_jpeg_processes.load()!=0||test_jpeg_opens.load()!=1)return 2;
 test_jpeg_pressure.store(false);
 // 原解码器解析失败时仍须归还本帧工作区。
 test_jpeg_parse_error.store(true);output.Submit({1,2,3});
 if(!closed(2)||test_jpeg_processes.load()!=0)return 3;
 test_jpeg_parse_error.store(false);
 output.Submit({1,2,3});
 if(!closed(3))return 4;
 for(int i=0;i<100&&frames.load()<1;++i)std::this_thread::sleep_for(5ms);
 if(frames.load()!=1)return 4;
 // 后续已支持的彩色帧应直接使用已分配工作区，不重新打开原解码器。
 output.Submit({0xff,0xd8,0xff,0xc0,0,17,8,0,64,0,64,3,1,0x22,0,2,0x11,1,3,0x11,1});
 for(int i=0;i<100&&frames.load()<2;++i)std::this_thread::sleep_for(5ms);
 const bool ok=frames.load()==2&&test_rom_decodes.load()==1&&test_jpeg_opens.load()==3&&!output.Disabled();
 output.Stop();return ok?0:5;
}
'''


# ROM_BUDGET_PROGRAM 执行真实ROM工作线程，独立改变连续块或总余量，不代签硬件听感。
ROM_BUDGET_PROGRAM = r'''
#include <atomic>
#include <chrono>
#include <thread>
#include "ur_signaling_call_jpeg.h"
std::atomic<size_t> test_dma{7168}; // 模拟碎片化后仍够ROM路径的连续块。
std::atomic<int64_t> test_clock_us{0}; // 使用真实宿主时钟推进等待与保护截止。
using namespace std::chrono_literals;
// frame 提供SDK替身接受的64方图帧头，尺寸校验仍执行生产函数。
static std::vector<uint8_t> frame(){return {0xff,0xd8,0xff,0xc0,0,17,8,0,64,0,64,3,1,0x22,0,2,0x11,1,3,0x11,1};}
// show 提交图像并等待实际回调推进到wanted，避免把入队当作恢复。
static bool show(UrCallJpegOutput& output,std::atomic<int>& frames,int wanted){
 for(int i=0;i<100&&frames.load()<wanted;++i){output.Submit(frame());std::this_thread::sleep_for(5ms);}
 return frames.load()>=wanted;
}
int main(){
 if(WatcherRomJpegDmaReady(6143,65536)||WatcherRomJpegDmaReady(6144,16383)||
    !WatcherRomJpegDmaReady(6144,16384)||!WatcherRomJpegDmaReady(7168,21347)||
    WatcherRomJpegDmaReady(20000,16000))return 1;
 test_dma_free.store(21347);std::atomic<int> frames{0};UrCallJpegOutput output;
 if(!output.Start([&](const uint8_t* p,int,int){if(p)++frames;}))return 2;
 // 连续7KB但总量足够的PSRAM路径能够实际完成解码，不创建普通解码上下文。
 if(!show(output,frames,1)||test_rom_decodes.load()<1||test_jpeg_opens.load()!=0)return 3;
 test_dma_free.store(16383);
 for(int i=0;i<60&&!output.WaitingForDma();++i)std::this_thread::sleep_for(5ms);
 if(!output.WaitingForDma()||output.Disabled())return 4;
 // 总量恢复后仍经过50ms稳定观察，随后必须真正交付新画面。
 const int previous=frames.load();test_dma_free.store(21347);
 if(!show(output,frames,previous+1)||output.Disabled())return 5;
 // 总量很大也不能掩盖连续块不足；持续不足仍执行原1秒永久保护。
 test_dma.store(6143);test_dma_free.store(65536);
 for(int i=0;i<300&&!output.Disabled();++i)std::this_thread::sleep_for(5ms);
 const bool protected_audio=output.Disabled();output.Stop();return protected_audio?0:6;
}
'''


class CallJpegDmaRecoveryTest(unittest.TestCase):
    """覆盖短时恢复、持续不足、显式音频保护及其他板型原行为。"""

    def test_real_worker_dma_recovery(self):
        temp_root = ROOT.parents[1] / '.temp'
        temp_root.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=temp_root, prefix='watcher-jpeg-dma-') as tmp:
            root = Path(tmp)
            for name, content in STUBS.items():
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content)
            for watcher in (0, 1):
                binary = root / f'test-{watcher}'
                build = subprocess.run(
                    ['g++', '-std=c++17', '-pthread',
                     f'-DCONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER={watcher}',
                     '-I', str(root), '-I', str(ROOT / 'main/signaling'),
                     '-x', 'c++', '-', '-o', str(binary)],
                    input=PROGRAM, text=True, capture_output=True, timeout=30)
                self.assertEqual(build.returncode, 0, build.stderr)
                run = subprocess.run([str(binary)], text=True, capture_output=True, timeout=10)
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def test_rom_workspace_and_fallback_release(self):
        """回退后水位不足或解析失败均归还上下文，后续彩色帧继续使用外部工作区。"""
        temp_root = ROOT.parents[1] / '.temp'
        temp_root.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=temp_root, prefix='watcher-jpeg-rom-') as tmp:
            root = Path(tmp)
            for name, content in STUBS.items():
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content)
            binary = root / 'test-rom'
            build = subprocess.run(
                ['g++', '-std=c++17', '-pthread',
                 '-DCONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER=1',
                 '-DCONFIG_UR_HUMAN_CALL_UDP_AV=1', '-DCONFIG_JD_USE_ROM=1',
                 '-I', str(root), '-I', str(ROOT / 'main/signaling'),
                 '-x', 'c++', '-', '-o', str(binary)],
                input=ROM_PROGRAM, text=True, capture_output=True, timeout=30)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(binary)], text=True, capture_output=True, timeout=10)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def test_rom_contiguous_and_total_dma_budget(self):
        """分别验证连续块和总量不足、暂态恢复及持续不足的真实保护分支。"""
        temp_root = ROOT.parents[1] / '.temp'
        temp_root.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=temp_root, prefix='watcher-jpeg-budget-') as tmp:
            root = Path(tmp)
            for name, content in STUBS.items():
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content)
            binary = root / 'test-rom'
            build = subprocess.run(
                ['g++', '-std=c++17', '-pthread',
                 '-DCONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER=1',
                 '-DCONFIG_UR_HUMAN_CALL_UDP_AV=1', '-DCONFIG_JD_USE_ROM=1',
                 '-I', str(root), '-I', str(ROOT / 'main/signaling'),
                 '-x', 'c++', '-', '-o', str(binary)],
                input=ROM_BUDGET_PROGRAM, text=True, capture_output=True, timeout=30)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(binary)], text=True, capture_output=True, timeout=10)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
