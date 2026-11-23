// 不可靠媒体数据报接口；套接字细节留在平台实现中。
#pragma once
#include <cstdint>
#include <string_view>
#include <vector>

namespace ur::device {
// ReceiveResult 区分无包、实际数据、畸形超大包和网络错误。
enum class ReceiveResult { Empty, Received, Invalid, Failed };
// DatagramPort 独占一个媒体端点，核心在事件线程中调用非阻塞 send/receive。
class DatagramPort {
public:
    virtual ~DatagramPort() = default;
    // open 绑定指定本地 IPv4；port=0 让操作系统选择空闲端口。
    virtual bool open(std::string_view local_address, std::uint16_t port) = 0;
    // set_peer 绑定已授权媒体对端，只接收来自该对端的数据报。
    virtual bool set_peer(std::string_view address, std::uint16_t port) = 0;
    // local_port 返回实际绑定端口，未打开返回0。
    virtual std::uint16_t local_port() const = 0;
    // send 失败返回 false，不缓存/无限重试实时帧。
    virtual bool send(const std::vector<std::uint8_t>& packet) = 0;
    // receive 无包时立即返回；失败不保留上次内容。
    virtual ReceiveResult receive(std::vector<std::uint8_t>& packet) = 0;
    // close 幂等释放句柄，结束会话不得保留媒体端点。
    virtual void close() = 0;
};
}  // namespace ur::device
