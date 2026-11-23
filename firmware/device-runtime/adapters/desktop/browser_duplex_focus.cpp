// 文件说明：原宿主绑定→完整组CAS签发→双绝对期限持续核验；不触发HAL或自动恢复。
#include "browser_duplex_focus.hpp"

#include <algorithm>

namespace ur::device {
namespace {
constexpr std::uint64_t safe = 9007199254740991ULL;                // 原跨语言水位与期限上界。
const std::vector<std::string> resources{"microphone", "speaker"}; // 唯一完整组，不接受半组。
// same_binding 对应宿主原bind参数及原意图，后续不能换Peer/路由或延长期限。
bool same_binding(const BrowserDuplexBinding& a, const BrowserDuplexBinding& b) {
    return a.path == b.path && a.session == b.session && a.focus == b.focus &&
           a.route_generation == b.route_generation && a.intent_version == b.intent_version &&
           a.peer_epoch == b.peer_epoch && a.deadline == b.deadline &&
           a.expires_at == b.expires_at && a.role == b.role;
}
// identity 仅核原可信绑定形状，不能代替宿主身份授权或实际scope_ready。
bool identity(std::string_view value, std::size_t maximum) {
    return !value.empty() && value.size() <= maximum &&
           std::none_of(value.begin(), value.end(),
                        [](unsigned char c) { return c < 32 || c == 127; });
}
// same_command 连同原信封保留全部字段，调用者副本不能更换票据、期限或CAS。
bool same_command(const FocusCommand& a, const FocusCommand& b) {
    return a.envelope.token == b.envelope.token && a.envelope.session == b.envelope.session &&
           a.envelope.method == b.envelope.method && a.envelope.request == b.envelope.request &&
           a.envelope.payload == b.envelope.payload && a.envelope.code == b.envelope.code &&
           a.envelope.event_sequence == b.envelope.event_sequence &&
           a.envelope.message_sequence == b.envelope.message_sequence &&
           a.operation == b.operation && a.expected_revision == b.expected_revision &&
           a.focus_id == b.focus_id && a.resources == b.resources && a.priority == b.priority &&
           a.resume_policy == b.resume_policy && a.expires_at == b.expires_at;
}
// same_permit 核对原签发记录，而非只接受仍落在宿主范围内的改写副本。
bool same_permit(const FocusPermit& a, const FocusPermit& b) {
    return a.deadline == b.deadline && a.intent_version == b.intent_version &&
           same_command(a.scope, b.scope);
}
} // namespace

// 构造仅保存原可信来源；不初始化driver、申请绑定或假造输入闲置。
BrowserDuplexFocusAuthority::BrowserDuplexFocusAuthority(DuplexFocusDriver& driver,
                                                         BrowserDuplexBinding binding,
                                                         BrowserDuplexScopeReader scope,
                                                         std::optional<BrowserDuplexCleanupBounds> cleanup)
    : driver_(driver), binding_(std::move(binding)), scope_(std::move(scope)), cleanup_(cleanup) {}

// observe 任一时钟回退永久拒绝，不改变driver或原物理状态。
bool BrowserDuplexFocusAuthority::observe(std::uint64_t now, std::uint64_t wall) const {
    if (clock_failed_ || !wall || now > safe || wall > safe || (now_ && now < *now_) ||
        (wall_ && wall < *wall_)) {
        clock_failed_ = true;
        return false;
    }
    now_ = now;
    wall_ = wall;
    return true;
}

// current_release
// 只为已固定的原停止指令核独立清理上界，原业务期限及意图一字不改。
std::optional<BrowserDuplexScope> BrowserDuplexFocusAuthority::current_release(
    std::uint64_t now, std::uint64_t wall) const {
    if (!cleanup_) return current(now, wall);
    if (!observe(now, wall) || !scope_ || !cleanup_->deadline || !cleanup_->expires_at ||
        cleanup_->deadline > safe || cleanup_->expires_at > safe || now >= cleanup_->deadline ||
        wall >= cleanup_->expires_at)
        return {};
    const auto value = scope_();
    if (!value || !same_binding(value->binding, binding_) ||
        (value->current_intent_version &&
         *value->current_intent_version != binding_.intent_version))
        return {};
    if (!value->cleanup_deadline && !value->cleanup_expires_at)
        return now < binding_.deadline && wall < binding_.expires_at ? value : std::nullopt;
    if (!value->closing || !value->release || !value->cleanup_deadline ||
        !value->cleanup_expires_at || *value->cleanup_deadline > cleanup_->deadline ||
        *value->cleanup_expires_at > cleanup_->expires_at || now >= *value->cleanup_deadline ||
        wall >= *value->cleanup_expires_at)
        return {};
    return value;
}

// current 读取原宿主独立权限，绝不由held=false推导恢复或清理权。
std::optional<BrowserDuplexScope> BrowserDuplexFocusAuthority::current(std::uint64_t now,
                                                                       std::uint64_t wall) const {
    if (!observe(now, wall) || !scope_ || now >= binding_.deadline || wall >= binding_.expires_at)
        return {};
    const auto value = scope_(); // 唯一宿主同步、不抛异常回调；页面或网络未知不提供此权限。
    if (!value || !same_binding(value->binding, binding_) ||
        (value->current_intent_version &&
         (*value->current_intent_version < binding_.intent_version ||
          *value->current_intent_version > safe)))
        return {};
    if (binding_.role == BrowserDuplexRole::AI && value->handoff_committed)
        ai_committed_ = true;
    return value;
}

// observe_allocation
// 原签发acquire/restore和driver实际整组拥有相符才保存分配证明。
void BrowserDuplexFocusAuthority::observe_allocation() const {
    if (!allocated_)
        allocated_ = std::any_of(issued_.begin(), issued_.end(), [&](const auto& permit) {
            return (permit.scope.operation == "acquire" || permit.scope.operation == "restore") &&
                   driver_.owns(permit.scope);
        });
}

// admit_binding 只接受宿主原入参与真实确认路径，失败不能新建或替换绑定。
bool BrowserDuplexFocusAuthority::admit_binding(const BrowserDuplexBinding& original,
                                                std::uint64_t now, std::uint64_t wall) {
    if (!same_binding(original, binding_) || binding_.path > 1 || !identity(binding_.session, 64) ||
        !identity(binding_.focus, 128) || !binding_.route_generation ||
        binding_.route_generation > safe || !binding_.intent_version ||
        binding_.intent_version > safe ||
        (binding_.role == BrowserDuplexRole::Human && !binding_.peer_epoch) ||
        binding_.peer_epoch > safe || !binding_.deadline || binding_.deadline > safe ||
        !binding_.expires_at || binding_.expires_at > safe ||
        (binding_.role != BrowserDuplexRole::AI && binding_.role != BrowserDuplexRole::Human))
        return false;
    const auto value = current(now, wall);
    if (!value || !value->authorized || !value->peer_ready || value->closing || ai_committed_ ||
        !driver_.scope_ready(binding_.path))
        return false;
    admitted_ = true;
    return true;
}

// authorize 核原操作合同/CAS与独立权限，最多32个完整原许可，重投不续期。
std::optional<FocusPermit> BrowserDuplexFocusAuthority::authorize(const FocusCommand& command,
                                                                  std::uint64_t now,
                                                                  std::uint64_t wall) {
    const auto value =
        command.operation == "release" ? current_release(now, wall) : current(now, wall);
    if (!admitted_ || !value || command.envelope.session != binding_.session ||
        command.focus_id != binding_.focus || command.resources.size() != 2 ||
        !std::is_permutation(command.resources.begin(), command.resources.end(),
                             resources.begin()) ||
        command.priority != (binding_.role == BrowserDuplexRole::AI ? "conversation" : "call") ||
        (binding_.role == BrowserDuplexRole::AI
             ? command.resume_policy != "explicit"
             : command.resume_policy != "never" && command.resume_policy != "explicit"))
        return {};
    // 对公共结构体再次使用严格报文合同，不能从改写payload或未知方法取得新许可。
    Envelope checked;
    if (focus_reply(command, "denied", driver_.revision(), 403, "permissionDenied", checked) !=
        WireResult::Ok)
        return {};
    // 同原token只能取回原许可，不推进CAS/刷新期限；改写任一字段必须拒绝。
    for (const auto& permit : issued_)
        if (permit.scope.envelope.token == command.envelope.token)
            return same_command(permit.scope, command) && valid(permit, now, wall)
                       ? std::optional<FocusPermit>(permit)
                       : std::nullopt;
    if (issued_.size() >= 32 || command.expected_revision != driver_.revision() ||
        command.expected_revision >= safe || value->operation_token.empty() ||
        value->operation_token != command.envelope.token)
        return {};
    observe_allocation();
    const bool cleanup = command.operation == "release" && value->closing && value->release &&
                         lease_deadline_ && allocated_;
    if (!cleanup && (!value->authorized || !value->peer_ready || value->closing || ai_committed_ ||
                     !driver_.scope_ready(binding_.path)))
        return {};
    const bool acquire = command.operation == "acquire", restore = command.operation == "restore";
    if ((acquire && !value->acquire) ||
        (restore && (!value->restore || !lease_deadline_ ||
                     (binding_.role == BrowserDuplexRole::Human &&
                      (command.resume_policy != "explicit" ||
                       std::none_of(issued_.begin(), issued_.end(),
                                    [](const FocusPermit& original) {
                                        return original.scope.operation == "acquire" &&
                                               original.scope.resume_policy == "explicit";
                                    }))))) ||
        (command.operation == "suspend" && (!value->suspend || !lease_deadline_)) ||
        (command.operation == "release" && (!value->release || !lease_deadline_)))
        return {};
    std::uint64_t deadline =
        cleanup && value->cleanup_deadline ? *value->cleanup_deadline : binding_.deadline;
    if (acquire || restore) {
        if (!command.expires_at || *command.expires_at <= wall ||
            *command.expires_at > binding_.expires_at)
            return {};
        // 两时钟独立推进，剩余时长不要求相等；许可取原两截止的较早值，绝不续长任一边界。
        deadline = std::min(binding_.deadline, now + (*command.expires_at - wall));
        if (lease_deadline_) {
            // 原期限与墙钟租约必须成对存在；缺失不得扩大或恢复输出授权。
            if (!lease_wall_.has_value())
                return {};
            // 不重算旧租约；双时钟独立推进可使新计算值不同，实际截止取较早原值。
            deadline = std::min(deadline, *lease_deadline_);
            if (*command.expires_at > *lease_wall_ || now >= deadline || wall >= *lease_wall_)
                return {};
        }
    }
    // 当前用户版本仅用于新restore许可，原引擎与暂停前版本比较后给出userIntentChanged。
    // acquire原许可不能因后来的用户停止被洗成新版本，也不能由新版本延长原截止。
    const FocusPermit permit{command, deadline,
                             restore
                                 ? value->current_intent_version.value_or(binding_.intent_version)
                                 : binding_.intent_version};
    issued_.push_back(permit);
    if (acquire && !lease_deadline_) {
        lease_deadline_ = deadline;
        lease_wall_ = command.expires_at;
    }
    return permit;
}

// valid 原在途许可不依赖scope_ready暂时false；实际执行由driver与引擎核验。
bool BrowserDuplexFocusAuthority::valid(const FocusPermit& permit, std::uint64_t now,
                                        std::uint64_t wall) const {
    return valid_observed(permit, now, wall, nullptr);
}
// valid_observed
// current以及restore拥有查询保持原顺序；正常分支选择false不形成首因。
bool BrowserDuplexFocusAuthority::valid_observed(const FocusPermit& permit, std::uint64_t now,
                                                 std::uint64_t wall,
                                                 ScopeFailure* observation) const {
    const auto value =
        permit.scope.operation == "release" ? current_release(now, wall) : current(now, wall);
    // 在途restore仍须匹配当前本人意图；已实际完整取得的原租约不因日常停止输入而误撤扬声器。
    // 这里只确认同原permit的实际拥有，不更新暂停冻结值；后续新restore仍携当前版本给引擎比较。
    const bool restored_and_owned = permit.scope.operation == "restore" &&
                                    permit.intent_version == binding_.intent_version &&
                                    driver_.owns(permit.scope);
    if (!admitted_ || !value)
        return scope_reject(observation, ScopeFailureCode::AuthorityCurrent);
    if (now >= permit.deadline ||
        permit.intent_version !=
            (permit.scope.operation == "restore" && !restored_and_owned
                 ? value->current_intent_version.value_or(binding_.intent_version)
                 : binding_.intent_version) ||
        std::none_of(issued_.begin(), issued_.end(),
                     [&](const auto& original) { return same_permit(original, permit); }))
        return scope_reject(observation, ScopeFailureCode::AuthorityPermit);
    observe_allocation();
    if (permit.scope.operation == "release" && value->closing)
        return (value->release && lease_deadline_.has_value() && allocated_) ||
               scope_reject(observation, ScopeFailureCode::AuthorityPermission);
    const bool operation_authorized = (permit.scope.operation == "acquire" && value->acquire) ||
                                      (permit.scope.operation == "suspend" && value->suspend) ||
                                      (permit.scope.operation == "restore" && value->restore) ||
                                      (permit.scope.operation == "release" &&
                                       value->release);  // 原独立操作权限不得被base权限替代。
    // 已签发lease仍供suspend/restore原引擎校验；当前操作token改变不能把旧lease误撤权。
    return (operation_authorized && value->authorized && value->peer_ready && !value->closing &&
            !ai_committed_ && (!permit.scope.expires_at || wall < *permit.scope.expires_at)) ||
           scope_reject(observation, ScopeFailureCode::AuthorityPermission);
}

// closed_release_valid 仅两份原宿主期限内清理已实际分配的整组，不能签新采集。
bool BrowserDuplexFocusAuthority::closed_release_valid(const FocusPermit& permit, std::uint64_t now,
                                                       std::uint64_t wall) const {
    const auto value = current_release(now, wall);
    return value && value->closing && value->release && permit.scope.operation == "release" &&
           lease_deadline_ && allocated_ && valid(permit, now, wall);
}
} // namespace ur::device
