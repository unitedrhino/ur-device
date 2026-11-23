// 文件说明：AI控制重鉴权的纯JSON契约；完整验证后才输出临时快照，不执行驱动或扩大原轨授权。
#include "ur/device/ai_control_wire.hpp"
#include <cJSON.h>
#include <algorithm>
#include <cmath>
#include <memory>
#include "ur/device/json_parse.hpp"

namespace ur::device {
namespace {
// Json 统一释放所有解析失败路径的临时树。
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
constexpr std::uint64_t maximum_integer = 9007199254740991ULL;  // 跨Go/JS安全整数边界。
// exact 拒绝未知、遗漏或重复字段；重复键已由公共信封递归拒绝。
bool exact(const cJSON* object, std::initializer_list<std::string_view> fields) {
    if (!cJSON_IsObject(object) || cJSON_GetArraySize(object) != static_cast<int>(fields.size()))
        return false;
    return std::all_of(fields.begin(), fields.end(), [&](const auto key) {
        return cJSON_GetObjectItemCaseSensitive(object, std::string(key).c_str()) != nullptr;
    });
}
// unsigned_lexemes 所有本契约数字均为uint；规范化之前拒绝1e0/1.0，保持Go严格整数语义。
bool unsigned_lexemes(std::string_view body) {
    bool quoted = false, escaped = false;  // 标识符/正文中的数字不得被当成JSON数值。
    for (std::size_t index = 0; index < body.size(); ++index) {
        const char ch = body[index];
        if (quoted) {
            if (escaped)
                escaped = false;
            else if (ch == '\\')
                escaped = true;
            else if (ch == '"')
                quoted = false;
            continue;
        }
        if (ch == '"') {
            quoted = true;
            continue;
        }
        if (ch == '-')
            return false;
        if (ch < '0' || ch > '9')
            continue;
        while (index + 1 < body.size() && body[index + 1] >= '0' && body[index + 1] <= '9')
            ++index;
        if (index + 1 < body.size() &&
            (body[index + 1] == '.' || body[index + 1] == 'e' || body[index + 1] == 'E'))
            return false;
    }
    return true;
}
// number 排除缺省、null、精度丢失；None和内容水位可显式为0。
bool number(const cJSON* root, const char* key, std::uint64_t& out, bool zero = false) {
    const auto* item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble) ||
        item->valuedouble < (zero ? 0 : 1) ||
        item->valuedouble > static_cast<double>(maximum_integer) ||
        std::floor(item->valuedouble) != item->valuedouble)
        return false;
    out = static_cast<std::uint64_t>(item->valuedouble);
    return true;
}
// text 只接有界无控制字符ID；从未采集的inputId允许显式空串，不允许省略。
bool text(const cJSON* root, const char* key, std::string& out, bool empty = false) {
    const auto* item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsString(item))
        return false;
    out = item->valuestring;
    return (empty || !out.empty()) && out.size() <= 128 &&
           std::none_of(out.begin(), out.end(),
                        [](unsigned char ch) { return ch < 32 || ch == 127; });
}
// literal 精确检查固定枚举，不把数字、null或空串解释为缺省状态。
bool literal(const cJSON* root, const char* key, std::string_view expected) {
    const auto* item = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsString(item) && std::string_view(item->valuestring) == expected;
}
// empty_array 已实现安全子集不接续附属任务；不能省略或null代替实际空集合。
bool empty_array(const cJSON* root, const char* key) {
    const auto* item = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsArray(item) && cJSON_GetArraySize(item) == 0;
}
// tracks 完整核原两向音轨及状态，不允许换源、视频、重复ID或新增媒体许可。
bool tracks(const cJSON* array, const VoiceMedia& media, bool ended,
            std::vector<std::string>& states) {
    if (!cJSON_IsArray(array))
        return false;
    if (ended || media.transport == VoiceTransport::None)
        return cJSON_GetArraySize(array) == 0;
    if (media.ai_vision || media.input_track.empty() || media.output_track.empty() ||
        media.input_track == media.output_track || cJSON_GetArraySize(array) != 2)
        return false;
    bool input_seen = false, output_seen = false;  // 轨顺序可变，但两份数组必须同序逐值对应。
    for (const auto* item = array->child; item; item = item->next) {
        std::string id, state;
        if (!exact(item, {"trackId", "kind", "sourceId", "direction", "codec", "state"}) ||
            !text(item, "trackId", id) || !text(item, "state", state) ||
            !literal(item, "kind", "audio") || !literal(item, "codec", "opus") ||
            (state != "active" && state != "paused" && state != "stopped"))
            return false;
        if (id == media.input_track && !input_seen && literal(item, "sourceId", "mic") &&
            literal(item, "direction", "sendonly"))
            input_seen = true;
        else if (id == media.output_track && !output_seen && literal(item, "sourceId", "speaker") &&
                 literal(item, "direction", "recvonly"))
            output_seen = true;
        else
            return false;
        states.push_back(id + ":" + state);
    }
    return input_seen && output_seen;
}
// focus_state 复用原严格焦点正文解析，只存展示事实；不会restore租约或远程打开麦克风。
bool focus_state(const cJSON* item, const Envelope& original, std::optional<FocusState>& out) {
    if (cJSON_IsNull(item))
        return true;
    if (!cJSON_IsObject(item))
        return false;
    std::uint64_t revision = 0;  // cJSON默认打印可能舍入2^53-1，临时副本须保留精确十进制原值。
    if (!number(item, "revision", revision, true))
        return false;
    Json precise(cJSON_Duplicate(item, true), cJSON_Delete);
    auto* raw_revision = cJSON_CreateRaw(std::to_string(revision).c_str());
    if (!precise || !raw_revision ||
        !cJSON_ReplaceItemInObjectCaseSensitive(precise.get(), "revision", raw_revision)) {
        cJSON_Delete(raw_revision);
        return false;
    }
    char* serialized = cJSON_PrintUnformatted(precise.get());
    if (!serialized)
        return false;
    Envelope wrapper;
    wrapper.token = original.token;
    wrapper.session = original.session;
    wrapper.method = "focusState";
    wrapper.request = false;
    wrapper.payload = serialized;
    cJSON_free(serialized);
    std::string encoded;
    FocusState checked;
    if (encode_envelope(wrapper, encoded) != DecodeResult::Ok ||
        decode_focus_state(WireRoute::Signaling, encoded, checked) != WireResult::Ok)
        return false;
    out = std::move(checked);
    return true;
}
}  // namespace

// decode_ai_control_created_binding 只读取公开绑定，不信任或接受设备自报socket来源。
WireResult decode_ai_control_created_binding(std::string_view body, AIControlBinding& out) {
    out = {};
    Envelope envelope;
    if (decode_envelope(body, envelope) != DecodeResult::Ok ||
        envelope.method != "sessionCreated" || envelope.request || envelope.session.empty() ||
        !envelope.code || *envelope.code != 200)
        return WireResult::Invalid;
    Json root(parse_json(envelope.payload.c_str()), cJSON_Delete);
    const bool epoch = cJSON_GetObjectItemCaseSensitive(root.get(), "endpointEpoch") != nullptr;
    const bool revision =
        cJSON_GetObjectItemCaseSensitive(root.get(), "capabilityRevision") != nullptr;
    if (!epoch && !revision)
        return WireResult::Ok;  // 不要求未声明恢复能力的既有端新增字段或改变旧解码。
    AIControlBinding binding;
    if (!epoch || !revision || !unsigned_lexemes(body) ||
        !number(root.get(), "endpointEpoch", binding.endpoint_epoch) ||
        !number(root.get(), "capabilityRevision", binding.capability_revision))
        return WireResult::Invalid;
    out = binding;
    return WireResult::Ok;
}

// ai_session_sync_command 使用十进制整数字面量，避免cJSON中间打印舍入安全边界。
WireResult ai_session_sync_command(std::string token, std::string session, std::uint64_t last_event,
                                   std::uint64_t last_message, const AIControlBinding& binding,
                                   std::uint64_t media_revision, VoiceTransport transport,
                                   Envelope& out) {
    out = {};
    if (session.empty() || session.size() > 64 || binding.endpoint_epoch == 0 ||
        binding.capability_revision == 0 || binding.endpoint_epoch > maximum_integer ||
        binding.capability_revision > maximum_integer || last_event > maximum_integer ||
        last_message > maximum_integer || media_revision > maximum_integer ||
        (transport == VoiceTransport::None ? media_revision != 0 : media_revision == 0) ||
        (transport != VoiceTransport::None && transport != VoiceTransport::Udp &&
         transport != VoiceTransport::WebRTC))
        return WireResult::Invalid;
    Envelope request;
    request.token = std::move(token);
    request.session = std::move(session);
    request.method = "sessionSync";
    request.payload = "{\"endpointEpoch\":" + std::to_string(binding.endpoint_epoch) +
                      ",\"lastEventSeq\":" + std::to_string(last_event) +
                      ",\"lastMessageSeq\":" + std::to_string(last_message) +
                      ",\"mediaRevision\":" + std::to_string(media_revision) + "}";
    std::string encoded;
    if (encode_envelope(request, encoded) != DecodeResult::Ok ||
        decode_envelope(encoded, out) != DecodeResult::Ok)
        return WireResult::Invalid;
    return WireResult::Ok;
}

// decode_ai_control_snapshot 对所有必填控制对象整批校验，任何非法字段都清空输出。
WireResult decode_ai_control_snapshot(WireRoute route, std::string_view body,
                                      const VoiceMedia& selected, AIControlSnapshot& out) {
    out = {};
    if (route != WireRoute::Signaling)
        return WireResult::WrongRoute;
    AIControlSnapshot snapshot;
    if (!unsigned_lexemes(body) || decode_envelope(body, snapshot.envelope) != DecodeResult::Ok)
        return WireResult::Invalid;
    const auto& envelope = snapshot.envelope;
    if (envelope.method != "sessionSnapshot" || envelope.request || envelope.session.empty() ||
        envelope.session.size() > 64 || !envelope.code || *envelope.code != 200 ||
        !envelope.event_sequence || envelope.message_sequence)
        return WireResult::Invalid;
    Json root(parse_json(envelope.payload.c_str()), cJSON_Delete);
    const bool ended = literal(root.get(), "state", "ended");
    const bool established = literal(root.get(), "state", "established");
    const auto* resume = cJSON_GetObjectItemCaseSensitive(root.get(), "resumeAllowed");
    if ((!ended && !established) || !cJSON_IsBool(resume) ||
        !number(root.get(), "revision", snapshot.revision) ||
        !number(root.get(), "eventWatermark", snapshot.event_watermark) ||
        snapshot.event_watermark != *envelope.event_sequence ||
        !number(root.get(), "messageWatermark", snapshot.message_watermark, true) ||
        !number(root.get(), "endpointEpoch", snapshot.binding.endpoint_epoch) ||
        !number(root.get(), "capabilityRevision", snapshot.binding.capability_revision) ||
        !number(root.get(), "mediaRevision", snapshot.media_revision, true) ||
        (selected.transport == VoiceTransport::None ? snapshot.media_revision != 0
                                                    : snapshot.media_revision == 0) ||
        !number(root.get(), "expiresAt", snapshot.expires_at) ||
        !empty_array(root.get(), "participants"))
        return WireResult::Invalid;
    snapshot.ended = ended;
    snapshot.resume_allowed = cJSON_IsTrue(resume);
    const bool reason_required = ended || !snapshot.resume_allowed;
    if (reason_required) {
        std::string reason;
        if (!exact(root.get(), {"state", "revision", "eventWatermark", "messageWatermark",
                                "endpointEpoch", "capabilityRevision", "mediaRevision", "expiresAt",
                                "resumeAllowed", "participants", "tracks", "controls", "reason"}) ||
            !text(root.get(), "reason", reason) ||
            (ended ? snapshot.resume_allowed : reason != "snapshotOnly"))
            return WireResult::Invalid;
    } else if (!exact(root.get(),
                      {"state", "revision", "eventWatermark", "messageWatermark", "endpointEpoch",
                       "capabilityRevision", "mediaRevision", "expiresAt", "resumeAllowed",
                       "participants", "tracks", "controls"}))
        return WireResult::Invalid;
    const auto* controls = cJSON_GetObjectItemCaseSensitive(root.get(), "controls");
    if (!exact(controls, {"call", "focus", "floor", "recordings", "playbacks", "processors",
                          "broadcasts", "handoffs", "mediaPlan", "ai"}) ||
        !cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(controls, "call")) ||
        !cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(controls, "floor")))
        return WireResult::Invalid;
    for (const char* key : {"recordings", "playbacks", "processors", "broadcasts", "handoffs"})
        if (!empty_array(controls, key))
            return WireResult::Invalid;
    if (!focus_state(cJSON_GetObjectItemCaseSensitive(controls, "focus"), envelope,
                     snapshot.focus) ||
        (ended && snapshot.focus))
        return WireResult::Invalid;
    const auto* plan = cJSON_GetObjectItemCaseSensitive(controls, "mediaPlan");
    std::uint64_t plan_revision = 0;
    std::vector<std::string> media_states, plan_states;
    if (!exact(plan, {"revision", "tracks"}) || !number(plan, "revision", plan_revision, true) ||
        plan_revision != snapshot.media_revision ||
        !tracks(cJSON_GetObjectItemCaseSensitive(root.get(), "tracks"), selected, ended,
                media_states) ||
        !tracks(cJSON_GetObjectItemCaseSensitive(plan, "tracks"), selected, ended, plan_states) ||
        media_states != plan_states)
        return WireResult::Invalid;
    const auto* ai = cJSON_GetObjectItemCaseSensitive(controls, "ai");
    if (ended) {
        if (!cJSON_IsNull(ai))
            return WireResult::Invalid;
    } else {
        if (!exact(ai, {"inputId", "inputState", "activeRespIds"}) ||
            !text(ai, "inputId", snapshot.input_id, true) ||
            (!literal(ai, "inputState", "active") && !literal(ai, "inputState", "paused")))
            return WireResult::Invalid;
        snapshot.input_active = literal(ai, "inputState", "active");
        if (snapshot.input_active && snapshot.input_id.empty())
            return WireResult::Invalid;
        const auto* responses = cJSON_GetObjectItemCaseSensitive(ai, "activeRespIds");
        if (!cJSON_IsArray(responses) || cJSON_GetArraySize(responses) > 512)
            return WireResult::Invalid;
        for (const auto* item = responses->child; item; item = item->next) {
            if (!cJSON_IsString(item))
                return WireResult::Invalid;
            const std::string id = item->valuestring;
            if (id.empty() || id.size() > 128 ||
                std::any_of(id.begin(), id.end(),
                            [](unsigned char ch) { return ch < 32 || ch == 127; }) ||
                std::find(snapshot.active_responses.begin(), snapshot.active_responses.end(), id) !=
                    snapshot.active_responses.end())
                return WireResult::Invalid;
            snapshot.active_responses.push_back(id);
        }
    }
    out = std::move(snapshot);
    return WireResult::Ok;
}
}  // namespace ur::device
