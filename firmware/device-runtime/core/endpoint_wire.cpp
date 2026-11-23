// 文件说明：跨端接续的有界JSON校验；不执行原子转移、不分配连接或撤销旧端发送权限。
#include "ur/device/endpoint_wire.hpp"
#include "ur/device/json_parse.hpp"
#include <cJSON.h>
#include <algorithm>
#include <cmath>
#include <memory>

namespace ur::device {
namespace {
using Json = std::unique_ptr<cJSON,
                             decltype(&cJSON_Delete)>;       // 所有失败出口自动释放解析树。
constexpr std::uint64_t safe_integer = 9007199254740991ULL;  // MQTT跨语言安全整数上界。
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
// one_of 校验封闭枚举，不把自由文字或未实现操作解释为接续指令。
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
// endpoint_reason 仅允许固定公开原因；内部调度或媒体错误原文由平台脱敏后丢弃。
bool endpoint_reason(std::string_view value) {
    return one_of(value, {"handoffTimeout", "userCancelled", "mediaFailed", "permissionRevoked", "sourceChanged"});
}
}  // namespace

// decode_endpoint_control
// 将指定通道的原JSON解析为完整接续请求；任一非法字段都清空输出并返回失败。
WireResult decode_endpoint_control(WireRoute route, std::string_view json, EndpointCommand& out) {
    out = {};
    if (route != WireRoute::Signaling)
        return WireResult::WrongRoute;
    EndpointCommand command;
    // 上行请求不携带应答或平台事件水位，与focus下行控制的方向约束相反。
    if (decode_envelope(json, command.envelope) != DecodeResult::Ok ||
        command.envelope.method != "endpointControl" || !command.envelope.request ||
        command.envelope.session.empty() || command.envelope.session.size() > 64 ||
        command.envelope.code || command.envelope.event_sequence ||
        command.envelope.message_sequence)
        return WireResult::Invalid;
    Json root(ur::device::parse_json(command.envelope.payload.c_str()), cJSON_Delete);
    if (!root || !text(root.get(), "operation", command.operation, 16) ||
        !one_of(command.operation, {"prepare", "commit", "abort"}) ||
        !integer(root.get(), "expectedRevision", command.expected_revision) ||
        !text(root.get(), "handoffId", command.handoff_id, 128))
        return WireResult::Invalid;
    const auto* target = cJSON_GetObjectItemCaseSensitive(root.get(), "target");
    const auto* candidate = cJSON_GetObjectItemCaseSensitive(root.get(), "candidateSessionId");
    const bool prepare = command.operation == "prepare";
    const bool commit = command.operation == "commit";
    // 条件字段只能由所属操作携带；abort不得夹带，防止把多种副作用合并到同一接续事务。
    if ((target != nullptr) != prepare || (candidate != nullptr) != commit ||
        cJSON_GetArraySize(root.get()) != (prepare || commit ? 4 : 3))
        return WireResult::Invalid;
    if (prepare) {
        // Party对象只允许kind/id两键；显式null不是缺省，kind固定endpoint。
        const auto* kind = cJSON_IsObject(target)
                               ? cJSON_GetObjectItemCaseSensitive(target, "kind")
                               : nullptr;
        const auto* id = cJSON_IsObject(target)
                             ? cJSON_GetObjectItemCaseSensitive(target, "id")
                             : nullptr;
        if (!kind || !id || cJSON_GetArraySize(target) != 2 || !cJSON_IsString(kind) ||
            !cJSON_IsString(id) || std::string_view(kind->valuestring) != "endpoint")
            return WireResult::Invalid;
        command.target = EndpointTarget{kind->valuestring, id->valuestring};
        // target.id与Go ValidID保持同一非空/长度/控制字符边界，不把空端当作合法路由目标。
        if (command.target->id.empty() || command.target->id.size() > 128 ||
            std::any_of(command.target->id.begin(), command.target->id.end(),
                        [](unsigned char ch) { return ch < 32 || ch == 127; }))
            return WireResult::Invalid;
    }
    if (commit &&
        (!text(root.get(), "candidateSessionId", command.candidate_session_id, 128)))
        return WireResult::Invalid;
    out = std::move(command);
    return WireResult::Ok;
}

// decode_endpoint_state
// 校验平台下行事实的形态、reason白名单与候选腿条件；不能仅凭code200推断接续完成。
WireResult decode_endpoint_state(WireRoute route, std::string_view json, EndpointState& out) {
    out = {};
    if (route != WireRoute::Signaling)
        return WireResult::WrongRoute;
    EndpointState state;
    if (decode_envelope(json, state.envelope) != DecodeResult::Ok ||
        state.envelope.method != "endpointState" || state.envelope.request ||
        state.envelope.session.empty() || state.envelope.session.size() > 64 ||
        state.envelope.message_sequence)
        return WireResult::Invalid;
    // 应答与主动事件互斥：code和事件水位必须恰好出现其一，不能互相伪装。
    const bool reply =
        state.envelope.code.has_value() && !state.envelope.event_sequence.has_value();
    const bool event =
        !state.envelope.code.has_value() && state.envelope.event_sequence.has_value();
    if (!reply && !event)
        return WireResult::Invalid;
    Json root(ur::device::parse_json(state.envelope.payload.c_str()), cJSON_Delete);
    if (!root || !text(root.get(), "state", state.state, 16) ||
        !one_of(state.state, {"preparing", "ready", "committed", "aborted", "failed"}) ||
        !integer(root.get(), "revision", state.revision) ||
        !text(root.get(), "handoffId", state.handoff_id, 128))
        return WireResult::Invalid;
    const auto* candidate = cJSON_GetObjectItemCaseSensitive(root.get(), "candidateSessionId");
    const auto* reason = cJSON_GetObjectItemCaseSensitive(root.get(), "reason");
    const bool with_candidate = state.state == "ready" || state.state == "committed";
    const bool with_reason = state.state == "failed" || state.state == "aborted";
    // 候选腿只随ready/committed出现；aborted/failed保留原端，不能预指派目标腿。
    if ((candidate != nullptr) != with_candidate || (reason != nullptr) != with_reason ||
        cJSON_GetArraySize(root.get()) != (with_candidate || with_reason ? 4 : 3))
        return WireResult::Invalid;
    if (with_candidate &&
        !text(root.get(), "candidateSessionId", state.candidate_session_id, 128))
        return WireResult::Invalid;
    if (with_reason &&
        (!text(root.get(), "reason", state.reason, 32) || !endpoint_reason(state.reason)))
        return WireResult::Invalid;
    // failed可由超时应答或主动事件表达；成功语义不允许非200应答，防止202冒充已提交。
    if (state.state == "failed") {
        if (state.envelope.code && *state.envelope.code < 400)
            return WireResult::Invalid;
    } else if (state.envelope.code && *state.envelope.code != 200)
        return WireResult::Invalid;
    out = std::move(state);
    return WireResult::Ok;
}
}  // namespace ur::device
