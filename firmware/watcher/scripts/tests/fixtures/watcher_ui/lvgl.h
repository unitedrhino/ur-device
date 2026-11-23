// 最小 LVGL 行为替身：仅覆盖生产页面管理器所使用的 API，不模拟实际渲染性能。
#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

struct lv_group_t;
// 测试对象保存父子关系、有效性和组归属。
struct lv_obj_t { lv_obj_t* parent = nullptr; bool valid = true; lv_group_t* group = nullptr; };
// 测试焦点组模拟 LVGL 的顺序导航。
struct lv_group_t { std::vector<lv_obj_t*> objects; size_t focus = 0; };
// 动画枚举与可观察屏幕状态用于验证惰性初始化与页面生命周期。
enum lv_screen_load_anim_t { LV_SCR_LOAD_ANIM_FADE_IN, LV_SCR_LOAD_ANIM_MOVE_LEFT, LV_SCR_LOAD_ANIM_MOVE_RIGHT };
inline lv_obj_t* loaded_screen = nullptr;
inline unsigned loads = 0;
inline bool lv_obj_is_valid(lv_obj_t* p) { return p && p->valid; }
inline lv_obj_t* lv_obj_get_parent(lv_obj_t* p) { return p->parent; }
inline lv_group_t* lv_group_create() { return new lv_group_t; }
inline void lv_group_remove_all_objs(lv_group_t* g) {
    for (auto* p : g->objects) p->group = nullptr;
    g->objects.clear(); g->focus = 0;
}
inline void lv_group_delete(lv_group_t* g) { lv_group_remove_all_objs(g); delete g; }
inline void lv_group_add_obj(lv_group_t* g, lv_obj_t* p) { g->objects.push_back(p); p->group = g; }
inline lv_obj_t* lv_group_get_focused(lv_group_t* g) {
    return g->objects.empty() ? nullptr : g->objects[g->focus];
}
inline void lv_group_focus_obj(lv_obj_t* p) {
    auto* g = p->group;
    if (g) g->focus = std::find(g->objects.begin(), g->objects.end(), p) - g->objects.begin();
}
inline void lv_group_focus_next(lv_group_t* g) { g->focus = (g->focus + 1) % g->objects.size(); }
inline void lv_group_focus_prev(lv_group_t* g) { g->focus = (g->focus + g->objects.size() - 1) % g->objects.size(); }
inline void lv_screen_load_anim(lv_obj_t* p, lv_screen_load_anim_t, uint32_t, uint32_t, bool) {
    loaded_screen = p; ++loads;
}

// 模拟同一显示器上的独占通话屏，以验证缓存页重新取回屏幕所有权。
inline void* lv_obj_get_display(lv_obj_t*) { return nullptr; }
inline lv_obj_t* lv_display_get_screen_active(void*) { return loaded_screen; }
