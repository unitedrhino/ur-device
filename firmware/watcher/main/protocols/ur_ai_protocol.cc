/* 联犀设备 AI 协议实现；时序以 backend/things/tools/devicesim 为准。 */
#include "ur_ai_protocol.h"
#if CONFIG_UR_UDP_TX_TRACE
#include "ur_udp_netif_trace.h"
#include "ur_udp_wifi_trace.h"
#endif

#include "application.h"
#include "assets/lang_config.h"
#include "board.h"
#include "unitedrhino/ur_bridge.h"
#include "unitedrhino/ur_device.h"

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <cJSON.h>

#include <array>
#include <cstring>
#include <utility>

namespace {
constexpr char kTag[] = "UrAiProtocol";
constexpr TickType_t kSessionTimeout = pdMS_TO_TICKS(10000);
constexpr TickType_t kCloseWait = pdMS_TO_TICKS(5000);
constexpr TickType_t kMqttReadyWait = pdMS_TO_TICKS(10000);
constexpr int64_t kAudioStartTimeoutUs = 10LL * 1000 * 1000;
constexpr int64_t kResponseTimeoutUs = 60LL * 1000 * 1000;
/* kUrAiSampleRate 是 Watcher AFE、Opus 编码和联犀会话共同使用的语音采样率。 */
constexpr int kUrAiSampleRate = 16000;

/* devicesim 以 16 kHz/60 ms Opus 编码得到的两帧静音，用于建立 UDP 路由。 */
constexpr std::array<uint8_t, 20> kSilenceFrameOne = {0x58, 0x02, 0xf9, 0x30, 0x4d, 0xbb, 0x0d,
                                                      0xe5, 0xe3, 0x92, 0x09, 0x89, 0x38, 0xeb,
                                                      0xca, 0xe1, 0xb1, 0xd1, 0xdd, 0x85};
constexpr std::array<uint8_t, 21> kSilenceFrameTwo = {0x58, 0x01, 0xf2, 0x5e, 0x72, 0x36, 0x92,
                                                      0x1f, 0x05, 0xfa, 0x47, 0x8d, 0x69, 0x2c,
                                                      0x8f, 0x20, 0x0a, 0xfb, 0xfb, 0x2b, 0x20};

/* JsonString 返回对象中的字符串字段。 */
const char* JsonString(cJSON* object, const char* name) {
    cJSON* item = cJSON_IsObject(object) ? cJSON_GetObjectItemCaseSensitive(object, name) : nullptr;
    return cJSON_IsString(item) ? item->valuestring : nullptr;
}

/* ElapsedMs 返回从指定单调时钟时间点到现在的毫秒数。 */
long ElapsedMs(int64_t since_us) {
    return since_us > 0 ? static_cast<long>((esp_timer_get_time() - since_us) / 1000) : -1;
}
}  // namespace

UrAiProtocol::UrAiProtocol() {
    event_group_ = xEventGroupCreate();
    terminal_queue_ = xQueueCreate(24, sizeof(IncomingMessage*));
    delta_queue_ = xQueueCreate(8, sizeof(IncomingMessage*));
    audio_channel_ = std::make_unique<UdpAudioChannel>();
    server_sample_rate_ = kUrAiSampleRate;
    server_frame_duration_ = 60;
}

UrAiProtocol::~UrAiProtocol() {
    *alive_ = false;
    ur_device_ai_set_callbacks(nullptr, nullptr, nullptr);
    if (audio_channel_ != nullptr) {
        audio_channel_->Close();
    }
    if (worker_task_ != nullptr) {
        vTaskDelete(worker_task_);
        worker_task_ = nullptr;
    }
    if (worker_task_stack_ != nullptr) {
        heap_caps_free(worker_task_stack_);
        worker_task_stack_ = nullptr;
    }
    if (worker_task_buffer_ != nullptr) {
        heap_caps_free(worker_task_buffer_);
        worker_task_buffer_ = nullptr;
    }
    IncomingMessage* message = nullptr;
    if (terminal_queue_ != nullptr) {
        while (xQueueReceive(terminal_queue_, &message, 0) == pdTRUE) {
            delete message;
        }
        vQueueDelete(terminal_queue_);
        terminal_queue_ = nullptr;
    }
    if (delta_queue_ != nullptr) {
        while (xQueueReceive(delta_queue_, &message, 0) == pdTRUE) {
            delete message;
        }
        vQueueDelete(delta_queue_);
        delta_queue_ = nullptr;
    }
    std::lock_guard<std::mutex> overflow_lock(terminal_overflow_mutex_);
    terminal_overflow_.clear();
    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
        event_group_ = nullptr;
    }
}

bool UrAiProtocol::Start() {
    if (event_group_ == nullptr || terminal_queue_ == nullptr || delta_queue_ == nullptr) {
        SetError(Lang::Strings::SERVER_ERROR);
        return false;
    }
    worker_task_stack_ = static_cast<StackType_t*>(
        heap_caps_malloc(kWorkerTaskStackSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    worker_task_buffer_ =
        static_cast<StaticTask_t*>(heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL));
    if (worker_task_stack_ == nullptr || worker_task_buffer_ == nullptr) {
        ESP_LOGE(kTag, "AI worker memory allocation failed");
        if (worker_task_stack_ != nullptr) {
            heap_caps_free(worker_task_stack_);
            worker_task_stack_ = nullptr;
        }
        if (worker_task_buffer_ != nullptr) {
            heap_caps_free(worker_task_buffer_);
            worker_task_buffer_ = nullptr;
        }
        SetError(Lang::Strings::SERVER_ERROR);
        return false;
    }
    worker_task_ = xTaskCreateStatic(WorkerEntry, "ur_ai_rx", kWorkerTaskStackSize, this, 4,
                                     worker_task_stack_, worker_task_buffer_);
    if (worker_task_ == nullptr) {
        ESP_LOGE(kTag, "AI worker task creation failed");
        SetError(Lang::Strings::SERVER_ERROR);
        return false;
    }
    ur_device_ai_set_callbacks(
        [](const char* payload, size_t length, void* context) {
            static_cast<UrAiProtocol*>(context)->EnqueueIncoming(payload, length);
        },
        [](bool connected, void* context) {
            auto* protocol = static_cast<UrAiProtocol*>(context);
            /* MQTT 回调先更新事件位，允许正在主任务中等待的冷启动唤醒继续；
             * UI/状态回调仍排回主任务执行，避免跨任务访问界面。 */
            if (connected) {
                xEventGroupSetBits(protocol->event_group_, kMqttConnectedEvent);
            } else {
                xEventGroupClearBits(protocol->event_group_, kMqttConnectedEvent);
            }
            auto alive = protocol->alive_;
            Application::GetInstance().Schedule([protocol, alive, connected]() {
                if (*alive) {
                    protocol->HandleConnectionChanged(connected);
                }
            });
        },
        this);
    HandleConnectionChanged(ur_device_is_connected());
    return true;
}

bool UrAiProtocol::OpenAudioChannel() {
    ur_ota_status_t ota_status = {0};
    ur_bridge_get_ota_status(&ota_status);
    if (ota_status.state == 1 || ota_status.state == 2) {
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }
    bool mqtt_connected = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        mqtt_connected = mqtt_connected_;
    }
    if (!mqtt_connected) {
        /* 冷启动时 Wi-Fi/MQTT 已连上但 AI 主题订阅仍可能需要数秒。等待真实的
         * 订阅就绪事件，避免把用户此时的唤醒误报为网络故障。 */
        const EventBits_t bits = xEventGroupWaitBits(event_group_, kMqttConnectedEvent, pdFALSE,
                                                     pdFALSE, kMqttReadyWait);
        if ((bits & kMqttConnectedEvent) == 0) {
            SetError(Lang::Strings::SERVER_NOT_CONNECTED);
            return false;
        }
    }

    /* 与 devicesim 一致，先尝试关闭服务端遗留会话；不存在时超时后继续。 */
    const std::string close_token = MakeToken("close");
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        expected_close_token_ = close_token;
    }
    xEventGroupClearBits(event_group_, kSessionClosedEvent | kSessionCreatedEvent);
    PublishMethod("sessionClose", close_token, nullptr);
    xEventGroupWaitBits(event_group_, kSessionClosedEvent, pdTRUE, pdFALSE, kCloseWait);

    audio_channel_->Close();
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        session_id_.clear();
        upload_url_.clear();
        expected_session_token_ = MakeToken("session");
        audio_upload_ready_ = false;
        pending_audio_frames_.clear();
        deferred_response_messages_.clear();
        ResetTurnState();
    }

    cJSON* params = cJSON_CreateObject();
    cJSON* modalities = cJSON_CreateArray();
    cJSON_AddItemToArray(modalities, cJSON_CreateString("text"));
    cJSON_AddItemToArray(modalities, cJSON_CreateString("audio"));
    cJSON_AddItemToArray(modalities, cJSON_CreateString("image"));
    cJSON_AddItemToObject(params, "modalities", modalities);
    cJSON_AddStringToObject(params, "transport", "udp");
    cJSON* audio_params = cJSON_CreateObject();
    cJSON_AddStringToObject(audio_params, "format", "opus");
    cJSON_AddNumberToObject(audio_params, "sampleRate", kUrAiSampleRate);
    cJSON_AddNumberToObject(audio_params, "channels", 1);
    cJSON_AddNumberToObject(audio_params, "frameDuration", 60);
    cJSON_AddItemToObject(params, "audioParams", audio_params);
    std::string session_token;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        session_token = expected_session_token_;
    }
    if (!PublishMethod("sessionCreate", session_token, params)) {
        return false;
    }

    EventBits_t bits =
        xEventGroupWaitBits(event_group_, kSessionCreatedEvent, pdTRUE, pdFALSE, kSessionTimeout);
    if ((bits & kSessionCreatedEvent) == 0) {
        ESP_LOGE(kTag, "sessionCreated timeout");
        SetError(Lang::Strings::SERVER_TIMEOUT);
        return false;
    }

    UdpAudioChannelConfig config;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        config = audio_channel_config_;
    }
    if (!audio_channel_->Open(Board::GetInstance().GetNetwork(), config,
                              [this](std::unique_ptr<AudioStreamPacket> packet) {
                                  HandleIncomingAudio(std::move(packet));
                              })) {
        SetError(Lang::Strings::SERVER_ERROR);
        return false;
    }

    if (!audio_channel_->SendPayload(kSilenceFrameOne.data(), kSilenceFrameOne.size())) {
        audio_channel_->Close();
        SetError(Lang::Strings::SERVER_ERROR);
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(60));
    if (!audio_channel_->SendPayload(kSilenceFrameTwo.data(), kSilenceFrameTwo.size())) {
        audio_channel_->Close();
        SetError(Lang::Strings::SERVER_ERROR);
        return false;
    }
    const size_t internal_dma_free =
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    const size_t internal_dma_largest =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    ESP_LOGI(kTag, "session memory: dma_free=%u dma_largest=%u",
             static_cast<unsigned>(internal_dma_free),
             static_cast<unsigned>(internal_dma_largest));
    if (internal_dma_free < kMinSessionInternalDmaBytes ||
        internal_dma_largest < kMinSessionDmaLargestBlockBytes) {
        ESP_LOGE(kTag, "insufficient internal DMA memory for voice session");
        audio_channel_->Close();
        PublishMethod("sessionClose", MakeToken("close"), nullptr);
        SetError("设备内存不足，请重启后重试");
        return false;
    }
    /* 与 devicesim 一致，给服务端一秒建立 UDP 路由后再允许 audioStart。 */
    vTaskDelay(pdMS_TO_TICKS(1000));
    last_incoming_time_ = std::chrono::steady_clock::now();
    error_occurred_ = false;
    if (on_audio_channel_opened_ != nullptr) {
        on_audio_channel_opened_();
    }
    return true;
}

void UrAiProtocol::CloseAudioChannel(bool send_goodbye) {
    CloseAudioChannelInternal(send_goodbye, send_goodbye);
}

// RequestCloseAudioChannel 复用关闭逻辑，晚到的关闭回执由既有 token 校验处理。
void UrAiProtocol::RequestCloseAudioChannel() {
    CloseAudioChannelInternal(true, false);
}

void UrAiProtocol::CloseAudioChannelInternal(bool send_goodbye, bool wait_reply) {
    bool was_open = audio_channel_ != nullptr && audio_channel_->IsOpen();
    bool close_published = false;
    if (send_goodbye && ur_device_is_connected()) {
        const std::string close_token = MakeToken("close");
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            expected_close_token_ = close_token;
        }
        xEventGroupClearBits(event_group_, kSessionClosedEvent);
        close_published = PublishMethod("sessionClose", close_token, nullptr);
    }
    if (close_published && wait_reply) {
        xEventGroupWaitBits(event_group_, kSessionClosedEvent, pdTRUE, pdFALSE, kCloseWait);
    }
    if (audio_channel_ != nullptr) {
        audio_channel_->Close();
    }
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        session_id_.clear();
        upload_url_.clear();
        audio_upload_ready_ = false;
        pending_audio_frames_.clear();
        deferred_response_messages_.clear();
        ResetTurnState();
    }
    if (was_open && on_audio_channel_closed_ != nullptr) {
        on_audio_channel_closed_();
    }
}

void UrAiProtocol::PrepareForOta() {
    /* MQTT 任务正在处理 OTA 下行，不能在这里等待由同一任务接收 sessionClosed。 */
    CloseAudioChannelInternal(true, false);
}

bool UrAiProtocol::IsAudioChannelOpened() const {
    bool connected = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        connected = mqtt_connected_;
    }
    return connected && audio_channel_ != nullptr && audio_channel_->IsOpen() && !error_occurred_ &&
           !IsTimeout();
}

std::string UrAiProtocol::GetUploadUrl() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return upload_url_;
}

bool UrAiProtocol::SendImageInput(const std::string& file_uri) {
    if (file_uri.empty() || file_uri.size() > 2048 || !IsAudioChannelOpened()) {
        return false;
    }

    const int64_t started_us = esp_timer_get_time();
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        /* 图片输入开始新一轮回复，不继承上一轮 respId、终态或音频缓冲。 */
        ResetTurnState();
        response_pending_ = true;
        turn_started_us_ = started_us;
        audio_stop_us_ = started_us;
        last_turn_activity_us_ = started_us;
    }

    cJSON* params = cJSON_CreateObject();
    cJSON* contents = cJSON_CreateArray();
    cJSON* prompt = cJSON_CreateObject();
    cJSON_AddStringToObject(prompt, "type", "text");
    cJSON_AddStringToObject(prompt, "text", "请描述刚拍摄的画面");
    cJSON_AddItemToArray(contents, prompt);
    cJSON* image = cJSON_CreateObject();
    cJSON_AddStringToObject(image, "type", "image_url");
    cJSON_AddStringToObject(image, "imageUrl", file_uri.c_str());
    cJSON_AddItemToArray(contents, image);
    cJSON_AddItemToObject(params, "contents", contents);
    cJSON* modalities = cJSON_CreateArray();
    cJSON_AddItemToArray(modalities, cJSON_CreateString("text"));
    cJSON_AddItemToArray(modalities, cJSON_CreateString("audio"));
    cJSON_AddItemToObject(params, "modalities", modalities);

    if (PublishMethod("inputSend", MakeToken("image"), params)) {
        ESP_LOGI(kTag, "method=inputSend image=1");
        return true;
    }
    std::lock_guard<std::mutex> lock(state_mutex_);
    ResetTurnState();
    return false;
}

bool UrAiProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    if (packet == nullptr || audio_channel_ == nullptr || !audio_channel_->IsOpen()) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!audio_upload_ready_) {
            if (pending_audio_frames_.size() >= kPendingAudioFramesMax) {
                pending_audio_frames_.pop_front();
            }
            pending_audio_frames_.push_back(std::move(packet));
            return true;
        }
    }
    return audio_channel_->Send(*packet);
}

void UrAiProtocol::SendWakeWordDetected(const std::string& wake_word) {
    (void)wake_word;
    /* 联犀只接收 audioStart 后的实时 Opus，不上传唤醒词原始数据或文本。 */
}

void UrAiProtocol::SendStartListening(ListeningMode mode) {
    (void)mode;
    std::string audio_token;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        // 先清旧轮，再设置新轮等待ACK状态；清理函数会重置audio_start_pending_。
        ResetTurnState();
        expected_audio_token_ = MakeToken("audio");
        audio_token = expected_audio_token_;
        audio_start_pending_ = true;
        response_pending_ = false;
        audio_upload_ready_ = false;
        pending_audio_frames_.clear();
        voice_seen_ = false;
        vad_speaking_ = false;
        audio_stop_sent_ = false;
        /* 新一轮在 respCreated 前不继承旧 respId，乱序终态才能进入延后队列。 */
        deferred_response_messages_.clear();
        turn_started_us_ = esp_timer_get_time();
        vad_endpoint_.Begin(turn_started_us_);
        audio_stop_us_ = 0;
        last_turn_activity_us_ = turn_started_us_;
    }
    xEventGroupClearBits(event_group_, kAudioStartedEvent);
    if (!PublishMethod("audioStart", audio_token, nullptr)) {
        SetError(Lang::Strings::SERVER_ERROR);
    }
}

void UrAiProtocol::SendStopListening() {
    int64_t turn_started_us = 0;
    std::string audio_token;  // 将等待提示关联到已结束收音的轮次。
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (audio_stop_sent_) {
            return;
        }
        audio_stop_sent_ = true;
        response_pending_ = true;
        audio_upload_ready_ = false;
        pending_audio_frames_.clear();
        audio_stop_us_ = esp_timer_get_time();
        last_turn_activity_us_ = audio_stop_us_;
        turn_started_us = turn_started_us_;
        audio_token = expected_audio_token_;
    }
    PublishMethod("audioStop", MakeToken("stop"), nullptr);
    EmitLegacyJson("listen", "wait", audio_token);
    ESP_LOGI(kTag, "method=audioStop elapsed=%ldms", ElapsedMs(turn_started_us));
}

bool UrAiProtocol::IsWaitingForResponse(const std::string& audio_token) const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return !audio_token.empty() && audio_token == expected_audio_token_ &&
           response_pending_ && audio_stop_sent_;
}

bool UrAiProtocol::ResumeAfterNoResult(const std::string& audio_token, ListeningMode mode,
                                      bool speech_resumed) {
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        // 用户已退出、已开启新轮或晚到的识别已有回复时，不得覆盖当前流程。
        if (audio_token.empty() || audio_token != expected_audio_token_ || !mqtt_connected_ ||
            session_id_.empty() || !audio_stop_sent_ || (response_pending_ && !speech_resumed) ||
            !stt_text_.empty() || !current_resp_id_.empty()) return false;
    }
    // 使用 devicesim 已验收的取消→audioStart 流程；新ACK前仍禁止真实音频上行。
    SendAbortSpeaking(kAbortReasonNone);
    SendStartListening(mode);
    // 新讲话已经产生VAD上升沿；新轮重置后须接续这一状态，不能等待不会重复上报的上升沿。
    if (speech_resumed) OnLocalVadState(true);
    return true;
}

void UrAiProtocol::OnLocalVadState(bool speaking) {
    std::string recovery_token;  // 新语音只恢复尚无识别和回复的旧轮，避免等待空结果时吞掉后续讲话。
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (audio_stop_sent_) {
            if (speaking && !vad_speaking_ && response_pending_ &&
                stt_text_.empty() && current_resp_id_.empty()) {
                recovery_token = expected_audio_token_;
            }
            vad_speaking_ = speaking;
        } else {
            if (!audio_start_pending_ && !audio_upload_ready_) return;
            vad_speaking_ = speaking;
            if (speaking) voice_seen_ = true;
            vad_endpoint_.Update(speaking, esp_timer_get_time());
            ESP_LOGI(kTag, "vad=%s elapsed=%ldms", speaking ? "speech" : "silence",
                     ElapsedMs(turn_started_us_));
        }
    }
    if (!recovery_token.empty()) {
        // 恢复交给主任务调度，并在真正取消前重新核对晚到的STT与轮次。
        ESP_LOGI(kTag, "new speech during empty-result wait; requesting listening recovery");
        EmitLegacyJson("listen", "speech_restart", recovery_token);
    }
}

void UrAiProtocol::SendAbortSpeaking(AbortReason reason) {
    (void)reason;
    if (PublishMethod("respCancel", MakeToken("cancel"), nullptr)) {
        ESP_LOGI(kTag, "method=respCancel queued=1");
    }
    std::lock_guard<std::mutex> lock(state_mutex_);
    /* 取消后的旧轮不再等待终态，避免新一轮开始前旧超时把界面打回 idle。 */
    response_pending_ = false;
    response_audio_expected_ = false;
    response_audio_announced_ = false;
    pending_incoming_audio_frames_.clear();
    deferred_audio_done_message_.clear();
}

void UrAiProtocol::SendMcpMessage(const std::string& message) {
    (void)message;
    /* MCP 工具由 session 绑定的服务端 Agent 执行，设备端不再转发公共小智 MCP。 */
}

#if CONFIG_UR_AI_E2E_TEST
void UrAiProtocol::ResetE2eObservation(bool clear_audio_started) {
    xEventGroupClearBits(event_group_, kE2eResultMask | (clear_audio_started ? kAudioStartedEvent : 0));
    std::lock_guard<std::mutex> lock(state_mutex_);
    stt_text_.clear();
    assistant_text_.clear();
}

UrAiE2eTurn UrAiProtocol::GetE2eTurn() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return {.ready = audio_upload_ready_ && !audio_start_pending_ && !audio_stop_sent_ &&
                         !session_id_.empty(),
            .session_id = session_id_, .audio_token = expected_audio_token_};
}

bool UrAiProtocol::WaitForAudioUploadReady(TickType_t timeout) const {
    const EventBits_t bits =
        xEventGroupWaitBits(event_group_, kAudioStartedEvent, pdFALSE, pdFALSE, timeout);
    return (bits & kAudioStartedEvent) != 0;
}

UrAiE2eResult UrAiProtocol::WaitForE2eResult(TickType_t timeout, const std::string& expected_text,
                                          const std::string& expected_stt) const {
    /* 图片轮没有 STT；仅在串口指定现场物品词时等待图片回复的三个阶段。 */
    const EventBits_t mask = expected_text.empty() ? kE2eResultMask :
        (kE2eTextDoneEvent | kE2eFirstAudioEvent | kE2eAudioDoneEvent);
    const EventBits_t bits =
        xEventGroupWaitBits(event_group_, mask, pdFALSE, pdTRUE, timeout);
    UrAiE2eResult result;
    result.stt_done = (bits & kE2eSttDoneEvent) != 0;
    result.text_done = (bits & kE2eTextDoneEvent) != 0;
    result.first_audio = (bits & kE2eFirstAudioEvent) != 0;
    result.audio_done = (bits & kE2eAudioDoneEvent) != 0;
    std::lock_guard<std::mutex> lock(state_mutex_);
    result.stt_nonempty = !stt_text_.empty();
    result.stt_matches_fixture = !expected_stt.empty() && stt_text_.find(expected_stt) != std::string::npos;
    result.text_nonempty = !assistant_text_.empty();
    /* 平台模型失败沿用文本/音频事件，事件到齐不等于模型调用成功。
     * 仅识别平台固定错误提示，不把正常回答中的“失败”或状态码误判为错误。 */
    result.service_error = assistant_text_.find("AI 服务本次调用失败（") != std::string::npos;
    result.expected_text_matched = !expected_text.empty() &&
        assistant_text_.find(expected_text) != std::string::npos;
    return result;
}
#endif

bool UrAiProtocol::SendText(const std::string& text) {
    return ur_device_ai_publish(text.data(), text.size());
}

bool UrAiProtocol::PublishMethod(const char* method, const std::string& token, cJSON* params) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "method", method);
    if (!token.empty()) {
        cJSON_AddStringToObject(root, "msgToken", token.c_str());
    }
    if (params != nullptr) {
        cJSON_AddItemToObject(root, "params", params);
    }
    char* encoded = cJSON_PrintUnformatted(root);
    bool published = encoded != nullptr && SendText(encoded);
    if (!published) {
        ESP_LOGW(kTag, "AI publish queue rejected method=%s", method);
    }
    cJSON_free(encoded);
    cJSON_Delete(root);
    return published;
}

std::string UrAiProtocol::MakeToken(const char* prefix) const {
    char token[48] = {0};
    snprintf(token, sizeof(token), "%s-%08lx-%08lx", prefix,
             static_cast<unsigned long>(esp_random()),
             static_cast<unsigned long>(esp_timer_get_time()));
    return token;
}

void UrAiProtocol::EnqueueIncoming(const char* payload, size_t length) {
    if (payload == nullptr || length == 0 || terminal_queue_ == nullptr ||
        delta_queue_ == nullptr) {
        return;
    }
    auto message = std::make_unique<IncomingMessage>();
    message->payload.assign(payload, length);
    IncomingMessage* raw_message = message.get();
    const bool low_priority =
        message->payload.find("\"method\":\"respTextDelta\"") != std::string::npos;
    if (low_priority) {
        if (xQueueSend(delta_queue_, &raw_message, 0) == pdTRUE) {
            message.release();
        }
        return;
    }
    {
        std::lock_guard<std::mutex> lock(terminal_overflow_mutex_);
        if (!terminal_overflow_.empty()) {
            terminal_overflow_.push_back(std::move(message));
            return;
        }
    }
    if (xQueueSend(terminal_queue_, &raw_message, 0) == pdTRUE) {
        message.release();
        return;
    }
    /* 高优先级队列满时转入无损溢出队列；仅文本 delta 允许丢弃。 */
    std::lock_guard<std::mutex> lock(terminal_overflow_mutex_);
    terminal_overflow_.push_back(std::move(message));
}

void UrAiProtocol::WorkerEntry(void* context) { static_cast<UrAiProtocol*>(context)->WorkerLoop(); }

void UrAiProtocol::WorkerLoop() {
    IncomingMessage* raw_message = nullptr;
    while (*alive_) {
#if CONFIG_UR_UDP_TX_TRACE
        /* 在普通任务读取有界数值队列，Wi-Fi回调不打印或阻塞。 */
        UrUdpNetifTraceFlush();
        UrUdpWifiTraceFlush();
#endif
        CheckTurnTimeouts();
        if (xQueueReceive(terminal_queue_, &raw_message, 0) == pdTRUE) {
            std::unique_ptr<IncomingMessage> message(raw_message);
            HandleIncomingPayload(message->payload);
            continue;
        }
        std::unique_ptr<IncomingMessage> overflow_message;
        {
            std::lock_guard<std::mutex> lock(terminal_overflow_mutex_);
            if (!terminal_overflow_.empty()) {
                overflow_message = std::move(terminal_overflow_.front());
                terminal_overflow_.pop_front();
            }
        }
        if (overflow_message != nullptr) {
            HandleIncomingPayload(overflow_message->payload);
            continue;
        }
        if (xQueueReceive(delta_queue_, &raw_message, pdMS_TO_TICKS(100)) == pdTRUE) {
            std::unique_ptr<IncomingMessage> message(raw_message);
            HandleIncomingPayload(message->payload);
        }
    }
    worker_task_ = nullptr;
    vTaskDelete(nullptr);
}

void UrAiProtocol::CheckTurnTimeouts() {
    const int64_t now_us = esp_timer_get_time();
    bool audio_start_timeout = false;
    bool response_timeout = false;
    bool vad_should_stop = false;
    std::string no_result_token;  // 无识别轮次走自动恢复，正常生成超时仍走错误保护。
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        vad_should_stop = audio_upload_ready_ && !audio_stop_sent_ &&
                          vad_endpoint_.ShouldStop(now_us);
        audio_start_timeout = audio_start_pending_ && turn_started_us_ > 0 &&
                              now_us - turn_started_us_ >= kAudioStartTimeoutUs;
        response_timeout = response_pending_ && last_turn_activity_us_ > 0 &&
                           now_us - last_turn_activity_us_ >= kResponseTimeoutUs;
        if (audio_start_timeout) {
            audio_start_pending_ = false;
            expected_audio_token_.clear();
            pending_audio_frames_.clear();
        }
        if (response_timeout) {
            if (!audio_start_timeout && audio_stop_sent_ && stt_text_.empty() &&
                current_resp_id_.empty()) no_result_token = expected_audio_token_;
            response_pending_ = false;
            response_audio_expected_ = false;
            pending_incoming_audio_frames_.clear();
        }
    }
    if (vad_should_stop) SendStopListening();
    if (response_timeout && !no_result_token.empty()) {
        ESP_LOGW(kTag, "no recognition result; requesting listening recovery");
        EmitLegacyJson("listen", "restart", no_result_token);
        return;
    }
    if (audio_start_timeout || response_timeout) {
        ESP_LOGE(kTag, "%s timeout", audio_start_timeout ? "audioStarted" : "response");
        SetError(Lang::Strings::SERVER_TIMEOUT);
    }
}

void UrAiProtocol::HandleIncomingPayload(const std::string& payload) {
    cJSON* root = cJSON_ParseWithLength(payload.data(), payload.size());
    if (root == nullptr) {
        ESP_LOGW(kTag, "ignored malformed AI downlink");
        return;
    }
    const char* method = JsonString(root, "method");
    const char* token = JsonString(root, "msgToken");
    cJSON* code = cJSON_GetObjectItemCaseSensitive(root, "code");
    cJSON* data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (method == nullptr) {
        cJSON_Delete(root);
        return;
    }
    last_incoming_time_ = std::chrono::steady_clock::now();

    if (strcmp(method, "sessionClosed") == 0) {
        bool matched = false;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            matched = token != nullptr && token == expected_close_token_;
        }
        if (matched) {
            xEventGroupSetBits(event_group_, kSessionClosedEvent);
        }
        cJSON_Delete(root);
        return;
    }
    if (strcmp(method, "sessionCreated") == 0) {
        bool token_matches = false;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            token_matches = token != nullptr && token == expected_session_token_;
        }
        bool valid =
            token_matches && cJSON_IsNumber(code) && code->valueint == 200 && cJSON_IsObject(data);
        cJSON* udp = valid ? cJSON_GetObjectItemCaseSensitive(data, "udp") : nullptr;
        const char* session_id = valid ? JsonString(data, "sessionId") : nullptr;
        const char* transport = valid ? JsonString(data, "transport") : nullptr;
        const char* upload_url = valid ? JsonString(data, "uploadUrl") : nullptr;
        const char* server = JsonString(udp, "server");
        const char* key = JsonString(udp, "key");
        const char* nonce = JsonString(udp, "nonce");
        cJSON* port = cJSON_IsObject(udp) ? cJSON_GetObjectItemCaseSensitive(udp, "port") : nullptr;
        valid = valid && session_id != nullptr && transport != nullptr &&
                strcmp(transport, "udp") == 0 && server != nullptr && key != nullptr &&
                nonce != nullptr && strlen(key) == 32 && strlen(nonce) == 32 &&
                cJSON_IsNumber(port) && port->valueint > 0 && port->valueint <= UINT16_MAX;
        if (valid) {
            std::lock_guard<std::mutex> lock(state_mutex_);
            session_id_ = session_id;
            upload_url_ = upload_url != nullptr ? upload_url : "";
            audio_channel_config_ = {.server = server,
                                     .port = port->valueint,
                                     .key_hex = key,
                                     .nonce_hex = nonce,
                                     .sample_rate = kUrAiSampleRate,
                                     .frame_duration = 60,
                                     .preserve_nonce_timestamp = true};
            ESP_LOGI(kTag, "method=sessionCreated session=%.8s", session_id_.c_str());
            xEventGroupSetBits(event_group_, kSessionCreatedEvent);
        } else {
            ESP_LOGE(kTag, "invalid sessionCreated response");
        }
        cJSON_Delete(root);
        return;
    }
    if (strcmp(method, "audioStarted") == 0) {
        bool valid = false;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            valid = token != nullptr && token == expected_audio_token_ && cJSON_IsNumber(code) &&
                    code->valueint == 200;
        }
        if (valid) {
            int64_t turn_started_us = 0;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                audio_start_pending_ = false;
                audio_upload_ready_ = true;
                turn_started_us = turn_started_us_;
            }
            xEventGroupSetBits(event_group_, kAudioStartedEvent);
            ESP_LOGI(kTag, "method=audioStarted elapsed=%ldms", ElapsedMs(turn_started_us));
            FlushPendingAudio();
        }
        cJSON_Delete(root);
        return;
    }

    if (!MatchesCurrentSession(root, data)) {
        cJSON_Delete(root);
        return;
    }

    bool deferred = false;
    /* STT 在 respCreated 之前产生，devicesim 契约不保证它携带 respId；其余回复
     * 必须绑定当前 respId，防止上轮文本、音频或表情污染新一轮。 */
    const bool is_stt = strcmp(method, "respSttDelta") == 0 || strcmp(method, "respSttDone") == 0;
    const bool response_scoped =
        (!is_stt && strncmp(method, "resp", 4) == 0) || strncmp(method, "toolCall", 8) == 0 ||
        strcmp(method, "audioSpeechStarted") == 0 || strcmp(method, "audioSpeechStopped") == 0 ||
        strcmp(method, "audioSpeechInterrupted") == 0;
    if (response_scoped && strcmp(method, "respCreated") != 0 &&
        !MatchesCurrentResponse(data, method, payload, &deferred)) {
        cJSON_Delete(root);
        return;
    }
    if (deferred) {
        cJSON_Delete(root);
        return;
    }
    if (strncmp(method, "resp", 4) == 0 || strncmp(method, "toolCall", 8) == 0 ||
        strncmp(method, "audioSpeech", 11) == 0) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        last_turn_activity_us_ = esp_timer_get_time();
    }

    if (strcmp(method, "respCreated") == 0) {
        const char* resp_id = JsonString(data, "respId");
        if (resp_id != nullptr) {
            std::deque<std::string> deferred_messages;
            int64_t audio_stop_us = 0;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                if (current_resp_id_ == resp_id) {
                    cJSON_Delete(root);
                    return;
                }
                current_resp_id_ = resp_id;
                audio_stop_us = audio_stop_us_;
                deferred_messages.swap(deferred_response_messages_);
            }
            ESP_LOGI(kTag, "method=respCreated resp=%.8s elapsed=%ldms", resp_id,
                     ElapsedMs(audio_stop_us));
            cJSON_Delete(root);
            for (const auto& deferred_payload : deferred_messages) {
                HandleIncomingPayload(deferred_payload);
            }
            return;
        }
        cJSON_Delete(root);
        return;
    }

    if (strcmp(method, "respAudioStart") == 0 || strcmp(method, "audioSpeechStarted") == 0) {
        std::string deferred_audio_done;
        std::string resp_id;
        int64_t audio_stop_us = 0;
        bool duplicate = false;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            duplicate =
                response_audio_expected_ || terminal_tracker_.Seen(UrAiTerminalEvent::kAudioDone);
            if (!duplicate) {
                response_audio_expected_ = true;
                resp_id = current_resp_id_;
                audio_stop_us = audio_stop_us_;
                deferred_audio_done.swap(deferred_audio_done_message_);
            }
        }
        if (duplicate) {
            cJSON_Delete(root);
            return;
        }
        ESP_LOGI(kTag, "method=%s resp=%.8s elapsed=%ldms", method, resp_id.c_str(),
                 ElapsedMs(audio_stop_us));
        FlushPendingIncomingAudio();
        cJSON_Delete(root);
        if (!deferred_audio_done.empty()) {
            HandleIncomingPayload(deferred_audio_done);
        }
        return;
    }

    if (strcmp(method, "respAudioDone") == 0 || strcmp(method, "audioSpeechStopped") == 0 ||
        strcmp(method, "audioSpeechInterrupted") == 0) {
        std::string resp_id;
        int64_t audio_stop_us = 0;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (terminal_tracker_.Seen(UrAiTerminalEvent::kAudioDone)) {
                cJSON_Delete(root);
                return;
            }
            if (!response_audio_expected_ && !response_audio_announced_) {
                deferred_audio_done_message_ = payload;
                cJSON_Delete(root);
                return;
            }
            terminal_tracker_.Mark(UrAiTerminalEvent::kAudioDone);
#if CONFIG_UR_AI_E2E_TEST
            xEventGroupSetBits(event_group_, kE2eAudioDoneEvent);
#endif
            resp_id = current_resp_id_;
            audio_stop_us = audio_stop_us_;
        }
        /* MQTT 与 UDP 是不同链路，保留三个 Opus 帧的乱序窗口后再交给播放队列排空。 */
        vTaskDelay(pdMS_TO_TICKS(180));
        FlushPendingIncomingAudio();
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (current_resp_id_ != resp_id) {
                cJSON_Delete(root);
                return;
            }
            response_audio_expected_ = false;
            response_pending_ = false;
        }
        EmitLegacyJson("tts", "stop", "");
        ESP_LOGI(kTag, "method=%s resp=%.8s elapsed=%ldms", method, resp_id.c_str(),
                 ElapsedMs(audio_stop_us));
        cJSON_Delete(root);
        return;
    }

    /* 单轮字幕、终态与耗时由 worker 串行更新，但主任务会重置轮次，故统一持锁。 */
    std::lock_guard<std::mutex> response_lock(state_mutex_);
    if (strcmp(method, "respSttDelta") == 0) {
        const char* delta = JsonString(data, "delta");
        if (!terminal_tracker_.Seen(UrAiTerminalEvent::kSttDone) && delta != nullptr) {
            stt_text_ += delta;
            EmitLegacyJson("stt", nullptr, stt_text_);
        }
    } else if (strcmp(method, "respSttDone") == 0) {
        const char* text = JsonString(data, "text");
        if (terminal_tracker_.Mark(UrAiTerminalEvent::kSttDone)) {
            if (text != nullptr) {
                stt_text_ = text;
                EmitLegacyJson("stt", nullptr, stt_text_);
            }
            ESP_LOGI(kTag, "method=respSttDone resp=%.8s elapsed=%ldms", current_resp_id_.c_str(),
                     ElapsedMs(audio_stop_us_));
#if CONFIG_UR_AI_E2E_TEST
            xEventGroupSetBits(event_group_, kE2eSttDoneEvent);
#endif
        }
    } else if (strcmp(method, "respTextDelta") == 0) {
        const char* delta = JsonString(data, "delta");
        if (!terminal_tracker_.Seen(UrAiTerminalEvent::kTextDone) && delta != nullptr) {
            assistant_text_ += delta;
            EmitLegacyJson("tts", "sentence_start", assistant_text_);
        }
    } else if (strcmp(method, "respTextDone") == 0) {
        const char* text = JsonString(data, "text");
        if (terminal_tracker_.Mark(UrAiTerminalEvent::kTextDone)) {
            if (text != nullptr && assistant_text_ != text) {
                assistant_text_ = text;
                EmitLegacyJson("tts", "sentence_start", assistant_text_);
            }
            ESP_LOGI(kTag, "method=respTextDone resp=%.8s elapsed=%ldms", current_resp_id_.c_str(),
                     ElapsedMs(audio_stop_us_));
#if CONFIG_UR_AI_E2E_TEST
            xEventGroupSetBits(event_group_, kE2eTextDoneEvent);
#endif
        }
    } else if (strcmp(method, "respEmotion") == 0) {
        if (terminal_tracker_.Mark(UrAiTerminalEvent::kEmotion)) {
            EmitLegacyJson("llm", nullptr, NormalizeEmotion(data));
        }
    } else if (strcmp(method, "toolCallStart") == 0 || strcmp(method, "toolCallResult") == 0) {
        ESP_LOGI(kTag, "method=%s resp=%.8s", method, current_resp_id_.c_str());
    } else if (strcmp(method, "error") == 0) {
        const char* error_code = JsonString(data, "code");
        response_pending_ = false;
        ESP_LOGW(kTag, "method=error code=%s resp=%.8s",
                 error_code != nullptr ? error_code : "unknown", current_resp_id_.c_str());
        SetError(Lang::Strings::SERVER_ERROR);
    }
    cJSON_Delete(root);
}

void UrAiProtocol::HandleIncomingAudio(std::unique_ptr<AudioStreamPacket> packet) {
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        last_turn_activity_us_ = esp_timer_get_time();
        if (!response_audio_expected_) {
            /* 输入阶段不缓存上一轮被打断的 UDP 尾帧；只有已经结束输入或已建立
             * 当前 respId 时，才允许为乱序的 respAudioStart 保留少量首帧。 */
            if (terminal_tracker_.Seen(UrAiTerminalEvent::kAudioDone) ||
                (!response_pending_ && current_resp_id_.empty())) {
                return;
            }
            if (pending_incoming_audio_frames_.size() >= kPendingIncomingAudioFramesMax) {
                pending_incoming_audio_frames_.pop_front();
            }
            pending_incoming_audio_frames_.push_back(std::move(packet));
            return;
        }
        pending_incoming_audio_frames_.push_back(std::move(packet));
    }
    FlushPendingIncomingAudio();
}

void UrAiProtocol::FlushPendingIncomingAudio() {
    bool announce = false;
    std::string resp_id;
    int64_t audio_stop_us = 0;
    std::deque<std::unique_ptr<AudioStreamPacket>> pending;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!response_audio_expected_ || pending_incoming_audio_frames_.empty()) {
            return;
        }
        if (!response_audio_announced_) {
            response_audio_announced_ = true;
            announce = true;
        }
        resp_id = current_resp_id_;
        audio_stop_us = audio_stop_us_;
        pending.swap(pending_incoming_audio_frames_);
    }
    if (announce) {
        EmitLegacyJson("tts", "start", "");
#if CONFIG_UR_AI_E2E_TEST
        xEventGroupSetBits(event_group_, kE2eFirstAudioEvent);
#endif
        ESP_LOGI(kTag, "method=firstAudio resp=%.8s elapsed=%ldms", resp_id.c_str(),
                 ElapsedMs(audio_stop_us));
    }
    if (on_incoming_audio_ != nullptr) {
        for (auto& packet : pending) {
            on_incoming_audio_(std::move(packet));
        }
    }
}

void UrAiProtocol::HandleConnectionChanged(bool connected) {
    if (connected) {
        xEventGroupSetBits(event_group_, kMqttConnectedEvent);
    } else {
        xEventGroupClearBits(event_group_, kMqttConnectedEvent);
    }
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        changed = mqtt_connected_ != connected;
        mqtt_connected_ = connected;
    }
    if (!changed) {
        return;
    }
    if (connected) {
        error_occurred_ = false;
        if (on_connected_ != nullptr) {
            on_connected_();
        }
    } else {
        CloseAudioChannel(false);
        if (on_disconnected_ != nullptr) {
            on_disconnected_();
        }
    }
}

void UrAiProtocol::EmitLegacyJson(const char* type, const char* state, const std::string& value) {
    if (on_incoming_json_ == nullptr) {
        return;
    }
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", type);
    if (state != nullptr) {
        cJSON_AddStringToObject(root, "state", state);
    }
    if (strcmp(type, "stt") == 0 || strcmp(type, "listen") == 0 ||
        (state != nullptr && strcmp(state, "sentence_start") == 0)) {
        cJSON_AddStringToObject(root, "text", value.c_str());
    } else if (strcmp(type, "llm") == 0) {
        cJSON_AddStringToObject(root, "emotion", value.c_str());
    }
    on_incoming_json_(root);
    cJSON_Delete(root);
}

bool UrAiProtocol::MatchesCurrentResponse(cJSON* data, const char* method,
                                          const std::string& raw_payload, bool* deferred) {
    *deferred = false;
    const char* resp_id = JsonString(data, "respId");
    std::lock_guard<std::mutex> lock(state_mutex_);
    const auto disposition = UrAiClassifyResponse(
        current_resp_id_, resp_id != nullptr ? resp_id : "", method != nullptr ? method : "");
    if (disposition == UrAiResponseDisposition::kDefer) {
        deferred_response_messages_.push_back(raw_payload);
        *deferred = true;
        return false;
    }
    return disposition == UrAiResponseDisposition::kAccept;
}

bool UrAiProtocol::MatchesCurrentSession(cJSON* root, cJSON* data) const {
    const char* message_session_id = JsonString(root, "sessionId");
    if (message_session_id == nullptr) {
        message_session_id = JsonString(data, "sessionId");
    }
    std::lock_guard<std::mutex> lock(state_mutex_);
    return UrAiMatchesSession(session_id_, message_session_id != nullptr ? message_session_id : "");
}

void UrAiProtocol::FlushPendingAudio() {
    std::deque<std::unique_ptr<AudioStreamPacket>> pending;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        pending.swap(pending_audio_frames_);
    }
    for (auto& packet : pending) {
        if (!audio_channel_->Send(*packet)) {
            break;
        }
    }
}

void UrAiProtocol::ResetTurnState() {
    audio_start_pending_ = false;
    response_pending_ = false;
    pending_incoming_audio_frames_.clear();
    deferred_audio_done_message_.clear();
    current_resp_id_.clear();
    stt_text_.clear();
    assistant_text_.clear();
    response_audio_expected_ = false;
    response_audio_announced_ = false;
    terminal_tracker_.Reset();
    last_turn_activity_us_ = 0;
}

std::string UrAiProtocol::NormalizeEmotion(cJSON* data) {
    const char* emoji = JsonString(data, "emoji");
    const char* emotion = JsonString(data, "emotion");
    return std::string(
        UrAiNormalizeEmotion(emoji != nullptr ? emoji : "", emotion != nullptr ? emotion : ""));
}
