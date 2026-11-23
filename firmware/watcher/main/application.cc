// 应用事件循环：协调设备状态、语音唤醒、会话及板级交互入口。
#include "application.h"
#include "assets.h"
#include "assets/lang_config.h"
#include "audio_codec.h"
#include "board.h"
#include "cjson_utils.h"
#include "display.h"
#include "mcp_server.h"
#include "mqtt_protocol.h"
#if CONFIG_UR_AI
#include "ur_ai_protocol.h"
#include "unitedrhino/ur_bridge.h"
#endif
#if CONFIG_UR_AI_SIGNALING
#include "ur_signaling_protocol.h"
#endif
#if CONFIG_UR_SPARKBOT_VOICE_MODEL_TEST
#include "ur_signaling_call.h"
#endif
#if CONFIG_UR_AI_E2E_TEST
#include "ur_ai_e2e_fixture.h"
#endif
#include "settings.h"
#include "system_info.h"
#include "text_glyph_payload.h"
#include "websocket_protocol.h"

#include <driver/gpio.h>
#include <esp_app_desc.h>
#include <esp_log.h>
#include <arpa/inet.h>
#include <cJSON.h>
#include <cstring>
#include <limits>

#if CONFIG_UR_AI_SIGNALING_WEBRTC
/* libpeer_src 的 DTLS 证书预热入口；用 extern 声明避免把 mbedtls/srtp 头
 * 引入 application.cc（libpeer_src 已由 main 组件 PRIV_REQUIRES 链接）。 */
extern "C" void dtls_srtp_prewarm_async(void);
#endif

#define TAG "Application"

Application::Application() : notify_player_(audio_service_) {
    event_group_ = xEventGroupCreate();

#if CONFIG_USE_DEVICE_AEC && CONFIG_USE_SERVER_AEC
#error "CONFIG_USE_DEVICE_AEC and CONFIG_USE_SERVER_AEC cannot be enabled at the same time"
#elif CONFIG_USE_DEVICE_AEC
    aec_mode_ = kAecOnDeviceSide;
#elif CONFIG_USE_SERVER_AEC
    aec_mode_ = kAecOnServerSide;
#else
    aec_mode_ = kAecOff;
#endif

    esp_timer_create_args_t clock_timer_args = {.callback =
                                                    [](void* arg) {
                                                        Application* app = (Application*)arg;
                                                        xEventGroupSetBits(app->event_group_,
                                                                           MAIN_EVENT_CLOCK_TICK);
                                                    },
                                                .arg = this,
                                                .dispatch_method = ESP_TIMER_TASK,
                                                .name = "clock_timer",
                                                .skip_unhandled_events = true};
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);

#if CONFIG_UR_AI_SIGNALING_WEBRTC
    /* WebRTC 媒体路径的 DTLS 自签证书在开机阶段异步预生成（RSA-1024
     * 约 650ms），避免首个及后续会话重复承担密钥生成成本。 */
    dtls_srtp_prewarm_async();
#endif
}

Application::~Application() {
    notify_player_.Stop();
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    vEventGroupDelete(event_group_);
}

bool Application::SetDeviceState(DeviceState state) { return state_machine_.TransitionTo(state); }

void Application::Initialize() {
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    // Setup the display
    auto display = board.GetDisplay();
    display->SetupUI();
    // Print board name/version info
    display->SetChatMessage("system", SystemInfo::GetUserAgent().c_str());

    // Setup the audio service
    auto codec = board.GetAudioCodec();
    audio_service_.Initialize(codec);
    audio_service_.Start();
    ESP_LOGI(TAG, "After board/audio init");
    SystemInfo::PrintHeapStats();

    AudioServiceCallbacks callbacks;
    callbacks.on_send_queue_available = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
    };
    callbacks.on_wake_word_detected = [this](const std::string& wake_word) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
    };
    callbacks.on_vad_change = [this](bool speaking) {
        /* VAD 的 true/false 都影响 audioStop，逐项排入主任务，避免 event bit 合并丢边沿。 */
        Schedule([this, speaking]() {
#if CONFIG_UR_AI_E2E_TEST
            if (ur_ai_e2e_running_.load()) {
                return;
            }
#endif
            if (GetDeviceState() != kDeviceStateListening) {
                return;
            }
            Board::GetInstance().GetLed()->OnStateChanged();
            if (protocol_ != nullptr) {
                protocol_->OnLocalVadState(speaking);
            }
        });
    };
    callbacks.on_playback_drained = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_PLAYBACK_DRAINED);
    };
    callbacks.on_playback_progress = [this](uint32_t playback_id, uint32_t media_position_ms) {
        notify_player_.OnPlaybackProgress(playback_id, media_position_ms);
    };
    audio_service_.SetCallbacks(callbacks);

    // Add state change listeners
    state_machine_.AddStateChangeListener([this](DeviceState old_state, DeviceState new_state) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
    });

    // Start the clock timer to update the status bar
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    // Add MCP common tools (only once during initialization)
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddCommonTools();
    mcp_server.AddUserOnlyTools();

    // Set network event callback for UI updates and network state handling
    board.SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
        auto display = Board::GetInstance().GetDisplay();

        switch (event) {
            case NetworkEvent::Scanning:
                display->ShowNotification(Lang::Strings::SCANNING_WIFI, 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::Connecting: {
                if (data.empty()) {
                    // Cellular network - registering without carrier info yet
                    display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                } else {
                    // WiFi or cellular with carrier info
                    std::string msg = Lang::Strings::CONNECT_TO;
                    msg += data;
                    msg += "...";
                    display->ShowNotification(msg.c_str(), 30000);
                }
                break;
            }
            case NetworkEvent::Connected: {
                std::string msg = Lang::Strings::CONNECTED_TO;
                msg += data;
                display->ShowNotification(msg.c_str(), 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_CONNECTED);
                break;
            }
            case NetworkEvent::Disconnected:
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::WifiConfigModeEnter:
                // WiFi config mode enter is handled by WifiBoard internally
                break;
            case NetworkEvent::WifiConfigModeExit:
                // WiFi config mode exit is handled by WifiBoard internally
                break;
            // Cellular modem specific events
            case NetworkEvent::ModemDetecting:
                display->SetStatus(Lang::Strings::DETECTING_MODULE);
                break;
            case NetworkEvent::ModemErrorNoSim:
                Alert(Lang::Strings::ERROR, Lang::Strings::PIN_ERROR, "warning",
                      Lang::Sounds::OGG_ERR_PIN);
                break;
            case NetworkEvent::ModemErrorRegDenied:
                Alert(Lang::Strings::ERROR, Lang::Strings::REG_ERROR, "warning",
                      Lang::Sounds::OGG_ERR_REG);
                break;
            case NetworkEvent::ModemErrorInitFailed:
                Alert(Lang::Strings::ERROR, Lang::Strings::MODEM_INIT_ERROR, "warning",
                      Lang::Sounds::OGG_EXCLAMATION);
                break;
            case NetworkEvent::ModemErrorTimeout:
                display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                break;
        }
    });

    // Start network asynchronously
    board.StartNetwork();

    // Update the status bar immediately to show the network state
    display->UpdateStatusBar(true);
}

void Application::Run() {
    // Set the priority of the main task to 10
    vTaskPrioritySet(nullptr, 10);

    const EventBits_t ALL_EVENTS =
        MAIN_EVENT_SCHEDULE | MAIN_EVENT_SEND_AUDIO | MAIN_EVENT_WAKE_WORD_DETECTED |
        MAIN_EVENT_CLOCK_TICK | MAIN_EVENT_ERROR |
        MAIN_EVENT_NETWORK_CONNECTED | MAIN_EVENT_NETWORK_DISCONNECTED | MAIN_EVENT_TOGGLE_CHAT |
        MAIN_EVENT_START_LISTENING | MAIN_EVENT_STOP_LISTENING | MAIN_EVENT_ACTIVATION_DONE |
        MAIN_EVENT_STATE_CHANGED | MAIN_EVENT_PLAYBACK_DRAINED | MAIN_EVENT_TAKE_PHOTO;

    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, ALL_EVENTS, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & MAIN_EVENT_ERROR) {
            if (GetDeviceState() == kDeviceStateNotifying) {
                StopNotification();
            }
#if CONFIG_UR_AI
            BeginUrAiTurn();
#endif
            SetDeviceState(kDeviceStateIdle);
            Alert(Lang::Strings::ERROR, last_error_message_.c_str(), "cancel",
                  Lang::Sounds::OGG_EXCLAMATION);
        }

        if (bits & MAIN_EVENT_NETWORK_CONNECTED) {
            HandleNetworkConnectedEvent();
        }

        if (bits & MAIN_EVENT_NETWORK_DISCONNECTED) {
            HandleNetworkDisconnectedEvent();
        }

        if (bits & MAIN_EVENT_ACTIVATION_DONE) {
            HandleActivationDoneEvent();
        }

        if (bits & MAIN_EVENT_STATE_CHANGED) {
            HandleStateChangedEvent();
        }

        if (bits & MAIN_EVENT_PLAYBACK_DRAINED) {
            if (audio_service_.IsPlaybackIdle()) {
                notify_player_.OnPlaybackDrained();
#if CONFIG_UR_AI
                ReleaseUrEmotionIfReady();
#endif
            }
            // Deferred listening start (auto mode): the playback queue has
            // drained, so it is now safe to enable voice processing.
            if (pending_listening_start_ && GetDeviceState() == kDeviceStateListening &&
                audio_service_.IsPlaybackIdle()
#if CONFIG_UR_AI
                && !ur_emotion_hold_
#endif
            ) {
                pending_listening_start_ = false;
                ESP_LOGI(TAG, "pending listening start fulfilled (playback drained)");
                StartListeningAudio();
            } else if (pending_listening_start_) {
                /* 兑现失败要可见：该分支是媒体中断场景"回聆听"落空的观测点
                 * （单次边沿错过即无人再试，设备从此不上行）。 */
                ESP_LOGW(TAG, "pending listening start held: state=%d idle=%d",
                         static_cast<int>(GetDeviceState()),
                         static_cast<int>(audio_service_.IsPlaybackIdle()));
            }
        }

        if (bits & MAIN_EVENT_TOGGLE_CHAT) {
            HandleToggleChatEvent();
        }

        if (bits & MAIN_EVENT_START_LISTENING) {
            HandleStartListeningEvent();
        }

        if (bits & MAIN_EVENT_STOP_LISTENING) {
            HandleStopListeningEvent();
        }

#if CONFIG_UR_AI
        if (bits & MAIN_EVENT_TAKE_PHOTO) {
            HandleTakePhotoEvent();
        }
#endif

        // 先兑现显示帧和通话状态任务；音频发送队列可能在半双工恢复时一次积压多帧，
        // 若先无界排空会让 LVGL 画面一直等在主任务，设备端网页画面只能落到约 3 fps。
        if (bits & MAIN_EVENT_SCHEDULE) {
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                task();
            }
        }

        if (bits & MAIN_EVENT_SEND_AUDIO) {
#if CONFIG_UR_AI_E2E_TEST
            if (ur_ai_e2e_running_.load()) {
                /* 自动测试直接发送内置 Opus，必须清掉同时采集的麦克风帧，避免污染 ASR。 */
                while (audio_service_.PopPacketFromSendQueue())
                    ;
            } else {
#endif
            while (auto packet = audio_service_.PopPacketFromSendQueue()) {
                if (protocol_ && !protocol_->SendAudio(std::move(packet))) {
                    // Drop the remaining packets. Leaving them in the queue would
                    // stall the Opus codec task (it waits for queue space), which in
                    // turn deadlocks the whole audio input pipeline, as no new
                    // MAIN_EVENT_SEND_AUDIO event would ever be triggered again.
                    while (audio_service_.PopPacketFromSendQueue())
                        ;
                    break;
                }
            }
#if CONFIG_UR_AI_E2E_TEST
            }
#endif
        }

        if (bits & MAIN_EVENT_WAKE_WORD_DETECTED) {
            HandleWakeWordDetectedEvent();
        }

        if (bits & MAIN_EVENT_CLOCK_TICK) {
            clock_ticks_++;
#if CONFIG_UR_AI_SIGNALING
            /* 信令启动失败兜底重试（每 5s）：身份未就绪/连接任务未完成时
             * Start 失败即返回 false，此处持续幂等重试直至任务真正跑起。 */
            if (clock_ticks_ % 5 == 0) {
                EnsureSignalingStarted();
            }
#endif
#if CONFIG_UR_AI
            ReleaseUrEmotionIfReady();
            /* 延迟开麦自愈（约每秒一次）：原兑现只在 PLAYBACK_DRAINED / 表情释放
             * 两个边沿触发，媒体中断场景两个边沿都可能整体错过（真机实证：设备已回
             * 监听态却一直不开麦、用户说话无上行）。这里按电平重试同一组条件；
             * 表情保持只影响观感，超过 3 秒仍未释放即放行，保证延迟开麦不会永久卡住。 */
            if (pending_listening_start_ && GetDeviceState() == kDeviceStateListening &&
                audio_service_.IsPlaybackIdle() &&
                (!ur_emotion_hold_ ||
                 esp_timer_get_time() - pending_listening_since_us_ >= 3LL * 1000 * 1000)) {
                if (ur_emotion_hold_) {
                    ESP_LOGW(TAG, "pending listening start fulfilled (tick, emotion hold expired)");
                } else {
                    ESP_LOGI(TAG, "pending listening start fulfilled (tick)");
                }
                pending_listening_start_ = false;
                StartListeningAudio();
            }
#endif
            auto display = Board::GetInstance().GetDisplay();
            display->UpdateStatusBar();

            // Print debug info every 10 seconds
            if (clock_ticks_ % 10 == 0) {
                SystemInfo::PrintHeapStats();
                // SystemInfo::PrintTaskList();
                // SystemInfo::PrintTaskCpuUsage(pdMS_TO_TICKS(1000));
            }
        }
    }
}

#if CONFIG_UR_AI_SIGNALING
void Application::EnsureSignalingStarted() {
    if (protocol_ == nullptr) {
        return;
    }
    if (protocol_->Start()) {
        return;
    }
    /* 失败属启动竞态（MQTT 设备身份尚未填充）：不打扰用户，由 tick 持续重试。 */
    ESP_LOGW(TAG, "signaling start deferred, will retry");
}
#endif

void Application::HandleNetworkConnectedEvent() {
    ESP_LOGI(TAG, "Network connected");
#if CONFIG_UR_AI_SIGNALING
    /* WiFi 就绪即重试一次信令启动：覆盖 MQTT 身份在连接任务中稍后才填充的
     * 主流时序；更晚就绪的尾巴由秒级 tick 兜底（见 EnsureSignalingStarted）。 */
    EnsureSignalingStarted();
#endif
    auto state = GetDeviceState();

    if (state == kDeviceStateStarting || state == kDeviceStateWifiConfiguring) {
        // Network is ready, start activation
        SetDeviceState(kDeviceStateActivating);
        if (activation_task_handle_ != nullptr) {
            ESP_LOGW(TAG, "Activation task already running");
            return;
        }

        xTaskCreate(
            [](void* arg) {
                Application* app = static_cast<Application*>(arg);
                app->ActivationTask();
                app->activation_task_handle_ = nullptr;
                vTaskDelete(NULL);
            },
            "activation", 4096 * 2, this, 2, &activation_task_handle_);
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleNetworkDisconnectedEvent() {
    // Close current conversation when network disconnected
    auto state = GetDeviceState();
    if (state == kDeviceStateNotifying) {
        StopNotification();
    }
    if (state == kDeviceStateConnecting || state == kDeviceStateListening ||
        state == kDeviceStateSpeaking) {
        ESP_LOGI(TAG, "Closing audio channel due to network disconnection");
        protocol_->CloseAudioChannel();
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleActivationDoneEvent() {
    ESP_LOGI(TAG, "Activation done");

    SystemInfo::PrintHeapStats();
    SetDeviceState(kDeviceStateIdle);

    has_server_time_ = ota_->HasServerTime();

    // Protocol start may have already raised MAIN_EVENT_ERROR. Do not replace
    // that alert with the "ready" UI/sound — the main loop can process both
    // events back-to-back because the activation task is lower priority.
    const bool has_error = !last_error_message_.empty();
    if (!has_error) {
        auto display = Board::GetInstance().GetDisplay();
#if CONFIG_UR_AI
        std::string message =
            std::string(Lang::Strings::VERSION) + esp_app_get_description()->version;
#else
        std::string message = std::string(Lang::Strings::VERSION) + ota_->GetCurrentVersion();
#endif
        display->ShowNotification(message.c_str());
        display->SetChatMessage("system", "");
    }

    // Release OTA object after activation is complete
    ota_.reset();
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);

    if (!has_error) {
        Schedule([this]() {
            // Play the success sound to indicate the device is ready
            audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS);
        });
    }
}

void Application::ActivationTask() {
    // Create OTA object for activation process
    ota_ = std::make_unique<Ota>();

    // Check for new assets version
    CheckAssetsVersion();

    // 联犀 AI 板型由平台 OTA 独占升级，不请求公共小智激活与语音配置。
#if !CONFIG_UR_AI
    CheckNewVersion();
#endif

    // Initialize the protocol
    InitializeProtocol();

    // Signal completion to main loop
    xEventGroupSetBits(event_group_, MAIN_EVENT_ACTIVATION_DONE);
}

void Application::CheckAssetsVersion() {
    // Only allow CheckAssetsVersion to be called once
    if (assets_version_checked_) {
        return;
    }
    assets_version_checked_ = true;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto& assets = Assets::GetInstance();

    if (!assets.partition_valid()) {
        ESP_LOGW(TAG, "Assets partition is disabled for board %s", BOARD_NAME);
        return;
    }

    Settings settings("assets", true);
    // Check if there is a new assets need to be downloaded
    std::string download_url = settings.GetString("download_url");

    if (!download_url.empty()) {
        settings.EraseKey("download_url");

        char message[256];
        snprintf(message, sizeof(message), Lang::Strings::FOUND_NEW_ASSETS, download_url.c_str());
        Alert(Lang::Strings::LOADING_ASSETS, message, "cloud_download", Lang::Sounds::OGG_UPGRADE);

        // Wait for the audio service to be idle for 3 seconds
        vTaskDelay(pdMS_TO_TICKS(3000));
        SetDeviceState(kDeviceStateUpgrading);
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        display->SetChatMessage("system", Lang::Strings::PLEASE_WAIT);

        bool success =
            assets.Download(download_url, [this, display](int progress, size_t speed) -> void {
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
                Schedule([display, message = std::string(buffer)]() {
                    display->SetChatMessage("system", message.c_str());
                });
            });

        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!success) {
            Alert(Lang::Strings::ERROR, Lang::Strings::DOWNLOAD_ASSETS_FAILED, "cancel",
                  Lang::Sounds::OGG_EXCLAMATION);
            vTaskDelay(pdMS_TO_TICKS(2000));
            SetDeviceState(kDeviceStateActivating);
            return;
        }
    }

    // Apply assets
    assets.Apply();
    display->SetChatMessage("system", "");
    display->SetEmotion("robot_2");
}

void Application::CheckNewVersion() {
    const int MAX_RETRY = 10;
    int retry_count = 0;
    int retry_delay = 10;  // Initial retry delay in seconds

    auto& board = Board::GetInstance();
    while (true) {
        auto display = board.GetDisplay();
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

        auto check = ota_->CheckVersion();
        if (!check) {
            retry_count++;
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                return;
            }

            const auto& err = check.error();
            char error_message[160];
            int error_message_length =
                snprintf(error_message, sizeof(error_message), "%s", err.ToString().c_str());
            if (error_message_length < 0 ||
                error_message_length >= static_cast<int>(sizeof(error_message))) {
                snprintf(error_message, sizeof(error_message), "%s", err.Message());
            }

            char buffer[320];
            int alert_message_length =
                snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED,
                         retry_delay, error_message);
            if (alert_message_length < 0 ||
                alert_message_length >= static_cast<int>(sizeof(buffer))) {
                snprintf(buffer, sizeof(buffer), "%s", err.Message());
            }
            Alert(Lang::Strings::ERROR, buffer, "cloud_off", Lang::Sounds::OGG_EXCLAMATION);

            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", retry_delay,
                     retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (GetDeviceState() == kDeviceStateIdle) {
                    break;
                }
            }
            retry_delay *= 2;  // Double the retry delay
            continue;
        }
        retry_count = 0;
        retry_delay = 10;  // Reset retry delay

#if !CONFIG_UR_PLATFORM_OTA
        if (ota_->HasNewVersion()) {
            if (UpgradeFirmware(ota_->GetFirmwareUrl(), ota_->GetFirmwareVersion())) {
                return;  // This line will never be reached after reboot
            }
            // If upgrade failed, continue to normal operation
        }

        // No new version, mark the current version as valid
        ota_->MarkCurrentVersionValid();
#else
        /* Watcher 的镜像有效性只由联犀 reportInfoReply 确认，不能由公共 OTA 检查提前确认。 */
        if (ota_->HasNewVersion()) {
            ESP_LOGI(TAG, "Ignoring public OTA firmware because UnitedRhino OTA is enabled");
        }
#endif
        if (!ota_->HasActivationCode() && !ota_->HasActivationChallenge()) {
            // Exit the loop if done checking new version
            break;
        }

        display->SetStatus(Lang::Strings::ACTIVATION);
        // Activation code is shown to the user and waiting for the user to input
        if (ota_->HasActivationCode()) {
            ShowActivationCode(ota_->GetActivationCode(), ota_->GetActivationMessage());
        }

        // This will block the loop until the activation is done or timeout
        for (int i = 0; i < 10; ++i) {
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t err = ota_->Activate();
            if (err == ESP_OK) {
                break;
            } else if (err == ESP_ERR_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(3000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
            if (GetDeviceState() == kDeviceStateIdle) {
                break;
            }
        }
    }
}

void Application::InitializeProtocol() {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto codec = board.GetAudioCodec();

    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

#if CONFIG_UR_AI_SIGNALING
    /* 统一信令新版协议；音频经 PCM 分流直采，旧 60ms 发送队列不再上行。 */
    protocol_ = std::make_unique<SignalingProtocol>();
#elif CONFIG_UR_AI
    protocol_ = std::make_unique<UrAiProtocol>();
#else
    if (ota_->HasMqttConfig()) {
        protocol_ = std::make_unique<MqttProtocol>();
    } else if (ota_->HasWebsocketConfig()) {
        protocol_ = std::make_unique<WebsocketProtocol>();
    } else {
        ESP_LOGW(TAG, "No protocol specified in the OTA config, using MQTT");
        protocol_ = std::make_unique<MqttProtocol>();
    }
#endif

    protocol_->OnConnected([this]() { DismissAlert(); });

    protocol_->OnNetworkError([this](const std::string& message) {
        Board::GetInstance().GetDisplay()->CancelPhotoStage();
        last_error_message_ = message;
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
    });

    protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
#if CONFIG_UR_AI
        /* 联犀以首个 UDP 帧触发 speaking；与同批帧一并排入主任务以保持状态顺序。 */
        auto shared_packet = std::shared_ptr<AudioStreamPacket>(std::move(packet));
        Schedule([this, shared_packet]() {
            if (GetDeviceState() == kDeviceStateSpeaking) {
                auto queued_packet = std::make_unique<AudioStreamPacket>(std::move(*shared_packet));
                audio_service_.PushPacketToDecodeQueue(std::move(queued_packet));
            }
        });
#else
        if (GetDeviceState() == kDeviceStateSpeaking) {
            audio_service_.PushPacketToDecodeQueue(std::move(packet));
        }
#endif
    });

    protocol_->OnAudioChannelOpened([this, codec, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
#if CONFIG_UR_AI_SIGNALING
        /* 全双工会话在播放期间保持采集；半双工沿用逐轮收发状态机。 */
        if (protocol_->IsFullDuplexSession()) {
            listening_mode_ = kListeningModeRealtime;
        } else {
            listening_mode_ = kListeningModeAutoStop;
        }
#if CONFIG_UR_AI_SIGNALING_DEVICE_AEC
        /* 本构建启用软件回采 AEC；回灌实测通过后才声明 aecMode=device。 */
        audio_service_.EnableDeviceAec(true);
#endif
#endif
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            /* Opus 支持选择原生解码输出率；AudioService 会直接解码到硬件采样率。 */
            ESP_LOGI(TAG, "Opus stream sample rate %d will decode at device output rate %d",
                     protocol_->server_sample_rate(), codec->output_sample_rate());
        }
    });

    protocol_->OnAudioChannelClosed([this, &board]() {
        board.GetDisplay()->CancelPhotoStage();
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
#if CONFIG_UR_AI_SIGNALING && CONFIG_UR_AI_SIGNALING_DEVICE_AEC
        audio_service_.EnableDeviceAec(false);
#endif
        Schedule([this]() {
            auto display = Board::GetInstance().GetDisplay();
            display->SetChatMessage("system", "");
            SetDeviceState(kDeviceStateIdle);
        });
    });

    protocol_->OnIncomingJson([this, display](const cJSON* root) {
        // Parse JSON data
        auto type = cJSON_GetObjectItem(root, "type");
        if (!cJSON_IsString(type)) {
            ESP_LOGW(TAG, "Incoming JSON message has no type");
            return;
        }
        if (strcmp(type->valuestring, "notify") == 0) {
            auto audio_url = cJSON_GetObjectItem(root, "audio_url");
            if (!cJSON_IsString(audio_url) || audio_url->valuestring[0] == '\0') {
                ESP_LOGW(TAG, "Notify message requires audio_url");
                return;
            }

            std::vector<NotifySubtitle> subtitles;
            auto subtitles_json = cJSON_GetObjectItem(root, "subtitles");
            if (subtitles_json != nullptr && !cJSON_IsArray(subtitles_json)) {
                ESP_LOGW(TAG, "Notify subtitles must be an array");
                return;
            }
            if (cJSON_IsArray(subtitles_json)) {
                cJSON* item = nullptr;
                cJSON_ArrayForEach (item, subtitles_json) {
                    auto start_ms = cJSON_GetObjectItem(item, "start_ms");
                    auto text = cJSON_GetObjectItem(item, "text");
                    if (!cJSON_IsNumber(start_ms) || start_ms->valuedouble < 0 ||
                        start_ms->valuedouble > std::numeric_limits<uint32_t>::max() ||
                        !cJSON_IsString(text)) {
                        ESP_LOGW(TAG, "Ignoring invalid notify subtitle");
                        continue;
                    }
                    subtitles.push_back({.start_ms = static_cast<uint32_t>(start_ms->valuedouble),
                                         .text = text->valuestring});
                }
            }

            Schedule([this, url = std::string(audio_url->valuestring),
                      subtitles = std::move(subtitles)]() mutable {
                StartNotification(std::move(url), std::move(subtitles));
            });
        } else if (strcmp(type->valuestring, "tts") == 0) {
            auto state = cJSON_GetObjectItem(root, "state");
            if (!cJSON_IsString(state)) {
                return;
            }
            if (strcmp(state->valuestring, "start") == 0) {
                Schedule([this]() {
                    Board::GetInstance().GetDisplay()->CancelPhotoStage();
                    aborted_ = false;
#if CONFIG_UR_AI_E2E_TEST
                    if (ur_ai_e2e_running_.load()) {
                        ur_ai_e2e_speaking_observed_.store(true);
                    }
#endif
#if CONFIG_UR_AI
                    /* 联犀以首个真实 UDP 帧宣告播报。先切换解码代际，再进入 Speaking，
                     * 避免同批排队的音频帧随后被状态回调中的 ResetDecoder 清空。 */
                    audio_service_.ResetDecoder();
#endif
                    SetDeviceState(kDeviceStateSpeaking);
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                Schedule([this]() {
                    Board::GetInstance().GetDisplay()->CancelPhotoStage();
#if CONFIG_UR_AI
                    ur_emotion_release_pending_ = true;
#endif
                    if (GetDeviceState() == kDeviceStateSpeaking) {
                        if (listening_mode_ == kListeningModeManualStop) {
                            SetDeviceState(kDeviceStateIdle);
                        } else {
                            SetDeviceState(kDeviceStateListening);
                        }
                    }
#if CONFIG_UR_AI
                    ReleaseUrEmotionIfReady();
#endif
                });
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                auto text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text)) {
                    std::vector<TextGlyph> glyphs;
                    uint8_t bpp = 0;
                    if (!TextGlyphPayload::Parse(root, glyphs, bpp)) {
                        glyphs.clear();
                    }
#if !CONFIG_UR_AI
                    ESP_LOGI(TAG, "<< %s", text->valuestring);
#endif
                    Schedule([display, message = std::string(text->valuestring),
                              glyphs = std::move(glyphs), bpp]() {
                        display->AddTextGlyphs(glyphs, bpp);
                        display->SetChatMessage("assistant", message.c_str());
                    });
                }
            }
#if CONFIG_UR_AI && !CONFIG_UR_AI_SIGNALING
        } else if (strcmp(type->valuestring, "listen") == 0) {
            // 协议worker仅投递事件；提示及恢复均在主任务检查当前状态和音频轮次。
            auto state = cJSON_GetObjectItem(root, "state");
            auto token = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(state) && cJSON_IsString(token)) {
                Schedule([this, display, stage = std::string(state->valuestring),
                          audio_token = std::string(token->valuestring)]() {
                    if (GetDeviceState() != kDeviceStateListening || protocol_ == nullptr ||
                        interaction_blocked_.load()) return;
                    auto* protocol = static_cast<UrAiProtocol*>(protocol_.get());
                    if (stage == "wait" && protocol->IsWaitingForResponse(audio_token)) {
                        display->SetStatus("等待回复");
                    } else if ((stage == "restart" || stage == "speech_restart") &&
                               protocol->ResumeAfterNoResult(audio_token, listening_mode_,
                                                             stage == "speech_restart")) {
                        display->SetStatus(Lang::Strings::LISTENING);
                    }
                });
            }
#endif
        } else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                std::vector<TextGlyph> glyphs;
                uint8_t bpp = 0;
                if (!TextGlyphPayload::Parse(root, glyphs, bpp)) {
                    glyphs.clear();
                }
#if !CONFIG_UR_AI
                ESP_LOGI(TAG, ">> %s", text->valuestring);
#endif
                Schedule([display, message = std::string(text->valuestring),
                          glyphs = std::move(glyphs), bpp]() {
                    display->AddTextGlyphs(glyphs, bpp);
                    display->SetChatMessage("user", message.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emotion)) {
                Schedule([this, display, emotion_str = std::string(emotion->valuestring)]() {
#if CONFIG_UR_AI
                    /* respEmotion 可能与字幕、音频交错；先退出相机预览并唤醒屏幕，
                     * 再保持表情到 tts/stop 且播放队列真正排空。 */
                    Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
                    display->SetPreviewImage(nullptr);
                    ur_emotion_hold_ = true;
                    ur_emotion_displayed_us_ = esp_timer_get_time();
#endif
                    display->SetEmotion(emotion_str.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "mcp") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(payload)) {
                McpServer::GetInstance().ParseMessage(payload);
            }
        } else if (strcmp(type->valuestring, "system") == 0) {
            auto command = cJSON_GetObjectItem(root, "command");
            if (cJSON_IsString(command)) {
                ESP_LOGI(TAG, "System command: %s", command->valuestring);
                if (strcmp(command->valuestring, "reboot") == 0) {
                    // Do a reboot if user requests a OTA update
                    Schedule([this]() { Reboot(); });
                } else {
                    ESP_LOGW(TAG, "Unknown system command: %s", command->valuestring);
                }
            }
        } else if (strcmp(type->valuestring, "alert") == 0) {
            auto status = cJSON_GetObjectItem(root, "status");
            auto message = cJSON_GetObjectItem(root, "message");
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(status) && cJSON_IsString(message) && cJSON_IsString(emotion)) {
                Alert(status->valuestring, message->valuestring, emotion->valuestring,
                      Lang::Sounds::OGG_VIBRATION);
            } else {
                ESP_LOGW(TAG, "Alert command requires status, message and emotion");
            }
#if CONFIG_RECEIVE_CUSTOM_MESSAGE
        } else if (strcmp(type->valuestring, "custom") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            CJsonStringUniquePtr root_json(cJSON_PrintUnformatted(root));
            ESP_LOGI(TAG, "Received custom message: %s", root_json ? root_json.get() : "");
            if (cJSON_IsObject(payload)) {
                CJsonStringUniquePtr payload_json(cJSON_PrintUnformatted(payload));
                if (payload_json) {
                    Schedule(
                        [this, display, payload_str = std::string(payload_json.get())]() {
                            display->SetChatMessage("system", payload_str.c_str());
                        });
                }
            } else {
                ESP_LOGW(TAG, "Invalid custom message format: missing payload");
            }
#endif
        } else {
            ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
        }
    });

    protocol_->Start();
}

void Application::ShowActivationCode(const std::string& code, const std::string& message) {
    struct digit_sound {
        char digit;
        const std::string_view& sound;
    };
    static const std::array<digit_sound, 10> digit_sounds{
        {digit_sound{'0', Lang::Sounds::OGG_0}, digit_sound{'1', Lang::Sounds::OGG_1},
         digit_sound{'2', Lang::Sounds::OGG_2}, digit_sound{'3', Lang::Sounds::OGG_3},
         digit_sound{'4', Lang::Sounds::OGG_4}, digit_sound{'5', Lang::Sounds::OGG_5},
         digit_sound{'6', Lang::Sounds::OGG_6}, digit_sound{'7', Lang::Sounds::OGG_7},
         digit_sound{'8', Lang::Sounds::OGG_8}, digit_sound{'9', Lang::Sounds::OGG_9}}};

    // This sentence uses 9KB of SRAM, so we need to wait for it to finish
    Alert(Lang::Strings::ACTIVATION, message.c_str(), "link", Lang::Sounds::OGG_ACTIVATION);

    for (const auto& digit : code) {
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
                               [digit](const digit_sound& ds) { return ds.digit == digit; });
        if (it != digit_sounds.end()) {
            audio_service_.PlaySound(it->sound);
        }
    }
}

void Application::Alert(const char* status, const char* message, const char* emotion,
                        const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert [%s] %s: %s", emotion, status, message);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
    if (!sound.empty()) {
        audio_service_.PlaySound(sound);
    }
}

void Application::DismissAlert() {
    last_error_message_.clear();
    if (GetDeviceState() == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetEmotion("neutral");
        display->SetChatMessage("system", "");
    }
}

void Application::ToggleChatState() { xEventGroupSetBits(event_group_, MAIN_EVENT_TOGGLE_CHAT); }

void Application::StartListening() { xEventGroupSetBits(event_group_, MAIN_EVENT_START_LISTENING); }

void Application::StopListening() { xEventGroupSetBits(event_group_, MAIN_EVENT_STOP_LISTENING); }

#if CONFIG_UR_AI
std::string Application::GetUrAiUploadUrl() {
    if (protocol_ == nullptr) {
        return {};
    }
    return static_cast<UrAiProtocol*>(protocol_.get())->GetUploadUrl();
}

void Application::TakePhoto() { xEventGroupSetBits(event_group_, MAIN_EVENT_TAKE_PHOTO); }

void Application::CompleteUrPhotoInput(int code, const std::string& file_uri) {
#if CONFIG_UR_AI_E2E_TEST
    if (code != 200) ur_photo_e2e_error_.store(code);
#endif
    Schedule([this, code, file_uri]() {
        if (code == 200 && !file_uri.empty() && protocol_ != nullptr) {
            BeginUrAiTurn();
        }
        if (code != 200 || file_uri.empty() || protocol_ == nullptr ||
            !static_cast<UrAiProtocol*>(protocol_.get())->SendImageInput(file_uri)) {
            // 本地发送失败不再等待不存在的模型回复；错误提示由既有失败入口显示。
            Board::GetInstance().GetDisplay()->CancelPhotoStage();
            ShowPhotoUnavailable(code == 200 ? 503 : code);
        }
    });
}
#endif

#if CONFIG_UR_AI_E2E_TEST && !CONFIG_UR_AI_SIGNALING
bool Application::RunUrPhotoE2eTest(const std::string& expected_text) {
    if (expected_text.empty() || expected_text.size() > 64 || GetDeviceState() != kDeviceStateIdle) {
        ESP_LOGE(TAG, "[UR_PHOTO_E2E] FAIL reason=invalid_input_or_not_idle");
        return false;
    }
    bool expected = false;
    if (!ur_ai_e2e_running_.compare_exchange_strong(expected, true)) {
        ESP_LOGE(TAG, "[UR_PHOTO_E2E] FAIL reason=already_running");
        return false;
    }
    auto* protocol = static_cast<UrAiProtocol*>(protocol_.get());
    if (protocol == nullptr) {
        ur_ai_e2e_running_.store(false);
        ESP_LOGE(TAG, "[UR_PHOTO_E2E] FAIL reason=protocol_unavailable");
        return false;
    }
    ur_ai_e2e_speaking_observed_.store(false);
    ur_photo_e2e_error_.store(0);
    protocol->ResetE2eObservation();
    ESP_LOGI(TAG, "[UR_PHOTO_E2E] START source=physical_camera");
    TakePhoto();
    UrAiE2eResult result;
    // 图片描述可能超过 500 字；等待完整 TTS 的预算不能混用首帧延迟门禁。
    const int64_t response_deadline = esp_timer_get_time() + 180LL * 1000 * 1000;
    do {
        result = protocol->WaitForE2eResult(pdMS_TO_TICKS(250), expected_text);
    } while (!(result.text_done && result.first_audio && result.audio_done) &&
             ur_photo_e2e_error_.load() == 0 && esp_timer_get_time() < response_deadline);
    const int64_t deadline = esp_timer_get_time() + 20LL * 1000 * 1000;
    while (result.audio_done && (!ur_ai_e2e_speaking_observed_.load() || !audio_service_.IsPlaybackIdle()) &&
           esp_timer_get_time() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    const bool played = ur_ai_e2e_speaking_observed_.load() && audio_service_.IsPlaybackIdle();
    const bool success = !result.service_error && result.text_done && result.text_nonempty && result.expected_text_matched &&
                         result.first_audio && result.audio_done && played;
    ESP_LOGI(TAG, "[UR_PHOTO_E2E] RESULT text_done=%d text_match=%d first_audio=%d audio_done=%d played=%d",
             result.text_done, result.expected_text_matched, result.first_audio, result.audio_done, played);
    protocol->CloseAudioChannel();
    ur_ai_e2e_running_.store(false);
    if (result.service_error) {
        ESP_LOGE(TAG, "[UR_PHOTO_E2E] FAIL reason=model_service_error");
        return false;
    }
    ESP_LOGI(TAG, "[UR_PHOTO_E2E] %s reason=physical_photo_roundtrip", success ? "PASS" : "FAIL");
    return success;
}

bool Application::RunUrAiE2eTest(int turns, bool take_photo, bool control_volume, bool tank_forward) {
    if (turns < 1 || turns > 5 || (take_photo && control_volume) ||
        (tank_forward && (take_photo || control_volume)) || ((control_volume || tank_forward) && turns != 1)) {
        ESP_LOGE(TAG, "[UR_AI_E2E] FAIL reason=invalid_turns");
        return false;
    }
    bool expected = false;
    if (!ur_ai_e2e_running_.compare_exchange_strong(expected, true)) {
        ESP_LOGE(TAG, "[UR_AI_E2E] FAIL reason=already_running");
        return false;
    }
    ur_ai_e2e_speaking_observed_.store(false);

    /* CONFIG_UR_AI_E2E_TEST 依赖 CONFIG_UR_AI，此构建分支在 InitializeProtocol 中
     * 只会创建 UrAiProtocol；固件全局关闭 RTTI，因此使用编译期受保护的静态转换。 */
    auto* protocol = static_cast<UrAiProtocol*>(protocol_.get());
    auto finish = [this, protocol](bool success, const char* reason) {
        if (protocol != nullptr && protocol->IsAudioChannelOpened()) {
            protocol->CloseAudioChannel();
        }
        ur_ai_e2e_running_.store(false);
        ESP_LOGI(TAG, "[UR_AI_E2E] %s reason=%s", success ? "PASS" : "FAIL", reason);
        return success;
    };
    if (protocol == nullptr) {
        return finish(false, "protocol_unavailable");
    }
    if (GetDeviceState() != kDeviceStateIdle) {
        return finish(false, "device_not_idle");
    }

    // 控制测试不偷偷设置准备值；调用方先用已有平台下行准备55，再证明语音实际改到40。
    auto* control_codec = control_volume ? Board::GetInstance().GetAudioCodec() : nullptr;
    const int control_before = control_codec != nullptr ? control_codec->output_volume() : -1;
    if (control_volume && control_before != 55) {
        return finish(false, "control_baseline_mismatch");
    }

    // 整组测试保持麦克风/VAD 隔离；仅首轮开会话，后续等待正式播放排空路径重新监听。
    std::string observed_session;
    std::string previous_audio_token;
    for (int turn = 1; turn <= turns; ++turn) {
        ur_ai_e2e_speaking_observed_.store(false);
        UrAiE2eFixture fixture(take_photo, control_volume, tank_forward);
        if (!fixture.IsValid()) {
            return finish(false, "invalid_fixture");
        }
        protocol->ResetE2eObservation(turn == 1);
        if (turn == 1) {
            ESP_LOGI(TAG,
                     "[UR_AI_E2E] START fixture=%s frames=%u sample_rate=%u frame_ms=%u",
                     tank_forward ? "tank_forward" :
                         (control_volume ? "control_volume" : (take_photo ? "take_photo" : "query_volume")),
                     static_cast<unsigned>(fixture.frame_count()),
                     static_cast<unsigned>(fixture.sample_rate()),
                     static_cast<unsigned>(fixture.frame_duration_ms()));
        }

        if (turns > 1) ESP_LOGI(TAG, "[UR_AI_E2E] TURN index=%d total=%d", turn, turns);
        if (turn == 1) ToggleChatState();
        const int64_t listening_deadline = esp_timer_get_time() + 20LL * 1000 * 1000;
        while (GetDeviceState() != kDeviceStateListening && esp_timer_get_time() < listening_deadline) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (GetDeviceState() != kDeviceStateListening) {
            return finish(false, "listening_timeout");
        }
        if (!protocol->WaitForAudioUploadReady(pdMS_TO_TICKS(10000))) {
            return finish(false, "audio_started_timeout");
        }

        // 单纯 Listening 或事件位可能属于旧轮；必须是同一 session 下新的成功 audioStart token。
        const int64_t ready_deadline = esp_timer_get_time() + 10LL * 1000 * 1000;
        UrAiE2eTurn ready;
        do {
            ready = protocol->GetE2eTurn();
            if (ready.ready && ready.audio_token != previous_audio_token) break;
            vTaskDelay(pdMS_TO_TICKS(20));
        } while (esp_timer_get_time() < ready_deadline);
        if (!ready.ready || ready.audio_token.empty() || ready.audio_token == previous_audio_token) {
            return finish(false, "new_turn_not_ready");
        }
        if (turn > 1 && ready.session_id != observed_session) {
            return finish(false, "session_changed_between_turns");
        }
        observed_session = ready.session_id;
        previous_audio_token = ready.audio_token;
        if (turns > 1) ESP_LOGI(TAG, "[UR_AI_E2E] TURN_READY index=%d same_session=1 new_audio_token=1", turn);

        protocol->OnLocalVadState(true);
        UrAiE2eAudioFrame frame;
        uint16_t sent_frames = 0;
        while (fixture.Next(&frame)) {
            auto packet = std::make_unique<AudioStreamPacket>();
            packet->sample_rate = fixture.sample_rate();
            packet->frame_duration = fixture.frame_duration_ms();
            packet->timestamp = static_cast<uint32_t>(esp_timer_get_time() / 1000);
            packet->payload.assign(frame.data, frame.data + frame.size);
            if (!protocol->SendAudio(std::move(packet))) {
                return finish(false, "udp_send_failed");
            }
            ++sent_frames;
            vTaskDelay(pdMS_TO_TICKS(fixture.frame_duration_ms()));
        }
        if (sent_frames != fixture.frame_count()) {
            return finish(false, "fixture_truncated");
        }
        protocol->OnLocalVadState(false);
        ESP_LOGI(TAG, "[UR_AI_E2E] INPUT_DONE frames=%u", static_cast<unsigned>(sent_frames));

        /* 拍照回复可超过一分钟；总播放预算独立于 runner 的首帧/首字门禁，仍保持有界。 */
        const UrAiE2eResult result = protocol->WaitForE2eResult(
            pdMS_TO_TICKS(take_photo ? 120000 : 60000), "",
            tank_forward ? "前" : (take_photo ? "拍照" : "音量"));
        if (result.service_error) {
            return finish(false, "model_service_error");
        }
        const bool response_complete = result.stt_done && result.stt_nonempty &&
                                       result.stt_matches_fixture && result.text_done &&
                                       result.text_nonempty && result.first_audio && result.audio_done;
        ESP_LOGI(TAG,
                 "[UR_AI_E2E] RESULT stt_done=%d stt_nonempty=%d stt_match=%d text_done=%d "
                 "text_nonempty=%d first_audio=%d audio_done=%d",
                 result.stt_done, result.stt_nonempty, result.stt_matches_fixture, result.text_done,
                 result.text_nonempty, result.first_audio, result.audio_done);
        if (!response_complete) {
            return finish(false, "response_incomplete");
        }

        const int64_t playback_deadline = esp_timer_get_time() + 20LL * 1000 * 1000;
        // 多轮还需等待旧轮正常退出播报，不能在 audioDone 事件位刚置位时跳过 UI/播放收尾。
        while ((!ur_ai_e2e_speaking_observed_.load() || !audio_service_.IsPlaybackIdle() ||
                (turns > 1 && GetDeviceState() != kDeviceStateListening)) &&
               esp_timer_get_time() < playback_deadline) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (!ur_ai_e2e_speaking_observed_.load() || !audio_service_.IsPlaybackIdle() ||
            (turns > 1 && GetDeviceState() != kDeviceStateListening)) {
            return finish(false, "playback_timeout");
        }
        if (control_volume) {
            const int control_after = control_codec->output_volume();  // 读取硬件实际值，不拿模型口头确认代替。
            ESP_LOGI(TAG, "[UR_VOICE_CONTROL_E2E] before=%d target=40 after=%d changed=%d",
                     control_before, control_after, control_after == 40 && control_after != control_before);
            if (control_after != 40) return finish(false, "control_not_applied");
        }
        if (turns > 1) ESP_LOGI(TAG, "[UR_AI_E2E] TURN_PASS index=%d", turn);
    }
    return finish(true, "voice_roundtrip_complete");
}
#endif

#if CONFIG_UR_AI
void Application::ShowPhotoUnavailable(int code) {
    const char* message = "拍照失败，请稍后重试";
    if (code == 501) {
        // 当前信令构建尚未迁移图片输入，明确提示能力限制以免用户反复重试。
        message = "当前模式暂不支持拍照";
    } else if (code == 409) {
        message = "正在拍照，请稍候";
    } else if (code == 423) {
        message = "升级中，暂不能拍照";
    } else if (code == 503) {
        message = "网络不可用，暂不能拍照";
    }
    auto* display = Board::GetInstance().GetDisplay();
    if (display != nullptr) {
        display->ShowNotification(message, 3000);
    }
}

void Application::HandleTakePhotoEvent() {
    if (protocol_ == nullptr) {
        ShowPhotoUnavailable(503);
        return;
    }
#if CONFIG_UR_AI_SIGNALING
    /* 拍照输入尚未迁移到统一信令协议；信令构建下双击拍照优雅拒绝，
     * 防止后续 GetUploadUrl/SendImageInput 对 SignalingProtocol 的错误强转。 */
    ShowPhotoUnavailable(501);
    return;
#endif
    ur_ota_status_t ota = {0};
    ur_bridge_get_ota_status(&ota);
    if (ota.state == 1 || ota.state == 2 || GetDeviceState() == kDeviceStateUpgrading) {
        ShowPhotoUnavailable(423);
        return;
    }
    /* 相机已被云端 takePhoto 占用时只提示，不打断当前播报或监听。 */
    if (ur_bridge_photo_busy()) {
        ShowPhotoUnavailable(409);
        return;
    }

    auto state = GetDeviceState();
    if (state == kDeviceStateNotifying) {
        StopNotification();
        state = kDeviceStateIdle;
    }
    if (state == kDeviceStateSpeaking || state == kDeviceStateListening) {
        /* 双击优先于当前一轮语音：取消服务端回复并停掉本地编解码，旧 respId
         * 会在 SendImageInput 重置后被协议关联规则丢弃。 */
        AbortSpeaking(kAbortReasonNone);
        audio_service_.EnableVoiceProcessing(false);
        audio_service_.ResetDecoder();
        SetDeviceState(kDeviceStateIdle);
        state = kDeviceStateIdle;
    }
    if (state != kDeviceStateIdle) {
        ShowPhotoUnavailable(409);
        return;
    }

    listening_mode_ = kListeningModeAutoStop;
    if (!protocol_->IsAudioChannelOpened()) {
        SetDeviceState(kDeviceStateConnecting);
        Schedule([this]() { ContinueOpenPhotoChannel(); });
        return;
    }
    const int result = ur_bridge_start_photo_input();
    if (result != 0) {
        ShowPhotoUnavailable(result);
    }
}

void Application::ContinueOpenPhotoChannel() {
    if (GetDeviceState() != kDeviceStateConnecting || protocol_ == nullptr) {
        return;
    }
    Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    if (!protocol_->IsAudioChannelOpened() && !protocol_->OpenAudioChannel()) {
        SetDeviceState(kDeviceStateIdle);
        ShowPhotoUnavailable(503);
        return;
    }
    SetDeviceState(kDeviceStateIdle);
    const int result = ur_bridge_start_photo_input();
    if (result != 0) {
        ShowPhotoUnavailable(result);
    }
}

void Application::BeginUrAiTurn() {
    ur_emotion_hold_ = false;
    ur_emotion_release_pending_ = false;
    ur_emotion_displayed_us_ = 0;
    auto* display = Board::GetInstance().GetDisplay();
    if (display != nullptr) {
        display->SetEmotion("neutral");
    }
}

void Application::ReleaseUrEmotionIfReady() {
    constexpr int64_t kMinimumEmotionVisibleUs = 800 * 1000;
    if (!ur_emotion_hold_ || !ur_emotion_release_pending_ ||
        !audio_service_.IsPlaybackIdle() ||
        esp_timer_get_time() - ur_emotion_displayed_us_ < kMinimumEmotionVisibleUs) {
        return;
    }
    BeginUrAiTurn();
    if (pending_listening_start_ && GetDeviceState() == kDeviceStateListening) {
        pending_listening_start_ = false;
        ESP_LOGI(TAG, "pending listening start fulfilled (emotion released)");
        StartListeningAudio();
    }
}
#endif

// EndConversation 不销毁协议实例；菜单/设置入口不应触发重新注册或丢失 MQTT。
void Application::EndConversation() {
    Board::GetInstance().GetDisplay()->CancelPhotoStage();
    Schedule([this]() {
        const auto state = GetDeviceState();
        if (state != kDeviceStateConnecting && state != kDeviceStateListening &&
            state != kDeviceStateSpeaking && state != kDeviceStateIdle) return;
        audio_service_.EnableVoiceProcessing(false);
        audio_service_.ResetDecoder();
        if (protocol_) protocol_->RequestCloseAudioChannel();
        SetDeviceState(kDeviceStateIdle);
    });
}

void Application::HandleToggleChatEvent() {
    if (interaction_blocked_.load()) return;
#if CONFIG_UR_AI_SIGNALING_WEBRTC
    // 通话采集属于人际腿，迟到的 AI 按键不得改变应用状态或媒体管线。
    if (human_call_audio_owned_) return;
#endif
    auto state = GetDeviceState();

    if (state == kDeviceStateNotifying) {
        StopNotification();
        state = kDeviceStateIdle;
    }

    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    } else if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (state == kDeviceStateIdle) {
        ListeningMode mode = GetDefaultListeningMode();
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this, mode]() { ContinueOpenAudioChannel(mode); });
            return;
        }
        SetListeningMode(mode);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
#if CONFIG_UR_AI
        /* 联犀取消回复不保证再发 respAudioDone，不能等待旧轮结束才重新监听。
         * 主动清除尾音和表情保持，再经正常监听入口发送新 audioStart。 */
        audio_service_.EnableVoiceProcessing(false);
        audio_service_.ResetDecoder();
        BeginUrAiTurn();
        SetListeningMode(GetDefaultListeningMode());
#endif
    } else if (state == kDeviceStateListening) {
        protocol_->CloseAudioChannel();
    }
}

// ContinueOpenAudioChannel 完成异步建连；mode为建连前请求，自动模式须以建连后的协商事实刷新。
void Application::ContinueOpenAudioChannel(ListeningMode mode) {
    // Check state again in case it was changed during scheduling
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }

    // Switch to performance mode before connecting to reduce latency
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

    if (!protocol_->IsAudioChannelOpened()) {
        if (!protocol_->OpenAudioChannel()) {
            // Return to idle so the device is not stuck in the connecting
            // state (not every failure path reports a network error)
            SetDeviceState(kDeviceStateIdle);
            return;
        }
    }

#if CONFIG_UR_AI_SIGNALING
    // 首次建连前尚无全双工事实；保留显式按住说话，其余请求采用本次协商模式。
    if (mode != kListeningModeManualStop) {
        mode = GetDefaultListeningMode();
    }
#endif
    SetListeningMode(mode);
}

void Application::HandleStartListeningEvent() {
    if (interaction_blocked_.load()) return;
#if CONFIG_UR_AI_SIGNALING_WEBRTC
    if (human_call_audio_owned_) return;
#endif
    auto state = GetDeviceState();

    if (state == kDeviceStateNotifying) {
        StopNotification();
        state = kDeviceStateIdle;
    }

    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (state == kDeviceStateIdle) {
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this]() { ContinueOpenAudioChannel(kListeningModeManualStop); });
            return;
        }
        SetListeningMode(kListeningModeManualStop);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
        SetListeningMode(kListeningModeManualStop);
    }
}

void Application::HandleStopListeningEvent() {
    auto state = GetDeviceState();

    if (state == kDeviceStateNotifying) {
        StopNotification();
    } else if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    } else if (state == kDeviceStateListening) {
        if (protocol_) {
            protocol_->SendStopListening();
        }
        SetDeviceState(kDeviceStateIdle);
    }
}

// HandleWakeWordDetectedEvent 消费已暂停检测器的唤醒事件；页面屏蔽时恢复检测而不建会话。
void Application::HandleWakeWordDetectedEvent() {
#if CONFIG_UR_AI_SIGNALING_WEBRTC
    // 迟到的唤醒结果不能在通话中重新打开 AI 或唤醒检测。
    if (human_call_audio_owned_) return;
#endif
#if CONFIG_UR_SPARKBOT_VOICE_MODEL_TEST
    // 来电识别期间唤醒词不能启动 AI 会话抢占内存；恢复检测以保留空闲能力。
    if (UrSignalingCall::Instance().VoiceAnswerGuardActive()) {
        audio_service_.EnableWakeWordDetection(true);
        return;
    }
#endif
    if (interaction_blocked_.load()) {
        // AFE 与 AudioService 在上报成功时均已清除检测位；直接丢弃会让回主页后永久失去唤醒。
        audio_service_.EnableWakeWordDetection(true);
        ESP_LOGI(TAG, "Wake word ignored while interaction blocked; detection rearmed");
        return;
    }
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();
    auto wake_word = audio_service_.GetLastWakeWord();
    ESP_LOGI(TAG, "Wake word detected: %s (state: %d)", wake_word.c_str(), (int)state);

    if (state == kDeviceStateIdle) {
        BeginWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateNotifying) {
        StopNotification();
        BeginWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateSpeaking || state == kDeviceStateListening) {
        AbortSpeaking(kAbortReasonWakeWordDetected);
        // Clear send queue to avoid sending residues to server
        while (audio_service_.PopPacketFromSendQueue())
            ;

        if (state == kDeviceStateListening) {
            protocol_->SendStartListening(GetDefaultListeningMode());
            audio_service_.ResetDecoder();
            audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
            // Re-enable wake word detection as it was stopped by the detection itself
            audio_service_.EnableWakeWordDetection(true);
        } else {
            // Play popup sound and start listening again
            play_popup_on_listening_ = true;
            SetListeningMode(GetDefaultListeningMode());
        }
    } else if (state == kDeviceStateActivating) {
        // Restart the activation check if the wake word is detected during activation
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::BeginWakeWordInvoke(const std::string& wake_word) {
    if (interaction_blocked_.load()) return;
    // Must run in the main task with the device in idle state
    audio_service_.EncodeWakeWord();

    // Always pass through the connecting state, even if the audio channel is
    // already opened. ContinueWakeWordInvoke() rejects any other state, so
    // skipping this transition would silently drop the wake word invocation.
    if (!SetDeviceState(kDeviceStateConnecting)) {
        // Wake word detection was stopped by the detection itself; restore it
        // so the device does not become unresponsive to wake words.
        audio_service_.EnableWakeWordDetection(true);
        return;
    }

    if (!protocol_->IsAudioChannelOpened()) {
        // Schedule to let the state change be processed first (UI update),
        // then continue with OpenAudioChannel which may block for ~1 second
        Schedule([this, wake_word]() { ContinueWakeWordInvoke(wake_word); });
        return;
    }
    // Channel already opened, continue directly
    ContinueWakeWordInvoke(wake_word);
}

void Application::ContinueWakeWordInvoke(const std::string& wake_word) {
    // Check state again in case it was changed during scheduling
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }

    // Switch to performance mode before connecting to reduce latency
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

    if (!protocol_->IsAudioChannelOpened()) {
        if (!protocol_->OpenAudioChannel()) {
            // Return to idle so the device is not stuck in the connecting
            // state (not every failure path reports a network error), and
            // wake word detection is re-enabled by the idle state handler.
            SetDeviceState(kDeviceStateIdle);
            return;
        }
    }

    ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_SEND_WAKE_WORD_DATA
    // Encode and send the wake word data to the server
    while (auto packet = audio_service_.PopWakeWordPacket()) {
        protocol_->SendAudio(std::move(packet));
    }
    // Set the chat state to wake word detected
    protocol_->SendWakeWordDetected(wake_word);
    SetListeningMode(GetDefaultListeningMode());
#else
    // Set flag to play popup sound after state changes to listening
    // (PlaySound here would be cleared by ResetDecoder in EnableVoiceProcessing)
    play_popup_on_listening_ = true;
    SetListeningMode(GetDefaultListeningMode());
#endif
}

void Application::HandleStateChangedEvent() {
    DeviceState new_state = state_machine_.GetState();
    clock_ticks_ = 0;
    // Any state change invalidates a pending deferred listening start;
    // the Listening case below re-arms it when needed.
    if (pending_listening_start_) {
        ESP_LOGI(TAG, "pending listening start cleared by state change -> %d",
                 static_cast<int>(new_state));
    }
    pending_listening_start_ = false;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();

    switch (new_state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
            // Keep a just-raised network error visible. SetDeviceState(idle)
            // queues STATE_CHANGED after Alert(), and the idle handler would
            // otherwise wipe the status, emotion, and chat message.
            if (last_error_message_.empty()) {
                display->SetStatus(Lang::Strings::STANDBY);
                display->ClearChatMessages();  // Clear messages first
#if CONFIG_UR_AI
                if (!ur_emotion_hold_) {
#endif
                    display->SetEmotion(
                        "neutral");  // Then set emotion (wechat mode checks child count)
#if CONFIG_UR_AI
                }
#endif
            }
#if CONFIG_UR_AI_SIGNALING_WEBRTC
            // AI 让位或建连失败的待机事件可能迟到；不得关闭人际通话采集。
            if (!human_call_audio_owned_)
#endif
            {
                audio_service_.EnableVoiceProcessing(false);
                audio_service_.EnableWakeWordDetection(true);
            }
            break;
        case kDeviceStateConnecting:
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetEmotion("neutral");
            display->SetChatMessage("system", "");
            break;
        case kDeviceStateListening:
            display->SetStatus(Lang::Strings::LISTENING);
#if CONFIG_UR_AI
            if (!ur_emotion_hold_) {
#endif
                display->SetEmotion("neutral");
#if CONFIG_UR_AI
            }
#endif

            // Make sure the audio processor is running
            if (play_popup_on_listening_ || !audio_service_.IsAudioProcessorRunning()) {
#if CONFIG_UR_AI
                if (ur_emotion_hold_) {
                    pending_listening_start_ = true;
                    pending_listening_since_us_ = esp_timer_get_time();
                    ESP_LOGI(TAG, "pending listening start armed (emotion hold)");
                    break;
                }
#endif
                // For auto mode, wait for the playback queue to drain before enabling
                // voice processing. This prevents audio truncation when STOP arrives
                // late due to network jitter. Instead of blocking the main loop here,
                // defer the start until MAIN_EVENT_PLAYBACK_DRAINED arrives.
                if (listening_mode_ == kListeningModeAutoStop && !audio_service_.IsPlaybackIdle()) {
                    pending_listening_start_ = true;
                    pending_listening_since_us_ = esp_timer_get_time();
                    ESP_LOGI(TAG, "pending listening start armed (playback not drained)");
                } else {
                    StartListeningAudio();
                }
            } else {
                ConfigureWakeWordForListening();
            }
            break;
        case kDeviceStateSpeaking:
            display->SetStatus(Lang::Strings::SPEAKING);

            if (listening_mode_ != kListeningModeRealtime) {
                audio_service_.EnableVoiceProcessing(false);
                // Only AFE wake word can be detected in speaking mode
                audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
            }
#if !CONFIG_UR_AI
            /* 公共小智在状态切换时尚未交付首帧，可继续在这里重置解码器。联犀首帧
             * 与 tts/start 同时到达，已在 tts/start 任务中先完成重置。 */
            audio_service_.ResetDecoder();
#endif
            break;
        case kDeviceStateNotifying:
            display->SetStatus(Lang::Strings::SPEAKING);
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
            break;
        case kDeviceStateWifiConfiguring:
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(false);
            break;
        default:
            // Do nothing
            break;
    }
}

void Application::EnsureListeningAudio() {
    /* 幂等自愈：由信令协议的自愈检查（或其它任务）调用；统一在主任务执行应用自己的
     * 开麦路径，保证媒体采集（audioStart）与语音处理（AFE 输出）一起恢复——只补
     * audioStart 时 AFE 未开，设备仍无上行（AfeAudioEngine 仅在语音处理开启时回调
     * 输出帧）。状态复核在 lambda 内做，非监听态直接放弃，重复触发不会重复上行。 */
    Schedule([this]() {
        if (GetDeviceState() != kDeviceStateListening) {
            return;
        }
        pending_listening_start_ = false;
        ESP_LOGW(TAG, "listening audio resume by request (capture self-heal)");
        StartListeningAudio();
    });
}

void Application::StartListeningAudio() {
    // Runs in the main loop, either directly from HandleStateChangedEvent or
    // deferred via MAIN_EVENT_PLAYBACK_DRAINED once the playback queue drains.
    if (GetDeviceState() != kDeviceStateListening) {
        return;
    }
    ESP_LOGI(TAG, "start listening audio mode=%d", static_cast<int>(listening_mode_));

#if CONFIG_UR_AI
    BeginUrAiTurn();
#endif
    // Send the start listening command
    protocol_->SendStartListening(listening_mode_);
    audio_service_.EnableVoiceProcessing(true);

    ConfigureWakeWordForListening();

    // Play popup sound after ResetDecoder (in EnableVoiceProcessing) has been called
    if (play_popup_on_listening_) {
        play_popup_on_listening_ = false;
        audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
    }
}

void Application::ConfigureWakeWordForListening() {
#ifdef CONFIG_WAKE_WORD_DETECTION_IN_LISTENING
    // Enable wake word detection in listening mode (configured via Kconfig)
    audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
#else
    // Disable wake word detection in listening mode
    audio_service_.EnableWakeWordDetection(false);
#endif
}

void Application::StartNotification(std::string audio_url, std::vector<NotifySubtitle> subtitles) {
    if (GetDeviceState() != kDeviceStateIdle || notify_player_.IsBusy()) {
        ESP_LOGW(TAG, "Ignoring notify message while device is busy");
        return;
    }

    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    audio_service_.EnableVoiceProcessing(false);
    audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
    audio_service_.ReleaseWakeWordResources();
    while (audio_service_.PopPacketFromSendQueue()) {
        // Discard microphone audio left over from a previous conversation.
    }

    if (!SetDeviceState(kDeviceStateNotifying)) {
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        return;
    }

    audio_service_.ResetDecoder();
    uint32_t playback_id = ++notification_playback_id_;
    if (playback_id == 0) {
        playback_id = ++notification_playback_id_;
    }
    audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);

    bool started = notify_player_.Start(
        std::move(audio_url), std::move(subtitles), playback_id,
        [this](uint32_t id, const std::string& text) {
            Schedule([this, id, text]() {
                if (GetDeviceState() == kDeviceStateNotifying && notification_playback_id_ == id) {
                    Board::GetInstance().GetDisplay()->SetChatMessage("assistant", text.c_str());
                }
            });
        },
        [this](uint32_t id, bool success) {
            Schedule([this, id, success]() { HandleNotificationFinished(id, success); });
        });

    if (!started) {
        ESP_LOGE(TAG, "Failed to start notification playback");
        StopNotification();
    }
}

void Application::StopNotification() {
    notify_player_.Stop();
    audio_service_.ResetDecoder();
    auto& board = Board::GetInstance();
    board.GetDisplay()->SetChatMessage("assistant", "");
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
    if (GetDeviceState() == kDeviceStateNotifying) {
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::HandleNotificationFinished(uint32_t playback_id, bool success) {
    if (GetDeviceState() != kDeviceStateNotifying || notification_playback_id_ != playback_id) {
        return;
    }
    ESP_LOGI(TAG, "Notification playback %lu %s", static_cast<unsigned long>(playback_id),
             success ? "completed" : "failed");
    StopNotification();
}

void Application::Schedule(std::function<void()>&& callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

void Application::AbortSpeaking(AbortReason reason) {
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;
    if (protocol_) {
        protocol_->SendAbortSpeaking(reason);
    }
}

void Application::SetListeningMode(ListeningMode mode) {
    ESP_LOGI(TAG, "set listening mode=%d (from state=%d)", static_cast<int>(mode),
             static_cast<int>(GetDeviceState()));
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
}

ListeningMode Application::GetDefaultListeningMode() const {
#if CONFIG_UR_AI_SIGNALING
    /* 全双工会话一次 audioStart 持续多轮；模式由平台协商结果决定。 */
    if (protocol_ && protocol_->IsFullDuplexSession()) {
        return kListeningModeRealtime;
    }
#endif
    return aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime;
}

void Application::Reboot() {
    ESP_LOGI(TAG, "Rebooting...");
    if (GetDeviceState() == kDeviceStateNotifying) {
        StopNotification();
    }
    // Disconnect the audio channel
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
    protocol_.reset();
    audio_service_.Stop();

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

bool Application::PrepareForPlatformOta() {
    xEventGroupClearBits(event_group_, MAIN_EVENT_PLATFORM_OTA_PREPARED);
    Schedule([this]() {
        if (protocol_ != nullptr) {
            protocol_->PrepareForOta();
        }
        /* Watcher 启用语音后内部 RAM 余量不足以再创建 10 KiB OTA worker。
         * Stop 会让输入、输出和 Opus 任务退出，释放其内部 RAM 栈；只关闭
         * voice processing 位不会释放任务栈，xTaskCreate 会直接失败。 */
        audio_service_.Stop();
        SetDeviceState(kDeviceStateIdle);
        xEventGroupSetBits(event_group_, MAIN_EVENT_PLATFORM_OTA_PREPARED);
    });
    EventBits_t bits = xEventGroupWaitBits(event_group_, MAIN_EVENT_PLATFORM_OTA_PREPARED,
                                           pdTRUE, pdFALSE, pdMS_TO_TICKS(2000));
    return (bits & MAIN_EVENT_PLATFORM_OTA_PREPARED) != 0;
}

void Application::RecoverFromPlatformOta() {
    Schedule([this]() {
        /* OTA 失败不能让设备永久失去唤醒和对话能力。Stop 后只在此处重建一次
         * 三个音频任务，再由待机状态恢复本地唤醒词检测。 */
        audio_service_.Start();
        audio_service_.EnableWakeWordDetection(true);
        SetDeviceState(kDeviceStateIdle);
    });
}

bool Application::UpgradeFirmware(const std::string& url, const std::string& version) {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();

    std::string upgrade_url = url;
    std::string version_info = version.empty() ? "(Manual upgrade)" : version;

    if (GetDeviceState() == kDeviceStateNotifying) {
        StopNotification();
    }

    // Close audio channel if it's open
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        ESP_LOGI(TAG, "Closing audio channel before firmware upgrade");
        protocol_->CloseAudioChannel();
    }
    ESP_LOGI(TAG, "Starting firmware upgrade");

    Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "download",
          Lang::Sounds::OGG_UPGRADE);
    vTaskDelay(pdMS_TO_TICKS(3000));

    SetDeviceState(kDeviceStateUpgrading);

    std::string message = std::string(Lang::Strings::NEW_VERSION) + version_info;
    display->SetChatMessage("system", message.c_str());

    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    audio_service_.Stop();
    vTaskDelay(pdMS_TO_TICKS(1000));

    bool upgrade_success = Ota::Upgrade(upgrade_url, [this, display](int progress, size_t speed) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
        Schedule([display, message = std::string(buffer)]() {
            display->SetChatMessage("system", message.c_str());
        });
    });

    if (!upgrade_success) {
        // Upgrade failed, restart audio service and continue running
        ESP_LOGE(TAG,
                 "Firmware upgrade failed, restarting audio service and continuing operation...");
        audio_service_.Start();                              // Restart audio service
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);  // Restore power save level
        Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "cancel",
              Lang::Sounds::OGG_EXCLAMATION);
        vTaskDelay(pdMS_TO_TICKS(3000));
        return false;
    } else {
        // Upgrade success, reboot immediately
        ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
        display->SetChatMessage("system", "Upgrade successful, rebooting...");
        vTaskDelay(pdMS_TO_TICKS(1000));  // Brief pause to show message
        Reboot();
        return true;
    }
}

void Application::WakeWordInvoke(const std::string& wake_word) {
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();

    if (state == kDeviceStateIdle) {
        // May be called from outside the main task (e.g. board button
        // callbacks), so schedule the invocation instead of running it here
        Schedule([this, wake_word]() {
            if (GetDeviceState() == kDeviceStateIdle) {
                BeginWakeWordInvoke(wake_word);
            }
        });
    } else if (state == kDeviceStateNotifying) {
        Schedule([this, wake_word]() {
            if (GetDeviceState() == kDeviceStateNotifying) {
                StopNotification();
                BeginWakeWordInvoke(wake_word);
            }
        });
    } else if (state == kDeviceStateSpeaking) {
        Schedule([this]() { AbortSpeaking(kAbortReasonNone); });
    } else if (state == kDeviceStateListening) {
        Schedule([this]() {
            if (protocol_) {
                protocol_->CloseAudioChannel();
            }
        });
    }
}

bool Application::CanEnterSleepMode() {
#if CONFIG_UR_AI
    // 云端拍照不一定打开语音通道，独立相机任务期间仍需保持屏幕和网络活动。
    if (ur_bridge_photo_busy()) {
        return false;
    }
#endif
    if (GetDeviceState() != kDeviceStateIdle) {
        return false;
    }

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        return false;
    }

    if (!audio_service_.IsIdle()) {
        return false;
    }

    // Now it is safe to enter sleep mode
    return true;
}

void Application::RegisterMcpBroadcastCallback(std::function<void(const std::string&)> callback) {
    mcp_broadcast_callback_ = std::move(callback);
}

void Application::SendMcpMessage(const std::string& payload) {
    // Always schedule to run in main task for thread safety
    Schedule([this, payload]() {
        if (protocol_) {
            protocol_->SendMcpMessage(payload);
        }
        if (mcp_broadcast_callback_) {
            mcp_broadcast_callback_(payload);
        }
    });
}

void Application::SetAecMode(AecMode mode) {
    aec_mode_ = mode;
    Schedule([this]() {
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        switch (aec_mode_) {
            case kAecOff:
                audio_service_.EnableDeviceAec(false);
                display->ShowNotification(Lang::Strings::RTC_MODE_OFF);
                break;
            case kAecOnServerSide:
                audio_service_.EnableDeviceAec(false);
                display->ShowNotification(Lang::Strings::RTC_MODE_ON);
                break;
            case kAecOnDeviceSide:
                audio_service_.EnableDeviceAec(true);
                display->ShowNotification(Lang::Strings::RTC_MODE_ON);
                break;
        }

        // If the AEC mode is changed, close the audio channel
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
    });
}

void Application::PlaySound(const std::string_view& sound) { audio_service_.PlaySound(sound); }

void Application::ResetProtocol() {
    Schedule([this]() {
        if (GetDeviceState() == kDeviceStateNotifying) {
            StopNotification();
        }
        // Close audio channel if opened
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
        // Reset protocol
        protocol_.reset();
    });
}
