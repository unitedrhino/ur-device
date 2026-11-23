// Watcher 页面与焦点管理：改编自 Seeed 官方 pm.h/pm.c，Apache-2.0。
// 修改：LVGL 9 接口、实例隔离、边界校验、逐页焦点索引及动画期输入互斥。
#pragma once

#include <lvgl.h>
#include <array>
#include <cstddef>
#include <cstdint>

namespace watcher {

// Page 保存由显示层持有的屏幕、可聚焦对象和最近焦点；初始化失败返回 false。
struct Page {
    static constexpr size_t kMaxObjects = 12;  // 延续官方页面焦点组容量上限。
    lv_obj_t* screen = nullptr;  // 页面根对象，由显示层负责销毁。
    std::array<lv_obj_t*, kMaxObjects> objects{};  // 当前页可聚焦对象，不允许跨页引用。
    size_t count = 0;  // 有效对象数量，不能超过 objects 容量。
    size_t focused = 0;  // 以索引记忆焦点，避免恢复已离开的对象指针。
    bool (*initialize)(Page&) = nullptr;  // 惰性初始化函数，成功时填充根对象及焦点组。
    lv_screen_load_anim_t transition = LV_SCR_LOAD_ANIM_FADE_IN;  // 页面指定过渡方向，复用对象避免透明大图层。
};

// PageManager 复用官方“惰性页面＋LVGL group＋过渡动画”，仅在显示线程内调用。
class PageManager {
public:
    PageManager() = default;
    ~PageManager();
    PageManager(const PageManager&) = delete;
    PageManager& operator=(const PageManager&) = delete;

    // Open 校验并打开页面；失败不改变当前页，动画完成前拒绝切页。
    bool Open(Page& page, uint32_t now, uint32_t duration_ms = 120);
    // Rotate 按方向移动当前页焦点；动画期间与无焦点页面返回 false。
    bool Rotate(bool clockwise, uint32_t now);
    // Confirm 返回当前焦点供调用方分发动作，动画期间返回 nullptr。
    lv_obj_t* Confirm(uint32_t now) const;
    // Current 返回当前页面的借用指针，不转移所有权。
    const Page* Current() const { return current_; }

private:
    // Validate 确认根、对象、层级与重复项，禁止把旧页对象加入新页焦点组。
    static bool Validate(const Page& page);
    // Busy 使用无符号差值处理系统毫秒计时回绕。
    bool Busy(uint32_t now) const;
    lv_group_t* group_ = nullptr;  // 管理器独占焦点组，首次切页时创建。
    Page* current_ = nullptr;  // 显示层必须保证页面记录在管理器存活期间有效。
    uint32_t transition_at_ = 0;  // 最近过渡开始时刻。
    uint32_t transition_ms_ = 0;  // 过渡期间拒绝旋转、确认和再次切页。
};

}  // namespace watcher
