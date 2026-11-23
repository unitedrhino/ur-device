"""执行生产视频任务，验证分片失败、停止屏障与有界帧节拍。"""
from pathlib import Path
import subprocess
import tempfile
import unittest
from .test_call_audio_decode import method

ROOT = Path(__file__).resolve().parents[2]


class CallUdpVideoWorkerTest(unittest.TestCase):
    """替换硬件与RTOS，保留生产视频任务和停止逻辑。"""

    def test_worker_stops_before_resources_and_drops_failed_frame(self):
        source = (ROOT / 'main/signaling/ur_signaling_call.cc').read_text()
        actual = '\n'.join(method(source, name) for name in
                           ['bool StartUdpVideo()', 'void StopUdpVideo()', 'void RunUdpVideo()',
                            'bool SendCallUDP(', 'bool SendEncodedCall('])
        harness = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
using namespace std::chrono_literals;
#define CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO 1
#define CONFIG_UR_SOFTWARE_AEC 1
#define CONFIG_FREERTOS_NUMBER_OF_CORES 2
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define pdPASS 1
#define portMAX_DELAY 0
#define pdMS_TO_TICKS(x) ((x)/10)
#define ESP_LOGI(...) ((void)0)
std::atomic<int64_t> clock_ms{1000};
int64_t esp_timer_get_time(){return clock_ms.load()*1000;}
using TickType_t=unsigned;
void vTaskDelay(int n){assert(n>=1);clock_ms+=n*10;std::this_thread::sleep_for(1ms);}
struct Semaphore{std::mutex mutex;std::condition_variable cv;bool done=false;};
using SemaphoreHandle_t=Semaphore*;using TaskHandle_t=std::thread*;
bool fail_semaphore=false,fail_task=false;int deleted_semaphores=0,video_priority=-1,video_core=-1;
Semaphore* xSemaphoreCreateBinary(){return fail_semaphore?nullptr:new Semaphore;}
void vSemaphoreDelete(Semaphore* p){++deleted_semaphores;delete p;}
void xSemaphoreGive(Semaphore* p){std::lock_guard<std::mutex> l(p->mutex);p->done=true;p->cv.notify_one();}
void xSemaphoreTake(Semaphore* p,int){std::unique_lock<std::mutex> l(p->mutex);p->cv.wait(l,[&]{return p->done;});}
void vTaskDeleteWithCaps(void*){}
int xTaskCreatePinnedToCoreWithCaps(void(*fn)(void*),const char*,int,void* ctx,int priority,TaskHandle_t* task,int core,int){video_priority=priority;video_core=core;if(fail_task)return 0;*task=new std::thread([=]{fn(ctx);});return pdPASS;}
struct Camera{uint32_t sequence=0;bool TakeStreamJPEG(std::vector<uint8_t>& out,uint32_t& seq){out={1};seq=++sequence;return true;}};
struct Board{static Board& GetInstance(){static Board b;return b;}Camera* GetCamera(){static Camera c;return &c;}};
struct Channel{unsigned frame=0;std::vector<int64_t> starts;std::atomic<int> active{0};std::atomic<bool> overlap{false};
 void enter(){if(active.fetch_add(1))overlap=true;std::this_thread::sleep_for(1ms);}void leave(){--active;}
 bool jpeg(const uint8_t*,size_t,std::vector<std::vector<uint8_t>>& out){enter();starts.push_back(clock_ms);out.assign(3,{static_cast<uint8_t>(++frame)});leave();return true;}
 bool audio(const std::vector<uint8_t>&,std::vector<uint8_t>& out){enter();out={200};leave();return true;}};
struct Port{std::mutex gate;std::condition_variable cv;bool block=false,entered=false;int fail_attempt=0;std::vector<uint8_t> attempts;
 bool send(const std::vector<uint8_t>& p){std::unique_lock<std::mutex> l(gate);attempts.push_back(p.front());entered=true;cv.notify_all();cv.wait(l,[&]{return !block;});return static_cast<int>(attempts.size())!=fail_attempt;}
 void wait_for(size_t n){std::unique_lock<std::mutex> l(gate);assert(cv.wait_for(l,2s,[&]{return attempts.size()>=n;}));}
};
int peer_connection_send_audio(void*,uint8_t*,size_t){return 0;}
struct Owner{bool media_active_=true,mic_paused_=false;uint64_t audio_capture_at_=0;};
struct PeerMedia{enum class PeerState{kConnected};Owner owner_;PeerState state_=PeerState::kConnected;void* pc_=nullptr;
 std::mutex udp_port_mutex_,udp_av_mutex_;std::atomic<bool> udp_video_stop_{true};bool udp_mode_=true;
 TaskHandle_t udp_video_task_=nullptr;SemaphoreHandle_t udp_video_stopped_=nullptr;
 uint64_t audio_send_us_=0;unsigned audio_sends_=0;int audio_up_=0,audio_up_fail_=0;
 Port udp_port_;std::unique_ptr<Channel> udp_channel_=std::make_unique<Channel>();
 ACTUAL
};
int main(){
 // 创建失败时回收屏障并保留可重试状态。
 {PeerMedia p;p.udp_mode_=false;assert(!p.StartUdpVideo());p.udp_mode_=true;
  fail_semaphore=true;assert(!p.StartUdpVideo());fail_semaphore=false;
  fail_task=true;auto before=deleted_semaphores;assert(!p.StartUdpVideo());fail_task=false;
  assert(deleted_semaphores==before+1&&!p.udp_video_task_&&!p.udp_video_stopped_);p.StopUdpVideo();}
 // stop必须等待在途发送完成，且结束后不再访问端口；暂停/关闭可安全释放相机和套接字。
 {PeerMedia p;p.udp_port_.block=true;assert(p.StartUdpVideo());assert(video_priority==4&&video_core==0);auto task=p.udp_video_task_;p.udp_port_.wait_for(1);
  std::atomic<bool> stopped{false};std::thread close([&]{p.StopUdpVideo();stopped=true;});std::this_thread::sleep_for(10ms);assert(!stopped.load());
  {std::lock_guard<std::mutex> l(p.udp_port_.gate);p.udp_port_.block=false;p.udp_port_.cv.notify_all();}
  close.join();task->join();delete task;assert(stopped&&p.udp_port_.attempts.size()==1);p.StopUdpVideo();}
 // 某片失败即丢弃该帧尾片；两种板型都必须遵守各自节拍且延时至少一个tick。
 {PeerMedia p;p.udp_port_.fail_attempt=2;assert(p.StartUdpVideo());auto task=p.udp_video_task_;p.udp_port_.wait_for(3);p.StopUdpVideo();task->join();delete task;
  assert(p.udp_port_.attempts[0]==1&&p.udp_port_.attempts[1]==1&&p.udp_port_.attempts[2]==2);
  assert(p.udp_channel_->starts.size()>=2&&p.udp_channel_->starts[1]-p.udp_channel_->starts[0]>=
         (CONFIG_BOARD_TYPE_ESP_SPARKBOT ? 1500 : 90));}
 // 音频和视频同时发送时，媒体加密端口不得并发使用，音频不应因视频任务而丢失。
 {PeerMedia p;assert(p.StartUdpVideo());auto task=p.udp_video_task_;p.udp_port_.wait_for(1);
  std::thread audio([&]{for(int i=0;i<30;++i){std::vector<uint8_t> opus{42};assert(p.SendEncodedCall(opus));}});
  audio.join();p.StopUdpVideo();task->join();delete task;assert(p.audio_up_==30&&!p.udp_channel_->overlap.load());}
}
'''.replace('ACTUAL', actual)
        output = ROOT.parents[1] / '.temp'
        output.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=output) as directory:
            for tank in (0, 1):
                binary = str(Path(directory) / f'video-worker-{tank}')
                built = subprocess.run(['g++', '-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror',
                                        f'-DCONFIG_BOARD_TYPE_ESP_SPARKBOT={tank}',
                                        f'-DCONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER={1-tank}',
                                        '-x', 'c++', '-', '-o', binary], input=harness, text=True,
                                       capture_output=True, timeout=30)
                self.assertEqual(built.returncode, 0, built.stderr)
                result = subprocess.run([binary], text=True, capture_output=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_slow_watcher_frame_still_yields_cpu(self):
        """实际取帧加密超过90ms时，仍应阻塞一tick，让低优先级控制台和空闲任务运行。"""
        source = (ROOT / 'main/signaling/ur_signaling_call.cc').read_text()
        actual = method(source, 'void RunUdpVideo()')
        code = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>
using TickType_t=unsigned;
#define CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO 1
#define CONFIG_BOARD_TYPE_ESP_SPARKBOT 0
#define CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER 1
#define pdMS_TO_TICKS(x) (x)
static int64_t clock_us=1000000;
static unsigned yields=0;
int64_t esp_timer_get_time(){return clock_us;}
void vTaskDelay(TickType_t ticks){assert(ticks>=1);++yields;clock_us+=ticks*1000;}
struct Camera {
 bool TakeStreamJPEG(std::vector<uint8_t>& out,uint32_t& seq){out={1};++seq;clock_us+=110000;return true;}
};
struct Board {static Board& GetInstance(){static Board board;return board;} Camera* GetCamera(){static Camera camera;return &camera;}};
struct Channel {bool jpeg(const uint8_t*,size_t,std::vector<std::vector<uint8_t>>& out){out={{1}};clock_us+=10000;return true;}};
struct Port {
 std::atomic<bool>* stop=nullptr;unsigned sent=0;
 bool send(const std::vector<uint8_t>&){if(++sent==3)stop->store(true);return true;}
};
struct Media {
 std::atomic<bool> udp_video_stop_{false};
 std::mutex udp_av_mutex_,udp_port_mutex_;
 std::unique_ptr<Channel> udp_channel_=std::make_unique<Channel>();
 Port udp_port_;
ACTUAL
};
int main(){Media media;media.udp_port_.stop=&media.udp_video_stop_;media.RunUdpVideo();assert(media.udp_port_.sent==3 && yields>=3);}
'''.replace('ACTUAL', actual)
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / 'slow-video')
            subprocess.run(['g++','-std=c++17','-pthread','-x','c++','-','-o',binary],
                           input=code,text=True,check=True)
            result = subprocess.run([binary],capture_output=True,text=True,timeout=5)
            self.assertEqual(result.returncode,0,result.stderr)
