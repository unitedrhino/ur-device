"""回放生产字体替换入口，验证隐藏菜单和状态层不会保存已释放字体。"""
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def method(source, signature):
    """按成对花括号提取生产方法；无 Watcher 覆盖时保留原父类行为以复现缺陷。"""
    if signature not in source:
        return ""
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class WatcherFontLifetimeTest(unittest.TestCase):
    """只替换 LVGL 与主题容器边界；执行真实 SetTextFont 和 Watcher SetTheme。"""

    def test_replace_font_rebinds_all_screens(self):
        """主页或菜单处于活动状态时，字体替换后两屏和独立状态层均使用新字体。"""
        board = (ROOT / "main/boards/sensecap-watcher/sensecap_watcher.cc").read_text()
        display = (ROOT / "main/display/lvgl_display/lvgl_display.cc").read_text()
        replace = method(display, "bool LvglDisplay::SetTextFont(").replace("LvglDisplay::", "")
        override = method(board, "void SetTheme(Theme* theme) override")
        self.assertIn("ApplyPagePalette();", override)
        render = method(board, "void RenderMenu()")
        self.assertLess(render.index("ApplyPagePalette();"), render.index("if (page =="))
        # 执行页面配色生产方法；缺失时保留父类颜色，以复现暗色菜单黑字。
        palette = method(board, "void ApplyPagePalette()") or "void ApplyPagePalette() {}"
        harness = r'''
#include <cassert>
#include <cstdint>
#include <memory>
#include <string>
#include "ui/skin_palette.h"
namespace watcher { void PrepareSkinThemes() {} }
struct lv_font_t {};
static lv_font_t lv_font_montserrat_48;
struct LvglFont { lv_font_t value; const lv_font_t* font() const { return &value; } };
struct Theme { virtual ~Theme() = default; std::string label="light"; std::string name() const {return label;} };
struct LvglTheme : Theme {
    int color = 0;
    int text_color() const { return color; }
    std::shared_ptr<LvglFont> owner;
    auto text_font() { return owner; }
    std::shared_ptr<int> background_image() {return nullptr;}
    auto large_icon_font() { return owner; }
    void set_text_font(std::shared_ptr<LvglFont> font) { owner = font; }
};
struct LvglThemeManager {
    LvglTheme light, dark;
    static auto& GetInstance() { static LvglThemeManager manager; return manager; }
    LvglTheme* GetTheme(const char* name) { return std::string(name) == "light" ? &light : &dark; }
};
struct lv_obj_t { const lv_font_t* font = nullptr; int color = 0; int opacity = 50; bool hidden = false; };
constexpr int LV_OBJ_FLAG_HIDDEN = 1;
void lv_obj_add_flag(lv_obj_t* obj, int) { obj->hidden = true; }
void lv_obj_remove_flag(lv_obj_t* obj, int) { obj->hidden = false; }
int lv_color_hex(int color) { return color; }
constexpr int LV_OPA_TRANSP = 0;
void lv_obj_set_style_text_color(lv_obj_t* obj, int color, int) { obj->color = color; }
void lv_obj_set_style_bg_opa(lv_obj_t* obj, int opacity, int) { obj->opacity = opacity; }
namespace watcher { enum class MenuPage { Home, Menu, NetworkResult }; }
struct lv_display_t { lv_obj_t top; };
lv_obj_t* lv_display_get_layer_top(lv_display_t* display) { return &display->top; }
void lv_obj_set_style_text_font(lv_obj_t* obj, const lv_font_t* font, int) { assert(obj); obj->font = font; }
struct Settings { Settings(const char*,bool) {} void SetInt(const char*,int) {} };
struct DisplayLockGuard { explicit DisplayLockGuard(void*) {} };
struct SpiLcdDisplay {
    Theme* current_theme_ = nullptr;
    lv_obj_t* active = nullptr;
    virtual ~SpiLcdDisplay() = default;
    virtual void SetTheme(Theme* theme) {
        current_theme_ = theme;
        active->font = static_cast<LvglTheme*>(theme)->text_font()->font();
    }
REPLACE
};
struct CustomLcdDisplay : SpiLcdDisplay {
    struct Menu { watcher::MenuPage page = watcher::MenuPage::Home; auto Page() const { return page; } } menu_model_;
    lv_obj_t network, status, notification, mute, battery, bar, status_bar;
    lv_obj_t* status_bar_ = &status_bar;
    lv_obj_t *network_label_ = &network, *status_label_ = &status, *notification_label_ = &notification;
    lv_obj_t *mute_label_ = &mute, *battery_label_ = &battery, *top_bar_ = &bar;
    struct Page { lv_obj_t* screen = nullptr; } home_page_, menu_page_;
    lv_display_t* display_ = nullptr;
    bool call_ui_active_=false,home_surface_=true;
    uint32_t notification_ms_=0;
    std::unique_ptr<LvglFont> menu_small_font_=std::make_unique<LvglFont>();
    // 夹具隔离LVGL对象布局，保留新视图字体绑定以验证旧主题能被释放。
    struct Dial {
        const lv_font_t* primary=nullptr;
        bool Screen() const {return true;}
        void SetFonts(const lv_font_t* p,const lv_font_t*,const lv_font_t*,const lv_font_t*,bool force) {assert(force);primary=p;}
    } dial_view_;
    void RenderMenu() {}
    void ApplySkinColors() {}
    void LayoutStatusBar(bool) {}
    void LayoutStatusText(bool) {}
OVERRIDE
PALETTE
};
int main() {
    for (int selected = 0; selected < 2; ++selected) {
        auto& manager = LvglThemeManager::GetInstance();
        auto original = std::make_shared<LvglFont>();
        std::weak_ptr<LvglFont> previous = original;
        manager.light.owner = original; manager.dark.owner = original;
        lv_obj_t home{original->font()}, menu{original->font()};
        lv_display_t screen{{original->font()}};
        CustomLcdDisplay display;
        display.home_page_.screen = &home; display.menu_page_.screen = &menu; display.display_ = &screen;
        display.current_theme_ = &manager.light; display.active = selected ? &menu : &home;
        original.reset();
        for (int iteration = 0; iteration < 10; ++iteration) {
            auto replacement = std::make_shared<LvglFont>();
            assert(display.SetTextFont(replacement));
            assert(previous.expired());
            assert(home.font == replacement->font());
            assert(menu.font == replacement->font());
            assert(display.dial_view_.primary == replacement->font());
            assert(screen.top.font == replacement->font());
            previous = replacement;
        }
        assert(!display.SetTextFont(nullptr));
        for (const char* theme_name : {"dark", "watcher-blue", "light"}) {
            manager.light.label = theme_name;
            for (auto page : {watcher::MenuPage::Home, watcher::MenuPage::Menu, watcher::MenuPage::NetworkResult, watcher::MenuPage::Home}) {
                display.menu_model_.page = page;
                display.ApplyPagePalette();
                int expected = watcher::SkinFor(theme_name).muted;
                for (auto* label : {display.network_label_, display.status_label_, display.notification_label_, display.mute_label_, display.battery_label_})
                    assert(label->color == expected);
                assert(display.top_bar_->opacity == LV_OPA_TRANSP);
                assert(display.status_bar_->hidden == (page != watcher::MenuPage::Home));
            }
        }
    }
}
'''.replace("REPLACE", replace).replace("OVERRIDE", override).replace("PALETTE", palette)
        with tempfile.TemporaryDirectory() as temporary:
            binary = str(Path(temporary) / "font-lifetime")
            subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            "-I",str(ROOT / "main/boards/sensecap-watcher"),
                            "-x", "c++", "-", "-o", binary], input=harness, text=True,
                           capture_output=True, check=True)
            result = subprocess.run([binary], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
