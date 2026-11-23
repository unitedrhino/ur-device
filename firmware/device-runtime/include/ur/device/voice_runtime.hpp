// 可移植设备事件循环：MQTT 订阅确认、控制会话和异步媒体在一个线程串行推进。
#pragma once
#include <array>
#include <functional>
#include <string_view>
#include "ur/device/action_engine.hpp"
#include "ur/device/client_tool_endpoint.hpp"
#include "ur/device/content_wire.hpp"
#include "ur/device/endpoint_handoff_client.hpp"
#include "ur/device/fixed_queue.hpp"
#include "ur/device/focus_endpoint.hpp"
#include "ur/device/live_runtime_port.hpp"
#include "ur/device/mqtt_port.hpp"
#include "ur/device/playback_endpoint.hpp"
#include "ur/device/voice_client.hpp"

namespace ur::device {
// RuntimeState 为应用提供连接生命周期，语音输入/播放状态仍由 VoiceClient
// 独立报告。
enum class RuntimeState {
    Idle,
    Connecting,
    Subscribing,
    Running,
    Failed,
    Draining,
    ControlRecovering
};
// RuntimeFailure 只标识原致命分支，不保存协议正文、标识或驱动数据。
enum class RuntimeFailure {
    MqttConnect,
    MqttDisconnected,
    MqttEventFailure,
    MqttSubscription,
    ClockRollback,
    ConnectionDeadline,
    MqttPoll,
    SignalEnvelope,
    SignalClientInvalid,
    SignalClientDriver,
    MessageEnvelope,
    MessageAck,
    MessageContent,
    MessageToolScope,
    MessageQueueFull,
    ActionReplyPublish,
    ClientTick,
    ToolPoll,
    MediaPoll,
    MediaOffer,
    MediaReady,
    MissingWallClock,
    DrainDeadline,
    SharedInputInvalid,
    Unknown
};
// runtime_failure_name 只返回固定枚举名称，非法枚举不映射为unknown。
std::string_view runtime_failure_name(RuntimeFailure failure);
// ClientFailureOrigin 区分原运行时准备失败与已经发生的客户端失败，不替代原RuntimeFailure分类。
enum class ClientFailureOrigin : std::uint8_t { ClientFailed, EndpointPrepareFailed };
// ClientFailureDiagnostic 仅本机闭集旁证；不含身份/正文、资源许可或可续期时间。
struct ClientFailureDiagnostic {
    ClientFailureOrigin origin = ClientFailureOrigin::ClientFailed;  // 原检测边界。
    VoiceFailureCause cause = VoiceFailureCause::Unknown;            // 已锁存分支，未知不猜测。
    VoiceFailureMethod method = VoiceFailureMethod::None;            // 仅请求类失败可携固定方法。
};
// BorrowedVoiceConnection是显式宿主装配标记，不表示网络/能力/租约已确认；宿主须先取得实际事实。
struct BorrowedVoiceConnection {};
// TextInputResult
// 保留原接口名，表示云端对离散输入的业务确认，不表示模型已回复或设备已播放。
struct TextInputResult {
    std::string input_id;  // 与 message/inputSend 对应的设备生成输入ID。
    std::string result;    // accepted/busy/unsupported/rejected。
};
// MessageDiagnosticOutcome 只描述原消费分支，不能作为Broker确认或设备执行成功。
enum class MessageDiagnosticOutcome {
    NoSession,
    Draining,
    EnvelopeRejected,
    Unsupported,
    ContentRejected,
    SessionMismatch,
    WatermarkDiscarded,
    ToolUnavailable,
    ExecuteAccepted,
    ExecuteReplay,
    ExecuteDenied,
    ExecuteInvalid,
    AckConsumed,
    AckRejected,
    ScopeRejected,
    QueueFull,
    ContentQueued
};
// MessageDiagnostic 仅保存固定方法和序号/匹配事实，绝不保存信封或任何原标识。
struct MessageDiagnostic {
    std::string method = "unknown";  // 仅平台固定方法白名单，未知正文不得反射。
    MessageDiagnosticOutcome outcome = MessageDiagnosticOutcome::EnvelopeRejected;  // 原分支结论。
    std::uint64_t sequence = 0, watermark = 0;  // 原消息序号与进入分支时的水位。
    bool parsed = false,
         session_matches = false;  // 原信封解码和原活会话比较结果。
    bool taken = false, host_observed = false, projected = false,
         sent = false;  // 原宿主交付/过滤/发送，不代表网页展示。
};
// MessageDiagnostics
// 是单次启用期间的有界累计快照；overflow明确表示不是全量记录。
struct MessageDiagnostics {
    std::array<MessageDiagnostic, 32>
        records{};               // 最多32条原收包记录；宿主事实补原项不额外占预算。
    std::size_t size = 0;        // 已保存条数，读取不增加总预算。
    std::uint64_t received = 0;  // 精确MSG Topic原MqttEvent数量，不借UI计数。
    std::uint64_t taken = 0, host_filtered = 0, host_sent = 0,
                  host_send_failed = 0;  // 独立宿主聚合，不占收包预算。
    bool overflow = false;               // 超出预算时只标未知，不阻塞任何业务。
};
// AcceptedVoiceEnd 仅指原VoiceClient真正接纳且造成非Idle到Idle的终态，不代表平台媒体已释放。
enum class AcceptedVoiceEnd { None, SessionEndAck, SessionEndedEvent };
// VoiceCloseCause 表示原运行时真正调用关闭的入口，不能据此推断服务器关闭的业务原因。
enum class VoiceCloseCause { None, ExplicitClose, DrainSettled, Failure };
// VoiceLifecycleDiagnostic 分别锁存原本地请求、实际接纳终态及关闭入口；没有原身份或正文槽。
struct VoiceLifecycleDiagnostic {
    bool local_end_applied = false;  // 只有原end返回Applied才为真，按钮点击和publish不是终态。
    AcceptedVoiceEnd accepted_end = AcceptedVoiceEnd::None;  // 原终态一次锁存，迟到/重复不覆盖。
    VoiceCloseCause close_cause = VoiceCloseCause::None;     // 原首次非Idle关闭的实际入口。
    unsigned observation = 0;  // 最多三个独立事实，读取不刷新或触发业务。
};
// message_diagnostic_method 只返回固定白名单方法，不返回任何未知原字符串。
std::string_view message_diagnostic_method(std::string_view method);
// message_diagnostic_outcome 返回固定枚举文本；非法枚举返回空值以供投影拒绝。
std::string_view message_diagnostic_outcome(MessageDiagnosticOutcome outcome);
// HumanDispatchHook 由唯一宿主注册：在信令主题现有分派之前咨询，
// 返回 true 表示该报文属人际通话用途并已被消费（不再进入 AI/文件/焦点分派）。
// 参数依次为完整下行 Topic、原始正文、单调毫秒与 Unix 毫秒墙钟。
using HumanDispatchHook =
    std::function<bool(std::string_view, std::string_view, std::uint64_t, std::uint64_t)>;
// VoiceRuntime 只组合平台接口；没有系统时钟、线程、浏览器、密码或音频硬件依赖。
class VoiceRuntime {
public:
    // identity 是 productID/deviceName，instance_id
    // 每次进程启动唯一；不能使用通配符。
    VoiceRuntime(MqttPort& mqtt, PumpedVoiceMediaPort& media, std::string identity,
                 VoiceCapability capability, std::string instance_id,
                 ActionEngine* actions = nullptr,
                 PlaybackAdmissionPort* playback_admission = nullptr,
                 FocusEndpoint* focus = nullptr, LiveRuntimePort* live = nullptr,
                 ClientToolDisplayPort* client_display = nullptr);
    // 纯文字构造只装配MQTT及可选设备动作；capability必须为None，不创建麦克风/Peer/UDP或空媒体端口。
    VoiceRuntime(MqttPort& mqtt, std::string identity, VoiceCapability capability,
                 std::string instance_id, ActionEngine* actions = nullptr,
                 ClientToolDisplayPort* client_display = nullptr);
    // 共享语音/文字入口不拥有网络、动作或全局焦点；这些由唯一宿主管理，媒体端口只能关闭原AI路由。
    VoiceRuntime(MqttPort& mqtt, PumpedVoiceMediaPort& media, std::string identity,
                 VoiceCapability capability, std::string instance_id, BorrowedVoiceConnection);
    VoiceRuntime(MqttPort& mqtt, std::string identity, VoiceCapability capability,
                 std::string instance_id, BorrowedVoiceConnection);
    ~VoiceRuntime();
    VoiceRuntime(const VoiceRuntime&) = delete;
    VoiceRuntime& operator=(const VoiceRuntime&) = delete;
    // start 仅开始网络连接，不启用麦克风；凭据只交给网络适配器，不在核心保留。
    Result start(const MqttConfig& config, std::uint64_t now);
    // set_endpoint_focus_prepare只在启动前装配实际候选双向准备HAL；不默认提供成功端口。
    Result set_endpoint_focus_prepare(EndpointFocusPreparePort& port);
    // start_listening显式选择候选接听宿主，只真实鉴权/订阅/报能力，不自动创建模型会话。
    Result start_listening(const MqttConfig& config, std::uint64_t now);
    // accept_endpoint_invite仅由真实本地接听动作调用，未提交/未获完整焦点前不允许采播。
    Result accept_endpoint_invite(std::uint64_t now, std::uint64_t wall);
    // reject_endpoint_invite仅拒绝当前候选，不伪装源端abort或结束原Provider。
    Result reject_endpoint_invite(std::uint64_t now, std::uint64_t wall);
    // prepare_endpoint_source 只使用当前已建立原腿；now为单调毫秒、wall为当前Unix毫秒，目标意图仍由平台核权。
    Result prepare_endpoint_source(std::string_view target, std::uint64_t now, std::uint64_t wall);
    // commit_endpoint_source 使用当前两份时钟提交原ready候选，不读取旧缓存或直接切媒体模型。
    Result commit_endpoint_source(std::uint64_t now, std::uint64_t wall);
    // abort_endpoint_source 使用当前两份时钟撤原意图，未知提交不更换token或猜测回滚。
    Result abort_endpoint_source(std::uint64_t now, std::uint64_t wall);
    // endpoint_source_receipt 返回原有限控制器事实；空指针表示未装配源能力。
    const EndpointHandoffReceipt* endpoint_source_receipt() const;
    // endpoint_source_intent_available仅只读投影本地源意图入口，不查询时钟/驱动/authority。
    // 返回true只表示界面可提交意图；prepare/commit/abort仍须原完整scope授权，不保证受理成功。
    bool endpoint_source_intent_available() const;
    // resume_connection
    // 只接显式短断连恢复；宿主重新取得凭据后传入，不在核心保存密码或默认自动联网。
    Result resume_connection(const MqttConfig& config, std::uint64_t now);
    // start_shared仅在宿主验证同一连接精确订阅及统一能力ACK后调用；本端不自行联网或重报能力。
    Result start_shared(std::uint64_t now);
    // receive_shared仅接宿主按原请求/会话归属分派的SIG/MSG；错误只关闭原AI，不消费他腿网络队列。
    bool receive_shared(IncomingMessage message, std::uint64_t now, std::uint64_t wall);
    // poll 每次最多消费32条网络事件，保证持续流量不会饿死音频及请求超时。
    void poll(std::uint64_t now);
    // poll 文件/焦点共存时须提供真实Unix毫秒墙钟；单调时间仅用于超时/调度。
    void poll(std::uint64_t now, std::uint64_t wall);
    // files_released 表示本运行时实际文件输出已释放，仍不代表Core已收到释放事实。
    bool files_released() const { return !playback_ || playback_->released(); }
    // live_released 只读本端节目实际资源退出，不将平台终态或命令接纳当释放。
    bool live_released() const { return !live_ || live_->released(); }
    // file_established 只读原文件子会话的已建立状态，供可信宿主准入焦点，不代表播放成功。
    bool file_established(std::string_view session) const {
        return state_ == RuntimeState::Running && playback_ && playback_->established(session);
    }
    // focus_released 只表示本运行时原焦点整组已实际释放，不代表Core持久收录。
    bool focus_released() const { return !focus_ || focus_->released(); }
    // close 幂等释放本运行时所有资源；不隐式恢复旧会话。
    void close(std::uint64_t now);
    Result start_input(std::uint64_t now) { return client_.start_input(now); }
    Result stop_input(std::uint64_t now) { return client_.stop_input(now); }
    // suspend_input_for_focus 仅可信本机完整原suspend许可可退休原采播；不发送audioStop或改用户意图。
    Result suspend_input_for_focus(const FocusPermit& original, std::uint64_t now);
    // send_text 与音频共用已鉴权会话，正文只在请求缓存内保留到确认或超时。
    Result send_text(std::string_view text, bool audio, std::uint64_t now) {
        return client_.send_text(text, audio, now);
    }
    // send_content 仅由宿主事件线程提交原Ready引用，HTTPS worker不得直接重入协议状态机。
    Result send_content(const std::vector<InputContentPart>& parts, bool audio, std::uint64_t now) {
        return client_.send_content(parts, audio, now);
    }
    // supports_content 仅在宿主事件线程查询已选模态，不允许HTTPS worker与协议线程并发调用。
    bool supports_content(InputContentKind kind) const { return client_.supports_content(kind); }
    // take_text_result 返回异步requestAck事实；本地publish成功不等同于accepted。
    std::optional<TextInputResult> take_text_result();
    // take_client_tool_receipt仅交付原Endpoint验证的结果收件ACK；无能力或未知确认时为空。
    std::optional<ClientToolReceipt> take_client_tool_receipt() {
        return client_tools_ ? client_tools_->take_receipt() : std::nullopt;
    }
    // response_control_target 只读原内容回复；终态、取消或退休后不借旧播放ID复活控制资格。
    std::string_view response_control_target() const {
        if (state_ != RuntimeState::Running || (client_.state() != VoiceState::Established &&
                                                client_.state() != VoiceState::Recovering))
            return {};
        if (content_response_open_)
            return content_response_;
        const auto& audio = client_.response_id();
        return audio == content_response_ ? std::string_view{} : std::string_view(audio);
    }
    // cancel_response 同时退休原工具scope；未发声回复只取消控制，不制造或停止别的音频边界。
    Result cancel_response(std::uint64_t now) {
        const auto original_session = client_.session_id();
        const std::string original_response(response_control_target());
        if (original_response.empty()) {
            // 保留原None/播放期重复取消的同token等待语义，不重新授权已退休内容或其他播放。
            if (content_response_cancelled_ && !content_response_.empty() &&
                content_response_ == client_.response_id())
                return client_.cancel_response(now);
            return Result::Stale;
        }
        auto result = original_response == client_.response_id()
                          ? client_.cancel_response(now)
                          : client_.cancel_content_response(original_response, now);
        if ((result == Result::Stale || result == Result::Duplicate) && content_response_open_ &&
            original_response == content_response_)
            result = client_.cancel_content_response(original_response, now);
        if (result == Result::Applied) {
            content_response_ = original_response;
            content_response_open_ = false;
            content_response_cancelled_ = true;
            if (client_tools_)
                client_tools_->cancel_response(original_session, original_response);
        }
        return result;
    }
    // end 请求父AI结束时立即关闭文件/焦点输出；原事实仍沿共享连接收尾。
    Result end(std::uint64_t now) {
        const auto original_session = client_.session_id();
        const auto result = client_.end(now);
        if (result == Result::Applied && message_diagnostics_enabled_ &&
            !lifecycle_diagnostic_.local_end_applied) {
            lifecycle_diagnostic_.local_end_applied = true;
            ++lifecycle_diagnostic_.observation;
            lifecycle_diagnostic_dirty_ = message_diagnostics_dirty_ = true;
        }
        if (result == Result::Applied && client_tools_)
            client_tools_->close_session(original_session, now);
        if (result == Result::Applied && playback_)
            playback_->close(now);
        if (result == Result::Applied && focus_)
            focus_->close();
        if (result == Result::Applied && live_)
            live_->begin_parent_close(now);
        return result;
    }
    // take_content
    // 仅交付已经核对设备路由/当前会话/水位的展示事实；不授权执行工具。
    std::optional<ContentEvent> take_content();
    // enable_message_diagnostics 仅由可信宿主显式启用；默认关闭，不改变业务生命周期。
    void enable_message_diagnostics();
    // enable_failure_diagnostics 只在原失败前启用，不补采已发生的关闭首因。
    void enable_failure_diagnostics();
    // failure_diagnostic 只读首个原失败，不执行poll、驱动或消息解析。
    std::optional<RuntimeFailure> failure_diagnostic() const { return failure_; }
    // take_failure_diagnostic 至多领取一次首因，close及重复enable不刷新交付。
    std::optional<RuntimeFailure> take_failure_diagnostic();
    // take_client_failure_diagnostic 单独领取一次ClientTick细分，不修改原五字段诊断或失败路径。
    std::optional<ClientFailureDiagnostic> take_client_failure_diagnostic();
    // take_message_diagnostics 交付累计固定快照；正常close/fail保留原观测，读取不消费内容。
    std::optional<MessageDiagnostics> take_message_diagnostics();
    // take_lifecycle_diagnostic 只交付新的固定事实；默认关闭且读取没有驱动/收包/发布副作用。
    std::optional<VoiceLifecycleDiagnostic> take_lifecycle_diagnostic();
    // observe_content_display
    // 仅记录原宿主一次投影/发送结果；sent不是网页展示确认。
    void observe_content_display(const ContentEvent& content, bool projected, bool sent);
    // set_human_dispatch 安装可选人际通话分派钩子；默认无钩子保持原宿主行为不变。
    // 仅在信令任务线程调用与替换；钩子内不得重入本运行时任何入口。
    void set_human_dispatch(HumanDispatchHook dispatch) { human_dispatch_ = std::move(dispatch); }
    RuntimeState state() const { return state_; }
    const VoiceClient& voice() const { return client_; }
    // request_vision_session 由宿主在连接前请求aiVision三轨会话；能力上报不变，缺省纯语音两轨。
    void request_vision_session(bool requested) { client_.request_vision_session(requested); }

private:
    // shared_reply_start_needs_relation只读核心真实准入/水位，false仍须沿原receive失权/重复处理。
    bool shared_reply_start_needs_relation(IncomingMessage message) const;
    friend class
        SharedHandoffHost;  // 唯一可信native共享宿主可同步原不可变AI绑定；网络/页面无此私有入口。
    // shared_reply_install_gate
    // 仅borrowed+selected腿接原实际完整组事实；永久围栏不可恢复，不发送控制或关闭共享连接。
    // original为宿主已核不可变AI会话，owned为原driver实际整组，permanent为原三屏障永久提交，now不续Start截止。
    Result shared_reply_install_gate(std::string_view original, bool owned, bool permanent,
                                     std::uint64_t now);
    // endpoint_source_receive 只消费原源腿的接续事实，并与原VoiceClient共享SIG事件水位。
    bool endpoint_source_receive(IncomingMessage message, std::uint64_t now);
    // endpoint_source_available 用当前now/wall复核原腿，不借候选能力、恢复或旧快照发起新接续。
    bool endpoint_source_available(std::uint64_t now, std::uint64_t wall) const;
    // suspend_control 必须先停实际输入/输出和旧工具scope，再关闭原物理连接；不清原AI水位。
    bool suspend_control(std::uint64_t now, std::uint64_t wall);
    bool failure_diagnostics_enabled_ = false, failure_terminal_ = false,
         failure_taken_ = false;                           // 开关、终态及独立单条交付预算。
    std::optional<RuntimeFailure> failure_;                // 关闭后仍保留原首因。
    std::optional<RuntimeFailure> pending_event_failure_;  // 仅当前原on_event失败供caller锁存。
    std::optional<ClientFailureDiagnostic> client_failure_;  // 原ClientTick前锁存，不保存原事件。
    bool client_failure_taken_ = false;                      // 单次只读交付，清理和重复启用不重置。
    // record_endpoint_failure 只锁原准备分支，调用者仍沿原return/fail控制业务。
    void record_endpoint_failure(VoiceFailureCause cause);
    // reject_event 只标当前false分支，不执行关闭、收包或额外解析。
    bool reject_event(RuntimeFailure failure);
    // close_observed 保持原关闭实现唯一，仅旁观首次非Idle关闭来源，不改变失败/排空策略。
    void close_observed(std::uint64_t now, VoiceCloseCause cause);
    VoiceLifecycleDiagnostic lifecycle_diagnostic_;  // 与原事件线程同生，不保留原session/token。
    bool lifecycle_diagnostic_dirty_ = false;        // 原事实变化时才交付一次快照。
    // record_message_diagnostic 超预算只置标记，不干预水位、ACK或任何驱动。
    void record_message_diagnostic(MessageDiagnostic diagnostic);
    // diagnostic_content 原内容交付只补充同序号已准入项，不消耗新的收包记录预算。
    MessageDiagnostic* diagnostic_content(const ContentEvent& content);
    bool message_diagnostics_enabled_ = false,
         message_diagnostics_dirty_ = false;  // 显式诊断开关和新事实。
    MessageDiagnostics message_diagnostics_;  // 原连接事件线程拥有，不跨线程读取。
    // 共享构造保留旧音频/文件装配；仅None入口可传空媒体，不绕过现有能力一致性检查。
    VoiceRuntime(MqttPort& mqtt, PumpedVoiceMediaPort* media, std::string identity,
                 VoiceCapability capability, std::string instance_id, ActionEngine* actions,
                 PlaybackAdmissionPort* playback_admission, FocusEndpoint* focus,
                 LiveRuntimePort* live, ClientToolDisplayPort* client_display);
    // on_event 保持网络事实与业务确认分离；PUBACK 不能推进语音状态。
    bool on_event(const MqttEvent& event, std::uint64_t now, std::uint64_t wall);
    // on_action 只接受当前设备的精确动作路由，动作与语音信令分开解码。
    bool on_action(std::string_view body, std::uint64_t now);
    // fail 任何驱动故障均先释放媒体，避免界面仍显示连接时继续采集。
    void fail(std::uint64_t now, RuntimeFailure failure = RuntimeFailure::Unknown);
    // begin_drain 只收尾已存在子会话，不准入新的文件/动作/输入，也不恢复父AI。
    bool begin_drain(std::uint64_t now);
    // drain_settled 组合媒体退出、Live原租约actual
    // release及原回执排空，不凭物理false提前断连接。
    bool drain_settled() const;
    // endpoint_focus只为监听候选固定一条原focusControl，真实准备前不调用原FocusEndpoint。
    bool endpoint_focus(IncomingMessage message, std::uint64_t now, std::uint64_t wall);
    // endpoint_prepare_tick核首次固定两秒/原字节/实际scope绑定，不用observer作为准入。
    bool endpoint_prepare_tick(std::uint64_t now, std::uint64_t wall);
    MqttPort& mqtt_;                         // 唯一网络驱动，必须比运行时活得更久。
    bool borrowed_connection_ = false;       // 构造时固定所有权，关闭或失败不能切换为独占网络。
    PumpedVoiceMediaPort* media_;            // 音频借用唯一真实驱动；None没有媒体对象或轮询。
    std::string identity_;                   // 已配置的精确设备身份。
    std::string signaling_topic_;            // 精确信令下行。
    std::string message_topic_;              // 精确内容下行。
    std::string action_topic_;               // 可选物模型动作下行。
    ActionEngine* actions_;                  // 可选动作执行器，调用方持有且跨重连保留历史。
    HumanDispatchHook human_dispatch_;       // 可选人际通话分派；仅信令任务线程读写。
    FocusEndpoint* focus_;                   // 可选焦点端点，独占事件线程分派且生命周期长于运行时。
    LiveRuntimePort* live_;                  // 可选原节目端点，借唯一连接，关闭/销毁前须实际释放。
    VoiceClient client_;                     // 已有协议状态机，不复制半/全双工实现。
    ClientToolDisplayPort* client_display_;  // 显式真实预检端口，不能借音频ready广告工具。
    std::unique_ptr<ClientToolEndpoint> client_tools_;  // 原MSG工具权限/结果与音频播放身份独立。
    bool client_tools_valid_ = true;              // 能力声明必须与实际端口一起装配，不默认启用。
    std::unique_ptr<PlaybackEndpoint> playback_;  // 共用MQTT固定身份，独立子会话及实际释放。
    struct EndpointRuntime; // 显式接续HAL与最多一条原焦点缓存，旧默认不分配。
    std::unique_ptr<EndpointRuntime> endpoint_; // 不保存网络凭据，不和VoiceClient原业务账本混用。
    std::unique_ptr<EndpointHandoffClient> endpoint_source_; // 原网络共享源控制，不消费第二份MQTT队列。
    bool playback_valid_ = true;  // 能力与真实准入器必须一起装配，缺一项禁止连接及虚报。
    bool live_valid_ = true;      // liveReceive实际驱动与单次能力必须一致，不凭类存在默认广告。
    /* 内容队列容量 32：LLM 文本流以 ~30ms/条下发，TTS 开始时 RTP/DTLS 处理
     * 会让消费线程单轮停顿数百毫秒，8 条缓冲在真机 WebRTC 形态下溢出导致
     * 会话误判失败（messageQueueFull）；32 条约 1 秒缓冲覆盖停顿窗口。 */
    FixedQueue<ContentEvent, 32> contents_;  // 上层必须及时消费，溢出显式失败不默默丢最终文本。
    FixedQueue<TextInputResult, 4> text_results_;  // 界面须及时读取业务确认，避免状态静默丢失。
    RuntimeState state_ = RuntimeState::Idle;      // 当前连接生命周期。
    std::optional<std::uint64_t> now_;             // 单调时间回退必须失败。
    std::uint64_t phase_at_ = 0;                   // 当前连接或订阅阶段开始时间。
    std::uint64_t drain_at_ = 0;                   // 父AI结束后的固定收尾起点，重复消息不刷新。
    std::uint64_t message_sequence_ = 0;           // 当前会话内容水位，与信令水位分开。
    std::string content_response_;             // 已通过原会话/MSG水位的最新内容目标，与播放ID分离。
    bool content_response_open_ = false;       // 原终态或主动取消后关闭，不由旧播放对象恢复。
    bool content_response_cancelled_ = false;  // 显式取消才拒绝迟到音频；正常完成仍允许排空。
    int signaling_subscription_ = 0;           // 待确认的 SUBSCRIBE MID。
    int message_subscription_ = 0;             // 内容订阅使用独立 MID。
    int action_subscription_ = 0;              // 动作订阅的独立MID，未启用时为0。
    bool action_ready_ = false;                // 启用动作时也必须等待SUBACK。
    bool signaling_ready_ = false;             // 收到成功 SUBACK 后才为真。
    bool message_ready_ = false;               // 两个路由均确认后才能发 capabilityReport。
    bool media_reported_ = false;              // 本会话双向探测只向状态机报告一次。
};
}  // namespace ur::device
