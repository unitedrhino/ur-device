// 物模型动作解析和回执序列化，复用公共JSON限长/深度/重复键/Unicode校验。
#include "ur/device/json_parse.hpp"
#include "ur/device/action_wire.hpp"
#include <cJSON.h>
#include <algorithm>
#include <cmath>
#include <memory>

namespace ur::device {
namespace {
// JsonPtr 负责cJSON解析树，失败出口不泄漏对象。
using JsonPtr = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
// identifier 验证关联与行为标识，不允许控制字符或NUL截断。
bool identifier(const std::string& value) {
    return !value.empty() && value.size() <= 128 &&
           std::none_of(value.begin(), value.end(),
                        [](unsigned char byte) { return byte < 32 || byte == 127; });
}
// serialize 将临时JSON缓冲复制到C++所有权后立即释放。
bool serialize(const cJSON* value, std::string& output) {
    auto* bytes = cJSON_PrintUnformatted(value);
    if (!bytes)
        return false;
    output = bytes;
    cJSON_free(bytes);
    return true;
}
}  // namespace

DecodeResult decode_action_request(std::string_view json, ActionRequest& request) {
    request = {};
    std::string canonical;
    const auto checked = canonical_json_object(json, canonical);
    if (checked != DecodeResult::Ok)
        return checked;
    JsonPtr root(ur::device::parse_json(canonical.c_str()), cJSON_Delete);
    if (!root)
        return DecodeResult::InvalidJson;
    const auto* method = cJSON_GetObjectItemCaseSensitive(root.get(), "method");
    const auto* token = cJSON_GetObjectItemCaseSensitive(root.get(), "msgToken");
    const auto* action = cJSON_GetObjectItemCaseSensitive(root.get(), "actionID");
    const auto* alias = cJSON_GetObjectItemCaseSensitive(root.get(), "actionId");
    if (action && alias)
        return DecodeResult::InvalidEnvelope;
    if (!action)
        action = alias;
    if (!cJSON_IsString(method) || std::string_view(method->valuestring) != "action" ||
        !cJSON_IsString(token) || !identifier(token->valuestring) || !cJSON_IsString(action) ||
        !identifier(action->valuestring))
        return DecodeResult::InvalidEnvelope;
    for (auto* field = root->child; field; field = field->next) {
        const std::string_view name(field->string);
        if (name != "method" && name != "msgToken" && name != "actionID" && name != "actionId" &&
            name != "params" && name != "timestamp" && name != "version")
            return DecodeResult::InvalidEnvelope;
    }
    const auto* timestamp = cJSON_GetObjectItemCaseSensitive(root.get(), "timestamp");
    if (timestamp && (!cJSON_IsNumber(timestamp) || timestamp->valuedouble < 0 ||
                      timestamp->valuedouble > 9007199254740991.0 ||
                      std::floor(timestamp->valuedouble) != timestamp->valuedouble))
        return DecodeResult::InvalidEnvelope;
    const auto* version = cJSON_GetObjectItemCaseSensitive(root.get(), "version");
    if (version && (!cJSON_IsString(version) || std::string_view(version->valuestring) != "1.0"))
        return DecodeResult::InvalidEnvelope;
    ActionRequest result;
    const auto* params = cJSON_GetObjectItemCaseSensitive(root.get(), "params");
    if (params && (!cJSON_IsObject(params) || !serialize(params, result.params)))
        return DecodeResult::InvalidEnvelope;
    result.token = token->valuestring;
    result.action = action->valuestring;
    request = std::move(result);
    return DecodeResult::Ok;
}

DecodeResult encode_action_reply(const ActionReply& reply, std::string& json) {
    json.clear();
    if (!identifier(reply.token) || !identifier(reply.action) || reply.code < 200 ||
        reply.code > 599 || reply.message.size() > 256 ||
        reply.message.find('\0') != std::string::npos)
        return DecodeResult::InvalidEnvelope;
    std::string canonical;
    const auto checked = canonical_json_object(reply.data, canonical);
    if (checked != DecodeResult::Ok)
        return checked;
    JsonPtr root(cJSON_CreateObject(), cJSON_Delete);
    JsonPtr data(ur::device::parse_json(canonical.c_str()), cJSON_Delete);
    if (!root || !data || !cJSON_AddStringToObject(root.get(), "method", "actionReply") ||
        !cJSON_AddStringToObject(root.get(), "msgToken", reply.token.c_str()) ||
        !cJSON_AddStringToObject(root.get(), "actionID", reply.action.c_str()) ||
        !cJSON_AddNumberToObject(root.get(), "code", reply.code) ||
        !cJSON_AddStringToObject(root.get(), "msg", reply.message.c_str()))
        return DecodeResult::InvalidJson;
    if (!cJSON_AddItemToObject(root.get(), "data", data.get()))
        return DecodeResult::InvalidJson;
    // 挂接成功后由root释放，返回指针仅记录已转交对象，不再次拥有或释放。
    [[maybe_unused]] auto* transferred_data = data.release();
    std::string encoded;
    if (!serialize(root.get(), encoded))
        return DecodeResult::InvalidJson;
    return canonical_json_object(encoded, json);
}
}  // namespace ur::device
