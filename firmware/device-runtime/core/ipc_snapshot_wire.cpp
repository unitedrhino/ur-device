// 文件说明：完整IPC名册同步契约；只校验原公开绑定、原轨及无附加授权的控制快照。
#include "ur/device/json_parse.hpp"
#include <cJSON.h>
#include <algorithm>
#include <cmath>
#include <memory>
#include "ur/device/ipc_wire.hpp"
#include "ur/device/participant_roster.hpp"

namespace ur::device {
namespace {
// Json 限定一次纯解析树的所有权，所有非法路径均自动释放。
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
// exact 完整对象必须包含全部且仅这些键，公共信封解析器已拒绝嵌套重复键。
bool exact(const cJSON* object, std::initializer_list<std::string_view> fields) {
    if (!cJSON_IsObject(object))
        return false;
    std::size_t count = 0;
    for (auto* item = object->child; item; item = item->next) {
        if (!item->string || std::find(fields.begin(), fields.end(), item->string) == fields.end())
            return false;
        ++count;
    }
    return count == fields.size();
}
// number 只读取精确JSON安全整数，不以缺省0补齐必填字段。
bool number(const cJSON* object, const char* key, std::uint64_t& value, bool zero = false) {
    const auto* item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble) ||
        item->valuedouble < (zero ? 0 : 1) || item->valuedouble > 9007199254740991.0 ||
        std::floor(item->valuedouble) != item->valuedouble)
        return false;
    value = static_cast<std::uint64_t>(item->valuedouble);
    return true;
}
// text 读取固定有界字符串，不暴露原自由正文或设备身份。
bool text(const cJSON* object, const char* key, std::string& value, std::size_t limit = 128) {
    const auto* item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsString(item))
        return false;
    value = item->valuestring;
    return !value.empty() && value.size() <= limit &&
           std::none_of(value.begin(), value.end(),
                        [](unsigned char c) { return c < 32 || c == 127; });
}
// literal 对必填字符串精确比较，null、数字或省略不能变成固定语义。
// key沿用cJSON查找指针，expected限定调用点枚举，不为纯解析辅助引入新线协议类型。
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
bool literal(const cJSON* object, const char* key, const char* expected) {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    const auto* item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(item) && std::string_view(item->valuestring) == expected;
}
// tracks 仅接受原邀请两条音轨的展示状态；不允许新增轨、换方向或假授予媒体。
bool tracks(const cJSON* array, const IPCInvite& invite,
            std::map<std::string, std::string>& states) {
    if (!cJSON_IsArray(array) || cJSON_GetArraySize(array) != 2)
        return false;
    for (auto* item = array->child; item; item = item->next) {
        std::string id, state;
        if (!exact(item, {"trackId", "kind", "sourceId", "direction", "codec", "state"}) ||
            !text(item, "trackId", id) || !text(item, "state", state, 16) ||
            !literal(item, "kind", "audio") || !literal(item, "codec", "opus") ||
            (state != "active" && state != "paused" && state != "stopped") || states.count(id))
            return false;
        if (id == invite.audio_track) {
            if (!literal(item, "sourceId", "mic") || !literal(item, "direction", "sendonly"))
                return false;
        } else if (id == invite.speaker_track) {
            if (!literal(item, "sourceId", "speaker") || !literal(item, "direction", "recvonly"))
                return false;
        } else
            return false;
        states.emplace(std::move(id), std::move(state));
    }
    return true;
}
// same_tracks
// 按原数组顺序比较已校验字段，不依赖JSON键顺序或以字符串格式差异拒绝合法对象。
bool same_tracks(const cJSON* left, const cJSON* right) {
    for (int index = 0; index < 2; ++index) {
        const auto* a = cJSON_GetArrayItem(left, index);
        const auto* b = cJSON_GetArrayItem(right, index);
        for (const auto* key : {"trackId", "kind", "sourceId", "direction", "codec", "state"}) {
            const auto* value = cJSON_GetObjectItemCaseSensitive(a, key);
            if (!cJSON_IsString(value) || !literal(b, key, value->valuestring))
                return false;
        }
    }
    return true;
}
}  // namespace

// decode_ipc_roster_snapshot
// 校验全量结果后才返回临时快照；接受established只读/恢复与ended终止三种完整形态，
// 在途token、水位及时间仍由设备状态机检查。
WireResult decode_ipc_roster_snapshot(WireRoute route, std::string_view body,
                                      const IPCInvite& invite, const IPCCapability& capability,
                                      IPCRosterSnapshot& out) {
    out = {};
    if (route != WireRoute::Signaling)
        return WireResult::WrongRoute;
    IPCRosterSnapshot snapshot;
    if (!capability.participant_roster_sync || !capability.participant_roster ||
        !capability.conference_talk || invite.audio_track.empty() || invite.speaker_track.empty() ||
        invite.audio_track == invite.speaker_track || invite.endpoint_epoch == 0 ||
        invite.capability_revision != capability.revision ||
        decode_envelope(body, snapshot.envelope) != DecodeResult::Ok)
        return WireResult::Invalid;
    const auto& message = snapshot.envelope;
    if (message.method != "sessionSnapshot" || message.session != invite.envelope.session ||
        message.request || !message.code || *message.code != 200 || !message.event_sequence ||
        message.message_sequence)
        return WireResult::Invalid;
    Json root(ur::device::parse_json(message.payload.c_str()), cJSON_Delete);
    std::uint64_t zero = 0, epoch = 0, revision = 0, media_revision = 0;
    if (!cJSON_IsObject(root.get()) ||
        !number(root.get(), "revision", snapshot.revision) ||
        !number(root.get(), "eventWatermark", snapshot.event_watermark) ||
        snapshot.event_watermark != *message.event_sequence ||
        !number(root.get(), "messageWatermark", zero, true) || zero != 0 ||
        !number(root.get(), "endpointEpoch", epoch) || epoch != invite.endpoint_epoch ||
        !number(root.get(), "capabilityRevision", revision) ||
        revision != invite.capability_revision ||
        !number(root.get(), "mediaRevision", media_revision) || media_revision != 1 ||
        !number(root.get(), "expiresAt", snapshot.expires_at_ms))
        return WireResult::Invalid;
    const bool ended = literal(root.get(), "state", "ended");
    const bool established = literal(root.get(), "state", "established");
    const auto* resume_item = cJSON_GetObjectItemCaseSensitive(root.get(), "resumeAllowed");
    if ((!established && !ended) || !cJSON_IsBool(resume_item))
        return WireResult::Invalid;
    snapshot.ended = ended;
    snapshot.resume_allowed = cJSON_IsTrue(resume_item);
    if (ended) {
        // 终止快照是终态投影：固定撤空授权轨与展示名册，携带有界终态原因（如sessionExpired），
        // 绝不携带恢复许可；设备据此执行R03清理而不是恢复媒体。
        std::string reason;
        if (snapshot.resume_allowed ||
            !exact(root.get(),
                   {"state", "revision", "eventWatermark", "messageWatermark", "endpointEpoch",
                    "mediaRevision", "tracks", "controls", "capabilityRevision", "resumeAllowed",
                    "reason", "participants", "expiresAt"}) ||
            !text(root.get(), "reason", reason))
            return WireResult::Invalid;
        const auto* media_tracks = cJSON_GetObjectItemCaseSensitive(root.get(), "tracks");
        const auto* controls = cJSON_GetObjectItemCaseSensitive(root.get(), "controls");
        if (!cJSON_IsArray(media_tracks) || cJSON_GetArraySize(media_tracks) != 0 ||
            !exact(controls, {"call", "focus", "floor", "recordings", "playbacks", "processors",
                              "broadcasts", "handoffs", "mediaPlan", "ai"}))
            return WireResult::Invalid;
        for (const auto* key : {"call", "focus", "floor", "ai"})
            if (!cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(controls, key)))
                return WireResult::Invalid;
        for (const auto* key : {"recordings", "playbacks", "processors", "broadcasts", "handoffs"}) {
            const auto* values = cJSON_GetObjectItemCaseSensitive(controls, key);
            if (!cJSON_IsArray(values) || cJSON_GetArraySize(values) != 0)
                return WireResult::Invalid;
        }
        const auto* plan = cJSON_GetObjectItemCaseSensitive(controls, "mediaPlan");
        const auto* plan_tracks = cJSON_GetObjectItemCaseSensitive(plan, "tracks");
        if (!exact(plan, {"revision", "tracks"}) || !number(plan, "revision", media_revision) ||
            media_revision != 1 || !cJSON_IsArray(plan_tracks) ||
            cJSON_GetArraySize(plan_tracks) != 0)
            return WireResult::Invalid;
        const auto* members = cJSON_GetObjectItemCaseSensitive(root.get(), "participants");
        if (!cJSON_IsArray(members) || cJSON_GetArraySize(members) != 0)
            return WireResult::Invalid;
        snapshot.participants = "[]";
        out = std::move(snapshot);
        return WireResult::Ok;
    }
    if (snapshot.resume_allowed
            ? !exact(root.get(),
                     {"state", "revision", "eventWatermark", "messageWatermark", "endpointEpoch",
                      "mediaRevision", "tracks", "controls", "capabilityRevision",
                      "resumeAllowed", "participants", "expiresAt"})
            : (!exact(root.get(),
                      {"state", "revision", "eventWatermark", "messageWatermark", "endpointEpoch",
                       "mediaRevision", "tracks", "controls", "capabilityRevision",
                       "resumeAllowed", "reason", "participants", "expiresAt"}) ||
               !literal(root.get(), "reason", "snapshotOnly")))
        return WireResult::Invalid;
    const auto* media_tracks = cJSON_GetObjectItemCaseSensitive(root.get(), "tracks");
    const auto* controls = cJSON_GetObjectItemCaseSensitive(root.get(), "controls");
    if (!tracks(media_tracks, invite, snapshot.track_states) ||
        !exact(controls, {"call", "focus", "floor", "recordings", "playbacks", "processors",
                          "broadcasts", "handoffs", "mediaPlan", "ai"}))
        return WireResult::Invalid;
    for (const auto* key : {"call", "focus", "floor", "ai"})
        if (!cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(controls, key)))
            return WireResult::Invalid;
    for (const auto* key : {"recordings", "playbacks", "processors", "broadcasts", "handoffs"}) {
        const auto* values = cJSON_GetObjectItemCaseSensitive(controls, key);
        if (!cJSON_IsArray(values) || cJSON_GetArraySize(values) != 0)
            return WireResult::Invalid;
    }
    const auto* plan = cJSON_GetObjectItemCaseSensitive(controls, "mediaPlan");
    const auto* plan_tracks = cJSON_GetObjectItemCaseSensitive(plan, "tracks");
    std::map<std::string, std::string> plan_states;
    if (!exact(plan, {"revision", "tracks"}) || !number(plan, "revision", media_revision) ||
        media_revision != 1 || !tracks(plan_tracks, invite, plan_states) ||
        !same_tracks(media_tracks, plan_tracks))
        return WireResult::Invalid;
    const auto* members = cJSON_GetObjectItemCaseSensitive(root.get(), "participants");
    if (!cJSON_IsArray(members) || cJSON_GetArraySize(members) > 32)
        return WireResult::Invalid;
    for (auto* member = members->child; member; member = member->next) {
        std::string id;
        const auto* member_tracks = cJSON_GetObjectItemCaseSensitive(member, "tracks");
        if (!exact(member, {"participantId", "tracks"}) || !text(member, "participantId", id, 64) ||
            !cJSON_IsArray(member_tracks) || cJSON_GetArraySize(member_tracks) != 0)
            return WireResult::Invalid;
    }
    char* serialized = cJSON_PrintUnformatted(members);  // 有界完整数组归临时对象，不把正文写日志。
    if (!serialized)
        return WireResult::Invalid;
    snapshot.participants = serialized;
    cJSON_free(serialized);
    ParticipantRoster projected;
    if (!projected.replace(snapshot.participants))
        return WireResult::Invalid;
    out = std::move(snapshot);
    return WireResult::Ok;
}
}  // namespace ur::device
