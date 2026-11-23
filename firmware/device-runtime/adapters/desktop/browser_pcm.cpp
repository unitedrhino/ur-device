// 浏览器驱动复用BufferedPcm；独立输入/输出不因回复播放自动关闭采集。
#include "browser_pcm.hpp"
#include <limits>
#include "browser_input_ownership_wire.hpp"

namespace ur::device {
BrowserPcm::BrowserPcm(AudioFormat format) : format_(format), buffer_(format) {}
bool BrowserPcm::ready(AudioFormat actual, bool echo_cancellation) {
    if (failed_ || ready_ || !buffer_.valid() || actual.sample_rate != format_.sample_rate ||
        actual.channels != format_.channels || actual.frame_ms != format_.frame_ms)
        return false;
    ready_ = true;
    aec_ = echo_cancellation;
    return true;
}
void BrowserPcm::disconnect() {
    failed_ = true;
    ready_ = false;
    buffer_.end_capture();
    buffer_.end_playback(epoch_);
    epoch_ = 0;
    response_.clear();
    commands_.clear();
    gate_confirmed_ = 0;
    gate_pending_ = gate_sent_ = false;
    capture_forwarding_ = false;
    capture_held_input_.clear();
}
bool BrowserPcm::command(BrowserAudioCommand value) {
    if (capture_generation_ && (value.kind == BrowserAudioCommandKind::CaptureStart ||
                                value.kind == BrowserAudioCommandKind::CaptureStop)) {
        value.capture_session = capture_session_;
        value.capture_generation = capture_generation_;
    }
    if (failed_ || !commands_.push_back(std::move(value))) {
        disconnect();
        return false;
    }
    return true;
}
bool BrowserPcm::begin_capture(std::string_view input_id) {
    if (!ready_ || failed_ || (capture_generation_ && !capture_forwarding_) ||
        !buffer_.begin_capture(input_id))
        return false;
    if (!capture_generation_)
        legacy_capture_used_ = true;
    return command({BrowserAudioCommandKind::CaptureStart, std::string(input_id), 0});
}
void BrowserPcm::end_capture() {
    const auto id = buffer_.input_id().empty() ? capture_held_input_ : buffer_.input_id();
    capture_held_input_.clear();
    if (id.empty())
        return;
    buffer_.end_capture();
    command({BrowserAudioCommandKind::CaptureStop, id, 0});
}
PcmResult BrowserPcm::read_frame(std::vector<std::int16_t>& pcm) {
    pcm.clear();
    if (failed_)
        return PcmResult::Failed;
    return capture_generation_ && !capture_forwarding_ ? PcmResult::Empty : buffer_.read_frame(pcm);
}
bool BrowserPcm::begin_playback(std::string_view response_id, std::uint64_t epoch) {
    if (!ready_ || failed_ || !buffer_.begin_playback(response_id, epoch))
        return false;
    epoch_ = epoch;
    response_ = response_id;
    return command({BrowserAudioCommandKind::PlaybackStart, response_, epoch_});
}
// playback_backpressured 不以WebSocket交付成功冒充Worklet实际消费。
bool BrowserPcm::playback_backpressured(std::uint64_t epoch) const {
    return !failed_ && buffer_.playback_backpressured(epoch);
}
bool BrowserPcm::write_frame(const std::vector<std::int16_t>& pcm, std::uint64_t epoch) {
    if (!failure_diagnostics_)
        return !failed_ && buffer_.write_frame(pcm, epoch);
    last_write_failure_ = PcmWriteFailure::None;
    if (failed_) {
        last_write_failure_ = PcmWriteFailure::DriverClosed;
        return false;
    }
    const bool accepted =
        buffer_.write_frame(pcm, epoch);  // 原写入口仍只调用一次，容量与返回值不变。
    if (!accepted)
        last_write_failure_ = buffer_.last_write_failure();
    return accepted;
}
// enable_failure_diagnostics
// 不从关闭后的空缓冲反推首因，也不因重复开启退休现有记录。
void BrowserPcm::enable_failure_diagnostics() {
    if (failed_ || failure_diagnostics_)
        return;
    failure_diagnostics_ = true;
    buffer_.enable_failure_diagnostics();
}
void BrowserPcm::end_playback(std::uint64_t epoch) {
    if (epoch == 0 || epoch != epoch_)
        return;
    buffer_.end_playback(epoch);
    const auto response = response_;
    epoch_ = 0;
    response_.clear();
    command({BrowserAudioCommandKind::PlaybackStop, response, epoch});
}
bool BrowserPcm::playback_empty(std::uint64_t epoch) const {
    return !failed_ && buffer_.playback_empty(epoch);
}
bool BrowserPcm::captured(std::string_view input_id, const std::vector<std::int16_t>& pcm) {
    // 明确绑定的新宿主不能退回只有inputId的旧入口，否则可能把他腿PCM写入原UDP。
    return !capture_generation_ && accept_capture_frame(input_id, pcm);
}
bool BrowserPcm::accept_capture_frame(std::string_view input_id,
                                      const std::vector<std::int16_t>& pcm) {
    if (failed_ || input_id.empty() || input_id != buffer_.input_id())
        return false;
    if (buffer_.captured(input_id, pcm))
        return true;
    disconnect();
    return false;
}
bool BrowserPcm::capture_route_matches(std::string_view session, std::uint64_t generation) const {
    return ready_ && !failed_ && generation && capture_generation_ == generation &&
           capture_session_ == session;
}
bool BrowserPcm::bind_capture_route(std::string_view session, std::uint64_t generation) {
    if (!ready_ || failed_ || capture_generation_ || legacy_capture_used_ ||
        !buffer_.input_id().empty() ||
        browser_input_ownership_command({std::string(session), generation, false}, 1).empty())
        return false;
    capture_session_ = session;
    capture_generation_ = generation;
    capture_forwarding_ = false;
    return true;
}
bool BrowserPcm::fence_capture_route(std::string_view session, std::uint64_t generation) {
    if (!capture_route_matches(session, generation))
        return false;
    if (!capture_forwarding_)
        return true;
    // 先停止原UDP媒体泵读帧再清空积压；后续浏览器false实际回执是独立屏障。
    capture_forwarding_ = false;
    capture_held_input_ = buffer_.input_id();
    buffer_.end_capture();
    FixedQueue<BrowserAudioCommand, 16> retained;
    while (auto* pending = commands_.front()) {
        if (pending->kind != BrowserAudioCommandKind::CaptureStart &&
            pending->kind != BrowserAudioCommandKind::CaptureStop)
            retained.push_back(std::move(*pending));
        commands_.pop_front();
    }
    // 两队列容量相同且原队列已排空，保留输出命令不会扩大容量或改变顺序。
    while (auto* pending = retained.front()) {
        commands_.push_back(std::move(*pending));
        retained.pop_front();
    }
    return true;
}
bool BrowserPcm::enable_capture_route(std::string_view session, std::uint64_t generation) {
    if (!capture_route_matches(session, generation))
        return false;
    if (capture_forwarding_)
        return true;
    if (!capture_held_input_.empty()) {
        const auto input = capture_held_input_;
        if (!buffer_.begin_capture(input) ||
            !command({BrowserAudioCommandKind::CaptureStart, input, 0}))
            return false;
        capture_held_input_.clear();
    }
    capture_forwarding_ = true;
    return true;
}
bool BrowserPcm::captured_route(std::string_view session, std::uint64_t generation,
                                std::string_view input_id, const std::vector<std::int16_t>& pcm) {
    return capture_route_matches(session, generation) && capture_forwarding_ &&
           accept_capture_frame(input_id, pcm);
}
bool BrowserPcm::played(std::uint64_t epoch, std::uint32_t ticket) {
    if (failed_ || epoch == 0 || epoch != epoch_)
        return false;
    if (buffer_.played(epoch, ticket))
        return true;
    disconnect();
    return false;
}
std::optional<BrowserAudioCommand> BrowserPcm::take_command() {
    auto* next = commands_.front();
    if (!next)
        return {};
    auto result = std::move(*next);
    commands_.pop_front();
    if (result.kind == BrowserAudioCommandKind::OutputGate)
        gate_sent_ = true;
    return result;
}
std::optional<PlaybackFrame> BrowserPcm::take_playback() {
    if (failed_ || commands_.front())
        return {};
    auto frame = buffer_.take_playback();
    if (frame)
        for (auto& sample : frame->samples)
            sample = static_cast<std::int16_t>(static_cast<std::int32_t>(sample) *
                                               static_cast<std::int32_t>(volume_) / 100);
    return frame;
}
bool BrowserPcm::set_volume(unsigned percent) {
    if (failed_ || percent > 100)
        return false;
    volume_ = percent;
    return true;
}
// poll_output_gate 对未确认状态有固定截止；超时不能当成已静音或资源已释放。
bool BrowserPcm::poll_output_gate(std::uint64_t now) {
    if (failed_)
        return false;
    if ((gate_clock_ && now < *gate_clock_) || (gate_pending_ && now >= gate_deadline_)) {
        disconnect();
        return false;
    }
    gate_clock_ = now;
    return true;
}
// request_output_gate 仅接纳独立票据，实际输出块确认前保持未知且不关闭采集。
std::optional<std::uint64_t> BrowserPcm::request_output_gate(bool enabled, std::uint64_t now) {
    if (!poll_output_gate(now) || !ready_ || gate_pending_)
        return {};
    if (gate_ticket_ == std::numeric_limits<std::uint64_t>::max() ||
        now > std::numeric_limits<std::uint64_t>::max() - 2000) {
        disconnect();
        return {};
    }
    ++gate_ticket_;
    gate_pending_ = true;
    gate_sent_ = false;
    gate_enabled_ = enabled;
    gate_deadline_ = now + 2000;
    if (!command({BrowserAudioCommandKind::OutputGate, {}, 0, gate_ticket_, enabled}))
        return {};
    return gate_ticket_;
}
// output_gate_confirmed 只反映当前原票据事实，不以目标、音量零或命令发出代替静音证明。
bool BrowserPcm::output_gate_confirmed(std::uint64_t ticket, bool enabled) const {
    return ready_ && !failed_ && !gate_pending_ && ticket != 0 && ticket == gate_confirmed_ &&
           enabled == gate_enabled_;
}
// output_gated 核验已交付的原票据和实际状态；旧事实隔离，错误当前事实使驱动终止。
bool BrowserPcm::output_gated(std::uint64_t ticket, bool enabled, std::uint64_t now) {
    if (!poll_output_gate(now) || !ready_)
        return false;
    if (ticket != 0 && ticket < gate_ticket_)
        return true;  // 旧回复/旧门控回调不能确认新票据，也不能让新采集被无谓终止。
    if (ticket == 0 || ticket != gate_ticket_ || enabled != gate_enabled_ ||
        (gate_pending_ && !gate_sent_)) {
        disconnect();
        return false;
    }
    if (!gate_pending_)
        return ticket == gate_confirmed_;  // 原实际确认重复不续期或新增副作用。
    gate_pending_ = false;
    gate_confirmed_ = ticket;
    return true;
}
}  // namespace ur::device
