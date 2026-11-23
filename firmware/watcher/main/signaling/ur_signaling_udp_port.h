/* 统一信令 UDP 数据报端口：lwIP 非阻塞收发，绑定已授权媒体对端。 */
#ifndef UR_SIGNALING_UDP_PORT_H
#define UR_SIGNALING_UDP_PORT_H

#include "ur/device/datagram_port.hpp"

#include "lwip/sockets.h"
#include <vector>

namespace ur::device {

/* UrSignalingUdpPort 不做内部同步；多任务调用方必须串行化收发并在关闭前等待退出。 */
class UrSignalingUdpPort final : public DatagramPort {
public:
    UrSignalingUdpPort() = default;
    ~UrSignalingUdpPort() override { close(); }
    UrSignalingUdpPort(const UrSignalingUdpPort&) = delete;
    UrSignalingUdpPort& operator=(const UrSignalingUdpPort&) = delete;

    bool open(std::string_view local_address, std::uint16_t port) override;
    bool set_peer(std::string_view address, std::uint16_t port) override;
    std::uint16_t local_port() const override { return local_port_; }
    bool send(const std::vector<std::uint8_t>& packet) override;
    ReceiveResult receive(std::vector<std::uint8_t>& packet) override;
    void close() override;

private:
    std::vector<std::uint8_t> receive_buffer_;  // 单线程复用2KiB接收区，关闭时释放，不改变数据报上限。
    int socket_ = -1;
    sockaddr_in peer_{};
    bool peer_set_ = false;
    std::uint16_t local_port_ = 0;
};

}  // namespace ur::device

#endif /* UR_SIGNALING_UDP_PORT_H */
