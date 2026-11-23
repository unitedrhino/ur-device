// PCM 事件交接核心：平台回调先转至设备事件线程，再按输入ID/播放代次交付帧和完成事实。
#pragma once
#include <optional>
#include <string>
#include "ur/device/fixed_queue.hpp"
#include "ur/device/pcm_port.hpp"

namespace ur::device {
// PcmWriteFailure 仅描述本次实际写入拒绝类别；不导出帧、代次或播放票据。
enum class PcmWriteFailure {
    None,
    DriverClosed,
    PlaybackInactive,
    EpochMismatch,
    FrameSize,
    BufferFull,
    TicketExhausted,
    QueueRejected,
    Unknown
};
// pcm_write_failure_name 只转换固定类别；非法枚举返回空值，不伪造unknown。
std::string_view pcm_write_failure_name(PcmWriteFailure failure);
// PlaybackFrame 的 ticket 是当前代次内的实际播放确认序号；取出不代表已经播完。
struct PlaybackFrame {
    std::vector<std::int16_t> samples;  // 明确协商格式的一帧PCM。
    std::uint64_t epoch = 0;            // 回复播放代次。
    std::uint32_t ticket = 0;           // 代次内从1递增的播放完成顺序。
};
// BufferedPcm 为浏览器、文件和嵌入式驱动共用有界策略；本类不是跨线程队列。
class BufferedPcm final : public DrainablePcmPort {
public:
    explicit BufferedPcm(AudioFormat format);
    // enable_failure_diagnostics 仅记录开启后的实际写入；重复启用不补采旧失败。
    void enable_failure_diagnostics() { failure_diagnostics_enabled_ = true; }
    // set_playback_pending_limit 收紧未完成播放帧上限；只允许在新播放代次开始前调整。
    bool set_playback_pending_limit(std::uint32_t limit);
    // last_write_failure 纯读取本次写入类别；成功写入清旧失败，关闭不覆盖。
    PcmWriteFailure last_write_failure() const { return write_failure_; }
    // valid 表示显式格式可用，不自动重采样或改变帧长。
    bool valid() const { return samples_ != 0; }
    bool begin_capture(std::string_view input_id) override;
    void end_capture() override;
    PcmResult read_frame(std::vector<std::int16_t>& pcm) override;
    bool begin_playback(std::string_view response_id, std::uint64_t epoch) override;
    // playback_backpressured 报告当前代次达到未完成帧上限；交付平台不归还容量。
    bool playback_backpressured(std::uint64_t epoch) const override;
    bool write_frame(const std::vector<std::int16_t>& pcm, std::uint64_t epoch) override;
    void end_playback(std::uint64_t epoch) override;
    bool playback_empty(std::uint64_t epoch) const override;
    // captured 按输入ID拒绝迟到回调；队列满明确返回false，平台须停止而非无限积压。
    bool captured(std::string_view input_id, const std::vector<std::int16_t>& pcm);
    // captured_owned 接收独占采集帧；身份、帧长或容量不符时返回 false 且不移动帧。
    bool captured_owned(std::string_view input_id, std::vector<std::int16_t>&& pcm);
    // take_playback 只将帧交给平台，仍计入未播放容量，必须随后调用played。
    std::optional<PlaybackFrame> take_playback();
    // played 要求当前代次按顺序确认实际播放完成，重复确认幂等，不接受未取走的帧。
    bool played(std::uint64_t epoch, std::uint32_t ticket);
    // reset_playback_epochs 跨会话复用复位播放代次；Session随runtime重建后epoch从1
    // 重新计数，残留last_epoch_会让新会话首个begin_playback被拒（真机signalClientDriver根因）。
    void reset_playback_epochs();
    const std::string& input_id() const { return input_; }
    std::size_t frame_samples() const { return samples_; }
    // pending_playback_frames 包括已交驱动但未确认写出的帧；不能把入队当作播放完成。
    std::uint32_t pending_playback_frames() const { return submitted_ - completed_; }
    // completed_playback_frames 仅统计played回执，不代表声学出声或对方已听见。
    std::uint32_t completed_playback_frames() const { return completed_; }

private:
    bool failure_diagnostics_enabled_ = false;               // 默认关闭，不改变默认32帧容量。
    std::uint32_t playback_pending_limit_ = 32;              // 普通PCM默认最多积压640毫秒。
    PcmWriteFailure write_failure_ = PcmWriteFailure::None;  // 只属于最近一次实际写入。
    std::size_t samples_ = 0;                                // 每帧精确样本数，0为格式非法。
    std::string input_;                                      // 当前采集段，空表示已停止。
    FixedQueue<std::vector<std::int16_t>, 16> input_queue_;  // 最多16帧采集积压。
    FixedQueue<PlaybackFrame, 32> output_queue_;             // 尚未交给平台的播放帧。
    std::uint64_t epoch_ = 0;                                // 当前播放代次。
    std::uint64_t last_epoch_ = 0;                           // 旧代次停止后仍不能复活。
    std::uint32_t submitted_ = 0;                            // 已接纳的帧序号。
    std::uint32_t handed_ = 0;                               // 已交给平台的帧序号。
    std::uint32_t completed_ = 0;                            // 平台实际播放完成的水位。
};
}  // namespace ur::device
