"""执行生产 LCD 预览逻辑，复现上一轮长字幕覆盖相机画面的问题。"""
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class PreviewVisibilityTest(unittest.TestCase):
    """替换 LVGL 边界，验证预览期间字幕不可见、结束后内容仍可恢复。"""

    def test_preview_hides_old_and_incoming_subtitles(self):
        """覆盖旧字幕、新字幕、清空、全局隐藏和重复结束预览。"""
        source = (ROOT / "main/display/lcd_display.cc").read_text()
        start = source.rindex("void LcdDisplay::SetPreviewImage(")
        end = source.index("void LcdDisplay::ClearChatMessages()", start)
        harness = r'''
#include <cassert>
#include <memory>
#include <string>
#define CONFIG_USE_MULTILINE_CHAT_MESSAGE 1
#define LV_OBJ_FLAG_HIDDEN 1
#define LV_ALIGN_BOTTOM_MID 0
#define PREVIEW_IMAGE_DURATION_MS 5000
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_ERROR_CHECK(x) ((void)(x))
struct Obj { bool hidden=false; std::string text; };
struct Dsc { struct { int w=640, h=480; } header; };
struct LvglImage { Dsc dsc; Dsc* image_dsc() { return &dsc; } };
struct Gif { void Start() {} void Stop() {} };
void lv_obj_add_flag(Obj* o,int) { o->hidden=true; }
void lv_obj_remove_flag(Obj* o,int) { o->hidden=false; }
void lv_image_set_src(Obj*,Dsc*) {}
void lv_image_set_scale(Obj*,int) {}
void lv_anim_delete(Obj*,void*) {}
void lv_obj_align(Obj*,int,int,int) {}
void lv_label_set_text(Obj* o,const char* t) { o->text=t ? t : ""; }
const char* lv_label_get_text(Obj* o) { return o->text.c_str(); }
int esp_timer_stop(int) { return 0; }
int esp_timer_start_once(int,int) { return 0; }
struct DisplayLockGuard { template<class T> explicit DisplayLockGuard(T*) {} };
struct LcdDisplay {
    Obj preview, emoji, bottom, label;
    Obj *preview_image_=&preview, *emoji_box_=&emoji, *bottom_bar_=&bottom,
        *chat_message_label_=&label;
    int preview_timer_=0, width_=412;
    bool hide_subtitle_=false, setup_ui_called_=true;
    std::unique_ptr<LvglImage> preview_image_cached_;
    std::unique_ptr<Gif> gif_controller_;
    void SetPreviewImage(std::unique_ptr<LvglImage> image);
    void SetChatMessage(const char* role,const char* content);
    void SetHideSubtitle(bool hide);
};
'''
        harness += source[start:end]
        harness += source[source.index("void LcdDisplay::SetHideSubtitle(bool hide)"):]
        harness += r'''
int main() {
    LcdDisplay display;
    display.SetChatMessage("assistant", std::string(1000,'x').c_str());
    assert(!display.bottom.hidden);
    display.SetPreviewImage(std::make_unique<LvglImage>());
    assert(display.bottom.hidden); // 旧长字幕不能覆盖预览。
    assert(!display.preview.hidden);
    display.SetHideSubtitle(true);
    display.SetHideSubtitle(false);
    assert(display.bottom.hidden);
    display.SetChatMessage("assistant","new reply");
    assert(display.bottom.hidden); // 新到字幕也不能抢占预览。
    display.SetPreviewImage(nullptr);
    assert(!display.bottom.hidden && display.label.text=="new reply");
    display.SetPreviewImage(nullptr);
    assert(!display.bottom.hidden);
    display.SetPreviewImage(std::make_unique<LvglImage>());
    display.SetChatMessage("assistant", "");
    display.SetPreviewImage(nullptr);
    assert(display.bottom.hidden);
    display.hide_subtitle_=true;
    display.SetPreviewImage(std::make_unique<LvglImage>());
    display.SetChatMessage("assistant","hidden");
    display.SetPreviewImage(nullptr);
    assert(display.bottom.hidden);
}
'''
        with tempfile.TemporaryDirectory() as temp:
            binary = str(Path(temp) / "preview")
            subprocess.run(["g++", "-std=c++17", "-x", "c++", "-", "-o", binary],
                           input=harness, text=True, check=True, capture_output=True)
            result = subprocess.run([binary], text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
