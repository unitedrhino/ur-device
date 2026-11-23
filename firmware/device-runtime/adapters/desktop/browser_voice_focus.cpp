// 文件说明：已鉴权AI会话的扬声器焦点准入；独立内容/视频/通话许可不从此处派生。
#include "browser_voice_focus.hpp"

namespace ur::device {
BrowserVoiceFocusAuthority::BrowserVoiceFocusAuthority(OutputFocusDriver& driver,
                                                       std::uint64_t deadline,
                                                       BrowserVoiceScope scope)
    : driver_(driver), deadline_(deadline), scope_(std::move(scope)) {}

bool BrowserVoiceFocusAuthority::eligible(const FocusCommand& command, std::uint64_t now,
                                          std::uint64_t wall) const {
    if (!scope_ || !wall || now >= deadline_ || command.envelope.session.empty() ||
        command.focus_id.empty() || command.resources != std::vector<std::string>{"speaker"} ||
        command.priority != "conversation" || command.resume_policy != "explicit")
        return false;
    const auto current = scope_();
    if (!current || *current != command.envelope.session)
        return false;
    if (command.operation == "acquire" || command.operation == "restore")
        return command.expires_at && *command.expires_at > wall &&
               *command.expires_at - wall <= deadline_ - now;
    return (command.operation == "suspend" || command.operation == "release") &&
           !command.expires_at;
}

std::optional<FocusPermit> BrowserVoiceFocusAuthority::authorize(const FocusCommand& command,
                                                                 std::uint64_t now,
                                                                 std::uint64_t wall) {
    if (!eligible(command, now, wall))
        return {};
    if (session_.empty()) {
        // 动态身份仅补给既有实际路径；未知静音、错水位或非首次取得不能接入。
        if (command.operation != "acquire" || command.expected_revision != driver_.revision() ||
            !driver_.bind_scope(0, command.envelope.session, command.focus_id, now))
            return {};
        session_ = command.envelope.session;
        focus_ = command.focus_id;
    }
    if (session_ != command.envelope.session || focus_ != command.focus_id)
        return {};
    const auto expires = command.expires_at ? now + (*command.expires_at - wall) : deadline_;
    return FocusPermit{command, expires, 1};  // 原会话不变才有效，不按新请求提升意图版本。
}

bool BrowserVoiceFocusAuthority::valid(const FocusPermit& permit, std::uint64_t now,
                                       std::uint64_t wall) const {
    return permit.intent_version == 1 && permit.deadline <= deadline_ && now < permit.deadline &&
           permit.scope.envelope.session == session_ && permit.scope.focus_id == focus_ &&
           eligible(permit.scope, now, wall);
}
}  // namespace ur::device
