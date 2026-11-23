// 桌面设备的软件动作：文件与浏览器共用音量端口，不宣称硬件扬声器已验收。
#pragma once
#include <cstdint>
#include "ur/device/action_engine.hpp"
#include "ur/device/volume_port.hpp"

namespace ur::device {
// FileActions
// 是ActionDriver桌面适配，嵌入式只需替换驱动，不复制协议与幂等状态机。
class FileActions final : public ActionDriver {
public:
    explicit FileActions(VolumePort& pcm, DeferredVolumePort* deferred = nullptr)
        : pcm_(pcm), deferred_(deferred) {}
    // begin
    // 支持本模拟产品setVolume/getVolume，未知动作返回501，不假装支持任意物模型。
    bool begin(const ActionRequest& request) override;
    std::optional<ActionCompletion> take_completion() override;
    // cancel 对已排队的异步音量命令返回未知结果，防止执行器错误地接纳后续动作。
    bool cancel(std::string_view token) override;
    // completed_count
    // 统计已在本软件驱动内同步完成的动作，含明确失败；拒收和重复取回执不增加。
    std::uint64_t completed_count() const { return completed_count_; }
    // last_code 返回最后一次实际执行结果，未执行时为0；不代表云端已经收到回执。
    int last_code() const { return last_code_; }

private:
    VolumePort& pcm_;                             // 同一设备实际播放增益驱动，生命周期长于本适配。
    DeferredVolumePort* deferred_ = nullptr;      // 非空时须等待跨线程应用回执。
    std::optional<ActionCompletion> completion_;  // 一次一个结果，由设备事件线程取走。
    std::optional<std::string> pending_token_;    // 异步音量请求，未确认前不可回报成功。
    std::uint64_t completed_count_ = 0;           // 仅供桌面设备验收观察，不参与协议幂等决策。
    int last_code_ = 0;  // 同线程驱动的最后执行状态，不保存动作正文或凭据。
};
}  // namespace ur::device
