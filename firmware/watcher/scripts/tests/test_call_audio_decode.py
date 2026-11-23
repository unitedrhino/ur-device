"""编译生产人际解码与播放交付：同步UDP背压、DSP任务隔离、取消在途帧与关闭等待。"""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def method(source, name):
    """按括号提取真实成员定义，避免另写一份生产状态机冒充回归。"""
    start = source.index(name)
    begin = source.index('{', start)
    level = 1
    end = begin + 1
    while level:
        level += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


class CallAudioDecodeTest(unittest.TestCase):
    """用受控阻塞解码器验证实际任务边界，FreeRTOS仅替换调度和完成屏障。"""
    def test_watcher_speech_matches_reference_and_keeps_frame_history(self):
        """真实整形器与独立实验向量一致，分帧、重置、极值和静音不产生残留。"""
        harness = r'''
#include <cassert>
#include <cmath>
#include <cstdint>
#include <vector>
#include "watcher_call_audio.h"
int main() {
    // 实验向量由SciPy二阶Butterworth及已确认的30ms包络独立生成。
    std::vector<int16_t> step(32, 5000);
    step[0] = step[1] = 0;
    for (size_t i = 16; i < step.size(); ++i) step[i] = -5000;
    const std::vector<int16_t> golden = {
        0, 0, 10000, 6706, 4021, 1900, 284, -892, -1696, -2196, -2452, -2521,
        -2452, -2288, -2062, -1803, -10000, -6813, -4200, -2123, -528, 644,
        1458, 1975, 2254, 2349, 2307, 2169, 1968, 1730, 1478, 1228
    };
    WatcherCallAudio reference;
    auto processed = step;
    reference.Process(processed);
    for (size_t i = 0; i < golden.size(); ++i)
        assert(std::abs(int(processed[i]) - int(golden[i])) <= 1);

    // 不同网络帧边界应得到相同PCM，防止每帧重置造成爆音。
    std::vector<int16_t> source(16000);
    for (size_t i = 0; i < source.size(); ++i)
        source[i] = static_cast<int16_t>(12000 * std::sin(i * 0.12) + 8000 * std::sin(i * 0.7));
    WatcherCallAudio whole, chunked;
    auto expected = source;
    whole.Process(expected);
    std::vector<int16_t> actual;
    for (size_t at = 0; at < source.size(); at += 137) {
        std::vector<int16_t> frame(source.begin() + at, source.begin() + std::min(at + 137, source.size()));
        chunked.Process(frame);
        actual.insert(actual.end(), frame.begin(), frame.end());
    }
    assert(actual == expected);
    chunked.Reset();
    processed = step;
    chunked.Process(processed);
    for (size_t i = 0; i < golden.size(); ++i)
        assert(std::abs(int(processed[i]) - int(golden[i])) <= 1);

    // 负满幅、正满幅及冲击均保留模拟峰值余量，静音尾部最终归零。
    std::vector<int16_t> extremes(320);
    for (size_t i = 0; i < extremes.size(); ++i) extremes[i] = i % 2 ? 32767 : -32768;
    chunked.Process(extremes);
    for (auto sample : extremes) assert(std::abs(int(sample)) <= 10000);
    for (int i = 0; i < 100; ++i) {
        std::vector<int16_t> silence(320);
        chunked.Process(silence);
        if (i > 1) for (auto sample : silence) assert(sample == 0);
    }
    std::vector<int16_t> empty;
    chunked.Process(empty);
    assert(empty.empty());
}
'''
        temp_root = ROOT.parents[1] / '.temp'
        temp_root.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=temp_root) as directory:
            binary = Path(directory) / 'watcher-call-audio'
            result = subprocess.run(
                ['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                 '-I', str(ROOT / 'main/signaling'), '-x', 'c++', '-', '-o', str(binary)],
                input=harness, text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(binary)], text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_only_watcher_successful_call_decodes_are_conditioned(self):
        """提取真实通话解码入口验证板型隔离，失败解码不能推进整形历史。"""
        source = (ROOT / 'main/signaling/ur_signaling_call.cc').read_text()
        decode = method(source, 'bool DecodeCall(')
        harness = r'''
#include <cassert>
#include <cstdint>
#include <mutex>
#include <vector>
#include "watcher_call_audio.h"
int64_t esp_timer_get_time() { return 1; }
int xPortGetCoreID() { return 0; }
// Codec 仅替换SDK解码结果，让测试能控制成功和失败而复用生产整形器。
struct Codec {
    bool ok = true;
    bool decode(const std::vector<uint8_t>&, std::vector<int16_t>& pcm) {
        pcm.assign(32, 5000);
        return ok;
    }
};
// PeerMedia 保留真实解码入口依赖的本通话状态与统计字段。
struct PeerMedia {
    Codec codec;
    Codec* call_codec_ = &codec;
#if CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
    WatcherCallAudio call_audio_;
#endif
    std::mutex decode_trace_mutex_;
    int last_decode_core_ = -1;
    int64_t decode_time_us_ = 0;
    unsigned decode_attempts_ = 0, decode_failures_ = 0;
    DECODE
};
int main() {
    PeerMedia current, control;
    std::vector<int16_t> actual, expected;
    current.codec.ok = false;
    assert(!current.DecodeCall({1}, actual));
    assert(current.decode_failures_ == 1);
    current.codec.ok = true;
    assert(current.DecodeCall({1}, actual));
    assert(control.DecodeCall({1}, expected));
    assert(actual == expected && actual.size() == 32);
#if CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
    assert(actual.front() == 10000 && actual.back() != 5000);
#else
    assert(actual == std::vector<int16_t>(32, 5000));
#endif
    assert(current.decode_attempts_ == 2 && current.last_decode_core_ == 0);
}
'''.replace('DECODE', decode)
        temp_root = ROOT.parents[1] / '.temp'
        temp_root.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=temp_root) as directory:
            for watcher in (0, 1):
                binary = Path(directory) / f'call-audio-{watcher}'
                result = subprocess.run(
                    ['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                     f'-DCONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER={watcher}',
                     '-I', str(ROOT / 'main/signaling'), '-x', 'c++', '-', '-o', str(binary)],
                    input=harness, text=True, capture_output=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                result = subprocess.run([str(binary)], text=True, capture_output=True)
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_synchronous_playback_keeps_packets_until_driver_has_capacity(self):
        """同步构建在播放满时保留压缩包，驱动释放后按原顺序解码交付。"""
        source = (ROOT / 'main/signaling/ur_signaling_call.cc').read_text()
        playback = method(source, 'void PumpPlayback(int limit = 1)')
        receive = method(method(source, 'void ReceiveUDP(int limit)'), 'if (!audio.empty())')
        on_audio = method(source, 'static void OnAudioTrack(')
        harness = r'''
#include <cassert>
#include <cstdint>
#include <deque>
#include <vector>
#ifndef CONFIG_UR_SOFTWARE_AEC
#define CONFIG_UR_SOFTWARE_AEC 0
#endif
constexpr size_t kDownlinkLimit = 100;
// PsramBuffer 替换分配器，保留生产队列中的完整包所有权。
struct PsramBuffer : std::vector<uint8_t> {
    using std::vector<uint8_t>::vector;
    bool Assign(const uint8_t* data, size_t size) { assign(data, data + size); return true; }
};
// Pcm 模拟两个在途播放槽，完成回执由测试显式释放。
struct Pcm {
    unsigned pending = 0;
    unsigned rejected = 0;
    std::vector<int16_t> delivered;
    void PumpPlayback() {}
    bool playback_backpressured(uint64_t) const { return pending == 2; }
    bool write(const std::vector<int16_t>& pcm) {
        if (pending == 2) {
            ++rejected;
            return false;
        }
        ++pending;
        delivered.push_back(pcm.front());
        return true;
    }
};
// PeerMedia 编译真实收包和播放方法，仅替换编解码及硬件端口。
struct PeerMedia {
    enum class PeerState { kConnected, kIdle };
    PeerState state_ = PeerState::kIdle;
    bool udp_mode_ = true;
    struct Owner {
        Pcm* pcm_;
        bool media_active_ = true;
        uint64_t playback_epoch_ = 1;
    } owner_;
    bool call_codec_ = true;
    unsigned decoded = 0;
    std::deque<PsramBuffer> downlink_;
    std::vector<uint8_t> decode_in_;
    bool DecodeCall(const std::vector<uint8_t>& opus, std::vector<int16_t>& pcm) {
        ++decoded;
        pcm.assign(1, opus.front());
        return true;
    }
    bool WriteCallPCM(const std::vector<int16_t>& pcm) { return owner_.pcm_->write(pcm); }
    PLAYBACK
    ON_AUDIO
    void ReceiveAudio(std::vector<uint8_t> audio) { RECEIVE }
};
int main() {
    Pcm pcm;
    PeerMedia media;
    media.owner_.pcm_ = &pcm;
    media.downlink_ = {{1}, {2}, {3}, {4}};
    media.PumpPlayback(4);
    assert(media.decoded == 2);
    assert(media.downlink_.size() == 2);
    assert(pcm.rejected == 0);
    pcm.pending = 0;
    media.PumpPlayback(4);
    assert(media.decoded == 4);
    assert(media.downlink_.empty());
    assert(pcm.rejected == 0);
    assert((pcm.delivered == std::vector<int16_t>{1, 2, 3, 4}));
    // 正式UDP收包分支只暂存原包，背压由同一播放泵处理。
    Pcm udp_pcm;
    PeerMedia udp;
    udp.owner_.pcm_ = &udp_pcm;
    for (uint8_t value = 1; value <= 4; ++value) udp.ReceiveAudio({value});
    assert(udp.decoded == 0 && udp_pcm.delivered.empty());
    udp.PumpPlayback(4);
    assert(udp.decoded == 2 && udp.downlink_.size() == 2 && udp_pcm.rejected == 0);
    udp_pcm.pending = 0;
    udp.PumpPlayback(4);
    assert((udp_pcm.delivered == std::vector<int16_t>{1, 2, 3, 4}));
    // 持续拥塞最多暂存80ms的新音频，避免修复丢帧时积累秒级旧声音。
    for (uint8_t value = 5; value <= 11; ++value) udp.ReceiveAudio({value});
    assert(udp.downlink_.size() == 4 && udp.downlink_.front().front() == 8);
    // 原WebRTC暂存上限保持100帧。
    udp.downlink_.clear();
    udp.udp_mode_ = false;
    for (int i = 0; i < 101; ++i) udp.ReceiveAudio({1});
    assert(udp.downlink_.size() == 100);
}
'''.replace('PLAYBACK', playback).replace('ON_AUDIO', on_audio).replace('RECEIVE', receive)
        temp_root = ROOT.parents[1] / '.temp'
        temp_root.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=temp_root) as directory:
            binary = Path(directory) / 'sync-playback'
            # Watcher编译了回采参考时，也必须执行真实同步背压与压缩包保留路径。
            for software,watcher in [(0,0),(1,1)]:
                with self.subTest(software_reference=software,watcher=watcher):
                    compile_result = subprocess.run(
                        ['g++', '-std=c++17', f'-DCONFIG_UR_SOFTWARE_AEC={software}',
                         f'-DCONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER={watcher}',
                         '-x', 'c++', '-', '-o', str(binary)],
                        input=harness, text=True, capture_output=True)
                    self.assertEqual(compile_result.returncode, 0, compile_result.stderr)
                    result = subprocess.run([str(binary)], capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stderr)

    def test_actual_worker_lifecycle_and_backpressure(self):
        source = (ROOT / 'main/signaling/ur_signaling_call.cc').read_text()
        methods = '\n'.join(method(source, name) for name in [
            'bool EnsureCallDecoder()', 'void StopCallDecoder()', 'void RunCallDecoder()',
            'void PumpCallDecoded()', 'void ResetPlayback()', 'ur::device::PcmResult ReadCapture(', 'bool WriteCallPCM(', 'bool SendCallUDP('])
        methods += '\n' + method(source, 'void PumpPlayback(int limit = 1)')
        capture = method(source, 'if (!owner_.mic_paused_)')
        methods += '\nvoid CaptureUDP() { const auto started = esp_timer_get_time(); std::vector<uint8_t> audio,packet;'+capture+'}'
        harness = r'''
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
int64_t esp_timer_get_time(){return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
#define CONFIG_UR_SOFTWARE_AEC 1
#define CONFIG_FREERTOS_NUMBER_OF_CORES 2
#define ESP_LOGI(...) ((void)0)
namespace ur::device {enum class PcmResult {Frame,Empty};}
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define pdPASS 1
#define portMAX_DELAY 0
struct Semaphore {std::mutex m;std::condition_variable cv;bool done=false;};
using SemaphoreHandle_t=Semaphore*;using TaskHandle_t=std::thread*;
bool fail_task=false;unsigned deleted_semaphores=0;
Semaphore* xSemaphoreCreateBinary(){return new Semaphore;}
void vSemaphoreDelete(Semaphore* p){++deleted_semaphores;delete p;}
void xSemaphoreGive(Semaphore* p){std::lock_guard<std::mutex> l(p->m);p->done=true;p->cv.notify_one();}
void xSemaphoreTake(Semaphore* p,int){std::unique_lock<std::mutex> l(p->m);p->cv.wait(l,[&]{return p->done;});}
void vTaskDeleteWithCaps(void*){}
int xTaskCreateWithCaps(void(*fn)(void*),const char*,int,void* c,int,TaskHandle_t* t,int){if(fail_task)return 0;*t=new std::thread([=]{fn(c);});return pdPASS;}
int xTaskCreatePinnedToCoreWithCaps(void(*fn)(void*),const char*name,int stack,void*c,int priority,TaskHandle_t*t,int core,int caps){assert(core==1);return xTaskCreateWithCaps(fn,name,stack,c,priority,t,caps);}
struct Codec {
 std::mutex mutex;std::condition_variable cv;bool blocked=true,entered=false;
 bool decode(const std::vector<uint8_t>& b,std::vector<int16_t>& out){
  std::unique_lock<std::mutex> l(mutex);entered=true;cv.notify_all();cv.wait(l,[&]{return !blocked;});out.assign(320,b.at(0));return true;
 }
 void wait_entered(){std::unique_lock<std::mutex> l(mutex);assert(cv.wait_for(l,1s,[&]{return entered;}));}
 void release(){std::lock_guard<std::mutex> l(mutex);blocked=false;cv.notify_all();}
};
struct Pcm {
 bool pressured=false;int captures=64;std::vector<int16_t> played;std::thread::id writer;
 ur::device::PcmResult read_frame(std::vector<int16_t>& p){if(captures--<=0)return ur::device::PcmResult::Empty;p.assign(320,0);return ur::device::PcmResult::Frame;}
 void PumpPlayback(){}
 bool playback_backpressured(uint64_t)const{return pressured;}
 bool write_frame(const std::vector<int16_t>& p,uint64_t){played.push_back(p.at(0));writer=std::this_thread::get_id();return true;}
};
struct Owner {Pcm* pcm_;bool media_active_=true;uint64_t playback_epoch_=1;bool mic_paused_=false;uint64_t audio_capture_at_=0;};
struct Channel {bool audio(const std::vector<uint8_t>&,std::vector<uint8_t>& p){p={1};return true;}};
struct Port {bool send(const std::vector<uint8_t>&){return true;}};
struct Frame {std::vector<uint8_t> v;uint8_t* data(){return v.data();}size_t size(){return v.size();}};
struct PeerMedia {
 Owner owner_;std::unique_ptr<Codec> call_codec_=std::make_unique<Codec>();std::deque<Frame> downlink_;std::vector<uint8_t> decode_in_;
 struct CallDecodeRequest {std::vector<uint8_t> opus;uint64_t generation;};
 unsigned udp_decrypt_calls_=0;  // 网络工作计数供实际服务循环判断，夹具本例无新UDP。
 std::mutex decode_trace_mutex_;  // 采集诊断的真实跨线程短锁。
 std::mutex call_decode_mutex_;std::condition_variable call_decode_cv_;
 std::deque<CallDecodeRequest> call_decode_waiting_;std::deque<std::vector<int16_t>> call_decode_ready_;
 uint64_t call_decode_generation_=0;bool call_decode_shutdown_=false,call_decode_busy_=false;
 TaskHandle_t call_decode_task_=nullptr;SemaphoreHandle_t call_decode_stopped_=nullptr;unsigned playback_rejected_=0;
 std::unique_ptr<Channel> udp_channel_=std::make_unique<Channel>();Port udp_port_;std::mutex udp_port_mutex_;int audio_up_=0;
 uint64_t capture_read_us_=0,playback_submit_us_=0,audio_send_us_=0;unsigned capture_reads_=0,playback_submits_=0,audio_sends_=0;
 void ResetCallEncoding(){}  // 编码代次由独立编码任务回放验证，解码用例隔离其外部端口。
 bool QueueCallCapture(){std::this_thread::sleep_for(4ms);return true;}
 void PumpCallEncoded(){}
 void ReceiveUDP(int){}  // 此用例只隔离解码生命周期，网络公平性由收包回放覆盖。
 bool EncodeCall(const std::vector<int16_t>&,std::vector<uint8_t>& out){std::this_thread::sleep_for(4ms);out={1};return true;}
 bool DecodeCall(const std::vector<uint8_t>& b,std::vector<int16_t>& p){return call_codec_->decode(b,p);}
''' + methods + r'''
};
template<class Fn> bool eventually(Fn fn){for(int i=0;i<200;++i){if(fn())return true;std::this_thread::sleep_for(2ms);}return false;}
void join_task(TaskHandle_t task){task->join();delete task;}
int main(){
 // 阻塞算法时Pump必须返回；Reset之后仅能提交新帧，并保持PCM票据的线程归属。
 {
  Pcm pcm;PeerMedia media{{&pcm}};assert(media.EnsureCallDecoder());auto task=media.call_decode_task_;
  media.downlink_.push_back({{1}});std::atomic<bool> returned=false;
  std::thread pump([&]{media.PumpPlayback(8);returned=true;});
  media.call_codec_->wait_entered();bool independent=eventually([&]{return returned.load();});
  if(!independent){media.call_codec_->release();pump.join();media.StopCallDecoder();join_task(task);std::cerr<<"media pump waited for blocked decoder";return 1;}
  pump.join();media.ResetPlayback();media.downlink_.push_back({{2}});media.PumpPlayback(8);
  media.call_codec_->release();assert(eventually([&]{media.PumpPlayback(8);return !pcm.played.empty();}));
  assert(pcm.played.size()==1&&pcm.played[0]==2&&pcm.writer==std::this_thread::get_id());
  media.StopCallDecoder();join_task(task);
 }
 // 关闭不能绕过正在执行的算法，也不允许在等待调用者时触碰网络或驱动。
 {
  Pcm pcm;PeerMedia media{{&pcm}};assert(media.EnsureCallDecoder());auto task=media.call_decode_task_;
  media.downlink_.push_back({{3}});media.PumpPlayback(8);media.call_codec_->wait_entered();std::atomic<bool> closed=false;
  std::thread close([&]{media.StopCallDecoder();closed=true;});std::this_thread::sleep_for(30ms);assert(!closed.load());
  media.call_codec_->release();close.join();join_task(task);assert(pcm.played.empty());
 }
 // 播放背压时两队列仍各最多四帧，解除后不丢弃剩余压缩帧。
 {
  Pcm pcm;pcm.pressured=true;PeerMedia media{{&pcm}};media.call_codec_->release();assert(media.EnsureCallDecoder());auto task=media.call_decode_task_;
  for(int i=0;i<20;++i)media.downlink_.push_back({{uint8_t(i+1)}});
  for(int i=0;i<30;++i){media.PumpPlayback(8);std::this_thread::sleep_for(2ms);std::lock_guard<std::mutex> l(media.call_decode_mutex_);assert(media.call_decode_waiting_.size()<=4&&media.call_decode_ready_.size()<=4);}
  assert(pcm.played.empty()&&!media.downlink_.empty());pcm.pressured=false;
  assert(eventually([&]{media.PumpPlayback(8);return pcm.played.size()==20;}));
  for(int i=0;i<20;++i)assert(pcm.played[i]==i+1);
  media.StopCallDecoder();join_task(task);
 }
 // 已解码队列满时，上行连续处理不得把下行交付拖到整批编码之后。
 {
  Pcm pcm;PeerMedia media{{&pcm}};media.call_codec_->release();assert(media.EnsureCallDecoder());auto task=media.call_decode_task_;
  {std::lock_guard<std::mutex> lock(media.call_decode_mutex_);
   for(int i=0;i<4;++i){media.call_decode_ready_.push_back(std::vector<int16_t>(320,1));media.call_decode_waiting_.push_back({{2},0});}}
  for(int i=0;i<8;++i)media.downlink_.push_back({{3}});
  assert(eventually([&]{media.CaptureUDP();return pcm.played.size()>=8;}));
  assert(pcm.writer==std::this_thread::get_id());media.StopCallDecoder();join_task(task);
 }
 // 任务创建失败时回收刚建的屏障，后续关闭幂等。
 {
  Pcm pcm;PeerMedia media{{&pcm}};auto before=deleted_semaphores;fail_task=true;assert(!media.EnsureCallDecoder());fail_task=false;
  assert(deleted_semaphores==before+1&&!media.call_decode_task_&&!media.call_decode_stopped_);media.StopCallDecoder();
 }
}
'''
        compiler = shutil.which('g++') or shutil.which('clang++')
        output = ROOT.parents[1] / '.temp'
        output.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=output) as folder:
            binary = Path(folder) / 'call-decode'
            flags = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if os.environ.get('CALL_TEST_SANITIZE') else []
            result = subprocess.run([compiler, *flags, '-std=c++17', '-pthread', '-Wall', '-Wextra', '-x', 'c++', '-', '-o', str(binary)], input=harness, text=True, capture_output=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr[-4000:])
            result = subprocess.run([str(binary)], capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr.decode(errors='replace')[-2000:])


if __name__ == '__main__':
    unittest.main()
