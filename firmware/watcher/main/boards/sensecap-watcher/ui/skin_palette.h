// Watcher统一皮肤配色：表盘、聆听、菜单和通话共用，主题名称沿用既有持久化入口。
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace watcher {
// SkinPalette 保存不依赖字体和LVGL对象的色值，主题资源更新不会改变用户选择。
struct SkinPalette {
    const char* id;       // 主题管理器中注册的稳定名称。
    const char* title;    // 设备菜单显示名称。
    bool light;           // 从系统浅色或深色主题复用资源。
    uint32_t background;  // 全部页面的统一背景。
    uint32_t text;        // 正文与状态标题。
    uint32_t surface;     // 普通按钮及字幕面板。
    uint32_t accent;      // 选中项及进度条。
    uint32_t on_accent;   // 选中背景上的文字。
    uint32_t muted;       // 辅助说明与状态图标。
};
// kSkins 用固定名称保持老light/dark调用兼容，三套字体资源均复用原主题。
inline constexpr std::array<SkinPalette, 3> kSkins{{
    {"dark", "暗夜绿", false, 0x080b09, 0xf0f4ec, 0x19221b, 0xa9de2c, 0x11190c, 0xc3cfc2},
    {"watcher-blue", "深海蓝", false, 0x091523, 0xecf3ff, 0x17293d, 0x69b8ff, 0x0b2032, 0xb5c8dc},
    {"light", "暖白", true, 0xf5f3ec, 0x242c25, 0xe6e9dd, 0x376b47, 0xf7f8f0, 0x566457},
}};
// SkinIndex 返回已注册名称的索引，未知名称回到暗夜绿，避免启动时主题为空。
inline size_t SkinIndex(std::string_view name) {
    for (size_t index = 0; index < kSkins.size(); ++index)
        if (name == kSkins[index].id)
            return index;
    return 0;
}
// SkinFor 根据名称返回进程期固定配色，不产生额外字体或图片分配。
inline const SkinPalette& SkinFor(std::string_view name) { return kSkins[SkinIndex(name)]; }
// BootSkin 一次迁移旧白底，之后恢复皮肤索引，并兼容回退旧包后修改的深浅主题。
inline const char* BootSkin(std::string_view stored, int migration_version, int stored_skin = -1) {
    if (migration_version < 1)
        return kSkins[0].id;
    if (stored_skin >= 0 && stored_skin < static_cast<int>(kSkins.size())) {
        const auto& selected = kSkins[stored_skin];  // 变体与基础主题不一致时优先采用旧包的新选择。
        if (stored == (selected.light ? "light" : "dark"))
            return selected.id;
    }
    return SkinFor(stored).id;
}
}  // namespace watcher
