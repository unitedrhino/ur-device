"""编译实际回复超时函数，复现无识别结果后被永久退回待机的问题。"""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

WATCHER_ROOT = Path(__file__).resolve().parents[2]


class NoResultRecoveryTest(unittest.TestCase):
    """仅替换时钟、硬件和消息边界，超时分支使用生产实现。"""

    def test_application_event_stays_on_current_listening_page(self):
        """编译真实事件消费体；等待文案不覆盖菜单、待机或晚到的新轮。"""
        source = (WATCHER_ROOT / 'main/application.cc').read_text()
        begin = source.index('if (GetDeviceState() != kDeviceStateListening || protocol_ == nullptr ||')
        end = source.index('\n                });', begin)
        fixture = r'''
// 替换显示、协议和调度边界；菜单/监听/轮次判断直接编译生产代码。
#include <cassert>
#include <atomic>
#include <memory>
#include <string>
constexpr int kDeviceStateListening = 5;
namespace Lang { namespace Strings { constexpr const char* LISTENING = "聆听中"; } }
struct Display {
    std::string status;
    void SetStatus(const char* value) { status = value; }
};
struct UrAiProtocol {
    bool waiting = true, resume = true;
    int calls = 0;
    bool IsWaitingForResponse(const std::string& token) { return waiting && token == "turn-1"; }
    bool ResumeAfterNoResult(const std::string& token, int, bool = false) {
        if (resume && token == "turn-1") { ++calls; return true; }
        return false;
    }
};
struct Application {
    int state = kDeviceStateListening, listening_mode_ = 0;
    std::atomic<bool> interaction_blocked_{false};
    std::unique_ptr<UrAiProtocol> protocol_ = std::make_unique<UrAiProtocol>();
    int GetDeviceState() const { return state; }
    void Dispatch(Display* display, const std::string& stage, const std::string& audio_token) {
        // PRODUCTION_EVENT
    }
};
int main() {
    Application app; Display display;
    app.Dispatch(&display, "wait", "turn-1");
    assert(display.status == "等待回复");
    app.Dispatch(&display, "restart", "turn-1");
    assert(display.status == "聆听中" && app.protocol_->calls == 1);
    app.Dispatch(&display, "speech_restart", "turn-1");
    assert(app.protocol_->calls == 2);
    app.protocol_->calls = 1;
    display.status = "新轮";
    app.Dispatch(&display, "wait", "old-turn");
    app.Dispatch(&display, "restart", "old-turn");
    assert(display.status == "新轮" && app.protocol_->calls == 1);
    app.interaction_blocked_ = true;
    app.Dispatch(&display, "restart", "turn-1");
    assert(app.protocol_->calls == 1);
    app.interaction_blocked_ = false; app.state = 3;
    app.Dispatch(&display, "restart", "turn-1");
    assert(app.protocol_->calls == 1);
    app.protocol_.reset();
    app.Dispatch(&display, "restart", "turn-1");
}
'''
        compiler = shutil.which('g++') or shutil.which('clang++')
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / 'application_recovery_test')
            subprocess.run([compiler, '-x', 'c++', '-std=c++17', '-Wall', '-Wextra',
                            '-Werror', '-', '-o', binary],
                           input=fixture.replace('// PRODUCTION_EVENT', source[begin:end]),
                           text=True, capture_output=True, check=True)
            subprocess.run([binary], check=True, capture_output=True)

    def test_resume_rechecks_late_messages_disconnect_and_new_turn(self):
        """实际恢复入口拒绝晚到STT、旧轮提示、断线和已关闭会话。"""
        source = (WATCHER_ROOT / 'main/protocols/ur_ai_protocol.cc').read_text()
        begin = source.index('bool UrAiProtocol::IsWaitingForResponse(')
        end = source.index('void UrAiProtocol::OnLocalVadState', begin)
        fixture = r'''
// 硬件边界只记录取消和开启；轮次有效性检查来自生产源码。
#include <cassert>
#include <mutex>
#include <string>
enum ListeningMode { AutoStop };
constexpr int kAbortReasonNone = 0;
struct UrAiProtocol {
    mutable std::mutex state_mutex_;
    std::string expected_audio_token_ = "turn-1", session_id_ = "session-1";
    std::string stt_text_, current_resp_id_;
    bool mqtt_connected_ = true, audio_stop_sent_ = true, response_pending_ = false;
    bool upload_ready = false;
    int cancels = 0, starts = 0, speech_seeds = 0;
    void OnLocalVadState(bool speaking) { if (speaking) ++speech_seeds; }
    bool IsWaitingForResponse(const std::string&) const;
    bool ResumeAfterNoResult(const std::string&, ListeningMode, bool = false);
    void SendAbortSpeaking(int) { ++cancels; }
    void SendStartListening(ListeningMode) {
        ++starts; expected_audio_token_ = "turn-2"; audio_stop_sent_ = false;
        upload_ready = false;  // 新audioStarted确认之前禁止上传。
    }
};
// PRODUCTION_RECOVERY
int main() {
    UrAiProtocol recovered;
    assert(!recovered.IsWaitingForResponse("turn-1"));
    recovered.response_pending_ = true;
    assert(recovered.IsWaitingForResponse("turn-1"));
    assert(!recovered.IsWaitingForResponse("old-turn"));
    assert(!recovered.ResumeAfterNoResult("turn-1", AutoStop));
    recovered.response_pending_ = false;
    assert(recovered.ResumeAfterNoResult("turn-1", AutoStop));
    UrAiProtocol speech; speech.response_pending_ = true;
    assert(speech.ResumeAfterNoResult("turn-1", AutoStop, true));
    assert(speech.cancels == 1 && speech.starts == 1 && speech.speech_seeds == 1);
    assert(recovered.speech_seeds == 0);
    assert(recovered.cancels == 1 && recovered.starts == 1 && !recovered.upload_ready);
    assert(!recovered.ResumeAfterNoResult("turn-1", AutoStop));
    UrAiProtocol late;
    late.stt_text_ = "晚到识别";
    late.response_pending_ = true;
    assert(!late.ResumeAfterNoResult("turn-1", AutoStop, true));
    late.response_pending_ = false;
    assert(!late.ResumeAfterNoResult("turn-1", AutoStop));
    late.stt_text_.clear(); late.current_resp_id_ = "response-1";
    assert(!late.ResumeAfterNoResult("turn-1", AutoStop));
    late.current_resp_id_.clear(); late.mqtt_connected_ = false;
    assert(!late.ResumeAfterNoResult("turn-1", AutoStop));
    late.mqtt_connected_ = true; late.session_id_.clear();
    assert(!late.ResumeAfterNoResult("turn-1", AutoStop));
    late.session_id_ = "new-session"; late.expected_audio_token_ = "new-turn";
    assert(!late.ResumeAfterNoResult("turn-1", AutoStop));
    assert(late.cancels == 0 && late.starts == 0);
}
'''
        compiler = shutil.which('g++') or shutil.which('clang++')
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / 'resume_test')
            subprocess.run([compiler, '-x', 'c++', '-std=c++17', '-Wall', '-Wextra',
                            '-Werror', '-', '-o', binary],
                           input=fixture.replace('// PRODUCTION_RECOVERY', source[begin:end]),
                           text=True, capture_output=True, check=True)
            subprocess.run([binary], check=True, capture_output=True)

    def test_audio_start_reset_preserves_pending_and_vad_seed(self):
        """实际开麦及清理函数组合不能覆盖等待ACK状态，也不能吞掉恢复语音的上升沿。"""
        source = (WATCHER_ROOT / 'main/protocols/ur_ai_protocol.cc').read_text()
        start = source.index('void UrAiProtocol::SendStartListening(')
        start_method = source[start:source.index('void UrAiProtocol::SendStopListening', start)]
        reset = source.index('void UrAiProtocol::ResetTurnState()')
        reset_method = source[reset:source.index('std::string UrAiProtocol::NormalizeEmotion', reset)]
        vad = source.index('void UrAiProtocol::OnLocalVadState(')
        vad_method = source[vad:source.index('void UrAiProtocol::SendAbortSpeaking', vad)]
        fixture = r'''
// 只替换MQTT、时钟和RTOS边界，组合真实开麦、清理和VAD入口。
#include "ur_ai_runtime.h"
#include <cassert>
#include <mutex>
#include <string>
#define ESP_LOGI(...) ((void)0)
static int64_t now_us=1000000;
int64_t esp_timer_get_time() { return now_us; }
using ListeningMode=int;
constexpr int kAudioStartedEvent=1;
void xEventGroupClearBits(int,int) {}
namespace Lang { namespace Strings { constexpr int SERVER_ERROR=1; } }
struct UrAiProtocol {
    std::mutex state_mutex_;
    std::string expected_audio_token_, pending_audio_frames_, deferred_response_messages_;
    std::string pending_incoming_audio_frames_, deferred_audio_done_message_;
    std::string current_resp_id_="old-response", stt_text_="旧识别", assistant_text_="旧回复";
    bool audio_start_pending_=false, response_pending_=true, audio_upload_ready_=true;
    bool voice_seen_=false, vad_speaking_=false, audio_stop_sent_=true;
    bool response_audio_expected_=true, response_audio_announced_=true;
    int64_t turn_started_us_=0,audio_stop_us_=10,last_turn_activity_us_=20;
    int event_group_=0;
    UrAiVadEndpoint vad_endpoint_;
    UrAiTerminalTracker terminal_tracker_;
    void SendStartListening(ListeningMode);
    void ResetTurnState();
    void OnLocalVadState(bool);
    std::string MakeToken(const char*) { return "new-audio"; }
    bool PublishMethod(const char*,const std::string&,void*) { return true; }
    void SetError(int) { assert(false); }
    void EmitLegacyJson(const char*,const char*,const std::string&) { assert(false); }
};
// PRODUCTION_METHODS
int main() {
    UrAiProtocol p;
    p.SendStartListening(0);
    assert(p.audio_start_pending_ && !p.audio_upload_ready_ && !p.response_pending_);
    assert(!p.audio_stop_sent_ && p.current_resp_id_.empty() && p.stt_text_.empty());
    p.OnLocalVadState(true);  // 恢复时只有这一次上升沿，不能靠ACK后重复注入掩盖错误。
    assert(p.voice_seen_ && p.vad_speaking_);
    now_us=2800000; p.OnLocalVadState(false);
    now_us=3500000; assert(p.vad_endpoint_.ShouldStop(now_us));
}
'''
        compiler = shutil.which('g++') or shutil.which('clang++')
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / 'audio_start_vad_test')
            subprocess.run([compiler, '-x', 'c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-I' + str(WATCHER_ROOT / 'main/protocols'), '-',
                            str(WATCHER_ROOT / 'main/protocols/ur_ai_runtime.cc'), '-o', binary],
                           input=fixture.replace('// PRODUCTION_METHODS', start_method+reset_method+vad_method),
                           text=True, capture_output=True, check=True)
            subprocess.run([binary], check=True, capture_output=True)

    def test_new_speech_during_empty_wait_requests_recovery(self):
        """真实VAD入口在空识别等待中恢复；已识别、生成中和静音不能误取消。"""
        source = (WATCHER_ROOT / 'main/protocols/ur_ai_protocol.cc').read_text()
        begin = source.index('void UrAiProtocol::OnLocalVadState(')
        end = source.index('void UrAiProtocol::SendAbortSpeaking', begin)
        fixture = r'''
// 仅替换时钟与事件边界，直接编译生产VAD入口并检查事件不持锁投递。
#include "ur_ai_runtime.h"
#include <cassert>
#include <mutex>
#include <string>
#define ESP_LOGI(...) ((void)0)
int64_t esp_timer_get_time() { return 5000000; }
struct UrAiProtocol {
    std::mutex state_mutex_;
    bool audio_start_pending_ = false, audio_upload_ready_ = false;
    bool audio_stop_sent_ = true, response_pending_ = true;
    bool vad_speaking_ = false, voice_seen_ = false;
    std::string stt_text_, current_resp_id_, expected_audio_token_ = "turn-1";
    int64_t turn_started_us_ = 1000000;
    UrAiVadEndpoint vad_endpoint_;
    int restarts = 0;
    void OnLocalVadState(bool);
    void EmitLegacyJson(const char* type, const char* state, const std::string& token) {
        assert(state_mutex_.try_lock()); state_mutex_.unlock();
        assert(std::string(type) == "listen" && std::string(state) == "speech_restart");
        assert(token == "turn-1"); ++restarts;
    }
};
// PRODUCTION_VAD
int main() {
    UrAiProtocol waiting;
    waiting.OnLocalVadState(false); assert(waiting.restarts == 0);
    waiting.OnLocalVadState(true); assert(waiting.restarts == 1);
    waiting.OnLocalVadState(true); assert(waiting.restarts == 1);
    UrAiProtocol recognized; recognized.stt_text_ = "向前走";
    recognized.OnLocalVadState(true); assert(recognized.restarts == 0);
    UrAiProtocol created; created.current_resp_id_ = "response-1";
    created.OnLocalVadState(true); assert(created.restarts == 0);
    UrAiProtocol cancelled; cancelled.response_pending_ = false;
    cancelled.OnLocalVadState(true); assert(cancelled.restarts == 0);
    UrAiProtocol capture; capture.audio_stop_sent_ = false; capture.audio_upload_ready_ = true;
    capture.vad_endpoint_.Begin(1000000);
    capture.OnLocalVadState(true); assert(capture.restarts == 0 && capture.voice_seen_);
}
'''
        compiler = shutil.which('g++') or shutil.which('clang++')
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / 'vad_empty_recovery_test')
            subprocess.run([compiler, '-x', 'c++', '-std=c++17', '-Wall', '-Wextra',
                            '-Werror', '-I' + str(WATCHER_ROOT / 'main/protocols'),
                            '-', str(WATCHER_ROOT / 'main/protocols/ur_ai_runtime.cc'),
                            '-o', binary], input=fixture.replace('// PRODUCTION_VAD', source[begin:end]),
                           text=True, capture_output=True, check=True)
            subprocess.run([binary], check=True, capture_output=True)

    def test_no_result_recovers_but_real_reply_keeps_error_timeout(self):
        """无STT恢复监听；已有识别、图片输入及audioStart超时仍报错。"""
        source = (WATCHER_ROOT / 'main/protocols/ur_ai_protocol.cc').read_text()
        begin = source.index('void UrAiProtocol::CheckTurnTimeouts()')
        end = source.index('void UrAiProtocol::HandleIncomingPayload', begin)
        fixture = r'''
// 测试时钟固定在回复超时边界；不运行真实RTOS或网络。
#include "ur_ai_runtime.h"
#include <cassert>
#include <mutex>
#include <string>
#include <cstdint>
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
static int64_t now_us = 61000000;
int64_t esp_timer_get_time() { return now_us; }
constexpr int64_t kAudioStartTimeoutUs = 10000000;
constexpr int64_t kResponseTimeoutUs = 60000000;
namespace Lang { namespace Strings { constexpr int SERVER_TIMEOUT = 1; } }
struct UrAiProtocol {
    std::mutex state_mutex_;
    bool audio_upload_ready_ = false, audio_stop_sent_ = true;
    bool audio_start_pending_ = false, response_pending_ = true;
    bool response_audio_expected_ = false;
    int64_t turn_started_us_ = 1000000, last_turn_activity_us_ = 1000000;
    int64_t audio_stop_us_ = 1000000;
    std::string expected_audio_token_ = "turn-1", stt_text_, current_resp_id_;
    std::string pending_audio_frames_, pending_incoming_audio_frames_;
    UrAiVadEndpoint vad_endpoint_;
    UrAiTerminalTracker terminal_tracker_;
    int errors = 0, stops = 0, restarts = 0;
    void SendStopListening() { ++stops; }
    void SetError(int) { ++errors; }
    void EmitLegacyJson(const char* type, const char* state, const std::string& value) {
        assert(std::string(type) == "listen");
        assert(std::string(state) == "restart");
        assert(value == "turn-1");
        ++restarts;
    }
    void CheckTurnTimeouts();
};
// PRODUCTION_TIMEOUT
int main() {
    UrAiProtocol empty;
    now_us = 60999999;
    empty.CheckTurnTimeouts();
    assert(empty.errors == 0 && empty.restarts == 0 && empty.response_pending_);
    now_us = 61000000;
    empty.CheckTurnTimeouts();
    assert(empty.errors == 0 && empty.restarts == 1 && !empty.response_pending_);
    empty.CheckTurnTimeouts();
    assert(empty.restarts == 1);
    UrAiProtocol recognized;
    recognized.stt_text_ = "真实识别";
    recognized.CheckTurnTimeouts();
    assert(recognized.errors == 1 && recognized.restarts == 0);
    UrAiProtocol created;
    created.current_resp_id_ = "response-1";
    created.CheckTurnTimeouts();
    assert(created.errors == 1 && created.restarts == 0);
    UrAiProtocol image;
    image.audio_stop_sent_ = false;
    image.CheckTurnTimeouts();
    assert(image.errors == 1 && image.restarts == 0);
    UrAiProtocol starting;
    starting.audio_start_pending_ = true;
    starting.response_pending_ = false;
    starting.CheckTurnTimeouts();
    assert(starting.errors == 1 && starting.restarts == 0);
}
'''
        compiler = shutil.which('g++') or shutil.which('clang++')
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / 'no_result_test')
            subprocess.run([compiler, '-x', 'c++', '-std=c++17', '-Wall', '-Wextra',
                            '-Werror', '-I' + str(WATCHER_ROOT / 'main/protocols'),
                            '-', str(WATCHER_ROOT / 'main/protocols/ur_ai_runtime.cc'),
                            '-o', binary], input=fixture.replace('// PRODUCTION_TIMEOUT',
                                                               source[begin:end]),
                           text=True, capture_output=True, check=True)
            subprocess.run([binary], check=True, capture_output=True)
