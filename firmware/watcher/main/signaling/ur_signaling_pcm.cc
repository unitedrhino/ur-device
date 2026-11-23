// 文件说明：信令PCM采集按需组帧，播放经驱动完成票据确认；采集由互斥锁串行，播放只由信令事件线程推进。
#include "ur_signaling_pcm.h"

#include <esp_log.h>
#include <utility>

namespace ur::device {
namespace {
constexpr const char* kTag = "ur_sig_pcm";
}  // namespace

UrSignalingPcm::UrSignalingPcm(AudioService& audio, AudioFormat format)
    : audio_(audio), buffered_(format) {
    /* 通话允许少量网络抖动，但不让过期语音排队太久；普通 BufferedPcm 保持原上限。 */
    buffered_.set_playback_pending_limit(4);
}

UrSignalingPcm::~UrSignalingPcm() {
    end_capture();
    end_playback(last_epoch_);
}

bool UrSignalingPcm::begin_capture(std::string_view input_id) {
    std::lock_guard<std::mutex> lock(capture_mutex_);
    input_id_.assign(input_id);
    capture_buffer_.clear();
    audio_.EnableSignalingCapture(true);
    return buffered_.begin_capture(input_id);
}

void UrSignalingPcm::end_capture() {
    std::lock_guard<std::mutex> lock(capture_mutex_);
    buffered_.end_capture();
    audio_.EnableSignalingCapture(false);
    capture_buffer_.clear();
}

// DrainPendingCapture 只准备一帧；保留不足一帧的样本，避免提前灌满核心队列而丢音。
void UrSignalingPcm::DrainPendingCapture() {
    if (input_id_.empty()) {
        return;
    }
    const std::size_t frame_samples = buffered_.frame_samples();
    if (frame_samples == 0) {
        return;
    }
    std::vector<int16_t> chunk;  // 临时区最多追加凑齐当前帧所需的一个AFE块。
    while (capture_buffer_.size() < frame_samples && audio_.PopSignalingPcm(chunk)) {
        capture_buffer_.insert(capture_buffer_.end(), chunk.begin(), chunk.end());
    }
    if (capture_buffer_.size() >= frame_samples) {
        std::vector<int16_t> frame(capture_buffer_.begin(),
                                   capture_buffer_.begin() + frame_samples);
        // 核心明确拒绝时不能先擦除样本；容量由调用者先读旧帧释放。
        if (buffered_.captured_owned(input_id_, std::move(frame))) {
            capture_buffer_.erase(capture_buffer_.begin(), capture_buffer_.begin() + frame_samples);
        }
    }
}

void UrSignalingPcm::DrainCaptureWhilePaused() {
    std::lock_guard<std::mutex> lock(capture_mutex_);
    if (input_id_.empty()) {
        return;
    }
    /* 提前武装的暂停期只需把 AFE 分流块搬入 capture_buffer_：不向 BufferedPcm
     * 推进，避免 16 帧有界队列在 ACK 迟到时丢最旧帧。16k 采样最多保留 2 秒
     * （32000 样本），超过后丢最旧，保证异常 ACK 不会无界占用堆内存。 */
    static constexpr std::size_t kPausedCaptureMaxSamples = 16000 * 2;
    std::vector<int16_t> chunk;
    while (audio_.PopSignalingPcm(chunk)) {
        capture_buffer_.insert(capture_buffer_.end(), chunk.begin(), chunk.end());
    }
    if (capture_buffer_.size() > kPausedCaptureMaxSamples) {
        capture_buffer_.erase(
            capture_buffer_.begin(),
            capture_buffer_.begin() + (capture_buffer_.size() - kPausedCaptureMaxSamples));
    }
}

// read_frame 优先消费旧帧，再按需组一帧；暂停期尾样本无需等待新麦克风块触发。
PcmResult UrSignalingPcm::read_frame(std::vector<std::int16_t>& pcm) {
    std::lock_guard<std::mutex> lock(capture_mutex_);
    if (buffered_.read_frame(pcm) == PcmResult::Frame) {
        return PcmResult::Frame;
    }
    DrainPendingCapture();
    return buffered_.read_frame(pcm);
}

bool UrSignalingPcm::begin_playback(std::string_view response_id, std::uint64_t epoch) {
    // 新代次开始前先清理上一会话的驱动完成回执，避免首帧消费旧 ticket。
    audio_.ResetSignalingPlayback();
    CollectDoneTickets();
    last_epoch_ = epoch;
    return buffered_.begin_playback(response_id, epoch);
}

bool UrSignalingPcm::write_frame(const std::vector<std::int16_t>& pcm, std::uint64_t epoch) {
    if (!buffered_.write_frame(pcm, epoch)) {
        return false;
    }
    HandOffPendingPlayback();
    return true;
}

void UrSignalingPcm::HandOffPendingPlayback() {
    CollectDoneTickets();
    while (true) {
        if (!audio_.SignalingPlaybackQueueHasSpace()) {
            /* 播放队列满：帧保留在核心缓冲，下一轮 poll 继续移交。 */
            return;
        }
        auto frame = buffered_.take_playback();
        if (!frame) {
            return;
        }
        if (!audio_.PushSignalingPcm(std::move(frame->samples), ticket_generator_ + 1)) {
            ESP_LOGE(kTag, "signaling playback push rejected");
            return;
        }
        ++ticket_generator_;
        tickets_out_.push_back({frame->epoch, frame->ticket});
    }
}

void UrSignalingPcm::CollectDoneTickets() {
    uint32_t ticket = 0;
    while (audio_.PopSignalingPlaybackDone(ticket)) {
        if (tickets_out_.empty()) {
            ESP_LOGW(kTag, "playback done without outstanding ticket");
            continue;
        }
        /* AudioService 按提交顺序串行写出；完成 ticket 顺序一致。 */
        const OutTicket& expected = tickets_out_.front();
        buffered_.played(expected.epoch, expected.ticket);
        tickets_out_.pop_front();
    }
}

void UrSignalingPcm::PumpPlayback() {
    CollectDoneTickets();
    HandOffPendingPlayback();
}

void UrSignalingPcm::end_playback(std::uint64_t epoch) {
    // 等待在途PCM写出并清空旁路队列，再清理本地代次和 ticket 映射。
    audio_.ResetSignalingPlayback();
    buffered_.end_playback(epoch);
    tickets_out_.clear();
}

void UrSignalingPcm::reset_playback_epochs() {
    buffered_.reset_playback_epochs();
    last_epoch_ = 0;
    tickets_out_.clear();
}

bool UrSignalingPcm::playback_empty(std::uint64_t epoch) const {
    /* const 限制下只读核心缓冲；done tickets 的最终确认在事件线程推进。 */
    return buffered_.playback_empty(epoch);
}

}  // namespace ur::device
