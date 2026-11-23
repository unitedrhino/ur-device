// 文件说明：人际来电让位期间的终态防重投，容量固定、过期前不淘汰。
#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

// PendingCallTombstones 只由信令任务访问；时间使用不回退的墙钟水位。
class PendingCallTombstones {
public:
    // Decision 区分已取消和已拒绝的原邀请；冲突身份不得重新准入。
    enum class Decision { None, Cancelled, Busy, Declined, Conflict };
    // Advance 根据墙钟更新水位并回收已过原截止的终态。
    void Advance(std::uint64_t wall) {
        highwater_ = std::max(highwater_, wall);
        for (auto& entry : entries_) {
            if (entry.expires != 0 && entry.expires <= highwater_) entry = {};
        }
        if (fence_ != 0 && fence_ <= highwater_) fence_ = 0;
    }
    // Find 按完整身份返回原终态；仅 token 或仅会话相同属于冲突。
    Decision Find(std::string_view token, std::string_view session,
                  std::string_view payload) const {
        for (const auto& entry : entries_) {
            if (!entry.expires) continue;
            if (entry.token == token || entry.session == session) {
                return entry.token == token && entry.session == session && entry.payload == payload
                           ? entry.decision : Decision::Conflict;
            }
        }
        return Decision::None;
    }
    // CanReserve 在开始让位前预留一个终态槽，满时不能产生无保护的等待。
    bool CanReserve(std::size_t bytes) const {
        if (fence_ || bytes > kMaxBytes || UsedBytes() > kMaxBytes - bytes) return false;
        for (const auto& entry : entries_) if (!entry.expires) return true;
        return false;
    }
    // CanRecordOther 为等待中的首个邀请保留一个槽，其他拒接不能占用它。
    bool CanRecordOther(std::size_t bytes, std::size_t reserved) const {
        if (bytes > kMaxBytes || reserved > kMaxBytes - bytes ||
            UsedBytes() > kMaxBytes - bytes - reserved) return false;
        unsigned free = 0;
        for (const auto& entry : entries_) if (!entry.expires) ++free;
        return free > 1;
    }
    // Remember 转移原邀请身份及决定，不因重投延长截止。
    bool Remember(std::string&& token, std::string&& session, std::string&& payload,
                  std::uint64_t expires, Decision decision) {
        if (expires == 0 || expires <= highwater_) return true;
        const auto bytes = token.size() + session.size() + payload.size();
        if (bytes > kMaxBytes || UsedBytes() > kMaxBytes - bytes) return false;
        for (auto& entry : entries_) {
            if (!entry.expires) {
                entry.token = std::move(token);
                entry.session = std::move(session);
                entry.payload = std::move(payload);
                entry.expires = expires;
                entry.decision = decision;
                return true;
            }
        }
        return false;
    }
    // Fence 为无法记录的拒接设置全局屏障，直到该邀请原截止后才再接受新邀请。
    void Fence(std::uint64_t expires) { fence_ = std::max(fence_, expires); }
    // Expired 使用墙钟高水位判断截止，不因系统时间回拨复活旧邀请。
    bool Expired(std::uint64_t expires) const { return expires <= highwater_; }
private:
    // Entry 保存最小身份；同 token 或会话的任何冲突均不得重开通话。
    struct Entry {
        std::string token;
        std::string session;
        std::string payload;
        std::uint64_t expires = 0;
        Decision decision = Decision::None;
    };
    // UsedBytes 统计逻辑载荷大小，拒绝跨多条终态无界增长。
    std::size_t UsedBytes() const {
        std::size_t bytes = 0;
        for (const auto& entry : entries_)
            bytes += entry.token.size() + entry.session.size() + entry.payload.size();
        return bytes;
    }
    static constexpr std::size_t kMaxBytes = 65536;  // 与核心邀请账本载荷上限一致。
    std::array<Entry, 4> entries_{};  // 固定槽位，不能淘汰未过期事实。
    std::uint64_t highwater_ = 0;     // 墙钟最大值，允许断网期间保持原值。
    std::uint64_t fence_ = 0;         // 未能登记的拒接邀请在原截止前禁止重新准入。
};
