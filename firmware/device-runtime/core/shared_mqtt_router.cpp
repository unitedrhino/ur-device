// 文件说明：精确Topic/token/method分派与显式会话腿绑定；早到等待、退休保护和背压均有界且不触媒体。
#include "ur/device/json_parse.hpp"
#include "ur/device/shared_mqtt_router.hpp"
#include <cJSON.h>
#include <algorithm>
#include <memory>
#include "ur/device/action_wire.hpp"
#include "ur/device/context_snapshot.hpp"
#include "ur/device/focus_wire.hpp"
#include "ur/device/voice_wire.hpp"

namespace ur::device {
namespace {
// 固定诊断/路由预算不改变业务协议期限或会议准入。
constexpr std::size_t kBytes = 262144, kRecords = 512, kScopes = 16, kPerOwner = 32, kWaiting = 16,
                      kQueued = 112, kFrame = 65536;
constexpr std::uint64_t kWait = 5000, kRequest = 30000, kTombstone = 600000;
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;  // 临时解析树所有权。
// member 只取已规范化对象的根字符串，不以嵌套文案推断purpose或requestMethod。
std::string member(std::string_view payload, const char* name) {
    const std::string bytes(payload);
    Json object(ur::device::parse_json(bytes.c_str()), cJSON_Delete);
    const auto* value = object ? cJSON_GetObjectItemCaseSensitive(object.get(), name) : nullptr;
    return cJSON_IsString(value) ? std::string(value->valuestring) : std::string{};
}
// valid_owner 拒绝强转非法枚举，避免数组越界或借用另一业务。
bool valid_owner(SharedMqttOwner owner) {
    return owner == SharedMqttOwner::AI || owner == SharedMqttOwner::Human ||
           owner == SharedMqttOwner::Global;
}
// valid_identity 验证原认证产品/设备两段，复用公共UTF8及控制字符验证。
bool valid_identity(const std::string& identity) {
    const auto slash = identity.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 == identity.size() ||
        identity.find('/', slash + 1) != std::string::npos ||
        identity.find_first_of("+#") != std::string::npos)
        return false;
    Envelope check;
    check.method = "identity";
    std::string ignored;
    // 产品和设备分别复用标识UTF8/长度守卫，不把两段总长误当单一msgToken的上限。
    check.token = identity.substr(0, slash);
    if (encode_envelope(check, ignored) != DecodeResult::Ok)
        return false;
    check.token = identity.substr(slash + 1);
    return encode_envelope(check, ignored) == DecodeResult::Ok;
}
// response_method 仅映射现有明确应答，不将未知method猜成某个原请求。
std::string response_method(const Envelope& envelope) {
    if (envelope.method == "requestAck")
        return member(envelope.payload, "requestMethod");
    if (envelope.method == "sessionCreated")
        return "sessionCreate";
    if (envelope.method == "mediaAnswer")
        return "mediaOffer";
    if (envelope.method == "sessionEnded")
        return "sessionEnd";
    if (envelope.method == "trackState")
        return "trackControl";
    if (envelope.method == "callState")
        return "callControl";
    if (envelope.method == "endpointState")
        return "endpointControl";  // 原请求token及session仍须下方严格账本关联，不给主动事件伪ACK。
    if (envelope.method == "sessionSnapshot")
        return "sessionSync";
    return {};
}
// envelope_bytes 包括解析后保留的重复字符串，不靠只限制原正文凑内存预算。
std::size_t envelope_bytes(const Envelope& envelope) {
    return envelope.token.size() + envelope.method.size() + envelope.session.size() +
           envelope.payload.size();
}
// action_reply 复用正式编码器，拒绝多余字段或与原物模型来源不符的回执。
bool action_reply(std::string_view body, ActionReply& reply) {
    std::string canonical;
    if (canonical_json_object(body, canonical) != DecodeResult::Ok)
        return false;
    Json object(ur::device::parse_json(canonical.c_str()), cJSON_Delete);
    if (!object || cJSON_GetArraySize(object.get()) != 6)
        return false;
    const auto* code = cJSON_GetObjectItemCaseSensitive(object.get(), "code");
    const auto* data = cJSON_GetObjectItemCaseSensitive(object.get(), "data");
    const auto* text = cJSON_GetObjectItemCaseSensitive(object.get(), "msg");
    if (!cJSON_IsNumber(code) || !cJSON_IsObject(data) || !cJSON_IsString(text) ||
        code->valuedouble < 200 || code->valuedouble > 599 ||
        code->valuedouble != static_cast<int>(code->valuedouble) ||
        member(canonical, "method") != "actionReply")
        return false;
    reply.token = member(canonical, "msgToken");
    reply.action = member(canonical, "actionID");
    reply.code = static_cast<int>(code->valuedouble);
    reply.message = text->valuestring;
    auto* bytes = cJSON_PrintUnformatted(data);
    if (!bytes)
        return false;
    reply.data = bytes;
    cJSON_free(bytes);
    std::string encoded;
    return encode_action_reply(reply, encoded) == DecodeResult::Ok && encoded == canonical;
}
}  // namespace

// 构造只冻结原认证身份和三个精确通道；不为借用业务创建任何网络。
SharedMqttRouter::SharedMqttRouter(PublishPort& sole_publisher, std::string identity, bool receipt)
    : sole_publisher_(sole_publisher),
      identity_(std::move(identity)),
      ports_{{BorrowedPort(*this, SharedMqttOwner::AI), BorrowedPort(*this, SharedMqttOwner::Human),
              BorrowedPort(*this, SharedMqttOwner::Global),
              BorrowedPort(*this, SharedMqttOwner::Invalid)}} {
    closed_ = !valid_identity(identity_);
    reply_receipt_enabled_ = receipt;
    const std::array<std::string, 3> channels{{"signaling", "message", "action"}};
    for (std::size_t i = 0; i < channels.size(); ++i) {
        up_[i] = "$thing/up/" + channels[i] + "/" + identity_;
        down_[i] = "$thing/down/" + channels[i] + "/" + identity_;
    }
}
// port 提供固定归属借用端口；非法枚举得到永久拒绝端口，不能退化为Global。
MqttPort& SharedMqttRouter::port(SharedMqttOwner owner) {
    return ports_[valid_owner(owner) ? static_cast<std::size_t>(owner) : 3];
}
// publish 仅交给原宿主路由登记；返回值仍为原适配器本地接纳事实。
bool SharedMqttRouter::BorrowedPort::publish(std::string_view topic, std::string_view body) {
    return router_.publish(owner_, topic, body);
}
// scope 保留退休腿用于迟到围栏；不从当前播放或候选状态猜归属。
SharedMqttRouter::Scope* SharedMqttRouter::scope(std::string_view session) {
    const auto found = std::find_if(scopes_.begin(), scopes_.end(),
                                    [&](const Scope& value) { return value.session == session; });
    return found == scopes_.end() ? nullptr : &*found;
}
// record 按完整来源查找，不因token相同而把异方法、异通道或异方向合并。
SharedMqttRouter::Record* SharedMqttRouter::record(Source source, std::string_view topic,
                                                   std::string_view method,
                                                   std::string_view token) {
    const auto found = std::find_if(records_.begin(), records_.end(), [&](const Record& value) {
        return value.source == source && value.topic == topic && value.envelope.method == method &&
               value.envelope.token == token;
    });
    return found == records_.end() ? nullptr : &*found;
}
// retained_bytes 包括原帧和解码关联的重复字符串；固定元数据另由条数上限约束。
std::size_t SharedMqttRouter::retained_bytes() const {
    std::size_t bytes = 0;
    for (const auto& value : records_)
        bytes += value.topic.size() + value.body.size() + envelope_bytes(value.envelope) +
                 value.reply.size() + value.candidate.size();
    for (const auto& value : scopes_)
        bytes += value.session.size();
    for (const auto& value : queue_)
        bytes += value.message.topic.size() + value.message.body.size() + value.session.size();
    return bytes;
}
// private_context_bytes 计入队列与原记录的全部私有副本，不把解析副本漏出预算。
std::size_t SharedMqttRouter::private_context_bytes() const {
    std::size_t bytes = 0;  // 仅逻辑留存字节，不能解释为分配器物理内存清零证明。
    for (const auto& value : records_)
        if (value.source == Source::Context)
            bytes += value.body.size() + value.envelope.payload.size() + value.reply.size();
    for (const auto& value : queue_)
        if (value.private_expires_at)
            bytes += value.message.body.size();
    return bytes;
}
// clear_private_record 释放正文/ACK的拥有字符串，仅原关联键参与十分钟迟到保护。
void SharedMqttRouter::clear_private_record(Record& value) {
    if (value.source != Source::Context)
        return;
    if (!value.retired)
        value.retired = now_;
    std::string{}.swap(value.body);
    std::string{}.swap(value.envelope.payload);
    std::string{}.swap(value.reply);
}
// expire_private_context 校时失败也撤销本地私有消费，不撤销原Human腿或普通消息。
void SharedMqttRouter::expire_private_context(std::uint64_t wall_ms) {
    private_clock_valid_ = wall_ms > 0 && wall_ms >= private_wall_highwater_;
    private_wall_highwater_ = std::max(private_wall_highwater_, wall_ms);
    for (auto& value : records_)
        if (value.source == Source::Context &&
            (!private_clock_valid_ || private_wall_highwater_ >= value.private_expires_at))
            clear_private_record(value);
    queue_.erase(std::remove_if(queue_.begin(), queue_.end(),
                                [&](const Queued& value) {
                                    return value.private_expires_at &&
                                           (!private_clock_valid_ ||
                                            private_wall_highwater_ >= value.private_expires_at);
                                }),
                 queue_.end());
}
// register_record 固定首次时间及原字节；冲突/已退休/预算满返回空，不更新原记录。
SharedMqttRouter::Record* SharedMqttRouter::register_record(Record value) {
    if (auto* old = record(value.source, value.topic, value.envelope.method, value.envelope.token))
        return !old->retired && old->owner == value.owner && old->body == value.body ? old
                                                                                     : nullptr;
    const auto bytes = value.topic.size() + value.body.size() + envelope_bytes(value.envelope) +
                       value.candidate.size();
    if (records_.size() >= kRecords || retained_bytes() + bytes > kBytes)
        return nullptr;
    records_.push_back(std::move(value));
    return &records_.back();
}
// tick 仅维护本地固定期限；倒退关闭全部路由但保留原帧，禁止借墙钟恢复失效腿。
bool SharedMqttRouter::tick(std::uint64_t now) {
    if (closed_ || publishing_)
        return false;
    if (has_time_ && now < now_) {
        close();
        return false;
    }
    has_time_ = true;
    now_ = now;
    // 未绑定来源在原30秒后退休；已绑定业务腿的请求仍保留到显式终态，不能淘汰迟到保护。
    for (auto& value : records_) {
        if (!value.retired && now - value.started >= kRequest &&
            (value.source == Source::Focus || value.source == Source::Action ||
             (!value.candidate.empty() && !scope(value.candidate))))
            value.retired = value.started + kRequest;
    }
    for (auto& value : queue_) {
        if (!value.owner && !value.reason && now - value.started >= kWait)
            value.reason = SharedUnroutableReason::Expired;
    }
    records_.erase(
        std::remove_if(records_.begin(), records_.end(),
                       [&](const Record& value) {
                           const auto finished = value.retired ? *value.retired : value.started;
                           // 到期的Context只保留小关联墓碑；原Human腿仍在时不能淘汰后重授权。
                           const auto* bound = scope(value.envelope.session);
                           if (value.source == Source::Context && bound && !bound->retired)
                               return false;
                           return (value.retired ||
                                   (value.envelope.session.empty() && value.candidate.empty())) &&
                                  now - finished >= kTombstone;
                       }),
        records_.end());
    scopes_.erase(std::remove_if(scopes_.begin(), scopes_.end(),
                                 [&](const Scope& value) {
                                     return value.retired && now - *value.retired >= kTombstone;
                                 }),
                  scopes_.end());
    return true;
}
// enqueue 预算不足不接纳原帧；早到同字节重复保持第一次等待期限。
SharedRouteResult SharedMqttRouter::enqueue(std::string_view topic, std::string_view body,
                                            std::string_view session,
                                            std::optional<SharedMqttOwner> owner,
                                            std::uint64_t private_expires_at) {
    if (!owner) {
        const auto duplicate = std::find_if(queue_.begin(), queue_.end(), [&](const Queued& value) {
            return !value.owner && !value.reason && value.message.topic == topic &&
                   value.message.body == body;
        });
        if (duplicate != queue_.end())
            return SharedRouteResult::Waiting;
    }
    const auto count = std::count_if(queue_.begin(), queue_.end(), [&](const Queued& value) {
        return !value.reason && value.owner == owner;
    });
    if (queue_.size() >= kQueued ||
        count >= static_cast<std::ptrdiff_t>(owner ? kPerOwner : kWaiting) ||
        retained_bytes() + topic.size() + body.size() + session.size() > kBytes)
        return SharedRouteResult::Full;
    Queued value;
    value.message.kind = MqttEvent::Kind::Message;
    value.message.topic = topic;
    value.message.body = body;
    value.session = session;
    value.owner = owner;
    value.started = now_;
    value.private_expires_at = private_expires_at;
    queue_.push_back(std::move(value));
    return owner ? SharedRouteResult::Routed : SharedRouteResult::Waiting;
}
// publish_context_ack 只核原交付关联；实际完整留存及墙钟截止由Human接收器另验。
bool SharedMqttRouter::publish_context_ack(SharedMqttOwner owner, std::string_view body) {
    ContextSnapshotAck ack;
    if (!private_clock_valid_ || owner != SharedMqttOwner::Human ||
        decode_context_snapshot_ack(WireRoute::Message, body, ack) != WireResult::Ok)
        return false;
    const auto* bound = scope(ack.envelope.session);
    auto* source = record(Source::Context, down_[1], "contextSnapshot", ack.delivery_id);
    if (!bound || bound->retired || bound->owner != owner || !source || source->retired ||
        !source->delivered || source->owner != owner ||
        source->envelope.session != ack.envelope.session ||
        private_wall_highwater_ >= source->private_expires_at)
        return false;
    ContextSnapshot snapshot;
    Envelope expected;
    std::string canonical, expected_wire;
    if (decode_context_snapshot(WireRoute::Message, source->body, snapshot) != WireResult::Ok ||
        context_snapshot_ack(snapshot, expected) != WireResult::Ok ||
        encode_envelope(expected, expected_wire) != DecodeResult::Ok ||
        encode_envelope(ack.envelope, canonical) != DecodeResult::Ok ||
        canonical != expected_wire || (!source->reply.empty() && source->reply != canonical))
        return false;
    if (source->reply.empty()) {
        if (retained_bytes() + canonical.size() > kBytes)
            return false;
        source->reply = canonical;
    }
    publishing_ = true;
    const bool accepted = sole_publisher_.publish(up_[1], body);
    publishing_ = false;
    return accepted;
}
// reply_receipt_selected复用原创建来源，不增Scope预算，不将统一广告当成会话选择。
bool SharedMqttRouter::reply_receipt_selected(std::string_view session) const {
    if (!reply_receipt_enabled_)
        return false;
    for (const auto& value : records_) {
        if (value.source != Source::Request || value.owner != SharedMqttOwner::AI ||
            value.envelope.method != "sessionCreate" || value.candidate != session ||
            value.retired || !value.delivered)
            continue;
        VoiceEvent created;
        if (decode_voice_event(WireRoute::Signaling, value.reply, created) == WireResult::Ok &&
            created.envelope.method == "sessionCreated" && created.envelope.session == session &&
            created.media.transport == VoiceTransport::Udp && created.media.udp_reply_start_receipt)
            return true;
    }
    return false;
}
// relate_reply_start仅由Host取出原帧且核真实VoiceClient准入后调用；原512/256KiB/首次时间不变。
bool SharedMqttRouter::relate_reply_start(std::string_view topic, std::string_view body,
                                          std::uint64_t now) {
    VoiceEvent start;
    Envelope expected;
    std::string canonical;
    if (!tick(now) || topic != down_[0] ||
        decode_voice_event(WireRoute::Signaling, body, start) != WireResult::Ok ||
        reply_start_receipt(start, expected) != WireResult::Ok ||
        encode_envelope(start.envelope, canonical) != DecodeResult::Ok ||
        !reply_receipt_selected(start.envelope.session))
        return false;
    const auto* bound = scope(start.envelope.session);
    if (!bound || bound->retired || bound->owner != SharedMqttOwner::AI)
        return false;
    Record value;
    value.source = Source::ReplyStart;
    value.owner = bound->owner;
    value.topic = topic;
    value.body = canonical;
    value.envelope = start.envelope;
    value.started = now_;
    value.delivered = true;  // 仅调用方已取出的原帧，不等于业务接受/驱动安装。
    return register_record(std::move(value)) != nullptr;
}
// publish_reply_receipt只核原AI事件关联，HAL安装/永久围栏仍由VoiceClient控制；首次2秒不续。
bool SharedMqttRouter::publish_reply_receipt(SharedMqttOwner owner, std::string_view topic,
                                             std::string_view body) {
    UDPReplyStartReceipt fact;
    if (owner != SharedMqttOwner::AI || topic != up_[0] ||
        decode_reply_start_receipt(WireRoute::Signaling, body, fact) != WireResult::Ok ||
        !reply_receipt_selected(fact.envelope.session))
        return false;
    const auto* bound = scope(fact.envelope.session);
    auto* source = record(Source::ReplyStart, down_[0], "respAudioStart", fact.envelope.token);
    if (!bound || bound->retired || bound->owner != owner || !source || source->retired ||
        !source->delivered || source->owner != owner ||
        source->envelope.session != fact.envelope.session || now_ - source->started >= 2000)
        return false;
    VoiceEvent start;
    Envelope expected;
    std::string canonical, expected_wire;
    if (decode_voice_event(WireRoute::Signaling, source->body, start) != WireResult::Ok ||
        reply_start_receipt(start, expected) != WireResult::Ok ||
        encode_envelope(expected, expected_wire) != DecodeResult::Ok ||
        encode_envelope(fact.envelope, canonical) != DecodeResult::Ok ||
        canonical != expected_wire || (!source->reply.empty() && source->reply != canonical))
        return false;
    if (source->reply.empty()) {
        if (retained_bytes() + canonical.size() > kBytes)
            return false;
        source->reply = canonical;
    }
    publishing_ = true;
    const bool accepted = sole_publisher_.publish(topic, body);
    publishing_ = false;
    return accepted;
}
// publish 先登记精确归属再调用原端口，背压不能让另一业务偷取同一个关联。
// 焦点/物模型只做正式结构与原命令关联，不执行驱动、不授权资源。
bool SharedMqttRouter::publish(SharedMqttOwner owner, std::string_view topic,
                               std::string_view body) {
    if (closed_ || publishing_ || !has_time_ || !valid_owner(owner) || body.size() > kFrame)
        return false;
    Envelope receipt;
    if (reply_receipt_enabled_ && topic == up_[0] &&
        decode_envelope(body, receipt) == DecodeResult::Ok && receipt.method == "respAudioStart" &&
        !receipt.request)
        return publish_reply_receipt(owner, topic, body);
    if (topic == up_[1]) {
        Envelope candidate;
        if (decode_envelope(body, candidate) == DecodeResult::Ok &&
            candidate.method == "contextSnapshotAck")
            return publish_context_ack(owner, body);
    }
    if (topic == up_[2]) {
        ActionReply reply;
        if (owner != SharedMqttOwner::Global || !action_reply(body, reply))
            return false;
        auto* source = record(Source::Action, down_[2], "action", reply.token);
        if (!source || source->retired || !source->delivered ||
            source->envelope.payload != reply.action)
            return false;
        if (!source->reply.empty() && source->reply != body)
            return false;
        if (source->reply.empty()) {
            if (retained_bytes() + body.size() > kBytes)
                return false;
            source->reply = body;
        }
    } else {
        if (topic != up_[0] && topic != up_[1])
            return false;
        Envelope envelope;
        if (decode_envelope(body, envelope) != DecodeResult::Ok)
            return false;
        if (owner == SharedMqttOwner::Global) {
            if (topic != up_[0])
                return false;
            if (envelope.method == "focusState") {
                FocusState state;
                if (decode_focus_state(WireRoute::Signaling, body, state) != WireResult::Ok)
                    return false;
                auto* source = record(Source::Focus, down_[0], "focusControl", envelope.token);
                FocusCommand command;
                if (!source || source->retired || !source->delivered ||
                    decode_focus_control(WireRoute::Signaling, source->body, command) !=
                        WireResult::Ok ||
                    !focus_reply_matches(command, state))
                    return false;
                if (!source->reply.empty() && source->reply != body)
                    return false;
                if (source->reply.empty()) {
                    if (retained_bytes() + body.size() > kBytes)
                        return false;
                    source->reply = body;
                }
            } else if (envelope.method != "capabilityReport" || !envelope.session.empty() ||
                       !envelope.request || envelope.code || envelope.event_sequence ||
                       envelope.message_sequence)
                return false;
        } else {
            if (!envelope.request || envelope.code || envelope.event_sequence ||
                envelope.message_sequence || envelope.method == "capabilityReport" ||
                envelope.method == "focusControl" || envelope.method == "focusState")
                return false;
            if (envelope.session.empty()) {
                if (topic != up_[0] || envelope.method != "sessionCreate")
                    return false;
            } else {
                const auto* bound = scope(envelope.session);
                if (!bound || bound->retired || bound->owner != owner) {
                    // 忙线的原合法邀请允许原Human业务明确拒接，不为未接受候选建立新会话归属。
                    const auto* invite =
                        record(Source::Invite, down_[0], "sessionInvite", envelope.token);
                    if (owner != SharedMqttOwner::Human || topic != up_[0] ||
                        envelope.method != "sessionReject" || !invite || invite->retired ||
                        !invite->delivered || invite->candidate != envelope.session ||
                        now_ - invite->started >= kRequest)
                        return false;
                }
                if (envelope.method == "sessionCreate")
                    return false;
            }
        }
        if (envelope.method != "focusState") {
            Record value;
            value.owner = owner;
            value.topic = topic;
            value.body = body;
            value.envelope = std::move(envelope);
            value.started = now_;
            auto* registered = register_record(std::move(value));
            if (!registered || now_ - registered->started >= kRequest)
                return false;
        }
    }
    publishing_ = true;
    const bool accepted = sole_publisher_.publish(topic, body);
    publishing_ = false;
    return accepted;
}
// ingest 的顺序为精确身份、Global独立命令、原请求应答、原腿事件；只有明确创建来源可待绑定。
// Full时宿主必须保留同一原帧，不能重新生成消息或交给其他业务。
SharedRouteResult SharedMqttRouter::ingest(std::string_view topic, std::string_view body,
                                           std::uint64_t now) {
    if (!tick(now))
        return closed_ ? SharedRouteResult::Closed : SharedRouteResult::Rejected;
    if (body.size() > kFrame || (topic != down_[0] && topic != down_[1] && topic != down_[2]))
        return SharedRouteResult::Rejected;
    if (topic == down_[2]) {
        ActionRequest action;
        if (decode_action_request(body, action) != DecodeResult::Ok)
            return SharedRouteResult::Rejected;
        Record value;
        value.source = Source::Action;
        value.topic = topic;
        value.body = body;
        value.envelope.token = action.token;
        value.envelope.method = "action";
        value.envelope.payload = action.action;
        value.started = now_;
        // 先检查可投递预算；Full时不能登记一个实际未交付的命令。
        const auto bytes = value.topic.size() + value.body.size() + envelope_bytes(value.envelope);
        if (!record(Source::Action, topic, "action", action.token) &&
            (records_.size() >= kRecords ||
             retained_bytes() + bytes + topic.size() + body.size() > kBytes))
            return SharedRouteResult::Full;
        if (auto* old = record(Source::Action, topic, "action", action.token);
            old && (old->body != body || old->retired))
            return SharedRouteResult::Rejected;
        const auto queued = enqueue(topic, body, {}, SharedMqttOwner::Global);
        if (queued != SharedRouteResult::Routed)
            return queued;
        (void)register_record(std::move(value));
        return queued;
    }
    Envelope envelope;
    if (decode_envelope(body, envelope) != DecodeResult::Ok)
        return SharedRouteResult::Rejected;
    if (envelope.method == "contextSnapshot") {
        ContextSnapshot snapshot;
        const auto* bound = scope(envelope.session);
        if (!private_clock_valid_ || topic != down_[1] || !bound || bound->retired ||
            bound->owner != SharedMqttOwner::Human ||
            decode_context_snapshot(WireRoute::Message, body, snapshot) != WireResult::Ok ||
            snapshot.expires_at <= private_wall_highwater_)
            return SharedRouteResult::Rejected;
        Record value;
        value.source = Source::Context;
        value.owner = SharedMqttOwner::Human;
        value.topic = topic;
        value.body = body;
        value.envelope = envelope;
        value.started = now_;
        value.private_expires_at = snapshot.expires_at;
        const auto* old = record(Source::Context, topic, envelope.method, envelope.token);
        if (old && (old->body != body || old->retired))
            return SharedRouteResult::Rejected;
        const auto bytes = value.topic.size() + value.body.size() + envelope_bytes(envelope);
        if (!old && (records_.size() >= kRecords || retained_bytes() + bytes + topic.size() +
                                                            body.size() + envelope.session.size() >
                                                        kBytes))
            return SharedRouteResult::Full;
        const auto queued =
            enqueue(topic, body, envelope.session, bound->owner, snapshot.expires_at);
        if (queued != SharedRouteResult::Routed)
            return queued;
        (void)register_record(std::move(value));
        return queued;
    }
    if (envelope.method == "focusControl") {
        FocusCommand command;
        if (topic != down_[0] ||
            decode_focus_control(WireRoute::Signaling, body, command) != WireResult::Ok)
            return SharedRouteResult::Rejected;
        Record value;
        value.source = Source::Focus;
        value.topic = topic;
        value.body = body;
        value.envelope = envelope;
        value.started = now_;
        const auto bytes = value.topic.size() + value.body.size() + envelope_bytes(envelope);
        const auto* old = record(Source::Focus, topic, envelope.method, envelope.token);
        if (old && (old->body != body || old->retired))
            return SharedRouteResult::Rejected;
        if (!old && (records_.size() >= kRecords || retained_bytes() + bytes + topic.size() +
                                                            body.size() + envelope.session.size() >
                                                        kBytes))
            return SharedRouteResult::Full;
        const auto queued = enqueue(topic, body, envelope.session, SharedMqttOwner::Global);
        if (queued != SharedRouteResult::Routed)
            return queued;
        (void)register_record(std::move(value));
        return queued;
    }
    if (envelope.method == "sessionInvite") {
        if (topic != down_[0] || !envelope.request || envelope.code || envelope.session.empty() ||
            member(envelope.payload, "purpose") != "humanCall")
            return SharedRouteResult::Rejected;
        const auto* bound = scope(envelope.session);
        if (bound && (bound->retired || bound->owner != SharedMqttOwner::Human))
            return SharedRouteResult::Rejected;
        Record value;
        value.source = Source::Invite;
        value.owner = SharedMqttOwner::Human;
        value.topic = topic;
        value.body = body;
        value.envelope = envelope;
        value.candidate = envelope.session;
        value.started = now_;
        const auto* old = record(Source::Invite, topic, envelope.method, envelope.token);
        if (old && (old->body != body || old->retired))
            return SharedRouteResult::Rejected;
        const auto bytes = value.topic.size() + value.body.size() + envelope_bytes(envelope) +
                           value.candidate.size();
        if (!old && (records_.size() >= kRecords || retained_bytes() + bytes + topic.size() +
                                                            body.size() + envelope.session.size() >
                                                        kBytes))
            return SharedRouteResult::Full;
        const auto queued = enqueue(topic, body, envelope.session, SharedMqttOwner::Human);
        if (queued != SharedRouteResult::Routed)
            return queued;
        (void)register_record(std::move(value));
        return queued;
    }
    // 普通请求应答不能借用session事件通道；sessionError的code+eventSeq仍按原腿交业务。
    if (envelope.code && !envelope.event_sequence && !envelope.message_sequence) {
        if (envelope.request || *envelope.code < 200 || *envelope.code > 599)
            return SharedRouteResult::Rejected;
        const auto method = response_method(envelope);
        const auto& source_topic = topic == down_[0] ? up_[0] : up_[1];
        auto* source = record(Source::Request, source_topic, method, envelope.token);
        if (!source || source->retired || now_ - source->started >= kRequest)
            return SharedRouteResult::Rejected;
        const bool created = envelope.method == "sessionCreated" && *envelope.code == 200 &&
                             source->envelope.session.empty();
        const auto* existing = scope(envelope.session);
        if ((created &&
             (topic != down_[0] || envelope.session.empty() ||
              (existing && (existing->retired || existing->owner != source->owner ||
                            source->reply.empty() || source->candidate != envelope.session)))) ||
            (!created && envelope.session != source->envelope.session))
            return SharedRouteResult::Rejected;
        std::string canonical;
        if (encode_envelope(envelope, canonical) != DecodeResult::Ok ||
            (!source->reply.empty() && source->reply != canonical))
            return SharedRouteResult::Rejected;
        const auto added =
            source->reply.empty() ? canonical.size() + (created ? envelope.session.size() : 0) : 0;
        if (retained_bytes() + added + topic.size() + body.size() + envelope.session.size() >
            kBytes)
            return SharedRouteResult::Full;
        const auto queued = enqueue(topic, body, envelope.session, source->owner);
        if (queued != SharedRouteResult::Routed)
            return queued;
        source->reply = std::move(canonical);
        if (created)
            source->candidate = envelope.session;
        return queued;
    }
    if (envelope.request || envelope.session.empty() ||
        (topic == down_[0] ? (!envelope.event_sequence || envelope.message_sequence)
                           : (!envelope.message_sequence || envelope.event_sequence)))
        return SharedRouteResult::Rejected;
    const auto* bound = scope(envelope.session);
    if (bound && bound->retired)
        return SharedRouteResult::Rejected;
    return enqueue(topic, body, envelope.session,
                   bound ? std::optional<SharedMqttOwner>(bound->owner) : std::nullopt);
}
// take_message 记录原来源已经交付，不把被取出当状态机接受或设备执行成功。
std::optional<MqttEvent> SharedMqttRouter::take_message(SharedMqttOwner owner) {
    if (!valid_owner(owner))
        return std::nullopt;
    const auto found = std::find_if(queue_.begin(), queue_.end(), [&](const Queued& value) {
        return !value.reason && value.owner == owner;
    });
    if (found == queue_.end())
        return std::nullopt;
    Envelope envelope;
    if (found->message.topic == down_[2]) {
        ActionRequest action;
        if (decode_action_request(found->message.body, action) == DecodeResult::Ok) {
            if (auto* source = record(Source::Action, down_[2], "action", action.token))
                source->delivered = true;
        }
    } else if (decode_envelope(found->message.body, envelope) == DecodeResult::Ok) {
        if (envelope.method == "contextSnapshot" && found->message.topic == down_[1]) {
            if (auto* source = record(Source::Context, down_[1], envelope.method, envelope.token))
                source->delivered = true;
        } else if (envelope.method == "sessionInvite" || envelope.method == "focusControl") {
            if (auto* source =
                    record(envelope.method == "sessionInvite" ? Source::Invite : Source::Focus,
                           found->message.topic, envelope.method, envelope.token))
                source->delivered = true;
        } else if (envelope.code && !envelope.event_sequence && !envelope.message_sequence) {
            const auto& topic = found->message.topic == down_[0] ? up_[0] : up_[1];
            if (auto* source =
                    record(Source::Request, topic, response_method(envelope), envelope.token))
                source->delivered = true;
        }
    }
    auto message = std::move(found->message);
    queue_.erase(found);
    return message;
}
// confirm_session 由已实际接受的原业务调用；预算不足时不部分绑定或提前搬迁待确认事件。
SharedRouteResult SharedMqttRouter::confirm_session(SharedMqttOwner owner,
                                                    SharedSessionBinding binding,
                                                    std::uint64_t now) {
    const auto token = binding.origin_token;  // 原来源与新腿分别明确，不能借session字段猜token。
    const auto session = binding.session;     // 原业务确认的新独立腿，不由原200自动绑定。
    if (!tick(now))
        return closed_ ? SharedRouteResult::Closed : SharedRouteResult::Rejected;
    if (!valid_owner(owner) || owner == SharedMqttOwner::Global || session.empty())
        return SharedRouteResult::Rejected;
    auto* source = record(Source::Request, up_[0], "sessionCreate", token);
    if (!source || source->owner != owner)
        source = owner == SharedMqttOwner::Human
                     ? record(Source::Invite, down_[0], "sessionInvite", token)
                     : nullptr;
    if (!source || source->retired || !source->delivered || source->owner != owner ||
        source->candidate != session || now_ - source->started >= kRequest)
        return SharedRouteResult::Rejected;
    const auto* bound = scope(session);
    if (bound)
        return !bound->retired && bound->owner == owner ? SharedRouteResult::Routed
                                                        : SharedRouteResult::Rejected;
    const auto waiting = std::count_if(queue_.begin(), queue_.end(), [&](const Queued& value) {
        return !value.owner && !value.reason && value.session == session;
    });
    const auto owned = std::count_if(queue_.begin(), queue_.end(), [&](const Queued& value) {
        return value.owner == owner && !value.reason;
    });
    if (scopes_.size() >= kScopes || waiting + owned > static_cast<std::ptrdiff_t>(kPerOwner) ||
        retained_bytes() + session.size() > kBytes)
        return SharedRouteResult::Full;
    scopes_.push_back(Scope{owner, std::string(session), std::nullopt});
    for (auto& value : queue_) {
        if (!value.owner && !value.reason && value.session == session)
            value.owner = owner;
    }
    return SharedRouteResult::Routed;
}
// retire_session 幂等保持第一次终态时间，仅退休原业务原腿，不关闭另一业务或网络。
bool SharedMqttRouter::retire_session(SharedMqttOwner owner, std::string_view session,
                                      std::uint64_t now) {
    if (!tick(now))
        return false;
    auto* bound = scope(session);
    if (!bound || bound->owner != owner)
        return false;
    if (!bound->retired)
        bound->retired = now_;
    for (auto& value : records_) {
        if (value.owner == owner &&
            (value.envelope.session == session || value.candidate == session) && !value.retired)
            value.retired = now_;
        if (value.owner == owner && value.envelope.session == session)
            clear_private_record(value);
    }
    queue_.erase(std::remove_if(queue_.begin(), queue_.end(),
                                [&](const Queued& value) {
                                    return value.private_expires_at && value.session == session;
                                }),
                 queue_.end());
    for (auto& value : queue_) {
        if (value.session == session && !value.reason)
            value.reason = SharedUnroutableReason::Retired;
    }
    return true;
}
// take_unroutable 明确交回原帧与固定原因，等待未知不能伪装成业务终态。
std::optional<SharedUnroutable> SharedMqttRouter::take_unroutable() {
    const auto found = std::find_if(queue_.begin(), queue_.end(),
                                    [](const Queued& value) { return value.reason.has_value(); });
    if (found == queue_.end())
        return std::nullopt;
    const auto reason = found->reason;  // 在移动原帧前冻结独立原因，不依赖已移动对象的成员。
    if (!reason.has_value())
        return std::nullopt;
    SharedUnroutable value{std::move(found->message), *reason};
    queue_.erase(found);
    return value;
}
// close 是宿主实际断连时的本地分派终态，未消费帧仍可取回，不声明资源已经释放。
void SharedMqttRouter::close() {
    closed_ = true;
    queue_.erase(std::remove_if(queue_.begin(), queue_.end(),
                                [](const Queued& value) { return value.private_expires_at != 0; }),
                 queue_.end());
    for (auto& value : queue_)
        if (!value.reason)
            value.reason = SharedUnroutableReason::Closed;
    for (auto& value : records_) {
        clear_private_record(value);
        if (!value.retired)
            value.retired = now_;
    }
    for (auto& value : scopes_)
        if (!value.retired)
            value.retired = now_;
}
}  // namespace ur::device
