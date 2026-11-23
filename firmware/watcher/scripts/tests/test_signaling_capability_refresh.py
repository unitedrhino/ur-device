"""回放真实能力续报调度，防止在线一小时后能力缓存过期而无法再次来电。"""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
from .test_call_audio_decode import method

ROOT = Path(__file__).resolve().parents[2]


class SignalingCapabilityRefreshTest(unittest.TestCase):
    """只替代时钟和发布端口，执行实际调度并检查空闲、活动两处入口。"""

    def test_current_connection_refresh_and_bounded_retry(self):
        source = (ROOT / 'main/signaling/ur_signaling_protocol.cc').read_text()
        signature = 'void SignalingProtocol::MaintainCapability(std::uint64_t now)'
        if signature in source:
            actual = method(source, signature)
        else:
            # 修复前直接执行旧主循环的一次性上报段，必须在第二次续期断言失败。
            start = source.index('            if (idle_down_subscribed_.load() && !idle_capability_reported_.load()')
            end = source.index('            /* 无 AI 运行时', start)
            actual = signature + ' {\n#if CONFIG_UR_AI_SIGNALING_WEBRTC\n' + source[start:end] + '\n#else\n(void)now;\n#endif\n}'
        constant = re.search(r'constexpr std::uint64_t kCapabilityRefreshMs = [^;]+;', source)
        cadence = constant.group(0) if constant else 'constexpr std::uint64_t kCapabilityRefreshMs = 30 * 60 * 1000;'
        code = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
CADENCE
std::uint64_t clock_now=0;
std::uint64_t MonotonicMs(){return clock_now;}
struct SignalingProtocol {
 std::atomic<bool> idle_down_subscribed_{false},idle_capability_reported_{false};
 std::uint64_t idle_capability_retry_at_=0;
 unsigned calls=0;bool accepted=true;
 bool ReportIdleCapability(){++calls;return accepted;}
 void MaintainCapability(std::uint64_t now);
};
ACTUAL
int main(){
 static_assert(kCapabilityRefreshMs==1800000,"refresh must precede one-hour cache expiry");
 SignalingProtocol p;
 auto step=[&](std::uint64_t now){clock_now=now;p.MaintainCapability(now);};
 step(0);assert(p.calls==0);p.idle_down_subscribed_=true;step(0);
#if CONFIG_UR_AI_SIGNALING_WEBRTC
 assert(p.calls==1);step(1799999);assert(p.calls==1);
 step(1800000);assert(p.calls==2&&"online capability expired without periodic report");
 step(3600000);assert(p.calls==3);
 p.accepted=false;step(5400000);assert(p.calls==4);
 step(5404999);assert(p.calls==4);p.accepted=true;step(5405000);assert(p.calls==5);
 step(7204999);assert(p.calls==5);step(7205000);assert(p.calls==6);
 SignalingProtocol failed;failed.idle_down_subscribed_=true;failed.accepted=false;
 clock_now=0;failed.MaintainCapability(0);assert(failed.calls==1);
 clock_now=4999;failed.MaintainCapability(4999);assert(failed.calls==1);
 clock_now=5000;failed.accepted=true;failed.MaintainCapability(5000);assert(failed.calls==2);
#else
 assert(p.calls==0);
#endif
}
'''.replace('CADENCE', cadence).replace('ACTUAL', actual)
        for enabled in (0, 1):
            with self.subTest(enabled=enabled), tempfile.TemporaryDirectory(prefix='watcher-cap-refresh-') as directory:
                binary = str(Path(directory) / 'refresh')
                built = subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                        '-DCONFIG_UR_AI_SIGNALING_WEBRTC=' + str(enabled),
                                        '-x', 'c++', '-', '-o', binary], input=code,
                                       text=True, capture_output=True, timeout=30)
                self.assertEqual(built.returncode, 0, built.stderr)
                result = subprocess.run([binary], text=True, capture_output=True, timeout=5)
                self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(source.count('MaintainCapability('), 3)
