/* 按键运行测试夹具：注入真实 HandleToggleChatEvent，仅替换硬件和云端边界。 */
#include <cassert>
#include <atomic>
#include <functional>
#include <string>
#include <vector>
#define CONFIG_UR_AI 1
#define CONFIG_UR_AI_SIGNALING_WEBRTC 1
#define ESP_LOGE(...) ((void)0)

enum State { kDeviceStateIdle, kDeviceStateSpeaking, kDeviceStateListening,
             kDeviceStateNotifying, kDeviceStateActivating, kDeviceStateWifiConfiguring,
             kDeviceStateAudioTesting, kDeviceStateConnecting };
enum ListeningMode { AutoStop };
constexpr int kAbortReasonNone = 0;
// Board 只记录显示取消边界；关闭菜单会话必须释放独立拍照提示。
struct Board {
    int photo_cancels = 0;
    static Board& GetInstance() { static Board board; return board; }
    Board* GetDisplay() { return this; }
    void CancelPhotoStage() { ++photo_cancels; }
};

/* ProtocolStub 不发送 audioDone，复现平台取消回复后没有终态的场景。 */
struct ProtocolStub {
    bool open = true;
    int closes = 0;
    bool IsAudioChannelOpened() { return open; }
    void CloseAudioChannel() { ++closes; open = false; }
    // RequestCloseAudioChannel 模拟非等待式关闭，不销毁协议实例。
    void RequestCloseAudioChannel() { ++closes; open = false; }
};
/* AudioStub 记录尾音清理和语音处理开关，不运行硬件驱动。 */
struct AudioStub {
    bool processing = true;
    int resets = 0;
    void EnableAudioTesting(bool) {}
    void EnableVoiceProcessing(bool enabled) { processing = enabled; }
    void ResetDecoder() { ++resets; }
};
/* Application 保留真实按键函数所需边界，记录取消、清轮和监听启动顺序。 */
struct Application {
    bool human_call_audio_owned_ = false;  // 模拟主任务已接管人际音频。
    std::atomic<bool> interaction_blocked_{false};  // 模拟菜单屏蔽新会话。
    State state = kDeviceStateSpeaking;
    ProtocolStub protocol_instance;
    ProtocolStub* protocol_ = &protocol_instance;
    AudioStub audio_service_;
    std::vector<std::string> events;
    State GetDeviceState() { return state; }
    void SetDeviceState(State next) { state = next; }
    void StopNotification() { state = kDeviceStateIdle; }
    ListeningMode GetDefaultListeningMode() { return AutoStop; }
    void Schedule(std::function<void()> callback) { callback(); }
    void ContinueOpenAudioChannel(ListeningMode mode) { protocol_->open = true; SetListeningMode(mode); }
    void AbortSpeaking(int) { events.push_back("cancel"); }
    void BeginUrAiTurn() { events.push_back("reset_turn"); }
    void SetListeningMode(ListeningMode) { events.push_back("listen"); state = kDeviceStateListening; }
    void HandleToggleChatEvent();
    // EndConversation 注入真实应用实现，验证菜单关闭与既有按键路径兼容。
    void EndConversation();
};

// PRODUCTION_BUTTON_HANDLER

int main() {
    Application app;
    app.HandleToggleChatEvent();
    assert(app.state == kDeviceStateListening);
    assert((app.events == std::vector<std::string>{"cancel", "reset_turn", "listen"}));
    assert(app.audio_service_.resets == 1);
    assert(!app.audio_service_.processing);
    assert(app.protocol_instance.closes == 0);
    // 新一轮监听时再次单击仍按原交互结束会话，不能重复发取消。
    app.HandleToggleChatEvent();
    assert(app.protocol_instance.closes == 1);
    assert(app.events.size() == 3);
    // 空闲按键仍能正常建立通道，保证修复只改变播报中按键。
    app.state = kDeviceStateIdle;
    app.HandleToggleChatEvent();
    assert(app.state == kDeviceStateListening);
    // 菜单打开在连接、监听和播报阶段都结束会话，返回待机且保留协议实例。
    for (auto state : {kDeviceStateConnecting, kDeviceStateListening, kDeviceStateSpeaking}) {
        app.state = state;
        app.protocol_instance.open = true;
        const auto before = app.protocol_instance.closes;
        app.EndConversation();
        assert(app.state == kDeviceStateIdle);
        assert(app.protocol_ == &app.protocol_instance);
        assert(app.protocol_instance.closes == before + 1);
        assert(!app.audio_service_.processing);
    }
    app.state = kDeviceStateWifiConfiguring;
    const auto before = app.protocol_instance.closes;
    app.EndConversation();
    assert(app.protocol_instance.closes == before);
    assert(app.state == kDeviceStateWifiConfiguring);
#if CONFIG_UR_AI_SIGNALING_WEBRTC
    // 人际通话占用音频时，排队迟到的 AI 按键不能改变状态或关闭媒体。
    app.human_call_audio_owned_ = true;
    app.state = kDeviceStateIdle;
    const auto events_before = app.events.size();
    app.HandleToggleChatEvent();
    assert(app.state == kDeviceStateIdle && app.events.size() == events_before);
    app.human_call_audio_owned_ = false;
#endif
    app.interaction_blocked_.store(true);
    app.state = kDeviceStateIdle;
    app.HandleToggleChatEvent();
    assert(app.state == kDeviceStateIdle);
    return 0;
}
