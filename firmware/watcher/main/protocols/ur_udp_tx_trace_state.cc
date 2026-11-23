/* 无线回调帧的有界解析与队列；不修改发送、重传、UDP序号或音频判定。 */
#include "ur_udp_tx_trace_state.h"
#include "ur_ai_runtime.h"

#include <cstring>

namespace {
/* ReadBe16/ReadBe32 在调用方已核验边界后读取网络序整数，避免未对齐访问。 */
uint16_t ReadBe16(const uint8_t* data) {
    return static_cast<uint16_t>((uint16_t{data[0]} << 8) | data[1]);
}
uint32_t ReadBe32(const uint8_t* data) {
    return (uint32_t{data[0]} << 24) | (uint32_t{data[1]} << 16) | (uint32_t{data[2]} << 8) |
           data[3];
}

/* ObserveMacLayout 仅校准标准MAC/LLC候选：帧控制决定唯一头尾，随后逐层检查
 * SNAP与IP/UDP声明边界。不跳过安全头、不展开分片/聚合、不匹配路由或创建事件。
 * 调用方已确认至少24字节且协议版本为0/数据类型；借用缓冲仅在此调用有效。 */
void ObserveMacLayout(const uint8_t* frame, size_t frame_size, UrUdpTxTraceStats* stats) {
    const bool qos = (frame[0] & 0x80) != 0;       // QoS子类型附加两个控制字节。
    const bool addr4 = (frame[1] & 0x03) == 0x03;  // 双DS位附加第四个地址。
    const size_t qos_offset = addr4 ? 30 : 24;     // QoS字段仅在基础地址头之后。
    // QoS与Order同时存在才追加HT控制；不把非QoS的Order位解释成额外头。
    const size_t header_size = qos_offset + (qos ? 2 : 0) + (qos && (frame[1] & 0x80) != 0 ? 4 : 0);
    stats->mac_qos += qos;
    stats->mac_addr4 += addr4;
    stats->mac_protected += (frame[1] & 0x40) != 0;
    if (frame_size < header_size) {
        ++stats->mac_incomplete;
        return;
    }
    switch (header_size) {
        case 24:
            ++stats->mac_h24;
            break;
        case 26:
            ++stats->mac_h26;
            break;
        case 30:
            ++stats->mac_h30;
            break;
        case 32:
            ++stats->mac_h32;
            break;
        case 36:
            ++stats->mac_h36;
            break;
    }
    const bool fragmented =
        (frame[1] & 0x04) != 0 || (frame[22] & 0x0f) != 0;      // 更多分片或非首片。
    const bool no_payload = (frame[0] & 0x40) != 0;             // Null子类型无MSDU。
    const bool amsdu = qos && (frame[qos_offset] & 0x80) != 0;  // 聚合位要求另解子帧。
    stats->mac_fragmented += fragmented;
    stats->mac_no_payload += no_payload;
    stats->mac_amsdu += amsdu;
    if (fragmented || no_payload || amsdu)
        return;
    if (frame_size - header_size < 8) {
        ++stats->llc_short;
        return;
    }
    // 标准RFC1042组织标识与IPv4协议类型必须逐字一致，不解析后移的可疑标记。
    constexpr uint8_t kIpv4Snap[] = {0xaa, 0xaa, 0x03, 0, 0, 0, 0x08, 0};
    if (std::memcmp(frame + header_size, kIpv4Snap, sizeof(kIpv4Snap)) != 0)
        return;
    ++stats->llc_ipv4;
    const size_t available = frame_size - header_size - sizeof(kIpv4Snap);  // 不含MAC/SNAP余量。
    if (available < 20)
        return;
    const uint8_t* ip = frame + header_size + sizeof(kIpv4Snap);  // 仅借用完整的网络头。
    const size_t ip_header = (ip[0] & 0x0f) * size_t{4};          // IPv4选项计入声明头长。
    const size_t ip_size = ReadBe16(ip + 2);                      // 声明长度不得越过借用缓冲。
    if ((ip[0] >> 4) != 4 || ip_header < 20 || ip_size < ip_header || ip_size > available)
        return;
    ++stats->llc_ip_valid;
    if (ip[9] != 17 || (ReadBe16(ip + 6) & 0x3fff) != 0 || ip_size - ip_header < 8)
        return;
    const size_t udp_size = ReadBe16(ip + ip_header + 4);  // 只读取已确认完整的八字节UDP头。
    if (udp_size >= 8 && udp_size == ip_size - ip_header)
        ++stats->llc_udp_valid;
}
}  // namespace

bool UrUdpTxTraceState::Begin(const uint8_t* nonce, size_t nonce_size, int port) {
    End();
    if (nonce == nullptr || nonce_size != kUrAiUdpHeaderSize || nonce[0] != 1 || port <= 0 ||
        port > UINT16_MAX)
        return false;
    const uint32_t generation = stats_.generation + 1;
    stats_ = {};
    stats_.generation = generation == 0 ? 1 : generation;
    std::memcpy(route_.data(), nonce + 4, route_.size());
    port_ = static_cast<uint16_t>(port);
    head_ = 0;
    count_ = 0;
    active_ = true;
    return true;
}

void UrUdpTxTraceState::End() {
    active_ = false;
    route_.fill(0);
    port_ = 0;
}

void UrUdpTxTraceState::Record(const uint8_t* frame, size_t frame_size, bool success,
                               uint64_t uptime_us) {
    UrUdpTxTraceEvent event;  // 立即返回的Wi-Fi回调保持原判据，与交接分阶段接口共用解析。
    if (Capture(frame, frame_size, &event))
        Complete(event, success, uptime_us);
}

bool UrUdpTxTraceState::Capture(const uint8_t* frame, size_t frame_size, UrUdpTxTraceEvent* event) {
    if (event == nullptr)
        return false;
    *event = {};
    if (!active_)
        return false;
    ++stats_.callbacks;
    /* 长度和拒绝原因帮助校准SDK封装，只记数值；超限先拒绝，不访问借用帧。
     * 头类型候选不是解析成功或源端证据，绝不据此换偏移寻找路由。 */
    const uint16_t observed_size =
        static_cast<uint16_t>(frame_size > UINT16_MAX ? UINT16_MAX : frame_size);
    if (stats_.callbacks == 1 || observed_size < stats_.min_frame_size)
        stats_.min_frame_size = observed_size;
    if (observed_size > stats_.max_frame_size)
        stats_.max_frame_size = observed_size;
    if (frame == nullptr) {
        ++stats_.null_frames;
        ++stats_.ignored_layout;
        return false;
    }
    if (frame_size > 1536) {
        ++stats_.large_frames;
        ++stats_.ignored_layout;
        return false;
    }
    if (frame_size >= 20 && (frame[0] >> 4) == 4) {
        const size_t candidate_header = (frame[0] & 0x0f) * size_t{4};
        const size_t candidate_size = ReadBe16(frame + 2);
        if (candidate_header >= 20 && candidate_header <= candidate_size &&
            candidate_size <= frame_size)
            ++stats_.candidate_ipv4;
    }
    if (frame_size >= 24 && (frame[0] & 0x0f) == 0x08) {
        ++stats_.candidate_80211_data;
        ObserveMacLayout(frame, frame_size, &stats_);
    }
    /* SDK私有回调的布局必须实际校准；不支持802.11/VLAN/IPv6/分片或截断帧。
     * 此分支也可能包含普通非音频网络流量，不将ignored_layout视为目标丢包。 */
    if (frame_size < 14 + 20 + 8 + kUrAiUdpHeaderSize) {
        ++stats_.short_frames;
        ++stats_.ignored_layout;
        return false;
    }
    if (ReadBe16(frame + 12) != 0x0800) {
        ++stats_.ethertype_rejected;
        ++stats_.ignored_layout;
        return false;
    }
    const uint8_t* ip = frame + 14;
    const size_t ip_header_size = (ip[0] & 0x0f) * size_t{4};
    const size_t ip_size = ReadBe16(ip + 2);
    if ((ip[0] >> 4) != 4 || ip_header_size < 20 || ip[9] != 17 ||
        (ReadBe16(ip + 6) & 0x3fff) != 0 || ip_size > frame_size - 14 ||
        ip_size < ip_header_size + 8 + kUrAiUdpHeaderSize) {
        ++stats_.ip_rejected;
        ++stats_.ignored_layout;
        return false;
    }
    const uint8_t* udp = ip + ip_header_size;
    if (ReadBe16(udp + 2) != port_)
        return false;
    const uint8_t* payload = udp + 8;
    if (std::memcmp(payload + 4, route_.data(), route_.size()) != 0)
        return false;
    ++stats_.matched;
    UrAiUdpHeader header{};
    const size_t bytes = ip_size - ip_header_size - 8;
    const bool valid =
        ReadBe16(udp + 4) == bytes + 8 && UrAiParseUdpHeader(payload, bytes, &header);
    *event = {0,    stats_.generation, ReadBe32(payload + 12), static_cast<uint16_t>(bytes), false,
              valid};
    ++stats_.pending_results;
    return true;
}

void UrUdpTxTraceState::Complete(UrUdpTxTraceEvent event, bool success, uint64_t uptime_us,
                                 int32_t result_code) {
    if (!active_ || event.generation != stats_.generation || stats_.pending_results == 0)
        return;
    --stats_.pending_results;
    if (count_ == kCapacity) {
        ++stats_.queue_dropped;
        return;
    }
    event.uptime_us = uptime_us;
    event.reported_success = success;
    event.result_code = result_code;
    events_[(head_ + count_) % kCapacity] = event;
    ++count_;
}

bool UrUdpTxTraceState::Pop(UrUdpTxTraceEvent* event) {
    if (event == nullptr || count_ == 0)
        return false;
    *event = events_[head_];
    events_[head_] = {};
    head_ = (head_ + 1) % kCapacity;
    --count_;
    return true;
}
