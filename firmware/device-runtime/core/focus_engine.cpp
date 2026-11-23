// 文件说明：固定租约准入→驱动整组CAS→原事实回执；未知/撤权只关闭本作用域，不自动恢复采集。
#include "ur/device/focus_engine.hpp"
#include <algorithm>
#include <limits>

namespace ur::device {
namespace {
constexpr std::uint64_t safe = 9007199254740991ULL;  // 跨语言全局资源修订号上界。
// group 用资源多重集合匹配，既不强制排序，也不允许重复元素配置偷换完整组。
bool group(const std::vector<std::string>& a, const std::vector<std::string>& b) {
    return a.size() == b.size() && std::is_permutation(a.begin(), a.end(), b.begin());
}
// same_scope 许可必须绑定全部原请求条件，不能从其他父会话或另一控制操作借用。
bool same_scope(const FocusCommand& a, const FocusCommand& b) {
    return a.envelope.token == b.envelope.token && a.envelope.session == b.envelope.session &&
           a.envelope.method == b.envelope.method && a.envelope.request == b.envelope.request &&
           a.envelope.payload == b.envelope.payload && a.envelope.code == b.envelope.code &&
           a.envelope.event_sequence == b.envelope.event_sequence &&
           a.envelope.message_sequence == b.envelope.message_sequence &&
           a.operation == b.operation && a.expected_revision == b.expected_revision &&
           a.focus_id == b.focus_id && a.resources == b.resources && a.priority == b.priority &&
           a.resume_policy == b.resume_policy && a.expires_at == b.expires_at;
}
// target 只接严格枚举，实际状态必须对应原操作，不能拿旧acquired完成restore。
std::string_view target(std::string_view operation) {
    if (operation == "acquire")
        return "acquired";
    if (operation == "suspend")
        return "suspended";
    if (operation == "restore")
        return "restored";
    return "released";
}
}  // namespace

// FocusEngine 固定原作用域及独立期限，构造不会分配资源或授权。
FocusEngine::FocusEngine(FocusDriver& driver, FocusAuthority& authority, std::string session,
                         std::string focus, std::vector<std::string> resources,
                         std::uint64_t deadline, std::uint64_t timeout)
    : driver_(driver),
      authority_(authority),
      session_(std::move(session)),
      focus_(std::move(focus)),
      resources_(std::move(resources)),
      deadline_(deadline),
      timeout_(timeout) {
    closed_ = !deadline || !timeout || timeout > 30000;
}
// 析构只能请求停止，宿主必须在销毁前等待实际释放或明确保留未知，不把析构当成功。
FocusEngine::~FocusEngine() { close(); }

// replies_drained 同时检查编码失败/有界队列背压保留的原结果，不能只看当前队列长度。
bool FocusEngine::replies_drained() const {
    return !active_ && replies_.empty() &&
           std::all_of(history_.begin(), history_.end(),
                       [](const auto& record) { return !record.result || record.queued; });
}

// retirement 不重新授权或读取驱动；保留全部原请求/回执，编码失败仍由原执行器保管。
std::optional<std::vector<FocusReplayRecord>> FocusEngine::retirement() const {
    if (!closed_ || !resources_released() || !replies_drained())
        return {};
    // 被动stop只证明原物理组已关闭；已知held租约须保留执行器直到原release事实，不丢后续关闭资格。
    if (!state_.empty() && state_ != "released")
        return {};
    std::vector<FocusReplayRecord> result;
    result.reserve(history_.size());
    for (const auto& record : history_) {
        std::string request;
        if (record.reply.empty() ||
            encode_envelope(record.command.envelope, request) != DecodeResult::Ok)
            return {};
        result.push_back({record.command.envelope.token, std::move(request), record.reply});
    }
    return result;
}

// emit 保留原字节；资源整组及修订号由严格焦点编码器再次核验。
bool FocusEngine::emit(std::size_t index, std::string_view state, std::uint64_t revision, int code,
                       std::string_view reason) {
    auto& record = history_[index];
    if (!record.result)
        record.result = Result{std::string(state), std::string(reason), revision, code};
    const auto& original = *record.result;
    Envelope reply;
    std::string json;
    if (focus_reply(record.command, original.state, original.revision, original.code,
                    original.reason, reply) != WireResult::Ok ||
        encode_envelope(reply, json) != DecodeResult::Ok)
        return false;
    record.reply = json;
    if (record.queued)
        return true;
    record.queued = replies_.push_back(std::move(json));
    return record.queued;
}
// flush 只补交原终态，队列满不会重新执行或丢掉无法编码的原实际结果。
void FocusEngine::flush() {
    for (std::size_t index = 0; index < history_.size() && replies_.size() < 4; ++index) {
        const auto& value = history_[index];
        if (value.result && !value.queued) {
            const auto result = *value.result;
            if (!emit(index, result.state, result.revision, result.code, result.reason))
                return;
        }
    }
}
// fail 固定原失败但不声称未执行；关闭后released仍只来自原驱动实际事实。
void FocusEngine::fail(std::string_view reason, int code) {
    const auto active = active_;
    active_.reset();
    pending_.reset();
    started_ = false;
    if (active)
        emit(*active, "denied", driver_.revision(), code, reason);
    closed_ = true;
    lease_.reset();
    if (allocated_)
        released_ = driver_.stop_scope(session_, focus_, resources_);
}
// close 幂等取消当前原操作，未知释放继续poll，不能重开旧租约或停止别的会话。
void FocusEngine::close() {
    if (!closed_)
        fail("sessionEnded", 410);
    else if (allocated_ && !released_)
        released_ = driver_.stop_scope(session_, focus_, resources_);
}
// observe 单调和Unix时钟都不得回退；原授权/请求截止不由重复请求或restore延长。
bool FocusEngine::observe(std::uint64_t now, std::uint64_t wall) {
    ScopeFailure observation;  // 仅在原实际租约存在时观察，不把准备过程的正常false记为失效。
    auto* diagnostic = scope_diagnostics_ && lease_ ? &observation : nullptr;
    // retain 在原拒绝分支、原lease清除前保存首因；不追加时钟或驱动读取。
    const auto retain = [&](ScopeFailureCode fallback) {
        if (diagnostic && first_scope_failure_ == ScopeFailureCode::None) {
            observation.reject(fallback);
            first_scope_failure_ = observation.code;
        }
    };
    if (closed_) {
        close();
        flush();
        return false;
    }
    if (!wall || (clock_ && now < *clock_) || (wall_ && wall < *wall_) || now >= deadline_) {
        retain(now >= deadline_ ? ScopeFailureCode::EngineDeadline : ScopeFailureCode::EngineClock);
        fail("leaseExpired", 410);
        return false;
    }
    clock_ = now;
    wall_ = wall;
    if (lease_ &&
        (now >= lease_deadline_ || (lease_wall_deadline_ && wall >= lease_wall_deadline_))) {
        retain(ScopeFailureCode::EngineDeadline);
        fail("leaseExpired", 410);
        return false;
    }
    if (lease_ && !(diagnostic ? authority_.valid_observed(*lease_, now, wall, diagnostic)
                               : authority_.valid(*lease_, now, wall))) {
        retain(ScopeFailureCode::AuthorityDenied);
        fail("permissionDenied", 403);
        return false;
    }
    if (pending_ &&
        (now >= pending_deadline_ || (pending_wall_deadline_ && wall >= pending_wall_deadline_))) {
        retain(ScopeFailureCode::EngineDeadline);
        fail("leaseExpired", 410);
        return false;
    }
    if (pending_ && !authority_.valid(*pending_, now, wall)) {
        retain(ScopeFailureCode::AuthorityDenied);
        fail("permissionDenied", 403);
        return false;
    }
    if (active_ && now - started_at_ >= timeout_) {
        retain(ScopeFailureCode::EngineBusy);
        fail("timeout", 504);
        return false;
    }
    return true;
}
// begin_cleanup 原对象保留最近已确认持有状态；可信原终态证明只授有限关闭权，默认Authority仍拒。
bool FocusEngine::begin_cleanup(const FocusCommand& command, std::uint64_t now,
                                std::uint64_t wall) {
    if (!closed_ || command.operation != "release" || !allocated_ || !intent_ ||
        (state_ != "acquired" && state_ != "restored" && state_ != "suspended") || !wall ||
        now >= deadline_ || (clock_ && now < *clock_) || (wall_ && wall < *wall_))
        return false;
    const auto permit = authority_.authorize(command, now, wall);
    if (!permit || !same_scope(permit->scope, command) || permit->deadline <= now ||
        permit->intent_version != *intent_ ||
        !authority_.closed_release_valid(*permit, now, wall) ||
        !authority_.valid(*permit, now, wall))
        return false;
    closed_ = false;
    cleanup_only_ = true;
    // 只打开关闭执行路径；原acquire许可已销毁，不回授输出，也不把原实际false当新release事实。
    return true;
}
// submit 去重先于准入；相同原token不刷新授权，不同字段冲突不能生成新的驱动操作。
FocusSubmission FocusEngine::submit(std::string_view json, std::uint64_t now, std::uint64_t wall) {
    FocusCommand command;
    if (decode_focus_control(WireRoute::Signaling, json, command) != WireResult::Ok ||
        !command.envelope.event_sequence.has_value() || command.envelope.session != session_ ||
        command.focus_id != focus_ || !group(command.resources, resources_))
        return {};
    const auto sequence =
        *command.envelope.event_sequence;  // 保留已验证序号，移动command不隐藏准入前置。
    for (const auto& old : history_)
        if (old.command.envelope.token == command.envelope.token) {
            if (!same_scope(old.command, command))
                return {FocusAdmission::Conflict, {}};
            observe(now, wall);
            // observe可能终结原在途；重新读取记录，不用之前借用引用猜测结果。
            for (const auto& value : history_)
                if (value.command.envelope.token == command.envelope.token)
                    return {value.reply.empty()
                                ? (closed_ ? FocusAdmission::Closed : FocusAdmission::Pending)
                                : FocusAdmission::Replay,
                            value.reply};
        }
    if (cleanup_only_ && command.operation != "release")
        return {FocusAdmission::Closed, {}};
    if (!observe(now, wall)) {
        if (!begin_cleanup(command, now, wall) || !observe(now, wall))
            return {FocusAdmission::Closed, {}};
    }
    if (active_)
        return {FocusAdmission::Busy, {}};
    if (history_.size() >= 32 || replies_.size() >= 4)
        return {FocusAdmission::Full, {}};
    history_.push_back({std::move(command), {}, std::nullopt, false});
    const auto index = history_.size() - 1;
    const auto& request = history_[index].command;
    const auto reject = [&](const char* reason, int code) {
        if (!emit(index, "denied", driver_.revision(), code, reason))
            close();
        return FocusSubmission{FocusAdmission::Accepted, {}};
    };
    if (sequence <= sequence_)
        return reject("invalidState", 409);
    sequence_ = sequence;
    if (request.expected_revision != driver_.revision() || request.expected_revision == safe)
        return reject("staleRevision", 409);
    const bool acquire = request.operation == "acquire", restore = request.operation == "restore";
    if ((acquire && !state_.empty()) || (restore && state_ != "suspended") ||
        (request.operation == "suspend" && state_ != "acquired" && state_ != "restored") ||
        (request.operation == "release" && state_.empty()))
        return reject("invalidState", 409);
    auto permit = authority_.authorize(request, now, wall);
    if (!permit || !same_scope(permit->scope, request) || permit->deadline <= now ||
        !authority_.valid(*permit, now, wall) ||
        (cleanup_only_ && (permit->intent_version != intent_ ||
                           !authority_.closed_release_valid(*permit, now, wall))))
        return reject("permissionDenied", 403);
    if (restore &&
        (!lease_ || lease_->scope.resume_policy == "never" ||
         request.resume_policy != lease_->scope.resume_policy ||
         request.priority != lease_->scope.priority || permit->intent_version != intent_))
        return reject("userIntentChanged", 409);
    auto effective_deadline = std::min(permit->deadline, deadline_);
    if (request.expires_at) {
        if (*request.expires_at <= wall ||
            *request.expires_at - wall > std::numeric_limits<std::uint64_t>::max() - now)
            return reject("leaseExpired", 410);
        effective_deadline = std::min(effective_deadline, now + (*request.expires_at - wall));
    }
    if (restore)
        effective_deadline = std::min(effective_deadline, lease_deadline_);
    if (effective_deadline <= now)
        return reject("leaseExpired", 410);
    // 有界历史不驱逐，每个新操作最多递增一次，因此本租约最多32个代次，不循环复用。
    ++generation_;
    pending_ = std::move(permit);
    pending_deadline_ = effective_deadline;
    pending_wall_deadline_ = request.expires_at.value_or(0);
    if (restore)
        pending_wall_deadline_ = std::min(pending_wall_deadline_, lease_wall_deadline_);
    active_ = index;
    started_at_ = now;
    started_ = false;
    return {FocusAdmission::Accepted, {}};
}
// poll 只有原代次、完整实际组与对应全局修订号能生成成功；未来/半组事实关闭原作用域。
void FocusEngine::poll(std::uint64_t now, std::uint64_t wall) {
    if (!observe(now, wall))
        return;
    if (active_ && !started_) {
        if (cleanup_only_ && !driver_.stop_scope(session_, focus_, resources_))
            return;  // 已知源实际退出后仍等原整组物理false；新release保有原10秒操作截止，不虚构完成。
        if (driver_.revision() != history_[*active_].command.expected_revision) {
            const auto index = *active_;
            active_.reset();
            pending_.reset();
            if (!emit(index, "denied", driver_.revision(), 409, "staleRevision"))
                close();
            return;
        }
        if (!driver_.begin(history_[*active_].command, generation_)) {
            const auto index = *active_;
            active_.reset();
            pending_.reset();
            if (!emit(index, "denied", driver_.revision(), 409, "resourceBusy"))
                close();
            return;
        }
        started_ = allocated_ = true;
    }
    for (unsigned count = 0; count < 4; ++count) {
        auto fact = driver_.take_fact(session_, focus_);
        if (!fact)
            return;
        if (fact->generation < generation_)
            continue;
        if (!active_) {
            if (fact->generation > generation_)
                close();
            continue;
        }
        const auto& request = history_[*active_].command;
        if (!started_ || fact->generation != generation_ || !fact->success ||
            !group(fact->resources, resources_) || fact->state != target(request.operation) ||
            fact->revision != request.expected_revision + 1 ||
            driver_.revision() < fact->revision || driver_.revision() > safe) {
            fail("driverFailed", 502);
            return;
        }
        // 取得/恢复还必须由共享资源管理器确认完整当前所有权，不能信任孤立回调。
        if ((request.operation == "acquire" || request.operation == "restore") &&
            !driver_.owns(request)) {
            fail("driverFailed", 502);
            return;
        }
        if ((request.operation == "suspend" || request.operation == "release") && lease_ &&
            driver_.owns(lease_->scope)) {
            fail("driverFailed", 502);
            return;
        }
        const auto index = *active_;
        state_ = fact->state;
        if (request.operation == "acquire" || request.operation == "restore") {
            lease_ = pending_;
            lease_deadline_ = pending_deadline_;
            lease_wall_deadline_ = pending_wall_deadline_;
            if (!intent_)
                intent_ = pending_->intent_version;
        }
        active_.reset();
        pending_.reset();
        started_ = false;
        if (!emit(index, state_, fact->revision, 200, "")) {
            close();
            return;
        }
        if (state_ == "released") {
            closed_ = released_ = true;
            lease_.reset();
        }
    }
}
// take_reply 交付原结果，不把发布端受理当成云端已收录。
std::optional<std::string> FocusEngine::take_reply() {
    auto* value = replies_.front();
    if (!value)
        return {};
    auto result = std::move(*value);
    replies_.pop_front();
    return result;
}
// reply_operation 在既有有界历史中匹配完整原回执，不按状态推测操作或重新申请许可。
std::optional<std::string_view> FocusEngine::reply_operation(std::string_view reply) const {
    if (reply.empty())
        return {};
    for (const auto& record : history_)
        if (record.result && record.reply == reply)
            return record.command.operation;
    return {};
}
// output_permitted 不修改用户音量/输入；仅允许原整组当前实际拥有且许可仍有效。
bool FocusEngine::output_permitted(std::uint64_t now, std::uint64_t wall) {
    return observe(now, wall) && !active_ && lease_ &&
           (state_ == "acquired" || state_ == "restored") && driver_.owns(lease_->scope);
}
// delivered_reply只认引擎保存的原成功字节及当前租约token，不从observer或传入状态推断。
void FocusEngine::delivered_reply(std::string_view reply) {
    if (!lease_)
        return;
    for (const auto& record : history_)
        if (record.result && record.reply == reply && record.result->code == 200 &&
            (record.result->state == "acquired" || record.result->state == "restored") &&
            record.command.envelope.token == lease_->scope.envelope.token) {
            delivered_lease_token_ = record.command.envelope.token;
            return;
        }
}
// scoped_permitted提供候选端准入的只读事实；旧取得历史、部分组及已过期原许可不能授权采播。
bool FocusEngine::scoped_permitted(const std::vector<std::string>& resources, std::uint64_t now,
                                   std::uint64_t wall, ScopeFailure* observation) const {
    if (closed_)
        return scope_reject(observation, first_scope_failure_ == ScopeFailureCode::None
                                             ? ScopeFailureCode::EngineClosed
                                             : first_scope_failure_);
    if (active_ || pending_)
        return scope_reject(observation, ScopeFailureCode::EngineBusy);
    if (!lease_ || released_)
        return scope_reject(observation, ScopeFailureCode::EngineLease);
    if (delivered_lease_token_ != lease_->scope.envelope.token)
        return scope_reject(observation, ScopeFailureCode::EngineReceipt);
    if (state_ != "acquired" && state_ != "restored")
        return scope_reject(observation, ScopeFailureCode::EngineState);
    if (resources.size() != resources_.size() ||
        !std::is_permutation(resources.begin(), resources.end(), resources_.begin()))
        return scope_reject(observation, ScopeFailureCode::EngineResources);
    if (!clock_ || !wall_ || now < *clock_ || wall < *wall_)
        return scope_reject(observation, ScopeFailureCode::EngineClock);
    if (now >= deadline_ || now >= lease_deadline_ || wall >= lease_wall_deadline_)
        return scope_reject(observation, ScopeFailureCode::EngineDeadline);
    if (!(observation ? authority_.valid_observed(*lease_, now, wall, observation)
                      : authority_.valid(*lease_, now, wall)))
        return false;
    return observation ? driver_.owns_observed(lease_->scope, observation)
                       : driver_.owns(lease_->scope);
}
}  // namespace ur::device
