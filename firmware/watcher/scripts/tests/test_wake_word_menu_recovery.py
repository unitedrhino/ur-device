"""编译实际唤醒事件入口，验证菜单吞掉唤醒后检测器仍能继续工作。"""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

# WATCHER_ROOT 指向被测固件，避免从其他构建目录读取实现。
WATCHER_ROOT = Path(__file__).resolve().parents[2]


class WakeWordMenuRecoveryTest(unittest.TestCase):
    """只替换音频和协议边界，菜单模型及唤醒事件使用生产代码。"""

    def test_blocked_detection_rearms_before_returning_home(self):
        """菜单及镜子内连续唤醒不建会话，回主页后的下一次唤醒正常建连。"""
        source = (WATCHER_ROOT / "main/application.cc").read_text()
        start = source.index("void Application::HandleWakeWordDetectedEvent() {")
        end = source.index("\nvoid Application::BeginWakeWordInvoke(", start)
        # fixture 只记录外设调用；Detect 模拟检测成功即暂停的音频回调契约。
        fixture = r'''
#include <atomic>
#include <cassert>
#include <string>
#include "main/boards/sensecap-watcher/ui/menu_model.h"
#define ESP_LOGI(...) ((void)0)
#define CONFIG_UR_AI_SIGNALING_WEBRTC 1
enum DeviceState { kDeviceStateIdle, kDeviceStateNotifying, kDeviceStateSpeaking,
                   kDeviceStateListening, kDeviceStateActivating };
constexpr int kAbortReasonWakeWordDetected = 1;
struct Audio {
    bool enabled = true;
    int rearms = 0;
    void EnableWakeWordDetection(bool enable) { enabled = enable; rearms += enable; }
    std::string GetLastWakeWord() { return "你好小智"; }
    bool PopPacketFromSendQueue() { return false; }
    void ResetDecoder() {}
    void PlaySound(int) {}
};
namespace Lang { namespace Sounds { constexpr int OGG_POPUP = 1; } }
struct Protocol { void SendStartListening(int) {} };
struct Application {
    std::atomic<bool> interaction_blocked_{false};
    bool human_call_audio_owned_=false;
    Protocol protocol_object;
    Protocol* protocol_ = &protocol_object;
    Audio audio_service_;
    DeviceState state = kDeviceStateIdle;
    bool play_popup_on_listening_ = false;
    int invocations = 0;
    DeviceState GetDeviceState() { return state; }
    void BeginWakeWordInvoke(const std::string&) { ++invocations; }
    void StopNotification() {}
    void AbortSpeaking(int) {}
    int GetDefaultListeningMode() { return 0; }
    void SetListeningMode(int) {}
    void SetDeviceState(DeviceState value) { state = value; }
    void HandleWakeWordDetectedEvent();
    void Detect() {
        assert(audio_service_.enabled);
        audio_service_.enabled = false;
        HandleWakeWordDetectedEvent();
    }
};
// PRODUCTION_HANDLER
int main() {
    for (bool mirror : {false, true}) {
        Application app;
        watcher::MenuModel menu;
        menu.Handle(mirror ? watcher::MenuInput::MirrorOn : watcher::MenuInput::LongPress, 1);
        app.interaction_blocked_ = menu.Page() != watcher::MenuPage::Home;
        app.Detect();
        assert(app.invocations == 0 && app.audio_service_.enabled);
        app.Detect();
        assert(app.invocations == 0 && app.audio_service_.rearms == 2);
        menu.Handle(watcher::MenuInput::Back, 2);
        assert(menu.Page() == watcher::MenuPage::Home);
        app.interaction_blocked_ = false;
        app.Detect();
        assert(app.invocations == 1);
    }
    Application call;
    call.human_call_audio_owned_=true;
    call.Detect();
    assert(call.invocations==0 && !call.audio_service_.enabled && call.audio_service_.rearms==0);
    Application normal;
    normal.Detect();
    assert(normal.invocations == 1 && normal.audio_service_.rearms == 0);
}
'''
        compiler = shutil.which("g++") or shutil.which("clang++")
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / "wake_menu_recovery")
            subprocess.run(
                [compiler, "-x", "c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                 "-I", str(WATCHER_ROOT), "-", "-o", binary],
                input=fixture.replace("// PRODUCTION_HANDLER", source[start:end]),
                text=True, capture_output=True, check=True,
            )
            result = subprocess.run([binary], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
