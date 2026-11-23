"""验证来电占用时 AI 建连立即返回，不误触发 MQTT 重连。"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class CallBusyOpenTest(unittest.TestCase):
    """编译生产建连、等待和忙线分支，只替换队列和事件组边界。"""

    def test_busy_releases_waiter_without_reconnect(self):
        source = (ROOT / 'main/signaling/ur_signaling_protocol.cc').read_text()
        start = source.index('bool SignalingProtocol::WaitForEstablished(')
        end = source.index('bool SignalingProtocol::OpenVisionChannel()', start)
        busy_start = source.index('        case Command::kCreateSession:')
        busy_end = source.index('            if (!EnsureRuntimeCreated())', busy_start)
        code = r'''
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <functional>
#define CONFIG_UR_AI_SIGNALING_WEBRTC 1
#define CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO 0
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
using EventBits_t = unsigned;
using TickType_t = unsigned;
constexpr unsigned pdTRUE=1, pdFALSE=0, kEstablishedTimeout=12000;
constexpr unsigned kSessionCreateStartedEvent=8;
// 事件组只响应本次等待掩码，旧失败通知不能替代建连命令接管屏障。
struct Events { unsigned bits=0; bool waited=false; unsigned waits=0;
    std::function<void()> dispatch; };
unsigned ticks=0;
unsigned xTaskGetTickCount() { return ticks; }
void xEventGroupSetBits(Events* e, unsigned b) { e->bits |= b; }
void xEventGroupClearBits(Events* e, unsigned b) { e->bits &= ~b; }
unsigned xEventGroupWaitBits(Events* e,unsigned mask,unsigned,unsigned,unsigned) {
    ++e->waits; ++ticks;
    if ((mask & kSessionCreateStartedEvent) && !(e->bits & mask) && e->dispatch)
        e->dispatch();
    e->waited = (e->bits & mask) == 0;
    auto result=e->bits & mask; e->bits &= ~mask; return result;
}
namespace ur { namespace device { enum class VoiceState {Idle, Reporting, Creating, Established}; } }
struct Voice { ur::device::VoiceState state() { return ur::device::VoiceState::Idle; } };
struct Runtime { Voice value; Voice& voice() { return value; } };
struct UrSignalingCall { static UrSignalingCall& Instance() { static UrSignalingCall s; return s; }
    bool busy=false; bool Busy() const { return busy; } };
int reconnects=0;
void ur_device_force_mqtt_reconnect() { ++reconnects; }
struct SignalingProtocol {
    enum class Command { kCreateSession };
    static constexpr unsigned kSessionEstablishedEvent=1, kSessionFinishedEvent=2;
    static constexpr unsigned kSessionBusyEvent=4;
    Events event; Events* events_=&event;
    Runtime* runtime_=nullptr; std::atomic<bool> runtime_started_{false},session_active_{false};
    bool await_session_create_=false;
    std::chrono::steady_clock::time_point last_incoming_time_;
    bool error_occurred_=false, inject_old_ready=false, inject_finished=false, succeed=false;
    bool deferred=false, queue_full=false, current_finished=false;
    void (*on_audio_channel_opened_)()=nullptr;
    bool EnsureTaskRunning() { return true; }
    void SetError(const char*) { error_occurred_=true; }
    void DispatchCommand(Command c) {
        switch (c) {
// BUSY_HANDLER
            break;
        }
        if (succeed && !UrSignalingCall::Instance().Busy())
            xEventGroupSetBits(events_, kSessionEstablishedEvent);
        if (current_finished) xEventGroupSetBits(events_, kSessionFinishedEvent);
    }
    bool PostCommand(Command c) {
        if (queue_full) return false;
        if (inject_old_ready) xEventGroupSetBits(events_, kSessionEstablishedEvent);
        if (inject_finished) xEventGroupSetBits(events_, kSessionFinishedEvent);
        if (deferred) event.dispatch = [this,c] { DispatchCommand(c); };
        else DispatchCommand(c);
        return true;
    }
    // WAIT_DECL
    bool OpenAudioChannel();
};
// PRODUCTION_METHODS
int main() {
    auto& call=UrSignalingCall::Instance();
    call.busy=true;
    SignalingProtocol blocked;
    assert(!blocked.OpenAudioChannel());
    assert(!blocked.event.waited);
    assert(reconnects==0);
    assert(!blocked.error_occurred_);
    SignalingProtocol stale_ready;
    stale_ready.inject_old_ready=true;
    assert(!stale_ready.OpenAudioChannel());
    assert(!stale_ready.event.waited && reconnects==0);
    call.busy=false;
    SignalingProtocol finished;
    finished.current_finished=true;
    assert(!finished.OpenAudioChannel());
    assert(!finished.event.waited && reconnects==0);
    SignalingProtocol ended_and_ready;
    ended_and_ready.inject_old_ready=true;
    ended_and_ready.current_finished=true;
    assert(!ended_and_ready.OpenAudioChannel());
    assert(!ended_and_ready.event.waited && reconnects==0);
    blocked.succeed=true;
    assert(blocked.OpenAudioChannel());
    assert(blocked.session_active_ && reconnects==0);
    SignalingProtocol stale_failure;
    stale_failure.deferred=true;
    stale_failure.inject_finished=true;
    stale_failure.succeed=true;
    assert(stale_failure.OpenAudioChannel());
    assert(stale_failure.session_active_ && stale_failure.event.waits==2 && reconnects==0);
    SignalingProtocol stale_success;
    stale_success.deferred=true;
    stale_success.inject_old_ready=true;
    stale_success.current_finished=true;
    assert(!stale_success.OpenAudioChannel());
    assert(!stale_success.session_active_ && stale_success.event.waits==2 && reconnects==0);
    SignalingProtocol congested;
    congested.queue_full=true;
    assert(!congested.OpenAudioChannel());
    assert(congested.error_occurred_ && congested.event.waits==0 && reconnects==0);
    SignalingProtocol timeout;
    assert(!timeout.OpenAudioChannel());
    assert(timeout.event.waited);
    assert(reconnects==1);
    assert(timeout.error_occurred_);
}
'''
        wait_decl = source[start:source.index('{', start)].replace(
            'SignalingProtocol::', '').strip() + ';'
        code = code.replace('// WAIT_DECL', wait_decl)
        code = code.replace('// BUSY_HANDLER', source[busy_start:busy_end])
        code = code.replace('// PRODUCTION_METHODS', source[start:end])
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'busy_test'
            subprocess.run(['c++', '-x', 'c++', '-std=c++17', '-Wall', '-Wextra',
                            '-Werror', '-', '-o', str(binary)], input=code, text=True, check=True)
            subprocess.run([str(binary)], check=True)
            # UDP 构建不编译人际占用分支，原有建连与超时行为仍须可用。
            udp_code = code.replace('#define CONFIG_UR_AI_SIGNALING_WEBRTC 1',
                                    '#define CONFIG_UR_AI_SIGNALING_WEBRTC 0')
            udp_code = udp_code[:udp_code.index('int main() {')] + r"""
int main() {
    SignalingProtocol opened;
    opened.succeed=true;
    assert(opened.OpenAudioChannel());
    SignalingProtocol timeout;
    assert(!timeout.OpenAudioChannel());
    assert(timeout.event.waited && reconnects==1);
}
"""
            subprocess.run(['c++', '-x', 'c++', '-std=c++17', '-Wall', '-Wextra',
                            '-Werror', '-', '-o', str(binary)], input=udp_code, text=True,
                           check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()
