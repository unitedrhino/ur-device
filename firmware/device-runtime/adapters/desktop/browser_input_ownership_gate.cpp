// 文件说明：真实两屏障输入门控；命令接纳、UI指标和半组回执均不能成为整组事实。
#include "browser_input_ownership_gate.hpp"
#include <limits>

namespace ur::device {
BrowserInputOwnershipGate::BrowserInputOwnershipGate(std::uint64_t boot, Source source, Scope scope,
                                                     Fence fence)
    : source_(std::move(source)), scope_(std::move(scope)), fence_(std::move(fence)), boot_(boot) {
    closed_ = !boot_ || boot_ > 9007199254740991ULL || !source_ || !scope_ || !fence_;
}
bool BrowserInputOwnershipGate::live() const {
    if (closed_ || !source_)
        return false;
    try {
        return source_();
    } catch (...) {
        return false;
    }
}
bool BrowserInputOwnershipGate::scope_live(const InputGateRequest& request) const {
    if (!live() || !scope_)
        return false;
    try {
        return scope_(request);
    } catch (...) {
        return false;
    }
}
std::optional<std::size_t> BrowserInputOwnershipGate::route(const InputGateRequest& request) const {
    for (std::size_t index = 0; index < routes_.size(); ++index)
        if (routes_[index].session == request.session &&
            routes_[index].generation == request.route_generation)
            return index;
    return {};
}
bool BrowserInputOwnershipGate::bind_route(std::string session, std::uint64_t generation,
                                           BrowserInputRouteKind kind) {
    InputGateRequest request{std::move(session), generation, false};
    if (!scope_live(request) || routes_.size() >= 2 ||
        browser_input_ownership_command(request, 1).empty() ||
        (kind != BrowserInputRouteKind::Udp && kind != BrowserInputRouteKind::HumanSender &&
         kind != BrowserInputRouteKind::AISender))
        return false;
    for (const auto& existing : routes_)
        if (existing.session == request.session || existing.generation == generation)
            return false;
    routes_.push_back({request.session, generation, kind});
    return true;
}
std::optional<std::uint64_t> BrowserInputOwnershipGate::initial_idle_observation() const {
    return live() && idle_ && records_.empty() ? std::optional<std::uint64_t>{boot_} : std::nullopt;
}
std::optional<std::uint64_t> BrowserInputOwnershipGate::request_input_gate(
    const InputGateRequest& request, std::uint64_t now) {
    const auto index = route(request);
    if (!index || !scope_live(request) || pending_ || records_.size() >= 128 ||
        ticket_ == UINT64_MAX || (clock_ && now < *clock_) || now > UINT64_MAX - 2000)
        return {};
    const auto body = browser_input_ownership_command(request, ticket_ + 1);
    if (body.empty())
        return {};
    if (!request.enabled) {
        try {
            if (!fence_(request.session, request.route_generation))
                return {};
        } catch (...) {
            return {};
        }
    }
    records_.push_back({request, ++ticket_, false});
    pending_ = records_.size() - 1;
    command_ = body;
    clock_ = now;
    deadline_ = now + 2000;
    sent_ = false;
    idle_ = false;
    return ticket_;
}
std::optional<std::string> BrowserInputOwnershipGate::take_command() {
    if (!live() || !pending_ || sent_)
        return {};
    sent_ = true;
    auto body = std::move(command_);
    command_.clear();
    return body;
}
bool BrowserInputOwnershipGate::receive(std::string_view body, std::uint64_t now) {
    if (!poll(now))
        return false;
    BrowserInputOwnershipEvent event;
    if (!decode_browser_input_ownership(body, event))
        return false;
    if (event.idle) {
        if (event.boot_generation != boot_ || pending_ || !records_.empty())
            return false;
        idle_ = true;
        return true;
    }
    const auto bound = route(event.scope);
    if (!bound || event.sender != (routes_[*bound].kind != BrowserInputRouteKind::Udp))
        return false;  // Human中间Worklet false不成为Sender取得/释放事实，亦不得伪造Worklet true。
    if (!pending_) {
        for (const auto& record : records_)
            if (record.ticket == event.ticket)
                return record.actual && record.request.session == event.scope.session &&
                       record.request.route_generation == event.scope.route_generation &&
                       record.request.enabled == event.enabled;
        return false;
    }
    auto& record = records_[*pending_];
    if (!sent_ || record.ticket != event.ticket || record.request.session != event.scope.session ||
        record.request.route_generation != event.scope.route_generation ||
        record.request.enabled != event.enabled || !scope_live(record.request))
        return false;
    record.actual = true;
    pending_.reset();
    return true;
}
bool BrowserInputOwnershipGate::input_gate_confirmed(std::uint64_t ticket,
                                                     const InputGateRequest& request) const {
    if (!scope_live(request))
        return false;
    for (auto item = records_.rbegin(); item != records_.rend(); ++item)
        if (item->request.session == request.session &&
            item->request.route_generation == request.route_generation)
            return item->ticket == ticket && item->actual &&
                   item->request.enabled == request.enabled;
    return false;
}
bool BrowserInputOwnershipGate::poll(std::uint64_t now) {
    if (!live() || (clock_ && now < *clock_) || (pending_ && now >= deadline_)) {
        disconnect();
        return false;
    }
    clock_ = now;
    return true;
}
void BrowserInputOwnershipGate::disconnect() {
    closed_ = true;
    idle_ = false;
    pending_.reset();
    command_.clear();
}
}  // namespace ur::device
