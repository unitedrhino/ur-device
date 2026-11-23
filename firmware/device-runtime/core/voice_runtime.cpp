// 单线程设备运行时：唯一MQTT连接、独立AI/文件/焦点分派及实际释放栅栏；驱动由宿主泵入。
#include "ur/device/voice_runtime.hpp"
#include <cJSON.h>
#include <algorithm>
#include <utility>
#include "ur/device/connect_trace.hpp"
#include "ur/device/json_parse.hpp"
#include "voice_runtime_endpoint_state.hpp"

namespace ur::device {
namespace {
// 连接提速测量标签：与信令/媒体适配层打点共用 [connect-ms] 前缀便于聚合。
constexpr const char* kConnectTag = "ur_conn_rt";
}  // namespace
// VoiceRuntime 借用唯一网络/媒体、可选文件准入和焦点端点；构造不连接或默认授权。
VoiceRuntime::VoiceRuntime(MqttPort& mqtt, PumpedVoiceMediaPort& media, std::string identity,
                           VoiceCapability capability, std::string instance_id,
                           ActionEngine* actions, PlaybackAdmissionPort* playback_admission,
                           FocusEndpoint* focus, LiveRuntimePort* live,
                           ClientToolDisplayPort* client_display)
    : VoiceRuntime(mqtt, &media, std::move(identity), capability, std::move(instance_id), actions,
                   playback_admission, focus, live, client_display) {}
// 纯文字入口不申请任何媒体驱动；网络、请求、内容、动作仍复用同一个运行时。
VoiceRuntime::VoiceRuntime(MqttPort& mqtt, std::string identity, VoiceCapability capability,
                           std::string instance_id, ActionEngine* actions,
                           ClientToolDisplayPort* client_display)
    : VoiceRuntime(mqtt, nullptr, std::move(identity), capability, std::move(instance_id), actions,
                   nullptr, nullptr, nullptr, client_display) {}
// 借用模式只拥有原AI媒体；动作、全局焦点及其他子会话必须在外部唯一宿主保持独立生命周期。
VoiceRuntime::VoiceRuntime(MqttPort& mqtt, PumpedVoiceMediaPort& media, std::string identity,
                           VoiceCapability capability, std::string instance_id,
                           BorrowedVoiceConnection)
    : VoiceRuntime(mqtt, &media, std::move(identity), capability, std::move(instance_id), nullptr,
                   nullptr, nullptr, nullptr, nullptr) {
    borrowed_connection_ = true;
}
VoiceRuntime::VoiceRuntime(MqttPort& mqtt, std::string identity, VoiceCapability capability,
                           std::string instance_id, BorrowedVoiceConnection)
    : VoiceRuntime(mqtt, nullptr, std::move(identity), capability, std::move(instance_id), nullptr,
                   nullptr, nullptr, nullptr, nullptr) {
    borrowed_connection_ = true;
}
VoiceRuntime::VoiceRuntime(MqttPort& mqtt, PumpedVoiceMediaPort* media, std::string identity,
                           VoiceCapability capability, std::string instance_id,
                           ActionEngine* actions, PlaybackAdmissionPort* playback_admission,
                           FocusEndpoint* focus, LiveRuntimePort* live,
                           ClientToolDisplayPort* client_display)
    : mqtt_(mqtt),
      media_(media),
      identity_(std::move(identity)),
      signaling_topic_("$thing/down/signaling/" + identity_),
      message_topic_("$thing/down/message/" + identity_),
      action_topic_("$thing/down/action/" + identity_),
      actions_(actions),
      focus_(focus),
      live_(live),
      client_(mqtt, media, "$thing/up/signaling/" + identity_, signaling_topic_, capability,
              instance_id, focus),
      client_display_(client_display) {
    // 原恢复绑定足以标识源端；监听接续能力仍单独由目标端声明，不给旧设备升级能力。
    if (media && (capability.session_sync || capability.ai_endpoint_handoff))
        endpoint_source_ = std::make_unique<EndpointHandoffClient>(
            mqtt, "$thing/up/signaling/" + identity_, signaling_topic_, instance_id);
    client_tools_valid_ = capability.client_display_theme == (client_display != nullptr);
    if (capability.client_display_theme && client_display)
        client_tools_ = std::make_unique<ClientToolEndpoint>(mqtt, *client_display, identity_,
                                                             std::move(instance_id));
    const bool enabled = capability.file_audio_output || capability.file_video_output;
    playback_valid_ = enabled == (playback_admission != nullptr) &&
                      ((capability.transport == VoiceTransport::None) == (media == nullptr));
    live_valid_ =
        (capability.live_audio_receive || capability.live_video_receive) == (live != nullptr);
    if (enabled && playback_admission)
        playback_ = std::make_unique<PlaybackEndpoint>(
            mqtt, *playback_admission,
            PlaybackCapability{capability.revision, capability.file_audio_output,
                               capability.file_video_output},
            "$thing/up/signaling/" + identity_, signaling_topic_);
}
VoiceRuntime::~VoiceRuntime() { close(now_.value_or(0)); }
// message_diagnostic_method 未知原方法统一归类，不把自由字符串存入诊断。
std::string_view message_diagnostic_method(std::string_view method) {
    for (const auto known : {"requestAck", "respCreated", "respSttDelta", "respSttDone",
                             "respTextDelta", "respTextDone", "respEmotion", "respDone",
                             "toolCallStart", "toolCallExecute", "toolCallResult"})
        if (method == known)
            return known;
    return "unknown";
}
// message_diagnostic_outcome 只返回编译期固定分支名，非法枚举不输出。
std::string_view message_diagnostic_outcome(MessageDiagnosticOutcome outcome) {
    // 枚举只描述观察所在分支；未知数值不能转换为自由错误正文。
    switch (outcome) {
        case MessageDiagnosticOutcome::NoSession:
            return "no_session";
        case MessageDiagnosticOutcome::Draining:
            return "draining";
        case MessageDiagnosticOutcome::EnvelopeRejected:
            return "envelope_rejected";
        case MessageDiagnosticOutcome::Unsupported:
            return "unsupported";
        case MessageDiagnosticOutcome::ContentRejected:
            return "content_rejected";
        case MessageDiagnosticOutcome::SessionMismatch:
            return "session_mismatch";
        case MessageDiagnosticOutcome::WatermarkDiscarded:
            return "watermark_discarded";
        case MessageDiagnosticOutcome::ToolUnavailable:
            return "tool_unavailable";
        case MessageDiagnosticOutcome::ExecuteAccepted:
            return "execute_accepted";
        case MessageDiagnosticOutcome::ExecuteReplay:
            return "execute_replay";
        case MessageDiagnosticOutcome::ExecuteDenied:
            return "execute_denied";
        case MessageDiagnosticOutcome::ExecuteInvalid:
            return "execute_invalid";
        case MessageDiagnosticOutcome::AckConsumed:
            return "ack_consumed";
        case MessageDiagnosticOutcome::AckRejected:
            return "ack_rejected";
        case MessageDiagnosticOutcome::ScopeRejected:
            return "scope_rejected";
        case MessageDiagnosticOutcome::QueueFull:
            return "queue_full";
        case MessageDiagnosticOutcome::ContentQueued:
            return "content_queued";
    }
    return {};
}
// runtime_failure_name 仅转换原失败分支闭集；非法枚举不会被unknown掩盖。
std::string_view runtime_failure_name(RuntimeFailure failure) {
    switch (failure) {
        case RuntimeFailure::MqttConnect:
            return "mqttConnect";
        case RuntimeFailure::MqttDisconnected:
            return "mqttDisconnected";
        case RuntimeFailure::MqttEventFailure:
            return "mqttEventFailure";
        case RuntimeFailure::MqttSubscription:
            return "mqttSubscription";
        case RuntimeFailure::ClockRollback:
            return "clockRollback";
        case RuntimeFailure::ConnectionDeadline:
            return "connectionDeadline";
        case RuntimeFailure::MqttPoll:
            return "mqttPoll";
        case RuntimeFailure::SignalEnvelope:
            return "signalEnvelope";
        case RuntimeFailure::SignalClientInvalid:
            return "signalClientInvalid";
        case RuntimeFailure::SignalClientDriver:
            return "signalClientDriver";
        case RuntimeFailure::MessageEnvelope:
            return "messageEnvelope";
        case RuntimeFailure::MessageAck:
            return "messageAck";
        case RuntimeFailure::MessageContent:
            return "messageContent";
        case RuntimeFailure::MessageToolScope:
            return "messageToolScope";
        case RuntimeFailure::MessageQueueFull:
            return "messageQueueFull";
        case RuntimeFailure::ActionReplyPublish:
            return "actionReplyPublish";
        case RuntimeFailure::ClientTick:
            return "clientTick";
        case RuntimeFailure::ToolPoll:
            return "toolPoll";
        case RuntimeFailure::MediaPoll:
            return "mediaPoll";
        case RuntimeFailure::MediaOffer:
            return "mediaOffer";
        case RuntimeFailure::MediaReady:
            return "mediaReady";
        case RuntimeFailure::MissingWallClock:
            return "missingWallClock";
        case RuntimeFailure::DrainDeadline:
            return "drainDeadline";
        case RuntimeFailure::SharedInputInvalid:
            return "sharedInputInvalid";
        case RuntimeFailure::Unknown:
            return "unknown";
    }
    return {};
}
// enable_failure_diagnostics 只开启未来原分支；终态不会补采。
void VoiceRuntime::enable_failure_diagnostics() {
    if (!failure_terminal_ && state_ != RuntimeState::Failed) {
        failure_diagnostics_enabled_ = true;
        client_.enable_failure_diagnostics();
    }
}
// take_failure_diagnostic 独立领取固定首因，不推进任何业务。
std::optional<RuntimeFailure> VoiceRuntime::take_failure_diagnostic() {
    if (failure_taken_ || !failure_)
        return std::nullopt;
    failure_taken_ = true;
    return failure_;
}
// take_client_failure_diagnostic 只读一次原锁存事实，不消费内容/ACK或推进驱动。
std::optional<ClientFailureDiagnostic> VoiceRuntime::take_client_failure_diagnostic() {
    if (client_failure_taken_ || !client_failure_)
        return {};
    client_failure_taken_ = true;
    return client_failure_;
}
// record_endpoint_failure 原准备返回false之前记录闭集，不改变原两秒截止/媒体或权限。
void VoiceRuntime::record_endpoint_failure(VoiceFailureCause cause) {
    if (failure_diagnostics_enabled_ && !failure_terminal_ && !client_failure_)
        client_failure_ = ClientFailureDiagnostic{ClientFailureOrigin::EndpointPrepareFailed, cause,
                                                  VoiceFailureMethod::None};
}
void VoiceRuntime::enable_message_diagnostics() {
    enable_failure_diagnostics();
    if (message_diagnostics_enabled_)
        return;  // 重复启用不刷新预算或抹掉原失败事实。
    message_diagnostics_enabled_ = true;
    message_diagnostics_dirty_ = true;
}
// reject_event 暂存当前失败原因，只有原caller实际fail才锁存首次诊断。
bool VoiceRuntime::reject_event(RuntimeFailure failure) {
    if (failure_diagnostics_enabled_)
        pending_event_failure_ = failure;
    return false;
}
// take_message_diagnostics 只读取新固定事实，不消费业务内容或修改接收水位。
std::optional<MessageDiagnostics> VoiceRuntime::take_message_diagnostics() {
    if (!message_diagnostics_enabled_ || !message_diagnostics_dirty_)
        return std::nullopt;
    message_diagnostics_dirty_ = false;
    return message_diagnostics_;
}
// take_lifecycle_diagnostic 读取只清交付标志，不调用驱动、不消费消息或刷新任何业务期限。
std::optional<VoiceLifecycleDiagnostic> VoiceRuntime::take_lifecycle_diagnostic() {
    if (!message_diagnostics_enabled_ || !lifecycle_diagnostic_dirty_)
        return std::nullopt;
    lifecycle_diagnostic_dirty_ = false;
    return lifecycle_diagnostic_;
}
// record_message_diagnostic
// 超预算保持累计计数和溢出事实，不影响原网络/媒体分支。
void VoiceRuntime::record_message_diagnostic(MessageDiagnostic diagnostic) {
    if (!message_diagnostics_enabled_)
        return;
    diagnostic.method = message_diagnostic_method(diagnostic.method);
    if (message_diagnostics_.size < message_diagnostics_.records.size())
        message_diagnostics_.records[message_diagnostics_.size++] = std::move(diagnostic);
    else
        message_diagnostics_.overflow = true;
    message_diagnostics_dirty_ = true;
}
// diagnostic_content 仅补同原序号已入队记录；没有原记录时不创建新的收包事实。
MessageDiagnostic* VoiceRuntime::diagnostic_content(const ContentEvent& content) {
    for (std::size_t index = 0; index < message_diagnostics_.size; ++index) {
        auto& fact = message_diagnostics_.records[index];
        if (fact.outcome == MessageDiagnosticOutcome::ContentQueued &&
            fact.sequence == content.envelope.message_sequence.value_or(0) &&
            fact.method == message_diagnostic_method(content.envelope.method))
            return &fact;
    }
    return nullptr;
}
// observe_content_display
// 原宿主过滤/发送与网页展示分层，统计不回调任何业务函数。
void VoiceRuntime::observe_content_display(const ContentEvent& content, bool projected, bool sent) {
    if (!message_diagnostics_enabled_)
        return;
    auto& count = !projected ? message_diagnostics_.host_filtered
                  : sent     ? message_diagnostics_.host_sent
                             : message_diagnostics_.host_send_failed;
    if (count < 9007199254740991ULL)
        ++count;
    if (auto* fact = diagnostic_content(content)) {
        fact->host_observed = true;
        fact->projected = projected;
        fact->sent = projected && sent;
    }
    message_diagnostics_dirty_ = true;
}
std::optional<ContentEvent> VoiceRuntime::take_content() {
    if (contents_.empty())
        return std::nullopt;
    auto event = std::move(*contents_.front());
    contents_.pop_front();
    if (message_diagnostics_enabled_) {
        if (message_diagnostics_.taken < 9007199254740991ULL)
            ++message_diagnostics_.taken;
        if (auto* fact = diagnostic_content(event))
            fact->taken = true;
        message_diagnostics_dirty_ = true;
    }
    return event;
}
std::optional<TextInputResult> VoiceRuntime::take_text_result() {
    if (text_results_.empty())
        return std::nullopt;
    auto result = std::move(*text_results_.front());
    text_results_.pop_front();
    return result;
}
// start 检查能力装配与旧输出释放，再连接新代次；不绕过资源未知状态强制重连。
Result VoiceRuntime::start(const MqttConfig& config, std::uint64_t now) {
    if (borrowed_connection_)
        return Result::Unsupported;
    if (client_.capability_.ai_endpoint_control_sync &&
        (!endpoint_ || !endpoint_->port || !endpoint_->port->control_recovery_supported()))
        return Result::Unsupported; // 无完整有限恢复HAL不得联网后对外声明新目标恢复能力。
    if (!playback_valid_ || !live_valid_ || !client_tools_valid_ ||
        (client_display_ && !client_display_->available()))
        return Result::Invalid;
    if (focus_ && !focus_->matches(mqtt_, "$thing/up/signaling/" + identity_, signaling_topic_))
        return Result::Invalid;
    if (live_ && !live_->matches(mqtt_, "$thing/up/signaling/" + identity_, signaling_topic_))
        return Result::Invalid;
    if (!files_released())
        return Result::Busy;
    if (!live_released() || (live_ && !live_->settled()))
        return Result::Busy;
    if (!focus_released() || (focus_ && !focus_->settled()))
        return Result::Busy;
    if (live_ && focus_ &&
        !focus_->cleanup_settled([&](std::string_view session) { return live_->owns(session); }))
        return Result::Busy;
    if (state_ != RuntimeState::Idle && state_ != RuntimeState::Failed)
        return Result::Busy;
    const auto slash = identity_.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 == identity_.size() ||
        identity_.find('/', slash + 1) != std::string::npos ||
        identity_.find_first_of("+#\0", 0, 3) != std::string::npos)
        return Result::Invalid;
    close(now);
    if (playback_ && !playback_->begin_connection(now))
        return Result::Busy;
    if (focus_ && !focus_->begin_connection(now))
        return Result::Busy;
    if (live_ && !live_->begin_connection(now))
        return Result::Busy;
    now_ = now;
    phase_at_ = now;
    if (!mqtt_.connect(config)) {
        fail(now, RuntimeFailure::MqttConnect);
        return Result::DriverFailed;
    }
    state_ = RuntimeState::Connecting;
    return Result::Applied;
}
// close 立即取消采集及文件/焦点准入；实际释放仍须宿主继续poll并泵驱动事实。
void VoiceRuntime::close(std::uint64_t now) { close_observed(now, VoiceCloseCause::ExplicitClose); }
// close_observed 复用原关闭路径，首次真实关闭才锁存入口，后续析构不会覆盖原原因。
void VoiceRuntime::close_observed(std::uint64_t now, VoiceCloseCause cause) {
    if (state_ != RuntimeState::Idle)
        failure_terminal_ = true;  // 终态首次enable不能补采已清理的资源。
    // 启动前/重复close不算业务退出；失败关闭也不能误记成用户明确关闭。
    if (message_diagnostics_enabled_ && state_ != RuntimeState::Idle &&
        lifecycle_diagnostic_.close_cause == VoiceCloseCause::None) {
        lifecycle_diagnostic_.close_cause = cause;
        ++lifecycle_diagnostic_.observation;
        lifecycle_diagnostic_dirty_ = message_diagnostics_dirty_ = true;
    }
    if (client_tools_)
        client_tools_->close(now);
    if (playback_)
        playback_->close(now);
    if (focus_)
        focus_->close();
    if (endpoint_) {
        const bool preparing = endpoint_->listening || endpoint_->pending.has_value();
        endpoint_->pending.reset();
        endpoint_->listening = false;
        if (preparing && endpoint_->port) endpoint_->port->cancel();
    }
    if (live_)
        live_->close(now);
    if (actions_)
        actions_->disconnected();
    client_.disconnected(now);
    if (!borrowed_connection_)
        mqtt_.disconnect();
    contents_.clear();
    text_results_.clear();
    now_.reset();
    message_sequence_ = 0;
    content_response_.clear();
    content_response_open_ = false;
    content_response_cancelled_ = false;
    signaling_subscription_ = message_subscription_ = 0;
    action_subscription_ = 0;
    action_ready_ = false;
    signaling_ready_ = message_ready_ = media_reported_ = false;
    state_ = RuntimeState::Idle;
    drain_at_ = 0;
}
void VoiceRuntime::fail(std::uint64_t now, RuntimeFailure failure) {
    if (failure_diagnostics_enabled_ && !failure_terminal_ &&
        failure == RuntimeFailure::ClientTick && !client_failure_) {
        const auto original = client_.failure_diagnostic();
        client_failure_ =
            ClientFailureDiagnostic{ClientFailureOrigin::ClientFailed,
                                    original ? original->cause : VoiceFailureCause::Unknown,
                                    original ? original->method : VoiceFailureMethod::None};
    }
    failure_terminal_ = true;
    if (failure_diagnostics_enabled_ && !failure_)
        failure_ = failure;  // 原close之前锁首因，后续失败及析构均不可覆盖。
    close_observed(now, VoiceCloseCause::Failure);
    state_ = RuntimeState::Failed;
}
// start_shared不构造网络就绪或能力ACK；调用权只属于已完成统一握手的可信宿主。
Result VoiceRuntime::start_shared(std::uint64_t now) {
    if (!borrowed_connection_)
        return Result::Unsupported;
    if (!playback_valid_ || !live_valid_ || !client_tools_valid_)
        return Result::Invalid;
    if (state_ != RuntimeState::Idle && state_ != RuntimeState::Failed)
        return Result::Busy;
    close(now);
    const auto result = client_.connect_shared(now);
    if (result != Result::Applied)
        return result;
    now_ = phase_at_ = now;
    state_ = RuntimeState::Running;
    return Result::Applied;
}
// receive_shared复用生产解析器及水位；错误路由零副作用，当前AI非法输入或回退只关闭原AI。
bool VoiceRuntime::receive_shared(IncomingMessage message, std::uint64_t now, std::uint64_t wall) {
    if (!borrowed_connection_ ||
        (state_ != RuntimeState::Running && state_ != RuntimeState::Draining) ||
        (message.topic != signaling_topic_ && message.topic != message_topic_))
        return false;
    // 保留原输入/时钟/on_event短路順序；只清当前旁观暂存，不改变任何协议水位。
    pending_event_failure_.reset();
    const bool invalid_input = message.body.empty() || message.body.size() > 65536;
    const bool clock_rollback = !invalid_input && now_ && now < *now_;
    if (invalid_input || clock_rollback ||
        !on_event(
            {MqttEvent::Kind::Message, 0, 0, std::string(message.topic), std::string(message.body)},
            now, wall)) {
        fail(now, invalid_input    ? RuntimeFailure::SharedInputInvalid
                  : clock_rollback ? RuntimeFailure::ClockRollback
                                   : pending_event_failure_.value_or(RuntimeFailure::Unknown));
        return false;
    }
    now_ = now;
    return true;
}
// begin_drain 保持原MQTT收录子腿终态，先关闭输出；实际退出、原租约release和排空未齐备只能继续收尾。
bool VoiceRuntime::begin_drain(std::uint64_t now) {
    if (endpoint_ && endpoint_->listening && state_ != RuntimeState::Draining) {
        endpoint_->pending.reset();
        if (endpoint_->port) endpoint_->port->cancel(); // 原候选完整双向退出也属于实际收尾，不等逻辑close。
    }
    if (live_ && state_ != RuntimeState::Draining)
        live_->begin_parent_close(
            now);  // 先封原节目准入；无租约的物理退出也不能替代平台childEnded。
    if ((!playback_ && !focus_ && !live_) || (drain_settled() && mqtt_.publications_drained()))
        return false;
    if (state_ != RuntimeState::Draining) {
        if (playback_)
            playback_->close(now);
        if (focus_)
            focus_->close();
        drain_at_ = now;
        state_ = RuntimeState::Draining;
    }
    return true;
}
// drain_settled 无Live时沿旧策略；原Live租约未知/未Released只能保留连接到固定失败截止。
bool VoiceRuntime::drain_settled() const {
    return (!playback_ || playback_->settled()) && (!live_ || live_->settled()) &&
           (!focus_ || focus_->settled()) &&
           (!endpoint_ || !endpoint_->port || endpoint_->port->settled()) &&
           (!endpoint_ || endpoint_->cleanup_session.empty() ||
            (focus_ && focus_->cleanup_settled([&](std::string_view session) {
                return session == endpoint_->cleanup_session;
            }))) &&
           (!live_ || !focus_ || focus_->cleanup_settled([&](std::string_view session) {
               return live_->owns(session);
           }));
}
bool VoiceRuntime::on_action(std::string_view body, std::uint64_t now) {
    const auto result = actions_->submit(body, now);
    if (result.state == ActionAdmission::Replay)
        return mqtt_.publish("$thing/up/action/" + identity_, result.reply) ||
               reject_event(RuntimeFailure::ActionReplyPublish);
    if (result.state == ActionAdmission::Accepted || result.state == ActionAdmission::Pending ||
        result.state == ActionAdmission::Invalid)
        return true;
    ActionRequest request;
    if (decode_action_request(body, request) != DecodeResult::Ok)
        return true;
    std::string reply;
    const bool conflict = result.state == ActionAdmission::Conflict;
    if (encode_action_reply({request.token, request.action, conflict ? 409 : 503,
                             conflict ? "requestConflict" : "actionUnavailable", "{}"},
                            reply) != DecodeResult::Ok)
        return reject_event(RuntimeFailure::ActionReplyPublish);
    return mqtt_.publish("$thing/up/action/" + identity_, reply) ||
           reject_event(RuntimeFailure::ActionReplyPublish);
}
// on_event 在固定设备路由分派独立子会话；网络确认不能冒充业务接听或实际媒体就绪。
bool VoiceRuntime::on_event(const MqttEvent& event, std::uint64_t now, std::uint64_t wall) {
    switch (event.kind) {
        case MqttEvent::Kind::Failed:
            if (suspend_control(now, wall))
                return true;
            return reject_event(RuntimeFailure::MqttEventFailure);
        case MqttEvent::Kind::Disconnected:
            if (suspend_control(now, wall))
                return true;
            return reject_event(RuntimeFailure::MqttDisconnected);
        case MqttEvent::Kind::Connected:
            if (state_ != RuntimeState::Connecting || event.code != 0)
                return reject_event(RuntimeFailure::MqttEventFailure);
            UR_CONNECT_TRACE(kConnectTag, "stage=mqtt_connected code=%d t=%llu", event.code,
                             static_cast<unsigned long long>(now));
            signaling_subscription_ = mqtt_.subscribe(signaling_topic_);
            message_subscription_ = mqtt_.subscribe(message_topic_);
            if (actions_)
                action_subscription_ = mqtt_.subscribe(action_topic_);
            UR_CONNECT_TRACE(kConnectTag,
                             "stage=subscribe_requested signaling=%d message=%d action=%d t=%llu",
                             signaling_subscription_, message_subscription_, action_subscription_,
                             static_cast<unsigned long long>(now));
            if (signaling_subscription_ <= 0 || message_subscription_ <= 0 ||
                signaling_subscription_ == message_subscription_)
                return reject_event(RuntimeFailure::MqttSubscription);
            if (actions_ &&
                (action_subscription_ <= 0 || action_subscription_ == signaling_subscription_ ||
                 action_subscription_ == message_subscription_))
                return reject_event(RuntimeFailure::MqttSubscription);
            state_ = RuntimeState::Subscribing;
            phase_at_ = now;
            return true;
        case MqttEvent::Kind::Subscribed:
            if (state_ != RuntimeState::Subscribing)
                return true;
            if (event.operation != signaling_subscription_ &&
                event.operation != message_subscription_ &&
                (!actions_ || event.operation != action_subscription_))
                return true;
            if (event.code != 0)
                return reject_event(RuntimeFailure::MqttSubscription);
            UR_CONNECT_TRACE(kConnectTag, "stage=suback operation=%d t=%llu", event.operation,
                             static_cast<unsigned long long>(now));
            if (event.operation == signaling_subscription_)
                signaling_ready_ = true;
            else if (event.operation == message_subscription_)
                message_ready_ = true;
            else
                action_ready_ = true;
            if (signaling_ready_ && message_ready_ && (!actions_ || action_ready_)) {
                UR_CONNECT_TRACE(kConnectTag, "stage=subscriptions_ready t=%llu",
                                 static_cast<unsigned long long>(now));
                const auto connected = client_.control_recovering()
                                           ? client_.control_reconnected(now)
                                           : endpoint_ && endpoint_->listening
                                                 ? client_.connect_listening(now)
                                                 : client_.connect(now);
                UR_CONNECT_TRACE(kConnectTag, "stage=session_connect_result result=%d t=%llu",
                                 static_cast<int>(connected),
                                 static_cast<unsigned long long>(now));
                if (connected != Result::Applied)
                    return reject_event(RuntimeFailure::SignalClientDriver);
                state_ = RuntimeState::Running;
            }
            return true;
        case MqttEvent::Kind::Published:
            return true;
        case MqttEvent::Kind::Message:
            if (message_diagnostics_enabled_ && event.topic == message_topic_) {
                // 只数原网络事件；正常关闭/解析失败都不抹掉计数。饱和后不能溢出JS安全整数。
                if (message_diagnostics_.received < 9007199254740991ULL)
                    ++message_diagnostics_.received;
                message_diagnostics_dirty_ = true;
                if (state_ == RuntimeState::Draining || client_.session_id().empty())
                    record_message_diagnostic({"unknown",
                                               state_ == RuntimeState::Draining
                                                   ? MessageDiagnosticOutcome::Draining
                                                   : MessageDiagnosticOutcome::NoSession,
                                               0, message_sequence_, false, false});
            }
            if (state_ == RuntimeState::Draining) {
                // 仅原子腿终态与原Live租约release可推进收尾；新邀请、开启控制和动作不再执行。
                if (event.topic != signaling_topic_)
                    return true;
                Envelope message;
                if (decode_envelope(event.body, message) != DecodeResult::Ok)
                    return reject_event(RuntimeFailure::SignalEnvelope);
                if (playback_ && message.method == "sessionEnded" &&
                    playback_->owns(message.session))
                    playback_->receive(event.topic, event.body, now, wall);
                if (live_ && message.method == "sessionEnded" && live_->owns(message.session))
                    live_->receive(event.topic, event.body, now, wall);
                if (live_ && focus_ && message.method == "focusControl" &&
                    live_->owns(message.session)) {
                    FocusCommand original;
                    if (decode_focus_control(WireRoute::Signaling, event.body, original) ==
                            WireResult::Ok &&
                        original.operation == "release")
                        focus_->receive(event.topic, event.body, now, wall);
                }  // 原Authority仍核known-held/平台Ended/全组/retain，不把Draining开放成新写通道。
                // 已实际取得的候选成为源端转出后，唯一原腿仍需正式整组release；不准新取得/恢复。
                // 原FocusEndpoint保留原focusId/完整组/CAS/期限，HAL closed_release_valid继续核实际清理。
                if (endpoint_ && endpoint_->listening && endpoint_->port && focus_ &&
                    !endpoint_->cleanup_session.empty() && message.method == "focusControl" &&
                    message.session == endpoint_->cleanup_session) {
                    FocusCommand original;
                    const std::vector<std::string> complete{"microphone", "speaker"};
                    if (decode_focus_control(WireRoute::Signaling, event.body, original) == WireResult::Ok &&
                        original.operation == "release" && original.resources.size() == complete.size() &&
                        std::is_permutation(original.resources.begin(), original.resources.end(), complete.begin()))
                        focus_->receive(event.topic, event.body, now, wall);
                }
                return true;
            }
            if (state_ != RuntimeState::Running)
                return true;
            if (client_.control_recovering() && event.topic != signaling_topic_)
                return true;  // 新连接尚未安装原会话快照，动作/内容/工具均不得越过控制授权屏障。
            if (actions_ && event.topic == action_topic_)
                if (endpoint_ && endpoint_->listening &&
                    client_.endpoint_phase() != AIEndpointPhase::Active)
                    return true; // 候选尚未正式取得完整资源时不执行迟到模型设备副作用。
            if (actions_ && event.topic == action_topic_)
                return on_action(event.body, now);
            if (event.topic == signaling_topic_) {
                // 人际通话钩子先于一切业务分派：返回 true 表示宿主已按用途消费本条报文。
                if (human_dispatch_ && human_dispatch_(event.topic, event.body, now, wall))
                    return true;
                if (focus_) {
                    Envelope envelope;
                    if (decode_envelope(event.body, envelope) != DecodeResult::Ok)
                        return reject_event(RuntimeFailure::SignalEnvelope);
                    if (envelope.method == "focusControl") {
                        if (endpoint_ && endpoint_->listening)
                            return endpoint_focus({event.topic,event.body},now,wall) ||
                                   reject_event(RuntimeFailure::SignalClientInvalid);
                        if (client_.state() == VoiceState::Established)
                            focus_->receive(event.topic, event.body, now, wall);
                        else if (live_ && live_->owns(envelope.session) &&
                                 (client_.state() == VoiceState::Ending ||
                                  client_.state() == VoiceState::Idle)) {
                            FocusCommand original;
                            if (decode_focus_control(WireRoute::Signaling, event.body, original) ==
                                    WireResult::Ok &&
                                original.operation == "release")
                                focus_->receive(event.topic, event.body, now, wall);
                        }  // 原父关闭但尚未Draining只允许原Live关闭；未知/未建立父不授任何新输出权。
                        return true;  // 先按method分派，Live所属session不能吞焦点；非法/满槽不落AI状态机。
                    }
                }
                if (live_) {
                    Envelope envelope;
                    if (decode_envelope(event.body, envelope) != DecodeResult::Ok)
                        return reject_event(RuntimeFailure::SignalEnvelope);
                    bool invitation = false;
                    if (envelope.method == "sessionInvite") {
                        std::unique_ptr<cJSON, decltype(&cJSON_Delete)> params(
                            ur::device::parse_json(envelope.payload.c_str()), cJSON_Delete);
                        const auto* purpose =
                            cJSON_GetObjectItemCaseSensitive(params.get(), "purpose");
                        invitation = cJSON_IsString(purpose) &&
                                     std::string_view(purpose->valuestring) == "liveBroadcast";
                    }
                    if (invitation || live_->owns(envelope.session)) {
                        // 只隔离节目子腿；畸形/迟到/不支持不进入AI状态机或停止独立麦克风。
                        if ((!invitation || client_.state() == VoiceState::Established) &&
                            envelope.session != client_.session_id())
                            live_->receive(event.topic, event.body, now, wall);
                        return true;
                    }
                }
                if (playback_) {
                    Envelope envelope;
                    if (decode_envelope(event.body, envelope) != DecodeResult::Ok)
                        return reject_event(RuntimeFailure::SignalEnvelope);
                    bool file_invite = false;
                    if (envelope.method == "sessionInvite") {
                        // 这里只做用途分派；全部邀请字段由文件端点严格校验，畸形文件不能落入AI。
                        std::unique_ptr<cJSON, decltype(&cJSON_Delete)> params(
                            ur::device::parse_json(envelope.payload.c_str()), cJSON_Delete);
                        const auto* purpose =
                            cJSON_GetObjectItemCaseSensitive(params.get(), "purpose");
                        file_invite = cJSON_IsString(purpose) &&
                                      std::string_view(purpose->valuestring) == "mediaPlayback";
                    }
                    if (file_invite || envelope.method == "playbackControl" ||
                        playback_->owns(envelope.session)) {
                        // 文件不能借用AI父ID；非法/迟到子会话不进入语音状态机或停止AI采集。
                        if (file_invite && client_.state() != VoiceState::Established)
                            return true;
                        if (envelope.session != client_.session_id())
                            playback_->receive(event.topic, event.body, now, wall);
                        return true;
                    }
                }
                if (endpoint_source_receive({event.topic, event.body}, now)) return true;
                const auto original_session = client_.session_id();
                const bool endpoint_allocated = endpoint_ && endpoint_->listening && endpoint_->port &&
                    focus_ && client_.endpoint_phase() == AIEndpointPhase::Active &&
                    focus_->scoped_permitted(original_session, {"microphone", "speaker"}, now, wall);
                const auto original_state =
                    client_.state();  // 原接纳前状态，不能借网络声称已结束。
                // 显式取消后的音频开始不能借SIG/MSG跨通道迟到安装旧nonce；正常完成不等于取消。
                VoiceEvent audio_start;
                if (decode_voice_event(WireRoute::Signaling, event.body, audio_start) ==
                        WireResult::Ok &&
                    audio_start.envelope.method == "respAudioStart" &&
                    audio_start.envelope.session == original_session &&
                    ((content_response_cancelled_ &&
                      audio_start.response_id == content_response_) ||
                     (client_tools_ && client_tools_->response_cancelled(original_session,
                                                                         audio_start.response_id))))
                    return true;
                const bool control_recovering = client_.control_recovering();
                const auto result = client_.receive({event.topic, event.body}, now, wall);
                // 必须先有原实际完整许可，再有严格已接纳transferred终态；下行自报身份不能创建清理锚点。
                if (endpoint_allocated && result == Result::Applied && client_.state() == VoiceState::Idle) {
                    VoiceEvent ended;
                    if (decode_voice_event(WireRoute::Signaling, event.body, ended) == WireResult::Ok &&
                        ended.envelope.method == "sessionEnded" && ended.envelope.session == original_session &&
                        ended.reason == "transferred")
                        endpoint_->cleanup_session = original_session;
                }
                if (control_recovering && result == Result::Applied &&
                    !client_.control_recovering())
                    message_sequence_ =
                        std::max(message_sequence_, client_.control_message_watermark());
                if (message_diagnostics_enabled_ && result == Result::Applied &&
                    original_state != VoiceState::Idle && client_.state() == VoiceState::Idle &&
                    lifecycle_diagnostic_.accepted_end == AcceptedVoiceEnd::None) {
                    VoiceEvent original;
                    if (decode_voice_event(WireRoute::Signaling, event.body, original) ==
                            WireResult::Ok &&
                        original.envelope.method == "sessionEnded") {
                        lifecycle_diagnostic_.accepted_end =
                            original.envelope.code ? AcceptedVoiceEnd::SessionEndAck
                                                   : AcceptedVoiceEnd::SessionEndedEvent;
                        ++lifecycle_diagnostic_.observation;
                        lifecycle_diagnostic_dirty_ = message_diagnostics_dirty_ = true;
                    }
                }
                // 取消ACK在SIG而非MSG；先由原VoiceClient核token/resp，再撤销相同工具scope。
                if (client_tools_ && result == Result::Applied) {
                    VoiceEvent original;
                    if (decode_voice_event(WireRoute::Signaling, event.body, original) ==
                            WireResult::Ok &&
                        original.request_method == "respCancel" && original.envelope.code &&
                        *original.envelope.code == 200)
                        client_tools_->cancel_response(original.envelope.session,
                                                       original.response_id);
                }
                if (client_tools_ &&
                    (client_.state() == VoiceState::Idle || client_.state() == VoiceState::Failed))
                    client_tools_->close_session(original_session, now);
                if (client_.state() == VoiceState::Idle && !client_.endpoint_waiting())
                    begin_drain(now);
                if (result == Result::DriverFailed)
                    return reject_event(RuntimeFailure::SignalClientDriver);
                if (result == Result::Invalid)
                    return reject_event(RuntimeFailure::SignalClientInvalid);
                return true;
            }
            if (event.topic == message_topic_ && !client_.session_id().empty()) {
                if (endpoint_ && endpoint_->listening &&
                    client_.endpoint_phase() != AIEndpointPhase::Active)
                    return true; // Held期间原MSG/工具水位不推进，不执行或展示旧回复。
                if (client_.control_recovering())
                    return true;  // 尚未迁移的旧回复/工具事件不消费水位、不触发实际副作用。
                Envelope envelope;
                if (decode_envelope(event.body, envelope) != DecodeResult::Ok) {
                    record_message_diagnostic({"unknown",
                                               MessageDiagnosticOutcome::EnvelopeRejected, 0,
                                               message_sequence_, false, false});
                    return reject_event(RuntimeFailure::MessageEnvelope);
                }
                // 从已解码原信封投影，只保存固定枚举及匹配布尔；所有业务调用仍只执行一次。
                const auto diagnostic = [&](MessageDiagnosticOutcome outcome) {
                    if (message_diagnostics_enabled_)
                        record_message_diagnostic(
                            {std::string(message_diagnostic_method(envelope.method)), outcome,
                             envelope.message_sequence.value_or(0), message_sequence_, true,
                             envelope.session == client_.session_id()});
                };
                // 专用执行命令不是展示事件；只在原活会话、水位和已准入内容scope下投递审核端点。
                if (envelope.method == "toolCallExecute") {
                    if (!client_tools_ || envelope.session != client_.session_id() ||
                        !envelope.message_sequence ||
                        *envelope.message_sequence <= message_sequence_) {
                        diagnostic(!client_tools_ ? MessageDiagnosticOutcome::ToolUnavailable
                                   : envelope.session != client_.session_id()
                                       ? MessageDiagnosticOutcome::SessionMismatch
                                   : !envelope.message_sequence
                                       ? MessageDiagnosticOutcome::ContentRejected
                                       : MessageDiagnosticOutcome::WatermarkDiscarded);
                        return true;
                    }
                    const auto decision = client_tools_->receive_execute(
                        event.body, client_.session_id(),
                        client_.state() == VoiceState::Established, now, wall);
                    diagnostic(decision == ClientToolDecision::Accepted
                                   ? MessageDiagnosticOutcome::ExecuteAccepted
                               : decision == ClientToolDecision::Replay
                                   ? MessageDiagnosticOutcome::ExecuteReplay
                               : decision == ClientToolDecision::Invalid
                                   ? MessageDiagnosticOutcome::ExecuteInvalid
                                   : MessageDiagnosticOutcome::ExecuteDenied);
                    if (decision == ClientToolDecision::Accepted ||
                        decision == ClientToolDecision::Replay)
                        message_sequence_ = *envelope.message_sequence;
                    return true;  // 非法/迟到工具被隔离，不能污染音频输入ACK或创建新回复。
                }
                if (envelope.method == "requestAck") {
                    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> params(
                        ur::device::parse_json(envelope.payload.c_str()), cJSON_Delete);
                    const auto* method =
                        params ? cJSON_GetObjectItemCaseSensitive(params.get(), "requestMethod")
                               : nullptr;
                    if (cJSON_IsString(method) &&
                        std::string_view(method->valuestring) == "toolCallSubmit") {
                        if (client_tools_)
                            client_tools_->receive_ack(event.body, now);
                        diagnostic(MessageDiagnosticOutcome::AckConsumed);
                        return true;  // 端结果受理与原模型续答/实际播放分层，不进入VoiceClient。
                    }
                    VoiceEvent ack;
                    if (decode_voice_event(WireRoute::Message, event.body, ack) != WireResult::Ok) {
                        diagnostic(MessageDiagnosticOutcome::AckRejected);
                        return reject_event(RuntimeFailure::MessageAck);
                    }
                    const auto original_input = client_.text_input_id();
                    const auto result = client_.receive({event.topic, event.body}, now);
                    if (ack.request_method == "inputSend" && result != Result::Stale &&
                        result != Result::Invalid && result != Result::DriverFailed) {
                        const auto accepted = ack.envelope.code && *ack.envelope.code == 200;
                        const auto outcome =
                            accepted                                         ? "accepted"
                            : ack.envelope.code && *ack.envelope.code == 409 ? "busy"
                            : ack.envelope.code && *ack.envelope.code == 422 ? "unsupported"
                                                                             : "rejected";
                        if (!text_results_.push_back(
                                {ack.input_id.empty() ? original_input : ack.input_id, outcome})) {
                            diagnostic(MessageDiagnosticOutcome::QueueFull);
                            return reject_event(RuntimeFailure::MessageQueueFull);
                        }
                    }
                    diagnostic(result == Result::Invalid || result == Result::DriverFailed
                                   ? MessageDiagnosticOutcome::AckRejected
                                   : MessageDiagnosticOutcome::AckConsumed);
                    return (result != Result::DriverFailed && result != Result::Invalid) ||
                           reject_event(RuntimeFailure::MessageAck);
                }
                ContentEvent content;
                const auto result = decode_content_event(WireRoute::Message, event.body, content);
                if (result == WireResult::Unsupported) {
                    diagnostic(MessageDiagnosticOutcome::Unsupported);
                    return true;
                }
                if (result != WireResult::Ok) {
                    diagnostic(MessageDiagnosticOutcome::ContentRejected);
                    return reject_event(RuntimeFailure::MessageContent);
                }
                if (content.envelope.session != client_.session_id() ||
                    *content.envelope.message_sequence <= message_sequence_) {
                    diagnostic(content.envelope.session != client_.session_id()
                                   ? MessageDiagnosticOutcome::SessionMismatch
                                   : MessageDiagnosticOutcome::WatermarkDiscarded);
                    return true;
                }
                if (client_.transport() == VoiceTransport::None) {
                    const auto admitted = client_.receive_content(content);
                    if (admitted == Result::Invalid) {
                        diagnostic(MessageDiagnosticOutcome::ContentRejected);
                        return reject_event(RuntimeFailure::MessageContent);
                    }
                    if (admitted != Result::Applied) {
                        diagnostic(MessageDiagnosticOutcome::ScopeRejected);
                        return true;
                    }
                }
                const auto sequence = *content.envelope.message_sequence;
                if (content.envelope.method != "respDone" &&
                    ((content_response_cancelled_ && content.response_id == content_response_) ||
                     (client_tools_ && client_tools_->response_cancelled(content.envelope.session,
                                                                         content.response_id)))) {
                    diagnostic(MessageDiagnosticOutcome::ScopeRejected);
                    message_sequence_ = sequence;
                    return true;  // 旧结果可被记录为迟到，但不能显示到新轮或复活原工具scope。
                }
                // 只观察已核原会话/水位及None输入准入的内容；音频播放response_id不作为工具授权来源。
                if (client_tools_ && !client_tools_->observe(content)) {
                    diagnostic(MessageDiagnosticOutcome::ScopeRejected);
                    return reject_event(RuntimeFailure::MessageToolScope);
                }
                if (content.envelope.method == "respCreated") {
                    content_response_ = content.response_id;
                    content_response_open_ = true;
                    content_response_cancelled_ = false;
                } else if (content.envelope.method == "respDone" &&
                           content.response_id == content_response_) {
                    content_response_open_ = false;
                }
                if (!contents_.push_back(std::move(content))) {
                    diagnostic(MessageDiagnosticOutcome::QueueFull);
                    return reject_event(RuntimeFailure::MessageQueueFull);
                }
                diagnostic(MessageDiagnosticOutcome::ContentQueued);
                message_sequence_ = sequence;
            }
            return true;
    }
    return reject_event(RuntimeFailure::Unknown);
}
// poll 单参数重载保留纯语音调用；文件/焦点共存不能用单调时间替代Unix截止。
void VoiceRuntime::poll(std::uint64_t now) { poll(now, 0); }
// poll 推进已关闭文件/焦点的实际释放，网络失败仍允许宿主泵驱动事实，不恢复准入。
void VoiceRuntime::poll(std::uint64_t now, std::uint64_t wall) {
    if ((playback_ || focus_ || live_ || client_tools_) && wall == 0 &&
        state_ != RuntimeState::Idle && state_ != RuntimeState::Failed) {
        fail(now, RuntimeFailure::MissingWallClock);  // 缺墙钟仍沿原关闭路径，不准入一次再失败。
        if (playback_)
            playback_->tick(now, wall);
        if (focus_)
            focus_->poll(now, wall);
        if (live_)
            live_->tick(now, wall);
        return;
    }
    if (state_ == RuntimeState::Idle || state_ == RuntimeState::Failed) {
        if (playback_)
            playback_->tick(now, wall);
        if (focus_)
            focus_->poll(now, wall);
        if (live_)
            live_->tick(now, wall);
        return;
    }
    // 原三个条件仍按时钟、阶段截止、MQTT顺序短路；不重复调用poll作取证。
    const bool clock_rollback = now_ && now < *now_;
    const bool connection_deadline =
        !clock_rollback &&
        (state_ == RuntimeState::Connecting || state_ == RuntimeState::Subscribing) &&
        now - phase_at_ >= 30000;
    if (client_.control_recovering()) {
        client_.tick(now);  // 重鉴权及SUBACK仍共享第一次失联15秒，不能借30秒普通连接预算续期。
        if (client_.state() == VoiceState::Failed) {
            fail(now, RuntimeFailure::ConnectionDeadline);
            return;
        }
    }
    if (state_ == RuntimeState::ControlRecovering && !clock_rollback) {
        now_ = now;
        if (focus_) focus_->poll(now, wall); // 离线也消费真实停流事实，但不签新取得许可。
        if (media_ && !media_->poll(now))
            fail(now, RuntimeFailure::MediaPoll);
        return;  // 无物理连接时不运行MQTT重试；宿主显式resume_connection才开始新鉴权。
    }
    const bool network_failed =
        !clock_rollback && !connection_deadline && !borrowed_connection_ && !mqtt_.poll();
    if (network_failed && suspend_control(now, wall)) {
        now_ = now;
        return;
    }
    if (clock_rollback || connection_deadline || network_failed) {
        fail(now, clock_rollback        ? RuntimeFailure::ClockRollback
                  : connection_deadline ? RuntimeFailure::ConnectionDeadline
                                        : RuntimeFailure::MqttPoll);
        return;
    }
    now_ = now;
    // 借用运行时由唯一宿主逐条分派，不能抢读人工腿、物模型或全局焦点的原队列。
    for (unsigned count = 0; !borrowed_connection_ && count < 32; ++count) {
        const auto event = mqtt_.take_event();
        if (!event)
            break;
        pending_event_failure_.reset();
        if (!on_event(*event, now, wall)) {
            fail(now, pending_event_failure_.value_or(RuntimeFailure::Unknown));
            return;
        }
    }
    if (state_ == RuntimeState::Draining) {
        if (playback_)
            playback_->tick(now, wall);
        if (focus_)
            focus_->poll(now, wall);
        if (live_)
            live_->tick(now, wall);
        if (drain_settled() && mqtt_.publications_drained())
            close_observed(now, VoiceCloseCause::DrainSettled);
        else if (now - drain_at_ >= 30000)
            fail(now, RuntimeFailure::DrainDeadline);  // 原固定截止不变，未知不能假报正常挂断。
        return;
    }
    if (state_ != RuntimeState::Running)
        return;
    if (client_.control_recovering()) {
        // 原固定截止已在本轮前部检查；快照前只泵实际停流和Peer保活，不执行动作或重传旧回执。
        if (media_ && !media_->poll(now))
            fail(now, RuntimeFailure::MediaPoll);
        return;
    }
    if (playback_)
        playback_->tick(now, wall);  // 先消费当前网络终态/断连，再推进异步准备，防止结束后接听。
    if (focus_)
        focus_->poll(now, wall);
    if (live_)
        live_->tick(now, wall);
    if (actions_) {
        actions_->poll(now);
        // 与MQTT事件/媒体一样有界推进；发布失败保留执行器历史供重投，不能重执行动作。
        for (unsigned count = 0; count < 4; ++count) {
            const auto reply = actions_->take_reply();
            if (!reply)
                break;
            if (!mqtt_.publish("$thing/up/action/" + identity_, *reply)) {
                fail(now, RuntimeFailure::ActionReplyPublish);
                return;
            }
        }
    }
    if (!endpoint_prepare_tick(now,wall)) {
        fail(now,RuntimeFailure::ClientTick);
        return;
    }
    client_.tick(now,wall);
    // 控制恢复/终态不能重传旧源请求；有限原账本保留供界面读取，下一原腿bind才清理。
    if (endpoint_source_available(now, wall) &&
        endpoint_source_->receipt().session == client_.session_id())
        endpoint_source_->poll(now);
    if (client_.state() == VoiceState::Failed) {
        fail(now, RuntimeFailure::ClientTick);
        return;
    }
    if (client_.state() == VoiceState::Idle) {
        if (client_.endpoint_waiting()) return;
        if (!begin_drain(now))
            close_observed(now, VoiceCloseCause::DrainSettled);
        return;
    }
    if (client_.state() != VoiceState::Negotiating && client_.state() != VoiceState::Established &&
        client_.state() != VoiceState::Recovering)
        return;
    // 独立工具在None的空媒体早退前推进；原截止和取消栅栏由端点限制结果重传。
    if (client_tools_ && !client_tools_->poll(now)) {
        fail(now, RuntimeFailure::ToolPoll);
        return;
    }
    if (!media_)
        return;  // None等待真实会话/内容事件；没有mediaState、Ready、SDP或播放排空阶段。
    if (!media_->poll(now)) {
        fail(now, RuntimeFailure::MediaPoll);
        return;
    }
    // 网络暂断只暂停本AI媒体，不关闭共享MQTT或其它腿；下一代Peer须等原断连ACK。
    if (client_.state() == VoiceState::Established && media_->recovery_needed()) {
        if (client_.media_disconnected(now) != Result::Applied) {
            fail(now, RuntimeFailure::MediaPoll);
            return;
        }
        media_reported_ = false;
    }
    if (client_.media_negotiating()) {
        const auto offer = media_->take_offer();
        if (offer && client_.media_offer_ready(*offer, now) != Result::Applied) {
            fail(now, RuntimeFailure::MediaOffer);
            return;
        }
    }
    if (media_->ready() && !media_reported_) {
        if (client_.media_ready(now) != Result::Applied) {
            fail(now, RuntimeFailure::MediaReady);
            return;
        }
        media_reported_ = true;
    }
    if (const auto epoch = media_->drained_epoch())
        client_.playback_drained(*epoch);
}
}  // namespace ur::device
