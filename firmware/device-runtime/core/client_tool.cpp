// 文件说明：审核本地主题工具的严格解析和原调用状态机；原scope与实际驱动观察缺一不可。
#include "ur/device/json_parse.hpp"
#include "ur/device/client_tool.hpp"
#include <cJSON.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

namespace ur::device {
namespace {
// Json 自动释放严格公共解析之后的有界对象，不将cJSON树暴露给执行驱动。
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
// identifier 检查固定原关联，不接受空值、控制字符或无界字符串。
bool identifier(std::string_view value, std::size_t maximum = 128) {
    return !value.empty() && value.size() <= maximum &&
           std::none_of(value.begin(), value.end(),
                        [](unsigned char byte) { return byte < 32 || byte == 127; });
}
// text 读取审核字段，拒绝数字/null冒充标识或枚举。
bool text(const cJSON* root, const char* key, std::string& value) {
    const auto* field = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsString(field))
        return false;
    value = field->valuestring;
    return identifier(value);
}
// same_call 比较原授权正文，不借新messageSeq或msgToken重建另一次副作用。
bool same_call(const ClientToolCommand& left, const ClientToolCommand& right) {
    return left.envelope.session == right.envelope.session && left.response == right.response &&
           left.call == right.call && left.name == right.name && left.theme == right.theme &&
           left.execution_token == right.execution_token && left.expires_at == right.expires_at;
}
}  // namespace
WireResult decode_client_tool_execute(WireRoute route, std::string_view body,
                                      ClientToolCommand& out) {
    out = {};
    if (route != WireRoute::Message)
        return WireResult::WrongRoute;
    ClientToolCommand command;
    auto& event = command.envelope;
    if (decode_envelope(body, event) != DecodeResult::Ok || event.request || event.code ||
        event.event_sequence || !event.message_sequence || !identifier(event.session, 64))
        return WireResult::Invalid;
    if (event.method != "toolCallExecute")
        return WireResult::Unsupported;
    Json data(ur::device::parse_json(event.payload.c_str()), cJSON_Delete);
    if (!data || cJSON_GetArraySize(data.get()) != 8)
        return WireResult::Invalid;
    std::string type;
    const auto* frontend = cJSON_GetObjectItemCaseSensitive(data.get(), "isFrontend");
    const auto* arguments = cJSON_GetObjectItemCaseSensitive(data.get(), "arguments");
    const auto* expiry = cJSON_GetObjectItemCaseSensitive(data.get(), "expiresAt");
    if (!text(data.get(), "respId", command.response) ||
        !text(data.get(), "toolCallId", command.call) || !text(data.get(), "name", command.name) ||
        command.name != "set_local_display_theme" || !text(data.get(), "interactionType", type) ||
        type != "action" || !cJSON_IsTrue(frontend) ||
        !text(data.get(), "executionToken", command.execution_token) ||
        !cJSON_IsObject(arguments) || cJSON_GetArraySize(arguments) != 1 ||
        !text(arguments, "theme", command.theme) ||
        (command.theme != "light" && command.theme != "dark") || !cJSON_IsNumber(expiry) ||
        !std::isfinite(expiry->valuedouble) || expiry->valuedouble <= 0 ||
        expiry->valuedouble > 9007199254740991.0 ||
        std::floor(expiry->valuedouble) != expiry->valuedouble)
        return WireResult::Invalid;
    command.expires_at = static_cast<std::uint64_t>(expiry->valuedouble);
    out = std::move(command);
    return WireResult::Ok;
}
ClientToolExecutor::ClientToolExecutor(ClientToolDisplayPort& display, std::string instance_id)
    : display_(display), instance_(std::move(instance_id)), valid_(identifier(instance_, 96)) {}
// accept 保留公共单调时钟／墙钟签名；前者用于本地截止，后者只验证平台绝对到期。
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
ClientToolDecision ClientToolExecutor::accept(const ClientToolCommand& command,
                                              const ClientToolScope& scope, std::uint64_t now,
                                              std::uint64_t wall_now) {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    poll(now);
    if (!valid_)
        return ClientToolDecision::Invalid;
    // 类型化调用也复用真正线协议检查，不能由宿主误传展示对象绕过审核字段。
    std::string body;
    ClientToolCommand checked;
    if (encode_envelope(command.envelope, body) != DecodeResult::Ok ||
        decode_client_tool_execute(WireRoute::Message, body, checked) != WireResult::Ok ||
        !same_call(command, checked))
        return ClientToolDecision::Invalid;
    if (!scope.established || scope.session != command.envelope.session ||
        scope.response != command.response || command.expires_at <= wall_now ||
        command.expires_at - wall_now > 60000)
        return ClientToolDecision::Denied;
    const auto key = std::make_pair(command.envelope.session, command.call);
    const auto previous = calls_.find(key);
    if (previous != calls_.end()) {
        if (!same_call(previous->second.command, command))
            return ClientToolDecision::Denied;
        if (previous->second.terminal)
            previous->second.emit = true;  // 仅重放原固定结果，不重启实际驱动。
        return ClientToolDecision::Replay;
    }
    if (calls_.size() >= 512 || generation_ == std::numeric_limits<std::uint64_t>::max())
        return ClientToolDecision::Denied;
    for (const auto& entry : calls_) {
        const auto& original = entry.second.command;
        if (original.envelope.session == command.envelope.session &&
            original.response == command.response && original.name == command.name &&
            original.theme == command.theme)
            return ClientToolDecision::Denied;  // 同回复换调用ID不能在unknown后自动补发同一副作用。
        if (!entry.second.terminal)
            return ClientToolDecision::Busy;  // 单末端不能覆盖原在途票据。
    }
    const auto budget = std::min<std::uint64_t>(2000, command.expires_at - wall_now);
    if (now > std::numeric_limits<std::uint64_t>::max() - budget)
        return ClientToolDecision::Invalid;
    auto& call =
        calls_.emplace(key, Call{command, ++generation_, now + budget, std::nullopt, false})
            .first->second;
    if (!display_.request_theme(command.theme, call.generation))
        finish(call, "failed");
    return ClientToolDecision::Accepted;
}
void ClientToolExecutor::poll(std::uint64_t now) {
    if (clock_ && now < *clock_)
        valid_ = false;
    clock_ = now;
    for (auto& entry : calls_) {
        auto& call = entry.second;
        if (call.terminal)
            continue;
        if (!valid_ || now >= call.deadline) {
            finish(call, "unknown");
            continue;
        }
        const auto fact = display_.take_theme_fact(call.generation);
        if (!fact || fact->generation != call.generation)
            continue;
        if (fact->status == "succeeded" && fact->theme == call.command.theme)
            finish(call, "succeeded");
        else if (fact->status == "failed" && fact->theme.empty())
            finish(call, "failed");
        else
            finish(call, "unknown");  // 错实际主题或未定义状态不能伪造确定成功/失败。
    }
}
std::optional<ClientToolSubmission> ClientToolExecutor::take_submission() {
    for (auto& entry : calls_) {
        auto& call = entry.second;
        if (call.emit && call.terminal) {
            call.emit = false;
            return call.terminal;
        }
    }
    return std::nullopt;
}
void ClientToolExecutor::cancel_response(std::string_view session, std::string_view response) {
    for (auto& entry : calls_) {
        auto& call = entry.second;
        if (!call.terminal && call.command.envelope.session == session &&
            call.command.response == response)
            finish(call, "unknown");
    }
}
void ClientToolExecutor::close_session(std::string_view session) {
    for (auto& entry : calls_) {
        auto& call = entry.second;
        if (!call.terminal && call.command.envelope.session == session)
            finish(call, "unknown");
    }
}
void ClientToolExecutor::finish(Call& call, std::string_view status) {
    if (call.terminal)
        return;
    // 所有自由参数已在原Execute闭合校验；cJSON负责转义令牌，不能手工拼接身份/凭据。
    Json data(cJSON_CreateObject(), cJSON_Delete);
    if (!data)
        return;
    cJSON_AddStringToObject(data.get(), "respId", call.command.response.c_str());
    cJSON_AddStringToObject(data.get(), "toolCallId", call.command.call.c_str());
    cJSON_AddStringToObject(data.get(), "executionToken", call.command.execution_token.c_str());
    cJSON_AddStringToObject(data.get(), "status", std::string(status).c_str());
    auto* result = cJSON_AddObjectToObject(data.get(), "result");
    if (!result)
        return;
    cJSON_AddStringToObject(result, status == "succeeded" ? "theme" : "reason",
                            status == "succeeded" ? call.command.theme.c_str()
                            : status == "failed"  ? "executionFailed"
                                                  : "outcomeUnknown");
    char* raw = cJSON_PrintUnformatted(data.get());
    if (!raw)
        return;
    Envelope submit;
    submit.token = instance_ + "-client-" + std::to_string(call.generation);
    submit.method = "toolCallSubmit";
    submit.session = call.command.envelope.session;
    const auto decoded = canonical_json_object(raw, submit.payload);
    cJSON_free(raw);
    if (decoded != DecodeResult::Ok) {
        valid_ = false;
        return;
    }
    call.terminal = ClientToolSubmission{std::move(submit), std::string(status), call.generation};
    // 所有唯一终态都撤销原末端代次；只封待发命令/迟到事实，不声称已发DOM副作用回滚。
    display_.retire_theme(call.generation);
    call.emit = true;
}
}  // namespace ur::device
