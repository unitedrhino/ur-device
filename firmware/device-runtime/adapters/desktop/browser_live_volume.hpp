// 文件说明：唯一动作音量聚合原AI PCM与实时节目末端；跨线程实际读回以前不生成成功回执。
#pragma once
#include <cstdint>
#include "ur/device/volume_port.hpp"
namespace ur::device {
// BrowserLiveVolume 聚合原AI和Live实际输出增益；同步PCM与异步WebRTC共用一条有界动作状态。
class BrowserLiveVolume final : public VolumePort, public DeferredVolumePort {
public:
    // pcm为原AI音量读回；deferred非空时须等原AI异步确认，两个借用端口寿命均长于聚合器。
    explicit BrowserLiveVolume(VolumePort& pcm, DeferredVolumePort* deferred = nullptr)
        : pcm_(pcm), deferred_(deferred) {}
    // ready 由宿主实际Live初始化交付，网页类型存在或输出焦点取得不是此事实。
    void ready(bool actual);
    bool set_volume(unsigned percent) override;
    unsigned volume() const override;
    bool request_volume(std::string_view token, unsigned percent) override;
    std::optional<VolumeApplied> take_volume_applied() override;
    // poll 同一单调时钟，未知超过两秒封本动作路径，不以超时生成成功。
    void poll(std::uint64_t now);
    // receive 本地固定三字段票据，不含平台动作token；迟到/改参不改变原PCM设置。
    bool receive(std::string_view body);
    std::optional<std::string> take_command();
    // close 保留未知原pending，不将断连误报应用成功或允许新请求抢占。
    void close();

private:
    // complete只有两条实际增益事实都齐全才完成原action，不回滚未知副作用或延长期限。
    void complete();
    VolumePort& pcm_;                                 // 原AI实际增益设置读取，非请求回显。
    std::optional<VolumeApplied> pending_, applied_;  // 一次一个原事实及原动作关联。
    std::optional<std::string> command_;              // 仅本地opaque票据/百分比，最多一条。
    std::optional<std::uint64_t> clock_, deadline_;   // 原单调水位和固定未知截止。
    std::uint64_t ticket_ = 0;                // 不循环复用的本地票据，不是MQTT字段或设备身份。
    bool ready_ = false, closed_ = false;     // 实际初始化与永久封闭分别维护。
    DeferredVolumePort* deferred_ = nullptr;  // 原AI跨线程端口，可空表示原同步PCM行为。
    bool live_applied_ = false, ai_applied_ = false;  // 原一次动作的两侧事实，不能由命令受理代替。
};
}  // namespace ur::device
