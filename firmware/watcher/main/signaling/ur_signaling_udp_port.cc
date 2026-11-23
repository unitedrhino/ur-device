// 文件说明：统一信令UDP端口，绑定授权对端并提供非阻塞收发及关闭。
#include "ur_signaling_udp_port.h"

#include <esp_log.h>
#include <esp_timer.h>

#include <cstring>
#include <string>

namespace ur::device {
namespace {
constexpr const char* kTag = "ur_sig_udp";
constexpr size_t kMaxDatagram = 2048;
}  // namespace

bool UrSignalingUdpPort::open(std::string_view local_address, std::uint16_t port) {
    close();
    socket_ = lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_ < 0) {
        ESP_LOGE(kTag, "socket create failed errno=%d", errno);
        return false;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (local_address.empty() || local_address == "0.0.0.0") {
        address.sin_addr.s_addr = INADDR_ANY;
    } else if (lwip_inet_pton(AF_INET, std::string(local_address).c_str(),
                              &address.sin_addr) != 1) {
        ESP_LOGE(kTag, "invalid local address");
        lwip_close(socket_);
        socket_ = -1;
        return false;
    }
    if (lwip_bind(socket_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
        ESP_LOGE(kTag, "bind failed errno=%d", errno);
        lwip_close(socket_);
        socket_ = -1;
        return false;
    }
    sockaddr_in actual{};
    socklen_t length = sizeof(actual);
    if (lwip_getsockname(socket_, reinterpret_cast<sockaddr*>(&actual), &length) < 0) {
        ESP_LOGE(kTag, "getsockname failed errno=%d", errno);
        lwip_close(socket_);
        socket_ = -1;
        return false;
    }
    local_port_ = ntohs(actual.sin_port);
    /* 媒体数据报必须非阻塞；核心在事件线程轮询，不允许等待。 */
    int flags = lwip_fcntl(socket_, F_GETFL, 0);
    const int set_flags = lwip_fcntl(socket_, F_SETFL, flags | O_NONBLOCK);
    // 只回读非阻塞事实；当前诊断不改变原失败策略，后续依据实机结果定位。
    const int observed_flags = lwip_fcntl(socket_, F_GETFL, 0);
    ESP_LOGI(kTag, "udp nonblocking set_rc=%d enabled=%d", set_flags,
             int(observed_flags >= 0 && (observed_flags & O_NONBLOCK)));
    ESP_LOGI(kTag, "[connect-ms] stage=udp_open port=%u t=%llu", static_cast<unsigned>(local_port_),
             static_cast<unsigned long long>(esp_timer_get_time() / 1000));
    return true;
}

bool UrSignalingUdpPort::set_peer(std::string_view address, std::uint16_t port) {
    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(port);
    if (lwip_inet_pton(AF_INET, std::string(address).c_str(), &peer.sin_addr) != 1) {
        ESP_LOGE(kTag, "invalid peer address");
        return false;
    }
    peer_ = peer;
    peer_set_ = true;
    ESP_LOGI(kTag, "[connect-ms] stage=udp_set_peer port=%u t=%llu", static_cast<unsigned>(port),
             static_cast<unsigned long long>(esp_timer_get_time() / 1000));
    return true;
}

bool UrSignalingUdpPort::send(const std::vector<std::uint8_t>& packet) {
    if (socket_ < 0 || !peer_set_ || packet.empty()) {
        return false;
    }
    ssize_t sent = lwip_sendto(socket_, packet.data(), packet.size(), 0,
                               reinterpret_cast<const sockaddr*>(&peer_), sizeof(peer_));
    return sent == static_cast<ssize_t>(packet.size());
}

ReceiveResult UrSignalingUdpPort::receive(std::vector<std::uint8_t>& packet) {
    packet.clear();
    if (socket_ < 0) {
        return ReceiveResult::Failed;
    }
    // 空轮询保留已有空间，避免每次申请并清零2KiB临时向量。
    if (receive_buffer_.size() != kMaxDatagram)
        receive_buffer_.resize(kMaxDatagram);
    ssize_t received = lwip_recvfrom(socket_, receive_buffer_.data(), receive_buffer_.size(),
                                     0, nullptr, nullptr);
    if (received < 0) {
        return errno == EWOULDBLOCK || errno == EAGAIN ? ReceiveResult::Empty
                                                       : ReceiveResult::Failed;
    }
    if (received == 0) {
        return ReceiveResult::Empty;
    }
    if (static_cast<size_t>(received) >= kMaxDatagram) {
        return ReceiveResult::Invalid;
    }
    packet.assign(receive_buffer_.begin(), receive_buffer_.begin() + received);
    return ReceiveResult::Received;
}

void UrSignalingUdpPort::close() {
    if (socket_ >= 0) {
        lwip_close(socket_);
        socket_ = -1;
    }
    peer_set_ = false;
    local_port_ = 0;
    std::vector<std::uint8_t>().swap(receive_buffer_);  // 结束后归还临时接收区，不能变成空闲常驻。
}

}  // namespace ur::device
