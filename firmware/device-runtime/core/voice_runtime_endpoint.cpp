// 文件说明：AI原源腿接续控制与候选Held-only焦点准备；共享原MQTT、会话和事件水位，不重开模型。
#include <algorithm>

#include "voice_endpoint_state.hpp"
#include "voice_runtime_endpoint_state.hpp"
#include "udp_reply_receipt_state.hpp"

namespace ur::device {
// shared_reply_start_needs_relation仅当前Shared
// source会话；目标独立MQTT不经此路由，candidate保守拒绝。
// 不授予许可、不消费原Start；只给Host登记已交付来源所需的纯事实，不代替目标接续验收。
bool VoiceRuntime::shared_reply_start_needs_relation(IncomingMessage message) const {
    VoiceEvent event;
    if (!borrowed_connection_ || state_ != RuntimeState::Running ||
        !client_.selected_.udp_reply_start_receipt || client_.control_recovering() ||
        message.topic != signaling_topic_ ||
        decode_voice_event(WireRoute::Signaling, message.body, event) != WireResult::Ok ||
        event.envelope.method != "respAudioStart" ||
        event.envelope.session != client_.session_id_ || !event.envelope.event_sequence ||
        *event.envelope.event_sequence <= client_.event_seq_ || client_.recovery_.focus_suspended ||
        (client_.endpoint_ && client_.endpoint_->candidate) || !client_.udp_reply_receipts_ ||
        !client_.udp_reply_receipts_->shared_managed ||
        !client_.udp_reply_receipts_->shared_owned || client_.udp_reply_receipts_->shared_permanent)
        return false;
    Envelope receipt;
    return client_.reply_start_admission(event) == Result::Applied &&
           reply_start_receipt(event, receipt) == WireResult::Ok;
}
// shared_reply_install_gate 不构造外部许可，只同步可信宿主原完整组失权；不给旧HAL或原票据恢复权限。
Result VoiceRuntime::shared_reply_install_gate(std::string_view original, bool owned,
                                               bool permanent, std::uint64_t now) {
    if (!borrowed_connection_ || !client_.selected_.udp_reply_start_receipt)
        return Result::Duplicate;  // 旧defaultfalse/省略selected完全沿原语义。
    if (state_ != RuntimeState::Running || client_.state() != VoiceState::Established ||
        original.empty() || original != client_.session_id_ || (now_ && now < *now_))
        return Result::Stale;
    if (!client_.udp_reply_receipts_)
        client_.udp_reply_receipts_ = std::make_unique<VoiceClient::UDPReplyReceiptState>();
    auto& ledger = *client_.udp_reply_receipts_;
    ledger.shared_managed = true;
    ledger.shared_permanent = ledger.shared_permanent || permanent;
    ledger.shared_owned = owned && !ledger.shared_permanent;
    if (!ledger.shared_owned) {
        ledger.retire();  // 原512摘要/首次截止不清、不续；publish背压也不可保留待发许可。
        if (client_.session_ && !client_.response_id_.empty()) {
            client_.session_->cancel_response(client_.session_id_, client_.response_id_);
            client_.reply_finished_ = true;  // 原媒体停止，非平台respCancel或网络释放。
        }
    }
    return Result::Applied;
}
// suspend_input_for_focus 只接唯一可信宿主已核权的完整原许可；网页与网络帧不能直接调用。
// 平台已先停止原模型输入，因此不补发无法证明UDP末包边界的audioStop，也不更新用户意图版本。
Result VoiceRuntime::suspend_input_for_focus(const FocusPermit& original, std::uint64_t now) {
    const auto& command = original.scope;
    const std::vector<std::string> resources{"microphone", "speaker"}; // 原AI完整原子组。
    std::string bytes; // 重新核对原信封，防止宿主结构体字段与实际params分离。
    FocusCommand decoded;
    if (encode_envelope(command.envelope, bytes) != DecodeResult::Ok ||
        decode_focus_control(WireRoute::Signaling, bytes, decoded) != WireResult::Ok ||
        decoded.operation != command.operation || decoded.focus_id != command.focus_id ||
        decoded.expected_revision != command.expected_revision ||
        decoded.resources != command.resources || decoded.priority != command.priority ||
        decoded.resume_policy != command.resume_policy || decoded.expires_at != command.expires_at)
        return Result::Stale;
    if (state_ != RuntimeState::Running || client_.state() != VoiceState::Established ||
        client_.control_recovering() || !client_.session_ ||
        (now_ && now < *now_) || now >= original.deadline || !original.intent_version ||
        command.envelope.session != client_.session_id() || command.envelope.method != "focusControl" ||
        !command.envelope.request || command.envelope.code || command.envelope.message_sequence ||
        !command.envelope.event_sequence || !*command.envelope.event_sequence ||
        command.envelope.token.empty() || command.envelope.token.size() > 128 ||
        command.focus_id.empty() || command.focus_id.size() > 128 ||
        command.operation != "suspend" || command.expires_at ||
        command.priority != "conversation" || command.resume_policy != "explicit" ||
        command.resources.size() != resources.size() ||
        !std::is_permutation(command.resources.begin(), command.resources.end(), resources.begin()))
        return Result::Stale;
    const bool already = client_.recovery_.focus_suspended && !client_.want_capture_ &&
                         !client_.capturing() && !client_.playing();
    client_.want_capture_ = false;
    client_.recovery_.input_acked = false;
    client_.recovery_.focus_suspended = true;
    if (!client_.input_id_.empty())
        client_.session_->stop_input(client_.session_id_, client_.input_id_);
    if (!client_.response_id_.empty()) {
        client_.session_->cancel_response(client_.session_id_, client_.response_id_);
        client_.reply_finished_ = true; // 保留原回复及排空代次墓碑，不发respCancel或清事件水位。
    }
    // 精确退休旧输入的等待，不重传旧audioStart，也不误取消同腿工具或会话控制请求。
    auto& pending = client_.pending_;
    pending.erase(std::remove_if(pending.begin(), pending.end(), [&](const auto& request) {
        if ((request.method != "audioStart" && request.method != "audioStop") ||
            request.object != client_.input_id_) return false;
        client_.requests_.cancel_request({client_.up_topic_, request.method, request.token}, now);
        return true;
    }), pending.end());
    return already ? Result::Duplicate : Result::Applied;
}
// endpoint_source_intent_available只读原成功事件泵后的本机状态，不用UI读取重复执行权限检查。
// Active源于原实际完整组取得；许可在读取后仍可能被撤销，提交时必须再次走原精确gate。
bool VoiceRuntime::endpoint_source_intent_available() const {
    return endpoint_source_ && state_ == RuntimeState::Running &&
           client_.state() == VoiceState::Established && !client_.control_recovering() &&
           !client_.session_id().empty() &&
           (client_.control_sync_enabled() ||
            (client_.capability_.ai_endpoint_handoff &&
             client_.endpoint_phase() == AIEndpointPhase::Active));
}
// endpoint_source_available 用当前单调/Unix毫秒复核已建立腿，断连或候选Held期间不得操作。
bool VoiceRuntime::endpoint_source_available(std::uint64_t now, std::uint64_t wall) const {
    if (!wall) return false; // 显式Unix快照缺失不能借单调时间推测权限。
    // 接管后的公开腿没有旧控制恢复绑定；只有原候选已Active且实际完整组仍获权才可再次发起。
    const bool acquired_candidate = client_.capability_.ai_endpoint_handoff &&
                                    client_.endpoint_phase() == AIEndpointPhase::Active &&
                                    client_.endpoint_establish_gate(now, wall);
    return endpoint_source_ && state_ == RuntimeState::Running &&
           client_.state() == VoiceState::Established && !client_.control_recovering() &&
           (client_.control_sync_enabled() || acquired_candidate) && !client_.session_id().empty() &&
           (!now_ || now >= *now_);
}
// prepare_endpoint_source 用当前now/wall绑定核心腿；仅提交原请求，不停源采播或假造目标就绪。
Result VoiceRuntime::prepare_endpoint_source(std::string_view target, std::uint64_t now, std::uint64_t wall) {
    if (!endpoint_source_) return Result::Unsupported;
    if (!endpoint_source_available(now, wall)) return Result::Stale;
    // 候选已由原精确gate核完整组取得，再从已验证提交继承CAS；普通首源仍从原零水位开始。
    const bool candidate = client_.endpoint_ && client_.endpoint_->candidate;
    const bool bound = candidate
                           ? (client_.endpoint_->committed &&
                              endpoint_source_->bind_committed_candidate(
                                  client_.session_id(), client_.endpoint_->committed_revision, now))
                           : endpoint_source_->bind(client_.session_id(), now);
    if (!bound)
        return Result::Stale;
    return endpoint_source_->prepare(target, now) ? Result::Applied : Result::Busy;
}
// commit_endpoint_source 用当前now/wall核ready、候选与修订；仍等正式源终态才关闭HAL。
Result VoiceRuntime::commit_endpoint_source(std::uint64_t now, std::uint64_t wall) {
    if (!endpoint_source_) return Result::Unsupported;
    if (!endpoint_source_available(now, wall) ||
        endpoint_source_->receipt().session != client_.session_id()) return Result::Stale;
    return endpoint_source_->commit(now) ? Result::Applied : Result::Busy;
}
// abort_endpoint_source 用当前now/wall保留原handoff及token，未知提交不能按本地猜测撤回。
Result VoiceRuntime::abort_endpoint_source(std::uint64_t now, std::uint64_t wall) {
    if (!endpoint_source_) return Result::Unsupported;
    if (!endpoint_source_available(now, wall) ||
        endpoint_source_->receipt().session != client_.session_id()) return Result::Stale;
    return endpoint_source_->abort(now) ? Result::Applied : Result::Busy;
}
// endpoint_source_receipt 终态后仍可读有限原事实，不授予操作或恢复媒体权限。
const EndpointHandoffReceipt* VoiceRuntime::endpoint_source_receipt() const {
    return endpoint_source_ ? &endpoint_source_->receipt() : nullptr;
}
// endpoint_source_receive 仅从原SIG消费源控制器事实；ACK与主动事件不能相互冒充。
bool VoiceRuntime::endpoint_source_receive(IncomingMessage message, std::uint64_t now) {
    if (!endpoint_source_ || message.topic != signaling_topic_ || client_.control_recovering())
        return false;
    const auto& receipt = endpoint_source_->receipt();
    if (receipt.session.empty() || receipt.session != client_.session_id()) return false;
    Envelope envelope;
    if (decode_envelope(message.body, envelope) != DecodeResult::Ok ||
        envelope.session != receipt.session) return false;
    if (endpoint_handoff_ingress_ack(message.body)) {
        endpoint_source_->receive(message.topic, message.body, now);
        return true;  // 原负向入口回执只作用原请求，不能使VoiceClient误判为非法输入ACK。
    }
    if (envelope.method != "endpointState") return false;
    EndpointState fact;
    if (decode_endpoint_state(WireRoute::Signaling, message.body, fact) != WireResult::Ok ||
        fact.handoff_id != receipt.handoff) return true;
    // 所有原SIG事件共用同一水位，不能让较旧ready通过独立控制器覆盖已观察的较新事实。
    if (fact.envelope.event_sequence && *fact.envelope.event_sequence <= client_.event_seq_)
        return true;
    if (endpoint_source_->receive(message.topic, message.body, now) &&
        fact.envelope.event_sequence)
        client_.event_seq_ = *fact.envelope.event_sequence;
    return true;
}
// set_endpoint_focus_prepare只接受同一明确接续能力/真实焦点宿主，禁止联网后替换HAL授权源。
Result VoiceRuntime::set_endpoint_focus_prepare(EndpointFocusPreparePort& port) {
    if (state_ != RuntimeState::Idle || borrowed_connection_) return Result::Busy;
    if (!client_.capability_.ai_endpoint_handoff || !focus_ || !media_) return Result::Unsupported;
    if (client_.capability_.ai_endpoint_control_sync && !port.control_recovery_supported())
        return Result::Unsupported; // 没有实际恢复HAL不得只凭feature声明控制恢复。
    if (endpoint_ && endpoint_->port != &port) return Result::Busy;
    if (!endpoint_) endpoint_ = std::make_unique<EndpointRuntime>();
    endpoint_->port = &port;
    return Result::Applied;
}
// start_listening只从显式完整HAL装配开始，原start负责鉴权/订阅/时间/生命周期核验。
Result VoiceRuntime::start_listening(const MqttConfig& config, std::uint64_t now) {
    if (!endpoint_ || !endpoint_->port || !endpoint_->port->settled() ||
        (client_.capability_.transport != VoiceTransport::Udp &&
         client_.capability_.transport != VoiceTransport::WebRTC))
        return Result::Unsupported;
    const auto result = start(config, now);
    if (result == Result::Applied) {
        endpoint_->listening = true;
        endpoint_->cleanup_session.clear(); // 新监听只在旧实际收尾完成后开始，不把旧清理腿借给新会话。
    }
    return result;
}
// accept_endpoint_invite是本地明确选择，不能由下行invite、媒体ready或焦点控制代为触发。
Result VoiceRuntime::accept_endpoint_invite(std::uint64_t now, std::uint64_t wall) {
    if (state_ != RuntimeState::Running || !endpoint_ || !endpoint_->listening || !endpoint_->port)
        return Result::Stale;
    return client_.accept_endpoint_invite(now, wall);
}
// reject_endpoint_invite仅拒候选当前腿，运行时没有源端abort/commit自动选择权。
Result VoiceRuntime::reject_endpoint_invite(std::uint64_t now, std::uint64_t wall) {
    if (state_ != RuntimeState::Running || !endpoint_ || !endpoint_->listening)
        return Result::Stale;
    return client_.reject_endpoint_invite(now, wall);
}
// endpoint_focus先冻结严格原请求，再由宿主异步关双向/绑定scope；未提交不能提前驱动原焦点。
bool VoiceRuntime::endpoint_focus(IncomingMessage message, std::uint64_t now, std::uint64_t wall) {
    if (!endpoint_ || !endpoint_->listening || !endpoint_->port || !focus_) return false;
    FocusCommand command;
    if (message.topic != signaling_topic_ ||
        decode_focus_control(WireRoute::Signaling, message.body, command) != WireResult::Ok)
        return false;
    if (command.envelope.session != client_.session_id()) return true;
    if (client_.endpoint_ && client_.endpoint_->candidate &&
        client_.state() == VoiceState::Ending) {
        // 只认本次真实候选；本人END后迟到acquire不得再创建准备或取消原HAL第二次。
        // 普通源及其speaker-only合同不走此分支，原完整release仍由原作用域与CAS严格核验。
        const std::vector<std::string> complete{"microphone", "speaker"};
        if (command.operation == "release" && command.resources.size() == complete.size() &&
            std::is_permutation(command.resources.begin(), command.resources.end(),
                                complete.begin()))
            focus_->receive(message.topic, message.body, now, wall);
        return true;
    }
    if (client_.endpoint_phase() == AIEndpointPhase::Active) {
        focus_->receive(message.topic, message.body, now, wall);
        return true;
    }
    const std::vector<std::string> resources{"microphone",
                                             "speaker"};  // 原完整原子组，不接受部分路径。
    if (client_.endpoint_control_rebinding()) {
        if (client_.control_recovering()) return true; // 未核当前快照前不消费新socket资源命令。
        if (command.resources.size() != resources.size() ||
            !std::is_permutation(command.resources.begin(), command.resources.end(), resources.begin()))
            return true;
        if (command.operation == "release") {
            focus_->receive(message.topic, message.body, now, wall); // 原关闭scope/CAS由Authority核验。
            return true;
        }
        if (!command.envelope.event_sequence ||
            *command.envelope.event_sequence <= client_.event_seq_) return true;
    }
    if (command.operation != "acquire" || command.resources.size() != 2 ||
        !std::is_permutation(command.resources.begin(), command.resources.end(), resources.begin()))
        return false;
    // 只比较已存在的原准备字节；重投不能替换token、资源或首次准备起点。
    const auto& pending = endpoint_->pending;
    if (pending.has_value()) {
        const auto& original = *pending;
        return original.topic == message.topic && original.body == message.body;
    }
    endpoint_->pending = EndpointRuntime::Pending{
        std::string(message.topic), std::string(message.body), std::move(command), now};
    return endpoint_prepare_tick(now, wall);
}
// endpoint_prepare_tick不刷新首次两秒；只把同一原字节交原FocusEndpoint一次，满槽继续有界等待。
bool VoiceRuntime::endpoint_prepare_tick(std::uint64_t now, std::uint64_t wall) {
    // 本人已关闭原AI时只取消本机尚未完成的准备；不能等原准备超时或让迟到事实再执行acquire。
    if (endpoint_ && client_.state() == VoiceState::Ending) {
        if (endpoint_->pending) {
            endpoint_->pending.reset();
            if (endpoint_->port)
                endpoint_->port->cancel();
        }
        return true;  // 结束ACK及请求原截止仍由VoiceClient推进，正式原完整release仍走FocusEndpoint。
    }
    if (endpoint_ && endpoint_->port && focus_ && client_.endpoint_control_rebinding() &&
        !client_.control_recovering() &&
        focus_->cleanup_settled([&](std::string_view session) {return session == client_.session_id();})) {
        const auto idle = endpoint_->port->control_idle_observation();
        if (idle && client_.report_endpoint_control_baseline(*idle, now) == Result::Busy) {
            record_endpoint_failure(VoiceFailureCause::EndpointPrepareDispatch);
            return false; // 正式release回执已排空，才提交本机真实当前全局revision基线。
        }
    }
    if (!endpoint_) return true;
    // 同一事件线程先核槽位再借用原记录；没有准备时不调用HAL或建立新的截止。
    auto& preparation = endpoint_->pending;
    if (!preparation.has_value()) return true;
    auto& pending = *preparation;
    if (!wall || now < pending.started) {
        record_endpoint_failure(VoiceFailureCause::EndpointPrepareClock);
        return false;
    }
    if (now - pending.started >= 2000) {
        record_endpoint_failure(VoiceFailureCause::EndpointPrepareDeadline);
        return false;
    }
    if (client_.state() == VoiceState::Idle || client_.state() == VoiceState::Failed) {
        // 客户端先失败时保持它的首因，不用准备Pending覆盖原sessionError或请求失败。
        if (client_.state() == VoiceState::Idle)
            record_endpoint_failure(VoiceFailureCause::Unknown);
        return false;
    }
    const auto scope = client_.candidate_focus_scope();
    if (!scope) return true;
    if (client_.endpoint_control_rebinding() && !client_.endpoint_control_baseline_ready())
        return true; // 缓存仍沿首次两秒，不因等待基线ACK刷新预算或提前执行新资源取得。
    const auto prepared = endpoint_->port->prepare(*scope, pending.command, now, wall);
    if (prepared == EndpointFocusPreparation::Pending) return true;
    if (prepared != EndpointFocusPreparation::Ready) {
        record_endpoint_failure(VoiceFailureCause::EndpointPrepareDriver);
        return false;
    }
    const auto result = focus_->receive(pending.topic, pending.body, now, wall);
    if (result == FocusEndpointResult::Full) return true;
    if (result != FocusEndpointResult::Handled) {
        record_endpoint_failure(VoiceFailureCause::EndpointPrepareDispatch);
        return false;
    }
    endpoint_->pending.reset();
    return true;
}
}  // namespace ur::device
