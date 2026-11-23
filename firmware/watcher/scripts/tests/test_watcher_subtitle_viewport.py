"""执行 Watcher 字幕视口生产函数，验证空角色、预览、滚动跟随与人工阅读隔离。"""
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class WatcherSubtitleViewportTest(unittest.TestCase):
    """LVGL 边界只记录可见性与滚动调用，不用几何替身冒充像素或触屏验收。"""

    def test_empty_roles_and_following_do_not_hide_reply(self):
        """单角色不占空行；新回复/尾部增量跟随，上滑阅读与预览不会被抢焦点。"""
        source = (ROOT / "main/boards/sensecap-watcher/sensecap_watcher.cc").read_text()
        begin = source.index("        void RefreshSubtitleViewport(")
        end = source.index("        // ApplyPagePalette", begin)
        follow_begin = source.index("            const bool follow_assistant =")
        follow_end = source.index("            if (update.user_dirty)", follow_begin)
        code = r'''
#include <cassert>
#include <string>
#include <utility>
constexpr int LV_OBJ_FLAG_HIDDEN = 1, LV_ALIGN_TOP_MID = 2, LV_ANIM_OFF = 0, LV_COORD_MAX = 32767;
struct lv_obj_t { bool hidden = false; int y = 0, scroll = -1, layouts = 0, remaining = 0; };
void lv_obj_add_flag(lv_obj_t* obj, int) { obj->hidden = true; }
void lv_obj_remove_flag(lv_obj_t* obj, int) { obj->hidden = false; }
void lv_obj_align(lv_obj_t* obj, int, int, int y) { obj->y = y; }
void lv_obj_update_layout(lv_obj_t* obj) { ++obj->layouts; }
void lv_obj_scroll_to_y(lv_obj_t* obj, int y, int) { assert(obj->layouts > 0); obj->scroll = y; }
int lv_obj_get_scroll_bottom(lv_obj_t* obj) { return obj->remaining; }
struct Update { bool assistant_dirty; std::string assistant; };
// 使用显示任务的真实判断表达式，覆盖首段、尾部增量和人工上滑边界。
bool should_follow(const Update& update, bool has_assistant_, lv_obj_t* bottom_bar_) {
FOLLOW
    return follow_assistant;
}
struct Display {
    lv_obj_t user, assistant, viewport;
    lv_obj_t* user_subtitle_ = &user;
    lv_obj_t* chat_message_label_ = &assistant;
    lv_obj_t* bottom_bar_ = &viewport;
    bool has_user_ = false, has_assistant_ = false, hide_subtitle_ = false, preview_image_cached_ = false;
METHOD
};
int main() {
    Display display;
    display.viewport.remaining = 40;
    assert(should_follow({true, "reply"}, false, &display.viewport));
    assert(!should_follow({true, "reply"}, true, &display.viewport));
    display.viewport.remaining = 2;
    assert(should_follow({true, "reply"}, true, &display.viewport));
    assert(!should_follow({true, ""}, true, &display.viewport));
    assert(!should_follow({false, "reply"}, true, &display.viewport));
    display.RefreshSubtitleViewport(false, false);
    assert(display.user.hidden && display.assistant.hidden && display.viewport.hidden);
    display.has_assistant_ = true;
    display.RefreshSubtitleViewport(false, true);
    assert(display.user.hidden && !display.assistant.hidden && !display.viewport.hidden);
    assert(display.viewport.scroll == LV_COORD_MAX && display.viewport.y == 140);
    display.has_user_ = true;
    display.viewport.scroll = 20; // 用户上滑后的实际阅读位置。
    display.RefreshSubtitleViewport(false, false);
    assert(!display.user.hidden && !display.assistant.hidden && display.viewport.scroll == 20);
    display.has_assistant_ = false;
    display.RefreshSubtitleViewport(true, false);
    assert(display.assistant.hidden && display.viewport.scroll == 0);
    display.preview_image_cached_ = true;
    const int layouts = display.viewport.layouts;
    display.RefreshSubtitleViewport(false, true);
    assert(display.viewport.hidden && display.viewport.layouts == layouts);
    display.preview_image_cached_ = false;
    display.has_assistant_ = true;
    display.RefreshSubtitleViewport(false, true);
    assert(!display.viewport.hidden && display.viewport.scroll == LV_COORD_MAX);
    display.hide_subtitle_ = true;
    display.RefreshSubtitleViewport(true, true);
    assert(display.viewport.hidden);
}
'''.replace("METHOD", source[begin:end]).replace("FOLLOW", source[follow_begin:follow_end])
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / "watcher-subtitles")
            result = subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                                     "-x", "c++", "-", "-o", binary], input=code,
                                    text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            for _ in range(10):
                result = subprocess.run([binary], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
