// 识别、回复文本、工具通知与聚合终态的严格解码；不把内容通知转换为执行授权。
#include "ur/device/json_parse.hpp"
#include "ur/device/content_wire.hpp"
#include <cJSON.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <utility>

namespace ur::device {
namespace {
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;  // 本次解析树唯一所有者。
// artifact_reference 限定正式上传生成的原引用，拒绝URL、对象路径及data内联媒体。
bool artifact_reference(std::string_view reference) {
    return reference.size() > 9 && reference.size() <= 64 &&
           reference.compare(0, 9, "artifact_") == 0 &&
           std::all_of(reference.begin() + 9, reference.end(), [](unsigned char ch) {
               return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                      (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
           });
}
// non_blank_text 与Go strings.TrimSpace的Unicode空白集合一致；UTF-8合法性仍由公共信封检查。
bool non_blank_text(std::string_view text) {
    constexpr std::array<std::string_view, 19> whitespace = {
        "\xc2\x85",     "\xc2\xa0",     "\xe1\x9a\x80", "\xe2\x80\x80", "\xe2\x80\x81",
        "\xe2\x80\x82", "\xe2\x80\x83", "\xe2\x80\x84", "\xe2\x80\x85", "\xe2\x80\x86",
        "\xe2\x80\x87", "\xe2\x80\x88", "\xe2\x80\x89", "\xe2\x80\x8a", "\xe2\x80\xa8",
        "\xe2\x80\xa9", "\xe2\x80\xaf", "\xe2\x81\x9f", "\xe3\x80\x80"};
    while (!text.empty()) {
        const auto ch = static_cast<unsigned char>(text.front());
        if (ch == ' ' || (ch >= 9 && ch <= 13)) {
            text.remove_prefix(1);
            continue;
        }
        const auto found = std::find_if(whitespace.begin(), whitespace.end(), [&](auto value) {
            return text.substr(0, value.size()) == value;
        });
        if (found == whitespace.end())
            return true;
        text.remove_prefix(found->size());
    }
    return false;
}
// supported_part_mime 逐项对齐当前DeviceContentService上传表，不默认开放音视频或未知解析格式。
bool supported_part_mime(InputContentKind kind, std::string_view mime) {
    constexpr std::array<std::string_view, 5> images = {"image/png", "image/jpeg", "image/gif",
                                                        "image/bmp", "image/webp"};
    constexpr std::array<std::string_view, 11> files = {
        "text/plain",
        "text/csv",
        "application/json",
        "text/markdown",
        "text/asciidoc",
        "text/html",
        "message/rfc822",
        "application/pdf",
        "application/vnd.openxmlformats-officedocument.wordprocessingml.document",
        "application/vnd.openxmlformats-officedocument.presentationml.presentation",
        "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"};
    return kind == InputContentKind::Image
               ? std::find(images.begin(), images.end(), mime) != images.end()
               : std::find(files.begin(), files.end(), mime) != files.end();
}
// read_string 区分可含换行的用户文本与不可含控制字符的标识，UTF-8
// 由公共解码器验证。
bool read_string(const cJSON* root, const char* key, std::string& out, bool identifier = true) {
    const auto* value = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsString(value))
        return false;
    out = value->valuestring;
    if (!identifier)
        return true;
    if (out.empty() || out.size() > 128)
        return false;
    for (auto ch : out)
        if (static_cast<unsigned char>(ch) < 32 || ch == 127)
            return false;
    return true;
}
// read_count 接受从零起的安全整数，不接受浮点数或字符串数字。
bool read_count(const cJSON* root, const char* key, std::uint64_t& out) {
    const auto* value = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsNumber(value) || value->valuedouble < 0 ||
        value->valuedouble > 9007199254740991.0 ||
        std::floor(value->valuedouble) != value->valuedouble)
        return false;
    out = static_cast<std::uint64_t>(value->valuedouble);
    return true;
}
// read_object 输出经过公共安全校验的对象；保留结构，不能把字符串 JSON
// 当对象接受。
bool read_object(const cJSON* root, const char* key, std::string& out) {
    const auto* value = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsObject(value))
        return false;
    auto* encoded = cJSON_PrintUnformatted(value);
    if (!encoded)
        return false;
    out = encoded;
    cJSON_free(encoded);
    return true;
}
// read_source
// 输入段与识别条目至少提供一个；存在的字段必须合法，禁止null退化为缺省。
bool read_source(const cJSON* root, ContentEvent& event) {
    const bool has_input = cJSON_GetObjectItemCaseSensitive(root, "inputId") != nullptr;
    const bool has_utterance = cJSON_GetObjectItemCaseSensitive(root, "utteranceId") != nullptr;
    return (has_input || has_utterance) &&
           (!has_input || read_string(root, "inputId", event.input_id)) &&
           (!has_utterance || read_string(root, "utteranceId", event.utterance_id));
}
}  // namespace

WireResult content_input_command(std::string token, std::string session, std::string input_id,
                                 const std::vector<InputContentPart>& parts, bool audio,
                                 Envelope& out) {
    out = {};
    if (session.empty() || input_id.empty() || input_id.size() > 128 || parts.empty() ||
        parts.size() > 16 || std::any_of(input_id.begin(), input_id.end(), [](unsigned char ch) {
            return ch < 32 || ch == 127;
        }))
        return WireResult::Invalid;
    // 原件不参与MQTT预算；所有引用和有序文字共同受公共编码后的64KiB整帧门禁约束。
    std::size_t text_bytes = 0;
    for (const auto& part : parts) {
        if (part.kind == InputContentKind::Text) {
            if (!part.content_ref.empty() || !part.mime_type.empty() ||
                part.text.find('\0') != std::string::npos || !non_blank_text(part.text) ||
                part.text.size() > 65536 - text_bytes)
                return WireResult::Invalid;
            text_bytes += part.text.size();
        } else if (part.kind == InputContentKind::Image || part.kind == InputContentKind::File) {
            if (!part.text.empty() || !artifact_reference(part.content_ref) ||
                !supported_part_mime(part.kind, part.mime_type))
                return WireResult::Invalid;
        } else {
            return WireResult::Unsupported;
        }
    }
    Json params(cJSON_CreateObject(), cJSON_Delete);
    if (!params || !cJSON_AddStringToObject(params.get(), "inputId", input_id.c_str()))
        return WireResult::Invalid;
    auto* contents = cJSON_AddArrayToObject(params.get(), "contents");
    auto* outputs = cJSON_AddArrayToObject(params.get(), "outputModalities");
    if (!contents || !outputs || !cJSON_AddItemToArray(outputs, cJSON_CreateString("text")) ||
        (audio && !cJSON_AddItemToArray(outputs, cJSON_CreateString("audio"))))
        return WireResult::Invalid;
    for (const auto& part : parts) {
        Json value(cJSON_CreateObject(), cJSON_Delete);
        const auto* kind = part.kind == InputContentKind::Text    ? "text"
                           : part.kind == InputContentKind::Image ? "image"
                                                                  : "file";
        if (!value || !cJSON_AddStringToObject(value.get(), "type", kind) ||
            (part.kind == InputContentKind::Text
                 ? !cJSON_AddStringToObject(value.get(), "text", part.text.c_str())
                 : (!cJSON_AddStringToObject(value.get(), "contentRef", part.content_ref.c_str()) ||
                    !cJSON_AddStringToObject(value.get(), "mimeType", part.mime_type.c_str()))))
            return WireResult::Invalid;
        auto* owned = value.release();
        if (!cJSON_AddItemToArray(contents, owned)) {
            value.reset(owned);
            return WireResult::Invalid;
        }
    }
    auto* body = cJSON_PrintUnformatted(params.get());
    if (!body)
        return WireResult::Invalid;
    Envelope command;
    command.token = std::move(token);
    command.session = std::move(session);
    command.method = "inputSend";
    command.payload = body;
    cJSON_free(body);
    std::string encoded;
    if (encode_envelope(command, encoded) != DecodeResult::Ok ||
        decode_envelope(encoded, out) != DecodeResult::Ok)
        return WireResult::Invalid;
    return WireResult::Ok;
}

WireResult decode_content_event(WireRoute route, std::string_view body, ContentEvent& out) {
    out = {};
    if (route != WireRoute::Message)
        return WireResult::WrongRoute;
    ContentEvent event;
    auto& envelope = event.envelope;
    if (decode_envelope(body, envelope) != DecodeResult::Ok || envelope.request || envelope.code ||
        envelope.event_sequence || !envelope.message_sequence || envelope.session.empty())
        return WireResult::Invalid;
    Json data(ur::device::parse_json(envelope.payload.c_str()), cJSON_Delete);
    if (!data || !read_string(data.get(), "respId", event.response_id))
        return WireResult::Invalid;
    const auto& method = envelope.method;
    bool valid = false;
    if (method == "respCreated" || method == "respSttDelta" || method == "respSttDone") {
        valid = read_source(data.get(), event);
        if (valid && method != "respCreated")
            valid = read_string(data.get(), method == "respSttDelta" ? "delta" : "text", event.text,
                                false);
    } else if (method == "respTextDelta" || method == "respTextDone") {
        valid = read_count(data.get(), "contentIndex", event.content_index) &&
                read_string(data.get(), method == "respTextDelta" ? "delta" : "text", event.text,
                            false);
    } else if (method == "respEmotion") {
        // 与服务端契约一致的显示白名单；不把任意字符串解释为驱动命令。
        constexpr std::array<std::string_view, 22> emojis = {
            "😶", "🙂", "😊", "😆", "😂", "😔", "😠", "😭", "😍", "😳", "😲",
            "😱", "🤔", "😉", "😎", "😌", "🤤", "😘", "😏", "😴", "😜", "🙄"};
        constexpr std::array<std::string_view, 5> emotions = {"neutral", "happy", "sad", "angry",
                                                              "surprised"};
        valid = read_string(data.get(), "emoji", event.emoji) &&
                read_string(data.get(), "emotion", event.emotion) &&
                std::find(emojis.begin(), emojis.end(), event.emoji) != emojis.end() &&
                std::find(emotions.begin(), emotions.end(), event.emotion) != emotions.end();
    } else if (method == "toolCallStart") {
        valid = read_string(data.get(), "toolCallId", event.tool_call_id) &&
                read_string(data.get(), "name", event.tool_name) &&
                read_object(data.get(), "arguments", event.object_json);
    } else if (method == "toolCallResult") {
        valid = read_string(data.get(), "toolCallId", event.tool_call_id) &&
                read_string(data.get(), "state", event.status) &&
                (event.status == "succeeded" || event.status == "failed" ||
                 event.status == "unknown") &&
                read_object(data.get(), "result", event.object_json);
    } else if (method == "respDone") {
        valid = read_source(data.get(), event) && read_string(data.get(), "status", event.status) &&
                (event.status == "completed" || event.status == "cancelled" ||
                 event.status == "failed") &&
                read_count(data.get(), "contentCount", event.content_count) &&
                read_string(data.get(), "audioState", event.audio_state) &&
                (event.audio_state == "none" || event.audio_state == "sent" ||
                 event.audio_state == "cancelled" || event.audio_state == "failed");
        if (valid &&
            (event.status != "completed" || cJSON_GetObjectItemCaseSensitive(data.get(), "reason")))
            valid = read_string(data.get(), "reason", event.reason);
    } else
        return WireResult::Unsupported;
    if (!valid)
        return WireResult::Invalid;
    out = std::move(event);
    return WireResult::Ok;
}
}  // namespace ur::device
