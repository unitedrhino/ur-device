// IPv4 UDP 原生驱动：非阻塞接收、连接对端过滤、严格数据报长度边界。
#include "udp_socket.hpp"
#include <array>
#include <string>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#endif

namespace ur::device {
namespace {
#ifdef _WIN32
using Handle = SOCKET;                             // Windows 原生套接字，不能缩窄为 int。
constexpr Handle invalid_handle = INVALID_SOCKET;  // 未持有句柄。
using SocketLength = int;                          // Winsock 地址长度类型。
// Winsock 在进程中初始化一次，所有本地 UDP 实例共享其生命周期。
struct Winsock {
    WSADATA data{};                                  // 初始化版本信息。
    int status = WSAStartup(MAKEWORD(2, 2), &data);  // 真实启动结果。
    ~Winsock() {
        if (status == 0)
            WSACleanup();
    }
};
#else
using Handle = int;                    // POSIX 文件描述符。
constexpr Handle invalid_handle = -1;  // 未持有句柄。
using SocketLength = socklen_t;        // POSIX 地址长度类型。
#endif
// endpoint 只接受数值 IPv4 和完整字符串，避免 NUL 截断及隐式 DNS 阻塞。
bool endpoint(std::string_view address, std::uint16_t port, sockaddr_in& out) {
    if (address.empty() || address.size() > 15 || address.find('\0') != std::string_view::npos)
        return false;
    out = {};
    out.sin_family = AF_INET;
    out.sin_port = htons(port);
    return inet_pton(AF_INET, std::string(address).c_str(), &out.sin_addr) == 1;
}
// receive_error_reason 将实际平台错误映射到固定分类；不输出系统正文，也不改变失败策略。
std::string_view receive_error_reason(int error) {
    switch (error) {
#ifdef _WIN32
        case WSAECONNREFUSED: return "peerRefused";
        case WSAECONNRESET: return "peerReset";
        case WSAENETUNREACH:
        case WSAEHOSTUNREACH: return "unreachable";
        case WSAEACCES: return "permissionDenied";
        case WSAENOTSOCK: return "invalidSocket";
#else
        case ECONNREFUSED: return "peerRefused";
        case ECONNRESET: return "peerReset";
        case ENETUNREACH:
        case EHOSTUNREACH: return "unreachable";
        case EACCES:
        case EPERM: return "permissionDenied";
        case EBADF:
        case ENOTSOCK: return "invalidSocket";
#endif
        default: return "otherSocketError";
    }
}
}  // namespace
struct UdpSocket::Impl {
    Handle socket = invalid_handle;  // 本实例持有的句柄。
    bool peer = false;               // 是否完成对端绑定。
    std::uint16_t port = 0;          // 实际本地端口。
};
UdpSocket::UdpSocket() : impl_(std::make_unique<Impl>()) {}
UdpSocket::~UdpSocket() { close(); }
// enable_receive_failure_diagnostics 在原首次receive前受理，拒绝事后追认故障。
bool UdpSocket::enable_receive_failure_diagnostics() {
    if (receive_diagnostics_enabled_)
        return true;
    if (receive_started_)
        return false;
    receive_diagnostics_enabled_ = true;
    return true;
}
// receive_failure_diagnostic 只读首次失败，释放句柄后仍保持原事实。
std::optional<std::string_view> UdpSocket::receive_failure_diagnostic() const {
    return receive_failure_;
}
// record_receive_failure 不覆盖首因，禁用时不保存任何失败信息。
void UdpSocket::record_receive_failure(std::string_view reason) {
    if (receive_diagnostics_enabled_ && !receive_failure_)
        receive_failure_ = reason;
}
bool UdpSocket::open(std::string_view local_address, std::uint16_t port) {
    close();
#ifdef _WIN32
    static const Winsock runtime;
    if (runtime.status != 0)
        return false;
#endif
    sockaddr_in local{};
    if (!endpoint(local_address, port, local))
        return false;
    impl_->socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (impl_->socket == invalid_handle)
        return false;
    if (bind(impl_->socket, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
        close();
        return false;
    }
#ifdef _WIN32
    unsigned long enabled = 1;
    const bool nonblocking = ioctlsocket(impl_->socket, FIONBIO, &enabled) == 0;
#else
    const int flags = fcntl(impl_->socket, F_GETFL, 0);
    const bool nonblocking = flags >= 0 && fcntl(impl_->socket, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
    SocketLength size = sizeof(local);
    if (!nonblocking ||
        getsockname(impl_->socket, reinterpret_cast<sockaddr*>(&local), &size) != 0) {
        close();
        return false;
    }
    impl_->port = ntohs(local.sin_port);
    return true;
}
bool UdpSocket::set_peer(std::string_view address, std::uint16_t port) {
    sockaddr_in peer{};
    if (impl_->socket == invalid_handle || impl_->peer || port == 0 ||
        !endpoint(address, port, peer))
        return false;
    if (::connect(impl_->socket, reinterpret_cast<const sockaddr*>(&peer), sizeof(peer)) != 0)
        return false;
    impl_->peer = true;
    return true;
}
std::uint16_t UdpSocket::local_port() const { return impl_->port; }
bool UdpSocket::send(const std::vector<std::uint8_t>& packet) {
    if (!impl_->peer || packet.empty() || packet.size() > 65507)
        return false;
    const auto sent = ::send(impl_->socket, reinterpret_cast<const char*>(packet.data()),
                             static_cast<int>(packet.size()), 0);
    return sent >= 0 && static_cast<std::size_t>(sent) == packet.size();
}
ReceiveResult UdpSocket::receive(std::vector<std::uint8_t>& packet) {
    receive_started_ = true;
    packet.clear();
    if (!impl_->peer) {
        record_receive_failure("notConnected");
        return ReceiveResult::Failed;
    }
    std::array<char, 65536> buffer{};
    const auto received = recv(impl_->socket, buffer.data(), static_cast<int>(buffer.size()), 0);
    if (received < 0) {
#ifdef _WIN32
        const auto error = WSAGetLastError();
        if (error == WSAEWOULDBLOCK)
            return ReceiveResult::Empty;
        // Windows UDP 的 ICMP 拒绝可能表现为 CONNECTIONRESET；不隐式忽略该失败。
        record_receive_failure(receive_error_reason(error));
#else
        const int error = errno;  // 在任何旁证操作之前冻结本次实际 recv 的错误。
        if (error == EAGAIN || error == EWOULDBLOCK || error == EINTR)
            return ReceiveResult::Empty;
        record_receive_failure(receive_error_reason(error));
#endif
        return ReceiveResult::Failed;
    }
    if (received == 0 || received > 65507)
        return ReceiveResult::Invalid;
    packet.assign(buffer.begin(), buffer.begin() + received);
    return ReceiveResult::Received;
}
void UdpSocket::close() {
    if (impl_->socket != invalid_handle) {
#ifdef _WIN32
        closesocket(impl_->socket);
#else
        ::close(impl_->socket);
#endif
    }
    *impl_ = Impl{};
}
}  // namespace ur::device
