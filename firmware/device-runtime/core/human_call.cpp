// 文件说明：设备侧人际语音／视频共用状态机；模式仅改变严格轨合同与媒体策略，回调由单事件线程推进。
#include "ur/device/human_call.hpp"
#include <cJSON.h>
#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>
#include "ur/device/endpoint_wire.hpp"
#include "ur/device/envelope.hpp"
#include "ur/device/focus_engine.hpp"
#include "ur/device/json_parse.hpp"

namespace ur::device {
// human_rejection_method_name 只投影编译期固定闭集，越界回落unknown。
std::string_view human_rejection_method_name(HumanRejectionMethod method) {
    static constexpr std::string_view names[] = {
        "unknown",          "requestAck",         "sessionCreated",  "sessionInvite",
        "mediaAnswer",      "sessionEstablished", "sessionEnded",    "callState",
        "trackState",       "participantChanged", "contextSnapshot", "endpointState",
        "capabilityReport", "sessionCreate",      "sessionAccept",   "sessionReject",
        "mediaOffer",       "mediaState",         "sessionEnd",      "trackControl",
        "callControl"};
    const auto index = static_cast<unsigned>(method);
    return index < sizeof(names) / sizeof(names[0]) ? names[index] : names[0];
}

// record_receive_rejection
// 不改变receive原返回值；失败解码只记录未知，不保留自由字段。
void HumanCallClient::record_receive_rejection(std::string_view body, HumanCallState previous) {
    if (!rejection_diagnostics_ || rejection_facts_)
        return;
    HumanCallRejectionFacts fact;
    fact.state = previous;
    Envelope message;
    if (decode_envelope(body, message) == DecodeResult::Ok) {
        for (unsigned i = 1; i <= static_cast<unsigned>(HumanRejectionMethod::EndpointState); ++i) {
            const auto method = static_cast<HumanRejectionMethod>(i);
            if (message.method == human_rejection_method_name(method))
                fact.method = method;
        }
        if (message.code && *message.code >= 200 && *message.code <= 599)
            fact.code = message.code;
        if (fact.method == HumanRejectionMethod::RequestAck) {
            const auto json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>(
                parse_json_with_length_opts(message.payload.c_str(), message.payload.size() + 1,
                                            nullptr, true),
                cJSON_Delete);
            const auto* value = cJSON_GetObjectItemCaseSensitive(json.get(), "requestMethod");
            if (cJSON_IsString(value)) {
                for (unsigned i = static_cast<unsigned>(HumanRejectionMethod::CapabilityReport);
                     i <= static_cast<unsigned>(HumanRejectionMethod::CallControl); ++i) {
                    const auto method = static_cast<HumanRejectionMethod>(i);
                    if (std::string_view(value->valuestring) == human_rejection_method_name(method))
                        fact.request_method = method;
                }
            }
        }
    }
    rejection_facts_ = fact;
}
namespace {
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
// field 读取有界标识，拒绝控制字符而不反射不可信正文。
bool field(const cJSON* object, const char* key, std::string_view expected) {
    const auto* value = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(value) && std::string_view(value->valuestring) == expected;
}
// string_field 读取会话内有界标识，不允许NUL或换行。
bool string_field(const cJSON* object, const char* key, std::string& out, std::size_t limit) {
    const auto* value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsString(value))
        return false;
    out = value->valuestring;
    return !out.empty() && out.size() <= limit &&
           std::none_of(out.begin(), out.end(), [](unsigned char c) { return c < 32 || c == 127; });
}
// object_field 只解析已经过公共信封归一化的单个方法正文。
Json object_field(const Envelope& envelope) {
    return Json(ur::device::parse_json_with_length_opts(envelope.payload.c_str(),
                                                        envelope.payload.size() + 1, nullptr, true),
                cJSON_Delete);
}
// exact_keys 防止未知或多余字段悄然改变当前设备的授权语义。
bool exact_keys(const cJSON* object, std::initializer_list<std::string_view> keys) {
    if (!cJSON_IsObject(object))
        return false;
    std::size_t count = 0;
    for (const auto* item = object->child; item; item = item->next) {
        if (std::find(keys.begin(), keys.end(), std::string_view(item->string)) == keys.end())
            return false;
        ++count;
    }
    return count == keys.size();
}
// call_revision_field
// 保持JSON安全整数边界；初始/拒绝版本可0，成功完成版本必须非0。
bool call_revision_field(const cJSON* object, const char* key, std::uint64_t& result,
                         bool zero_allowed) {
    const auto* value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) ||
        value->valuedouble < (zero_allowed ? 0 : 1) || value->valuedouble > 9007199254740991.0 ||
        std::floor(value->valuedouble) != value->valuedouble)
        return false;
    result = static_cast<std::uint64_t>(value->valuedouble);
    return true;
}
// tracks_payload
// 限定唯一语音轨、原视频三轨及显式授权的第四接收轨；不从来电扩权或接纳未知sourceType。
bool tracks_payload(const cJSON* tracks, const HumanCallConfig& config, bool udp_av = false) {
    const bool voice = config.mode == HumanCallMode::Voice;
    const int count = voice ? 1 : cJSON_GetArraySize(tracks);
    if (!voice && count != 3 && !(count == 4 && config.receive_video))
        return false;
    if (!cJSON_IsArray(tracks) || cJSON_GetArraySize(tracks) != count)
        return false;
    struct Expected {
        const char* id;
        const char* kind;
        const char* source;
        const char* direction;
        const char* codec;
    };
    constexpr Expected voice_track[] = {{"voice", "audio", "microphone", "sendrecv", "opus"}};
    constexpr Expected video_tracks[] = {{"mic-1", "audio", "mic", "sendonly", "opus"},
                                         {"speaker-1", "audio", "speaker", "recvonly", "opus"},
                                         {"video-1", "video", nullptr, "sendonly", "H264"},
                                         {"video-in", "video", "remote", "recvonly", "H264"}};
    const auto* expected = voice ? voice_track : video_tracks;
    for (int i = 0; i < count; ++i) {
        const auto* track = cJSON_GetArrayItem(tracks, i);
        const bool keys =
            voice
                ? exact_keys(track, {"trackId", "kind", "sourceId", "sourceType", "direction",
                                     "codec", "state"})
                : exact_keys(track, {"trackId", "kind", "sourceId", "direction", "codec", "state"});
        if (!keys || (voice && !field(track, "sourceType", "microphone")) ||
            !field(track, "trackId", expected[i].id) || !field(track, "kind", expected[i].kind) ||
            !field(track, "sourceId", expected[i].source ? expected[i].source : config.camera_id) ||
            !field(track, "direction", expected[i].direction) ||
            !field(track, "codec", udp_av && !voice && i >= 2 ? "jpeg" : expected[i].codec) || !field(track, "state", "requested"))
            return false;
    }
    return true;
}
// receive_video_selected 只在整份轨已严格校验后读取第四轨，不能由页面布尔值补授权。
bool receive_video_selected(const Envelope& message) {
    auto data = object_field(message);
    return cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(data.get(), "tracks")) == 4;
}
// invite_payload 仅接受对应轨、本人接听及有效截止；新候选出处必须显式SDK支持且原源不自指。
bool invite_payload(const Envelope& message, std::uint64_t wall_ms, std::uint64_t& expires,
                    const HumanCallConfig& config, bool endpoint_enabled,
                    std::string& source_session, std::string& handoff_id) {
    auto root = object_field(message);
    const auto* deadline = cJSON_GetObjectItemCaseSensitive(root.get(), "expiresAt");
    const auto* handoff = cJSON_GetObjectItemCaseSensitive(root.get(), "handoff");
    source_session.clear();
    handoff_id.clear();
    if (handoff && (!endpoint_enabled || config.mode != HumanCallMode::Voice ||
                    !exact_keys(handoff, {"sourceSessionId", "handoffId"}) ||
                    !string_field(handoff, "sourceSessionId", source_session, 64) ||
                    !string_field(handoff, "handoffId", handoff_id, 128) ||
                    source_session == message.session || message.session.size() > 64))
        return false;
    std::string group;
    const bool keys = handoff
        ? exact_keys(root.get(), {"purpose", "callType", "admission", "transport", "expiresAt", "tracks", "callGroupId", "handoff"})
        : exact_keys(root.get(), {"purpose", "callType", "admission", "transport", "expiresAt", "tracks", "callGroupId"});
    if (!keys || !field(root.get(), "purpose", "humanCall") ||
        !field(root.get(), "callType",
               config.mode == HumanCallMode::Voice ? "voice" : "video") ||
        !field(root.get(), "admission", "accept") ||
        (config.udp_only && !field(root.get(), "transport", "udpAv")) ||
        (!field(root.get(), "transport", "webrtc") &&
         !(config.udp_av && field(root.get(), "transport", "udpAv"))) ||
        !string_field(root.get(), "callGroupId", group, 128) ||
        !cJSON_IsNumber(deadline) || !std::isfinite(deadline->valuedouble) ||
        deadline->valuedouble <= static_cast<double>(wall_ms) ||
        deadline->valuedouble > 9007199254740991.0 ||
        std::floor(deadline->valuedouble) != deadline->valuedouble ||
        !tracks_payload(cJSON_GetObjectItemCaseSensitive(root.get(), "tracks"),
                        config, field(root.get(), "transport", "udpAv")))
      return false;
    expires = static_cast<std::uint64_t>(deadline->valuedouble);
    return true;
}
// created_payload 只接受原请求对应传输与轨；组ID不等于本端独立sessionId。
bool created_payload(const Envelope& message, const HumanCallConfig& config) {
    auto root = object_field(message);
    std::string group;
    const auto* selected = cJSON_GetObjectItemCaseSensitive(root.get(), "selected");
    return exact_keys(root.get(), {"state", "callGroupId", "selected", "tracks"}) &&
           field(root.get(), "state", "inviting") &&
           string_field(root.get(), "callGroupId", group, 128) &&
           exact_keys(selected, {"transport"}) &&
           field(selected, "transport", config.udp_av ? "udpAv" : "webrtc") &&
           tracks_payload(cJSON_GetObjectItemCaseSensitive(root.get(), "tracks"), config, config.udp_av);
}
// create_payload 用结构化JSON生成设备目标，禁止将界面字符串拼接成平台命令。
std::string create_payload(std::string_view target, const HumanCallConfig& config) {
    const auto separator = target.find(':');
    if (target.size() < 3 || target.size() > 128 || separator == 0 ||
        separator == std::string_view::npos || separator == target.size() - 1 ||
        target.substr(0, separator).find_first_of("/+#") != std::string_view::npos ||
        target.substr(separator + 1).find_first_of("/+#") != std::string_view::npos ||
        std::any_of(target.begin(), target.end(),
                    [](unsigned char value) { return value < 32 || value == 127; }))
        return {};
    Json root(cJSON_CreateObject(), cJSON_Delete);
    if (!root || !cJSON_AddStringToObject(root.get(), "purpose", "humanCall") ||
        !cJSON_AddStringToObject(root.get(), "transport", config.udp_av ? "udpAv" : "webrtc") ||
        !cJSON_AddStringToObject(root.get(), "callType",
                                 config.mode == HumanCallMode::Voice ? "voice" : "video"))
        return {};
    auto* party = cJSON_AddObjectToObject(root.get(), "targetParty");
    const std::string owned(target);
    if (!party || !cJSON_AddStringToObject(party, "kind", "device") ||
        !cJSON_AddStringToObject(party, "id", owned.c_str()))
        return {};
    auto* tracks = cJSON_AddArrayToObject(root.get(), "tracks");
    if (!tracks)
        return {};
    struct Track {
        const char* id;
        const char* kind;
        const char* source;
        const char* direction;
        const char* codec;
    };
    constexpr Track voice_track[] = {{"voice", "audio", "microphone", "sendrecv", "opus"}};
    constexpr Track video_tracks[] = {{"mic-1", "audio", "mic", "sendonly", "opus"},
                                      {"speaker-1", "audio", "speaker", "recvonly", "opus"},
                                      {"video-1", "video", nullptr, "sendonly", "H264"},
                                      {"video-in", "video", "remote", "recvonly", "H264"}};
    const bool voice = config.mode == HumanCallMode::Voice;
    const auto* expected = voice ? voice_track : video_tracks;
    const int count = voice ? 1 : (config.receive_video ? 4 : 3);
    for (int index = 0; index < count; ++index) {
        const auto& item = expected[index];
        auto* track = cJSON_CreateObject();
        if (!track)
            return {};
        cJSON_AddItemToArray(tracks, track);
        if (!cJSON_AddStringToObject(track, "trackId", item.id) ||
            !cJSON_AddStringToObject(track, "kind", item.kind) ||
            !cJSON_AddStringToObject(track, "sourceId",
                                     item.source ? item.source : config.camera_id.c_str()) ||
            (voice && !cJSON_AddStringToObject(track, "sourceType", "microphone")) ||
            !cJSON_AddStringToObject(track, "direction", item.direction) ||
            !cJSON_AddStringToObject(track, "codec",
                                     config.udp_av && !voice && item.kind == std::string_view("video")
                                         ? "jpeg" : item.codec) ||
            !cJSON_AddStringToObject(track, "state", "requested"))
            return {};
    }
    char* bytes = cJSON_PrintUnformatted(root.get());
    if (!bytes)
        return {};
    std::string result(bytes);
    cJSON_free(bytes);
    return result;
}
// sdp_payload 从完整WebRTC应答读取SDP，拒绝伪装成空媒体或更高代次。
bool sdp_payload(const Envelope& message, std::string& sdp) {
    auto root = object_field(message);
    const auto* revision = cJSON_GetObjectItemCaseSensitive(root.get(), "mediaRevision");
    const auto* webrtc = cJSON_GetObjectItemCaseSensitive(root.get(), "webrtc");
    if (!exact_keys(root.get(), {"mediaRevision", "webrtc"}) || !cJSON_IsNumber(revision) ||
        revision->valuedouble != 1 || !exact_keys(webrtc, {"iceMode", "sdp"}) ||
        !field(webrtc, "iceMode", "complete"))
        return false;
    const auto* value = cJSON_GetObjectItemCaseSensitive(webrtc, "sdp");
    if (!cJSON_IsString(value))
        return false;
    sdp = value->valuestring;
    return sdp.size() >= 8 && sdp.size() <= 49152 && sdp.rfind("v=0\r\n", 0) == 0;
}
// udp_av_answer 严格解析当前UDP应答；不接受WebRTC字段、反射nonce或任意主机名。
bool udp_av_answer(const Envelope& message, CallMediaPort& media) {
 auto root=object_field(message);
 const auto* rev=cJSON_GetObjectItemCaseSensitive(root.get(),"mediaRevision");
 const auto* body=cJSON_GetObjectItemCaseSensitive(root.get(),"udpAv");
 const auto* port=cJSON_GetObjectItemCaseSensitive(body,"port");
 CallUDPAVParameters params;
 if (!exact_keys(root.get(),{"mediaRevision","udpAv"}) || !cJSON_IsNumber(rev) || rev->valuedouble!=1 ||
     !exact_keys(body,{"profile","server","port","key","upNonce","downNonce"}) || !field(body,"profile","opus16-jpeg240-ctr128-v1") ||
     !string_field(body,"server",params.server,15) || !cJSON_IsNumber(port) || !std::isfinite(port->valuedouble) || port->valuedouble<1 || port->valuedouble>65535 || std::floor(port->valuedouble)!=port->valuedouble) return false;
 unsigned octet=0,count=0,digits=0,first=0;
 for (std::size_t i=0;i<=params.server.size();++i) {
  const char c=i==params.server.size()?'.':params.server[i];
  if (c=='.') { if (!digits || octet>255 || count>=4) return false; if(count==0) first=octet; ++count;octet=digits=0; }
  else { if(c<'0'||c>'9'||++digits>3) return false;octet=octet*10+(c-'0'); }
 }
 if(count!=4 || first==0 || first>=224) return false;
 auto hex=[&](const char* name, auto& output) {
  const auto* v=cJSON_GetObjectItemCaseSensitive(body,name);
  if(!cJSON_IsString(v)||std::char_traits<char>::length(v->valuestring)!=output.size()*2) return false;
  auto digit=[](char c)->int { if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1; };
  for(std::size_t i=0;i<output.size();++i) { const int a=digit(v->valuestring[i*2]),b=digit(v->valuestring[i*2+1]);if(a<0||b<0)return false;output[i]=static_cast<std::uint8_t>((a<<4)|b); } return true;
 };
 params.port=static_cast<std::uint16_t>(port->valuedouble);
 return hex("key",params.key)&&hex("upNonce",params.up)&&hex("downNonce",params.down)&&params.up!=params.down&&media.apply_udp_av_answer(params);
}
// offer_payload 将浏览器完成候选收集的文本编码为结构化JSON，不拼接原始SDP。
std::string offer_payload(std::string_view sdp) {
    if (sdp.size() < 8 || sdp.size() > 49152 || sdp.substr(0, 5) != "v=0\r\n" ||
        sdp.find('\0') != std::string_view::npos)
        return {};
    Json root(cJSON_CreateObject(), cJSON_Delete);
    if (!root || !cJSON_AddNumberToObject(root.get(), "mediaRevision", 1))
        return {};
    auto* media = cJSON_AddObjectToObject(root.get(), "webrtc");
    const std::string owned(sdp);
    if (!media || !cJSON_AddStringToObject(media, "iceMode", "complete") ||
        !cJSON_AddStringToObject(media, "sdp", owned.c_str()))
        return {};
    char* bytes = cJSON_PrintUnformatted(root.get());
    if (!bytes)
        return {};
    std::string result(bytes);
    cJSON_free(bytes);
    return result;
}
// ack_method 只提取请求方法及必填结果对象，具体状态再由调用点核对。
bool ack_method(const Envelope& message, std::string& method) {
    auto root = object_field(message);
    return cJSON_IsObject(root.get()) && string_field(root.get(), "requestMethod", method, 64) &&
           cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(root.get(), "result"));
}
// capability_accepted
// 只接受原能力revision1的完整成功ACK，不能用旧或错误修订推进Available。
bool capability_accepted(const cJSON* data) {
    const auto* result = cJSON_GetObjectItemCaseSensitive(data, "result");
    const auto* revision = cJSON_GetObjectItemCaseSensitive(result, "acceptedRevision");
    return exact_keys(data, {"requestMethod", "result"}) &&
           field(data, "requestMethod", "capabilityReport") &&
           exact_keys(result, {"acceptedRevision"}) && cJSON_IsNumber(revision) &&
           revision->valuedouble == 1;
}
// capability_conflict 仅识别完整固定409原因；未知失败不得当作可恢复连接竞态。
bool capability_conflict(const cJSON* data) {
    return exact_keys(data, {"requestMethod", "reason"}) &&
           field(data, "requestMethod", "capabilityReport") &&
           field(data, "reason", "capabilityNotAccepted");
}
// camera_id 校验宿主已声明标识，不允许无效相机配置退回默认front。
bool camera_id(const std::string& camera) {
    return !camera.empty() && camera.size() <= 128 &&
           std::none_of(camera.begin(), camera.end(),
                        [](unsigned char value) { return value < 32 || value == 127; });
}
// media_config_valid 纯语音不校验／申请相机，但必须由真实候选驱动显式支持；非法枚举不会降级视频。
bool media_config_valid(const HumanCallConfig& config, const CallMediaPort& media) {
    return (config.mode == HumanCallMode::Video && camera_id(config.camera_id) &&
            (!config.receive_video || media.video_receive_supported())) ||
           (config.mode == HumanCallMode::Voice && !config.receive_video &&
            media.voice_supported());
}
// capability_payload
// 默认保留原独立连接能力；仅显式支持时报告participantRoster，不从人际用途推导。
std::string capability_payload(const HumanCallConfig& config) {
    Json root(
        ur::device::parse_json(
            config.mode == HumanCallMode::Voice
                ? R"({"revision":1,"purposes":["humanCall"],"transports":["webrtc"],"features":["audio","localAccept"],"audio":{"format":"opus","sampleRate":48000,"channels":1,"frameDuration":20}})"
                : R"({"revision":1,"purposes":["humanCall"],"transports":["webrtc"],"features":["audio","video","localAccept"],"audio":{"format":"opus","sampleRate":48000,"channels":1,"frameDuration":20},"cameras":[{"cameraId":"front","codecs":["H264"],"maxPublishers":1}]})"),
        cJSON_Delete);
    if (config.udp_av) {
        auto* transports=cJSON_GetObjectItemCaseSensitive(root.get(),"transports");
        auto* profile=cJSON_AddObjectToObject(root.get(),"humanUdpAv");
        if (config.mode!=HumanCallMode::Video || !cJSON_IsArray(transports) || !profile ||
            !cJSON_AddItemToArray(transports,cJSON_CreateString("udpAv")) ||
            !cJSON_AddStringToObject(profile,"profile","opus16-jpeg240-ctr128-v1")) return {};
    }
    if (config.participant_roster) {
        Json feature(cJSON_CreateString("participantRoster"), cJSON_Delete);
        auto* features = cJSON_GetObjectItemCaseSensitive(root.get(), "features");
        if (!feature || !cJSON_IsArray(features))
            return {};
        auto* transferred = feature.release();  // 成功加入后归JSON树，失败重新由局部对象释放。
        if (!cJSON_AddItemToArray(features, transferred)) {
            feature.reset(transferred);
            return {};
        }
    }
    if (config.receive_video) {
        auto* features = cJSON_GetObjectItemCaseSensitive(root.get(), "features");
        if (!cJSON_IsArray(features) ||
            !cJSON_AddItemToArray(features, cJSON_CreateString("videoReceive")))
            return {};
    }
    if (config.mode == HumanCallMode::Video) {
        auto* source =
            cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(root.get(), "cameras"), 0);
        Json identifier(cJSON_CreateString(config.camera_id.c_str()), cJSON_Delete);
        if (!root || !source || !identifier)
            return {};
        auto* tree_identifier =
            identifier.release();  // 相机节点交给JSON树；替换失败仍由局部对象释放。
        if (!cJSON_ReplaceItemInObjectCaseSensitive(source, "cameraId", tree_identifier)) {
            identifier.reset(tree_identifier);
            return {};
        }
    }
    auto* raw = cJSON_PrintUnformatted(root.get());
    if (!raw)
        return {};
    std::string result(raw);
    cJSON_free(raw);
    return result;
}
}  // namespace

// human_call_invitation 不用子串猜用途，嵌套purpose不能改变根邀请归属。
bool human_call_invitation(std::string_view body) {
    Envelope message;
    if (decode_envelope(body, message) != DecodeResult::Ok || message.method != "sessionInvite" ||
        !message.request)
        return false;
    const auto params = object_field(message);
    return field(params.get(), "purpose", "humanCall");
}

// human_call_pending_deadline 只提取防重投所需原截止；完整邀请仍由通话状态机校验。
std::optional<std::uint64_t> human_call_pending_deadline(const Envelope& invite) {
    if (invite.method != "sessionInvite" || !invite.request || invite.code ||
        invite.token.empty() || invite.session.empty() || invite.event_sequence != 1)
        return {};
    const auto data = object_field(invite);
    const auto* deadline = cJSON_GetObjectItemCaseSensitive(data.get(), "expiresAt");
    if (!field(data.get(), "purpose", "humanCall") || !cJSON_IsNumber(deadline) ||
        !std::isfinite(deadline->valuedouble) || deadline->valuedouble <= 0 ||
        deadline->valuedouble > 9007199254740991.0 ||
        std::floor(deadline->valuedouble) != deadline->valuedouble)
        return {};
    return static_cast<std::uint64_t>(deadline->valuedouble);
}

// human_call_terminal_matches 保持人际腿结束事件的会话、方向与水位校验口径。
bool human_call_terminal_matches(const Envelope& message, std::string_view session,
                                 std::uint64_t previous_sequence) {
    return message.method == "sessionEnded" && !message.request && !message.code &&
           message.session == session && message.event_sequence &&
           *message.event_sequence > previous_sequence;
}

// 构造沿用设备身份／实例参数：identity确定Topic，instance仅标识本机连接能力快照。
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
HumanCallClient::HumanCallClient(MqttPort& mqtt, CallMediaPort& media, std::string identity,
                                 std::string instance, HumanCallConfig config)
    : mqtt_(mqtt),
      media_(media),
      requests_(mqtt),
      call_requests_(mqtt),
      config_(std::move(config)),
      up_("$thing/up/signaling/" + identity),
      down_("$thing/down/signaling/" + identity),
      message_up_("$thing/up/message/" + identity),
      message_down_("$thing/down/message/" + identity),
      instance_(std::move(instance)) {}
// NOLINTEND(bugprone-easily-swappable-parameters)
HumanCallClient::~HumanCallClient() { close(); }
// set_endpoint_source 仅启动前安装可信宿主，不从网页创建资源许可。
bool HumanCallClient::set_endpoint_source(EndpointSourceGate gate) {
    if (state_ != HumanCallState::Idle || !gate || endpoint_source_ ||
        config_.mode != HumanCallMode::Voice)
        return false;
    endpoint_source_gate_ = std::move(gate);
    endpoint_source_ =
        std::make_unique<EndpointHandoffClient>(mqtt_, up_, down_, instance_ + "-endpoint");
    return true;
}
// endpoint_source_available 核原连接/完整组及独立本人意图，不以Connected推导资源许可。
bool HumanCallClient::endpoint_source_available(std::uint64_t now, std::uint64_t wall) const {
    if (!endpoint_source_ || !endpoint_source_gate_ ||
        (!candidate_handoff_.empty() && !candidate_revision_inherited_) || state_ != HumanCallState::Connected ||
        session_.empty() || !media_.connected() || call_held_ || !input_wanted_ ||
        pending_call_media_ || !track_token_.empty() || transfer_busy() ||
        control_state_ == HumanCallControlState::Pending ||
        control_state_ == HumanCallControlState::Unknown || now < last_now_ || !wall ||
        wall < wall_highwater_ || now > 9007199254740991ULL || wall > 9007199254740991ULL)
        return false;
    return read_endpoint_source_gate(now, wall);
}
// read_endpoint_source_gate 不在可移植核心强制启用异常；可信宿主回调必须同步且不得重入。
bool HumanCallClient::read_endpoint_source_gate(std::uint64_t now, std::uint64_t wall,
                                                HumanEndpointSourceOperation operation) const {
    if (!endpoint_source_gate_) return false;
#if defined(__cpp_exceptions)
    try {
        return endpoint_source_gate_(session_, now, wall, operation);
    } catch (...) {
        return false;
    }
#else
    return endpoint_source_gate_(session_, now, wall, operation);
#endif
}
// prepare_endpoint_source 不驱动媒体，沿原会话固定token/CAS提交一次本人目标意图。
bool HumanCallClient::prepare_endpoint_source(std::string_view target, std::uint64_t now,
                                              std::uint64_t wall) {
    if (target.empty() || target.size() > 128 ||
        std::any_of(target.begin(), target.end(),
                    [](unsigned char value) { return value < 32 || value == 127; }))
        return false;  // 参数拒绝在bind前，不能推进原控制器时钟或清旧事实。
    if (!endpoint_source_available(now, wall) || !endpoint_source_->bind(session_, now) ||
        !endpoint_source_->prepare(target, now))
        return false;
    last_now_ = now;
    wall_highwater_ = wall;
    return true;
}
// commit_endpoint_source ready/ACK不代替原Focus释放；只发送原事务的明确提交。
bool HumanCallClient::commit_endpoint_source(std::uint64_t now, std::uint64_t wall) {
    if (!endpoint_source_available(now, wall) || endpoint_source_->receipt().session != session_ ||
        !endpoint_source_->commit(now))
        return false;
    last_now_ = now;
    wall_highwater_ = wall;
    return true;
}
// abort_endpoint_source 仅撤已知原事务；未知结果及用户已停输入不会被自动恢复。
bool HumanCallClient::abort_endpoint_source(std::uint64_t now, std::uint64_t wall) {
    if (!endpoint_source_abort_available(now, wall) || !endpoint_source_->abort(now)) return false;
    last_now_ = now;
    wall_highwater_ = wall;
    return true;
}
// endpoint_source_abort_available 撤候选不改变输入意图，仍严格核当前原scope/资源和请求期限。
bool HumanCallClient::endpoint_source_abort_available(std::uint64_t now, std::uint64_t wall) const {
    // 用户停输入后仍可撤已知候选，不要求重新开启输入或取得新的资源租约。
    if (!endpoint_source_ || state_ != HumanCallState::Connected || session_.empty() ||
        endpoint_source_->receipt().session != session_ || now < last_now_ || !wall ||
        wall < wall_highwater_ || now > 9007199254740991ULL || wall > 9007199254740991ULL ||
        !read_endpoint_source_gate(now, wall, HumanEndpointSourceOperation::Abort))
        return false;
    const auto& receipt = endpoint_source_->receipt();
    return !receipt.handoff.empty() && !receipt.pending && !receipt.unknown &&
           receipt.state != "committed" && receipt.state != "aborted" && receipt.state != "failed";
}
// endpoint_source_receipt 返回唯一有界原控制器的事实。
const EndpointHandoffReceipt* HumanCallClient::endpoint_source_receipt() const {
    return endpoint_source_ ? &endpoint_source_->receipt() : nullptr;
}
// endpoint_source_busy 阻止同腿并发修改；已终态保留账本不阻止后续显式意图。
bool HumanCallClient::endpoint_source_busy() const {
    if (!endpoint_source_) return false;
    const auto& receipt = endpoint_source_->receipt();
    return receipt.pending || receipt.unknown ||
           (!receipt.handoff.empty() && receipt.state != "committed" &&
            receipt.state != "aborted" && receipt.state != "failed");
}
// receive_endpoint_source 先双只读验证，再在同一事件线程提交，不借独立账本吞全腿跳号。
bool HumanCallClient::receive_endpoint_source(std::string_view topic, std::string_view body,
                                              std::uint64_t now, std::uint64_t wall) {
    if (!endpoint_source_ || endpoint_source_->receipt().session != session_ || now < last_now_ ||
        !wall || wall < wall_highwater_ || state_ == HumanCallState::Idle ||
        state_ == HumanCallState::Available || state_ == HumanCallState::Failed)
        return false;
    Envelope envelope;
    if (decode_envelope(body, envelope) != DecodeResult::Ok) return false;
    const bool event = envelope.method == "endpointState" && !envelope.code;
    if ((event && !can_observe_endpoint_event(topic, body, now, wall)) ||
        !endpoint_source_->can_receive(topic, body, now))
        return false;
    if (!endpoint_source_->receive(topic, body, now)) return false;
    if (event) return record_endpoint_event(topic, body, now, wall);
    last_now_ = now;
    wall_highwater_ = wall;
    return true;
}
bool HumanCallClient::prepare_media() {
    return config_.mode == HumanCallMode::Voice
               ? (media_.voice_supported() && media_.prepare_voice(CallVoiceProfile{}))
               : media_.prepare();
}
bool HumanCallClient::begin_media(std::string_view session) {
  if (session.empty() || !media_.bind_session_scope(session) ||
      !media_.configure_udp_av(selected_udp_av_))
    return false;
  return config_.mode == HumanCallMode::Voice
             ? (media_.voice_supported() &&
                media_.begin_voice_candidate(CallVoiceProfile{}))
             : (media_.configure_video_receive(selected_receive_video_) &&
                media_.begin());
}

std::string HumanCallClient::next_token() { return instance_ + "-" + std::to_string(++serial_); }
// remember_invite
// 不提前淘汰有效或当前身份；堆另含vector/allocator元数据，逻辑预算不是堆上限。
HumanCallClient::InviteDecision* HumanCallClient::remember_invite(const Envelope& message,
                                                                  std::uint64_t expires,
                                                                  std::string rejection) {
    std::size_t bytes = sizeof(InviteDecision) + message.token.size() + message.session.size() +
                        message.payload.size() + rejection.size();
    for (const auto& fact : invitations_)
        bytes += sizeof(InviteDecision) + fact.token.size() + fact.session.size() +
                 fact.payload.size() + fact.rejection.size();
    if (invitations_.size() >= 32 || bytes > 65536)
        return nullptr;
    const bool terminal = !rejection.empty();
    invitations_.push_back(
        {message.token, message.session, message.payload, std::move(rejection), expires, terminal});
    return &invitations_.back();
}
// retire_invite
// 终态在清空当前session前落原事实，同连接close或失败不得重新许可旧邀请。
void HumanCallClient::retire_invite() {
    for (auto& fact : invitations_)
        if (fact.session == session_ && fact.token == invite_token_)
            fact.terminal = true;
}
// clear_session_state
// 结束单轮业务暂存，保留原请求历史与邀请墓碑，迟到旧消息不能借下一轮重新授权。
// 此函数不触碰驱动：close命令不等于Stopped，下一轮必须仍受平台真实整组预约门控。
void HumanCallClient::clear_session_state() {
    cleanup_only_ = false; // 明确结束或新轮清原停止墓碑，不能跨腿复用旧scope。
    cleanup_released_ = false;
    if (endpoint_source_)
        endpoint_source_->suspend(last_now_);  // 关闭不能抹未知远端事务，保留只读原事实。
    input_wanted_ = false;  // 关闭不重开本人意图；只有后继明确拨号/接听才创建新腿输入意图。
    candidate_source_session_.clear();
    candidate_handoff_.clear();
    candidate_commit_token_.clear();
    candidate_revision_inherited_ = false;
    context_inbox_.clear();
    shared_focus_.clear();
    shared_focus_bytes_ = 0;
    shared_endpoint_.clear();
    shared_endpoint_bytes_ = 0;
    pending_call_media_.reset();
    call_controls_.clear();
    transfers_.clear();
    call_revision_ = 0;
    platform_revision_ = 0;
    platform_call_state_.clear();
    platform_control_token_.clear();
    platform_call_payload_.clear();
    call_held_ = false;
    control_state_ = HumanCallControlState::Idle;
    session_.clear();
    participant_roster_.clear();
    invite_token_.clear();
    invite_payload_.clear();
    invite_method_.clear();
    create_token_.clear();
    create_response_payload_.clear();
    offer_token_.clear();
    state_token_.clear();
    end_token_.clear();
    track_token_.clear();
    track_id_.clear();
    track_state_.clear();
    event_seq_ = expires_at_ = track_sent_at_ = 0;
    state_acked_ = established_ = false;
    selected_receive_video_ = false;
    selected_udp_av_ = false;
}
// update_participant
// 在单事件线程完整核验后提交；成员变化只投影名册，不操作驱动或MAC。
bool HumanCallClient::update_participant(const Envelope& message) {
    return participant_roster_.apply(message.payload);
}
// close
// 结束本腿与连接级暂存；借用模式保留宿主网络及原请求历史，不自行恢复失败入口。
void HumanCallClient::close() {
    if (endpoint_source_) endpoint_source_->suspend(last_now_);
    retire_invite();
    media_.close();
    call_requests_.cancel_session(session_, last_now_);
    requests_.cancel_session(session_, last_now_);
    if (!config_.borrowed_connection) {
        // 本client独占能力账本；取消空会话在途，下一start不能被旧tick复活。
        if (!session_.empty())
            requests_.cancel_session("", last_now_);
        mqtt_.disconnect();
    }
    clear_session_state();
    capability_token_.clear();
    capability_retry_at_.reset();
    capability_conflicts_ = 0;
    subscription_ = 0;
    state_ = HumanCallState::Idle;
}
void HumanCallClient::fail(HumanCallFailure reason) {
    if (failure_facts_.failure == HumanCallFailure::None)
        failure_facts_.failure = reason;
    // 阶段超时撤销业务并真实关媒体，但原已分配完整组仍须沿原水位接正式release。
    // 只保留有界原事实，不生成资源许可；Host/FocusEngine仍核原代次与实际双停止票据。
    const bool cleanup = reason == HumanCallFailure::PhaseDeadline &&
                         config_.borrowed_connection && !session_.empty() && !shared_focus_.empty();
    // 保留原session给close核销邀请和两份请求表，不让清理读取已移动的身份对象。
    auto original_session = cleanup ? session_ : std::string{};
    auto original_focus = cleanup ? std::move(shared_focus_) : std::vector<SharedFocusFact>{};
    const auto original_sequence = event_seq_;
    const auto original_bytes = shared_focus_bytes_;
    // 候选撤销还会先送aborted再送Ended；只保原邀请出处和有界被动账本，不能保留已继承源的业务资格。
    const bool candidate_cleanup = cleanup && !candidate_revision_inherited_ &&
                                   !candidate_source_session_.empty() &&
                                   !candidate_handoff_.empty();
    auto original_candidate_source = candidate_cleanup ? candidate_source_session_ : std::string{};
    auto original_candidate_handoff = candidate_cleanup ? candidate_handoff_ : std::string{};
    auto original_endpoint =
        candidate_cleanup ? std::move(shared_endpoint_) : std::vector<SharedFocusFact>{};
    const auto original_endpoint_bytes = candidate_cleanup ? shared_endpoint_bytes_ : 0;
    const bool original_released = cleanup_released_;
    close();
    if (cleanup) {
        session_ = std::move(original_session);
        shared_focus_ = std::move(original_focus);
        shared_focus_bytes_ = original_bytes;
        event_seq_ = original_sequence;
        cleanup_only_ = true;
        cleanup_released_ = original_released;
        candidate_source_session_ = std::move(original_candidate_source);
        candidate_handoff_ = std::move(original_candidate_handoff);
        shared_endpoint_ = std::move(original_endpoint);
        shared_endpoint_bytes_ = original_endpoint_bytes;
    }
    state_ = HumanCallState::Failed;
}
bool HumanCallClient::start(const MqttConfig& config, std::uint64_t now) {
    if (config_.borrowed_connection || !media_config_valid(config_, media_))
        return false;
    if (state_ != HumanCallState::Idle && state_ != HumanCallState::Failed)
        return false;
    const auto slash = up_.find('/', 20);
    if (instance_.empty() || instance_.size() > 64 || slash == std::string::npos ||
        up_.find_first_of("+#", 0) != std::string::npos)
        return false;
    close();
    failure_facts_ = {};  // 显式新连接才清原失败；普通close不能抹除收尾前事实。
    rejection_facts_.reset(); // 新连接不能继承旧拒帧。
    if (!mqtt_.connect(config)) {
        fail(HumanCallFailure::StartConnect);
        return false;
    }
    state_ = HumanCallState::Connecting;
    last_now_ = now;
    phase_at_ = now;
    return true;
}
// start_shared 只绑定宿主已证实的连接，不重复鉴权、订阅或能力报告。
bool HumanCallClient::start_shared(std::uint64_t now) {
    const auto slash = up_.find('/', 20);
    if (!config_.borrowed_connection || !media_config_valid(config_, media_) || instance_.empty() ||
        instance_.size() > 64 || slash == std::string::npos ||
        up_.find_first_of("+#", 0) != std::string::npos ||
        (state_ != HumanCallState::Idle && state_ != HumanCallState::Failed))
        return false;
    close();
    failure_facts_ = {};  // 新借用腿亦不继承旧腿故障。
    rejection_facts_.reset(); // 新借用腿独立取证。
    last_now_ = phase_at_ = now;
    state_ = HumanCallState::Available;
    return true;
}
// owns_shared_create_reply 为尚未获得会话ID的共享主叫提供精确分派，正文仍由receive_shared校验。
bool HumanCallClient::owns_shared_create_reply(const Envelope& message) const {
    return config_.borrowed_connection && state_ == HumanCallState::Inviting &&
           !create_token_.empty() && message.method == "sessionCreated" && !message.request &&
           message.token == create_token_ && message.code && *message.code >= 200 &&
           *message.code <= 599;
}
// retired_shared_create_rejection 不创建第二账本；只有原已确认失败的创建关联可供宿主隔离迟到重放。
bool HumanCallClient::retired_shared_create_rejection(const Envelope& message) const {
    return config_.borrowed_connection && message.method == "sessionCreated" &&
           !message.request && message.session.empty() && message.token != create_token_ &&
           message.code && *message.code >= 200 && *message.code <= 599 &&
           *message.code != 200 && !message.event_sequence && !message.message_sequence &&
           requests_.state({up_, "sessionCreate", message.token}) == RequestState::Failed;
}

// receive_shared 复用原方法状态机，非法或时钟回退只关闭本腿，不断宿主MQTT。
bool HumanCallClient::receive_shared(std::string_view topic, std::string_view body,
                                     std::uint64_t now, std::uint64_t wall_ms) {
    // 失败墓碑不接业务消息；实际完整释放后只接原候选aborted水位或原腿后继终态。
    if (cleanup_only_ && state_ == HumanCallState::Failed) {
        Envelope terminal;
        if (!cleanup_released_ || topic != down_ || now < last_now_ || wall_ms < wall_highwater_ ||
            decode_envelope(body, terminal) != DecodeResult::Ok || terminal.request ||
            terminal.code || terminal.session != session_ || !terminal.event_sequence)
            return false;
        if (terminal.method == "endpointState")
            return observe_shared_endpoint(topic, body, now, wall_ms);
        if (terminal.method != "sessionEnded" || *terminal.event_sequence != event_seq_ + 1)
            return false;
        last_now_ = now;
        wall_highwater_ = wall_ms;
        return receive(topic, body, now, wall_ms);
    }
    if (!config_.borrowed_connection || state_ == HumanCallState::Idle ||
        state_ == HumanCallState::Failed)
        return false;
    if (now < last_now_) {
        fail(HumanCallFailure::ClockRollback);
        return false;
    }
    last_now_ = now;
    expire_call_control(now);
    expire_transfer(now);
    if (topic == message_down_)
        return receive_context(body, wall_ms);
    Envelope routed;
    if (decode_envelope(body, routed) == DecodeResult::Ok &&
        (routed.method == "endpointState" || endpoint_handoff_ingress_ack(body)))
        return routed.method == "endpointState" && !routed.code
                   ? observe_shared_endpoint(topic, body, now, wall_ms)
                   : endpoint_source_ && !endpoint_source_->receipt().session.empty()
                   ? receive_endpoint_source(topic, body, now, wall_ms)
                   : observe_shared_endpoint(topic, body, now, wall_ms);
    const auto previous = state_; // 拒帧可能改变阶段，首因必须保存调用前事实。
    if (!receive(topic, body, now, wall_ms)) {
        record_receive_rejection(body, previous);
        fail(HumanCallFailure::ReceiveRejected);
        return false;
    }
    return true;
}

// observe_shared_endpoint 接续不另开信令水位；仅原顺序事实可推进，重复原字节不动驱动或阶段截止。
// 保留 topic/body 共享端腿观察接口；topic 核原下行范围，body 核原会话/端腿及信令水位，不生成新邀请。
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
bool HumanCallClient::can_observe_endpoint_event(std::string_view topic, std::string_view body,
                                              std::uint64_t now, std::uint64_t wall_ms) const {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    if ((!config_.borrowed_connection && !endpoint_source_) || topic != down_ || session_.empty() ||
        state_ == HumanCallState::Idle ||
        (state_ == HumanCallState::Failed && !(cleanup_only_ && cleanup_released_)) ||
        state_ == HumanCallState::Available || now < last_now_ || !wall_ms ||
        wall_ms < wall_highwater_)
        return false;
    EndpointState fact;
    if (decode_endpoint_state(WireRoute::Signaling, body, fact) != WireResult::Ok ||
        fact.envelope.session != session_ || !fact.envelope.event_sequence || fact.envelope.code)
        return false;
    if (cleanup_only_ && (fact.state != "aborted" || candidate_source_session_.empty() ||
                          candidate_handoff_.empty() || fact.handoff_id != candidate_handoff_))
        return false;
    const auto sequence = *fact.envelope.event_sequence;
    for (const auto& saved : shared_endpoint_)
        if (saved.token == fact.envelope.token || saved.sequence == sequence) {
            if (saved.body != body)
                return false;
            return true;
        }
    constexpr std::size_t maximum_bytes = 65536;  // 逻辑对象与有效字符串预算，不是堆使用精确承诺。
    const auto bytes = sizeof(SharedFocusFact) + body.size() + fact.envelope.token.size();
    if (state_ == HumanCallState::Ending || sequence != event_seq_ + 1 ||
        shared_endpoint_.size() >= 128 || bytes > maximum_bytes - shared_endpoint_bytes_)
        return false;
    return true;
}
// observe_shared_endpoint 只在原完整主动事件可接纳后提交水位，不改变媒体或阶段截止。
bool HumanCallClient::observe_shared_endpoint(std::string_view topic, std::string_view body,
                                              std::uint64_t now, std::uint64_t wall_ms) {
    if (!candidate_handoff_.empty()) {
        EndpointState fact;
        if (decode_endpoint_state(WireRoute::Signaling, body, fact) != WireResult::Ok)
            return false;
        if (fact.handoff_id == candidate_handoff_) {
            // aborted只关闭候选事务；必须已有同腿真实完整release，且不能用于已继承新源的候选。
            if (fact.state == "aborted") {
                if (!cleanup_released_ || candidate_revision_inherited_ ||
                    candidate_source_session_.empty() ||
                    !can_observe_endpoint_event(topic, body, now, wall_ms))
                    return false;
                for (const auto& saved : shared_endpoint_) {
                    EndpointState prior;
                    if (decode_endpoint_state(WireRoute::Signaling, saved.body, prior) ==
                            WireResult::Ok &&
                        prior.state == "aborted" && prior.handoff_id == candidate_handoff_)
                        return saved.body == body &&
                               record_endpoint_event(topic, body, now, wall_ms);
                }
                return record_endpoint_event(topic, body, now, wall_ms);
            }
            // 平台先给候选ready，再按同腿水位排release或提交；这里只记录原事实，不继承源CAS或打开媒体。
            if (fact.state == "ready") {
                if (!can_observe_endpoint_event(topic, body, now, wall_ms) ||
                    candidate_source_session_.empty() || fact.candidate_session_id != session_ ||
                    !fact.revision || invite_method_ != "sessionAccept" ||
                    (state_ != HumanCallState::WaitingEstablished &&
                     state_ != HumanCallState::Connected))
                    return false;
                for (const auto& saved : shared_endpoint_) {
                    EndpointState prior;
                    if (decode_endpoint_state(WireRoute::Signaling, saved.body, prior) ==
                            WireResult::Ok &&
                        prior.state == "ready" && prior.handoff_id == candidate_handoff_)
                        return saved.body == body &&
                               record_endpoint_event(topic, body, now, wall_ms);
                }
                return !candidate_revision_inherited_ &&
                       record_endpoint_event(topic, body, now, wall_ms);
            }
            // 邀请出处只能由原已接听腿的同一提交确认；旧来源重投不覆盖本端后继事务。
            if (!can_observe_endpoint_event(topic, body, now, wall_ms) ||
                fact.state != "committed" || fact.candidate_session_id != session_)
                return false;
            if (candidate_revision_inherited_)
                return fact.envelope.token == candidate_commit_token_ &&
                       record_endpoint_event(topic, body, now, wall_ms);
            if (!endpoint_source_ || !fact.revision || state_ != HumanCallState::Connected ||
                invite_method_ != "sessionAccept" || !state_acked_ || !established_ ||
                !media_.connected() || call_held_ || !input_wanted_ || pending_call_media_ ||
                !track_token_.empty() || transfer_busy() ||
                control_state_ == HumanCallControlState::Pending ||
                control_state_ == HumanCallControlState::Unknown ||
                !read_endpoint_source_gate(now, wall_ms))
                return false;
            // 两份原账本已完整预检，同一事件线程提交，不公开写revision或借用AI授权。
            if (!endpoint_source_->bind_committed_candidate(session_, fact.revision, now) ||
                !record_endpoint_event(topic, body, now, wall_ms))
                return false;
            candidate_revision_inherited_ = true;
            candidate_commit_token_ = fact.envelope.token;
            return true;
        }
        if (!candidate_revision_inherited_ || !endpoint_source_ ||
            endpoint_source_->receipt().session != session_ ||
            endpoint_source_->receipt().handoff.empty())
            return false; // 未由本人prepare创建的后继handoff不能作为被动事件补授或推进水位。
    }
    if (endpoint_source_ && endpoint_source_->receipt().session == session_ &&
        !endpoint_source_->receipt().handoff.empty())
        return receive_endpoint_source(topic, body, now, wall_ms);
    return record_endpoint_event(topic, body, now, wall_ms);
}
// record_endpoint_event 两份原账本在同线程预检后提交；被动观察不触发媒体动作。
bool HumanCallClient::record_endpoint_event(std::string_view topic, std::string_view body,
                                            std::uint64_t now, std::uint64_t wall_ms) {
    if (!can_observe_endpoint_event(topic, body, now, wall_ms)) return false;
    EndpointState fact;
    if (decode_endpoint_state(WireRoute::Signaling, body, fact) != WireResult::Ok) return false;
    const auto sequence = *fact.envelope.event_sequence;
    for (const auto& saved : shared_endpoint_)
        if (saved.token == fact.envelope.token || saved.sequence == sequence) {
            last_now_ = now;
            wall_highwater_ = wall_ms;
            return true;
        }
    const auto bytes = sizeof(SharedFocusFact) + body.size() + fact.envelope.token.size();
    shared_endpoint_.push_back({fact.envelope.token, std::string(body), sequence});
    shared_endpoint_bytes_ += bytes;
    event_seq_ = sequence;
    last_now_ = now;
    wall_highwater_ = wall_ms;
    return true;
}
// can_observe_shared_focus 在宿主触发停止前只读核原Topic、scope、完整组及共享水位。
// 冲突、跳号、晚到控制和时钟回退均无副作用；Ending只允许原完整release关联。
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
bool HumanCallClient::can_observe_shared_focus(std::string_view topic, std::string_view body,
                                               std::uint64_t now, std::uint64_t wall_ms) const {
    if (!config_.borrowed_connection || topic != down_ || session_.empty() ||
        state_ == HumanCallState::Idle || (state_ == HumanCallState::Failed && !cleanup_only_) ||
        state_ == HumanCallState::Available || now < last_now_ || !wall_ms ||
        wall_ms < wall_highwater_)
        return false;
    FocusCommand command;
    if (decode_focus_control(WireRoute::Signaling, body, command) != WireResult::Ok ||
        command.envelope.session != session_ || command.resources.size() != 2 ||
        std::find(command.resources.begin(), command.resources.end(), "microphone") ==
            command.resources.end() ||
        std::find(command.resources.begin(), command.resources.end(), "speaker") ==
            command.resources.end())
        return false;
    if (state_ == HumanCallState::Ending && command.operation != "release") return false;
    if (cleanup_only_) {
        if (command.operation != "release") return false;
        bool original_focus = false;
        for (const auto& fact : shared_focus_) {
            FocusCommand prior;
            if (decode_focus_control(WireRoute::Signaling, fact.body, prior) == WireResult::Ok &&
                prior.envelope.session == command.envelope.session &&
                prior.focus_id == command.focus_id &&
                prior.resources.size() == command.resources.size() &&
                std::is_permutation(prior.resources.begin(), prior.resources.end(),
                                    command.resources.begin()))
                original_focus = true;
        }
        if (!original_focus) return false;
    }
    const auto sequence = *command.envelope.event_sequence;
    for (const auto& fact : shared_focus_)
        if (fact.token == command.envelope.token || fact.sequence == sequence)
            return fact.body == body;
    constexpr std::size_t byte_limit = std::size_t{64} * 1024;
    const auto bytes = sizeof(SharedFocusFact) + body.size() + command.envelope.token.size() + 2;
    return sequence == event_seq_ + 1 && shared_focus_.size() < 32 &&
           bytes <= byte_limit - shared_focus_bytes_;
}
// observe_shared_focus
// 只关联原端点接纳及实际回执；Ending允许原释放在途，但永不把接纳当释放。
bool HumanCallClient::observe_shared_focus(std::string_view topic, std::string_view body,
                                           const FocusSubmission& submission, std::uint64_t now,
                                           std::uint64_t wall_ms) {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    if (!can_observe_shared_focus(topic, body, now, wall_ms)) return false;
    FocusCommand command;
    if (decode_focus_control(WireRoute::Signaling, body, command) != WireResult::Ok)
        return false;
    // Accepted/Pending只证明唯一原端点接纳；Replay必须携原完整操作的实际成功结果。
    // 不尝试由网络focusState推导该证明，调用者必须保留实际端点返回的原对象。
    if (submission.admission == FocusAdmission::Replay) {
        FocusState actual;
        if (decode_focus_state(WireRoute::Signaling, submission.reply, actual) != WireResult::Ok ||
            !actual.envelope.code || *actual.envelope.code != 200 ||
            !focus_reply_matches(command, actual))
            return false;
        if (command.operation == "release" && actual.state == "released")
            cleanup_released_ = true;
    } else if ((submission.admission != FocusAdmission::Accepted &&
                submission.admission != FocusAdmission::Pending) ||
               !submission.reply.empty()) {
        return false;
    }
    const auto sequence = *command.envelope.event_sequence;
    for (const auto& fact : shared_focus_)
        if (fact.token == command.envelope.token || fact.sequence == sequence) {
            if (fact.body != body)
                return false;
            // 原字节重投只观察两类时钟，不刷新期限、驱动或共享事件水位。
            last_now_ = now;
            wall_highwater_ = wall_ms;
            return true;
        }
    constexpr std::size_t byte_limit =
        std::size_t{64} * 1024;  // 包含对象及原字符串的有界本腿逻辑字节。
    const auto bytes = sizeof(SharedFocusFact) + body.size() + command.envelope.token.size() + 2;
    if (sequence != event_seq_ + 1 || shared_focus_.size() >= 32 ||
        bytes > byte_limit - shared_focus_bytes_)
        return false;
    shared_focus_.push_back({command.envelope.token, std::string(body), sequence});
    shared_focus_bytes_ += bytes;
    event_seq_ = sequence;
    last_now_ = now;
    wall_highwater_ = wall_ms;
    return true;
}
bool HumanCallClient::send(std::string method, std::string token, std::string payload,
                           bool session) {
    return send_at(std::move(method), std::move(token), std::move(payload), session, phase_at_);
}
bool HumanCallClient::send_at(std::string method, std::string token, std::string payload,
                              bool session, std::uint64_t now) {
    Envelope message;
    message.method = std::move(method);
    message.token = std::move(token);
    message.payload = std::move(payload);
    if (session)
        message.session = session_;
    std::string body;
    if (encode_envelope(message, body) != DecodeResult::Ok)
        return false;
    return requests_.submit({message.token, message.session, message.method, up_, body}, now) ==
           SubmitResult::Accepted;
}
void HumanCallClient::poll(std::uint64_t now, std::uint64_t wall_ms) {
    if (state_ == HumanCallState::Idle || state_ == HumanCallState::Failed)
        return;
    if (now < phase_at_ || now < last_now_) {
        fail(HumanCallFailure::ClockRollback);
        return;
    }
    if (!config_.borrowed_connection && !mqtt_.poll()) {
        fail(HumanCallFailure::NetworkPoll);
        return;
    }
    last_now_ = now;
    wall_highwater_ = std::max(wall_highwater_, wall_ms);
    context_inbox_.expire(wall_highwater_);
    expire_call_control(now);
    expire_transfer(now);
    poll_call_media(now);
    if (endpoint_source_) endpoint_source_->poll(now);
    // Reporting原阶段截止先于请求重传；恢复请求自身较晚起点不能越过原30秒再发布。
    if (state_ == HumanCallState::Reporting && now - phase_at_ >= 30000) {
        fail(HumanCallFailure::PhaseDeadline);
        return;
    }
    call_requests_.tick(now);
    requests_.tick(now);
    if (!track_token_.empty() && now - track_sent_at_ >= 10000) {
        fail(HumanCallFailure::TrackDeadline);
        return;
    }
    const bool timed_out =
        (state_ == HumanCallState::Connecting || state_ == HumanCallState::Subscribing ||
         state_ == HumanCallState::Reporting || state_ == HumanCallState::Inviting ||
         state_ == HumanCallState::Accepting || state_ == HumanCallState::Negotiating ||
         state_ == HumanCallState::ConnectingMedia ||
         state_ == HumanCallState::WaitingEstablished || state_ == HumanCallState::Ending);
    if (timed_out && now - phase_at_ >= 30000) {
        fail(HumanCallFailure::PhaseDeadline);
        return;
    }
    if (state_ == HumanCallState::Ringing && wall_highwater_ >= expires_at_) {
        // 无人接听是正常单轮终态，不是设备网络故障；尚未取得媒体，不关闭共享驱动或MQTT。
        // 退休原邀请并取消本腿在途，迟到原邀请/结束不能复活，下一次独立来电仍可受理。
        retire_invite();
        call_requests_.cancel_session(session_, now);
        requests_.cancel_session(session_, now);
        clear_session_state();
        state_ = HumanCallState::Available;
        phase_at_ = now;
        return;
    }
    if (config_.borrowed_connection)
        return;  // 唯一宿主负责消费网络，借用模式只推进原腿期限和重试。
    // 原绝对30秒检查已完成；新token以实际now提交，不刷新phase_at_或改能力内容。
    if (state_ == HumanCallState::Reporting && capability_retry_at_ &&
        now >= *capability_retry_at_ && now - *capability_retry_at_ >= 500) {
        capability_retry_at_.reset();
        capability_token_ = next_token();
        if (!send_at("capabilityReport", capability_token_, capability_payload(config_), false,
                     now)) {
            fail(HumanCallFailure::CapabilitySubmit);
            return;
        }
    }
    for (unsigned n = 0; n < 32; ++n) {
        auto event = mqtt_.take_event();
        if (!event)
            break;
        switch (event->kind) {
            case MqttEvent::Kind::Connected:
                if (state_ != HumanCallState::Connecting || event->code != 0) {
                    fail(HumanCallFailure::ConnectEvent);
                    return;
                }
                subscription_ = mqtt_.subscribe(down_);
                if (subscription_ <= 0) {
                    fail(HumanCallFailure::SubscriptionStart);
                    return;
                }
                state_ = HumanCallState::Subscribing;
                phase_at_ = now;
                break;
            case MqttEvent::Kind::Subscribed:
                if (state_ != HumanCallState::Subscribing || event->operation != subscription_ ||
                    event->code != 0) {
                    fail(HumanCallFailure::SubscriptionEvent);
                    return;
                }
                capability_token_ = next_token();
                phase_at_ = now;
                if (!send("capabilityReport", capability_token_, capability_payload(config_),
                          false)) {
                    // 这里只证明原send未受理，不能据此声称Broker发布或网络交付失败。
                    fail(HumanCallFailure::CapabilitySubmit);
                    return;
                }
                state_ = HumanCallState::Reporting;
                break;
            case MqttEvent::Kind::Published:
                break;
            case MqttEvent::Kind::Message:
                if (endpoint_source_ && !endpoint_source_->receipt().session.empty()) {
                    Envelope routed;
                    if (decode_envelope(event->body, routed) == DecodeResult::Ok &&
                        (routed.method == "endpointState" ||
                         endpoint_handoff_ingress_ack(event->body))) {
                        (void)receive_endpoint_source(event->topic, event->body, now, wall_ms);
                        break;  // 接续scope/水位拒绝不能关闭不受影响的原通话。
                    }
                }
                {
                    const auto previous = state_; // 独立MQTT入口沿同一原拒绝边界取证。
                    if (!receive(event->topic, event->body, now, wall_ms)) {
                        record_receive_rejection(event->body, previous);
                        fail(HumanCallFailure::ReceiveRejected);
                        return;
                    }
                }
                break;
            default:
                fail(HumanCallFailure::NetworkEvent);
                return;
        }
    }
}
// receive_context 接收失败不影响实际主通话；发布返回值只是本地网络受理，原留存不回滚。
bool HumanCallClient::receive_context(std::string_view body, std::uint64_t wall_ms) {
    Envelope ack;
    const bool allowed = active() && media_.connected() && !transfer_busy() &&
                         control_state_ != HumanCallControlState::Pending &&
                         control_state_ != HumanCallControlState::Unknown;
    if (!context_inbox_.receive(body, session_, allowed, call_held_, wall_ms, ack))
        return false;
    std::string wire;
    if (encode_envelope(ack, wire) != DecodeResult::Ok)
        return false;
    const bool accepted = mqtt_.publish(message_up_, wire);  // 原本地网络受理，未推断Broker交付。
    if (context_ack_observer_)
        context_ack_observer_(accepted);
    return accepted;
}
// receive
// 保留网络端口公共签名；Topic先匹配，正文独立解码，now／wall_ms分别限定本地与平台期限。
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
bool HumanCallClient::receive(std::string_view topic, std::string_view body, std::uint64_t now,
                              std::uint64_t wall_ms) {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    if (topic != down_)
        return false;
    Envelope message;
    if (endpoint_source_ && !endpoint_source_->receipt().session.empty() &&
        decode_envelope(body, message) == DecodeResult::Ok &&
        (message.method == "endpointState" || endpoint_handoff_ingress_ack(body)))
        return receive_endpoint_source(topic, body, now, wall_ms);
    if (decode_envelope(body, message) != DecodeResult::Ok ||
        (message.request && message.method != "sessionInvite"))
        return false;
    // 只处理本独占client的原能力身份；失败账本已消费的旧ACK幂等丢弃，不触发新请求。
    if (!config_.borrowed_connection && message.method == "requestAck" && !message.request &&
        message.session.empty() && message.code) {
        auto data = object_field(message);
        const auto key = RequestKey{up_, "capabilityReport", message.token};
        if (requests_.state(key) == RequestState::Failed)
            return (*message.code == 409 && capability_conflict(data.get())) ||
                   (*message.code == 200 && capability_accepted(data.get()));
        if (message.token == capability_token_ && *message.code == 200 &&
            !capability_accepted(data.get()))
            return false;
        if (state_ == HumanCallState::Reporting && message.token == capability_token_) {
            if (*message.code == 409 && capability_conflict(data.get())) {
                // 先消费原终态，再有限退避；重复同token不会刷新500ms或30秒期限。
                if (!requests_.acknowledge({message.token, "", "capabilityReport", 409, up_}, now))
                    return false;
                if (capability_conflicts_ < 2) {
                    ++capability_conflicts_;
                    capability_retry_at_ = now;
                    return true;
                }
            }
        }
    }
    // 仅在最终Reporting拒绝边界记录首业务失败；已恢复的历史409不污染后续失败事实。
    if (state_ == HumanCallState::Reporting && message.method == "requestAck" && !message.request &&
        message.token == capability_token_ && message.session.empty() && message.code &&
        *message.code >= 200 && *message.code <= 599 && *message.code != 200) {
        auto data = object_field(message);
        if (field(data.get(), "requestMethod", "capabilityReport")) {
            failure_facts_.capability_ack_code = *message.code;
            failure_facts_.capability_reason = HumanCapabilityRejection::Unknown;
            if (*message.code == 409 && field(data.get(), "reason", "capabilityNotAccepted"))
                failure_facts_.capability_reason = HumanCapabilityRejection::CapabilityNotAccepted;
            else if (*message.code == 403 && field(data.get(), "reason", "focusDeviceUnavailable"))
                failure_facts_.capability_reason = HumanCapabilityRejection::FocusDeviceUnavailable;
            else if (*message.code == 403 && field(data.get(), "reason", "requestDenied"))
                failure_facts_.capability_reason = HumanCapabilityRejection::RequestDenied;
        }
    }
    wall_highwater_ = std::max(wall_highwater_, wall_ms);
    if (message.method == "sessionCreated") {
        const auto previous = requests_.state({up_, "sessionCreate", message.token});
        if (state_ != HumanCallState::Inviting)
            return !message.request && message.code &&
                   ((previous == RequestState::Succeeded && *message.code == 200 &&
                     ((message.session == session_ &&
                       message.payload == create_response_payload_) ||
                      (state_ == HumanCallState::Available && session_.empty()))) ||
                    (previous == RequestState::Failed && *message.code != 200 &&
                     state_ == HumanCallState::Available && session_.empty()));
        if (message.request || message.token != create_token_ || !message.code ||
            !requests_.acknowledge({message.token, "", "sessionCreate", *message.code, up_}, now))
            return false;
        if (*message.code != 200) {
            create_token_.clear();
            media_.close();  // 拒绝创建撤销此前音视频预约，不影响原共享连接。
            state_ = HumanCallState::Available;
            phase_at_ = now;
            return true;
        }
        if (message.session.empty() || message.event_sequence || !created_payload(message, config_))
            return false;
        selected_receive_video_ = receive_video_selected(message);
        selected_udp_av_ = config_.udp_av;  // 原创建请求与选择已严格一致，HAL必须在begin前切到同一格式。
        if (!begin_media(message.session))
            return false;
        session_ = message.session;
        create_response_payload_ = message.payload;
        event_seq_ = 0;
        state_ = HumanCallState::Negotiating;
        phase_at_ = now;
        return true;
    }
    // 已结束会话的迟到确认不得让新来电失败，更不能恢复旧媒体。
    if (message.method != "sessionInvite" && !message.session.empty() &&
        message.session != session_)
        return true;
    if (message.method == "callState")
        return receive_call_state(message, now);
    if (message.method == "participantChanged") {
        if (!config_.participant_roster || message.request || message.code ||
            message.message_sequence || message.session.empty() || message.session != session_ ||
            !message.event_sequence)
            return false;
        if (state_ == HumanCallState::Ending)
            return true;  // 本地已停流，迟到名册不能复活或阻断原挂断ACK。
        if (state_ != HumanCallState::Connected)
            return false;
        if (*message.event_sequence <= event_seq_)
            return true;
        // eventSeq是原端腿共享水位，不以名册事件连续性推断其他方法的消息丢失。
        if (!update_participant(message))
            return false;
        event_seq_ = *message.event_sequence;
        return true;
    }
    if (message.method == "trackState") {
        if (track_token_.empty() &&
            requests_.state({up_, "trackControl", message.token}) == RequestState::Succeeded)
            return true;
        if (track_token_.empty() || message.token != track_token_ || message.session != session_ ||
            !message.code || *message.code != 200)
            return false;
        auto data = object_field(message);
        const auto* revision = cJSON_GetObjectItemCaseSensitive(data.get(), "mediaRevision");
        if (!exact_keys(data.get(), {"trackId", "state", "mediaRevision"}) ||
            !field(data.get(), "trackId", track_id_) || !field(data.get(), "state", track_state_) ||
            !cJSON_IsNumber(revision) || revision->valuedouble != 1)
            return false;
        if (!requests_.acknowledge({message.token, message.session, "trackControl", 200, up_}, now))
            return false;
        track_token_.clear();
        track_id_.clear();
        track_state_.clear();
        return true;
    }
    if (message.method == "requestAck" || message.method == "mediaAnswer" ||
        (message.method == "sessionEnded" && message.code)) {
        std::string expected, method;
        if (message.method == "mediaAnswer")
            expected = offer_token_, method = "mediaOffer";
        else if (message.method == "sessionEnded")
            expected = end_token_, method = "sessionEnd";
        else if (!message.session.empty() && message.token == invite_token_) {
            expected = invite_token_;
            method = invite_method_;
        } else if (message.token == state_token_)
            expected = state_token_, method = "mediaState";
        else if (message.token == capability_token_)
            expected = capability_token_, method = "capabilityReport";
        if (expected.empty() || method.empty() || message.token != expected ||
            message.session != (method == "capabilityReport" ? "" : session_) || !message.code ||
            *message.code != 200)
            return false;
        const auto key = RequestKey{up_, method, message.token};
        if (requests_.state(key) == RequestState::Succeeded)
            return true;
        if (!requests_.acknowledge({message.token, message.session, method, *message.code, up_},
                                   now))
            return false;
        if (method == "capabilityReport") {
            if (state_ != HumanCallState::Reporting || message.method != "requestAck" ||
                !ack_method(message, expected) || expected != method)
                return false;
            capability_retry_at_.reset();
            state_ = HumanCallState::Available;
            phase_at_ = now;
        } else if (method == "sessionAccept") {
            if (state_ != HumanCallState::Accepting || message.method != "requestAck" ||
                !ack_method(message, expected) || expected != method || !begin_media(session_))
                return false;
            state_ = HumanCallState::Negotiating;
            phase_at_ = now;
        } else if (method == "sessionReject") {
            if (message.method != "requestAck")
                return false;
            media_.close();
            state_ = HumanCallState::Ending;
            phase_at_ = now;
        } else if (method == "mediaOffer") {
            std::string answer;
            if (state_ != HumanCallState::Negotiating || message.method != "mediaAnswer" ||
                !(selected_udp_av_ ? udp_av_answer(message,media_) : (sdp_payload(message, answer) && media_.apply_answer(answer))))
                return false;
            state_ = HumanCallState::ConnectingMedia;
            phase_at_ = now;
        } else if (method == "mediaState") {
            if (message.method != "requestAck" || !ack_method(message, expected) ||
                expected != method)
                return false;
            state_acked_ = true;
            if (established_) {
                state_ = HumanCallState::Connected;
                phase_at_ = now;
            }
        } else if (method == "sessionEnd") {
            if (state_ != HumanCallState::Ending || message.method != "sessionEnded")
                return false;
            retire_invite();
            call_requests_.cancel_session(session_, now);
            requests_.cancel_session(session_, now);
            clear_session_state();
            state_ = HumanCallState::Available;
            phase_at_ = now;
        }
        return true;
    }
    if (message.method == "sessionInvite") {
        // 完整原身份先比较；已拒绝或结束的精确重投隔离/重播原Busy，不随资源变化重新决策。
        for (const auto& fact : invitations_) {
            if (fact.token != message.token && fact.session != message.session)
                continue;
            if (!message.request || message.token != fact.token ||
                message.session != fact.session || message.payload != fact.payload ||
                message.event_sequence != 1)
                return false;
            if (fact.expires <= wall_highwater_)
                return true;
            if (fact.terminal)
                return fact.rejection.empty() || mqtt_.publish(up_, fact.rejection);
            return message.session == session_ && message.token == invite_token_;
        }
        // 已过原截止的终态可有界回收，当前腿不能因邀请截止已过而丢失原决策保护。
        invitations_.erase(std::remove_if(invitations_.begin(), invitations_.end(),
                                          [&](const InviteDecision& fact) {
                                              return fact.terminal && fact.session != session_ &&
                                                     fact.expires <= wall_highwater_;
                                          }),
                           invitations_.end());
        HumanCallConfig invite_config =
            config_; // 只在宿主已装配的两种类型内选择，不从不可信轨扩大能力。
        if (config_.voice_and_video) {
          const bool voice =
              field(object_field(message).get(), "callType", "voice");
          invite_config.mode =
              voice ? HumanCallMode::Voice : HumanCallMode::Video;
          if (voice)
            invite_config.receive_video = false;
        }
        std::uint64_t deadline = 0;
        std::string source_session, handoff_id;
        if (message.request && message.session == session_ && message.token == invite_token_)
            return message.payload == invite_payload_;  // 同token不能改写轨或授权。
        if (!message.request || message.session.empty() ||
            !message.event_sequence || *message.event_sequence != 1 ||
            !invite_payload(message, wall_highwater_, deadline, invite_config,
                            endpoint_source_ != nullptr, source_session,
                            handoff_id))
          return false;
        if (state_ != HumanCallState::Available || !media_.available()) {
            Envelope busy;
            busy.token = message.token;
            busy.method = "sessionReject";
            busy.session = message.session;
            busy.payload = R"({"reason":"busy"})";
            std::string encoded;
            return encode_envelope(busy, encoded) == DecodeResult::Ok &&
                   remember_invite(message, deadline, encoded) && mqtt_.publish(up_, encoded);
        }
        if (!remember_invite(message, deadline))
            return false;
        config_.mode = invite_config.mode;
        session_ = message.session;
        invite_token_ = message.token;
        invite_payload_ = message.payload;
        candidate_source_session_ = std::move(source_session);
        candidate_handoff_ = std::move(handoff_id);
        candidate_commit_token_.clear();
        candidate_revision_inherited_ = false;
        invite_method_.clear();
        expires_at_ = deadline;
        event_seq_ = 1;
        selected_receive_video_ = receive_video_selected(message);
        selected_udp_av_ = field(object_field(message).get(), "transport", "udpAv");
        phase_at_ = now;
        state_ = HumanCallState::Ringing;
        return true;
    }
    if (message.method == "sessionEstablished") {
        if (message.session == session_ && message.event_sequence &&
            *message.event_sequence <= event_seq_)
            return true;
        auto data = object_field(message);
        if (message.request || message.code || message.session != session_ ||
            !message.event_sequence || *message.event_sequence != event_seq_ + 1 ||
            !exact_keys(data.get(), {"state", "mediaRevision"}) ||
            !field(data.get(), "state", "established") ||
            !cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(data.get(), "mediaRevision")) ||
            cJSON_GetObjectItemCaseSensitive(data.get(), "mediaRevision")->valuedouble != 1 ||
            (state_ != HumanCallState::WaitingEstablished && state_ != HumanCallState::Connected))
            return false;
        event_seq_ = *message.event_sequence;
        established_ = true;
        if (state_acked_) {
            state_ = HumanCallState::Connected;
            phase_at_ = now;
        }
        return true;
    }
    if (message.method == "sessionEnded") {
        if (message.session != session_ && state_ == HumanCallState::Available)
            return true;
        if (!human_call_terminal_matches(message, session_, event_seq_))
            return false;
        media_.close();
        call_requests_.cancel_session(session_, now);
        requests_.cancel_session(session_, now);
        retire_invite();
        clear_session_state();
        state_ = HumanCallState::Available;
        phase_at_ = now;
        return true;
    }
    return false;
}
// expire_call_control
// 只取消独立控制账本；原会话/名册和媒体握手不因控制未知消失。
void HumanCallClient::expire_call_control(std::uint64_t now) {
    if (control_state_ != HumanCallControlState::Pending || call_controls_.empty())
        return;
    auto& fact = call_controls_.back();
    if (now >= fact.started && now - fact.started >= 10000) {
        fact.unknown = true;
        pending_call_media_.reset();
        control_state_ = HumanCallControlState::Unknown;
        call_requests_.cancel_session(session_, now);
        report_control(fact);
    }
}
// report_control 当前原事实只读投影，ACK accepted与实际门控结果保持独立。
void HumanCallClient::report_control(const CallControlFact& fact) const {
    if (control_observer_)
        control_observer_({session_, fact.token, fact.operation, fact.reason, fact.ack_revision,
                           fact.ack_code, control_state_, call_held_, fact.media_applied});
}
// poll_call_media 实际驱动确认与平台事件分开，未知不延长原预算或清空原名册。
void HumanCallClient::poll_call_media(std::uint64_t now) {
    if (!pending_call_media_ || state_ != HumanCallState::Connected)
        return;
    const auto pending = *pending_call_media_;
    auto found =
        std::find_if(call_controls_.begin(), call_controls_.end(),
                     [&](const CallControlFact& fact) { return fact.token == pending.token; });
    const bool expired = now < pending.started || now - pending.started >= 2000;
    const auto result =
        expired ? std::optional<bool>{false} : media_.hold_result(pending.ticket, pending.held);
    if (!result)
        return;
    pending_call_media_.reset();
    if (!*result) {
        control_state_ = HumanCallControlState::Unknown;
        if (found != call_controls_.end())
            found->unknown = true;
        call_requests_.cancel_session(session_, now);
        if (found != call_controls_.end())
            report_control(*found);
        return;  // 未知不冒实际停声或恢复；驱动仍必须保持失败安全门控。
    }
    call_held_ = pending.held;
    if (found != call_controls_.end() && !found->unknown) {
        found->media_applied = true;
        if (found->event_seen && found->ack_code == 200)
            control_state_ = HumanCallControlState::Completed;
        report_control(*found);
    }
}
// receive_call_state
// 将ACK、独立完成事件和实际媒体结果分开；QoS重投只消费原事实。
bool HumanCallClient::receive_call_state(const Envelope& message, std::uint64_t now) {
    if (message.request || message.session.empty() || message.session != session_ ||
        message.message_sequence ||
        (message.code.has_value() == message.event_sequence.has_value()))
        return false;
    bool transfer_handled = false;
    const auto transfer_valid = receive_transfer_state(message, now, transfer_handled);
    if (transfer_handled)
        return transfer_valid;
    auto data = object_field(message);
    if (message.code) {
        auto found =
            std::find_if(call_controls_.begin(), call_controls_.end(),
                         [&](const CallControlFact& fact) { return fact.token == message.token; });
        if (found == call_controls_.end())
            return false;
        auto& fact = *found;
        std::uint64_t revision = 0;
        if (*message.code == 200) {
            const auto expected = fact.operation == "hold"     ? "held"
                                  : fact.operation == "resume" ? "active"
                                                               : "cancelled";
            if (!(fact.operation == "cancel"
                      ? exact_keys(data.get(), {"state", "revision", "reason"}) &&
                            field(data.get(), "reason", "callerCancelled")
                      : exact_keys(data.get(), {"state", "revision"})) ||
                !field(data.get(), "state", expected) ||
                !call_revision_field(data.get(), "revision", revision, false) ||
                revision != fact.revision + 1)
                return false;
        } else {
            std::string reason;
            if (*message.code < 400 || *message.code >= 600 ||
                !string_field(data.get(), "reason", reason, 128))
                return false;
            if (*message.code == 409 && exact_keys(data.get(), {"reason", "currentRevision"})) {
                if ((reason != "staleRevision" && reason != "invalidState") ||
                    !call_revision_field(data.get(), "currentRevision", revision, true))
                    return false;
            } else if (!exact_keys(data.get(), {"reason"}))
                return false;
            if (fact.event_seen)
                return false;  // 原成功完成不能被同身份拒绝改写。
        }
        if (fact.ack_code != 0)
            return fact.ack_code == *message.code && fact.ack_payload == message.payload;
        if (fact.unknown)
            return true;  // 晚到应答不把原Unknown改写成成功，也不补投原操作。
        if (!call_requests_.acknowledge(
                {message.token, session_, "callControl", *message.code, up_}, now))
            return false;
        fact.ack_code = *message.code;
        fact.ack_payload = message.payload;
        fact.ack_revision = revision;
        if (*message.code != 200) {
            std::string reason;
            string_field(data.get(), "reason", reason, 128);
            fact.reason =
                reason == "staleRevision" || reason == "invalidState" ? reason : "rejected";
            if (cJSON_GetObjectItemCaseSensitive(data.get(), "currentRevision"))
                call_revision_ = std::max(call_revision_, revision);
            control_state_ = HumanCallControlState::Rejected;
        } else if (fact.operation == "cancel") {
            state_ = HumanCallState::Ending;
            phase_at_ = now;  // 不释放本地媒体；独立sessionEnded才结束原腿。
        } else if (fact.event_seen && fact.media_applied)
            control_state_ = HumanCallControlState::Completed;
        report_control(fact);
        return true;
    }
    std::string state, token;
    std::uint64_t revision = 0;
    if (!exact_keys(data.get(), {"state", "revision", "requestToken"}) ||
        !string_field(data.get(), "state", state, 16) || (state != "held" && state != "active") ||
        !string_field(data.get(), "requestToken", token, 128) ||
        !call_revision_field(data.get(), "revision", revision, false))
        return false;
    if (*message.event_sequence <= event_seq_)
        return true;
    if (state_ == HumanCallState::Ending)
        return true;  // 本地已结束意图不能被旧完成事件恢复。
    if (state_ != HumanCallState::Connected)
        return false;
    auto found = std::find_if(call_controls_.begin(), call_controls_.end(),
                              [&](const CallControlFact& fact) { return fact.token == token; });
    if (found != call_controls_.end()) {
        if (found->operation == "cancel" || revision != found->revision + 1 ||
            state != (found->operation == "hold" ? "held" : "active") ||
            (found->ack_code != 0 && found->ack_code != 200))
            return false;
        if (found->unknown) {
            event_seq_ = *message.event_sequence;
            return true;  // 已过原期限的同控制完成不重新开启媒体或冒称成功。
        }
    }
    if (revision <= platform_revision_) {
        if (revision == platform_revision_ &&
            (state != platform_call_state_ || token != platform_control_token_))
            return false;
        event_seq_ = *message.event_sequence;
        return true;
    }
    const bool held = state == "held";
    if (pending_call_media_)
        return false;  // 不用新平台版本替换尚未完成的原实际门控票据。
    const bool asynchronous = held != call_held_ && media_.hold_async_supported();
    const auto ticket = asynchronous ? media_.request_hold(held, token, revision, now)
                                     : std::optional<std::uint64_t>{};
    const bool applied =
        held == call_held_ || (!asynchronous && media_.hold_supported() && media_.set_held(held));
    if (asynchronous && ticket)
        pending_call_media_ = PendingCallMedia{token, *ticket, now, held};
    if (!applied && held && !(asynchronous && ticket))
        return false;  // 无法确认本端停声时安全关闭，不假称保持已完成。
    call_revision_ = std::max(call_revision_, revision);
    platform_revision_ = revision;
    platform_call_state_ = state;
    platform_control_token_ = token;
    platform_call_payload_ = message.payload;
    event_seq_ = *message.event_sequence;
    if (asynchronous && ticket) {
        if (found != call_controls_.end())
            found->event_seen = true;
        return true;  // 请求已排队但原实际回执未到，不能关闭通话或造Completed。
    }
    if (!applied) {
        control_state_ = HumanCallControlState::Unknown;
        if (found != call_controls_.end())
            found->unknown = true;
        call_requests_.cancel_session(session_, now);
        if (found != call_controls_.end())
            report_control(*found);
        return true;  // 焦点未重获时保留原保持与连接，不隐式恢复扬声器或麦克风。
    }
    call_held_ = held;
    if (found != call_controls_.end()) {
        found->event_seen = true;
        found->media_applied = true;
        if (found->ack_code == 200)
            control_state_ = HumanCallControlState::Completed;
        report_control(*found);
    }
    return true;
}
// transfer_projection
// 保留受理码与独立完成版本，不能以transferred替代原腿实际退出。
HumanCallTransferReceipt HumanCallClient::transfer_projection(const TransferFact& fact) const {
    return {session_,           fact.token,        fact.target,
            fact.candidate,     fact.reason,       fact.cancel_token,
            fact.cancel_reason, fact.ack_revision, fact.result_revision,
            fact.ack_code,      fact.cancel_code,  fact.state,
            fact.cancel_state};
}
// transfer_receipt 无当前转接时返回Idle，不从旧腿请求历史重建事实。
HumanCallTransferReceipt HumanCallClient::transfer_receipt() const {
    return transfers_.empty() ? HumanCallTransferReceipt{} : transfer_projection(transfers_.back());
}
// report_transfer 仅调用显式宿主观察者，不触发网络或本地媒体。
void HumanCallClient::report_transfer(const TransferFact& fact) const {
    if (transfer_observer_)
        transfer_observer_(transfer_projection(fact));
}
// transfer_busy
// 只允许一个未决转接；撤候选另核原ACK，失败确认后才可显式发起下一次。
bool HumanCallClient::transfer_busy() const {
    return std::any_of(transfers_.begin(), transfers_.end(), [](const TransferFact& fact) {
        return fact.state == HumanCallTransferState::Pending ||
               fact.state == HumanCallTransferState::Transferring ||
               fact.state == HumanCallTransferState::Transferred ||
               fact.state == HumanCallTransferState::Unknown ||
               fact.cancel_state == HumanCallControlState::Pending ||
               (fact.ack_code == 0 && !fact.ack_expired);
    });
}
// expire_transfer
// 受理缺失只标未知；终结事件已有时仍保留原事实，但绝不补造迟到ACK。
void HumanCallClient::expire_transfer(std::uint64_t now) {
    bool cancel_requests = false;
    for (auto& fact : transfers_) {
        const auto elapsed = now >= fact.started ? now - fact.started : 45000;
        bool changed = false;
        if (!fact.ack_code && !fact.ack_expired && elapsed >= 10000) {
            fact.ack_expired = true;
            if (fact.state == HumanCallTransferState::Pending)
                fact.state = HumanCallTransferState::Unknown;
            cancel_requests = changed = true;
        }
        if ((fact.state == HumanCallTransferState::Pending ||
             fact.state == HumanCallTransferState::Transferring) &&
            elapsed >= 45000) {
            fact.state = HumanCallTransferState::Unknown;
            cancel_requests = changed = true;
        }
        if (fact.cancel_state == HumanCallControlState::Pending &&
            (elapsed >= 45000 || now < fact.cancel_started || now - fact.cancel_started >= 10000)) {
            fact.cancel_state = HumanCallControlState::Unknown;
            fact.cancel_expired = true;
            cancel_requests = changed = true;
        }
        if (changed)
            report_transfer(fact);
    }
    if (cancel_requests)
        call_requests_.cancel_session(session_,
                                      now);  // 同期不许普通控制，原媒体/轨请求账本不受影响。
}
// receive_transfer_state
// 转接受理、完成与撤候选分开校验；留存端合法失败只更新版本，不操纵媒体。
bool HumanCallClient::receive_transfer_state(const Envelope& message, std::uint64_t now,
                                             bool& handled) {
    handled = false;
    auto data = object_field(message);
    auto found = std::find_if(transfers_.begin(), transfers_.end(), [&](const TransferFact& fact) {
        return message.code ? fact.token == message.token || fact.cancel_token == message.token
                            : field(data.get(), "requestToken", fact.token) ||
                                  (!fact.cancel_token.empty() &&
                                   field(data.get(), "requestToken", fact.cancel_token));
    });
    if (message.code) {
        if (found == transfers_.end())
            return true;
        handled = true;
        auto& fact = *found;
        const bool cancel = message.token == fact.cancel_token;
        std::uint64_t revision = 0;
        std::string candidate, reason;
        if (*message.code == 200) {
            if (!(cancel ? exact_keys(data.get(),
                                      {"state", "revision", "candidateSessionId", "reason"}) &&
                               field(data.get(), "state", "active") &&
                               field(data.get(), "reason", "candidateCancelled")
                         : exact_keys(data.get(), {"state", "revision", "candidateSessionId"}) &&
                               field(data.get(), "state", "transferring")) ||
                !call_revision_field(data.get(), "revision", revision, false) ||
                revision != (cancel ? fact.cancel_revision : fact.revision) + 1 ||
                !string_field(data.get(), "candidateSessionId", candidate, 128) ||
                candidate == session_ || (!fact.candidate.empty() && candidate != fact.candidate))
                return false;
        } else {
            if (*message.code < 400 || *message.code >= 600 ||
                !string_field(data.get(), "reason", reason, 128))
                return false;
            if (*message.code == 409 && exact_keys(data.get(), {"reason", "currentRevision"})) {
                if ((reason != "staleRevision" && reason != "invalidState") ||
                    !call_revision_field(data.get(), "currentRevision", revision, true))
                    return false;
            } else if (!exact_keys(data.get(), {"reason"}))
                return false;
            if (cancel ? !fact.cancel_result_payload.empty() : !fact.result_payload.empty())
                return false;
        }
        const int previous = cancel ? fact.cancel_code : fact.ack_code;
        if (previous)
            return previous == *message.code &&
                   (cancel ? fact.cancel_ack_payload : fact.ack_payload) == message.payload;
        if (cancel ? fact.cancel_expired : fact.ack_expired)
            return true;
        if (!call_requests_.acknowledge(
                {message.token, session_, "callControl", *message.code, up_}, now))
            return false;
        if (cancel) {
            fact.cancel_code = *message.code;
            fact.cancel_ack_payload = message.payload;
            if (*message.code != 200)
                fact.cancel_reason =
                    reason == "staleRevision" || reason == "invalidState" ? reason : "rejected";
            fact.cancel_state = *message.code != 200 ? HumanCallControlState::Rejected
                                : fact.cancel_result_payload.empty()
                                    ? HumanCallControlState::Pending
                                    : HumanCallControlState::Completed;
        } else {
            fact.ack_code = *message.code;
            fact.ack_revision = revision;
            fact.ack_payload = message.payload;
            if (*message.code != 200) {
                fact.state = HumanCallTransferState::Rejected;
                fact.reason =
                    reason == "staleRevision" || reason == "invalidState" ? reason : "rejected";
            } else {
                fact.candidate = candidate;
                if (fact.state == HumanCallTransferState::Pending)
                    fact.state = HumanCallTransferState::Transferring;
            }
        }
        call_revision_ = std::max(call_revision_, revision);
        report_transfer(fact);
        return true;
    }
    std::string state;
    if (!string_field(data.get(), "state", state, 16))
        return true;  // 旧控制仍由原严格解码分支裁决。
    const bool cancelled =
        state == "active" && cJSON_GetObjectItemCaseSensitive(data.get(), "candidateSessionId");
    if (state != "transferred" && state != "failed" && !cancelled)
        return true;
    handled = true;
    std::string token, candidate, reason;
    std::uint64_t revision = 0;
    const bool has_candidate = cJSON_GetObjectItemCaseSensitive(data.get(), "candidateSessionId");
    if (!string_field(data.get(), "requestToken", token, 128) ||
        !call_revision_field(data.get(), "revision", revision, false) ||
        (has_candidate && (!string_field(data.get(), "candidateSessionId", candidate, 128) ||
                           candidate == session_)))
        return false;
    if (state == "transferred") {
        if (!exact_keys(data.get(), {"state", "revision", "requestToken", "candidateSessionId"}) ||
            !has_candidate)
            return false;
    } else if (cancelled) {
        if (!exact_keys(data.get(),
                        {"state", "revision", "requestToken", "candidateSessionId", "reason"}) ||
            !field(data.get(), "reason", "candidateCancelled"))
            return false;
        reason = "candidateCancelled";
    } else {
        if (!(has_candidate
                  ? exact_keys(data.get(), {"state", "revision", "requestToken",
                                            "candidateSessionId", "reason"})
                  : exact_keys(data.get(), {"state", "revision", "requestToken", "reason"})) ||
            !string_field(data.get(), "reason", reason, 64) ||
            (reason != "transferRejected" && reason != "transferTimeout" &&
             reason != "transferMediaFailed" && reason != "transferUnavailable" &&
             reason != "transferRevisionChanged" && reason != "transferPermissionRevoked"))
            return false;
    }
    // 私有直接调用也必须有原信令水位；缺失时不修改平台版本、转接事实或实际媒体。
    if (!message.event_sequence.has_value())
        return false;
    if (*message.event_sequence <= event_seq_ || state_ == HumanCallState::Ending)
        return true;
    if (state_ != HumanCallState::Connected)
        return false;
    if (found != transfers_.end()) {
        auto& fact = *found;
        if ((!fact.candidate.empty() && fact.candidate != candidate) ||
            (cancelled ? token != fact.cancel_token || revision != fact.cancel_revision + 1 ||
                             (fact.cancel_code && fact.cancel_code != 200)
                       : token != fact.token || revision < fact.revision + 2 ||
                             (state == "transferred" && revision != fact.revision + 2) ||
                             (fact.ack_code && fact.ack_code != 200)))
            return false;
        const auto& previous = cancelled ? fact.cancel_result_payload : fact.result_payload;
        if (!previous.empty() && previous != message.payload)
            return false;
    } else if (state == "transferred")
        return false;  // 只有原发起端原操作能确认退出；留存端不能从陌生token取得源角色。
    if (revision <= platform_revision_) {
        if (revision == platform_revision_ && platform_call_payload_ != message.payload)
            return false;
        event_seq_ = *message.event_sequence;
        return true;
    }
    if (found != transfers_.end()) {
        auto& fact = *found;
        if (has_candidate)
            fact.candidate = candidate;
        fact.result_revision = revision;
        fact.reason = reason;
        if (cancelled) {
            fact.cancel_result_payload = message.payload;
            if (!fact.cancel_expired)
                fact.cancel_state = fact.cancel_code == 200 ? HumanCallControlState::Completed
                                                            : HumanCallControlState::Pending;
            if (fact.state != HumanCallTransferState::Unknown)
                fact.state = HumanCallTransferState::Cancelled;
        } else {
            fact.result_payload = message.payload;
            if (fact.state != HumanCallTransferState::Unknown)
                fact.state = state == "transferred" ? HumanCallTransferState::Transferred
                                                    : HumanCallTransferState::Failed;
        }
        report_transfer(fact);
    }
    call_revision_ = std::max(call_revision_, revision);
    platform_revision_ = revision;
    platform_call_state_ = state;
    platform_control_token_ = token;
    platform_call_payload_ = message.payload;
    event_seq_ = *message.event_sequence;
    return true;  // 不调用hold/begin/close；平台操作结果与原端实际媒体生命期分开。
}
// transfer_call
// 提交原设备Party，不赋予候选身份；与保持/恢复分账但共享同腿控制预算。
bool HumanCallClient::transfer_call(std::string_view target_device, std::uint64_t now) {
    if (endpoint_source_busy()) return false;
    if (now < last_now_)
        return false;
    expire_transfer(now);
    expire_call_control(now);
    const auto separator = target_device.find(':');
    if (state_ != HumanCallState::Connected || call_held_ || pending_call_media_ ||
        transfer_busy() || control_state_ == HumanCallControlState::Pending ||
        control_state_ == HumanCallControlState::Unknown ||
        call_controls_.size() + transfers_.size() >= 24 || call_revision_ >= 9007199254740990ULL ||
        target_device.size() < 3 || target_device.size() > 128 || separator == 0 ||
        separator == std::string_view::npos || separator == target_device.size() - 1 ||
        target_device.find_first_of("/+#") != std::string_view::npos ||
        std::any_of(target_device.begin(), target_device.end(),
                    [](unsigned char ch) { return ch < 32 || ch == 127; }))
        return false;
    Json root(cJSON_CreateObject(), cJSON_Delete);
    if (!root || !cJSON_AddStringToObject(root.get(), "operation", "transfer") ||
        !cJSON_AddNumberToObject(root.get(), "expectedRevision",
                                 static_cast<double>(call_revision_)))
        return false;
    auto* target = cJSON_AddObjectToObject(root.get(), "target");
    const std::string owned(target_device);
    if (!target || !cJSON_AddStringToObject(target, "kind", "device") ||
        !cJSON_AddStringToObject(target, "id", owned.c_str()))
        return false;
    char* raw = cJSON_PrintUnformatted(root.get());
    if (!raw)
        return false;
    Envelope request;
    request.method = "callControl";
    request.token = next_token();
    request.session = session_;
    request.payload = raw;
    cJSON_free(raw);
    std::string body;
    if (encode_envelope(request, body) != DecodeResult::Ok ||
        call_requests_.submit({request.token, session_, "callControl", up_, body}, now) !=
            SubmitResult::Accepted)
        return false;
    TransferFact fact;
    fact.token = request.token;
    fact.target = owned;
    fact.revision = call_revision_;
    fact.started = now;
    transfers_.push_back(std::move(fact));
    last_now_ = now;
    report_transfer(transfers_.back());
    return true;
}
// cancel_transfer
// 只能使用当前受理候选和原修订；撤候选受理与45秒总窗口相交，不能另开转接代次。
bool HumanCallClient::cancel_transfer(std::uint64_t now) {
    if (now < last_now_)
        return false;
    expire_transfer(now);
    if (state_ != HumanCallState::Connected || call_held_ || transfers_.empty() ||
        pending_call_media_)
        return false;
    auto& fact = transfers_.back();
    if (fact.state != HumanCallTransferState::Transferring || fact.ack_code != 200 ||
        fact.candidate.empty() || !fact.cancel_token.empty() ||
        call_revision_ >= 9007199254740991ULL)
        return false;
    Json root(cJSON_CreateObject(), cJSON_Delete);
    if (!root || !cJSON_AddStringToObject(root.get(), "operation", "cancel") ||
        !cJSON_AddNumberToObject(root.get(), "expectedRevision",
                                 static_cast<double>(call_revision_)) ||
        !cJSON_AddStringToObject(root.get(), "candidateSessionId", fact.candidate.c_str()))
        return false;
    char* raw = cJSON_PrintUnformatted(root.get());
    if (!raw)
        return false;
    Envelope request;
    request.method = "callControl";
    request.token = next_token();
    request.session = session_;
    request.payload = raw;
    cJSON_free(raw);
    std::string body;
    if (encode_envelope(request, body) != DecodeResult::Ok ||
        call_requests_.submit({request.token, session_, "callControl", up_, body}, now) !=
            SubmitResult::Accepted)
        return false;
    fact.cancel_token = request.token;
    fact.cancel_revision = call_revision_;
    fact.cancel_started = last_now_ = now;
    fact.cancel_state = HumanCallControlState::Pending;
    report_transfer(fact);
    return true;
}
// control_call 一个本端控制只生成一次原请求；能力、角色阶段和期限在发送前限制。
bool HumanCallClient::control_call(std::string_view operation, std::uint64_t now) {
    if (endpoint_source_busy()) return false;
    if (now < last_now_)
        return false;
    expire_call_control(now);
    expire_transfer(now);
    if (session_.empty() || call_controls_.size() + transfers_.size() >= 24 || transfer_busy() ||
        control_state_ == HumanCallControlState::Pending ||
        control_state_ == HumanCallControlState::Unknown || call_revision_ >= 9007199254740991ULL)
        return false;
    if (operation == "cancel") {
        if (create_token_.empty() || state_ == HumanCallState::Ending)
            return false;  // 原主叫可以请求平台裁决，已接通拒绝不降级成挂断。
    } else if ((operation != "hold" && operation != "resume") ||
               state_ != HumanCallState::Connected || !media_.hold_supported() ||
               (operation == "hold" ? call_held_ : !call_held_))
        return false;
    Envelope message;
    message.method = "callControl";
    message.token = next_token();
    message.session = session_;
    message.payload = "{\"operation\":\"" + std::string(operation) +
                      "\",\"expectedRevision\":" + std::to_string(call_revision_) + "}";
    std::string body;
    if (encode_envelope(message, body) != DecodeResult::Ok ||
        call_requests_.submit({message.token, session_, "callControl", up_, body}, now) !=
            SubmitResult::Accepted)
        return false;
    CallControlFact fact;
    fact.token = message.token;
    fact.operation = operation;
    fact.revision = call_revision_;
    fact.started = now;
    call_controls_.push_back(std::move(fact));
    last_now_ = now;
    control_state_ = HumanCallControlState::Pending;
    return true;
}
bool HumanCallClient::dial(std::string_view target, std::uint64_t now) {
    if (state_ != HumanCallState::Available || !create_token_.empty())
        return false;
    auto payload = create_payload(target, config_);
    if (payload.empty())
        return false;
    // 主叫音视频均需预约共享硬件，平台成功应答后才能在同一预约上启动媒体。
    if (!prepare_media())
        return false;
    create_token_ = next_token();
    phase_at_ = now;
    if (!send("sessionCreate", create_token_, std::move(payload), false)) {
        create_token_.clear();
        media_.close();  // 失败提交释放本次候选预约，不借旧AI资源。
        return false;
    }
    state_ = HumanCallState::Inviting;
    input_wanted_ = true;  // 明确本人拨号，仍等平台Focus与实际HAL后才采集。
    return true;
}
bool HumanCallClient::accept(std::uint64_t now) {
    if (state_ != HumanCallState::Ringing)
        return false;
    for (const auto& fact : invitations_)
        if (fact.token == invite_token_ && fact.session == session_ && fact.terminal)
            return false;
    phase_at_ = now;
    last_now_ = now;
    if (!prepare_media()) {
        retire_invite();  // 原本地Busy决定即使背压也不能改释成新的接听许可。
        if (!send("sessionReject", invite_token_, R"({"reason":"busy"})", true))
            return false;
        invite_method_ = "sessionReject";
        state_ = HumanCallState::Ending;
        return true;
    }
    if (!send("sessionAccept", invite_token_,
              config_.mode == HumanCallMode::Voice
                  ? R"({"acceptedTracks":["voice"]})"
                  : (selected_receive_video_
                         ? R"({"acceptedTracks":["mic-1","speaker-1","video-1","video-in"]})"
                         : R"({"acceptedTracks":["mic-1","speaker-1","video-1"]})"),
              true)) {
        media_.close();
        return false;
    }
    invite_method_ = "sessionAccept";
    state_ = HumanCallState::Accepting;
    input_wanted_ = true;  // 本人明确接听，不借候选ready/abort或重投恢复旧输入。
    return true;
}
bool HumanCallClient::reject(std::uint64_t now) {
    if (state_ != HumanCallState::Ringing)
        return false;
    phase_at_ = now;
    retire_invite();  // 用户明确拒绝后不能借QoS1旧邀请重新弹窗。
    if (!send("sessionReject", invite_token_, R"({"reason":"declined"})", true))
        return false;
    invite_method_ = "sessionReject";
    media_.close();
    state_ = HumanCallState::Ending;
    return true;
}
bool HumanCallClient::offer_ready(std::string_view sdp, std::uint64_t now) {
    if (state_ != HumanCallState::Negotiating || !offer_token_.empty())
        return false;
    auto payload = selected_udp_av_ ? std::string(R"({"mediaRevision":1,"udpAv":{"profile":"opus16-jpeg240-ctr128-v1"}})") : offer_payload(sdp);
    if (payload.empty())
        return false;
    offer_token_ = next_token();
    phase_at_ = now;
    return send("mediaOffer", offer_token_, std::move(payload), true);
}
bool HumanCallClient::media_connected(std::uint64_t now) {
    if (state_ != HumanCallState::ConnectingMedia || !media_.connected())
        return false;
    state_token_ = next_token();
    phase_at_ = now;
    if (!send("mediaState", state_token_, R"({"mediaRevision":1,"state":"connected"})", true))
        return false;
    state_ = HumanCallState::WaitingEstablished;
    return true;
}
bool HumanCallClient::control_track(std::string_view track_id, bool paused, std::uint64_t now) {
    if (!paused && endpoint_source_busy()) return false;  // 本人可停止输入，接续不能暗中重启它。
    if (state_ != HumanCallState::Connected || !track_token_.empty() ||
        (config_.mode == HumanCallMode::Voice ? track_id != "voice"
                                              : (track_id != "mic-1" && track_id != "video-1" && !(track_id == "video-in" && paused && selected_receive_video_))))
        return false;
    track_token_ = next_token();
    track_id_ = track_id;
    track_state_ = paused ? "paused" : "active";
    track_sent_at_ = phase_at_ = now;
    const std::string payload = std::string("{\"trackId\":\"") + track_id_ + "\",\"operation\":\"" +
                                (paused ? "pause" : "resume") + "\",\"mediaRevision\":1}";
    if (!send("trackControl", track_token_, payload, true)) {
        track_token_.clear();
        track_id_.clear();
        track_state_.clear();
        return false;
    }
    if (config_.mode == HumanCallMode::Voice || track_id == "mic-1") input_wanted_ = !paused;
    return true;
}
bool HumanCallClient::end(std::uint64_t now) {
    if (endpoint_source_) endpoint_source_->suspend(now);  // 不造abort或清未知，正式终态后才清腿。
    if (state_ == HumanCallState::Ringing)
        return reject(now);
    if (session_.empty() || state_ == HumanCallState::Ending || state_ == HumanCallState::Idle ||
        state_ == HumanCallState::Failed)
        return false;
    input_wanted_ = false; // 本人挂断立即撤销输入意图，迟到原提交/保持回执不能恢复它。
    media_.close();
    call_requests_.cancel_session(session_, now);
    if (control_state_ == HumanCallControlState::Pending && !call_controls_.empty()) {
        call_controls_.back().unknown = true;
        control_state_ = HumanCallControlState::Unknown;
    }
    participant_roster_.clear();  // 本地挂断立即撤销展示事实，不等待网络ACK才清名册。
    end_token_ = next_token();
    phase_at_ = now;
    if (!send("sessionEnd", end_token_, R"({"reason":"hangup"})", true)) {
        fail(HumanCallFailure::EndPublish);
        return false;
    }
    state_ = HumanCallState::Ending;
    return true;
}
}  // namespace ur::device
