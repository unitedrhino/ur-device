// 文件说明：本地显示主题票据回执；只有固定真实网页驱动事实能结束原客户端工具等待。
#include "ur/device/json_parse.hpp"
#include "browser_client_tool.hpp"
#include <cJSON.h>
#include <memory>
#include "ur/device/client_tool_endpoint.hpp"
#include "ur/device/envelope.hpp"

namespace ur::device {
std::string browser_client_tool_submitted(const ClientToolReceipt& receipt) {
    if (receipt.session.empty() || receipt.session.size() > 64 || receipt.response.empty() ||
        receipt.response.size() > 128 || receipt.call.empty() || receipt.call.size() > 128 ||
        receipt.index == 0 || receipt.index > 512 ||
        (receipt.code != 200 && (receipt.code < 400 || receipt.code > 599)) ||
        (receipt.continuation != "accepted" && receipt.continuation != "unknown") ||
        (receipt.code != 200 && receipt.continuation != "unknown"))
        return {};
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> data(cJSON_CreateObject(), cJSON_Delete);
    if (!data || !cJSON_AddStringToObject(data.get(), "kind", "clientToolSubmitted") ||
        !cJSON_AddStringToObject(data.get(), "sessionId", receipt.session.c_str()) ||
        !cJSON_AddStringToObject(data.get(), "responseId", receipt.response.c_str()) ||
        !cJSON_AddStringToObject(data.get(), "toolCallId", receipt.call.c_str()) ||
        !cJSON_AddStringToObject(data.get(), "requestMethod", "toolCallSubmit") ||
        !cJSON_AddStringToObject(data.get(), "continuation", receipt.continuation.c_str()) ||
        !cJSON_AddNumberToObject(data.get(), "code", receipt.code) ||
        !cJSON_AddNumberToObject(data.get(), "receiptIndex", static_cast<double>(receipt.index)))
        return {};
    char* encoded = cJSON_PrintUnformatted(data.get());
    if (!encoded)
        return {};
    std::string body(encoded);
    cJSON_free(encoded);
    return body;
}
void BrowserClientToolDisplay::ready(bool actual) {
    if (!closed_)
        ready_ = actual;
}
bool BrowserClientToolDisplay::receive_ready(std::string_view body) {
    if (ready_ || closed_ || body.size() > 128)
        return false;
    std::string canonical;
    if (canonical_json_object(body, canonical) != DecodeResult::Ok)
        return false;
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> data(ur::device::parse_json(canonical.c_str()),
                                                         cJSON_Delete);
    const auto* kind = cJSON_GetObjectItemCaseSensitive(data.get(), "kind");
    const auto* revision = cJSON_GetObjectItemCaseSensitive(data.get(), "schemaRevision");
    if (!data || cJSON_GetArraySize(data.get()) != 2 || !cJSON_IsString(kind) ||
        std::string_view(kind->valuestring) != "clientThemeReady" || !cJSON_IsNumber(revision) ||
        revision->valuedouble != 1)
        return false;
    ready_ = true;
    return true;
}
bool BrowserClientToolDisplay::request_theme(std::string_view theme, std::uint64_t generation) {
    if (!available() || command_ || generation_ || fact_ || generation == 0 ||
        (theme != "light" && theme != "dark"))
        return false;
    generation_ = generation;
    theme_ = theme;
    command_ = "{\"kind\":\"clientThemeSet\",\"themeTicket\":\"" + std::to_string(generation) +
               "\",\"theme\":\"" + theme_ + "\"}";
    return true;
}
std::optional<std::string> BrowserClientToolDisplay::take_command() {
    auto result = std::move(command_);
    command_.reset();
    return result;
}
bool BrowserClientToolDisplay::receive(std::string_view body) {
    if (!available() || !generation_ || command_ || fact_ || body.size() > 256)
        return false;
    std::string canonical;
    if (canonical_json_object(body, canonical) != DecodeResult::Ok)
        return false;
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> data(ur::device::parse_json(canonical.c_str()),
                                                         cJSON_Delete);
    if (!data || cJSON_GetArraySize(data.get()) != 4)
        return false;
    const auto* kind = cJSON_GetObjectItemCaseSensitive(data.get(), "kind");
    const auto* ticket = cJSON_GetObjectItemCaseSensitive(data.get(), "themeTicket");
    const auto* state = cJSON_GetObjectItemCaseSensitive(data.get(), "status");
    const auto* theme = cJSON_GetObjectItemCaseSensitive(data.get(), "theme");
    if (!cJSON_IsString(kind) || std::string_view(kind->valuestring) != "clientThemeActual" ||
        !cJSON_IsString(ticket) ||
        std::string_view(ticket->valuestring) != std::to_string(*generation_) ||
        !cJSON_IsString(state) || !cJSON_IsString(theme))
        return false;
    const std::string status = state->valuestring;
    const std::string actual = theme->valuestring;
    if ((status != "succeeded" && status != "failed" && status != "unknown") ||
        (status == "succeeded" && actual != "light" && actual != "dark") ||
        (status != "succeeded" && !actual.empty()))
        return false;
    fact_ = ClientToolFact{*generation_, status, actual};
    return true;
}
std::optional<ClientToolFact> BrowserClientToolDisplay::take_theme_fact(std::uint64_t generation) {
    if (!fact_ || fact_->generation != generation)
        return std::nullopt;
    auto result = std::move(fact_);
    fact_.reset();
    generation_.reset();
    theme_.clear();
    return result;
}
void BrowserClientToolDisplay::retire_theme(std::uint64_t generation) {
    if (!generation_ || *generation_ != generation)
        return;
    // 尚未派发时撤销命令；已经派发时只能隔离迟到回执，不假称DOM未执行或已回滚。
    command_.reset();
    fact_.reset();
    generation_.reset();
    theme_.clear();
}
void BrowserClientToolDisplay::close() {
    closed_ = true;
    ready_ = false;
    command_.reset();
    if (generation_ && !fact_)
        fact_ = ClientToolFact{*generation_, "unknown", ""};
}
}  // namespace ur::device
