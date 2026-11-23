// 无实体硬件的文件音频端口：固定时钟驱动采集与虚拟播放，供桌面设备进程联调。
#pragma once
#include "ur/device/buffered_pcm.hpp"
#include "ur/device/volume_port.hpp"
#include "wave_file.hpp"

namespace ur::device {
// FilePcm 独立实现驱动边界；虚拟播放完成只表示文件消费，不能报告扬声器/AEC验收。
class FilePcm final : public DrainablePcmPort, public VolumePort {
public:
    explicit FilePcm(AudioFormat format);
    // queue_input 一次排入一段完整语料；消费中禁止覆盖，不自动重新采样。
    bool queue_input(WaveAudio audio);
    // poll 使用可控单调毫秒，不补发积压帧；超时调度延迟会延长语料而非爆发发送。
    bool poll(std::uint64_t now);
    bool begin_capture(std::string_view input_id) override;
    void end_capture() override;
    PcmResult read_frame(std::vector<std::int16_t>& pcm) override;
    bool begin_playback(std::string_view response_id, std::uint64_t epoch) override;
    // playback_backpressured
    // 同样依据样本时钟的实际消费，不以虚拟驱动领帧作为归还容量。
    bool playback_backpressured(std::uint64_t epoch) const override;
    bool write_frame(const std::vector<std::int16_t>& pcm, std::uint64_t epoch) override;
    void end_playback(std::uint64_t epoch) override;
    bool playback_empty(std::uint64_t epoch) const override;
    bool input_complete() const { return position_ == input_.samples.size(); }
    // rendered 是实际消费的PCM，取消会丢弃尚未消费帧但保留已消费证据。
    const WaveAudio& rendered() const { return rendered_; }
    const std::string& response_id() const { return response_; }
    std::optional<std::uint64_t> last_input_ms() const { return last_input_; }
    // set_volume 修改实际虚拟播放增益，不改变上行采集；范围0至100，不静默裁剪非法值。
    bool set_volume(unsigned percent) override;
    unsigned volume() const override { return volume_; }  // 返回当前驱动实际设置。

private:
    AudioFormat format_;                       // 明确协商的帧时长及采样率。
    BufferedPcm buffer_;                       // 有界播放队列与代次确认策略。
    WaveAudio input_;                          // 当前语料，不超过两分钟。
    WaveAudio rendered_;                       // 当前回复实际消费的样本，最多两分钟。
    std::size_t position_ = 0;                 // 当前语料已发送样本数。
    std::string capture_;                      // 非空才允许读取语料。
    std::string response_;                     // 当前录制结果所属回复。
    std::uint64_t epoch_ = 0;                  // 当前播放代次。
    std::optional<std::uint64_t> now_;         // 设备主循环单调时刻。
    std::optional<std::uint64_t> last_input_;  // 最近实际交付的上行帧时刻。
    std::optional<std::uint64_t> playing_at_;  // 当前虚拟播放帧开始时刻。
    std::optional<PlaybackFrame> playing_;     // 已交给虚拟驱动但尚未播完的一帧。
    bool failed_ = false;                      // 不可恢复驱动错误，必须重新创建对象。
    unsigned volume_ = 100;                    // 虚拟播放增益百分比，默认保持解码样本原值。
};
}  // namespace ur::device
