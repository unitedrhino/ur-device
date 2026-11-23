"""执行真实 ESP Video 视频流方法，校验取帧独占、有限等待和收尾。"""
from pathlib import Path
import subprocess
import tempfile
import unittest


class EspVideoStreamTest(unittest.TestCase):
    """用相机及编码边界替身执行生产状态迁移。"""

    def test_stream_lifecycle(self):
        root = Path(__file__).resolve().parents[2]
        text = (root / 'main/boards/common/esp_video.cc').read_text()
        begin = text.index('bool EspVideo::StartCompressedStream()')
        end = text.index('bool EspVideo::SetHMirror(', begin)
        body = text[begin:end]
        capture_begin = text.index('bool EspVideo::Capture()')
        capture_end = text.index('// 调用者持有 frame_mutex_', capture_begin)
        capture = text[capture_begin:capture_end]
        code = r'''
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <sys/time.h>
#include <vector>
#include "stream_video_pixels.h"
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define V4L2_PIX_FMT_RGB565 1
#define V4L2_PIX_FMT_RGB565X 2
#define VIDIOC_G_DQBUF_TIMEOUT 3
#define VIDIOC_S_DQBUF_TIMEOUT 4
#define ESP_LOGE(...) ((void)0)
#define pdMS_TO_TICKS(ms) ((uint32_t)((uint32_t)(ms) * 100U) / 1000U)
#define pdTICKS_TO_MS(ticks) ((uint32_t)((uint64_t)(ticks) * 1000U / 100U))
static bool ioctl_failure=false, image_failure=false, invalid_jpeg=false, capture_failure=false;
static bool last_preview=false;
static uint64_t timeout_us=1000000;
static int allocations=0;
void *heap_caps_malloc(size_t n,int) { allocations++;return std::malloc(n); }
void heap_caps_free(void *p) { if(p) allocations--;std::free(p); }
int ioctl(int fd,int request,timeval *value) {
 assert(fd==3);if(ioctl_failure)return -1;
 if(request==VIDIOC_G_DQBUF_TIMEOUT){value->tv_sec=timeout_us/1000000;value->tv_usec=timeout_us%1000000;}
 else if(request==VIDIOC_S_DQBUF_TIMEOUT) timeout_us=static_cast<uint64_t>(value->tv_sec)*1000000+value->tv_usec;
 else return -1;
 return 0;
}
bool image_to_jpeg(uint8_t*,size_t n,int w,int h,int fmt,int,uint8_t** out,size_t* size) {
 assert(n==240*240*2 && w==240 && h==240 && fmt==V4L2_PIX_FMT_RGB565);
 if(image_failure)return false;
 *size=5;*out=static_cast<uint8_t*>(heap_caps_malloc(*size,0));
 uint8_t good[]={0xff,0xd8,0xff,0xff,0xd9};memcpy(*out,good,*size);
 if(invalid_jpeg) (*out)[2]=0;
 return true;
}
class EspVideo {
public:
 struct Frame { uint8_t* data;size_t len;int width,height,format; } frame_{};
 std::mutex frame_mutex_;
 bool compressed_stream_owned_=false, streaming_on_=true, still_frame_valid_=true, stream_timeout_ready_=true;
 int video_fd_=3, sensor_format_=V4L2_PIX_FMT_RGB565;
 timeval previous_dq_timeout_{};
 uint8_t* stream_rgb_=nullptr;
 uint32_t stream_sequence_=0;
 bool CaptureFrameLocked(bool preview){last_preview=preview;return !capture_failure;}
 bool Capture();bool StartCompressedStream();void StopStream();
 bool TakeStreamJPEG(std::vector<uint8_t>&,uint32_t&);
};
METHODS
int main() {
 uint8_t pixels[16]={}; EspVideo camera; camera.frame_={pixels,sizeof(pixels),4,2,V4L2_PIX_FMT_RGB565};
 std::vector<uint8_t> jpeg; uint32_t sequence=0;
 assert(!camera.TakeStreamJPEG(jpeg,sequence));
 assert(camera.Capture() && camera.still_frame_valid_ && last_preview);
 ioctl_failure=true;assert(!camera.StartCompressedStream() && allocations==0);
 ioctl_failure=false;assert(camera.StartCompressedStream() && timeout_us==200000 && allocations==1 && !camera.still_frame_valid_);
 assert(!camera.StartCompressedStream() && !camera.Capture());
 assert(camera.TakeStreamJPEG(jpeg,sequence) && sequence==1 && jpeg.size()==5 && allocations==1 && !last_preview);
 invalid_jpeg=true;assert(!camera.TakeStreamJPEG(jpeg,sequence) && sequence==1 && allocations==1);
 invalid_jpeg=false;image_failure=true;assert(!camera.TakeStreamJPEG(jpeg,sequence) && allocations==1);
 image_failure=false;capture_failure=true;assert(!camera.TakeStreamJPEG(jpeg,sequence));
 camera.StopStream();assert(timeout_us==1000000 && allocations==0);camera.StopStream();
 assert(!camera.still_frame_valid_ && !camera.TakeStreamJPEG(jpeg,sequence));
 capture_failure=false;assert(camera.Capture() && camera.still_frame_valid_);
 camera.sensor_format_=42;assert(!camera.StartCompressedStream() && allocations==0);
 camera.sensor_format_=V4L2_PIX_FMT_RGB565;
 camera.stream_timeout_ready_=false;assert(!camera.StartCompressedStream() && allocations==0);
 camera.stream_timeout_ready_=true;
 timeout_us=static_cast<uint64_t>(pdTICKS_TO_MS(UINT32_MAX))*1000;
 const auto original_timeout=timeout_us;
 assert(!camera.StartCompressedStream() && timeout_us==original_timeout && allocations==0);
}
'''.replace('METHODS', capture + body)
        with tempfile.TemporaryDirectory() as directory:
            driver = Path(directory) / 'stream.cc'
            driver.write_text(code)
            binary = Path(directory) / 'stream'
            built = subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                    '-I', str(root / 'main/boards/common'), str(driver),
                                    '-o', str(binary)], text=True, capture_output=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()
