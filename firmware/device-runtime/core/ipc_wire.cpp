// IPC邀请编码与严格解析；不隐式开启扬声器、对讲或AI取帧。
#include "ur/device/ipc_wire.hpp"
#include "ur/device/json_parse.hpp"

#include <cJSON.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <memory>

namespace ur::device {
namespace {
// Json 统一管理解析树所有权，所有失败出口都释放资源。
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
// valid_id 与公共信封相同，拒绝空值、越界和控制字符。
bool valid_id(const std::string& value) {
    return !value.empty() && value.size() <= 128 &&
           std::none_of(value.begin(), value.end(),
                        [](unsigned char ch) { return ch < 32 || ch == 127; });
}
// valid_capability 限定当前摄像头发布预算，不从平台请求扩大能力。
bool valid_capability(const IPCCapability& capability) {
    return valid_id(capability.camera_id) && capability.revision > 0 &&
           capability.revision <= 9007199254740991ULL && capability.max_publishers >= 1 &&
           capability.max_publishers <= 4 &&
           (!capability.participant_roster || capability.conference_talk ||
            capability.human_call) &&
           (!capability.participant_roster_sync ||
            (capability.participant_roster && capability.conference_talk)) &&
           (!capability.conference_talk || capability.microphone) &&
           (!capability.receive_video || capability.human_call) &&
           (!capability.human_call ||
            (capability.microphone && capability.conference_talk && capability.local_accept));
}
// keys 禁止其它用途字段混入IPC；重复键由公共信封解析器预先拒绝。
bool keys(const cJSON* object, std::initializer_list<const char*> allowed) {
    if (!cJSON_IsObject(object))
        return false;
    for (const auto* field = object->child; field; field = field->next)
        if (std::none_of(allowed.begin(), allowed.end(),
                         [&](const char* key) { return std::strcmp(field->string, key) == 0; }))
            return false;
    return true;
}
// text 读取有界标识，不允许数字或null冒充字符串。
bool text(const cJSON* object, const char* key, std::string& value) {
    const auto* item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsString(item))
        return false;
    value = item->valuestring;
    return valid_id(value);
}
// literal
// 对必填字符串精确比较，null、数字或省略不能变成固定语义；供媒体投影解析复用。
bool literal(const cJSON* object, const char* key, std::string_view expected) {
    const auto* item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(item) && std::string_view(item->valuestring) == expected;
}
// exact 完整对象必须包含全部且仅这些键；媒体修订投影不接受任何附加私有字段。
bool exact(const cJSON* object, std::initializer_list<std::string_view> fields) {
    if (!cJSON_IsObject(object))
        return false;
    std::size_t count = 0;
    for (auto* item = object->child; item; item = item->next) {
        if (!item->string ||
            std::find(fields.begin(), fields.end(), std::string_view(item->string)) == fields.end())
            return false;
        ++count;
    }
    return count == fields.size();
}
// number 只读取精确JSON安全整数；可选zero允许0作为合法计划修订占位。
bool exact_number(const cJSON* object, const char* key, std::uint64_t& value, bool zero = false) {
    const auto* item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble) ||
        item->valuedouble < (zero ? 0 : 1) || item->valuedouble > 9007199254740991.0 ||
        std::floor(item->valuedouble) != item->valuedouble)
        return false;
    value = static_cast<std::uint64_t>(item->valuedouble);
    return true;
}
}  // namespace

WireResult decode_ipc_track(WireRoute route, std::string_view body, const IPCInvite& invite,
                            IPCTrackCommand& out, std::uint64_t media_revision) {
    out = {};
    if (route != WireRoute::Signaling)
        return WireResult::WrongRoute;
    IPCTrackCommand command;
    if (decode_envelope(body, command.envelope) != DecodeResult::Ok ||
        command.envelope.method != "trackControl" || !command.envelope.request ||
        command.envelope.session != invite.envelope.session ||
        !valid_id(command.envelope.session) || !command.envelope.event_sequence ||
        *command.envelope.event_sequence == 0 || command.envelope.message_sequence ||
        command.envelope.code)
        return WireResult::Invalid;
    Json params(ur::device::parse_json(command.envelope.payload.c_str()), cJSON_Delete);
    if (!params || !keys(params.get(), {"trackId", "operation", "mediaRevision", "reason"}) ||
        !text(params.get(), "trackId", command.track_id) ||
        !text(params.get(), "operation", command.operation))
        return WireResult::Invalid;
    // 接收扬声器只允许收紧或恢复已有播放，不从控制报文新建接收轨或改变采集权限。
    const bool speaker = !invite.speaker_track.empty() && command.track_id == invite.speaker_track;
    if (speaker ? (command.operation != "pause" && command.operation != "resume")
                : (command.track_id != invite.video_track &&
                   (invite.audio_track.empty() || command.track_id != invite.audio_track)))
        return WireResult::Invalid;
    if (media_revision == 0 || media_revision > 9007199254740991ULL ||
        !exact_number(params.get(), "mediaRevision", command.revision) ||
        command.revision != media_revision)
        return WireResult::Invalid;
    const auto& operation = command.operation;
    if (operation != "publish" && operation != "subscribe" && operation != "pause" &&
        operation != "resume" && operation != "stop" && operation != "unsubscribe")
        return WireResult::Invalid;
    if (cJSON_GetObjectItemCaseSensitive(params.get(), "reason") &&
        !text(params.get(), "reason", command.reason))
        return WireResult::Invalid;
    out = std::move(command);
    return WireResult::Ok;
}

WireResult ipc_capability_command(std::string token, const IPCCapability& capability,
                                  Envelope& out) {
    out = {};
    if (!valid_capability(capability))
        return WireResult::Invalid;
    Json root(cJSON_CreateObject(), cJSON_Delete);
    if (!root)
        return WireResult::Invalid;
    auto* purposes = cJSON_AddArrayToObject(root.get(), "purposes");
    auto* transports = cJSON_AddArrayToObject(root.get(), "transports");
    auto* features = cJSON_AddArrayToObject(root.get(), "features");
    auto* cameras = cJSON_AddArrayToObject(root.get(), "cameras");
    auto* camera = cameras ? cJSON_CreateObject() : nullptr;
    if (camera && !cJSON_AddItemToArray(cameras, camera)) {
        cJSON_Delete(camera);
        camera = nullptr;
    }
    if (!purposes || !transports || !features || !camera ||
        !cJSON_AddNumberToObject(root.get(), "revision",
                                 static_cast<double>(capability.revision)) ||
        !cJSON_AddItemToArray(purposes, cJSON_CreateString("ipcLiveView")) ||
        (capability.conference_talk &&
         !cJSON_AddItemToArray(purposes, cJSON_CreateString("ipcTalk"))) ||
        (capability.human_call &&
         !cJSON_AddItemToArray(purposes, cJSON_CreateString("humanCall"))) ||
        !cJSON_AddItemToArray(transports, cJSON_CreateString("webrtc")) ||
        !cJSON_AddItemToArray(features, cJSON_CreateString("video")) ||
        (capability.receive_video &&
         !cJSON_AddItemToArray(features, cJSON_CreateString("videoReceive"))) ||
        (capability.participant_roster &&
         !cJSON_AddItemToArray(features, cJSON_CreateString("participantRoster"))) ||
        (capability.participant_roster_sync &&
         !cJSON_AddItemToArray(features, cJSON_CreateString("participantRosterSync"))) ||
        !cJSON_AddStringToObject(camera, "cameraId", capability.camera_id.c_str()) ||
        !cJSON_AddNumberToObject(camera, "maxPublishers", capability.max_publishers))
        return WireResult::Invalid;
    auto* codecs = cJSON_AddArrayToObject(camera, "codecs");
    if (!codecs || !cJSON_AddItemToArray(codecs, cJSON_CreateString("H264")) ||
        (capability.local_accept &&
         !cJSON_AddItemToArray(features, cJSON_CreateString("localAccept"))))
        return WireResult::Invalid;
    if (capability.microphone) {
        auto* audio = cJSON_AddObjectToObject(root.get(), "audio");
        if (!audio || !cJSON_AddItemToArray(features, cJSON_CreateString("audio")) ||
            !cJSON_AddStringToObject(audio, "format", "opus") ||
            !cJSON_AddNumberToObject(audio, "sampleRate", 48000) ||
            !cJSON_AddNumberToObject(audio, "channels", 1) ||
            !cJSON_AddNumberToObject(audio, "frameDuration", 20))
            return WireResult::Invalid;
    }
    char* payload = cJSON_PrintUnformatted(root.get());
    if (!payload)
        return WireResult::Invalid;
    Envelope command;
    command.token = std::move(token);
    command.method = "capabilityReport";
    command.payload = payload;
    cJSON_free(payload);
    std::string encoded;
    if (encode_envelope(command, encoded) != DecodeResult::Ok ||
        decode_envelope(encoded, out) != DecodeResult::Ok)
        return WireResult::Invalid;
    return WireResult::Ok;
}

WireResult decode_ipc_invite(WireRoute route, std::string_view body,
                             const IPCCapability& capability, IPCInvite& out) {
    out = {};
    if (route != WireRoute::Signaling)
        return WireResult::WrongRoute;
    IPCInvite invite;
    if (!valid_capability(capability) || decode_envelope(body, invite.envelope) != DecodeResult::Ok)
        return WireResult::Invalid;
    const auto& envelope = invite.envelope;
    if (envelope.method != "sessionInvite" || envelope.session.empty() || !envelope.request ||
        envelope.code || !envelope.event_sequence || envelope.message_sequence)
        return WireResult::Invalid;
    Json root(ur::device::parse_json(envelope.payload.c_str()), cJSON_Delete);
    std::string purpose, admission, transport;
    if (!keys(root.get(),
              {"purpose", "admission", "cameraId", "transport", "expiresAt", "tracks"}) ||
        !text(root.get(), "purpose", purpose) || purpose != "ipcLiveView" ||
        !text(root.get(), "admission", admission) ||
        (admission != "direct" && admission != "accept") ||
        !text(root.get(), "cameraId", invite.camera_id) ||
        invite.camera_id != capability.camera_id || !text(root.get(), "transport", transport) ||
        transport != "webrtc" || (admission == "accept" && !capability.local_accept))
        return WireResult::Invalid;
    const auto* expiry = cJSON_GetObjectItemCaseSensitive(root.get(), "expiresAt");
    const auto* tracks = cJSON_GetObjectItemCaseSensitive(root.get(), "tracks");
    if (!cJSON_IsNumber(expiry) || !std::isfinite(expiry->valuedouble) || expiry->valuedouble < 1 ||
        expiry->valuedouble > 9007199254740991.0 ||
        std::floor(expiry->valuedouble) != expiry->valuedouble || !cJSON_IsArray(tracks) ||
        cJSON_GetArraySize(tracks) < 1 || cJSON_GetArraySize(tracks) > 2)
        return WireResult::Invalid;
    invite.expires_at_ms = static_cast<std::uint64_t>(expiry->valuedouble);
    invite.needs_accept = admission == "accept";
    for (const auto* track = tracks->child; track; track = track->next) {
        std::string id, kind, source, direction, codec, state;
        if (!keys(track, {"trackId", "kind", "sourceId", "direction", "codec", "state"}) ||
            !text(track, "trackId", id) || !text(track, "kind", kind) ||
            !text(track, "sourceId", source) || !text(track, "direction", direction) ||
            !text(track, "codec", codec) || !text(track, "state", state) ||
            direction != "sendonly" || state != "requested" || id == invite.video_track ||
            id == invite.audio_track)
            return WireResult::Invalid;
        if (kind == "video" && source == invite.camera_id && codec == "H264" &&
            invite.video_track.empty())
            invite.video_track = id;
        else if (kind == "audio" && source == "mic" && codec == "opus" && capability.microphone &&
                 invite.audio_track.empty())
            invite.audio_track = id;
        else
            return WireResult::Invalid;
    }
    if (invite.video_track.empty())
        return WireResult::Invalid;
    out = std::move(invite);
    return WireResult::Ok;
}

// decode_ipc_talk_invite
// 严格分离预览和会议轨；只有声明过的真实驱动才能收到对讲邀请。
WireResult decode_ipc_talk_invite(WireRoute route, std::string_view body,
                                  const IPCCapability& capability, IPCTalkInvite& out) {
    out = {};
    if (route != WireRoute::Signaling)
        return WireResult::WrongRoute;
    IPCTalkInvite invite;
    if (!valid_capability(capability) || !capability.conference_talk ||
        decode_envelope(body, invite.envelope) != DecodeResult::Ok)
        return WireResult::Invalid;
    const auto& envelope = invite.envelope;
    if (envelope.method != "sessionInvite" || envelope.session.empty() || !envelope.request ||
        envelope.code || !envelope.event_sequence || envelope.message_sequence)
        return WireResult::Invalid;
    Json root(ur::device::parse_json(envelope.payload.c_str()), cJSON_Delete);
    std::string purpose, admission, transport;
    if (!(capability.participant_roster_sync
              ? keys(root.get(), {"purpose", "admission", "cameraId", "transport", "expiresAt",
                                  "tracks", "endpointEpoch", "capabilityRevision"})
              : keys(root.get(),
                     {"purpose", "admission", "cameraId", "transport", "expiresAt", "tracks"})) ||
        !text(root.get(), "purpose", purpose) || purpose != "ipcTalk" ||
        !text(root.get(), "admission", admission) ||
        (admission != "direct" && admission != "accept") ||
        !text(root.get(), "cameraId", invite.camera_id) ||
        invite.camera_id != capability.camera_id || !text(root.get(), "transport", transport) ||
        transport != "webrtc" || (admission == "accept" && !capability.local_accept))
        return WireResult::Invalid;
    if (capability.participant_roster_sync) {
        const auto* epoch = cJSON_GetObjectItemCaseSensitive(root.get(), "endpointEpoch");
        const auto* revision = cJSON_GetObjectItemCaseSensitive(root.get(), "capabilityRevision");
        if (!cJSON_IsNumber(epoch) || !std::isfinite(epoch->valuedouble) ||
            epoch->valuedouble < 1 || epoch->valuedouble > 9007199254740991.0 ||
            std::floor(epoch->valuedouble) != epoch->valuedouble || !cJSON_IsNumber(revision) ||
            revision->valuedouble != static_cast<double>(capability.revision))
            return WireResult::Invalid;
        invite.endpoint_epoch = static_cast<std::uint64_t>(epoch->valuedouble);
        invite.capability_revision = capability.revision;
    }
    const auto* expiry = cJSON_GetObjectItemCaseSensitive(root.get(), "expiresAt");
    const auto* tracks = cJSON_GetObjectItemCaseSensitive(root.get(), "tracks");
    if (!cJSON_IsNumber(expiry) || !std::isfinite(expiry->valuedouble) || expiry->valuedouble < 1 ||
        expiry->valuedouble > 9007199254740991.0 ||
        std::floor(expiry->valuedouble) != expiry->valuedouble || !cJSON_IsArray(tracks) ||
        cJSON_GetArraySize(tracks) != 2)
        return WireResult::Invalid;
    invite.expires_at_ms = static_cast<std::uint64_t>(expiry->valuedouble);
    invite.needs_accept = admission == "accept";
    for (const auto* track = tracks->child; track; track = track->next) {
        std::string id, kind, source, direction, codec, state;
        if (!keys(track, {"trackId", "kind", "sourceId", "direction", "codec", "state"}) ||
            !text(track, "trackId", id) || !text(track, "kind", kind) ||
            !text(track, "sourceId", source) || !text(track, "direction", direction) ||
            !text(track, "codec", codec) || !text(track, "state", state) || kind != "audio" ||
            codec != "opus" || state != "requested" || id == invite.microphone_track ||
            id == invite.speaker_track)
            return WireResult::Invalid;
        if (source == "mic" && direction == "sendonly" && invite.microphone_track.empty())
            invite.microphone_track = id;
        else if (source == "speaker" && direction == "recvonly" && invite.speaker_track.empty())
            invite.speaker_track = id;
        else
            return WireResult::Invalid;
    }
    if (invite.microphone_track.empty() || invite.speaker_track.empty())
        return WireResult::Invalid;
    out = std::move(invite);
    return WireResult::Ok;
}

// ipc_media_update_command 编码弱网setQuality修订；目标轨必须是原邀请授权轨，
// 质量档位固定四枚举；不携带同意引用或新轨，不能借质量修订改写用途或传输供应商。
WireResult ipc_media_update_command(std::string token, std::string session,
                                    std::string_view track_id, std::uint64_t expected_revision,
                                    std::string_view profile, const IPCInvite& invite,
                                    Envelope& out) {
    out = {};
    if (!valid_id(token) || !valid_id(session) || session != invite.envelope.session ||
        expected_revision > 9007199254740991ULL)
        return WireResult::Invalid;
    const bool authorized = track_id == invite.video_track ||
                            (!invite.audio_track.empty() && track_id == invite.audio_track) ||
                            (!invite.speaker_track.empty() && track_id == invite.speaker_track);
    if (!authorized || (profile != "audioOnly" && profile != "low" && profile != "standard" &&
                        profile != "high"))
        return WireResult::Invalid;
    Json root(cJSON_CreateObject(), cJSON_Delete);
    if (!root || !cJSON_AddStringToObject(root.get(), "operation", "setQuality") ||
        !cJSON_AddNumberToObject(root.get(), "expectedRevision",
                                 static_cast<double>(expected_revision)) ||
        !cJSON_AddItemToArray(cJSON_AddArrayToObject(root.get(), "trackIds"),
                              cJSON_CreateString(std::string(track_id).c_str())) ||
        !cJSON_AddStringToObject(root.get(), "profile", std::string(profile).c_str()))
        return WireResult::Invalid;
    char* payload = cJSON_PrintUnformatted(root.get());
    if (!payload)
        return WireResult::Invalid;
    Envelope command;
    command.token = std::move(token);
    command.session = std::move(session);
    command.method = "mediaUpdate";
    command.payload = payload;
    cJSON_free(payload);
    std::string json;
    if (encode_envelope(command, json) != DecodeResult::Ok)
        return WireResult::Invalid;
    out = std::move(command);
    return WireResult::Ok;
}

// track_state_report_command
// 编码设备主动实态上报；无code独立事件，目标轨必须是原授权轨。
WireResult track_state_report_command(std::string token, std::string session,
                                      std::string_view track_id, std::string_view state,
                                      const IPCInvite& invite, Envelope& out,
                                      std::string_view request_token, std::uint64_t revision,
                                      std::string_view reason, std::uint64_t media_revision) {
    out = {};
    if (!valid_id(token) || !valid_id(session) || session != invite.envelope.session ||
        media_revision == 0 || media_revision > 9007199254740991ULL)
        return WireResult::Invalid;
    const bool authorized = track_id == invite.video_track ||
                            (!invite.audio_track.empty() && track_id == invite.audio_track) ||
                            (!invite.speaker_track.empty() && track_id == invite.speaker_track);
    const bool failure = state == "failed";
    const bool known = state == "active" || state == "paused" || state == "stopped" || failure;
    if (!authorized || !known || request_token.empty() != (revision == 0) ||
        revision > 9007199254740991ULL ||
        (!request_token.empty() && !valid_id(std::string(request_token))) ||
        (failure ? (request_token.empty() ||
                    (reason != "qualityFailed" && reason != "qualityUnsupported" &&
                     reason != "qualityTimeout"))
                 : !reason.empty()))
        return WireResult::Invalid;
    Json root(cJSON_CreateObject(), cJSON_Delete);
    if (!root || !cJSON_AddStringToObject(root.get(), "trackId", std::string(track_id).c_str()) ||
        !cJSON_AddStringToObject(root.get(), "state", std::string(state).c_str()) ||
        !cJSON_AddRawToObject(root.get(), "mediaRevision", std::to_string(media_revision).c_str()))
        return WireResult::Invalid;
    if ((!request_token.empty() &&
         (!cJSON_AddStringToObject(root.get(), "requestToken",
                                   std::string(request_token).c_str()) ||
          !cJSON_AddNumberToObject(root.get(), "revision", static_cast<double>(revision)))) ||
        (!reason.empty() &&
         !cJSON_AddStringToObject(root.get(), "reason", std::string(reason).c_str())))
        return WireResult::Invalid;
    char* payload = cJSON_PrintUnformatted(root.get());
    if (!payload)
        return WireResult::Invalid;
    Envelope report;
    report.token = std::move(token);
    report.session = std::move(session);
    report.method = "trackState";
    report.request = false;
    report.payload = payload;
    cJSON_free(payload);
    std::string json;
    if (encode_envelope(report, json) != DecodeResult::Ok)
        return WireResult::Invalid;
    out = std::move(report);
    return WireResult::Ok;
}

// decode_media_updated
// 严格解析质量修订下行投影；成功投影不得夹带失败原因，失败投影只保留原因，
// 冲突应答只携带当前计划修订。授权与在途关联仍由设备状态机核对。
WireResult decode_media_updated(WireRoute route, std::string_view body, MediaUpdatedEvent& out,
                                std::uint64_t expected_media_revision) {
    out = {};
    if (route != WireRoute::Signaling)
        return WireResult::WrongRoute;
    MediaUpdatedEvent event;
    if (decode_envelope(body, event.envelope) != DecodeResult::Ok ||
        event.envelope.method != "mediaUpdated" || event.envelope.session.empty())
        return WireResult::Invalid;
    Json data(ur::device::parse_json(event.envelope.payload.c_str()), cJSON_Delete);
    if (!cJSON_IsObject(data.get()))
        return WireResult::Invalid;
    const auto* revision = cJSON_GetObjectItemCaseSensitive(data.get(), "revision");
    // 409冲突应答只有原因与当前修订；其余形态必须携带安全整数计划修订。
    if (event.envelope.code && *event.envelope.code != 200) {
        std::string reason;
        std::uint64_t current = 0;
        if (*event.envelope.code != 409 || event.envelope.event_sequence ||
            !exact(data.get(), {"reason", "currentRevision"}) ||
            !text(data.get(), "reason", reason) ||
            !exact_number(data.get(), "currentRevision", current, true))
            return WireResult::Invalid;
        event.reason = std::move(reason);
        event.current_revision = current;
        out = std::move(event);
        return WireResult::Ok;
    }
    if (!literal(data.get(), "state", "pending") && !literal(data.get(), "state", "applied") &&
        !literal(data.get(), "state", "failed"))
        return WireResult::Invalid;
    event.state = cJSON_GetObjectItemCaseSensitive(data.get(), "state")->valuestring;
    if (!cJSON_IsNumber(revision) || !std::isfinite(revision->valuedouble) ||
        revision->valuedouble < 1 || revision->valuedouble > 9007199254740991.0 ||
        std::floor(revision->valuedouble) != revision->valuedouble)
        return WireResult::Invalid;
    event.revision = static_cast<std::uint64_t>(revision->valuedouble);
    if (event.state == "failed") {
        std::string reason;
        if (event.envelope.code || !event.envelope.event_sequence ||
            !exact(data.get(), {"state", "revision", "reason", "requestToken"}) ||
            !text(data.get(), "requestToken", event.request_token) ||
            !text(data.get(), "reason", reason))
            return WireResult::Invalid;
        event.reason = std::move(reason);
        out = std::move(event);
        return WireResult::Ok;
    }
    std::uint64_t media_revision = 0;
    if (!exact_number(data.get(), "mediaRevision", media_revision) ||
        expected_media_revision == 0 || media_revision != expected_media_revision)
        return WireResult::Invalid;
    event.media_revision = media_revision;
    if ((event.state == "pending" &&
         (!event.envelope.code || *event.envelope.code != 200 || event.envelope.event_sequence ||
          !keys(data.get(), {"state", "revision", "mediaRevision", "tracks", "requestToken"}))) ||
        (event.state == "applied" &&
         !exact(data.get(), {"state", "revision", "mediaRevision", "tracks", "requestToken"})))
        return WireResult::Invalid;
    // 成功投影只携带状态、修订、代次与受影响轨实态；不解析自由轨正文，授权仍由控制器核对。
    const auto* tracks = cJSON_GetObjectItemCaseSensitive(data.get(), "tracks");
    if (!cJSON_IsArray(tracks) || cJSON_GetArraySize(tracks) < 1 || cJSON_GetArraySize(tracks) > 8)
        return WireResult::Invalid;
    for (const auto* track = tracks->child; track; track = track->next) {
        std::string id, track_state;
        if (!exact(track, {"trackId", "kind", "sourceId", "direction", "codec", "state"}) ||
            !text(track, "trackId", id) || !text(track, "state", track_state) ||
            (track_state != "active" && track_state != "paused" && track_state != "stopped") ||
            !event.tracks.emplace(id, track_state).second)
            return WireResult::Invalid;
    }
    if (event.state == "applied") {
        if (event.envelope.code || !event.envelope.event_sequence ||
            !text(data.get(), "requestToken", event.request_token))
            return WireResult::Invalid;
    } else if (cJSON_GetObjectItemCaseSensitive(data.get(), "requestToken") &&
               !text(data.get(), "requestToken", event.request_token))
        return WireResult::Invalid;
    out = std::move(event);
    return WireResult::Ok;
}
}  // namespace ur::device
