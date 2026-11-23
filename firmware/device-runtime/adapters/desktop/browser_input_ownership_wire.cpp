// 文件说明：原本地票据使用十进制字符串保全64位，scope仍由已鉴权宿主独立校验。
#include "ur/device/json_parse.hpp"
#include "browser_input_ownership_wire.hpp"
#include <cJSON.h>
#include <charconv>
#include <memory>
#include "ur/device/envelope.hpp"

namespace ur::device {
namespace {
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;  // 独占严格解析树。
constexpr std::uint64_t safe = 9007199254740991ULL;            // boot/路由跨语言安全水位。
// text读取原UTF8标识，不从别的JSON类型补值。
std::string text(const cJSON* object, const char* name) {
    const auto* value = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(value) && value->valuestring ? value->valuestring : "";
}
// number只解析规范非零十进制字符串，禁止数值精度丢失或尾随字符。
bool number(std::string_view value, std::uint64_t& out, std::uint64_t maximum) {
    if (value.empty() || value.front() == '0')
        return false;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), out);
    return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && out &&
           out <= maximum;
}
// scope检查原会话与路由代次；公开字符串禁止控制字符。
bool scope(const InputGateRequest& value) {
    if (value.session.empty() || value.session.size() > 64 || !value.route_generation ||
        value.route_generation > safe)
        return false;
    for (const unsigned char c : value.session)
        if (c < 32 || c == 127)
            return false;
    return true;
}
}  // namespace
bool decode_browser_input_ownership(std::string_view body, BrowserInputOwnershipEvent& out) {
    out = {};
    std::string canonical;
    if (canonical_json_object(body, canonical) != DecodeResult::Ok)
        return false;
    Json object(ur::device::parse_json(canonical.c_str()), &cJSON_Delete);
    if (!object)
        return false;
    BrowserInputOwnershipEvent result;
    const auto kind = text(object.get(), "kind");
    if (kind == "inputOwnershipIdle") {
        if (cJSON_GetArraySize(object.get()) != 2 ||
            !number(text(object.get(), "bootGeneration"), result.boot_generation, safe))
            return false;
        result.idle = true;
    } else if (kind == "inputOwnershipGated" || kind == "inputOwnershipSenderGated") {
        result.sender = kind == "inputOwnershipSenderGated";
        const auto* enabled = cJSON_GetObjectItemCaseSensitive(object.get(), "enabled");
        result.scope.session = text(object.get(), "sessionId");
        if (cJSON_GetArraySize(object.get()) != 5 || !cJSON_IsBool(enabled) ||
            !number(text(object.get(), "routeGeneration"), result.scope.route_generation, safe) ||
            !number(text(object.get(), "ticket"), result.ticket, UINT64_MAX) ||
            !scope(result.scope))
            return false;
        result.enabled = result.scope.enabled = cJSON_IsTrue(enabled);
    } else
        return false;
    out = std::move(result);
    return true;
}
std::string browser_input_ownership_command(const InputGateRequest& request, std::uint64_t ticket) {
    if (!scope(request) || !ticket)
        return {};
    Json object(cJSON_CreateObject(), &cJSON_Delete);
    if (!object || !cJSON_AddStringToObject(object.get(), "kind", "inputOwnershipGate") ||
        !cJSON_AddStringToObject(object.get(), "sessionId", request.session.c_str()) ||
        !cJSON_AddStringToObject(object.get(), "routeGeneration",
                                 std::to_string(request.route_generation).c_str()) ||
        !cJSON_AddStringToObject(object.get(), "ticket", std::to_string(ticket).c_str()) ||
        !cJSON_AddBoolToObject(object.get(), "enabled", request.enabled))
        return {};
    char* raw = cJSON_PrintUnformatted(object.get());
    if (!raw)
        return {};
    std::string result(raw);
    cJSON_free(raw);
    std::string canonical;
    return canonical_json_object(result, canonical) == DecodeResult::Ok ? canonical : std::string{};
}
}  // namespace ur::device
