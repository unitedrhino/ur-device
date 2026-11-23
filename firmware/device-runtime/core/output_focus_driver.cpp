// 文件说明：输出路径全局CAS、实际门控与取消栅栏；不包含网络、采集或默认业务授权。
#include "ur/device/output_focus_driver.hpp"
#include <algorithm>
#include <limits>
#include <utility>

namespace ur::device {
namespace {
constexpr std::uint64_t safe = 9007199254740991ULL;  // MQTT/JSON跨语言安全水位。
constexpr std::uint64_t budget = 2000;               // 固定门控等待预算，不能由重试延长。
// group比较原完整集合，允许顺序不同但不允许添加、删减或重复资源。
bool group(const std::vector<std::string>& one, const std::vector<std::string>& two) {
    return one.size() == two.size() && std::is_permutation(one.begin(), one.end(), two.begin());
}
// held区分实际输出所有权与悬挂/释放，后两者不授予当前播放许可。
bool held(std::string_view state) { return state == "acquired" || state == "restored"; }
// identity校验宿主绑定身份；正文命令还由focus_reply独立重验。
bool identity(const std::string& value) {
    return !value.empty() && value.size() <= 128 &&
           std::none_of(value.begin(), value.end(),
                        [](unsigned char c) { return c < 32 || c == 127; });
}
}  // namespace

OutputFocusDriver::OutputFocusDriver(std::vector<FocusOutputBinding> bindings,
                                     std::uint64_t initial_revision, FocusBindingMode mode)
    : revision_(initial_revision), initial_revision_(initial_revision), binding_mode_(mode) {
    valid_ = !bindings.empty() && bindings.size() <= 4 && initial_revision <= safe &&
             (mode == FocusBindingMode::Fixed || mode == FocusBindingMode::DeferredOnce ||
              mode == FocusBindingMode::DeferredReleased);
    if (!valid_)
        return;  // 非法容量不复制到有界管理集合，不触碰任何借用端口。
    for (auto& binding : bindings) {
        const bool scope = mode == FocusBindingMode::Fixed
                               ? identity(binding.session) && identity(binding.focus)
                               : binding.session.empty() && binding.focus.empty();
        valid_ = valid_ && scope && binding.port && !binding.resources.empty() &&
                 binding.resources.size() <= 2;
        for (std::size_t index = 0; index < binding.resources.size(); ++index) {
            const auto& resource = binding.resources[index];  // 不隐含控制microphone或camera。
            // vector 的有效迭代距离受其 difference_type 上界约束，显式转换不改变去重范围。
            valid_ = valid_ && (resource == "speaker" || resource == "display") &&
                     std::find(binding.resources.begin(),
                               binding.resources.begin() + static_cast<std::ptrdiff_t>(index),
                               resource) ==
                         binding.resources.begin() + static_cast<std::ptrdiff_t>(index);
        }
        for (const auto& old : paths_)
            valid_ = valid_ && old.binding.port != binding.port &&
                     (mode != FocusBindingMode::Fixed || !(old.binding.session == binding.session &&
                                                           old.binding.focus == binding.focus));
        Path path;  // 配置检查完之前不对借用端口产生动作。
        path.binding = std::move(binding);
        paths_.push_back(std::move(path));
    }
}
// bind_scope 不启用输出或提供业务授权；显式复用只清除已释放原身份，并保留旧身份栅栏。
bool OutputFocusDriver::bind_scope(std::size_t index, std::string session, std::string focus,
                                   std::uint64_t now) {
    if (binding_mode_ == FocusBindingMode::Fixed || !valid_ || !ready_ || pending_ ||
        index >= paths_.size() || !identity(session) || session.size() > 64 || !identity(focus) ||
        find(session, focus) ||
        std::find(retired_.begin(), retired_.end(), std::make_pair(session, focus)) !=
            retired_.end())
        return false;
    auto& path = paths_[index];
    const bool fresh =
        path.binding.session.empty() && path.binding.focus.empty() && path.state.empty();
    const bool released = binding_mode_ == FocusBindingMode::DeferredReleased &&
                          path.state == "released" && retired_.size() < 128;
    if ((!fresh && !released) || path.enabled || path.stopping || path.fact || path.refreshing ||
        path.unavailable)
        return false;
    // 绑定前重新观察所有实际路径；失效或时钟回退只能收敛关闭，不能接入新租约绕过栅栏。
    if (!poll(now) || !confirmed(path) ||
        !std::all_of(paths_.begin(), paths_.end(), [&](const auto& other) {
            return other.refreshing || other.unavailable || confirmed(other);
        }))
        return false;
    if (!fresh) {
        retired_.emplace_back(path.binding.session, path.binding.focus);
        path.state.clear();
        path.applied.reset();  // 旧token所有权不能被新业务继承，实际false票据仍保持原水位。
    }
    path.binding.session = std::move(session);
    path.binding.focus = std::move(focus);
    return true;
}
std::optional<std::size_t> OutputFocusDriver::find(std::string_view session,
                                                   std::string_view focus) const {
    for (std::size_t index = 0; index < paths_.size(); ++index)
        if (paths_[index].binding.session == session && paths_[index].binding.focus == focus)
            return index;
    return {};
}
bool OutputFocusDriver::confirmed(const Path& path) const {
    return path.ticket && path.binding.port->output_gate_confirmed(*path.ticket, path.enabled);
}
OutputFocusDriver::~OutputFocusDriver() {
    for (const auto& path : paths_)
        stop_scope(path.binding.session, path.binding.focus, path.binding.resources);
}
bool OutputFocusDriver::initialize(std::uint64_t now) {
    if (!valid_ || clock_ || initializing_ || ready_ || used_ ||
        now > std::numeric_limits<std::uint64_t>::max() - budget)
        return false;
    clock_ = now;
    initializing_ = true;
    for (auto& path : paths_) {
        path.enabled = false;
        path.deadline = now + budget;
        path.ticket = path.binding.port->request_output_gate(false, now);
        if (!path.ticket) {
            fail();
            return false;
        }
    }
    return true;  // 等全部实际端口回原票据，绝不是同步idle成功。
}
bool OutputFocusDriver::initial_idle() const {
    return ready_ && valid_ && !used_ && revision_ == initial_revision_ &&
           std::all_of(paths_.begin(), paths_.end(), [&](const auto& path) {
               return !path.refreshing && !path.unavailable && confirmed(path);
           });
}
// refresh_idle 保留唯一Path票据所有权；仅明确原无lease状态的新false观察，不将被动关闭当显式释放。
bool OutputFocusDriver::refresh_idle(std::size_t index, std::uint64_t now) {
    if (!valid_ || !ready_ || !clock_ || pending_ || index >= paths_.size() || now < *clock_ ||
        now > std::numeric_limits<std::uint64_t>::max() - budget)
        return false;
    auto& path = paths_[index];
    if (path.enabled || path.stopping || path.fact || path.refreshing || path.unavailable ||
        (!path.state.empty() && path.state != "released") || !confirmed(path))
        return false;
    const auto ticket = path.binding.port->request_output_gate(false, now);
    if (!ticket)
        return false;
    path.ticket = ticket;
    path.deadline = now + budget;
    path.refreshing = true;
    clock_ = now;
    return true;
}
bool OutputFocusDriver::idle_refresh_ready(std::size_t index) const {
    if (!valid_ || !ready_ || index >= paths_.size())
        return false;
    const auto& path = paths_[index];
    return !path.enabled && !path.stopping && !path.refreshing && !path.unavailable && !path.fact &&
           (path.state.empty() || path.state == "released") && confirmed(path);
}
bool OutputFocusDriver::idle_refresh_failed(std::size_t index) const {
    return !valid_ || index >= paths_.size() || paths_[index].unavailable;
}
// initial_idle_observation 聚合全部实际管理路径；同资源的不同路径均须确认，不能只证明其中一条。
std::optional<FocusIdleObservation> OutputFocusDriver::initial_idle_observation() const {
    if (!initial_idle())
        return {};
    FocusIdleObservation observation{{}, revision_};
    for (const auto& path : paths_)
        for (const auto& resource : path.binding.resources)
            if (std::find(observation.resources.begin(), observation.resources.end(), resource) ==
                observation.resources.end())
                observation.resources.push_back(resource);
    return observation;
}

// fail保留失败状态，不生成成功Fact或回零；仍由后续poll尽力请求原路径关闭。
void OutputFocusDriver::fail() {
    valid_ = ready_ = initializing_ = false;
    if (pending_) {
        auto& path = paths_[pending_->path];
        path.fact = FocusFact{pending_->generation, false, "", path.binding.resources, revision_};
        pending_.reset();
    }
    for (auto& path : paths_) {
        if (!path.stopping && (path.enabled || !confirmed(path))) {
            path.enabled = false;
            path.ticket.reset();  // 原true确认不能成为关闭完成；待端口可接纳false再观察。
        }
        path.stopping = true;  // 之后保留同一false在途票据，不循环取消真实关闭确认。
    }
}
bool OutputFocusDriver::poll(std::uint64_t now) {
    if (!clock_)
        return false;
    if (now < *clock_ || now > std::numeric_limits<std::uint64_t>::max() - budget)
        fail();
    clock_ = now;
    for (auto& path : paths_) {
        if (path.refreshing) {
            if (confirmed(path))
                path.refreshing = false;
            else if (now >= path.deadline) {
                path.unavailable = true;
                path.refreshing = false;
            }
            continue;  // 局部重建未完成不破坏原Released历史或全局其它正在输出的路径。
        }
        if (path.unavailable)
            continue;
        if (path.stopping && !path.ticket && path.binding.port)
            path.ticket = path.binding.port->request_output_gate(false, now);
        if (path.ticket && !confirmed(path) && now >= path.deadline)
            fail();
    }
    if (!valid_)
        return false;
    if (initializing_) {
        if (!std::all_of(paths_.begin(), paths_.end(),
                         [&](const auto& path) { return confirmed(path); }))
            return true;
        initializing_ = false;
        ready_ = true;
    }
    for (std::size_t index = 0; index < paths_.size(); ++index)
        if ((!pending_ || pending_->path != index) && !paths_[index].stopping &&
            !paths_[index].refreshing && !paths_[index].unavailable && !confirmed(paths_[index])) {
            fail();  // 其他路径证明失效时，不能先为目标路径推进全局成功水位。
            return false;
        }
    if (pending_) {
        auto& path = paths_[pending_->path];
        if (!confirmed(path))
            return true;
        ++revision_;  // 唯一全局原CAS实际完成后推进一次，不因各租约或队列接纳递增。
        const auto& operation = pending_->command.operation;
        path.state = operation == "acquire"   ? "acquired"
                     : operation == "restore" ? "restored"
                     : operation == "suspend" ? "suspended"
                                              : "released";
        if (operation == "release")
            path.stopping = false;
        // 仅新原release票据实际完成才解除被动关闭栅栏；不提前标Released或人工清旧lease。
        path.applied = pending_->command;
        path.fact =
            FocusFact{pending_->generation, true, path.state, path.binding.resources, revision_};
        pending_.reset();
    }
    return true;
}
bool OutputFocusDriver::begin(const FocusCommand& command, std::uint64_t generation) {
    const auto index = find(command.envelope.session, command.focus_id);
    Envelope checked;  // 公开结构体也必须经过原报文封闭校验，不能只检查部分字段。
    const auto target = command.operation == "acquire"   ? "acquired"
                        : command.operation == "restore" ? "restored"
                        : command.operation == "suspend" ? "suspended"
                                                         : "released";
    if (!ready_ || !valid_ || !clock_ || pending_ || !index || !generation ||
        command.expected_revision != revision_ || revision_ >= safe ||
        focus_reply(command, target, revision_ + 1, 200, "", checked) != WireResult::Ok)
        return false;
    for (const auto& other : paths_)
        if (!other.stopping && !other.refreshing && !other.unavailable && !confirmed(other))
            return false;  // 未先poll也不能用已失效的其他路径证明取得输出。
    auto& path = paths_[*index];
    const bool enable = command.operation == "acquire" || command.operation == "restore";
    const bool stopped_release = command.operation == "release" && path.stopping && !path.enabled &&
                                 confirmed(path) && !path.state.empty() && path.state != "released";
    // 原实际被动false仍不是显式release；只允许原关闭许可另发新false，原CAS及未取事实仍严格保留。
    if (path.fact || (path.stopping && !stopped_release) || path.refreshing || path.unavailable ||
        !group(command.resources, path.binding.resources) ||
        (command.operation == "acquire" && !path.state.empty()) ||
        (command.operation == "restore" && path.state != "suspended") ||
        (command.operation == "suspend" && !held(path.state)) ||
        (command.operation == "release" && (path.state.empty() || path.state == "released")))
        return false;
    for (std::size_t other = 0; other < paths_.size(); ++other)
        if (enable && other != *index &&
            (held(paths_[other].state) || paths_[other].stopping || paths_[other].refreshing ||
             paths_[other].unavailable))
            for (const auto& resource : command.resources)
                if (std::find(paths_[other].binding.resources.begin(),
                              paths_[other].binding.resources.end(),
                              resource) != paths_[other].binding.resources.end())
                    return false;  // 优先级不自行提权；平台必须先完成原显式suspend/release。
    const auto ticket = path.binding.port->request_output_gate(enable, *clock_);
    if (!ticket)
        return false;  // 未受理保证本驱动不改变原所有权、修订号或在途记录。
    path.ticket = ticket;
    path.enabled = enable;
    path.deadline = *clock_ + budget;
    pending_ = Pending{*index, command, generation};
    used_ = true;
    return true;
}
std::optional<FocusFact> OutputFocusDriver::take_fact(std::string_view session,
                                                      std::string_view focus) {
    const auto index = find(session, focus);
    if (!index || !paths_[*index].fact)
        return {};
    auto result = std::move(paths_[*index].fact);
    paths_[*index].fact.reset();
    return result;
}
bool OutputFocusDriver::owns(const FocusCommand& applied) const {
    const auto index = find(applied.envelope.session, applied.focus_id);
    if (!valid_ || !ready_ || !index || (pending_ && pending_->path == *index))
        return false;
    const auto& path = paths_[*index];
    for (std::size_t other = 0; other < paths_.size(); ++other)
        if ((!pending_ || pending_->path != other) && !paths_[other].stopping &&
            !paths_[other].refreshing && !paths_[other].unavailable && !confirmed(paths_[other]))
            return false;
    return !path.stopping && !path.refreshing && !path.unavailable && held(path.state) &&
           confirmed(path) && path.applied &&
           path.applied->envelope.token == applied.envelope.token &&
           group(path.binding.resources, applied.resources);
}
bool OutputFocusDriver::stop_scope(std::string_view session, std::string_view focus,
                                   const std::vector<std::string>& resources) {
    const auto index = find(session, focus);
    if (!index || !clock_ || !group(paths_[*index].binding.resources, resources))
        return false;
    auto& path = paths_[*index];
    if (pending_ && pending_->path == *index)
        pending_.reset();  // 被动关闭不能成为原release成功回执，也不推进全局修订号。
    path.stopping = true;
    if (path.enabled) {
        path.enabled = false;
        path.ticket.reset();
        path.deadline = *clock_ + budget;
    }
    if (!path.ticket)
        path.ticket = path.binding.port->request_output_gate(false, *clock_);
    return confirmed(path);  // 只有实际false票据确认，才证明本原组已停止输出。
}
}  // namespace ur::device
