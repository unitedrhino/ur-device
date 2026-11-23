// AI语音与None离散输入方法编码及严格下行校验；cJSON细节不泄漏到平台驱动或公开头文件。
#include "ur/device/json_parse.hpp"
#include "ur/device/voice_wire.hpp"
#include "ur/device/ai_control_wire.hpp"
#include <cJSON.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <utility>
#include "ur/device/content_wire.hpp"

namespace ur::device {
namespace {
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;  // 解析树的唯一所有者。
// valid_format 必须与后端方法契约及实际 Opus 适配器保持一致。
bool valid_format(AudioFormat audio) {
    return audio.channels == 1 && (audio.frame_ms == 20 || audio.frame_ms == 60) &&
           (audio.sample_rate == 16000 || audio.sample_rate == 24000 || audio.sample_rate == 48000);
}
// valid_capability 不把设备的全双工意愿当成实际 AEC 能力。
bool valid_capability(const VoiceCapability& capability) {
  if ((capability.human_video_receive || capability.human_udp_av ||
       capability.human_voice_call) &&
      !capability.human_call)
    return false;
  if (capability.udp_reply_start_receipt &&
      (capability.transport != VoiceTransport::Udp || capability.ai_vision))
    return false;
  // 新目标恢复必须完整具备原控制同步、本人接听和音频，不允许单feature虚报装配。
  if (capability.ai_endpoint_control_sync &&
      (!capability.session_sync || !capability.ai_endpoint_handoff ||
       capability.transport == VoiceTransport::None || capability.ai_vision))
    return false;
  if (capability.ai_vision &&
      (capability.session_sync || capability.ai_endpoint_handoff ||
       capability.transport != VoiceTransport::WebRTC ||
       capability.text_input || capability.image_input ||
       capability.file_input))
    return false;
  // humanCall邀请投递走WebRTC白名单预检；无webrtc端点的能力不得声明接听用途。
  if (capability.human_call && capability.transport != VoiceTransport::WebRTC)
    return false;
  if (capability.transport == VoiceTransport::None)
    return capability.revision > 0 &&
           capability.revision <= 9007199254740991ULL &&
           (capability.text_input || capability.image_input ||
            capability.file_input) &&
           !capability.file_audio_output && !capability.file_video_output &&
           !capability.live_audio_receive && !capability.live_video_receive &&
           !capability.ai_endpoint_handoff;
  return valid_format(capability.audio) && capability.revision > 0 &&
         capability.revision <= 9007199254740991ULL &&
         (capability.modes.half_duplex || capability.modes.full_duplex) &&
         (!capability.modes.full_duplex || capability.modes.device_aec) &&
         (capability.transport == VoiceTransport::Udp ||
          (capability.transport == VoiceTransport::WebRTC &&
           capability.audio.sample_rate == 48000 &&
           capability.audio.frame_ms == 20));
}
// string_field 读取方法必填字符串；协议标识不接受空字符串或控制字符。
bool string_field(const cJSON* root, const char* name, std::string& out, std::size_t limit = 128) {
    const auto* value = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsString(value))
        return false;
    out = value->valuestring;
    if (out.empty() || out.size() > limit)
        return false;
    for (auto ch : out)
        if (static_cast<unsigned char>(ch) < 32 || ch == 127)
            return false;
    return true;
}
// sdp_field 允许规范行结束但拒绝其它控制字符；SDP不能走标识字段校验。
bool sdp_field(const cJSON* root, const char* name, std::string& out) {
    const auto* value = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsString(value))
        return false;
    out = value->valuestring;
    if (out.size() < 8 || out.size() > 49152 || out.compare(0, 5, "v=0\r\n") != 0)
        return false;
    for (auto ch : out)
        if ((static_cast<unsigned char>(ch) < 32 && ch != '\r' && ch != '\n' && ch != '\t') ||
            ch == 127)
            return false;
    return true;
}
// uint_field 不接受小数、负数及浏览器无法精确表示的水位。
bool uint_field(const cJSON* root, const char* name, std::uint64_t& out, bool allow_zero = false) {
    const auto* value = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) ||
        value->valuedouble < (allow_zero ? 0 : 1) || value->valuedouble > 9007199254740991.0 ||
        std::floor(value->valuedouble) != value->valuedouble)
        return false;
    out = static_cast<std::uint64_t>(value->valuedouble);
    return true;
}
// audio_fields 解析与平台一致的 Opus 参数，不自动纠正非法格式。
bool audio_fields(const cJSON* root, AudioFormat& audio) {
    std::string format;
    std::uint64_t rate = 0, channels = 0, duration = 0;
    if (!cJSON_IsObject(root) || !string_field(root, "format", format) || format != "opus" ||
        !uint_field(root, "sampleRate", rate) || !uint_field(root, "channels", channels) ||
        !uint_field(root, "frameDuration", duration) || rate > 48000 || channels > 1 ||
        duration > 60)
        return false;
    audio = {static_cast<int>(rate), static_cast<int>(channels), static_cast<int>(duration)};
    return valid_format(audio);
}
// hex_bytes 只接受长度完全匹配的十六进制，不接受截断/扩展或明文密钥。
template <std::size_t Size>
bool hex_bytes(const cJSON* root, const char* name, std::array<std::uint8_t, Size>& out) {
    std::string value;
    if (!string_field(root, name, value, Size * 2) || value.size() != Size * 2)
        return false;
    const auto nibble = [](char ch) -> int {
        if (ch >= '0' && ch <= '9')
            return ch - '0';
        if (ch >= 'a' && ch <= 'f')
            return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F')
            return ch - 'A' + 10;
        return -1;
    };
    for (std::size_t index = 0; index < Size; ++index) {
        const auto high = nibble(value[index * 2]);
        const auto low = nibble(value[index * 2 + 1]);
        if (high < 0 || low < 0)
            return false;
        out[index] = static_cast<std::uint8_t>(high * 16 + low);
    }
    return true;
}
// add_audio 添加四个必填格式字段；OOM 时不产生缺字段的成功请求。
bool add_audio(cJSON* root, const char* name, AudioFormat audio) {
    auto* value = cJSON_AddObjectToObject(root, name);
    return value && cJSON_AddStringToObject(value, "format", "opus") &&
           cJSON_AddNumberToObject(value, "sampleRate", audio.sample_rate) &&
           cJSON_AddNumberToObject(value, "channels", audio.channels) &&
           cJSON_AddNumberToObject(value, "frameDuration", audio.frame_ms);
}
// add_modes 仅序列化真实声明的交互模式。
bool add_modes(cJSON* root, Capabilities capability) {
    auto* modes = cJSON_AddArrayToObject(root, "supportedInteractionModes");
    if (!modes)
        return false;
    if (capability.half_duplex && !cJSON_AddItemToArray(modes, cJSON_CreateString("halfDuplex")))
        return false;
    return !capability.full_duplex || cJSON_AddItemToArray(modes, cJSON_CreateString("fullDuplex"));
}
// finish 使用同一公共编码器校验标识和最大报文，失败清空输出。
WireResult finish(std::string token, std::string session, std::string method, const cJSON* params,
                  Envelope& out) {
    out = {};
    char* serialized = cJSON_PrintUnformatted(params);
    if (!serialized)
        return WireResult::Invalid;
    Envelope result;
    result.token = std::move(token);
    result.session = std::move(session);
    result.method = std::move(method);
    result.payload = serialized;
    cJSON_free(serialized);
    std::string encoded;
    if (encode_envelope(result, encoded) != DecodeResult::Ok)
        return WireResult::Invalid;
    if (decode_envelope(encoded, out) != DecodeResult::Ok)
        return WireResult::Invalid;
    return WireResult::Ok;
}
// add_discrete_modes 只声明宿主实际装配的文字/附件模态；连续audio不混入离散输入列表。
bool add_discrete_modes(cJSON* root, const VoiceCapability& capability, bool audio = true) {
    if (!capability.text_input && !capability.image_input && !capability.file_input)
        return true;
    auto* inputs = cJSON_AddArrayToObject(root, "inputModalities");
    auto* outputs = cJSON_AddArrayToObject(root, "outputModalities");
    return inputs && outputs &&
           (!capability.text_input || cJSON_AddItemToArray(inputs, cJSON_CreateString("text"))) &&
           (!capability.image_input || cJSON_AddItemToArray(inputs, cJSON_CreateString("image"))) &&
           (!capability.file_input || cJSON_AddItemToArray(inputs, cJSON_CreateString("file"))) &&
           cJSON_AddItemToArray(outputs, cJSON_CreateString("text")) &&
           (!audio || cJSON_AddItemToArray(outputs, cJSON_CreateString("audio")));
}
// add_frontend_tools 只声明代码审核的三字段能力；真实端口未装配时不广告，端不能自授schema。
bool add_frontend_tools(cJSON* root, bool enabled) {
    if (!enabled)
        return true;
    auto* tools = cJSON_AddArrayToObject(root, "frontendTools");
    auto* tool = tools ? cJSON_CreateObject() : nullptr;
    if (!tool || !cJSON_AddItemToArray(tools, tool)) {
        cJSON_Delete(tool);
        return false;
    }
    return cJSON_AddStringToObject(tool, "name", "set_local_display_theme") &&
           cJSON_AddStringToObject(tool, "interactionType", "action") &&
           cJSON_AddNumberToObject(tool, "schemaRevision", 1);
}
// add_capability_outputs 合并完整设备输出快照，去重audio；不扩大AI建会话的模态请求。
bool add_capability_outputs(cJSON* root, const VoiceCapability& capability) {
    if (!capability.file_audio_output && !capability.file_video_output)
        return add_discrete_modes(root, capability);
    auto* inputs = cJSON_AddArrayToObject(root, "inputModalities");
    auto* outputs = cJSON_AddArrayToObject(root, "outputModalities");
    auto* playback = cJSON_AddArrayToObject(root, "playbackOutputModalities");
    return inputs && outputs && playback &&
           (!capability.file_audio_output ||
            cJSON_AddItemToArray(playback, cJSON_CreateString("audio"))) &&
           (!capability.file_video_output ||
            cJSON_AddItemToArray(playback, cJSON_CreateString("video"))) &&
           (!capability.text_input || cJSON_AddItemToArray(inputs, cJSON_CreateString("text"))) &&
           (!capability.image_input || cJSON_AddItemToArray(inputs, cJSON_CreateString("image"))) &&
           (!capability.file_input || cJSON_AddItemToArray(inputs, cJSON_CreateString("file"))) &&
           (!(capability.text_input || capability.image_input || capability.file_input) ||
            cJSON_AddItemToArray(outputs, cJSON_CreateString("text"))) &&
           (!(capability.text_input || capability.image_input || capability.file_input ||
              capability.file_audio_output) ||
            cJSON_AddItemToArray(outputs, cJSON_CreateString("audio"))) &&
           (!capability.file_video_output ||
            cJSON_AddItemToArray(outputs, cJSON_CreateString("video")));
}
// add_live_receive 声明实际独立节目格式；不改AI音频、全局离散模态或相机发布能力。
bool add_live_receive(cJSON* root, const VoiceCapability& capability) {
    if (!capability.live_audio_receive && !capability.live_video_receive)
        return true;
    auto* receive = cJSON_AddObjectToObject(root, "liveReceive");
    if (!receive ||
        (capability.live_audio_receive && !add_audio(receive, "audio", AudioFormat{48000, 1, 20})))
        return false;
    if (!capability.live_video_receive)
        return true;
    auto* codecs = cJSON_AddArrayToObject(receive, "videoCodecs");
    return codecs && cJSON_AddItemToArray(codecs, cJSON_CreateString("H264"));
}
// read_discrete_modes 允许平台选择实际交集，缺省只保留旧无离散输入；绝不猜测附件支持。
bool read_discrete_modes(const cJSON* selected, VoiceMedia& media) {
    const auto* inputs = cJSON_GetObjectItemCaseSensitive(selected, "inputModalities");
    const auto* outputs = cJSON_GetObjectItemCaseSensitive(selected, "outputModalities");
    if (!inputs && !outputs)
        return true;
    if (!cJSON_IsArray(inputs) || !cJSON_IsArray(outputs) || cJSON_GetArraySize(inputs) > 3 ||
        cJSON_GetArraySize(outputs) > 2)
        return false;
    if ((cJSON_GetArraySize(inputs) == 0) != (cJSON_GetArraySize(outputs) == 0))
        return false;
    for (int i = 0; i < cJSON_GetArraySize(inputs); ++i) {
        const auto* item = cJSON_GetArrayItem(inputs, i);
        if (!cJSON_IsString(item))
            return false;
        const std::string_view mode(item->valuestring);
        bool* target = mode == "text"    ? &media.text_input
                       : mode == "image" ? &media.image_input
                       : mode == "file"  ? &media.file_input
                                         : nullptr;
        if (!target || *target)
            return false;
        *target = true;
    }
    for (int i = 0; i < cJSON_GetArraySize(outputs); ++i) {
        const auto* item = cJSON_GetArrayItem(outputs, i);
        if (!cJSON_IsString(item))
            return false;
        const std::string_view mode(item->valuestring);
        bool* target = mode == "text"    ? &media.text_output
                       : mode == "audio" ? &media.audio_output
                                         : nullptr;
        if (!target || *target)
            return false;
        *target = true;
    }
    return true;
}
// read_media校验UDP/WebRTC轨或None显式无媒体选择，不把参数到达当作已建立。
bool read_media(const cJSON* data, VoiceMedia& media, const VoiceVisionProfile* vision) {
    std::string state, transport, mode;
    const auto* selected = cJSON_GetObjectItemCaseSensitive(data, "selected");
    if (!string_field(data, "state", state) || state != "negotiating" ||
        !string_field(selected, "transport", transport))
        return false;
    const auto* receipt = cJSON_GetObjectItemCaseSensitive(selected, "udpReplyStartReceipt");
    if (receipt && (!cJSON_IsTrue(receipt) || transport != "udp"))
        return false;
    media.udp_reply_start_receipt = receipt != nullptr;
    if (vision && transport != "webrtc")
        return false;
    if (transport == "none") {
        // None允许实际已选的离散图片/文件，但没有媒体凭据或交互模式，输出仍只含text。
        const auto* tracks = cJSON_GetObjectItemCaseSensitive(data, "tracks");
        if (!cJSON_IsArray(tracks) || cJSON_GetArraySize(tracks) != 0 ||
            cJSON_GetObjectItemCaseSensitive(data, "media") ||
            cJSON_GetObjectItemCaseSensitive(selected, "interactionMode") ||
            cJSON_GetObjectItemCaseSensitive(selected, "supportedInteractionModes") ||
            cJSON_GetObjectItemCaseSensitive(selected, "audioParams") ||
            cJSON_GetObjectItemCaseSensitive(selected, "aecMode") ||
            !read_discrete_modes(selected, media) ||
            !(media.text_input || media.image_input || media.file_input) || !media.text_output ||
            media.audio_output)
            return false;
        media.transport = VoiceTransport::None;
        return true;
    }
    if (!string_field(data, "state", state) || state != "negotiating" ||
        !string_field(selected, "transport", transport) ||
        (transport != "udp" && transport != "webrtc") ||
        !string_field(selected, "interactionMode", mode) ||
        (mode != "halfDuplex" && mode != "fullDuplex") ||
        !audio_fields(cJSON_GetObjectItemCaseSensitive(selected, "audioParams"), media.audio) ||
        !read_discrete_modes(selected, media))
        return false;
    media.mode = mode == "fullDuplex" ? Mode::FullDuplex : Mode::HalfDuplex;
    media.transport = transport == "webrtc" ? VoiceTransport::WebRTC : VoiceTransport::Udp;
    if (media.transport == VoiceTransport::Udp) {
        const auto* udp = cJSON_GetObjectItemCaseSensitive(
            cJSON_GetObjectItemCaseSensitive(data, "media"), "udp");
        std::uint64_t port = 0;
        std::array<std::uint8_t, 16> header{};
        if (!string_field(udp, "server", media.host, 253) || !uint_field(udp, "port", port) ||
            port > 65535 || !hex_bytes(udp, "key", media.key) || !hex_bytes(udp, "nonce", header) ||
            header[0] != 1 || header[1] != 0)
            return false;
        media.port = static_cast<std::uint16_t>(port);
        std::copy_n(header.begin() + 4, 8, media.nonce.begin());
    } else if (cJSON_GetObjectItemCaseSensitive(data, "media") ||
               media.audio.sample_rate != 48000 || media.audio.frame_ms != 20)
        return false;
    const auto* tracks = cJSON_GetObjectItemCaseSensitive(data, "tracks");
    const bool visual = vision != nullptr;
    if (!cJSON_IsArray(tracks) || cJSON_GetArraySize(tracks) != (visual ? 3 : 2))
        return false;
    if (visual &&
        (vision_profile_binding(vision).empty() || media.transport != VoiceTransport::WebRTC ||
         cJSON_GetArraySize(data) != 3 ||
         cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(selected, "audioParams")) != 4))
        return false;
    if (visual) {
        /* selected 字段白名单：模态回填（inputModalities/outputModalities）是模型协商
         * 的合法扩展；其他字段（如伪造 cameraId）仍拒绝。 */
        for (const auto* field = selected->child; field; field = field->next) {
            if (field->string == nullptr ||
                (strcmp(field->string, "transport") != 0 &&
                 strcmp(field->string, "interactionMode") != 0 &&
                 strcmp(field->string, "audioParams") != 0 &&
                 strcmp(field->string, "inputModalities") != 0 &&
                 strcmp(field->string, "outputModalities") != 0))
                return false;
        }
    }
    unsigned video_count = 0;  // 初始画面只安装暂停轨；不由解析授予取帧许可。
    for (const auto* track = tracks->child; track; track = track->next) {
        std::string id, kind, source, direction, codec, track_state;
        if (!string_field(track, "trackId", id) || !string_field(track, "kind", kind) ||
            !string_field(track, "sourceId", source) ||
            !string_field(track, "direction", direction) || !string_field(track, "codec", codec) ||
            !string_field(track, "state", track_state) ||
            (visual && (!cJSON_IsObject(track) || cJSON_GetArraySize(track) != 6)))
            return false;
        if (visual && kind == "video") {
            if (++video_count != 1 || id != vision->video_track || source != vision->camera_id ||
                direction != "sendonly" || codec != "H264" || track_state != "paused")
                return false;
            continue;
        }
        if (kind != "audio" || codec != "opus" || track_state != "requested" ||
            (visual && !((id == "mic-1" && source == "mic" && direction == "sendonly") ||
                         (id == "speaker-1" && source == "speaker" && direction == "recvonly"))))
            return false;
        auto* target = direction == "sendonly"   ? &media.input_track
                       : direction == "recvonly" ? &media.output_track
                                                 : nullptr;
        if (!target || !target->empty())
            return false;
        *target = std::move(id);
    }
    if (media.input_track.empty() || media.output_track.empty() ||
        media.input_track == media.output_track || (visual && video_count != 1))
        return false;
    if (visual) {
        media.ai_vision = true;
    }
    return true;
}
// ack_fields 按原请求逐项校验 result；错误应答不能带伪造成功结果。
bool ack_fields(const cJSON* data, VoiceEvent& event) {
    if (!event.envelope.code || !string_field(data, "requestMethod", event.request_method, 64))
        return false;
    if (*event.envelope.code != 200)
        return !cJSON_GetObjectItemCaseSensitive(data, "result") &&
               string_field(data, "reason", event.reason);
    const auto* result = cJSON_GetObjectItemCaseSensitive(data, "result");
    if (!cJSON_IsObject(result))
        return false;
    if (event.request_method == "capabilityReport")
        return event.envelope.session.empty() &&
               uint_field(result, "acceptedRevision", event.revision);
    if (event.envelope.session.empty())
        return false;
    if (event.request_method == "audioStart" || event.request_method == "audioStop")
        return string_field(result, "inputId", event.input_id);
    if (event.request_method == "inputSend")
        return string_field(result, "inputId", event.input_id) &&
               string_field(result, "state", event.state) && event.state == "accepted";
    if (event.request_method == "respCancel") {
        // 原音频后端只确认目标；None的实际状态由会话层按已选transport额外强制核验。
        const bool state_present = cJSON_GetObjectItemCaseSensitive(result, "state") != nullptr;
        return string_field(result, "respId", event.response_id) &&
               (!state_present ||
                (string_field(result, "state", event.state) &&
                 (event.state == "cancelled" || event.state == "alreadyFinished")));
    }
    if (event.request_method == "mediaState")
        return uint_field(result, "mediaRevision", event.revision) &&
               string_field(result, "state", event.state) &&
               (event.state == "connected" || event.state == "disconnected" ||
                event.state == "failed");
    return false;
}
}  // namespace

// vision_profile_binding复用公共信封的UTF8/标识边界；分隔符不允许出现在标识，快照不碰撞。
std::string vision_profile_binding(const VoiceVisionProfile* profile) {
    if (!profile || !profile->ready || profile->max_publishers < 1 || profile->max_publishers > 4 ||
        profile->video_track == "mic-1" || profile->video_track == "speaker-1")
        return {};
    Envelope probe;
    probe.token = profile->camera_id;
    probe.session = profile->video_track;
    probe.method = "sessionCreate";
    std::string encoded;
    if (encode_envelope(probe, encoded) != DecodeResult::Ok)
        return {};
    return profile->camera_id + "\n" + profile->video_track + "\n" +
           std::to_string(profile->max_publishers);
}
WireResult capability_command(std::string token, const VoiceCapability& capability, Envelope& out,
                              const VoiceVisionProfile* vision) {
    out = {};
    if (!valid_capability(capability) ||
        ((capability.ai_vision || capability.human_call) && vision_profile_binding(vision).empty()))
        return WireResult::Invalid;
    Json params(cJSON_CreateObject(), cJSON_Delete);
    if (!params)
        return WireResult::Invalid;
    auto* purpose = cJSON_AddArrayToObject(params.get(), "purposes");
    auto* transport = cJSON_AddArrayToObject(params.get(), "transports");
    auto* features = cJSON_AddArrayToObject(params.get(), "features");
    if (capability.transport == VoiceTransport::None) {
        // text归离散模态；只有实际控制恢复装配才附加sessionSync，旧默认保持显式空数组。
        if (!purpose || !transport || !features ||
            !cJSON_AddItemToArray(purpose, cJSON_CreateString("aiConversation")) ||
            !cJSON_AddItemToArray(transport, cJSON_CreateString("none")) ||
            (capability.session_sync &&
             !cJSON_AddItemToArray(features, cJSON_CreateString("sessionSync"))) ||
            !cJSON_AddNumberToObject(params.get(), "revision",
                                     static_cast<double>(capability.revision)) ||
            !add_discrete_modes(params.get(), capability, false) ||
            !add_frontend_tools(params.get(), capability.client_display_theme))
            return WireResult::Invalid;
        return finish(std::move(token), {}, "capabilityReport", params.get(), out);
    }
    if (capability.human_voice_call) {
      auto *human = cJSON_AddObjectToObject(params.get(), "humanCall");
      auto *types =
          human ? cJSON_AddArrayToObject(human, "callTypes") : nullptr;
      if (!types ||
          !cJSON_AddStringToObject(human, "transport",
                                   capability.human_udp_av ? "udpAv"
                                                           : "webrtc") ||
          !cJSON_AddItemToArray(types, cJSON_CreateString("voice")) ||
          !cJSON_AddItemToArray(types, cJSON_CreateString("video")))
        return WireResult::Invalid;
    }
    if (capability.human_udp_av) {
        auto* profile = cJSON_AddObjectToObject(params.get(), "humanUdpAv");
        if (!profile || !cJSON_AddStringToObject(profile, "profile", "opus16-jpeg240-ctr128-v1")) return WireResult::Invalid;
    }
    if (!purpose || !transport || !features ||
        !cJSON_AddItemToArray(purpose, cJSON_CreateString("aiConversation")) ||
        (capability.ai_vision && !cJSON_AddItemToArray(purpose, cJSON_CreateString("aiVision"))) ||
        (capability.human_call && !cJSON_AddItemToArray(purpose, cJSON_CreateString("humanCall"))) ||
        !cJSON_AddItemToArray(
            transport,
            cJSON_CreateString(capability.transport == VoiceTransport::Udp ? "udp" : "webrtc")) ||
        ((capability.file_audio_output || capability.file_video_output) &&
         (!cJSON_AddItemToArray(purpose, cJSON_CreateString("mediaPlayback")) ||
          !cJSON_AddItemToArray(transport, cJSON_CreateString("none")))) ||
        ((capability.live_audio_receive || capability.live_video_receive) &&
         (!cJSON_AddItemToArray(purpose, cJSON_CreateString("liveBroadcast")) ||
          (capability.transport != VoiceTransport::WebRTC &&
           !cJSON_AddItemToArray(transport, cJSON_CreateString("webrtc"))))) ||
        !cJSON_AddItemToArray(features, cJSON_CreateString("audio")) ||
        ((capability.ai_vision || capability.human_call) &&
         !cJSON_AddItemToArray(features, cJSON_CreateString("video"))) ||
        (capability.human_call && !cJSON_AddItemToArray(features, cJSON_CreateString("localAccept"))) ||
        (capability.human_udp_av &&
         !cJSON_AddItemToArray(transport, cJSON_CreateString("udpAv"))) ||
        (capability.human_video_receive &&
         !cJSON_AddItemToArray(features, cJSON_CreateString("videoReceive"))) ||
        (capability.udp_reply_start_receipt &&
         !cJSON_AddItemToArray(features, cJSON_CreateString("udpReplyStartReceipt"))) ||
        (capability.session_sync &&
         !cJSON_AddItemToArray(features, cJSON_CreateString("sessionSync"))) ||
        (capability.ai_endpoint_handoff &&
         (!cJSON_AddItemToArray(features, cJSON_CreateString("localAccept")) ||
          !cJSON_AddItemToArray(features, cJSON_CreateString("aiEndpointHandoff")))) ||
        (capability.ai_endpoint_control_sync &&
         !cJSON_AddItemToArray(features, cJSON_CreateString("aiEndpointControlSync"))) ||
        !cJSON_AddNumberToObject(params.get(), "revision",
                                 static_cast<double>(capability.revision)) ||
        !cJSON_AddStringToObject(params.get(), "aecMode",
                                 capability.modes.device_aec ? "device" : "none") ||
        !add_modes(params.get(), capability.modes) ||
        !add_audio(params.get(), "audio", capability.audio) ||
        !add_capability_outputs(params.get(), capability) ||
        !add_live_receive(params.get(), capability) ||
        !add_frontend_tools(params.get(), capability.client_display_theme))
        return WireResult::Invalid;
    if (capability.ai_vision || capability.human_call) {
        auto* cameras = cJSON_AddArrayToObject(params.get(), "cameras");
        auto* camera = cameras ? cJSON_CreateObject() : nullptr;
        if (!camera || !cJSON_AddItemToArray(cameras, camera)) {
            cJSON_Delete(camera);
            return WireResult::Invalid;
        }
        auto* codecs = cJSON_AddArrayToObject(camera, "codecs");
        if (!cJSON_AddStringToObject(camera, "cameraId", vision->camera_id.c_str()) ||
            !cJSON_AddNumberToObject(camera, "maxPublishers", vision->max_publishers) || !codecs ||
            !cJSON_AddItemToArray(codecs, cJSON_CreateString("H264")))
            return WireResult::Invalid;
    }
    return finish(std::move(token), {}, "capabilityReport", params.get(), out);
}
WireResult create_voice_command(std::string token, const VoiceCapability& capability, Envelope& out,
                                const VoiceVisionProfile* vision) {
    out = {};
    // 视觉能力与本次会话解耦：capability.ai_vision 是设备能力，会话是否为aiVision三轨
    // 由调用方传入的vision快照决定；设备本地语音缺省两轨aiConversation，能力上报不变。
    const bool vision_session = capability.ai_vision && vision != nullptr;
    if (!valid_capability(capability) ||
        (vision_session && vision_profile_binding(vision).empty()))
        return WireResult::Invalid;
    Json params(cJSON_CreateObject(), cJSON_Delete);
    if (!params)
        return WireResult::Invalid;
    if (capability.transport == VoiceTransport::None) {
        if (!cJSON_AddStringToObject(params.get(), "purpose", "aiConversation") ||
            !cJSON_AddStringToObject(params.get(), "transport", "none") ||
            !cJSON_AddArrayToObject(params.get(), "tracks") ||
            !add_discrete_modes(params.get(), capability, false) ||
            !add_frontend_tools(params.get(), capability.client_display_theme))
            return WireResult::Invalid;
        return finish(std::move(token), {}, "sessionCreate", params.get(), out);
    }
    if (!cJSON_AddStringToObject(params.get(), "purpose",
                                 vision_session ? "aiVision" : "aiConversation") ||
        (vision_session &&
         !cJSON_AddStringToObject(params.get(), "cameraId", vision->camera_id.c_str())) ||
        !cJSON_AddStringToObject(params.get(), "transport",
                                 capability.transport == VoiceTransport::Udp ? "udp" : "webrtc") ||
        !add_modes(params.get(), capability.modes) ||
        !add_audio(params.get(), "audioParams", capability.audio) ||
        !add_discrete_modes(params.get(), capability) ||
        !add_frontend_tools(params.get(), !vision_session && capability.client_display_theme))
        return WireResult::Invalid;
    auto* tracks = cJSON_AddArrayToObject(params.get(), "tracks");
    if (!tracks)
        return WireResult::Invalid;
    for (bool input : {true, false}) {
        auto* track = cJSON_CreateObject();
        if (!track || !cJSON_AddItemToArray(tracks, track)) {
            cJSON_Delete(track);
            return WireResult::Invalid;
        }
        if (!cJSON_AddStringToObject(track, "trackId", input ? "mic-1" : "speaker-1") ||
            !cJSON_AddStringToObject(track, "kind", "audio") ||
            !cJSON_AddStringToObject(track, "sourceId", input ? "mic" : "speaker") ||
            !cJSON_AddStringToObject(track, "direction", input ? "sendonly" : "recvonly") ||
            !cJSON_AddStringToObject(track, "codec", "opus") ||
            !cJSON_AddStringToObject(track, "state", "requested"))
            return WireResult::Invalid;
    }
    if (vision_session) {
        auto* track = cJSON_CreateObject();
        if (!track || !cJSON_AddItemToArray(tracks, track)) {
            cJSON_Delete(track);
            return WireResult::Invalid;
        }
        if (!cJSON_AddStringToObject(track, "trackId", vision->video_track.c_str()) ||
            !cJSON_AddStringToObject(track, "kind", "video") ||
            !cJSON_AddStringToObject(track, "sourceId", vision->camera_id.c_str()) ||
            !cJSON_AddStringToObject(track, "direction", "sendonly") ||
            !cJSON_AddStringToObject(track, "codec", "H264") ||
            !cJSON_AddStringToObject(track, "state", "paused"))
            return WireResult::Invalid;
    }
    return finish(std::move(token), {}, "sessionCreate", params.get(), out);
}
WireResult media_offer_command(std::string token, std::string session, std::string_view sdp,
                               Envelope& out) {
    return media_offer_command(std::move(token), std::move(session), sdp, 1, out);
}
// media_offer_command 保留原首次协商入口；显式代次与下行解析共用浏览器安全整数边界。
WireResult media_offer_command(std::string token, std::string session, std::string_view sdp,
                               std::uint64_t revision, Envelope& out) {
    out = {};
    if (revision == 0 || revision > 9007199254740991ULL || session.empty() ||
        sdp.size() < 8 || sdp.size() > 49152 || sdp.substr(0, 5) != "v=0\r\n" ||
        sdp.find('\0') != std::string_view::npos)
        return WireResult::Invalid;
    Json params(cJSON_CreateObject(), cJSON_Delete);
    // 私有构造只接收已限界整数的十进制串，避免进入公共精确编码器前被cJSON的15位打印舍入。
    // 不是接受外部raw JSON；后续finish仍按公共信封解析、范围和重复键规则完整校验。
    const auto exact_revision = std::to_string(revision);
    if (!params || !cJSON_AddRawToObject(params.get(), "mediaRevision", exact_revision.c_str()))
        return WireResult::Invalid;
    auto* media = cJSON_AddObjectToObject(params.get(), "webrtc");
    const std::string owned(sdp);
    if (!media || !cJSON_AddStringToObject(media, "iceMode", "complete") ||
        !cJSON_AddStringToObject(media, "sdp", owned.c_str()))
        return WireResult::Invalid;
    return finish(std::move(token), std::move(session), "mediaOffer", params.get(), out);
}
WireResult voice_control(std::string token, std::string session, std::string method, std::string id,
                         std::uint64_t revision, Envelope& out, std::uint32_t packet_sequence,
                         VoiceTransport transport) {
    out = {};
    if (session.empty())
        return WireResult::Invalid;
    if (transport == VoiceTransport::None && method != "respCancel" && method != "sessionEnd")
        return WireResult::Unsupported;
    Json params(cJSON_CreateObject(), cJSON_Delete);
    if (!params)
        return WireResult::Invalid;
    if (method == "audioStart") {
        if (transport == VoiceTransport::Udp &&
            (packet_sequence == 0 || packet_sequence % 128U != 0 ||
             !cJSON_AddNumberToObject(params.get(), "firstPacketSeq", packet_sequence)))
            return WireResult::Invalid;
    } else if (method == "audioStop") {
        if (transport == VoiceTransport::Udp &&
            (packet_sequence % 128U != 0 ||
             !cJSON_AddNumberToObject(params.get(), "lastPacketSeq", packet_sequence)))
            return WireResult::Invalid;
    } else if (packet_sequence != 0)
        return WireResult::Invalid;
    if (transport == VoiceTransport::WebRTC && packet_sequence != 0)
        return WireResult::Invalid;
    if (method == "mediaState") {
        if (revision == 0 || revision > 9007199254740991ULL ||
            (id != "connected" && !(id == "disconnected" && transport == VoiceTransport::WebRTC)))
            return WireResult::Invalid;
        // 连通事实必须与Offer精确同代；同样只将已限界整数构造成JSON数字，不接受外部raw正文。
        const auto exact_revision = std::to_string(revision);
        if (!cJSON_AddRawToObject(params.get(), "mediaRevision", exact_revision.c_str()) ||
            !cJSON_AddStringToObject(params.get(), "state", id.c_str()))
            return WireResult::Invalid;
    } else {
        const char* field = nullptr;
        if (method == "audioStart" || method == "audioStop")
            field = "inputId";
        else if (method == "respCancel")
            field = "respId";
        else if (method == "sessionEnd" && (id == "userHangup" || id == "userStopped"))
            field = "reason";
        else
            return WireResult::Unsupported;
        if (id.empty() || id.size() > 128 || id.find('\0') != std::string::npos ||
            !cJSON_AddStringToObject(params.get(), field, id.c_str()))
            return WireResult::Invalid;
    }
    return finish(std::move(token), std::move(session), std::move(method), params.get(), out);
}
WireResult text_input_command(std::string token, std::string session, std::string input_id,
                              std::string_view text, bool audio, Envelope& out) {
    out = {};
    if (session.empty() || input_id.empty() || input_id.size() > 128 || text.empty() ||
        text.size() > 60000 || text.find('\0') != std::string_view::npos ||
        text.find_first_not_of(" \t\r\n") == std::string_view::npos)
        return WireResult::Invalid;
    for (const auto ch : input_id)
        if (static_cast<unsigned char>(ch) < 32 || ch == 127)
            return WireResult::Invalid;
    // 原send_text的60000字节保护保持；有序typed内容统一受公共编码后的整帧64KiB限制。
    return content_input_command(std::move(token), std::move(session), std::move(input_id),
                                 {{InputContentKind::Text, std::string(text), {}, {}}}, audio, out);
}
WireResult decode_voice_event(WireRoute route, std::string_view body, VoiceEvent& out,
                              const VoiceVisionProfile* vision) {
    out = {};
    VoiceEvent event;
    if (decode_envelope(body, event.envelope) != DecodeResult::Ok || event.envelope.request ||
        event.envelope.message_sequence)
        return WireResult::Invalid;
    Json data(ur::device::parse_json(event.envelope.payload.c_str()), cJSON_Delete);
    if (!data)
        return WireResult::Invalid;
    const auto& message = event.envelope;
    if (route == WireRoute::Message &&
        (message.method != "requestAck" || !message.code || message.event_sequence))
        return WireResult::WrongRoute;
    bool valid = false;
    if (message.method == "requestAck") {
        valid =
            message.code.has_value() && !message.event_sequence && ack_fields(data.get(), event);
        if (valid && ((event.request_method == "inputSend") != (route == WireRoute::Message)))
            return WireResult::WrongRoute;
    } else if (message.method == "sessionCreated") {
        valid = message.code.has_value() && !message.event_sequence;
        if (valid)
            valid = *message.code == 200
                        ? !message.session.empty() && read_media(data.get(), event.media, vision)
                        : string_field(data.get(), "reason", event.reason);
        if (valid && *message.code == 200) {
            AIControlBinding binding;
            valid = decode_ai_control_created_binding(body, binding) == WireResult::Ok;
            event.endpoint_epoch = binding.endpoint_epoch;
            event.capability_revision = binding.capability_revision;
        }
    } else if (message.method == "mediaAnswer") {
        valid = message.code.has_value() && !message.event_sequence && !message.session.empty();
        if (valid && *message.code == 200) {
            const auto* rtc = cJSON_GetObjectItemCaseSensitive(data.get(), "webrtc");
            std::string mode;
            valid = uint_field(data.get(), "mediaRevision", event.revision) &&
                    string_field(rtc, "iceMode", mode) && mode == "complete" &&
                    sdp_field(rtc, "sdp", event.answer_sdp);
        } else if (valid)
            valid = string_field(data.get(), "reason", event.reason);
    } else if (message.method == "sessionEnded") {
        valid = !message.session.empty() &&
                (message.code.has_value() != message.event_sequence.has_value()) &&
                string_field(data.get(), "reason", event.reason);
        if (valid && (!message.code || *message.code == 200))
            valid = string_field(data.get(), "state", event.state) && event.state == "ended";
    } else if (message.method == "sessionError") {
        const auto* retryable = cJSON_GetObjectItemCaseSensitive(data.get(), "retryable");
        event.scope = "session";
        valid = message.code && *message.code >= 400 && message.event_sequence &&
                !message.session.empty() && cJSON_IsBool(retryable) &&
                string_field(data.get(), "reason", event.reason);
        if (valid && cJSON_GetObjectItemCaseSensitive(data.get(), "scope"))
            valid = string_field(data.get(), "scope", event.scope);
        if (valid && event.scope != "session" && event.scope != "response" &&
            event.scope != "input" && event.scope != "tool")
            valid = false;
        if (valid && (event.scope == "response" || event.scope == "tool"))
            valid = string_field(data.get(), "respId", event.response_id);
        if (valid && event.scope == "tool")
            valid = string_field(data.get(), "toolCallId", event.tool_call_id);
        if (valid && event.scope == "input")
            valid = string_field(data.get(), "inputId", event.input_id);
        if (valid && cJSON_GetObjectItemCaseSensitive(data.get(), "mediaRevision"))
            valid = uint_field(data.get(), "mediaRevision", event.revision, true);
        event.retryable = cJSON_IsTrue(retryable);
    } else if (message.method == "sessionEstablished") {
        valid = !message.code && message.event_sequence && !message.session.empty() &&
                string_field(data.get(), "state", event.state) && event.state == "established" &&
                uint_field(data.get(), "mediaRevision", event.revision, true);
        // 只有新目标使用成对公开恢复绑定；缺省维持旧应答，不填造epoch或能力修订。
        const auto* epoch = cJSON_GetObjectItemCaseSensitive(data.get(), "endpointEpoch");
        const auto* revision = cJSON_GetObjectItemCaseSensitive(data.get(), "capabilityRevision");
        if (epoch || revision)
            valid = valid && epoch && revision &&
                    uint_field(data.get(), "endpointEpoch", event.endpoint_epoch) &&
                    uint_field(data.get(), "capabilityRevision", event.capability_revision);
    } else if (message.method == "respAudioStart" || message.method == "respAudioDone" ||
               message.method == "audioSpeechInterrupted") {
        valid = !message.code && message.event_sequence && !message.session.empty() &&
                string_field(data.get(), "respId", event.response_id) &&
                string_field(data.get(), "trackId", event.track_id) &&
                uint_field(data.get(), "mediaRevision", event.revision);
        if (valid && message.method == "respAudioStart") {
            const auto* first = cJSON_GetObjectItemCaseSensitive(data.get(), "firstPacketSeq");
            const auto* nonce = cJSON_GetObjectItemCaseSensitive(data.get(), "udpNonce");
            if (first || nonce) {
                valid = first && nonce && hex_bytes(data.get(), "udpNonce", event.reply_nonce) &&
                        cJSON_IsNumber(first) && first->valuedouble >= 128 &&
                        first->valuedouble <= 4294967295.0 &&
                        std::floor(first->valuedouble) == first->valuedouble;
                if (valid) {
                    event.first_packet_sequence = static_cast<std::uint32_t>(first->valuedouble);
                    valid = event.first_packet_sequence % 128U == 0;
                    event.udp_boundary = valid;
                }
            }
        }
        if (valid && message.method == "respAudioDone") {
            const auto* last = cJSON_GetObjectItemCaseSensitive(data.get(), "lastPacketSeq");
            if (last) {
                valid = cJSON_IsNumber(last) && last->valuedouble >= 0 &&
                        last->valuedouble <= 4294967295.0 &&
                        std::floor(last->valuedouble) == last->valuedouble;
                if (valid) {
                    event.last_packet_sequence = static_cast<std::uint32_t>(last->valuedouble);
                    valid = event.last_packet_sequence % 128U == 0;
                    event.udp_boundary = valid;
                }
            }
        }
    } else
        return WireResult::Unsupported;
    if (!valid)
        return WireResult::Invalid;
    out = std::move(event);
    return WireResult::Ok;
}
// decode_voice_vision_control不借音频事件的宽松字段；仅当前已有轨publish/stop原请求。
WireResult decode_voice_vision_control(WireRoute route, std::string_view body,
                                      VoiceVisionCommand& out) {
    out = {};
    if (route != WireRoute::Signaling) return WireResult::WrongRoute;
    VoiceVisionCommand command;
    if (decode_envelope(body, command.envelope) != DecodeResult::Ok)
        return WireResult::Invalid;
    const auto& message = command.envelope;
    if (message.method != "trackControl") return WireResult::Unsupported;
    if (!message.request || message.code || message.message_sequence ||
        !message.event_sequence || message.session.empty())
        return WireResult::Invalid;
    Json params(ur::device::parse_json(message.payload.c_str()), cJSON_Delete);
    std::string operation;
    if (!params || cJSON_GetArraySize(params.get()) != 3 ||
        !string_field(params.get(), "trackId", command.track_id) ||
        !string_field(params.get(), "operation", operation) ||
        !uint_field(params.get(), "mediaRevision", command.media_revision))
        return WireResult::Invalid;
    if (operation == "publish") command.operation = VoiceVisionOperation::Publish;
    else if (operation == "stop") command.operation = VoiceVisionOperation::Stop;
    else return WireResult::Unsupported;
    out = std::move(command);
    return WireResult::Ok;
}
// voice_vision_receipt复用严格公共编码；成功三字段/失败单字段，关联原token而非新事件。
WireResult voice_vision_receipt(const VoiceVisionCommand& command, int code,
                               std::string_view state, std::string_view reason, Envelope& out) {
    out = {};
    constexpr std::string_view reasons[] = {"unsupportedCapability", "resourceBusy", "tokenConflict",
                                          "staleRevision", "sessionExpired", "mediaFailed",
                                          "operationTimedOut", "deviceRejected"};
    if ((code == 200 && (!reason.empty() ||
                        (command.operation == VoiceVisionOperation::Publish ? state != "active"
                                                                            : state != "stopped"))) ||
        (code != 200 && (code < 400 || code > 599 || !state.empty() ||
                        std::find(std::begin(reasons), std::end(reasons), reason) == std::end(reasons))))
        return WireResult::Invalid;
    std::string original;
    VoiceVisionCommand checked;
    if (encode_envelope(command.envelope, original) != DecodeResult::Ok ||
        decode_voice_vision_control(WireRoute::Signaling, original, checked) != WireResult::Ok ||
        checked.track_id != command.track_id || checked.media_revision != command.media_revision ||
        checked.operation != command.operation)
        return WireResult::Invalid;
    Json data(cJSON_CreateObject(), cJSON_Delete);
    const auto value = std::string(code == 200 ? state : reason);
    if (!data || !cJSON_AddStringToObject(data.get(), code == 200 ? "state" : "reason", value.c_str()))
        return WireResult::Invalid;
    if (code == 200 &&
        (!cJSON_AddStringToObject(data.get(), "trackId", command.track_id.c_str()) ||
         !cJSON_AddNumberToObject(data.get(), "mediaRevision", static_cast<double>(command.media_revision))))
        return WireResult::Invalid;
    Envelope receipt;
    const auto result = finish(command.envelope.token, command.envelope.session, "trackState",
                               data.get(), receipt);
    if (result != WireResult::Ok) return result;
    receipt.request = false;
    receipt.code = code;
    std::string encoded;
    if (encode_envelope(receipt, encoded) != DecodeResult::Ok) return WireResult::Invalid;
    out = std::move(receipt);
    return WireResult::Ok;
}

// decode_reply_start_receipt不复用下行事件分派，严格拒绝params、水位及任何额外数据字段。
WireResult decode_reply_start_receipt(WireRoute route, std::string_view body,
                                      UDPReplyStartReceipt& out) {
    out = {};
    if (route != WireRoute::Signaling)
        return WireResult::WrongRoute;
    UDPReplyStartReceipt fact;
    if (decode_envelope(body, fact.envelope) != DecodeResult::Ok || fact.envelope.request ||
        fact.envelope.method != "respAudioStart" || fact.envelope.session.empty() ||
        fact.envelope.session.size() > 128 || !fact.envelope.code || *fact.envelope.code != 200 ||
        fact.envelope.event_sequence || fact.envelope.message_sequence)
        return WireResult::Invalid;
    Json data(parse_json(fact.envelope.payload.c_str()), cJSON_Delete);
    if (!data || cJSON_GetArraySize(data.get()) != 3 ||
        !string_field(data.get(), "respId", fact.response_id) ||
        !string_field(data.get(), "trackId", fact.track_id) ||
        !uint_field(data.get(), "mediaRevision", fact.media_revision))
        return WireResult::Invalid;
    out = std::move(fact);
    return WireResult::Ok;
}
// reply_start_receipt复核原五字段UDP事件，编码成功不是安装证明，只有原客户端实际完成后可调用发布。
WireResult reply_start_receipt(const VoiceEvent& start, Envelope& out) {
    out = {};
    std::string original;
    VoiceEvent checked;
    if (encode_envelope(start.envelope, original) != DecodeResult::Ok ||
        decode_voice_event(WireRoute::Signaling, original, checked) != WireResult::Ok ||
        checked.envelope.method != "respAudioStart" || !checked.udp_boundary ||
        checked.revision == 0 || checked.response_id != start.response_id ||
        checked.track_id != start.track_id || checked.revision != start.revision ||
        checked.reply_nonce != start.reply_nonce ||
        checked.first_packet_sequence != start.first_packet_sequence)
        return WireResult::Invalid;
    Json data(parse_json(checked.envelope.payload.c_str()), cJSON_Delete);
    if (!data || cJSON_GetArraySize(data.get()) != 5)
        return WireResult::Invalid;
    data.reset(cJSON_CreateObject());
    if (!data || !cJSON_AddStringToObject(data.get(), "respId", checked.response_id.c_str()) ||
        !cJSON_AddStringToObject(data.get(), "trackId", checked.track_id.c_str()) ||
        !cJSON_AddNumberToObject(data.get(), "mediaRevision",
                                 static_cast<double>(checked.revision)))
        return WireResult::Invalid;
    char* bytes = cJSON_PrintUnformatted(data.get());
    if (!bytes)
        return WireResult::Invalid;
    Envelope fact;
    fact.token = checked.envelope.token;
    fact.session = checked.envelope.session;
    fact.method = "respAudioStart";
    fact.request = false;
    fact.code = 200;
    fact.payload = bytes;
    cJSON_free(bytes);
    std::string encoded;
    UDPReplyStartReceipt parsed;
    if (encode_envelope(fact, encoded) != DecodeResult::Ok ||
        decode_reply_start_receipt(WireRoute::Signaling, encoded, parsed) != WireResult::Ok)
        return WireResult::Invalid;
    out = std::move(fact);
    return WireResult::Ok;
}
}  // namespace ur::device
