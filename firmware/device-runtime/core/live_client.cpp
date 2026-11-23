// 文件说明：实时接收原准入→真实网络→单轨实际输出；重放/背压保持原事实，结束切断完整原组。
// 模块划分：固定编码与关联、接收协商、两轨异步控制、实际事实和资源退出；不实现采集或焦点策略。
#include "ur/device/json_parse.hpp"
#include "ur/device/live_client.hpp"
#include <cJSON.h>
#include <algorithm>
#include <limits>
#include <utility>

namespace ur::device {
namespace {
// Json 只用于有限线上字段编码，不保存自由平台错误或重型媒体资源。
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
// same 对规范化正文全字段比较，额外成功字段不能被宽松generic解析忽略。
bool same(std::string_view actual, std::string_view expected) {
    std::string normalized;
    return canonical_json_object(expected, normalized) == DecodeResult::Ok && actual == normalized;
}
// fixed_reason 与云端接收腿稳定失败枚举一致，不从驱动自由错误文本取值。
bool fixed_reason(std::string_view reason) {
    constexpr std::string_view reasons[] = {
        "invalidParams",     "unsupportedCapability", "resourceBusy",  "deviceRejected",
        "userRejected",      "userStopped",           "parentEnded",   "permissionRevoked",
        "operationTimedOut", "deviceDisconnected",    "sourceExpired", "mediaFailed",
        "idleTimeout"};
    return std::find(std::begin(reasons), std::end(reasons), reason) != std::end(reasons);
}
// error_payload 严格只接原方法/稳定原因；generic VoiceEvent解析不替代Live字段白名单。
bool error_payload(const Envelope& message, std::string_view method) {
    Json root(ur::device::parse_json(message.payload.c_str()), cJSON_Delete);
    const auto* reason = root ? cJSON_GetObjectItemCaseSensitive(root.get(), "reason") : nullptr;
    if (!message.code || *message.code < 400 || *message.code > 599 || message.request ||
        message.event_sequence || message.message_sequence || !cJSON_IsString(reason) ||
        !fixed_reason(reason->valuestring))
        return false;
    if (message.method == "mediaAnswer")
        return cJSON_GetArraySize(root.get()) == 1;
    const auto* original = cJSON_GetObjectItemCaseSensitive(root.get(), "requestMethod");
    return message.method == "requestAck" && cJSON_GetArraySize(root.get()) == 2 &&
           cJSON_IsString(original) && std::string_view(original->valuestring) == method;
}
// payload 从编码树取得有限JSON原字节，不向失败日志反射树或私有SDP。
std::string payload(const Json& root) {
    std::unique_ptr<char, decltype(&cJSON_free)> value(cJSON_PrintUnformatted(root.get()),
                                                       cJSON_free);
    return value ? value.get() : std::string{};
}
}  // namespace

// LiveClient 固定原邀请与宿主期限；手填扩组或无效绑定永久拒绝，不执行准备/播放。
LiveClient::LiveClient(PublishPort& mqtt, LiveMediaPort& port, LiveInvite binding, std::string up,
                       std::string down, std::uint64_t deadline)
    : mqtt_(mqtt),
      port_(port),
      binding_(std::move(binding)),
      requests_(mqtt),
      up_(std::move(up)),
      down_(std::move(down)),
      deadline_(deadline) {
    std::string body;
    LiveInvite checked;
    if (up_.empty() || down_.empty() || up_ == down_ || !deadline_ ||
        encode_envelope(binding_.envelope, body) != DecodeResult::Ok ||
        decode_live_invite(WireRoute::Signaling, body, checked) != WireResult::Ok ||
        checked.audio != binding_.audio || checked.video != binding_.video ||
        checked.expires_at_ms != binding_.expires_at_ms)
        phase_ = LivePhase::Ended;
    history_.reserve(32);
}
// ~LiveClient 只请求原端口退出；宿主仍须收录实际释放，析构不能冒充成功事实。
LiveClient::~LiveClient() { port_.stop_all(); }

// observe 持续核原单调期限和有限业务授权；临时focus关闭不改变节目会话存活。
bool LiveClient::observe(std::uint64_t now) {
    if (phase_ == LivePhase::Ended)
        return false;
    if ((clock_ && now < *clock_) || now >= deadline_ ||
        (phase_ != LivePhase::Prepared && phase_ != LivePhase::Rejecting &&
         !port_.authorization_valid()) ||
        ((phase_ == LivePhase::Accepting || phase_ == LivePhase::Rejecting) &&
         now >= admission_deadline_)) {
        close(now);
        return false;
    }
    clock_ = now;
    return true;
}
// send 注册原规范化请求；发布背压由原Requests重发，不换token或重造正文。
bool LiveClient::send(Envelope command, std::uint64_t now) {
    std::string body;
    if (encode_envelope(command, body) != DecodeResult::Ok)
        return false;
    const auto result = requests_.submit(
        {command.token, command.session, command.method, up_, std::move(body)}, now);
    return result == SubmitResult::Accepted || result == SubmitResult::Duplicate;
}
// receive 核原鉴权路由、会话和关联，再推进原协商；下行消息不能冒充设备播放actual。
bool LiveClient::receive(std::string_view topic, std::string_view body, std::uint64_t now,
                         std::uint64_t wall) {
    Envelope message;
    if (topic != down_ || decode_envelope(body, message) != DecodeResult::Ok ||
        message.session != binding_.envelope.session)
        return false;
    // 无论云端正文多像成功事实，下行trackState都不能作为本端播放/焦点actual。
    if (message.method == "trackState")
        return false;
    if (message.method == "trackControl")
        return control(body, now);
    if (message.method == "sessionEnded") {
        VoiceEvent event;
        Json root(ur::device::parse_json(message.payload.c_str()), cJSON_Delete);
        if (phase_ == LivePhase::Prepared || message.request || message.message_sequence || !root ||
            cJSON_GetArraySize(root.get()) != 2 ||
            decode_voice_event(WireRoute::Signaling, body, event) != WireResult::Ok ||
            !fixed_reason(event.reason) ||
            (message.code ? (*message.code != 200 || end_token_.empty() ||
                             message.token != end_token_ || event.reason != end_reason_)
                          : (!message.event_sequence || *message.event_sequence <= sequence_)))
            return false;
        if (message.event_sequence)
            sequence_ = *message.event_sequence;
        if (message.code) {
            requests_.acknowledge({message.token, message.session, "sessionEnd", 200, up_}, now);
        }
        requests_.cancel_session(binding_.envelope.session, now);
        close(now);
        return true;
    }
    if (!observe(now))
        return false;
    if (message.method == "sessionInvite") {
        std::string expected, actual;
        LiveInvite invite;
        if (decode_live_invite(WireRoute::Signaling, body, invite) != WireResult::Ok ||
            encode_envelope(binding_.envelope, expected) != DecodeResult::Ok ||
            encode_envelope(invite.envelope, actual) != DecodeResult::Ok || expected != actual ||
            !message.event_sequence.has_value())
            return false;
        if (phase_ != LivePhase::Prepared)
            return true;
        if (wall >= invite.expires_at_ms ||
            now > std::numeric_limits<std::uint64_t>::max() - (invite.expires_at_ms - wall))
            return false;
        admission_deadline_ = now + (invite.expires_at_ms - wall);
        const auto prepared = port_.prepare(binding_, deadline_);
        Envelope command;
        command.token = message.token;
        command.session = message.session;
        if (prepared == LivePreparation::Ready) {
            command.method = "sessionAccept";
            command.payload = binding_.audio && binding_.video
                                  ? R"({"acceptedTracks":["broadcast-out","broadcast-video-out"]})"
                              : binding_.audio ? R"({"acceptedTracks":["broadcast-out"]})"
                                               : R"({"acceptedTracks":["broadcast-video-out"]})";
        } else {
            command.method = "sessionReject";
            rejection_reason_ = prepared == LivePreparation::Unsupported ? "unsupportedCapability"
                                : prepared == LivePreparation::Denied    ? "deviceRejected"
                                                                         : "resourceBusy";
            command.payload = "{\"reason\":\"" + rejection_reason_ + "\"}";
        }
        admission_method_ = command.method;
        if (!send(command, now)) {
            close(now);
            return false;
        }
        sequence_ = *message.event_sequence;
        started_at_ = now;
        phase_ = prepared == LivePreparation::Ready ? LivePhase::Accepting : LivePhase::Rejecting;
        if (phase_ == LivePhase::Rejecting)
            released_ = port_.stop_all();
        return true;
    }
    if (message.method == "requestAck" && message.token == binding_.envelope.token &&
        !admission_method_.empty()) {
        if (message.request || !message.code || message.event_sequence || message.message_sequence)
            return false;
        if (*message.code != 200) {
            if (!error_payload(message, admission_method_))
                return false;
        } else if (!same(
                       message.payload,
                       admission_method_ == "sessionAccept"
                           ? R"({"requestMethod":"sessionAccept","result":{"state":"negotiating"}})"
                           : "{\"requestMethod\":\"sessionReject\",\"result\":{\"state\":\"ended\","
                             "\"reason\":\"" +
                                 rejection_reason_ + "\"}}")) {
            return false;
        }
        if (!requests_.acknowledge(
                {message.token, message.session, admission_method_, *message.code, up_}, now))
            return requests_.state({up_, admission_method_, message.token}) ==
                   RequestState::Succeeded;
        if (*message.code != 200 || admission_method_ == "sessionReject") {
            close(now);
            return true;
        }
        if (!port_.begin_peer()) {
            close(now);
            return false;
        }
        peer_started_ = true;
        started_at_ = now;
        phase_ = LivePhase::Negotiating;
        return true;
    }
    VoiceEvent event;
    if (decode_voice_event(WireRoute::Signaling, body, event) != WireResult::Ok)
        return false;
    if (message.code && *message.code != 200 &&
        ((message.method == "mediaAnswer" && message.token == offer_token_) ||
         (message.method == "requestAck" && message.token == media_token_))) {
        if (!error_payload(message, message.method == "mediaAnswer" ? "mediaOffer" : "mediaState"))
            return false;
        close(now);
        return true;
    }
    if (message.method == "mediaAnswer") {
        Json root(ur::device::parse_json(message.payload.c_str()), cJSON_Delete);
        const auto* rtc = root ? cJSON_GetObjectItemCaseSensitive(root.get(), "webrtc") : nullptr;
        if (phase_ != LivePhase::Negotiating || !peer_started_ || answered_ ||
            offer_token_.empty() || message.token != offer_token_ || message.code != 200 ||
            event.revision != 1 || !root || cJSON_GetArraySize(root.get()) != 2 ||
            !cJSON_IsObject(rtc) || cJSON_GetArraySize(rtc) != 2)
            return false;
        if (!port_.apply_answer(event.answer_sdp) ||
            !requests_.acknowledge({message.token, message.session, "mediaOffer", 200, up_}, now)) {
            close(now);
            return false;
        }
        answered_ = true;
        return true;
    }
    if (message.method == "requestAck") {
        if (media_token_.empty() || message.token != media_token_ || message.code != 200 ||
            !same(
                message.payload,
                R"({"requestMethod":"mediaState","result":{"state":"connected","mediaRevision":1}})"))
            return false;
        if (!media_acked_ &&
            !requests_.acknowledge({message.token, message.session, "mediaState", 200, up_}, now))
            return false;
        media_acked_ = true;
        activate();
        return true;
    }
    if (message.method == "sessionEstablished") {
        if (!peer_started_ || !message.event_sequence || *message.event_sequence <= sequence_ ||
            !same(message.payload, R"({"state":"established","mediaRevision":1})"))
            return false;
        sequence_ = *message.event_sequence;
        established_ = true;
        activate();
        return true;
    }
    return false;
}

// offer_ready 仅接当前原Peer完整SDP；第二不同offer不能借旧媒体代次重新授权。
bool LiveClient::offer_ready(std::string_view sdp, std::uint64_t now) {
    if (!observe(now) || phase_ != LivePhase::Negotiating || !peer_started_ || sdp.size() < 8 ||
        sdp.size() > 49152 || sdp.substr(0, 5) != "v=0\r\n" ||
        sdp.find('\0') != std::string_view::npos)
        return false;
    Json root(cJSON_CreateObject(), cJSON_Delete);
    auto* rtc = root ? cJSON_AddObjectToObject(root.get(), "webrtc") : nullptr;
    if (!root || !rtc || !cJSON_AddNumberToObject(root.get(), "mediaRevision", 1) ||
        !cJSON_AddStringToObject(rtc, "iceMode", "complete") ||
        !cJSON_AddStringToObject(rtc, "sdp", std::string(sdp).c_str()))
        return false;
    const auto body = payload(root);
    if (!offer_token_.empty())
        return body == offer_payload_;
    Envelope command;
    command.method = "mediaOffer";
    command.session = binding_.envelope.session;
    command.token = "live-" + command.session + "-" + std::to_string(++serial_);
    command.payload = body;
    if (!send(command, now))
        return false;
    offer_token_ = command.token;
    offer_payload_ = body;
    return true;
}
// media_connected 仅报告真实当前网络事实，仍须原平台ACK与Established才可接播放意图。
bool LiveClient::media_connected(std::uint64_t now) {
    if (!observe(now) || phase_ != LivePhase::Negotiating || !answered_ || !port_.connected())
        return false;
    if (!media_token_.empty())
        return true;
    Envelope command;
    command.method = "mediaState";
    command.session = binding_.envelope.session;
    command.token = "live-" + command.session + "-" + std::to_string(++serial_);
    command.payload = R"({"state":"connected","mediaRevision":1})";
    if (!send(command, now))
        return false;
    media_token_ = command.token;
    return true;
}
// activate 交集当前网络、原answer与平台事实；不打开任何末端输出或生成active。
void LiveClient::activate() {
    if (phase_ == LivePhase::Negotiating && answered_ && media_acked_ && established_ &&
        port_.connected())
        phase_ = LivePhase::Established;
}
// track_index 限定原邀请两轨，不按收到的轨字段扩展接收组。
std::optional<std::size_t> LiveClient::track_index(std::string_view track_id) const {
    if (binding_.audio && track_id == "broadcast-out")
        return 0;
    if (binding_.video && track_id == "broadcast-video-out")
        return 1;
    return {};
}
// pending_count 为原异步操作预留回执容量，避免终态背压吞掉已受理原意图。
std::size_t LiveClient::pending_count() const {
    return static_cast<std::size_t>(pending_[0].has_value()) +
           static_cast<std::size_t>(pending_[1].has_value());
}
// actual_state 只读最近收录的历史实际状态；当前focus mask与授权须另核，不据此恢复输出。
std::string_view LiveClient::actual_state(std::string_view track_id) const {
    const auto index = track_index(track_id);
    return index ? actual_[*index] : std::string_view{};
}
// control 先核原幂等历史再核当前许可；每轨独立在途，重复同值新token仍是新原意图。
bool LiveClient::control(std::string_view body, std::uint64_t now) {
    LiveTrackControl command;
    if (decode_live_track_control(WireRoute::Signaling, body, binding_, command) !=
            WireResult::Ok ||
        !command.envelope.event_sequence.has_value())
        return false;
    for (const auto& entry : history_) {
        if (entry.command.envelope.token != command.envelope.token)
            continue;
        if (entry.command.envelope.payload != command.envelope.payload ||
            entry.command.envelope.event_sequence != command.envelope.event_sequence)
            return false;
        return entry.reply.empty() ? phase_ != LivePhase::Ended : mqtt_.publish(up_, entry.reply);
    }
    const auto index = track_index(command.track_id);
    if (!observe(now) || phase_ != LivePhase::Established || !index ||
        command.media_revision != 1 || *command.envelope.event_sequence <= sequence_ ||
        history_.size() >= 32 || replies_.size() + pending_count() >= replies_.capacity() ||
        generation_ == 9007199254740991ULL)
        return false;
    sequence_ = *command.envelope.event_sequence;
    history_.push_back({std::move(command), {}, ++generation_, now});
    const auto operation = history_.size() - 1;
    auto& entry = history_.back();
    if (pending_[*index] || (actual_[*index] == "stopped" && entry.command.operation != "stop"))
        return receipt(operation, 409, {}, "invalidParams");
    if (entry.command.operation == "resume" &&
        !port_.presentation_permitted(entry.command.track_id))
        return receipt(operation, 409, {}, "resourceBusy");
    pending_[*index] = operation;
    if (!port_.begin_track(entry.command, entry.generation)) {
        pending_[*index].reset();
        return receipt(operation, 503, {}, "mediaFailed");
    }
    return true;
}
// receipt 关联原操作生成一次确定字节；失败不附带成功实际状态，背压不改写结果。
// 保留 operation/code/state/reason 回执接口；operation 索引原历史，code 决定成功实态或失败原因字段，不重排调用。
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
bool LiveClient::receipt(std::size_t operation, int code, std::string_view state,
                         std::string_view reason) {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    auto& entry = history_[operation];
    Envelope message;
    message.request = false;
    message.token = entry.command.envelope.token;
    message.method = "trackState";
    message.session = binding_.envelope.session;
    message.code = code;
    message.payload = code == 200 ? "{\"trackId\":\"" + entry.command.track_id + "\",\"state\":\"" +
                                        std::string(state) + "\",\"mediaRevision\":1}"
                                  : "{\"reason\":\"" + std::string(reason) + "\"}";
    if (encode_envelope(message, entry.reply) != DecodeResult::Ok)
        return false;
    return replies_.push_back(std::string(entry.reply));
}
// flush 只交原回执队列，MQTT本地接纳不等同于平台持久收录。
void LiveClient::flush() {
    while (!replies_.empty()) {
        if (!mqtt_.publish(up_, *replies_.front()))
            break;
        replies_.pop_front();
    }
}
// close 一次终止原业务并保留未知回执；全组释放持续等待真实端口，不丢迟到隔离。
void LiveClient::close(std::uint64_t now) {
    if (phase_ != LivePhase::Ended) {
        for (auto& pending : pending_) {
            if (pending)
                receipt(*pending, 504, {}, "operationTimedOut");
            pending.reset();
        }
        requests_.cancel_session(binding_.envelope.session, now);
        phase_ = LivePhase::Ended;
    }
    released_ = port_.stop_all();
}
// end 保留明确原结束意图；先退役旧业务，再允许同一原sessionEnd重试而不复活节目。
bool LiveClient::end(std::string_view reason, std::uint64_t now) {
    if (!fixed_reason(reason))
        return false;
    if (!end_token_.empty())
        return reason == end_reason_;
    if (!observe(now) || phase_ == LivePhase::Prepared)
        return false;
    Envelope command;
    command.method = "sessionEnd";
    command.session = binding_.envelope.session;
    command.token = "live-" + command.session + "-" + std::to_string(++serial_);
    command.payload = "{\"reason\":\"" + std::string(reason) + "\"}";
    // 先退役旧在途意图再登记结束，避免cancel_session把背压未发送的原结束一并取消。
    close(now);
    if (!send(command, now))
        return false;
    end_token_ = command.token;
    end_reason_ = reason;
    flush();
    return true;
}
// tick 有界推进网络等待、原驱动事实与释放；临时focus变化由真实端口处理，不伪造停止。
void LiveClient::tick(std::uint64_t now) {
    flush();
    if (phase_ == LivePhase::Ended) {
        // 结束后仅原sessionEnd可能仍在途；旧业务已取消，不恢复任何播放或准入。
        if (!clock_ || now >= *clock_)
            requests_.tick(now);
        released_ = port_.stop_all();
        return;
    }
    if (!observe(now)) {
        flush();
        return;
    }
    requests_.tick(now);
    if (((phase_ == LivePhase::Accepting || phase_ == LivePhase::Rejecting) &&
         requests_.state({up_, admission_method_, binding_.envelope.token}) ==
             RequestState::TimedOut) ||
        (phase_ == LivePhase::Negotiating && now - started_at_ >= 10000) ||
        (phase_ == LivePhase::Established && !port_.connected())) {
        close(now);
    }
    if (phase_ == LivePhase::Ended) {
        flush();
        return;
    }
    for (std::size_t index = 0; index < pending_.size(); ++index) {
        const auto operation = pending_[index];
        if (operation && now - history_[*operation].started >= 10000) {
            close(now);
            flush();
            return;
        }
    }
    // 每tick最多八个驱动事实；错误/旧generation/m不得推进当前输出或丢原待完成请求。
    for (unsigned count = 0; count < 8; ++count) {
        const auto fact = port_.take_fact();
        if (!fact)
            break;
        const auto index = track_index(fact->track_id);
        if (!index || !pending_[*index])
            continue;
        const auto operation = *pending_[*index];
        const auto& entry = history_[operation];
        if (fact->generation != entry.generation || fact->media_revision != 1)
            continue;
        const auto expected = entry.command.operation == "resume"  ? "active"
                              : entry.command.operation == "pause" ? "paused"
                                                                   : "stopped";
        if (fact->code == 200) {
            // 端口形成事实时已核原授权/操作/mask；后来临时关闭mask不得抹除已完成原事实。
            // 这里只收录历史actual/原ACK，不调用驱动，也不从历史active恢复当前输出意图。
            if (fact->state != expected ||
                (fact->state == "active" &&
                 (phase_ != LivePhase::Established || !port_.connected())))
                continue;
            if (!receipt(operation, 200, fact->state, {})) {
                close(now);
                break;
            }
            actual_[*index] = fact->state;
        } else {
            if (fact->code < 400 || fact->code > 599 || !fact->state.empty())
                continue;
            // 临时focus取消必须来自原驱动实际事实；核心不伪造关闭、paused或失败完成。
            const auto reason = fact->failure == LiveMediaFailure::MediaFailed ? "mediaFailed"
                                : fact->failure == LiveMediaFailure::OutputBusy && fact->code == 409
                                    ? "resourceBusy"
                                    : nullptr;
            if (!reason)
                continue;
            if (!receipt(operation, fact->code, {}, reason)) {
                close(now);
                break;
            }
        }
        pending_[*index].reset();
    }
    // 单轨停止不能切另一轨；完整原组实际stopped后才退出Peer与全部输出资源。
    if ((!binding_.audio || actual_[0] == "stopped") &&
        (!binding_.video || actual_[1] == "stopped"))
        close(now);
    flush();
}
}  // namespace ur::device
