// cJSON 仅在本文件可见；限制包长/深度和重复键，输出不包含错误原文。
#include "ur/device/json_parse.hpp"
#include "ur/device/envelope.hpp"
#include <cJSON.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

namespace ur::device {
namespace {
// JsonPtr 自动释放解析树，所有失败出口统一遵守 cJSON 所有权。
using JsonPtr = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
// bounded_depth 在递归解析器前检查容器层数，不依赖桌面/ESP所链接cJSON的默认栈限制。
// 字符串内的括号和转义不计层数；具体JSON语法仍交给解析器验证。
bool bounded_depth(std::string_view bytes) {
    unsigned depth = 0;
    bool quoted = false;
    bool escaped = false;
    for (const auto ch : bytes) {
        if (quoted) {
            if (escaped)
                escaped = false;
            else if (ch == '\\')
                escaped = true;
            else if (ch == '"')
                quoted = false;
        } else if (ch == '"')
            quoted = true;
        else if (ch == '{' || ch == '[') {
            if (++depth > 32)
                return false;
        } else if (ch == '}' || ch == ']') {
            if (depth == 0)
                return false;
            --depth;
        }
    }
    return depth == 0 && !quoted;
}
// escaped_nul 只拒绝实际 Unicode NUL 转义，不误拒绝正文中的字面量反斜杠-u0000。
bool escaped_nul(std::string_view bytes) {
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (bytes[index] != '\\')
            continue;
        if (bytes.substr(index, 6) == "\\u0000")
            return true;
        ++index;  // 跳过已转义字符，下一反斜杠不再单独解释。
    }
    return false;
}
// valid_utf8 拒绝过长编码、代理码位及越界 Unicode，保持 Go 与设备解析器一致。
bool valid_utf8(std::string_view bytes) {
    std::uint32_t scalar = 0;
    std::uint32_t minimum = 0;
    unsigned remaining = 0;
    for (const auto character : bytes) {
        const auto byte = static_cast<unsigned char>(character);
        if (remaining != 0) {
            if ((byte & 0xc0U) != 0x80U)
                return false;
            scalar = (scalar << 6U) | (byte & 0x3fU);
            if (--remaining == 0 && (scalar < minimum || scalar > 0x10ffffU ||
                                     (scalar >= 0xd800U && scalar <= 0xdfffU)))
                return false;
        } else if (byte >= 0x80U) {
            if (byte >= 0xc2U && byte <= 0xdfU) {
                scalar = byte & 0x1fU;
                minimum = 0x80U;
                remaining = 1;
            } else if (byte >= 0xe0U && byte <= 0xefU) {
                scalar = byte & 0xfU;
                minimum = 0x800U;
                remaining = 2;
            } else if (byte >= 0xf0U && byte <= 0xf4U) {
                scalar = byte & 7U;
                minimum = 0x10000U;
                remaining = 3;
            } else
                return false;
        }
    }
    return remaining == 0;
}
// normalize 拒绝重复对象键及过深结构，同时按键排序以稳定重试正文。
bool normalize(cJSON* item, unsigned depth) {
    if (depth > 32)
        return false;
    if (cJSON_IsNumber(item) && (!std::isfinite(item->valuedouble) ||
                                 (depth > 1 && std::abs(item->valuedouble) > 9007199254740991.0)))
        return false;
    std::vector<cJSON*> children;
    for (auto* child = item->child; child; child = child->next) {
        if (!normalize(child, depth + 1))
            return false;
        children.push_back(child);
    }
    if (!cJSON_IsObject(item))
        return true;
    std::sort(children.begin(), children.end(), [](const cJSON* left, const cJSON* right) {
        return std::strcmp(left->string, right->string) < 0;
    });
    for (std::size_t i = 1; i < children.size(); ++i)
        if (std::strcmp(children[i - 1]->string, children[i]->string) == 0)
            return false;
    if (children.empty())
        return true;
    item->child = children.front();
    for (std::size_t i = 0; i < children.size(); ++i) {
        children[i]->next = i + 1 < children.size() ? children[i + 1] : nullptr;
        children[i]->prev = i == 0 ? children.back() : children[i - 1];
    }
    return true;
}
// text 读取有界非空字符串，不接受 JSON null 或数字冒充标识符。
bool text(const cJSON* root, const char* key, std::string& value, std::size_t limit,
          bool required) {
    const auto* item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!item)
        return !required;
    if (!cJSON_IsString(item))
        return false;
    value = item->valuestring;
    return !value.empty() && value.size() <= limit &&
           std::none_of(value.begin(), value.end(),
                        [](unsigned char byte) { return byte < 0x20U || byte == 0x7fU; });
}
// sequence 限制为 JSON 可精确表达的正整数，不允许两个通道共享水位。
bool sequence(const cJSON* root, const char* key, std::optional<std::uint64_t>& value) {
    const auto* item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!item)
        return true;
    if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble) || item->valuedouble < 1 ||
        item->valuedouble > 9007199254740991.0 ||
        std::floor(item->valuedouble) != item->valuedouble)
        return false;
    value = static_cast<std::uint64_t>(item->valuedouble);
    return true;
}
// exact_numbers 将临时副本中的数字改为精确JSON字面量，绕开cJSON的相对误差打印比较。
// 特别是2^53-1不能用15位有效数字近似；原解析树保持数字类型，校验路径不受影响。
bool exact_numbers(cJSON* root) {
    for (auto* item = root->child; item;) {
        auto* next = item->next;
        if (cJSON_IsNumber(item)) {
            char buffer[32];  // double最短往返表示有界，禁止使用locale相关小数逗号。
            // 安全整数用十进制整数字面量，既保持数值也保持现有协议规范化习惯；小数用最短往返表示。
            const double value = item->valuedouble;
            if (!std::isfinite(value))
                return false;
            const auto result = std::floor(value) == value && std::abs(value) <= 9007199254740991.0
                                    ? std::to_chars(buffer, buffer + sizeof(buffer) - 1,
                                                    static_cast<std::int64_t>(value))
                                    : std::to_chars(buffer, buffer + sizeof(buffer) - 1, value);
            if (result.ec != std::errc{})
                return false;
            *result.ptr = '\0';
            auto* raw = cJSON_CreateRaw(buffer);
            if (!raw)
                return false;
            // 对象替换必须复制原字段名；数组没有字段名，按节点替换而不是误用对象API。
            const bool replaced =
                cJSON_IsObject(root)
                    ? cJSON_ReplaceItemInObjectCaseSensitive(root, item->string, raw)
                    : cJSON_ReplaceItemViaPointer(root, item, raw);
            if (!replaced) {
                cJSON_Delete(raw);
                return false;
            }
        } else if (!exact_numbers(item)) {
            return false;
        }
        item = next;
    }
    return true;
}
// print 使用精确数字副本生成文本并释放临时内存，不能改动后续校验使用的原解析树。
bool print(const cJSON* root, std::string& out) {
    JsonPtr precise(cJSON_Duplicate(root, true), cJSON_Delete);
    if (!precise || !exact_numbers(precise.get()))
        return false;
    char* value = cJSON_PrintUnformatted(precise.get());
    if (!value)
        return false;
    out = value;
    cJSON_free(value);
    return true;
}
}  // namespace
DecodeResult canonical_json_object(std::string_view json, std::string& canonical) {
    canonical.clear();
    if (json.size() > 65536)
        return DecodeResult::TooLarge;
    if (!valid_utf8(json) || json.find('\0') != std::string_view::npos || escaped_nul(json) ||
        !bounded_depth(json))
        return DecodeResult::InvalidJson;
    const std::string owned(json);
    JsonPtr root(ur::device::parse_json_with_length_opts(owned.c_str(), owned.size() + 1, nullptr, true),
                 cJSON_Delete);
    if (!root || !normalize(root.get(), 0))
        return DecodeResult::InvalidJson;
    if (!cJSON_IsObject(root.get()))
        return DecodeResult::InvalidEnvelope;
    return print(root.get(), canonical) ? DecodeResult::Ok : DecodeResult::InvalidJson;
}
DecodeResult decode_envelope(std::string_view json, Envelope& envelope) {
    envelope = Envelope{};
    if (json.size() > 65536)
        return DecodeResult::TooLarge;
    // cJSON 使用 C 字符串；禁止 NUL 避免协议标识被截断。转义 NUL 同样禁止。
    if (!valid_utf8(json) || json.find('\0') != std::string_view::npos || escaped_nul(json) ||
        !bounded_depth(json))
        return DecodeResult::InvalidJson;
    const std::string owned(json);
    JsonPtr root(ur::device::parse_json_with_length_opts(owned.c_str(), owned.size() + 1, nullptr, true),
                 cJSON_Delete);
    if (!root || !normalize(root.get(), 0))
        return DecodeResult::InvalidJson;
    if (!cJSON_IsObject(root.get()))
        return DecodeResult::InvalidEnvelope;
    Envelope result;
    if (!text(root.get(), "msgToken", result.token, 128, true) ||
        !text(root.get(), "method", result.method, 64, true) ||
        !text(root.get(), "sessionId", result.session, 128, false))
        return DecodeResult::InvalidEnvelope;
    for (auto* item = root->child; item; item = item->next) {
        const std::string_view key(item->string);
        if (key != "msgToken" && key != "method" && key != "sessionId" && key != "params" &&
            key != "data" && key != "code" && key != "eventSeq" && key != "messageSeq")
            return DecodeResult::InvalidEnvelope;
    }
    const auto* params = cJSON_GetObjectItemCaseSensitive(root.get(), "params");
    const auto* data = cJSON_GetObjectItemCaseSensitive(root.get(), "data");
    if ((params && data) || (!params && !data) || !cJSON_IsObject(params ? params : data))
        return DecodeResult::InvalidEnvelope;
    result.request = params != nullptr;
    const auto* code = cJSON_GetObjectItemCaseSensitive(root.get(), "code");
    if (code) {
        if (!cJSON_IsNumber(code) || !std::isfinite(code->valuedouble) || code->valuedouble < 200 ||
            code->valuedouble > 599 || std::floor(code->valuedouble) != code->valuedouble ||
            result.request)
            return DecodeResult::InvalidEnvelope;
        result.code = code->valueint;
    }
    if (!sequence(root.get(), "eventSeq", result.event_sequence) ||
        !sequence(root.get(), "messageSeq", result.message_sequence) ||
        (result.event_sequence && result.message_sequence))
        return DecodeResult::InvalidEnvelope;
    if (!print(params ? params : data, result.payload))
        return DecodeResult::InvalidJson;
    envelope = std::move(result);
    return DecodeResult::Ok;
}
DecodeResult encode_envelope(const Envelope& envelope, std::string& json) {
    json.clear();
    if (envelope.payload.size() > 65536)
        return DecodeResult::TooLarge;
    if (envelope.token.find('\0') != std::string::npos ||
        envelope.method.find('\0') != std::string::npos ||
        envelope.session.find('\0') != std::string::npos ||
        envelope.payload.find('\0') != std::string::npos || escaped_nul(envelope.payload))
        return DecodeResult::InvalidEnvelope;
    if (!bounded_depth(envelope.payload))
        return DecodeResult::InvalidJson;
    JsonPtr root(cJSON_CreateObject(), cJSON_Delete);
    JsonPtr payload(ur::device::parse_json_with_length_opts(envelope.payload.c_str(), envelope.payload.size() + 1,
                                              nullptr, true),
                    cJSON_Delete);
    if (!root || !payload || !cJSON_IsObject(payload.get()))
        return DecodeResult::InvalidJson;
    if (!cJSON_AddStringToObject(root.get(), "msgToken", envelope.token.c_str()) ||
        !cJSON_AddStringToObject(root.get(), "method", envelope.method.c_str()))
        return DecodeResult::InvalidJson;
    if (!envelope.session.empty() &&
        !cJSON_AddStringToObject(root.get(), "sessionId", envelope.session.c_str()))
        return DecodeResult::InvalidJson;
    auto* attached = payload.release();  // 成功由 root 拥有；添加失败时本函数释放。
    if (!cJSON_AddItemToObject(root.get(), envelope.request ? "params" : "data", attached)) {
        cJSON_Delete(attached);
        return DecodeResult::InvalidJson;
    }
    if (envelope.code && !cJSON_AddNumberToObject(root.get(), "code", *envelope.code))
        return DecodeResult::InvalidJson;
    if (envelope.event_sequence &&
        !cJSON_AddNumberToObject(root.get(), "eventSeq",
                                 static_cast<double>(*envelope.event_sequence)))
        return DecodeResult::InvalidJson;
    if (envelope.message_sequence &&
        !cJSON_AddNumberToObject(root.get(), "messageSeq",
                                 static_cast<double>(*envelope.message_sequence)))
        return DecodeResult::InvalidJson;
    std::string encoded;
    if (!print(root.get(), encoded))
        return DecodeResult::InvalidJson;
    Envelope validated;
    const auto result = decode_envelope(encoded, validated);
    if (result != DecodeResult::Ok)
        return result;
    if (!normalize(root.get(), 0) || !print(root.get(), json))
        return DecodeResult::InvalidJson;
    return DecodeResult::Ok;
}
}  // namespace ur::device
