// 两向媒体独立轮询；每次有限工作量，坏包丢弃，驱动故障关闭资源。
#include "ur/device/udp_audio.hpp"

namespace ur::device {
// udp_failure_name 的字面值只标原分支，不携带媒体或连接身份。
std::string_view udp_failure_name(UdpFailure failure) {
    switch (failure) {
        case UdpFailure::Unconfigured:
            return "unconfigured";
        case UdpFailure::ClockRollback:
            return "clockRollback";
        case UdpFailure::ProbeReceive:
            return "probeReceive";
        case UdpFailure::ProbeSend:
            return "probeSend";
        case UdpFailure::ProbeDeadline:
            return "probeDeadline";
        case UdpFailure::CaptureRead:
            return "captureRead";
        case UdpFailure::CaptureEncode:
            return "captureEncode";
        case UdpFailure::CaptureEncrypt:
            return "captureEncrypt";
        case UdpFailure::CaptureSend:
            return "captureSend";
        case UdpFailure::ReplyReceive:
            return "replyReceive";
        case UdpFailure::ReplyCrypto:
            return "replyCrypto";
        case UdpFailure::ReplyBeyondEnd:
            return "replyBeyondEnd";
        case UdpFailure::PcmWrite:
            return "pcmWrite";
        case UdpFailure::PlaybackStall:
            return "playbackStall";
        case UdpFailure::Unknown:
            return "unknown";
    }
    return {};
}
// fail_observed 不改变关闭实现；首次事实在原PCM/通道被清理之前保存。
bool UdpAudio::fail_observed(UdpFailure failure) {
    if (failure_diagnostics_enabled_ && !failure_)
        failure_ = failure;
    close();
    return false;
}
UdpAudio::~UdpAudio() { close(); }
bool UdpAudio::start_capture(std::string_view input_id) {
    if (closed_ || capture_ || input_id.empty() || input_id.size() > 128)
        return false;
    capture_ = pcm_.begin_capture(input_id);
    if (capture_)
        last_voice_sent_.reset();
    return capture_;
}
void UdpAudio::stop_capture() {
    if (!capture_)
        return;
    capture_ = false;
    pcm_.end_capture();
}
bool UdpAudio::start_playback(std::string_view response_id, std::uint64_t epoch) {
    if (closed_ || playback_ || response_id.empty() || response_id.size() > 128 || epoch == 0 ||
        epoch <= epoch_)
        return false;
    if (!pcm_.begin_playback(response_id, epoch))
        return false;
    epoch_ = epoch;
    playback_ = true;
    onset_.reset();
    timing_ = {};
    playback_blocked_at_.reset();
    last_reply_sequence_.reset();
    return true;
}
void UdpAudio::stop_playback(std::uint64_t epoch) {
    if (!playback_ || epoch != epoch_)
        return;
    playback_ = false;
    playback_blocked_at_.reset();
    pcm_.end_playback(epoch);
}
bool UdpAudio::expect_first_sequence(std::uint32_t first_sequence) {
    const auto stride = channel_.sequence_stride();
    if (!playback_ || timing_.network_packets != 0 || last_reply_sequence_ || stride == 0 ||
        first_sequence < stride || first_sequence % stride != 0)
        return false;
    last_reply_sequence_ = first_sequence - stride;
    return true;
}
bool UdpAudio::poll(std::uint64_t now) {
    if (closed_)
        return false;
    // 上行不以 playback_ 为条件；半双工互斥由 Session 策略控制。
    for (unsigned index = 0; capture_ && index < 8; ++index) {
        std::vector<std::int16_t> pcm;
        const auto result = pcm_.read_frame(pcm);
        if (result == PcmResult::Empty)
            break;
        std::vector<std::uint8_t> opus, packet;
        // 保持原短路顺序与各驱动一次调用；仅拆开实际失败类别。
        if (result != PcmResult::Frame)
            return fail_observed(UdpFailure::CaptureRead);
        if (!codec_.encode(pcm, opus))
            return fail_observed(UdpFailure::CaptureEncode);
        if (channel_.encode(opus, packet) != UdpResult::Ok)
            return fail_observed(UdpFailure::CaptureEncrypt);
        if (!socket_.send(packet))
            return fail_observed(UdpFailure::CaptureSend);
        if (non_silent_pcm(pcm))
            last_voice_sent_ = now;
    }
    for (unsigned index = 0; index < 8; ++index) {
        // 32帧额度包含已经交给平台但未实际消费的帧。容量耗尽不先收包、更不先解码后丢帧；
        // 暂留在原有界UDP接收队列，让下一次played归还容量。上行及信令仍按原轮询推进。
        // 两秒持续无容量说明播放驱动停滞，必须真实关闭而不是无限等待；取消不受此屏障阻挡。
        if (playback_ && pcm_.playback_backpressured(epoch_)) {
            if (!playback_blocked_at_)
                playback_blocked_at_ = now;
            if (now < *playback_blocked_at_)
                return fail_observed(UdpFailure::ClockRollback);
            if (now - *playback_blocked_at_ >= 2000)
                return fail_observed(UdpFailure::PlaybackStall);
            break;
        }
        playback_blocked_at_.reset();
        std::vector<std::uint8_t> packet, opus;
        const auto received = socket_.receive(packet);
        if (received == ReceiveResult::Empty)
            break;
        if (received == ReceiveResult::Failed)
            return fail_observed(UdpFailure::ReplyReceive);
        if (received == ReceiveResult::Invalid)
            continue;
        const auto decoded = channel_.decode(packet.data(), packet.size(), opus);
        if (decoded == UdpResult::CryptoFailed || decoded == UdpResult::BeyondEnd)
            return fail_observed(decoded == UdpResult::CryptoFailed ? UdpFailure::ReplyCrypto
                                                                    : UdpFailure::ReplyBeyondEnd);
        if (decoded != UdpResult::Ok || !playback_)
            continue;
        // 即使暂停/取消也消费接收序号，避免重新播放已经见过的旧包。
        // 新信令由外层 ReplyDatagram 的回复 nonce 过滤；旧通道沿用其原始序号语义。
        // 新信令在启动时预告首包序号，跳号即为不可修复的丢包；旧通道仍从首个可见包统计。
        const auto sequence = channel_.received_sequence();
        if (last_reply_sequence_ && sequence > *last_reply_sequence_) {
            const auto stride = channel_.sequence_stride();
            const auto distance = sequence - *last_reply_sequence_;
            if (stride != 0 && distance > stride)
                timing_.missing_packets += distance / stride - 1;
        }
        last_reply_sequence_ = sequence;
        if (!timing_.first_network_ms)
            timing_.first_network_ms = now;
        ++timing_.network_packets;
        std::vector<std::int16_t> pcm;
        if (!codec_.decode(opus, pcm)) {
            onset_.reset();
            continue;
        }
        if (!pcm_.write_frame(pcm, epoch_))
            return fail_observed(UdpFailure::PcmWrite);
        ++timing_.decoded_frames;
        if (non_silent_pcm(pcm))
            ++timing_.non_silent_frames;
        if (onset_.observe(pcm) && !timing_.first_playable_ms)
            timing_.first_playable_ms = now;
    }
    return true;
}
void UdpAudio::close() {
    if (closed_)
        return;
    closed_ = true;
    stop_capture();
    stop_playback(epoch_);
    codec_.reset();
    socket_.close();
}
}  // namespace ur::device
