// 回复数据报归属过滤；未知未来包不推进 AES 解码器接收水位，取消后的旧回复不能污染新回复。
#include "ur/device/reply_datagram.hpp"
#include <algorithm>

namespace ur::device {
bool ReplyDatagram::enable_presignal_diagnostics() {
    if (presignal_enabled_)
        return !closed_;
    if (closed_ || active_ || !pending_.empty())
        return false;
    presignal_enabled_ = true;
    return true;
}
std::optional<ReplyPreSignalDiagnostic> ReplyDatagram::presignal_diagnostic() const {
    return presignal_snapshot_;
}
void ReplyDatagram::observe_received(const std::array<std::uint8_t, 8>& nonce) {
    auto entry = std::find_if(presignal_.begin(), presignal_.end(), [&nonce](const auto& value) {
        return value.used && value.nonce == nonce;
    });
    if (entry == presignal_.end()) {
        entry = std::find_if(presignal_.begin(), presignal_.end(),
                             [](const auto& value) { return !value.used; });
        if (entry == presignal_.end()) {
            presignal_incomplete_ = true;
            return;
        }
        *entry = PreSignalEntry{};
        entry->nonce = nonce;
        entry->used = true;
    }
    if (!detail::presignal_increment(entry->received))
        entry->complete = false;
}
void ReplyDatagram::observe_evicted(const std::array<std::uint8_t, 8>& nonce) {
    const auto entry =
        std::find_if(presignal_.begin(), presignal_.end(),
                     [&nonce](const auto& value) { return value.used && value.nonce == nonce; });
    if (entry == presignal_.end()) {
        presignal_incomplete_ = true;
        return;
    }
    if (!detail::presignal_increment(entry->evicted))
        entry->complete = false;
}
void ReplyDatagram::freeze_presignal(const std::array<std::uint8_t, 8>& nonce) {
    // 序号饱和仅停止新观察，不能让合法选择失败、回绕或重复冒称新回复。
    if (!detail::presignal_increment(presignal_observation_)) {
        presignal_snapshot_.reset();
        return;
    }
    ReplyPreSignalDiagnostic snapshot;
    snapshot.observation = presignal_observation_;
    const auto entry =
        std::find_if(presignal_.begin(), presignal_.end(),
                     [&nonce](const auto& value) { return value.used && value.nonce == nonce; });
    if (entry != presignal_.end()) {
        snapshot.received = entry->received;
        snapshot.evicted = entry->evicted;
        snapshot.complete = entry->complete;
        *entry = PreSignalEntry{};
    } else {
        snapshot.complete = true;
    }
    snapshot.retained = static_cast<std::uint32_t>(
        std::count_if(pending_.begin(), pending_.end(),
                      [&nonce](const auto& packet) { return packet_nonce(packet) == nonce; }));
    snapshot.complete = snapshot.complete && !presignal_incomplete_ &&
                        static_cast<std::uint64_t>(snapshot.received) ==
                            static_cast<std::uint64_t>(snapshot.evicted) + snapshot.retained;
    presignal_snapshot_ = snapshot;
}
std::optional<std::array<std::uint8_t, 8>> ReplyDatagram::packet_nonce(
    const std::vector<std::uint8_t>& packet) {
    if (packet.size() <= 16 || packet.size() > 1291 || packet[0] != 1 || packet[1] != 0 ||
        ((static_cast<std::size_t>(packet[2]) << 8U) | packet[3]) != packet.size() - 16)
        return {};
    std::array<std::uint8_t, 8> nonce{};
    std::copy_n(packet.begin() + 4, 8, nonce.begin());
    return nonce;
}
bool ReplyDatagram::select_nonce(const std::array<std::uint8_t, 8>& nonce) {
    if (closed_)
        return false;
    if (active_ && *active_ == nonce)
        return true;
    if (retired_.size() >= 512 ||
        std::find(retired_.begin(), retired_.end(), nonce) != retired_.end())
        return false;
    if (active_)
        retired_.push_back(*active_);
    active_ = nonce;
    // 提交新回复时清掉已经退休的缓存，保留其他尚未收到信令的未来回复。
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                  [this](const auto& packet) {
                                      const auto value = packet_nonce(packet);
                                      return !value || std::find(retired_.begin(), retired_.end(),
                                                                 *value) != retired_.end();
                                  }),
                   pending_.end());
    if (presignal_enabled_)
        freeze_presignal(nonce);
    return true;
}
ReceiveResult ReplyDatagram::receive(std::vector<std::uint8_t>& packet) {
    packet.clear();
    if (closed_)
        return ReceiveResult::Failed;
    if (active_) {
        const auto found = std::find_if(pending_.begin(), pending_.end(), [this](const auto& item) {
            return packet_nonce(item) == active_;
        });
        if (found != pending_.end()) {
            packet = std::move(*found);
            pending_.erase(found);
            return ReceiveResult::Received;
        }
    }
    for (unsigned count = 0; count < 16; ++count) {
        std::vector<std::uint8_t> incoming;
        const auto result = socket_.receive(incoming);
        if (result == ReceiveResult::Failed || result == ReceiveResult::Empty)
            return result;
        if (result == ReceiveResult::Invalid)
            continue;
        const auto nonce = packet_nonce(incoming);
        if (!nonce)
            continue;
        if (active_ == nonce) {
            packet = std::move(incoming);
            return ReceiveResult::Received;
        }
        if (std::find(retired_.begin(), retired_.end(), *nonce) != retired_.end())
            continue;
        if (pending_.size() == 16) {
            if (presignal_enabled_) {
                const auto evicted_nonce = packet_nonce(pending_.front());
                if (evicted_nonce)
                    observe_evicted(*evicted_nonce);
            }
            pending_.erase(pending_.begin());
        }
        pending_.push_back(std::move(incoming));
        if (presignal_enabled_)
            observe_received(*nonce);
    }
    return ReceiveResult::Empty;
}
void ReplyDatagram::close() {
    if (closed_)
        return;
    closed_ = true;
    pending_.clear();
    retired_.clear();
    active_.reset();
    presignal_ = {};  // 擦除内部匹配标记，固定计数快照保留供关闭后观察。
    socket_.close();
}
}  // namespace ur::device
