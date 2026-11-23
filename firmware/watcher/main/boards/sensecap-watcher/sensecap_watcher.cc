// Watcher 板级硬件与交互：圆屏显示、菜单输入分发、相机和独占 BLE 启动。
#include <ctime>
#include "display/lv_display.h"
#include "ui/dial_view.h"
#include "ui/menu_model.h"
#include "ui/skin_theme.h"
#include "settings.h"

// 完整辅助字体保存在应用Flash中，cbin只为字体描述分配少量运行内存。
extern const uint8_t watcher_menu_font_start[] asm("_binary_font_noto_sans_common_20_4_bin_start");
LV_FONT_DECLARE(lv_font_montserrat_48);
LV_FONT_DECLARE(font_material_symbols_20_4);
#include "ur_signaling_contacts.h"
#include "ui/page_manager.h"
#include "ui/touch_gate.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/idf_additions.h>
#include <esp_heap_caps.h>
#include <mutex>
#include <algorithm>
#include <utility>
#include "misc/lv_event.h"
#include "wifi_board.h"
#include "ur_bridge.h"
#include "ur_device.h"
#include "ur_network.h"
#include <esp_random.h>
#if CONFIG_UR_BLE_PROVISION
#include "ur_ble_provision.h"
#endif
#include <ssid_manager.h>
#include "sensecap_audio_codec.h"
#include "display/lcd_display.h"
#include "application.h"
#include "system_info.h"
#include "settings.h"
#include "knob.h"
#include "config.h"
#include "led/single_led.h"
#include "power_save_timer.h"
#include "sscma_camera.h"
#if CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO
#include "ur_av_probe.h"
#endif

#include <cstdlib>

/* 镜子任务由应用任务创建一次，通知唤醒后采集；关闭时停流并等待下次通知。
 * 开关只表示期望状态，快速退出重进不会创建第二个任务或被旧任务的清理覆盖。 */
static std::atomic<bool> g_mirror_on{false};  // 应用任务提交、采集任务读取的期望状态。
static TaskHandle_t g_mirror_task = nullptr;  // 仅应用任务创建和访问，空闲时保留等待通知。

// UrMirrorTask 串行执行相机启停及预览清理；arg 是板级持有、生命周期覆盖任务的相机。
static void UrMirrorTask(void* arg) {
    auto* camera = static_cast<Camera*>(arg);  // 已由启动入口校验的可用相机。
    auto* lcd = static_cast<LcdDisplay*>(Board::GetInstance().GetDisplay());  // 可选显示能力。
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!g_mirror_on.load())
            continue;
        if (lcd)
            lcd->SetPreviewFullscreen(true);
        camera->StartStream();
        while (g_mirror_on.load()) {
            if (!camera->Capture())
                ESP_LOGW("ur_mirror", "capture failed");
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        camera->StopStream();
        // 停流与清理都由同一任务完成；随后才能处理再次开启，避免旧任务清掉新预览。
        if (lcd) {
            lcd->SetPreviewFullscreen(false);
            lcd->SetPreviewImage(nullptr);
        }
    }
}

// SetMirrorEnabled 仅由应用任务提交显式启停；返回 false 表示相机不可用或任务分配失败。
static bool SetMirrorEnabled(Camera* camera, bool enabled) {
#if CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO
    if (enabled && UrAVProbeActive()) return false;
#endif
    if (enabled && !camera)
        return false;
    g_mirror_on.store(enabled);
    // 镜子栈不参与 DMA，放入 PSRAM；通话后的内部堆碎片不能阻止镜子启动或挤占音频余量。
    if (enabled && !g_mirror_task &&
        xTaskCreateWithCaps(UrMirrorTask, "ur_mirror", 8192, camera, 3, &g_mirror_task,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        g_mirror_on.store(false);
        g_mirror_task = nullptr;
        return false;
    }
    if (g_mirror_task)
        xTaskNotifyGive(g_mirror_task);
    return true;
}
#include "lvgl_theme.h"

#include <esp_log.h>
/* sig_screen 通话截图命令的 JPEG CRC32 取证头；原被包在 E2E 条件内，
 * 信令（非 E2E）构建的截图复用同一 esp_rom_crc32_le。 */
#include <esp_rom_crc.h>
#if CONFIG_UR_AI_E2E_TEST
#include <esp_wifi.h>
#include "ur_hw_test_input.h"
#endif
#include <esp_check.h>
#include <esp_lvgl_port.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_spd2010.h>
#include <esp_lcd_touch_spd2010.h>
#include <esp_adc/adc_oneshot.h>
#include <driver/spi_master.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <iot_button.h>
#include <iot_knob.h>
#include <esp_io_expander_tca95xx_16bit.h>
#include <esp_sleep.h>
#include <esp_console.h>
#include <esp_mac.h>
#if CONFIG_UR_AI_SIGNALING_WEBRTC
#include "ur_signaling_call.h"
#include "ur_signaling_protocol.h"
#include "watcher_call_ui.h"
#endif
#include <nvs_flash.h>
#include <esp_app_desc.h>

#include "assets/lang_config.h"

#define TAG "sensecap_watcher"

class CustomLcdDisplay : public SpiLcdDisplay {
    private:
        // MenuEvent 复制输入快照到显示任务，不在按钮/音频任务访问 LVGL。
        struct MenuEvent {
            watcher::MenuInput input;  // 已经由按钮驱动区分的手势。
            bool asleep;  // 本次输入是否仅用于唤醒。
            bool busy;  // OTA 或相机占用时拒绝进入菜单。
            int volume;  // 进入菜单时的实际设置。
            int brightness;  // 进入菜单时的实际背光。
            bool charging;  // 电源页区分 USB 重启与电池关机。
            bool confirm_network = false;  // 云端请求只打开确认页，不自动执行。
            int touch_index = -1;  // 非负值为触摸选中的行，仍经应用任务检查资源占用。
            uint32_t input_epoch = 0;     // 输入所属的表盘、AI或通话显示阶段。
            uint32_t touch_revision = 0;  // 按下时页面代次，拒绝跨页面迟到点击。
        };
        QueueHandle_t menu_events_ = xQueueCreate(16, sizeof(MenuEvent));  // 有界输入队列。
        watcher::MenuModel menu_model_;  // 仅显示任务修改。
        watcher::PageManager pages_;  // 改编官方页面管理器，复用页面对象。
        watcher::Page home_page_, menu_page_;  // 屏幕生命周期与板实例一致。
        std::unique_ptr<LvglCBinFont>
            menu_small_font_;                   // 完整20px字体的运行描述，位图直接引用Flash。
        watcher::DialView dial_view_;           // 表盘、设置和通讯录的固定对象视图。
        std::atomic<uint32_t> input_epoch_{0};  // 跨任务输入模式代次。
        std::atomic<uint32_t> published_revision_{0};  // 本地诊断也通过发布快照读取页面代次。
        bool render_pending_ = false;                  // 页面切换动画完成后补交期间的最新目标页。
        std::atomic<bool> menu_visible_{false};        // 应用任务读取最新菜单锁状态。
        bool home_surface_ = false;                    // 原对话或拍照页面当前拥有主页区域。
        int64_t clock_minute_ = -1;                    // 只在分钟变化时更新表盘时间。
        lv_obj_t* home_menu_ = nullptr;  // 主页“菜单”按钮，镜子全屏时隐藏避免误触退出。
        lv_timer_t* menu_timer_ = nullptr;  // 在 LVGL 任务中消费输入及处理空闲返回。
        std::vector<UrCallContact> contacts_; // 仅显示任务持有当前页四个联系人。
        std::optional<UrContactsResult> pending_contacts_; // 网络回调经ui_mutex_投递的最新结果。
        bool call_ui_active_ = false;  // 通话页占用屏幕期间冻结菜单渲染与输入。
        bool charging_ = false;  // 最近一次输入携带的供电快照。
        bool binding_flow_ = false;  // 本次独占启动是否允许发送绑定 Token，与普通换网区分。
        std::function<void(watcher::MenuAction, int)> menu_action_;  // 业务经主任务执行。
        std::function<void(size_t, uint32_t)> touch_item_;  // 触摸确认交给板级输入门禁。
        std::function<bool()> touch_wake_;  // 首次接触返回是否只允许唤醒。
        esp_lcd_touch_handle_t touch_handle_ = nullptr;  // 由板实例持有的硬件驱动。
        watcher::TouchGate touch_gate_;  // 唤醒或通讯失败后等待完整抬手。
        uint32_t touch_revision_ = 0;  // 一次手势开始时的页面代次。
        uint32_t touch_epoch_ = 0;     // 按下时的显示模式，切换到通话后不能沿用旧接触。
        uint32_t touch_started_ = 0;   // 当前接触起点，用于有界输入诊断。
        lv_point_t touch_origin_{};    // 首次按下坐标，排查硬件接触而不记录媒体内容。
        bool touch_origin_valid_ = false;     // 通讯故障后无法推断原接触，应等完整抬手。
        unsigned touch_trace_count_ = 0;      // 每次启动最多64条通话触点日志，限制输入噪声开销。
        uint32_t mirror_touch_revision_ = 0;  // 镜子第一次短点按所属页面，拒绝跨页面拼成双击。
        bool mirror_touch_pending_ = false;  // 只有同一镜子页面内完整的第一击才能配对退出。
        uint32_t mirror_touch_started_ = 0;  // 第一次短点按时间，限制双击窗口且允许毫秒计数回绕。
        lv_point_t mirror_touch_point_{};  // 第一次短点按坐标，容忍手指落点小幅偏移。
#if CONFIG_UR_AI_E2E_TEST
        uint32_t touch_reads_ = 0;  // 显示任务累计采样次数，区分输入未注册与没有触点。
        uint32_t touch_errors_ = 0;  // I2C 读取失败次数，不保存触点或业务正文。
        uint32_t touch_contacts_ = 0;  // 完整抬手后开始的新接触次数。
        uint32_t touch_wakes_ = 0;  // 仅唤醒、不执行业务的接触次数。
        uint32_t touch_events_ = 0;  // 显示任务实际消费的点击次数，不把入队当作完成。
        uint32_t touch_busy_ = 0;  // 点击因 OTA、相机或换网占用被门禁拒绝的次数。
        uint32_t touch_stale_ = 0;  // 点击所属页面代次已过期的次数。
#endif
        // UiPending 对高频字幕按角色合并到最新快照，避免把每个增量都排入 LVGL 队列。
        struct UiPending {
            std::string user, assistant, status, emotion;  // 协议层传来的完整字幕快照与状态。
            bool user_dirty = false, assistant_dirty = false, status_dirty = false, emotion_dirty = false;
            bool preview_dirty = false;  // 区分“不更新”和“明确清除预览”。
            std::string notification;  // 最新短提示，显示任务维护到期时间。
            bool notification_dirty = false, status_bar_dirty = false, update_all = false;
            ur_network_status_t network{};  // 非敏感事务快照，由状态栏更新入口投递。
            bool network_dirty = false;  // 只在显示线程消费，不跨线程修改页面。
            std::string restored_network_request;  // 普通启动恢复的旧成功记录，不作为本次换网结果弹出。
            uint32_t notification_ms = 3000;  // 限定在 1ms～180s，避免无效定时值。
            std::unique_ptr<LvglImage> preview;  // 捕获任务移交所有权，显示任务最终持有图像。
        };
        std::mutex ui_mutex_;  // 只保护快照交换，不持锁执行 LVGL 或网络操作。
        UiPending pending_ui_;  // 每类最多一条更新，不积累无界事件。
        bool network_startup_known_ = false;  // ui_mutex_ 保护；启动恢复完成前不展示网络事务。
        PhotoUiState photo_ui_;  // 与邮箱共用互斥锁，旧 worker 不能恢复已取消的提示。
        std::string photo_status_;  // 显示线程的覆盖文本，不改变音频 Idle/Listening。
        std::string normal_status_;  // 取消拍照覆盖后恢复最近一次真实应用状态，而非残留上传提示。
        lv_obj_t* user_subtitle_ = nullptr;  // 对话用户字幕，独立于助手区。
        bool has_user_ = false, has_assistant_ = false;  // 显示任务维护字幕可见性。
        uint32_t notification_at_ = 0, notification_ms_ = 0;  // 显示线程本地提示有效期。
        ur_network_status_t shown_network_{};  // 去重已展示状态，用户退出结果页后不反复弹出。
        std::string restored_network_request_;  // 仅显示线程使用，保留 BLE 历史结果但不重复锁住对话。

        // UpdateNetworkPage 仅消费变化的快照；请求 ID 区分相同阶段的两次独立换网。
        void UpdateNetworkPage(const ur_network_status_t& network) {
            if (network.stage == shown_network_.stage && network.error == shown_network_.error &&
                network.blocked == shown_network_.blocked &&
                strcmp(network.request_id, shown_network_.request_id) == 0) return;
            shown_network_ = network;
            if (network.stage == 7 && !network.blocked && !restored_network_request_.empty() &&
                restored_network_request_ == network.request_id) return;
            menu_model_.ShowNetworkStatus(network.stage, network.blocked, lv_tick_get());
            if (menu_action_) menu_action_(watcher::MenuAction::Visibility,
                menu_model_.Page() != watcher::MenuPage::Home);
            RenderMenu();
        }

        // DrainUi 在显示任务交换快照；普通状态和字幕不能清除相机预览。
        void DrainUi() {
            UiPending update;
            const bool had_notification = notification_ms_ != 0; // 过期时也收回空闲提示层。
            const auto previous_photo_status = photo_status_;  // 只在阶段变化或底层重绘后刷新，避免持续布局影响音频。
            {
                std::lock_guard<std::mutex> lock(ui_mutex_);
                update = std::move(pending_ui_);
                pending_ui_ = UiPending{};
                photo_status_ = photo_ui_.Text();
            }
            if (update.status_dirty) normal_status_ = update.status;
            if (!update.restored_network_request.empty())
                restored_network_request_ = std::move(update.restored_network_request);
            if (update.status_dirty || (!previous_photo_status.empty() && photo_status_.empty()))
                SpiLcdDisplay::SetStatus(normal_status_.c_str());
            if (update.network_dirty) UpdateNetworkPage(update.network);
            if (update.status_bar_dirty) {
                SpiLcdDisplay::UpdateStatusBar(update.update_all);

            }
            // 每次刷新后覆盖空闲时钟；识图等待不能显示成待命，也不能触发收音。
            if (!photo_status_.empty() && (photo_status_ != previous_photo_status ||
                update.status_dirty || update.status_bar_dirty))
                lv_label_set_text(status_label_, photo_status_.c_str());
            if (update.notification_dirty) {
                lv_label_set_text(notification_label_, update.notification.c_str());
                notification_at_ = lv_tick_get(); notification_ms_ = update.notification_ms;
            }
            if (notification_ms_ && uint32_t(lv_tick_get() - notification_at_) >= notification_ms_)
                notification_ms_ = 0;
            if (notification_ms_) {
                lv_obj_remove_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
                lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
            }
            if (update.emotion_dirty) SpiLcdDisplay::SetEmotion(update.emotion.c_str());
            if (update.preview_dirty) {
                SpiLcdDisplay::SetPreviewImage(std::move(update.preview));
                // 预览随回复生命周期结束，不再由固定五秒定时器提前隐藏。
                esp_timer_stop(preview_timer_);
            }
            // 第一段回复必须可见；已有回复仅在读者仍停留尾部时跟随增量。
            const bool follow_assistant = update.assistant_dirty && !update.assistant.empty() &&
                (!has_assistant_ || lv_obj_get_scroll_bottom(bottom_bar_) <= 2);
            if (update.user_dirty) {
                has_user_ = !update.user.empty();
                lv_label_set_text(user_subtitle_, has_user_ ? ("你：\n" + update.user).c_str() : "");
            }
            if (update.assistant_dirty) {
                has_assistant_ = !update.assistant.empty();
                lv_label_set_text(chat_message_label_, has_assistant_ ? ("联犀：\n" + update.assistant).c_str() : "");
            }
            if (update.user_dirty || update.assistant_dirty || update.preview_dirty) {
                RefreshSubtitleViewport(update.user_dirty, follow_assistant ||
                    (update.preview_dirty && !preview_image_cached_ && has_assistant_));
            }
            if (update.user_dirty || update.assistant_dirty || update.emotion_dirty || update.preview_dirty) {
                const bool dialogue = has_user_ || has_assistant_;
                lv_obj_set_size(emoji_box_, dialogue ? 64 : 160, dialogue ? 64 : 160);
                lv_obj_align(emoji_box_, dialogue ? LV_ALIGN_TOP_MID : LV_ALIGN_CENTER, 0, dialogue ? 70 : 0);
                lv_obj_center(emoji_label_);
                lv_image_set_scale(emoji_image_, dialogue ? 128 : 256);
            }
            // 基类SetStatus可能重新显示状态容器，全部邮箱更新完成后再收敛其可见性。
            if (update.status_dirty || update.status_bar_dirty || update.notification_dirty ||
                had_notification != (notification_ms_ != 0))
                ApplyPagePalette();
        }

        // RefreshSubtitleViewport 隐藏空角色并定位新回复；用户上滑阅读时不被后续增量抢回底部。
        void RefreshSubtitleViewport(bool user_changed, bool follow_assistant) {
            for (auto entry : {std::pair<lv_obj_t*, bool>{user_subtitle_, has_user_},
                               std::pair<lv_obj_t*, bool>{chat_message_label_, has_assistant_}}) {
                if (entry.second) lv_obj_remove_flag(entry.first, LV_OBJ_FLAG_HIDDEN);
                else lv_obj_add_flag(entry.first, LV_OBJ_FLAG_HIDDEN);
            }
            if (preview_image_cached_ || hide_subtitle_ || (!has_user_ && !has_assistant_)) {
                lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
                return;
            }
            lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_align(bottom_bar_, LV_ALIGN_TOP_MID, 0, 140);
            // 换行高度及空角色可见性必须先完成布局，再计算本轮真实可滚动范围。
            lv_obj_update_layout(bottom_bar_);
            if (follow_assistant) lv_obj_scroll_to_y(bottom_bar_, LV_COORD_MAX, LV_ANIM_OFF);
            else if (user_changed) lv_obj_scroll_to_y(bottom_bar_, 0, LV_ANIM_OFF);
        }

        // LayoutStatusBar 让表盘状态图标位于左侧时钟上方，其余独占页面沿用顶部布局。
        void LayoutStatusBar(bool dial_home) {
            if (!top_bar_)
                return;
            const auto* theme = static_cast<LvglTheme*>(current_theme_);
            const auto* font =
                dial_home
                    ? &font_material_symbols_20_4
                    : theme->large_icon_font()->font();  // 按当前页面使用辅助字号或原图标字号。
            lv_obj_set_size(top_bar_, dial_home ? 180 : 230, 35);
            lv_obj_align(top_bar_, dial_home ? LV_ALIGN_TOP_LEFT : LV_ALIGN_TOP_MID,
                         dial_home ? 48 : 0, dial_home ? 68 : 35);
            int index = 0;  // 三个图标沿状态容器均匀排列，不与右侧快捷项重叠。
            for (auto* label : {network_label_, mute_label_, battery_label_}) {
                if (label) {
                    lv_obj_set_style_text_font(label, font, 0);
                    lv_obj_align(label, LV_ALIGN_CENTER, (index - 1) * (dial_home ? 54 : 64), 0);
                }
                ++index;
            }
        }

        // LayoutStatusText 将空闲提示放在左侧功能名称下方，原对话状态沿用底部区域。
        void LayoutStatusText(bool dial_home) {
            if (!status_bar_ || !notification_label_) return;
            const auto* theme = static_cast<LvglTheme*>(current_theme_); // 字体由当前主题及辅助字库持有。
            lv_obj_set_size(status_bar_, dial_home ? 180 : 260, dial_home ? 86 : 48);
            lv_obj_align(status_bar_, dial_home ? LV_ALIGN_TOP_LEFT : LV_ALIGN_TOP_MID,
                         dial_home ? 48 : 0, dial_home ? 266 : 292);
            lv_obj_set_style_text_font(notification_label_, dial_home ? menu_small_font_->font()
                : theme->text_font()->font(), 0);
            lv_obj_set_width(notification_label_, dial_home ? 180 : 250);
            lv_obj_set_height(notification_label_, dial_home ? 86 : LV_SIZE_CONTENT);
            lv_label_set_long_mode(notification_label_, dial_home ? LV_LABEL_LONG_WRAP : LV_LABEL_LONG_SCROLL_CIRCULAR);
            lv_obj_align(notification_label_, dial_home ? LV_ALIGN_TOP_LEFT : LV_ALIGN_CENTER, 0, 0);
        }

        // ApplySkinColors 在同一显示锁内更新所有页面，聆听与隐藏菜单始终使用同一皮肤。
        void ApplySkinColors() {
            const auto& skin = watcher::SkinFor(current_theme_->name());  // 目录对象固定，切换不分配字体。
            for (auto* root : {home_page_.screen, menu_page_.screen}) {
                if (root) {
                    lv_obj_set_style_bg_color(root, lv_color_hex(skin.background), 0);
                    lv_obj_set_style_text_color(root, lv_color_hex(skin.text), 0);
                }
            }
            if (container_) {
                lv_obj_set_style_bg_image_src(container_, nullptr, 0);
                lv_obj_set_style_bg_color(container_, lv_color_hex(skin.background), 0);
            }
            if (bottom_bar_) {
                lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_COVER, 0);
                lv_obj_set_style_bg_color(bottom_bar_, lv_color_hex(skin.surface), 0);
            }
            for (auto* label : {chat_message_label_, user_subtitle_, emoji_label_})
                if (label) lv_obj_set_style_text_color(label, lv_color_hex(skin.text), 0);
            if (home_menu_) {
                lv_obj_set_style_bg_color(home_menu_, lv_color_hex(skin.surface), 0);
                lv_obj_set_style_text_color(home_menu_, lv_color_hex(skin.text), 0);
            }
            if (dial_view_.Screen()) dial_view_.SetPalette(skin);
            menu_model_.SyncSkin(watcher::SkinIndex(skin.id));
        }

        // ApplyPagePalette 在显示锁内同步独立状态层，所有页面共用当前皮肤的辅助文字颜色。
        void ApplyPagePalette() {
            const auto* theme = static_cast<LvglTheme*>(current_theme_);
            const auto color = lv_color_hex(watcher::SkinFor(theme ? theme->name() : "dark").muted);
            for (auto* label : {network_label_, status_label_, notification_label_, mute_label_, battery_label_}) {
                if (label) lv_obj_set_style_text_color(label, color, 0);
            }
            LayoutStatusBar(!call_ui_active_ && menu_model_.Page() == watcher::MenuPage::Home &&
                            !home_surface_);
            if (top_bar_) lv_obj_set_style_bg_opa(top_bar_, LV_OPA_TRANSP, 0);
            LayoutStatusText(!home_surface_ && menu_model_.Page() == watcher::MenuPage::Home);
            // 独立顶层跨屏存在；菜单和通话均隐藏主页提示，防止时钟/通知盖住操作。
            if (status_bar_) {
                if (!call_ui_active_ && menu_model_.Page() == watcher::MenuPage::Home &&
                    (home_surface_ || notification_ms_ != 0))
                    lv_obj_remove_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);
                else
                    lv_obj_add_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);
            }
        }

        // RenderMenu 将空闲表盘与活动对话分屏，所有绘制仍在唯一显示任务执行。
        void RenderMenu() {
            if (call_ui_active_) return;
            published_revision_.store(menu_model_.Revision());
            ApplyPagePalette();
            const auto page = menu_model_.Page();  // 镜子和AI表情沿用原主页画布。
            menu_visible_.store(page != watcher::MenuPage::Home);
            SetMenuButtonVisible(page == watcher::MenuPage::Home && home_surface_);
            if (page != watcher::MenuPage::Mirror)
                mirror_touch_pending_ = false;
            if (preview_image_) {
                if (page == watcher::MenuPage::Mirror)
                    lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_CLICKABLE);
                else
                    lv_obj_remove_flag(preview_image_, LV_OBJ_FLAG_CLICKABLE);
            }
            if (page == watcher::MenuPage::Mirror ||
                (page == watcher::MenuPage::Home && home_surface_)) {
                render_pending_ = !pages_.Open(home_page_, lv_tick_get(), 150);
                return;
            }
            const auto* theme =
                static_cast<LvglTheme*>(current_theme_);  // 主题释放前SetTheme也会重绑隐藏视图。
            dial_view_.SetFonts(theme->text_font()->font(), menu_small_font_->font(),
                                theme->large_icon_font()->font(), &lv_font_montserrat_48);
            const std::string network =
                shown_network_.blocked ? "配置存储异常，请重启"
                : shown_network_.stage == 7
                    ? (binding_flow_ ? "网络已配置\n请在手机完成添加" : "Wi-Fi 已更新")
                : shown_network_.stage == 8 ? watcher::NetworkErrorText(shown_network_.error)
                : shown_network_.stage <= 2 ? "等待手机连接"
                : shown_network_.stage <= 4 ? "正在连接 Wi-Fi"
                                            : "正在保存网络";
            dial_view_.Render(menu_model_, contacts_, charging_, network,
                              std::string("Watcher\n") + esp_app_get_description()->version);
            render_pending_ = !pages_.Open(menu_page_, lv_tick_get(), 150);
        }

        // RefreshHomeMode 读取线程安全设备状态，输入代次隔离表盘和AI/拍照的不同含义。
        bool RefreshHomeMode() {
            const auto state = Application::GetInstance().GetDeviceState();  // 状态机的原子快照。
            const bool active = state == kDeviceStateConnecting || state == kDeviceStateListening ||
                                state == kDeviceStateSpeaking || state == kDeviceStateNotifying;
            const bool changed = menu_model_.SetConversationActive(active);
            const bool surface =
                active || !photo_status_.empty();  // 拍照阶段即使应用仍Idle也保持原完整画布。
            if (changed || surface != home_surface_) {
                home_surface_ = surface;
                input_epoch_.fetch_add(1);
                return true;
            }
            return false;
        }

        // RefreshDialClock 按北京时间显示已校时的分钟；只修改独立表盘文字。
        void RefreshDialClock() {
            const time_t now = time(nullptr);  // NTP前设备时间不能作为真实表盘时间。
            const int64_t minute = now >= 1704067200 ? now / 60 : -1;
            if (minute == clock_minute_)
                return;
            clock_minute_ = minute;
            char value[6] = "--:--";  // 固定HH:mm，不展示秒针造成持续重绘。
            if (minute >= 0) {
                const time_t beijing = now + 8 * 3600;
                struct tm local{};  // gmtime_r不修改全局时区。
                gmtime_r(&beijing, &local);
                strftime(value, sizeof(value), "%H:%M", &local);
            }
            dial_view_.UpdateClock(value);
        }

        // DrainContacts 只接纳仍在浏览的原页面结果，迟到应答不能重开已退出的通讯录。
        bool DrainContacts() {
            std::optional<UrContactsResult> result; // 将最多四项快照移出互斥区后再更新页面。
            {
                std::lock_guard<std::mutex> lock(ui_mutex_);
                result.swap(pending_contacts_);
            }
            if (!result || !menu_model_.ApplyContacts(result->revision, result->offset,
                    result->contacts.size(), result->more, result->ok)) return false;
            contacts_ = std::move(result->contacts);
            return true;
        }

        // PollMenu 由 LVGL 定时器执行，业务回调仅投递应用任务，不能等待网络。
        void PollMenu() {
            DrainUi();
            const bool mode_dirty = RefreshHomeMode();
            RefreshDialClock();
            // 通话页不运行菜单状态机；旋钮单击控制通话，旋转只调整扬声器音量。
            if (call_ui_active_) {
#if CONFIG_UR_AI_SIGNALING_WEBRTC
                MenuEvent event;
                while (xQueueReceive(menu_events_, &event, 0) == pdTRUE) {
                    if (event.input_epoch != input_epoch_.load() || event.touch_index >= 0 ||
                        event.confirm_network)
                        continue;
                    if (event.input == watcher::MenuInput::Click && !event.asleep)
                        watcher::WatcherCallUi::Instance().OnKnobClick();
                    else if (!event.asleep && event.touch_index < 0 && menu_action_ &&
                             (event.input == watcher::MenuInput::Next || event.input == watcher::MenuInput::Previous))
                        // 在应用任务按最新硬件音量增减，快速转动时不使用排队前的过期快照。
                        menu_action_(watcher::MenuAction::CallVolumeDelta,
                                     event.input == watcher::MenuInput::Next ? -5 : 5);
                }
#endif
                return;
            }
            MenuEvent event;
            bool dirty = DrainContacts() || mode_dirty || render_pending_;
            while (xQueueReceive(menu_events_, &event, 0) == pdTRUE) {
                if (event.input_epoch != input_epoch_.load())
                    continue;
                if (event.confirm_network) {
                    menu_model_.RequestNetworkConfirmation(lv_tick_get());
                    if (menu_action_) menu_action_(watcher::MenuAction::Visibility, 1);
                    dirty = true;
                    continue;
                }
                // 页面打开、返回或联系人回复改变代次后，旧页排队按键不能确认新页操作。
                if (event.touch_revision != menu_model_.Revision())
                    continue;
                charging_ = event.charging;
                // 首页/对话的旋钮统一交给菜单模型调音量，字幕由触屏滚动。
                const bool was_menu = menu_model_.Page() != watcher::MenuPage::Home;
                if (menu_model_.Page() == watcher::MenuPage::Home) menu_model_.SyncValues(event.volume, event.brightness);
#if CONFIG_UR_AI_E2E_TEST
                if (event.touch_index >= 0) {
                    ++touch_events_;
                    if (event.busy) ++touch_busy_;
                    if (event.touch_revision != menu_model_.Revision()) ++touch_stale_;
                }
#endif
                const auto action =
                    event.touch_index == static_cast<int>(watcher::TouchBackIndex)
                        ? menu_model_.TouchBack(event.touch_revision, lv_tick_get(), event.asleep,
                                                event.busy)
                    : event.touch_index >= 0
                        ? menu_model_.TouchTarget(
                              static_cast<watcher::MenuTarget>(event.touch_index),
                              event.touch_revision, lv_tick_get(), event.asleep, event.busy)
                        : menu_model_.Handle(event.input, lv_tick_get(), event.asleep, event.busy);
                const bool is_menu = menu_model_.Page() != watcher::MenuPage::Home;
                if (was_menu != is_menu && menu_action_) menu_action_(watcher::MenuAction::Visibility, is_menu);
                if (action == watcher::MenuAction::LoadContacts) {
                    contacts_.clear();
                    const auto revision = menu_model_.Revision(); // 结果必须绑定这次加载页面。
#if CONFIG_UR_AI_SIGNALING_WEBRTC
                    if (!UrSignalingContacts::Instance().Query(revision, menu_model_.ContactsOffset()))
#endif
                        menu_model_.ApplyContacts(revision, menu_model_.ContactsOffset(), 0, false, false);
                } else if (action == watcher::MenuAction::DialVoice || action == watcher::MenuAction::DialVideo) {
                    bool accepted = false; // 仅排队成功时离开通讯录，不将失败展示为已呼叫。
#if CONFIG_UR_AI_SIGNALING_WEBRTC
                    const auto selected = menu_model_.SelectedContact(); // 按值目标由原列表给出。
                    if (selected < contacts_.size()) accepted = UrSignalingCall::Instance().Dial(
                        contacts_[selected].target, action == watcher::MenuAction::DialVoice);
#endif
                    if (accepted) {
                        menu_model_.ForceHome();
                        if (menu_action_) menu_action_(watcher::MenuAction::Visibility, 0);
                    } else {
                        ShowNotification("暂时无法拨号，请稍后重试");
                    }
                } else if (action != watcher::MenuAction::None && menu_action_) {
                    const int value = action == watcher::MenuAction::Skin
                                          ? static_cast<int>(menu_model_.Skin())
                                      : action == watcher::MenuAction::Mirror
                                          ? menu_model_.Page() == watcher::MenuPage::Mirror
                                      : action == watcher::MenuAction::Brightness
                                          ? menu_model_.Brightness()
                                          : menu_model_.Volume();
                    menu_action_(action, value);
                }
                dirty = true;
            }
            const auto previous = menu_model_.Page();
            const auto tick_action = menu_model_.Handle(watcher::MenuInput::Tick, lv_tick_get());
            if (tick_action != watcher::MenuAction::None && menu_action_) menu_action_(tick_action, 0);
            if (previous != menu_model_.Page() && menu_action_) menu_action_(watcher::MenuAction::Visibility, 0);
            if (dirty || previous != menu_model_.Page()) RenderMenu();
        }

        // SetupMirrorExitTouch 配对镜子内的两次短点按，覆盖背景命中并容忍手指落点偏移。
        void SetupMirrorExitTouch() {
            // on_click 保留 LVGL 对长按/拖动的过滤，仅为镜子独立放宽双击时间与距离。
            auto on_click = [](lv_event_t* event) {
                auto* self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(event));
                if (self->menu_model_.Page() != watcher::MenuPage::Mirror ||
                    self->touch_revision_ != self->menu_model_.Revision())
                    return;
                auto* input = lv_indev_active();  // 当前短点按所属的正式触摸输入。
                if (!input)
                    return;
                lv_point_t point{};  // 抬手时 LVGL 保留的最后触点。
                lv_indev_get_point(input, &point);
                const uint32_t now = lv_tick_get();  // 时间与显示线程共用，避免跨任务时钟差。
                const int32_t dx = point.x - self->mirror_touch_point_.x;
                const int32_t dy = point.y - self->mirror_touch_point_.y;
                // 600ms 内、48 像素半径内的第二击才退出；不能拼接不同页面或相距很远的点按。
                if (self->mirror_touch_pending_ &&
                    self->mirror_touch_revision_ == self->touch_revision_ &&
                    uint32_t(now - self->mirror_touch_started_) <= 600 &&
                    dx * dx + dy * dy <= 48 * 48) {
                    self->mirror_touch_pending_ = false;
                    if (self->touch_item_) {
                        ESP_LOGI(TAG, "Mirror touch double click: return home");
                        self->touch_item_(watcher::TouchBackIndex, self->touch_revision_);
                    }
                } else {
                    self->mirror_touch_revision_ = self->touch_revision_;
                    self->mirror_touch_started_ = now;
                    self->mirror_touch_point_ = point;
                    self->mirror_touch_pending_ = true;
                }
            };
            for (auto* target : {home_page_.screen, container_, preview_image_}) {
                if (!target)
                    continue;
                lv_obj_add_event_cb(target, on_click, LV_EVENT_SHORT_CLICKED, this);
            }
        }

        // SetupMenu 创建一次中央安全区列表，不导入官方云服务或第二套显示驱动。
        void SetupMenu() {
            if (!menu_events_) return;
            home_page_.screen = lv_display_get_screen_active(display_);
            home_page_.transition = LV_SCR_LOAD_ANIM_MOVE_RIGHT;
            menu_page_.transition = LV_SCR_LOAD_ANIM_MOVE_LEFT;
            SetupMirrorExitTouch();
            // 首页显式菜单按钮，不把触摸表情或字幕误当作对话/拍照。
            home_menu_ = lv_button_create(home_page_.screen);
            lv_obj_set_size(home_menu_, 96, 48);
            lv_obj_align(home_menu_, LV_ALIGN_BOTTOM_MID, 0, -14);
            auto* home_menu_label = lv_label_create(home_menu_);
            lv_label_set_text(home_menu_label, "设置");
            lv_obj_center(home_menu_label);
            lv_obj_add_event_cb(home_menu_, [](lv_event_t* event) {
                auto* self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(event));
                if (self->touch_item_)
                    self->touch_item_(size_t(watcher::MenuTarget::Settings), self->touch_revision_);
            }, LV_EVENT_SHORT_CLICKED, this);
            menu_small_font_ =
                std::make_unique<LvglCBinFont>(const_cast<uint8_t*>(watcher_menu_font_start));
            dial_view_.Create(
                [this](watcher::MenuTarget target, uint32_t revision) {
                    if (touch_item_)
                        touch_item_(static_cast<size_t>(target), revision);
                },
                [this]() { return touch_revision_; },
                [this](watcher::MenuInput input, uint32_t revision) {
                    PostInput(input, false, false, menu_model_.Volume(), menu_model_.Brightness(),
                              charging_, touch_epoch_, revision);
                });
            menu_page_.screen = dial_view_.Screen();
            menu_model_.SyncSkin(watcher::SkinIndex(current_theme_->name()));
            // 与原菜单共用左滑返回和完整抬手门禁。
            lv_obj_add_event_cb(menu_page_.screen, [](lv_event_t* event) {
                auto* self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(event));
                auto* input = lv_indev_active();
                if (!input || lv_indev_get_gesture_dir(input) != LV_DIR_LEFT || !self->touch_item_) return;
                lv_indev_wait_release(input);
                self->touch_item_(watcher::TouchBackIndex, self->touch_revision_);
            }, LV_EVENT_GESTURE, this);
            menu_page_.count = 0;  // 输入焦点由MenuModel统一管理，视图按钮使用稳定目标。
            pages_.Open(home_page_, lv_tick_get(), 0);
            menu_timer_ = lv_timer_create([](lv_timer_t* timer) {
                static_cast<CustomLcdDisplay*>(lv_timer_get_user_data(timer))->PollMenu();
            }, 50, this);
            RenderMenu();
        }
    public:
#if CONFIG_UR_AI_E2E_TEST
        // PrintTestState 只读取显示任务持有的非敏感状态，队列未清空时不能判定操作完成。
        void PrintTestState() {
            DisplayLockGuard lock(this);
            printf("[UR_UI] page=%d focus=%u revision=%lu pending=%u\n",
                static_cast<int>(menu_model_.Page()), static_cast<unsigned>(menu_model_.Focus()),
                static_cast<unsigned long>(menu_model_.Revision()),
                static_cast<unsigned>(uxQueueMessagesWaiting(menu_events_)));
            printf("[UR_TOUCH] available=%d reads=%lu errors=%lu contacts=%lu wakes=%lu events=%lu busy=%lu stale=%lu down=%d\n",
                touch_handle_ != nullptr, static_cast<unsigned long>(touch_reads_),
                static_cast<unsigned long>(touch_errors_), static_cast<unsigned long>(touch_contacts_),
                static_cast<unsigned long>(touch_wakes_), static_cast<unsigned long>(touch_events_),
                static_cast<unsigned long>(touch_busy_), static_cast<unsigned long>(touch_stale_), touch_gate_.Down());
        }
#endif
        // SetBindingFlow 在启动 worker 前设置显示模式，所有 LVGL 更新仍由显示任务消费。
        void SetBindingFlow() { DisplayLockGuard lock(this); binding_flow_ = true; }
        // AttachTouch 复用现有显示任务；唤醒后的整段接触被吞掉，拖动仍由 LVGL 判定。
        void AttachTouch(esp_lcd_touch_handle_t handle, std::function<bool()> wake,
                         std::function<void(size_t, uint32_t)> item) {
            DisplayLockGuard lock(this);
            touch_handle_ = handle;
            touch_wake_ = std::move(wake);
            touch_item_ = std::move(item);
            auto* input = lv_indev_create();
            lv_indev_set_type(input, LV_INDEV_TYPE_POINTER);
            lv_indev_set_display(input, display_);
            lv_indev_set_user_data(input, this);
            lv_indev_set_read_cb(input, [](lv_indev_t* input, lv_indev_data_t* data) {
                auto* self = static_cast<CustomLcdDisplay*>(lv_indev_get_user_data(input));
                esp_lcd_touch_point_data_t point{};
                uint8_t count = 0;
                data->state = LV_INDEV_STATE_RELEASED;
#if CONFIG_UR_AI_E2E_TEST
                ++self->touch_reads_;
#endif
                // I2C 故障取消整次手势，避免将通讯中断误判成一次确认点击。
                if (esp_lcd_touch_read_data(self->touch_handle_) != ESP_OK ||
                    esp_lcd_touch_get_data(self->touch_handle_, &point, &count, 1) != ESP_OK) {
#if CONFIG_UR_AI_E2E_TEST
                    ++self->touch_errors_;
#endif
                    lv_indev_reset(input, nullptr);
                    self->touch_gate_.Sample(false, false, false);
                    self->touch_origin_valid_ = false;
                    return;
                }
                bool wake_only = false;
                if (count > 0 && !self->touch_gate_.Down()) {
                    self->touch_revision_ = self->menu_model_.Revision();
                    self->touch_epoch_ = self->input_epoch_.load();
                    self->touch_started_ = lv_tick_get();
                    self->touch_origin_ = {static_cast<int32_t>(point.x),
                                           static_cast<int32_t>(point.y)};
                    self->touch_origin_valid_ = true;
                    if (self->call_ui_active_ && self->touch_trace_count_++ < 64)
                        ESP_LOGI(TAG, "touch begin x=%u y=%u epoch=%lu revision=%lu", point.x,
                                 point.y, static_cast<unsigned long>(self->touch_epoch_),
                                 static_cast<unsigned long>(self->touch_revision_));
                    wake_only = self->touch_wake_ && self->touch_wake_();
#if CONFIG_UR_AI_E2E_TEST
                    ++self->touch_contacts_;
                    if (wake_only) ++self->touch_wakes_;
#endif
                }
                // 页面或通话模式切换取消持续接触，必须抬手后重新触摸才能执行业务。
                const bool stale = self->touch_origin_valid_ &&
                                   (self->touch_epoch_ != self->input_epoch_.load() ||
                                    self->touch_revision_ != self->menu_model_.Revision());
                if (stale) {
                    lv_indev_reset(input, nullptr);
                    self->touch_gate_.CancelUntilRelease();
                }
                if (count == 0 && self->touch_origin_valid_) {
                    if (self->call_ui_active_ && self->touch_trace_count_++ < 64)
                        ESP_LOGI(TAG,
                                 "touch release x=%ld y=%ld held=%lu stale=%d origin=%lu/%lu "
                                 "current=%lu/%lu",
                                 static_cast<long>(self->touch_origin_.x),
                                 static_cast<long>(self->touch_origin_.y),
                                 static_cast<unsigned long>(lv_tick_get() - self->touch_started_),
                                 stale, static_cast<unsigned long>(self->touch_epoch_),
                                 static_cast<unsigned long>(self->touch_revision_),
                                 static_cast<unsigned long>(self->input_epoch_.load()),
                                 static_cast<unsigned long>(self->menu_model_.Revision()));
                    self->touch_origin_valid_ = false;
                }
                if (!self->touch_gate_.Sample(true, count > 0, wake_only)) return;
                self->menu_model_.TouchActivity(lv_tick_get());
                data->point.x = point.x;
                data->point.y = point.y;
                data->state = LV_INDEV_STATE_PRESSED;
            });
        }
        // PostTouch 将经板级互斥检查的触摸输入交回唯一显示任务。
        void PostTouch(size_t index, uint32_t revision, bool busy, int volume, int brightness, bool charging) {
            if (!menu_events_ || index > watcher::TouchBackIndex) return;
            MenuEvent event{watcher::MenuInput::Click, false, busy, volume, brightness, charging};
            event.input_epoch = input_epoch_.load();
            event.touch_index = static_cast<int>(index);
            event.touch_revision = revision;
            xQueueSend(menu_events_, &event, 0);
        }
        // PostTouchCurrent 以当前页面代次模拟一次触摸选中，供串口验收复用正式输入路径。
        void PostTouchCurrent(size_t index) {
            if (touch_item_)
                touch_item_(index, published_revision_.load());
        }
        // SetTheme 在旧字体所有者释放前重绑全部屏幕和独立状态层；父类仅更新活动屏幕。
        // theme 为资源系统提供的有效主题，空值忽略；隐藏菜单不能保留旧字体的裸指针。
        void SetTheme(Theme* theme) override {
            if (!theme) return;
            DisplayLockGuard lock(this);
            const bool refresh_fonts = theme == current_theme_;  // 资源重载需强制重绑，换色复用原字体。
            // 保留旧资源直到隐藏页面和通话页完成重绑，防止显示样式持有悬空指针。
            auto previous_font = current_theme_ ? static_cast<LvglTheme*>(current_theme_)->text_font() : nullptr;
            auto previous_background = current_theme_ ? static_cast<LvglTheme*>(current_theme_)->background_image() : nullptr;
            watcher::PrepareSkinThemes();
            theme = LvglThemeManager::GetInstance().GetTheme(watcher::SkinFor(theme->name()).id);
            SpiLcdDisplay::SetTheme(theme);
            // 基础主题名称由父类保存，具体皮肤单独保存，旧恢复包不会读到未知主题。
            Settings skin_settings("display", true);
            skin_settings.SetInt("watch_skin", static_cast<int>(watcher::SkinIndex(theme->name())));
            const auto* text_font = static_cast<LvglTheme*>(theme)->text_font()->font();
            if (home_page_.screen) lv_obj_set_style_text_font(home_page_.screen, text_font, 0);
            if (menu_page_.screen) lv_obj_set_style_text_font(menu_page_.screen, text_font, 0);
            if (display_) lv_obj_set_style_text_font(lv_display_get_layer_top(display_), text_font, 0);
            if (dial_view_.Screen())
                dial_view_.SetFonts(text_font, menu_small_font_->font(),
                                    static_cast<LvglTheme*>(theme)->large_icon_font()->font(),
                                    &lv_font_montserrat_48, refresh_fonts);
            ApplySkinColors();
            ApplyPagePalette();
#if CONFIG_UR_AI_SIGNALING_WEBRTC
            watcher::WatcherCallUi::RefreshTheme();
#endif
            if (dial_view_.Screen()) RenderMenu();
        }
        // SelectSkin 在应用任务应用已校验选择，通过原主题设置保存，重复选择不重新刷新。
        void SelectSkin(size_t index) {
            DisplayLockGuard lock(this);
            if (index >= watcher::kSkins.size()) return;
            auto* theme = LvglThemeManager::GetInstance().GetTheme(watcher::kSkins[index].id);
            if (theme && theme != current_theme_) SetTheme(theme);
        }

        // SetChatMessage 接收协议已累积去重的完整快照；用户与助手各自替换，避免重复追加。
        void SetChatMessage(const char* role, const char* text) override {
            std::lock_guard<std::mutex> lock(ui_mutex_);
            const std::string value = text ? text : "";
            if (role && strcmp(role, "user") == 0) { pending_ui_.user = value; pending_ui_.user_dirty = true; }
            else if (role && strcmp(role, "assistant") == 0) { pending_ui_.assistant = value; pending_ui_.assistant_dirty = true; }
            else if (value.empty()) {
                pending_ui_.user.clear(); pending_ui_.assistant.clear();
                pending_ui_.user_dirty = pending_ui_.assistant_dirty = true;
            } else { pending_ui_.status = value; pending_ui_.status_dirty = true; }
        }
        // ClearChatMessages 通过同一邮箱清空两个字幕区。
        void ClearChatMessages() override { SetChatMessage("system", ""); }
        // SetStatus 仅投递状态文本，不在应用或网络任务触碰 LVGL。
        void SetStatus(const char* status) override {
            std::lock_guard<std::mutex> lock(ui_mutex_);
            pending_ui_.status = status ? status : ""; pending_ui_.status_dirty = true;
        }
        // SetPhotoStage 只更新有界状态快照；所有 LVGL 操作仍由 DrainUi 消费。
        void SetPhotoStage(uint32_t generation, PhotoUiStage stage) override {
            std::lock_guard<std::mutex> lock(ui_mutex_);
            if (stage == PhotoUiStage::Capturing) photo_ui_.Begin(generation);
            else photo_ui_.Advance(generation, stage);
        }
        // CancelPhotoStage 保留代次墓碑，取消后的上传完成事件不能重新占住界面。
        void CancelPhotoStage() override {
            std::lock_guard<std::mutex> lock(ui_mutex_);
            photo_ui_.Cancel();
            pending_ui_.status_bar_dirty = true;
        }
        // SetEmotion 仍复用既有 21 类字体/图像资源与协议校验。
        void SetEmotion(const char* emotion) override {
            std::lock_guard<std::mutex> lock(ui_mutex_);
            pending_ui_.emotion = emotion ? emotion : "neutral"; pending_ui_.emotion_dirty = true;
        }
        // SetPreviewImage 只移交图像所有权；相机 worker 不直接更新显示对象。
        void SetPreviewImage(std::unique_ptr<LvglImage> image) override {
            {
                std::lock_guard<std::mutex> lock(ui_mutex_);
                pending_ui_.preview = std::move(image);
                pending_ui_.preview_dirty = true;
            }
            /* 预览帧不再等 50ms 菜单轮询周期：先释放 ui_mutex_ 再拿 LVGL 锁，
             * 避免与 DrainUi（持 LVGL 锁后再取 ui_mutex_）形成锁顺序反转。
             * 唤醒只让下一轮 LVGL 定时器立即执行，实际渲染仍在 LVGL 任务内。 */
            if (menu_timer_ != nullptr) {
                lvgl_port_lock(0);
                lv_timer_ready(menu_timer_);
                lvgl_port_unlock();
            }
        }
        // ShowNotification 使用显示任务时钟，不启动跨任务操作 LVGL 的旧提示定时器。
        void ShowNotification(const char* text, int duration_ms = 3000) override {
            std::lock_guard<std::mutex> lock(ui_mutex_);
            pending_ui_.notification = text ? text : "";
            pending_ui_.notification_dirty = true;
            pending_ui_.notification_ms = duration_ms < 1 ? 1 : duration_ms > 180000 ? 180000 : duration_ms;
        }
        // ShowNotification 字符串入口复用同一快照，避免引用调用者的临时缓冲。
        void ShowNotification(const std::string& text, int duration_ms = 3000) override {
            ShowNotification(text.c_str(), duration_ms);
        }
        // SetNetworkStartup 在事务恢复后发布初始快照；正常重启不重复展示上次成功结果。
        void SetNetworkStartup(bool normal_boot) {
            ur_network_status_t network{};
            ur_network_status(&network);
            std::lock_guard<std::mutex> lock(ui_mutex_);
            network_startup_known_ = true;
            pending_ui_.network = network;
            pending_ui_.network_dirty = true;
            if (normal_boot && network.stage == 7 && !network.blocked)
                pending_ui_.restored_network_request = network.request_id;
        }
        // UpdateStatusBar 仅在启动快照就绪后发布网络事务，避免恢复和显示线程竞态。
        void UpdateStatusBar(bool update_all = false) override {
            ur_network_status_t network{};
            ur_network_status(&network);
            std::lock_guard<std::mutex> lock(ui_mutex_);
            pending_ui_.network = network;
            pending_ui_.network_dirty = network_startup_known_;
            pending_ui_.status_bar_dirty = true;
            pending_ui_.update_all = pending_ui_.update_all || update_all;
        }
        // PostInput 非阻塞复制输入；满队列丢弃本次事件，不阻塞音频或按钮任务。
        void PostInput(watcher::MenuInput input, bool asleep, bool busy, int volume, int brightness,
                       bool charging, uint32_t epoch = UINT32_MAX, uint32_t revision = UINT32_MAX) {
            if (!menu_events_) return;
            MenuEvent event{input, asleep, busy, volume, brightness, charging};
            event.input_epoch = epoch == UINT32_MAX ? input_epoch_.load() : epoch;
            event.touch_revision = revision == UINT32_MAX ? published_revision_.load() : revision;
            xQueueSend(menu_events_, &event, 0);
        }
        // SetMenuButtonVisible 镜子全屏时隐藏主页“菜单”按钮，避免误触退出。
        void SetMenuButtonVisible(bool visible) {
            if (home_menu_ == nullptr) return;
            if (visible) lv_obj_remove_flag(home_menu_, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(home_menu_, LV_OBJ_FLAG_HIDDEN);
        }
        // PostContactsResult 由信令线程投递有界结果，LVGL对象只能在显示定时器中访问。
        void PostContactsResult(UrContactsResult result) {
            std::lock_guard<std::mutex> lock(ui_mutex_);
            pending_contacts_ = std::move(result);
        }
        // OnMenuAction 在板初始化阶段注册业务调度入口。
        void OnMenuAction(std::function<void(watcher::MenuAction, int)> callback) { menu_action_ = std::move(callback); }
        // PostNetworkConfirmation 只排队页面事件；返回失败时调用者撤销请求预留。
        bool PostNetworkConfirmation() {
            MenuEvent event{};
            event.input_epoch = input_epoch_.load();
            event.confirm_network = true;
            return menu_events_ && xQueueSend(menu_events_, &event, 0) == pdTRUE;
        }
        // SetCallUiActive 由通话 UI 在显示任务（或已持有显示锁）调用：true 冻结
        // 菜单渲染让出屏幕；false 菜单模型归位主页并重新加载主页屏幕。
        void SetCallUiActive(bool active) {
            call_ui_active_ = active;
            input_epoch_.fetch_add(1);
            ApplyPagePalette();
            if (active) {
                menu_visible_.store(false);
                return;
            }
            menu_model_.ForceHome();
            menu_model_.SyncSkin(watcher::SkinIndex(current_theme_->name()));
            RenderMenu();
        }
        // InputEpoch 供板级任务捕获输入所属模式，不读取显示任务私有菜单模型。
        uint32_t InputEpoch() const { return input_epoch_.load(); }
        // InputRevision 供非显示任务捕获已发布页面，避免读取菜单模型产生数据竞争。
        uint32_t InputRevision() const { return published_revision_.load(); }
        void PublishMenuVisible(bool visible) {
            menu_visible_.store(visible);
        }  // 先发布再排队应用锁更新。
        bool MenuVisible() const {
            return menu_visible_.load();
        }  // 只发布锁状态，不跨线程读取模型。
        CustomLcdDisplay(esp_lcd_panel_io_handle_t io_handle, 
                        esp_lcd_panel_handle_t panel_handle,
                        int width,
                        int height,
                        int offset_x,
                        int offset_y,
                        bool mirror_x,
                        bool mirror_y,
                        bool swap_xy) 
            : SpiLcdDisplay(io_handle, panel_handle, width, height, offset_x, offset_y, mirror_x, mirror_y, swap_xy) {
            // Note: UI customization should be done in SetupUI(), not in constructor
            // to ensure lvgl objects are created before accessing them
        }

        virtual void SetupUI() override {
            if (IsSetupUICalled()) return;
            // Call parent SetupUI() first to create all lvgl objects
            SpiLcdDisplay::SetupUI();

            DisplayLockGuard lock(this);
            auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
            auto text_font = lvgl_theme->text_font()->font();
            auto icon_font = lvgl_theme->icon_font()->font();
            const auto& skin = watcher::SkinFor(lvgl_theme->name());  // 启动布局也使用已保存的皮肤。

            lv_obj_set_size(top_bar_, LV_HOR_RES, text_font->line_height);
            lv_obj_set_style_layout(top_bar_, LV_LAYOUT_NONE, 0);
            lv_obj_set_style_pad_top(top_bar_, 10, 0);
            lv_obj_set_style_pad_bottom(top_bar_, 1, 0);

            lv_obj_set_size(status_bar_, LV_HOR_RES, text_font->line_height);
            lv_obj_set_style_layout(status_bar_, LV_LAYOUT_NONE, 0);
            lv_obj_set_style_pad_top(status_bar_, 10, 0);
            lv_obj_set_style_pad_bottom(status_bar_, 1, 0);
            lv_obj_set_y(status_bar_, text_font->line_height);
            lv_obj_add_flag(status_bar_, LV_OBJ_FLAG_IGNORE_LAYOUT);

            // Reparent mute and battery labels to top_bar_ to allow absolute positioning
            lv_obj_set_parent(mute_label_, top_bar_);
            lv_obj_set_parent(battery_label_, top_bar_);
            lv_obj_set_style_margin_left(battery_label_, 0, 0);

            // 针对圆形屏幕调整位置
            //      network  mute  battery     //
            //               status            //
            lv_obj_align(network_label_, LV_ALIGN_TOP_MID, -1.5 * icon_font->line_height, 0);
            lv_obj_align(mute_label_, LV_ALIGN_TOP_MID, 1.0 * icon_font->line_height, 0);
            lv_obj_align(battery_label_, LV_ALIGN_TOP_MID, 2.5 * icon_font->line_height, 0);
            
            lv_obj_align(status_label_, LV_ALIGN_BOTTOM_MID, 0, 0);
            lv_obj_set_flex_grow(status_label_, 0);
            lv_obj_set_width(status_label_, LV_HOR_RES * 0.75);
            lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);

            lv_obj_align(notification_label_, LV_ALIGN_BOTTOM_MID, 0, 0);
            lv_obj_set_width(notification_label_, LV_HOR_RES * 0.75);
            lv_label_set_long_mode(notification_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);

            lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, -20);
            lv_obj_set_style_bg_color(low_battery_popup_, lv_color_hex(0xFF0000), 0);
            lv_obj_set_width(low_battery_label_, LV_HOR_RES * 0.75);
            lv_label_set_long_mode(low_battery_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);

            // 针对圆形屏幕调整底部对话框位置，避免被圆角遮挡
            lv_obj_set_style_pad_bottom(bottom_bar_, 30, 0);
            lv_obj_set_width(chat_message_label_, LV_HOR_RES * 0.75); // 限制宽度，避免文字贴边
            // 圆屏上下弦宽较窄，状态内收，字幕与预览放入中部 288px 安全区。
            lv_obj_set_size(top_bar_, 230, 35);
            lv_obj_set_style_pad_all(top_bar_, 0, 0);
            lv_obj_set_style_bg_opa(top_bar_, LV_OPA_TRANSP, 0);
            // 状态层独立于具体页面，菜单中也能看到网络、电量及操作拒绝原因。
            lv_obj_set_parent(top_bar_, lv_display_get_layer_top(display_));
            lv_obj_set_parent(status_bar_, lv_display_get_layer_top(display_));
            lv_obj_set_parent(low_battery_popup_, lv_display_get_layer_top(display_));
            lv_obj_align(top_bar_, LV_ALIGN_TOP_MID, 0, 35);
            lv_obj_set_size(status_bar_, 260, 48);
            lv_obj_set_style_pad_all(status_bar_, 0, 0);
            // 30px 字体实际行高为 43px：字幕至 y=284，状态至 y=340，菜单从 y=350 开始。
            lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, 292);
            lv_obj_set_width(status_label_, 250);
            lv_obj_set_width(notification_label_, 250);
            lv_obj_set_size(bottom_bar_, 288, 144);
            lv_obj_set_style_pad_all(bottom_bar_, 10, 0);
            lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(container_, lv_color_hex(skin.background), 0);
            lv_obj_set_style_bg_color(bottom_bar_, lv_color_hex(skin.surface), 0);
            lv_obj_set_style_text_color(chat_message_label_, lv_color_hex(skin.text), 0);
            lv_obj_set_style_text_color(status_label_, lv_color_hex(skin.muted), 0);
            lv_obj_set_style_text_color(notification_label_, lv_color_hex(skin.text), 0);
            lv_obj_set_style_text_color(network_label_, lv_color_hex(skin.muted), 0);
            lv_obj_set_style_text_color(battery_label_, lv_color_hex(skin.muted), 0);
            lv_obj_set_style_text_color(mute_label_, lv_color_hex(skin.muted), 0);
            lv_obj_set_flex_flow(bottom_bar_, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_style_pad_row(bottom_bar_, 12, 0);
            lv_obj_set_scroll_dir(bottom_bar_, LV_DIR_VER);
            lv_obj_set_width(chat_message_label_, 260);
            lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_LEFT, 0);
            user_subtitle_ = lv_label_create(bottom_bar_);
            lv_obj_set_width(user_subtitle_, 260);
            lv_obj_set_style_text_color(user_subtitle_, lv_color_hex(skin.text), 0);
            lv_label_set_long_mode(user_subtitle_, LV_LABEL_LONG_WRAP);
            lv_label_set_text(user_subtitle_, "");
            lv_obj_add_flag(user_subtitle_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_to_index(user_subtitle_, 0);
            lv_obj_align(bottom_bar_, LV_ALIGN_TOP_MID, 0, 140);
            lv_obj_set_size(low_battery_popup_, 260, 80);
            lv_obj_center(low_battery_popup_);
            lv_obj_set_width(low_battery_label_, 240);
            lv_obj_center(low_battery_label_);
            SetupMenu();
            ApplySkinColors();
            ApplyPagePalette();
        }
};

class SensecapWatcher : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    CustomLcdDisplay* display_ = nullptr;  // 输入初始化早于显示，回调须检查就绪。
    std::unique_ptr<Knob> knob_;
    std::atomic<uint32_t> knob_press_epoch_{0};     // 按下时模式绑定到延迟识别的单击、双击与长按。
    std::atomic<uint32_t> knob_press_revision_{0};  // 按下时页面代次，禁止抬手后换绑新页面。
    esp_io_expander_handle_t io_exp_handle;
    button_handle_t btns;
    PowerSaveTimer* power_save_timer_;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    i2c_master_bus_handle_t touch_bus_ = nullptr;  // 触摸独占 I2C1，不与音频 I2C0 争用。
    esp_lcd_panel_io_handle_t touch_io_ = nullptr;  // 触摸通信句柄随板实例存活。
    esp_lcd_touch_handle_t touch_ = nullptr;  // 不额外创建显示驱动。
    std::atomic<bool> screen_asleep_{false};  // 首次唤醒手势不触发业务动作。
    std::atomic<bool> network_flow_{false};  // 独占换网期间禁止普通按钮业务和息屏。
    std::string network_confirmation_id_;  // 当前确认页关联请求，仅应用任务读写。
    button_driver_t* btn_driver_ = nullptr;
    std::mutex io_input_mutex_;  // 同步扩展器读取及共享INT消除后的输入快照。
    uint32_t io_input_cache_ = DRV_IO_EXP_INPUT_MASK;  // 未取得真实样本前按键默认为松开。
    int64_t io_input_at_ = -20000;  // 最近成功读取时间，按钮兜底20ms刷新。
    unsigned io_input_reads_ = 0, knob_cache_hits_ = 0, io_input_failures_ = 0;  // 本地轮询取证。
    static SensecapWatcher* instance_;
    SscmaCamera* camera_ = nullptr;

    void InitializePowerSaveTimer() {
        power_save_timer_ = new PowerSaveTimer(-1, 60, 300);
        power_save_timer_->OnEnterSleepMode([this]() {
            screen_asleep_.store(true);
            GetDisplay()->SetPowerSaveMode(true);
            GetBacklight()->SetBrightness(10);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            screen_asleep_.store(false);
            GetDisplay()->SetPowerSaveMode(false);
            GetBacklight()->RestoreBrightness();
        });
        power_save_timer_->OnShutdownRequest([this]() {
            ESP_LOGI(TAG, "Shutting down");
            bool is_charging = (IoExpanderGetLevel(BSP_PWR_VBUS_IN_DET) == 0);
            if (is_charging) {
                ESP_LOGI(TAG, "charging");
                GetBacklight()->SetBrightness(0);
            } else {
                IoExpanderSetLevel(BSP_PWR_SYSTEM, 0);
            }
        });
        // USB 供电时保持屏幕与语音入口可用；拔掉 USB 后由电池状态回调重新启用省电计时。
        const bool is_charging = (IoExpanderGetLevel(BSP_PWR_VBUS_IN_DET) == 0);
        power_save_timer_->SetEnabled(!is_charging);
    }

    void InitializeI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)0,
            .sda_io_num = BSP_GENERAL_I2C_SDA,
            .scl_io_num = BSP_GENERAL_I2C_SCL,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));

        // pulldown for lcd i2c
        const gpio_config_t io_config = {
            .pin_bit_mask = (1ULL << BSP_TOUCH_I2C_SDA) | (1ULL << BSP_TOUCH_I2C_SCL) | (1ULL << BSP_SPI3_HOST_PCLK) | (1ULL << BSP_SPI3_HOST_DATA0) | (1ULL << BSP_SPI3_HOST_DATA1)
                            | (1ULL << BSP_SPI3_HOST_DATA2) | (1ULL << BSP_SPI3_HOST_DATA3) | (1ULL << BSP_LCD_SPI_CS) | (1UL << DISPLAY_BACKLIGHT_PIN),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io_config);

        gpio_set_level(BSP_TOUCH_I2C_SDA, 0);
        gpio_set_level(BSP_TOUCH_I2C_SCL, 0);
    
        gpio_set_level(BSP_LCD_SPI_CS, 0);
        gpio_set_level(DISPLAY_BACKLIGHT_PIN, 0);
        gpio_set_level(BSP_SPI3_HOST_PCLK, 0);
        gpio_set_level(BSP_SPI3_HOST_DATA0, 0);
        gpio_set_level(BSP_SPI3_HOST_DATA1, 0);
        gpio_set_level(BSP_SPI3_HOST_DATA2, 0);
        gpio_set_level(BSP_SPI3_HOST_DATA3, 0);

    }

    esp_err_t IoExpanderSetLevel(uint16_t pin_mask, uint8_t level) {
        return esp_io_expander_set_level(io_exp_handle, pin_mask, level);
    }

    // IoExpanderGetLevel 按原语义实时读取指定输入；成功时更新被共享INT确认的快照。
    uint8_t IoExpanderGetLevel(uint16_t pin_mask) {
        std::lock_guard<std::mutex> lock(io_input_mutex_);
        uint32_t pin_val = 0;
        ++io_input_reads_;
        if (esp_io_expander_get_level(io_exp_handle, DRV_IO_EXP_INPUT_MASK, &pin_val) == ESP_OK) {
            io_input_cache_ = pin_val;
            io_input_at_ = esp_timer_get_time();
        } else {
            ++io_input_failures_;
        }
        pin_mask &= DRV_IO_EXP_INPUT_MASK;
        return (uint8_t)((pin_val & pin_mask) ? 1 : 0);
    }

    // ReadKnobButton 返回滚轮是否按下；保持5ms消抖采样，INT变更即读，空闲最多20ms刷新。
    uint8_t ReadKnobButton() {
        std::lock_guard<std::mutex> lock(io_input_mutex_);
        const auto now = esp_timer_get_time();  // 单调时钟仅用于限制无变化时的I2C开销。
        if (gpio_get_level(BSP_IO_EXPANDER_INT) == 0 || now < io_input_at_ ||
            now - io_input_at_ >= 20000) {
            uint32_t pin_val = 0;
            ++io_input_reads_;
            if (esp_io_expander_get_level(io_exp_handle, DRV_IO_EXP_INPUT_MASK, &pin_val) == ESP_OK) {
                io_input_cache_ = pin_val;
                io_input_at_ = now;
            } else {
                // 读失败不制造按下边沿；保留最后成功样本，下一原采样tick继续重试。
                ++io_input_failures_;
            }
        } else {
            ++knob_cache_hits_;
        }
        return (io_input_cache_ & BSP_KNOB_BTN) == 0;
    }

    // TraceButtonPoll 在显式本地诊断中输出读取/复用计数；reset只清统计，不改变输入状态。
    void TraceButtonPoll(bool reset) {
        std::lock_guard<std::mutex> lock(io_input_mutex_);
        printf("[UR_BUTTON_POLL] read_n=%u cache_n=%u failed_n=%u irq_low=%d\n",
               io_input_reads_, knob_cache_hits_, io_input_failures_,
               gpio_get_level(BSP_IO_EXPANDER_INT) == 0);
        if (reset) io_input_reads_ = knob_cache_hits_ = io_input_failures_ = 0;
    }

    void InitializeExpander() {
        esp_err_t ret = ESP_OK;
        esp_io_expander_new_i2c_tca95xx_16bit(i2c_bus_, ESP_IO_EXPANDER_I2C_TCA9555_ADDRESS_001, &io_exp_handle);

        ret |= esp_io_expander_set_dir(io_exp_handle, DRV_IO_EXP_INPUT_MASK, IO_EXPANDER_INPUT);
        ret |= esp_io_expander_set_dir(io_exp_handle, DRV_IO_EXP_OUTPUT_MASK, IO_EXPANDER_OUTPUT);
        ret |= esp_io_expander_set_level(io_exp_handle, DRV_IO_EXP_OUTPUT_MASK, 0);
        ret |= esp_io_expander_set_level(io_exp_handle, BSP_PWR_SYSTEM, 1);
        vTaskDelay(100 / portTICK_PERIOD_MS);
        ret |= esp_io_expander_set_level(io_exp_handle, BSP_PWR_START_UP, 1);
        vTaskDelay(50 / portTICK_PERIOD_MS);
    
        uint32_t pin_val = 0;
        ret |= esp_io_expander_get_level(io_exp_handle, DRV_IO_EXP_INPUT_MASK, &pin_val);
        ESP_LOGI(TAG, "IO expander initialized: %x", DRV_IO_EXP_OUTPUT_MASK | (uint16_t)pin_val);
    
        // 扩展器现有INT线为开漏低有效，只读GPIO；沿用板级引脚，不新增ISR或任务。
        const gpio_config_t input_irq = {
            .pin_bit_mask = 1ULL << BSP_IO_EXPANDER_INT,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ret |= gpio_config(&input_irq);
        assert(ret == ESP_OK);
    }

    // ApplyMenuVolume 应用模型给定百分比并展示硬件回读值；边界滚动也反馈当前音量。
    void ApplyMenuVolume(int requested_volume) {
        auto codec = GetAudioCodec();
        codec->SetOutputVolume(std::clamp(requested_volume, 0, 100));
        /* 本地音量变更立即上报物模型，平台状态不必等 60s 周期上报。 */
        ur_device_request_report();
        GetDisplay()->ShowNotification(std::string(Lang::Strings::VOLUME) + ": " +
                                      std::to_string(codec->output_volume()) + "%", 2000);
    }

    void InitializeKnob() {
        knob_ = std::make_unique<Knob>(BSP_KNOB_A_PIN, BSP_KNOB_B_PIN);
        knob_->OnRotate([this](bool clockwise) {
            ESP_LOGD(TAG, "Knob rotation detected. Clockwise:%s", clockwise ? "true" : "false");
            DispatchInput(clockwise ? watcher::MenuInput::Next : watcher::MenuInput::Previous);
        });
        ESP_LOGI(TAG, "Knob initialized with pins A:%d B:%d", BSP_KNOB_A_PIN, BSP_KNOB_B_PIN);
    }

    void InitializeButton() {
        // 设置静态实例指针
        instance_ = this;
        
        // watcher 是通过长按滚轮进行开机的, 需要等待滚轮释放, 否则用户开机松手时可能会误触成单击
        ESP_LOGI(TAG, "waiting for knob button release");
        while(IoExpanderGetLevel(BSP_KNOB_BTN) == 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        button_config_t btn_config = {
            .long_press_time = 2000,
            .short_press_time = 0
        };
        btn_driver_ = (button_driver_t*)calloc(1, sizeof(button_driver_t));
        btn_driver_->enable_power_save = false;
        btn_driver_->get_key_level = [](button_driver_t *button_driver) -> uint8_t {
            return instance_->ReadKnobButton();
        };
        
        ESP_ERROR_CHECK(iot_button_create(&btn_config, btn_driver_, &btns));

        // 在手势识别之前保存来源，双击等待窗口内的页面切换不能改变按键含义。
        iot_button_register_cb(
            btns, BUTTON_PRESS_DOWN, nullptr,
            [](void*, void* usr_data) {
                auto* self =
                    static_cast<SensecapWatcher*>(usr_data);  // 驱动回调只读取原子显示快照。
                self->knob_press_epoch_.store(self->display_ ? self->display_->InputEpoch() : 0);
                self->knob_press_revision_.store(self->display_ ? self->display_->InputRevision()
                                                                : 0);
            },
            this);
        iot_button_register_cb(
            btns, BUTTON_SINGLE_CLICK, nullptr,
            [](void* button_handle, void* usr_data) {
                auto* self = static_cast<SensecapWatcher*>(usr_data);  // 手势沿用按下时的显示归属。
                self->DispatchInput(watcher::MenuInput::Click, self->knob_press_epoch_.load(),
                                    self->knob_press_revision_.load());
            },
            this);

#if CONFIG_UR_AI
        /* iot_button 在注册双击后会延迟确认单击，两个回调不会同时触发。 */
        iot_button_register_cb(
            btns, BUTTON_DOUBLE_CLICK, nullptr,
            [](void* button_handle, void* usr_data) {
                auto* self = static_cast<SensecapWatcher*>(usr_data);  // 手势沿用按下时的显示归属。
                self->DispatchInput(watcher::MenuInput::DoubleClick, self->knob_press_epoch_.load(),
                                    self->knob_press_revision_.load());
            },
            this);
#endif

        iot_button_register_cb(
            btns, BUTTON_LONG_PRESS_START, nullptr,
            [](void* button_handle, void* usr_data) {
                auto* self = static_cast<SensecapWatcher*>(usr_data);  // 手势沿用按下时的显示归属。
                self->DispatchInput(watcher::MenuInput::LongPress, self->knob_press_epoch_.load(),
                                    self->knob_press_revision_.load());
            },
            this);
    }

    // DispatchInput 先捕获睡眠状态，再投递主任务读取硬件；显示任务仅消费值快照。
    void DispatchInput(watcher::MenuInput input, uint32_t origin_epoch = UINT32_MAX,
                       uint32_t origin_revision = UINT32_MAX) {
        const bool asleep = screen_asleep_.load();
        const auto epoch = origin_epoch == UINT32_MAX ? (display_ ? display_->InputEpoch() : 0)
                                                      : origin_epoch;  // 主任务排队不能换绑新来电。
        const auto revision = origin_revision == UINT32_MAX
                                  ? (display_ ? display_->InputRevision() : 0)
                                  : origin_revision;  // 保留原页面代次。
        Application::GetInstance().Schedule([this, input, asleep, epoch, revision]() {
            if (!display_)
                return;
            power_save_timer_->WakeUp();
            ur_network_status_t status{};
            ur_network_status(&status);
            // 受理阶段也可能遇到存储故障，不能只在独占启动模式提供恢复入口。
            if (status.blocked) {
                if (!asleep && (input == watcher::MenuInput::Click || input == watcher::MenuInput::LongPress))
                    Application::GetInstance().Reboot();
                return;
            }
            if (network_flow_.load() && status.stage != 7) {
                // 活动换网页不进入普通菜单；单击/长按仅取消，睡眠唤醒仍只唤醒。
                if (!asleep && (input == watcher::MenuInput::Click || input == watcher::MenuInput::LongPress)) {
                    if (ur_network_fail(UR_NETWORK_ERROR_CANCELLED) == UR_NETWORK_ROLLBACK)
                        esp_restart();
                    else
                        display_->ShowNotification("配置存储异常，请重启重试");
                }
                return;
            }
            ur_ota_status_t ota{};
            ur_bridge_get_ota_status(&ota);
            bool busy = ota.state == 1 || ota.state == 2;
#if CONFIG_UR_AI
            busy = busy || ur_bridge_photo_busy();
#endif
            display_->PostInput(input, asleep, busy, GetAudioCodec()->output_volume(),
                                GetBacklight()->brightness(),
                                IoExpanderGetLevel(BSP_PWR_VBUS_IN_DET) == 0, epoch, revision);
        });
    }

#if CONFIG_UR_AI_E2E_TEST
    // DispatchTestInput 将测试命令投递到真实板级输入入口；返回 false 表示非输入命令。
    bool DispatchTestInput(const char* operation) {
        if (!operation) return false;
        watcher::MenuInput input;
        if (strcmp(operation, "press") == 0) input = watcher::MenuInput::Click;
        else if (strcmp(operation, "double-press") == 0) input = watcher::MenuInput::DoubleClick;
        else if (strcmp(operation, "long-press") == 0) input = watcher::MenuInput::LongPress;
        else if (strcmp(operation, "next") == 0) input = watcher::MenuInput::Next;
        else if (strcmp(operation, "previous") == 0) input = watcher::MenuInput::Previous;
        else return false;
        DispatchInput(input);
        return true;
    }
#endif

    // ExecuteMenuAction 在应用任务执行设置和安全操作，LVGL 回调不得调用阻塞硬件接口。
    void ExecuteMenuAction(watcher::MenuAction action, int value) {
        auto& app = Application::GetInstance();
        switch (action) {
            case watcher::MenuAction::FinishNetwork: {
                // 仅已可靠保存的成功终态允许提前结束结果窗口；不清凭据、不回滚网络。
                ur_network_status_t status{};
                ur_network_status(&status);
                if (status.stage == 7 && !status.blocked) {
                    if (network_flow_.load()) app.Reboot();
                    else display_->PostInput(watcher::MenuInput::NetworkDone, false, false,
                        GetAudioCodec()->output_volume(), GetBacklight()->brightness(),
                        IoExpanderGetLevel(BSP_PWR_VBUS_IN_DET) == 0);
                }
                break;
            }
            case watcher::MenuAction::Visibility:
                app.SetInteractionBlocked(value != 0 || !network_confirmation_id_.empty() || network_flow_.load());
                break;
            case watcher::MenuAction::Chat: app.ToggleChatState(); break;
            case watcher::MenuAction::CloseChat: app.EndConversation(); break;
#if CONFIG_UR_AI
            case watcher::MenuAction::Photo: app.TakePhoto(); break;
#endif
            case watcher::MenuAction::Mirror: {
                // 菜单模型传递目标状态，重复开启不再误关；相机启停在专用任务串行完成。
                if (!SetMirrorEnabled(GetCamera(), value != 0) && display_) {
                    ESP_LOGE("ur_mirror", "mirror unavailable or task allocation failed");
                    display_->PostInput(watcher::MenuInput::MirrorOff, false, false, 50, 50, false);
                    display_->ShowNotification("镜子暂不可用");
                }
                break;
            }
            case watcher::MenuAction::Skin:
                display_->SelectSkin(static_cast<size_t>(value));
                break;
            case watcher::MenuAction::Volume: ApplyMenuVolume(value); break;
            case watcher::MenuAction::CallVolumeDelta:
                ApplyMenuVolume(GetAudioCodec()->output_volume() + value);
                break;
            case watcher::MenuAction::Brightness:
                GetBacklight()->SetBrightness(value, true);
                /* 本地亮度变更立即上报物模型，避免平台读到旧值。 */
                ur_device_request_report();
                break;
            case watcher::MenuAction::Power:
                if (IoExpanderGetLevel(BSP_PWR_VBUS_IN_DET) == 0) app.Reboot();
                else { IoExpanderSetLevel(BSP_PWR_LCD, 0); IoExpanderSetLevel(BSP_PWR_SYSTEM, 0); }
                break;
            case watcher::MenuAction::Reset:
                ur_bridge_clear_device_info();
                app.Reboot();
                break;
            case watcher::MenuAction::ChangeNetwork:
                BeginNetworkChange();
                break;
            case watcher::MenuAction::ProvisionBind:
                BeginNetworkChange(nullptr, true);
                break;
            case watcher::MenuAction::ConfirmNetwork:
                BeginNetworkChange(network_confirmation_id_.c_str());
                ur_network_cancel_request(network_confirmation_id_.c_str(), UR_NETWORK_ERROR_CANCELLED);
                network_confirmation_id_.clear();
                app.SetInteractionBlocked(false);
                ur_device_request_report();
                break;
            case watcher::MenuAction::CancelNetwork:
            case watcher::MenuAction::NetworkTimeout:
                ur_network_cancel_request(network_confirmation_id_.c_str(),
                    action == watcher::MenuAction::NetworkTimeout ? UR_NETWORK_ERROR_TIMEOUT : UR_NETWORK_ERROR_CANCELLED);
                network_confirmation_id_.clear();
                app.SetInteractionBlocked(false);
                ur_device_request_report();
                break;
            default: break;
        }
    }

    // BeginNetworkChange 只由本地网络菜单确认触发，保留身份；不允许在未绑定设备上冒充换网。
    void BeginNetworkChange(const char* confirmed_id = nullptr, bool binding = false) {
#if CONFIG_UR_BLE_PROVISION
        ur_ota_status_t ota{};
        ur_bridge_get_ota_status(&ota);
        bool busy = ota.state == 1 || ota.state == 2 || (!confirmed_id && ur_network_protect_credentials());
#if CONFIG_UR_AI
        busy = busy || ur_bridge_photo_busy();
#endif
        if (busy) { display_->ShowNotification("设备忙，请稍后换网"); return; }
        Settings device("ur_dev", false);
        if (!binding && device.GetString("secret").empty()) {
            display_->ShowNotification("请先完成首次配网绑定");
            return;
        }
        char request_id[17]{};
        if (confirmed_id) snprintf(request_id, sizeof(request_id), "%s", confirmed_id);
        else snprintf(request_id, sizeof(request_id), "%08lx%08lx",
                      static_cast<unsigned long>(esp_random()), static_cast<unsigned long>(esp_random()));
        if (!ur_bridge_begin_network_change(request_id)) {
            display_->ShowNotification("无法保存换网请求，请重试");
            return;
        }
        Application::GetInstance().SetInteractionBlocked(true);
        if (binding) { ur_network_restart_binding(); return; }
        ur_network_restart();
#else
        display_->ShowNotification("当前固件不支持蓝牙换网");
#endif
    }

    void InitializeSpi() {
        ESP_LOGI(TAG, "Initialize SSCMA SPI bus");
        spi_bus_config_t spi_cfg = {0};

        spi_cfg.mosi_io_num = BSP_SPI2_HOST_MOSI;
        spi_cfg.miso_io_num = BSP_SPI2_HOST_MISO;
        spi_cfg.sclk_io_num = BSP_SPI2_HOST_SCLK;
        spi_cfg.quadwp_io_num = -1;
        spi_cfg.quadhd_io_num = -1;
        spi_cfg.isr_cpu_id = ESP_INTR_CPU_AFFINITY_1;
        spi_cfg.max_transfer_sz = 4095;
   
        ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &spi_cfg, SPI_DMA_CH_AUTO));

        ESP_LOGI(TAG, "Initialize QSPI bus");

        spi_bus_config_t qspi_cfg = {0};
        qspi_cfg.sclk_io_num = BSP_SPI3_HOST_PCLK;
        qspi_cfg.data0_io_num = BSP_SPI3_HOST_DATA0;
        qspi_cfg.data1_io_num = BSP_SPI3_HOST_DATA1;
        qspi_cfg.data2_io_num = BSP_SPI3_HOST_DATA2;
        qspi_cfg.data3_io_num = BSP_SPI3_HOST_DATA3;
        qspi_cfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * DRV_LCD_BITS_PER_PIXEL / 8 / CONFIG_BSP_LCD_SPI_DMA_SIZE_DIV;
    
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &qspi_cfg, SPI_DMA_CH_AUTO));
    }

    // InitializeTouch 使用与官方硬件一致的管脚，通过 IDF 6 主机驱动接入现有 LVGL。
    void InitializeTouch() {
        i2c_master_bus_config_t bus{};
        bus.i2c_port = I2C_NUM_1;
        bus.sda_io_num = BSP_TOUCH_I2C_SDA;
        bus.scl_io_num = BSP_TOUCH_I2C_SCL;
        bus.clk_source = I2C_CLK_SRC_DEFAULT;
        bus.glitch_ignore_cnt = 7;
        bus.flags.enable_internal_pullup = true;
        esp_err_t result = i2c_new_master_bus(&bus, &touch_bus_);
        // 驱动宏使用 C 指定初始化顺序，与 IDF 6 的 C++ 字段顺序不兼容；逐项保留相同值。
        esp_lcd_panel_io_i2c_config_t io{};
        io.dev_addr = ESP_LCD_TOUCH_IO_I2C_SPD2010_ADDRESS;
        io.control_phase_bytes = 1;
        io.flags.disable_control_phase = true;
        io.scl_speed_hz = BSP_TOUCH_I2C_CLK;
        if (result == ESP_OK) result = esp_lcd_new_panel_io_i2c(touch_bus_, &io, &touch_io_);
        esp_lcd_touch_config_t config{};
        config.x_max = DISPLAY_WIDTH;
        config.y_max = DISPLAY_HEIGHT;
        config.rst_gpio_num = GPIO_NUM_NC;
        config.int_gpio_num = GPIO_NUM_NC;
        config.flags.swap_xy = DISPLAY_SWAP_XY;
        config.flags.mirror_x = DISPLAY_MIRROR_X;
        config.flags.mirror_y = DISPLAY_MIRROR_Y;
        if (result == ESP_OK) result = esp_lcd_touch_new_i2c_spd2010(touch_io_, &config, &touch_);
        if (result != ESP_OK) {
            // 触摸是可选输入；故障时保留滚轮，不能让整机语音和联网陷入重启循环。
            if (touch_io_) { esp_lcd_panel_io_del(touch_io_); touch_io_ = nullptr; }
            if (touch_bus_) { i2c_del_master_bus(touch_bus_); touch_bus_ = nullptr; }
            ESP_LOGW(TAG, "Touch unavailable: %s; keep roller input", esp_err_to_name(result));
            return;
        }
        // 控制器与 LCD 共享复位，沿用板厂初始化后的首读要求。
        vTaskDelay(pdMS_TO_TICKS(50));
        esp_lcd_touch_read_data(touch_);
        vTaskDelay(pdMS_TO_TICKS(100));
        display_->AttachTouch(touch_, [this]() {
            const bool asleep = screen_asleep_.load();
            Application::GetInstance().Schedule([this]() { power_save_timer_->WakeUp(); });
            return asleep;
        }, [this](size_t index, uint32_t revision) {
            Application::GetInstance().Schedule([this, index, revision]() {
                ur_ota_status_t ota{};
                ur_bridge_get_ota_status(&ota);
                ur_network_status_t network{};
                ur_network_status(&network);
                bool busy = (network_flow_.load() && network.stage != 7) || network.blocked || ota.state == 1 || ota.state == 2;
#if CONFIG_UR_AI
                busy = busy || ur_bridge_photo_busy();
#endif
                display_->PostTouch(index, revision, busy, GetAudioCodec()->output_volume(),
                    GetBacklight()->brightness(), IoExpanderGetLevel(BSP_PWR_VBUS_IN_DET) == 0);
            });
        });
    }

    void Initializespd2010Display() {
        ESP_LOGI(TAG, "Install panel IO");
        const esp_lcd_panel_io_spi_config_t io_config = {
            .cs_gpio_num = BSP_LCD_SPI_CS,
            .dc_gpio_num = GPIO_NUM_NC,
            .spi_mode = 3,
            .pclk_hz = DRV_LCD_PIXEL_CLK_HZ,
            .trans_queue_depth = 2,
            .lcd_cmd_bits = DRV_LCD_CMD_BITS,
            .lcd_param_bits = DRV_LCD_PARAM_BITS,
            .flags = {
                .quad_mode = true,
            },
        };
        spd2010_vendor_config_t vendor_config = {
            .flags = {
                .use_qspi_interface = 1,
            },
        };
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BSP_LCD_SPI_NUM, &io_config, &panel_io_);
    
        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.rgb_ele_order = DRV_LCD_RGB_ELEMENT_ORDER;
        panel_config.bits_per_pixel = DRV_LCD_BITS_PER_PIXEL;
        panel_config.reset_gpio_num = BSP_LCD_GPIO_RST; // Shared with Touch reset
        panel_config.vendor_config = &vendor_config;
        esp_lcd_new_panel_spd2010(panel_io_, &panel_config, &panel_);

        esp_lcd_panel_reset(panel_);
        esp_lcd_panel_init(panel_);
        esp_lcd_panel_mirror(panel_, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        esp_lcd_panel_disp_on_off(panel_, true);

        display_ = new CustomLcdDisplay(panel_io_, panel_,
            DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
        
        // 使每次刷新的起始列数索引是4的倍数且列数总数是4的倍数，以满足SPD2010的要求
        lv_display_add_event_cb(lv_display_get_default(), [](lv_event_t *e) {
            lv_area_t *area = (lv_area_t *)lv_event_get_param(e);
            uint16_t x1 = area->x1;
            uint16_t x2 = area->x2;
            // round the start of area down to the nearest 4N number
            area->x1 = (x1 >> 2) << 2;
            // round the end of area up to the nearest 4M+3 number
            area->x2 = ((x2 >> 2) << 2) + 3;
        }, LV_EVENT_INVALIDATE_AREA, NULL);
        
    }

    uint16_t BatterygetVoltage(void) {
        static bool initialized = false;
        static adc_oneshot_unit_handle_t adc_handle;
        static adc_cali_handle_t cali_handle = NULL;
        if (!initialized) {
            adc_oneshot_unit_init_cfg_t init_config = {
                .unit_id = ADC_UNIT_1,
            };
            adc_oneshot_new_unit(&init_config, &adc_handle);
    
            adc_oneshot_chan_cfg_t ch_config = {
                .atten = BSP_BAT_ADC_ATTEN,
                .bitwidth = ADC_BITWIDTH_DEFAULT,
            };
            adc_oneshot_config_channel(adc_handle, BSP_BAT_ADC_CHAN, &ch_config);
    
            adc_cali_curve_fitting_config_t cali_config = {
                .unit_id = ADC_UNIT_1,
                .chan = BSP_BAT_ADC_CHAN,
                .atten = BSP_BAT_ADC_ATTEN,
                .bitwidth = ADC_BITWIDTH_DEFAULT,
            };
            if (adc_cali_create_scheme_curve_fitting(&cali_config, &cali_handle) == ESP_OK) {
                initialized = true;
            }
        }
        if (initialized) {
            int raw_value = 0;
            int voltage = 0; // mV
            adc_oneshot_read(adc_handle, BSP_BAT_ADC_CHAN, &raw_value);
            adc_cali_raw_to_voltage(cali_handle, raw_value, &voltage);
            voltage = voltage * 82 / 20;
            // ESP_LOGI(TAG, "voltage: %dmV", voltage);
            return (uint16_t)voltage;
        }
        return 0;
    }

    uint8_t BatterygetPercent(bool print = false) {
        int voltage = 0;
        for (uint8_t i = 0; i < 10; i++) {
            voltage += BatterygetVoltage();
        }
        voltage /= 10;
        int percent = (-1 * voltage * voltage + 9016 * voltage - 19189000) / 10000;
        percent = (percent > 100) ? 100 : (percent < 0) ? 0 : percent;
        if (print) {
            printf("voltage: %dmV, percentage: %d%%\r\n", voltage, percent);
        }
        return (uint8_t)percent;
    }

    void InitializeCmd() {
        esp_console_repl_t *repl = NULL;
        esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
#if CONFIG_UR_AI_E2E_TEST
        // 截图会递归渲染 LVGL；默认 4KB 控制台栈在 page_begin 后损坏回溯。
        // 测试固件保留 12KB 并由截图阶段高水位核验。
        repl_config.task_stack_size = 12 * 1024;
#elif CONFIG_UR_AI_SIGNALING
        // 信令固件的 sig_screen 需要深栈；但通话期相机 SPI 需要 16KB 内部 DMA
        // 连续块，12KB 控制台栈会挤占余量导致相机 Capture 永久失败（真机实证）。
        // 8KB 覆盖命令解析与截图调用栈，腾出 4KB 内部内存给媒体路径。
        repl_config.task_stack_size = 8 * 1024;
#endif
        repl_config.max_cmdline_length = 1024;
        repl_config.prompt = "SenseCAP>";
        
        const esp_console_cmd_t cmd1 = {
            .command = "reboot",
            .help = "reboot the device",
            .hint = nullptr,
            .func = [](int argc, char** argv) -> int {
                esp_restart();
                return 0;
            },
            .argtable = nullptr
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmd1));

        const esp_console_cmd_t cmd2 = {
            .command = "shutdown",
            .help = "shutdown the device",
            .hint = nullptr,
            .func = NULL,
            .argtable = NULL,
            .func_w_context = [](void *context,int argc, char** argv) -> int {
                auto self = static_cast<SensecapWatcher*>(context);
                self->GetBacklight()->SetBrightness(0);
                self->IoExpanderSetLevel(BSP_PWR_SYSTEM, 0);
                return 0;
            },
            .context =this
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmd2));

        const esp_console_cmd_t cmd3 = {
            .command = "battery",
            .help = "get battery percent",
            .hint = NULL,
            .func = NULL,
            .argtable = NULL,
            .func_w_context = [](void *context,int argc, char** argv) -> int {
                auto self = static_cast<SensecapWatcher*>(context);
                self->BatterygetPercent(true);
                return 0;
            },
            .context =this
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmd3));

        const esp_console_cmd_t cmd4 = {
            .command = "factory_reset",
            .help = "factory reset and reboot the device",
            .hint = NULL,
            .func = NULL,
            .argtable = NULL,
            .func_w_context = [](void *context,int argc, char** argv) -> int {
                nvs_flash_erase();
                esp_restart();
                return 0;
            },
            .context =this
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmd4));

#if CONFIG_UR_AI_SIGNALING && CONFIG_UR_AI_SIGNALING_WEBRTC
        /* esp_peer 本地自检已随产线形态裁剪（分区空间紧张，见 main/CMakeLists.txt
         * WEBRTC 分支注释）；需要时恢复 ur_signaling_webrtc_selftest.cc 编译与本命令。 */

        /* sig_call <action>：humanCall 通话串口控制（供无触屏条件的真机远程验收）；
         * accept/reject/hangup/mic/cam 分别映射接听/拒接/挂断/麦克风开关/摄像头开关。 */
        const esp_console_cmd_t cmd_sig_call = {
            .command = "sig_call",
            .help = "human call control: sig_call <accept|reject|hangup|mic|cam|dial target [voice]>",
            .hint = NULL,
            .func = NULL,
            .argtable = NULL,
            .func_w_context = [](void *context, int argc, char** argv) -> int {
                auto& call = UrSignalingCall::Instance();
                const char* action = argc >= 2 ? argv[1] : "";
                if (strcmp(action, "accept") == 0) {
                    call.UiAccept();
                } else if (strcmp(action, "reject") == 0) {
                    call.UiReject();
                } else if (strcmp(action, "hangup") == 0) {
                    call.UiHangUp();
                } else if (strcmp(action, "mic") == 0) {
                    call.UiToggleMic();
                } else if (strcmp(action, "cam") == 0) {
                    call.UiToggleCamera();
                } else if (strcmp(action, "dial") == 0 && (argc == 3 || argc == 4)) {
                    const bool voice_only = argc == 4 && strcmp(argv[3], "voice") == 0;
                    if ((argc == 4 && !voice_only) || !call.Dial(argv[2], voice_only)) return 1;
                } else {
                    printf("usage: sig_call <accept|reject|hangup|mic|cam|dial target [voice]>\n");
                    return 1;
                }
                printf("sig_call %s dispatched\n", action);
                return 0;
            },
            .context = this,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmd_sig_call));

        /* sig_screen：通话/空闲期屏幕取证截图（JPEG hex 经 [UR_SCREEN] 串口输出，
         * 与 ur_hw_test snapshot 同协议）；视频形态回归需要真机显示证据，
         * 而截图命令原本只注册在 E2E 非信令构建，信令构建下无屏可证。 */
        const esp_console_cmd_t cmd_sig_screen = {
            .command = "sig_screen",
            .help = "capture screen snapshot as [UR_SCREEN] hex jpeg (for video call evidence)",
            .hint = NULL,
            .func = NULL,
            .argtable = NULL,
            .func_w_context = [](void* context, int argc, char** argv) -> int {
                (void)context;
                (void)argc;
                (void)argv;
                auto& app = Application::GetInstance();
                auto& board = static_cast<SensecapWatcher&>(Board::GetInstance());
                std::string jpeg;
                /* 截图仅经本地测试串口传输；不上传云端、不记录访问凭据或完整对话日志。 */
                if (!board.display_ || !board.display_->SnapshotToJpeg(jpeg, 75) ||
                    jpeg.empty() || jpeg.size() > 256 * 1024) {
                    printf("[UR_SCREEN] FAIL capture\n");
                    return 1;
                }
                const unsigned id = esp_random();  // 每次传输独立关联，拒绝历史片段拼接。
                const unsigned crc = esp_rom_crc32_le(
                    0, reinterpret_cast<const uint8_t*>(jpeg.data()), jpeg.size());
                printf("[UR_SCREEN] BEGIN %08x %u %08x\n", id, unsigned(jpeg.size()), crc);
                for (size_t offset = 0; offset < jpeg.size(); offset += 128) {
                    char hex[257];  // 每条消息上限 128 字节，允许常规日志在帧间穿插。
                    const size_t count = std::min(size_t(128), jpeg.size() - offset);
                    for (size_t i = 0; i < count; ++i)
                        snprintf(hex + i * 2, 3, "%02x", static_cast<uint8_t>(jpeg[offset + i]));
                    printf("[UR_SCREEN] DATA %08x %u %s\n", id, unsigned(offset), hex);
                }
                printf("[UR_SCREEN] END %08x\n", id);
                (void)app;
                return 0;
            },
            .context = this,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmd_sig_screen));

#if CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO
        // sig_av 启动一次60秒私网音视频验收；镜子必须已退出，正式通话占用时拒绝。
        const esp_console_cmd_t cmd_sig_av = {
            .command = "sig_av",
            .help = "private lab: sig_av <IPv4> <mqtt-port> <test-id>; stop mirror first",
            .hint = nullptr, .func = nullptr, .argtable = nullptr,
            .func_w_context = [](void*, int argc, char** argv) -> int {
                if (argc != 4 || g_mirror_on.load()) return 1;
                char* end = nullptr;
                const long port = strtol(argv[2], &end, 10);  // 严格拒绝尾随字符与端口溢出。
                return end && *end == '\0' && port > 0 && port <= 65535 &&
                       UrStartAVProbe(argv[1], static_cast<uint16_t>(port), argv[3]) ? 0 : 1;
            },
            .context = this,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmd_sig_av));
#endif

        // sig_perf 只导出或清空本机呈现耗时，不改变相机、网络或设备保存的配置。
        const esp_console_cmd_t cmd_sig_perf = {
            .command = "sig_perf",
            .help = "preview timings or task CPU snapshot: sig_perf [reset|tasks|ui]",
            .hint = nullptr,
            .func = nullptr,
            .argtable = nullptr,
            .func_w_context = [](void* context, int argc, char** argv) -> int {
                if (argc == 2 && strcmp(argv[1], "ui") == 0) {
                    // 只读通话界面快照不导出性能历史，避免串口背压拖延接挂等控制命令。
#if CONFIG_UR_AI_SIGNALING_WEBRTC
                    watcher::WatcherCallUi::TraceState();
                    return 0;
#else
                    return 1;
#endif
                }
                if (argc == 2 && strcmp(argv[1], "tasks") == 0) {
                    // 显式本地取证等待一秒，不持有显示锁或停用媒体任务。
                    auto& audio = Application::GetInstance().GetAudioService();
#if CONFIG_UR_AI_SIGNALING
                    audio.TraceSignalingCapture();  // 显式CPU取证同时读取同一媒体窗口的样本边界。
#endif
                    printf("[UR_CALL_AUDIO] wake=%d processing=%d\n", audio.IsWakeWordRunning(),
                           audio.IsAudioProcessorRunning());
                    return SystemInfo::PrintTaskCpuUsage(pdMS_TO_TICKS(1000)) == ESP_OK ? 0 : 1;
                }
                if (argc > 2 || (argc == 2 && strcmp(argv[1], "reset") != 0))
                    return 1;
                auto* board = static_cast<SensecapWatcher*>(context);  // 当前 Watcher 显示实例。
                if (!board->display_)
                    return 1;
                board->display_->PrintPreviewPerformance(argc == 2);
                board->TraceButtonPoll(argc == 2);
#if CONFIG_UR_AI_SIGNALING_WEBRTC
                watcher::WatcherCallUi::TraceState();  // 本地只读取证，避免把黑截图直接归因于解码。
#endif
                if (board->camera_)
                    board->camera_->PrintStreamPerformance(argc == 2);
                return 0;
            },
            .context = this,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmd_sig_perf));

        /* sig_mirror 与菜单共用显示任务中的状态机，避免调试入口与用户页面状态分离。 */
        const esp_console_cmd_t cmd_sig_mirror = {
            .command = "sig_mirror",
            .help = "mirror preview: sig_mirror <on|off>",
            .hint = NULL,
            .func = NULL,
            .argtable = NULL,
            .func_w_context = [](void* context, int argc, char** argv) -> int {
                auto* self = static_cast<SensecapWatcher*>(context);  // 当前板级显示入口。
                if (argc != 2 || !self->display_ || !self->GetCamera() ||
                    (strcmp(argv[1], "on") != 0 && strcmp(argv[1], "off") != 0))
                    return 1;
                const bool enabled = strcmp(argv[1], "on") == 0;  // 显式目标状态，重复命令幂等。
                self->display_->PostInput(
                    enabled ? watcher::MenuInput::MirrorOn : watcher::MenuInput::MirrorOff, false,
                    false, 50, 50, false);
                printf("mirror %s requested\n", enabled ? "on" : "off");
                return 0;
            },
            .context = this,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmd_sig_mirror));

        /* sig_menu：串口模拟菜单输入，用于远程验收菜单入口与页面切换。 */
        const esp_console_cmd_t cmd_sig_menu = {
            .command = "sig_menu",
            .help = "menu input: sig_menu [open|next|prev|click|double|back]",
            .hint = NULL,
            .func = NULL,
            .argtable = NULL,
            .func_w_context = [](void* context, int argc, char** argv) -> int {
                auto* self = static_cast<SensecapWatcher*>(context);
                if (self->display_ == nullptr) {
                    printf("display unavailable\n");
                    return 1;
                }
                const char* action = argc >= 2 ? argv[1] : "open";
                if (strcmp(action, "touch") == 0) {
                    const int index = argc >= 3 ? atoi(argv[2]) : 0;
                    self->display_->PostTouchCurrent(static_cast<size_t>(index < 0 ? 0 : index));
                    printf("sig_menu touch %d\n", index);
                    return 0;
                }
                watcher::MenuInput input = watcher::MenuInput::LongPress;
                if (strcmp(action, "next") == 0) input = watcher::MenuInput::Next;
                else if (strcmp(action, "prev") == 0) input = watcher::MenuInput::Previous;
                else if (strcmp(action, "click") == 0) input = watcher::MenuInput::Click;
                else if (strcmp(action, "double") == 0) input = watcher::MenuInput::DoubleClick;
                else if (strcmp(action, "back") == 0) input = watcher::MenuInput::Back;
                self->DispatchInput(input);
                printf("sig_menu %s\n", action);
                return 0;
            },
            .context = this,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmd_sig_menu));

        /* sig_sscma：向 HX6538 发送原始 AT 探测命令，确认连续流等能力。 */
        const esp_console_cmd_t cmd_sig_sscma = {
            .command = "sig_sscma",
            .help = "probe SSCMA AT command: sig_sscma <VER?|VIDEO?|STREAM?|INFO?>",
            .hint = NULL,
            .func = NULL,
            .argtable = NULL,
            .func_w_context = [](void* context, int argc, char** argv) -> int {
                (void)context;
                auto* camera = Board::GetInstance().GetCamera();
                if (camera == nullptr) {
                    printf("camera unavailable\n");
                    return 1;
                }
                camera->SendRawCommand(argc >= 2 ? argv[1] : "VER?");
                return 0;
            },
            .context = this,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmd_sig_sscma));
#endif
#if CONFIG_UR_AI_SIGNALING
        /* 串口触发语音会话入口：等效物理单击，供真机远程验收与卡死诊断。 */
        const esp_console_cmd_t cmd_sig_chat = {
            .command = "sig_chat",
            .help = "trigger a voice chat toggle (same as pressing the wheel button)",
            .hint = NULL,
            .func = NULL,
            .argtable = NULL,
            .func_w_context = [](void *context, int argc, char** argv) -> int {
                Application::GetInstance().ToggleChatState();
                return 0;
            },
            .context = this,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmd_sig_chat));

#if CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO
        /* sig_vision：启动 aiVision 三轨视觉会话（等效视觉入口，供真机调试与
         * APP attach 联动验证）；设备本地点击/sig_chat 默认纯语音两轨会话。 */
        const esp_console_cmd_t cmd_sig_vision = {
            .command = "sig_vision",
            .help = "open an aiVision camera session (voice chat stays on sig_chat)",
            .hint = NULL,
            .func = NULL,
            .argtable = NULL,
            .func_w_context = [](void *context, int argc, char** argv) -> int {
                auto* protocol = SignalingProtocol::Instance();
                if (protocol == nullptr) {
                    printf("no protocol\n");
                    return 1;
                }
                printf("opening vision session...\n");
                return protocol->OpenVisionChannel() ? 0 : 1;
            },
            .context = this,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmd_sig_vision));

        /* sig_listen：语音会话内直接触发开麦（audioStart），供无人现场验证
         * 上行采集/平台ASR链路；等效唤醒后的自动开麦路径。 */
        const esp_console_cmd_t cmd_sig_listen = {
            .command = "sig_listen",
            .help = "start mic capture in the active voice session (audioStart)",
            .hint = NULL,
            .func = NULL,
            .argtable = NULL,
            .func_w_context = [](void *context, int argc, char** argv) -> int {
                auto* protocol = SignalingProtocol::Instance();
                if (protocol == nullptr || !protocol->IsAudioChannelOpened()) {
                    printf("no active session (sig_chat first)\n");
                    return 1;
                }
                protocol->SendStartListening(kListeningModeAutoStop);
                printf("listening requested\n");
                return 0;
            },
            .context = this,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmd_sig_listen));

        /* sig_abort：本地打断当前播报（等价唤醒词/交互打断），上行 respCancel；
         * 供真机验证打断链路（TTS 播放中调用）。 */
        const esp_console_cmd_t cmd_sig_abort = {
            .command = "sig_abort",
            .help = "abort current speaking (local interrupt, sends respCancel)",
            .hint = NULL,
            .func = NULL,
            .argtable = NULL,
            .func_w_context = [](void *context, int argc, char** argv) -> int {
                auto* protocol = SignalingProtocol::Instance();
                if (protocol == nullptr) {
                    printf("no protocol\n");
                    return 1;
                }
                protocol->SendAbortSpeaking(kAbortReasonNone);
                printf("abort requested\n");
                return 0;
            },
            .context = this,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmd_sig_abort));

        /* sig_ask <问题>：aiVision 会话文字提问（视觉链触发入口）；
         * 平台同轮取摄像头帧交视觉模型后语音回复。 */
        const esp_console_cmd_t cmd_sig_ask = {
            .command = "sig_ask",
            .help = "send a text question in the vision session (e.g. sig_ask 你看到了什么)",
            .hint = NULL,
            .func = NULL,
            .argtable = NULL,
            .func_w_context = [](void *context, int argc, char** argv) -> int {
                /* esp_console 分词对非 ASCII 参数不稳定；无参时用默认视觉问题。 */
                std::string question = "你看到了什么，请描述画面内容";
                if (argc >= 2) {
                    question.clear();
                    for (int i = 1; i < argc; ++i) {
                        if (!question.empty()) question += " ";
                        question += argv[i];
                    }
                }
                auto* protocol = SignalingProtocol::Instance();
                if (protocol == nullptr || !protocol->SendTextQuestion(question)) {
                    printf("send failed (no protocol or session busy)\n");
                    return 1;
                }
                printf("text sent\n");
                return 0;
            },
            .context = this,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmd_sig_ask));
#endif

        /* sig_pcm：串口流式注入 16k/单声道调试 PCM，用于无人在场的语音轮次验收：
         * sig_chat 建会话 + sig_listen 开麦后，begin → 多行 base64（≤200字符/行）→ end
         * 按 AFE 10ms 块节奏注入。语料经串口传输不进固件分区（app 分区已无余量）。 */
#if CONFIG_UR_AI_SIGNALING_WEBRTC
        /* AudioService 的 InjectMicInput/SignalingCaptureEnabled 只在 WebRTC 形态编译；
         * UDP 信令形态没有该调试注入通道，命令必须同门禁，否则 signaling flavor 编译失败。 */
        const esp_console_cmd_t cmd_sig_pcm = {
            .command = "sig_pcm",
            .help = "inject 16k pcm: sig_pcm begin | sig_pcm <base64> ... | sig_pcm end",
            .hint = NULL,
            .func = NULL,
            .argtable = NULL,
            .func_w_context = [](void *context, int argc, char** argv) -> int {
                static uint8_t* pcm_buf = nullptr;
                static size_t pcm_used = 0;
                constexpr size_t kMaxBytes = 256 * 1024;
                auto b64val = [](char c) -> int {
                    if (c >= 'A' && c <= 'Z') return c - 'A';
                    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
                    if (c >= '0' && c <= '9') return c - '0' + 52;
                    if (c == '+') return 62;
                    if (c == '/') return 63;
                    return -1;
                };
                if (argc < 2) {
                    printf("usage: sig_pcm begin|<base64>|end\n");
                    return 1;
                }
                if (strcmp(argv[1], "begin") == 0) {
                    if (pcm_buf == nullptr) {
                        pcm_buf = static_cast<uint8_t*>(
                            heap_caps_malloc(kMaxBytes, MALLOC_CAP_SPIRAM));
                    }
                    pcm_used = 0;
                    printf("pcm begin (buf=%s)\n", pcm_buf ? "ok" : "null");
                    return pcm_buf ? 0 : 1;
                }
                if (strcmp(argv[1], "end") == 0) {
                    if (pcm_buf == nullptr || pcm_used < 640) {
                        printf("pcm empty used=%u\n", (unsigned)pcm_used);
                        return 1;
                    }
                    const auto* pcm = reinterpret_cast<const int16_t*>(pcm_buf);
                    const int total = static_cast<int>(pcm_used / sizeof(int16_t));
                    constexpr int kChunkSamples = 160; /* 10ms，与 AFE 输入块同节奏 */
                    auto& audio = Application::GetInstance().GetAudioService();
                    /* 等待信令会话开麦（sig_chat 后 audioStart ACK），最长 15 秒。 */
                    bool ready = false;
                    for (int wait = 0; wait < 150 && !ready; ++wait) {
                        ready = audio.SignalingCaptureEnabled();
                        if (!ready) vTaskDelay(pdMS_TO_TICKS(100));
                    }
                    if (!ready) {
                        printf("capture not open\n");
                        return 1;
                    }
                    /* 语料送入AFE队列；块间可能回落物理麦克风，注入成功不等于
                     * 独占连续声源或ASR已通过，调试验收须单独核对识别结果。 */
                    int injected = 0;
                    for (int i = 0; i + kChunkSamples <= total; i += kChunkSamples) {
                        std::vector<int16_t> chunk(pcm + i, pcm + i + kChunkSamples);
                        if (!audio.InjectMicInput(chunk)) {
                            break;
                        }
                        ++injected;
                        vTaskDelay(pdMS_TO_TICKS(10));
                    }
                    printf("sig_pcm injected %d chunks (%d ms, %u bytes)\n", injected,
                           injected * 10, (unsigned)pcm_used);
                    pcm_used = 0;
                    return 0;
                }
                if (pcm_buf == nullptr || pcm_used >= kMaxBytes) {
                    printf("pcm overflow\n");
                    return 1;
                }
                const char* text = argv[1];
                int digits[4] = {0, 0, 0, 0};
                int group = 0;
                bool done = false;
                for (const char* p = text; *p != '\0' && !done; ++p) {
                    if (*p == '=') {
                        break;
                    }
                    const int v = b64val(*p);
                    if (v < 0) {
                        continue; /* 忽略换行/空白，兼容不同客户端分包。 */
                    }
                    digits[group++] = v;
                    if (group == 4) {
                        pcm_buf[pcm_used++] = static_cast<uint8_t>((digits[0] << 2) | (digits[1] >> 4));
                        pcm_buf[pcm_used++] = static_cast<uint8_t>((digits[1] << 4) | (digits[2] >> 2));
                        pcm_buf[pcm_used++] = static_cast<uint8_t>((digits[2] << 6) | digits[3]);
                        group = 0;
                    }
                }
                if (group == 2) {
                    pcm_buf[pcm_used++] = static_cast<uint8_t>((digits[0] << 2) | (digits[1] >> 4));
                } else if (group == 3) {
                    pcm_buf[pcm_used++] = static_cast<uint8_t>((digits[0] << 2) | (digits[1] >> 4));
                    pcm_buf[pcm_used++] = static_cast<uint8_t>((digits[1] << 4) | (digits[2] >> 2));
                }
                return 0;
            },
            .context = this,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmd_sig_pcm));
#endif /* CONFIG_UR_AI_SIGNALING_WEBRTC */
#endif

        /* 串口直写 WiFi 凭据：wifi <ssid> <password>，供刷机后免 BLE
         * 配网恢复（SsidManager 十槽合同，与 App 配网同一存储）。 */
        const esp_console_cmd_t cmd_wifi = {
            .command = "wifi",
            .help = "save WiFi credentials: wifi <ssid> <password>",
            .hint = NULL,
            .func = [](int argc, char** argv) -> int {
                if (argc != 3) {
                    printf("usage: wifi <ssid> <password>\n");
                    return 1;
                }
                SsidManager::GetInstance().AddSsid(argv[1], argv[2]);
                printf("wifi saved, rebooting\n");
                vTaskDelay(pdMS_TO_TICKS(300));
                esp_restart();
                return 0;
            },
            .argtable = NULL,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmd_wifi));

#if CONFIG_UR_AI_SIGNALING_WEBRTC && CONFIG_UR_AI_E2E_TEST
        /* sig_speak：把嵌入的 16k 语料按 AFE 节奏注入信令采集队列，
         * 与 sig_chat 组合即可无人现场完成真机语音轮次验收。
         * 语料嵌入与命令同挂 UR_AI_E2E_TEST，产线形态不编译。 */
        const esp_console_cmd_t cmd_sig_speak = {
            .command = "sig_speak",
            .help = "inject embedded 16k corpus into signaling capture (voice roundtrip)",
            .hint = NULL,
            .func = NULL,
            .argtable = NULL,
            .func_w_context = [](void *context, int argc, char** argv) -> int {
                extern const uint8_t corpus_start[] asm("_binary_ur_sig_speak_corpus_wav_start");
                extern const uint8_t corpus_end[] asm("_binary_ur_sig_speak_corpus_wav_end");
                const size_t size = corpus_end - corpus_start;
                if (size < 128) {
                    printf("corpus empty\n");
                    return 1;
                }
                /* 动态定位 data 块：ffmpeg 可能生成含 LIST 扩展的非 44 字节头。 */
                size_t offset = 0;
                for (size_t i = 12; i + 8 <= size;) {
                    const auto chunk_size = static_cast<uint32_t>(corpus_start[i + 4]) |
                                            (static_cast<uint32_t>(corpus_start[i + 5]) << 8) |
                                            (static_cast<uint32_t>(corpus_start[i + 6]) << 16) |
                                            (static_cast<uint32_t>(corpus_start[i + 7]) << 24);
                    if (memcmp(corpus_start + i, "data", 4) == 0) {
                        offset = i + 8;
                        break;
                    }
                    i += 8 + chunk_size + (chunk_size & 1);
                }
                if (offset == 0 || offset + 2 * 160 > size) {
                    printf("corpus pcm not found\n");
                    return 1;
                }
                auto& audio = Application::GetInstance().GetAudioService();
                constexpr int kChunkSamples = 160;  // 10ms，与 AFE 输出块同节奏
                const auto* pcm = reinterpret_cast<const int16_t*>(corpus_start + offset);
                const int total = static_cast<int>((size - offset) / sizeof(int16_t));
                /* 等待信令采集开启（sig_chat 后 audioStart ACK），最长 15 秒。 */
                for (int wait = 0; wait < 150; ++wait) {
                    std::vector<int16_t> probe(pcm, pcm + kChunkSamples);
                    if (audio.InjectSignalingCapture(probe)) {
                        break;
                    }
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
                int injected = 0;
                for (int i = 0; i + kChunkSamples <= total; i += kChunkSamples) {
                    std::vector<int16_t> chunk(pcm + i, pcm + i + kChunkSamples);
                    if (!audio.InjectSignalingCapture(chunk)) {
                        break;
                    }
                    ++injected;
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
                printf("sig_speak injected %d chunks (%d ms)\n", injected, injected * 10);
                return 0;
            },
            .context = this,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmd_sig_speak));
#endif

        const esp_console_cmd_t cmd5 = {
            .command = "read_mac",
            .help = "Read mac address",
            .hint = NULL,
            .func = NULL,
            .argtable = NULL,
            .func_w_context = [](void *context,int argc, char** argv) -> int {
                uint8_t mac[6];
                esp_read_mac(mac, ESP_MAC_WIFI_STA);
                printf("wifi_sta_mac: " MACSTR "\n", MAC2STR(mac));
                esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
                printf("wifi_softap_mac: " MACSTR "\n", MAC2STR(mac));
                esp_read_mac(mac, ESP_MAC_BT);
                printf("bt_mac: " MACSTR "\n", MAC2STR(mac));
                return 0;
            },
            .context =this
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmd5));

        const esp_console_cmd_t cmd6 = {
            .command = "version",
            .help = "Read version info",
            .hint = NULL,
            .func = NULL,
            .argtable = NULL,
            .func_w_context = [](void *context,int argc, char** argv) -> int {
                auto self = static_cast<SensecapWatcher*>(context);
                auto app_desc = esp_app_get_description();
                const char* region = "UNKNOWN";
                #if defined(CONFIG_LANGUAGE_ZH_CN)
                    region = "CN";
                #elif defined(CONFIG_LANGUAGE_EN_US)
                    region = "US";
                #elif defined(CONFIG_LANGUAGE_JA_JP)
                    region = "JP";
                #elif defined(CONFIG_LANGUAGE_ES_ES)
                    region = "ES";
                #elif defined(CONFIG_LANGUAGE_DE_DE)
                    region = "DE";
                #elif defined(CONFIG_LANGUAGE_FR_FR)
                    region = "FR";
                #elif defined(CONFIG_LANGUAGE_IT_IT)
                    region = "IT";
                #elif defined(CONFIG_LANGUAGE_PT_PT)
                    region = "PT";
                #elif defined(CONFIG_LANGUAGE_RU_RU)
                    region = "RU";
                #elif defined(CONFIG_LANGUAGE_KO_KR)
                    region = "KR";
                #endif
                printf("{\"type\":0,\"name\":\"VER?\",\"code\":0,\"data\":{\"software\":\"%s\",\"hardware\":\"watcher xiaozhi agent\",\"camera\":%d,\"region\":\"%s\"}}\n",
                       app_desc->version,
                       self->GetCamera() == nullptr ? 0 : 1,
                       region);
                return 0;
            },
            .context =this
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmd6));

#if CONFIG_UR_AI_E2E_TEST && !CONFIG_UR_AI_SIGNALING
        const esp_console_cmd_t ur_ai_e2e_cmd = {
            .command = "ur_ai_e2e",
            .help = "run embedded Opus: ur_ai_e2e [1-5] [photo|control]; control requires volume55 and one turn",
            .hint = nullptr,
            .func = [](int argc, char** argv) -> int {
                // 只允许单字符轮数，避免无界测试或宽松 atoi 接受非法参数。
                if (argc > 3 || (argc >= 2 &&
                    (strlen(argv[1]) != 1 || argv[1][0] < '1' || argv[1][0] > '5')) ||
                    (argc == 3 && strcmp(argv[2], "photo") != 0 && strcmp(argv[2], "control") != 0)) return 1;
                const int turns = argc >= 2 ? argv[1][0] - '0' : 1;
                const bool photo = argc == 3 && strcmp(argv[2], "photo") == 0;
                const bool control = argc == 3 && strcmp(argv[2], "control") == 0;
                if (control && turns != 1) return 1;
                /* 样本只替换上行音频，仍走真实ASR和工具；控制必须逐次准备有效初值。 */
                return Application::GetInstance().RunUrAiE2eTest(turns, photo, control) ? 0 : 1;
            },
            .argtable = nullptr,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&ur_ai_e2e_cmd));
        /* 仅测试固件暴露硬件控制；调用正式按键入口，不伪造照片或平台回包。 */
        const esp_console_cmd_t ur_hw_test_cmd = {
            .command = "ur_hw_test",
            .help = "test-only: photo <expected-word> | photo-start | press | double-press | long-press | next | previous | state | snapshot | wifi-reconnect",
            .hint = nullptr,
            .func = [](int argc, char** argv) -> int {
                if (argc < 2) return 1;
                auto& app = Application::GetInstance();
                auto& board = static_cast<SensecapWatcher&>(Board::GetInstance());
                if (strcmp(argv[1], "snapshot") == 0 && argc == 2) {
                    // 截图仅经本地测试串口传输；不上传云端、不记录访问凭据或完整对话日志。
                    std::string jpeg;
                    if (!board.display_ || !board.display_->SnapshotToJpeg(jpeg, 75) ||
                        jpeg.empty() || jpeg.size() > 256 * 1024) {
                        printf("[UR_SCREEN] FAIL capture\n");
                        return 1;
                    }
                    const unsigned id = esp_random();  // 每次传输独立关联，拒绝历史片段拼接。
                    const unsigned crc = esp_rom_crc32_le(0,
                        reinterpret_cast<const uint8_t*>(jpeg.data()), jpeg.size());
                    printf("[UR_SCREEN] BEGIN %08x %u %08x\n", id, unsigned(jpeg.size()), crc);
                    for (size_t offset = 0; offset < jpeg.size(); offset += 128) {
                        char hex[257];  // 每条消息上限 128 字节，允许常规日志在帧间穿插。
                        const size_t count = std::min(size_t(128), jpeg.size() - offset);
                        for (size_t i = 0; i < count; ++i)
                            snprintf(hex + i * 2, 3, "%02x", static_cast<uint8_t>(jpeg[offset + i]));
                        printf("[UR_SCREEN] DATA %08x %u %s\n", id, unsigned(offset), hex);
                    }
                    printf("[UR_SCREEN] END %08x\n", id);
                    return 0;
                }
                if (strcmp(argv[1], "photo-hex") == 0 && argc == 3) {
                    return app.RunUrPhotoE2eTest(UrDecodeTestWord(argv[2])) ? 0 : 1;
                }
                if (strcmp(argv[1], "photo") == 0 && argc == 3) {
                    return app.RunUrPhotoE2eTest(argv[2]) ? 0 : 1;
                }
                if (strcmp(argv[1], "photo-start") == 0 && argc == 2) {
                    app.TakePhoto();
                } else if (argc == 2 && static_cast<SensecapWatcher&>(Board::GetInstance()).DispatchTestInput(argv[1])) {
                    // ACK 只代表输入已排队，不能当作菜单渲染或业务完成。
                } else if (strcmp(argv[1], "wifi-reconnect") == 0 && argc == 2) {
                    /* 只断开当前设备，保持凭据；等待 SDK 观察断线后重新关联。 */
                    if (esp_wifi_disconnect() != ESP_OK) return 1;
                    vTaskDelay(pdMS_TO_TICKS(5000));
                    if (esp_wifi_connect() != ESP_OK) return 1;
                } else if (strcmp(argv[1], "state") != 0 || argc != 2) {
                    return 1;
                }
                if (board.display_) board.display_->PrintTestState();
                printf("[UR_UI] volume=%d brightness=%d\n", board.GetAudioCodec()->output_volume(),
                       board.GetBacklight()->brightness());
                ESP_LOGI(TAG, "[UR_HW_TEST] ACK op=%s state=%d mqtt=%d", argv[1],
                         static_cast<int>(app.GetDeviceState()), ur_device_is_connected());
                return 0;
            },
            .argtable = nullptr,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&ur_hw_test_cmd));
#endif

        esp_console_dev_uart_config_t hw_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw_config, &repl_config, &repl));
        ESP_ERROR_CHECK(esp_console_start_repl(repl));
    }

    void InitializeCamera() {

        ESP_LOGI(TAG, "Initialize Camera");

        // !!!NOTE: SD Card use same SPI bus as sscma client, so we need to disable SD card CS pin first
        const gpio_config_t io_config = {
            .pin_bit_mask = (1ULL << BSP_SD_SPI_CS),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        esp_err_t ret = gpio_config(&io_config);
        if (ret != ESP_OK)
            return;

        gpio_set_level(BSP_SD_SPI_CS, 1);

        camera_ = new SscmaCamera(io_exp_handle);
    }

public:
    SensecapWatcher() {
        ESP_LOGI("HeapProbe", "board-ctor-begin internal=%u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        ESP_LOGI(TAG, "Initialize Sensecap Watcher");
        InitializeI2c();
        InitializeSpi();
        InitializeExpander();
        // 省电初始化会读取扩展器上的 USB 供电引脚，必须晚于扩展器就绪。
        InitializePowerSaveTimer();
        InitializeCmd();  //工厂生产测试使用
        InitializeButton();
        InitializeKnob();
        Initializespd2010Display();
        display_->OnMenuAction([this](watcher::MenuAction action, int value) {
            if (action == watcher::MenuAction::Visibility)
                display_->PublishMenuVisible(value != 0);
            const auto epoch = display_->InputEpoch();  // 应用队列迟到动作不能落到另一种屏幕用途。
            Application::GetInstance().Schedule([this, action, value, epoch]() {
                if (action == watcher::MenuAction::Visibility)
                    ExecuteMenuAction(action, display_->MenuVisible());
                else if (display_->InputEpoch() == epoch)
                    ExecuteMenuAction(action, value);
            });
        });
#if CONFIG_UR_AI_SIGNALING_WEBRTC
        UrSignalingContacts::Instance().SetCallback([this](UrContactsResult result) {
            display_->PostContactsResult(std::move(result));
        });
        // 注册设备视频通话页面钩子；来电/帧回调经主任务收敛后再操作 LVGL。
        // 真机验收要求通话页可见，恢复此前为定位启动内存差临时关闭的安装。
        watcher::WatcherCallUi::Install(
            display_,
            [this]() { Application::GetInstance().Schedule([this]() { power_save_timer_->WakeUp(); }); },
            [this](bool active) { display_->SetCallUiActive(active); });
#endif
        ESP_LOGI("HeapProbe", "post-display internal=%u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        InitializeTouch();
        GetBacklight()->RestoreBrightness();  // 对于不带摄像头的版本，InitializeCamera需要3s, 所以先恢复背光亮度
        InitializeCamera();
        ESP_LOGI("HeapProbe", "post-camera internal=%u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    }

    virtual AudioCodec* GetAudioCodec() override {
        static SensecapAudioCodec audio_codec(
            i2c_bus_, 
            AUDIO_INPUT_SAMPLE_RATE, 
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, 
            AUDIO_I2S_GPIO_BCLK, 
            AUDIO_I2S_GPIO_WS, 
            AUDIO_I2S_GPIO_DOUT, 
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, 
            AUDIO_CODEC_ES8311_ADDR, 
            AUDIO_CODEC_ES7243E_ADDR, 
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
    
    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    // 根据 https://github.com/Seeed-Studio/OSHW-SenseCAP-Watcher/blob/main/Hardware/SenseCAP_Watcher_v1.0_SCH.pdf
    // RGB LED型号为 ws2813 mini, 连接在GPIO 40，供电电压 3.3v, 没有连接 BIN 双信号线
    // 可以直接兼容SingleLED采用的ws2812
    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    /* 联网启动：没有任何 WiFi 凭据时独占运行 LLSync BLE 配网；流程结束先释放蓝牙栈，
     * 成功则重启进入正常联网，失败/超时则重启进入 xiaozhi 热点配网。 */
    // PresentNetworkChange 由已发送 actionReply 的桥接调度，默认取消并结束当前语音。
    bool PresentNetworkChange(const std::string& request_id) override {
        if (network_flow_.load() || !display_->PostNetworkConfirmation()) return false;
        network_confirmation_id_ = request_id;
        power_save_timer_->WakeUp();
        auto& app = Application::GetInstance();
        app.SetInteractionBlocked(true);
        app.EndConversation();
        return true;
    }

    // AllowAutomaticWifiConfig 保留已保存网络的自动重连，换网仅由用户打开本地菜单。
    bool AllowAutomaticWifiConfig() const override { return false; }

    // OnNetworkEvent 仅在正常 Wi-Fi 已取得 IP 时更新最近顺序，不以扫描/连接请求作为成功。
    void OnNetworkEvent(NetworkEvent event, const std::string& data = "") override {
        WifiBoard::OnNetworkEvent(event, data);
        if (event == NetworkEvent::Connected && !data.empty()) {
            if (!ur_network_remember_connection(data.c_str()))
                ESP_LOGW(TAG, "WiFi recency not saved; credentials unchanged");
        }
    }

    void StartNetwork() override {
        // 事务恢复必须先于 SsidManager 建缓存，提交中断时先恢复旧列表再正常联网。
        const auto mode = ur_network_boot();
        display_->SetNetworkStartup(mode == UR_NETWORK_NORMAL);
        if (mode != UR_NETWORK_NORMAL) {
            network_flow_.store(true);
            Application::GetInstance().SetInteractionBlocked(true);
            power_save_timer_->SetEnabled(false);
            if (mode == UR_NETWORK_HALT) {
                display_->SetStatus("配置恢复失败");
                display_->ShowNotification("存储异常，请重启重试", 180000);
                return;
            }
#if CONFIG_UR_BLE_PROVISION
            if (mode == UR_NETWORK_BIND) {
                display_->SetBindingFlow();
                display_->SetStatus("配网并绑定");
                display_->ShowNotification("小程序添加设备 · 单击取消", 180000);
                // 与正常 MQTT/WifiManager 完全隔离；成功、取消和失败均重启恢复常规启动链。
                if (xTaskCreate([](void*) {
                    ur_ble_rebind_start(180000);
                    ur_ble_provision_deinit_bt();
                    esp_restart();
                }, "ur_bind", 8192, nullptr, 4, nullptr) != pdPASS) {
                    ur_network_fail(UR_NETWORK_ERROR_STORAGE);
                    esp_restart();
                }
                return;
            }
            const bool trial = mode == UR_NETWORK_TRIAL;
            display_->SetStatus(trial ? "正在恢复连接" : "等待手机连接");
            display_->ShowNotification(trial ? "请保持设备通电" : "小程序 → 设备设置 → Wi-Fi 管理", 180000);
            const bool started = ur_ble_network_start(trial, [](bool connected, void* context) {
                auto* self = static_cast<SensecapWatcher*>(context);
                Application::GetInstance().Schedule([self, connected]() {
                    ur_network_status_t status{};
                    ur_network_status(&status);
                    if (status.stage != 5 || status.blocked) return;
                    self->OnNetworkEvent(connected ? NetworkEvent::Connected : NetworkEvent::Disconnected);
                    if (connected) ur_device_task_start();
                });
            }, this);
            if (!started) {
                if (ur_network_fail(UR_NETWORK_ERROR_NETWORK) == UR_NETWORK_ROLLBACK) esp_restart();
                display_->ShowNotification("换网启动失败，请重启", 180000);
            }
#else
            if (ur_network_fail(UR_NETWORK_ERROR_NETWORK) == UR_NETWORK_ROLLBACK) esp_restart();
#endif
            return;
        }
        ur_network_status_t network_status{};
        ur_network_status(&network_status);
        if (network_status.stage == 8) display_->ShowNotification("换网未完成，已恢复原网络", 10000);
        Settings wifi_settings("wifi", true);
        bool force_ap = wifi_settings.GetInt("force_ap") == 1;
        auto &ssid_manager = SsidManager::GetInstance();
        if (ssid_manager.GetSsidList().empty()) {
            if (ur_bridge_load_preprovision_wifi()) {
                ESP_LOGI(TAG, "preprovisioned wifi loaded");
            }
        }

#if CONFIG_UR_BLE_PROVISION
        if (!force_ap && ssid_manager.GetSsidList().empty()) {
            GetDisplay()->ShowNotification("等待蓝牙配网", 60000);
            ESP_LOGI(TAG, "no WiFi credentials, start LLSync BLE provisioning");
            bool provisioned = ur_ble_provision_start(CONFIG_UR_BLE_PROVISION_TIMEOUT_MS);
            ur_ble_provision_deinit_bt();
            if (provisioned) {
                esp_restart();
            }
            ESP_LOGW(TAG, "BLE provisioning failed or timed out, fallback to AP provisioning");
            /* BLE 接收 WiFi 后会先保存以供成功重启使用；绑定失败或超时时必须丢弃
             * 这次未完成绑定的凭据，否则 WifiBoard 会再次走 STA 而不会进入热点。 */
            ur_bridge_clear_wifi();
            wifi_settings.SetInt("force_ap", 1);
            esp_restart();
        }
#endif

        WifiBoard::StartNetwork();

        // 联网成功后启动联犀 MQTT 设备任务（动态注册/物模型上报/控制）
        ur_device_task_start();
    }

    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level != PowerSaveLevel::LOW_POWER) {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveLevel(level);
    }

    virtual bool GetBatteryLevel(int &level, bool& charging, bool& discharging) override {
        static bool last_discharging = false;
        charging = (IoExpanderGetLevel(BSP_PWR_VBUS_IN_DET) == 0);
        discharging = !charging;
        level = (int)BatterygetPercent(false);

        if (discharging != last_discharging) {
            power_save_timer_->SetEnabled(discharging && !network_flow_.load());
            last_discharging = discharging;
        }
        if (level <= 1  &&  discharging) {
            ESP_LOGI(TAG, "Battery level is low, shutting down");
            IoExpanderSetLevel(BSP_PWR_SYSTEM, 0);
        }
        return true;
    }

    virtual Camera* GetCamera() override {
        return camera_;
    }
};

DECLARE_BOARD(SensecapWatcher);

// 定义静态成员变量
SensecapWatcher* SensecapWatcher::instance_ = nullptr;
