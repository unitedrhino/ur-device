// 语音会话实现：先校验再产生驱动副作用；暂停输入与取消输出互不影响。
#include "ur/device/session.hpp"
#include <limits>
#include <utility>

namespace ur::device {
Session::~Session() { close(); }
bool Session::matches(std::string_view id) const { return active_ && session_id_ == id; }
Result Session::open(std::string id, Mode mode, Pipeline pipeline, Capabilities caps) {
    if (active_)
        return Result::Busy;
    if (id.empty() || id.size() > 128)
        return Result::Invalid;
    if ((mode != Mode::HalfDuplex && mode != Mode::FullDuplex) ||
        (pipeline != Pipeline::Cascade && pipeline != Pipeline::Realtime))
        return Result::Invalid;
    if (mode == Mode::HalfDuplex && !caps.half_duplex)
        return Result::Unsupported;
    if (mode == Mode::FullDuplex &&
        (!caps.full_duplex || !caps.device_aec || pipeline != Pipeline::Realtime))
        return Result::Unsupported;
    session_id_ = std::move(id);
    mode_ = mode;
    active_ = true;
    return Result::Applied;
}
Result Session::start_input(std::string_view session, std::string input) {
    if (!matches(session))
        return Result::Stale;
    if (input.empty() || input.size() > 128)
        return Result::Invalid;
    if (output_.media_suspended) return Result::Busy;
    if (input == input_id_)
        return capturing_ ? Result::Duplicate : Result::Stale;
    if (capturing_ || (mode_ == Mode::HalfDuplex && playing_))
        return Result::Busy;
    if (!port_.start_capture(input))
        return Result::DriverFailed;
    input_id_ = std::move(input);
    capturing_ = true;
    return Result::Applied;
}
Result Session::stop_input(std::string_view session, std::string_view input) {
    if (!matches(session) || input.empty() || input != input_id_)
        return Result::Stale;
    // 恢复窗口内的主动暂停撤销原意图；物理采集早已停止，不重复关闭驱动。
    if (output_.media_suspended && output_.resume_capture) {
        output_.resume_capture = false;
        return Result::Applied;
    }
    if (!capturing_)
        return Result::Duplicate;
    port_.stop_capture();
    capturing_ = false;
    return Result::Applied;
}
// suspend_media保留逻辑会话与有界去重水位，只清除原Peer的采集和播放资源。
Result Session::suspend_media(std::string_view session) {
    if (!matches(session))
        return Result::Stale;
    if (output_.media_suspended) return Result::Duplicate;
    output_.resume_capture = capturing_;
    if (capturing_)
        port_.stop_capture();
    if (playing_)
        port_.stop_playback(epoch_);
    capturing_ = playing_ = output_.network_done = false;
    output_.media_suspended = true;
    return Result::Applied;
}
// resume_media不重放旧音频，也不借用start_input绕过同inputId禁止复活的业务规则。
Result Session::resume_media(std::string_view session) {
    if (!matches(session))
        return Result::Stale;
    if (!output_.media_suspended) return Result::Duplicate;
    if (output_.resume_capture && !port_.start_capture(input_id_)) return Result::DriverFailed;
    capturing_ = output_.resume_capture;
    output_.resume_capture = output_.media_suspended = false;
    return Result::Applied;
}
Result Session::begin_response(std::string_view session, std::string response,
                               std::uint64_t sequence) {
    if (!matches(session))
        return Result::Stale;
    if (response.empty() || response.size() > 128 || sequence == 0)
        return Result::Invalid;
    if (output_.media_suspended) return Result::Busy;
    if (sequence == sequence_ && response == response_id_)
        return Result::Duplicate;
    if (sequence <= sequence_ || response == response_id_)
        return Result::Stale;
    if (playing_ || (mode_ == Mode::HalfDuplex && capturing_))
        return Result::Busy;
    if (epoch_ == std::numeric_limits<std::uint64_t>::max())
        return Result::Invalid;
    if (!port_.start_playback(response, epoch_ + 1))
        return Result::DriverFailed;
    ++epoch_;
    sequence_ = sequence;
    response_id_ = std::move(response);
    playing_ = true;
    output_.network_done = false;
    return Result::Applied;
}
Result Session::network_done(std::string_view session, std::string_view response) {
    if (!matches(session) || response.empty() || response != response_id_)
        return Result::Stale;
    if (!playing_ || output_.network_done) return Result::Duplicate;
    output_.network_done = true;
    return Result::Applied;
}
Result Session::playback_drained(std::uint64_t epoch) {
    if (!active_ || epoch != epoch_)
        return Result::Stale;
    if (!playing_)
        return Result::Duplicate;
    if (!output_.network_done) return Result::Busy;
    port_.stop_playback(epoch_);
    playing_ = false;
    return Result::Applied;
}
Result Session::cancel_response(std::string_view session, std::string_view response) {
    if (!matches(session) || response.empty() || response != response_id_)
        return Result::Stale;
    if (!playing_)
        return Result::Duplicate;
    port_.stop_playback(epoch_);
    playing_ = false;
    return Result::Applied;
}
void Session::close() {
    if (capturing_)
        port_.stop_capture();
    if (playing_)
        port_.stop_playback(epoch_);
    active_ = capturing_ = playing_ = output_.network_done = false;
    output_.media_suspended = output_.resume_capture = false;
    session_id_.clear();
    input_id_.clear();
    response_id_.clear();
    sequence_ = 0;
}
}  // namespace ur::device
