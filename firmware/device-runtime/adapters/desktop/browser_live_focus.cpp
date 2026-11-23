// 文件说明：实时节目完整组许可映射；末端mask与实际单轨控制分离，不创建第二仲裁体系。
#include "browser_live_focus.hpp"
#include <algorithm>

namespace ur::device {
// BrowserLiveFocusAuthority 借用唯一仲裁器，AI0与节目固定三组互不提升来源/采集权限。
BrowserLiveFocusAuthority::BrowserLiveFocusAuthority(OutputFocusDriver& driver,
                                                     std::uint64_t deadline,
                                                     BrowserVoiceScope voice,
                                                     BrowserLiveFocusQuery current,
                                                     BrowserLiveReleaseQuery release)
    : driver_(driver),
      deadline_(deadline),
      voice_(driver, deadline, std::move(voice)),
      current_(std::move(current)),
      release_(std::move(release)) {}
// scope_for release优先查询原会话墓碑；查不到只允许当前原Established，不借新节目当旧租约许可。
std::optional<BrowserLiveFocusScope> BrowserLiveFocusAuthority::scope_for(
    const FocusCommand& command) const {
    if (command.operation == "release" && release_)
        if (auto original = release_(command.envelope.session))
            return original;
    const auto current = current_ ? current_() : std::nullopt;
    return current && current->session == command.envelope.session ? current : std::nullopt;
}
// path 验证当前原完整组、已建立、有限授权与明确恢复策略，返回固定物理组位置。
std::optional<std::size_t> BrowserLiveFocusAuthority::path(const FocusCommand& command,
                                                           std::uint64_t now,
                                                           std::uint64_t wall) const {
    const auto scope = scope_for(command);
    if (!wall || now >= deadline_ || !scope || scope->session.empty() ||
        scope->session != command.envelope.session || (!scope->audio && !scope->video) ||
        command.focus_id.empty() || command.priority != "background" ||
        command.resume_policy != "explicit")
        return {};
    std::vector<std::string> resources;  // 完整原组，不能拿单speaker控制AV租约或新增摄像头。
    if (scope->audio)
        resources.push_back("speaker");
    if (scope->video)
        resources.push_back("display");
    if (resources != command.resources)
        return {};
    // 节目/源已关闭不再授输出权；原授信父身份仅作历史绑定，原平台Ended+actual退出后只关闭旧lease。
    if (scope->platform_ended) {
        if (command.operation != "release" || command.expires_at || !scope->actual_released ||
            scope->established || scope->authorized || !scope->retain_until ||
            now >= scope->retain_until || scope->parent_session.empty())
            return {};
        return scope->audio ? (scope->video ? 3U : 1U) : 2U;
    }
    if (!scope->authorized || !scope->established || now >= scope->deadline)
        return {};
    const auto until = std::min(deadline_, scope->deadline);
    if (command.operation == "acquire" || command.operation == "restore") {
        if (!command.expires_at || *command.expires_at <= wall ||
            *command.expires_at - wall > until - now)
            return {};
    } else if ((command.operation != "suspend" && command.operation != "release") ||
               command.expires_at)
        return {};
    return scope->audio ? (scope->video ? 3U : 1U) : 2U;
}
// authorize 复用原全局CAS/实际释放栅栏；暂时取得末端mask不触发节目轨resume。
std::optional<FocusPermit> BrowserLiveFocusAuthority::authorize(const FocusCommand& command,
                                                                std::uint64_t now,
                                                                std::uint64_t wall) {
    if (command.priority == "conversation")
        return voice_.authorize(command, now, wall);
    const auto slot = path(command, now, wall);
    if (!slot)
        return {};
    auto& original = scopes_[*slot - 1];
    if (original.first != command.envelope.session || original.second != command.focus_id) {
        if (command.operation != "acquire" || command.expected_revision != driver_.revision() ||
            !driver_.bind_scope(*slot, command.envelope.session, command.focus_id, now))
            return {};
        original = {command.envelope.session, command.focus_id};
    }
    const auto scope = scope_for(command);
    if (!scope)
        return {};
    const auto until =
        std::min(deadline_, scope->platform_ended ? scope->retain_until : scope->deadline);
    return FocusPermit{command, command.expires_at ? now + (*command.expires_at - wall) : until, 1};
}
// valid 只检查原完整业务许可，不把历史actual或当前mask状态提升成来源授权。
bool BrowserLiveFocusAuthority::valid(const FocusPermit& permit, std::uint64_t now,
                                      std::uint64_t wall) const {
    if (permit.scope.priority == "conversation")
        return voice_.valid(permit, now, wall);
    const auto slot = path(permit.scope, now, wall);
    const auto scope = scope_for(permit.scope);
    if (!slot || !scope || permit.intent_version != 1 || now >= permit.deadline ||
        permit.deadline >
            std::min(deadline_, scope->platform_ended ? scope->retain_until : scope->deadline))
        return false;
    const auto& original = scopes_[*slot - 1];
    return original.first == permit.scope.envelope.session &&
           original.second == permit.scope.focus_id;
}
bool BrowserLiveFocusAuthority::closed_release_valid(const FocusPermit& permit, std::uint64_t now,
                                                     std::uint64_t wall) const {
    if (permit.scope.operation != "release" || permit.scope.priority != "background")
        return false;
    const auto original = scope_for(permit.scope);
    // 继续复核原scope/focus/全组/历史父/固定retain，不默认开放普通租约。
    return original && original->platform_ended && original->actual_released &&
           valid(permit, now, wall);
}
}  // namespace ur::device
