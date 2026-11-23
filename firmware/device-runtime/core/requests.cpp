// 请求生命周期实现：丢包、发布失败和重试都不能制造第二个业务请求。
#include "ur/device/requests.hpp"
#include <utility>

namespace ur::device {
namespace {
// kRequestTimeoutMs 是单条请求的常规应答预算；媒体恢复期由 extend_pending 统一延长。
constexpr std::uint64_t kRequestTimeoutMs = 30000;
}  // namespace
SubmitResult Requests::submit(Request request, std::uint64_t now) {
    if (request.token.empty() || request.token.size() > 128 || request.session.size() > 128 ||
        request.method.empty() || request.method.size() > 64 || request.topic.empty() ||
        request.topic.size() > 512 || request.body.empty() || request.body.size() > 65536)
        return SubmitResult::Invalid;
    tick(now);
    // 在途槽位独立于十分钟幂等窗口；正文精确比较，不使用有碰撞风险的摘要。
    std::size_t available = entries_.size();
    std::size_t pending = 0;
    std::size_t retained_bytes = 0;
    const auto request_bytes = [](const Request& value) {
        return value.token.size() + value.session.size() + value.method.size() +
               value.topic.size() + value.body.size();
    };
    for (auto& entry : entries_) {
        if (entry.state == RequestState::Missing) {
            available = static_cast<std::size_t>(&entry - entries_.data());
            continue;
        }
        pending += entry.state == RequestState::Pending ? 1U : 0U;
        retained_bytes += request_bytes(entry.request);
        if (entry.request.token != request.token || entry.request.method != request.method ||
            entry.request.topic != request.topic)
            continue;
        const auto& existing = entry.request;
        return existing.session == request.session && existing.body == request.body
                   ? SubmitResult::Duplicate
                   : SubmitResult::Conflict;
    }
    // 同时约束记录数与字符串有效字节，防止大正文扩大十分钟保留缓存。
    if (pending >= 32 || (available == entries_.size() && entries_.size() >= 512) ||
        retained_bytes + request_bytes(request) > 131072)
        return SubmitResult::Full;
    if (available == entries_.size())
        entries_.emplace_back();
    auto& entry = entries_[available];
    entry = Entry{std::move(request), RequestState::Pending, now, 0, 0};
    // 恢复期内新提交的请求同样按延长后的统一截止判定，不能因普通 30 秒预算提前作废。
    entry.deadline = extended_until_ > now + kRequestTimeoutMs ? extended_until_ : now + kRequestTimeoutMs;
    port_.publish(entry.request.topic, entry.request.body);
    return SubmitResult::Accepted;
}
// republish_pending 不接受退休/异scope/回拨/原30秒已届满的对象；发布背压仅返回false，不造新请求。
bool Requests::republish_pending(RequestKey key, std::string_view session, std::uint64_t now) {
    for (const auto& entry : entries_) {
        if (entry.state == RequestState::Pending && !entry.session_closed &&
            entry.request.topic == key.topic && entry.request.method == key.method &&
            entry.request.token == key.token && entry.request.session == session &&
            now >= entry.started && now - entry.started < 30000)
            return port_.publish(entry.request.topic, entry.request.body);
    }
    return false;
}
void Requests::tick(std::uint64_t now) {
    for (auto& entry : entries_) {
        if (entry.state == RequestState::Missing)
            continue;
        if (entry.state != RequestState::Pending) {
            if ((entry.request.session.empty() || entry.session_closed) && now >= entry.finished &&
                now - entry.finished >= 600000)
                entry = Entry{};
            continue;
        }
        if (now < entry.started)
            continue;
        const auto elapsed = now - entry.started;
        if (now >= entry.deadline) {
            entry.state = RequestState::TimedOut;
            entry.finished = now;
        } else if (elapsed >= std::uint64_t{10000} * (entry.retries + 1U) && entry.retries < 2) {
            // 定时器延迟时不突发补发多条；下一次 tick 才考虑下一次重试。
            ++entry.retries;
            port_.publish(entry.request.topic, entry.request.body);
        }
    }
}
// extend_pending 只延长不缩短：恢复窗口内的迟到 ACK 必须仍能关联到原请求，
// 否则设备会因本地提前作废而永远等不到恢复准入。
void Requests::extend_pending(std::uint64_t until) {
    if (until > extended_until_)
        extended_until_ = until;
    for (auto& entry : entries_) {
        if (entry.state != RequestState::Pending)
            continue;
        if (until > entry.deadline)
            entry.deadline = until;
    }
}
void Requests::clear_pending_extension() { extended_until_ = 0; }
bool Requests::acknowledge(Acknowledgement ack, std::uint64_t now) {
    if (ack.code < 200 || ack.code > 599)
        return false;
    for (auto& entry : entries_) {
        if (entry.state != RequestState::Pending || entry.request.token != ack.token ||
            entry.request.session != ack.session || entry.request.method != ack.method ||
            entry.request.topic != ack.topic)
            continue;
        if (now < entry.started)
            return false;
        if (now >= entry.deadline) {
            entry.state = RequestState::TimedOut;
            entry.finished = now;
            return false;
        }
        entry.state = ack.code == 200 ? RequestState::Succeeded : RequestState::Failed;
        entry.finished = now;
        return true;
    }
    return false;
}
void Requests::cancel_session(std::string_view session, std::uint64_t now) {
    for (auto& entry : entries_) {
        if (entry.state == RequestState::Missing || entry.request.session != session ||
            entry.session_closed)
            continue;
        if (entry.state == RequestState::Pending)
            entry.state = RequestState::Cancelled;
        entry.session_closed = true;
        entry.finished = now;
    }
}
// cancel_request 单请求退休不取消同腿工具/结束等待；时钟回退不能改写原记录。
bool Requests::cancel_request(RequestKey key, std::uint64_t now) {
    for (auto& entry : entries_) {
        if (entry.request.topic != key.topic || entry.request.method != key.method ||
            entry.request.token != key.token || entry.state == RequestState::Missing)
            continue;
        if (now < entry.started) return false;
        if (entry.state == RequestState::Cancelled) return true;
        if (entry.state != RequestState::Pending) return false;
        entry.state = RequestState::Cancelled;
        entry.finished = now;
        return true;
    }
    return false;
}
RequestState Requests::state(RequestKey key) const {
    for (const auto& entry : entries_)
        if (entry.state != RequestState::Missing && entry.request.token == key.token &&
            entry.request.method == key.method && entry.request.topic == key.topic)
            return entry.state;
    return RequestState::Missing;
}
}  // namespace ur::device
