// 音量驱动边界：动作读写实际增益，不依赖文件、浏览器或I2S实现。
#pragma once
#include <optional>
#include <string>
#include <string_view>
namespace ur::device {
class VolumePort {
public:
    virtual ~VolumePort() = default;
    // set_volume接受0至100；成功代表驱动设置生效，不代表硬件声学验收。
    virtual bool set_volume(unsigned percent) = 0;
    virtual unsigned volume() const = 0;  // 返回驱动当前设置，不回显请求参数。
};
// VolumeApplied 是异步驱动确认的实际音量；请求参数本身不能作为执行结果。
struct VolumeApplied {
    std::string token;
    unsigned percent = 0;
};
// DeferredVolumePort 用于浏览器等跨线程驱动，完成事实必须经事件线程取回。
class DeferredVolumePort {
public:
    virtual ~DeferredVolumePort() = default;
    // request_volume 只表示命令已排队，不表示增益已应用。
    virtual bool request_volume(std::string_view token, unsigned percent) = 0;
    // take_volume_applied 返回与请求标识关联的真实应用结果。
    virtual std::optional<VolumeApplied> take_volume_applied() = 0;
};
}  // namespace ur::device
