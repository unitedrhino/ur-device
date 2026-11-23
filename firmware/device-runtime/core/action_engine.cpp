// 动作单线程状态机：提交→异步启动→固定完成；超时/取消保留未知结果，不复活旧动作。
#include "ur/device/action_engine.hpp"
#include <utility>

namespace ur::device {
ActionEngine::ActionEngine(ActionDriver& driver, std::uint64_t timeout_ms, ActionHistoryLedger* ledger)
    : driver_(driver), ledger_(ledger), timeout_ms_(timeout_ms) {
    closed_ = timeout_ms == 0 || timeout_ms > 30000;
    history_.reserve(32);
}
ActionEngine::~ActionEngine() { close(); }
bool ActionEngine::observe(std::uint64_t now) {
    if (last_time_ && now < *last_time_) {
        closed_ = true;
        abort("clockInvalid");
        return false;
    }
    last_time_ = now;
    return true;
}
ActionSubmission ActionEngine::submit(std::string_view json, std::uint64_t now) {
    ActionRequest request;
    if (decode_action_request(json, request) != DecodeResult::Ok)
        return {};
    observe(now);
    for (const auto& record : history_) {
        if (record.request.token != request.token)
            continue;
        if (record.request.action != request.action || record.request.params != request.params)
            return {ActionAdmission::Conflict, {}};
        if (!record.reply.empty()) {
            if (ledger_) {
                const auto stored = ledger_->persistence(request.token, true);
                if (record.persistence_failed || stored == ActionPersistence::Failed) {
                    closed_ = true;
                    return {ActionAdmission::Closed, {}};
                }
                if (stored == ActionPersistence::Pending)
                    return {ActionAdmission::Pending, {}};
            }
            return {ActionAdmission::Replay, record.reply};
        }
        return {ActionAdmission::Pending, {}};
    }
    // 跨重启恢复（账本探针）：已固化终态只重放原文；已受理无终态是重启窗口内的在途动作，
    // 结果未知——两条路径都不触发驱动，绝不重发副作用。
    if (ledger_ != nullptr) {
        if (ledger_->recorded(request.token)) {
            const auto accepted = ledger_->find_request(request.token);
            if (!accepted || accepted->token != request.token) {
                closed_ = true;  // 账本不完整时既不能重执行，也不能把未知请求当成旧结果。
                return {ActionAdmission::Closed, {}};
            }
            if (accepted->action != request.action || accepted->params != request.params)
                return {ActionAdmission::Conflict, {}};
            if (const auto reply = ledger_->find_reply(request.token)) {
                if (ledger_->persistence(request.token, true) == ActionPersistence::Stored)
                    return {ActionAdmission::Replay, *reply};
                return {ActionAdmission::Pending, {}};
            }
            ActionSubmission pending{ActionAdmission::Pending, {}};
            // 在途跨重启必须给出稳定的未知回执原文，调用方不能拿空回执当可重试依据。
            if (encode_action_reply({request.token, request.action, 504, "resultUnknown", "{}"},
                                    pending.reply) != DecodeResult::Ok)
                pending.reply.clear();
            return pending;
        }
    }
    if (closed_)
        return {ActionAdmission::Closed, {}};
    if (active_ || replies_.full())
        return {ActionAdmission::Busy, {}};
    const auto bytes = request.token.size() + request.action.size() + request.params.size();
    // 为可能的最大回执预留64KiB。无账本时容量耗尽拒绝新动作（不驱逐即无重放缺口）；
    // 有账本兜底时驱逐最旧已完成记录——其终态仍可从账本Replay，驱逐不产生重执行风险。
    if (history_.size() >= 32 || retained_bytes_ + bytes + 65536 > 262144) {
        if (ledger_ == nullptr || !evictOldestRetired(bytes))
            return {ActionAdmission::Full, {}};
    }
    if (ledger_ != nullptr && !ledger_->record(request)) {
        // 持久受理失败意味着重启后无法拒绝重放；保守停机，与分配失败保留未决的口径一致。
        closed_ = true;
        return {ActionAdmission::Closed, {}};
    }
    retained_bytes_ += bytes;
    history_.push_back({std::move(request), {}});
    active_ = history_.size() - 1;
    admitted_at_ = now;
    started_ = false;
    return {ActionAdmission::Accepted, {}};
}
bool ActionEngine::evictOldestRetired(std::size_t needed) {
    // 最旧记录必非当前在途（active_恒为最新插入项）；只驱逐已完成记录，未决结果无处恢复。
    while (history_.size() >= 32 || retained_bytes_ + needed + 65536 > 262144) {
        if (history_.empty() || history_.front().reply.empty())
            return false;
        if (active_ && *active_ == 0)
            return false;  // 不变量防御：在途索引永远不该落在队首。
        const auto& front = history_.front();
        // 仅排队不等于固化；未越过终态屏障时驱逐会丢失当前实例的唯一原回执。
        if (front.persistence_failed ||
            ledger_->persistence(front.request.token, true) != ActionPersistence::Stored)
            return false;
        retained_bytes_ -=
            front.request.token.size() + front.request.action.size() +
            front.request.params.size() + front.reply.size();
        history_.erase(history_.begin());
        if (active_)
            --*active_;
    }
    return history_.size() < 32 && retained_bytes_ + needed + 65536 <= 262144;
}
void ActionEngine::finish(int code, const char* message, std::string data) {
    if (!active_)
        return;
    auto& record = history_[*active_];
    if (encode_action_reply(
            {record.request.token, record.request.action, code, message, std::move(data)},
            record.reply) != DecodeResult::Ok) {
        if (encode_action_reply(
                {record.request.token, record.request.action, 500, "invalidDriverResult", "{}"},
                record.reply) != DecodeResult::Ok) {
            closed_ = true;  // 分配失败时保留Token为未决，绝不能再执行。
            return;
        }
    }
    retained_bytes_ += record.reply.size();
    if (ledger_ != nullptr && !ledger_->record_reply(record.request.token, record.reply)) {
        // 终态未固化不能发布成功；保留内存事实供诊断，云端只能收敛为未知，不重执行。
        record.persistence_failed = true;
        closed_ = true;
    }
    PendingReply queued{record.request.token, record.reply};
    if (!replies_.push_back(std::move(queued)))
        closed_ = true;
    active_.reset();
    started_ = false;
}
void ActionEngine::abort(const char* reason) {
    if (!active_)
        return;
    if (started_ && !driver_.cancel(history_[*active_].request.token))
        closed_ = true;
    finish(504, reason, "{}");
}
void ActionEngine::poll(std::uint64_t now) {
    if (!observe(now) || closed_ || !active_)
        return;
    if (now - admitted_at_ >= timeout_ms_) {
        abort("resultUnknown");
        return;
    }
    if (!started_) {
        if (ledger_) {
            const auto stored = ledger_->persistence(history_[*active_].request.token, false);
            if (stored == ActionPersistence::Pending)
                return;  // 文件worker完成前只保留原受理，总超时仍由上方原截止约束。
            if (stored == ActionPersistence::Failed) {
                closed_ = true;
                abort("resultUnknown");
                return;
            }
        }
        if (!driver_.begin(history_[*active_].request)) {
            finish(503, "driverUnavailable", "{}");
            return;
        }
        started_ = true;
    }
    for (unsigned count = 0; count < 4 && active_; ++count) {
        auto completion = driver_.take_completion();
        if (!completion)
            break;
        if (completion->token != history_[*active_].request.token)
            continue;  // 已取消/其他请求结果不能结束新动作。
        finish(completion->code, completion->code == 200 ? "success" : "deviceRejected",
               std::move(completion->data));
    }
}
std::optional<std::string> ActionEngine::take_reply() {
    if (replies_.empty())
        return std::nullopt;
    const auto& pending = *replies_.front();
    if (ledger_) {
        bool failed = false;
        for (const auto& record : history_)
            if (record.request.token == pending.token)
                failed = record.persistence_failed;
        const auto stored = ledger_->persistence(pending.token, true);
        if (!failed && stored == ActionPersistence::Pending)
            return std::nullopt;
        if (failed || stored == ActionPersistence::Failed) {
            closed_ = true;
            replies_.pop_front();
            return std::nullopt;
        }
    }
    auto reply = std::move(replies_.front()->body);
    replies_.pop_front();
    return reply;
}
void ActionEngine::close() {
    if (!closed_ || active_) {
        closed_ = true;
        abort("resultUnknown");
    }
}
void ActionEngine::disconnected() { abort("resultUnknown"); }
}  // namespace ur::device
