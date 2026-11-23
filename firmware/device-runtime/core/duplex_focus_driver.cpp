// 文件说明：完整组CAS与实际双票据执行；本人仅停止输入保持原租约/输出，正式release仍须双false。
#include "ur/device/duplex_focus_driver.hpp"
#include <algorithm>
#include <limits>

namespace ur::device {
// duplex_driver_failure_name 将reason映射为闭集字面量；返回unknown而非输出未知枚举的自由内容。
std::string_view duplex_driver_failure_name(DuplexDriverFailureReason reason) {
    switch (reason) {
    case DuplexDriverFailureReason::NoClock:
        return "noClock";
    case DuplexDriverFailureReason::ClockRollback:
        return "clockRollback";
    case DuplexDriverFailureReason::ClockOverflow:
        return "clockOverflow";
    case DuplexDriverFailureReason::InvalidLatched:
        return "invalidLatched";
    case DuplexDriverFailureReason::BootDeadline:
        return "bootDeadline";
    case DuplexDriverFailureReason::OutputUnconfirmed:
        return "outputUnconfirmed";
    case DuplexDriverFailureReason::InputUnconfirmed:
        return "inputUnconfirmed";
    case DuplexDriverFailureReason::PendingDeadline:
        return "pendingDeadline";
    case DuplexDriverFailureReason::InputTicketOrder:
        return "inputTicketOrder";
    case DuplexDriverFailureReason::OutputTicketOrder:
        return "outputTicketOrder";
    }
    return "unknown";
}
// enable_failure_diagnostics 仅在原初始化前开启观察，无返回值；迟到调用不追溯既有故障。
void DuplexFocusDriver::enable_failure_diagnostics() {
    if (!clock_ && valid_)
        observe_failure_ = true;
}
// record_failure 保存reason与固定path首因，无返回值；默认关闭及后继调用都不改变已锁事实。
void DuplexFocusDriver::record_failure(DuplexDriverFailureReason reason, int path) {
    if (observe_failure_ && !failure_)
        failure_ = DuplexDriverFailure{reason, path};
}
namespace {
constexpr std::uint64_t safe = 9007199254740991ULL;  // 跨语言水位上限，不允许回零。
constexpr std::uint64_t budget = 8000;               // 候选绑定/初始化确认门控八秒：真实部署需覆盖MQTT往返与媒体协商节奏，重试或迟到不续期。
const std::vector<std::string> resources{"microphone", "speaker"};  // 唯一受管完整组。
constexpr std::uint64_t input_stop_budget = 2000;  // 本人仅停止原输入的实际回执预算，重投不续。
// identity只校验可信宿主提供的原绑定形状，不能替代授权。
bool identity(std::string_view value, std::size_t maximum) {
    return !value.empty() && value.size() <= maximum &&
           std::none_of(value.begin(), value.end(),
                        [](unsigned char c) { return c < 32 || c == 127; });
}
// group接受原集合不同顺序，但不接受半组、重复或多余资源。
bool group(const std::vector<std::string>& value) {
    return value.size() == resources.size() &&
           std::is_permutation(value.begin(), value.end(), resources.begin());
}
bool held(std::string_view state) { return state == "acquired" || state == "restored"; }
// same_operation防止调用者借历史token用其他CAS/意图字段冒充当前拥有。
bool same_operation(const FocusCommand& left, const FocusCommand& right) {
    return left.envelope.token == right.envelope.token && left.operation == right.operation &&
           left.expected_revision == right.expected_revision && left.priority == right.priority &&
           left.resume_policy == right.resume_policy && left.expires_at == right.expires_at &&
           group(right.resources);
}
}  // namespace

// 保留 boot_generation/revision 构造合同；启动代次须非零，初始资源修订允许零，不更换既有宿主接口。
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
DuplexFocusDriver::DuplexFocusDriver(InputOwnershipGatePort& input,
                                     std::vector<OutputGroupGatePort*> outputs,
                                     std::uint64_t boot_generation, std::uint64_t revision)
    // NOLINTEND(bugprone-easily-swappable-parameters)
    : input_(input), boot_(boot_generation), revision_(revision), initial_revision_(revision) {
    valid_ = boot_ && boot_ <= safe && revision_ <= safe && !outputs.empty() && outputs.size() <= 2;
    if (!valid_)
        return;
    for (auto* output : outputs) {
        valid_ = valid_ && output &&
                 std::none_of(paths_.begin(), paths_.end(),
                              [&](const auto& path) { return path.output == output; });
        Path path;
        path.output = output;
        paths_.push_back(std::move(path));
    }
}
DuplexFocusDriver::~DuplexFocusDriver() {
    if (clock_)
        for (auto& path : paths_)
            close_path(path, *clock_);
}
bool DuplexFocusDriver::request_input(Path& path, bool enabled, std::uint64_t now) {
    path.request.enabled = enabled;
    const auto ticket = input_.request_input_gate(path.request, now);
    path.input_ticket = ticket;
    if (!ticket)
        return false;
    if (!*ticket || *ticket <= last_input_ticket_) {
        record_failure(DuplexDriverFailureReason::InputTicketOrder, static_cast<int>(&path - paths_.data()));
        valid_ = false;
        return false;
    }
    last_input_ticket_ = *ticket;
    return true;
}
bool DuplexFocusDriver::request_output(Path& path, bool enabled, std::uint64_t now) {
    if (!path.output)
        return false;
    const auto ticket = path.output->request_output_gate(enabled, now);
    path.output_ticket = ticket;
    if (!ticket)
        return false;
    if (!*ticket || *ticket <= path.last_output_ticket) {
        record_failure(DuplexDriverFailureReason::OutputTicketOrder, static_cast<int>(&path - paths_.data()));
        valid_ = false;
        return false;
    }
    path.last_output_ticket = *ticket;
    path.output_enabled = enabled;
    return true;
}
bool DuplexFocusDriver::confirmed(const Path& path, ScopeFailure* observation) const {
    if (!path.output || !path.output_ticket ||
        !path.output->output_gate_confirmed(*path.output_ticket, path.output_enabled))
        return scope_reject(observation, ScopeFailureCode::DriverOutput);
    return path.session.empty() ||
           (path.input_ticket && input_.input_gate_confirmed(*path.input_ticket, path.request)) ||
           scope_reject(observation, ScopeFailureCode::DriverInput);
}
bool DuplexFocusDriver::unallocated(const Path& path) const {
    return path.session.empty() && path.focus.empty() && path.state.empty() &&
           !path.request.enabled && !path.output_enabled && !path.applied && !path.input_ticket && !path.binding &&
           !path.stopping && !path.unavailable;
}
bool DuplexFocusDriver::initialize(std::uint64_t now) {
    if (!valid_ || clock_ || used_ || now > std::numeric_limits<std::uint64_t>::max() - budget)
        return false;
    clock_ = now;
    initializing_ = true;
    for (auto& path : paths_) {
        path.deadline = now + budget;
        if (!request_output(path, false, now)) {
            fail(now);
            return false;
        }
    }
    return true;
}
std::optional<FocusIdleObservation> DuplexFocusDriver::initial_idle_observation() const {
    const auto probe = input_.initial_idle_observation();
    if (!valid_ || !ready_ || used_ || revision_ != initial_revision_ || !probe ||
        *probe != boot_ || !std::all_of(paths_.begin(), paths_.end(), [&](const auto& path) {
            return confirmed(path);
        }))
        return {};
    return FocusIdleObservation{resources, revision_};
}
std::optional<std::size_t> DuplexFocusDriver::find(std::string_view session,
                                                   std::string_view focus) const {
    for (std::size_t index = 0; index < paths_.size(); ++index)
        if (paths_[index].session == session && paths_[index].focus == focus && !session.empty())
            return index;
    return {};
}
bool DuplexFocusDriver::bind_scope(std::size_t index, std::string session, std::string focus,
                                   std::uint64_t generation, std::uint64_t now) {
    if (!valid_ || !ready_ || !clock_ || now < *clock_ || pending_ || index >= paths_.size() ||
        !identity(session, 64) || !identity(focus, 128) || !generation || generation > safe ||
        now > std::numeric_limits<std::uint64_t>::max() - budget || !paths_[index].session.empty())
        return false;
    if (!used_ && !initial_idle_observation())
        return false;  // 首次绑定不能借已失效的pre-session探针绕过真实采集空闲。
    for (const auto& path : paths_)
        if (path.binding || path.stopping ||
            (!confirmed(path) && !(used_ && &path == &paths_[index] && unallocated(path))) ||
            path.session == session ||
            (!path.session.empty() && path.request.route_generation == generation))
            return false;
    auto& path = paths_[index];
    path.session = std::move(session);
    path.focus = std::move(focus);
    path.request = {path.session, generation, false};
    path.deadline = now + budget;
    path.binding = true;
    used_ = true;
    clock_ = now;
    if (!request_input(path, false, now) || !request_output(path, false, now)) {
        // 候选只请求自身false，从未取得租约；背压不等于原AI授权失效。
        // 但共享HAL返回非单调票据属于全局事实不可信，仍须关闭完整驱动。
        if (!valid_) {
            fail(now);
            return false;
        }
        path.binding = false;
        path.unavailable = true;
        close_path(path, now);
        return false;
    }
    return true;
}
// released_idle_observation被动stop不生成idle基线；必须有正式整组release及已消费原完成事实。
std::optional<FocusIdleObservation> DuplexFocusDriver::released_idle_observation(std::size_t index) const {
    if (!valid_ || !ready_ || !clock_ || pending_ || index >= paths_.size()) return {};
    const auto& path = paths_[index];
    if (path.session.empty() || path.state != "released" || !path.applied ||
        path.applied->operation != "release" || path.fact || path.binding || path.unavailable ||
        path.request.enabled || path.output_enabled || !confirmed(path) ||
        !std::all_of(paths_.begin(), paths_.end(), [&](const auto& other) {
            return !other.binding && !other.request.enabled && !other.output_enabled && confirmed(other);
        })) return {};
    return FocusIdleObservation{resources, revision_};
}
// replace_released_focus只接同原会话退役后的新焦点；保留原媒体路由、共享CAS、递增票据及墓碑。
bool DuplexFocusDriver::replace_released_focus(std::size_t index, std::string_view session,
                                               std::string_view focus, std::string next,
                                               std::uint64_t now) {
    if (!released_idle_observation(index) || !clock_ || now < *clock_ ||
        now > std::numeric_limits<std::uint64_t>::max() - budget || retired_.size() >= 128 ||
        !identity(next, 128) || next == focus || paths_[index].session != session ||
        paths_[index].focus != focus ||
        std::any_of(paths_.begin(), paths_.end(), [&](const auto& path) { return path.focus == next; }) ||
        std::any_of(retired_.begin(), retired_.end(), [&](const auto& old) {
            return old.session == session && old.focus == next;
        })) return false;
    auto& path = paths_[index];
    retired_.push_back({path.session, path.focus});
    path.focus = std::move(next);
    path.state.clear();
    path.applied.reset();
    path.stopping = false;
    path.binding = true;
    path.deadline = now + budget;
    clock_ = now;
    // 新scope重新取得两个false票据；旧true/旧false均不能匹配本次准备。
    if (!request_input(path, false, now) || !request_output(path, false, now)) {
        path.binding = false;
        path.unavailable = true;
        close_path(path, now);
        return false;
    }
    return true;
}
bool DuplexFocusDriver::scope_ready(std::size_t index) const {
    return valid_ && ready_ && index < paths_.size() && !paths_[index].session.empty() &&
           !paths_[index].binding && !paths_[index].input_stopping && !paths_[index].stopping && !paths_[index].unavailable &&
           confirmed(paths_[index]);
}
bool DuplexFocusDriver::stopped_scope_ready(std::size_t index) const {
    if (!valid_ || !ready_ || pending_ || index >= paths_.size())
        return false;
    const auto& path = paths_[index];
    return !path.session.empty() && path.stopping && !path.binding && !path.unavailable &&
           !path.request.enabled && !path.output_enabled && path.applied && !path.fact && !path.state.empty() &&
           path.state != "released" && confirmed(path);
}
bool DuplexFocusDriver::quiescent_scope_ready(std::size_t index) const {
    if (!valid_ || !ready_ || pending_ || index >= paths_.size())
        return false;
    const auto& path = paths_[index];
    return !path.session.empty() && !path.binding && !path.unavailable && !path.input_stopping &&
           !path.request.enabled && !path.output_enabled &&
           !path.fact && confirmed(path);
}
bool DuplexFocusDriver::refresh_idle_output(std::size_t index, std::uint64_t now) {
    if (!valid_ || !ready_ || !clock_ || now < *clock_ || pending_ || index >= paths_.size() ||
        !unallocated(paths_[index]) || now > std::numeric_limits<std::uint64_t>::max() - budget)
        return false;
    if (!used_) {
        // 首个人工Peer换scope只撤销输出观察；原完整boot须确曾完成且输入当前仍独立确认空闲。
        // 不创造影子AI租约或替代首次bind的完整initial_idle_observation核验。
        const auto probe = input_.initial_idle_observation();
        if (!probe || *probe != boot_ || revision_ != initial_revision_ ||
            confirmed(paths_[index]) ||
            !std::all_of(paths_.begin(), paths_.end(),
                         [&](const auto& path) { return unallocated(path); }))
            return false;
    }
    for (std::size_t other = 0; other < paths_.size(); ++other)
        if (other != index && (paths_[other].binding || paths_[other].stopping ||
                               paths_[other].unavailable || !confirmed(paths_[other])))
            return false;
    auto& path = paths_[index];
    path.deadline = now + budget;
    clock_ = now;
    if (!request_output(path, false, now)) {
        // 没有原候选租约，拒绝仅隔离该候选；非单调票据则仍是全局HAL契约失效。
        path.unavailable = true;
        if (!valid_)
            fail(now);
        return false;
    }
    path.binding = true;  // 复用有界false等待；session/focus/input_ticket仍空，不授任何绑定资格。
    path.refreshed_idle = true;
    return true;
}
bool DuplexFocusDriver::idle_output_ready(std::size_t index) const {
    return valid_ && ready_ && index < paths_.size() && unallocated(paths_[index]) &&
           confirmed(paths_[index]) &&
           (used_ || (paths_[index].refreshed_idle && initial_idle_observation().has_value()));
}
void DuplexFocusDriver::close_path(Path& path, std::uint64_t now) {
    if (!path.output)
        return;
    if (path.request.enabled || path.output_enabled) {
        path.input_ticket.reset();
        path.output_ticket.reset();
        path.request.enabled = false;
        path.output_enabled = false;
        path.deadline =
            now <= std::numeric_limits<std::uint64_t>::max() - budget ? now + budget : now;
    }
    path.stopping = true;
    path.input_stopping = false;
    if (!path.session.empty() && !path.input_ticket)
        (void)request_input(path, false, now);
    if (!path.output_ticket)
        (void)request_output(path, false, now);
}
void DuplexFocusDriver::fail(std::uint64_t now) {
    valid_ = ready_ = initializing_ = false;
    if (pending_) {
        paths_[pending_->path].fact =
            FocusFact{pending_->generation, false, "", resources, revision_};
        pending_.reset();
    }
    for (auto& path : paths_)
        close_path(path, now);
}
bool DuplexFocusDriver::poll(std::uint64_t now) {
    if (!clock_) {
        record_failure(DuplexDriverFailureReason::NoClock);
        return false;
    }
    if (now < *clock_ || now > std::numeric_limits<std::uint64_t>::max() - budget) {
        record_failure(now < *clock_ ? DuplexDriverFailureReason::ClockRollback : DuplexDriverFailureReason::ClockOverflow);
        fail(now);
    }
    clock_ = now;
    if (!valid_) {
        record_failure(DuplexDriverFailureReason::InvalidLatched);
        for (auto& path : paths_)
            close_path(path, now);
        return false;
    }
    if (initializing_) {
        const auto probe = input_.initial_idle_observation();
        const bool complete = probe && *probe == boot_ &&
                              std::all_of(paths_.begin(), paths_.end(),
                                          [&](const auto& path) { return confirmed(path); });
        if (!complete) {
            if (now >= paths_[0].deadline) {
                record_failure(DuplexDriverFailureReason::BootDeadline);
                fail(now);
                return false;
            }
            return true;
        }
        ready_ = true;
        initializing_ = false;
    }
    for (std::size_t index = 0; index < paths_.size(); ++index) {
        auto& path = paths_[index];
        // 后继候选绑定媒体scope可能撤销旧boot票据，但从未分配的路径没有正在运行的租约。
        // 不因此终结仍有实际双票据的AI；新bind必须另取false，begin仍拒绝任何未知原路径。
        if (used_ && unallocated(path) && !confirmed(path))
            continue;
        if (path.unavailable) {
            close_path(path, now);
            continue;  // 未取得候选的false确认未知不关闭原仍有效的AI租约。
        }
        if (path.stopping && !(pending_ && pending_->path == index)) {
            close_path(path, now);
            continue;
        }
        const bool waiting = path.binding || path.input_stopping || (pending_ && pending_->path == index);
        ScopeFailure observed; // 只复用本次原票据读取的失败分类，不额外查询端口。
        if (!confirmed(path, observe_failure_ ? &observed : nullptr)) {
            if (path.binding && now >= path.deadline) {
                path.binding = false;
                path.unavailable = true;
                close_path(path, now);
                continue;
            }
            if (!waiting || now >= path.deadline) {
                record_failure(waiting ? DuplexDriverFailureReason::PendingDeadline :
                    observed.code == ScopeFailureCode::DriverInput ? DuplexDriverFailureReason::InputUnconfirmed : DuplexDriverFailureReason::OutputUnconfirmed,
                    static_cast<int>(index));
                fail(now);
                return false;
            }
        } else {
            if (path.binding)
            path.binding = false;
            if (path.input_stopping) path.input_stopping = false;
        }
    }
    if (pending_ && confirmed(paths_[pending_->path])) {
        auto& path = paths_[pending_->path];
        const auto& operation = pending_->command.operation;
        path.state = operation == "acquire"   ? "acquired"
                     : operation == "restore" ? "restored"
                     : operation == "suspend" ? "suspended"
                                              : "released";
        path.applied = pending_->command;
        path.fact = FocusFact{pending_->generation, true, path.state, resources, ++revision_};
        pending_.reset();
    }
    return true;
}
bool DuplexFocusDriver::begin(const FocusCommand& command, std::uint64_t generation) {
    const auto index = find(command.envelope.session, command.focus_id);
    const auto state = command.operation == "acquire"   ? "acquired"
                       : command.operation == "restore" ? "restored"
                       : command.operation == "suspend" ? "suspended"
                                                        : "released";
    Envelope checked;
    if (!valid_ || !ready_ || !clock_ || pending_ || !index || !generation || revision_ >= safe ||
        command.expected_revision != revision_ || !group(command.resources) ||
        focus_reply(command, state, revision_ + 1, 200, "", checked) != WireResult::Ok)
        return false;
    auto& path = paths_[*index];
    const bool enabled = command.operation == "acquire" || command.operation == "restore";
    // 原关闭已实际围栏之后，只准同一租约正式release；不能复活采集、替代授权或跳过新票据。
    const bool cleanup_release = path.stopping && command.operation == "release" &&
                                 !path.request.enabled && !path.output_enabled && path.applied && confirmed(path);
    if (path.fact || path.binding || path.input_stopping || (path.stopping && !cleanup_release) || path.unavailable ||
        !confirmed(path) || (command.operation == "acquire" && !path.state.empty()) ||
        (command.operation == "restore" && path.state != "suspended") ||
        (command.operation == "suspend" && !held(path.state)) ||
        (command.operation == "release" && (path.state.empty() || path.state == "released")))
        return false;
    for (std::size_t other = 0; other < paths_.size(); ++other) {
        // 另一原腿也可能已退出；只允许全组原false已确认的清理，不向未知退役腿授开启权。
        // 另一腿正式释放且原fact已消费后，当前已持有腿可保持/恢复；不回授退役腿权限。
        const auto& applied =
            paths_[other].applied;  // 同一原腿的可选事实，不靠重复下标表达式推断存在。
        const bool retired_idle =
            other != *index && (command.operation == "suspend" || command.operation == "restore") &&
            paths_[other].state == "released" && applied.has_value() &&
            applied->operation == "release" && !paths_[other].fact &&
            !paths_[other].request.enabled && !paths_[other].output_enabled &&
            confirmed(paths_[other]);
        const bool stopped_idle =
            cleanup_release && !paths_[other].request.enabled &&
                                  !paths_[other].output_enabled && confirmed(paths_[other]);
        if (paths_[other].binding || (paths_[other].stopping && !stopped_idle && !retired_idle) ||
            (paths_[other].unavailable && !stopped_idle) || !confirmed(paths_[other]) ||
            (enabled && other != *index && held(paths_[other].state)))
            return false;
    }
    const auto old_request = path.request;
    const auto old_ticket = path.input_ticket;
    // 原完整组恢复只授资源许可；本人已停输入仍请求实际false，输出独立维持原目标。
    if (!request_input(path, enabled && !path.input_stop_latched, *clock_)) {
        if (!path.input_ticket) {
            path.request = old_request;
            path.input_ticket = old_ticket;
            return false;
        }
        pending_ = Pending{*index, command, generation};
        fail(*clock_);
        return true;
    }
    pending_ = Pending{*index, command, generation};
    path.deadline = *clock_ + budget;
    used_ = true;
    if (!request_output(path, enabled, *clock_))
        fail(*clock_);
    return true;
}
std::optional<FocusFact> DuplexFocusDriver::take_fact(std::string_view session,
                                                      std::string_view focus) {
    const auto index = find(session, focus);
    if (!index)
        return {};
    auto value = std::move(paths_[*index].fact);
    paths_[*index].fact.reset();
    return value;
}
bool DuplexFocusDriver::owns(const FocusCommand& applied) const {
    return owns_observed(applied, nullptr);
}
// owns_observed 原当前路径及all_of仍分别核票据，不为诊断去重或追加查询。
bool DuplexFocusDriver::owns_observed(const FocusCommand& applied,
                                      ScopeFailure* observation) const {
    const auto index = find(applied.envelope.session, applied.focus_id);
    if (!valid_ || !ready_ || !index || (pending_ && pending_->path == *index))
        return scope_reject(observation, ScopeFailureCode::DriverState);
    const auto& path = paths_[*index];
    if (path.binding || path.stopping || path.input_stopping || !held(path.state))
        return scope_reject(observation, ScopeFailureCode::DriverState);
    if (!confirmed(path, observation))
        return false;
    if (!path.applied || !same_operation(*path.applied, applied))
        return scope_reject(observation, ScopeFailureCode::DriverState);
    return std::all_of(paths_.begin(), paths_.end(), [&](const auto& other) {
        // 从未取得的候选准备及旧已暂停腿退役，不撤销当前原完整组实际所有权。
        // 这些例外不授予新acquire；begin仍明确拒绝未知候选路径。
        return ((other.binding || other.unavailable) && other.state.empty() &&
                !other.request.enabled && !other.output_enabled) ||
               (used_ && unallocated(other)) ||
               (other.stopping && !other.request.enabled && confirmed(other, observation)) ||
               (!other.binding && !other.stopping && !other.unavailable &&
                confirmed(other, observation)) ||
               scope_reject(observation, ScopeFailureCode::DriverOther);
    });
}
// stop_input 原整组实际租约仍保留，固定等待仅对应新原输入false票据，不延租约期限。
bool DuplexFocusDriver::stop_input(std::string_view session, std::string_view focus,
                                   std::uint64_t now) {
    const auto index = find(session, focus);
    if (!valid_ || !ready_ || !clock_ || now < *clock_ || now > safe - input_stop_budget ||
        !index || pending_)
        return false;
    auto& path = paths_[*index];
    if (path.input_stopping) return true;  // 原本人重复停止不换票据或延两秒等待。
    if (!path.applied || path.binding || path.stopping || !held(path.state) || !confirmed(path))
        return false;
    if (!path.request.enabled) return path.output_enabled;  // 原已停止只读重放，无开启资格。
    const auto request = path.request;
    const auto ticket = path.input_ticket;
    if (!request_input(path, false, now)) {
        if (valid_ && !path.input_ticket) {
            path.request = request;
            path.input_ticket = ticket;
        } else
            fail(now);
        return false;
    }
    clock_ = now;
    path.deadline = now + input_stop_budget;
    path.input_stopping = true;
    path.input_stop_latched = true; // 仅原可信停止已受理才锁存，不因未受理请求修改本人意图。
    return true;
}

// input_stopped 原本人关闭完成与完整组释放分开，只读确认不授恢复权。
bool DuplexFocusDriver::input_stopped(std::string_view session, std::string_view focus) const {
    const auto index = find(session, focus);
    return valid_ && ready_ && index && !paths_[*index].input_stopping &&
           !paths_[*index].request.enabled && paths_[*index].output_enabled &&
           held(paths_[*index].state) && confirmed(paths_[*index]);
}
bool DuplexFocusDriver::stop_scope(std::string_view session, std::string_view focus,
                                   const std::vector<std::string>& requested) {
    const auto index = find(session, focus);
    if (!clock_ || !index || !group(requested))
        return false;
    if (pending_ && pending_->path == *index) {
        paths_[*index].fact = FocusFact{pending_->generation, false, "", resources, revision_};
        pending_.reset();
    }
    close_path(paths_[*index], *clock_);
    return confirmed(paths_[*index]);
}
}  // namespace ur::device
