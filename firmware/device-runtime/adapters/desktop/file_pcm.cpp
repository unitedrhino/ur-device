// 文件驱动以样本时钟模拟输入/播放，不依赖睡眠、线程或操作系统音频库。
#include "file_pcm.hpp"
#include <algorithm>

namespace ur::device {
FilePcm::FilePcm(AudioFormat format) : format_(format), buffer_(format) {
    rendered_.sample_rate = static_cast<std::uint32_t>(format.sample_rate);
}
bool FilePcm::queue_input(WaveAudio audio) {
    if (failed_ || !buffer_.valid() || !input_complete() || audio.samples.empty() ||
        audio.sample_rate != static_cast<std::uint32_t>(format_.sample_rate) ||
        audio.samples.size() > static_cast<std::size_t>(audio.sample_rate) * 120)
        return false;
    input_ = std::move(audio);
    position_ = 0;
    return true;
}
bool FilePcm::poll(std::uint64_t now) {
    if (failed_ || !buffer_.valid() || (now_ && now < *now_)) {
        failed_ = true;
        end_capture();
        end_playback(epoch_);
        return false;
    }
    now_ = now;
    if (playing_ && playing_at_ &&
        now - *playing_at_ >= static_cast<std::uint64_t>(format_.frame_ms)) {
        const auto limit = static_cast<std::size_t>(format_.sample_rate) * 120;
        if (rendered_.samples.size() > limit - playing_->samples.size() ||
            !buffer_.played(playing_->epoch, playing_->ticket)) {
            failed_ = true;
            end_capture();
            end_playback(epoch_);
            return false;
        }
        for (const auto sample : playing_->samples)
            rendered_.samples.push_back(static_cast<std::int16_t>(
                static_cast<std::int32_t>(sample) * static_cast<std::int32_t>(volume_) / 100));
        playing_.reset();
        playing_at_.reset();
    }
    if (!playing_) {
        playing_ = buffer_.take_playback();
        if (playing_)
            playing_at_ = now;
    }
    return true;
}
bool FilePcm::begin_capture(std::string_view input_id) {
    if (failed_ || input_id.empty() || !buffer_.begin_capture(input_id))
        return false;
    capture_ = std::string(input_id);
    return true;
}
void FilePcm::end_capture() {
    capture_.clear();
    position_ = input_.samples.size();
    buffer_.end_capture();
}
PcmResult FilePcm::read_frame(std::vector<std::int16_t>& pcm) {
    pcm.clear();
    if (failed_)
        return PcmResult::Failed;
    if (capture_.empty() || !now_ || input_complete() ||
        (last_input_ && *now_ - *last_input_ < static_cast<std::uint64_t>(format_.frame_ms)))
        return PcmResult::Empty;
    pcm.resize(buffer_.frame_samples(), 0);
    const auto count = std::min(pcm.size(), input_.samples.size() - position_);
    std::copy_n(input_.samples.begin() + static_cast<std::ptrdiff_t>(position_), count,
                pcm.begin());
    position_ += count;
    last_input_ = now_;
    return PcmResult::Frame;
}
bool FilePcm::begin_playback(std::string_view response_id, std::uint64_t epoch) {
    if (failed_ || !buffer_.begin_playback(response_id, epoch))
        return false;
    response_ = std::string(response_id);
    rendered_.samples.clear();
    playing_.reset();
    playing_at_.reset();
    epoch_ = epoch;
    return true;
}
// playback_backpressured
// 只传播有效驱动的原缓冲状态，失败仍保留原write_frame拒绝语义。
bool FilePcm::playback_backpressured(std::uint64_t epoch) const {
    return !failed_ && buffer_.playback_backpressured(epoch);
}
bool FilePcm::write_frame(const std::vector<std::int16_t>& pcm, std::uint64_t epoch) {
    return !failed_ && buffer_.write_frame(pcm, epoch);
}
void FilePcm::end_playback(std::uint64_t epoch) {
    buffer_.end_playback(epoch);
    if (epoch != epoch_)
        return;
    playing_.reset();
    playing_at_.reset();
}
bool FilePcm::playback_empty(std::uint64_t epoch) const { return buffer_.playback_empty(epoch); }
bool FilePcm::set_volume(unsigned percent) {
    if (failed_ || percent > 100)
        return false;
    volume_ = percent;
    return true;
}
}  // namespace ur::device
