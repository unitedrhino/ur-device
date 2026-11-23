// 文件说明：云端录制控制构造与状态解析的有界JSON校验；不执行取流、封装或同意仲裁。
#include "ur/device/json_parse.hpp"
#include "ur/device/recording_wire.hpp"
#include <cJSON.h>
#include <algorithm>
#include <cmath>
#include <memory>

namespace ur::device {
namespace {
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;       // 所有失败出口自动释放解析树。
constexpr std::uint64_t safe_integer = 9007199254740991ULL;         // MQTT跨语言安全整数上界。
// text 排除控制字符并限制长度，公共信封已校验UTF-8/重复键/深度。
bool text(const cJSON* root, const char* key, std::string& out, std::size_t limit) {
    const auto* value = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsString(value))
        return false;
    out = value->valuestring;
    return !out.empty() && out.size() <= limit &&
           std::none_of(out.begin(), out.end(),
                        [](unsigned char ch) { return ch < 32 || ch == 127; });
}
// one_of 校验封闭枚举，不把自由文字解释为指令或失败原因。
bool one_of(std::string_view value, std::initializer_list<std::string_view> allowed) {
    return std::find(allowed.begin(), allowed.end(), value) != allowed.end();
}
// integer 保留零值，拒绝字符串、null、分数、负数和精度溢出。
bool integer(const cJSON* root, const char* key, std::uint64_t& out) {
    const auto* value = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) || value->valuedouble < 0 ||
        value->valuedouble > static_cast<double>(safe_integer) ||
        std::floor(value->valuedouble) != value->valuedouble)
        return false;
    out = static_cast<std::uint64_t>(value->valuedouble);
    return true;
}
// valid_id 排除空值、超长与控制字符；ID不是本地路径，语义由会话层核验。
bool valid_id(std::string_view value, std::size_t limit) {
    return !value.empty() && value.size() <= limit &&
           std::none_of(value.begin(), value.end(),
                        [](unsigned char ch) { return ch < 32 || ch == 127; });
}
// content_ref 复用历史引用语法边界：不接受URL、路径回退或内嵌数据，授权仍由内容服务独立判定。
bool content_ref(std::string_view value) {
    return valid_id(value, 512) && value.find("://") == std::string_view::npos &&
           value.find('\\') == std::string_view::npos && value.front() != '/' &&
           value.rfind("../", 0) != 0 && value.find("/../") == std::string_view::npos &&
           value.rfind("data:", 0) != 0 && value.rfind("file:", 0) != 0;
}
// recording_reason 是录制域固定公开原因集合；错误原文由宿主脱敏，不通过信令带出URL或凭据。
bool recording_reason(std::string_view value) {
    return one_of(value, {"recordingConsentMissing", "permissionDenied", "controlTimeout",
                          "invalidState", "internalError", "driverFailed"});
}
// exact_integers 保留临时树中安全整数的精确字面量，避免中间打印近似修订号或时长。
bool exact_integers(cJSON* root) {
    for (auto* item = root->child; item;) {
        auto* next = item->next;  // 节点替换会释放原对象，不能再借用原next。
        if (cJSON_IsNumber(item) && std::isfinite(item->valuedouble) &&
            std::floor(item->valuedouble) == item->valuedouble &&
            std::abs(item->valuedouble) <= static_cast<double>(safe_integer)) {
            const auto literal = std::to_string(static_cast<std::int64_t>(item->valuedouble));
            auto* raw = cJSON_CreateRaw(literal.c_str());
            if (!raw)
                return false;
            const bool replaced =
                cJSON_IsObject(root)
                    ? cJSON_ReplaceItemInObjectCaseSensitive(root, item->string, raw)
                    : cJSON_ReplaceItemViaPointer(root, item, raw);
            if (!replaced) {
                cJSON_Delete(raw);
                return false;
            }
        } else if (!exact_integers(item)) {
            return false;
        }
        item = next;
    }
    return true;
}
// add_strings 输出非空ID数组；编码阶段不再重复逐项校验，入口已保证有界与去重。
bool add_strings(cJSON* root, const char* key, const std::vector<std::string>& values) {
    auto* array = cJSON_AddArrayToObject(root, key);
    if (!array)
        return false;
    for (const auto& value : values)
        if (!cJSON_AddItemToArray(array, cJSON_CreateString(value.c_str())))
            return false;
    return true;
}
}  // namespace

// recording_control_command
// 从已知本端意图构造原上行请求；条件字段与操作严格绑定，编码失败清空输出。
WireResult recording_control_command(const RecordingControlCommand& control, std::string_view token,
                                     std::string_view session, Envelope& out) {
    out = {};
    const bool starting = control.operation == "start";
    const bool resuming = control.operation == "resume";
    if (token.empty() || token.size() > 128 || session.empty() || session.size() > 64 ||
        !one_of(control.operation, {"start", "pause", "resume", "stop"}) ||
        control.expected_revision > safe_integer || !valid_id(control.recording_id, 128) ||
        (!control.policy_id.empty() && !valid_id(control.policy_id, 128)) ||
        control.source_track_ids.size() > 16 || control.consent_ids.size() > 64)
        return WireResult::Invalid;
    for (const auto& track : control.source_track_ids)
        if (!valid_id(track, 128) ||
            std::count(control.source_track_ids.begin(), control.source_track_ids.end(), track) != 1)
            return WireResult::Invalid;
    for (const auto& consent : control.consent_ids)
        if (!valid_id(consent, 128) ||
            std::count(control.consent_ids.begin(), control.consent_ids.end(), consent) != 1)
            return WireResult::Invalid;
    // start要求轨/同意/策略齐备，resume只补同意；其他操作禁止夹带start副作用字段。
    if (control.source_track_ids.empty() == starting ||
        control.consent_ids.empty() == (starting || resuming) ||
        control.policy_id.empty() == starting)
        return WireResult::Invalid;
    Json root(cJSON_CreateObject(), cJSON_Delete);
    if (!root || !cJSON_AddStringToObject(root.get(), "operation", control.operation.c_str()) ||
        !cJSON_AddNumberToObject(root.get(), "expectedRevision",
                                 static_cast<double>(control.expected_revision)) ||
        !cJSON_AddStringToObject(root.get(), "recordingId", control.recording_id.c_str()))
        return WireResult::Invalid;
    if (!control.source_track_ids.empty() && !add_strings(root.get(), "sourceTrackIds", control.source_track_ids))
        return WireResult::Invalid;
    if (!control.consent_ids.empty() && !add_strings(root.get(), "consentIds", control.consent_ids))
        return WireResult::Invalid;
    if (!control.policy_id.empty() &&
        !cJSON_AddStringToObject(root.get(), "policyId", control.policy_id.c_str()))
        return WireResult::Invalid;
    if (!exact_integers(root.get()))
        return WireResult::Invalid;
    char* body = cJSON_PrintUnformatted(root.get());
    if (!body)
        return WireResult::Invalid;
    Envelope request;
    request.token = std::string(token);
    request.session = std::string(session);
    request.method = "recordingControl";
    request.request = true;
    request.payload = body;
    cJSON_free(body);
    std::string json;
    Envelope validated;
    // 回读规范化后的信封：payload统一为排序键紧凑形式，与其他wire构造产物一致。
    if (encode_envelope(request, json) != DecodeResult::Ok ||
        decode_envelope(json, validated) != DecodeResult::Ok)
        return WireResult::Invalid;
    out = std::move(validated);
    return WireResult::Ok;
}

// decode_recording_state
// 解析平台对录制的受理应答、失败应答与自主完成事件；ready/failed只出现在自主事件中。
WireResult decode_recording_state(WireRoute route, std::string_view json, RecordingStateFact& out) {
    out = {};
    if (route != WireRoute::Signaling)
        return WireResult::WrongRoute;
    RecordingStateFact fact;
    if (decode_envelope(json, fact.envelope) != DecodeResult::Ok ||
        fact.envelope.method != "recordingState" || fact.envelope.request ||
        fact.envelope.session.empty() || fact.envelope.session.size() > 64 ||
        fact.envelope.message_sequence)
        return WireResult::Invalid;
    Json root(ur::device::parse_json(fact.envelope.payload.c_str()), cJSON_Delete);
    if (!root)
        return WireResult::Invalid;
    const int fields = cJSON_GetArraySize(root.get());
    if (fact.envelope.code) {
        // 受理应答只允许恰好三字段成功事实；失败应答只保留固定脱敏原因。
        if (fact.envelope.event_sequence)
            return WireResult::Invalid;
        const int code = *fact.envelope.code;
        if (code >= 400) {
            if (code > 599 || fields != 1 || !text(root.get(), "reason", fact.reason, 32) ||
                !recording_reason(fact.reason))
                return WireResult::Invalid;
            out = std::move(fact);
            return WireResult::Ok;
        }
        if (code != 200 || fields != 3 || !text(root.get(), "state", fact.state, 16) ||
            !one_of(fact.state, {"starting", "active", "paused", "finalizing"}) ||
            !integer(root.get(), "revision", fact.revision) ||
            !text(root.get(), "recordingId", fact.recording_id, 128))
            return WireResult::Invalid;
        out = std::move(fact);
        return WireResult::Ok;
    }
    // 自主事件必须带信令水位；按状态锁定条件字段，自动paused须说明原因。
    if (!fact.envelope.event_sequence ||
        !text(root.get(), "state", fact.state, 16) ||
        !one_of(fact.state, {"starting", "active", "paused", "finalizing", "ready", "failed"}) ||
        !integer(root.get(), "revision", fact.revision) ||
        !text(root.get(), "recordingId", fact.recording_id, 128))
        return WireResult::Invalid;
    const bool ready = fact.state == "ready";
    std::string token;
    const bool requesting = cJSON_GetObjectItemCaseSensitive(root.get(), "requestToken") != nullptr;
    if (requesting && !text(root.get(), "requestToken", token, 128))
        return WireResult::Invalid;
    // 主动暂停以原请求关联为事实，不能伪造自动原因；自动暂停和失败仍须给固定原因。
    // 保留带原因及请求关联的既有合法事件，空原因或额外字段仍由严格字段数拒绝。
    const bool has_reason = cJSON_GetObjectItemCaseSensitive(root.get(), "reason") != nullptr;
    const bool closed = fact.state == "failed" ||
                        (fact.state == "paused" && (!requesting || has_reason));
    const int expected = 3 + (requesting ? 1 : 0) + (ready ? 2 : 0) + (closed ? 1 : 0);
    if (fields != expected)
        return WireResult::Invalid;
    if (ready) {
        if (!text(root.get(), "contentRef", fact.content_ref, 512) || !content_ref(fact.content_ref) ||
            !integer(root.get(), "durationMs", fact.duration_ms) || fact.duration_ms == 0)
            return WireResult::Invalid;
    } else if (closed) {
        if (!text(root.get(), "reason", fact.reason, 32) || !recording_reason(fact.reason))
            return WireResult::Invalid;
    }
    if (requesting)
        fact.request_token = token;
    out = std::move(fact);
    return WireResult::Ok;
}
}  // namespace ur::device
