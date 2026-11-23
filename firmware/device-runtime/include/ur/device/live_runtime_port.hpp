// 文件说明：同一设备MQTT运行时借用的节目端点；端口拥有独立原子会话，不复制AI采集或焦点策略。
#pragma once
#include "ur/device/requests.hpp"
namespace ur::device {
// LiveRuntimePort 独立节目事件循环；调用者保持端点/媒体驱动比VoiceRuntime活得更久。
class LiveRuntimePort {
public:
    virtual ~LiveRuntimePort() = default;
    // matches 核原唯一网络驱动及鉴权Topic，不能用第二连接或正文身份替代。
    virtual bool matches(PublishPort& mqtt, std::string_view up, std::string_view down) const = 0;
    // begin_connection 仅在原输出实际释放、回执已交付后准入新连接；不清掉仍有效原请求历史。
    virtual bool begin_connection(std::uint64_t now) = 0;
    // owns 只读原已知节目会话，不根据未知轨/云端active扩大所属范围。
    virtual bool owns(std::string_view session) const = 0;
    // receive 只接精确SIG；host仍须核父AI存在，不允许原终态或旧token创建新媒体。
    virtual bool receive(std::string_view topic, std::string_view body, std::uint64_t now,
                         std::uint64_t wall) = 0;
    // tick 有界推进协商、原控制事实、超时和全部actual释放；没有墙钟不得新准入。
    virtual void tick(std::uint64_t now, std::uint64_t wall) = 0;
    // close 停原节目，不断共享MQTT、不停止另一路媒体或伪报已释放。
    virtual void close(std::uint64_t now) = 0;
    // begin_parent_close 默认沿硬关闭；专用适配仅保同连接原终态的有限关闭证明，不能准入新节目。
    virtual void begin_parent_close(std::uint64_t now) { close(now); }
    virtual bool released() const = 0;  // 本节目所有真实媒体资源已退出。
    virtual bool settled() const = 0;   // 真实退出且原确定回执已交本地发布，非后端持久收录。
};
}  // namespace ur::device
