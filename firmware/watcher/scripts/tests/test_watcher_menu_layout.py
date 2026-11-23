"""使用实际 LVGL 与内置字体验证布局、长字幕跟随和生产触摸输入路由。"""
import subprocess
import os
import resource
import signal
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class WatcherMenuLayoutTest(unittest.TestCase):
    """真实布局引擎与字体检查坐标，不将主机布局当作物理LCD或触屏验收。"""

    def test_network_titles_do_not_cover_menu_rows(self):
        """覆盖全部标题、菜单完整行、长字幕拖动和真实指针；不替代物理触屏验收。"""
        lvgl = ROOT / 'managed_components/lvgl__lvgl'
        font = ROOT / 'managed_components/78__xiaozhi-fonts/src/font_noto_sans_basic_30_4.c'
        if not (lvgl / 'CMakeLists.txt').is_file() or not font.is_file():
            self.skipTest('需要IDF安装的实际LVGL与字体依赖；未验证实际布局')
        source = (ROOT / 'main/boards/sensecap-watcher/sensecap_watcher.cc').read_text()
        setup_begin = source.index('            menu_page_.screen = lv_obj_create(nullptr);')
        setup_end = source.index('            menu_page_.count = 7;', setup_begin)
        render_begin = source.index('        void RenderMenu()')
        render_end = source.index('        // PollMenu', render_begin)
        # 字幕复用同一次真实LVGL构建，提取生产布局和滚动策略而非模拟坐标。
        subtitle_begin = source.index('            lv_obj_set_size(bottom_bar_, 288, 144);')
        subtitle_end = source.index('            lv_obj_set_style_bg_color(container_', subtitle_begin)
        roles_begin = source.index('            lv_obj_set_flex_flow(bottom_bar_, LV_FLEX_FLOW_COLUMN);')
        roles_end = source.index('            lv_obj_set_size(low_battery_popup_', roles_begin)
        viewport_begin = source.index('        void RefreshSubtitleViewport(')
        viewport_end = source.index('        // ApplyPagePalette', viewport_begin)
        follow_begin = source.index('            const bool follow_assistant =')
        follow_end = source.index('            if (update.user_dirty)', follow_begin)
        code = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdlib>
#include <functional>
#include <deque>
#include <iostream>
#include <string>
#include "lvgl.h"
#include "ui/menu_model.h"
#include "ui/page_manager.h"
#include "ui/touch_gate.h"
extern "C" const lv_font_t font_noto_sans_basic_30_4;
// 字体与页面采用真实依赖；平台描述和页面切换仅隔离不相关硬件边界。
struct Font { const lv_font_t* font() { return &font_noto_sans_basic_30_4; } };
struct LvglTheme {
    Font font_; Font* text_font() { return &font_; }
    lv_color_t text_color() const { return lv_color_hex(0xffffff); }
};
struct Description { const char* version; };
const Description* esp_app_get_description() { static Description d{"2.8.1-rc.1"}; return &d; }
// 保留真实页面管理器及焦点组，避免仅换屏替身遗漏自动滚动对布局的影响。
using Page = watcher::Page;
struct CustomLcdDisplay {
    LvglTheme theme;
    LvglTheme* current_theme_ = &theme;
    Page home_page_, menu_page_;
    watcher::PageManager pages_;
    watcher::MenuModel menu_model_;
    lv_obj_t* menu_title_ = nullptr;
    lv_obj_t* menu_list_ = nullptr;
    std::array<lv_obj_t*, 7> menu_labels_{};
    bool charging_ = true, binding_flow_ = false;
    bool call_ui_active_ = false;  // 通话页占用屏幕期间冻结菜单渲染与输入。
    uint32_t touch_revision_ = 0;
    std::function<void(size_t, uint32_t)> touch_item_;
    struct { bool blocked = false; int stage = 0; uint8_t error = 0; } shown_network_;
    void ApplyPagePalette() {}
    void Setup() {
SETUP
        menu_page_.count = 7;
    }
RENDER
    bool Verify() {
        lv_obj_update_layout(menu_page_.screen);
        const int title_bottom = lv_obj_get_y(menu_title_) + lv_obj_get_height(menu_title_);
        const int list_top = lv_obj_get_y(menu_list_);
        std::cout << "title_bottom=" << title_bottom << " list_top=" << list_top
                  << " list_height=" << lv_obj_get_height(menu_list_) << std::endl;
        return list_top >= title_bottom && lv_obj_get_height(menu_list_) >= 45 &&
               list_top + lv_obj_get_height(menu_list_) <= 316;
    }
    bool VerifyVisibleMenuRows() {
        // 菜单只露出完整行；视口边缘半截文字会重复触发用户的“被遮挡”反馈。
        lv_obj_update_layout(menu_page_.screen);
        lv_area_t viewport;
        lv_obj_get_coords(menu_list_, &viewport);
        bool complete = lv_obj_get_scroll_y(menu_page_.screen) == 0;
        for (size_t i = 0; i < menu_model_.Count(); ++i) {
            lv_area_t row, label;
            lv_obj_get_coords(menu_page_.objects[i], &row);
            lv_obj_get_coords(menu_labels_[i], &label);
            if (row.y2 < viewport.y1 || row.y1 > viewport.y2) continue;
            const bool inside = label.y1 >= viewport.y1 && label.y2 <= viewport.y2;
            if (!inside) std::cout << "partial menu row=" << i << " label=" << label.y1 << ":"
                                  << label.y2 << " viewport=" << viewport.y1 << ":" << viewport.y2 << std::endl;
            complete = inside && complete;
        }
        return complete;
    }
};
// 仅隔离基类和硬件创建；字体、角色布局、滚动范围和生产策略均由真实LVGL执行。
struct SubtitleDisplay {
    lv_obj_t* bottom_bar_;
    lv_obj_t* chat_message_label_;
    lv_obj_t* user_subtitle_;
    bool has_user_ = false, has_assistant_ = false;
    bool preview_image_cached_ = false, hide_subtitle_ = false;
    struct { bool assistant_dirty = true; std::string assistant = "增量"; } update;
    void Setup() {
        auto* screen = lv_screen_active();
        lv_obj_set_style_text_font(screen, &font_noto_sans_basic_30_4, 0);
        bottom_bar_ = lv_obj_create(screen);
        lv_obj_set_style_border_width(bottom_bar_, 0, 0);
        lv_obj_set_style_radius(bottom_bar_, 0, 0);
        chat_message_label_ = lv_label_create(bottom_bar_);
SUBTITLE_SETUP
    }
VIEWPORT
    bool Follow() {
FOLLOW
        return follow_assistant;
    }
    void Verify() {
        // 使用跨多屏实际汉字长文，首段隐藏空用户，尾部更新跟随，人工上滑不抢回。
        std::string text = "联犀：\n";
        for (int i = 0; i < 20; ++i) text += "这是完整的长回复文字。";
        has_assistant_ = true;
        lv_label_set_text(chat_message_label_, text.c_str());
        RefreshSubtitleViewport(false, true);
        assert(lv_obj_has_flag(user_subtitle_, LV_OBJ_FLAG_HIDDEN));
        assert(lv_obj_get_y(bottom_bar_) == 140 && lv_obj_get_height(bottom_bar_) == 144);
        assert(lv_obj_get_scroll_y(bottom_bar_) > 144);
        assert(lv_obj_get_scroll_bottom(bottom_bar_) <= 2);
        assert(Follow());
        lv_obj_scroll_to_y(bottom_bar_, 43, LV_ANIM_OFF);
        const int reading_position = lv_obj_get_scroll_y(bottom_bar_);
        assert(!Follow());
        text += "新的增量不会打断阅读。";
        lv_label_set_text(chat_message_label_, text.c_str());
        RefreshSubtitleViewport(false, false);
        assert(lv_obj_get_scroll_y(bottom_bar_) == reading_position);
        has_user_ = true;
        lv_label_set_text(user_subtitle_, "你：你好，请继续。");
        RefreshSubtitleViewport(true, false);
        assert(lv_obj_get_scroll_y(bottom_bar_) == 0);
        assert(!lv_obj_has_flag(user_subtitle_, LV_OBJ_FLAG_HIDDEN));
        preview_image_cached_ = true;
        RefreshSubtitleViewport(false, false);
        assert(lv_obj_has_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN));
        preview_image_cached_ = false;
        RefreshSubtitleViewport(false, true);
        assert(!lv_obj_has_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN));
        assert(lv_obj_get_scroll_bottom(bottom_bar_) <= 2);
        lv_obj_delete(bottom_bar_);
    }
};
int main() {
    lv_init();
    auto* display = lv_display_create(412, 412);
    bool passed = true;
    {
    // 页面管理器必须先于LVGL反初始化析构，保持焦点组的真实生命周期。
    CustomLcdDisplay board;
    board.home_page_.screen = lv_screen_active();
    board.Setup();
    // 逐个经过七个焦点并返回首页，验证首尾及跨页恢复，不借新窗口覆盖失败位置。
    for (int round = 0; round < 10; ++round) {
        board.menu_model_.Handle(watcher::MenuInput::LongPress, lv_tick_get());
        assert(board.menu_model_.Page() == watcher::MenuPage::Main);
        for (size_t item = 0; item < 7; ++item) {
            board.RenderMenu();
            passed = board.VerifyVisibleMenuRows() && passed;
            board.menu_model_.Handle(watcher::MenuInput::Next, lv_tick_get());
        }
        board.menu_model_.Handle(watcher::MenuInput::LongPress, lv_tick_get());
        board.RenderMenu();
    }
    for (int round = 0; round < 10; ++round) {
        for (bool binding : {false, true}) {
            for (int error : {0, 1, 2, 3, 4, 5, 6, 255}) {
                board.binding_flow_ = binding;
                board.shown_network_.stage = error ? 8 : 7;
                board.shown_network_.error = error;
                board.menu_model_.ShowNetworkStatus(board.shown_network_.stage, false, lv_tick_get());
                board.RenderMenu();
                passed = board.Verify() && passed;
                passed = board.VerifyVisibleMenuRows() && passed;
            }
        }
    }
    if (passed) std::cout << "PASS actual LVGL whole menu rows rounds=10" << std::endl;
    lv_screen_load(board.home_page_.screen);
    for (int round = 0; round < 10; ++round) {
        SubtitleDisplay subtitles;
        subtitles.Setup();
        subtitles.Verify();
    }
    touch_e2e::Run(display, std::getenv("WATCHER_TOUCH_REPLAY_DROP_CLICK") != nullptr);
    }
    lv_display_delete(display);
    lv_deinit();
    return passed ? 0 : 1;
}
'''.replace('SUBTITLE_SETUP', source[subtitle_begin:subtitle_end] + source[roles_begin:roles_end]) \
   .replace('VIEWPORT', source[viewport_begin:viewport_end]) \
   .replace('FOLLOW', source[follow_begin:follow_end]) \
   .replace('SETUP', source[setup_begin:setup_end]).replace('RENDER', source[render_begin:render_end])
        # 用同一实际LVGL构建追加指针回放；不直接发点击事件绕过命中和手势处理。
        fixture = (ROOT / 'scripts/tests/cpp/watcher_touch_lvgl_test.inc').read_text()
        event_begin = source.index('        struct MenuEvent {')
        event_end = source.index('        QueueHandle_t menu_events_', event_begin)
        touch_begin = source.index('        void AttachTouch(')
        touch_end = source.index('        // SetTheme', touch_begin)
        menu_begin = source.index('        void PollMenu()')
        menu_end = source.index('    public:', menu_begin)
        palette_begin = source.index('        void ApplyPagePalette()')
        palette_end = source.index('        // RenderMenu', palette_begin)
        overlay_begin = source.index('            lv_obj_set_size(top_bar_, 230, 35);')
        overlay_end = source.index('            lv_obj_set_size(bottom_bar_, 288, 144);', overlay_begin)
        fixture = fixture.replace('EVENT', source[event_begin:event_end]).replace('METHODS',
            source[palette_begin:palette_end] + source[render_begin:render_end] +
            source[menu_begin:menu_end] + source[touch_begin:touch_end]) \
            .replace('OVERLAY', source[overlay_begin:overlay_end])
        code = code.replace('int main() {', fixture + '\nint main() {')
        # 所有生成文件放任务临时区，不写vendor、不自建Go/pnpm缓存；最多4个编译并发。
        temporary_root = ROOT.parents[1] / '.temp'
        temporary_root.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='watcher-menu-lvgl-', dir=temporary_root) as directory:
            folder = Path(directory)
            (folder / 'layout.cc').write_text(code)
            (folder / 'lv_conf.h').write_text('#define LV_COLOR_DEPTH 16\n#define LV_USE_OS 0\n')
            (folder / 'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.16)
project(watcher_menu_layout C CXX)
set(CMAKE_CXX_STANDARD 17)
set(LV_BUILD_CONF_PATH "{folder / 'lv_conf.h'}" CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
add_subdirectory("{lvgl}" lvgl-build)
add_executable(watcher-menu layout.cc "{font}" "{ROOT / 'main/boards/sensecap-watcher/watcher_page_manager.cc'}")
target_include_directories(watcher-menu PRIVATE "{ROOT / 'main/boards/sensecap-watcher'}")
target_link_libraries(watcher-menu PRIVATE lvgl m)
''')
            configure = subprocess.run(['cmake', '-S', str(folder), '-B', str(folder / 'build')],
                                       capture_output=True, text=True)
            self.assertEqual(configure.returncode, 0, configure.stdout + configure.stderr)
            build = subprocess.run(['cmake', '--build', str(folder / 'build'), '--parallel', '4'],
                                   capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout[-6000:] + build.stderr[-6000:])
            result = subprocess.run([str(folder / 'build/watcher-menu')], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('PASS actual LVGL whole menu rows rounds=10', result.stdout)
            self.assertIn('PASS actual LVGL touch routing rounds=10', result.stdout)
            self.assertIn('PASS actual LVGL subtitle pointer scrolling rounds=10', result.stdout)
            # 负例只阻断硬件边界后的事件入队，不改生产实现；验证链路确实能发现菜单不动。
            negative_env = dict(os.environ, WATCHER_TOUCH_REPLAY_DROP_CLICK='1')
            negative = subprocess.run([str(folder / 'build/watcher-menu')],
                capture_output=True, text=True, env=negative_env, cwd=folder,
                preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)), timeout=30)
            self.assertEqual(negative.returncode, -signal.SIGABRT, negative.stdout + negative.stderr)
            self.assertIn('board.menu_model_.Page() == watcher::MenuPage::Main', negative.stderr)
            # 仅移除硬件移动采样：真实LVGL不得把按下/抬手当作字幕已经滚动。
            no_move_env = dict(os.environ, WATCHER_SUBTITLE_REPLAY_DROP_MOVE='1')
            no_move = subprocess.run([str(folder / 'build/watcher-menu')],
                capture_output=True, text=True, env=no_move_env, cwd=folder,
                preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)), timeout=30)
            self.assertEqual(no_move.returncode, -signal.SIGABRT, no_move.stdout + no_move.stderr)
            self.assertIn('reading_position < end_position', no_move.stderr)
