// 文件说明：资源焦点的有界JSON校验与原请求关联；不分配驱动、不实现优先级仲裁或自动恢复。
#include "ur/device/json_parse.hpp"
#include "ur/device/focus_wire.hpp"
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
// one_of 校验封闭枚举，不把模型自由文字或未实现资源名称解释为指令。
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
// resources
// 只允许文档资源名及有界摄像头ID，保留原顺序；最多16项且不允许空集或重复项。
bool resources(const cJSON* root, std::vector<std::string>& out, const char* key = "resources") {
    const auto* values = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsArray(values) || cJSON_GetArraySize(values) < 1 || cJSON_GetArraySize(values) > 16)
        return false;
    for (const auto* item = values->child; item; item = item->next) {
        if (!cJSON_IsString(item))
            return false;
        const std::string value = item->valuestring;
        const bool camera = value.rfind("camera:", 0) == 0 && value.size() > 7 &&
                            value.size() <= 128 &&
                            std::none_of(value.begin() + 7, value.end(), [](unsigned char ch) {
                                return ch <= 32 || ch == 127 || ch == '/' || ch == '\\';
                            });
        if ((!one_of(value, {"microphone", "speaker", "display"}) && !camera) ||
            std::find(out.begin(), out.end(), value) != out.end())
            return false;
        out.push_back(value);
    }
    return true;
}
// reason
// 仅允许固定公开原因；错误原文由驱动宿主脱敏，不能通过信令带出URL或凭据。
bool reason(std::string_view value) {
    return one_of(value, {"permissionDenied", "resourceBusy", "unsupportedResource",
                          "staleRevision", "leaseExpired", "invalidState", "userIntentChanged",
                          "sessionEnded", "timeout", "driverFailed"});
}
// group_equal
// 资源集合不要求排序，但成功或拒绝都必须包含原完整组，禁止缩减为speaker。
bool group_equal(const std::vector<std::string>& left, const std::vector<std::string>& right) {
    return left.size() == right.size() &&
           std::all_of(left.begin(), left.end(), [&](const auto& value) {
               return std::find(right.begin(), right.end(), value) != right.end();
           });
}
// cameras_declared
// 要求每个相机资源来自同一全量能力，不因有视频输出而推断采集能力。
bool cameras_declared(const cJSON* root, const std::vector<std::string>& group) {
    for (const auto& value : group) {
        if (value.rfind("camera:", 0) != 0)
            continue;
        const auto* cameras = cJSON_GetObjectItemCaseSensitive(root, "cameras");
        bool found = false;  // cameraId与trackId/sessionId不同，不允许跨快照借用。
        if (cJSON_IsArray(cameras)) {
            for (const auto* camera = cameras->child; camera; camera = camera->next) {
                const auto* id = cJSON_GetObjectItemCaseSensitive(camera, "cameraId");
                if (cJSON_IsString(id) && value.substr(7) == id->valuestring)
                    found = true;
            }
        }
        if (!found)
            return false;
    }
    return true;
}
// exact_integers 保留临时树中安全整数的精确字面量，避免中间打印近似资源水位。
// 最终信封规范化无法恢复已经舍入的数字；只在字段校验结束后变更临时树。
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
// requested_state 成功结果必须与原动作一致；不是收到任意code200就算完成。
std::string_view requested_state(std::string_view operation) {
    if (operation == "acquire")
        return "acquired";
    if (operation == "suspend")
        return "suspended";
    if (operation == "restore")
        return "restored";
    if (operation == "release")
        return "released";
    return {};
}
// valid_command
// 防止直接修改公共结构体后绕过原信封；原请求字段必须与严格解析结果一致。
bool valid_command(const FocusCommand& command) {
    std::string json;
    FocusCommand checked;
    return encode_envelope(command.envelope, json) == DecodeResult::Ok &&
           decode_focus_control(WireRoute::Signaling, json, checked) == WireResult::Ok &&
           checked.operation == command.operation &&
           checked.expected_revision == command.expected_revision &&
           checked.focus_id == command.focus_id && checked.resources == command.resources &&
           checked.priority == command.priority && checked.resume_policy == command.resume_policy &&
           checked.expires_at == command.expires_at;
}
// valid_state
// 同样复核原实际报文，避免半组、任意错误文字或伪造method通过独立关联入口。
bool valid_state(const FocusState& state) {
    std::string json;
    FocusState checked;
    return encode_envelope(state.envelope, json) == DecodeResult::Ok &&
           decode_focus_state(WireRoute::Signaling, json, checked) == WireResult::Ok &&
           checked.state == state.state && checked.revision == state.revision &&
           checked.focus_id == state.focus_id && checked.resources == state.resources &&
           checked.reason == state.reason;
}
}  // namespace

// decode_focus_control
// 将指定通道的原JSON解析为完整焦点请求；任一非法字段都清空输出并返回失败。
WireResult decode_focus_control(WireRoute route, std::string_view json, FocusCommand& out) {
    out = {};
    if (route != WireRoute::Signaling)
        return WireResult::WrongRoute;
    FocusCommand command;
    if (decode_envelope(json, command.envelope) != DecodeResult::Ok ||
        command.envelope.method != "focusControl" || !command.envelope.request ||
        command.envelope.session.empty() || command.envelope.session.size() > 64 ||
        !command.envelope.event_sequence || command.envelope.code ||
        command.envelope.message_sequence)
        return WireResult::Invalid;
    Json root(ur::device::parse_json(command.envelope.payload.c_str()), cJSON_Delete);
    if (!root || !text(root.get(), "operation", command.operation, 16) ||
        !one_of(command.operation, {"acquire", "suspend", "restore", "release"}) ||
        !integer(root.get(), "expectedRevision", command.expected_revision) ||
        !text(root.get(), "focusId", command.focus_id, 128) ||
        !resources(root.get(), command.resources) ||
        !text(root.get(), "priority", command.priority, 16) ||
        !one_of(command.priority, {"background", "conversation", "call", "emergency"}) ||
        !text(root.get(), "resumePolicy", command.resume_policy, 16) ||
        !one_of(command.resume_policy, {"never", "explicit", "ifUnchanged"}))
        return WireResult::Invalid;
    const bool expiring = command.operation == "acquire" || command.operation == "restore";
    if (cJSON_GetArraySize(root.get()) != (expiring ? 7 : 6))
        return WireResult::Invalid;
    if (expiring) {
        std::uint64_t expires = 0;
        if (!integer(root.get(), "expiresAt", expires) || expires == 0)
            return WireResult::Invalid;
        command.expires_at = expires;
    } else if (cJSON_GetObjectItemCaseSensitive(root.get(), "expiresAt"))
        return WireResult::Invalid;
    out = std::move(command);
    return WireResult::Ok;
}

// decode_focus_state
// 校验实际事实的资源组、修订号和固定原因；不能仅凭code200推断授权。
WireResult decode_focus_state(WireRoute route, std::string_view json, FocusState& out) {
    out = {};
    if (route != WireRoute::Signaling)
        return WireResult::WrongRoute;
    FocusState state;
    if (decode_envelope(json, state.envelope) != DecodeResult::Ok ||
        state.envelope.method != "focusState" || state.envelope.request ||
        state.envelope.session.empty() || state.envelope.session.size() > 64 ||
        state.envelope.event_sequence || state.envelope.message_sequence)
        return WireResult::Invalid;
    Json root(ur::device::parse_json(state.envelope.payload.c_str()), cJSON_Delete);
    if (!root || !text(root.get(), "state", state.state, 16) ||
        !one_of(state.state, {"acquired", "suspended", "restored", "released", "denied"}) ||
        !integer(root.get(), "revision", state.revision) ||
        !text(root.get(), "focusId", state.focus_id, 128) ||
        !resources(root.get(), state.resources))
        return WireResult::Invalid;
    const bool denied = state.state == "denied";
    if (cJSON_GetArraySize(root.get()) != (denied ? 5 : 4))
        return WireResult::Invalid;
    if (denied) {
        if (!text(root.get(), "reason", state.reason, 32) || !reason(state.reason) ||
            !state.envelope.code || *state.envelope.code < 400 || *state.envelope.code > 599)
            return WireResult::Invalid;
    } else if (cJSON_GetObjectItemCaseSensitive(root.get(), "reason") ||
               (state.envelope.code && *state.envelope.code != 200))
        return WireResult::Invalid;
    out = std::move(state);
    return WireResult::Ok;
}

// focus_reply_matches
// 复核原请求与原事实全字段，只返回原操作是否精确完成，主动事件不能代替回执。
bool focus_reply_matches(const FocusCommand& command, const FocusState& state) {
    if (!valid_command(command) || !valid_state(state) || !state.envelope.code ||
        state.envelope.token != command.envelope.token ||
        state.envelope.session != command.envelope.session || state.focus_id != command.focus_id ||
        !group_equal(command.resources, state.resources))
        return false;
    // 拒绝不推进资源修订号，但staleRevision必须报告设备实际当前值，未必等于过期请求值。
    if (state.state == "denied")
        return *state.envelope.code >= 400 && *state.envelope.code <= 599 && reason(state.reason) &&
               state.revision <= safe_integer;
    return *state.envelope.code == 200 && state.state == requested_state(command.operation) &&
           command.expected_revision < safe_integer &&
           state.revision == command.expected_revision + 1;
}

// focus_reply
// 从已知实际结果生成原关联应答；失败清空输出，不执行资源动作或生成默认成功。
// 保留 command/state/revision/code/why 编码接口；原命令关联不变，成功编码实态修订，失败编码原因。
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
WireResult focus_reply(const FocusCommand& command, std::string_view state, std::uint64_t revision,
                       int code, std::string_view why, Envelope& out) {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    out = {};
    // 公共结构体可能由宿主直接构造，必须再次严格解析，不能绕过枚举/资源去重等不可信边界。
    if (!valid_command(command))
        return WireResult::Invalid;
    Json root(cJSON_CreateObject(), cJSON_Delete);
    if (!root || !cJSON_AddStringToObject(root.get(), "state", std::string(state).c_str()) ||
        !cJSON_AddNumberToObject(root.get(), "revision", static_cast<double>(revision)) ||
        !cJSON_AddStringToObject(root.get(), "focusId", command.focus_id.c_str()))
        return WireResult::Invalid;
    auto* values = cJSON_AddArrayToObject(root.get(), "resources");
    if (!values)
        return WireResult::Invalid;
    for (const auto& resource : command.resources)
        if (!cJSON_AddItemToArray(values, cJSON_CreateString(resource.c_str())))
            return WireResult::Invalid;
    if (!why.empty() && !cJSON_AddStringToObject(root.get(), "reason", std::string(why).c_str()))
        return WireResult::Invalid;
    if (!exact_integers(root.get()))
        return WireResult::Invalid;
    char* body = cJSON_PrintUnformatted(root.get());
    if (!body)
        return WireResult::Invalid;
    Envelope reply;
    reply.token = command.envelope.token;
    reply.session = command.envelope.session;
    reply.method = "focusState";
    reply.request = false;
    reply.code = code;
    reply.payload = body;
    cJSON_free(body);
    std::string json;
    FocusState fact;
    if (revision > safe_integer || encode_envelope(reply, json) != DecodeResult::Ok ||
        decode_focus_state(WireRoute::Signaling, json, fact) != WireResult::Ok ||
        !focus_reply_matches(command, fact))
        return WireResult::Invalid;
    out = std::move(reply);
    return WireResult::Ok;
}
// add_focus_capability
// 仅附加宿主明确声明的完整资源组，不覆盖既有控制声明或推断权限。
WireResult add_focus_capability(const Envelope& source, const std::vector<std::string>& group,
                                Envelope& out) {
    // 先保存输入，允许宿主原地扩充；失败始终清空输出而不保留半份能力。
    std::string original;
    const auto encoded = encode_envelope(source, original);
    out = {};
    Envelope checked;
    if (encoded != DecodeResult::Ok || decode_envelope(original, checked) != DecodeResult::Ok ||
        checked.method != "capabilityReport" || !checked.request || !checked.session.empty() ||
        checked.code || checked.event_sequence || checked.message_sequence || group.empty() ||
        group.size() > 16)
        return WireResult::Invalid;
    Json root(ur::device::parse_json(checked.payload.c_str()), cJSON_Delete);
    if (!root || cJSON_GetObjectItemCaseSensitive(root.get(), "controlCapabilities"))
        return WireResult::Invalid;
    auto* controls = cJSON_AddObjectToObject(root.get(), "controlCapabilities");
    auto* operations = controls ? cJSON_AddArrayToObject(controls, "operations") : nullptr;
    if (!operations || !cJSON_AddItemToArray(operations, cJSON_CreateString("focusControl")))
        return WireResult::Invalid;
    auto* values = cJSON_AddArrayToObject(controls, "focusResources");
    if (!values)
        return WireResult::Invalid;
    for (const auto& value : group) {
        // cJSON使用零结尾字符串，先拒绝内嵌NUL，避免截断后误报合法资源。
        if (value.find('\0') != std::string::npos ||
            !cJSON_AddItemToArray(values, cJSON_CreateString(value.c_str())))
            return WireResult::Invalid;
    }
    std::vector<std::string> validated;
    if (!resources(controls, validated, "focusResources"))
        return WireResult::Invalid;
    if (!cameras_declared(root.get(), validated))
        return WireResult::Invalid;
    if (!exact_integers(root.get()))
        return WireResult::Invalid;
    std::unique_ptr<char, decltype(&cJSON_free)> body(cJSON_PrintUnformatted(root.get()),
                                                      cJSON_free);
    if (!body)
        return WireResult::Invalid;
    checked.payload = body.get();
    std::string json;
    if (encode_envelope(checked, json) != DecodeResult::Ok ||
        decode_envelope(json, out) != DecodeResult::Ok) {
        out = {};
        return WireResult::Invalid;
    }
    return WireResult::Ok;
}
// add_focus_baseline 仅编码完整原组的首次证明，实际事实由宿主驱动独立负责。
WireResult add_focus_baseline(const Envelope& source,
                              const std::vector<std::string>& idle_resources,
                              std::uint64_t revision, Envelope& out) {
    std::string original;  // 保存原输入后清输出，支持原地组合且任何失败不留下半份证明。
    const auto encoded = encode_envelope(source, original);
    out = {};
    Envelope checked;
    if (revision > safe_integer || idle_resources.empty() || idle_resources.size() > 16 ||
        encoded != DecodeResult::Ok || decode_envelope(original, checked) != DecodeResult::Ok ||
        checked.method != "capabilityReport" || !checked.request || !checked.session.empty() ||
        checked.code || checked.event_sequence || checked.message_sequence)
        return WireResult::Invalid;
    Json root(ur::device::parse_json(checked.payload.c_str()), cJSON_Delete);
    auto* controls =
        root ? cJSON_GetObjectItemCaseSensitive(root.get(), "controlCapabilities") : nullptr;
    const auto* operations =
        controls ? cJSON_GetObjectItemCaseSensitive(controls, "operations") : nullptr;
    const auto* operation = cJSON_IsArray(operations) ? cJSON_GetArrayItem(operations, 0) : nullptr;
    std::vector<std::string> declared;  // 必须是已声明的完整组，而非当前只空闲的子集。
    if (!cJSON_IsObject(controls) || cJSON_GetArraySize(controls) != 2 ||
        !cJSON_IsArray(operations) || cJSON_GetArraySize(operations) != 1 ||
        !cJSON_IsString(operation) || std::string_view(operation->valuestring) != "focusControl" ||
        !resources(controls, declared, "focusResources") || !cameras_declared(root.get(), declared))
        return WireResult::Invalid;
    auto* baseline = cJSON_AddObjectToObject(controls, "baseline");
    if (!baseline || !cJSON_AddStringToObject(baseline, "state", "idle") ||
        !cJSON_AddNumberToObject(baseline, "revision", static_cast<double>(revision)))
        return WireResult::Invalid;
    auto* values = cJSON_AddArrayToObject(baseline, "resources");
    if (!values)
        return WireResult::Invalid;
    for (const auto& value : idle_resources) {
        if (value.find('\0') != std::string::npos ||
            !cJSON_AddItemToArray(values, cJSON_CreateString(value.c_str())))
            return WireResult::Invalid;
    }
    std::vector<std::string> actual;  // 编码后再验证非空/去重/有界和原组，不能靠集合包含忽略重复。
    if (!resources(baseline, actual) || !group_equal(declared, actual))
        return WireResult::Invalid;
    if (!exact_integers(root.get()))
        return WireResult::Invalid;
    std::unique_ptr<char, decltype(&cJSON_free)> body(cJSON_PrintUnformatted(root.get()),
                                                      cJSON_free);
    if (!body)
        return WireResult::Invalid;
    checked.payload = body.get();
    std::string json;
    if (encode_envelope(checked, json) != DecodeResult::Ok ||
        decode_envelope(json, out) != DecodeResult::Ok) {
        out = {};
        return WireResult::Invalid;
    }
    return WireResult::Ok;
}
}  // namespace ur::device
