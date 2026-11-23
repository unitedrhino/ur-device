// 文件说明：AI与文件完整输出组共享许可入口；内容授权与末端mask独立，仍须真实门控完成。
#include "browser_media_focus.hpp"
#include <algorithm>

namespace ur::device {
BrowserMediaFocusAuthority::BrowserMediaFocusAuthority(OutputFocusDriver& driver,
                                                       BrowserResolvedPlaybackAdmission& files,
                                                       std::uint64_t deadline,
                                                       BrowserVoiceScope voice,
                                                       BrowserFileEstablished established)
    : driver_(driver),
      files_(files),
      deadline_(deadline),
      voice_(driver, deadline, std::move(voice)),
      established_(std::move(established)) {}

std::optional<std::size_t> BrowserMediaFocusAuthority::file_path(const FocusCommand& command,
                                                                 std::uint64_t now,
                                                                 std::uint64_t wall) const {
    const auto* media = files_.active_media();
    if (!wall || now >= deadline_ || !media || !media->ready() || !media->preparation_permitted() ||
        !established_ || !established_(command.envelope.session) ||
        command.envelope.session != media->binding().session || command.focus_id.empty() ||
        command.priority != "background" || command.resume_policy != "explicit" ||
        now >= media->binding().expires_at)
        return {};
    const auto& binding = media->binding();
    const bool video = binding.mime == "video/mp4" || binding.mime == "video/webm";
    std::vector<std::string> resources;  // 固定完整组，不从请求子集提升音轨或显示能力。
    if (binding.audio)
        resources.push_back("speaker");
    if (video)
        resources.push_back("display");
    if (resources != command.resources)
        return {};
    const auto until = std::min(deadline_, binding.expires_at);
    if (command.operation == "acquire" || command.operation == "restore") {
        if (!command.expires_at || *command.expires_at <= wall ||
            *command.expires_at - wall > until - now)
            return {};
    } else if ((command.operation != "suspend" && command.operation != "release") ||
               command.expires_at)
        return {};
    return binding.audio ? (video ? 3U : 1U) : 2U;
}

std::optional<FocusPermit> BrowserMediaFocusAuthority::authorize(const FocusCommand& command,
                                                                 std::uint64_t now,
                                                                 std::uint64_t wall) {
    if (command.priority == "conversation")
        return voice_.authorize(command, now, wall);
    auto* media = files_.active_media();
    if (!media || !media->tick(now))
        return {};
    const auto path = file_path(command, now, wall);
    if (!path)
        return {};
    auto& original = scopes_[*path - 1];
    if (original.first != command.envelope.session || original.second != command.focus_id) {
        if (command.operation != "acquire" || command.expected_revision != driver_.revision() ||
            !driver_.bind_scope(*path, command.envelope.session, command.focus_id, now))
            return {};
        original = {command.envelope.session, command.focus_id};
    }
    const auto until = std::min(deadline_, media->binding().expires_at);
    return FocusPermit{command, command.expires_at ? now + (*command.expires_at - wall) : until, 1};
}

bool BrowserMediaFocusAuthority::valid(const FocusPermit& permit, std::uint64_t now,
                                       std::uint64_t wall) const {
    if (permit.scope.priority == "conversation")
        return voice_.valid(permit, now, wall);
    const auto path = file_path(permit.scope, now, wall);
    if (!path || permit.intent_version != 1 || now >= permit.deadline ||
        permit.deadline > std::min(deadline_, files_.active_media()->binding().expires_at))
        return false;
    const auto& original = scopes_[*path - 1];
    return original.first == permit.scope.envelope.session &&
           original.second == permit.scope.focus_id;
}
}  // namespace ur::device
