"""验证真实远端帧回调在停流与拷贝并发时不会重新提交旧画面。"""
from pathlib import Path
import subprocess
import tempfile
import unittest

from .test_call_audio_decode import method

ROOT = Path(__file__).resolve().parents[2]


class CallFrameGenerationTest(unittest.TestCase):
    """复用生产回调，控制内存分配与停流的先后顺序。"""

    def test_inflight_frame_does_not_survive_clear(self):
        source = (ROOT / 'main/boards/sensecap-watcher/watcher_call_ui.cc').read_text()
        actual = '\n'.join(method(source, signature) for signature in (
            'void WatcherCallUi::HandleState(',
            'void WatcherCallUi::HandleRemoteFrame(',
        ))
        code = r'''
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define LV_COLOR_FORMAT_RGB565 0
#define LV_IMAGE_FLAGS_USER2 0x0200
#define LV_OBJ_FLAG_HIDDEN 1
std::mutex gate_mutex; std::condition_variable gate_cv; bool entered=false, proceed=false;
void* heap_caps_malloc(size_t n, int) { std::unique_lock<std::mutex> l(gate_mutex); entered=true; gate_cv.notify_all(); gate_cv.wait(l, []{return proceed;}); return malloc(n); }
void heap_caps_free(void* p) { free(p); }
struct LvglImage { virtual ~LvglImage()=default; };
struct LvglAllocatedImage: LvglImage {
 void* p=nullptr; size_t capacity=0;
 LvglAllocatedImage()=default;
 ~LvglAllocatedImage(){free(p);}
 bool CopyFrom(const void* src,size_t size,int,int,int,int){
  if(!src||size==0)return false;
  if(capacity<size){void* next=heap_caps_malloc(size,0);if(!next)return false;free(p);p=next;capacity=size;}
  memcpy(p,src,size);return true;
 }
 void SetFlags(unsigned) {}
 const void* image_dsc() const {return p;}
};
struct Display {};
struct DisplayLockGuard { explicit DisplayLockGuard(Display*){} explicit operator bool() const{return true;} };
void lv_image_set_src(void*,void*) {} void lv_obj_add_flag(void*,int) {}
struct Application { std::vector<std::function<void()>> tasks; static Application& GetInstance(){static Application a;return a;} void Schedule(std::function<void()> f){tasks.push_back(std::move(f));} };
struct UrSignalingCall { enum class UiState {Hidden,Connecting,Active,Ended}; };
struct WatcherCallUi {
 Display display_storage; Display* display_=&display_storage; void* remote_image_=this;
 std::unique_ptr<LvglAllocatedImage> current_frame_,pending_frame_,spare_frame_; std::mutex frame_mutex_;
 std::uint64_t frame_generation_=0,pending_frame_generation_=0,displayed_frame_generation_=0;
 bool frame_accepting_=true;
 std::atomic<bool> frame_scheduled_{false}; std::atomic<int> ui_state_{int(UrSignalingCall::UiState::Active)};
 unsigned dropped_frames_=0; void DrainFrame(){} void ApplyState(UrSignalingCall::UiState){}
 void HandleState(UrSignalingCall::UiState); void HandleRemoteFrame(const std::uint8_t*,int,int);
};
PRODUCTION
int main(){
 WatcherCallUi ui; std::uint8_t pixels[8]={};
 std::thread frame([&]{ui.HandleRemoteFrame(pixels,2,2);});
 {std::unique_lock<std::mutex> l(gate_mutex);gate_cv.wait(l,[]{return entered;});}
 ui.HandleRemoteFrame(nullptr,0,0);
 {std::lock_guard<std::mutex> l(gate_mutex);proceed=true;} gate_cv.notify_all();frame.join();
 assert(!ui.pending_frame_ && "frame copied before clear was reintroduced after clear");
 // 清屏回调排队期间，新帧已上屏时，迟到的清屏不能擦掉新画面。
 ui.HandleRemoteFrame(pixels,2,2);
 {std::lock_guard<std::mutex> l(ui.frame_mutex_);ui.current_frame_=std::move(ui.pending_frame_);ui.displayed_frame_generation_=ui.pending_frame_generation_;}
 Application::GetInstance().tasks.front()();
 assert(ui.current_frame_ && "delayed clear removed a newer frame");
 // 终态信令排队到 UI 前，旧会话中的拷贝也必须作废。
 WatcherCallUi ending;
 {std::lock_guard<std::mutex> l(gate_mutex);entered=false;proceed=false;}
 std::thread old_frame([&]{ending.HandleRemoteFrame(pixels,2,2);});
 {std::unique_lock<std::mutex> l(gate_mutex);gate_cv.wait(l,[]{return entered;});}
 ending.HandleState(UrSignalingCall::UiState::Ended);
 {std::lock_guard<std::mutex> l(gate_mutex);proceed=true;} gate_cv.notify_all();old_frame.join();
 assert(!ending.pending_frame_ && "frame copied before hangup survived the call");
}
'''.replace('PRODUCTION', actual)
        with tempfile.TemporaryDirectory(prefix='watcher-frame-generation-') as directory:
            binary = str(Path(directory) / 'test')
            built = subprocess.run(['g++', '-std=c++17', '-pthread', '-Wall', '-Wextra',
                                    '-Werror', '-x', 'c++', '-', '-o', binary],
                                   input=code, text=True, capture_output=True, timeout=30)
            self.assertEqual(built.returncode, 0, built.stderr)
            result = subprocess.run([binary], text=True, capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
