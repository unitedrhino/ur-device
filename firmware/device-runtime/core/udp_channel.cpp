// UDP 数据报边界校验与 AES 平台接口组合；不实现自制密码算法。
#include "ur/device/udp_channel.hpp"
#include <algorithm>
#include <limits>

namespace ur::device {
UdpResult UdpChannel::encode(const std::vector<std::uint8_t>& opus,
                             std::vector<std::uint8_t>& packet) {
    // 禁止输入输出别名，避免 resize/clear 破坏调用者输入。
    if (&opus == &packet)
        return UdpResult::Invalid;
    packet.clear();
    if (opus.empty() || opus.size() > 65507U - 16U)
        return UdpResult::Invalid;
    if (next_sequence() == 0)
        return UdpResult::Exhausted;
    if (stride_ != 1 && ((opus.size() + 15U) / 16U) > stride_)
        return UdpResult::Invalid;
    const auto seq = next_sequence();
    std::array<std::uint8_t, 16> header{};
    header[0] = 1;
    header[2] = static_cast<std::uint8_t>(opus.size() >> 8U);
    header[3] = static_cast<std::uint8_t>(opus.size());
    std::copy(nonce_.begin(), nonce_.end(), header.begin() + 4);
    for (unsigned index = 0; index < 4; ++index)
        header[12 + index] = static_cast<std::uint8_t>(seq >> (24U - index * 8U));
    packet.resize(16 + opus.size());
    std::copy(header.begin(), header.end(), packet.begin());
    // 即使驱动失败也消耗该序号，不能在未知加密结果后复用 IV。
    local_seq_ = seq;
    if (!cipher_.transform(header, opus.data(), packet.data() + 16, opus.size())) {
        packet.clear();
        return UdpResult::CryptoFailed;
    }
    return UdpResult::Ok;
}
UdpResult UdpChannel::decode(const std::uint8_t* packet, std::size_t size,
                             std::vector<std::uint8_t>& opus) {
    // 输入可引用输出原缓冲，因此全部验证/转换结束后才替换输出。
    std::vector<std::uint8_t> decoded;
    if (packet == nullptr || size <= 16 || size > 65507 || packet[0] != 1 || packet[1] != 0) {
        opus.clear();
        return UdpResult::Invalid;
    }
    const auto length = (static_cast<std::size_t>(packet[2]) << 8U) | packet[3];
    if (length != size - 16) {
        opus.clear();
        return UdpResult::Invalid;
    }
    if (!std::equal(receive_nonce_.begin(), receive_nonce_.end(), packet + 4)) {
        opus.clear();
        return UdpResult::WrongSession;
    }
    std::uint32_t seq = 0;
    for (unsigned index = 12; index < 16; ++index)
        seq = (seq << 8U) | packet[index];
    if (stride_ != 1 &&
        (stride_ == 0 || seq == 0 || seq % stride_ != 0 || ((length + 15U) / 16U) > stride_)) {
        opus.clear();
        return UdpResult::Invalid;
    }
    if (seq < receive_floor_ || (remote_seen_ && seq <= remote_seq_)) {
        opus.clear();
        return UdpResult::Stale;
    }
    if (receive_limit_ && seq > *receive_limit_) {
        opus.clear();
        return UdpResult::BeyondEnd;
    }
    std::array<std::uint8_t, 16> header{};
    std::copy_n(packet, 16, header.begin());
    decoded.resize(length);
    if (!cipher_.transform(header, packet + 16, decoded.data(), length)) {
        opus.clear();
        return UdpResult::CryptoFailed;
    }
    remote_seen_ = true;
    remote_seq_ = seq;
    opus = std::move(decoded);
    return UdpResult::Ok;
}
std::uint32_t UdpChannel::next_sequence() const {
    return stride_ == 0 || local_seq_ > std::numeric_limits<std::uint32_t>::max() - stride_
               ? 0
               : local_seq_ + stride_;
}
void UdpChannel::select_receive_nonce(std::array<std::uint8_t, 8> nonce) {
    if (receive_nonce_ == nonce)
        return;
    receive_nonce_ = nonce;
    remote_seen_ = false;
    remote_seq_ = 0;
    receive_limit_.reset();
    receive_floor_ = 0;
}
bool UdpChannel::set_receive_floor(std::uint32_t first_sequence) {
    if (stride_ == 0 || first_sequence == 0 || first_sequence % stride_ != 0 || remote_seen_ ||
        receive_floor_ != 0)
        return false;
    receive_floor_ = first_sequence;
    return true;
}
bool UdpChannel::limit_receive_sequence(std::uint32_t last_sequence) {
    if (stride_ == 0 || last_sequence % stride_ != 0 ||
        (last_sequence != 0 && last_sequence < receive_floor_) ||
        (remote_seen_ && last_sequence < remote_seq_) ||
        (receive_limit_ && *receive_limit_ != last_sequence))
        return false;
    receive_limit_ = last_sequence;
    return true;
}
}  // namespace ur::device
