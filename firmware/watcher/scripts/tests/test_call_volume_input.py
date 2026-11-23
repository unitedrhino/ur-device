"""通话页面使用生产输入分支验证旋钮操作和音量边界。"""
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class CallVolumeInputTest(unittest.TestCase):
    """验证通话输入不会进入普通菜单，并按最新音量执行连续增减。"""

    def test_call_knob_controls_volume_and_click(self):
        source = (ROOT / 'main/boards/sensecap-watcher/sensecap_watcher.cc').read_text()
        start = source.index('            if (call_ui_active_) {', source.index('void PollMenu()'))
        end = source.index('            MenuEvent event;', source.index('                return;', start))
        route = source[start:end]
        start = source.index('            case watcher::MenuAction::CallVolumeDelta:')
        end = source.index('                break;', start) + len('                break;')
        action = source[start:end]
        code = r'''
#include <algorithm>
#include <cassert>
#include <deque>
#include <functional>
#include "ui/menu_model.h"
#define CONFIG_UR_AI_SIGNALING_WEBRTC 1
#define pdTRUE 1
namespace watcher {
struct WatcherCallUi {
    int clicks = 0;
    static WatcherCallUi& Instance() { static WatcherCallUi ui; return ui; }
    void OnKnobClick() { ++clicks; }
};
}
struct MenuEvent { watcher::MenuInput input; bool asleep; int touch_index = -1; };
using Queue = std::deque<MenuEvent>;
int xQueueReceive(Queue* q, MenuEvent* e, int) {
    if (q->empty()) return 0;
    *e = q->front(); q->pop_front(); return pdTRUE;
}
struct Codec { int volume = 50; int output_volume() const { return volume; } };
struct Display {
    bool call_ui_active_ = true;
    Queue events;
    Queue* menu_events_ = &events;
    Codec codec;
    std::function<void(watcher::MenuAction, int)> menu_action_;
    Codec* GetAudioCodec() { return &codec; }
    void ApplyMenuVolume(int value) { codec.volume = std::clamp(value, 0, 100); }
    void Execute(watcher::MenuAction action, int value) {
        switch (action) {
ACTION
        default: assert(false);
        }
    }
    void PollMenu() {
ROUTE
        assert(false);
    }
};
int main() {
    Display display;
    std::deque<std::pair<watcher::MenuAction, int>> pending;
    display.menu_action_ = [&](auto action, auto value) { pending.emplace_back(action, value); };
    using I = watcher::MenuInput;
    display.events = {{I::Previous, false}, {I::Previous, false}, {I::Next, false},
                      {I::Previous, true}, {I::Previous, false, 1},
                      {I::Click, false}, {I::Click, true}, {I::LongPress, false}};
    display.PollMenu();
    assert(watcher::WatcherCallUi::Instance().clicks == 1);
    assert(pending.size() == 3);
    for (auto [action, value] : pending) display.Execute(action, value);
    assert(display.codec.volume == 55);
    display.codec.volume = 99;
    display.Execute(watcher::MenuAction::CallVolumeDelta, 5);
    assert(display.codec.volume == 100);
    display.codec.volume = 1;
    display.Execute(watcher::MenuAction::CallVolumeDelta, -5);
    assert(display.codec.volume == 0);
}
'''.replace('ROUTE', route).replace('ACTION', action)
        with tempfile.TemporaryDirectory() as temp:
            src = Path(temp) / 'call_volume.cc'
            exe = Path(temp) / 'call_volume'
            src.write_text(code)
            subprocess.run(['c++', '-std=c++17', '-I', str(ROOT / 'main/boards/sensecap-watcher'),
                            str(src), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)
