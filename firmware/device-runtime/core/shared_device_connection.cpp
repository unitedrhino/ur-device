// 文件说明：唯一连接握手、单一能力快照与原背压消息保留；不代替媒体、会话或资源授权状态机。
#include "ur/device/json_parse.hpp"
#include "ur/device/shared_device_connection.hpp"
#include <cJSON.h>
#include <algorithm>
#include <cmath>
#include <memory>
#include "ur/device/context_snapshot.hpp"

namespace ur::device {
namespace {
using Json = std::unique_ptr<cJSON,
                             decltype(&cJSON_Delete)>;   // 临时编码树，不保存网络凭据。
constexpr std::uint64_t max_safe = 9007199254740991ULL;  // JSON跨语言安全整数上界。
// read_revision只读取已严格归一化能力快照的非零安全整数，不用能力号冒充资源水位。
std::uint64_t read_revision(const Envelope& capability) {
    Json root(ur::device::parse_json(capability.payload.c_str()), cJSON_Delete);
    const auto* value = root ? cJSON_GetObjectItemCaseSensitive(root.get(), "revision") : nullptr;
    if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) || value->valuedouble < 1 ||
        value->valuedouble > static_cast<double>(max_safe) ||
        std::floor(value->valuedouble) != value->valuedouble)
        return 0;
    return static_cast<std::uint64_t>(value->valuedouble);
}
// receipt_capability只从本机构造的统一快照读取AI/UDP/feature交集，不接受Human或网络正文补授能力。
bool receipt_capability(const Envelope& capability) {
    Json root(ur::device::parse_json(capability.payload.c_str()), cJSON_Delete);
    if (!root || capability.method != "capabilityReport" || !capability.request ||
        capability.code || !capability.session.empty())
        return false;
    const auto contains = [&](const char* name, const char* expected) {
        const auto* array = cJSON_GetObjectItemCaseSensitive(root.get(), name);
        if (!cJSON_IsArray(array))
            return false;
        const cJSON* value = nullptr;
        cJSON_ArrayForEach (value, array)
            if (cJSON_IsString(value) && std::string_view(value->valuestring) == expected)
                return true;
        return false;
    };
    return contains("purposes", "aiConversation") && contains("transports", "udp") &&
           contains("features", "udpReplyStartReceipt");
}
}  // namespace

WireResult handoff_capability_command(std::string token, const VoiceCapability& voice,
                                      const FocusDriver& driver, Envelope& out, bool human_only,
                                      bool human_endpoint_handoff) {
    out = {};
    // 当前公共audio只有一组格式；两用途必须真实共同支持48k，而不是上报16k后让人工偷换格式。
    if (voice.transport != VoiceTransport::Udp || voice.audio.sample_rate != 48000 ||
        voice.audio.channels != 1 || voice.audio.frame_ms != 20 || voice.file_audio_output ||
        voice.file_video_output || voice.live_audio_receive || voice.live_video_receive ||
        voice.client_display_theme || (human_only && voice.text_input))
        return WireResult::Invalid;
    const auto idle = driver.initial_idle_observation();
    if (!idle || idle->revision != driver.revision() || idle->revision > max_safe ||
        idle->resources.size() != 2 ||
        std::find(idle->resources.begin(), idle->resources.end(), "microphone") ==
            idle->resources.end() ||
        std::find(idle->resources.begin(), idle->resources.end(), "speaker") ==
            idle->resources.end() ||
        capability_command(std::move(token), voice, out) != WireResult::Ok)
        return WireResult::Invalid;
    Json root(ur::device::parse_json(out.payload.c_str()), cJSON_Delete);
    auto* purposes = root ? cJSON_GetObjectItemCaseSensitive(root.get(), "purposes") : nullptr;
    auto* transports = root ? cJSON_GetObjectItemCaseSensitive(root.get(), "transports") : nullptr;
    auto* features = root ? cJSON_GetObjectItemCaseSensitive(root.get(), "features") : nullptr;
    auto* controls = root ? cJSON_AddObjectToObject(root.get(), "controlCapabilities") : nullptr;
    auto* operations = controls ? cJSON_AddArrayToObject(controls, "operations") : nullptr;
    auto* resources = controls ? cJSON_AddArrayToObject(controls, "focusResources") : nullptr;
    auto* baseline = controls ? cJSON_AddObjectToObject(controls, "baseline") : nullptr;
    auto* baseline_resources = baseline ? cJSON_AddArrayToObject(baseline, "resources") : nullptr;
    if (!purposes || !transports || !features || !operations || !resources || !baseline_resources ||
        !cJSON_AddItemToArray(purposes, cJSON_CreateString("humanCall")) ||
        !cJSON_AddItemToArray(transports, cJSON_CreateString("webrtc")) ||
        !cJSON_AddItemToArray(features, cJSON_CreateString("localAccept")) ||
        (human_endpoint_handoff && !cJSON_AddItemToArray(features, cJSON_CreateString("humanEndpointHandoff"))) ||
        !cJSON_AddItemToArray(operations, cJSON_CreateString("focusControl")) ||
        !cJSON_AddStringToObject(baseline, "state", "idle") ||
        !cJSON_AddNumberToObject(baseline, "revision", static_cast<double>(idle->revision))) {
        out = {};
        return WireResult::Invalid;
    }
    if (human_only) {
        // 纯受话端没有AI/sendText入口；语音由audio声明，原私有快照只需要被动文字输出。
        cJSON_DeleteItemFromArray(purposes, 0);
        cJSON_DeleteItemFromArray(transports, 0);
        cJSON_DeleteItemFromObjectCaseSensitive(root.get(), "aecMode");
        cJSON_DeleteItemFromObjectCaseSensitive(root.get(), "supportedInteractionModes");
        auto* inputs = cJSON_AddArrayToObject(root.get(), "inputModalities");
        auto* outputs = cJSON_AddArrayToObject(root.get(), "outputModalities");
        if (!inputs || !outputs || !cJSON_AddItemToArray(outputs, cJSON_CreateString("audio")) ||
            !cJSON_AddItemToArray(outputs, cJSON_CreateString("text"))) {
            out = {};
            return WireResult::Invalid;
        }
    }
    for (const auto* resource : {"microphone", "speaker"}) {
        if (!cJSON_AddItemToArray(resources, cJSON_CreateString(resource)) ||
            !cJSON_AddItemToArray(baseline_resources, cJSON_CreateString(resource))) {
            out = {};
            return WireResult::Invalid;
        }
    }
    auto* bytes = cJSON_PrintUnformatted(root.get());
    if (!bytes) {
        out = {};
        return WireResult::Invalid;
    }
    out.payload = bytes;
    cJSON_free(bytes);
    std::string encoded;
    if (encode_envelope(out, encoded) != DecodeResult::Ok ||
        decode_envelope(encoded, out) != DecodeResult::Ok) {
        out = {};
        return WireResult::Invalid;
    }
    return WireResult::Ok;
}

SharedDeviceConnection::SharedDeviceConnection(MqttPort& network, const std::string& identity,
                                               Envelope capability, std::uint64_t deadline)
    : network_(network),
      router_(network, identity, receipt_capability(capability)),
      requests_(router_.port(SharedMqttOwner::Global)),
      capability_(std::move(capability)),
      up_("$thing/up/signaling/" + identity),
      down_("$thing/down/signaling/" + identity),
      topics_{{down_, "$thing/down/message/" + identity, "$thing/down/action/" + identity}},
      deadline_(deadline) {
    Envelope canonical;
    valid_ = !router_.closed() && deadline_ != 0 && capability_.request &&
             capability_.method == "capabilityReport" && capability_.session.empty() &&
             !capability_.code && !capability_.event_sequence && !capability_.message_sequence &&
             encode_envelope(capability_, capability_bytes_) == DecodeResult::Ok &&
             decode_envelope(capability_bytes_, canonical) == DecodeResult::Ok;
    if (valid_) {
        capability_ = std::move(canonical);
        revision_ = read_revision(capability_);
        valid_ = revision_ != 0;
    }
}
SharedDeviceConnection::~SharedDeviceConnection() { close(); }

bool SharedDeviceConnection::observe(std::uint64_t now) {
    if ((clock_ && now < *clock_) || now >= deadline_ || !router_.tick(now)) {
        fail();
        return false;
    }
    clock_ = now;
    return true;
}
bool SharedDeviceConnection::start(const MqttConfig& config, std::uint64_t now) {
    if (state_ != SharedConnectionState::Idle)
        return false;
    if (!valid_ || !observe(now)) {
        fail();
        return false;
    }
    state_ = SharedConnectionState::Connecting;
    phase_started_ = now;
    // 驱动connect失败也可能留部分资源；唯一拥有者负责精确disconnect一次。
    network_started_ = true;
    if (!network_.connect(config)) {
        fail();
        return false;
    }
    return true;
}
bool SharedDeviceConnection::report(std::uint64_t now) {
    const auto result =
        requests_.submit({capability_.token, {}, "capabilityReport", up_, capability_bytes_}, now);
    if (result != SubmitResult::Accepted)
        return false;
    state_ = SharedConnectionState::Reporting;
    phase_started_ = now;
    return true;
}
bool SharedDeviceConnection::consume(const MqttEvent& event, std::uint64_t now) {
    if (event.kind == MqttEvent::Kind::Disconnected || event.kind == MqttEvent::Kind::Failed)
        return false;
    if (event.kind == MqttEvent::Kind::Connected) {
        if (state_ != SharedConnectionState::Connecting || event.code != 0)
            return false;
        for (std::size_t i = 0; i < topics_.size(); ++i) {
            subscriptions_[i] = network_.subscribe(topics_[i]);
            if (subscriptions_[i] <= 0 ||
                std::find(subscriptions_.begin(), subscriptions_.begin() + i, subscriptions_[i]) !=
                    subscriptions_.begin() + i)
                return false;
        }
        state_ = SharedConnectionState::Subscribing;
        return true;
    }
    if (event.kind == MqttEvent::Kind::Subscribed) {
        const auto position =
            std::find(subscriptions_.begin(), subscriptions_.end(), event.operation);
        if (event.operation <= 0 || position == subscriptions_.end())
            return true;
        const auto index = static_cast<std::size_t>(position - subscriptions_.begin());
        if (event.code != 0)
            return false;
        if (subscribed_[index])
            return true;
        if (state_ != SharedConnectionState::Subscribing)
            return false;
        subscribed_[index] = true;
        return !std::all_of(subscribed_.begin(), subscribed_.end(), [](bool value) {
            return value;
        }) || report(now);
    }
    if (event.kind != MqttEvent::Kind::Message)
        return true;  // PUBACK不代表业务能力确认。
    // 精确SIG的消息可能早于另两条SUBACK；保留原邀请，但take_message仍等统一能力成功。
    if (state_ != SharedConnectionState::Subscribing &&
        state_ != SharedConnectionState::Reporting && state_ != SharedConnectionState::Ready)
        return true;
    VoiceEvent ack;
    if (state_ != SharedConnectionState::Subscribing && event.topic == down_ &&
        decode_voice_event(WireRoute::Signaling, event.body, ack) == WireResult::Ok &&
        ack.envelope.token == capability_.token && ack.request_method == "capabilityReport") {
        if (!ack.envelope.code || *ack.envelope.code != 200 || ack.revision != revision_)
            return false;
        if (state_ == SharedConnectionState::Ready)
            return true;  // 原同快照QoS重复不再启业务。
        if (!requests_.acknowledge({capability_.token, {}, "capabilityReport", 200, up_}, now))
            return false;
        state_ = SharedConnectionState::Ready;
        return true;
    }
    const auto routed = router_.ingest(event.topic, event.body, now);
    if (routed == SharedRouteResult::Full) {
        if (!retained_) {
            retained_ = event;
            retained_at_ = now;
        }
    }
    return routed != SharedRouteResult::Closed;
}
// 保留 now/wall_ms 双时钟入口；now 管原连接等待，wall_ms 管私有内容期限，各自保留原回退检查。
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
void SharedDeviceConnection::poll(std::uint64_t now, std::uint64_t wall_ms) {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    router_.expire_private_context(wall_ms);
    // 唯一连接背压时也可能持有未登记的私有原帧；校时失效/原截止后不再重试或交业务。
    if (retained_) {
        ContextSnapshot snapshot;  // 只识别严格私有消息，不静默丢普通业务背压帧。
        if (decode_context_snapshot(WireRoute::Message, retained_->body, snapshot) ==
                WireResult::Ok &&
            (!wall_ms || wall_ms < router_wall_highwater_ || wall_ms >= snapshot.expires_at))
            retained_.reset();
    }
    router_wall_highwater_ = std::max(router_wall_highwater_, wall_ms);
    if (state_ == SharedConnectionState::Idle || state_ == SharedConnectionState::Closed ||
        state_ == SharedConnectionState::Failed)
        return;
    if (!observe(now))
        return;
    if ((state_ == SharedConnectionState::Connecting ||
         state_ == SharedConnectionState::Subscribing) &&
        now - phase_started_ >= 10000) {
        fail();
        return;
    }
    requests_.tick(now);
    if (state_ == SharedConnectionState::Reporting &&
        requests_.state({up_, "capabilityReport", capability_.token}) != RequestState::Pending) {
        fail();
        return;
    }
    if (!network_.poll()) {
        fail();
        return;
    }
    if (retained_) {
        if (now - retained_at_ >= 5000) {
            fail();
            return;
        }
        const auto result = router_.ingest(retained_->topic, retained_->body, now);
        if (result == SharedRouteResult::Full)
            return;
        if (result == SharedRouteResult::Closed) {
            fail();
            return;
        }
        retained_.reset();
    }
    for (unsigned count = 0; count < 32 && !retained_; ++count) {
        const auto event = network_.take_event();
        if (!event)
            break;
        if (!consume(*event, now)) {
            fail();
            return;
        }
    }
}
std::optional<MqttEvent> SharedDeviceConnection::take_message(SharedMqttOwner owner) {
    return state_ == SharedConnectionState::Ready ? router_.take_message(owner) : std::nullopt;
}
void SharedDeviceConnection::fail() {
    state_ = SharedConnectionState::Failed;
    router_.close();
    retained_.reset();
    if (network_started_ && !disconnected_) {
        disconnected_ = true;
        network_.disconnect();
    }
}
void SharedDeviceConnection::close() {
    router_.close();
    retained_.reset();
    if (network_started_ && !disconnected_) {
        disconnected_ = true;
        network_.disconnect();
    }
    if (state_ != SharedConnectionState::Failed)
        state_ = SharedConnectionState::Closed;
}
}  // namespace ur::device
