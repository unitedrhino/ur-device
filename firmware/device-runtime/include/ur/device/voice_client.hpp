// 统一 UDP 语音客户端：控制报文由单事件线程推进，媒体驱动不直接改变协议状态。
#pragma once
#include "ur/device/ai_endpoint_focus.hpp"
#include "ur/device/content_wire.hpp"
#include "ur/device/focus_scope_diagnostic.hpp"
#include "ur/device/requests.hpp"
#include "ur/device/voice_wire.hpp"
#include <memory>
#include <optional>
#include <vector>

namespace ur::device {
class FocusEndpoint;  // 可选显式焦点能力提供者，寿命必须长于语音客户端。
class AIControlRecovery;  // 独立短断连状态，仅显式同步能力分配，不能复用视觉配置存授权。
// VoiceVisionRequestResult区分驱动受理与实际完成；默认Unsupported不能被当作画面已发布。
enum class VoiceVisionRequestResult { Unsupported, Busy, Accepted };
// VoiceVisionState只来自原驱动实际事实；Failed不得携伪active/stopped成功字段。
enum class VoiceVisionState { Active, Stopped, Failed };
// VoiceVisionFailure是本地有限故障类别；网络回执由核心映射已有公共错误，不透传自由文本。
enum class VoiceVisionFailure { None, Unsupported, Busy, MediaFailed, Timeout, Cancelled };
// VoiceVisionControl绑定同一AI会话原摄像头/轨/代次；generation仅本地关联，不暴露MQTTtoken给网页。
struct VoiceVisionControl {
    std::uint64_t generation = 0; // 本客户端生命周期内单调非零，本地事实不得跨会话重用。
    std::string session_id;       // 原VoiceClient当前会话，不由驱动猜测或自报。
    std::string camera_id;        // 已冻结的专用Profile源；不从IPC或附件借许可。
    std::string track_id;         // 原selected三轨中的唯一sendonly画面轨。
    std::uint64_t media_revision = 0; // 原已建立媒体代次，不是摄像头配置版本。
    VoiceVisionOperation operation = VoiceVisionOperation::Stop; // 未授权时默认关闭。
};
// VoiceVisionFact仅确认同一受理generation的真实结果；驱动先核原完整scope再交事件线程。
struct VoiceVisionFact {
    std::uint64_t generation = 0; // 原已受理控制，旧/未来事实不得作用当前意图。
    VoiceVisionState state = VoiceVisionState::Failed; // Active须实际发布，Stopped须真实退出。
    VoiceVisionFailure failure = VoiceVisionFailure::None; // 成功为None，失败必须有限非None类别。
};
// VoiceMediaPort 隔离媒体参数安装和真实双向探测；configure
// 不能启动麦克风或扬声器。
class VoiceMediaPort : public AudioPort {
public:
    // configure 安装已授权的短期通道；失败须自行回收部分创建的资源。
    virtual bool configure(const VoiceMedia& media) = 0;
    // vision_profile借用不可变实际配置，须持续有效到原会话终态；默认无摄像头能力。
    virtual const VoiceVisionProfile* vision_profile() const { return nullptr; }
    // call_video_profile 描述人际通话的原相机；旧设备默认复用已装配的视觉描述。
    virtual const VoiceVisionProfile* call_video_profile() const { return vision_profile(); }
    // configure_vision仅安装原三轨和paused视频，不取帧、不签本人同意；失败回收部分资源。
    virtual bool configure_vision(const VoiceMedia&, const VoiceVisionProfile&) { return false; }
    // begin_vision_control只受理原控制；Stop须抢占未完成Publish，Accepted不等于实际active/stopped。
    virtual VoiceVisionRequestResult begin_vision_control(const VoiceVisionControl&) {
        return VoiceVisionRequestResult::Unsupported;
    }
    // take_vision_fact交接已实际完成原事实；不能以队列受理、SDP/连接成功替代摄像头结果。
    virtual std::optional<VoiceVisionFact> take_vision_fact() { return std::nullopt; }
    // cancel_vision_control立即撤销原generation意图并回收迟到取流；不是Stopped成功证明。
    // 原核心10秒截止不续，关闭/超时调用；不能清除后继generation或其他音视频腿。
    virtual void cancel_vision_control(std::uint64_t) {}
    // udp_reply_start_receipt_ready只声明prepare_reply和start_playback同步完成真实安装，默认不支持。
    virtual bool udp_reply_start_receipt_ready() const { return false; }
    // next_input_sequence 在开始新输入前返回下一上行序号，0代表不可继续。
    virtual std::uint32_t next_input_sequence() const = 0;
    // prepare_reply 安装新回复专属 nonce；旧回复 nonce 不得重新激活。
    virtual bool prepare_reply(const std::array<std::uint8_t, 8>& nonce,
                               std::uint32_t first_sequence) = 0;
    // finish_reply 记录实际末包序号，驱动仍须等待队列排空或丢包截止时间。
    virtual bool finish_reply(std::uint32_t last_sequence) = 0;
    // take_offer 返回WebRTC驱动完成候选收集后的SDP；UDP没有此阶段。
    virtual std::optional<std::string> take_offer() { return std::nullopt; }
    // apply_answer 将云端answer交给实际Peer；成功接纳不等于媒体已连通。
    virtual bool apply_answer(std::string_view) { return false; }
    // begin_recovery只创建原会话下一代Peer；默认不支持，不从旧configure猜测恢复能力。
    virtual bool begin_recovery(std::uint64_t) { return false; }
    // ready只声明驱动已实际验证的双向连通（UDP探测或WebRTC ICE）；默认不声明，
    // 恢复重发不得凭configure、SDP应答或SDP文本推断连通。
    virtual bool ready() const { return false; }
    // arm_capture 提前打开采集队列但暂停上行（WebRTC 把 audioStart 往返移出开麦
    // 关键路径，ACK 前的语音帧先缓冲）；默认不支持，返回 false，调用方在 ACK 后
    // 仍走 start_capture 的旧路径。
    virtual bool arm_capture(std::string_view) { return false; }
    // start_capture_after_ack 在 audioStart 成功 ACK 后解除暂停并按序发送 arm 期间
    // 缓冲的帧；默认直接复用 start_capture，UDP/旧驱动行为不变。
    virtual bool start_capture_after_ack(std::string_view input_id) {
        return start_capture(input_id);
    }
    // disarm_capture 撤销提前武装并清空缓冲（ACK 失败/暂停/关闭）；默认空操作。
    virtual void disarm_capture() {}
    // capture_armed 报告提前武装状态，避免 ACK 等待期被误判为"未开麦"而重复补发。
    virtual bool capture_armed() const { return false; }
    // close 幂等关闭本会话全部媒体资源；新的 configure 可创建新通道。
    virtual void close() = 0;
};
// PumpedVoiceMediaPort
// 将异步媒体推进接入设备事件线程；驱动不能直接重入会话状态机。
class PumpedVoiceMediaPort : public VoiceMediaPort {
public:
    // poll 非阻塞推进探测及双向收发，失败由上层统一关闭控制/媒体资源。
    virtual bool poll(std::uint64_t now) = 0;
    // ready 表示当前媒体的双向连通已经验证，不等于播放完成。
    virtual bool ready() const = 0;
    // recovery_needed仅报告原已连通WebRTC暂断；永久错误仍由poll返回失败。
    virtual bool recovery_needed() const { return false; }
    // drained_epoch 只回报实际完成的本地播放代次，不能用网络结束事件代替。
    // 契约：媒体实现必须在播放完成时主动上报该值（播放缓冲空即可），禁止要求
    // 上层先调用 stop_playback——上层复位 playing 恰依赖本值，条件互锁会导致
    // 回复播完永远无法收尾（Watcher 真机"UI 卡说话中"根因，勿复制该反模式）。
    virtual std::optional<std::uint64_t> drained_epoch() const = 0;
};
// VoiceState 区分 MQTT 可用、媒体已分配与真正建立，避免握手成功被视为可发声。
enum class VoiceState {
    Idle,
    Reporting,
    Creating,
    Negotiating,
    Established,
    Recovering,
    Ending,
    Failed
};
// IncomingMessage 将已接收 Topic/正文组合，避免调用处两个字符串参数传反。
struct IncomingMessage {
    std::string_view topic;  // 完整已订阅 Topic。
    std::string_view body;   // 原始 JSON，仅在 receive 调用期间借用。
};
// VoiceRejection
// 仅保留已关联请求的方法、状态码及能力基线403固定原因；不暴露服务端正文、标识或媒体凭据。
struct VoiceRejection {
    std::string method;  // 本端实际发送的方法，不使用云端自由文本。
    int code = 0;        // 云端明确返回的非成功状态码，不用于表示本地驱动故障。
    std::string
        reason;  // 仅capabilityReport403的focusDeviceUnavailable/requestDenied，其它原因保持空。
};
// VoiceFailureCause 仅表示客户端首次真实失败分支；None是未观察，未知入口不猜测首因。
enum class VoiceFailureCause : std::uint8_t {
    None,
    Unknown,
    SessionError,
    RequestRejected,
    RequestsTimeout,
    ActiveScopeLost,
    CandidateClock,
    CandidateEstablishDeadline,
    CandidateRecoveryDeadline,
    VisionProfileInvalid,
    MediaRecoveryDeadline,
    NegotiatingDeadline,
    EndpointPrepareDeadline,
    EndpointPrepareClock,
    EndpointPrepareDriver,
    EndpointPrepareDispatch
};
// VoiceFailureMethod 只投影本端固定方法；未知方法不保存原字符串、身份或参数。
enum class VoiceFailureMethod : std::uint8_t {
    None,
    Unknown,
    CapabilityReport,
    SessionCreate,
    SessionAccept,
    SessionReject,
    MediaState,
    MediaOffer,
    AudioStart,
    AudioStop,
    RespCancel,
    SessionEnd,
    InputSend,
    SessionSync,
    EndpointControl
};
// VoiceFailureDiagnostic 不包含任何身份或正文，仅供可信本机读取已发生的失败旁证。
struct VoiceFailureDiagnostic {
    VoiceFailureCause cause =
        VoiceFailureCause::Unknown;  // 首次实际失败路径，未细分入口为Unknown。
    VoiceFailureMethod method = VoiceFailureMethod::None;  // 仅请求拒绝或请求超时携固定方法。
};
// voice_failure_method 只分类本端固定方法；未知名称仅返回Unknown，不保留调用者字节。
VoiceFailureMethod voice_failure_method(std::string_view method);
// AIEndpointPhase是候选端本地阶段，不包含源腿/接续/设备身份，不以Ready代表接续已提交。
enum class AIEndpointPhase {
    Disabled, Listening, Invited, Accepting, Preparing, AwaitingCommit,
    AwaitingFocus, Active, Ending, Failed
};
// VoiceClient负责设备发起的AI语音或None文字会话；共享请求与生命周期，文字不装配媒体状态机。
class VoiceClient {
public:
    // up/down 必须是同一设备路由；instance_id
    // 由平台提供每次进程启动唯一的随机标识。
    VoiceClient(PublishPort& publisher, VoiceMediaPort& media, std::string up_topic,
                std::string down_topic, VoiceCapability capability, std::string instance_id,
                FocusEndpoint* focus = nullptr);
    // 纯文字构造只借用发布器；capability必须为None，不创建或借用空媒体驱动。
    VoiceClient(PublishPort& publisher, std::string up_topic, std::string down_topic,
                VoiceCapability capability, std::string instance_id);
    ~VoiceClient();
    // enable_failure_diagnostics 默认关闭；只能在失败前开启，不能由清理或网络消息补造。
    void enable_failure_diagnostics();
    // failure_diagnostic 只读首次锁存事实，不调用时钟/驱动或改变原请求和会话状态。
    std::optional<VoiceFailureDiagnostic> failure_diagnostic() const;
    // scope_failure
    // 只读候选Active原整组gate首次拒绝，不追加权限、时钟或驱动查询。
    std::optional<ScopeFailureCode> scope_failure() const;
    VoiceClient(const VoiceClient&) = delete;
    VoiceClient& operator=(const VoiceClient&) = delete;
    // connect 由适配器在订阅确认后调用；先报告能力，收到确认才请求会话。
    Result connect(std::uint64_t now);
    // connect_listening只报告显式接续能力并等平台邀请，不自动sessionCreate或配置媒体。
    Result connect_listening(std::uint64_t now);
    // accept_endpoint_invite只接真实本地用户动作；完整邀请/期限/能力仍由核心复核。
    Result accept_endpoint_invite(std::uint64_t now, std::uint64_t wall);
    // reject_endpoint_invite使用AI候选sessionReject/userRejected，不自行取消源模型或改变人际拒接。
    Result reject_endpoint_invite(std::uint64_t now, std::uint64_t wall);
    // endpoint_phase仅供可信宿主投影固定枚举，不返回候选、源会话或handoff标识。
    AIEndpointPhase endpoint_phase() const;
    // candidate_focus_scope仅在本地接听/实际媒体就绪/平台双提交事实齐全时借出完整候选scope。
    // 不是Established或采播许可，宿主必须等待原FocusEngine整组实际取得及ACK交付。
    std::optional<AIEndpointFocusScope> candidate_focus_scope() const;
    // connect_shared只供已确认统一能力快照的可信宿主；不重报能力，只创建本端AI会话。
    // 能力失效由宿主重新协商，本客户端不能擅自覆盖其他腿的已确认能力。
    Result connect_shared(std::uint64_t now);
    // receive 严格验证下行路由和关联；陈旧/重复报文不能启动媒体。
    Result receive(IncomingMessage message, std::uint64_t now);
    // receive的双时钟入口核有截止的控制快照/候选邀请；普通旧报文仍可使用原二参数接口。
    Result receive(IncomingMessage message, std::uint64_t now, std::uint64_t wall);
    // control_disconnected 保留原业务会话/水位并停两向；不复用旧输入、不重建模型。
    Result control_disconnected(std::uint64_t now, std::uint64_t last_message);
    // control_reconnected 仅实际新连接完整SUBACK后调用；先重报当前能力再请求原sessionSync。
    Result control_reconnected(std::uint64_t now);
    // control_recovering 区分MQTT屏障与WebRTC媒体重协商，不能在控制断连时自动创建新Peer。
    bool control_recovering() const;
    // control_sync_enabled 仅平台创建应答确认完整公开绑定后为真，不从本地能力意愿猜测。
    bool control_sync_enabled() const;
    // control_message_watermark 只有成功完整快照才能更新宿主MSG水位；0也是合法已确认水位。
    std::uint64_t control_message_watermark() const;
    // control_endpoint_epoch 仅返回已成功安装的公开代次，供本机宿主展示恢复事实，不是授权输入。
    std::uint64_t control_endpoint_epoch() const;
    // control_original_session_preserved 核当前业务ID与初建恢复scope相同，不以Peer或公开代次猜测。
    bool control_original_session_preserved() const;
    // media_ready 仅由驱动确认双向 UDP 探测完成后调用，不接受单纯 socket
    // 创建成功。
    Result media_ready(std::uint64_t now);
    // media_offer_ready 每个WebRTC媒体代次只发送一次完整SDP；恢复期单代未连通时
    // 由tick重建下一代补发，本代不重发。
    Result media_offer_ready(std::string_view sdp, std::uint64_t now);
    // media_disconnected仅暂停原WebRTC媒体；平台确认断连后才创建下一代Peer，首次25秒不续期。
    Result media_disconnected(std::uint64_t now);
    // start_input 生成新输入段，等待对应成功 ACK 后才允许真实采集。
    Result start_input(std::uint64_t now);
    // stop_input 立即停止本地采集，并使迟到的 start ACK 无法重新打开麦克风。
    Result stop_input(std::uint64_t now);
    // send_text 仅在已协商文字且暂停采集时提交；ACK确认受理，不代表模型已回复。
    Result send_text(std::string_view text, bool audio, std::uint64_t now);
    // send_content 只提交本会话已协商的有序文字/Ready附件引用，复用原请求关联、超时和取消。
    // 不上传原件、不扩大模态，不在采集或本地播放中绕过原输入准入。
    Result send_content(const std::vector<InputContentPart>& parts, bool audio, std::uint64_t now);
    // supports_content 只供原事件线程读取平台已选交集，不授素材权限或跨线程读取许可。
    // worker启动前由宿主固定原session/连接，完成后仍须在事件线程调用send_content再次核验。
    bool supports_content(InputContentKind kind) const;
    // receive_content只关联None当前input/response的已解码内容；终态完成前拒绝第二轮，不能授权工具执行。
    Result receive_content(const ContentEvent& content);
    // text_input_id保留原接口名，返回最近一次本地离散输入ID，供失败ACK关联，不猜测云端错误正文。
    const std::string& text_input_id() const { return text_input_id_; }
    // response_id只供已接纳本地取消时撤销原回复；音频播放目标不能作为工具执行授权来源。
    const std::string& response_id() const { return response_id_; }
    // text_input_pending只报告本轮文字等待原回复终态或取消ACK，供宿主有界等待，不代表音频状态。
    bool text_input_pending() const { return text_pending_; }
    // transport仅在会话创建成功后代表平台选择；None不具备媒体Ready或交互模式。
    VoiceTransport transport() const { return selected_.transport; }
    // vision_session 仅报告平台在sessionCreated里确认的视觉三轨；Established前不代表请求意图。
    bool vision_session() const { return selected_.ai_vision; }
    // request_vision_session 在连接前显式请求aiVision三轨会话；能力上报保持完整，缺省纯语音两轨。
    void request_vision_session(bool requested) { vision_requested_ = requested; }
    // cancel_response 立即停止当前本地输出，远端命令仍需独立确认。
    Result cancel_response(std::uint64_t now);
    // end 先释放本地媒体，保留应答关联直到 sessionEnded 或超时。
    Result end(std::uint64_t now);
    // tick 处理重试和有界握手；未知执行结果采取关闭媒体的安全状态。
    void tick(std::uint64_t now);
    // tick双时钟仅为显式候选端继续核固定期限和真实焦点许可；旧单时钟入口不伪造Unix时间。
    void tick(std::uint64_t now, std::uint64_t wall);
    // disconnected 不自动重放旧会话；重新联网必须重新报告能力并创建新会话。
    void disconnected(std::uint64_t now);
    // playback_drained 只能来自真实驱动排空回调，而不是网络音频结束事件。
    Result playback_drained(std::uint64_t epoch) {
        return session_ ? session_->playback_drained(epoch) : Result::Unsupported;
    }
    VoiceState state() const { return state_; }
    // media_negotiating只报告本端Offer阶段，不授予采集或输出许可。
    bool media_negotiating() const {
        return state_ == VoiceState::Negotiating ||
               (state_ == VoiceState::Recovering && !control_recovering() &&
                recovery_.disconnected_acked);
    }
    // mode仅在Established时代表平台已协商结果，不从设备能力或用户意愿推断。
    Mode mode() const { return selected_.mode; }
    // interrupted_playbacks 只计云端定向中断实际停止的本地播放，不计重复或已排空回复。
    std::uint64_t interrupted_playbacks() const { return interrupted_playbacks_; }
    bool capturing() const {
        // 提前武装的 WebRTC 采集同样算"已开麦"：audioStart ACK 等待期的自愈扫描
        // 不得再补发一次开始命令（真机曾出现 result=0 后紧跟 result=5 的重复）。
        return (session_ && session_->capturing()) || (media_ && media_->capture_armed());
    }
    bool playing() const { return session_ && session_->playing(); }
    std::uint64_t playback_epoch() const { return session_ ? session_->playback_epoch() : 0; }
    const std::string& session_id() const { return session_id_; }
    // last_rejection 在资源关闭后仍可用于诊断；下次合法建连开始时清除。
    const std::optional<VoiceRejection>& last_rejection() const { return rejection_; }

private:
    friend class VoiceClientOptionalTestPort;  // 专用故障注入fixture，不开放生产调用方法。
    // reply_start_admission只读原状态/回复/播放/nonce条件；返回原Stale/Busy/Invalid，不配置驱动或续截止。
    Result reply_start_admission(const VoiceEvent& event) const;
    friend class VoiceRuntime;  // 仅原运行时可将已校验内容scope用于尚未发声的定向取消。
    // cancel_content_response 不授予播放资格；目标必须由Runtime原内容水位和会话核验。
    Result cancel_content_response(std::string_view response, std::uint64_t now);
    friend class VoiceRuntime;  // 共享运行时使用同一个可空媒体装配入口，不公开自由指针构造。
    // 共享构造只有实际音频端口时才装配Session；None路径始终没有音频对象。
    VoiceClient(PublishPort& publisher, VoiceMediaPort* media, std::string up_topic,
                std::string down_topic, VoiceCapability capability, std::string instance_id,
                FocusEndpoint* focus);
    // capability_report 所有新token的完整能力均经同一焦点装配，恢复不能丢掉完整资源组。
    WireResult capability_report(std::string token, Envelope& out) const;
    // connect_mode共用身份/能力校验，shared只跳过宿主已完成的能力发布，不跳过会话或媒体ACK。
    Result connect_mode(std::uint64_t now, bool shared);
    // Pending 仅保存驱动副作用需要的关联字段；正文和超时保存在 Requests 中。
    struct Pending {
        std::string token;   // 本客户端生成的唯一 token。
        std::string method;  // 原请求方法。
        std::string object;  // inputId/respId 或空值。
    };
    // send 在有界请求缓存接纳后登记关联，重试不产生新 token。
    bool send(const Envelope& envelope, std::string object, std::uint64_t now);
    // control 为当前会话创建关联命令，不直接改变设备采集或播放。
    bool control(std::string method, std::string object, std::uint64_t now);
    // next_id 在对象生命周期内单调唯一；耗尽时拒绝继续发送。
    std::string next_id();
    // acknowledge 先校验字段再消费在途请求，防止错误 ACK 抢占正确 ACK。
    Result acknowledge(const VoiceEvent& event, std::uint64_t now);
    // establish 只有本地媒体确认、平台确认和已建立事件都到达才允许音频。
    Result establish();
    // endpoint_receive只隔离显式候选/接续事实；空值交回原语音方法，不复制音频状态机。
    std::optional<Result> endpoint_receive(IncomingMessage message, std::uint64_t now,
                                           std::uint64_t wall);
    // endpoint_tick核原候选截止/双提交/真实焦点，不能把收到事件或prepare就绪当实际许可。
    void endpoint_tick(std::uint64_t now, std::uint64_t wall);
    // endpoint_waiting保留已报能力的监听连接，避免空业务ID被原运行时当成已结束会话。
    bool endpoint_waiting() const;
    // endpoint_establish_gate只在候选双事实与原完整focus实际许可齐全时允许原Session::open。
    bool endpoint_establish_gate(bool observe_failure = false) const;
    // endpoint_establish_gate 显式接收当前单调/Unix毫秒，旧快照不用于新的源意图权限检查。
    bool endpoint_establish_gate(std::uint64_t now, std::uint64_t wall,
                                 bool observe_failure = false) const;
    // endpoint_control_rebinding保存目标恢复的独立15秒屏障，快照确认不等于新整组取得。
    bool endpoint_control_rebinding() const;
    // endpoint_control_baseline_ready仅当前新socket真实基线的原能力ACK后为真。
    bool endpoint_control_baseline_ready() const;
    // report_endpoint_control_baseline报告原formal release后的实际idle；不重建会话或自动输入。
    Result report_endpoint_control_baseline(const FocusIdleObservation& idle, std::uint64_t now);
    // recovery_expired核固定单调截止；回执、驱动事件和用户控制不能在tick前越过截止。
    bool recovery_expired(std::uint64_t now);
    // receive_control_snapshot 只处理原控制恢复应答，整批校验后替换水位，不执行焦点恢复。
    Result receive_control_snapshot(IncomingMessage message, std::uint64_t now, std::uint64_t wall);
    // vision_profile_current核原创建绑定；Profile改变/失效不得沿旧会话换摄像头。
    bool vision_profile_current() const;
    // vision_control仅受理原已建立视觉轨；有界历史重放不重开媒体，Stop可抢占待发布。
    Result vision_control(const VoiceVisionCommand& command, std::uint64_t now);
    // vision_tick先收录合法原事实再核固定截止；背压保留原字节，不重执行驱动。
    void vision_tick(std::uint64_t now);
    // vision_cancel撤销原未完成控制，保留失败历史；不伪报摄像头已经退出。
    void vision_cancel();
    // reply_receipt_tick只重发已实际安装的原字节，固定2秒不续期、不用Requests产生ACK-of-ACK。
    void reply_receipt_tick(std::uint64_t now);
    struct UDPReplyReceiptState;  // selected-only的单当前事实、单pending及512个保守拒绝摘要。
    std::unique_ptr<UDPReplyReceiptState> udp_reply_receipts_;  // 普通/未selected会话不分配。
    // release 统一处理断网、驱动失败和结束，不将未知结果误报成功。
    void release(VoiceState state, std::uint64_t now,
                 VoiceFailureCause cause = VoiceFailureCause::Unknown,
                 VoiceFailureMethod method = VoiceFailureMethod::None);
    // 私有字段按对齐分组以减少嵌入式对象间隙；不序列化内存，所有消费者须一同重编译。
    PublishPort& publisher_;                   // 唯一原发布器，用于校验焦点不能另建连接。
    FocusEndpoint* focus_;                     // 可选共享端点；没有实际装配时不广告焦点能力。
    VoiceMediaPort* media_;                    // 音频会话借用实际端口；None必须为空。
    std::uint64_t stream_watermark_ = 0;  // None内容水位或音频Peer代次；两种会话互斥，关闭清零。
    std::uint64_t counter_ = 0;                // 跨重连递增的请求标识。
    std::uint64_t interrupted_playbacks_ = 0;  // 生命周期内实际应用的远端播放中断次数。
    std::uint64_t event_seq_ = 0;              // 当前会话信令事件水位。
    std::uint64_t phase_started_ = 0;          // 媒体协商最长等待起点。
    std::uint64_t offer_sent_at_ = 0;          // 恢复期本代offer发出时刻；0表示尚无在途代次。
    std::optional<std::uint64_t>
        phase_observed_;  // Reporting退避起点或Recovering最近单调观察；状态互斥。
    std::vector<Pending> pending_;                           // 最多 32 条在途副作用关联。
    Requests requests_;                                      // 重试及终态保留。
    std::string up_topic_;                                   // 唯一发布路由。
    std::string down_topic_;                                 // 唯一接收路由。
    std::string content_up_;                   // 同设备message上行，不能用于音频控制。
    std::string content_down_;                 // 同设备message回执路由。
    std::string instance_id_;                  // 进程重启不能重复十分钟幂等窗口内的 token。
    std::string session_id_;                   // 当前平台会话，不接受报文自报设备身份。
    std::string input_id_;                     // 最近一次本地输入请求。
    std::string text_input_id_;                // 当前/最近一次离散输入，独立于麦克风段。
    std::string response_id_;                  // 当前/最近一次回复。
    VoiceCapability capability_;               // 本机真实能力快照。
    std::optional<VoiceRejection> rejection_;  // 最近一次经关联校验的拒绝，不保存原始报文。
    std::optional<Session> session_;           // 仅音频装配，保留原跨会话播放代次。
    VoiceMedia selected_;                      // 平台选择的媒体配置。
    struct VisionState; // 视觉专用不可变Profile和最多32条原控制账本；普通语音不分配。
    // vision_仅视觉会话分配独立有界状态，保持VoiceClient指针预算，不复用UDP地址槽。
    std::unique_ptr<VisionState> vision_;
    // control_只为显式sessionSync会话分配；普通语音不分配恢复缓存，原Binding和媒体不搬入此对象。
    std::unique_ptr<AIControlRecovery> control_;
    struct AIEndpointState; // 显式接续独立有界账本，不能借控制恢复/视觉字段保存候选授权。
    std::unique_ptr<AIEndpointState> endpoint_; // 仅启用接续时分配，包含源腿与候选独立水位。
    unsigned capability_conflicts_ = 0;        // 已匹配的能力快照冲突恢复次数，最多两次。
    std::uint32_t input_first_ = 0;            // 本采集段首包边界，停止时区分无音频和已发尾帧。
    VoiceState state_ = VoiceState::Idle;      // 当前协议状态。
    bool shared_connection_ = false;           // 统一能力快照归宿主所有，原AI不自主重报或续许可。
    bool capability_recovered_ = false;  // 每次连接至多一次能力重报，避免配置错误造成重试风暴。
    bool text_pending_ = false;          // None等待当前回复终态；仅定向取消ACK可提前解除输入背压。
    bool want_capture_ = false;          // 用户最新采集意图，优先于迟到 ACK。
    bool vision_requested_ = false;      // 宿主连接前显式请求的aiVision三轨会话；缺省纯语音两轨。
    // RecoveryFlags保存恢复及取消终态事实，压入原尾部空隙，不提高已有嵌入式对象预算。
    struct RecoveryFlags {
        bool input_acked : 1;           // 原audioStart已确认，迟到ACK仍须等新Peer。
        bool disconnected_acked : 1;    // 原暂断获准，才允许下一代Offer。
        bool response_cancel_sent : 1;  // 当前回复已发取消，不制造重复副作用。
        bool none_terminal_pending : 1;  // None取消ACK仅保留原回复唯一终态，不再接纳内容或工具。
        bool focus_suspended : 1;  // 本机原整组suspend退休旧采播；仅新输入成功ACK解除。
        bool failure_diagnostics_enabled : 1;  // 可信本机显式开关，未启用时不保留失败首因。
        bool failure_terminal : 1;             // 首次失败后禁止重新启用补采，关闭不清除原终态事实。
    } recovery_{};
    bool media_reported_ = false;        // 驱动双向探测完成且已发 mediaState。
    bool offer_sent_ = false;            // WebRTC本代次offer已发出；恢复期重建下一代时复位。
    bool answer_applied_ = false;        // answer已交Peer后才允许上报连通。
    bool media_acked_ = false;           // mediaState 成功 ACK。
    bool established_event_ = false;     // 平台 sessionEstablished 事件。
    bool reply_finished_ = false;        // 本回复末包边界仅安装一次，重复终态不得延长截止时间。
    // 普通客户端原因1..11沿高四位原因/低四位方法；12..15只由Runtime保存prepare首因。
    // 本对象原未用的0xC1..0xD7保存ActiveScopeLost固定子码，不增加嵌入式对象或保存身份。
    mutable std::uint8_t failure_diagnostic_ = 0;
    std::uint8_t recovery_offer_retries_ = 0;  // 本恢复期已重建下一代Peer重发的次数，上限1。
};
}  // namespace ur::device
