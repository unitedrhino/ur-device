"""联编生产通话输入分支，验证旧菜单点击不会被解释为挂断。"""
from pathlib import Path
import subprocess
import tempfile
import unittest

# ROOT固定被测工程，事件定义及消费者均从生产板级源码提取。
ROOT = Path(__file__).resolve().parents[2]


class DialInputEpochTest(unittest.TestCase):
    """模式代次与触摸来源在消费时再次检查。"""

    def test_stale_menu_input_does_not_hang_up(self):
        """有效通话按键和音量仍生效，过期点击、菜单触点和唤醒点击被丢弃。"""
        source = (ROOT / 'main/boards/sensecap-watcher/sensecap_watcher.cc').read_text()
        start = source.index('        struct MenuEvent {')
        event = source[start:source.index('\n        };', start) + len('\n        };')]
        start = source.index('        void PollMenu() {')
        consumer = source[start:source.index('            MenuEvent event;\n            bool dirty', start)] + '\n        }'
        code = r'''

#include "ui/menu_model.h"
#include <atomic>
#include <cassert>
#include <deque>
#include <functional>
#define CONFIG_UR_AI_SIGNALING_WEBRTC 1
#define pdTRUE 1
namespace watcher {
struct WatcherCallUi {
  int clicks = 0;
  static WatcherCallUi &Instance() {
    static WatcherCallUi value;
    return value;
  }
  void OnKnobClick() { ++clicks; }
};
} // namespace watcher
struct Display {
  EVENT
  std::deque<MenuEvent> queue;
  std::deque<MenuEvent> *menu_events_ = &queue;
  std::atomic<uint32_t> input_epoch_{7};
  bool call_ui_active_ = true;
  int volume = 0;
  std::function<void(watcher::MenuAction, int)> menu_action_ =
      [&](auto, int delta) { volume += delta; };
  void DrainUi() {}
  bool RefreshHomeMode() { return false; }
  void RefreshDialClock() {}
  static int xQueueReceive(std::deque<MenuEvent> *q, MenuEvent *event, int) {
    if (q->empty())
      return 0;
    *event = q->front();
    q->pop_front();
    return 1;
  }
  CONSUMER
};
int main() {
  Display view;
  Display::MenuEvent event{};
  event.input = watcher::MenuInput::Click;
  event.input_epoch = 6;
  view.queue.push_back(event);
  event.input_epoch = 7;
  event.touch_index = 24;
  view.queue.push_back(event);
  event.touch_index = -1;
  event.asleep = true;
  view.queue.push_back(event);
  event.asleep = false;
  view.queue.push_back(event);
  event.confirm_network = true;
  view.queue.push_back(event);
  event.confirm_network = false;
  event.input = watcher::MenuInput::Next;
  view.queue.push_back(event);
  event.input = watcher::MenuInput::SwipeNext;
  view.queue.push_back(event);
  event.input = watcher::MenuInput::SwipePrevious;
  view.queue.push_back(event);
  view.PollMenu();
  assert(watcher::WatcherCallUi::Instance().clicks == 1 && view.volume == -5);
}
'''.replace('EVENT', event).replace('CONSUMER', consumer)
        with tempfile.TemporaryDirectory() as directory:
            executable = str(Path(directory) / 'epoch')
            subprocess.run(['g++', '-std=c++17', '-I', str(ROOT / 'main/boards/sensecap-watcher'),
                            '-x', 'c++', '-', '-o', executable], input=code, text=True, check=True)
            subprocess.run([executable], check=True)

    def test_clock_beijing_and_minute_updates(self):
        """执行生产时钟函数，覆盖校时前、北京时间跨日和同分钟去重。"""
        source = (ROOT / 'main/boards/sensecap-watcher/sensecap_watcher.cc').read_text()
        start = source.index('        void RefreshDialClock() {')
        method = source[start:source.index('        // DrainContacts', start)]
        code = r'''
#include <ctime>
#include <cstdint>
#include <cassert>
#include <string>
static time_t now_value = 0;
static time_t test_time(time_t*) { return now_value; }
#define time test_time
struct Display {
    int64_t clock_minute_=-1;
    struct View {
        std::string text="--:--";
        int updates=0;
        void UpdateClock(const char* value) { text=value; ++updates; }
    } dial_view_;
CLOCK_METHOD
};
int main() {
    Display display;
    display.RefreshDialClock();
    assert(display.dial_view_.text=="--:--" && display.dial_view_.updates==0);
    now_value=1704067200;
    display.RefreshDialClock();
    assert(display.dial_view_.text=="08:00" && display.dial_view_.updates==1);
    now_value+=20;
    display.RefreshDialClock();
    assert(display.dial_view_.updates==1);
    now_value=1704124800;
    display.RefreshDialClock();
    assert(display.dial_view_.text=="00:00");
}
'''.replace('CLOCK_METHOD', method)
        with tempfile.TemporaryDirectory() as directory:
            executable = str(Path(directory) / 'clock')
            subprocess.run(['g++', '-std=c++17', '-x', 'c++', '-', '-o', executable],
                           input=code, text=True, check=True)
            subprocess.run([executable], check=True)

    def test_skin_status_colors_match_all_pages(self):
        """执行生产调色函数，三套皮肤的聆听、表盘和菜单均使用对应辅助文字颜色。"""
        source = (ROOT / 'main/boards/sensecap-watcher/sensecap_watcher.cc').read_text()
        start = source.index('        void ApplyPagePalette() {')
        method = source[start:source.index('        // RenderMenu', start)]
        code = r'''
#include <cassert>
#include <cstdint>
#include <initializer_list>
#include "ui/menu_model.h"
#include "ui/skin_palette.h"
struct Object { uint32_t color=0;bool hidden=false; };
struct LvglTheme { const char* selected="dark"; const char* name() const {return selected;} };
constexpr int LV_OPA_TRANSP=0,LV_OBJ_FLAG_HIDDEN=1;
uint32_t lv_color_hex(uint32_t color){return color;}
void lv_obj_set_style_text_color(Object* object,uint32_t color,int){object->color=color;}
void lv_obj_set_style_bg_opa(Object*,int,int){}
void lv_obj_remove_flag(Object* object,int){object->hidden=false;}
void lv_obj_add_flag(Object* object,int){object->hidden=true;}
struct Display {
    bool call_ui_active_=false,home_surface_=false;
    uint32_t notification_ms_=0;
    watcher::MenuModel menu_model_;
    void LayoutStatusBar(bool) {}
    void LayoutStatusText(bool) {}
    LvglTheme theme;
    void* current_theme_=&theme;
    Object network,mute,battery,status,notice,top,bar;
    Object *network_label_=&network,*mute_label_=&mute,*battery_label_=&battery,
      *status_label_=&status,*notification_label_=&notice,*top_bar_=&top,*status_bar_=&bar;
PALETTE
};
int main(){
    Display display;
    display.ApplyPagePalette();assert(display.network.color==0xc3cfc2 && display.bar.hidden);
    display.notification_ms_=2000;
    display.ApplyPagePalette();assert(!display.bar.hidden);
    display.notification_ms_=0;
    display.ApplyPagePalette();assert(display.bar.hidden);
    display.home_surface_=true;
    display.ApplyPagePalette();assert(display.network.color==0xc3cfc2 && !display.bar.hidden);
    for(const auto& skin : watcher::kSkins) {
      display.theme.selected=skin.id;
      display.ApplyPagePalette();
      assert(display.network.color==skin.muted);
    }
    display.theme.selected="dark";
    display.menu_model_.Handle(watcher::MenuInput::LongPress,1);
    display.ApplyPagePalette();assert(display.network.color==0xc3cfc2 && display.bar.hidden);
}
'''.replace('PALETTE', method)
        with tempfile.TemporaryDirectory() as directory:
            executable = str(Path(directory) / 'palette')
            subprocess.run(['g++','-std=c++17','-I',str(ROOT / 'main/boards/sensecap-watcher'),
                            '-x','c++','-','-o',executable],input=code,text=True,check=True)
            subprocess.run([executable],check=True)

    def test_held_touch_cannot_cross_page_or_call_mode(self):
        """执行生产触摸读取回调，切页和来电时取消旧触点，抬手后新点击恢复。"""
        source = (ROOT / 'main/boards/sensecap-watcher/sensecap_watcher.cc').read_text()
        start = source.index('        void AttachTouch(')
        method = source[start:source.index('        // PostTouch ', start)]
        code = r'''
#include <atomic>
#include <cstdint>
#include <functional>
#include <utility>
#include <cassert>
#include "ui/menu_model.h"
#include "ui/touch_gate.h"
#define ESP_LOGI(...) ((void)0)
constexpr int ESP_OK=0,LV_INDEV_STATE_RELEASED=0,LV_INDEV_STATE_PRESSED=1,LV_INDEV_TYPE_POINTER=0;
struct lv_point_t {int32_t x=0,y=0;};
struct lv_indev_data_t {int state;lv_point_t point;};
struct lv_indev_t {void* user=nullptr;void(*read)(lv_indev_t*,lv_indev_data_t*)=nullptr;int resets=0;} pointer;
using esp_lcd_touch_handle_t=void*;
struct esp_lcd_touch_point_data_t {uint16_t x=200,y=300;};
bool pressed=false,valid=true;
uint32_t lv_tick_get(){return 100;}
lv_indev_t* lv_indev_create(){return &pointer;}
void lv_indev_set_type(lv_indev_t*,int){}
void lv_indev_set_display(lv_indev_t*,void*){}
void lv_indev_set_user_data(lv_indev_t* p,void* v){p->user=v;}
void* lv_indev_get_user_data(lv_indev_t* p){return p->user;}
void lv_indev_set_read_cb(lv_indev_t* p,void(*cb)(lv_indev_t*,lv_indev_data_t*)){p->read=cb;}
void lv_indev_reset(lv_indev_t* p,void*){++p->resets;}
int esp_lcd_touch_read_data(void*){return valid ? 0:1;}
int esp_lcd_touch_get_data(void*,esp_lcd_touch_point_data_t*,uint8_t* count,int){*count=pressed?1:0;return 0;}
struct DisplayLockGuard{DisplayLockGuard(void*){}};
struct CustomLcdDisplay {
    void* display_=nullptr;void* touch_handle_=nullptr;
    std::function<bool()> touch_wake_;
    std::function<void(size_t,uint32_t)> touch_item_;
    watcher::MenuModel menu_model_;watcher::TouchGate touch_gate_;
    std::atomic<uint32_t> input_epoch_{0};
    uint32_t touch_revision_=0,touch_epoch_=0,touch_started_=0;
    lv_point_t touch_origin_{};
    bool touch_origin_valid_=false,call_ui_active_=false;
    unsigned touch_trace_count_=0;
METHOD
};
int main(){
    CustomLcdDisplay display;bool waking=false;
    display.AttachTouch(nullptr,[&]{return waking;},[](auto,auto){});
    auto read=[] {lv_indev_data_t data{};pointer.read(&pointer,&data);return data.state;};
    pressed=true;assert(read()==LV_INDEV_STATE_PRESSED);
    display.menu_model_.Handle(watcher::MenuInput::LongPress,100);
    assert(read()==LV_INDEV_STATE_RELEASED && pointer.resets>0);
    assert(read()==LV_INDEV_STATE_RELEASED);
    pressed=false;assert(read()==LV_INDEV_STATE_RELEASED);
    pressed=true;assert(read()==LV_INDEV_STATE_PRESSED);
    ++display.input_epoch_;display.call_ui_active_=true;
    assert(read()==LV_INDEV_STATE_RELEASED);
    pressed=false;read();pressed=true;assert(read()==LV_INDEV_STATE_PRESSED);
    valid=false;assert(read()==LV_INDEV_STATE_RELEASED);
    valid=true;assert(read()==LV_INDEV_STATE_RELEASED);
    pressed=false;read();waking=true;pressed=true;assert(read()==LV_INDEV_STATE_RELEASED);
    waking=false;assert(read()==LV_INDEV_STATE_RELEASED);
    pressed=false;read();pressed=true;assert(read()==LV_INDEV_STATE_PRESSED);
}
'''.replace('METHOD', method)
        with tempfile.TemporaryDirectory() as directory:
            executable = str(Path(directory) / 'touch')
            subprocess.run(['g++','-std=c++17','-I',str(ROOT / 'main/boards/sensecap-watcher'),
                            '-x','c++','-','-o',executable],input=code,text=True,check=True)
            subprocess.run([executable],check=True)
