// 文件说明：原内容scope先行，独立Executor实际完成后才Submit；结果重试原字节不越原期限/取消。
#include "ur/device/json_parse.hpp"
#include "ur/device/client_tool_endpoint.hpp"
#include <cJSON.h>
#include <limits>
#include <memory>

namespace ur::device {
namespace {
// field只读取固定ACK字段；公共解析已经拒绝重复键/null/超界正文。
std::string field(const cJSON* object, const char* key) {
    const auto* value = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(value) ? value->valuestring : "";
}
}  // namespace
// 构造保留已用公共签名：identity限定Topic，instance_id限定本机票据，二者不互作权限。
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
ClientToolEndpoint::ClientToolEndpoint(MqttPort& mqtt, ClientToolDisplayPort& display,
                                       std::string identity, std::string instance_id)
    : mqtt_(mqtt),
      topic_("$thing/up/message/" + identity),
      executor_(display, std::move(instance_id)),
      requests_(*this) {}
// NOLINTEND(bugprone-easily-swappable-parameters)
bool ClientToolEndpoint::observe(const ContentEvent& content) {
    if (content.envelope.method == "respCreated") {
        if (scopes_.find(content.response_id) != scopes_.end())
            return false;
        if (scopes_.size() >= 512)
            return false;
        scopes_.emplace(content.response_id, Scope{content.envelope.session, {}, false});
    } else if (content.envelope.method == "toolCallStart") {
        auto scope = scopes_.find(content.response_id);
        if (scope == scopes_.end() || scope->second.closed ||
            scope->second.session != content.envelope.session)
            return false;
        auto& tools = scope->second.tools;
        const auto previous = tools.find(content.tool_call_id);
        if (previous != tools.end())
            return previous->second.name == content.tool_name;
        if (tools.size() >= 64)
            return false;
        tools.emplace(content.tool_call_id, Scope::Tool{content.tool_name, false});
    } else if (content.envelope.method == "toolCallResult") {
        const auto scope = scopes_.find(content.response_id);
        if (scope == scopes_.end() || scope->second.session != content.envelope.session)
            return false;
        const auto tool = scope->second.tools.find(content.tool_call_id);
        if (tool == scope->second.tools.end())
            return false;
        tool->second.done = true;
    } else if (content.envelope.method == "respDone") {
        const auto scope = scopes_.find(content.response_id);
        if (scope != scopes_.end() && scope->second.session == content.envelope.session) {
            scope->second.closed = true;
            executor_.cancel_response(content.envelope.session, content.response_id);
        }
    }
    return true;
}
// receive_execute 保留公共正文／会话接口；先严格解码body，再独立核对session绑定。
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
ClientToolDecision ClientToolEndpoint::receive_execute(std::string_view body,
                                                       std::string_view session, bool established,
                                                       std::uint64_t now, std::uint64_t wall) {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    if (!valid_ || now < now_)
        return ClientToolDecision::Invalid;
    now_ = now;
    ClientToolCommand command;
    if (decode_client_tool_execute(WireRoute::Message, body, command) != WireResult::Ok)
        return ClientToolDecision::Invalid;
    const auto scope = scopes_.find(command.response);
    if (!established || scope == scopes_.end() || scope->second.closed ||
        scope->second.session != session || command.envelope.session != session)
        return ClientToolDecision::Denied;
    const auto start = scope->second.tools.find(command.call);
    if (start == scope->second.tools.end() || start->second.done ||
        start->second.name != command.name || command.expires_at <= wall ||
        command.expires_at - wall > 60000 ||
        now > std::numeric_limits<std::uint64_t>::max() - (command.expires_at - wall))
        return ClientToolDecision::Denied;
    const auto decision =
        executor_.accept(command, {std::string(session), command.response, true}, now, wall);
    if (decision == ClientToolDecision::Accepted)
        deadlines_.emplace(std::make_pair(command.envelope.session, command.call),
                           now + command.expires_at - wall);
    return decision;
}
bool ClientToolEndpoint::active(const Receipt& receipt) const {
    const auto scope = scopes_.find(receipt.response);
    return valid_ && now_ < receipt.deadline && scope != scopes_.end() && !scope->second.closed &&
           scope->second.session == receipt.session;
}
bool ClientToolEndpoint::publish(std::string_view topic, std::string_view body) {
    Envelope request;
    if (topic != topic_ || decode_envelope(body, request) != DecodeResult::Ok ||
        request.method != "toolCallSubmit")
        return false;
    const auto original = receipts_.find(request.token);
    return original != receipts_.end() && request.session == original->second.session &&
           active(original->second) && mqtt_.publish(topic, body);
}
bool ClientToolEndpoint::poll(std::uint64_t now) {
    if (now < now_)
        valid_ = false;
    now_ = now;
    executor_.poll(now);
    for (unsigned count = 0; count < 32; ++count) {
        const auto submission = executor_.take_submission();
        if (!submission)
            break;
        std::unique_ptr<cJSON, decltype(&cJSON_Delete)> data(
            ur::device::parse_json(submission->envelope.payload.c_str()), cJSON_Delete);
        if (!data)
            return false;
        const auto response = field(data.get(), "respId"), call = field(data.get(), "toolCallId");
        const auto deadline = deadlines_.find({submission->envelope.session, call});
        if (deadline == deadlines_.end())
            return false;
        Receipt receipt{submission->envelope.session, response, call, deadline->second};
        if (!active(receipt))
            continue;  // 原终态/截止先于结果发布；已排队传输不能延长授权或复活回复。
        const auto previous = receipts_.find(submission->envelope.token);
        if (previous == receipts_.end()) {
            if (receipts_.size() >= 512)
                return false;
            receipts_.emplace(submission->envelope.token, std::move(receipt));
        }
        std::string body;
        if (encode_envelope(submission->envelope, body) != DecodeResult::Ok)
            return false;
        const auto submitted =
            requests_.submit({submission->envelope.token, submission->envelope.session,
                              "toolCallSubmit", topic_, body},
                             now);
        if (submitted != SubmitResult::Accepted && submitted != SubmitResult::Duplicate)
            return false;
    }
    requests_.tick(now);  // Requests内部可保留未知，但真正发布仍过原deadline/取消栅栏。
    return valid_;
}
bool ClientToolEndpoint::receive_ack(std::string_view body, std::uint64_t now) {
    if (now < now_)
        return false;
    now_ = now;
    Envelope ack;
    if (decode_envelope(body, ack) != DecodeResult::Ok || ack.method != "requestAck" ||
        ack.request || !ack.code || ack.message_sequence || ack.event_sequence)
        return false;
    const auto original = receipts_.find(ack.token);
    if (original == receipts_.end() || original->second.session != ack.session ||
        !active(original->second))
        return false;
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> data(ur::device::parse_json(ack.payload.c_str()),
                                                         cJSON_Delete);
    if (!data || cJSON_GetArraySize(data.get()) != 2 ||
        field(data.get(), "requestMethod") != "toolCallSubmit")
        return false;
    if (*ack.code == 200) {
        const auto* result = cJSON_GetObjectItemCaseSensitive(data.get(), "result");
        const auto* continuation = cJSON_GetObjectItemCaseSensitive(result, "continuation");
        if (!cJSON_IsObject(result) || cJSON_GetArraySize(result) != (continuation ? 4 : 3) ||
            field(result, "respId") != original->second.response ||
            field(result, "toolCallId") != original->second.call ||
            field(result, "state") != "accepted" ||
            (continuation && field(result, "continuation") != "accepted" &&
             field(result, "continuation") != "unknown"))
            return false;
    } else if (*ack.code < 400 || *ack.code > 599 || field(data.get(), "reason").empty()) {
        return false;
    }
    if (observations_.full() ||
        !requests_.acknowledge({ack.token, ack.session, "toolCallSubmit", *ack.code, topic_}, now))
        return false;
    const auto* result = cJSON_GetObjectItemCaseSensitive(data.get(), "result");
    const auto continuation = field(result, "continuation");
    return observations_.push_back(
        {original->second.session, original->second.response, original->second.call,
         continuation == "accepted" ? "accepted" : "unknown", *ack.code, ++observation_index_});
}
std::optional<ClientToolReceipt> ClientToolEndpoint::take_receipt() {
    if (observations_.empty())
        return std::nullopt;
    auto receipt = std::move(*observations_.front());
    observations_.pop_front();
    return receipt;
}
void ClientToolEndpoint::cancel_response(std::string_view session, std::string_view response) {
    const auto scope = scopes_.find(std::string(response));
    if (scope == scopes_.end() || scope->second.session != session)
        return;
    scope->second.closed = true;
    scope->second.cancelled = true;
    executor_.cancel_response(session, response);
}
// response_cancelled 不新增墓碑；同名跨会话不能借原取消事实阻止新授权。
// 保留已用session/response接口：response定位原scope，session独立核验所属会话，不能交换权限。
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
bool ClientToolEndpoint::response_cancelled(std::string_view session,
                                            std::string_view response) const {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    const auto scope = scopes_.find(std::string(response));
    return scope != scopes_.end() && scope->second.session == session && scope->second.cancelled;
}
void ClientToolEndpoint::close_session(std::string_view session, std::uint64_t now) {
    for (auto& scope : scopes_)
        if (scope.second.session == session)
            scope.second.closed = true;
    executor_.close_session(session);
    requests_.cancel_session(session, now);
}
void ClientToolEndpoint::close(std::uint64_t now) {
    observations_.clear();  // 连接退休后不将历史ACK观察转交新页面。
    for (auto& scope : scopes_) {
        scope.second.closed = true;
        executor_.close_session(scope.second.session);
        requests_.cancel_session(scope.second.session, now);
    }
}
}  // namespace ur::device
