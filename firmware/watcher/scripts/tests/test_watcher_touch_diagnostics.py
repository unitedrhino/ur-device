"""回放生产触摸采样与点击消费，验证诊断只在测试固件存在且不改变输入门禁。"""
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class WatcherTouchDiagnosticsTest(unittest.TestCase):
    """使用硬件边界替身执行真实回调，不把采样回放视为物理触屏验收。"""

    def test_sampling_and_test_only_counters(self):
        """覆盖无触点、完整接触、仅唤醒和连续 I2C 故障；正式版不保留计数器。"""
        source = (ROOT / "main/boards/sensecap-watcher/sensecap_watcher.cc").read_text()
        begin = source.index("        void AttachTouch(")
        end = source.index("        // PostTouch", begin)
        fields_begin = source.index("#if CONFIG_UR_AI_E2E_TEST\n        uint32_t touch_reads_")
        fields_end = source.index("#endif", fields_begin) + len("#endif")
        harness = r'''
#include <cassert>
#include <cstdint>
#include <functional>
#include <utility>
#include "ui/touch_gate.h"
// 只替代 I2C 和 LVGL 注册边界，生产回调及 TouchGate 均原样执行。
constexpr int ESP_OK=0, LV_INDEV_STATE_RELEASED=0, LV_INDEV_STATE_PRESSED=1, LV_INDEV_TYPE_POINTER=2;
using esp_lcd_touch_handle_t=void*;
struct esp_lcd_touch_point_data_t { int x=12,y=34; };
struct lv_indev_data_t { int state=-1; struct { int x=0,y=0; } point; };
struct lv_indev_t { void* user=nullptr; void (*read)(lv_indev_t*,lv_indev_data_t*)=nullptr; } input;
bool read_ok=true, data_ok=true;
uint8_t point_count=0;
int reset_count=0;
int esp_lcd_touch_read_data(void*) { return read_ok ? ESP_OK : 1; }
int esp_lcd_touch_get_data(void*,esp_lcd_touch_point_data_t*,uint8_t* count,int) {
    *count=point_count; return data_ok ? ESP_OK : 1;
}
lv_indev_t* lv_indev_create() { return &input; }
void lv_indev_set_type(lv_indev_t*,int) {}
void lv_indev_set_display(lv_indev_t*,void*) {}
void lv_indev_set_user_data(lv_indev_t* p,void* user) { p->user=user; }
void* lv_indev_get_user_data(lv_indev_t* p) { return p->user; }
void lv_indev_set_read_cb(lv_indev_t* p,void (*read)(lv_indev_t*,lv_indev_data_t*)) { p->read=read; }
void lv_indev_reset(lv_indev_t*,void*) { ++reset_count; }
uint32_t lv_tick_get() { return 1; }
struct DisplayLockGuard { explicit DisplayLockGuard(void*) {} };
struct Model { int activities=0; uint32_t Revision() { return 7; } void TouchActivity(uint32_t) { ++activities; } };
struct CustomLcdDisplay {
    void* display_=nullptr;
    esp_lcd_touch_handle_t touch_handle_=nullptr;
    std::function<bool()> touch_wake_;
    std::function<void(size_t,uint32_t)> touch_item_;
    watcher::TouchGate touch_gate_;
    uint32_t touch_revision_=0;
    Model menu_model_;
FIELDS
METHOD
};
int main() {
    CustomLcdDisplay display;
    bool asleep=false;
    int wake_calls=0;
    display.AttachTouch(&display,[&] { ++wake_calls; return asleep; },[](size_t,uint32_t) {});
    auto sample=[&](int expected) {
        lv_indev_data_t data;
        input.read(&input,&data);
        assert(data.state==expected);
        if (expected==LV_INDEV_STATE_PRESSED) assert(data.point.x==12 && data.point.y==34);
    };
    sample(LV_INDEV_STATE_RELEASED);
    point_count=1;
    sample(LV_INDEV_STATE_PRESSED); sample(LV_INDEV_STATE_PRESSED);
    assert(wake_calls==1 && display.touch_revision_==7);
    point_count=0; sample(LV_INDEV_STATE_RELEASED);
    asleep=true; point_count=1;
    sample(LV_INDEV_STATE_RELEASED); sample(LV_INDEV_STATE_RELEASED);
    assert(wake_calls==2);
    point_count=0; sample(LV_INDEV_STATE_RELEASED);
    asleep=false; read_ok=false;
    sample(LV_INDEV_STATE_RELEASED);
    read_ok=true; point_count=1;
    sample(LV_INDEV_STATE_RELEASED);
    assert(wake_calls==2); // 通讯失败后未抬手，不能重新执行业务。
    point_count=0; sample(LV_INDEV_STATE_RELEASED);
    point_count=1; sample(LV_INDEV_STATE_PRESSED);
    data_ok=false; sample(LV_INDEV_STATE_RELEASED);
    assert(reset_count==2 && display.menu_model_.activities==3);
#if CONFIG_UR_AI_E2E_TEST
    assert(display.touch_reads_==12 && display.touch_errors_==2);
    assert(display.touch_contacts_==3 && display.touch_wakes_==1);
#endif
}
'''.replace("FIELDS", source[fields_begin:fields_end]).replace("METHOD", source[begin:end])
        with tempfile.TemporaryDirectory() as directory:
            for enabled in (0, 1):
                binary = str(Path(directory) / f"touch-{enabled}")
                subprocess.run(["g++", "-std=c++17", f"-DCONFIG_UR_AI_E2E_TEST={enabled}",
                                "-I", str(ROOT / "main/boards/sensecap-watcher"),
                                "-x", "c++", "-", "-o", binary], input=harness,
                               text=True, capture_output=True, check=True)
                for _ in range(10):
                    subprocess.run([binary], check=True, capture_output=True)

    def test_consumed_click_reason_counters(self):
        """执行实际消费片段：只统计触摸，区分占用和旧页面，计数不参与动作决策。"""
        source = (ROOT / "main/boards/sensecap-watcher/sensecap_watcher.cc").read_text()
        begin = source.index("#if CONFIG_UR_AI_E2E_TEST\n                if (event.touch_index >= 0)")
        end = source.index("#endif", begin) + len("#endif")
        code = r'''
#include <cassert>
struct Event { int touch_index=-1; bool busy=false; unsigned touch_revision=7; } event;
struct Model { unsigned Revision() { return 7; } } menu_model_;
unsigned touch_events_=0, touch_busy_=0, touch_stale_=0;
void consume() {
COUNTERS
}
int main() {
    consume(); assert(touch_events_==0);
    event.touch_index=0; consume();
    assert(touch_events_==1 && touch_busy_==0 && touch_stale_==0);
    event.busy=true; consume();
    assert(touch_events_==2 && touch_busy_==1 && touch_stale_==0);
    event.busy=false; event.touch_revision=6; consume();
    assert(touch_events_==3 && touch_busy_==1 && touch_stale_==1);
}
'''.replace("COUNTERS", source[begin:end])
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / "touch-consume")
            subprocess.run(["g++", "-std=c++17", "-DCONFIG_UR_AI_E2E_TEST=1",
                            "-x", "c++", "-", "-o", binary], input=code,
                           text=True, capture_output=True, check=True)
            for _ in range(10):
                subprocess.run([binary], check=True, capture_output=True)


if __name__ == "__main__":
    unittest.main()
