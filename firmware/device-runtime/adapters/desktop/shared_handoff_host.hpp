// 文件说明：Linux共享AI／人工设备正式宿主，唯一连接与真实输入输出票据串联，不使用独立预检替代业务。
#pragma once
#include <functional>
#include <memory>

#include "browser_pcm.hpp"
#include "ur/device/action_engine.hpp"
#include "ur/device/ai_human_handoff.hpp"
#include "ur/device/context_snapshot.hpp"
#include "ur/device/shared_device_connection.hpp"
#include "ur/device/udp_audio.hpp"
#include "ur/device/voice_runtime.hpp"

namespace ur::device {
// SharedHandoffHostConfig
// 固定已鉴权身份和native启动意图；截止不能由页面或平台邀请续期。
struct SharedHandoffHostConfig {
    std::string identity, instance; // 正式签名身份及单次连接实例，不发送到页面。
    MqttConfig mqtt;                // 原唯一连接凭据，仅交给原MQTT端口。
    std::uint64_t boot = 0, deadline = 0,
                  expires_at = 0;        // 独立native代次和双时钟截止。
    std::function<AudioTiming()> timing; // 原实际UDP计数快照，不用空值伪造完整音频。
    std::function<std::optional<AudioTiming>(std::uint64_t)>
        stopped_timing;              // 原停播代次一次性冻结统计。
    bool human_receiver = false;     // native显式纯Human入口，不创建AI；显式启用源能力时可主动拨号。
    bool context_diagnostic = false; // 仅native严格开关开启私有留存闭集观察，不授分享或读取权限。
    bool temporary_calls = false;    // native显式启用普通人际通话暂借资源；默认原永久escalate不变。
    bool udp_reply_start_receipt =
        false;  // native显式选定软件安装合同，真实PCMReady后才可声明；非扬声器事实。
    bool half_duplex_only = false; // 仅收窄可协商模式；实际设备AEC能力不得因该策略被伪报或关闭。
    bool human_endpoint_source = false;  // 显式源请求入口；不绕过平台完整Focus换端围栏。
    std::function<std::optional<std::uint64_t>()> last_voice_sent_ms =
        {}; // 原UDP最后有效语音发送时刻；缺失不借网页时间补测。
};
// SharedContextDiagnostic
// 仅原实际留存与本地ACK提交的有界观察，不输出标识、摘要或正文。
struct SharedContextDiagnostic {
    bool retained = false, cleared = false,
         expired = false; // cleared/expired为原留存失效观察的锁存事实。
    unsigned ack_attempts = 0,
             ack_accepted = 0; // 饱和4096；accepted仅本地提交受理，非Broker/对端交付。
};
// SharedHostExitBranch
// 仅标记main实际执行的退出分支，不能用它推断具体网络或驱动原因。
enum class SharedHostExitBranch { Unknown, BridgeInvalid, BridgeClosed, HostPollFalse };
// SharedHostExitReason
// 只记录首个已走到的原Host退出条件；枚举不证明底层故障原因或资源已释放。
enum class SharedHostExitReason {
    Unknown,
    Closed,
    PriorFailure,
    MonotonicDeadline,
    WallDeadline,
    DisposalStopped,
    DisposalDeadline,
    DisposalSourceUnavailable,
    SourceUnavailable,
    InputGateFailed,
    HumanMediaFailed,
    PcmOutputGateFailed,
    DuplexDriverFailed,
    CapabilityEncodingFailed,
    ConnectionStartFailed,
    ConnectionFailed,
    AiClientStartFailed,
    HumanClientStartFailed,
    DelayedFocusFailed,
    VoiceRuntimeFailed,
    VoiceSessionFailed,
    EmitFailed,
    CandidateCloseDriverFailed,
    CandidateSetupFailed,
    SharedFocusRelayRejected,
    HumanFocusReceiptRejected
};
// SharedHandoffHost
// 事件线程组合现有真实端口；页面仅负责原媒体驱动和明确本地操作。
class SharedHandoffHost {
    public:
    using Source = std::function<bool()>;                 // 回读唯一已配对浏览器连接是否仍存活。
    using Emit = std::function<bool(const std::string&)>; // 原有界回环桥投递，不是媒体实际回执。
    // 原网络、AI媒体、PCM及动作执行器须比宿主存活更久；构造不连接或声明空闲。
    SharedHandoffHost(MqttPort& network, PumpedVoiceMediaPort& media, BrowserPcm& pcm,
                      SharedHandoffHostConfig config, Source paired, Emit emit,
                      ActionEngine* actions = nullptr);
    ~SharedHandoffHost();
    SharedHandoffHost(const SharedHandoffHost&) = delete;
    SharedHandoffHost& operator=(const SharedHandoffHost&) = delete;
    // receive_local
    // 只消费已认证本地桥报文；解析成功不代替平台授权或实际设备门控。
    bool receive_local(std::string_view body, std::uint64_t now, std::uint64_t wall);
    // poll 推进同一物理连接与三屏障；不启动第二采集源、不默认恢复未知租约。
    bool poll(std::uint64_t now, std::uint64_t wall);
    void close(std::uint64_t now); // 只在整个宿主退出时关闭共享网络，AI移交不调用此入口。
    SharedConnectionState connection_state() const;
    VoiceState voice_state() const;
    AiHandoffState handoff_state() const;
    bool ai_owned() const; // 原完整实际组仍由AI持有，非平台Established推断。
    // context_snapshot
    // 只读实际Human私有留存，不投影到页面/日志；终态、断连或expire后指针失效。
    const ContextSnapshot* context_snapshot() const;
    // context_diagnostic
    // 默认关闭返回空事实；开启只读本腿原Inbox及实际ACK提交统计。
    SharedContextDiagnostic context_diagnostic() const;
    // context_diagnostic_line
    // 返回闭集JSON单行；关闭返回空串，禁止正文/ID/hash进入native日志。
    std::string context_diagnostic_line() const;
    // startup_diagnostic_line
    // 仅显式诊断开启时返回六个原启动事实的固定JSON，不输出身份或授权正文。
    std::string startup_diagnostic_line() const;
    // exit_diagnostic_line
    // 默认关闭；只读原main分支、退出布尔及首个原Host失败条件，不消费业务事件。
    std::string exit_diagnostic_line(SharedHostExitBranch branch, bool failed) const;
    // human_media_failure_diagnostic_line
    // 默认关闭；只读已锁定首因，不读取业务端口。
    std::string human_media_failure_diagnostic_line() const;
    // ai_runtime_failure_diagnostic_line 默认关闭；只读AI运行时已锁存的固定失败分类，不读取消息正文。
    std::string ai_runtime_failure_diagnostic_line() const;
    // duplex_driver_failure_diagnostic_line 默认空；只读Driver原锁存首因，不再次调用驱动或端口。
    std::string duplex_driver_failure_diagnostic_line() const;
    // human_signal_diagnostic_line 默认空；只读现有Human状态和首失败，main须在close前投影原阶段。
    std::string human_signal_diagnostic_line() const;
    // human_rejection_diagnostic_line
    // 仅显式开启且已保存首拒帧时输出固定五字段。
    std::string human_rejection_diagnostic_line() const;

    private:
    struct Impl; // 桌面组合细节不进入可移植协议核心。
    std::unique_ptr<Impl> impl_;
};
} // namespace ur::device
