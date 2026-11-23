/* 联犀设备 AI 协议：复用设备 MQTT，管理多轮会话与加密 UDP Opus 音频。 */
#ifndef UR_AI_PROTOCOL_H
#define UR_AI_PROTOCOL_H

#include "protocol.h"
#include "udp_audio_channel.h"
#include "ur_ai_runtime.h"

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

#if CONFIG_UR_AI_E2E_TEST
/* UrAiE2eTurn 保存测试所需的就绪轮次标识；完整标识仅内存比较，不输出日志。 */
struct UrAiE2eTurn {
    bool ready = false;  // 已收到当前 token 的成功回执，且本轮尚未停止上传。
    std::string session_id;  // 用于证明多轮没有重建会话。
    std::string audio_token;  // 用于拒绝把上一轮就绪状态复用于下一轮。
};

/* UrAiE2eResult 汇总一次内置语音测试必须经过的服务端与设备端阶段。 */
struct UrAiE2eResult {
    bool stt_done = false;
    bool stt_nonempty = false;
    bool stt_matches_fixture = false;
    bool text_done = false;
    bool text_nonempty = false;
    /* service_error 拦截平台以普通文本播报的模型失败，不能作为成功对话验收。 */
    bool service_error = false;
    bool first_audio = false;
    bool audio_done = false;
    /* expected_text_matched 表示回复包含测试指定的现场物品词，不输出对话正文。 */
    bool expected_text_matched = false;
};
#endif

/* UrAiProtocol 将 devicesim 已验证的 AI MQTT/UDP 契约适配到现有应用协议接口。 */
class UrAiProtocol : public Protocol {
public:
    UrAiProtocol();
    ~UrAiProtocol() override;

    bool Start() override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel(bool send_goodbye = true) override;
    // RequestCloseAudioChannel 发送 sessionClose 后立即释放本地通道，避免菜单阻塞五秒。
    void RequestCloseAudioChannel() override;
    bool IsAudioChannelOpened() const override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;
    void SendWakeWordDetected(const std::string& wake_word) override;
    void SendStartListening(ListeningMode mode) override;
    void SendStopListening() override;
    /* IsWaitingForResponse 校验音频轮次仍在等回复，避免迟到提示覆盖新轮。 */
    bool IsWaitingForResponse(const std::string& audio_token) const;
    /* ResumeAfterNoResult 仅恢复无识别及回复的原轮；speech_resumed允许新讲话提前结束空等待，返回是否已重启。 */
    bool ResumeAfterNoResult(const std::string& audio_token, ListeningMode mode,
                             bool speech_resumed = false);
    /* OnLocalVadState 更新句尾状态；空识别等待中的新语音边沿请求重新开麦。 */
    void OnLocalVadState(bool speaking) override;
    void PrepareForOta() override;
    void SendAbortSpeaking(AbortReason reason) override;
    void SendMcpMessage(const std::string& message) override;
    /* GetUploadUrl 返回当前 sessionCreated 下发的同会话文件上传入口。 */
    std::string GetUploadUrl() const;
    /* SendImageInput 在当前会话发送拍照图片与固定识图提示，并请求文字、语音回复。 */
    bool SendImageInput(const std::string& file_uri);
#if CONFIG_UR_AI_E2E_TEST
    /* ResetE2eObservation 清除终态和文本；多轮保留可能已到达的新 audioStarted 事件。 */
    void ResetE2eObservation(bool clear_audio_started = true);
    /* GetE2eTurn 在锁内获取会话及就绪轮次快照，仅供测试门禁比较。 */
    UrAiE2eTurn GetE2eTurn() const;
    /* WaitForAudioUploadReady 等待匹配 token 的 audioStarted 成功回复。 */
    bool WaitForAudioUploadReady(TickType_t timeout) const;
    /* WaitForE2eResult 在 timeout 内返回阶段快照；expected_text/expected_stt 分别校验回复/语料。 */
    UrAiE2eResult WaitForE2eResult(TickType_t timeout, const std::string& expected_text = "",
                                 const std::string& expected_stt = "音量") const;
#endif

private:
    /* 会话创建、音频就绪和会话关闭确认分别使用独立事件位。 */
    static constexpr EventBits_t kSessionCreatedEvent = 1 << 0;
    static constexpr EventBits_t kAudioStartedEvent = 1 << 1;
    static constexpr EventBits_t kSessionClosedEvent = 1 << 2;
    static constexpr EventBits_t kMqttConnectedEvent = 1 << 3;
    static constexpr EventBits_t kE2eSttDoneEvent = 1 << 4;
    static constexpr EventBits_t kE2eTextDoneEvent = 1 << 5;
    static constexpr EventBits_t kE2eFirstAudioEvent = 1 << 6;
    static constexpr EventBits_t kE2eAudioDoneEvent = 1 << 7;
    static constexpr EventBits_t kE2eResultMask =
        kE2eSttDoneEvent | kE2eTextDoneEvent | kE2eFirstAudioEvent | kE2eAudioDoneEvent;
    /* AI JSON 解析会出现较深调用栈，栈放入 PSRAM，避免挤占相机 SPI/UDP 必需的内部 DMA 内存。 */
    static constexpr size_t kWorkerTaskStackSize = 8192;
    /* 会话开始时至少保留这些内部 DMA 内存；不足时安全拒绝，不能让 SSCMA SPI 崩溃。 */
    static constexpr size_t kMinSessionInternalDmaBytes = 8192;
    /* SSCMA 每次 SPI 接收最多 4095 字节，需要至少一个完整且连续的 DMA 块。 */
    static constexpr size_t kMinSessionDmaLargestBlockBytes = 4096;
    /* 覆盖现有发送队列的 2.4 秒窗口，避免 audioStarted 稍慢时丢掉句首。 */
    static constexpr size_t kPendingAudioFramesMax = 40;
    /* 缓存 respAudioStart 乱序到达前的少量下行帧，避免截掉回复开头。 */
    static constexpr size_t kPendingIncomingAudioFramesMax = 12;

    /* IncomingMessage 是 MQTT 回调与协议解析任务之间的报文副本。 */
    struct IncomingMessage {
        std::string payload;
    };

    /* state_mutex_ 保护跨 MQTT worker、UDP 回调与应用主任务共享的会话状态。 */
    mutable std::mutex state_mutex_;
    /* FreeRTOS 对象承载请求确认和高低优先级下行队列。 */
    EventGroupHandle_t event_group_ = nullptr;
    QueueHandle_t terminal_queue_ = nullptr;
    QueueHandle_t delta_queue_ = nullptr;
    TaskHandle_t worker_task_ = nullptr;
    StaticTask_t* worker_task_buffer_ = nullptr;
    StackType_t* worker_task_stack_ = nullptr;
    /* alive_ 防止已排入应用主任务的连接回调访问已析构协议。 */
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
    /* audio_channel_ 与配置描述当前 session 唯一的加密 UDP 通道。 */
    std::unique_ptr<UdpAudioChannel> audio_channel_;
    UdpAudioChannelConfig audio_channel_config_;
    /* 暂存 audioStarted 前的音频及 respCreated 前乱序到达的终态。 */
    std::deque<std::unique_ptr<AudioStreamPacket>> pending_audio_frames_;
    std::deque<std::unique_ptr<AudioStreamPacket>> pending_incoming_audio_frames_;
    std::deque<std::string> deferred_response_messages_;
    /* audio done 先于 audio start 时暂存原报文，待本轮播放开始后再结束。 */
    std::string deferred_audio_done_message_;
    /* 高优先级队列满时使用无损溢出队列，独立锁避免阻塞状态机。 */
    std::mutex terminal_overflow_mutex_;
    std::deque<std::unique_ptr<IncomingMessage>> terminal_overflow_;

    /* 以下标志记录连接、单轮 VAD、播放及终态去重状态。 */
    bool mqtt_connected_ = false;
    bool audio_start_pending_ = false;
    bool response_pending_ = false;
    bool audio_upload_ready_ = false;
    bool voice_seen_ = false;
    bool vad_speaking_ = false;
    /* vad_endpoint_ 在协议 worker 中按持续静音判句尾，不在 AFE 回调立即停流。 */
    UrAiVadEndpoint vad_endpoint_;
    bool audio_stop_sent_ = false;
    bool response_audio_expected_ = false;
    bool response_audio_announced_ = false;
    UrAiTerminalTracker terminal_tracker_;
    /* 三类请求 token 分别匹配 session、audioStart 与 close 回复。 */
    std::string expected_session_token_;
    std::string expected_audio_token_;
    std::string expected_close_token_;
    /* 当前 respId 与字幕累计值用于乱序关联和增量去重。 */
    std::string current_resp_id_;
    /* upload_url_ 只在当前 session 生命周期内有效，断线和关闭时立即清空。 */
    std::string upload_url_;
    std::string stt_text_;
    std::string assistant_text_;
    /* 单调时钟时间点用于与 devicesim 同口径输出阶段耗时。 */
    int64_t turn_started_us_ = 0;
    int64_t audio_stop_us_ = 0;
    int64_t last_turn_activity_us_ = 0;

    /* SendText 把完整 JSON 放入联犀 MQTT 任务的上行队列。 */
    bool SendText(const std::string& text) override;
    /* PublishMethod 构造统一 method/msgToken/params 信封并发布。 */
    bool PublishMethod(const char* method, const std::string& token, cJSON* params);
    /* MakeToken 生成只用于本次启动期内关联响应的消息 token。 */
    std::string MakeToken(const char* prefix) const;
    /* EnqueueIncoming 在 MQTT 回调内完成有界复制，终态消息可短暂等待队列空间。 */
    void EnqueueIncoming(const char* payload, size_t length);
    /* WorkerEntry/WorkerLoop 在独立任务解析 AI JSON，避免阻塞 MQTT Yield。 */
    static void WorkerEntry(void* context);
    void WorkerLoop();
    /* CheckTurnTimeouts 结束未确认的 audioStart 或长期无终态的回复，避免会话卡死。 */
    void CheckTurnTimeouts();
    /* HandleIncomingPayload 校验关联关系并把平台事件翻译为应用内部协议事件。 */
    void HandleIncomingPayload(const std::string& payload);
    /* HandleIncomingAudio 在首帧到达时才宣告播报，并转交解码队列。 */
    void HandleIncomingAudio(std::unique_ptr<AudioStreamPacket> packet);
    /* FlushPendingIncomingAudio 按顺序交付 respAudioStart 前乱序到达的 UDP 帧。 */
    void FlushPendingIncomingAudio();
    /* HandleConnectionChanged 在应用主任务关闭失效会话。 */
    void HandleConnectionChanged(bool connected);
    /* EmitLegacyJson 构造现有 Application 可消费的 JSON 事件。 */
    void EmitLegacyJson(const char* type, const char* state, const std::string& value);
    /* MatchesCurrentResponse 校验 session 响应的 respId，未知当前响应时请求延后处理。 */
    bool MatchesCurrentResponse(cJSON* data, const char* method, const std::string& raw_payload,
                                bool* deferred);
    /* MatchesCurrentSession 在服务端携带 sessionId 时拒绝旧会话消息，缺省字段保持兼容。 */
    bool MatchesCurrentSession(cJSON* root, cJSON* data) const;
    /* FlushPendingAudio 在 audioStarted 后按原顺序发送暂存的麦克风帧。 */
    void FlushPendingAudio();
    /* ResetTurnState 清理单轮关联与去重状态，不销毁多轮 session。 */
    void ResetTurnState();
    /* CloseAudioChannelInternal 可选择等待关闭确认；OTA 路径只排队后立即让出 MQTT。 */
    void CloseAudioChannelInternal(bool send_goodbye, bool wait_reply);
    /* NormalizeEmotion 校验 21 种平台表情，未知值回退 neutral。 */
    static std::string NormalizeEmotion(cJSON* data);
};

#endif  // UR_AI_PROTOCOL_H
