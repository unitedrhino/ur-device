"""执行真实建连续接及默认模式函数，验证首轮遵循建连后协商结果。"""
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def method(source, signature):
    """提取完整生产方法，不复制模式决策。"""
    start = source.index(signature)
    return source[start:source.index('\n}\n', start) + 3]


class NegotiatedListeningModeTest(unittest.TestCase):
    """只替换通道和板级操作，复现连接前快照覆盖新模式。"""

    def test_first_turn_uses_negotiated_mode(self):
        source = (ROOT / 'main/application.cc').read_text()
        methods = '\n'.join(method(source, name) for name in [
            'void Application::ContinueOpenAudioChannel(',
            'ListeningMode Application::GetDefaultListeningMode() const',
        ])
        code = r'''
#include <cassert>
#define CONFIG_UR_AI_SIGNALING 1
enum ListeningMode { kListeningModeAutoStop, kListeningModeManualStop, kListeningModeRealtime };
enum DeviceState { kDeviceStateIdle, kDeviceStateConnecting, kDeviceStateListening };
enum class PowerSaveLevel { PERFORMANCE };
constexpr int kAecOff=0;
struct Board {
    static Board& GetInstance() { static Board value; return value; }
    void SetPowerSaveLevel(PowerSaveLevel) {}
};
// OpenAudioChannel只模拟连接结束时协商出的事实，不实现应用模式决策。
struct Protocol {
    bool opened=false, full=false, negotiatedFull=true, succeeds=true;
    int opens=0;
    bool IsAudioChannelOpened() { return opened; }
    bool IsFullDuplexSession() { return full; }
    bool OpenAudioChannel() { ++opens; full=negotiatedFull; opened=succeeds; return opened; }
};
struct Application {
    Protocol protocol;
    Protocol* protocol_=&protocol;
    int aec_mode_=kAecOff, modeCalls=0;
    DeviceState state=kDeviceStateConnecting;
    ListeningMode selected=kListeningModeAutoStop;
    DeviceState GetDeviceState() const { return state; }
    void SetDeviceState(DeviceState value) { state=value; }
    void SetListeningMode(ListeningMode value) { selected=value; ++modeCalls; state=kDeviceStateListening; }
    ListeningMode GetDefaultListeningMode() const;
    void ContinueOpenAudioChannel(ListeningMode);
};
METHODS
int main() {
    // 自动请求在连接前只能读到半双工，连接后必须采用真实全双工。
    Application full;
    auto before=full.GetDefaultListeningMode();
    assert(before==kListeningModeAutoStop);
    full.ContinueOpenAudioChannel(before);
    assert(full.protocol.full);
    assert(full.selected==kListeningModeRealtime && full.modeCalls==1);
    Application half; half.protocol.negotiatedFull=false;
    half.ContinueOpenAudioChannel(kListeningModeAutoStop);
    assert(half.selected==kListeningModeAutoStop);
    // 按住说话的显式手动模式不因协商被悄悄改为自动。
    Application manual;
    manual.ContinueOpenAudioChannel(kListeningModeManualStop);
    assert(manual.selected==kListeningModeManualStop);
    Application failed; failed.protocol.succeeds=false;
    failed.ContinueOpenAudioChannel(kListeningModeAutoStop);
    assert(failed.state==kDeviceStateIdle && failed.modeCalls==0);
    Application stale; stale.state=kDeviceStateIdle;
    stale.ContinueOpenAudioChannel(kListeningModeAutoStop);
    assert(stale.protocol.opens==0 && stale.modeCalls==0);
    Application opened; opened.protocol.opened=true; opened.protocol.full=true;
    opened.ContinueOpenAudioChannel(kListeningModeAutoStop);
    assert(opened.selected==kListeningModeRealtime && opened.protocol.opens==0);
}
'''.replace('METHODS', methods)
        with tempfile.TemporaryDirectory(prefix='watcher-negotiated-mode-') as directory:
            binary = str(Path(directory) / 'negotiated-mode')
            built = subprocess.run(['g++', '-std=c++17', '-x', 'c++', '-', '-o', binary],
                                   input=code, text=True, capture_output=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            result = subprocess.run([binary], text=True, capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
