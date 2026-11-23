/* 统一信令协议适配：把可移植 C++ 语音核心包装成现有 Protocol 接口供应用层复用。 */
#ifndef UR_SIGNALING_PROTOCOL_H
#define UR_SIGNALING_PROTOCOL_H

#include "protocol.h"
#include "ur/device/mqtt_port.hpp"
#include "ur/device/udp_channel.hpp"
#include "ur/device/udp_voice_media.hpp"
#include "ur/device/voice_runtime.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "ur_ai_runtime.h"
#include "ur_signaling_cipher.h"
#include "ur_signaling_mqtt_port.h"
#include "ur_signaling_opus.h"
#include "ur_signaling_pcm.h"
#include "ur_signaling_udp_port.h"
#if CONFIG_UR_AI_SIGNALING_WEBRTC
#include "ur_signaling_call.h"
#include "ur_signaling_webrtc_media.h"

#if CONFIG_UR_AI_SIGNALING_H264_UPLINK
#include "boards/sensecap-watcher/sscma_camera.h"
#include "ur_signaling_video.h"
#endif
#endif

/* SignalingProtocol 在独立信令任务中推进 VoiceRuntime；应用主任务只投递命令并
 * 等待事件位。音频经 AudioService 信令旁路队列跨线程交接，协议状态全部留在
 * 信令任务内。 */
class SignalingProtocol final : public Protocol {
public:
    SignalingProtocol();
    ~SignalingProtocol() override;

    /* Instance 返回当前协议实例；串口调试命令（sig_ask 等）经此触达文字提问。 */
    static SignalingProtocol* Instance() { return instance_; }
    /* SendTextQuestion 公开文字提问入口（基类 SendText 为 protected 应用接口）。 */
    bool SendTextQuestion(const std::string& text);
    /* OpenVisionChannel 启动 aiVision 三轨视觉会话并阻塞至 Established；
     * 视觉会话由 APP attach/detach 控制，本端自动开麦禁用，sig_ask 提问走此会话。 */
    bool OpenVisionChannel();
    /* SendPendingText 送出排队文字（信令任务内）；半双工先停采集再发。 */
    void SendPendingText(const char* trigger);
    bool Start() override;
    /* OpenAudioChannel 确保 MQTT/能力/会话全部建立并返回；阻塞至 Established。 */
    bool OpenAudioChannel() override;
    void CloseAudioChannel(bool send_goodbye = true) override;
    bool IsAudioChannelOpened() const override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;
    void SendWakeWordDetected(const std::string& wake_word) override;
    void SendStartListening(ListeningMode mode) override;
    void SendStopListening() override;
    void OnLocalVadState(bool speaking) override;
    void PrepareForOta() override;
    void SendAbortSpeaking(AbortReason reason) override;
    void SendMcpMessage(const std::string& message) override;
    /* IsFullDuplexSession 仅在平台协商 fullDuplex 后为真；应用据此保持播放期间采集。 */
    bool IsFullDuplexSession() const { return full_duplex_.load(); }

protected:
    /* SendText 在信令语音档未启用离散文字输入；保留接口占位并显式拒绝。 */
    bool SendText(const std::string& text) override;

private:
    /* 信令任务命令类型；参数仅保留少量整值，正文不经队列传递。 */
    enum class Command {
        kEnsureStarted, /* 连接 + 订阅 + 能力上报（首次或重连后）。 */
        kCreateSession, /* 请求新会话（capabilityReport + sessionCreate）。 */
        kStartInput,
        kStopInput,
        kCancelResponse,
        kSendText, /* 文字问题（aiVision 视觉提问）；正文经 pending_text_ 传递。 */
        kEndSession,
        kShutdown,
    };
    struct CommandItem {
        Command type;
    };

    /* 事件位：会话建立、会话结束与任务就绪分别等待。 */
    static constexpr EventBits_t kSessionEstablishedEvent = 1 << 0;
    static constexpr EventBits_t kSessionFinishedEvent = 1 << 1;
    // 人际通话占用时唤醒 AI 建连方，不把占用误判成网络超时。
    static constexpr EventBits_t kSessionBusyEvent = 1 << 2;
    // 建连命令由信令任务接管后再等待结果，隔离旧运行时的结束通知。
    static constexpr EventBits_t kSessionCreateStartedEvent = 1 << 3;

    bool EnsureTaskRunning();
    bool PostCommand(Command type);
    /* EnsureRuntimeCreated 创建（或会话级重建）VoiceRuntime 并发起连接与能力上报。 */
    bool EnsureRuntimeCreated();
    /* BuildCapability 组装当前构建的真实能力参数；空闲上报与运行时共用，
     * 保证同一修订号的能力指纹一致（否则平台按冲突拒绝后续上报）。 */
    ur::device::VoiceCapability BuildCapability() const;
    /* ReportIdleCapability 发布当前装配的原能力，填充平台准入缓存；
     * 返回仅表示本地受理，续报和失败重试由主循环安排。 */
    bool ReportIdleCapability();
    // MaintainCapability 在两条主循环续报原能力；now为单调毫秒，失败仅有界重试。
    void MaintainCapability(std::uint64_t now);
    /* EnsureSignalingAudio 装配信令编解码器/PCM桥/视频管线；AI 会话与空闲来电共用，
     * 首次调用会挂起旧协议解码器以腾出内部内存。 */
    bool EnsureSignalingAudio();
    /* EnsureCallAppearance 在收到首个来电前把人际通话外观绑定到共享端口与媒体桥；
     * 无 AI 运行时时也必须可用，否则冷启动来电无法接听。 */
    void EnsureCallAppearance();
    // WaitForEstablished 返回建立结果，并区分人际占用和会话已结束。
    bool WaitForEstablished(TickType_t timeout, bool* call_busy, bool* finished);
    void InvalidateSession(const char* reason);
    /* HealListeningCapture 低频自愈：会话已建立、应用处于监听态但设备侧采集未开时，
     * 请求应用主任务重放自己的开麦路径（Application::EnsureListeningAudio），
     * 修复媒体恢复链闭环后不回开采集的缺陷；细节与防误伤闸门见实现处注释。 */
    void HealListeningCapture(std::uint64_t now);

    /* EmitLegacyJson 沿用旧协议事件词汇驱动现有界面与状态机。 */
    void EmitLegacyJson(const char* type, const char* state, const std::string& value);
    /* HandleContent 把核心内容事件翻译为应用可消费的 JSON。 */
    void HandleContent(const ur::device::ContentEvent& content);
    /* HandlePlaybackEdges 按播放状态沿改变发出 tts start/stop。 */
    void HandlePlaybackEdges();

    /* TaskEntry/TaskLoop 在唯一信令任务串行推进网络与媒体。 */
    static void TaskEntry(void* context);
    void TaskLoop();
    // WaitForNextPoll 人际结果通知可提前唤醒，至少让出一tick；AI保持原轮询节拍。
    void WaitForNextPoll();
    /* HandleCommand 在信令任务内执行一条主任务命令。 */
    void HandleCommand(Command command);

    /* 以下端口对象生命周期与协议一致，先于运行时构造。 */
    ur::device::UrSignalingMqttPort mqtt_port_;
    ur::device::UrSignalingUdpPort udp_port_;
    ur::device::UrSignalingCipher cipher_;
    ur::device::UrSignalingOpus codec_;
    std::unique_ptr<ur::device::UrSignalingPcm> pcm_;
    /* media_ 按构建形态持有 UDP 或 WebRTC 媒体端口；生命周期随会话。 */
    std::unique_ptr<ur::device::PumpedVoiceMediaPort> media_;
#if CONFIG_UR_AI_SIGNALING_H264_UPLINK
    /* video_ 为视频形态的 H264 上行管线；纯音频形态不编译。 */
    std::unique_ptr<ur::device::UrSignalingVideo> video_;
    /* video_starter_ 保存相机管线启动闭包，供人际通话外观共享注入。 */
    std::function<bool()> video_starter_;
#endif
    std::unique_ptr<ur::device::VoiceRuntime> runtime_;

    /* 任务与同步原语；命令队列深度覆盖并发交互入口。 */
    static SignalingProtocol* instance_; /* 构造注册、析构注销的当前实例。 */
    QueueHandle_t commands_ = nullptr;
    EventGroupHandle_t events_ = nullptr;
    TaskHandle_t task_ = nullptr;
    std::atomic<bool> task_running_{false};
    std::atomic<bool> session_active_{false};
    std::atomic<bool> full_duplex_{false};
    /* runtime_started_ 防止重复连接；失败后由事件任务复位。 */
    std::atomic<bool> runtime_started_{false};
    // await_session_create_ 由建连调用线程独占，表示本轮须先等待命令接管屏障。
    bool await_session_create_ = false;
    /* vision_session_requested_ 仅在 OpenVisionChannel 至会话结束期间为真；
     * 传递给下一次 runtime 连接决定 aiVision 三轨，默认设备本地纯语音两轨。 */
    std::atomic<bool> vision_session_requested_{false};
    /* pending_text_ 由应用线程写入、信令任务在 kSendText 消费；单轮覆盖。 */
    std::string pending_text_;
    /* last_voice_state_ 仅用于状态变化日志（信令任务线程内读写）。 */
    ur::device::VoiceState last_voice_state_ = ur::device::VoiceState::Idle;

    /* awaiting_reply_ 表示本轮采集由本地 VAD 句尾主动停下、平台回复尚未起播。
     * 该窗口内"监听态但采集未开"是半双工正常交接，自愈补发会打断本轮回复，
     * 故仅在为假时允许自愈；回复起播/新监听轮/媒体重协商/超时都会清位。
     * 应用任务写、信令任务读，用原子标志避免竞态误判。 */
    std::atomic<bool> awaiting_reply_{false};
    /* awaiting_reply_at_ms_ 等待回复起点，用于等待超时清位（回复丢失兜底）。 */
    std::atomic<std::uint64_t> awaiting_reply_at_ms_{0};
    /* capture_heal_scan_ms_ 上次自愈扫描时刻（约每秒一次的低频扫描节拍）。 */
    std::uint64_t capture_heal_scan_ms_ = 0;
    /* capture_heal_at_ms_ 上次自愈补发时刻，作为最小补发间隔（重复触发安全）。 */
    std::uint64_t capture_heal_at_ms_ = 0;
    /* start_command_pending_ 表示开麦命令已由应用线程排队、尚未被信令任务处理。
     * 自愈扫描据此跳过"命令在途"窗口，避免在 audioStart 受理前重复补发
     * （真机日志出现过 result=0 后 20ms 紧跟 result=5 的重复命令）。 */
    std::atomic<bool> start_command_pending_{false};

    /* playback_playing_ 记录上次播放沿；vad 端点沿用旧协议句尾判定。 */
    bool playback_playing_ = false;
    std::mutex vad_mutex_;  // 应用任务上报VAD边沿，信令任务周期检查句尾，共同保护单调计时。
#if CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
    // Watcher 的实体语音中，弱音可能连续漏检。允许平台优先判句尾，再由设备收尾。
    UrAiVadEndpoint vad_endpoint_{2200000};
#else
    UrAiVadEndpoint vad_endpoint_;  // 其他板型沿用默认静音等待时间。
#endif
    bool vad_auto_stop_ = false;  // 同一互斥保护，仅自动监听允许静音交句，按住说话由本人停止。
    void CheckVadEndpoint();  // 静音边沿之后继续检查截止，只为本轮半双工提交一次停麦。
    /* 运行时重建所需快照；仅在事件线程读取，构造后不可变。 */
    ur::device::MqttConfig runtime_config_{};
    ur::device::VoiceCapability capability_{};
    std::string identity_;
    /* 空闲期常驻订阅标志：humanCall 来电由平台主动投递，Broker 按订阅判定设备
     * 可达；订阅此前只随 AI 会话（VoiceClient）建立，冷启动或 AI 会话缺席时
     * 来电投递被 Broker 拒（nativeDeviceUnavailable 根因）。 */
    std::atomic<bool> idle_down_subscribed_{false};
    /* 当前能力下次续报或失败重试时刻；只由信令任务推进，跨AI/人际状态保留。 */
    std::uint64_t idle_capability_retry_at_ = 0;
    /* 人际通话外观是否已绑定共享端口；仅信令任务线程访问。无 AI 运行时时
     * 也必须提前绑定，否则冷启动来电会在 HandleDownlink 前被丢弃。 */
    bool call_appearance_ready_ = false;
};

#endif /* UR_SIGNALING_PROTOCOL_H */
