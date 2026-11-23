// MQTT 平台接口：协议核心不依赖特定 MQTT 库或操作系统线程。
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include "ur/device/requests.hpp"

namespace ur::device {
// MqttConfig 使用已生成的短期认证信息；日志禁止输出 username/password。
struct MqttConfig {
    std::string host;                          // Broker 主机名，不包含 URL scheme。
    std::uint16_t port = 8883;                 // MQTT TCP/TLS 端口。
    std::string client_id;                     // 平台设备身份。
    std::string username;                      // 短期签名用户名。
    std::string password;                      // 短期签名，不是设备长期密钥。
    std::string ca_file;                       // TLS CA 文件；为空仅允许回环明文测试。
    std::uint32_t connect_timeout_ms = 10000;  // 等待 CONNACK 的上限，1～30000毫秒。
};
// MqttEvent 不包含凭据；message 内容由上层验签/契约校验后分派。
struct MqttEvent {
    enum class Kind { Connected, Disconnected, Subscribed, Published, Message, Failed };
    Kind kind = Kind::Failed;  // 传输事件类型。
    int operation = 0;         // 发布/订阅 MID，仅为网络确认，不是业务成功。
    int code = 0;              // 协议/传输错误码，不输出原始凭据。
    std::string topic;         // 已接收完整 Topic。
    std::string body;          // 原始正文，限制 64KiB。
};
// MqttPort 由唯一设备事件线程驱动，驱动将回调转为 take_event，不重入核心。
class MqttPort : public PublishPort {
public:
    // connect 开始连接；返回接受不代表已收到 CONNACK。
    virtual bool connect(const MqttConfig& config) = 0;
    // subscribe 单设备精确 Topic，返回 MID 或 0；等待 Subscribed 才能建会话。
    virtual int subscribe(std::string_view topic) = 0;
    // poll 非阻塞推进网络；false须立即停媒体；只有显式同步能力可进入固定短恢复，旧默认关闭会话。
    virtual bool poll() = 0;
    // take_event 按实际到达顺序取事件；空时无值。
    virtual std::optional<MqttEvent> take_event() = 0;
    // publications_drained 连接有效且已接纳发布全部收到网络确认；未知适配缺省false，非业务完成。
    virtual bool publications_drained() { return false; }
    // disconnect 幂等释放资源，不隐式重连或恢复已失效会话。
    virtual void disconnect() = 0;
};
}  // namespace ur::device
