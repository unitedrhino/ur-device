// 文件说明：SparkBot 坦克板级外设、联犀物模型和可中断底盘控制。
// 文件说明：ESP-SparkBot 坦克板级外设、工具及遥控所需的网络省电策略。
#include "application.h"
#include "button.h"
#include "codecs/es8311_audio_codec.h"
#include "config.h"
#include "display/lcd_display.h"
#include "mcp_server.h"
#include "settings.h"
#include "wifi_board.h"

#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <driver/touch_sens.h>
#include <driver/uart.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <cJSON.h>
#include <atomic>
#include <cstring>
#include <mutex>
#if CONFIG_UR_AI_E2E_TEST
#include <esp_console.h>
#endif
#include "ur/device/sparkbot_controls.hpp"
#if CONFIG_UR_AI_SIGNALING_WEBRTC
#include "sparkbot_call_controls.h"
#include "ur_signaling_call.h"
#include "assets/lang_config.h"
#if CONFIG_UR_AI_E2E_TEST
#include "sparkbot_signaling_test.h"
#endif
#if CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO
#include "sparkbot_call_video.h"
#endif
#endif

#include "esp_video.h"
#include "ur_device.h"
#include "wifi_manager.h"

#define TAG "esp_sparkbot"

class SparkBotEs8311AudioCodec : public Es8311AudioCodec {
private:
public:
    SparkBotEs8311AudioCodec(void* i2c_master_handle, i2c_port_t i2c_port, int input_sample_rate,
                             int output_sample_rate, gpio_num_t mclk, gpio_num_t bclk,
                             gpio_num_t ws, gpio_num_t dout, gpio_num_t din, gpio_num_t pa_pin,
                             uint8_t es8311_addr, bool use_mclk = true)
        : Es8311AudioCodec(i2c_master_handle, i2c_port, input_sample_rate, output_sample_rate, mclk,
                           bclk, ws, dout, din, pa_pin, es8311_addr, use_mclk, false, true) {
        // 仅坦克初始音量为20%，已有用户设置由基类 Start 正常读取。
        output_volume_ = 20;
    }

#if CONFIG_UR_AI_E2E_TEST
    // InputData 在测试包统计真实采音的能量与峰值，不替换麦克风或输出音频。
    bool InputData(std::vector<int16_t>& data) override {
        const bool success = Es8311AudioCodec::InputData(data);
        if (!success) return false;
        const int count = static_cast<int>(data.size());
        const int16_t* dest = data.data();
        for (int i = 0; i < count; ++i) {
            const int value = dest[i];
            const unsigned peak = value < 0 ? -value : value;
            if (peak > mic_peak_)
                mic_peak_ = peak;
            mic_energy_ += static_cast<uint64_t>(value * value);
        }
        if (count > 0)
            mic_samples_ += count;
        const int64_t now = esp_timer_get_time();
        if (now - mic_logged_us_ >= 2000000 && mic_samples_ != 0) {
            ESP_LOGI(TAG, "MIC_DIAG samples=%u peak=%u mean_square=%u", mic_samples_, mic_peak_,
                     static_cast<unsigned>(mic_energy_ / mic_samples_));
            mic_logged_us_ = now;
            mic_energy_ = 0;
            mic_samples_ = 0;
            mic_peak_ = 0;
        }
        return success;
    }

private:
    uint64_t mic_energy_ = 0;    // 当前2秒窗口的采音能量。
    uint32_t mic_samples_ = 0;   // 当前窗口有效采样数。
    unsigned mic_peak_ = 0;      // 当前窗口绝对峰值。
    int64_t mic_logged_us_ = 0;  // 最近诊断日志的单调时间。
public:
#endif

    void EnableOutput(bool enable) override {
        if (enable == output_enabled_) {
            return;
        }
        if (enable) {
            Es8311AudioCodec::EnableOutput(enable);
        } else {
            // Nothing todo because the display io and PA io conflict
        }
    }
};

class EspSparkBot : public WifiBoard {
private:
    // 触摸控制器只属于坦克；三个触点在ISR中更新位图，任务中完成去抖和会话切换。
    touch_sensor_handle_t touch_sensor_ = nullptr;
    touch_channel_handle_t touch_channels_[3] = {};
    std::atomic<uint32_t> touch_active_{0};
    ur::device::SparkBotTouchDebounce touch_debounce_;
#if CONFIG_UR_AI_SIGNALING_WEBRTC
    // 调用线程只读取状态快照；触点在状态切换时若已按住，须松开后才允许操作。
    static_assert(static_cast<unsigned>(UrSignalingCall::UiState::Ended) < 8,
                  "通话状态必须可放入三位状态快照");
    sparkbot::CallStateEpoch<UrSignalingCall::UiState> call_state_{
        UrSignalingCall::UiState::Hidden};
    ur::device::SparkBotTouchDebounce call_touch_debounce_[2];
#if CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO
    std::unique_ptr<sparkbot::CallVideoView> call_video_;  // 通话帧接力与显示对象。
#endif
#endif
    i2c_master_bus_handle_t i2c_bus_;
    Button boot_button_;
    Display* display_;
    EspVideo* camera_;
    light_mode_t light_mode_ = LIGHT_MODE_BLINK;
    int ur_light_mode_ = 1;  // 保留业务灯效编号；0与6当前都发底盘休眠，非彻底熄灭。
    /* 联犀坦克扩展：Speed 属性（0-100）映射底盘 x/y 幅度；运动超时自动零速兜底。 */
    int ur_speed_ = 100;
    ur::device::SparkBotMotion ur_motion_;  // 主控舞步与运动超时状态。
    std::mutex ur_motion_mutex_;            // 串行化 MQTT、定时器与断连的运动状态。
    std::mutex ur_uart_mutex_;              // 防止定时器和灯光命令交错写入 UART。
    std::string ur_last_command_;           // 仅记录变化指令，避免心跳刷屏。
    esp_timer_handle_t ur_motion_timer_ = nullptr;
    bool ur_motion_priority_ = true;  // 停止/新运动优先于灯光排队，最迟下一周期输出。
    int ur_pending_light_ = 3;        // 待发送底盘灯效；零表示无待发灯光。

    /* 运动指令执行时长上限：物模型行为未定义时长参数，语音控制按固定时长运动后自停，
     * 避免断链或漏发停止导致坦克持续运动。 */
    static constexpr uint32_t kMotionTimeoutMs = 3000;

    void InitializeI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags =
                {
                    .enable_internal_pullup = 1,
                },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_MOSI_GPIO;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_CLK_GPIO;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
    }

    // TouchActive/TouchInactive 仅修改无锁位图，ISR不调用应用或音频方法。
    static bool TouchActive(touch_sensor_handle_t, const touch_active_event_data_t* event,
                            void* arg) {
        static_cast<EspSparkBot*>(arg)->touch_active_.fetch_or(1U << event->chan_id);
        return false;
    }
    static bool TouchInactive(touch_sensor_handle_t, const touch_inactive_event_data_t* event,
                              void* arg) {
        static_cast<EspSparkBot*>(arg)->touch_active_.fetch_and(~(1U << event->chan_id));
        return false;
    }

    // InitializeTouch 使用IDF6原生触摸驱动，沿用官方GPIO1/2/3和灵敏度比例。
    void InitializeTouch() {
        touch_sensor_sample_config_t sample =
            TOUCH_SENSOR_V2_DEFAULT_SAMPLE_CONFIG(500, TOUCH_VOLT_LIM_L_0V5, TOUCH_VOLT_LIM_H_2V2);
        touch_sensor_config_t config = TOUCH_SENSOR_DEFAULT_BASIC_CONFIG(1, &sample);
        ESP_ERROR_CHECK(touch_sensor_new_controller(&config, &touch_sensor_));
        touch_channel_config_t channel = {};
        channel.active_thresh[0] = 2000;
        channel.charge_speed = TOUCH_CHARGE_SPEED_7;
        channel.init_charge_volt = TOUCH_INIT_CHARGE_VOLT_DEFAULT;
        for (int i = 0; i < 3; ++i) {
            ESP_ERROR_CHECK(
                touch_sensor_new_channel(touch_sensor_, i + 1, &channel, &touch_channels_[i]));
        }
        touch_sensor_filter_config_t filter = TOUCH_SENSOR_DEFAULT_FILTER_CONFIG();
        ESP_ERROR_CHECK(touch_sensor_config_filter(touch_sensor_, &filter));
        ESP_ERROR_CHECK(touch_sensor_enable(touch_sensor_));
        for (int i = 0; i < 3; ++i) {
            ESP_ERROR_CHECK(touch_sensor_trigger_oneshot_scanning(touch_sensor_, 2000));
        }
        ESP_ERROR_CHECK(touch_sensor_disable(touch_sensor_));
        constexpr float sensitivity[] = {0.035f, 0.08f, 0.08f};
        for (int i = 0; i < 3; ++i) {
            uint32_t benchmark = 0;
            ESP_ERROR_CHECK(touch_channel_read_data(touch_channels_[i],
                                                    TOUCH_CHAN_DATA_TYPE_BENCHMARK, &benchmark));
            channel.active_thresh[0] = static_cast<uint32_t>(benchmark * sensitivity[i]);
            if (channel.active_thresh[0] == 0)
                channel.active_thresh[0] = 1;
            ESP_ERROR_CHECK(touch_sensor_reconfig_channel(touch_channels_[i], &channel));
        }
        touch_event_callbacks_t callbacks = {};
        callbacks.on_active = TouchActive;
        callbacks.on_inactive = TouchInactive;
        ESP_ERROR_CHECK(touch_sensor_register_callbacks(touch_sensor_, &callbacks, this));
        ESP_ERROR_CHECK(touch_sensor_enable(touch_sensor_));
        ESP_ERROR_CHECK(touch_sensor_start_continuous_scanning(touch_sensor_));
#if CONFIG_UR_AI_SIGNALING_WEBRTC
#if CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO
        call_video_ = std::make_unique<sparkbot::CallVideoView>(display_);
#endif
        UrSignalingCall::UiHooks hooks;
        hooks.on_state_generation = [this](UrSignalingCall::UiState state, std::uint32_t generation) {
            const auto snapshot = call_state_.Store(state, generation);
#if CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO
            call_video_->SetActive(state == UrSignalingCall::UiState::Connecting ||
                                   state == UrSignalingCall::UiState::Active);
#endif
            Application::GetInstance().Schedule([this, state, snapshot] {
                if (!call_state_.IsCurrent(snapshot)) return;
                switch (state) {
                    case UrSignalingCall::UiState::Ringing:
                        display_->ShowNotification("来电：左侧接听，右侧拒接", 30000);
                        Application::GetInstance().PlaySound(Lang::Sounds::OGG_POPUP); break;
                    case UrSignalingCall::UiState::Connecting:
                        display_->ShowNotification("正在接通，右侧挂断", 3000); break;
                    case UrSignalingCall::UiState::Active:
                        display_->ShowNotification("通话中，右侧挂断", 3000); break;
                    case UrSignalingCall::UiState::Ended:
                        display_->ShowNotification("通话已结束", 1500); break;
                    default: break;
                }
            });
        };
#if CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO
        hooks.on_remote_frame = [this](const std::uint8_t* frame, int width, int height) {
            call_video_->Submit(frame, width, height);
        };
#endif
        UrSignalingCall::Instance().SetUiHooks(std::move(hooks));
#endif
        if (xTaskCreate(
                [](void* arg) {
                    auto* self = static_cast<EspSparkBot*>(arg);
#if CONFIG_UR_AI_SIGNALING_WEBRTC
                    sparkbot::CallTouchGate call_touch_gate;
#endif
                    for (;;) {
                        const auto active = self->touch_active_.load();
                        const auto now = esp_timer_get_time();
                        const bool chat_pressed = self->touch_debounce_.Update(active != 0, now);
                        bool chat_allowed = true;
#if CONFIG_UR_AI_SIGNALING_WEBRTC
                        const auto call_snapshot = self->call_state_.Snapshot();
                        const auto state = decltype(self->call_state_)::StateOf(call_snapshot);
                        const bool left = self->call_touch_debounce_[0].Update(active & (1U << 2), now);
                        const bool right = self->call_touch_debounce_[1].Update(active & (1U << 3), now);
                        const auto call_state = state == UrSignalingCall::UiState::Ringing
                            ? sparkbot::CallState::Ringing
                            : (state == UrSignalingCall::UiState::Connecting
                                ? sparkbot::CallState::Connecting
                                : (state == UrSignalingCall::UiState::Active
                                    ? sparkbot::CallState::Active
                                    : (state == UrSignalingCall::UiState::Ended
                                        ? sparkbot::CallState::Ended : sparkbot::CallState::Hidden)));
                        const auto action = call_touch_gate.Update(call_state, active, left, right,
                                                                   call_snapshot.generation);
                        chat_allowed = call_touch_gate.AllowChat();
                        // 任一通话阶段独占触点；无通话时维持原有小智对话操作。
                        if (state != UrSignalingCall::UiState::Hidden) {
                            auto& call = UrSignalingCall::Instance();
                            if (action == sparkbot::CallAction::Accept) call.UiAccept(call_snapshot.generation);
                            else if (action == sparkbot::CallAction::Reject) call.UiReject(call_snapshot.generation);
                            else if (action == sparkbot::CallAction::HangUp) call.UiHangUp(call_snapshot.generation);
                        } else
#endif
                        if (chat_pressed && chat_allowed) {
                            ESP_LOGI(TAG, "touch stable press -> toggle chat");
                            Application::GetInstance().Schedule(
#if CONFIG_UR_AI_SIGNALING_WEBRTC
                                [self, call_snapshot]() {
                                    // 队列等待期间发生过来电时，不执行旧的触摸对话动作。
                                    if (!self->call_state_.AllowsChat(
                                            call_snapshot, UrSignalingCall::UiState::Hidden))
                                        return;
#else
                                []() {
#endif
                                    auto& app = Application::GetInstance();
                                    if (app.GetDeviceState() != kDeviceStateStarting)
                                        app.ToggleChatState();
                                });
                        }
                        vTaskDelay(pdMS_TO_TICKS(20));
                    }
                },
                "tank_touch", 3072, this, 3, nullptr) != pdPASS) {
            ESP_LOGE(TAG, "touch task allocation failed");
            ESP_ERROR_CHECK(touch_sensor_stop_continuous_scanning(touch_sensor_));
        }
        ESP_LOGI(TAG, "touch buttons ready (top/left/right -> toggle chat)");
    }

    void InitializeDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS_GPIO;
        io_config.dc_gpio_num = DISPLAY_DC_GPIO;
        io_config.spi_mode = 0;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片
        ESP_LOGD(TAG, "Install LCD driver");

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));

        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, true);
        esp_lcd_panel_disp_on_off(panel, true);
        display_ = new SpiLcdDisplay(panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                     DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X,
                                     DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

    void InitializeCamera() {
        // DVP pin configuration
        static esp_cam_ctlr_dvp_pin_config_t dvp_pin_config = {
            .data_width = CAM_CTLR_DATA_WIDTH_8,
            .data_io =
                {
                    [0] = SPARKBOT_CAMERA_D0,
                    [1] = SPARKBOT_CAMERA_D1,
                    [2] = SPARKBOT_CAMERA_D2,
                    [3] = SPARKBOT_CAMERA_D3,
                    [4] = SPARKBOT_CAMERA_D4,
                    [5] = SPARKBOT_CAMERA_D5,
                    [6] = SPARKBOT_CAMERA_D6,
                    [7] = SPARKBOT_CAMERA_D7,
                },
            .vsync_io = SPARKBOT_CAMERA_VSYNC,
            .de_io = SPARKBOT_CAMERA_HSYNC,
            .pclk_io = SPARKBOT_CAMERA_PCLK,
            .xclk_io = SPARKBOT_CAMERA_XCLK,
        };

        // 复用 I2C 总线
        esp_video_init_sccb_config_t sccb_config = {
            .init_sccb = false,      // 不初始化新的 SCCB，使用现有的 I2C 总线
            .i2c_handle = i2c_bus_,  // 使用现有的 I2C 总线句柄
            .freq = 100000,          // 100kHz
        };

        // DVP configuration
        esp_video_init_dvp_config_t dvp_config = {
            .sccb_config = sccb_config,
            .reset_pin = SPARKBOT_CAMERA_RESET,
            .pwdn_pin = SPARKBOT_CAMERA_PWDN,
            .dvp_pin = dvp_pin_config,
            .xclk_freq = SPARKBOT_CAMERA_XCLK_FREQ,
        };

        // Main video configuration
        esp_video_init_config_t video_config = {
            .dvp = &dvp_config,
        };

        camera_ = new EspVideo(video_config);

        Settings settings("sparkbot", false);
        // 未保存偏好时按坦克原生安装方向输出，显式保存的翻转设置仍优先。
        const bool camera_flipped = static_cast<bool>(settings.GetInt("camera-flipped", 0));
        // 记录驱动应用结果，不能把设置调用当作图像方向已经改变。
        const bool hmirror_applied = camera_->SetHMirror(camera_flipped);
        const bool vflip_applied = camera_->SetVFlip(camera_flipped);
        ESP_LOGI(TAG, "Camera orientation flipped=%d hmirror_applied=%d vflip_applied=%d",
                 camera_flipped, hmirror_applied, vflip_applied);
        if (!hmirror_applied || !vflip_applied) {
            ESP_LOGE(TAG, "Camera orientation configuration failed");
        }
    }

    /*
        ESP-SparkBot 的底座
        https://gitee.com/esp-friends/esp_sparkbot/tree/master/example/tank/c2_tracked_chassis
    */
    void InitializeEchoUart() {
        uart_config_t uart_config = {
            .baud_rate = ECHO_UART_BAUD_RATE,
            .data_bits = UART_DATA_8_BITS,
            .parity = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };
        int intr_alloc_flags = 0;

        ESP_ERROR_CHECK(
            uart_driver_install(ECHO_UART_PORT_NUM, BUF_SIZE * 2, 0, 0, NULL, intr_alloc_flags));
        ESP_ERROR_CHECK(uart_param_config(ECHO_UART_PORT_NUM, &uart_config));
        ESP_ERROR_CHECK(uart_set_pin(ECHO_UART_PORT_NUM, UART_ECHO_TXD, UART_ECHO_RXD,
                                     UART_ECHO_RTS, UART_ECHO_CTS));

        // 官方底盘会回显收到的字节；独立读取，区分主控发送与底盘实际收包。
        // 回显只能证明UART往返，不能代替电机或履带的物理反馈。
        if (xTaskCreate(
                [](void*) {
                    char received[128];    // 每次有界读取，避免底盘异常输出耗尽内存。
                    int64_t next_sample_us = 0;  // 限制回显样本频率，避免分包边界变化绕过去重。
                    uint32_t total = 0;    // 当前启动累计收到的底盘字节数。
                    int64_t next_report_us = esp_timer_get_time() + 5000000;  // 首次5秒报告RX活性。
                    for (;;) {
                        const int length = uart_read_bytes(
                            ECHO_UART_PORT_NUM, received, sizeof(received) - 1, pdMS_TO_TICKS(100));
                        if (length > 0) {
                            total += length;
                            // 底盘输出作为日志数据处理，不透传控制字符到调试终端。
                            for (int i = 0; i < length; ++i) {
                                if (received[i] < 0x20 || received[i] > 0x7e)
                                    received[i] = '?';
                            }
                            received[length] = '\0';
                            const int64_t sample_us = esp_timer_get_time();
                            if (sample_us >= next_sample_us) {
                                ESP_LOGI(TAG, "Chassis UART RX len=%d data=%s", length, received);
                                next_sample_us = sample_us + 5000000;
                            }
                        }
                        const int64_t now_us = esp_timer_get_time();
                        if (now_us >= next_report_us) {
                            ESP_LOGI(TAG, "Chassis UART RX total=%lu bytes",
                                     static_cast<unsigned long>(total));
                            next_report_us = now_us + 30000000;
                        }
                    }
                },
                "ur_chassis_rx", 3072, nullptr, 5, nullptr) != pdPASS) {
            ESP_LOGE(TAG, "chassis UART RX task creation failed");
        }
        ESP_LOGI(TAG, "Chassis UART ready: tx=%d rx=%d baud=%d", UART_ECHO_TXD, UART_ECHO_RXD,
                 ECHO_UART_BAUD_RATE);

        // 第一个周期先发送零速，第二个周期发送默认灯光，避免底盘将两条指令合包。
    }

    // SendUartMessage 原子发送一条底盘命令；心跳保持速度但仅在指令改变时记录日志。
    void SendUartMessage(const char* command_str) {
        std::lock_guard<std::mutex> lock(ur_uart_mutex_);
        const size_t length = strlen(command_str);
        if (uart_write_bytes(ECHO_UART_PORT_NUM, command_str, length) != static_cast<int>(length)) {
            ESP_LOGE(TAG, "chassis UART write failed");
            return;
        }
        if (ur_last_command_ != command_str) {
            ur_last_command_ = command_str;
            ESP_LOGI(TAG, "Sent command: %s", command_str);
        }
    }

    // UrWriteMotionLocked 发送当前速度；调用方须持有运动锁，保证停止后旧舞步不能复活。
    void UrWriteMotionLocked() {
        const auto frame = ur_motion_.Current(esp_timer_get_time() / 1000, ur_speed_);
        if (!ur_motion_priority_ && ur_pending_light_ != 0) {
            char light[3] = {'w', static_cast<char>('0' + ur_pending_light_), 0};
            SendUartMessage(light);
            ur_pending_light_ = 0;
            return;
        }
        ur_motion_priority_ = false;
        char command[48];
        // 两位小数保留百分比速度，避免低速因一位小数被舍入成零。
        snprintf(command, sizeof(command), "x%.2f y%.2f", frame.x, frame.y);
        SendUartMessage(command);
    }

    // UrSetLight 将灯光排入同一个50ms发送节拍，避免底盘20ms接收窗口吞掉后续指令。
    void UrSetLight(int mode) {
        bool changed = false;  // 只有设置变化才写NVS，避免心跳或重复指令磨损Flash。
        {
            std::lock_guard<std::mutex> lock(ur_motion_mutex_);
            changed = ur_light_mode_ != mode;
            ur_pending_light_ = ur::device::SparkBotLightCommand(mode) - '0';
            ur_light_mode_ = mode;
            light_mode_ = static_cast<light_mode_t>(mode == 0 ? LIGHT_MODE_SLEEP : mode + 2);
        }
        if (changed) {
            Settings settings("sparkbot", true);
            settings.SetInt("light-mode", mode);
        }
    }

    // UrSetSpeed 保存有效速度；NVS写入放在运动锁外，避免阻塞50ms发送节拍。
    void UrSetSpeed(int speed) {
        const int value = speed < 0 ? 0 : (speed > 100 ? 100 : speed);  // 统一控制边界。
        bool changed = false;  // 重复写入同一速度不触发Flash更新。
        {
            std::lock_guard<std::mutex> lock(ur_motion_mutex_);
            changed = ur_speed_ != value;
            ur_speed_ = value;
            ur_motion_priority_ = true;
        }
        if (changed) {
            Settings settings("sparkbot", true);
            settings.SetInt("speed", value);
        }
    }

    // RestoreChassisSettings 在启动UART定时器前恢复用户设置，首次使用沿用旧默认值。
    void RestoreChassisSettings() {
        Settings settings("sparkbot", false);
        const int speed = settings.GetInt("speed", 100);  // 历史固件尚未保存该键。
        const int mode = settings.GetInt("light-mode", 1);  // 不迁移或覆盖其他板型设置。
        ur_speed_ = speed >= 0 && speed <= 100 ? speed : 100;
        ur_light_mode_ = mode >= 0 && mode <= 6 ? mode : 1;
        ur_pending_light_ = ur::device::SparkBotLightCommand(ur_light_mode_) - '0';
        light_mode_ = static_cast<light_mode_t>(ur_light_mode_ == 0 ? LIGHT_MODE_SLEEP : ur_light_mode_ + 2);
    }

    // UrStartMotion 开始限时运动；50ms心跳防止底盘自身约500ms看门狗提前停车。
    void UrStartMotion(float x, float y, uint32_t timeout_ms) {
        std::lock_guard<std::mutex> lock(ur_motion_mutex_);
        ur_motion_.Start(x, y, esp_timer_get_time() / 1000, timeout_ms);
        ur_motion_priority_ = true;
    }

    // UrStartDance 在主控上推进官方舞步，使停止/断连可取消全部后续动作。
    void UrStartDance() {
        std::lock_guard<std::mutex> lock(ur_motion_mutex_);
        ur_motion_.Dance(esp_timer_get_time() / 1000);
        ur_motion_priority_ = true;
    }

    // UrStopChassis 停止并清除运动状态；显式停止与MQTT断连共用。
    void UrStopChassis() {
        std::lock_guard<std::mutex> lock(ur_motion_mutex_);
        ur_motion_.Stop();
        ur_motion_priority_ = true;
    }

    // UrMotionStopCallback 只推进短时底盘输出，不阻塞等待舞步，不操作音频状态机。
    static void UrMotionStopCallback(void* arg) {
        auto* board = static_cast<EspSparkBot*>(arg);
        std::lock_guard<std::mutex> lock(board->ur_motion_mutex_);
        board->UrWriteMotionLocked();
    }

    // InitializeUrMotionTimer 周期刷新速度；即使主控失联，底盘仍保留自身停机看门狗。
    void InitializeUrMotionTimer() {
        const esp_timer_create_args_t timer_args = {
            .callback = UrMotionStopCallback,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "ur_motion_stop",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &ur_motion_timer_));
        ESP_ERROR_CHECK(esp_timer_start_periodic(ur_motion_timer_, 50000));
    }

    /* 物模型行为映射；返回 HTTP 风格 code，404 表示行为不属于坦克。 */
    int HandleUrAction(const std::string& action_id, const std::string& params_json) override {
        if (action_id == "tankForward") {
            UrStartMotion(0.0f, 1.0f, kMotionTimeoutMs);
#if CONFIG_UR_AI_E2E_TEST
            // 标记正式行为入口已执行；底盘接收仍须由独立C2日志确认。
            ESP_LOGI(TAG, "[UR_VOICE_MOTION_E2E] action=tankForward");
#endif
        } else if (action_id == "tankBackward") {
            UrStartMotion(0.0f, -1.0f, kMotionTimeoutMs);
        } else if (action_id == "tankTurnLeft") {
            UrStartMotion(-1.0f, 0.0f, kMotionTimeoutMs);
        } else if (action_id == "tankTurnRight") {
            UrStartMotion(1.0f, 0.0f, kMotionTimeoutMs);
        } else if (action_id == "tankStop") {
            UrStopChassis();
        } else if (action_id == "tankDance") {
            UrStartDance();
        } else if (action_id == "tankSetLight") {
            int mode = 0;
            cJSON* params = cJSON_Parse(params_json.c_str());
            if (params != nullptr) {
                cJSON* item = cJSON_GetObjectItemCaseSensitive(params, "LightMode");
                if (cJSON_IsNumber(item)) {
                    mode = item->valueint;
                }
                cJSON_Delete(params);
            }
            if (mode < 0 || mode > 6) {
                return 400;
            }
            UrSetLight(mode);
        } else {
            return 404;
        }
        return 200;
    }

    /* 物模型属性控制：LightMode（0兼容休眠、1-6灯效）、Speed（0-100 运动幅度）与 Volume（0-100 音量）。 */
    int HandleUrControl(const cJSON* params, std::string* reply_data) override {
        int handled = 0;
        std::string reply = "{";
        cJSON* volume = cJSON_GetObjectItemCaseSensitive(params, "Volume");
        if (cJSON_IsNumber(volume)) {
            int value =
                volume->valueint < 0 ? 0 : (volume->valueint > 100 ? 100 : volume->valueint);
            Application::GetInstance().Schedule(
                [value]() { Board::GetInstance().GetAudioCodec()->SetOutputVolume(value); });
            char text[24];
            snprintf(text, sizeof(text), "\"Volume\":%d", value);
            if (handled)
                reply += ",";
            reply += text;
            handled++;
        }
        cJSON* light = cJSON_GetObjectItemCaseSensitive(params, "LightMode");
        if (cJSON_IsNumber(light)) {
            int mode = light->valueint;
            if (mode >= 0 && mode <= 6) {
                UrSetLight(mode);
                char value[16];
                snprintf(value, sizeof(value), "\"LightMode\":%d", mode);
                if (handled)
                    reply += ",";
                reply += value;
                handled++;
            }
        }
        cJSON* speed = cJSON_GetObjectItemCaseSensitive(params, "Speed");
        if (cJSON_IsNumber(speed)) {
            const int requested_speed = speed->valueint < 0 ? 0 : (speed->valueint > 100 ? 100 : speed->valueint);
            UrSetSpeed(requested_speed);
            char value[16];
            snprintf(value, sizeof(value), "\"Speed\":%d", requested_speed);
            if (handled)
                reply += ",";
            reply += value;
            handled++;
        }
        reply += "}";
        *reply_data = reply;
        return handled;
    }

    /* 上报联犀物模型属性集合；SparkBot 无电量计，电池按 0/未充电上报。 */
    std::string GetUrPropertiesJson() override {
        int battery = 0;
        bool charging = false;
        bool discharging = false;
        GetBatteryLevel(battery, charging, discharging);
        int rssi = 0;
        auto& wifi_manager = WifiManager::GetInstance();
        if (wifi_manager.IsConnected()) {
            rssi = wifi_manager.GetRssi();
        }
        int light_mode = 0;  // 同一运动锁下取得属性快照，避免与控制线程并发读写。
        int speed = 0;       // 上报使用快照，不在格式化期间读取共享状态。
        {
            std::lock_guard<std::mutex> lock(ur_motion_mutex_);
            light_mode = ur_light_mode_;
            speed = ur_speed_;
        }
        char json[224];
        snprintf(json, sizeof(json),
                 "{\"Battery\":%d,\"Charging\":%s,\"Rssi\":%d,\"LightMode\":%d,\"Speed\":%d,"
                 "\"Volume\":%d}",
                 battery, charging ? "true" : "false", rssi, light_mode, speed,
                 Board::GetInstance().GetAudioCodec()->output_volume());
        return json;
    }

    /* 联犀 MQTT 断连：立即停止底盘，防止运动指令残留。 */
    void OnUrDisconnected() override { UrStopChassis(); }

#if CONFIG_UR_AI_E2E_TEST
    // InitializeTestConsole 只在测试包开放循环语音和真实行为入口；正式包不注册命令。
    void InitializeTestConsole() {
        esp_console_repl_t* repl = nullptr;
        esp_console_repl_config_t config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
        config.prompt = "Tank>";
        config.task_stack_size = 12288;
        esp_console_dev_usb_serial_jtag_config_t hardware =
            ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hardware, &config, &repl));
#if !CONFIG_UR_AI_SIGNALING
        const esp_console_cmd_t voice = {
            .command = "ur_ai_e2e",
            .help = "ur_ai_e2e [1-5] [photo|control|forward]; control requires volume55",
            .hint = nullptr,
            .func = [](int argc, char** argv) -> int {
                if (argc > 3 ||
                    (argc >= 2 && (strlen(argv[1]) != 1 || argv[1][0] < '1' || argv[1][0] > '5')) ||
                    (argc == 3 && strcmp(argv[2], "photo") != 0 && strcmp(argv[2], "control") != 0 &&
                     strcmp(argv[2], "forward") != 0))
                    return 1;
                const int turns = argc >= 2 ? argv[1][0] - '0' : 1;
                const bool photo = argc == 3 && strcmp(argv[2], "photo") == 0;
                const bool control = argc == 3 && strcmp(argv[2], "control") == 0;
                const bool forward = argc == 3 && strcmp(argv[2], "forward") == 0;
                if ((control || forward) && turns != 1)
                    return 1;
                return Application::GetInstance().RunUrAiE2eTest(turns, photo, control, forward) ? 0 : 1;
            },
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&voice));
#endif
#if CONFIG_UR_AI_SIGNALING_WEBRTC
        // 测试包的串口入口只投递真实通话命令，不作为物理电容触点验收。
        const esp_console_cmd_t call = {
            .command = "sig_call",
            .help = "human call control: sig_call <accept|reject|hangup|mic|cam>",
            .hint = nullptr,
            .func = [](int argc, char** argv) -> int {
                if (argc != 2) return 1;
                auto& call = UrSignalingCall::Instance();
                if (strcmp(argv[1], "accept") == 0) call.UiAccept();
                else if (strcmp(argv[1], "reject") == 0) call.UiReject();
                else if (strcmp(argv[1], "hangup") == 0) call.UiHangUp();
                else if (strcmp(argv[1], "mic") == 0) call.UiToggleMic();
                else if (strcmp(argv[1], "cam") == 0) call.UiToggleCamera();
                else return 1;
                printf("sig_call %s dispatched\n", argv[1]);
                return 0;
            },
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&call));
        RegisterSparkbotSignalingTestCommands();
#endif
        const esp_console_cmd_t action = {
            .command = "ur_tank_action",
            .help = "ur_tank_action <actionID> [LightMode 0-6]",
            .hint = nullptr,
            .func = nullptr,
            .argtable = nullptr,
            .func_w_context = [](void* context, int argc, char** argv) -> int {
                if (argc < 2 || argc > 3)
                    return 1;
                std::string params = "{}";
                if (argc == 3) {
                    if (strlen(argv[2]) != 1 || argv[2][0] < '0' || argv[2][0] > '6')
                        return 1;
                    params = std::string("{\"LightMode\":") + argv[2] + "}";
                }
                const int code =
                    static_cast<EspSparkBot*>(context)->HandleUrAction(argv[1], params);
                ESP_LOGI(TAG, "[UR_TANK_TEST] action=%s code=%d", argv[1], code);
                return code == 200 ? 0 : 1;
            },
            .context = this,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&action));
        ESP_ERROR_CHECK(esp_console_start_repl(repl));
    }
#endif

    /* 联网成功后启动联犀 MQTT 设备任务（动态注册/物模型上报/控制），与 Watcher 板同模式。 */
    virtual void StartNetwork() override {
        WifiBoard::StartNetwork();
        ur_device_task_start();
    }

    void InitializeTools() {
        auto& mcp_server = McpServer::GetInstance();
        // 定义设备的属性
        mcp_server.AddTool(
            "self.chassis.get_light_mode", "获取灯光效果编号", PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                std::lock_guard<std::mutex> lock(ur_motion_mutex_);
                return ur_light_mode_;
            });

        mcp_server.AddTool("self.chassis.go_forward", "前进", PropertyList(),
                           [this](const PropertyList& properties) -> ReturnValue {
                               UrStartMotion(0, 1, kMotionTimeoutMs);
                               return true;
                           });

        mcp_server.AddTool("self.chassis.go_back", "后退", PropertyList(),
                           [this](const PropertyList& properties) -> ReturnValue {
                               UrStartMotion(0, -1, kMotionTimeoutMs);
                               return true;
                           });

        mcp_server.AddTool("self.chassis.turn_left", "向左转", PropertyList(),
                           [this](const PropertyList& properties) -> ReturnValue {
                               UrStartMotion(-1, 0, kMotionTimeoutMs);
                               return true;
                           });

        mcp_server.AddTool("self.chassis.turn_right", "向右转", PropertyList(),
                           [this](const PropertyList& properties) -> ReturnValue {
                               UrStartMotion(1, 0, kMotionTimeoutMs);
                               return true;
                           });

        mcp_server.AddTool("self.chassis.dance", "跳舞", PropertyList(),
                           [this](const PropertyList& properties) -> ReturnValue {
                               UrStartDance();
                               // 跳舞只控制运动，保留实际灯光状态。
                               return true;
                           });

        mcp_server.AddTool("self.chassis.switch_light_mode", "打开灯光效果",
                           PropertyList({Property("light_mode", kPropertyTypeInteger, 0, 6)}),
                           [this](const PropertyList& properties) -> ToolResult {
                               const int mode = properties["light_mode"].value<int>();
                               if (mode >= 0 && mode <= 6) {
                                   UrSetLight(mode);
                                   return true;
                               }
                               return std::unexpected("Invalid light mode");
                           });

        mcp_server.AddTool("self.camera.set_camera_flipped", "翻转摄像头图像方向", PropertyList(),
                           [this](const PropertyList& properties) -> ReturnValue {
                               Settings settings("sparkbot", true);
                               // 未保存偏好时与启动默认方向一致，再切换并保存。
                               bool flipped =
                                   !static_cast<bool>(settings.GetInt("camera-flipped", 0));

                               camera_->SetHMirror(flipped);
                               camera_->SetVFlip(flipped);

                               settings.SetInt("camera-flipped", flipped ? 1 : 0);

                               return true;
                           });
    }

public:
    // SetPowerSaveLevel 将联犀坦克的空闲请求限制为平衡省电，避免十个信标周期的下行等待；语音性能请求保持原样。
    void SetPowerSaveLevel(PowerSaveLevel level) override {
#if CONFIG_UR_AI
        if (level == PowerSaveLevel::LOW_POWER) {
            level = PowerSaveLevel::BALANCED;
        }
#endif
        WifiBoard::SetPowerSaveLevel(level);
    }

    EspSparkBot() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializeI2c();
        InitializeSpi();
        InitializeDisplay();
        InitializeButtons();
        InitializeTouch();
        InitializeCamera();
        RestoreChassisSettings();
        InitializeEchoUart();
        InitializeUrMotionTimer();
        InitializeTools();
#if CONFIG_UR_AI_E2E_TEST
        InitializeTestConsole();
#endif
        GetBacklight()->RestoreBrightness();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static SparkBotEs8311AudioCodec audio_codec(
            i2c_bus_, I2C_NUM_0, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN, AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override { return display_; }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual Camera* GetCamera() override { return camera_; }
};

DECLARE_BOARD(EspSparkBot);
