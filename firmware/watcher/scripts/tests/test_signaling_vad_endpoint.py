"""真实信令句尾方法与可移植VAD计时联编，验证单次静音边沿之后仍会提交本轮。"""
from pathlib import Path
import subprocess
import tempfile
import unittest
from .test_call_audio_decode import method

# ROOT定位生产源码，回归直接抽取实际方法而非复制实现。
ROOT = Path(__file__).resolve().parents[2]


class SignalingVadEndpointTest(unittest.TestCase):
    def test_periodic_deadline_and_cancel(self):
        """覆盖700ms边界、背压重试、停麦去重、全双工与本人取消。"""
        source = (ROOT / 'main/signaling/ur_signaling_protocol.cc').read_text()
        self.assertIn('CheckVadEndpoint();', method(source, 'void SignalingProtocol::TaskLoop('))
        actual = '\n'.join(method(source, signature) for signature in (
            'void SignalingProtocol::OnLocalVadState(',
            'void SignalingProtocol::CheckVadEndpoint(',
            'void SignalingProtocol::SendStopListening(',
        ))
        code = r'''
#include "ur_ai_runtime.h"
#include <atomic>
#include <cassert>
#include <cstdint>
#include <mutex>
#define ESP_LOGI(...) ((void)0)
std::int64_t now_us = 0;
std::int64_t esp_timer_get_time() {
    return now_us;
}
namespace ur::device {
enum class Result { Applied, Busy };
}
struct Runtime {
    bool active = true, accept = true;
    int attempts = 0;
    Runtime &voice() {
        return *this;
    }
    bool capturing() {
        return active;
    }
    ur::device::Result stop_input(std::uint64_t) {
        ++attempts;
        if (!accept)
            return ur::device::Result::Busy;
        active = false;
        return ur::device::Result::Applied;
    }
};
struct SignalingProtocol {
    enum class Command { kStopInput };
    std::atomic<bool> full_duplex_{false}, session_active_{true}, awaiting_reply_{false};
    std::atomic<std::uint64_t> awaiting_reply_at_ms_{0};
    std::mutex vad_mutex_;
    UrAiVadEndpoint vad_endpoint_;
    bool vad_auto_stop_ = true;
    Runtime storage;
    Runtime *runtime_ = &storage;
    int posted = 0;
    std::uint64_t MonotonicMs() {
        return now_us / 1000;
    }
    bool PostCommand(Command) {
        ++posted;
        return true;
    }
    void OnLocalVadState(bool);
    void CheckVadEndpoint();
    void SendStopListening();
};
PRODUCTION
void speech(SignalingProtocol &p) {
    p.vad_endpoint_.Begin(0);
    now_us = 1000000;
    p.OnLocalVadState(true);
    now_us = 1400000;
    p.OnLocalVadState(false);
}
int main() {
    SignalingProtocol p;
    speech(p);
    now_us = 2099999;
    p.CheckVadEndpoint();
    assert(p.storage.attempts == 0);
    now_us = 2100000;
    p.CheckVadEndpoint();
    assert(p.storage.attempts == 1 && p.awaiting_reply_);
    assert(p.awaiting_reply_at_ms_ == 2100);
    now_us = 3000000;
    p.CheckVadEndpoint();
    assert(p.storage.attempts == 1);
    SignalingProtocol busy;
    speech(busy);
    busy.storage.accept = false;
    now_us = 2200000;
    busy.CheckVadEndpoint();
    assert(busy.storage.attempts == 1 && !busy.awaiting_reply_);
    busy.storage.accept = true;
    now_us = 2220000;
    busy.CheckVadEndpoint();
    assert(busy.awaiting_reply_);
    SignalingProtocol full;
    speech(full);
    full.full_duplex_ = true;
    now_us = 2300000;
    full.CheckVadEndpoint();
    assert(full.storage.attempts == 0);
    SignalingProtocol manual;
    speech(manual);
    manual.vad_auto_stop_ = false;
    now_us = 2300000;
    manual.CheckVadEndpoint();
    assert(manual.storage.attempts == 0);
    SignalingProtocol ended;
    speech(ended);
    ended.session_active_ = false;
    now_us = 2400000;
    ended.CheckVadEndpoint();
    assert(ended.storage.attempts == 0);
    SignalingProtocol stopped;
    speech(stopped);
    stopped.storage.active = false;
    now_us = 2500000;
    stopped.CheckVadEndpoint();
    assert(stopped.storage.attempts == 0);
    SignalingProtocol cancelled;
    speech(cancelled);
    now_us = 1500000;
    cancelled.SendStopListening();
    now_us = 4000000;
    cancelled.CheckVadEndpoint();
    assert(cancelled.posted == 1 && cancelled.storage.attempts == 0);
}
'''.replace('PRODUCTION', actual)
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / 'vad-endpoint')
            result = subprocess.run([
                'c++', '-x', 'c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-I', str(ROOT / 'main/protocols'), '-',
                str(ROOT / 'main/protocols/ur_ai_runtime.cc'), '-o', binary,
            ], input=code, text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([binary], check=True)
