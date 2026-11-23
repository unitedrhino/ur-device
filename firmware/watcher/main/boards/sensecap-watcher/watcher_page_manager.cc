// Watcher 页面管理：改编自 Seeed-Studio/SenseCAP-Watcher-Firmware 的 pm.c。
// 原始提交 8e37f7ccd3d836bb99f483e0f6e74e8252406644，Apache-2.0，见 ui/SEEED-LICENSE。
// 修改：去除官方业务页面依赖，迁移 LVGL 9，修正初始化顺序及跨页焦点恢复。
#include "ui/page_manager.h"

namespace watcher {

PageManager::~PageManager() {
    if (group_) lv_group_delete(group_);
}

bool PageManager::Validate(const Page& page) {
    if (!page.screen || !lv_obj_is_valid(page.screen) || page.count > Page::kMaxObjects) return false;
    for (size_t i = 0; i < page.count; ++i) {
        auto* object = page.objects[i];
        if (!object || !lv_obj_is_valid(object)) return false;
        auto* parent = object;
        while (parent && parent != page.screen) parent = lv_obj_get_parent(parent);
        if (!parent) return false;
        for (size_t j = 0; j < i; ++j) if (page.objects[j] == object) return false;
    }
    return true;
}

bool PageManager::Busy(uint32_t now) const {
    return static_cast<uint32_t>(now - transition_at_) < transition_ms_;
}

bool PageManager::Open(Page& page, uint32_t now, uint32_t duration_ms) {
    if (Busy(now) || duration_ms > 1000) return false;
    if (!page.screen && (!page.initialize || !page.initialize(page))) return false;
    if (!Validate(page)) return false;
    if (current_ == &page) {
        // 通话页独占屏幕后需重新收回同一菜单实例，不能把缓存指针当成当前屏幕。
        if (lv_display_get_screen_active(lv_obj_get_display(page.screen)) != page.screen) {
            transition_at_ = now;
            transition_ms_ = duration_ms;
            lv_screen_load_anim(page.screen, page.transition, duration_ms, 0, false);
        }
        return true;
    }
    if (!group_) group_ = lv_group_create();
    if (!group_) return false;
    // 官方实现保存对象指针；这里保存索引，返回该页时只恢复该页有效对象。
    if (current_) {
        auto* focused = lv_group_get_focused(group_);
        for (size_t i = 0; i < current_->count; ++i) {
            if (current_->objects[i] == focused) current_->focused = i;
        }
    }
    lv_group_remove_all_objs(group_);
    for (size_t i = 0; i < page.count; ++i) lv_group_add_obj(group_, page.objects[i]);
    if (page.count) {
        if (page.focused >= page.count) page.focused = 0;
        lv_group_focus_obj(page.objects[page.focused]);
    }
    current_ = &page;
    transition_at_ = now;
    transition_ms_ = duration_ms;
    // 与官方一样复用页面，不在动画结束时自动删除旧屏，避免反复创建大对象。
    lv_screen_load_anim(page.screen, page.transition, duration_ms, 0, false);
    return true;
}

bool PageManager::Rotate(bool clockwise, uint32_t now) {
    if (Busy(now) || !current_ || !current_->count) return false;
    if (clockwise) lv_group_focus_next(group_);
    else lv_group_focus_prev(group_);
    return true;
}

lv_obj_t* PageManager::Confirm(uint32_t now) const {
    if (Busy(now) || !group_ || !current_ || !current_->count) return nullptr;
    return lv_group_get_focused(group_);
}

}  // namespace watcher
