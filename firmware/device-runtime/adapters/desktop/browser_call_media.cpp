// 文件说明：WebRTC视频驱动只传递SDP和媒体启停命令，不持有设备密钥。
#include "browser_call_media.hpp"
#include <cJSON.h>
#include <algorithm>
#include <limits>
#include <memory>
#include "ur/device/json_parse.hpp"

namespace ur::device {
// browser_call_transfer_receipt 严格固定13字段；事件先到时不补造ACK，未知保持原未知。
std::string browser_call_transfer_receipt(const HumanCallTransferReceipt& receipt) {
    const auto safe_text = [](std::string_view value, std::size_t limit, bool empty) {
        return (empty || !value.empty()) && value.size() <= limit &&
               std::none_of(value.begin(), value.end(),
                            [](unsigned char ch) { return ch < 32 || ch == 127; });
    };
    const auto safe_code = [](int code) {
        return code == 0 || code == 200 || (code >= 400 && code <= 599);
    };
    const auto reject_reason = [](std::string_view value) {
        return value.empty() || value == "staleRevision" || value == "invalidState" ||
               value == "rejected";
    };
    const bool transfer_reason =
        reject_reason(receipt.reason) || receipt.reason == "candidateCancelled" ||
        receipt.reason == "transferRejected" || receipt.reason == "transferTimeout" ||
        receipt.reason == "transferMediaFailed" || receipt.reason == "transferUnavailable" ||
        receipt.reason == "transferRevisionChanged" ||
        receipt.reason == "transferPermissionRevoked";
    constexpr std::uint64_t max_safe = 9007199254740991ULL;
    if (!safe_text(receipt.session, 64, false) || !safe_text(receipt.token, 128, false) ||
        !safe_text(receipt.cancel_token, 128, true) || !safe_code(receipt.ack_code) ||
        !safe_code(receipt.cancel_code) || receipt.ack_revision > max_safe ||
        receipt.result_revision > max_safe || !transfer_reason ||
        !reject_reason(receipt.cancel_reason))
        return {};
    const char* state = nullptr;
    switch (receipt.state) {
        case HumanCallTransferState::Pending:
            state = "pending";
            break;
        case HumanCallTransferState::Transferring:
            state = "transferring";
            break;
        case HumanCallTransferState::Transferred:
            state = "transferred";
            break;
        case HumanCallTransferState::Failed:
            state = "failed";
            break;
        case HumanCallTransferState::Cancelled:
            state = "cancelled";
            break;
        case HumanCallTransferState::Rejected:
            state = "rejected";
            break;
        case HumanCallTransferState::Unknown:
            state = "unknown";
            break;
        default:
            return {};
    }
    const char* cancel_state = nullptr;
    switch (receipt.cancel_state) {
        case HumanCallControlState::Idle:
            cancel_state = "idle";
            break;
        case HumanCallControlState::Pending:
            cancel_state = "pending";
            break;
        case HumanCallControlState::Completed:
            cancel_state = "completed";
            break;
        case HumanCallControlState::Rejected:
            cancel_state = "rejected";
            break;
        case HumanCallControlState::Unknown:
            cancel_state = "unknown";
            break;
        default:
            return {};
    }
    using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
    Json value(cJSON_CreateObject(), cJSON_Delete);
    if (!value || !cJSON_AddStringToObject(value.get(), "kind", "callTransferReceipt") ||
        !cJSON_AddStringToObject(value.get(), "sessionId", receipt.session.c_str()) ||
        !cJSON_AddStringToObject(value.get(), "controlToken", receipt.token.c_str()) ||
        !cJSON_AddStringToObject(value.get(), "state", state) ||
        !cJSON_AddNumberToObject(value.get(), "code", receipt.ack_code) ||
        !cJSON_AddNumberToObject(value.get(), "ackRevision",
                                 static_cast<double>(receipt.ack_revision)) ||
        !cJSON_AddNumberToObject(value.get(), "resultRevision",
                                 static_cast<double>(receipt.result_revision)) ||
        !cJSON_AddStringToObject(value.get(), "reason", receipt.reason.c_str()) ||
        !cJSON_AddBoolToObject(value.get(), "candidatePresent", !receipt.candidate.empty()) ||
        !cJSON_AddStringToObject(value.get(), "cancelToken", receipt.cancel_token.c_str()) ||
        !cJSON_AddStringToObject(value.get(), "cancelState", cancel_state) ||
        !cJSON_AddNumberToObject(value.get(), "cancelCode", receipt.cancel_code) ||
        !cJSON_AddStringToObject(value.get(), "cancelReason", receipt.cancel_reason.c_str()))
        return {};
    auto* bytes = cJSON_PrintUnformatted(value.get());
    if (!bytes)
        return {};
    std::string body(bytes);
    cJSON_free(bytes);
    return body;
}
// browser_call_control_receipt JSON所有权留在既有编码适配，桌面入口不增加私有cJSON依赖。
std::string browser_call_control_receipt(const HumanCallControlReceipt& receipt) {
    using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
    Json value(cJSON_CreateObject(), cJSON_Delete);
    const char* state = receipt.state == HumanCallControlState::Completed  ? "completed"
                        : receipt.state == HumanCallControlState::Rejected ? "rejected"
                        : receipt.state == HumanCallControlState::Unknown  ? "unknown"
                                                                           : "pending";
    if (!value || !cJSON_AddStringToObject(value.get(), "kind", "callControlReceipt") ||
        !cJSON_AddStringToObject(value.get(), "sessionId", receipt.session.c_str()) ||
        !cJSON_AddStringToObject(value.get(), "controlToken", receipt.token.c_str()) ||
        !cJSON_AddStringToObject(value.get(), "operation", receipt.operation.c_str()) ||
        !cJSON_AddStringToObject(value.get(), "state", state) ||
        !cJSON_AddStringToObject(value.get(), "reason", receipt.reason.c_str()) ||
        !cJSON_AddNumberToObject(value.get(), "revision", static_cast<double>(receipt.revision)) ||
        !cJSON_AddNumberToObject(value.get(), "code", receipt.code) ||
        !cJSON_AddBoolToObject(value.get(), "actualHeld", receipt.actual_held) ||
        !cJSON_AddBoolToObject(value.get(), "actualApplied", receipt.actual_applied))
        return {};
    auto* bytes = cJSON_PrintUnformatted(value.get());
    if (!bytes)
        return {};
    std::string body(bytes);
    cJSON_free(bytes);
    return body;
}
// enable_hold 明确装配独立许可及实际端口，旧宿主不因编译进模块自动获得能力。
bool BrowserCallMedia::enable_hold(OutputFocusDriver& driver, FocusAuthority& authority,
                                   std::uint64_t deadline) {
    if (focus_driver_ || started_ || !deadline || gate_)
        return false;
    focus_driver_ = &driver;
    focus_authority_ = &authority;
    control_deadline_ = deadline;
    return true;
}
// bind_control_scope 只从可信核心绑定已经建立的原腿；新Peer必须已有刷新后的实际静音。
bool BrowserCallMedia::bind_control_scope(std::string_view session) {
    if (!focus_driver_ || !connected_ || session.empty() || session.size() > 64 ||
        control_failed_ || !focus_driver_->idle_refresh_ready(0))
        return false;
    if (!control_session_.empty())
        return session == control_session_;
    control_session_ = session;
    focus_id_ = "call-output-" + std::to_string(peer_epoch_);
    return true;
}
// output_readiness 在途不是拒绝；只有原false实态确认后才允许宿主绑定并开始acquire。
CallOutputReadiness BrowserCallMedia::output_readiness() const {
    if (!focus_driver_ || !connected_ || control_failed_ || !control_session_.empty() ||
        focus_driver_->idle_refresh_failed(0))
        return CallOutputReadiness::Failed;
    return focus_driver_->idle_refresh_ready(0) ? CallOutputReadiness::Ready
                                                : CallOutputReadiness::Pending;
}
// focus_scope 只提供本宿主原Peer事实；established仍需调用者结合核心独立校验。
std::optional<BrowserCallFocusScope> BrowserCallMedia::focus_scope() const {
    if (control_session_.empty() || !peer_epoch_)
        return {};
    return BrowserCallFocusScope{control_session_, peer_epoch_,    connected_,
                                 hold_token_,      hold_revision_, hold_pending_ && !hold_target_};
}
// begin_focus 许可、完整资源和CAS分别核验，排队不能改变output_ready实态。
bool BrowserCallMedia::begin_focus(std::string_view operation, std::string_view token,
                                   std::uint64_t now) {
    if (!focus_driver_ || !focus_authority_ || focus_command_ || control_failed_ ||
        now >= control_deadline_ || !control_wall_ || control_session_.empty() ||
        focus_generation_ == std::numeric_limits<std::uint64_t>::max())
        return false;
    FocusCommand command;
    command.envelope.token = token;
    command.envelope.session = control_session_;
    command.envelope.method = "focusControl";
    command.envelope.request = true;
    command.envelope.event_sequence = focus_generation_ + 1;
    command.operation = operation;
    command.expected_revision = focus_driver_->revision();
    command.focus_id = focus_id_;
    command.resources = {"speaker", "display"};
    command.priority = "call";
    command.resume_policy = "explicit";
    if (operation == "acquire" || operation == "restore") {
        if (control_wall_ > std::numeric_limits<std::uint64_t>::max() - (control_deadline_ - now))
            return false;
        command.expires_at = control_wall_ + (control_deadline_ - now);
    }
    // Driver再次严格解码完整信封，可信宿主构造也不能绕过原payload与结构体一致性检查。
    using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
    Json payload(cJSON_CreateObject(), cJSON_Delete);
    if (!payload ||
        !cJSON_AddStringToObject(payload.get(), "operation", command.operation.c_str()) ||
        !cJSON_AddNumberToObject(payload.get(), "expectedRevision",
                                 static_cast<double>(command.expected_revision)) ||
        !cJSON_AddStringToObject(payload.get(), "focusId", command.focus_id.c_str()) ||
        !cJSON_AddStringToObject(payload.get(), "priority", "call") ||
        !cJSON_AddStringToObject(payload.get(), "resumePolicy", "explicit"))
        return false;
    auto* resources = cJSON_AddArrayToObject(payload.get(), "resources");
    if (!resources)
        return false;
    for (const auto& resource : command.resources) {
        Json value(cJSON_CreateString(resource.c_str()), cJSON_Delete);
        if (!value)
            return false;
        auto* transferred = value.release();
        if (!cJSON_AddItemToArray(resources, transferred)) {
            value.reset(transferred);
            return false;
        }
    }
    if (command.expires_at && !cJSON_AddNumberToObject(payload.get(), "expiresAt",
                                                       static_cast<double>(*command.expires_at)))
        return false;
    auto* raw = cJSON_PrintUnformatted(payload.get());
    if (!raw)
        return false;
    command.envelope.payload = raw;
    cJSON_free(raw);
    const auto permit = focus_authority_->authorize(command, now, control_wall_);
    if (!permit || !focus_authority_->valid(*permit, now, control_wall_) ||
        !focus_driver_->begin(command, focus_generation_ + 1))
        return false;
    ++focus_generation_;
    focus_command_ = std::move(command);
    focus_permit_ = permit;
    return true;
}
// acquire_output 首次原已建立腿取得整组实际输出，无隐式恢复或跨进程系统焦点声明。
bool BrowserCallMedia::acquire_output(std::uint64_t now) {
    return focus_state_.empty() && connected_ &&
           begin_focus("acquire", "call-acquire-" + std::to_string(peer_epoch_), now);
}
// request_hold 平台原完成事件只授予一次驱动意图，仍等上行停轨及输出整组实际回执。
std::optional<std::uint64_t> BrowserCallMedia::request_hold(bool held, std::string_view token,
                                                            std::uint64_t revision,
                                                            std::uint64_t now) {
    if (!focus_driver_ || !connected_ || control_failed_ || hold_pending_ || focus_command_ ||
        hold_command_ || token.empty() || token.size() > 128 || !revision ||
        revision > 9007199254740991ULL || now >= control_deadline_ ||
        now > std::numeric_limits<std::uint64_t>::max() - 2000 ||
        hold_ticket_ == std::numeric_limits<std::uint64_t>::max() ||
        (held ? !output_ready_ : focus_state_ != "suspended"))
        return {};
    using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
    Json object(cJSON_CreateObject(), cJSON_Delete);
    if (!object || !cJSON_AddStringToObject(object.get(), "kind", "callHoldGate") ||
        !cJSON_AddStringToObject(object.get(), "sessionId", control_session_.c_str()) ||
        !cJSON_AddStringToObject(object.get(), "peerEpoch", std::to_string(peer_epoch_).c_str()) ||
        !cJSON_AddStringToObject(object.get(), "ticket",
                                 std::to_string(hold_ticket_ + 1).c_str()) ||
        !cJSON_AddBoolToObject(object.get(), "held", held) ||
        !cJSON_AddStringToObject(object.get(), "controlToken", std::string(token).c_str()) ||
        !cJSON_AddNumberToObject(object.get(), "revision", static_cast<double>(revision)))
        return {};
    auto* bytes = cJSON_PrintUnformatted(object.get());
    if (!bytes)
        return {};
    std::string body(bytes);
    cJSON_free(bytes);
    hold_token_ = token;
    hold_revision_ = revision;
    hold_target_ = held;
    hold_pending_ = true;
    hold_up_applied_ = hold_focus_applied_ = false;
    hold_result_.reset();
    if (!begin_focus(held ? "suspend" : "restore", token, now)) {
        hold_pending_ = false;
        return {};
    }
    ++hold_ticket_;
    hold_deadline_ = std::min(now + 2000, control_deadline_);
    hold_command_ = std::move(body);
    hold_sent_ = false;
    return hold_ticket_;
}
// hold_result 只读原票据；另一个目标、结束腿或未知不成为实际完成。
std::optional<bool> BrowserCallMedia::hold_result(std::uint64_t ticket, bool held) const {
    if (!ticket || ticket != hold_ticket_ || held != hold_target_)
        return false;
    return hold_result_;
}
// receive_control 回执严格沿本地原票据，网页不能借当前held或另一个session补造实际完成。
bool BrowserCallMedia::receive_control(std::string_view body, std::uint64_t now) {
    std::string canonical;
    if (canonical_json_object(body, canonical) != DecodeResult::Ok)
        return false;
    using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
    Json root(ur::device::parse_json(canonical.c_str()), cJSON_Delete);
    const auto matches = [&](const char* key, const std::string& expected) {
        const auto* value = cJSON_GetObjectItemCaseSensitive(root.get(), key);
        return cJSON_IsString(value) && expected == value->valuestring;
    };
    const auto* applied = cJSON_GetObjectItemCaseSensitive(root.get(), "applied");
    if (!cJSON_IsBool(applied))
        return false;
    if (matches("kind", "callOutputApplied")) {
        const auto* enabled = cJSON_GetObjectItemCaseSensitive(root.get(), "enabled");
        if (cJSON_GetArraySize(root.get()) != 6 || !gate_pending_ || !gate_sent_ ||
            now >= gate_deadline_ || !matches("sessionId", gate_session_) ||
            !matches("peerEpoch", std::to_string(gate_epoch_)) ||
            !matches("gateTicket", std::to_string(gate_ticket_)) || !cJSON_IsBool(enabled) ||
            cJSON_IsTrue(enabled) != gate_enabled_)
            return false;
        if (!cJSON_IsTrue(applied)) {
            control_failed_ = true;
            return true;
        }
        gate_confirmed_ = gate_ticket_;
        gate_pending_ = false;
        return true;
    }
    const auto* held = cJSON_GetObjectItemCaseSensitive(root.get(), "held");
    const auto* revision = cJSON_GetObjectItemCaseSensitive(root.get(), "revision");
    if (!matches("kind", "callHoldApplied") || cJSON_GetArraySize(root.get()) != 8 ||
        !hold_pending_ || !hold_sent_ || now >= hold_deadline_ ||
        !matches("sessionId", control_session_) ||
        !matches("peerEpoch", std::to_string(peer_epoch_)) ||
        !matches("ticket", std::to_string(hold_ticket_)) || !matches("controlToken", hold_token_) ||
        !cJSON_IsBool(held) || cJSON_IsTrue(held) != hold_target_ || !cJSON_IsNumber(revision) ||
        revision->valuedouble != static_cast<double>(hold_revision_))
        return false;
    if (!cJSON_IsTrue(applied)) {
        control_failed_ = true;
        hold_result_ = false;
        return true;
    }
    hold_up_applied_ = true;
    return true;
}
// poll_control 收齐两条实态后才完成保持；未知永久禁止此路径打开，不重发或自动换票据。
bool BrowserCallMedia::poll_control(std::uint64_t now, std::uint64_t wall) {
    if (!focus_driver_ || now < control_now_ || now >= control_deadline_ || !wall)
        return false;
    control_now_ = now;
    control_wall_ = wall;
    if ((gate_pending_ && now >= gate_deadline_) || (hold_pending_ && now >= hold_deadline_))
        control_failed_ = true;
    if (focus_permit_ && !focus_authority_->valid(*focus_permit_, now, wall))
        control_failed_ = true;
    if (!focus_driver_->poll(now))
        control_failed_ = true;
    if (focus_command_) {
        const auto fact = focus_driver_->take_fact(control_session_, focus_id_);
        if (fact) {
            if (!fact->success || fact->generation != focus_generation_ ||
                fact->resources != std::vector<std::string>{"speaker", "display"} ||
                fact->revision != focus_command_->expected_revision + 1)
                control_failed_ = true;
            else {
                focus_state_ = fact->state;
                output_ready_ = fact->state == "acquired" || fact->state == "restored";
                hold_focus_applied_ =
                    hold_pending_ && fact->state == (hold_target_ ? "suspended" : "restored");
                if (fact->state == "released") {
                    control_session_.clear();
                    focus_id_.clear();
                }
            }
            focus_command_.reset();
            focus_permit_.reset();
        }
    }
    if (control_failed_) {
        output_ready_ = false;
        if (hold_pending_) {
            hold_pending_ = false;
            hold_result_ = false;
        }
        return false;
    }
    if (hold_pending_ && hold_up_applied_ && hold_focus_applied_) {
        hold_pending_ = false;
        hold_result_ = true;
    }
    return true;
}
// release_output 只能沿原租约显式释放；在途或未知不冒资源已释放。
bool BrowserCallMedia::release_output(std::uint64_t now) {
    output_ready_ = false;
    return !focus_state_.empty() && focus_state_ != "released" &&
           begin_focus("release", "call-release-" + std::to_string(peer_epoch_), now);
}
// request_output_gate 仅显式Call宿主排队原实际票据，原配对页面确认之前不能提供完成事实。
std::optional<std::uint64_t> BrowserCallMedia::request_output_gate(bool enabled,
                                                                   std::uint64_t now) {
    if (!focus_driver_ || control_failed_ || gate_pending_ || gate_command_ ||
        gate_ticket_ == std::numeric_limits<std::uint64_t>::max() ||
        now > std::numeric_limits<std::uint64_t>::max() - 2000)
        return {};
    using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
    Json object(cJSON_CreateObject(), cJSON_Delete);
    const auto ticket = gate_ticket_ + 1;
    const auto epoch = std::to_string(peer_epoch_), key = std::to_string(ticket);
    if (!object || !cJSON_AddStringToObject(object.get(), "kind", "callOutputGate") ||
        !cJSON_AddStringToObject(object.get(), "sessionId", control_session_.c_str()) ||
        !cJSON_AddStringToObject(object.get(), "peerEpoch", epoch.c_str()) ||
        !cJSON_AddStringToObject(object.get(), "gateTicket", key.c_str()) ||
        !cJSON_AddBoolToObject(object.get(), "enabled", enabled))
        return {};
    auto* raw = cJSON_PrintUnformatted(object.get());
    if (!raw)
        return {};
    gate_command_ = raw;
    cJSON_free(raw);
    gate_ticket_ = ticket;
    gate_deadline_ = now + 2000;
    gate_epoch_ = peer_epoch_;
    gate_session_ = control_session_;
    gate_enabled_ = enabled;
    gate_pending_ = true;
    gate_sent_ = false;
    return ticket;
}
// output_gate_confirmed 只提供原当前票据实态，排队/旧票据或未知输出不能成为许可。
bool BrowserCallMedia::output_gate_confirmed(std::uint64_t ticket, bool enabled) const {
    return !control_failed_ && ticket && ticket == gate_ticket_ && ticket == gate_confirmed_ &&
           enabled == gate_enabled_ && !gate_pending_;
}
// browser_call_state 用结构化JSON转义原会话，不从拼接片段猜测引号或对象边界。
std::string browser_call_state(std::string_view state, std::string_view session) {
    if ((state != "ringing" && state != "connected" && state != "available" &&
         state != "unavailable" && state != "negotiating" && state != "connecting" &&
         state != "inviting") ||
        session.size() > 128 || std::any_of(session.begin(), session.end(), [](unsigned char ch) {
            return ch < 32 || ch == 127;
        }))
        return {};
    using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
    Json object(cJSON_CreateObject(), cJSON_Delete);
    const std::string phase(state), identity(session);
    if (!object || !cJSON_AddStringToObject(object.get(), "kind", "callState") ||
        !cJSON_AddStringToObject(object.get(), "state", phase.c_str()) ||
        !cJSON_AddStringToObject(object.get(), "sessionId", identity.c_str()))
        return {};
    auto* raw = cJSON_PrintUnformatted(object.get());
    if (!raw)
        return {};
    std::string body(raw);
    cJSON_free(raw);
    return body;
}
// configure_video_receive 在原核心完成协商后、启动驱动之前绑定接收授权。
bool BrowserCallMedia::configure_video_receive(bool enabled) {
    if (started_ || stopping_)
        return false;
    receive_video_ = enabled;
    return true;
}
// 保留既有调用签名兼容原宿主；命令种类与SDP各自编码，原媒体关联不变。
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
bool BrowserCallMedia::emit(std::string_view kind, std::string_view sdp) {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
    Json object(cJSON_CreateObject(), cJSON_Delete);
    const std::string type(kind), description(sdp);
    if (!object || !cJSON_AddStringToObject(object.get(), "kind", type.c_str()) ||
        (!sdp.empty() && !cJSON_AddStringToObject(object.get(), "sdp", description.c_str())))
        return false;
    if (focus_driver_ && kind == "callConfigure" &&
        !cJSON_AddStringToObject(object.get(), "peerEpoch", std::to_string(peer_epoch_).c_str()))
        return false;
    if (kind == "callConfigure" && receive_video_ &&
        !cJSON_AddBoolToObject(object.get(), "videoReceive", true))
        return false;
    if (gate_) {
        if (!lease_ || session_.empty() ||
            !cJSON_AddStringToObject(object.get(), "sessionId", session_.c_str()) ||
            !cJSON_AddStringToObject(object.get(), "releaseId", std::to_string(*lease_).c_str()) ||
            (kind == "callConfigure" &&
             !cJSON_AddStringToObject(object.get(), "cameraId", camera_.c_str())))
            return false;
    }
    char* bytes = cJSON_PrintUnformatted(object.get());
    if (!bytes)
        return false;
    std::string payload(bytes);
    cJSON_free(bytes);
    return payload.size() <= 65536 && commands_.push_back(std::move(payload));
}
bool BrowserCallMedia::begin() {
    if (focus_driver_) {
        if (control_failed_ || !control_session_.empty() ||
            peer_epoch_ == std::numeric_limits<std::uint64_t>::max() ||
            !focus_driver_->idle_refresh_ready(0))
            return false;
        ++peer_epoch_;
        focus_state_.clear();
    }
    if (started_ || stopping_ || (gate_ && !prepare()) || !emit("callConfigure"))
        return false;
    if (focus_driver_ && !focus_driver_->refresh_idle(0, control_now_))
        return false;
    started_ = true;
    answered_ = connected_ = false;
    return true;
}
// available 无副作用核验真实整组，新宿主实际停止之前保持忙线。
bool BrowserCallMedia::available() const {
    if (focus_driver_)
        return !control_failed_ && !started_ && !focus_command_ && control_session_.empty() &&
               focus_driver_->idle_refresh_ready(0);
    return !gate_ || (!stopping_ && !started_ && (lease_ || gate_->available(true, true, true)));
}
// prepare 在接听ACK之前占用mic/speaker/video，拒绝部分预约和重复绑定。
bool BrowserCallMedia::prepare() {
    if (focus_driver_)
        return available();
    if (!gate_)
        return true;
    if (stopping_ || session_.empty() || camera_.empty() || camera_.size() > 128 ||
        std::any_of(camera_.begin(), camera_.end(),
                    [](unsigned char ch) { return ch < 32 || ch == 127; }))
        return false;
    if (!lease_)
        lease_ = gate_->acquire(true, true, true);
    return lease_.has_value();
}
// bind_session 不能把旧驱动或尚未收录Stopped的预约改成新来电。
bool BrowserCallMedia::bind_session(std::string_view session) {
    if (!gate_ || stopping_ || session.empty() || session.size() > 128 ||
        std::any_of(session.begin(), session.end(),
                    [](unsigned char ch) { return ch < 32 || ch == 127; }))
        return false;
    if (started_ || lease_)
        return session_ == session;
    session_ = session;
    return true;
}
// report_stopped 只清除实际原租约，重复或迟到的停止消息不授予新会话。
bool BrowserCallMedia::report_stopped(std::string_view session, std::string_view release_id) {
    if (!gate_ || !stopping_ || !lease_ || session != session_ ||
        release_id != std::to_string(*lease_) || !gate_->release(*lease_))
        return false;
    lease_.reset();
    session_.clear();
    stopping_ = false;
    return true;
}
// matches 将实际offer/connected回调绑定到当前原采集预约。
bool BrowserCallMedia::matches(std::string_view session, std::string_view release_id) const {
    return gate_ && lease_ && started_ && !stopping_ && session == session_ &&
           release_id == std::to_string(*lease_);
}
bool BrowserCallMedia::apply_answer(std::string_view sdp) {
    if (!started_ || answered_ || sdp.size() < 8 || sdp.size() > 49152 ||
        sdp.substr(0, 5) != "v=0\r\n" || sdp.find('\0') != std::string_view::npos ||
        !emit("callAnswer", sdp))
        return false;
    answered_ = true;
    return true;
}
bool BrowserCallMedia::report_connected() {
    if (!started_ || !answered_ || connected_ || natural_suspended_ || natural_expired_)
        return false;
    connected_ = true;
    return true;
}
// report_connectivity 只改变原Peer实际连通事实，不换采集租约、不发布初次握手或新开麦命令。
Result BrowserCallMedia::report_connectivity(std::string_view session, std::string_view release_id,
                                             bool connected, std::uint64_t now) {
    if (!matches(session, release_id))
        return Result::Stale;
    if (natural_expired_)
        return Result::DriverFailed;
    if (natural_suspended_) {
        if (now < natural_started_ || now - natural_started_ >= 5000) {
            natural_expired_ = true;
            return Result::DriverFailed;
        }
        if (!connected)
            return Result::Duplicate;
        natural_suspended_ = false;
        connected_ = true;
        return Result::Applied;
    }
    if (!connected_)
        return Result::Invalid;  // 初次ICE未建立不得借暂断窗口获得许可。
    if (connected)
        return Result::Duplicate;
    natural_started_ = now;
    natural_suspended_ = true;
    connected_ = false;
    return Result::Applied;
}
// connectivity_expired 与网页固定窗口独立；回调丢失仍只终止原腿一次。
bool BrowserCallMedia::connectivity_expired(std::uint64_t now) {
    if (!started_ || natural_notified_)
        return false;
    if (natural_suspended_ && (now < natural_started_ || now - natural_started_ >= 5000))
        natural_expired_ = true;
    if (!natural_expired_)
        return false;
    natural_notified_ = true;
    return true;
}
void BrowserCallMedia::close() {
    if (focus_driver_ && !control_session_.empty()) {
        if (hold_pending_) {
            hold_pending_ = false;
            hold_result_ = false;
        }
        if (!release_output(control_now_))
            control_failed_ = true;
    }
    if (started_) {
        emit("callStop");
        if (gate_)
            stopping_ = true;
    } else if (gate_ && lease_ && !stopping_) {
        gate_->release(*lease_);  // 未创建媒体的预约可以直接取消，不需要虚构Stopped。
        lease_.reset();
        session_.clear();
    }
    started_ = answered_ = connected_ = false;
    natural_suspended_ = natural_expired_ = natural_notified_ = false;
    natural_started_ = 0;
    receive_video_ = false;
}
std::optional<std::string> BrowserCallMedia::take_command() {
    // 配置/停止先于门控，使原新Peer回执不能沿尚未安装节点提前完成。
    auto* item = commands_.front();
    if (!item) {
        if (gate_command_) {
            auto body = std::move(gate_command_);
            gate_command_.reset();
            gate_sent_ = true;
            return body;
        }
        if (hold_command_) {
            auto body = std::move(hold_command_);
            hold_command_.reset();
            hold_sent_ = true;
            return body;
        }
        return {};
    }
    auto result = std::move(*item);
    commands_.pop_front();
    return result;
}
}  // namespace ur::device
