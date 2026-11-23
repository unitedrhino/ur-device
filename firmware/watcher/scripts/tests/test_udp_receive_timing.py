"""回放生产收包循环，验证数据报按实际领取时间计算重组期限。"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
RUNTIME = ROOT.parent / 'device-runtime'


class UdpReceiveTimingTest(unittest.TestCase):
    """使用生产 UdpAV 与可控网络时钟覆盖批次延迟和超时边界。"""

    def test_each_datagram_uses_its_own_receipt_time(self):
        source = (ROOT / 'main/signaling/ur_signaling_call.cc').read_text()
        start = source.index('    void ReceiveUDP(int limit) {')
        end = source.index('            if (!audio.empty()) {', start)
        # 保留真实收包及重组代码；不模拟设备播放或渲染。
        actual = source[start:end] + '            if (!image.empty()) output = image;\n        }\n#endif\n    }\n'
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
#define CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO 1
static int64_t clock_us = 0;
static int64_t esp_timer_get_time() { return clock_us; }
struct Cipher : ur::device::AesCtrPort {
 bool transform(const std::array<std::uint8_t,16>&,const std::uint8_t* in,std::uint8_t* out,std::size_t n) override { std::copy_n(in,n,out);return true; }
};
struct Port {
 std::deque<std::pair<std::vector<uint8_t>,int64_t>> packets;
 ur::device::ReceiveResult receive(std::vector<uint8_t>& out) {
  if(packets.empty())return ur::device::ReceiveResult::Empty;
  auto entry=std::move(packets.front());packets.pop_front();clock_us+=entry.second;out=std::move(entry.first);return ur::device::ReceiveResult::Received;
 }
};
struct Peer {
 std::mutex udp_port_mutex_,udp_av_mutex_;
 Port udp_port_;std::unique_ptr<ur::device::UdpAV> udp_channel_;
 std::vector<uint8_t> udp_receive_packet_,output;
 uint64_t udp_receive_calls_=0,udp_receive_us_=0,udp_receive_max_us_=0,udp_empty_calls_=0,udp_empty_us_=0,udp_decrypt_calls_=0,udp_decrypt_us_=0;
 ACTUAL
};
int main(){
 Cipher c;std::array<uint8_t,8> a{1},b{2};ur::device::UdpAV tx(c,a,b);std::vector<uint8_t> frame(2400,42);std::vector<std::vector<uint8_t>> packets;assert(tx.jpeg(frame.data(),frame.size(),packets)&&packets.size()==2);
 // 首片在批次开始后延迟 100ms 领取，尾片实际间隔 150ms，应成功组帧。
 {Peer p;p.udp_channel_=std::make_unique<ur::device::UdpAV>(c,b,a);clock_us=0;p.udp_port_.packets.push_back({packets[0],100000});p.ReceiveUDP(1);clock_us=250000;p.udp_port_.packets.push_back({packets[1],0});p.ReceiveUDP(1);assert(p.output==frame && "first fragment timestamp predates actual receipt");}
 // 同一批次中尾片延迟至 201ms，不能因复用批次时间而接收过期帧。
 {Peer p;p.udp_channel_=std::make_unique<ur::device::UdpAV>(c,b,a);clock_us=0;p.udp_port_.packets.push_back({packets[0],0});p.udp_port_.packets.push_back({packets[1],201000});p.ReceiveUDP(2);assert(p.output.empty() && "expired frame accepted using stale batch clock");}
}
'''.replace('ACTUAL', actual)
        temp_root = ROOT.parents[1] / '.temp'
        temp_root.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=temp_root, prefix='udp-receipt-time-') as tmp:
            binary = str(Path(tmp) / 'test')
            build = subprocess.run(['g++', '-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror',
                                    '-I', str(RUNTIME / 'include'), '-x', 'c++', '-', '-o', binary],
                                   input=code, text=True, capture_output=True, timeout=30)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([binary], text=True, capture_output=True, timeout=10)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == '__main__':
    unittest.main()
