// 文件说明：实时节目recvonly契约解析及能力编码；复用公共JSON安全边界，不创建媒体或扩大设备权限。
#include "ur/device/json_parse.hpp"
#include "ur/device/live_wire.hpp"
#include <cJSON.h>
#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <memory>
#include <utility>

namespace ur::device {
namespace {
// Json 在所有非法报文出口释放树；UTF-8、重复键、深度及整帧预算由公共信封校验。
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
// keys 要求全部指定字段且拒绝未知字段，禁止携带URL、摄像头或用户身份扩大授权。
bool keys(const cJSON* object, std::initializer_list<std::string_view> expected) {
    if (!cJSON_IsObject(object))
        return false;
    for (const auto key : expected)
        if (!cJSON_GetObjectItemCaseSensitive(object, std::string(key).c_str()))
            return false;
    for (const auto* child = object->child; child; child = child->next)
        if (std::find(expected.begin(), expected.end(), std::string_view(child->string)) ==
            expected.end())
            return false;
    return true;
}
// matches 限定已提交跨端枚举，不把本地文件或实时采集源误认为节目接收源。
bool matches(const cJSON* object, const char* key, std::string_view expected) {
    const auto* value = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(value) && std::string_view(value->valuestring) == expected;
}
// text 读取有界非空标识，控制字符不能混入原操作或稳定失败原因。
bool text(const cJSON* object, const char* key, std::string& out) {
    const auto* value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsString(value))
        return false;
    out = value->valuestring;
    return !out.empty() && out.size() <= 128 &&
           std::none_of(out.begin(), out.end(),
                        [](unsigned char ch) { return ch < 32 || ch == 127; });
}
// positive 保持跨语言安全正整数精度，零或缺省不能代表合法媒体代次及截止。
bool positive(const cJSON* object, const char* key, std::uint64_t& out) {
    const auto* value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) || value->valuedouble < 1 ||
        value->valuedouble > 9007199254740991.0 ||
        std::floor(value->valuedouble) != value->valuedouble)
        return false;
    out = static_cast<std::uint64_t>(value->valuedouble);
    return true;
}
// track 核对固定六字段接收轨；方向、来源、编解码和requested不能由客户端替换。
bool track(const cJSON* object, bool audio) {
    return keys(object, {"trackId", "kind", "sourceId", "direction", "codec", "state"}) &&
           matches(object, "trackId", audio ? "broadcast-out" : "broadcast-video-out") &&
           matches(object, "kind", audio ? "audio" : "video") &&
           matches(object, "sourceId", audio ? "broadcast-audio" : "broadcast-video") &&
           matches(object, "direction", "recvonly") &&
           matches(object, "codec", audio ? "opus" : "H264") &&
           matches(object, "state", "requested");
}
// request 校验同一独立SIG会话的下行请求信封，内容事件与失败应答不得混入。
bool request(const Envelope& envelope, std::string_view method) {
    return envelope.method == method && envelope.request && !envelope.session.empty() &&
           envelope.session.size() <= 64 && envelope.event_sequence && !envelope.code &&
           !envelope.message_sequence;
}
}  // namespace

WireResult live_capability_command(std::string token, const LiveReceiveCapability& capability,
                                   Envelope& out) {
    out = {};
    if ((!capability.audio && !capability.video) || capability.revision == 0 ||
        capability.revision > 9007199254740991ULL)
        return WireResult::Invalid;
    Envelope command;
    command.token = std::move(token);
    command.method = "capabilityReport";
    command.payload =
        R"({"revision":)" + std::to_string(capability.revision) +
        R"(,"purposes":["liveBroadcast"],"transports":["webrtc"],"supportedInteractionModes":[],"aecMode":"","features":[],"audio":{"format":"","sampleRate":0,"channels":0,"frameDuration":0},"liveReceive":{)";
    if (capability.audio)
        command.payload +=
            R"("audio":{"format":"opus","sampleRate":48000,"channels":1,"frameDuration":20})";
    if (capability.audio && capability.video)
        command.payload += ',';
    if (capability.video)
        command.payload += R"("videoCodecs":["H264"])";
    command.payload += "}}";
    std::string body;
    if (encode_envelope(command, body) != DecodeResult::Ok ||
        decode_envelope(body, out) != DecodeResult::Ok)
        return WireResult::Invalid;
    return WireResult::Ok;
}

WireResult decode_live_invite(WireRoute route, std::string_view body, LiveInvite& out) {
    out = {};
    if (route != WireRoute::Signaling)
        return WireResult::WrongRoute;
    LiveInvite invite;
    if (decode_envelope(body, invite.envelope) != DecodeResult::Ok ||
        !request(invite.envelope, "sessionInvite"))
        return WireResult::Invalid;
    Json root(ur::device::parse_json(invite.envelope.payload.c_str()), cJSON_Delete);
    const auto* tracks = cJSON_GetObjectItemCaseSensitive(root.get(), "tracks");
    if (!keys(root.get(), {"purpose", "admission", "transport", "expiresAt", "tracks"}) ||
        !matches(root.get(), "purpose", "liveBroadcast") ||
        !matches(root.get(), "admission", "direct") ||
        !matches(root.get(), "transport", "webrtc") ||
        !positive(root.get(), "expiresAt", invite.expires_at_ms) || !cJSON_IsArray(tracks))
        return WireResult::Invalid;
    const auto count = cJSON_GetArraySize(tracks);
    if (count < 1 || count > 2)
        return WireResult::Invalid;
    invite.audio = track(cJSON_GetArrayItem(tracks, 0), true);
    invite.video = count == 1 ? track(cJSON_GetArrayItem(tracks, 0), false)
                              : track(cJSON_GetArrayItem(tracks, 1), false);
    if ((!invite.audio && !invite.video) || (count == 2 && (!invite.audio || !invite.video)))
        return WireResult::Invalid;
    out = std::move(invite);
    return WireResult::Ok;
}

bool live_has_track(const LiveInvite& invite, std::string_view track_id) {
    return (invite.audio && track_id == "broadcast-out") ||
           (invite.video && track_id == "broadcast-video-out");
}

WireResult decode_live_track_control(WireRoute route, std::string_view body,
                                     const LiveInvite& invite, LiveTrackControl& out) {
    out = {};
    if (route != WireRoute::Signaling)
        return WireResult::WrongRoute;
    LiveTrackControl command;
    if (decode_envelope(body, command.envelope) != DecodeResult::Ok ||
        !request(command.envelope, "trackControl") ||
        command.envelope.session != invite.envelope.session)
        return WireResult::Invalid;
    Json root(ur::device::parse_json(command.envelope.payload.c_str()), cJSON_Delete);
    if (!keys(root.get(), {"trackId", "operation", "mediaRevision"}) ||
        !text(root.get(), "trackId", command.track_id) ||
        !live_has_track(invite, command.track_id) ||
        !text(root.get(), "operation", command.operation) ||
        (command.operation != "pause" && command.operation != "resume" &&
         command.operation != "stop") ||
        !positive(root.get(), "mediaRevision", command.media_revision))
        return WireResult::Invalid;
    out = std::move(command);
    return WireResult::Ok;
}

WireResult decode_live_track_state(WireRoute route, std::string_view body, LiveTrackState& out) {
    out = {};
    if (route != WireRoute::Signaling)
        return WireResult::WrongRoute;
    LiveTrackState fact;
    if (decode_envelope(body, fact.envelope) != DecodeResult::Ok ||
        fact.envelope.method != "trackState" || fact.envelope.request ||
        fact.envelope.session.empty() || fact.envelope.session.size() > 64 || !fact.envelope.code ||
        fact.envelope.event_sequence || fact.envelope.message_sequence)
        return WireResult::Invalid;
    Json root(ur::device::parse_json(fact.envelope.payload.c_str()), cJSON_Delete);
    if (*fact.envelope.code == 200) {
        if (!keys(root.get(), {"trackId", "state", "mediaRevision"}) ||
            !text(root.get(), "trackId", fact.track_id) || !text(root.get(), "state", fact.state) ||
            (fact.state != "active" && fact.state != "paused" && fact.state != "stopped") ||
            !positive(root.get(), "mediaRevision", fact.media_revision))
            return WireResult::Invalid;
    } else if (*fact.envelope.code < 400 || *fact.envelope.code > 599 ||
               !keys(root.get(), {"reason"}) || !text(root.get(), "reason", fact.reason)) {
        return WireResult::Invalid;
    }
    out = std::move(fact);
    return WireResult::Ok;
}
}  // namespace ur::device
