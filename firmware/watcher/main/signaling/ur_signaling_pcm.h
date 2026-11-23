/* 统一信令 PCM 桥：BufferedPcm 与 AudioService 信令旁路队列之间的事件线程适配。 */
#ifndef UR_SIGNALING_PCM_H
#define UR_SIGNALING_PCM_H

#include "audio_service.h"
#include "ur/device/buffered_pcm.hpp"

#include <deque>
#include <mutex>
#include <string>

#include "freertos/FreeRTOS.h"

namespace ur::device {

/* UrSignalingPcm 的采集方法由独立互斥锁串行；人际编码任务可读取采集，
 * 播放及ticket仍只属于信令事件线程。BufferedPcm两方向成员独立，格式构造后不变。 */
class UrSignalingPcm final : public DrainablePcmPort {
public:
    UrSignalingPcm(AudioService& audio, AudioFormat format);
    ~UrSignalingPcm() override;
    UrSignalingPcm(const UrSignalingPcm&) = delete;
    UrSignalingPcm& operator=(const UrSignalingPcm&) = delete;

    /* enable_failure_diagnostics 打开后记录最近一次写入失败类别。 */
    void enable_failure_diagnostics() { buffered_.enable_failure_diagnostics(); }
    /* last_write_failure 只读最近一次写入类别，不改变缓冲状态。 */
    PcmWriteFailure last_write_failure() const { return buffered_.last_write_failure(); }

    bool begin_capture(std::string_view input_id) override;
    void end_capture() override;
    PcmResult read_frame(std::vector<std::int16_t>& pcm) override;
    // WaitCapture 只等待AFE队列，先read_frame耗尽暂存尾样本；关闭等待最多20ms。
    void WaitCapture() { audio_.WaitSignalingPcm(std::chrono::milliseconds(20)); }
    bool begin_playback(std::string_view response_id, std::uint64_t epoch) override;
    // playback_backpressured 按尚未实际播放的帧数报告容量，包含已移交硬件的帧。
    bool playback_backpressured(std::uint64_t epoch) const override {
        return buffered_.playback_backpressured(epoch);
    }
    bool write_frame(const std::vector<std::int16_t>& pcm, std::uint64_t epoch) override;
    void end_playback(std::uint64_t epoch) override;
    /* 跨会话复用复位播放代次；媒体close时调用，避免Session重建后epoch回绕被拒。 */
    void reset_playback_epochs();
    bool playback_empty(std::uint64_t epoch) const override;

    /* configure 供构造后统一初始化编解码格式；失败返回 false。 */
    bool valid() const { return buffered_.valid(); }
    /* underlying 仅供事件线程查询播放水位；采集状态不可经此跨线程读取。 */
    const BufferedPcm& underlying() const { return buffered_; }
    /* PumpPlayback 在事件线程领取播放完成票据并继续移交待播帧；每轮 poll 调用。 */
    void PumpPlayback();
    /* DrainCaptureWhilePaused 仅提前武装的暂停期调用：把 AFE 分流队列的 10ms 块
     * 搬入 capture_buffer_ 但不推进核心帧队列，供 ACK 后按序发送。最多保留约
     * 2 秒音频，防止 ACK 长期不到时无界占用堆内存。 */
    void DrainCaptureWhilePaused();

private:
    /* DrainPendingCapture 仅按需准备一帧；先消费暂存尾样本，再领取AFE块。 */
    void DrainPendingCapture();
    /* HandOffPendingPlayback 把已写入的播放帧交给 AudioService 实际输出。 */
    void HandOffPendingPlayback();
    /* CollectDoneTickets 领取实际写出的播放 ticket 并推进完成水位。 */
    void CollectDoneTickets();

    AudioService& audio_;
    BufferedPcm buffered_;
    std::mutex capture_mutex_;  // 保护采集缓冲与起止，读帧不持播放或编码队列锁。
    std::string input_id_;
    std::uint64_t last_epoch_ = 0;
    /* capture_buffer_ 保留AFE块的未消费样本；正常最多一帧加一个块，暂停期最多2秒。 */
    std::deque<std::int16_t> capture_buffer_;
    /* ticket_generator_ 在事件线程生成递增播放票据。 */
    std::uint32_t ticket_generator_ = 0;
    /* tickets_out_ 保存已交给 AudioService 的 (epoch, ticket)，按序确认。 */
    struct OutTicket {
        std::uint64_t epoch;
        std::uint32_t ticket;
    };
    std::deque<OutTicket> tickets_out_;
};

}  // namespace ur::device

#endif /* UR_SIGNALING_PCM_H */
