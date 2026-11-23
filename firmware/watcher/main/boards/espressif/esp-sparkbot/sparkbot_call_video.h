// 文件说明：坦克通话画面交接；解码线程只拷贝一帧，LVGL 对象由应用任务维护。
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>

#include "application.h"
#include "display.h"
#include "lvgl_image.h"

namespace sparkbot {

// CallVideoView 将临时 RGB565 帧复制到有界槽，代次隔离停流和新会话。
class CallVideoView {
public:
    explicit CallVideoView(Display* display) : display_(display) {}

    // SetActive 切换画面接收；停流立即作废在途帧并排队清屏。
    void SetActive(bool active) {
        std::uint64_t generation;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (accepting_ == active) return;
            accepting_ = active;
            generation = ++generation_;
            pending_.reset();
        }
        if (!active) ScheduleClear(generation);
    }

    // Submit 仅在回调期间借用输入；空帧表示清除当前画面而不结束通话。
    void Submit(const std::uint8_t* rgb, int width, int height) {
        if (!rgb) {
            std::uint64_t generation;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                generation = ++generation_;
                pending_.reset();
            }
            ScheduleClear(generation);
            return;
        }
        if (width <= 0 || height <= 0 || width > 240 || height > 240) return;
        std::uint64_t generation;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!accepting_) return;
            generation = generation_;
        }
        auto frame = std::make_unique<LvglAllocatedImage>();
        if (!frame->CopyFrom(rgb, static_cast<size_t>(width) * height * 2,
                             width, height, width * 2, LV_COLOR_FORMAT_RGB565)) return;
        bool schedule = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!accepting_ || generation != generation_) return;
            pending_ = std::move(frame);
            pending_generation_ = generation;
            if (!scheduled_) { scheduled_ = true; schedule = true; }
        }
        if (schedule) Application::GetInstance().Schedule([this] { Drain(); });
    }

private:
    // ScheduleClear 只清理同一代画面，迟到的清屏不能遮住之后的新帧。
    void ScheduleClear(std::uint64_t generation) {
        Application::GetInstance().Schedule([this, generation] {
            DisplayLockGuard display_lock(display_);
            if (!display_lock) return;
            std::lock_guard<std::mutex> lock(mutex_);
            if (generation_ != generation || displayed_generation_ >= generation) return;
            if (image_) {
                lv_image_set_src(image_, nullptr);
                lv_obj_add_flag(image_, LV_OBJ_FLAG_HIDDEN);
            }
            current_.reset();
        });
    }

    // Drain 在主任务上提交最新一帧，并在锁内复核代次以防止旧帧复活。
    void Drain() {
        DisplayLockGuard display_lock(display_);
        std::lock_guard<std::mutex> lock(mutex_);
        scheduled_ = false;
        if (!display_lock || !pending_ || !accepting_ || pending_generation_ != generation_) {
            pending_.reset();
            return;
        }
        if (!image_) {
            image_ = lv_image_create(lv_screen_active());
            lv_obj_center(image_);
        }
        lv_image_set_src(image_, pending_->image_dsc());
        current_ = std::move(pending_);
        displayed_generation_ = generation_;
        lv_obj_center(image_);
        lv_obj_remove_flag(image_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(image_);
    }

    Display* display_;                               // 生命周期由板级保证长于本视图。
    std::mutex mutex_;                               // 保护代次、槽及显示图像所有权。
    bool accepting_ = false;                         // 当前通话是否允许接收画面。
    bool scheduled_ = false;                         // 最多一个主任务投递在途。
    std::uint64_t generation_ = 1;                   // 停流和清屏时递增。
    std::uint64_t pending_generation_ = 0;           // 待显示帧所属代次。
    std::uint64_t displayed_generation_ = 0;         // 已显示帧所属代次。
    std::unique_ptr<LvglAllocatedImage> pending_;      // 可被新帧覆盖的唯一待显示槽。
    std::unique_ptr<LvglAllocatedImage> current_;      // LVGL 正在引用的帧。
    lv_obj_t* image_ = nullptr;                       // 主任务及显示锁内访问。
};

}  // namespace sparkbot
