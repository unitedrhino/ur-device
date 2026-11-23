"""回放真实板级音量方法，验证数值提示、边界与硬件回读，不代替物理滚轮验收。"""
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class WatcherVolumeFeedbackTest(unittest.TestCase):
    """仅替换音频硬件及显示邮箱边界，执行生产方法。"""

    def test_actual_volume_notification(self):
        """方向由菜单模型验证；板级必须反馈真实值及百分号，并限制短提示时长。"""
        source = (ROOT / 'main/boards/sensecap-watcher/sensecap_watcher.cc').read_text()
        begin = source.index('    void ApplyMenuVolume(')
        end = source.index('    void InitializeKnob()', begin)
        code = r'''
#include <algorithm>
#include <cassert>
#include <string>
namespace Lang { namespace Strings { const char* VOLUME = "音量"; } }
// 模拟板级上报入口；测试只关注音量硬件回读和提示文案，不连接物模型服务。
void ur_device_request_report() {}
struct Board {
    int volume = 0, readback = -1, duration = 0;
    std::string notification;
    Board* GetAudioCodec() { return this; }
    Board* GetDisplay() { return this; }
    void SetOutputVolume(int value) { volume = value; }
    int output_volume() { return readback < 0 ? volume : readback; }
    void ShowNotification(const std::string& text, int ms) { notification = text; duration = ms; }
METHOD
};
int main() {
    Board board;
    board.ApplyMenuVolume(55);
    assert(board.volume == 55 && board.notification == "音量: 55%" && board.duration == 2000);
    board.ApplyMenuVolume(105);
    assert(board.volume == 100 && board.notification == "音量: 100%");
    board.ApplyMenuVolume(-5);
    assert(board.volume == 0 && board.notification == "音量: 0%");
    board.readback = 40;
    board.ApplyMenuVolume(45);
    assert(board.volume == 45 && board.notification == "音量: 40%");
}
'''.replace('METHOD', source[begin:end])
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / 'volume-feedback')
            compiled = subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                       '-x', 'c++', '-', '-o', binary], input=code,
                                      text=True, capture_output=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            for _ in range(10):
                result = subprocess.run([binary], text=True, capture_output=True)
                self.assertEqual(result.returncode, 0, result.stderr)
