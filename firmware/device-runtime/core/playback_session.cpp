// 文件说明：固定文件邀请、接听ACK、平台建立及播放事实的协调；不创建麦克风/摄像头或授予内容访问。
#include "ur/device/playback_session.hpp"
#include <cJSON.h>

namespace ur::device {
// PlaybackSession 固定宿主已授权的邀请与资源期限；构造不执行采集、下载或播放。
PlaybackSession::PlaybackSession(PublishPort& mqtt, PlaybackDriver& driver, PlaybackInvite binding,
                                 PlaybackCapability capability, std::string up, std::string down,
                                 std::uint64_t deadline)
    : mqtt_(mqtt),
      driver_(driver),
      binding_(std::move(binding)),
      capability_(capability),
      requests_(mqtt),
      up_(std::move(up)),
      down_(std::move(down)),
      deadline_(deadline) {}
// 析构只关闭本实例，驱动须由宿主保持到本对象释放之后。
PlaybackSession::~PlaybackSession() { close(clock_.value_or(0)); }

// observe 生命周期失败只影响本实例；墙钟不用于延长驱动授权。
bool PlaybackSession::observe(std::uint64_t now) {
    if (phase_ == PlaybackPhase::Ended)
        return false;
    if ((clock_ && now < *clock_) || now >= deadline_ || !driver_.output_permitted()) {
        close(now);
        return false;
    }
    clock_ = now;
    return true;
}
// activate 建立事件与接听ACK允许乱序，但二者缺一都不能播放。
void PlaybackSession::activate() {
    if (phase_ == PlaybackPhase::Accepting && acknowledged_ && established_)
        phase_ = PlaybackPhase::Active;
}
// receive 先公共解码再精确关联；固定邀请规范化比较，重投不延长原等待。
// 保留 topic/body 播放入口；topic 核原下行范围，body 解码原邀请及播放会话，双时钟仍各用原范围。
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
bool PlaybackSession::receive(std::string_view topic, std::string_view body, std::uint64_t now,
                              std::uint64_t wall) {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    if (topic != down_)
        return false;
    Envelope message;
    if (decode_envelope(body, message) != DecodeResult::Ok ||
        message.session != binding_.envelope.session)
        return false;
    // 平台终态只收敛既有实例；本地撤权/到期先关闭也必须保留原会话终态关联。
    // 不依赖已撤销输出许可，不恢复授权；水位拒绝旧事件和重复终态。
    if (message.method == "sessionEnded") {
        VoiceEvent event;
        if (phase_ == PlaybackPhase::Prepared || !terminal_token_.empty() ||
            decode_voice_event(WireRoute::Signaling, body, event) != WireResult::Ok ||
            message.code || !message.event_sequence || *message.event_sequence <= sequence_)
            return false;
        sequence_ = *message.event_sequence;
        terminal_token_ = message.token;
        close(now);
        return true;
    }
    if (!observe(now))
        return false;
    if (message.method == "sessionInvite") {
        PlaybackInvite invite;
        std::string expected, actual;
        if (decode_playback_invite(WireRoute::Signaling, body, invite) != WireResult::Ok ||
            validate_playback_outputs(capability_, invite) != WireResult::Ok ||
            encode_envelope(binding_.envelope, expected) != DecodeResult::Ok ||
            encode_envelope(invite.envelope, actual) != DecodeResult::Ok || expected != actual ||
            binding_.playback_id != invite.playback_id ||
            binding_.content_ref != invite.content_ref ||
            binding_.expires_at != invite.expires_at ||
            binding_.output_modalities != invite.output_modalities ||
            !message.event_sequence.has_value() || wall >= invite.expires_at ||
            invite.expires_at - wall > 30000)
            return false;
        if (phase_ != PlaybackPhase::Prepared)
            return true;  // Requests保持原等待与原字节，不因重复邀请刷新期限。
        Envelope accept;
        accept.token = message.token;
        accept.session = message.session;
        accept.method = "sessionAccept";
        accept.payload = R"({"acceptedTracks":[]})";
        std::string payload;
        if (encode_envelope(accept, payload) != DecodeResult::Ok ||
            requests_.submit({accept.token, accept.session, accept.method, up_, payload}, now) !=
                SubmitResult::Accepted)
            return false;
        engine_ = std::make_unique<PlaybackEngine>(driver_, message.session, binding_.playback_id,
                                                   deadline_);
        phase_ = PlaybackPhase::Accepting;
        sequence_ = *message.event_sequence;
        return true;
    }
    if (phase_ == PlaybackPhase::Prepared)
        return false;
    if (message.method == "requestAck") {
        VoiceEvent rejected;
        if (message.code && *message.code >= 400 && message.token == binding_.envelope.token &&
            decode_voice_event(WireRoute::Signaling, body, rejected) == WireResult::Ok &&
            rejected.request_method == "sessionAccept") {
            if (acknowledged_ ||
                !requests_.acknowledge(
                    {message.token, message.session, "sessionAccept", *message.code, up_}, now))
                return false;
            close(now);
            return true;
        }
        if (message.request || !message.code || message.event_sequence ||
            message.message_sequence || message.token != binding_.envelope.token ||
            message.payload !=
                R"({"requestMethod":"sessionAccept","result":{"state":"established"}})" ||
            *message.code != 200)
            return false;
        if (!acknowledged_ && !requests_.acknowledge(
                                  {message.token, message.session, "sessionAccept", 200, up_}, now))
            return false;
        acknowledged_ = true;
        activate();
        return true;
    }
    if (message.method == "sessionEstablished") {
        if (message.request || message.code || message.message_sequence ||
            !message.event_sequence || !binding_.envelope.event_sequence.has_value() ||
            *message.event_sequence != *binding_.envelope.event_sequence + 1 ||
            message.payload != R"({"mediaRevision":0,"state":"established"})")
            return false;
        established_ = true;
        if (sequence_ < *message.event_sequence)
            sequence_ = *message.event_sequence;
        activate();
        return true;
    }
    if (message.method != "playbackControl" || phase_ != PlaybackPhase::Active ||
        !message.event_sequence || !binding_.envelope.event_sequence.has_value() ||
        *message.event_sequence <= *binding_.envelope.event_sequence + 1)
        return false;
    PlaybackCommand command;
    if (decode_playback_control(WireRoute::Signaling, body, command) != WireResult::Ok ||
        (command.operation == "load" && command.content_ref != binding_.content_ref))
        return false;
    const auto result = engine_->submit(body, now);
    if (result.admission == PlaybackAdmission::Accepted && sequence_ < *message.event_sequence)
        sequence_ = *message.event_sequence;
    if (result.admission == PlaybackAdmission::Replay)
        return mqtt_.publish(up_, result.reply);
    return result.admission == PlaybackAdmission::Accepted ||
           result.admission == PlaybackAdmission::Pending;
}
// publish_release 只编码原实例固定释放事实；不以播放state或请求ACK冒充整组资源释放。
void PlaybackSession::publish_release() {
    if (!released_ || terminal_token_.empty() || release_published_ || pending_fact_ ||
        (engine_ && !engine_->replies_drained()))
        return;
    if (!release_fact_) {
        std::unique_ptr<cJSON, decltype(&cJSON_Delete)> data(cJSON_CreateObject(), cJSON_Delete);
        if (!data ||
            !cJSON_AddStringToObject(data.get(), "playbackId", binding_.playback_id.c_str()) ||
            !cJSON_AddBoolToObject(data.get(), "released", true) ||
            !cJSON_AddRawToObject(data.get(), "endEventSeq", std::to_string(sequence_).c_str()))
            return;
        std::unique_ptr<char, decltype(&cJSON_free)> text(cJSON_PrintUnformatted(data.get()),
                                                          cJSON_free);
        if (!text)
            return;
        Envelope fact;
        fact.token = terminal_token_;
        fact.method = "playbackState";
        fact.session = binding_.envelope.session;
        fact.request = false;
        fact.payload = text.get();
        std::string body;
        if (encode_envelope(fact, body) != DecodeResult::Ok)
            return;
        release_fact_ = std::move(body);
    }
    if (mqtt_.publish(up_, *release_fact_))
        release_published_ = true;
}

// publish_pending 原操作事实先于独立释放证明交付；不恢复执行或读取迟到媒体回调。
bool PlaybackSession::publish_pending() {
    if (!pending_fact_ && engine_)
        pending_fact_ = engine_->take_reply();
    if (pending_fact_ && mqtt_.publish(up_, *pending_fact_))
        pending_fact_.reset();
    return !pending_fact_ && (!engine_ || engine_->replies_drained());
}

// retirement_release 只提供已固定的原终态事实，缺少任何释放/交付条件都不能退役。
std::optional<std::string> PlaybackSession::retirement_release() const {
    return release_submitted() ? release_fact_ : std::nullopt;
}
// tick 原事实保留到发布接纳；未建立不轮询驱动，也不制造loaded/playing。
// 保留 now/wall 双时钟轮询接口；now 推进原单调等待，wall 核原内容授权截止，不互换时钟职责。
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
void PlaybackSession::tick(std::uint64_t now, std::uint64_t wall) {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    if (phase_ == PlaybackPhase::Ended) {
        // 关闭后仍确认实际释放；仅原平台终态关联的释放事实可发送，不恢复播放或授权。
        if (!released_)
            released_ = driver_.stop_all();
        if (publish_pending())
            publish_release();
        return;
    }
    if (!observe(now))
        return;
    requests_.tick(now);
    if (phase_ == PlaybackPhase::Accepting &&
        (wall >= binding_.expires_at ||
         requests_.state({up_, "sessionAccept", binding_.envelope.token}) ==
             RequestState::TimedOut)) {
        close(now);
        return;
    }
    if (phase_ != PlaybackPhase::Active)
        return;
    engine_->poll(now);
    publish_pending();
}
// close 不宣称驱动实际释放成功，结束后拒绝新邀请或控制恢复本实例。
void PlaybackSession::close(std::uint64_t now) {
    if (phase_ == PlaybackPhase::Ended)
        return;
    requests_.cancel_session(binding_.envelope.session, now);
    if (engine_)
        engine_->close();
    else
        driver_.stop_all();
    phase_ = PlaybackPhase::Ended;
}
}  // namespace ur::device
