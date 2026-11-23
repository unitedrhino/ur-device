"""执行完整生产菜单消费者，拒绝上一页排队按键进入新页面。"""
from pathlib import Path
import subprocess
import tempfile
import unittest

# ROOT定位真实板级事件及消费者，夹具只隔离RTOS和硬件边界。
ROOT = Path(__file__).resolve().parents[2]


class DialPageQueueTest(unittest.TestCase):
    """菜单页变化后，原表盘单击不能确认设置中的另一项。"""

    def test_old_click_cannot_confirm_new_page(self):
        """同批次长按打开设置，先前排队的表盘单击应作废。"""
        source = (ROOT / 'main/boards/sensecap-watcher/sensecap_watcher.cc').read_text()
        start = source.index('        struct MenuEvent {')
        event = source[start:source.index('\n        };', start) + len('\n        };')]
        start = source.index('        void PollMenu() {')
        consumer = source[start:source.index('        // SetupMirrorExitTouch', start)]
        code = r'''
#include <atomic>
#include <cassert>
#include <deque>
#include <functional>
#include <vector>
#include "ui/menu_model.h"
#define CONFIG_UR_AI_SIGNALING_WEBRTC 0
#define pdTRUE 1
uint32_t lv_tick_get() { return 100; }
struct Display {
EVENT
    watcher::MenuModel menu_model_;
    std::deque<MenuEvent> queue;
    std::deque<MenuEvent>* menu_events_=&queue;
    std::atomic<uint32_t> input_epoch_{7};
    bool call_ui_active_=false,charging_=false,render_pending_=false;
    std::vector<int> contacts_;
    std::function<void(watcher::MenuAction,int)> menu_action_=[](auto,int){};
    void DrainUi() {}
    bool RefreshHomeMode() {return false;}
    void RefreshDialClock() {}
    bool DrainContacts() {return false;}
    void RenderMenu() {}
    void ShowNotification(const char*) {}
    static int xQueueReceive(std::deque<MenuEvent>* queue,MenuEvent* event,int) {
        if(queue->empty())return 0;
        *event=queue->front();queue->pop_front();return 1;
    }
CONSUMER
};
int main() {
    Display display;
    Display::MenuEvent event{};
    event.input_epoch=7;event.touch_revision=display.menu_model_.Revision();
    event.input=watcher::MenuInput::LongPress;display.queue.push_back(event);
    event.input=watcher::MenuInput::Click;display.queue.push_back(event);
    event.input=watcher::MenuInput::SwipeNext;display.queue.push_back(event);
    display.PollMenu();
    assert(display.menu_model_.Page()==watcher::MenuPage::Main && display.menu_model_.Focus()==1);
}
'''.replace('EVENT', event).replace('CONSUMER', consumer)
        with tempfile.TemporaryDirectory() as directory:
            executable = str(Path(directory) / 'page')
            subprocess.run(['g++','-std=c++17','-I',str(ROOT / 'main/boards/sensecap-watcher'),
                            '-x','c++','-','-o',executable],input=code,text=True,check=True)
            subprocess.run([executable],check=True)
