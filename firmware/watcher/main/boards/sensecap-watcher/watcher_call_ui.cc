/* Watcher 设备端语音/视频通话界面实现：微信式来电/通话页面。
 * 线程模型：
 *  1) 信令任务 on_state/on_track_state 与解码任务 on_remote_frame 只拷贝快照，
 *     经 Application::Schedule 转主任务；
 *  2) 主任务持有显示锁（DisplayLockGuard）执行全部 LVGL 变更；
 *  3) 视频帧经互斥槽合并为最新一帧，用原子标志保证同一时刻至多一次上屏
 *     在途，避免高频帧淹没主任务队列；
 *  4) 通话结束提示 1.5 秒后由 LVGL 一次性定时器收回页面并归位主页。 */
#include "sdkconfig.h"

#include "watcher_call_ui.h"

#if CONFIG_UR_AI_SIGNALING_WEBRTC

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <material_symbols.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

#include "application.h"
#include "lvgl_theme.h"
#include "ui/skin_palette.h"

#define TAG "watcher_call"

namespace watcher {

namespace {

/* 圆屏 412x412 通话页布局常量：控件中心坐标均相对屏幕中心系。 */
constexpr int kScreenSize = 412;
/* 来电页：标题在中心上方，接听/拒接钮靠近中心，文字在钮下方。
 * 这些值均为相对屏幕中心的偏移；原正值叠加中心后落到底部屏幕外，
 * 导致真机来电页看不到接听/拒接按钮（412 圆屏实测）。 */
constexpr int kRingTitleY = -96;
constexpr int kRingButtonY = 12;
constexpr int kRingButtonSize = 96;
constexpr int kRingButtonSpan = 86;  // 接听/拒接钮距屏幕中心的水平偏移。
constexpr int kRingHintY = 78;
/* 通话页：控制钮在中心下方，状态文字在中心上方。 */
constexpr int kControlY = 118;
constexpr int kHangupButtonSize = 76;
constexpr int kTrackButtonSize = 64;
constexpr int kTrackButtonSpan = 96;  // 麦克风/摄像头钮外移，避免与中央挂断钮重叠。
constexpr int kStatusY = -116;
/* 通话结束后提示停留时长。 */
constexpr uint32_t kEndedLingerMs = 1500;
/* 通话期间限频唤醒息屏计时的间隔。 */
constexpr uint32_t kKeepAwakePeriodMs = 30000;

/* PlaceAt 先解析延迟尺寸，再把控件中心对齐到屏幕中心系的(cx,cy)。 */
void PlaceAt(lv_obj_t* obj, int cx, int cy) {
    lv_obj_update_layout(obj);  // 新圆钮尚未布局时宽高可能为零，不能据此计算左上角。
    lv_obj_set_pos(obj, kScreenSize / 2 + cx - lv_obj_get_width(obj) / 2,
                   kScreenSize / 2 + cy - lv_obj_get_height(obj) / 2);
}

/* CreateRoundButton 创建圆形背景控制钮并返回钮对象；钮内图标字体标签经
 * icon_out 返回，供调用方更换图标。 */
lv_obj_t* CreateRoundButton(lv_obj_t* parent, int size, lv_color_t bg, lv_opa_t opa,
                            const char* symbol, const lv_font_t* icon_font, lv_event_cb_t cb,
                            lv_obj_t** icon_out) {
    auto* button = lv_button_create(parent);
    lv_obj_set_size(button, size, size);
    lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(button, bg, 0);
    lv_obj_set_style_bg_opa(button, opa, 0);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    auto* icon = lv_label_create(button);
    lv_obj_set_style_text_font(icon, icon_font, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(0xffffff), 0);
    lv_label_set_text(icon, symbol);
    lv_obj_center(icon);
    /* 触摸门禁可能在抬手阶段吞掉 CLICKED；用 SHORT_CLICKED 与主页菜单保持一致，
     * 真机通话页点挂断无响应即由事件类型不匹配导致。 */
    lv_obj_add_event_cb(button, cb, LV_EVENT_SHORT_CLICKED, nullptr);
    if (icon_out != nullptr)
        *icon_out = icon;
    return button;
}

/* CreateHint 创建控制钮下方的小号状态文字，返回标签对象。 */
lv_obj_t* CreateHint(lv_obj_t* parent, int cx, int cy, const char* text, const lv_font_t* font,
                     lv_color_t color) {
    auto* label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_label_set_text(label, text);
    lv_obj_update_layout(label);
    PlaceAt(label, cx, cy);
    return label;
}

/* RotateIcon135 把电话图标旋转 135 度，形成微信式“挂断”视觉。 */
void RotateIcon135(lv_obj_t* icon) {
    lv_obj_set_style_transform_rotation(icon, 1350, 0);  // 单位 0.1 度。
    lv_obj_set_style_transform_pivot_x(icon, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(icon, LV_PCT(50), 0);
}

/* SetStatusText 用短文字区分连接、通话和结束，避免电话图标被误认为待接听。
 * 更新后重新布局居中，保持文字位于圆屏中央可视区域。 */
void SetStatusText(lv_obj_t* label, const char* text) {
    lv_label_set_text(label, text);
    lv_obj_update_layout(label);
    PlaceAt(label, 0, kStatusY);
}

}  // namespace

WatcherCallUi& WatcherCallUi::Instance() {
    static WatcherCallUi instance;
    return instance;
}

void WatcherCallUi::Install(Display* display, std::function<void()> keep_awake,
                            std::function<void(bool)> set_active) {
    auto& ui = Instance();
    ui.display_ = display;
    ui.keep_awake_ = std::move(keep_awake);
    ui.set_active_ = std::move(set_active);
    UrSignalingCall::UiHooks hooks;
    hooks.on_call_kind = [](bool voice) { Instance().HandleCallKind(voice); };
    hooks.on_state = [](UrSignalingCall::UiState state) { Instance().HandleState(state); };
    hooks.on_track_state = [](bool mic_paused, bool camera_paused) {
        Instance().HandleTrackState(mic_paused, camera_paused);
    };
    hooks.on_remote_frame = [](const std::uint8_t* rgb565, int width, int height) {
        Instance().HandleRemoteFrame(rgb565, width, height);
    };
    UrSignalingCall::Instance().SetUiHooks(std::move(hooks));
    ESP_LOGI(TAG, "call ui installed");
}

// TraceState 在显示锁内输出安全状态，区分实际页面隐藏、覆盖层与截图取证问题。
void WatcherCallUi::TraceState() {
    auto& ui = Instance();  // 只读取当前单例，不创建页面。
    if (!ui.display_)
        return;
    DisplayLockGuard lock(ui.display_);
    if (!lock)
        return;
    auto* display = lv_display_get_default();  // Watcher仅装配一个LCD显示。
    auto* active = lv_display_get_screen_active(display);
    printf("[UR_CALL_UI] state=%d shown=%d active_matches=%d frame=%d\n", ui.ui_state_.load(),
           int(ui.shown_), int(active == ui.screen_), int(ui.current_frame_ != nullptr));
    // 只记录可见性、透明度和尺寸；不输出文本、图像、身份或对象地址。
    const auto trace = [](const char* name, lv_obj_t* object) {
        if (!object) {
            printf("[UR_CALL_UI] %s absent\n", name);
            return;
        }
        printf(
            "[UR_CALL_UI] %s visible=%d hidden=%d opa=%u bg_opa=%u children=%u size=%dx%d "
            "pos=%d,%d\n",
            name, int(lv_obj_is_visible(object)), int(lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN)),
            unsigned(lv_obj_get_style_opa(object, LV_PART_MAIN)),
            unsigned(lv_obj_get_style_bg_opa(object, LV_PART_MAIN)),
            unsigned(lv_obj_get_child_count(object)), int(lv_obj_get_width(object)),
            int(lv_obj_get_height(object)), int(lv_obj_get_x(object)), int(lv_obj_get_y(object)));
    };
    trace("active", active);
    trace("call", ui.screen_);
    trace("controls", ui.control_box_);
    trace("remote", ui.remote_image_);
    trace("top", lv_display_get_layer_top(display));
    trace("system", lv_display_get_layer_sys(display));
    // 圆屏四角不可见，按控件实际位置核对圆形可视区域，不能只依据矩形截图。
    trace("accept", ui.accept_btn_);
    trace("reject", ui.reject_btn_);
    trace("mic", ui.mic_btn_);
    trace("hangup", ui.hangup_btn_);
    trace("camera", ui.camera_btn_);
}

// RefreshTheme 只刷新已有控件，视频像素、红绿通话动作及媒体生命周期保持原有语义。
void WatcherCallUi::RefreshTheme() {
    auto& ui = Instance();  // 页面惰性创建，未初始化时无操作。
    if (!ui.display_ || !ui.screen_)
        return;
    const auto* theme = static_cast<LvglTheme*>(ui.display_->GetTheme());  // 主题字体由资源管理器持有。
    if (!theme || !theme->text_font() || !theme->large_icon_font())
        return;
    const auto& skin = SkinFor(theme->name());  // 所有页面共用同一目录。
    lv_obj_set_style_bg_color(ui.screen_, lv_color_hex(skin.background), 0);
    lv_obj_set_style_text_color(ui.screen_, lv_color_hex(skin.text), 0);
    lv_obj_set_style_text_font(ui.screen_, theme->text_font()->font(), 0);
    for (auto* label : {ui.status_label_, ui.ring_title_, ui.accept_hint_, ui.reject_hint_}) {
        if (label) {
            lv_obj_set_style_text_font(label, theme->text_font()->font(), 0);
            lv_obj_set_style_text_color(label, lv_color_hex(skin.text), 0);
        }
    }
    if (ui.status_label_) PlaceAt(ui.status_label_, 0, kStatusY);
    if (ui.ring_title_) PlaceAt(ui.ring_title_, 0, kRingTitleY);
    if (ui.accept_hint_) PlaceAt(ui.accept_hint_, kRingButtonSpan, kRingHintY);
    if (ui.reject_hint_) PlaceAt(ui.reject_hint_, -kRingButtonSpan, kRingHintY);
    for (auto* icon : {ui.accept_icon_, ui.reject_icon_, ui.hangup_icon_, ui.mic_icon_, ui.camera_icon_})
        if (icon) lv_obj_set_style_text_font(icon, theme->large_icon_font()->font(), 0);
}

void WatcherCallUi::OnKnobClick() {
    ESP_LOGI(TAG, "call input source=knob state=%d", ui_state_.load());
    // 显示任务上下文调用；UiAccept/UiHangUp 线程安全，内部转信令任务执行。
    if (static_cast<UrSignalingCall::UiState>(ui_state_.load()) ==
        UrSignalingCall::UiState::Ringing) {
        UrSignalingCall::Instance().UiAccept();
    } else {
        UrSignalingCall::Instance().UiHangUp();
    }
}

// HandleCallKind 与状态同序排入应用任务，避免来电页先按旧类型绘制。
void WatcherCallUi::HandleCallKind(bool voice) {
    Application::GetInstance().Schedule([this, voice] {
        if (!display_)
            return;
        DisplayLockGuard lock(display_);
        if (lock)
            ApplyCallKind(voice);
    });
}

void WatcherCallUi::HandleState(UrSignalingCall::UiState state) {
    // 在转主任务前作废旧帧，避免排队期间迟到拷贝跨越通话边界。
    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        frame_accepting_ = state == UrSignalingCall::UiState::Connecting ||
                           state == UrSignalingCall::UiState::Active;
        if (!frame_accepting_) {
            ++frame_generation_;
            pending_frame_.reset();
        }
    }
    // 信令任务上下文：只转主任务，回调体不触碰 LVGL。
    Application::GetInstance().Schedule([this, state]() {
        if (display_ == nullptr)
            return;
        DisplayLockGuard lock(display_);
        if (!lock)
            return;
        ApplyState(state);
    });
}

void WatcherCallUi::HandleTrackState(bool mic_paused, bool camera_paused) {
    Application::GetInstance().Schedule([this, mic_paused, camera_paused]() {
        if (display_ == nullptr)
            return;
        DisplayLockGuard lock(display_);
        if (!lock)
            return;
        ApplyTrackState(mic_paused, camera_paused);
    });
}

void WatcherCallUi::HandleRemoteFrame(const std::uint8_t* rgb565, int width, int height) {
    // 空帧是信令线程的过期/停显通知；在主任务清除图像，保留通话按钮和音频。
    if (rgb565 == nullptr) {
        std::uint64_t clear_generation;
        {
            std::lock_guard<std::mutex> lock(frame_mutex_);
            clear_generation = ++frame_generation_;  // 作废尚在拷贝的旧帧。
            pending_frame_.reset();
        }
        Application::GetInstance().Schedule([this, clear_generation] {
            if (display_ == nullptr)
                return;
            DisplayLockGuard lock(display_);
            if (!lock || remote_image_ == nullptr)
                return;
            {
                std::lock_guard<std::mutex> frame_lock(frame_mutex_);
                if (frame_generation_ != clear_generation ||
                    (current_frame_ && displayed_frame_generation_ >= clear_generation))
                    return;  // 迟到清屏不能遮挡新会话或清屏后的新帧。
            }
            lv_image_set_src(remote_image_, nullptr);
            lv_obj_add_flag(remote_image_, LV_OBJ_FLAG_HIDDEN);
            std::lock_guard<std::mutex> frame_lock(frame_mutex_);
            current_frame_.reset();
            spare_frame_.reset();
        });
        return;
    }
    // 解码任务上下文：非通话占用时直接丢帧；缓冲仅回调期间有效，须先拷贝。
    const auto state = static_cast<UrSignalingCall::UiState>(ui_state_.load());
    if (rgb565 == nullptr || width <= 0 || height <= 0 || width > 1920 || height > 1920 ||
        (state != UrSignalingCall::UiState::Active &&
         state != UrSignalingCall::UiState::Connecting))
        return;
    // 主任务尚未取走待显示帧时，直接复用该槽覆盖为最新帧，避免显示任务稍慢时
    // 继续等待过期画面；槽位仍有界，且不改变“只显示最新帧”的语义。
    std::uint64_t generation;  // 本次拷贝所属的通话帧代次，用于阻断停流后的迟到帧。
    std::unique_ptr<LvglAllocatedImage> frame;  // 当前回调复用的待显示或备用缓冲。
    bool replacing_pending = false;  // 标记本次是否覆盖了尚未上屏的旧待显示帧。
    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        if (!frame_accepting_)
            return;
        generation = frame_generation_;
        if (pending_frame_ != nullptr) {
            frame = std::move(pending_frame_);
            replacing_pending = true;
            ++dropped_frames_;  // 统计被最新帧覆盖的待显示帧，不记录逐帧日志。
        } else {
            frame = std::move(spare_frame_);
        }
    }
    static uint32_t received = 0;
    if ((++received % 10) == 1) {
        ESP_LOGI(TAG, "remote frame #%u %dx%d state=%d", received, width, height,
                 static_cast<int>(state));
    }
    const size_t image_size = static_cast<size_t>(width) * height * 2;
    if (frame == nullptr)
        frame = std::make_unique<LvglAllocatedImage>();
    if (!frame->CopyFrom(rgb565, image_size, width, height, width * 2, LV_COLOR_FORMAT_RGB565)) {
        ESP_LOGW(TAG, "drop frame: no reusable spiram (%u bytes)",
                 static_cast<unsigned>(image_size));
        std::lock_guard<std::mutex> lock(frame_mutex_);
        if (replacing_pending && pending_frame_ == nullptr)
            pending_frame_ = std::move(frame);
        else if (spare_frame_ == nullptr)
            spare_frame_ = std::move(frame);
        return;
    }
    // 远端通话帧固定为不透明 RGB565；标记后由 Watcher 直接绘制单元逐行复制，
    // 避免 LVGL 通用图像混合器在每个像素上重复判断透明度和变换状态。
    frame->SetFlags(LV_IMAGE_FLAGS_USER2);
    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        if (!frame_accepting_ || generation != frame_generation_) {
            if (spare_frame_ == nullptr)
                spare_frame_ = std::move(frame);
            return;  // 停流或挂断期间完成的拷贝不能重新进入显示槽。
        }
        pending_frame_generation_ = generation;
        if (pending_frame_ == nullptr) {
            pending_frame_ = std::move(frame);  // 只交换槽位，后续帧复用备用缓冲。
        } else if (spare_frame_ == nullptr) {
            // 拷贝期间已有更新帧进入待显示槽，保留更新帧并回收当前较旧槽位。
            spare_frame_ = std::move(frame);
        }
    }
    if (!frame_scheduled_.exchange(true)) {
        Application::GetInstance().Schedule([this]() {
            if (display_ == nullptr) {
                frame_scheduled_.store(false);
                return;
            }
            DisplayLockGuard lock(display_);
            if (!lock) {
                // 拿不到显示锁也要复位在途标志，允许下一帧重试。
                frame_scheduled_.store(false);
                return;
            }
            DrainFrame();
        });
    }
}

// ApplyCallKind 可先保存快照再建页；视频来电恢复三按钮，语音来电只留麦克风和挂断。
void WatcherCallUi::ApplyCallKind(bool voice) {
    voice_call_ = voice;
    if (ring_title_) {
        lv_label_set_text(ring_title_, voice ? "语音来电" : "视频来电");
        lv_obj_update_layout(ring_title_);
        PlaceAt(ring_title_, 0, kRingTitleY);
    }
    if (camera_btn_) {
        if (voice)
            lv_obj_add_flag(camera_btn_, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_remove_flag(camera_btn_, LV_OBJ_FLAG_HIDDEN);
    }
    if (mic_btn_ && hangup_btn_) {
        PlaceAt(mic_btn_, voice ? -kTrackButtonSpan * 2 / 3 : -kTrackButtonSpan, kControlY);
        PlaceAt(hangup_btn_, voice ? kTrackButtonSpan * 2 / 3 : 0, kControlY);
    }
}

void WatcherCallUi::ApplyState(UrSignalingCall::UiState state) {
    if (state == UrSignalingCall::UiState::Hidden) {
        HideLocked();
        return;
    }
    if (!BuildLocked())
        return;
    if (ended_timer_ != nullptr && state != UrSignalingCall::UiState::Ended) {
        // 新来电或状态回退时取消未到期的自动收回。
        lv_timer_delete(ended_timer_);
        ended_timer_ = nullptr;
    }
    ui_state_.store(static_cast<int>(state));
    switch (state) {
        case UrSignalingCall::UiState::Ringing:
            lv_obj_remove_flag(ring_box_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(control_box_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(remote_image_, LV_OBJ_FLAG_HIDDEN);
            break;
        case UrSignalingCall::UiState::Connecting:
        case UrSignalingCall::UiState::Active: {
            lv_obj_add_flag(ring_box_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(control_box_, LV_OBJ_FLAG_HIDDEN);
            // 接通中提示；Active 且尚无帧时同样提示等待对方视频。
            const bool has_frame = current_frame_ != nullptr;
            if (state == UrSignalingCall::UiState::Connecting || !has_frame) {
                SetStatusText(status_label_, state == UrSignalingCall::UiState::Connecting
                                                 ? "连接中"
                                                 : (voice_call_ ? "语音通话中" : "等待对方画面"));
                lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
            }
            if (has_frame)
                lv_obj_remove_flag(remote_image_, LV_OBJ_FLAG_HIDDEN);
            break;
        }
        case UrSignalingCall::UiState::Ended:
            // 结束页明确显示已结束，视频与控制钮全部隐藏。
            lv_obj_add_flag(ring_box_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(control_box_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(remote_image_, LV_OBJ_FLAG_HIDDEN);
            SetStatusText(status_label_, "通话已结束");
            lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
            if (ended_timer_ == nullptr) {
                ended_timer_ = lv_timer_create(OnEndedTimeout, kEndedLingerMs, this);
                if (ended_timer_ != nullptr)
                    lv_timer_set_repeat_count(ended_timer_, 1);
            }
            break;
        default:
            break;
    }
    /* Ended 只在页面仍显示时短暂展示；Hide 后迟到的 Ended 不能重新拉起通话页，
     * 否则会与主页切换形成闪烁循环（真机“通话已结束”页反复闪烁根因）。 */
    if (!shown_ && state != UrSignalingCall::UiState::Ended)
        ShowLocked();
    TouchKeepAwake();
}

void WatcherCallUi::ApplyTrackState(bool mic_paused, bool camera_paused) {
    mic_paused_ = mic_paused;
    camera_paused_ = camera_paused;
    if (!shown_ || mic_btn_ == nullptr)
        return;
    // 关闭态用红色底与替换图标表达；图标字体无摄像头关闭字形，摄像头仅变色换字。
    /* 圆屏只保留图标：麦克风/摄像头文字提示已去掉，状态用图标与红色底表达。 */
    lv_label_set_text(mic_icon_, mic_paused ? MATERIAL_SYMBOLS_MIC_OFF : MATERIAL_SYMBOLS_MIC);
    lv_obj_set_style_bg_color(mic_btn_, lv_color_hex(mic_paused ? 0xd94040 : 0x333333), 0);
    lv_obj_set_style_bg_opa(mic_btn_, mic_paused ? LV_OPA_COVER : LV_OPA_80, 0);
    lv_obj_set_style_bg_color(camera_btn_, lv_color_hex(camera_paused ? 0xd94040 : 0x333333), 0);
    lv_obj_set_style_bg_opa(camera_btn_, camera_paused ? LV_OPA_COVER : LV_OPA_80, 0);
}

void WatcherCallUi::DrainFrame() {
    std::unique_ptr<LvglAllocatedImage> taken;
    std::uint64_t generation;
    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        taken = std::move(pending_frame_);
        generation = pending_frame_generation_;
    }
    frame_scheduled_.store(false);
    {
        // 取帧后又有新帧到达时补排一次，避免最后一帧滞留槽内。
        std::lock_guard<std::mutex> lock(frame_mutex_);
        if (pending_frame_ != nullptr && !frame_scheduled_.exchange(true)) {
            Application::GetInstance().Schedule([this]() {
                if (display_ == nullptr) {
                    frame_scheduled_.store(false);
                    return;
                }
                DisplayLockGuard lock(display_);
                if (!lock) {
                    frame_scheduled_.store(false);
                    return;
                }
                DrainFrame();
            });
        }
    }
    if (taken == nullptr || !shown_) {
        if (taken != nullptr) {
            std::lock_guard<std::mutex> lock(frame_mutex_);
            if (spare_frame_ == nullptr)
                spare_frame_ = std::move(taken);
        }
        return;
    }
    const auto state = static_cast<UrSignalingCall::UiState>(ui_state_.load());
    if (state != UrSignalingCall::UiState::Active &&
        state != UrSignalingCall::UiState::Connecting) {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        if (spare_frame_ == nullptr)
            spare_frame_ = std::move(taken);
        return;
    }
    const auto* dsc = taken->image_dsc();
    if (remote_image_ == nullptr || dsc == nullptr) {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        if (spare_frame_ == nullptr)
            spare_frame_ = std::move(taken);
        return;
    }
    if (dsc->header.w != frame_width_ || dsc->header.h != frame_height_) {
        // 设备端收到的网页帧固定为240×240；保持原尺寸居中，避免LVGL每帧执行
        // 240→412的软件放大。缩放本身会占用主任务，实机日志显示它是3～4fps的
        // 主要候选瓶颈；帧率验证完成后再评估是否需要单独的硬件放大路径。
        frame_width_ = dsc->header.w;
        frame_height_ = dsc->header.h;
        lv_image_set_scale(remote_image_, 256);
        lv_obj_set_size(remote_image_, frame_width_, frame_height_);
        lv_obj_center(remote_image_);
    }
    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        if (!frame_accepting_ || generation != frame_generation_) {
            if (spare_frame_ == nullptr)
                spare_frame_ = std::move(taken);
            return;  // 领取后若已停流，不能在清屏或新会话中显示旧帧。
        }
        lv_image_set_src(remote_image_, dsc);
        spare_frame_ = std::move(current_frame_);
        current_frame_ = std::move(taken);  // 旧帧已摘除，回收到备用槽复用。
        displayed_frame_generation_ = generation;
    }
    lv_obj_remove_flag(remote_image_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
    static uint32_t shown = 0;
    if ((++shown % 10) == 1) {
        ESP_LOGI(TAG, "frame shown #%u %dx%d", shown, dsc->header.w, dsc->header.h);
    }
    ++displayed_frames_;
    const auto now_us = esp_timer_get_time();
    if (display_rate_started_us_ == 0)
        display_rate_started_us_ = now_us;
    const auto elapsed_us = now_us - display_rate_started_us_;
    if (elapsed_us >= 1000000) {
        const auto fps_milli = static_cast<std::uint64_t>(displayed_frames_) * 1000000000ULL /
                               static_cast<std::uint64_t>(elapsed_us);
        ESP_LOGI(TAG, "remote display rate=%u.%03u fps frames=%u window_ms=%u",
                 static_cast<unsigned>(fps_milli / 1000), static_cast<unsigned>(fps_milli % 1000),
                 displayed_frames_, static_cast<unsigned>(elapsed_us / 1000));
        displayed_frames_ = 0;
        display_rate_started_us_ = now_us;
    }
    TouchKeepAwake();
}

bool WatcherCallUi::BuildLocked() {
    if (screen_ != nullptr)
        return true;
    auto* theme = static_cast<LvglTheme*>(display_ != nullptr ? display_->GetTheme() : nullptr);
    if (theme == nullptr || theme->text_font() == nullptr || theme->large_icon_font() == nullptr)
        return false;
    const auto& skin = SkinFor(theme->name());  // 首次建页也使用已保存皮肤，避免黑白跳变。
    const lv_font_t* text_font = theme->text_font()->font();
    const lv_font_t* icon_font = theme->large_icon_font()->font();
    screen_ = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(screen_, lv_color_hex(skin.background), 0);
    lv_obj_set_style_text_font(screen_, text_font, 0);
    lv_obj_remove_flag(screen_, LV_OBJ_FLAG_SCROLLABLE);

    // 全屏对方视频：默认隐藏，首帧到达后按分辨率缩放显示。
    remote_image_ = lv_image_create(screen_);
    lv_obj_set_size(remote_image_, kScreenSize, kScreenSize);
    lv_obj_set_pos(remote_image_, 0, 0);
    lv_obj_add_flag(remote_image_, LV_OBJ_FLAG_HIDDEN);

    // 状态文字与来电标题共用中文正文字体，不能用仅含图标的Material字体。
    status_label_ = lv_label_create(screen_);
    lv_obj_set_style_text_font(status_label_, text_font, 0);
    lv_obj_set_style_text_color(status_label_, lv_color_hex(skin.text), 0);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(status_label_, "");
    lv_obj_update_layout(status_label_);
    PlaceAt(status_label_, 0, kStatusY);

    // 来电容器：标题 + 红拒接/绿接听圆钮及文字。
    ring_box_ = lv_obj_create(screen_);
    lv_obj_set_size(ring_box_, kScreenSize, kScreenSize);
    lv_obj_set_pos(ring_box_, 0, 0);
    lv_obj_remove_flag(ring_box_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(ring_box_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ring_box_, 0, 0);
    lv_obj_set_style_pad_all(ring_box_, 0, 0);
    ring_title_ = lv_label_create(ring_box_);
    lv_obj_set_style_text_color(ring_title_, lv_color_hex(0xffffff), 0);
    lv_label_set_text(ring_title_, "视频来电");
    lv_obj_update_layout(ring_title_);
    PlaceAt(ring_title_, 0, kRingTitleY);
    accept_btn_ = CreateRoundButton(
        ring_box_, kRingButtonSize, lv_color_hex(0x35c26a), LV_OPA_COVER, MATERIAL_SYMBOLS_PHONE,
        icon_font, [](lv_event_t*) { UrSignalingCall::Instance().UiAccept(); }, &accept_icon_);
    PlaceAt(accept_btn_, kRingButtonSpan, kRingButtonY);
    reject_btn_ = CreateRoundButton(
        ring_box_, kRingButtonSize, lv_color_hex(0xd94040), LV_OPA_COVER, MATERIAL_SYMBOLS_PHONE,
        icon_font, [](lv_event_t*) { UrSignalingCall::Instance().UiReject(); }, &reject_icon_);
    RotateIcon135(reject_icon_);
    PlaceAt(reject_btn_, -kRingButtonSpan, kRingButtonY);
    accept_hint_ = CreateHint(ring_box_, kRingButtonSpan, kRingHintY, "接听", text_font, lv_color_hex(0xbdf2c9));
    reject_hint_ = CreateHint(ring_box_, -kRingButtonSpan, kRingHintY, "拒接", text_font, lv_color_hex(0xffb3b3));

    // 通话控制容器：挂断居中，麦克风/摄像头分列两侧。
    control_box_ = lv_obj_create(screen_);
    lv_obj_set_size(control_box_, kScreenSize, kScreenSize);
    lv_obj_set_pos(control_box_, 0, 0);
    lv_obj_remove_flag(control_box_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(control_box_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(control_box_, 0, 0);
    lv_obj_set_style_pad_all(control_box_, 0, 0);
    hangup_btn_ = CreateRoundButton(
        control_box_, kHangupButtonSize, lv_color_hex(0xd94040), LV_OPA_COVER,
        MATERIAL_SYMBOLS_PHONE, icon_font,
        [](lv_event_t*) {
            ESP_LOGI(TAG, "call input source=touch action=hangup");
            UrSignalingCall::Instance().UiHangUp();
        }, &hangup_icon_);
    RotateIcon135(hangup_icon_);
    PlaceAt(hangup_btn_, 0, kControlY);
    mic_btn_ = CreateRoundButton(
        control_box_, kTrackButtonSize, lv_color_hex(0x333333), LV_OPA_80, MATERIAL_SYMBOLS_MIC,
        icon_font, [](lv_event_t*) { UrSignalingCall::Instance().UiToggleMic(); }, &mic_icon_);
    PlaceAt(mic_btn_, -kTrackButtonSpan, kControlY);
    camera_btn_ = CreateRoundButton(
        control_box_, kTrackButtonSize, lv_color_hex(0x333333), LV_OPA_80,
        MATERIAL_SYMBOLS_PHOTO_CAMERA, icon_font,
        [](lv_event_t*) { UrSignalingCall::Instance().UiToggleCamera(); }, &camera_icon_);
    PlaceAt(camera_btn_, kTrackButtonSpan, kControlY);
    /* 圆屏空间有限：只保留图标，不再创建“麦克风/摄像头”文字提示。 */
    /* 远端全屏画面可能在 LVGL 重绘时覆盖同级控件；显式置顶控制层，保证
     * 挂断/麦克风/摄像头按钮始终可点可见。 */
    lv_obj_move_foreground(control_box_);
    ApplyTrackState(mic_paused_, camera_paused_);
    ApplyCallKind(voice_call_);
    RefreshTheme();
    ESP_LOGI(TAG, "call screen built");
    return true;
}

void WatcherCallUi::ShowLocked() {
    RefreshTheme();
    shown_ = true;
    displayed_frames_ = 0;
    display_rate_started_us_ = 0;
    // 通知板级显示冻结菜单渲染，再加载通话屏幕；两者都在显示锁内完成。
    if (set_active_)
        set_active_(true);
    lv_screen_load_anim(screen_, LV_SCR_LOAD_ANIM_FADE_IN, 120, 0, false);
}

void WatcherCallUi::HideLocked() {
    if (ended_timer_ != nullptr) {
        lv_timer_delete(ended_timer_);
        ended_timer_ = nullptr;
    }
    ui_state_.store(static_cast<int>(UrSignalingCall::UiState::Hidden));
    if (!shown_)
        return;
    shown_ = false;
    if (remote_image_ != nullptr) {
        // 先摘除图像源再释放帧缓冲，期间持有显示锁不会发生绘制。
        lv_image_set_src(remote_image_, nullptr);
        lv_obj_add_flag(remote_image_, LV_OBJ_FLAG_HIDDEN);
    }
    {
        std::lock_guard<std::mutex> frame_lock(frame_mutex_);
        current_frame_.reset();  // 释放最后一帧 PSRAM。
        pending_frame_.reset();
        spare_frame_.reset();
    }
    frame_width_ = frame_height_ = 0;
    // 板级归位主页并解冻菜单渲染；主页屏幕由板级页面管理器加载。
    if (set_active_)
        set_active_(false);
}

void WatcherCallUi::OnEndedTimeout(lv_timer_t* timer) {
    // LVGL 定时器上下文即显示任务，可直接收回页面。
    auto* self = static_cast<WatcherCallUi*>(lv_timer_get_user_data(timer));
    if (self == nullptr)
        return;
    self->ended_timer_ = nullptr;  // 一次性定时器即将自毁，先清引用。
    self->HideLocked();
}

void WatcherCallUi::TouchKeepAwake() {
    const uint32_t now = lv_tick_get();
    if (last_awake_ms_ != 0 && now - last_awake_ms_ < kKeepAwakePeriodMs)
        return;
    last_awake_ms_ = now;
    if (keep_awake_)
        keep_awake_();
}

}  // namespace watcher

#endif /* CONFIG_UR_AI_SIGNALING_WEBRTC */
