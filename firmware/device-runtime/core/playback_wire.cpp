// 文件播放器控制的严格解析；复用公共有界JSON，直播不借用load/seek，身份和源权限由上层核验。
#include "ur/device/json_parse.hpp"
#include "ur/device/playback_wire.hpp"
#include <cJSON.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <memory>

namespace ur::device {
namespace {
// Json 确保所有非法报文出口都释放解析树。
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
// text 读取有界标识，公共解码器已经验证UTF-8和重复键。
bool text(const cJSON* root, const char* name, std::string& value, std::size_t limit) {
    const auto* item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsString(item))
        return false;
    value = item->valuestring;
    return !value.empty() && value.size() <= limit &&
           std::none_of(value.begin(), value.end(),
                        [](unsigned char ch) { return ch < 32 || ch == 127; });
}
// integer 接受0起的安全整数，拒绝字符串、null、负数、非整数及跨语言精度溢出。
bool integer(const cJSON* root, const char* name, std::uint64_t& value) {
    const auto* item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble) || item->valuedouble < 0 ||
        item->valuedouble > 9007199254740991.0 ||
        std::floor(item->valuedouble) != item->valuedouble)
        return false;
    value = static_cast<std::uint64_t>(item->valuedouble);
    return true;
}
// valid_reference 排除URL和路径语法；引用授权仍由内容服务核验。
bool valid_reference(const std::string& value) {
    if (value.empty())
        return false;
    std::string lower = value;
    for (auto& ch : lower)
        if (ch >= 'A' && ch <= 'Z')
            ch = static_cast<char>(ch - 'A' + 'a');
    return value.find("://") == std::string::npos && value.find('\\') == std::string::npos &&
           value.front() != '/' && value.front() != ' ' && value.back() != ' ' &&
           value.rfind("../", 0) != 0 && value.find("/../") == std::string::npos &&
           lower.rfind("data:", 0) != 0 && lower.rfind("file:", 0) != 0 &&
           lower.rfind("javascript:", 0) != 0 &&
           !(value.size() >= 3 && value[1] == ':' && value[2] == '/');
}
}  // namespace

WireResult playback_capability_command(std::string token, const PlaybackCapability& capability,
                                       Envelope& out) {
    out = {};
    if (capability.revision == 0 || capability.revision > 9007199254740991ULL ||
        (!capability.audio_output && !capability.video_output))
        return WireResult::Invalid;
    Json root(cJSON_CreateObject(), cJSON_Delete);
    if (!root)
        return WireResult::Invalid;
    auto* purposes =
        cJSON_AddArrayToObject(root.get(), "purposes");  // 独立用途，不能借用AI协议授权。
    auto* transports = cJSON_AddArrayToObject(root.get(), "transports");  // 文件不建立UDP/RTP轨。
    auto* outputs =
        cJSON_AddArrayToObject(root.get(), "outputModalities");  // 实际输出，不是摄像头源。
    if (!purposes || !transports || !outputs || !cJSON_AddArrayToObject(root.get(), "features") ||
        !cJSON_AddNumberToObject(root.get(), "revision",
                                 static_cast<double>(capability.revision)) ||
        !cJSON_AddItemToArray(purposes, cJSON_CreateString("mediaPlayback")) ||
        !cJSON_AddItemToArray(transports, cJSON_CreateString("none")) ||
        (capability.audio_output && !cJSON_AddItemToArray(outputs, cJSON_CreateString("audio"))) ||
        (capability.video_output && !cJSON_AddItemToArray(outputs, cJSON_CreateString("video"))))
        return WireResult::Invalid;
    char* serialized = cJSON_PrintUnformatted(root.get());  // 输出由公共编码器验证和规范化。
    if (!serialized)
        return WireResult::Invalid;
    Envelope command;
    command.token = std::move(token);
    command.method = "capabilityReport";
    command.payload = serialized;
    cJSON_free(serialized);
    std::string body;
    if (encode_envelope(command, body) != DecodeResult::Ok ||
        decode_envelope(body, out) != DecodeResult::Ok)
        return WireResult::Invalid;
    return WireResult::Ok;
}

WireResult validate_playback_outputs(const PlaybackCapability& capability,
                                     const PlaybackInvite& invite) {
    if (capability.revision == 0 || capability.revision > 9007199254740991ULL ||
        invite.output_modalities.empty() || invite.output_modalities.size() > 2)
        return WireResult::Invalid;
    bool audio = false, video = false;  // 重复、未知或未装配输出都拒绝，不只检测交集非空。
    for (const auto& output : invite.output_modalities) {
        if (output == "audio" && !audio && capability.audio_output)
            audio = true;
        else if (output == "video" && !video && capability.video_output)
            video = true;
        else
            return WireResult::Invalid;
    }
    return WireResult::Ok;
}

WireResult decode_playback_invite(WireRoute route, std::string_view body, PlaybackInvite& out) {
    out = {};
    if (route != WireRoute::Signaling)
        return WireResult::WrongRoute;
    PlaybackInvite invite;
    if (decode_envelope(body, invite.envelope) != DecodeResult::Ok ||
        invite.envelope.method != "sessionInvite" || !invite.envelope.request ||
        invite.envelope.session.empty() || invite.envelope.session.size() > 64 ||
        !invite.envelope.event_sequence || invite.envelope.code || invite.envelope.message_sequence)
        return WireResult::Invalid;
    Json root(ur::device::parse_json(invite.envelope.payload.c_str()), cJSON_Delete);
    constexpr const char* allowed[] = {"purpose",     "admission",       "transport",
                                       "tracks",      "expiresAt",       "playbackId",
                                       "mediaSource", "inputModalities", "outputModalities"};
    if (!root || !cJSON_IsObject(root.get()) || cJSON_GetArraySize(root.get()) != 9)
        return WireResult::Invalid;
    for (const auto* item = root->child; item; item = item->next)
        if (cJSON_IsNull(item) ||
            std::none_of(std::begin(allowed), std::end(allowed),
                         [&](const char* key) { return std::strcmp(item->string, key) == 0; }))
            return WireResult::Invalid;
    std::string purpose, admission, transport;  // 固定语义，不把文件输出变成摄像头或实时Peer。
    if (!text(root.get(), "purpose", purpose, 32) || purpose != "mediaPlayback" ||
        !text(root.get(), "admission", admission, 16) || admission != "direct" ||
        !text(root.get(), "transport", transport, 16) || transport != "none" ||
        !text(root.get(), "playbackId", invite.playback_id, 128) ||
        !integer(root.get(), "expiresAt", invite.expires_at) || invite.expires_at == 0)
        return WireResult::Invalid;
    for (const auto* name : {"tracks", "inputModalities"}) {
        const auto* value = cJSON_GetObjectItemCaseSensitive(root.get(), name);
        if (!cJSON_IsArray(value) || cJSON_GetArraySize(value) != 0)
            return WireResult::Invalid;
    }
    const auto* outputs = cJSON_GetObjectItemCaseSensitive(root.get(), "outputModalities");
    if (!cJSON_IsArray(outputs) || cJSON_GetArraySize(outputs) < 1 ||
        cJSON_GetArraySize(outputs) > 2)
        return WireResult::Invalid;
    for (const auto* item = outputs->child; item; item = item->next) {
        if (!cJSON_IsString(item) ||
            (std::strcmp(item->valuestring, "audio") != 0 &&
             std::strcmp(item->valuestring, "video") != 0) ||
            std::find(invite.output_modalities.begin(), invite.output_modalities.end(),
                      item->valuestring) != invite.output_modalities.end())
            return WireResult::Invalid;
        invite.output_modalities.emplace_back(item->valuestring);
    }
    const auto* source = cJSON_GetObjectItemCaseSensitive(root.get(), "mediaSource");
    if (!cJSON_IsObject(source) || cJSON_GetArraySize(source) != 1 ||
        !text(source, "contentRef", invite.content_ref, 512))
        return WireResult::Invalid;
    if (!valid_reference(invite.content_ref))
        return WireResult::Invalid;
    out = std::move(invite);
    return WireResult::Ok;
}

WireResult decode_playback_control(WireRoute route, std::string_view body, PlaybackCommand& out) {
    out = {};
    if (route != WireRoute::Signaling)
        return WireResult::WrongRoute;
    PlaybackCommand command;
    if (decode_envelope(body, command.envelope) != DecodeResult::Ok ||
        command.envelope.method != "playbackControl" || !command.envelope.request ||
        command.envelope.session.empty() || !command.envelope.event_sequence ||
        command.envelope.code || command.envelope.message_sequence)
        return WireResult::Invalid;
    Json root(ur::device::parse_json(command.envelope.payload.c_str()), cJSON_Delete);
    if (!root || !cJSON_IsObject(root.get()))
        return WireResult::Invalid;
    constexpr const char* allowed[] = {"operation",    "expectedRevision", "playbackId",
                                       "contentRef",   "positionMs",       "rate",
                                       "mediaRevision"};
    for (const auto* item = root->child; item; item = item->next)
        if (cJSON_IsNull(item) ||
            std::none_of(std::begin(allowed), std::end(allowed),
                         [&](const char* key) { return std::strcmp(item->string, key) == 0; }))
            return WireResult::Invalid;
    if (!text(root.get(), "operation", command.operation, 128) ||
        !text(root.get(), "playbackId", command.playback_id, 128) ||
        !integer(root.get(), "expectedRevision", command.expected_revision))
        return WireResult::Invalid;
    const auto* content = cJSON_GetObjectItemCaseSensitive(root.get(), "contentRef");
    const auto* position = cJSON_GetObjectItemCaseSensitive(root.get(), "positionMs");
    const auto* rate = cJSON_GetObjectItemCaseSensitive(root.get(), "rate");
    if ((content != nullptr) != (command.operation == "load") ||
        (position != nullptr) != (command.operation == "seek") ||
        (rate != nullptr) != (command.operation == "setRate"))
        return WireResult::Invalid;
    if (command.operation == "load") {
        if (!text(root.get(), "contentRef", command.content_ref, 512) ||
            !valid_reference(command.content_ref))
            return WireResult::Invalid;
    } else if (command.operation == "seek") {
        std::uint64_t value = 0;  // 0位置与缺省不同，不由字段零值猜测操作。
        if (!integer(root.get(), "positionMs", value))
            return WireResult::Invalid;
        command.position_ms = value;
    } else if (command.operation == "setRate") {
        if (!cJSON_IsNumber(rate) || (rate->valuedouble != 0.5 && rate->valuedouble != 1 &&
                                      rate->valuedouble != 1.5 && rate->valuedouble != 2))
            return WireResult::Invalid;
        command.rate = rate->valuedouble;
    } else if (command.operation != "play" && command.operation != "pause" &&
               command.operation != "stop") {
        return WireResult::Invalid;
    }
    if (cJSON_GetObjectItemCaseSensitive(root.get(), "mediaRevision")) {
        std::uint64_t value = 0;  // 历史流代次不能取0；是否必填由当前用途确认。
        if (!integer(root.get(), "mediaRevision", value) || value == 0)
            return WireResult::Invalid;
        command.media_revision = value;
    }
    out = std::move(command);
    return WireResult::Ok;
}
}  // namespace ur::device
