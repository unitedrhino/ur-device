// Watcher皮肤与系统主题桥接：复用系统字体和表情资源，颜色由固定皮肤统一管理。
#pragma once
#include "lvgl_theme.h"
#include "skin_palette.h"

namespace watcher {
// SkinVariantTheme 让新增深色变体沿用旧dark存储名称，回退旧包时仍能安全加载。
class SkinVariantTheme : public LvglTheme {
public:
    explicit SkinVariantTheme(const char* name) : LvglTheme(name) {}
    std::string storage_name() const override { return "dark"; }  // 具体皮肤索引另存。
};

// PrepareSkinThemes 注册一次皮肤对象，资源重载时更新共享所有者，不重建显示页面。
inline void PrepareSkinThemes() {
    auto& manager = LvglThemeManager::GetInstance();  // 系统已先建立light/dark基础资源。
    for (const auto& skin : kSkins) {
        auto* resources = manager.GetTheme(skin.light ? "light" : "dark");  // 基础字体及表情。
        auto* theme = manager.GetTheme(skin.id);  // 已存在主题保持对象地址稳定。
        if (!resources)
            continue;
        if (!theme) {
            theme = new SkinVariantTheme(skin.id);
            manager.RegisterTheme(skin.id, theme);
        }
        theme->set_background_color(lv_color_hex(skin.background));
        theme->set_text_color(lv_color_hex(skin.text));
        theme->set_chat_background_color(lv_color_hex(skin.background));
        theme->set_user_bubble_color(lv_color_hex(skin.surface));
        theme->set_assistant_bubble_color(lv_color_hex(skin.surface));
        theme->set_system_bubble_color(lv_color_hex(skin.background));
        theme->set_system_text_color(lv_color_hex(skin.text));
        theme->set_border_color(lv_color_hex(skin.surface));
        theme->set_low_battery_color(lv_color_hex(0xd94040));
        theme->set_background_image(nullptr);
        theme->set_text_font(resources->text_font());
        theme->set_icon_font(resources->icon_font());
        theme->set_large_icon_font(resources->large_icon_font());
        theme->set_emoji_font(resources->emoji_font());
        theme->set_emoji_collection(resources->emoji_collection());
    }
}
}  // namespace watcher
