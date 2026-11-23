"""验证 Watcher 的视频接收节拍与真实 UDP 音视频组帧互不干扰。"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
RUNTIME = ROOT.parent / 'device-runtime'


class WatcherUdpVideoGateTest(unittest.TestCase):
    """覆盖完整帧后限速、帧丢弃、音频透传和新会话重置。"""

    def test_real_packets_keep_audio_and_reassemble_after_gate(self):
        code = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <vector>
#include "ur/device/udp_av.hpp"
#include "signaling/watcher_udp_video_gate.h"
struct Cipher : ur::device::AesCtrPort {
 bool transform(const std::array<std::uint8_t,16>&, const std::uint8_t* in,
                std::uint8_t* out, std::size_t n) override {
  std::copy_n(in,n,out); return true;
 }
};
int main() {
 Cipher cipher; std::array<uint8_t,8> a{1}, b{2};
 ur::device::UdpAV tx(cipher,a,b), rx(cipher,b,a);
 WatcherUdpVideoGate gate;
 std::vector<uint8_t> expected(2400,42), audio{5,6,7}, opus, image, audio_packet;
 std::vector<std::vector<uint8_t>> first, skipped, split, resumed;
 assert(tx.jpeg(expected.data(),expected.size(),first));
 assert(tx.audio(audio,audio_packet));
 assert(tx.jpeg(expected.data(),expected.size(),skipped));
 assert(tx.jpeg(expected.data(),expected.size(),split));
 assert(tx.jpeg(expected.data(),expected.size(),resumed));
 // 首帧允许完整接收；只有完成并通过协议校验后才启动限速。
 for (const auto& packet:first) {
  assert(!gate.ShouldSkip(packet,100));
  assert(rx.receive(packet,100,opus,image));
 }
 assert(image==expected); gate.OnFrame(100);
 // 音频即使处在视频限速窗口也必须照常解密和交付。
 assert(!gate.ShouldSkip(audio_packet,120));
 assert(rx.receive(audio_packet,120,opus,image) && opus==audio);
 for (const auto& packet:skipped) assert(gate.ShouldSkip(packet,150));
 // 限速截止落在帧中间时，尾片可进入协议但不能组成错误画面。
 assert(split.size()>1 && gate.ShouldSkip(split.front(),265));
 for (size_t i=1; i<split.size(); ++i) {
  assert(!gate.ShouldSkip(split[i],266));
  assert(rx.receive(split[i],266,opus,image) && image.empty());
 }
 // 下一帧可重新组装；丢掉中间帧或只收到尾片不得破坏视频序号状态。
 for (const auto& packet:resumed) {
  assert(!gate.ShouldSkip(packet,266));
  assert(rx.receive(packet,266,opus,image));
 }
 assert(image==expected); gate.OnFrame(266);
 assert(gate.ShouldSkip(resumed[0],300));
 // 会话重置与时钟回退不能让首帧无限等待。
 gate.Reset(); assert(!gate.ShouldSkip(resumed[0],300));
 gate.OnFrame(300); assert(!gate.ShouldSkip(resumed[0],299));
 // 非视频和截断输入仍留给原协议校验，不由节拍器吞掉。
 assert(!gate.ShouldSkip(std::vector<uint8_t>{2,0},301));
 assert(!gate.ShouldSkip(audio_packet,301));
}
'''
        temp_root = ROOT.parents[1] / '.temp'
        temp_root.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=temp_root, prefix='watcher-video-gate-') as tmp:
            binary = str(Path(tmp) / 'test')
            built = subprocess.run(
                ['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                 '-I', str(ROOT / 'main'), '-I', str(RUNTIME / 'include'),
                 '-x', 'c++', '-', '-o', binary], input=code, text=True,
                capture_output=True, timeout=30)
            self.assertEqual(built.returncode, 0, built.stderr)
            run = subprocess.run([binary], text=True, capture_output=True, timeout=10)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def test_production_receive_limits_only_watcher_video(self):
        """执行生产收包分支，确认 Watcher 丢中间视频时音频继续且其他板型不受影响。"""
        source = (ROOT / 'main/signaling/ur_signaling_call.cc').read_text()
        start = source.index('    void ReceiveUDP(int limit) {')
        end = source.index('            if (!audio.empty()) {', start)
        mark_start = source.index('#if CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER', end)
        mark_end = source.index('            if (!image.empty() &&', mark_start)
        actual = (source[start:end] + '            if (!audio.empty()) audios.push_back(audio);\n'
                  + source[mark_start:mark_end]
                  + '            if (!image.empty()) images.push_back(image);\n        }\n#endif\n    }\n')
        code = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>
#include "ur/device/udp_av.hpp"
#include "ur/device/datagram_port.hpp"
#include "signaling/watcher_udp_video_gate.h"
#define CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO 1
static int64_t clock_us = 0;
static int64_t esp_timer_get_time() { return clock_us; }
struct Cipher : ur::device::AesCtrPort {
 bool transform(const std::array<std::uint8_t,16>&, const std::uint8_t* in,
                std::uint8_t* out, std::size_t n) override {
  std::copy_n(in,n,out); return true;
 }
};
struct Port {
 std::deque<std::vector<uint8_t>> packets;
 ur::device::ReceiveResult receive(std::vector<uint8_t>& out) {
  if (packets.empty()) return ur::device::ReceiveResult::Empty;
  out=std::move(packets.front()); packets.pop_front(); return ur::device::ReceiveResult::Received;
 }
};
struct JpegStub { bool waiting=false; bool WaitingForDma() const { return waiting; } };
struct Peer {
 std::mutex udp_port_mutex_,udp_av_mutex_;
 Port udp_port_; std::unique_ptr<ur::device::UdpAV> udp_channel_;
 std::vector<uint8_t> udp_receive_packet_;
 std::vector<std::vector<uint8_t>> images,audios;
 WatcherUdpVideoGate watcher_video_gate_;
 std::unique_ptr<JpegStub> jpeg_output_;
 uint64_t udp_receive_calls_=0,udp_receive_us_=0,udp_receive_max_us_=0,
          udp_empty_calls_=0,udp_empty_us_=0,udp_decrypt_calls_=0,udp_decrypt_us_=0;
 ACTUAL
};
int main() {
 Cipher c; std::array<uint8_t,8> a{1},b{2}; ur::device::UdpAV tx(c,a,b);
 std::vector<uint8_t> frame(2400,42), audio{5,6,7}, packet, packet2;
 std::vector<std::vector<uint8_t>> first, middle, last, during_wait, after_wait;
 assert(tx.jpeg(frame.data(),frame.size(),first));
 assert(tx.jpeg(frame.data(),frame.size(),middle));
 assert(tx.audio(audio,packet));
 assert(tx.jpeg(frame.data(),frame.size(),last));
 assert(tx.jpeg(frame.data(),frame.size(),during_wait));
 assert(tx.audio(audio,packet2));
 assert(tx.jpeg(frame.data(),frame.size(),after_wait));
 Peer p;p.udp_channel_=std::make_unique<ur::device::UdpAV>(c,b,a);
 p.jpeg_output_=std::make_unique<JpegStub>();
 clock_us=100000;for(auto& x:first)p.udp_port_.packets.push_back(x);p.ReceiveUDP(2);
 clock_us=150000;for(auto& x:middle)p.udp_port_.packets.push_back(x);
 p.udp_port_.packets.push_back(packet);p.ReceiveUDP(3);
 clock_us=266000;for(auto& x:last)p.udp_port_.packets.push_back(x);p.ReceiveUDP(2);
 assert(p.audios.size()==1 && p.audios[0]==audio);
#if CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
 assert(p.images.size()==2);
#else
 assert(p.images.size()==3);
#endif
 for(const auto& image:p.images)assert(image==frame);
 // 只跳过等待期间的视频，音频仍须按原路径处理；资源恢复后继续组帧。
 p.jpeg_output_->waiting=true;clock_us=500000;
 for(auto& x:during_wait)p.udp_port_.packets.push_back(x);
 p.udp_port_.packets.push_back(packet2);p.ReceiveUDP(3);
 p.jpeg_output_->waiting=false;clock_us=700000;
 for(auto& x:after_wait)p.udp_port_.packets.push_back(x);p.ReceiveUDP(2);
 assert(p.audios.size()==2);
#if CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
 assert(p.images.size()==3);
#else
 assert(p.images.size()==5);
#endif
}
'''.replace('ACTUAL', actual)
        temp_root = ROOT.parents[1] / '.temp'
        temp_root.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=temp_root, prefix='watcher-video-receive-') as tmp:
            for watcher in (0, 1):
                binary = str(Path(tmp) / f'test-{watcher}')
                built = subprocess.run(
                    ['g++', '-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror',
                     f'-DCONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER={watcher}',
                     '-I', str(ROOT / 'main'), '-I', str(RUNTIME / 'include'),
                     '-x', 'c++', '-', '-o', binary], input=code, text=True,
                    capture_output=True, timeout=30)
                self.assertEqual(built.returncode, 0, built.stderr)
                run = subprocess.run([binary], text=True, capture_output=True, timeout=10)
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
