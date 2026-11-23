// 文件说明：原呼叫输出许可与实际CAS分别验证，平台active不授予设备扬声器权限。
#include "browser_call_focus.hpp"

namespace ur::device {
namespace {
// same_scope 精确绑定原完整信封与操作字段，不以仍在宿主范围内接受被篡改的许可副本。
bool same_scope(const FocusCommand& one, const FocusCommand& two) {
    return one.envelope.token == two.envelope.token &&
           one.envelope.session == two.envelope.session &&
           one.envelope.method == two.envelope.method &&
           one.envelope.request == two.envelope.request &&
           one.envelope.payload == two.envelope.payload && one.envelope.code == two.envelope.code &&
           one.envelope.event_sequence == two.envelope.event_sequence &&
           one.envelope.message_sequence == two.envelope.message_sequence &&
           one.operation == two.operation && one.expected_revision == two.expected_revision &&
           one.focus_id == two.focus_id && one.resources == two.resources &&
           one.priority == two.priority && one.resume_policy == two.resume_policy &&
           one.expires_at == two.expires_at;
}
}  // namespace

BrowserCallFocusAuthority::BrowserCallFocusAuthority(OutputFocusDriver& driver,
                                                     std::uint64_t deadline, BrowserCallScope scope)
    : driver_(driver), deadline_(deadline), scope_(std::move(scope)) {}

// eligible 关闭只沿原租约收敛，取得/恢复持续核原业务资格；TTL上界仅在新签发时比较。
bool BrowserCallFocusAuthority::eligible(const FocusCommand& command, std::uint64_t now,
                                         std::uint64_t wall, bool creation_only) const {
    if (!scope_ || !wall || now >= deadline_ || command.envelope.session.empty() ||
        command.focus_id.empty() ||
        command.resources != std::vector<std::string>{"speaker", "display"} ||
        command.priority != "call" || command.resume_policy != "explicit")
        return false;
    if (command.operation == "release")
        return !command.expires_at && command.envelope.session == session_ &&
               command.focus_id == focus_;
    const auto current = scope_();
    if (!current || !current->established || !current->peer_epoch ||
        current->session != command.envelope.session)
        return false;
    if (command.operation == "acquire" || command.operation == "restore") {
        if (!command.expires_at || *command.expires_at <= wall ||
            (creation_only && *command.expires_at - wall > deadline_ - now))
            return false;
        if (command.operation == "restore" &&
            (!current->explicit_restore || !current->control_revision ||
             current->control_token.empty() || current->control_token != command.envelope.token))
            return false;
        return true;
    }
    return command.operation == "suspend" && !command.expires_at &&
           !current->control_token.empty() && current->control_revision &&
           current->control_token == command.envelope.token;
}

// authorize 首次只绑定原完整路径，新腿不能借旧未释放身份；绑定本身不启用实际输出。
std::optional<FocusPermit> BrowserCallFocusAuthority::authorize(const FocusCommand& command,
                                                                std::uint64_t now,
                                                                std::uint64_t wall) {
    if (!eligible(command, now, wall, true))
        return {};
    const auto current = scope_();
    if (command.envelope.session != session_ || command.focus_id != focus_) {
        if (!current || command.operation != "acquire" ||
            command.expected_revision != driver_.revision() ||
            !driver_.bind_scope(0, command.envelope.session, command.focus_id, now))
            return {};
        session_ = command.envelope.session;
        focus_ = command.focus_id;
        peer_epoch_ = current->peer_epoch;
    }
    if (command.operation != "release" && (!current || current->peer_epoch != peer_epoch_))
        return {};
    // TTL上界已确认，计算不会溢出或超过原宿主截止；两份绝对期限此后均固定，不再重算。
    const FocusPermit permit{
        command, command.expires_at ? now + (*command.expires_at - wall) : deadline_, peer_epoch_};
    issued_ = permit;
    return permit;
}

// valid 先核唯一原签发完整副本，再分别核两绝对期限与业务资格，不比较独立时钟的剩余差。
// 期限/意图/任何请求字段变化均拒绝，旧许可不能借后续新签发记录取得额外能力。
bool BrowserCallFocusAuthority::valid(const FocusPermit& permit, std::uint64_t now,
                                      std::uint64_t wall) const {
    if (!issued_ || issued_->deadline != permit.deadline ||
        issued_->intent_version != permit.intent_version ||
        !same_scope(issued_->scope, permit.scope))
        return false;
    const auto current = scope_ ? scope_() : std::nullopt;
    return permit.intent_version == peer_epoch_ && peer_epoch_ && permit.deadline <= deadline_ &&
           now < permit.deadline && permit.scope.envelope.session == session_ &&
           permit.scope.focus_id == focus_ &&
           (permit.scope.operation == "release" ||
            (current && current->peer_epoch == peer_epoch_)) &&
           eligible(permit.scope, now, wall, false);
}
}  // namespace ur::device
