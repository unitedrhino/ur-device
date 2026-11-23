"""回放真实PCM采集桥，检查突发/暂停积压不丢帧、不滞留尾帧且组帧有界。"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
RUNTIME = ROOT.parent / 'device-runtime'


def method(source, signature):
    """提取生产方法，队列与组帧逻辑不在夹具中重写。"""
    start = source.index(signature)
    return source[start:source.index('\n}\n', start) + 3]


class SignalingPcmCaptureTest(unittest.TestCase):
    """替换硬件音频源，使用真实BufferedPcm及适配方法验证样本顺序。"""

    def test_bursts_paused_tail_and_bounded_staging(self):
        source = (ROOT / 'main/signaling/ur_signaling_pcm.cc').read_text()
        methods = '\n'.join(method(source, signature) for signature in [
            'bool UrSignalingPcm::begin_capture(', 'void UrSignalingPcm::end_capture(',
            'void UrSignalingPcm::DrainPendingCapture(',
            'void UrSignalingPcm::DrainCaptureWhilePaused(',
            'PcmResult UrSignalingPcm::read_frame(',
        ])
        code = r'''
#include <cassert>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <thread>
#include <cstdint>
#include <deque>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>
#include "ur/device/buffered_pcm.hpp"
// AudioSource只替代硬件生产者，沿用其48块丢旧容量；样本数不计为播放成功。
struct AudioSource {
    std::deque<std::vector<int16_t>> chunks;
    bool active = false;
    std::function<void()> on_pop;
    void EnableSignalingCapture(bool enabled) { active=enabled; if(!active) chunks.clear(); }
    void Push(std::vector<int16_t> data) {
        if(chunks.size()==48) chunks.pop_front();
        chunks.push_back(std::move(data));
    }
    bool PopSignalingPcm(std::vector<int16_t>& out) {
        if(chunks.empty()) return false;
        if(on_pop) on_pop();
        out=std::move(chunks.front()); chunks.pop_front(); return true;
    }
};
namespace ur::device {
struct UrSignalingPcm {
    AudioSource& audio_;
    std::mutex capture_mutex_;
    BufferedPcm buffered_;
    std::string input_id_;
    std::deque<int16_t> capture_buffer_;
    UrSignalingPcm(AudioSource& audio, AudioFormat format):audio_(audio),buffered_(format) {}
    bool begin_capture(std::string_view);
    void end_capture();
    void DrainPendingCapture();
    void DrainCaptureWhilePaused();
    PcmResult read_frame(std::vector<int16_t>&);
};
METHODS
}
using namespace ur::device;
// Sequence按样本编号核对，避免仅数包掩盖20ms帧丢失或重放。
std::vector<int16_t> Sequence(int begin, int samples) {
    std::vector<int16_t> data(samples);
    for(int i=0;i<samples;++i) data[i]=static_cast<int16_t>((begin+i)%30000);
    return data;
}
int main() {
    for(int frame_ms: {20,60}) {
        const int samples=16*frame_ms;
        for(bool paused: {false,true}) {
            AudioSource audio;
            UrSignalingPcm bridge(audio,{16000,1,frame_ms});
            assert(bridge.begin_capture("mic-1"));
            // 积压跨越16帧容量但不超过暂停期2秒上限；无新块时尾帧仍必须可读。
            const int frames=frame_ms==60 ? 32 : 40;
            for(int offset=0;offset<frames*samples;offset+=2*samples)
                audio.Push(Sequence(offset,2*samples));
            if(paused) bridge.DrainCaptureWhilePaused();
            std::vector<int16_t> pcm;
            for(int frame=0;frame<frames;++frame) {
                assert(bridge.read_frame(pcm)==PcmResult::Frame && "buffered tail stranded");
                assert(pcm==Sequence(frame*samples,samples) && "capture frame lost or reordered");
            }
            assert(bridge.read_frame(pcm)==PcmResult::Empty);
            bridge.end_capture();
            assert(bridge.begin_capture("mic-2"));
            audio.Push(Sequence(123,samples));
            assert(bridge.read_frame(pcm)==PcmResult::Frame && pcm==Sequence(123,samples));
        }
    }
    AudioSource audio;
    UrSignalingPcm bridge(audio,{16000,1,20});
    assert(bridge.begin_capture("mic"));
    std::vector<int16_t> pcm;
    // 模拟生产快于消费，组帧临时区不能随时长增长；老块由硬件队列原策略处置。
    for(int tick=0;tick<200;++tick) {
        audio.Push(Sequence(tick*960,960));
        assert(bridge.read_frame(pcm)==PcmResult::Frame);
        assert(bridge.capture_buffer_.size()<960 && "capture staging grows without bound");
    }
    bridge.end_capture();
    assert(bridge.read_frame(pcm)==PcmResult::Empty);
    assert(bridge.begin_capture("partial"));
    audio.Push(Sequence(0,319));
    assert(bridge.read_frame(pcm)==PcmResult::Empty);
    audio.Push(Sequence(319,1));
    assert(bridge.read_frame(pcm)==PcmResult::Frame && pcm==Sequence(0,320));
    // 不规则AFE块必须逐样本拼接，不能重复/丢弃跨块边界。
    int offset=0;
    for(int size: {7,333,1,512,107}) {
        audio.Push(Sequence(offset,size)); offset+=size;
    }
    for(int frame=0;frame<3;++frame)
        assert(bridge.read_frame(pcm)==PcmResult::Frame && pcm==Sequence(frame*320,320));
    assert(bridge.read_frame(pcm)==PcmResult::Empty);
    // 关闭须等待正在组帧的独立消费者，返回后不能再消费旧段或写入已清理的缓冲。
    bridge.end_capture();assert(bridge.begin_capture("closing"));audio.Push(Sequence(77,320));
    std::mutex gate;std::condition_variable cv;bool entered=false,released=false;
    std::atomic<bool> closed=false;
    audio.on_pop=[&]{std::unique_lock<std::mutex>lock(gate);entered=true;cv.notify_all();cv.wait(lock,[&]{return released;});};
    std::thread reader([&]{std::vector<int16_t> frame;assert(bridge.read_frame(frame)==PcmResult::Frame);assert(frame==Sequence(77,320));});
    {std::unique_lock<std::mutex>lock(gate);assert(cv.wait_for(lock,std::chrono::seconds(1),[&]{return entered;}));}
    std::thread closer([&]{bridge.end_capture();closed=true;});std::this_thread::sleep_for(std::chrono::milliseconds(15));assert(!closed);
    {std::lock_guard<std::mutex>lock(gate);released=true;cv.notify_all();}
    reader.join();closer.join();audio.on_pop={};assert(closed);assert(bridge.read_frame(pcm)==PcmResult::Empty);
    assert(bridge.begin_capture("next"));audio.Push(Sequence(1000,320));assert(bridge.read_frame(pcm)==PcmResult::Frame&&pcm==Sequence(1000,320));
    std::cout << "capture samples ordered; paused tail drained; staging bounded\n";
}
'''.replace('METHODS', methods)
        with tempfile.TemporaryDirectory(prefix='watcher-pcm-capture-') as directory:
            binary = str(Path(directory) / 'capture')
            built = subprocess.run([
                'g++', '-std=c++17', '-pthread', '-Wall', '-Wextra', '-x', 'c++', '-',
                str(RUNTIME / 'core/buffered_pcm.cpp'), '-I', str(RUNTIME / 'include'),
                '-o', binary,
            ], input=code, text=True, capture_output=True, timeout=30)
            self.assertEqual(built.returncode, 0, built.stderr)
            result = subprocess.run([binary], text=True, capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
