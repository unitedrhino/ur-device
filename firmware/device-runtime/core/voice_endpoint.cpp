// 文件说明：AI候选端监听/明确接听/Held/双提交与原焦点准入；只复用原语音媒体和Session，不创建模型。
#include <cJSON.h>

#include <algorithm>
#include <memory>

#include "ur/device/endpoint_wire.hpp"
#include "ur/device/focus_endpoint.hpp"
#include "ur/device/json_parse.hpp"
#include "voice_endpoint_state.hpp"
#include "voice_failure_storage.hpp"

namespace ur::device {
namespace {
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;  // 固定ACK解析树不逃出事件线程。
// same读取固定枚举，错误正文不进入授权或诊断。
bool same(const cJSON* object, const char* key, std::string_view expected) {
    const auto* value = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(value) && value->valuestring &&
           std::string_view(value->valuestring) == expected;
}
}  // namespace

// endpoint_phase仅投影闭集；源端初建仍由原VoiceState管理，不把功能声明当候选邀请。
AIEndpointPhase VoiceClient::endpoint_phase() const {
    if (!capability_.ai_endpoint_handoff) return AIEndpointPhase::Disabled;
    if (state_ == VoiceState::Failed) return AIEndpointPhase::Failed;
    return endpoint_ ? endpoint_->phase : AIEndpointPhase::Disabled;
}
// endpoint_waiting只保持显式已报能力监听连接，不保留已结束候选占用或旧模型连接。
bool VoiceClient::endpoint_waiting() const {
    return endpoint_ && endpoint_->listening &&
           (endpoint_->phase == AIEndpointPhase::Listening ||
            endpoint_->phase == AIEndpointPhase::Invited);
}
// connect_listening复用原Topic/身份/能力握手；只接受已装配原媒体端口的UDP或WebRTC候选。
Result VoiceClient::connect_listening(std::uint64_t now) {
    if (!capability_.ai_endpoint_handoff ||
        (capability_.transport != VoiceTransport::Udp &&
         capability_.transport != VoiceTransport::WebRTC) ||
        capability_.ai_vision || !focus_ || !media_)
        return Result::Unsupported;
    if (state_ != VoiceState::Idle && state_ != VoiceState::Failed) return Result::Busy;
    endpoint_ = std::make_unique<AIEndpointState>();
    endpoint_->listening = true;
    endpoint_->now = now;
    const auto result = connect_mode(now, false);
    if (result != Result::Applied) endpoint_.reset();
    return result;
}
// candidate_focus_scope在Active仍保留原绑定供可信authority复核；期限仅约束接续建立，不续原焦点。
std::optional<AIEndpointFocusScope> VoiceClient::candidate_focus_scope() const {
    if (!endpoint_ || !endpoint_->candidate || !endpoint_->accept_acked || !endpoint_->committed ||
        !media_reported_ || !media_acked_ || !established_event_ ||
        (endpoint_->phase != AIEndpointPhase::AwaitingFocus &&
         endpoint_->phase != AIEndpointPhase::Active) ||
        (state_ != VoiceState::Negotiating && state_ != VoiceState::Established &&
         !(state_ == VoiceState::Recovering && capability_.ai_endpoint_control_sync &&
           endpoint_->control_recovery_started)))
        return {};
    return AIEndpointFocusScope{session_id_, selected_.input_track, selected_.output_track,
                                stream_watermark_, endpoint_->binding.endpoint_epoch,
                                endpoint_->binding.capability_revision};
}
// endpoint_establish_gate复核原FocusEngine当前整组许可及回执交付，不借observer历史或HAL受理。
bool VoiceClient::endpoint_establish_gate(bool observe_failure) const {
    return endpoint_ && endpoint_establish_gate(endpoint_->now, endpoint_->wall, observe_failure);
}
// endpoint_establish_gate 使用本次调用者的两份时钟复核原完整组，不能将上轮缓存查询冒充新的物理时钟。
bool VoiceClient::endpoint_establish_gate(std::uint64_t now, std::uint64_t wall,
                                          bool observe_failure) const {
    // 本人新意图必须携当前快照；排队旧快照只拒请求，不让它进入authority的物理回拨检测。
    if (!endpoint_ || !wall || now < endpoint_->now || wall < endpoint_->wall)
        return false;
    endpoint_->now = now;
    endpoint_->wall = wall; // 仅记录已观察时钟；started、邀请截止与原资源租约均不续长。
    ScopeFailure failure;  // 仅Active tick的最终false可提交；准备/邀请中的正常等待不锁存。
    auto* observation =
        observe_failure && recovery_.failure_diagnostics_enabled ? &failure : nullptr;
    const auto scope = candidate_focus_scope();
    const bool permitted =
        !scope    ? scope_reject(observation, ScopeFailureCode::CandidateBinding)
        : !focus_ ? scope_reject(observation, ScopeFailureCode::CandidateEndpoint)
                  : focus_->scoped_permitted(scope->session_id, {"microphone", "speaker"},
                                             now, wall, observation);
    if (!permitted && observation && !failure_diagnostic_ && failure.code != ScopeFailureCode::None)
        failure_diagnostic_ = encode_scope_failure(failure.code);
    return permitted;
}
// endpoint_control_rebinding只认已明确接入的新目标，不从source的普通同步或等待邀请推断恢复。
bool VoiceClient::endpoint_control_rebinding() const {
    return capability_.ai_endpoint_control_sync && endpoint_ && endpoint_->candidate &&
           endpoint_->control_recovery_started.has_value();
}
// endpoint_control_baseline_ready只读原当前恢复记录，不消费旧capabilityReport ACK。
bool VoiceClient::endpoint_control_baseline_ready() const {
    return endpoint_control_rebinding() && endpoint_->control_baseline_acked;
}
// report_endpoint_control_baseline保留完整caprev，仅补真实当前资源idle；token新建且原ACK关联。
Result VoiceClient::report_endpoint_control_baseline(const FocusIdleObservation& idle,
                                                     std::uint64_t now) {
    if (!endpoint_control_rebinding() || control_recovering() || !focus_ ||
        state_ != VoiceState::Established || endpoint_->phase != AIEndpointPhase::AwaitingFocus ||
        endpoint_->control_baseline_sent || !idle.revision) return Result::Stale;
    // 发送前固定原首次恢复起点；发布端禁止同步重入Requests，不从跨方法判断推断optional可取值。
    const auto recovery_started = endpoint_->control_recovery_started;
    if (!recovery_started.has_value()) return Result::Stale;
    Envelope source, report;
    if (capability_command(next_id(), capability_, source) != WireResult::Ok ||
        focus_->recovery_capability(source, session_id_, report) != WireResult::Ok ||
        add_focus_baseline(report, idle.resources, idle.revision, report) != WireResult::Ok ||
        !send(report, "endpointControlBaseline", now)) return Result::Busy;
    endpoint_->baseline_retry = AIEndpointState::BaselineRetry{report.token,
                                                               report.session,
                                                               session_id_,
                                                               endpoint_->binding,
                                                               recovery_started.value(),
                                                               now,
                                                               now,
                                                               0};
    endpoint_->control_baseline_sent = true;
    return Result::Applied;
}
// accept_endpoint_invite等待原接听ACK才安装媒体；未确认前不向UDP探测器提交短期配置。
Result VoiceClient::accept_endpoint_invite(std::uint64_t now, std::uint64_t wall) {
    if (!endpoint_ || !endpoint_->candidate || endpoint_->phase != AIEndpointPhase::Invited)
        return Result::Stale;
    if (now < endpoint_->now || wall < endpoint_->wall || !wall ||
        wall >= endpoint_->invite.expires_at || now - endpoint_->started >= 30000)
        return Result::Stale;
    Envelope request;
    request.token = endpoint_->invite.envelope.token;
    request.session = session_id_;
    request.method = "sessionAccept";
    // 轨ID已由严格邀请解析冻结；固定两轨，不从网页或用户正文拼接能力。
    Json params(cJSON_CreateObject(), cJSON_Delete);
    auto* tracks = params ? cJSON_AddArrayToObject(params.get(), "acceptedTracks") : nullptr;
    if (!tracks ||
        !cJSON_AddItemToArray(tracks,
                              cJSON_CreateString(endpoint_->invite.media.input_track.c_str())) ||
        !cJSON_AddItemToArray(tracks,
                              cJSON_CreateString(endpoint_->invite.media.output_track.c_str())))
        return Result::Invalid;
    char* encoded = cJSON_PrintUnformatted(params.get());
    if (!encoded) return Result::Invalid;
    request.payload = encoded;
    cJSON_free(encoded);
    if (!send(request, {}, now)) return Result::Busy;
    endpoint_->phase = AIEndpointPhase::Accepting;
    endpoint_->now = now;
    endpoint_->wall = wall;
    state_ = VoiceState::Creating;
    return Result::Applied;
}
// reject_endpoint_invite使用AI候选既有userRejected枚举，不发送源腿sessionEnd/abort；人际declined不受影响。
Result VoiceClient::reject_endpoint_invite(std::uint64_t now, std::uint64_t wall) {
    if (!endpoint_ || !endpoint_->candidate || endpoint_->phase != AIEndpointPhase::Invited)
        return Result::Stale;
    if (now < endpoint_->now || !wall || wall < endpoint_->wall ||
        wall >= endpoint_->invite.expires_at || now - endpoint_->started >= 30000)
        return Result::Stale;
    Envelope request;
    request.token = endpoint_->invite.envelope.token;
    request.session = session_id_;
    request.method = "sessionReject";
    request.payload = R"({"reason":"userRejected"})";
    if (!send(request, {}, now)) return Result::Busy;
    endpoint_->phase = AIEndpointPhase::Ending;
    endpoint_->now = now;
    endpoint_->wall = wall;
    state_ = VoiceState::Ending;
    return Result::Applied;
}
// endpoint_tick保持原固定30秒和双时钟；Active后只读原实时焦点许可，撤权即沿原终态停媒体。
void VoiceClient::endpoint_tick(std::uint64_t now, std::uint64_t wall) {
    if (!endpoint_ || !endpoint_->candidate) return;
    if (!wall || now < endpoint_->now || wall < endpoint_->wall) {
        release(VoiceState::Failed, now, VoiceFailureCause::CandidateClock);
        return;
    }
    endpoint_->now = now;
    endpoint_->wall = wall;
    // 仅当前原基线等待可快重发；同一次tick最多一次，迟到tick不补发风暴，原请求寿命不被快调度预算改变。
    // 同一原调度记录显式核存在；首次恢复起点另取只读快照，不能被快重投延期。
    auto& baseline_retry = endpoint_->baseline_retry;
    if (baseline_retry.has_value()) {
        auto& retry = baseline_retry.value();
        const auto retry_recovery_started = endpoint_->control_recovery_started;
        const RequestKey key{up_topic_, "capabilityReport", retry.token};
        const bool current =
            state_ == VoiceState::Established && !control_recovering() &&
            endpoint_->phase == AIEndpointPhase::AwaitingFocus &&
            !endpoint_->control_baseline_acked && retry_recovery_started.has_value() &&
            retry_recovery_started.value() == retry.recovery_started &&
            session_id_ == retry.public_session &&
            endpoint_->binding.endpoint_epoch == retry.binding.endpoint_epoch &&
            endpoint_->binding.capability_revision == retry.binding.capability_revision;
        if (!current || now < retry.started || now < retry.last || now < retry.recovery_started ||
            now - retry.recovery_started >= 15000 ||
            requests_.state(key) != RequestState::Pending) {
            // 来源换代只退休本人的原请求；已成功或已拒绝对象只停止快调度，其他请求不受影响。
            if (!current)
                requests_.cancel_request(key, now);
            endpoint_->baseline_retry.reset();
        } else if (retry.attempts < 12 && now - retry.started <= 2400 && now - retry.last >= 200) {
            retry.last = now;
            ++retry.attempts; // 背压同样消耗本次尝试，不在同now busy loop。
            requests_.republish_pending(key, retry.request_session, now);
        }
    }
    // 本人结束已关闭采播，不能再用建立屏障判断授权；原sessionEnd请求仍受原30秒截止约束。
    // 原双时钟检查保留；Ending不提供candidate_focus_scope，拒接的原建立期限也不受影响。
    if (state_ == VoiceState::Ending && endpoint_->phase != AIEndpointPhase::Ending)
        return;
    // 冻结本次检查的首次恢复起点；后续恢复完成只清原记录，不改变本次15秒判断。
    const auto recovery_started = endpoint_->control_recovery_started;
    if (recovery_started.has_value()) {
        const auto started = *recovery_started;
        if (now < started || now - started >= 15000) {
            release(VoiceState::Failed, now, VoiceFailureCause::CandidateRecoveryDeadline);
            return;
        }
        if (control_recovering()) return; // 新socket快照未确认前只保留原关闭资格，不授新整组。
        if (endpoint_->phase == AIEndpointPhase::AwaitingFocus &&
            endpoint_->control_baseline_acked && endpoint_establish_gate()) {
            endpoint_->phase = AIEndpointPhase::Active;
            endpoint_->control_recovery_started.reset(); // 新实际取得和原回执交付结束恢复预算。
        }
        return;
    }
    if (endpoint_->phase != AIEndpointPhase::Active &&
        (now - endpoint_->started >= 30000 || wall >= endpoint_->invite.expires_at)) {
        release(VoiceState::Failed, now, VoiceFailureCause::CandidateEstablishDeadline);
        return;
    }
    if (endpoint_->phase == AIEndpointPhase::Active) {
        if (!endpoint_establish_gate(true))
            release(VoiceState::Failed, now, VoiceFailureCause::ActiveScopeLost);
        return;
    }
    if (endpoint_->accept_acked && endpoint_->committed && media_reported_ && media_acked_ &&
        established_event_) {
        endpoint_->phase = AIEndpointPhase::AwaitingFocus;
        if (establish() == Result::DriverFailed) release(VoiceState::Failed, now);
    }
}
// endpoint_receive只消费显式接续相关方法；源腿水位与候选水位独立，原字节幂等不提前授媒体许可。
std::optional<Result> VoiceClient::endpoint_receive(IncomingMessage message, std::uint64_t now,
                                                    std::uint64_t wall) {
    if (!capability_.ai_endpoint_handoff || message.topic != down_topic_) return {};
    Envelope envelope;
    if (decode_envelope(message.body, envelope) != DecodeResult::Ok) return {};
    if (endpoint_ && endpoint_->candidate) {
        if (!wall || now < endpoint_->now || wall < endpoint_->wall) return Result::Stale;
        if ((state_ != VoiceState::Ending || endpoint_->phase == AIEndpointPhase::Ending) &&
            endpoint_->phase != AIEndpointPhase::Active && !endpoint_->control_recovery_started &&
            (now - endpoint_->started >= 30000 || wall >= endpoint_->invite.expires_at)) {
            release(VoiceState::Failed, now, VoiceFailureCause::CandidateEstablishDeadline);
            return Result::Stale;
        }
        endpoint_->now = now;
        endpoint_->wall = wall;
    }
    if (envelope.method == "sessionInvite") {
        if (!endpoint_ || !endpoint_->listening) return Result::Stale;
        if (endpoint_->candidate)
            return endpoint_->invite_body == message.body ? Result::Duplicate : Result::Busy;
        AIEndpointInvite invite;
        if (decode_ai_endpoint_invite(WireRoute::Signaling, message.body, capability_, true,
                                      invite) != WireResult::Ok ||
            !envelope.event_sequence.has_value())
            return Result::Invalid;
        // 先保存原邀请已核验的事件水位，再安装候选；缺字段不能留下部分候选状态。
        const auto sequence = *envelope.event_sequence;
        if (endpoint_->phase != AIEndpointPhase::Listening || !wall || wall < endpoint_->wall ||
            now < endpoint_->now || invite.expires_at <= wall || invite.expires_at - wall > 30000)
            return Result::Stale;
        endpoint_->invite = std::move(invite);
        endpoint_->invite_body = std::string(message.body);
        endpoint_->started = endpoint_->now = now;
        endpoint_->wall = wall;
        endpoint_->candidate = true;
        endpoint_->phase = AIEndpointPhase::Invited;
        session_id_ = envelope.session;
        event_seq_ = sequence;
        return Result::Applied;
    }
    if (!endpoint_) return {};
    if (envelope.method == "endpointState") {
        EndpointState fact;
        if (decode_endpoint_state(WireRoute::Signaling, message.body, fact) != WireResult::Ok)
            return Result::Invalid;
        const auto scope = endpoint_->candidate ? endpoint_->invite.source_session : session_id_;
        if (scope.empty() || fact.envelope.session != scope || !fact.envelope.event_sequence ||
            fact.envelope.code)
            return Result::Stale;
        if (endpoint_->candidate && (fact.handoff_id != endpoint_->invite.handoff_id ||
                                     ((fact.state == "ready" || fact.state == "committed") &&
                                      fact.candidate_session_id != session_id_)))
            return Result::Stale;
        const auto sequence = *fact.envelope.event_sequence;
        for (const auto& saved : endpoint_->facts)
            if (saved.token == fact.envelope.token || saved.sequence == sequence)
                return saved.body == message.body ? Result::Duplicate : Result::Invalid;
        auto& watermark = endpoint_->candidate ? endpoint_->source_watermark : event_seq_;
        if (sequence <= watermark) return Result::Stale;
        // 已确认提交是原接续事务的终态；更大投递序号不是重开/撤销权限。
        // 迟到准备、失败或abort不得关掉新腿，正式关闭仍须候选腿sessionEnded或实际焦点撤权。
        if (endpoint_->candidate && endpoint_->committed && fact.state != "committed")
            return Result::Stale;
        const auto bytes =
            sizeof(AIEndpointState::Fact) + fact.envelope.token.size() + message.body.size();
        if (endpoint_->facts.size() >= 128 || bytes > 65536 - endpoint_->fact_bytes) {
            release(VoiceState::Failed, now);
            return Result::Busy;
        }
        endpoint_->facts.push_back({fact.envelope.token, std::string(message.body), sequence});
        endpoint_->fact_bytes += bytes;
        watermark = sequence;
        if (endpoint_->candidate) {
            if (fact.state == "failed" || fact.state == "aborted") {
                release(VoiceState::Idle, now);
                return Result::Applied;
            }
            if (fact.state == "committed") {
                // 只保留已核原source/handoff/candidate的首次提交；旧事务重投不得重写二跳水位。
                if (!endpoint_->committed)
                    endpoint_->committed_revision = fact.revision;
                endpoint_->committed = true;
            }
            endpoint_tick(now, wall);
        }
        return Result::Applied;
    }
    if (endpoint_->candidate && envelope.method == "requestAck") {
        Json data(parse_json(envelope.payload.c_str()), cJSON_Delete);
        const bool accept = same(data.get(), "requestMethod", "sessionAccept");
        const bool reject = same(data.get(), "requestMethod", "sessionReject");
        if (!accept && !reject) return {};
        if (!envelope.code || envelope.session != session_id_ || !data ||
            cJSON_GetArraySize(data.get()) != 2)
            return Result::Invalid;
        const auto method = accept ? "sessionAccept" : "sessionReject";
        const auto found = std::find_if(pending_.begin(), pending_.end(), [&](const auto& item) {
            return item.token == envelope.token && item.method == method;
        });
        if (found == pending_.end()) return Result::Stale;
        const auto* result = cJSON_GetObjectItemCaseSensitive(data.get(), "result");
        if (*envelope.code == 200) {
            if (!cJSON_IsObject(result) ||
                !same(result, "state", accept ? "negotiating" : "ended"))
                return Result::Invalid;
            // 原Go拒接ACK回显本次固定userRejected；仅state的既有ACK仍有效。
            // 接听不携reason，其他原因或未知键不能消费原pending或重建媒体。
            const auto* reason = reject ? cJSON_GetObjectItemCaseSensitive(result, "reason") : nullptr;
            if ((reason && !same(result, "reason", "userRejected")) ||
                cJSON_GetArraySize(result) != (reason ? 2 : 1))
                return Result::Invalid;
        }
        if (*envelope.code != 200 &&
            !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(data.get(), "reason")))
            return Result::Invalid;
        if (!requests_.acknowledge({envelope.token, session_id_, method, *envelope.code, up_topic_},
                                   now))
            return Result::Stale;
        pending_.erase(found);
        if (*envelope.code != 200) {
            release(VoiceState::Failed, now, VoiceFailureCause::RequestRejected,
                    voice_failure_method(method));
            return Result::Unsupported;
        }
        if (reject) {
            release(VoiceState::Idle, now);
            return Result::Applied;
        }
        if (endpoint_->phase != AIEndpointPhase::Accepting) return Result::Stale;
        selected_ = endpoint_->invite.media;
        stream_watermark_ = 1;
        endpoint_->accept_acked = true;
        if (!media_->configure(selected_)) {
            release(VoiceState::Failed, now);
            return Result::DriverFailed;
        }
        phase_started_ = now;
        state_ = VoiceState::Negotiating;
        endpoint_->phase = AIEndpointPhase::Preparing;
        return Result::Applied;
    }
    return {};
}
}  // namespace ur::device
