// 采集与输出队列独立；停止清理只影响对应方向，丢弃旧代次回调不影响新回复。
#include "ur/device/buffered_pcm.hpp"
#include <limits>

namespace ur::device {
// pcm_write_failure_name 仅转换固定写入类别，不输出帧、票据或代次。
std::string_view pcm_write_failure_name(PcmWriteFailure failure) {
    switch (failure) {
        case PcmWriteFailure::None:
            return "none";
        case PcmWriteFailure::DriverClosed:
            return "driverClosed";
        case PcmWriteFailure::PlaybackInactive:
            return "playbackInactive";
        case PcmWriteFailure::EpochMismatch:
            return "epochMismatch";
        case PcmWriteFailure::FrameSize:
            return "frameSize";
        case PcmWriteFailure::BufferFull:
            return "bufferFull";
        case PcmWriteFailure::TicketExhausted:
            return "ticketExhausted";
        case PcmWriteFailure::QueueRejected:
            return "queueRejected";
        case PcmWriteFailure::Unknown:
            return "unknown";
    }
    return {};
}
BufferedPcm::BufferedPcm(AudioFormat format) {
    if ((format.sample_rate == 16000 || format.sample_rate == 24000 ||
         format.sample_rate == 48000) &&
        format.channels == 1 && (format.frame_ms == 20 || format.frame_ms == 60))
        samples_ = static_cast<std::size_t>(format.sample_rate * format.frame_ms / 1000);
}
bool BufferedPcm::set_playback_pending_limit(std::uint32_t limit) {
    if (limit == 0 || limit > playback_pending_limit_ || epoch_ != 0)
        return false;
    playback_pending_limit_ = limit;
    return true;
}
bool BufferedPcm::begin_capture(std::string_view input_id) {
    if (!valid() || !input_.empty() || input_id.empty() || input_id.size() > 128)
        return false;
    input_ = input_id;
    input_queue_.clear();
    return true;
}
void BufferedPcm::end_capture() {
    input_.clear();
    input_queue_.clear();
}
bool BufferedPcm::captured(std::string_view input_id, const std::vector<std::int16_t>& pcm) {
    if (input_.empty() || input_id != input_ || pcm.size() != samples_)
        return false;
    return input_queue_.push_back(std::vector<std::int16_t>(pcm));
}
// captured_owned 转交独占采集帧，拒绝时保留调用方的数据以便继续处理。
bool BufferedPcm::captured_owned(std::string_view input_id, std::vector<std::int16_t>&& pcm) {
    if (input_.empty() || input_id != input_ || pcm.size() != samples_)
        return false;
    return input_queue_.push_back(std::move(pcm));
}
PcmResult BufferedPcm::read_frame(std::vector<std::int16_t>& pcm) {
    pcm.clear();
    auto* frame = input_queue_.front();
    if (!frame)
        return PcmResult::Empty;
    pcm = std::move(*frame);
    input_queue_.pop_front();
    return PcmResult::Frame;
}
bool BufferedPcm::begin_playback(std::string_view response_id, std::uint64_t epoch) {
    if (!valid() || epoch_ != 0 || epoch == 0 || epoch <= last_epoch_ || response_id.empty() ||
        response_id.size() > 128)
        return false;
    epoch_ = last_epoch_ = epoch;
    submitted_ = handed_ = completed_ = 0;
    output_queue_.clear();
    return true;
}
// playback_backpressured
// 不吞掉无效代次错误；只有真实played推进水位才能解除背压。
bool BufferedPcm::playback_backpressured(std::uint64_t epoch) const {
    return epoch_ != 0 && epoch == epoch_ && submitted_ - completed_ >= playback_pending_limit_;
}
bool BufferedPcm::write_frame(const std::vector<std::int16_t>& pcm, std::uint64_t epoch) {
    // 每次原写入独立分类；成功会清掉旧拒绝，不能把以前的背压借给下一次失败。
    write_failure_ = PcmWriteFailure::None;
    const auto rejected = [this](PcmWriteFailure failure) {
        if (failure_diagnostics_enabled_)
            write_failure_ = failure;
        return false;
    };
    if (epoch_ == 0)
        return rejected(PcmWriteFailure::PlaybackInactive);
    if (epoch != epoch_)
        return rejected(PcmWriteFailure::EpochMismatch);
    if (pcm.size() != samples_)
        return rejected(PcmWriteFailure::FrameSize);
    if (submitted_ - completed_ >= playback_pending_limit_)
        return rejected(PcmWriteFailure::BufferFull);
    if (submitted_ == std::numeric_limits<std::uint32_t>::max())
        return rejected(PcmWriteFailure::TicketExhausted);
    if (!output_queue_.push_back({pcm, epoch, submitted_ + 1}))
        return rejected(PcmWriteFailure::QueueRejected);
    ++submitted_;
    return true;
}
void BufferedPcm::end_playback(std::uint64_t epoch) {
    if (epoch_ == 0 || epoch != epoch_)
        return;
    output_queue_.clear();
    epoch_ = 0;
    submitted_ = handed_ = completed_ = 0;
}
void BufferedPcm::reset_playback_epochs() {
    /* 仅跨会话复用复位；同会话内禁止调用，防旧回复借epoch回绕复活。 */
    output_queue_.clear();
    epoch_ = 0;
    last_epoch_ = 0;
    submitted_ = handed_ = completed_ = 0;
}
bool BufferedPcm::playback_empty(std::uint64_t epoch) const {
    return epoch_ != 0 && epoch == epoch_ && completed_ == submitted_;
}
std::optional<PlaybackFrame> BufferedPcm::take_playback() {
    auto* frame = output_queue_.front();
    if (!frame)
        return {};
    auto owned = std::move(*frame);
    output_queue_.pop_front();
    handed_ = owned.ticket;
    return owned;
}
bool BufferedPcm::played(std::uint64_t epoch, std::uint32_t ticket) {
    if (epoch_ == 0 || epoch != epoch_ || ticket == 0 || ticket > handed_)
        return false;
    if (ticket <= completed_)
        return true;
    if (ticket != completed_ + 1)
        return false;
    completed_ = ticket;
    return true;
}
}  // namespace ur::device
