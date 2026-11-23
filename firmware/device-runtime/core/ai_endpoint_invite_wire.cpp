// 文件说明：AI候选邀请严格解析；复用音频选择解码，不触发旧创建生命周期或模型连接。
#include "ur/device/ai_endpoint_invite_wire.hpp"
#include <cJSON.h>
#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>
#include "ur/device/json_parse.hpp"

namespace ur::device {
namespace {
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;  // 失败出口释放短期媒体参数。
// keys 限定唯一字段集合；公共解析器已拒绝重复键和超深输入。
// 保留既有调用签名兼容原宿主；必需字段与允许字段的缺项、额外项检查分别保留。
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
bool keys(const cJSON* object, std::initializer_list<std::string_view> required,
          std::initializer_list<std::string_view> allowed) {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    if (!cJSON_IsObject(object))
        return false;
    for (const auto key : required)
        if (!cJSON_GetObjectItemCaseSensitive(object, std::string(key).c_str()))
            return false;
    for (const auto* item = object->child; item; item = item->next)
        if (!item->string ||
            std::find(allowed.begin(), allowed.end(), item->string) == allowed.end())
            return false;
    return true;
}
// text 拒绝空值、控制字符和资源超限，不返回原报文片段。
bool text(const cJSON* object, const char* key, std::string& out, std::size_t limit) {
    const auto* value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsString(value))
        return false;
    out = value->valuestring;
    return !out.empty() && out.size() <= limit &&
           std::none_of(out.begin(), out.end(),
                        [](unsigned char ch) { return ch < 32 || ch == 127; });
}
// deadline 只解析安全正整数；当前期限和原30秒事务由接听入口另核。
bool deadline(const cJSON* object, std::uint64_t& out) {
    const auto* value = cJSON_GetObjectItemCaseSensitive(object, "expiresAt");
    if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) || value->valuedouble < 1 ||
        value->valuedouble > 9007199254740991.0 ||
        std::floor(value->valuedouble) != value->valuedouble)
        return false;
    out = static_cast<std::uint64_t>(value->valuedouble);
    return true;
}
// media_host 限定实际udp4工厂支持的IPv4或DNS，不引入平台网络依赖。
bool media_host(std::string_view host) {
    std::size_t offset = 0, labels = 0;
    unsigned first = 0;
    bool ipv4 = true, all_zero = true;
    while (offset < host.size()) {
        const auto end = host.find('.', offset);
        const auto label = host.substr(
            offset, end == std::string_view::npos ? host.size() - offset : end - offset);
        if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-')
            return false;
        unsigned value = 0;
        bool numeric = label.size() == 1 || label.front() != '0';
        for (const auto ch : label) {
            if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                  (ch >= '0' && ch <= '9') || ch == '-'))
                return false;
            if (ch < '0' || ch > '9')
                numeric = false;
            if (numeric && value <= 255)
                value = value * 10 + static_cast<unsigned>(ch - '0');
        }
        if (labels == 0)
            first = value;
        ipv4 = ipv4 && numeric && value <= 255;
        all_zero = all_zero && value == 0;
        ++labels;
        if (end == std::string_view::npos)
            break;
        offset = end + 1;
        if (offset == host.size())
            return false;
    }
    return labels > 0 && !(ipv4 && labels == 4 && (all_zero || (first >= 224 && first <= 239)));
}
// udp_shape 核对新候选头模板和主机形态，不将参数校验冒充真实网络就绪。
bool udp_shape(const cJSON* media) {
    const auto* udp = cJSON_GetObjectItemCaseSensitive(media, "udp");
    std::string host, nonce;
    if (!keys(media, {"udp"}, {"udp"}) ||
        !keys(udp, {"server", "port", "key", "nonce"}, {"server", "port", "key", "nonce"}) ||
        !text(udp, "server", host, 253) || !text(udp, "nonce", nonce, 32) || nonce.size() != 32 ||
        !media_host(host) || nonce.substr(0, 8) != "01000000" || nonce.substr(24) != "00000000")
        return false;
    return true;
}
}  // namespace

// decode_ai_endpoint_invite 只产出held选择，不调用VoiceClient自动创建、采集或模型路径。
WireResult decode_ai_endpoint_invite(WireRoute route, std::string_view body,
                                     const VoiceCapability& capability, bool local_accept,
                                     AIEndpointInvite& out) {
    out = {};
    if (route != WireRoute::Signaling)
        return WireResult::WrongRoute;
    AIEndpointInvite invite;
    if (!local_accept || !capability.ai_endpoint_handoff || capability.ai_vision ||
        capability.transport == VoiceTransport::None ||
        decode_envelope(body, invite.envelope) != DecodeResult::Ok ||
        invite.envelope.method != "sessionInvite" || !invite.envelope.request ||
        invite.envelope.session.empty() || invite.envelope.session.size() > 64 ||
        invite.envelope.code || !invite.envelope.event_sequence || invite.envelope.message_sequence)
        return WireResult::Invalid;
    Json params(parse_json(invite.envelope.payload.c_str()), cJSON_Delete);
    std::string purpose, admission;
    if (!keys(params.get(), {"purpose", "admission", "expiresAt", "handoff", "selected", "tracks"},
              {"purpose", "admission", "expiresAt", "handoff", "selected", "tracks", "media"}) ||
        !text(params.get(), "purpose", purpose, 32) || purpose != "aiConversation" ||
        !text(params.get(), "admission", admission, 16) || admission != "accept" ||
        !deadline(params.get(), invite.expires_at))
        return WireResult::Invalid;
    const auto* handoff = cJSON_GetObjectItemCaseSensitive(params.get(), "handoff");
    const auto* selected = cJSON_GetObjectItemCaseSensitive(params.get(), "selected");
    const auto* tracks = cJSON_GetObjectItemCaseSensitive(params.get(), "tracks");
    const auto* media = cJSON_GetObjectItemCaseSensitive(params.get(), "media");
    if (!keys(handoff, {"sourceSessionId", "handoffId"}, {"sourceSessionId", "handoffId"}) ||
        !text(handoff, "sourceSessionId", invite.source_session, 64) ||
        !text(handoff, "handoffId", invite.handoff_id, 128) ||
        invite.source_session == invite.envelope.session ||
        !keys(selected, {"transport", "interactionMode", "audioParams"},
              {"transport", "interactionMode", "audioParams", "udpReplyStartReceipt"}) ||
        !keys(cJSON_GetObjectItemCaseSensitive(selected, "audioParams"),
              {"format", "sampleRate", "channels", "frameDuration"},
              {"format", "sampleRate", "channels", "frameDuration"}) ||
        !cJSON_IsArray(tracks) || cJSON_GetArraySize(tracks) != 2 || (media && !udp_shape(media)))
        return WireResult::Invalid;
    for (const auto* track = tracks->child; track; track = track->next) {
        std::string source, direction;
        if (!keys(track, {"trackId", "kind", "sourceId", "direction", "codec", "state"},
                  {"trackId", "kind", "sourceId", "direction", "codec", "state"}) ||
            !text(track, "sourceId", source, 128) || !text(track, "direction", direction, 16) ||
            !(((source == "mic" || source == "microphone") && direction == "sendonly") ||
              (source == "speaker" && direction == "recvonly")))
            return WireResult::Invalid;
    }
    // 合成选择仅交给无状态旧解码器，绝不投递客户端或请求账本。
    Json data(cJSON_CreateObject(), cJSON_Delete);
    if (!data || !cJSON_AddStringToObject(data.get(), "state", "negotiating") ||
        !cJSON_AddItemToObject(data.get(), "selected", cJSON_Duplicate(selected, true)) ||
        !cJSON_AddItemToObject(data.get(), "tracks", cJSON_Duplicate(tracks, true)) ||
        (media && !cJSON_AddItemToObject(data.get(), "media", cJSON_Duplicate(media, true))))
        return WireResult::Invalid;
    auto* serialized = cJSON_PrintUnformatted(data.get());
    if (!serialized)
        return WireResult::Invalid;
    Envelope created;
    created.token = invite.envelope.token;
    created.session = invite.envelope.session;
    created.method = "sessionCreated";
    created.request = false;
    created.code = 200;
    created.payload = serialized;
    cJSON_free(serialized);
    std::string encoded;
    VoiceEvent event;
    Envelope capability_check;
    if (create_voice_command("ai-invite-cap-check", capability, capability_check) !=
            WireResult::Ok ||
        encode_envelope(created, encoded) != DecodeResult::Ok ||
        decode_voice_event(WireRoute::Signaling, encoded, event) != WireResult::Ok)
        return WireResult::Invalid;
    const auto& chosen = event.media;
    if ((chosen.udp_reply_start_receipt && !capability.udp_reply_start_receipt) ||
        chosen.transport != capability.transport ||
        chosen.audio.sample_rate != capability.audio.sample_rate ||
        chosen.audio.channels != capability.audio.channels ||
        chosen.audio.frame_ms != capability.audio.frame_ms ||
        (chosen.mode == Mode::HalfDuplex && !capability.modes.half_duplex) ||
        (chosen.mode == Mode::FullDuplex &&
         (!capability.modes.full_duplex || !capability.modes.device_aec)))
        return WireResult::Invalid;
    invite.media = chosen;
    out = std::move(invite);
    return WireResult::Ok;
}
}  // namespace ur::device
