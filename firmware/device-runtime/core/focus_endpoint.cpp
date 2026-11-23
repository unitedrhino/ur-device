// 文件说明：严格Topic/整组校验后分派固定焦点租约；终态和发布背压不制造资源空闲证明。
#include "ur/device/focus_endpoint.hpp"
#include <algorithm>
#include <utility>

namespace ur::device {
namespace {
constexpr std::size_t retired_limit = 128;  // 同端点各连接代次的原租约身份上限，不淘汰后重新准入。
constexpr std::size_t retired_bytes =
    65536;  // 墓碑对象/记录/字符串容量预算，不含分配器及容器空余槽开销。
constexpr std::uint64_t receipt_limit =
    9007199254740991ULL;  // 浏览器可精确表示的本地观察水位上限。
}  // namespace
// FocusEndpoint 构造不分配资源；非法宿主资源配置或路由只能关闭，不能降级为部分组。
FocusEndpoint::FocusEndpoint(PublishPort& mqtt, FocusDriver& driver, FocusAuthority& authority,
                             std::vector<std::string> resources, std::string up, std::string down,
                             std::uint64_t deadline, bool report_initial_idle)
    : mqtt_(mqtt),
      driver_(driver),
      authority_(authority),
      resources_(std::move(resources)),
      up_(std::move(up)),
      down_(std::move(down)),
      deadline_(deadline),
      report_initial_idle_(report_initial_idle) {
    closed_ = !deadline_ || up_.empty() || down_.empty() || up_ == down_ || resources_.empty() ||
              resources_.size() > 16;
    for (auto first = resources_.begin(); first != resources_.end(); ++first)
        if (first->empty() ||
            std::find(std::next(first), resources_.end(), *first) != resources_.end())
            closed_ = true;
    valid_ = !closed_;
}
// 析构只请求本端点原作用域停止，调用方仍负责在销毁前完成实际释放验证。
FocusEndpoint::~FocusEndpoint() { close(); }

// begin_connection 不清原token/实际事实；宿主确认旧发布边界后才能请求重新准入。
bool FocusEndpoint::begin_connection(std::uint64_t now) {
    if (!valid_ || now >= deadline_ || !settled() || (clock_ && now < *clock_))
        return false;
    close();
    closed_ = false;
    clock_ = now;
    wall_.reset();
    return true;
}
bool FocusEndpoint::matches(const PublishPort& publisher, std::string_view up,
                            std::string_view down) const {
    return valid_ && &publisher == &mqtt_ && up == up_ && down == down_;
}
// capability 只公开已显式装配的资源类型，首次完整空闲必须另外从实际驱动取证。
WireResult FocusEndpoint::capability(const Envelope& source, Envelope& out) const {
    if (!valid_ || closed_) {
        out = {};
        return WireResult::Invalid;
    }
    if (!report_initial_idle_)
        return add_focus_capability(source, resources_, out);
    const auto observed = driver_.initial_idle_observation();
    Envelope declared;
    if (!observed || observed->revision != driver_.revision() ||
        add_focus_capability(source, resources_, declared) != WireResult::Ok) {
        out = {};
        return WireResult::Invalid;
    }
    // 编码器复核实际完整组必须精确覆盖所声明资源；不允许部分证明或覆盖既有基线。
    return add_focus_baseline(declared, observed->resources, observed->revision, out);
}
// recovery_capability用于新连接恢复握手；原正式释放前资源声明不等于当前全组空闲。
WireResult FocusEndpoint::recovery_capability(const Envelope& source, std::string_view session,
                                              Envelope& out) const {
    if (!valid_ || closed_ || session.empty() ||
        !std::any_of(slots_.begin(), slots_.end(), [&](const auto& slot) {
            return slot.engine && slot.session == session;
        })) {
        out = {};
        return WireResult::Invalid;
    }
    return add_focus_capability(source, resources_, out);
}

// observe 两种时钟均保持单调观察；失效只关闭，不从新请求重置原期限。
void FocusEndpoint::observe(std::uint64_t now, std::uint64_t wall) {
    if (!wall || (clock_ && now < *clock_) || (wall_ && wall < *wall_) || now >= deadline_)
        close();
    clock_ = now;
    wall_ = wall;
}

// retire_slot 先完整导出原事实并核验预算，再释放执行器；析构不会关闭已实际释放的资源。
FocusEndpoint::Slot* FocusEndpoint::retire_slot() {
    if (retired_.size() >= retired_limit)
        return nullptr;
    for (auto& slot : slots_) {
        if (!slot.engine || slot.pending)
            continue;
        auto records = slot.engine->retirement();
        if (!records)
            continue;
        Retired candidate{slot.session, slot.focus, std::move(*records), {}};
        auto bytes = sizeof(Retired) + candidate.session.capacity() + candidate.focus.capacity() +
                     2 + candidate.records.capacity() * sizeof(FocusReplayRecord);
        for (const auto& record : candidate.records)
            bytes +=
                record.token.capacity() + record.request.capacity() + record.reply.capacity() + 3;
        if (bytes > retired_bytes - retired_bytes_)
            continue;
        retired_.push_back(std::move(candidate));
        retired_bytes_ += bytes;
        slot = Slot{};
        return &slot;
    }
    return nullptr;
}

// receive 区分能力全集与原租约完整组；只允许已声明子集，之后由执行器固定原组。
// 重复原请求交由执行器判定，不能扩大原组、接受缺项回执或因终态重新建执行器。
// 保留 topic/json 资源入口；topic 核原下行范围，json 解码原资源命令及邀请关联，双时钟仍各用原范围。
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
FocusEndpointResult FocusEndpoint::receive(std::string_view topic, std::string_view json,
                                           std::uint64_t now, std::uint64_t wall) {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    if (topic != down_)
        return FocusEndpointResult::Ignored;
    Envelope envelope;
    if (decode_envelope(json, envelope) != DecodeResult::Ok)
        return FocusEndpointResult::Invalid;
    if (envelope.method != "focusControl")
        return FocusEndpointResult::Ignored;
    FocusCommand command;
    if (decode_focus_control(WireRoute::Signaling, json, command) != WireResult::Ok ||
        !std::all_of(command.resources.begin(), command.resources.end(), [&](const auto& resource) {
            return std::find(resources_.begin(), resources_.end(), resource) != resources_.end();
        }))
        return FocusEndpointResult::Invalid;
    observe(now, wall);
    for (auto& retired : retired_) {
        if (retired.session != command.envelope.session || retired.focus != command.focus_id)
            continue;
        std::string canonical;
        if (encode_envelope(command.envelope, canonical) != DecodeResult::Ok)
            return FocusEndpointResult::Invalid;
        for (std::size_t index = 0; index < retired.records.size(); ++index) {
            const auto& record = retired.records[index];
            if (record.token != command.envelope.token)
                continue;
            if (record.request != canonical)
                return FocusEndpointResult::Invalid;
            if (retired.pending && retired.records[*retired.pending].reply != record.reply)
                return FocusEndpointResult::Full;
            retired.pending = index;
            return FocusEndpointResult::Handled;
        }
        return FocusEndpointResult::Closed;
    }
    Slot* target = nullptr;
    for (auto& slot : slots_)
        if (slot.engine && slot.session == command.envelope.session &&
            slot.focus == command.focus_id)
            target = &slot;
    if (!target) {
        if (closed_ || now >= deadline_)
            return FocusEndpointResult::Closed;
        // 只有acquire能创建原租约；无原租约的restore/release不能借驱动恢复或释放其他会话。
        if (command.operation != "acquire")
            return FocusEndpointResult::Invalid;
        for (auto& slot : slots_)
            if (!slot.engine) {
                target = &slot;
                break;
            }
        if (!target)
            target = retire_slot();
        if (!target)
            return FocusEndpointResult::Full;
        target->session = command.envelope.session;
        target->focus = command.focus_id;
        target->engine = std::make_unique<FocusEngine>(driver_, authority_, target->session,
                                                       target->focus, command.resources, deadline_);
        if (scope_diagnostics_)
            target->engine->enable_scope_diagnostics();
    }
    const auto result = target->engine->submit(json, now, wall);
    switch (result.admission) {
        case FocusAdmission::Replay:
            // 已有同一原字节的待发布结果可合并重放，其他待发结果不能被覆盖。
            if (target->pending && *target->pending != result.reply)
                return FocusEndpointResult::Full;
            target->pending = result.reply;
            return FocusEndpointResult::Handled;
        case FocusAdmission::Accepted:
        case FocusAdmission::Pending:
            return FocusEndpointResult::Handled;
        case FocusAdmission::Full:
        case FocusAdmission::Busy:
            return FocusEndpointResult::Full;
        case FocusAdmission::Closed:
            return FocusEndpointResult::Closed;
        default:
            return FocusEndpointResult::Invalid;
    }
}
// poll 每租约每轮最多发布一条，保持公平；只有publish接纳才移除本地不可变结果。
void FocusEndpoint::poll(std::uint64_t now, std::uint64_t wall) {
    observe(now, wall);
    for (auto& slot : slots_) {
        if (!slot.engine)
            continue;
        slot.engine->poll(now, wall);
        if (!slot.pending)
            slot.pending = slot.engine->take_reply();
        if (slot.pending && mqtt_.publish(up_, *slot.pending)) {
            slot.engine->delivered_reply(*slot.pending);
            if (receipt_observer_)
                if (const auto operation = slot.engine->reply_operation(*slot.pending))
                    report_receipt(*slot.pending, *operation);
            slot.pending.reset();
        }
    }
    for (auto& retired : retired_)
        if (retired.pending && mqtt_.publish(up_, retired.records[*retired.pending].reply)) {
            if (receipt_observer_) {
                const auto& record = retired.records[*retired.pending];
                FocusCommand command;
                if (decode_focus_control(WireRoute::Signaling, record.request, command) ==
                    WireResult::Ok)
                    report_receipt(record.reply, command.operation);
            }
            retired.pending.reset();
        }
}
// report_receipt 重放仍是原修订的历史回执；观察没有当前授权含义，也不回写驱动或结果队列。
// 保留 reply/operation 观察接口；reply 解码历史实态与修订，operation 仅透传原操作名称，不新授资源权限。
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
void FocusEndpoint::report_receipt(std::string_view reply, std::string_view operation) {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    if (!receipt_observer_ || receipt_index_ >= receipt_limit)
        return;
    FocusState original;
    if (decode_focus_state(WireRoute::Signaling, reply, original) != WireResult::Ok ||
        !original.envelope.code)
        return;
    receipt_observer_(original.state, original.revision, operation, *original.envelope.code,
                      ++receipt_index_);
}
// close 保留历史及未发送原结果；只请求已保留执行器释放其原完整资源。
void FocusEndpoint::close() {
    closed_ = true;
    for (auto& slot : slots_)
        if (slot.engine)
            slot.engine->close();
}
// close_scope不删除历史或重建驱动，原租约只保留有限formal release清理资格。
bool FocusEndpoint::close_scope(std::string_view session) {
    if (!valid_ || closed_ || session.empty()) return false;
    bool found = false;
    for (auto& slot : slots_) {
        if (!slot.engine || slot.session != session) continue;
        slot.engine->close();
        found = true;
    }
    return found;
}
bool FocusEndpoint::released() const {
    return std::all_of(slots_.begin(), slots_.end(), [](const auto& slot) {
        return !slot.engine || slot.engine->resources_released();
    });
}
bool FocusEndpoint::settled() const {
    return released() &&
           std::all_of(slots_.begin(), slots_.end(),
                       [](const auto& slot) {
                           return !slot.engine || (!slot.pending && slot.engine->replies_drained());
                       }) &&
           std::all_of(retired_.begin(), retired_.end(),
                       [](const auto& retired) { return !retired.pending; });
}
// cleanup_settled
// 原同连接Live子腿必须有原release事实；实际false只关闭输出，未知/held均继续有界收尾。
bool FocusEndpoint::cleanup_settled(const std::function<bool(std::string_view)>& owns) const {
    if (!owns || !settled())
        return false;
    return std::all_of(slots_.begin(), slots_.end(), [&](const auto& slot) {
        return !slot.engine || !owns(slot.session) || slot.engine->lease_settled();
    });
}
// scoped_permitted仅授权当前实际租约及已交付的原取得回执；旧QoS重放背压不撤销原有效许可。
bool FocusEndpoint::scoped_permitted(std::string_view session,
                                     const std::vector<std::string>& resources, std::uint64_t now,
                                     std::uint64_t wall, ScopeFailure* observation) const {
    if (closed_ || !valid_ || session.empty() || resources.empty())
        return scope_reject(observation, ScopeFailureCode::EndpointState);
    if (!clock_ || !wall_ || now < *clock_ || wall < *wall_ || now >= deadline_)
        return scope_reject(observation, ScopeFailureCode::EndpointClock);
    ScopeFailure rejected;  // 先前槽位false只是分支选择；后续任一槽位成功必须丢弃该局部观察。
    const bool permitted = std::any_of(slots_.begin(), slots_.end(), [&](const auto& slot) {
        if (slot.session != session || !slot.engine)
            return false;
        ScopeFailure local;
        const bool allowed =
            slot.engine->scoped_permitted(resources, now, wall, observation ? &local : nullptr);
        if (!allowed && observation &&
            (rejected.code == ScopeFailureCode::None ||
             rejected.code == ScopeFailureCode::EngineClosed))
            rejected.code = local.code;
        return allowed;
    });
    return permitted || scope_reject(observation, rejected.code == ScopeFailureCode::None
                                                      ? ScopeFailureCode::EndpointSession
                                                      : rejected.code);
}
// enable_scope_diagnostics 不读取HAL；开启后只记录未来原poll发生的租约失效。
void FocusEndpoint::enable_scope_diagnostics() {
    scope_diagnostics_ = true;
    for (auto& slot : slots_)
        if (slot.engine)
            slot.engine->enable_scope_diagnostics();
}
}  // namespace ur::device
