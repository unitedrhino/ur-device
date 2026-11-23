// UDP 探测与收发组合不依赖桌面库；设备移植保留同一状态/错误处理路径。
#include "ur/device/udp_voice_media.hpp"

#include "ur/device/connect_trace.hpp"

namespace ur::device {
namespace {
// 连接提速测量标签：UDP 媒体装配与探测就绪。
constexpr const char* kConnectTag = "ur_conn_udp";
}  // namespace
bool UdpVoiceMedia::enable_presignal_diagnostics() {
    if (presignal_enabled_)
        return true;
    if (pump_ || failure_terminal_)
        return false;
    presignal_enabled_ = true;
    return true;
}
// enable_failure_diagnostics 不恢复通道；仅绑定之后实际失败的固定观察。
void UdpVoiceMedia::enable_failure_diagnostics() {
    if (failure_terminal_)
        return;
    failure_diagnostics_enabled_ = true;
    if (pump_)
        pump_->enable_failure_diagnostics();
}
// fail_observed 终态首次enable不能补采；原首因在泵销毁后仍可只读。
bool UdpVoiceMedia::fail_observed(UdpFailure failure) {
    failure_terminal_ = true;
    if (failure_diagnostics_enabled_ && !failure_)
        failure_ = failure;
    close();
    return false;
}
bool UdpVoiceMedia::configure(const VoiceMedia& media) {
    close();
    if (media.host.empty() || media.port == 0 || media.audio.channels != 1 ||
        (media.audio.sample_rate != 16000 && media.audio.sample_rate != 24000 &&
         media.audio.sample_rate != 48000) ||
        (media.audio.frame_ms != 20 && media.audio.frame_ms != 60))
        return false;
    if (!cipher_.install(media.key) || !codec_.configure(media.audio) ||
        !socket_.open("0.0.0.0", 0) || !socket_.set_peer(media.host, media.port)) {
        close();
        return false;
    }
    uplink_nonce_ = media.nonce;
    channel_ = std::make_unique<UdpChannel>(cipher_, media.nonce, 128);
    replies_ = std::make_unique<ReplyDatagram>(socket_);
    if (presignal_enabled_)
        (void)replies_->enable_presignal_diagnostics();
    pump_ = std::make_unique<UdpAudio>(pcm_, codec_, *replies_, *channel_);
    if (failure_diagnostics_enabled_)
        pump_->enable_failure_diagnostics();
    std::vector<std::int16_t> silence(
        static_cast<std::size_t>(media.audio.sample_rate * media.audio.frame_ms / 1000), 0);
    std::vector<std::uint8_t> opus;
    if (!codec_.encode(silence, opus) || opus.size() > 1275 ||
        channel_->encode(opus, probe_) != UdpResult::Ok) {
        close();
        return false;
    }
    UR_CONNECT_TRACE(kConnectTag,
                     "stage=udp_media_configured host_len=%u port=%u rate=%d t=%llu",
                     static_cast<unsigned>(media.host.size()), static_cast<unsigned>(media.port),
                     media.audio.sample_rate, UR_CONNECT_MS());
    return true;
}
std::uint32_t UdpVoiceMedia::next_input_sequence() const {
    return ready_ && channel_ ? channel_->next_sequence() : 0;
}
bool UdpVoiceMedia::prepare_reply(const std::array<std::uint8_t, 8>& nonce,
                                  std::uint32_t first_sequence) {
    if (!ready_ || playing_ || !replies_ || nonce == uplink_nonce_ ||
        (reply_nonce_ && *reply_nonce_ == nonce) || first_sequence == 0 ||
        first_sequence % 128 != 0 || !replies_->select_nonce(nonce))
        return false;
    channel_->select_receive_nonce(nonce);
    if (!channel_->set_receive_floor(first_sequence))
        return false;
    reply_nonce_ = nonce;
    first_sequence_ = first_sequence;
    reply_prepared_ = true;
    if (presignal_enabled_) {
        presignal_snapshot_ = replies_->presignal_diagnostic();
        if (presignal_snapshot_) {
            if (!detail::presignal_increment(presignal_observation_))
                presignal_snapshot_.reset();
            else
                presignal_snapshot_->observation = presignal_observation_;
        }
    }
    return true;
}
bool UdpVoiceMedia::finish_reply(std::uint32_t last_sequence) {
    if (!ready_ || !playing_ || !now_ || last_sequence % 128 != 0 ||
        last_sequence < channel_->received_sequence() ||
        (last_sequence != 0 && last_sequence < first_sequence_))
        return false;
    if (end_at_)
        return last_sequence_ == last_sequence;
    if (!channel_->limit_receive_sequence(last_sequence))
        return false;
    last_sequence_ = last_sequence;
    end_at_ = now_;
    tail_idle_at_ = now_;
    return true;
}
bool UdpVoiceMedia::start_capture(std::string_view input_id) {
    const bool started = ready_ && pump_ && pump_->start_capture(input_id);
    if (started)
        UR_CONNECT_TRACE(kConnectTag, "stage=capture_driver_start transport=udp t=%llu",
                         UR_CONNECT_MS());
    return started;
}
void UdpVoiceMedia::stop_capture() {
    if (pump_)
        pump_->stop_capture();
}
bool UdpVoiceMedia::start_playback(std::string_view response_id, std::uint64_t epoch) {
    if (!ready_ || !reply_prepared_ || !pump_ || !pump_->start_playback(response_id, epoch) ||
        !pump_->expect_first_sequence(first_sequence_))
        return false;
    epoch_ = epoch;
    playing_ = true;
    reply_prepared_ = false;
    end_at_.reset();
    tail_idle_at_.reset();
    last_sequence_ = 0;
    missing_tail_ = false;
    return true;
}
void UdpVoiceMedia::stop_playback(std::uint64_t epoch) {
    if (!playing_ || epoch != epoch_)
        return;
    // 驱动停播命令可能晚于下一回复启动才被桌面桥消费；先按当前代次冻结统计。
    // 积压超过固定容量时丢弃最旧事实，领取旧命令将失败而不是误报下一回复计数。
    if (stopped_timings_.full())
        stopped_timings_.pop_front();
    auto final = pump_->timing();
    final.observed_complete =
        end_at_.has_value() && !missing_tail_ && channel_->received_sequence() == last_sequence_ &&
        final.missing_packets == 0 && final.decoded_frames == final.network_packets;
    stopped_timings_.push_back(StoppedTiming{epoch, final});
    pump_->stop_playback(epoch);
    playing_ = false;
}
bool UdpVoiceMedia::poll(std::uint64_t now) {
    if (!pump_) {
        // 原未配置/已关闭poll仍直接false；不增加一次原close或驱动调用。
        if (!failure_terminal_) {
            failure_terminal_ = true;
            if (failure_diagnostics_enabled_ && !failure_)
                failure_ = UdpFailure::Unconfigured;
        }
        return false;
    }
    if (now_ && now < *now_)
        return fail_observed(UdpFailure::ClockRollback);
    now_ = now;
    if (!ready_) {
        // 回显是网络连通证明；不触发采集/播放，亦不消耗下行回复的 nonce 空间。
        for (unsigned i = 0; i < 16; ++i) {
            std::vector<std::uint8_t> packet;
            const auto result = socket_.receive(packet);
            if (result == ReceiveResult::Failed)
                return fail_observed(UdpFailure::ProbeReceive);
            if (result == ReceiveResult::Empty)
                break;
            if (result == ReceiveResult::Received && probe_at_ && packet == probe_) {
                ready_ = true;
                probe_.clear();
                UR_CONNECT_TRACE(kConnectTag, "stage=udp_probe_ready attempts=%u t=%llu",
                                 static_cast<unsigned>(probe_attempts_), UR_CONNECT_MS());
                return true;
            }
        }
        if (!probe_at_ || now - *probe_at_ >= 500) {
            if (probe_attempts_ == 3)
                return fail_observed(UdpFailure::ProbeDeadline);
            if (!socket_.send(probe_))
                return fail_observed(UdpFailure::ProbeSend);
            probe_at_ = now;
            ++probe_attempts_;
        }
        return true;
    }
    const auto previous_sequence = channel_->received_sequence();  // 只读原水位，不提前领取UDP包。
    if (!pump_->poll(now)) {
        return fail_observed(pump_->failure_diagnostic().value_or(UdpFailure::Unknown));
    }
    // 250ms只度量允许继续接收但无进展的网络等待；原PCM满队列仍由媒体泵两秒截止独立约束。
    // 否则MQTT已结束而浏览器尚未消费时，会把缓存在UDP中的真音频误判丢失并提前排空。
    if (playing_ && end_at_ &&
        (channel_->received_sequence() != previous_sequence || pcm_.playback_backpressured(epoch_)))
        tail_idle_at_ = now;
    if (playing_ && tail_idle_at_ && now - *tail_idle_at_ >= 250 &&
        channel_->received_sequence() < last_sequence_)
        missing_tail_ = true;
    return true;
}
std::optional<std::uint64_t> UdpVoiceMedia::drained_epoch() const {
    if (playing_ && end_at_ && (missing_tail_ || channel_->received_sequence() == last_sequence_) &&
        pcm_.playback_empty(epoch_))
        return epoch_;
    return {};
}
const AudioTiming& UdpVoiceMedia::timing() const {
    static const AudioTiming empty;  // 未配置时不泄露上一会话的统计。
    return pump_ ? pump_->timing() : empty;
}
std::optional<AudioTiming> UdpVoiceMedia::take_stopped_timing(std::uint64_t epoch) {
    const auto* stopped = stopped_timings_.front();
    if (!stopped || stopped->epoch != epoch)
        return std::nullopt;
    const auto timing = stopped->timing;
    stopped_timings_.pop_front();
    return timing;
}
std::optional<std::uint64_t> UdpVoiceMedia::last_non_silent_sent_ms() const {
    return pump_ ? pump_->last_non_silent_sent_ms() : std::nullopt;
}
void UdpVoiceMedia::close() {
    if (pump_)
        failure_terminal_ = true;  // 正常关闭也不允许事后开启重建首因。
    stopped_timings_.clear();
    pump_.reset();
    replies_.reset();
    channel_.reset();
    socket_.close();
    codec_.reset();
    cipher_.clear();
    probe_.clear();
    reply_nonce_.reset();
    first_sequence_ = 0;
    uplink_nonce_ = {};
    now_.reset();
    probe_at_.reset();
    end_at_.reset();
    tail_idle_at_.reset();
    epoch_ = last_sequence_ = probe_attempts_ = 0;
    ready_ = playing_ = reply_prepared_ = missing_tail_ = false;
}
}  // namespace ur::device
