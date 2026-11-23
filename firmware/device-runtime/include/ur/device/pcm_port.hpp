// PCM 驱动边界：桌面媒体与嵌入式 I2S 共用；不得在驱动回调中重入会话。
#pragma once
#include <string_view>
#include "ur/device/audio_codec.hpp"

namespace ur::device {
// PcmResult 区分尚无完整帧、成功取得帧及驱动失败。
enum class PcmResult { Empty, Frame, Failed };
// PcmPort 由单事件线程调用；内部跨线程队列必须有界并保持播放代次。
class PcmPort {
public:
    virtual ~PcmPort() = default;
    // begin_capture 启动真实采集；如无有效 AEC，不得因此宣称全双工能力。
    virtual bool begin_capture(std::string_view input_id) = 0;
    // end_capture 幂等停止采集并清空输入积压。
    virtual void end_capture() = 0;
    // read_frame 非阻塞取得一帧显式采样格式的 PCM，Empty 不是失败。
    virtual PcmResult read_frame(std::vector<std::int16_t>& pcm) = 0;
    // begin_playback 为一个回复分配有界播放队列，不能复用旧代次资源。
    virtual bool begin_playback(std::string_view response_id, std::uint64_t epoch) = 0;
    // playback_backpressured
    // 仅指当前有效代次的暂时容量耗尽；不代替驱动错误或实际播放确认。
    // 默认保持旧驱动行为；有界异步驱动须按尚未实际消费的帧数报告，不能只数本地待交付帧。
    virtual bool playback_backpressured(std::uint64_t) const { return false; }
    // write_frame
    // 接受不代表物理播放；媒体泵先检查背压，真实写入失败仍由上层明确终止。
    virtual bool write_frame(const std::vector<std::int16_t>& pcm, std::uint64_t epoch) = 0;
    // end_playback 清空对应代次队列；迟到的其他代次回调不得影响当前播放。
    virtual void end_playback(std::uint64_t epoch) = 0;
};
// DrainablePcmPort 允许媒体泵询问当前代次是否真正排空；网络结束不能代替该事实。
class DrainablePcmPort : public PcmPort {
public:
    // reply_install_ready只声明当前驱动已准备同步建立本机有界回复对象；不证明出声或远端消费。
    // 旧驱动及未装配的嵌入式/模拟端口默认false，不因继承UDP组合类便自动声明新能力。
    virtual bool reply_install_ready() const { return false; }
    virtual bool playback_empty(std::uint64_t epoch) const = 0;
};
}  // namespace ur::device
