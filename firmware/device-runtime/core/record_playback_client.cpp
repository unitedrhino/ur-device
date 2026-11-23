// 文件说明：历史接收→真实加载→当前Peer→实际播放；seek先停旧代次，终态后仅补交原事实。
// 模块划分：固定JSON编码、原请求接纳、实际驱动事实、媒体协商与全组释放；不实现文件或采集驱动。
#include "ur/device/json_parse.hpp"
#include "ur/device/record_playback_client.hpp"
#include <cJSON.h>
#include <limits>
#include <utility>

namespace ur::device {
namespace {
// safe_integer 保持设备与平台JSON整数精度，媒体代次不能溢出重用。
constexpr std::uint64_t safe_integer = 9007199254740991ULL;
// Json 释放临时编码树，不把平台依赖保存在共享核心状态中。
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
// add_uint 用原十进制数字编码已校验uint，避免浮点近似改写关联。
bool add_uint(cJSON* object, const char* key, std::uint64_t value) {
    return cJSON_AddRawToObject(object, key, std::to_string(value).c_str()) != nullptr;
}
// print 返回固定字段JSON，失败返回空，不带平台原始错误或私有路径。
std::string print(const Json& object) {
    std::unique_ptr<char, decltype(&cJSON_free)> text(cJSON_PrintUnformatted(object.get()),
                                                      cJSON_free);
    return text ? text.get() : std::string{};
}
// same_payload 对固定结果逐字段规范化比较，不接受额外成功字段。
bool same_payload(std::string_view actual, std::string_view expected) {
    std::string canonical;
    return canonical_json_object(expected, canonical) == DecodeResult::Ok && actual == canonical;
}
// valid_sdp 仅检查有界完整SDP的基本格式；实际WebRTC适配器负责语义与方向。
bool valid_sdp(std::string_view sdp) {
    return sdp.size() >= 8 && sdp.size() <= 49152 && sdp.substr(0, 5) == "v=0\r\n" &&
           sdp.find('\0') == std::string_view::npos;
}
}  // namespace

// 构造只保存经wire再次校验的固定绑定，不能用调用者手填结构绕过字段检查。
RecordPlaybackClient::RecordPlaybackClient(PublishPort& mqtt, RecordPlaybackPort& port,
                                           RecordPlaybackInvite binding, std::string up,
                                           std::string down, std::uint64_t deadline)
    : mqtt_(mqtt),
      port_(port),
      binding_(std::move(binding)),
      requests_(mqtt),
      up_(std::move(up)),
      down_(std::move(down)),
      deadline_(deadline) {
    std::string body;
    RecordPlaybackInvite validated;
    if (up_.empty() || down_.empty() || up_ == down_ || deadline_ == 0 ||
        encode_envelope(binding_.envelope, body) != DecodeResult::Ok ||
        decode_record_playback_invite(WireRoute::Signaling, body, validated) != WireResult::Ok ||
        validated.camera_id != binding_.camera_id ||
        validated.recording_id != binding_.recording_id ||
        validated.recording_version != binding_.recording_version ||
        validated.content_ref != binding_.content_ref ||
        validated.playback_id != binding_.playback_id || validated.audio != binding_.audio ||
        validated.clip.start_ms != binding_.clip.start_ms ||
        validated.clip.end_ms != binding_.clip.end_ms ||
        validated.expires_at_ms != binding_.expires_at_ms)
        phase_ = RecordPlaybackPhase::Ended;
    history_.reserve(32);
}
// 析构切断全组但不制造线上released；宿主必须先等待实际Stopped再释放驱动。
RecordPlaybackClient::~RecordPlaybackClient() { port_.stop_all(); }

bool RecordPlaybackClient::observe(std::uint64_t now) {
    if (phase_ == RecordPlaybackPhase::Ended)
        return false;
    if ((clock_ && now < *clock_) || now >= deadline_ ||
        ((phase_ == RecordPlaybackPhase::Accepting || phase_ == RecordPlaybackPhase::Rejecting) &&
         now >= invite_deadline_)) {
        close(now);
        return false;
    }
    clock_ = now;
    return true;
}
bool RecordPlaybackClient::send(Envelope command, std::uint64_t now) {
    std::string body;
    if (encode_envelope(command, body) != DecodeResult::Ok)
        return false;
    const auto result = requests_.submit(
        {command.token, command.session, command.method, up_, std::move(body)}, now);
    return result == SubmitResult::Accepted || result == SubmitResult::Duplicate;
}

// 保留 topic/body 录制播放入口；topic 核原下行范围，body 解码原会话与录制/播放操作关联，不互换职责。
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
bool RecordPlaybackClient::receive(std::string_view topic, std::string_view body, std::uint64_t now,
                                   std::uint64_t wall) {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    Envelope message;
    if (topic != down_ || decode_envelope(body, message) != DecodeResult::Ok ||
        message.session != binding_.envelope.session)
        return false;
    // 终态可在本地撤权／断连后补到；只收原平台水位，不能据迟到终态恢复权限。
    if (message.method == "sessionEnded") {
        VoiceEvent event;
        if (phase_ == RecordPlaybackPhase::Prepared || message.code || !message.event_sequence ||
            message.message_sequence || *message.event_sequence <= sequence_ ||
            !terminal_token_.empty() ||
            decode_voice_event(WireRoute::Signaling, body, event) != WireResult::Ok)
            return false;
        Json payload(ur::device::parse_json(message.payload.c_str()), cJSON_Delete);
        if (!payload || cJSON_GetArraySize(payload.get()) != 2)
            return false;
        terminal_token_ = message.token;
        sequence_ = *message.event_sequence;
        close(now);
        publish_release();
        return true;
    }
    // 已记录原控制的回执可在Ended后重放，不执行驱动、不延长授权。
    if (message.method == "playbackControl")
        return submit_control(body, now);
    if (!observe(now))
        return false;
    if (message.method == "sessionInvite") {
        std::string expected, actual;
        RecordPlaybackInvite invite;
        if (decode_record_playback_invite(WireRoute::Signaling, body, invite) != WireResult::Ok ||
            encode_envelope(binding_.envelope, expected) != DecodeResult::Ok ||
            encode_envelope(invite.envelope, actual) != DecodeResult::Ok || expected != actual ||
            !message.event_sequence.has_value())
            return false;
        if (phase_ != RecordPlaybackPhase::Prepared)
            return true;
        if (wall >= invite.expires_at_ms || invite.expires_at_ms - wall > 30000)
            return false;
        const auto remaining = invite.expires_at_ms - wall;
        if (now > std::numeric_limits<std::uint64_t>::max() - remaining)
            return false;
        invite_deadline_ = now + remaining;
        const auto preparation = port_.prepare(binding_, deadline_);
        Envelope command;
        command.token = message.token;
        command.session = message.session;
        if (preparation == RecordPreparation::Ready) {
            command.method = "sessionAccept";
            command.payload = binding_.audio ? R"({"acceptedTracks":["video-1","audio-1"]})"
                                             : R"({"acceptedTracks":["video-1"]})";
        } else {
            command.method = "sessionReject";
            const char* reason = preparation == RecordPreparation::Unsupported
                                     ? "unsupportedCapability"
                                 : preparation == RecordPreparation::Denied ? "permissionDenied"
                                                                            : "resourceBusy";
            reject_reason_ = reason;
            command.payload = std::string("{\"reason\":\"") + reason + "\"}";
        }
        if (!send(command, now)) {
            close(now);
            return false;
        }
        accept_method_ = command.method;
        sequence_ = *message.event_sequence;
        changed_at_ = now;
        phase_ = preparation == RecordPreparation::Ready ? RecordPlaybackPhase::Accepting
                                                         : RecordPlaybackPhase::Rejecting;
        if (phase_ == RecordPlaybackPhase::Rejecting)
            released_ = port_.stop_all();
        return true;
    }
    if (message.method == "requestAck" && message.token == binding_.envelope.token &&
        !accept_method_.empty()) {
        if (message.request || !message.code || message.event_sequence || message.message_sequence)
            return false;
        if (*message.code == 200 && accept_method_ == "sessionAccept" &&
            !same_payload(message.payload,
                          R"({"requestMethod":"sessionAccept","result":{"state":"negotiating"}})"))
            return false;
        if (*message.code == 200 && accept_method_ == "sessionReject" &&
            !same_payload(
                message.payload,
                std::string(
                    R"({"requestMethod":"sessionReject","result":{"state":"ended","reason":")") +
                    reject_reason_ + "\"}}"))
            return false;
        if (*message.code != 200) {
            Json payload(ur::device::parse_json(message.payload.c_str()), cJSON_Delete);
            const auto* method =
                payload ? cJSON_GetObjectItemCaseSensitive(payload.get(), "requestMethod")
                        : nullptr;
            const auto* reason =
                payload ? cJSON_GetObjectItemCaseSensitive(payload.get(), "reason") : nullptr;
            if (*message.code < 400 || *message.code > 599 || !payload ||
                cJSON_GetArraySize(payload.get()) != 2 || !cJSON_IsString(method) ||
                method->valuestring != accept_method_ || !cJSON_IsString(reason))
                return false;
        }
        if (!requests_.acknowledge(
                {message.token, message.session, accept_method_, *message.code, up_}, now))
            return requests_.state({up_, accept_method_, message.token}) == RequestState::Succeeded;
        if (*message.code != 200) {
            close(now);
            return true;
        }
        if (accept_method_ == "sessionReject") {
            finish(now);
            return true;
        }
        phase_ = RecordPlaybackPhase::Ready;
        return true;
    }
    VoiceEvent event;
    if (decode_voice_event(WireRoute::Signaling, body, event) != WireResult::Ok)
        return false;
    if (message.code && *message.code >= 400 && *message.code <= 599 &&
        ((message.method == "mediaAnswer" && message.token == offer_token_) ||
         (message.method == "requestAck" && message.token == state_token_ &&
          event.request_method == "mediaState"))) {
        close(now);
        return true;
    }
    if (message.method == "mediaAnswer") {
        if (message.token != offer_token_ || !peer_started_ || answered_ || message.code != 200 ||
            event.revision != media_revision_)
            return false;
        Json payload(ur::device::parse_json(message.payload.c_str()), cJSON_Delete);
        const auto* rtc =
            payload ? cJSON_GetObjectItemCaseSensitive(payload.get(), "webrtc") : nullptr;
        if (!payload || cJSON_GetArraySize(payload.get()) != 2 || !cJSON_IsObject(rtc) ||
            cJSON_GetArraySize(rtc) != 2 ||
            !port_.apply_answer(media_revision_, event.answer_sdp) ||
            !requests_.acknowledge({message.token, message.session, "mediaOffer", 200, up_}, now)) {
            close(now);
            return false;
        }
        answered_ = true;
        return true;
    }
    if (message.method == "requestAck") {
        if (message.token != state_token_ || event.request_method != "mediaState" ||
            event.revision != media_revision_ || event.state != "connected" ||
            message.code != 200 ||
            !same_payload(
                message.payload,
                std::string(
                    R"({"requestMethod":"mediaState","result":{"state":"connected","mediaRevision":)") +
                    std::to_string(media_revision_) + "}}"))
            return false;
        if (!media_acked_ &&
            !requests_.acknowledge({message.token, message.session, "mediaState", 200, up_}, now))
            return false;
        media_acked_ = true;
        activate(now);
        return true;
    }
    if (message.method == "sessionEstablished") {
        if (event.revision != media_revision_ || !peer_started_ || !message.event_sequence ||
            *message.event_sequence <= sequence_ ||
            !same_payload(message.payload,
                          std::string(R"({"state":"established","mediaRevision":)") +
                              std::to_string(media_revision_) + "}"))
            return false;
        sequence_ = *message.event_sequence;
        established_ = true;
        activate(now);
        return true;
    }
    return false;
}

bool RecordPlaybackClient::submit_control(std::string_view body, std::uint64_t now) {
    PlaybackCommand command;
    if (decode_record_playback_control(WireRoute::Signaling, body, command) != WireResult::Ok ||
        command.envelope.session != binding_.envelope.session ||
        command.playback_id != binding_.playback_id || !command.media_revision.has_value() ||
        !command.envelope.event_sequence.has_value())
        return false;
    for (const auto& record : history_) {
        if (record.command.envelope.token != command.envelope.token)
            continue;
        if (record.command.envelope.payload != command.envelope.payload ||
            record.command.envelope.event_sequence != command.envelope.event_sequence)
            return false;
        if (!record.reply.empty())
            return mqtt_.publish(up_, record.reply);
        return phase_ != RecordPlaybackPhase::Ended;
    }
    const bool interrupt_seek =
        active_ && command.operation == "stop" && history_[*active_].command.operation == "seek";
    if (!observe(now) || (active_ && !interrupt_seek) || replies_.full() ||
        (interrupt_seek && replies_.size() >= replies_.capacity() - 1) || history_.size() == 32 ||
        command.expected_revision != revision_ || *command.media_revision != media_revision_ ||
        *command.envelope.event_sequence <= sequence_)
        return false;
    const auto operation = command.operation;
    const bool playable = state_ == "loaded" || state_ == "paused" || state_ == "playing";
    if ((operation == "load" && (phase_ != RecordPlaybackPhase::Ready || !state_.empty() ||
                                 command.content_ref != binding_.content_ref)) ||
        (operation != "load" && state_.empty()) ||
        (operation != "load" && operation != "stop" && phase_ != RecordPlaybackPhase::Active) ||
        (operation == "play" && (!playable || !port_.publishing_permitted(media_revision_))) ||
        (operation == "pause" && state_ != "playing" && state_ != "paused") ||
        (operation == "seek" &&
         (!playable || !command.position_ms ||
          *command.position_ms > binding_.clip.end_ms - binding_.clip.start_ms ||
          media_revision_ == safe_integer)) ||
        generation_ == safe_integer)
        return false;
    if (interrupt_seek) {
        // stop可切断正在定位的原操作；其执行范围未知，不能伪造seek成功或未执行。
        fail_active(504, "resultUnknown");
        seek_fact_.reset();
        restoring_ = false;
    }
    sequence_ = *command.envelope.event_sequence;
    history_.push_back({std::move(command), {}});
    active_ = history_.size() - 1;
    ++generation_;
    operation_at_ = now;
    started_ = false;
    if (operation == "seek") {
        // 唯一已鉴权平台seek才保留m+1；平台已先持久原token，重放不会再次执行此分支。
        previous_intent_ = state_;
        old_revision_ = media_revision_;
        ++media_revision_;
        phase_ = RecordPlaybackPhase::Seeking;
        changed_at_ = now;
        port_.close_peer(old_revision_);
        peer_started_ = answered_ = media_acked_ = established_ = restoring_ = false;
        offer_token_.clear();
        state_token_.clear();
        offer_body_.clear();
    }
    return true;
}

bool RecordPlaybackClient::begin_current_peer(std::uint64_t now) {
    if (peer_started_ || (old_revision_ && !port_.peer_stopped(old_revision_)))
        return false;
    if (!port_.begin_peer(media_revision_)) {
        close(now);
        return false;
    }
    peer_started_ = true;
    if (phase_ != RecordPlaybackPhase::Seeking)
        phase_ = RecordPlaybackPhase::Negotiating;
    changed_at_ = now;
    return true;
}
bool RecordPlaybackClient::offer_ready(std::uint64_t media_revision, std::string_view sdp,
                                       std::uint64_t now) {
    if (!observe(now) || media_revision != media_revision_ || !peer_started_ || !valid_sdp(sdp) ||
        (phase_ != RecordPlaybackPhase::Negotiating && phase_ != RecordPlaybackPhase::Seeking))
        return false;
    Json payload(cJSON_CreateObject(), cJSON_Delete);
    auto* rtc = payload ? cJSON_AddObjectToObject(payload.get(), "webrtc") : nullptr;
    if (!payload || !rtc || !add_uint(payload.get(), "mediaRevision", media_revision_) ||
        !cJSON_AddStringToObject(rtc, "iceMode", "complete") ||
        !cJSON_AddStringToObject(rtc, "sdp", std::string(sdp).c_str()))
        return false;
    const auto body = print(payload);
    if (!offer_token_.empty())
        return body == offer_body_;
    Envelope command;
    command.session = binding_.envelope.session;
    command.method = "mediaOffer";
    command.token = "record-" + command.session + "-" + std::to_string(++serial_);
    command.payload = body;
    if (!send(command, now)) {
        close(now);
        return false;
    }
    offer_token_ = command.token;
    offer_body_ = body;
    return true;
}
bool RecordPlaybackClient::media_connected(std::uint64_t media_revision, std::uint64_t now) {
    if (!observe(now) || media_revision != media_revision_ || !answered_ ||
        !port_.connected(media_revision_))
        return false;
    if (!state_token_.empty())
        return true;  // 重复真实连通不生成新token或刷新协商期限。
    Envelope command;
    command.session = binding_.envelope.session;
    command.token = "record-" + command.session + "-" + std::to_string(++serial_);
    if (voice_control(command.token, command.session, "mediaState", "connected", media_revision_,
                      command, 0, VoiceTransport::WebRTC) != WireResult::Ok ||
        !send(command, now)) {
        close(now);
        return false;
    }
    state_token_ = command.token;
    return true;
}
void RecordPlaybackClient::activate(std::uint64_t now) {
    if (!media_acked_ || !established_ || !answered_ || !port_.connected(media_revision_))
        return;
    if (phase_ == RecordPlaybackPhase::Seeking) {
        if (!restoring_ && !seek_fact_ && !active_) {
            if (!port_.restore_intent(media_revision_, previous_intent_, generation_)) {
                close(now);
                return;
            }
            restoring_ = true;
        }
    } else if (phase_ == RecordPlaybackPhase::Negotiating) {
        phase_ = RecordPlaybackPhase::Active;
    }
}

bool RecordPlaybackClient::emit_fact(const RecordPlaybackFact& fact) {
    // 非自发事实必须关联仍在途的操作；缺失关联时不发布或推进业务修订。
    if ((!fact.spontaneous && !active_.has_value()) || revision_ == safe_integer || replies_.full())
        return false;
    Json payload(cJSON_CreateObject(), cJSON_Delete);
    if (!payload ||
        !cJSON_AddStringToObject(payload.get(), "playbackId", binding_.playback_id.c_str()) ||
        !cJSON_AddStringToObject(payload.get(), "state", fact.state.c_str()) ||
        !add_uint(payload.get(), "revision", revision_ + 1) ||
        !add_uint(payload.get(), "mediaRevision", fact.media_revision))
        return false;
    if (fact.state == "failed") {
        if (!cJSON_AddStringToObject(payload.get(), "reason", "deviceRejected"))
            return false;
    } else if (!add_uint(payload.get(), "positionMs", fact.position_ms) ||
               !cJSON_AddNumberToObject(payload.get(), "rate", 1) ||
               (fact.duration_ms && !add_uint(payload.get(), "durationMs", *fact.duration_ms)))
        return false;
    Envelope message;
    message.request = false;
    message.session = binding_.envelope.session;
    message.method = "playbackState";
    message.token = fact.spontaneous
                        ? "record-event-" + message.session + "-" + std::to_string(revision_ + 1)
                        : history_[*active_].command.envelope.token;
    if (!fact.spontaneous)
        message.code = 200;
    message.payload = print(payload);
    std::string body;
    RecordPlaybackState validated;
    if (encode_envelope(message, body) != DecodeResult::Ok ||
        decode_record_playback_state(WireRoute::Signaling, body, validated) != WireResult::Ok)
        return false;
    if (!fact.spontaneous)
        history_[*active_].reply = body;
    if (!replies_.push_back(std::move(body)))
        return false;
    ++revision_;
    state_ = fact.state;
    if (!fact.spontaneous) {
        active_.reset();
        started_ = false;
    }
    return true;
}
void RecordPlaybackClient::fail_active(int code, const char* reason) {
    if (!active_)
        return;
    auto& record = history_[*active_];
    Envelope message;
    message.request = false;
    message.session = binding_.envelope.session;
    message.token = record.command.envelope.token;
    message.method = "playbackState";
    message.code = code;
    message.payload = std::string("{\"reason\":\"") + reason + "\"}";
    if (encode_envelope(message, record.reply) == DecodeResult::Ok)
        replies_.push_back(std::string(record.reply));
    active_.reset();
    started_ = false;
}
void RecordPlaybackClient::finish(std::uint64_t now) {
    requests_.cancel_session(binding_.envelope.session, now);
    phase_ = RecordPlaybackPhase::Ended;
    seek_fact_.reset();
    restoring_ = false;
    released_ = port_.stop_all();
}
void RecordPlaybackClient::close(std::uint64_t now) {
    if (phase_ != RecordPlaybackPhase::Ended) {
        fail_active(504, "resultUnknown");
        finish(now);
    }
}
void RecordPlaybackClient::flush() {
    while (!replies_.empty()) {
        if (!mqtt_.publish(up_, *replies_.front()))
            break;
        replies_.pop_front();
    }
}
void RecordPlaybackClient::publish_release() {
    if (!released_ || terminal_token_.empty() || !replies_.empty() || release_published_)
        return;
    if (!release_fact_) {
        Json payload(cJSON_CreateObject(), cJSON_Delete);
        if (!payload ||
            !cJSON_AddStringToObject(payload.get(), "playbackId", binding_.playback_id.c_str()) ||
            !cJSON_AddBoolToObject(payload.get(), "released", true) ||
            !add_uint(payload.get(), "endEventSeq", sequence_))
            return;
        Envelope message;
        message.request = false;
        message.token = terminal_token_;
        message.session = binding_.envelope.session;
        message.method = "playbackState";
        message.payload = print(payload);
        std::string body;
        if (encode_envelope(message, body) != DecodeResult::Ok)
            return;
        release_fact_ = std::move(body);
    }
    release_published_ = mqtt_.publish(up_, *release_fact_);
}

void RecordPlaybackClient::tick(std::uint64_t now) {
    flush();
    if (phase_ == RecordPlaybackPhase::Ended) {
        if (!released_)
            released_ = port_.stop_all();
        publish_release();
        return;
    }
    if (!observe(now)) {
        flush();
        return;
    }
    requests_.tick(now);
    if ((phase_ == RecordPlaybackPhase::Accepting || phase_ == RecordPlaybackPhase::Rejecting) &&
        (now >= invite_deadline_ ||
         requests_.state({up_, accept_method_, binding_.envelope.token}) ==
             RequestState::TimedOut)) {
        close(now);
        flush();
        return;
    }
    if (generation_ == 0)
        return;  // 尚未受理播放器操作，不接旧档案队列的未绑定事实。
    if ((active_ && now - operation_at_ >= 10000) ||
        (phase_ == RecordPlaybackPhase::Seeking && now - operation_at_ >= 25000) ||
        (peer_started_ && phase_ != RecordPlaybackPhase::Active && now - changed_at_ >= 15000) ||
        (state_ == "playing" && phase_ != RecordPlaybackPhase::Seeking &&
         (!port_.publishing_permitted(media_revision_) || !port_.connected(media_revision_)))) {
        close(now);
        flush();
        return;
    }
    if (active_ && !started_) {
        if (!port_.begin(history_[*active_].command, generation_, media_revision_)) {
            fail_active(503, "sourceUnavailable");
            finish(now);
            flush();
            return;
        }
        started_ = true;
    }
    if (seek_fact_ && port_.peer_stopped(old_revision_) && !replies_.full()) {
        auto fact = std::move(*seek_fact_);
        seek_fact_.reset();
        if (!emit_fact(fact) || !begin_current_peer(now)) {
            close(now);
            flush();
            return;
        }
    }
    // 每轮最多4条真实事实；输出背压时不取走驱动队列中的不可丢原结果。
    for (unsigned count = 0; count < 4 && !replies_.full() && !seek_fact_; ++count) {
        auto fact = port_.take_fact();
        if (!fact)
            break;
        if (fact->generation != generation_ || fact->media_revision != media_revision_)
            continue;
        if (!fact->spontaneous && !active_)
            continue;
        if (!fact->spontaneous && fact->code >= 400 && fact->code <= 599) {
            fail_active(fact->code, "deviceRejected");
            finish(now);
            break;
        }
        const auto duration = binding_.clip.end_ms - binding_.clip.start_ms;
        bool valid = fact->code == 200 && fact->position_ms <= duration &&
                     (!fact->duration_ms || *fact->duration_ms == duration);
        if (fact->spontaneous) {
            valid = valid && !active_ &&
                    ((fact->state == "ended" && state_ == "playing") || fact->state == "failed" ||
                     (restoring_ && fact->state == previous_intent_ &&
                      (fact->state != "playing" || port_.publishing_permitted(media_revision_))));
        } else {
            const auto& operation = history_[*active_].command.operation;
            valid =
                valid && ((operation == "load" && fact->state == "loaded" && fact->duration_ms) ||
                          (operation == "play" && fact->state == "playing" &&
                           port_.publishing_permitted(media_revision_)) ||
                          (operation == "pause" && fact->state == "paused") ||
                          (operation == "stop" && fact->state == "stopped") ||
                          (operation == "seek" && fact->state == "seeking"));
        }
        if (!valid) {
            close(now);
            break;
        }
        if (fact->state == "seeking" && !port_.peer_stopped(old_revision_)) {
            seek_fact_ = std::move(*fact);
            break;
        }
        const auto fact_state = fact->state;
        if (!emit_fact(*fact)) {
            close(now);
            break;
        }
        if ((fact_state == "loaded" && !peer_started_) || fact_state == "seeking") {
            if (!begin_current_peer(now))
                break;
        } else if (restoring_ && fact->spontaneous && fact_state == previous_intent_) {
            phase_ = RecordPlaybackPhase::Active;
            restoring_ = false;
            old_revision_ = 0;
            previous_intent_.clear();
        } else if (fact_state == "ended" || fact_state == "stopped" || fact_state == "failed") {
            finish(now);
            break;
        }
    }
    activate(now);
    flush();
    publish_release();
}
}  // namespace ur::device
