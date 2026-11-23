/* humanCall 人际通话外观：把 device-runtime 的 HumanCallClient 与本工程媒体适配
 * 收敛到信令任务驱动；板级通话页面经线程安全 Ui* 入口交互，状态与远端视频帧经
 * UiHooks 回调送出。重组件懒构造（首条邀请建腿、结束销毁），空闲不驻留。
 * 仅 WebRTC 形态编译。 */
#ifndef UR_SIGNALING_CALL_H
#define UR_SIGNALING_CALL_H

#include <cstdint>
#include <functional>
#include <string>

#include "sdkconfig.h"
#include "ur_signaling_mqtt_port.h"
#include "ur_signaling_opus.h"
#include "ur_signaling_pcm.h"

#if CONFIG_UR_AI_SIGNALING_WEBRTC

#if CONFIG_UR_AI_SIGNALING_H264_UPLINK
#include "ur_signaling_video.h"
#elif CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO
namespace ur::device { class UrSignalingVideo; }
#endif

class UrSignalingCall {
public:
    /* UiState 为页面视角的粗粒度通话状态，由内部 HumanCallState 归并得到。 */
    enum class UiState {
        Hidden,     /* 无通话。 */
        Ringing,    /* 收到 sessionInvite 等待本地接听/拒接。 */
        Connecting, /* 已接听，协商/媒体连接中。 */
        Active,     /* 通话中。 */
        Ended,      /* 通话结束（短暂展示后页面自行收回）。 */
    };

    /* UiHooks 由板级通话 UI 注册，用于接收状态、轨开关与远端视频帧。
     * on_remote_frame 缓冲仅回调期间有效且在解码任务上下文调用，接收方须拷贝；
     * 空指针表示信令任务要求清除过期或停显画面，不结束通话；
     * 其余回调在信令任务上下文，不得直接做 LVGL 操作。 */
    struct UiHooks {
        std::function<void(UiState)> on_state;
        // on_state_generation 附带该状态所属通话代次，供异步输入绑定原来电。
        std::function<void(UiState, std::uint32_t)> on_state_generation;
        std::function<void(bool mic_paused, bool camera_paused)> on_track_state;
        std::function<void(const std::uint8_t* rgb565, int width, int height)> on_remote_frame;
        // on_call_kind 在状态回调前投递已校验类型；true为语音，兼容未安装此回调的宿主。
        std::function<void(bool)> on_call_kind;
    };

    /* Instance 返回进程级单例；未通话时各入口均为安全空操作。 */
    static UrSignalingCall& Instance();

    /* Attach 绑定共享 MQTT 端口与设备身份；由信令协议在信令任务启动后调用。
     * 内存纪律：只保存轻量指针，不构造通话客户端等重组件——首条 humanCall
     * 邀请到达才建腿，通话归位后销毁（空闲内部 SRAM 不被通话栈占用）。
     * 可重复绑定：runtime 每次重建后重调即可，已存在的通话腿不受影响。 */
    void Attach(ur::device::UrSignalingMqttPort* port, const std::string& identity);

    // Detach 在宿主任务退出前关闭通话并等待算法退出，解除借用的端口、PCM和回调。
    void Detach();
    /* SetAudioBridge 注入共享 PCM 桥与 Opus 编解码端口；与 AI 语音互斥使用，
     * 由信令协议在媒体对象创建后调用（通话期间 AI 会话必然不在采集/播放）。 */
    void SetAudioBridge(ur::device::UrSignalingPcm* pcm, ur::device::UrSignalingOpus* codec);
    /* SetHostBusy 注入宿主占用查询：AI 语音腿存活期间人际来电先请求让位
     * （SetHostYield），让位超时/不受理才回 busy。 */
    void SetHostBusy(std::function<bool()> host_busy);
    /* SetHostYield 注入宿主让位请求：在信令任务上下文同步调用（不得阻塞），
     * 语义为结束当前 AI 语音会话；返回 true 表示已受理（不等于已让位完成，
     * 让位完成以 host_busy 变 false 为准）。 */
    void SetHostYield(std::function<bool()> yield);
#if CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO
    /* SetVideoPipeline 注入上行视频管线与启动闭包（绑定板级相机与帧源）；
     * 未注入时视频来电按能力不足处理（自动回 busy）。 */
    void SetVideoPipeline(ur::device::UrSignalingVideo* video, std::function<bool()> starter);
#endif

    void SetUiHooks(UiHooks hooks);

    /* ---- 以下方法仅允许信令任务调用 ---- */

    /* HandleDownlink 分派一条设备下行信令报文；返回 true 表示属 humanCall 用途并
     * 已消费（调用方不得再交给 AI 语音状态机）。string_view 重载避免宿主钩子内
     * 的额外拷贝；正文仅在调用期间借用。 */
    bool HandleDownlink(const std::string& topic, const std::string& body, std::uint64_t now_ms,
                        std::uint64_t wall_ms);
    bool HandleDownlink(std::string_view topic, std::string_view body, std::uint64_t now_ms,
                        std::uint64_t wall_ms);
    /* Poll 周期推进通话状态机与媒体收发；须在信令任务主循环每轮调用。 */
    void Poll(std::uint64_t now_ms, std::uint64_t wall_ms);
    /* Busy 表示通话占用媒体资源；AI 会话建立前须避让或拒绝。 */
    bool Busy() const;
#if CONFIG_UR_SPARKBOT_VOICE_MODEL_TEST
    // VoiceAnswerGuardActive 跨任务查询试验来电期间是否需要抑制 AI 唤醒。
    bool VoiceAnswerGuardActive() const;
#endif

    /* ---- 以下方法线程安全，任意任务可调（内部转信令任务执行）---- */

    void UiAccept();       /* Ringing 时接听。 */
    void UiReject();       /* Ringing 时拒接。 */
    void UiHangUp();       /* 通话中挂断。 */
    // 带代次的操作绑定状态回调提供的原通话，过期代次在消费时丢弃。
    void UiAccept(std::uint32_t generation);
    void UiReject(std::uint32_t generation);
    void UiHangUp(std::uint32_t generation);
    void UiToggleMic();    /* 切换本端麦克风轨 pause/resume。 */
    void UiToggleCamera(); /* 切换本端摄像头轨 pause/resume。 */

    // Dial 将已解析的产品:设备目标排入信令任务；true只表示受理，接通由状态回调确认。
    bool Dial(const std::string& target, bool voice_only = false);

private:
    UrSignalingCall();
    ~UrSignalingCall();
    UrSignalingCall(const UrSignalingCall&) = delete;
    UrSignalingCall& operator=(const UrSignalingCall&) = delete;

    struct Impl;
    Impl* impl_;
};

#endif /* CONFIG_UR_AI_SIGNALING_WEBRTC */

#endif /* UR_SIGNALING_CALL_H */
