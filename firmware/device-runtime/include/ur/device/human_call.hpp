// 文件说明：可移植的人际语音／视频共用通话状态机；MQTT信令与媒体驱动由平台端口注入。
#pragma once
#include <functional>
#include <array>
#include <memory>
#include <optional>
#include <vector>
#include "ur/device/context_snapshot.hpp"
#include "ur/device/envelope.hpp"
#include "ur/device/endpoint_handoff_client.hpp"
#include "ur/device/mqtt_port.hpp"
#include "ur/device/participant_roster.hpp"

namespace ur::device {
// FocusSubmission 只允许由唯一实际焦点执行端点交付，不能由网络ACK或宿主布尔值拼造。
struct FocusSubmission;
// HumanCallMode 显式选择单语音轨或旧三轨视频，不从邀请或驱动能力自动升级模式。
enum class HumanCallMode { Video, Voice };
// CallVoiceProfile 固定纯语音的Opus格式；只配置候选Peer／编解码，不授予麦克风或扬声器所有权。
struct CallVoiceProfile {
    std::uint32_t sample_rate = 48000;     // Opus输入固定48kHz，与能力报告一致。
    std::uint16_t frame_duration_ms = 20;  // 固定20毫秒，不能借媒体协商改帧长。
    std::uint8_t channels = 1;             // 单声道，不从默认视频驱动推断支持。
};
// CallUDPAVParameters 由核心严格核验的本次UDP参数；不能从UI传入或跨会话保存。
struct CallUDPAVParameters {
 std::string server; // 显式IPv4媒体地址。
 std::uint16_t port = 0; // 已授权的非零端口。
 std::array<std::uint8_t,16> key{}; // 一次性AES密钥，不得写日志。
 std::array<std::uint8_t,8> up{}, down{}; // 独立且不同方向nonce。
};
// CallMediaPort 将实际摄像头、麦克风、扬声器与WebRTC端点留给平台驱动。
class CallMediaPort {
public:
    virtual ~CallMediaPort() = default;
    // available 判断整组驱动资源是否可预约，旧独立驱动默认保持原行为。
    virtual bool available() const { return true; }
    // prepare 在接听请求前预约原资源，ACK期间不能被IPC抢占；失败须回复忙线。
    virtual bool prepare() { return true; }
    // begin 创建本地媒体意图；SDP候选收集和权限仍异步完成。
    virtual bool begin() = 0;
    // configure_udp_av 只在本人接听后的固定协商中选择UDP；旧HAL默认拒绝。
    virtual bool configure_udp_av(bool enabled) { return !enabled; }
    // apply_udp_av_answer 接受核心已校验的一次性参数，成功安装不代表媒体连通。
    virtual bool apply_udp_av_answer(const CallUDPAVParameters&) { return false; }
    // video_receive_supported 只有具备独立远端视频接收/解码/停止能力的HAL可声明。
    virtual bool video_receive_supported() const { return false; }
    // configure_video_receive 仅按本次严格协商的video-in轨配置；默认旧HAL只允许不接收视频。
    virtual bool configure_video_receive(bool enabled) { return !enabled; }
    // bind_session_scope 在开始采集前绑定严格选定的本端会话；旧独立HAL无需共享作用域时保持兼容。
    virtual bool bind_session_scope(std::string_view) { return true; }
    // voice_supported 只有实际实现纯语音候选契约的驱动声明；旧视频驱动默认不支持。
    virtual bool voice_supported() const { return false; }
    // prepare_voice 预约固定格式的候选资源，不申请相机、不开始采集或播放；失败须保持未接听。
    virtual bool prepare_voice(const CallVoiceProfile&) { return false; }
    // begin_voice_candidate 仅建立Peer／编解码候选，不借网络Connected／Established启动第二麦克风。
    // 真正采集／播放还需独立整组focus及宿主移交，本核心尚未装配该授权入口。
    virtual bool begin_voice_candidate(const CallVoiceProfile&) { return false; }
    // apply_answer 接受服务器SDP，不把此结果误认为双向媒体连通。
    virtual bool apply_answer(std::string_view sdp) = 0;
    // connected 只在ICE/DTLS都已连通后返回true。
    virtual bool connected() const = 0;
    // hold_supported
    // 只有实际支持停声/停麦与显式恢复授权的驱动才能声明保持能力。
    virtual bool hold_supported() const { return false; }
    // set_held 返回实际停声/停止麦克风转发，或经驱动明确重获焦点后的恢复结果。
    // false不等于远端门控失败；恢复失败必须保持本端静音，不能隐式抢焦点。
    virtual bool set_held(bool) { return false; }
    // hold_async_supported
    // 显式选择实际回执驱动；旧同步端口不会被默认转为异步失败。
    virtual bool hold_async_supported() const { return false; }
    // request_hold
    // 绑定原控制身份并返回局部票据；非空只表示受理，不是实际停止或恢复。
    virtual std::optional<std::uint64_t> request_hold(bool, std::string_view, std::uint64_t,
                                                      std::uint64_t) {
        return std::nullopt;
    }
    // hold_result
    // 只查询原票据和目标；空值表示在途，false表示未知或失败而非已释放。
    virtual std::optional<bool> hold_result(std::uint64_t, bool) const { return false; }
    // close 幂等请求停止原轨和Peer；共享资源须实际释放之后才允许新预约。
    virtual void close() = 0;
};

// HumanCallConfig
// 绑定本地驱动、连接所有权和显式名册能力，不能从成员事件取得媒体权限。
struct HumanCallConfig {
    std::string camera_id = "front";   // 视频绑定已声明相机；Voice完全忽略相机且不广告video。
    bool borrowed_connection = false;  // 借用统一宿主时禁止重复connect/poll/disconnect。
    bool participant_roster =
        false;  // 仅已声明participantRoster的宿主开启，只读名册不授予媒体权限。
    HumanCallMode mode = HumanCallMode::Video;  // 默认旧视频；纯语音必须显式选择且驱动支持。
    bool receive_video =
        false;  // 显式远端视频接收能力；默认旧三轨，实际开启仍以平台选中video-in为准。
    bool udp_av = false; // 实际装配固定人际UDP驱动时显式开启，保留旧聚合初始化顺序。
    bool voice_and_video =
        false;             // 宿主显式装配两种通话类型，邀请只在此范围内选择。
    bool udp_only = false; // 人际UDP单协议固件拒绝WebRTC邀请，与AI传输无关。
};
// HumanParticipantTrack
// 保存公共Track元数据；不是驱动轨、发布/订阅许可或IM话权。
using HumanParticipantTrack = ParticipantTrack;
// HumanParticipant 是平台推送的有界成员名册条目；成员准入仍由IM决定。
using HumanParticipant = RosterParticipant;
// human_call_invitation 识别root
// purpose供唯一宿主分派，不代表邀请合法或已经接听。
bool human_call_invitation(std::string_view body);
// human_call_pending_deadline 校验等待期需要的身份及原截止；不授予接听或媒体许可。
std::optional<std::uint64_t> human_call_pending_deadline(const Envelope& invite);
// human_call_terminal_matches 判断结束事件是否属于指定腿且序号前进；不检查 Topic 授权。
bool human_call_terminal_matches(const Envelope& message, std::string_view session,
                                 std::uint64_t previous_sequence);

// HumanCallState 将邀请、接听、媒体协商及IM接通分开；网络确认不冒充设备接听。
enum class HumanCallState {
    Idle,
    Connecting,
    Subscribing,
    Reporting,
    Available,
    Inviting,
    Ringing,
    Accepting,
    Negotiating,
    ConnectingMedia,
    WaitingEstablished,
    Connected,
    Ending,
    Failed
};

// HumanCallFailure 仅分类原失败边界，不改变重试、期限或资源释放决定。
enum class HumanCallFailure {
    None,
    Unknown,
    ClockRollback,
    NetworkPoll,
    TrackDeadline,
    PhaseDeadline,
    InvitationDeadline,
    ConnectEvent,
    SubscriptionStart,
    SubscriptionEvent,
    CapabilitySubmit,  // 原send/请求账本未受理；不是Broker发布或网络交付事实。
    ReceiveRejected,
    NetworkEvent,
    StartConnect,
    EndPublish
};
// HumanCapabilityRejection
// 仅保留VoiceClient已采用的固定拒绝原因，不保存自由正文。
enum class HumanCapabilityRejection {
    None,
    Unknown,
    CapabilityNotAccepted,
    FocusDeviceUnavailable,
    RequestDenied
};
// HumanCallFailureFacts
// 为宿主提供只读首失败和原能力拒绝；零码表示未核到可信拒绝ACK。
struct HumanCallFailureFacts {
    HumanCallFailure failure = HumanCallFailure::None;  // 原首个失败边界。
    int capability_ack_code = 0;  // 仅原能力请求身份、方法及合法非200状态码都匹配时收录。
    HumanCapabilityRejection capability_reason =
        HumanCapabilityRejection::None;  // 固定原因或未知。
};

// HumanRejectionMethod 仅保存固定方法编号，未知值不保留原始字符串。
enum class HumanRejectionMethod {
    Unknown,
    RequestAck,
    SessionCreated,
    SessionInvite,
    MediaAnswer,
    SessionEstablished,
    SessionEnded,
    CallState,
    TrackState,
    ParticipantChanged,
    ContextSnapshot,
    EndpointState,
    CapabilityReport,
    SessionCreate,
    SessionAccept,
    SessionReject,
    MediaOffer,
    MediaState,
    SessionEnd,
    TrackControl,
    CallControl
};
// human_rejection_method_name 返回固定闭集名称，不反射网络正文。
std::string_view human_rejection_method_name(HumanRejectionMethod method);
// HumanCallRejectionFacts
// 保存首个原ReceiveRejected边界；state是调用receive前的阶段。
struct HumanCallRejectionFacts {
    HumanRejectionMethod method = HumanRejectionMethod::Unknown;         // 外层固定方法。
    HumanRejectionMethod request_method = HumanRejectionMethod::Unknown; // 仅ACK允许请求方法。
    std::optional<int> code;                     // 仅可信信封中200..599状态码，否则为空。
    HumanCallState state = HumanCallState::Idle; // 原拒帧前状态，不受close覆盖。
};

// HumanCallControlState
// 仅描述原控制等待；Completed还需独立完成事件，不冒称平台ACK已停声。
enum class HumanCallControlState { Idle, Pending, Completed, Rejected, Unknown };

// HumanCallControlReceipt
// 仅投影已核验原ACK与本地实际状态，不包含平台自由正文或成员信息。
struct HumanCallControlReceipt {
    std::string session, token,
        operation;               // 原腿、原控制与固定操作；不能用于授予新媒体权限。
    std::string reason;          // 仅固定staleRevision/invalidState/rejected或空。
    std::uint64_t revision = 0;  // 原ACK报告的版本，不声称实际门控完成。
    int code = 0;                // 未取得原ACK时为0，不能用完成事件伪造200。
    HumanCallControlState state = HumanCallControlState::Idle;  // 当前控制等待状态。
    bool actual_held = false,
         actual_applied = false;  // 本端原实际事实，与平台RTP门控分开。
};

// HumanCallTransferState
// 只表示单次转接操作；Transferred不代替原sessionEnded或新端实际焦点回执。
enum class HumanCallTransferState {
    Idle,
    Pending,
    Transferring,
    Transferred,
    Failed,
    Cancelled,
    Rejected,
    Unknown
};
// HumanCallTransferReceipt
// 投影原请求、受理、独立结果与撤候选确认，不含自由平台正文。
struct HumanCallTransferReceipt {
    std::string session, token, target,
        candidate;  // 本端原腿、操作和平台分配候选，不是新腿授权。
    std::string reason, cancel_token, cancel_reason;  // 固定结果/撤候选拒绝原因，不反射错误正文。
    std::uint64_t ack_revision = 0,
                  result_revision = 0;  // 受理与独立结果分别记录，不回退当前组版本。
    int ack_code = 0, cancel_code = 0;  // 没有原ACK时为0，事件不能补造200。
    HumanCallTransferState state = HumanCallTransferState::Idle;       // 当前原操作事实。
    HumanCallControlState cancel_state = HumanCallControlState::Idle;  // 撤候选等待独立于转接受理。
};

// HumanEndpointSourceOperation 仅可信核心选择读取或撤候选，不可由网页或MQTT填写。
enum class HumanEndpointSourceOperation { Read, Abort };
// HumanCallClient
// 管理单个人际腿；默认独占连接，借用模式由唯一宿主分派新signaling。
class HumanCallClient {
public:
    HumanCallClient(MqttPort& mqtt, CallMediaPort& media, std::string identity,
                    std::string instance, HumanCallConfig config = {});
    ~HumanCallClient();
    HumanCallClient(const HumanCallClient&) = delete;
    HumanCallClient& operator=(const HumanCallClient&) = delete;
    // EndpointSourceGate 由可信宿主核原scope/完整实际租约/本人输入意图，不接网页许可。
    using EndpointSourceGate = std::function<bool(std::string_view, std::uint64_t, std::uint64_t,
                                                  HumanEndpointSourceOperation)>;
    // set_endpoint_source 仅启动前显式装配；默认无入口，不宣称后端完整换端已可用。
    bool set_endpoint_source(EndpointSourceGate gate);
    // endpoint_source_available 只读核本腿及可信宿主事实；不驱动媒体或创建请求。
    bool endpoint_source_available(std::uint64_t now, std::uint64_t wall) const;
    // endpoint_source_abort_available 本人已停输入仍可撤已知候选，但不能越过原scope授权。
    bool endpoint_source_abort_available(std::uint64_t now, std::uint64_t wall) const;
    // prepare/commit/abort_endpoint_source 只发送原接续请求，不停源或恢复用户已停输入。
    bool prepare_endpoint_source(std::string_view target, std::uint64_t now, std::uint64_t wall);
    bool commit_endpoint_source(std::uint64_t now, std::uint64_t wall);
    bool abort_endpoint_source(std::uint64_t now, std::uint64_t wall);
    // endpoint_source_receipt 只读原事务事实，ACK/ready不表示媒体移交完成。
    const EndpointHandoffReceipt* endpoint_source_receipt() const;
    // input_wanted 仅本人轨启停意图，不代替实际采集或输出所有权。
    bool input_wanted() const { return input_wanted_; }
    // start 连接后精确订阅设备下行，能力只在收到SUBACK后报告。
    bool start(const MqttConfig& config, std::uint64_t now);
    // start_shared 由宿主在精确订阅及单一能力ACK后启动，不报告第二份能力快照。
    bool start_shared(std::uint64_t now);
    // receive_shared 仅接宿主按用途和原会话归属分派的信令，不消费他人Topic。
    bool receive_shared(std::string_view topic, std::string_view body, std::uint64_t now,
                        std::uint64_t wall_ms);
    // owns_shared_create_reply 仅按本腿原创建token分派新sessionId应答，不接纳内容或执行驱动。
    bool owns_shared_create_reply(const Envelope& message) const;
    // retired_shared_create_rejection 只查本腿原Failed创建账本；Router仍须校验旧应答精确字节和身份。
    bool retired_shared_create_rejection(const Envelope& message) const;
    // observe_shared_focus
    // 观察唯一实际Endpoint对同一原body返回的准入或原实际重放，仅补本腿共享事件水位。
    // Accepted/Pending不是资源取得；网络focusState、普通平台ACK和自报Accepted不能作为证明。
    // Ending仅关联原release的接纳/实际成功Replay，接纳不代表资源释放；最终清腿后全部拒绝。
    // 非法来源、冲突、跳号及两类时钟回退返回false且不修改原状态，不执行媒体或请求副作用。
    bool observe_shared_focus(std::string_view topic, std::string_view body,
                              const FocusSubmission& submission, std::uint64_t now,
                              std::uint64_t wall_ms);
    // can_observe_shared_focus
    // 只读核原scope、完整字节和共享水位，供宿主在停止驱动前拒绝stale控制。
    bool can_observe_shared_focus(std::string_view topic, std::string_view body, std::uint64_t now,
                                  std::uint64_t wall_ms) const;
    // observe_shared_endpoint
    // 观察原Topic/原腿endpointState水位；新候选仅同邀请提交及实际整组成立才私有继承源CAS。
    // 不把ready当接通或授权，不操作HAL，不允许外部设置revision。
    // 相同原字节重投幂等，冲突、跳号、错误会话或时钟回退无副作用；终态原腿清理后拒绝。
    bool observe_shared_endpoint(std::string_view topic, std::string_view body, std::uint64_t now,
                                 std::uint64_t wall_ms);
    // poll 使用可控单调时钟和Unix毫秒时间验证期限，不阻塞设备媒体线程。
    void poll(std::uint64_t now, std::uint64_t wall_ms);
    // accept/reject 仅能针对当前未过期邀请操作，本地真实授权必须由界面触发。
    bool accept(std::uint64_t now);
    bool reject(std::uint64_t now);
    // dial
    // 只发送设备目标标识；平台依照可信归属和授权决定是否邀请，设备不自报用户身份。
    bool dial(std::string_view target, std::uint64_t now);
    // offer_ready 仅接收本次WebRTC端点完整SDP；不能重复发送另一份offer。
    bool offer_ready(std::string_view sdp, std::uint64_t now);
    // media_connected 必须基于驱动双向ICE/DTLS事实，接通还需平台事件。
    bool media_connected(std::uint64_t now);
    // control_track
    // 仅报告驱动已执行的配置对应轨暂停／恢复；voice不接受旧mic／camera轨，也不远程开启采集。
    bool control_track(std::string_view track_id, bool paused, std::uint64_t now);
    // control_call
    // 只支持cancel/hold/resume，原revision与token固定，10秒未知不换身份重发。
    bool control_call(std::string_view operation, std::uint64_t now);
    // transfer_call
    // 只提交明确设备目标；平台核原主体/修订/权限，10秒受理和首次起45秒完成不续期。
    bool transfer_call(std::string_view target_device, std::uint64_t now);
    // cancel_transfer
    // 仅撤已确认原候选；调用方不能选择candidate/session/revision或关闭原主通话。
    bool cancel_transfer(std::uint64_t now);
    // transfer_receipt
    // 返回最近原转接固定事实；无操作为Idle，未知不能解释成未执行。
    HumanCallTransferReceipt transfer_receipt() const;
    // observe_transfer
    // 安装可选只读出口；回调不得重入业务或以状态代替实际媒体证据。
    void observe_transfer(std::function<void(const HumanCallTransferReceipt&)> observer) {
        transfer_observer_ = std::move(observer);
    }
    // call_held
    // 只返回驱动确认的本端保持事实，与平台RTP门控及业务Connected分开。
    bool call_held() const { return call_held_; }
    // call_revision
    // 返回当前平台call组控制版本，不与mediaRevision或eventSeq混用。
    std::uint64_t call_revision() const { return call_revision_; }
    // call_control_state 返回最近本端操作等待，不把Unknown解释成远端未执行。
    HumanCallControlState call_control_state() const { return control_state_; }
    // observe_control
    // 安装宿主只读出口；回调不得重入状态机或改变原命令及媒体决定。
    void observe_control(std::function<void(const HumanCallControlReceipt&)> observer) {
        control_observer_ = std::move(observer);
    }
    // end 本地立即停流，平台结束确认与资源释放独立进行。
    bool end(std::uint64_t now);
    // close 幂等停止本腿；独立模式释放MQTT，借用模式不动宿主连接或其他业务请求。
    void close();
    HumanCallState state() const { return state_; }
    // failure_facts
    // 返回有界脱敏快照；关闭原资源不会擦除首失败，显式新start才清空。
    HumanCallFailureFacts failure_facts() const { return failure_facts_; }
    // enable_rejection_diagnostics
    // 显式开启首拒帧取证；默认关闭不增加解码或输出。
    void enable_rejection_diagnostics() { rejection_diagnostics_ = true; }
    // rejection_facts 返回固定大小首因；只有显式新start才清空。
    const std::optional<HumanCallRejectionFacts>& rejection_facts() const {
        return rejection_facts_;
    }
    // mode 返回已校验邀请选中的通话类型；空闲期为本地缺省，不授予媒体资源。
    HumanCallMode mode() const { return config_.mode; }
    const std::string& session_id() const { return session_; }
    // active 只表示本腿平台及网络已建立；Voice仍是候选，不代表已取得实际采集／播放焦点。
    bool active() const { return state_ == HumanCallState::Connected; }
    bool track_control_pending() const { return !track_token_.empty(); }
    // participants 只读当前原腿名册；终态清空，不代表成员已连通或获准发言。
    const std::vector<HumanParticipant>& participants() const {
        return participant_roster_.members();
    }
    // context_snapshot 返回本腿完整私有留存；终态/断连/到期清空，不表示用户已阅读或播放。
    const std::optional<ContextSnapshot>& context_snapshot() const {
        return context_inbox_.snapshot();
    }
    // observe_context_ack 只观察原私有ACK完整编码后的本地提交返回值，不代表Broker或对端收到。
    // 回调不得重入状态机；默认空，不保存正文或任何会话/授权标识。
    void observe_context_ack(std::function<void(bool)> observer) {
        context_ack_observer_ = std::move(observer);
    }

private:
    // record_receive_rejection 仅在启用且首次原拒帧时解码，不参与业务判断。
    void record_receive_rejection(std::string_view body, HumanCallState previous);
 // receive_endpoint_source 以两份只读验证原子提交事务账本及全腿共享水位。
 bool receive_endpoint_source(std::string_view topic, std::string_view body, std::uint64_t now,
                              std::uint64_t wall);
 // can_observe_endpoint_event 核原主动事件/完整字节/水位，失败无副作用。
 bool can_observe_endpoint_event(std::string_view topic, std::string_view body, std::uint64_t now,
                                 std::uint64_t wall) const;
 // record_endpoint_event 只在预检成功后提交主动事件；不允许外部绕过事务账本。
 bool record_endpoint_event(std::string_view topic, std::string_view body, std::uint64_t now,
                            std::uint64_t wall);
 // endpoint_source_busy 避免接续与同腿轨、保持及普通转接并行。
 bool endpoint_source_busy() const;
 // read_endpoint_source_gate 按当前双时钟读取可信宿主，桌面异常或缺口不产生许可。
 bool read_endpoint_source_gate(
     std::uint64_t now, std::uint64_t wall,
     HumanEndpointSourceOperation operation = HumanEndpointSourceOperation::Read) const;
 EndpointSourceGate endpoint_source_gate_;                 // 启动前固定的本机可信读取口。
 std::unique_ptr<EndpointHandoffClient> endpoint_source_;  // 原共享发布口及有界原请求账本。
 std::string candidate_source_session_, candidate_handoff_; // 当前候选邀请冻结的来源，不从后继事件补授。
 std::string candidate_commit_token_; // 仅原已核提交事件可重放，不能以新token推进旧候选水位。
 bool candidate_revision_inherited_ = false; // 本人接听及实际当前完整租约成立后才成为新源。
 bool input_wanted_ = true;                                // 本人输入意图与独立扬声器所有权分开。
 friend class HumanCallStaticFaultTest;  // 仅测试私有缺水位故障，不增加运行时接口或授权入口。
 friend class HumanEndpointSourceStaticFaultTest;  // 仅注入账本背压，不改身份或资源许可。
    // prepare_media 按显式模式选择资源预约；语音不能借旧视频prepare返回true而被空壳驱动接听。
    bool prepare_media();
    // begin_media 共用协商状态机，语音只创建候选Peer／codec，不申请camera或启动采集／播放。
    bool begin_media(std::string_view session);
    // TransferFact
    // 单腿至多24个控制共享预算；只保存单次转接和最多一次原候选取消，旧代次不复活。
    struct TransferFact {
        std::string token, target, candidate;  // 本端原请求和已核原ACK/事件的候选。
        std::string ack_payload, result_payload,
            reason;  // 规范化确认用于严格重投比对，原因限定枚举。
        std::string cancel_token, cancel_ack_payload, cancel_result_payload,
            cancel_reason;  // 唯一撤候选身份与固定确认。
        std::uint64_t revision = 0,
                      started = 0;  // 首次expectedRevision和单调提交时刻，不因ACK刷新。
        std::uint64_t ack_revision = 0,
                      result_revision = 0;  // 两条平台事实各自版本。
        std::uint64_t cancel_revision = 0,
                      cancel_started = 0;  // 撤候选按当前已核版本和自己的10秒。
        int ack_code = 0,
            cancel_code = 0;  // 首个可信原ACK；自由错误正文不进入公共收据。
        bool ack_expired = false,
             cancel_expired = false;  // 截止后迟到不能制造受理成功。
        HumanCallTransferState state = HumanCallTransferState::Pending;  // 不改变原HumanCallState。
        HumanCallControlState cancel_state =
            HumanCallControlState::Idle;  // 不借原普通cancel结束整个呼叫。
    };
    // receive_transfer_state
    // 仅消费已核信封内的原转接ACK或固定事件；handled区分旧保持/恢复路径。
    bool receive_transfer_state(const Envelope& message, std::uint64_t now, bool& handled);
    // expire_transfer
    // 先于原Requests.tick取消到期控制；不关媒体，不延长原10/45秒。
    void expire_transfer(std::uint64_t now);
    // transfer_busy
    // 未决/未知或待原终结的转接禁止另起控制，精确撤候选经专用入口例外。
    bool transfer_busy() const;
    // report_transfer 只投影当前原操作，不从观察回调取得业务授权。
    void report_transfer(const TransferFact& fact) const;
    // transfer_projection 生成有界原身份事实，不把候选标识当新腿媒体许可。
    HumanCallTransferReceipt transfer_projection(const TransferFact& fact) const;
    std::vector<TransferFact> transfers_;  // 与普通控制合计至多24条，结束当前腿时清理。
    std::function<void(const HumanCallTransferReceipt&)>
        transfer_observer_;  // 默认关闭的宿主只读出口。
    // CallControlFact
    // 保留当前腿至多24次原控制的ACK/完成身份，重复不再次触碰媒体。
    struct CallControlFact {
        std::string token;               // 本端原请求身份，不随超时或重投刷新。
        std::string operation;           // 严格cancel/hold/resume，不接受转接等未实现操作。
        std::string ack_payload;         // 规范化原应答，用于拒绝同身份冲突重投。
        std::uint64_t revision = 0;      // 首次expectedRevision，成功必须是加一。
        std::uint64_t started = 0;       // 首次单调时间，10秒等待不能刷新。
        std::uint64_t ack_revision = 0;  // 已校验原ACK版本，不借当前全局版本改写旧应答。
        std::string reason;              // 固定拒绝原因，不向宿主透传自由错误正文。
        int ack_code = 0;                // 尚无原ACK时为0，事件先到不伪造ACK。
        bool event_seen = false;         // 原完成事件独立于ACK，副作用只执行一次。
        bool media_applied = false;      // 原票据的实际门控完成，排队或平台事件不能代替。
        bool unknown = false;            // 原截止已过或实际恢复未完成，不重建控制。
    };
    // receive_call_state
    // 严格关联原腿/控制版本；拒绝仅结束该控制，不结束原通话。
    bool receive_call_state(const Envelope& message, std::uint64_t now);
    // expire_call_control 先于账本tick/接收执行，10秒边界不得触发默认重传。
    void expire_call_control(std::uint64_t now);
    // poll_call_media 在原两秒局部预算内收录实际票据；超时保留原通话且不重发。
    void poll_call_media(std::uint64_t now);
    // report_control 默认无观察者，无额外账本；仅在原状态变化时发固定只读快照。
    void report_control(const CallControlFact& fact) const;
    // PendingCallMedia
    // 原平台事件至多驱动一个实际门控，结束腿时丢弃等待而不复活旧媒体。
    struct PendingCallMedia {
        std::string token;          // 原平台控制关联身份。
        std::uint64_t ticket = 0;   // 本地驱动票据，不与平台revision混用。
        std::uint64_t started = 0;  // 首次实际请求时间，重复事件不能刷新两秒预算。
        bool held = false;          // 原停止或显式恢复目标。
    };
    std::optional<PendingCallMedia> pending_call_media_;  // 单腿唯一实际门控等待。
    std::function<void(const HumanCallControlReceipt&)>
        control_observer_;  // 宿主可选有界记录出口。
    // InviteDecision
    // 仅保存原邀请及已作决定，不保存驱动；有效期内不可重释成新采集许可。
    struct InviteDecision {
        std::string token;          // 首次邀请身份，不因Busy或重投换token。
        std::string session;        // 首次独立腿，同token不能迁移至另一腿。
        std::string payload;        // 完整规范化原参数，避免有损摘要误认权限。
        std::string rejection;      // 原裸Busy拒绝字节，发布受理不等于实际送达。
        std::uint64_t expires = 0;  // 首次平台截止，不随重投延长。
        bool terminal = false;      // 原已拒绝或关闭意图，不再呈现来电。
    };
    // remember_invite 在呈现来电或发布Busy前保留事实，满额时不产生无保护副作用。
    InviteDecision* remember_invite(const Envelope& message, std::uint64_t expires,
                                    std::string rejection = {});
    // retire_invite 只结束当前原腿，不清其他仍有效的邀请决策。
    void retire_invite();
    // clear_session_state
    // 只清当前腿暂存；正常终态调用前须保存原决定并取消原腿在途请求。
    // 不清请求历史、连接身份或驱动Stopped租约，下一腿仍须重新获得实际媒体与建立许可。
    void clear_session_state();
    // update_participant
    // 校验整条快照后原子更新名册，超出设备存储预算拒绝，不改变IM成员或媒体。
    bool update_participant(const Envelope& message);
    // send 序列化已验证公共信封；一次业务命令只生成一个token。
    bool send(std::string method, std::string token, std::string payload, bool session);
    // send_at 按实际提交时刻登记新请求；能力退避不改变原Reporting阶段截止。
    bool send_at(std::string method, std::string token, std::string payload, bool session,
                 std::uint64_t now);
    // receive 限制Topic、token、会话和水位，过时结果不能重新打开媒体。
    bool receive(std::string_view topic, std::string_view body, std::uint64_t now,
                 std::uint64_t wall_ms);
    // receive_context 独立处理私有MSG；失败不关闭主通话，只有实际完整留存后才发布原ACK。
    bool receive_context(std::string_view body, std::uint64_t wall_ms);
    // fail
    // 保留首个原失败边界后终止本腿；借用模式不断宿主MQTT，不重拨或复活旧通话。
    void fail(HumanCallFailure reason = HumanCallFailure::Unknown);
    // next_token 使用进程唯一前缀和单调序号，重试不产生新身份。
    std::string next_token();
    MqttPort& mqtt_;          // 驱动生命周期必须长于本状态机。
    CallMediaPort& media_;    // 设备平台媒体驱动，不进入可移植协议核心。
    Requests requests_;       // 在途请求原正文保留与有界重试；远端超时结果视为未知。
    Requests call_requests_;  // 复用有界原请求账本，10秒取消不影响同腿媒体/轨请求。
    std::vector<CallControlFact> call_controls_;  // 单腿最多24条，结束后清理且旧腿消息隔离。
    std::uint64_t call_revision_ = 0;             // call组版本；初次控制固定0，不由媒体版本推断。
    std::uint64_t platform_revision_ =
        0;                             // 已收到完成事件的版本，拒绝ACK的currentRevision不是完成。
    std::string platform_call_state_;  // 最近平台控制状态，不声称设备已实际停/恢复。
    std::string platform_control_token_;  // 当前版本原控制身份，防止同版本替换完成事件。
    std::string platform_call_payload_;   // 同版本完整固定字段比对，候选不能被同身份换代。
    bool call_held_ = false;              // 仅实际媒体端口成功后改变。
    HumanCallControlState control_state_ = HumanCallControlState::Idle;  // 本端原控制等待。
    HumanCallConfig config_;                   // 固定模式、相机和MQTT所有权，不从来电扩权。
    std::vector<InviteDecision> invitations_;  // 最多32条，对象与字符串逻辑预算64KiB，按需分配。
    ParticipantRoster participant_roster_;     // 复用公共有界投影，不重复定义解析或IM准入规则。
    ContextSnapshotInbox context_inbox_;       // 当前人工腿唯一授权文本，不进入公共名册或日志。
    std::function<void(bool)> context_ack_observer_;  // 可选原ACK本地提交事实旁路，不参与接纳决定。
    std::uint64_t wall_highwater_ = 0;   // 只升墙钟水位，时钟回退不能使过期邀请重新有效。
    std::uint64_t last_now_ = 0;         // 最近单调时钟，取消借用腿不影响其他业务。
    std::string up_;                     // 当前设备的精确上行信令Topic。
    std::string down_;                   // 当前设备的精确下行信令Topic。
    std::string message_up_;             // 私有上下文接收ACK的精确MSG上行。
    std::string message_down_;           // 私有上下文下行，不隐含旧AI Topic。
    std::string instance_;               // 每次进程运行唯一的请求前缀。
    std::string session_;                // 唯一在场的IM通话ID。
    std::string invite_token_;           // 设备接听/拒接须使用邀请关联token。
    std::string invite_payload_;         // QoS1重投须与首次邀请完全一致，防止同token改写权限。
    std::string invite_method_;          // 同token接听/拒接仅能执行其中一个，不随阶段变化重新解释。
    std::string capability_token_;       // 当前连接能力报告的关联token。
    unsigned capability_conflicts_ = 0;  // 本连接明确能力冲突最多恢复两次，不修改revision。
    std::optional<std::uint64_t>
        capability_retry_at_;              // 原409受理时刻；500毫秒后仅同连接重报，关闭即取消。
    std::string create_token_;             // 设备主叫创建请求token，应答会分配独立会话腿。
    std::string create_response_payload_;  // 当前主叫应答正文，用于检测同token冲突重投。
    std::string offer_token_;              // 当前媒体offer的关联token。
    std::string state_token_;              // 当前连通报告的关联token。
    std::string end_token_;                // 当前主动挂断的关联token。
    std::string track_token_;              // 在途轨状态确认token，同一设备至多一个。
    std::string track_id_;                 // 在途轨状态的会话轨ID。
    std::string track_state_;              // 在途实际状态active或paused。
    std::uint64_t expires_at_ = 0;         // 邀请Unix毫秒绝对截止。
    std::uint64_t phase_at_ = 0;           // 单调时钟阶段起点，避免网络无限等待。
    std::uint64_t track_sent_at_ = 0;      // 在途轨确认起点，失败时关闭不一致媒体。
    std::uint64_t serial_ = 0;             // 本进程单调请求号。
    std::uint64_t event_seq_ = 0;          // 当前会话服务端信令事件水位。
    // SharedFocusFact 保存本腿至多32条、合计64KiB的原字节观察，重投不重推进共享水位。
    struct SharedFocusFact {
        std::string token, body;     // 原端点接纳的身份与完整字节，不能用规范化摘要替代。
        std::uint64_t sequence = 0;  // 原focus事件序号，不是资源修订号或实际资源许可。
    };
    bool cleanup_only_ = false; // 原阶段超时停止墓碑，只保原release/候选aborted/终态关联，不授业务资格。
    bool cleanup_released_ = false; // 仅原FocusEngine实际released回执后允许候选aborted水位及原平台终态；不授媒体权限。
    std::vector<SharedFocusFact> shared_focus_;  // 仅当前本腿观察，终态不清其他端点事实。
    std::size_t shared_focus_bytes_ = 0;         // 对象与有效原字符串字节预算，拒绝时不更新。
    std::vector<SharedFocusFact>
        shared_endpoint_;  // 当前腿接续主动事件原字节；至多128条且64KiB，不提前驱逐。
    std::size_t shared_endpoint_bytes_ = 0;  // 接续观察预算独立于focus，不借换端绕过原资源许可。
    int subscription_ = 0;                   // 待确认SUBSCRIBE MID。
    HumanCallState state_ = HumanCallState::Idle;  // 当前明确阶段。
    HumanCallFailureFacts failure_facts_;          // 原连接首失败有界投影，正常close不擦除诊断。
    bool rejection_diagnostics_ = false; // 默认不解析诊断正文。
    std::optional<HumanCallRejectionFacts> rejection_facts_; // 固定大小，不保存原文或身份。
    bool state_acked_ = false;                     // mediaState应答与sessionEstablished均需到达。
    bool established_ = false;                     // 只接受对应水位的建立事件。
    bool selected_udp_av_ = false; // 仅当前严格邀请选择，不从媒体报文推断。
    bool selected_receive_video_ =
        false;  // 本次原邀请/创建应答是否明确选中独立video-in，不从human用途推断。
};
}  // namespace ur::device
