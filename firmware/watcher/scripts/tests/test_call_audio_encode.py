"""执行生产人际编码任务及采集入口，覆盖事件线程独立推进、代次、静音、背压和关闭屏障。"""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest
from .test_call_audio_decode import method

ROOT = Path(__file__).resolve().parents[2]

class CallAudioEncodeTest(unittest.TestCase):
    def test_watcher_reference_uses_serial_capture_and_send(self):
        """Watcher回采构建执行真实同步入口，覆盖静音、缺帧与编码失败的发送边界。"""
        source = (ROOT / 'main/signaling/ur_signaling_call.cc').read_text()
        actual = method(source, 'void PumpCapture()')
        code = r'''
#include <cassert>
#include <cstdint>
#include <vector>
#define CONFIG_UR_SOFTWARE_AEC 1
#define CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER 1
constexpr size_t kIn16kSamples = 320;
namespace ur::device { enum class PcmResult { Frame, WouldBlock }; }
// 只替代采集、编码与网络端口，保留生产的同步选择及边界判断。
struct Media {
    struct Owner { void* pcm_ = nullptr; bool media_active_ = false, mic_paused_ = false; } owner_;
    bool call_codec_ = true, encode_ok = true, output_empty = false;
    ur::device::PcmResult next = ur::device::PcmResult::Frame;
    size_t samples = 320;
    int reads = 0, encodes = 0, sends = 0;
    ur::device::PcmResult ReadCapture(std::vector<int16_t>& pcm) {
        ++reads;
        pcm.assign(samples, 123);
        return next;
    }
    bool EncodeCall(const std::vector<int16_t>& pcm, std::vector<uint8_t>& opus) {
        assert(pcm.size() == 320 && pcm.front() == 123);
        ++encodes;
        if (!output_empty) opus = {42};
        return encode_ok;
    }
    void SendEncodedCall(const std::vector<uint8_t>& opus) {
        assert(opus == std::vector<uint8_t>{42});
        ++sends;
    }
    ACTUAL
};
int main() {
    Media media;
    media.PumpCapture(); assert(media.reads == 0);
    media.owner_.pcm_ = &media;
    media.PumpCapture(); assert(media.reads == 0);
    media.owner_.media_active_ = true;
    media.owner_.mic_paused_ = true;
    media.PumpCapture(); assert(media.reads == 0);
    media.owner_.mic_paused_ = false;
    media.next = ur::device::PcmResult::WouldBlock;
    media.PumpCapture(); assert(media.reads == 1 && media.encodes == 0);
    media.next = ur::device::PcmResult::Frame;
    media.samples = 319;
    media.PumpCapture(); assert(media.encodes == 0);
    media.samples = 320;
    media.PumpCapture(); assert(media.encodes == 1 && media.sends == 1);
    media.encode_ok = false;
    media.PumpCapture(); assert(media.sends == 1);
    media.encode_ok = true;
    media.output_empty = true;
    media.PumpCapture(); assert(media.sends == 1);
}
'''.replace('ACTUAL', actual)
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / 'watcher-capture')
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-x', 'c++', '-', '-o', binary], input=code, text=True, check=True)
            result = subprocess.run([binary], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
    """仅替换DSP和FreeRTOS，实际队列/代次/采集入口保持生产代码。"""
    def test_actual_encoder_worker_and_capture_ownership(self):
        source=(ROOT/'main/signaling/ur_signaling_call.cc').read_text()
        actual='\n'.join(method(source,name) for name in ['bool EnsureCallEncoder()', 'void StopCallEncoder()', 'void RunCallEncoder()', 'void ResetCallEncoding()', 'bool QueueCallCapture()', 'void PumpCallEncoded()'])
        capture=method(source,'void PumpCapture()')
        actual+='\n'+capture
        harness=r'''
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
using namespace std::chrono_literals;
#define CONFIG_UR_SOFTWARE_AEC 1
#define CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO 1
#define CONFIG_FREERTOS_NUMBER_OF_CORES 2
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define pdPASS 1
#define portMAX_DELAY 0
constexpr int kIn16kSamples=320;
static int64_t simulated_cost_us=0;  // 模拟算法任务被发送唤醒后的抢占时长。
int64_t esp_timer_get_time(){return simulated_cost_us+std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
int peer_connection_send_audio(void*,uint8_t*,size_t){return 0;}
namespace ur::device{enum class PcmResult{Frame,Empty};}
struct Semaphore{std::mutex m;std::condition_variable cv;bool done=false;};
using SemaphoreHandle_t=Semaphore*;using TaskHandle_t=std::thread*;
std::atomic<unsigned> event_wakes{0};
const auto event_thread=std::this_thread::get_id();
TaskHandle_t xTaskGetCurrentTaskHandle(){return reinterpret_cast<TaskHandle_t>(std::this_thread::get_id()==event_thread?uintptr_t{1}:uintptr_t{2});}
void xTaskNotifyGive(TaskHandle_t t){assert(t==reinterpret_cast<TaskHandle_t>(uintptr_t{1}));++event_wakes;}
bool fail_task=false;unsigned deleted_semaphores=0;
Semaphore* xSemaphoreCreateBinary(){return new Semaphore;}
void vSemaphoreDelete(Semaphore*p){++deleted_semaphores;delete p;}
void xSemaphoreGive(Semaphore*p){std::lock_guard<std::mutex>lock(p->m);p->done=true;p->cv.notify_one();}
void xSemaphoreTake(Semaphore*p,int){std::unique_lock<std::mutex>lock(p->m);p->cv.wait(lock,[&]{return p->done;});}
void vTaskDeleteWithCaps(void*){}
int xTaskCreatePinnedToCoreWithCaps(void(*fn)(void*),const char*,int,void*c,int,TaskHandle_t*t,int core,int){assert(core==1);if(fail_task)return 0;*t=new std::thread([=]{fn(c);});return pdPASS;}
struct Codec{std::mutex m;std::condition_variable cv;bool blocked=true,entered=false;
 bool encode(const std::vector<int16_t>&pcm,std::vector<uint8_t>&out){std::unique_lock<std::mutex>lock(m);entered=true;cv.notify_all();cv.wait(lock,[&]{return !blocked;});out={uint8_t(pcm.at(0))};return true;}
 void wait_entered(){std::unique_lock<std::mutex>lock(m);assert(cv.wait_for(lock,1s,[&]{return entered;}));}
 void release(){std::lock_guard<std::mutex>lock(m);blocked=false;cv.notify_all();}
};
// Pcm替代硬件队列时保留互斥与有界等待，避免夹具自身制造生产者/工作者数据竞争。
struct Pcm{std::mutex mutex;std::condition_variable cv;std::deque<int16_t> values{1};
 ur::device::PcmResult read(std::vector<int16_t>&out){std::lock_guard<std::mutex>lock(mutex);if(values.empty())return ur::device::PcmResult::Empty;out.assign(320,values.front());values.pop_front();return ur::device::PcmResult::Frame;}
 void Push(int16_t value){std::lock_guard<std::mutex>lock(mutex);values.push_back(value);cv.notify_one();}
 size_t size(){std::lock_guard<std::mutex>lock(mutex);return values.size();}
 void WaitCapture(){std::unique_lock<std::mutex>lock(mutex);cv.wait_for(lock,20ms,[&]{return !values.empty();});}
};
struct Owner{Pcm*pcm_;bool media_active_=true,mic_paused_=false;uint64_t audio_capture_at_=0;};
struct PeerMedia{
 explicit PeerMedia(Owner owner):owner_(owner){}
 Owner owner_;std::unique_ptr<Codec>call_codec_=std::make_unique<Codec>();void*pc_=this;int audio_up_=0,audio_up_fail_=0;
 std::mutex call_encode_mutex_;std::condition_variable call_encode_cv_;bool call_capture_active_=false;
 std::deque<std::vector<uint8_t>>call_encode_ready_;
 uint64_t call_encode_generation_=0;bool call_encode_shutdown_=false,call_encode_busy_=false;
 TaskHandle_t call_encode_task_=nullptr,call_event_task_=nullptr;SemaphoreHandle_t call_encode_stopped_=nullptr;
 bool udp_mode_=false;int rx_waiting=0,rx_received=0,rx_dropped=0;bool costly=false,slow_playback=false;int playback_pumps=0;
 void ReceiveUDP(int limit){if(costly)simulated_cost_us+=12000;while(limit--&&rx_waiting){--rx_waiting;++rx_received;}}
 void PumpPlayback(int){++playback_pumps;if(slow_playback)simulated_cost_us+=12000;}
 std::vector<uint8_t>sent;std::thread::id sender;
 ur::device::PcmResult ReadCapture(std::vector<int16_t>&p){return owner_.pcm_->read(p);}
 bool EncodeCall(const std::vector<int16_t>&p,std::vector<uint8_t>&o){return call_codec_->encode(p,o);}
 bool SendEncodedCall(std::vector<uint8_t>&p){if(udp_mode_){rx_waiting+=4;if(rx_waiting>6){rx_dropped+=rx_waiting-6;rx_waiting=6;}}sent.push_back(p.at(0));sender=std::this_thread::get_id();return true;}
''' + actual + r'''
};
template<class Fn>bool eventually(Fn fn){for(int i=0;i<200;++i){if(fn())return true;std::this_thread::sleep_for(2ms);}return false;}
void join_task(TaskHandle_t t){t->join();delete t;}
int main(){
 // 四帧结果保留突发排空余量，逐包排六包邮箱；剩余帧须由下一次继续发送。
 {Pcm pcm;PeerMedia media{{&pcm}};media.udp_mode_=true;for(int i=0;i<4;++i)media.call_encode_ready_.push_back({uint8_t(i+1)});
  media.PumpCallEncoded();assert((media.sent==std::vector<uint8_t>{1,2,3,4}));assert(media.rx_received==16&&media.rx_dropped==0);
  media.PumpCallEncoded();assert((media.sent==std::vector<uint8_t>{1,2,3,4}));assert(media.rx_received==16&&media.rx_dropped==0&&media.rx_waiting==0);}
 // 持续有结果时，实际抢占超过10ms后必须退出本次发送，保留余帧等后续调度。
 {Pcm pcm;PeerMedia media{{&pcm}};media.udp_mode_=true;media.costly=true;
  for(int i=0;i<4;++i)media.call_encode_ready_.push_back({uint8_t(i+1)});
  media.PumpCallEncoded();assert(media.sent.size()==1 && "packet count bound starved event loop");
  assert(media.call_encode_ready_.size()==3);}
 // 批内逐包收网，但批尾才递交解码：不能在每个包之间被慢算法抢占。
 {Pcm pcm;PeerMedia media{{&pcm}};media.udp_mode_=true;media.slow_playback=true;
  for(int i=0;i<4;++i)media.call_encode_ready_.push_back({uint8_t(i+1)});
  media.PumpCallEncoded();assert(media.sent.size()==4 && media.playback_pumps==1);
  assert(media.rx_received==16 && media.rx_dropped==0);}
 // 事件线程不再调用采集入口时，工作任务仍须消费下一批AFE样本。
 {Pcm pcm;PeerMedia media{{&pcm}};media.call_codec_->release();assert(media.EnsureCallEncoder());auto task=media.call_encode_task_;
  const auto before=event_wakes.load();assert(media.QueueCallCapture());
  assert(eventually([&]{return event_wakes.load()>before;}) && "encoded result did not wake the media event task");
  assert(media.sent.empty());assert(eventually([&]{media.PumpCallEncoded();return media.sent.size()==1;}));
  pcm.Push(9);bool independent=eventually([&]{media.PumpCallEncoded();return media.sent.size()==2;});
  media.StopCallEncoder();join_task(task);if(!independent){std::cerr<<"capture consumption depends on event loop";return 1;}
 }
 // 真实PumpCapture在算法阻塞时必须返回，网络发送只能由原事件线程完成。
 {Pcm pcm;PeerMedia media{{&pcm}};assert(media.EnsureCallEncoder());auto task=media.call_encode_task_;
  std::atomic<bool>returned=false;std::thread pump([&]{media.PumpCapture();returned=true;});media.call_codec_->wait_entered();
  bool independent=eventually([&]{return returned.load();});
  if(!independent){media.call_codec_->release();pump.join();media.StopCallEncoder();join_task(task);std::cerr<<"capture waited for blocked encoder";return 1;}
  pump.join();media.ResetCallEncoding();pcm.Push(2);assert(media.QueueCallCapture());media.call_codec_->release();
  assert(eventually([&]{media.PumpCallEncoded();return !media.sent.empty();}));assert((media.sent==std::vector<uint8_t>{2}));assert(media.sender==std::this_thread::get_id());
  media.StopCallEncoder();join_task(task);
 }
 // 关闭必须等在途算法及队列访问结束，不能先销毁编解码器。
 {Pcm pcm;PeerMedia media{{&pcm}};assert(media.EnsureCallEncoder());auto task=media.call_encode_task_;assert(media.QueueCallCapture());media.call_codec_->wait_entered();
  std::atomic<bool>stopped=false;std::thread stopper([&]{media.StopCallEncoder();stopped=true;});std::this_thread::sleep_for(15ms);assert(!stopped);
  media.call_codec_->release();stopper.join();assert(stopped);join_task(task);assert(media.sent.empty());
 }
 // 四帧结果满时不能额外消费PCM；解除背压后继续处理，且不得突破四帧上限。
 {Pcm pcm;PeerMedia media{{&pcm}};media.call_codec_->release();assert(media.EnsureCallEncoder());auto task=media.call_encode_task_;
  {std::lock_guard<std::mutex>lock(media.call_encode_mutex_);for(int i=0;i<4;++i)media.call_encode_ready_.push_back({3});}
  pcm.Push(4);pcm.Push(4);pcm.Push(4);
  assert(!media.QueueCallCapture()&&pcm.size()==4);
  assert(eventually([&]{media.PumpCallEncoded();return media.sent.size()>=8;}));
  {std::lock_guard<std::mutex>lock(media.call_encode_mutex_);assert(media.call_encode_ready_.size()<=4);}
  media.StopCallEncoder();join_task(task);
 }
 // 静音作废在途及待发帧，恢复后只发送本次新PCM。
 {Pcm pcm;PeerMedia media{{&pcm}};assert(media.EnsureCallEncoder());auto task=media.call_encode_task_;assert(media.QueueCallCapture());media.call_codec_->wait_entered();
  media.owner_.mic_paused_=true;media.PumpCallEncoded();media.owner_.mic_paused_=false;pcm.Push(5);assert(media.QueueCallCapture());media.call_codec_->release();
  assert(eventually([&]{media.PumpCallEncoded();return !media.sent.empty();}));assert((media.sent==std::vector<uint8_t>{5}));media.StopCallEncoder();join_task(task);
 }
 // 创建失败回收屏障，不留半初始化任务，关闭幂等。
 {Pcm pcm;PeerMedia media{{&pcm}};auto before=deleted_semaphores;fail_task=true;assert(!media.EnsureCallEncoder());fail_task=false;
  assert(deleted_semaphores==before+1&&!media.call_encode_task_&&!media.call_encode_stopped_);media.StopCallEncoder();}
}
'''
        output=ROOT.parents[1]/'.temp';output.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=output) as directory:
            binary=Path(directory)/'encode'
            flags=['-fsanitize=address,undefined','-fno-omit-frame-pointer'] if os.environ.get('CALL_TEST_SANITIZE') else []
            p=subprocess.run(['g++','-std=c++17','-pthread','-Wall','-Wextra','-Werror',*flags,'-x','c++','-','-o',str(binary)],input=harness,text=True,capture_output=True)
            self.assertEqual(p.returncode,0,p.stderr)
            p=subprocess.run([str(binary)],capture_output=True,timeout=10)
            self.assertEqual(p.returncode,0,p.stderr.decode(errors='replace'))

if __name__=='__main__':unittest.main()
