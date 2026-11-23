"""执行真实应用待机分支，验证迟到状态不会关闭人际音频。"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class CallIdleAudioOwnerTest(unittest.TestCase):
    """复用真实待机分支，隔离显示和音频驱动。"""

    def test_idle_preserves_owned_call_audio(self):
        source = (ROOT / 'main/application.cc').read_text()
        start = source.index('        case kDeviceStateUnknown:', source.index('void Application::HandleStateChangedEvent()'))
        end = source.index('        case kDeviceStateConnecting:', start)
        code = r'''
#include <cassert>
#include <string>
#define CONFIG_UR_AI 0
#define CONFIG_UR_AI_SIGNALING_WEBRTC 1
namespace Lang { namespace Strings { constexpr const char* STANDBY = "standby"; } }
enum State { kDeviceStateUnknown, kDeviceStateIdle };
struct Display {void SetStatus(const char*) {} void ClearChatMessages() {} void SetEmotion(const char*) {}};
struct Audio {bool processing=true, wake=false;
 void EnableVoiceProcessing(bool v) { processing=v; }
 void EnableWakeWordDetection(bool v) { wake=v; }};
struct App {bool human_call_audio_owned_=false;std::string last_error_message_;Audio audio_service_;
 void handle(State state) { Display local;auto* display=&local; switch(state) {
// IDLE_BRANCH
 } }
};
int main() {
 App app;app.human_call_audio_owned_=true;app.handle(kDeviceStateIdle);
 assert(app.audio_service_.processing && !app.audio_service_.wake);
 app.human_call_audio_owned_=false;app.handle(kDeviceStateIdle);
 assert(!app.audio_service_.processing && app.audio_service_.wake);
}
'''.replace('// IDLE_BRANCH', source[start:end])
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / 'idle_test')
            subprocess.run(['c++', '-x', 'c++', '-std=c++17', '-Wall', '-Wextra',
                            '-Werror', '-', '-o', binary], input=code, text=True, check=True)
            subprocess.run([binary], check=True)
            # 不含人际通话的构建保持原有待机处理。
            udp_code = code.replace('#define CONFIG_UR_AI_SIGNALING_WEBRTC 1',
                                    '#define CONFIG_UR_AI_SIGNALING_WEBRTC 0')
            udp_code = udp_code[:udp_code.index('int main() {')] + r"""
int main() {
 App app; app.handle(kDeviceStateIdle);
 assert(!app.audio_service_.processing && app.audio_service_.wake);
}
"""
            subprocess.run(['c++', '-x', 'c++', '-std=c++17', '-Wall', '-Wextra',
                            '-Werror', '-', '-o', binary], input=udp_code, text=True, check=True)
            subprocess.run([binary], check=True)


if __name__ == '__main__':
    unittest.main()
