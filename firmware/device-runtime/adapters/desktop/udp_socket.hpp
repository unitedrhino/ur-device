// Winsock/POSIX 的 UDP 实现，操作系统头文件不进入公开核心。
#pragma once
#include <memory>
#include <optional>
#include "ur/device/datagram_port.hpp"

namespace ur::device {
// UdpSocket 使用非阻塞数据报；目前显式支持数值 IPv4，不伪造 IPv6/DNS 能力。
class UdpSocket final : public DatagramPort {
public:
    UdpSocket();
    ~UdpSocket() override;
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;
    bool open(std::string_view local_address, std::uint16_t port) override;
    bool set_peer(std::string_view address, std::uint16_t port) override;
    std::uint16_t local_port() const override;
    bool send(const std::vector<std::uint8_t>& packet) override;
    ReceiveResult receive(std::vector<std::uint8_t>& packet) override;
    void close() override;
    // enable_receive_failure_diagnostics 必须在首次接收前显式启用；不改变收发或失败策略。
    bool enable_receive_failure_diagnostics();
    // receive_failure_diagnostic 返回首个实际接收失败的固定分类；无地址、报文或系统错误正文。
    std::optional<std::string_view> receive_failure_diagnostic() const;

private:
    // record_receive_failure 只保存启用后的首因，关闭与后续失败不能覆盖它。
    void record_receive_failure(std::string_view reason);
    struct Impl;                  // 隔离 SOCKET/fd 与 Winsock 生命周期。
    std::unique_ptr<Impl> impl_;  // 本实例独占套接字状态。
    bool receive_started_ = false;  // 禁止首次调用后补采此前失败。
    bool receive_diagnostics_enabled_ = false;  // 旁证默认关闭，不是重试或容错开关。
    std::optional<std::string_view> receive_failure_;  // 静态闭集合首因，释放句柄后保留。
};
}  // namespace ur::device
