// 文件说明：公共成员正文严格解析与单腿有界投影；成功也不代表会议准入或驱动权限。
#include "ur/device/json_parse.hpp"
#include "ur/device/participant_roster.hpp"
#include <cJSON.h>
#include <algorithm>
#include <memory>
#include "ur/device/envelope.hpp"

namespace ur::device {
namespace {
// Json 将临时解析树的所有权限定在一次纯校验内。
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
// exact_keys 禁止未知或重复键改变完整条目语义。
bool exact_keys(const cJSON* object, std::initializer_list<std::string_view> keys) {
    if (!cJSON_IsObject(object))
        return false;
    std::size_t count = 0;
    for (auto* field = object->child; field; field = field->next) {
        if (!field->string || std::find(keys.begin(), keys.end(), field->string) == keys.end())
            return false;
        for (auto* previous = object->child; previous != field; previous = previous->next)
            if (std::string_view(previous->string) == field->string)
                return false;
        ++count;
    }
    return count == keys.size();
}
// text 接受公共有界字符串，拒绝控制字符，不将不可信正文写入诊断。
bool text(const cJSON* object, const char* key, std::string& out, std::size_t limit) {
    const auto* value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsString(value))
        return false;
    out = value->valuestring;
    return !out.empty() && out.size() <= limit &&
           std::none_of(out.begin(), out.end(),
                        [](unsigned char ch) { return ch < 32 || ch == 127; });
}
// decode 验证完整公共Track集合；当前IPC生产仅空轨，未来轨描述仍不能执行驱动动作。
bool decode(const cJSON* root, RosterParticipant& member, bool& joined, bool incremental = true) {
    std::string change;  // 沿原joined/left枚举，不创造新的成员方法。
    const auto* tracks = cJSON_GetObjectItemCaseSensitive(root, "tracks");
    if (!(incremental ? exact_keys(root, {"participantId", "change", "tracks"})
                      : exact_keys(root, {"participantId", "tracks"})) ||
        !text(root, "participantId", member.participant_id, 128) ||
        (incremental &&
         (!text(root, "change", change, 16) || (change != "joined" && change != "left"))) ||
        !cJSON_IsArray(tracks) || cJSON_GetArraySize(tracks) > 8)
        return false;
    joined = change == "joined";
    for (auto* item = tracks->child; item; item = item->next) {
        ParticipantTrack track;  // 全字段先核验，失败不提交半条目。
        if (!exact_keys(item, {"trackId", "kind", "sourceId", "direction", "codec", "state"}) ||
            !text(item, "trackId", track.track_id, 128) || !text(item, "kind", track.kind, 16) ||
            !text(item, "sourceId", track.source_id, 128) ||
            !text(item, "direction", track.direction, 16) ||
            !text(item, "codec", track.codec, 64) || !text(item, "state", track.state, 16) ||
            (track.kind != "audio" && track.kind != "video" && track.kind != "subtitle") ||
            (track.direction != "sendonly" && track.direction != "recvonly" &&
             track.direction != "sendrecv") ||
            (track.state != "requested" && track.state != "active" && track.state != "paused" &&
             track.state != "stopped") ||
            std::any_of(member.tracks.begin(), member.tracks.end(),
                        [&](const auto& old) { return old.track_id == track.track_id; }))
            return false;
        member.tracks.push_back(std::move(track));
    }
    return true;
}
// bytes 计算条目及字符串的逻辑预算，不把allocator额外开销伪称为固定堆内存。
std::size_t bytes(const RosterParticipant& member) {
    std::size_t result = sizeof(RosterParticipant) + member.participant_id.size();
    for (const auto& track : member.tracks)
        result += sizeof(ParticipantTrack) + track.track_id.size() + track.kind.size() +
                  track.source_id.size() + track.direction.size() + track.codec.size() +
                  track.state.size();
    return result;
}
}  // namespace

// apply 只在整份正文和最终名册边界合法后提交；left未知成员仍幂等，不复活其他成员。
bool ParticipantRoster::apply(std::string_view payload) {
    std::string body;  // 复用公共有界JSON检查，纯入口也拒绝非法UTF-8、NUL与深度炸弹。
    if (payload.empty() || canonical_json_object(payload, body) != DecodeResult::Ok)
        return false;
    Json root(ur::device::parse_json_with_length_opts(body.c_str(), body.size() + 1, nullptr, true),
              cJSON_Delete);
    RosterParticipant candidate;
    bool joined = false;
    if (!decode(root.get(), candidate, joined))
        return false;
    auto previous = std::find_if(members_.begin(), members_.end(), [&](const auto& member) {
        return member.participant_id == candidate.participant_id;
    });
    if (!joined) {
        if (previous != members_.end())
            members_.erase(previous);
        return true;
    }
    if (previous == members_.end() && members_.size() >= 32)
        return false;
    std::size_t total = bytes(candidate);
    for (const auto& member : members_) {
        if (member.participant_id == candidate.participant_id)
            continue;
        total += bytes(member);
        for (const auto& track : member.tracks)
            if (std::any_of(candidate.tracks.begin(), candidate.tracks.end(),
                            [&](const auto& next) { return next.track_id == track.track_id; }))
                return false;
    }
    if (total > 65536)
        return false;
    if (previous == members_.end())
        members_.push_back(std::move(candidate));
    else
        *previous = std::move(candidate);
    return true;
}

// replace 不合并旧名册；整批验证成员、跨成员轨唯一和逻辑预算后一次移动提交。
bool ParticipantRoster::replace(std::string_view participants) {
    if (participants.empty() || participants.size() > 65536)
        return false;
    std::string body;  // 独立数组套固定对象复用公共安全检查，不引入第二套UTF-8或深度规则。
    if (canonical_json_object("{\"participants\":" + std::string(participants) + "}", body) !=
        DecodeResult::Ok)
        return false;
    Json root(ur::device::parse_json_with_length_opts(body.c_str(), body.size() + 1, nullptr, true),
              cJSON_Delete);
    const auto* array = cJSON_GetObjectItemCaseSensitive(root.get(), "participants");
    if (!cJSON_IsArray(array) || cJSON_GetArraySize(array) > 32)
        return false;
    std::vector<RosterParticipant> replacement;  // 不在原投影上逐项更新，失败不留下半批新状态。
    std::size_t total = 0;                       // 全量与增量沿用同一逻辑64KiB预算。
    for (auto* item = array->child; item; item = item->next) {
        RosterParticipant candidate;
        bool unused_joined = false;  // 全量条目没有change，不借用joined/left语义。
        if (!decode(item, candidate, unused_joined, false))
            return false;
        total += bytes(candidate);
        if (total > 65536)
            return false;
        for (const auto& member : replacement) {
            if (member.participant_id == candidate.participant_id)
                return false;
            for (const auto& track : member.tracks)
                if (std::any_of(candidate.tracks.begin(), candidate.tracks.end(),
                                [&](const auto& next) { return next.track_id == track.track_id; }))
                    return false;
        }
        replacement.push_back(std::move(candidate));
    }
    members_ = std::move(replacement);
    return true;
}
}  // namespace ur::device
