#ifndef _APPLICATION_H_
#define _APPLICATION_H_

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <esp_timer.h>

#include <string>
#include <atomic>
#include <mutex>
#include <deque>
#include <memory>
#include <functional>
#include <cstdint>
#include <vector>

#include "protocol.h"
#include "ota.h"
#include "audio_service.h"
#include "device_state.h"
#include "device_state_machine.h"
#include "notify/notify_player.h"

// Main event bits
#define MAIN_EVENT_SCHEDULE             (1 << 0)
#define MAIN_EVENT_SEND_AUDIO           (1 << 1)
#define MAIN_EVENT_WAKE_WORD_DETECTED   (1 << 2)
#define MAIN_EVENT_ERROR                (1 << 4)
#define MAIN_EVENT_ACTIVATION_DONE      (1 << 5)
#define MAIN_EVENT_CLOCK_TICK           (1 << 6)
#define MAIN_EVENT_NETWORK_CONNECTED    (1 << 7)
#define MAIN_EVENT_NETWORK_DISCONNECTED (1 << 8)
#define MAIN_EVENT_TOGGLE_CHAT          (1 << 9)
#define MAIN_EVENT_START_LISTENING      (1 << 10)
#define MAIN_EVENT_STOP_LISTENING       (1 << 11)
#define MAIN_EVENT_STATE_CHANGED        (1 << 12)
#define MAIN_EVENT_PLAYBACK_DRAINED     (1 << 13)
#define MAIN_EVENT_PLATFORM_OTA_PREPARED (1 << 14)
#define MAIN_EVENT_TAKE_PHOTO           (1 << 15)


enum AecMode {
    kAecOff,
    kAecOnDeviceSide,
    kAecOnServerSide,
};

class Application {
public:
    static Application& GetInstance() {
        static Application instance;
        return instance;
    }
    // Delete copy constructor and assignment operator
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    /**
     * Initialize the application
     * This sets up display, audio, network callbacks, etc.
     * Network connection starts asynchronously.
     */
    void Initialize();

    /**
     * Run the main event loop
     * This function runs in the main task and never returns.
     * It handles all events including network, state changes, and user interactions.
     */
    void Run();

    DeviceState GetDeviceState() const { return state_machine_.GetState(); }
    bool IsVoiceDetected() const { return audio_service_.IsVoiceDetected(); }
    
    /**
     * Request state transition
     * Returns true if transition was successful
     */
    bool SetDeviceState(DeviceState state);

    /**
     * Schedule a callback to be executed in the main task
     */
    void Schedule(std::function<void()>&& callback);

    /* EnsureListeningAudio 幂等自愈入口（任意任务可调用）：设备处于监听态但未真正开麦时，
     * 在主任务重放应用自己的开麦路径（送 audioStart + 打开语音处理 + 唤醒词配置）。
     * 内部状态复核后生效：非监听态/已采集时是空操作，重复触发不会重复上行。 */
    void EnsureListeningAudio();

    /**
     * Alert with status, message, emotion and optional sound
     */
    void Alert(const char* status, const char* message, const char* emotion = "", const std::string_view& sound = "");
    void DismissAlert();

    void AbortSpeaking(AbortReason reason);

    /**
     * Toggle chat state (event-based, thread-safe)
     * Sends MAIN_EVENT_TOGGLE_CHAT to be handled in Run()
     */
    void ToggleChatState();

    /**
     * Start listening (event-based, thread-safe)
     * Sends MAIN_EVENT_START_LISTENING to be handled in Run()
     */
    void StartListening();

    /**
     * Stop listening (event-based, thread-safe)
     * Sends MAIN_EVENT_STOP_LISTENING to be handled in Run()
     */
    void StopListening();

    // EndConversation 在主任务结束语音并回到待机，保留 MQTT 与协议实例供菜单退出后复用。
    void EndConversation();
    // SetInteractionBlocked 由设置界面控制新会话入口，原子标志不改变网络或凭据。
    void SetInteractionBlocked(bool blocked) { interaction_blocked_.store(blocked); }
#if CONFIG_UR_AI_SIGNALING_WEBRTC
    // SetHumanCallAudioOwned 只由应用任务更新，保护通话采集不受旧 AI 事件影响。
    void SetHumanCallAudioOwned(bool owned) { human_call_audio_owned_ = owned; }
#endif

#if CONFIG_UR_AI
    /* GetUrAiUploadUrl 返回当前联犀 AI session 的临时文件上传入口。 */
    std::string GetUrAiUploadUrl();
    /* TakePhoto 请求主任务建立或复用 AI 会话后启动本地双击拍照。 */
    void TakePhoto();
    /* CompleteUrPhotoInput 把 worker 结果安全切回主任务并发送图片输入。 */
    void CompleteUrPhotoInput(int code, const std::string& file_uri);
#endif

#if CONFIG_UR_AI_E2E_TEST
    /* RunUrAiE2eTest执行互斥语音样本；前进/音量控制仅单轮，音量控制须先准备55。 */
    bool RunUrAiE2eTest(int turns = 1, bool take_photo = false, bool control_volume = false,
                        bool tank_forward = false);
    /* RunUrPhotoE2eTest 走正式相机链路，校验现场物品词和完整播放；不模拟实体双击触点。 */
    bool RunUrPhotoE2eTest(const std::string& expected_text);
#endif

    void Reboot();
    void WakeWordInvoke(const std::string& wake_word);
    bool UpgradeFirmware(const std::string& url, const std::string& version = "");
    /* PrepareForPlatformOta 在主任务关闭会话并释放音频任务栈，供联犀 OTA worker 启动前调用。 */
    bool PrepareForPlatformOta();
    /* RecoverFromPlatformOta 在升级启动或下载失败后重建音频任务并恢复待机。 */
    void RecoverFromPlatformOta();
    bool CanEnterSleepMode();
    void SendMcpMessage(const std::string& payload);
    void RegisterMcpBroadcastCallback(std::function<void(const std::string&)> callback);
    void SetAecMode(AecMode mode);
    AecMode GetAecMode() const { return aec_mode_; }
    void PlaySound(const std::string_view& sound);
    AudioService& GetAudioService() { return audio_service_; }
    
    /**
     * Reset protocol resources (thread-safe)
     * Can be called from any task to release resources allocated after network connected
     * This includes closing audio channel, resetting protocol and ota objects
     */
    void ResetProtocol();

private:
    Application();
    ~Application();

    std::mutex mutex_;
    std::deque<std::function<void()>> main_tasks_;
    std::unique_ptr<Protocol> protocol_;
    EventGroupHandle_t event_group_ = nullptr;
    esp_timer_handle_t clock_timer_handle_ = nullptr;
    DeviceStateMachine state_machine_;
    ListeningMode listening_mode_ = kListeningModeAutoStop;
    AecMode aec_mode_ = kAecOff;
    std::string last_error_message_;
    AudioService audio_service_;
    NotifyPlayer notify_player_;
    uint32_t notification_playback_id_ = 0;
    std::unique_ptr<Ota> ota_;

    std::function<void(const std::string&)> mcp_broadcast_callback_;

    bool has_server_time_ = false;
    bool aborted_ = false;
    bool assets_version_checked_ = false;
    bool play_popup_on_listening_ = false;  // Flag to play popup sound after state changes to listening
    bool pending_listening_start_ = false;  // Waiting for playback to drain before starting listening (auto mode)
    /* pending_listening_since_us_ 延迟开麦置位时刻：兑现边沿（播放排空/表情释放）
     * 可能整体丢失，见 Run() 时钟节拍处的电平重试与超时放行。 */
    int64_t pending_listening_since_us_ = 0;
#if CONFIG_UR_AI
    /* 联犀表情在本轮真实音频播放结束前保持，终态乱序时至少可见一个时钟周期。 */
    bool ur_emotion_hold_ = false;
    bool ur_emotion_release_pending_ = false;
    int64_t ur_emotion_displayed_us_ = 0;
#endif
    int clock_ticks_ = 0;
    std::atomic<bool> interaction_blocked_{false};  // 菜单/确认页显示时禁止唤醒与新会话。
#if CONFIG_UR_AI_SIGNALING_WEBRTC
    bool human_call_audio_owned_ = false;  // 主任务持有的人际音频接管状态。
#endif
    TaskHandle_t activation_task_handle_ = nullptr;
#if CONFIG_UR_AI_E2E_TEST
    /* 测试期间丢弃麦克风帧和本地 VAD，只允许固定样本进入真实协议链路。 */
    std::atomic<bool> ur_ai_e2e_running_{false};
    /* ur_ai_e2e_speaking_observed_ 记录下行首帧已驱动界面进入播报状态。 */
    std::atomic<bool> ur_ai_e2e_speaking_observed_{false};
    /* 拍照 worker 已明确失败时让自动测试立即结束，不空等模型超时。 */
    std::atomic<int> ur_photo_e2e_error_{0};
#endif


    // Event handlers
    void HandleStateChangedEvent();
    void HandleToggleChatEvent();
    void HandleStartListeningEvent();
    void HandleStopListeningEvent();
    void HandleNetworkConnectedEvent();
    void HandleNetworkDisconnectedEvent();
    void HandleActivationDoneEvent();
    void HandleWakeWordDetectedEvent();
#if CONFIG_UR_AI_SIGNALING
    /* EnsureSignalingStarted 幂等启动统一信令任务：Start 依赖的 MQTT 设备身份
     * 由连接任务稍后填充，启动即调可能失败（冷启动竞态，失败后 humanCall 能力
     * 永不上报、视频邀请被拒）。网络连接事件与秒级 tick 持续重试直至成功。 */
    void EnsureSignalingStarted();
#endif
#if CONFIG_UR_AI
    /* HandleTakePhotoEvent/ContinueOpenPhotoChannel 串行处理打断、建会话与拍照启动。 */
    void HandleTakePhotoEvent();
    void ContinueOpenPhotoChannel();
    void ShowPhotoUnavailable(int code);
    /* BeginUrAiTurn/ReleaseUrEmotionIfReady 管理表情跨状态切换的显示生命周期。 */
    void BeginUrAiTurn();
    void ReleaseUrEmotionIfReady();
#endif
    void ContinueOpenAudioChannel(ListeningMode mode);
    void BeginWakeWordInvoke(const std::string& wake_word);
    void ContinueWakeWordInvoke(const std::string& wake_word);
    void StartListeningAudio();
    void ConfigureWakeWordForListening();
    void StartNotification(std::string audio_url, std::vector<NotifySubtitle> subtitles);
    void StopNotification();
    void HandleNotificationFinished(uint32_t playback_id, bool success);

    // Activation task (runs in background)
    void ActivationTask();

    // Helper methods
    void CheckAssetsVersion();
    void CheckNewVersion();
    void InitializeProtocol();
    void ShowActivationCode(const std::string& code, const std::string& message);
    void SetListeningMode(ListeningMode mode);
    ListeningMode GetDefaultListeningMode() const;
    
    // State change handler called by state machine
    void OnStateChanged(DeviceState old_state, DeviceState new_state);
};


class TaskPriorityReset {
public:
    TaskPriorityReset(BaseType_t priority) {
        original_priority_ = uxTaskPriorityGet(NULL);
        vTaskPrioritySet(NULL, priority);
    }
    ~TaskPriorityReset() {
        vTaskPrioritySet(NULL, original_priority_);
    }

private:
    BaseType_t original_priority_;
};

#endif // _APPLICATION_H_
