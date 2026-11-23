// 文件说明：历史录像严格报文解析；复用公共信封及播放命令，不扩展现行文件播放器权限。
#include "ur/device/json_parse.hpp"
#include "ur/device/record_playback_wire.hpp"
#include <cJSON.h>
#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <memory>
#include <utility>

namespace ur::device {
namespace {
// Json 保证非法报文出口释放解析树，公共信封已核验UTF-8、深度与重复键。
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
// keys 限定固定必填及可选字段，未知字段不悄然增加源或身份授权。
bool keys(const cJSON* object, std::initializer_list<std::string_view> required,
          std::initializer_list<std::string_view> optional = {}) {
    if (!cJSON_IsObject(object))
        return false;
    for (const auto key : required)
        if (!cJSON_GetObjectItemCaseSensitive(object, std::string(key).c_str()))
            return false;
    for (const auto* child = object->child; child; child = child->next) {
        const std::string_view key(child->string);
        if (std::find(required.begin(), required.end(), key) == required.end() &&
            std::find(optional.begin(), optional.end(), key) == optional.end())
            return false;
    }
    return true;
}
// text 读取非空有界标识，控制字符不能被后续驱动误解为路径或另一条协议命令。
bool text(const cJSON* object, const char* key, std::string& out, std::size_t limit) {
    const auto* value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsString(value))
        return false;
    out = value->valuestring;
    return !out.empty() && out.size() <= limit &&
           std::none_of(out.begin(), out.end(),
                        [](unsigned char ch) { return ch < 32 || ch == 127; });
}
// matches 固定用途、轨和编解码枚举，不从客户端值猜测实际能力。
bool matches(const cJSON* object, const char* key, std::string_view expected) {
    const auto* value = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(value) && std::string_view(value->valuestring) == expected;
}
// integer 保持跨语言安全精度；非正水位不能与合法初始代次混淆。
bool integer(const cJSON* object, const char* key, std::uint64_t& out, bool positive = false) {
    const auto* value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) ||
        value->valuedouble < (positive ? 1 : 0) || value->valuedouble > 9007199254740991.0 ||
        std::floor(value->valuedouble) != value->valuedouble)
        return false;
    out = static_cast<std::uint64_t>(value->valuedouble);
    return true;
}
// opaque_reference 只表达清单引用，URL/路径无法借历史invite变成任意文件或网络读取。
bool opaque_reference(const std::string& reference) {
    std::string lower = reference;  // 大小写变体不能把执行/文件URI伪装成清单引用。
    for (auto& ch : lower)
        if (ch >= 'A' && ch <= 'Z')
            ch = static_cast<char>(ch - 'A' + 'a');
    return reference.find_first_of("/\\ \t\r\n") == std::string::npos && reference != "." &&
           reference != ".." && reference.find("://") == std::string::npos &&
           lower.rfind("file:", 0) != 0 && lower.rfind("data:", 0) != 0 &&
           lower.rfind("javascript:", 0) != 0;
}
// track 只接受档案发布轨，不借用camera/mic/speaker源名称或sendrecv方向。
bool track(const cJSON* object, bool audio) {
    return keys(object, {"trackId", "kind", "sourceId", "direction", "codec", "state"}) &&
           matches(object, "trackId", audio ? "audio-1" : "video-1") &&
           matches(object, "kind", audio ? "audio" : "video") &&
           matches(object, "sourceId", audio ? "record-audio" : "record-video") &&
           matches(object, "direction", "sendonly") &&
           matches(object, "codec", audio ? "opus" : "H264") &&
           matches(object, "state", "requested");
}
// state_name 限定历史实际状态，播放意图或connected不能冒充播放器实际进展。
bool state_name(const std::string& state) {
    constexpr std::string_view allowed[] = {"loaded",  "playing", "paused", "seeking",
                                            "stopped", "ended",   "failed"};
    return std::find(std::begin(allowed), std::end(allowed), state) != std::end(allowed);
}
}  // namespace

WireResult decode_record_playback_invite(WireRoute route, std::string_view body,
                                         RecordPlaybackInvite& out) {
    out = {};
    if (route != WireRoute::Signaling)
        return WireResult::WrongRoute;
    RecordPlaybackInvite invite;
    if (decode_envelope(body, invite.envelope) != DecodeResult::Ok ||
        invite.envelope.method != "sessionInvite" || !invite.envelope.request ||
        invite.envelope.session.empty() || invite.envelope.session.size() > 64 ||
        !invite.envelope.event_sequence || invite.envelope.code || invite.envelope.message_sequence)
        return WireResult::Invalid;
    Json root(ur::device::parse_json(invite.envelope.payload.c_str()), cJSON_Delete);
    const auto* source = cJSON_GetObjectItemCaseSensitive(root.get(), "mediaSource");
    const auto* clip = cJSON_GetObjectItemCaseSensitive(root.get(), "clip");
    const auto* tracks = cJSON_GetObjectItemCaseSensitive(root.get(), "tracks");
    if (!keys(root.get(),
              {"purpose", "admission", "transport", "expiresAt", "cameraId", "recordingId",
               "recordingVersion", "playbackId", "mediaSource", "clip", "tracks"}) ||
        !matches(root.get(), "purpose", "recordPlayback") ||
        !matches(root.get(), "admission", "direct") ||
        !matches(root.get(), "transport", "webrtc") ||
        !integer(root.get(), "expiresAt", invite.expires_at_ms, true) ||
        !text(root.get(), "cameraId", invite.camera_id, 128) ||
        !text(root.get(), "recordingId", invite.recording_id, 64) ||
        !text(root.get(), "recordingVersion", invite.recording_version, 64) ||
        !text(root.get(), "playbackId", invite.playback_id, 128) || !keys(source, {"contentRef"}) ||
        !text(source, "contentRef", invite.content_ref, 512) ||
        !opaque_reference(invite.content_ref) || !keys(clip, {"startMs", "endMs"}) ||
        !integer(clip, "startMs", invite.clip.start_ms) ||
        !integer(clip, "endMs", invite.clip.end_ms) || invite.clip.end_ms <= invite.clip.start_ms ||
        !cJSON_IsArray(tracks) || cJSON_GetArraySize(tracks) < 1 ||
        cJSON_GetArraySize(tracks) > 2 || !track(cJSON_GetArrayItem(tracks, 0), false))
        return WireResult::Invalid;
    invite.audio = cJSON_GetArraySize(tracks) == 2;
    if (invite.audio && !track(cJSON_GetArrayItem(tracks, 1), true))
        return WireResult::Invalid;
    out = std::move(invite);
    return WireResult::Ok;
}

WireResult decode_record_playback_control(WireRoute route, std::string_view body,
                                          PlaybackCommand& out) {
    out = {};
    PlaybackCommand command;
    const auto result = decode_playback_control(route, body, command);
    if (result != WireResult::Ok)
        return result;
    if (!command.media_revision || command.envelope.session.size() > 64)
        return WireResult::Invalid;
    if (command.operation == "load" && !opaque_reference(command.content_ref))
        return WireResult::Invalid;
    if (command.operation == "setRate")
        return WireResult::Unsupported;
    out = std::move(command);
    return WireResult::Ok;
}

WireResult decode_record_playback_state(WireRoute route, std::string_view body,
                                        RecordPlaybackState& out) {
    out = {};
    if (route != WireRoute::Signaling)
        return WireResult::WrongRoute;
    RecordPlaybackState fact;
    if (decode_envelope(body, fact.envelope) != DecodeResult::Ok ||
        fact.envelope.method != "playbackState" || fact.envelope.request ||
        fact.envelope.session.empty() || fact.envelope.session.size() > 64 ||
        fact.envelope.event_sequence || fact.envelope.message_sequence)
        return WireResult::Invalid;
    Json root(ur::device::parse_json(fact.envelope.payload.c_str()), cJSON_Delete);
    if (fact.envelope.code && *fact.envelope.code != 200) {
        if (*fact.envelope.code < 400 || *fact.envelope.code > 599 ||
            !keys(root.get(), {"reason"}) || !text(root.get(), "reason", fact.reason, 64))
            return WireResult::Invalid;
        fact.kind = RecordPlaybackStateKind::Failure;
    } else if (cJSON_GetObjectItemCaseSensitive(root.get(), "released")) {
        if (fact.envelope.code || !keys(root.get(), {"playbackId", "released", "endEventSeq"}) ||
            !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root.get(), "released")) ||
            !text(root.get(), "playbackId", fact.playback_id, 128) ||
            !integer(root.get(), "endEventSeq", fact.end_event_sequence, true))
            return WireResult::Invalid;
        fact.kind = RecordPlaybackStateKind::Released;
    } else {
        if (!text(root.get(), "state", fact.state, 16) || !state_name(fact.state) ||
            !text(root.get(), "playbackId", fact.playback_id, 128) ||
            !integer(root.get(), "revision", fact.revision) ||
            !integer(root.get(), "mediaRevision", fact.media_revision, true))
            return WireResult::Invalid;
        if (fact.state == "failed") {
            if (fact.envelope.code ||
                !keys(root.get(), {"state", "revision", "playbackId", "reason", "mediaRevision"}) ||
                !text(root.get(), "reason", fact.reason, 64))
                return WireResult::Invalid;
        } else {
            if (!keys(root.get(),
                      {"state", "revision", "playbackId", "positionMs", "rate", "mediaRevision"},
                      {"durationMs"}) ||
                !integer(root.get(), "positionMs", fact.position_ms))
                return WireResult::Invalid;
            const auto* rate = cJSON_GetObjectItemCaseSensitive(root.get(), "rate");
            if (!cJSON_IsNumber(rate) || rate->valuedouble != 1)
                return WireResult::Invalid;
            if (cJSON_GetObjectItemCaseSensitive(root.get(), "durationMs")) {
                std::uint64_t duration = 0;  // 实测时长，不由档案查询范围或seek目标推断。
                if (!integer(root.get(), "durationMs", duration) || duration < fact.position_ms)
                    return WireResult::Invalid;
                fact.duration_ms = duration;
            }
        }
    }
    out = std::move(fact);
    return WireResult::Ok;
}
}  // namespace ur::device
