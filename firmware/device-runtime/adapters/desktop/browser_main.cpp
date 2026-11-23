// 浏览器桌面设备：C++独占身份/MQTT/UDP状态，浏览器仅以回环桥提供PCM与操作界面。
#include <cJSON.h>
#include <openssl/rand.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>
#include "action_file_ledger.hpp"
#include "browser_ai_control.hpp"
#include "browser_ai_endpoint_focus.hpp"
#include "browser_bridge.hpp"
#include "browser_client_tool.hpp"
#include "browser_file_catalog.hpp"
#include "browser_file_lane.hpp"
#include "browser_live_endpoint.hpp"
#include "browser_live_volume.hpp"
#include "browser_media_focus.hpp"
#include "browser_voice_focus.hpp"
#include "browser_webrtc_media.hpp"
#include "browser_wire.hpp"
#include "device_credentials.hpp"
#include "environment.hpp"
#include "file_actions.hpp"
#include "file_assets.hpp"
#include "live_assets.hpp"
#include "mosquitto_port.hpp"
#include "openssl_ctr.hpp"
#include "opus_codec.hpp"
#include "udp_socket.hpp"
#include "ur/device/json_parse.hpp"
#include "ur/device/udp_voice_media.hpp"
#include "ur/device/voice_runtime.hpp"
#include "web_assets.hpp"

namespace ur::device {
namespace {
// VisionLocalResult区分非视觉帧、已消费原帧与拒绝；本地分类不授相机或模型权限。
enum class VisionLocalResult { Other, Handled, Rejected };
// EndpointLocalIntent 只表示认证回环桥上的本人接听/拒接，不接受任何平台标识或授权参数。
enum class EndpointLocalIntent { Other, Accept, Reject, Invalid };
// EndpointSourceIntent 是认证本机源端意图；只允许目标愿望，不允许页面自报原腿或候选身份。
struct EndpointSourceIntent {
    std::string operation, target;  // 原三项操作与prepare唯一目标；没有会话、token或期限字段。
    bool matched = false, valid = false;  // 非本类帧与畸形本类帧分开，不误分派原媒体消息。
};
// endpoint_source_intent 严格按操作核条件字段，完整Topic和当前原腿只能由C++生成。
EndpointSourceIntent endpoint_source_intent(std::string_view body) {
    EndpointSourceIntent result;
    std::string canonical;
    if (canonical_json_object(body, canonical) != DecodeResult::Ok) return result;
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> value(parse_json(canonical.c_str()), cJSON_Delete);
    const auto* kind = cJSON_GetObjectItemCaseSensitive(value.get(), "kind");
    if (!cJSON_IsString(kind) || std::string_view(kind->valuestring) != "endpointSourceControl")
        return result;
    result.matched = true;
    const auto* operation = cJSON_GetObjectItemCaseSensitive(value.get(), "operation");
    if (body.size() > 1024 || !cJSON_IsString(operation)) return result;
    result.operation = operation->valuestring;
    if (result.operation != "prepare" && result.operation != "commit" && result.operation != "abort")
        return result;
    const bool prepare = result.operation == "prepare";  // 只有prepare允许一个目标愿望字段。
    if (cJSON_GetArraySize(value.get()) != (prepare ? 3 : 2)) return result;
    if (prepare) {
        const auto* target = cJSON_GetObjectItemCaseSensitive(value.get(), "target");
        if (!cJSON_IsString(target)) return result;
        result.target = target->valuestring;
        if (result.target.empty() || result.target.size() > 128 ||
            std::any_of(result.target.begin(), result.target.end(),
                        [](unsigned char ch) { return ch < 32 || ch == 127; })) return result;
    }
    result.valid = true;
    return result;
}
// endpoint_source_state 只读原控制器的固定事实；不暴露设备身份、原腿、handoff或请求关联。
std::string endpoint_source_state(const EndpointHandoffReceipt* receipt, bool available) {
    if (!receipt) return {};
    const bool terminal = receipt->state == "committed" || receipt->state == "aborted" ||
                          receipt->state == "failed";
    if (!receipt->state.empty() && receipt->state != "preparing" && receipt->state != "ready" &&
        !terminal) return {};
    const bool permitted = available && !receipt->pending && !receipt->unknown;
    const auto state = receipt->unknown ? "unknown" :
                       !receipt->state.empty() ? receipt->state.c_str() :
                       receipt->pending ? "pending" : receipt->code >= 400 ? "rejected" : "idle";
    return std::string("{\"kind\":\"endpointSourceState\",\"state\":\"") + state +
           "\",\"canPrepare\":" + (permitted && (receipt->handoff.empty() || terminal) ? "true" : "false") +
           ",\"canCommit\":" + (permitted && receipt->state == "ready" && !receipt->candidate.empty() ? "true" : "false") +
           ",\"canAbort\":" + (permitted && !receipt->handoff.empty() && !terminal ? "true" : "false") +
           ",\"pending\":" + (receipt->pending ? "true" : "false") +
           ",\"unknown\":" + (receipt->unknown ? "true}" : "false}");
}
// endpoint_local_intent 严格单字段；页面不能选择会话、修改截止或伪造接续提交。
EndpointLocalIntent endpoint_local_intent(std::string_view body) {
    std::string canonical;
    if (canonical_json_object(body, canonical) != DecodeResult::Ok)
        return EndpointLocalIntent::Invalid;
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> value(parse_json(canonical.c_str()), cJSON_Delete);
    const auto* kind = cJSON_GetObjectItemCaseSensitive(value.get(), "kind");
    if (!cJSON_IsString(kind))
        return EndpointLocalIntent::Other;
    const std::string_view name = kind->valuestring;
    if (name != "endpointAccept" && name != "endpointReject")
        return EndpointLocalIntent::Other;
    if (body.size() > 128 || cJSON_GetArraySize(value.get()) != 1)
        return EndpointLocalIntent::Invalid;
    return name == "endpointAccept" ? EndpointLocalIntent::Accept : EndpointLocalIntent::Reject;
}
// endpoint_local_state 只投影固定阶段及两项本地按钮资格，不暴露原设备、公开腿或接续ID。
std::string endpoint_local_state(AIEndpointPhase phase) {
    if (phase < AIEndpointPhase::Disabled || phase > AIEndpointPhase::Failed)
        return {};
    const auto invited = phase == AIEndpointPhase::Invited;
    return "{\"kind\":\"endpointState\",\"phase\":" + std::to_string(static_cast<int>(phase)) +
           ",\"canAccept\":" + (invited ? "true" : "false") +
           ",\"canReject\":" + (invited ? "true}" : "false}");
}
// endpoint_assets 只装配原白名单同源固定脚本；boot来自native，不接受URL或自由JS正文。
bool endpoint_assets(std::map<std::string, std::string>& assets, std::uint64_t boot,
                     bool use_webrtc) {
    const auto page = assets.find("/index.html");
    if (!boot || boot > 9007199254740991ULL || page == assets.end() || !assets.count("/app.js"))
        return false;
    const auto marker = page->second.find("  <script src=\"/app.js\"");
    if (marker == std::string::npos)
        return false;
    assets["/shared-handoff-native.js"] =
        "'use strict';globalThis.UrAIEndpointListen=true;globalThis.UrInputOwnershipBoot='" +
        std::to_string(boot) + "';globalThis.UrInputOwnershipSampleRate=" +
        (use_webrtc ? "48000" : "16000") + ";\n";
    page->second.insert(marker,
        "  <script src=\"/shared-handoff-native.js\"></script>\n"
        "  <script src=\"/input-ownership.js\" defer></script>\n");
    return true;
}
// vision_local只分派原认证桥原始JSON；适配器核完整scope，不从指标或kind名称造HAL事实。
VisionLocalResult vision_local(std::string_view body, bool use_webrtc, bool session_ready,
                               BrowserWebRTCMedia& media) {
    std::string canonical;
    if (canonical_json_object(body, canonical) != DecodeResult::Ok)
        return VisionLocalResult::Other;
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> value(parse_json(canonical.c_str()),
                                                          cJSON_Delete);
    const auto* kind = value ? cJSON_GetObjectItemCaseSensitive(value.get(), "kind") : nullptr;
    if (!cJSON_IsString(kind))
        return VisionLocalResult::Other;
    const std::string_view name = kind->valuestring;
    if (name != "webrtcVisionActual" && name != "webrtcVisionReady")
        return VisionLocalResult::Other;
    if (!use_webrtc)
        return VisionLocalResult::Rejected;
    if (name == "webrtcVisionReady") {
        // 一次实际预检必须先于能力报告；显式视觉不支持时关闭，不换普通AI用途暗中降级。
        return !session_ready && media.receive_vision_ready(body) && media.vision_profile()
                   ? VisionLocalResult::Handled
                   : VisionLocalResult::Rejected;
    }
    return session_ready && media.receive_vision_actual(body) ? VisionLocalResult::Handled
                                                              : VisionLocalResult::Rejected;
}
// VisionCameraConfig仅来自可信native显式映射；profile未预检时ready=false，不授取帧/默认设备选择。
struct VisionCameraConfig {
    VoiceVisionProfile profile;  // 原camera/track绑定，实际adapter预检后才可声明ready。
    std::string device_id;       // 仅本机浏览器exact设备映射，不发送到MQTT或模型。
};
// vision_camera_config严格解析三字段；默认空不启用，UDP、未知字段、非法/过长标识均拒绝。
bool vision_camera_config(std::string_view body, bool use_webrtc,
                          std::optional<VisionCameraConfig>& out) {
    out.reset();
    if (body.empty())
        return true;
    std::string canonical;
    if (!use_webrtc || body.size() > 2048 ||
        canonical_json_object(body, canonical) != DecodeResult::Ok)
        return false;
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(parse_json(canonical.c_str()),
                                                         cJSON_Delete);
    if (!root || cJSON_GetArraySize(root.get()) != 3)
        return false;
    VisionCameraConfig config;
    const auto field = [&root](const char* key, std::size_t maximum, std::string& target) {
        const auto* value = cJSON_GetObjectItemCaseSensitive(root.get(), key);
        if (!cJSON_IsString(value))
            return false;
        target = value->valuestring;
        if (target.empty() || target.size() > maximum)
            return false;
        return std::none_of(target.begin(), target.end(),
                            [](unsigned char byte) { return byte < 32 || byte == 127; });
    };
    if (!field("cameraId", 64, config.profile.camera_id) ||
        !field("videoTrack", 64, config.profile.video_track) ||
        !field("deviceId", 256, config.device_id) || config.profile.video_track == "mic-1" ||
        config.profile.video_track == "speaker-1")
        return false;
    // 配置只能固定原标识；真实可用性由后续enumerateDevices/H264预检回读，不能从env存在推断。
    config.profile.ready = false;
    out = std::move(config);
    return true;
}
// vision_camera_script仅安全序列化本机映射到同源外部资产，不注入自由JS、权限或任何平台凭据。
std::string vision_camera_script(const VisionCameraConfig& config) {
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_CreateObject(), cJSON_Delete);
    if (!root ||
        !cJSON_AddStringToObject(root.get(), "cameraId", config.profile.camera_id.c_str()) ||
        !cJSON_AddStringToObject(root.get(), "videoTrack", config.profile.video_track.c_str()) ||
        !cJSON_AddStringToObject(root.get(), "deviceId", config.device_id.c_str()))
        return {};
    std::unique_ptr<char, decltype(&cJSON_free)> raw(cJSON_PrintUnformatted(root.get()),
                                                     cJSON_free);
    if (!raw)
        return {};
    std::string canonical;
    if (canonical_json_object(raw.get(), canonical) != DecodeResult::Ok)
        return {};
    return "'use strict';globalThis.UrAIVisionCamera=" + canonical + ";\n";
}
// vision_capability只在同一实际预检端口已ready时增加用途；旧单双工/AEC仍按原真实音频事实声明。
bool vision_capability(const std::optional<VisionCameraConfig>& config,
                       const BrowserWebRTCMedia& media, VoiceCapability& capability) {
    if (!config)
        return true;
    const auto* profile = media.vision_profile();
    if (!profile || !profile->ready || profile->camera_id != config->profile.camera_id ||
        profile->video_track != config->profile.video_track ||
        capability.transport != VoiceTransport::WebRTC)
        return false;
    capability.ai_vision = true;
    capability.text_input = false;  // 专用三轨用途不借文字/附件Input协商，保持原视觉创建合同。
    return true;
}
// local_live_kind
// 只识别固定本地驱动出口，实际字段/代次由各原端口核验，不以kind授予权限。
std::string local_live_kind(std::string_view body) {
    std::string canonical;
    if (canonical_json_object(body, canonical) != DecodeResult::Ok)
        return {};
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> value(ur::device::parse_json(canonical.c_str()),
                                                          cJSON_Delete);
    const auto* kind = value ? cJSON_GetObjectItemCaseSensitive(value.get(), "kind") : nullptr;
    if (!cJSON_IsString(kind))
        return {};
    const std::string name = kind->valuestring;
    const std::array<std::string_view, 8> allowed{
        "liveReady",  "liveLaneGated", "liveOffer",   "liveConnected",
        "liveFailed", "liveTrackFact", "liveStopped", "liveVolumeApplied"};
    return std::find(allowed.begin(), allowed.end(), name) != allowed.end() ? name : "";
}
// live_readiness
// 仅一次原真实预检的两个boolean；不是业务授权/焦点mask/已播放事实。
std::optional<BrowserLiveReadiness> live_readiness(std::string_view body) {
    std::string canonical;
    if (canonical_json_object(body, canonical) != DecodeResult::Ok)
        return {};
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> value(ur::device::parse_json(canonical.c_str()),
                                                          cJSON_Delete);
    if (!value || cJSON_GetArraySize(value.get()) != 3)
        return {};
    const auto* kind = cJSON_GetObjectItemCaseSensitive(value.get(), "kind");
    const auto* audio = cJSON_GetObjectItemCaseSensitive(value.get(), "audio");
    const auto* video = cJSON_GetObjectItemCaseSensitive(value.get(), "video");
    if (!cJSON_IsString(kind) || std::string_view(kind->valuestring) != "liveReady" ||
        !cJSON_IsBool(audio) || !cJSON_IsBool(video))
        return {};
    return BrowserLiveReadiness{cJSON_IsTrue(audio) != 0, cJSON_IsTrue(video) != 0};
}
// env只读取显式进程环境，不打印三元组或从其他环境自动发现身份。
std::string env(const char* key) { return environment_value(key).value_or(""); }
// now为协议计时提供单调毫秒，签名过期时间另取系统时钟。
std::uint64_t now() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}
// wall_now 仅供独立租约截止与实际信令核验，不能用单调时间冒充Unix毫秒。
std::uint64_t wall_now() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
}
// random_token每个进程生成256位随机配对令牌，绝不输出至控制台。
std::string random_token() {
    std::array<unsigned char, 32> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
        return {};
    const char hex[] = "0123456789abcdef";
    std::string result;
    for (const auto byte : bytes) {
        result += hex[byte >> 4];
        result += hex[byte & 15];
    }
    return result;
}
// port_number拒绝隐式默认目标和越界端口。
unsigned port_number(const std::string& text) {
    unsigned result = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && result <= 65535
               ? result
               : 0;
}
// configuration复用正式设备签名；密码只传给MQTT适配器。
bool configuration(MqttConfig& config, std::string& identity, const std::string& instance) {
    const auto product = env("UR_DEVICE_PRODUCT");
    const auto device = env("UR_DEVICE_NAME");
    const auto expiry = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count() +
                        3600;
    if (expiry <= 0)
        return false;
    const auto credentials = device_credentials(product, device, env("UR_DEVICE_SECRET"), instance,
                                                static_cast<std::uint64_t>(expiry));
    const auto port = port_number(env("UR_MQTT_PORT"));
    if (!credentials || port == 0 || env("UR_MQTT_HOST").empty())
        return false;
    config.host = env("UR_MQTT_HOST");
    config.port = static_cast<std::uint16_t>(port);
    config.ca_file = env("UR_MQTT_CA");
    config.client_id = credentials->client_id;
    config.username = credentials->username;
    config.password = credentials->password;
    identity = product + "/" + device;
    return true;
}
// run先等待实际端口准备再声明能力；None只预检显示，音频路径须确认真实媒体，关闭网页释放云端连接。
int run(unsigned port) {
    const auto media_setting = env("UR_MEDIA_TRANSPORT");
    if (!media_setting.empty() && media_setting != "udp" && media_setting != "webrtc" &&
        media_setting != "none") {
        std::cerr << "invalid_media_transport\n";
        return 2;
    }
    const bool use_webrtc = media_setting == "webrtc";
    const bool use_none = media_setting == "none";  // 原核心无媒体构造，不借空驱动广告音频。
    const auto reply_receipt_setting = env("UR_DEVICE_UDP_REPLY_START_RECEIPT");
    const bool reply_receipt_requested =
        reply_receipt_setting == "1";  // 只有可信native启动显式启用。
    if ((!reply_receipt_setting.empty() && reply_receipt_setting != "0" &&
         !reply_receipt_requested) ||
        (reply_receipt_requested && (use_webrtc || use_none))) {
        std::cerr << "invalid_udp_reply_start_receipt_configuration\n";
        return 2;
    }
    std::optional<VisionCameraConfig> vision_config;
    if (!vision_camera_config(env("UR_AI_VISION_CAMERA"), use_webrtc, vision_config)) {
        std::cerr << "invalid_vision_configuration\n";
        return 2;
    }
    const auto focus_setting = env("UR_OUTPUT_FOCUS");
    const bool use_files = focus_setting == "media";
    const bool use_live = focus_setting == "live";
    const bool use_focus = focus_setting == "voice" || use_files || use_live;
    const auto endpoint_setting = env("UR_DEVICE_AI_ENDPOINT_LISTEN");
    const bool endpoint_listen = endpoint_setting == "1";
    const auto endpoint_sync_setting = env("UR_DEVICE_AI_ENDPOINT_CONTROL_SYNC");
    const bool endpoint_sync = endpoint_sync_setting == "1";
    // 目标恢复只装配本人接听、完整音频HAL和原控制同步；None/共享输出/视觉不能虚报。
    if ((!endpoint_sync_setting.empty() && !endpoint_sync) ||
        (endpoint_sync && (!endpoint_listen || env("UR_DEVICE_CONTROL_SYNC") != "1" ||
                          use_none || use_focus || vision_config))) {
        std::cerr << "invalid_ai_endpoint_control_sync_configuration\n";
        return 2;
    }
    // 候选仅装配原UDP或WebRTC完整组；不借None、视觉或另一输出仲裁器虚报接续能力。
    if ((!endpoint_setting.empty() && !endpoint_listen) ||
        (endpoint_listen && (use_none || use_focus || vision_config))) {
        std::cerr << "invalid_ai_endpoint_configuration\n";
        return 2;
    }
    const auto endpoint_boot = endpoint_listen ? now() : 0; // 原宿主启动代次，不取网络正文。
    const auto control_sync_setting = env("UR_DEVICE_CONTROL_SYNC");
    const bool control_sync = control_sync_setting == "1";
    if ((!control_sync_setting.empty() && !control_sync) ||
        (control_sync && (use_focus || vision_config))) {
        // 首期只协商已接入独占控制恢复的AI端，不能虚报共享附属资源或视觉已可迁移。
        std::cerr << "invalid_control_sync_configuration\n";
        return 2;
    }
    if ((!focus_setting.empty() && !use_focus) || (use_none && use_focus)) {
        // 非法宿主模式拒绝；有效能力仍须等待选中真实媒体端口的完整idle确认。
        std::cerr << "invalid_output_focus\n";
        return 2;
    }
    std::vector<BrowserFileBinding>
        file_entries;  // 只在明确全媒体模式消费可信宿主配置，不打印源地址。
    if ((use_files &&
         (!decode_browser_file_catalog(env("UR_FILE_CATALOG"), now(), wall_now(), file_entries) ||
          file_entries.front().origin.empty())) ||
        (!use_files && !env("UR_FILE_CATALOG").empty())) {
        std::cerr << "invalid_file_catalog\n";
        return 2;
    }
    const auto token = random_token();
    // MQTT签名connID最多32字符；配对令牌保持256位，连接实例独立使用128位。
    const auto instance = random_token().substr(0, 32);
    MqttConfig config;
    std::string identity;
    if (token.empty() || instance.empty() || !configuration(config, identity, instance)) {
        std::cerr << "invalid_device_environment\n";
        return 2;
    }
    auto assets = browser_assets();  // 默认资产不启用文件媒体；显式模式加入固定本地脚本。
    if (endpoint_listen && !endpoint_assets(assets, endpoint_boot, use_webrtc))
        return 2;
    if (control_sync) {
        // 只启用固定事实消费者，不向页面注入设备配置、凭据或自动恢复权限。
        assets.at("/app.js").insert(0, "'use strict';globalThis.UrAIControlRecovery=true;\n");
    }
    const bool endpoint_source = !use_none && (control_sync || endpoint_listen); // 只显式原绑定媒体宿主。
    if (endpoint_source)
        assets.at("/app.js").insert(0, "'use strict';globalThis.UrAIEndpointSource=true;\n");
    if (use_none) {
        // 固定布尔前缀不接受环境正文；浏览器只能配合本机已选择的无媒体运行时。
        assets.at("/app.js").insert(0, "'use strict';globalThis.UrTextOnlyDevice=true;\n");
    }
    if (vision_config) {
        const auto prefix = vision_camera_script(*vision_config);
        if (prefix.empty())
            return 2;
        assets.at("/app.js").insert(0, prefix);
    }
    if (env("UR_DEVICE_AUDIO_FAILURE_DIAGNOSTICS") == "1") {
        // 固定旁证由原同源资产加载，遵守script-src
        // self；前缀保留严格模式，不注入环境正文。
        assets.at("/app.js").insert(0, "'use strict';globalThis.UrAudioFailureDiagnostics=true;\n");
    }
    if (use_webrtc && (use_focus || endpoint_listen)) {
        // 输出焦点开关必须由原同源脚本在app读取前加载；不放宽CSP或默认授予末端输出。
        assets.at("/app.js").insert(0, "'use strict';globalThis.UrWebRTCOutputFocus=true;\n");
    }
    if (use_files) {
        const auto marker = assets.at("/index.html").find("  <script src=\"/app.js\"");
        if (marker == std::string::npos)
            return 2;
        assets.at("/index.html")
            .insert(marker,
                    "  <script src=\"/file-player.js\" defer></script>\n"
                    "  <script src=\"/file-lane.js\" defer></script>\n"
                    "  <script src=\"/file-host.js\" defer></script>\n");
        auto files = file_assets();
        assets.insert(files.begin(), files.end());
    }
    if (use_live) {
        const auto marker = assets.at("/index.html").find("  <script src=\"/app.js\"");
        if (marker == std::string::npos)
            return 2;
        assets.at("/index.html")
            .insert(marker,
                    "  <script src=\"/live-probe.js\" defer></script>\n"
                    "  <script src=\"/live-player.js\" defer></script>\n"
                    "  <script src=\"/live-host.js\" defer></script>\n");
        auto live = live_assets();
        assets.insert(live.begin(), live.end());
    }
    // 新诊断只由宿主精确开关启用；共享对象仅原子槽，不借用Runtime或延长业务寿命。
    const auto* progress_switch = std::getenv("UR_DEVICE_THREAD_PROGRESS_DIAGNOSTICS");
    auto progress = make_thread_progress(progress_switch ? std::string_view(progress_switch) : "");
    BrowserBridge bridge(port, token, std::move(assets),
                         use_files ? file_entries.front().origin : "", progress);
    if (!bridge.start()) {
        std::cerr << "browser_bind_failed\n";
        return 2;
    }
    std::cout << "http://127.0.0.1:" << bridge.port() << (use_webrtc ? "/?media=webrtc\n" : "/\n")
              << std::flush;
    const AudioFormat format{use_webrtc ? 48000 : 16000, 1, 20};
    BrowserPcm pcm(format);
    OpusCodec codec;
    UdpSocket udp;
    OpenSslCtr cipher;
    UdpVoiceMedia udp_media(pcm, codec, udp, cipher);
    // 预信令观察只由本机精确开关启用，必须在原configure创建未知队列前装配。
    const bool presignal_diagnostics = env("UR_DEVICE_UDP_PRESIGNAL_DIAGNOSTICS") == "1";
    if (presignal_diagnostics && !use_webrtc)
        (void)udp_media.enable_presignal_diagnostics();
    std::uint32_t last_presignal_observation = 0;  // 只输出新冻结快照，不反复统计或重收包。
    BrowserWebRTCMedia webrtc_media;
    if (vision_config &&
        !webrtc_media.prepare_vision_preflight(vision_config->profile, vision_config->device_id)) {
        std::cerr << "invalid_vision_configuration\n";
        return 2;
    }
    PumpedVoiceMediaPort& media = use_webrtc ? static_cast<PumpedVoiceMediaPort&>(webrtc_media)
                                             : static_cast<PumpedVoiceMediaPort&>(udp_media);
    BrowserLiveVolume live_volume(
        use_webrtc ? static_cast<VolumePort&>(webrtc_media) : static_cast<VolumePort&>(pcm),
        use_webrtc ? static_cast<DeferredVolumePort*>(&webrtc_media)
                   : nullptr);  // 原唯一动作同时等待真实AI和节目增益。
    VolumePort& volume = use_live ? static_cast<VolumePort&>(live_volume)
                                  : (use_webrtc ? static_cast<VolumePort&>(webrtc_media)
                                                : static_cast<VolumePort&>(pcm));
    MosquittoPort mqtt(progress);
    FileActions action_driver(
        volume, use_live
                    ? static_cast<DeferredVolumePort*>(&live_volume)
                    : (use_webrtc ? static_cast<DeferredVolumePort*>(&webrtc_media) : nullptr));
    std::unique_ptr<ActionFileLedger> action_ledger;  // 先于执行器创建，最后回收自己的文件worker。
    if (!configured_action_ledger(identity, action_ledger)) {
        std::cerr << "action_ledger_unavailable\n";
        return 2;
    }
    ActionEngine actions(action_driver, 5000, action_ledger.get());
    std::array<BrowserFileLane, 3> file_lanes{
        BrowserFileLane("audio"), BrowserFileLane("video"),
        BrowserFileLane("audioVideo")};  // 固定实际节点长于借用驱动。
    std::array<BrowserLiveLane, 3> live_lanes{
        BrowserLiveLane("audio"), BrowserLiveLane("video"),
        BrowserLiveLane("audioVideo")};              // 与旧file不装配同一输出路径。
    std::optional<BrowserLiveReadiness> live_ready;  // 原真实预检快照，不由平台邀请提升能力。
    std::unique_ptr<BrowserLiveFactory> live_factory;
    std::unique_ptr<BrowserLiveEndpoint> live_endpoint;  // 在Runtime之后销毁，保存紧凑原事实。
    std::optional<std::size_t> live_refresh;  // 唯一仲裁器实际新false在途，未齐全前继续Busy。
    std::unique_ptr<BrowserFileCatalog> file_catalog;  // 不从MQTT正文加载URL或授予采集权限。
    std::unique_ptr<BrowserResolvedPlaybackAdmission>
        file_admission;                                // 同一实际播放器/共享运行时。
    std::unique_ptr<OutputFocusDriver> output_driver;  // 仅显式模式装配，默认路径不触碰实际门控。
    BrowserClientToolDisplay
        client_display;  // 配对页独立主题端口，生命周期长于Runtime，不借媒体ready。
    std::unique_ptr<VoiceRuntime> runtime;     // 返回前显式reset，保证借用的焦点端点仍有效。
    std::unique_ptr<BrowserAIEndpointFocus> endpoint_hal; // 唯一完整组，生命周期长于原FocusEndpoint。
    bool endpoint_source_live = false; // 原ready到桥关闭的实际本地源可用期，不从邀请推断。
    std::string endpoint_pcm_session; // 已实际绑定的唯一PCM路由，终态后不换腿复用。
    std::optional<std::string> endpoint_idle; // 仅保存先于ready的原一次idle事实，不造新票据。
    std::string previous_endpoint_state; // 固定UI投影去重，不作为资源或接续依据。
    std::string previous_endpoint_source; // 源端闭集事实投影，不包含协议身份或代办控制参数。
    bool runtime_failure_diagnostics = false;  // 精确宿主开关，默认不读取任何层级首因。
    // emit_runtime_failure
    // 领取原失败首因一次；启动和提前退出共用，不补采驱动或向MQTT发送。
    const auto emit_runtime_failure = [&]() {
        if (!runtime_failure_diagnostics || !runtime)
            return;
        const auto failure = runtime->take_failure_diagnostic();
        if (!failure)
            return;
        std::optional<UdpFailure> udp_failure;     // 其他runtime分支不借用旧UDP故障。
        auto pcm_failure = PcmWriteFailure::None;  // 当前UDP写失败以外不导出旧写帧原因。
        if (*failure == RuntimeFailure::MediaPoll && !use_webrtc) {
            udp_failure = udp_media.failure_diagnostic();
            if (udp_failure && *udp_failure == UdpFailure::PcmWrite) {
                pcm_failure = pcm.last_write_failure();
                if (pcm_failure == PcmWriteFailure::None)
                    pcm_failure = PcmWriteFailure::Unknown;  // 缺同次事实不能推测队列已满。
            }
        }
        const auto diagnostic =
            browser_runtime_failure_diagnostic(*failure, udp_failure, pcm_failure);
        if (!diagnostic.empty())
            std::cout << "host_failure=" << diagnostic << '\n' << std::flush;
        if (const auto client_failure = runtime->take_client_failure_diagnostic()) {
            const auto fixed = browser_client_failure_diagnostic(*client_failure);
            if (!fixed.empty())
                std::cout << "host_client_failure=" << fixed << '\n' << std::flush;
        }
        if (const auto scope_failure = runtime->voice().scope_failure()) {
            const auto fixed = browser_scope_failure_diagnostic(*scope_failure);
            if (!fixed.empty())
                std::cout << "host_scope_failure=" << fixed << '\n' << std::flush;
        }  // 随同原首个Runtime失败只输出一次；不查询HAL或改变原退出路径。
    };
    // emit_lifecycle 只读原已启用诊断；固定native
    // JSON不走MQTT，不影响业务退出或网页发送。
    const auto emit_lifecycle = [&]() {
        if (!runtime)
            return;
        if (const auto fact = runtime->take_lifecycle_diagnostic()) {
            const auto fixed = browser_lifecycle_diagnostic(*fact);
            if (!fixed.empty())
                std::cout << "host_lifecycle=" << fixed << '\n' << std::flush;
        }
    };
    std::unique_ptr<FocusAuthority> focus_authority;  // 原已建立业务提供许可，不按网页状态授权。
    std::unique_ptr<FocusEndpoint> focus_endpoint;    // 同一MQTT，独立实际静音后才广告基线。
    std::optional<bool> browser_aec;  // 原ready只接纳一次，不能在等待实际静音时重复初始化。
    bool text_ready = false;          // 无媒体端口已预检显示且只接纳一次，不从麦克风ready反推。
    if (endpoint_listen) {
        const auto deadline = now() + 3600000; // 原宿主独立截止，网络重试不得续期。
        endpoint_hal = std::make_unique<BrowserAIEndpointFocus>(
            use_webrtc ? static_cast<OutputGroupGatePort&>(webrtc_media)
                       : static_cast<OutputGroupGatePort&>(pcm),
            endpoint_boot, deadline, wall_now() + 3600000,
            [&] { return endpoint_source_live; },
            [&](std::string_view session, std::uint64_t generation) {
                if (endpoint_pcm_session.empty()) {
                    if (!(use_webrtc ? webrtc_media.bind_capture_route(session, generation)
                                     : pcm.bind_capture_route(session, generation)))
                        return false;
                    endpoint_pcm_session = session;
                }
                return endpoint_pcm_session == session &&
                    (use_webrtc ? webrtc_media.fence_capture_route(session, generation)
                                : pcm.fence_capture_route(session, generation));
            }, [&]() -> std::optional<AIEndpointFocusScope> {
                return runtime ? runtime->voice().candidate_focus_scope() : std::nullopt;
            }, [&](const std::string& body) { return bridge.send(body); },
            use_webrtc ? BrowserInputRouteKind::AISender : BrowserInputRouteKind::Udp,
            endpoint_sync ? BrowserAIEndpointFocus::Released([&](std::string_view session) {
                return focus_endpoint && focus_endpoint->cleanup_settled(
                    [&](std::string_view original) {return original == session;});
            }) : BrowserAIEndpointFocus::Released{});
        focus_endpoint = std::make_unique<FocusEndpoint>(
            mqtt, endpoint_hal->driver(), *endpoint_hal,
            std::vector<std::string>{"microphone", "speaker"}, "$thing/up/signaling/" + identity,
            "$thing/down/signaling/" + identity, deadline, true);
    }
    if (use_focus) {
        const auto deadline = now() + 3600000;  // 进程独立一小时截止，不从网络请求续长。
        const auto voice_scope = [&]() -> std::optional<std::string> {
            if (!runtime || runtime->state() != RuntimeState::Running ||
                runtime->voice().state() != VoiceState::Established)
                return {};
            return runtime->voice().session_id();
        };  // C++核心原会话事实，不从网页或文件邀请自报父身份。
        OutputGroupGatePort* voice_output = use_webrtc
                                                ? static_cast<OutputGroupGatePort*>(&webrtc_media)
                                                : static_cast<OutputGroupGatePort*>(&pcm);
        std::vector<FocusOutputBinding> outputs{{"", "", {"speaker"}, voice_output}};
        if (use_files) {
            file_catalog =
                std::make_unique<BrowserFileCatalog>(std::move(file_entries), voice_scope);
            file_admission = std::make_unique<BrowserResolvedPlaybackAdmission>(
                [&](const PlaybackInvite& invite, std::uint64_t clock) {
                    return file_catalog->resolve(invite, clock);
                });
            outputs.push_back({"", "", {"speaker"}, &file_lanes[0]});
            outputs.push_back({"", "", {"display"}, &file_lanes[1]});
            outputs.push_back({"", "", {"speaker", "display"}, &file_lanes[2]});
        }
        if (use_live) {
            outputs.push_back({"", "", {"speaker"}, &live_lanes[0]});
            outputs.push_back({"", "", {"display"}, &live_lanes[1]});
            outputs.push_back({"", "", {"speaker", "display"}, &live_lanes[2]});
        }
        output_driver = std::make_unique<OutputFocusDriver>(std::move(outputs), 0,
                                                            (use_files || use_live)
                                                                ? FocusBindingMode::DeferredReleased
                                                                : FocusBindingMode::DeferredOnce);
        if (use_files)
            focus_authority = std::make_unique<BrowserMediaFocusAuthority>(
                *output_driver, *file_admission, deadline, voice_scope,
                [&](std::string_view session) {
                    return runtime && runtime->file_established(session);
                });
        else if (use_live)
            focus_authority = std::make_unique<BrowserLiveFocusAuthority>(
                *output_driver, deadline, voice_scope,
                [&]() { return live_endpoint ? live_endpoint->scope() : std::nullopt; },
                [&](std::string_view session) {
                    return live_endpoint ? live_endpoint->release_scope(session) : std::nullopt;
                });
        else
            focus_authority =
                std::make_unique<BrowserVoiceFocusAuthority>(*output_driver, deadline, voice_scope);
        focus_endpoint = std::make_unique<FocusEndpoint>(
            mqtt, *output_driver, *focus_authority,
            (use_files || use_live) ? std::vector<std::string>{"speaker", "display"}
                                    : std::vector<std::string>{"speaker"},
            "$thing/up/signaling/" + identity, "$thing/down/signaling/" + identity, deadline, true);
    }
    if (focus_endpoint) {
        // 只读回执来自核心原结果和本地MQTT接纳，不曝光身份，不决定输出、授权或后端落库。
        // 候选完整组与原单向输出复用同一观察入口，不能用媒体ready补造焦点事实。
        focus_endpoint->set_receipt_observer([&](std::string_view state, std::uint64_t revision,
                                                 std::string_view operation, int code,
                                                 std::uint64_t receipt_index) {
            bridge.send("{\"kind\":\"focusReceipt\",\"state\":\"" + std::string(state) +
                        "\",\"revision\":" + std::to_string(revision) + ",\"operation\":\"" +
                        std::string(operation) + "\",\"code\":" + std::to_string(code) +
                        ",\"receiptIndex\":" + std::to_string(receipt_index) + "}");
        });
    }
    std::string response;  // 当前媒体播放目标，仅用于帧关联与统计，不提供回复控制资格。
    std::string first_text_response; // 仅最近原文本回复的测量关联，不能授予控制或覆盖旧回复事实。
    std::optional<std::uint64_t> first_text_at; // C++原单调时钟；并发旧回复不匹配时明确缺失。
    std::string previous_control_response;  // 已发送的核心控制目标，变化不依赖媒体状态。
    auto previous = VoiceState::Idle;
    std::uint64_t previous_actions = 0;        // 实际软件驱动完成水位，不使用模型文本推断控制结果。
    std::uint64_t previous_interruptions = 0;  // C++已应用的播放中断，独立于内容终态。
    std::string previous_control_sync;  // 仅投影核心确认的公开代次和水位，不以本地点击认定恢复。
    bool done = false;
    bool failed = false;
    while (!done) {
        if (progress) {
            progress->loops_entered.increment();
            progress->host(HostStage::BridgeInput);
        }
        ProgressExit loop_exit(progress ? &progress->loops_exited : nullptr);
        const auto clock = now();
        const auto wall = wall_now(); // 同一事件批与权限复核共享一份Unix时钟，不把旧缓存当新采样。
        for (unsigned count = 0; count < 32; ++count) {
            auto event = bridge.poll();
            if (!event)
                break;
            if (event->kind == BrowserEventKind::Closed ||
                event->kind == BrowserEventKind::Failed) {
                done = true;
                failed = event->kind == BrowserEventKind::Failed;
                if (event->kind == BrowserEventKind::Closed) {
                    // 只读正常退出边界不反推WS原因；精确开关沿用原端消息诊断，默认不输出。
                    const auto* diagnostic_switch = std::getenv("UR_DEVICE_MESSAGE_DIAGNOSTICS");
                    if (diagnostic_switch && std::string_view(diagnostic_switch) == "1")
                        std::cout << "host_exit_branch=browser_closed\n" << std::flush;
                }
                break;
            }
            if (event->kind != BrowserEventKind::Message)
                continue;
            const auto source_intent = endpoint_source_intent(event->body);
            if (source_intent.matched) {
                if (!endpoint_source || !runtime || !source_intent.valid) {
                    failed = done = true;
                    break;
                }
                // 本地忙/陈旧意图不终止原AI；真正应答由原控制器消费，点击不假造平台受理。
                if (source_intent.operation == "prepare")
                    runtime->prepare_endpoint_source(source_intent.target, clock, wall);
                else if (source_intent.operation == "commit") runtime->commit_endpoint_source(clock, wall);
                else runtime->abort_endpoint_source(clock, wall);
                continue;
            }
            const auto endpoint_intent = endpoint_local_intent(event->body);
            if (endpoint_intent != EndpointLocalIntent::Other) {
                if (!endpoint_hal || !runtime || endpoint_intent == EndpointLocalIntent::Invalid ||
                    (endpoint_intent == EndpointLocalIntent::Accept
                         ? runtime->accept_endpoint_invite(clock, wall)
                         : runtime->reject_endpoint_invite(clock, wall)) != Result::Applied) {
                    failed = done = true;
                    break;
                }
                continue;
            }
            if (endpoint_hal) {
                BrowserInputOwnershipEvent actual;
                if (decode_browser_input_ownership(event->body, actual)) {
                    if (!browser_aec && actual.idle && actual.boot_generation == endpoint_boot &&
                        !endpoint_idle)
                        endpoint_idle = event->body;
                    else if (browser_aec)
                        endpoint_hal->receive(event->body, clock); // 迟到/错代原票据丢弃；当前缺事实由原HAL超时关闭。
                    continue;
                }
            }
            const auto control_request = decode_browser_ai_control_request(event->body);
            if (control_request != BrowserAIControlRequest::Other) {
                if (!control_sync || !runtime ||
                    control_request == BrowserAIControlRequest::Invalid) {
                    failed = done = true;
                    break;
                }
                if (control_request == BrowserAIControlRequest::Disconnect) {
                    if (runtime->state() != RuntimeState::Running ||
                        runtime->voice().state() != VoiceState::Established ||
                        !runtime->voice().control_sync_enabled()) {
                        failed = done = true;
                        break;
                    }
                    // 使用实际网络断连路径；不直接制造平台失联或成功快照。
                    mqtt.disconnect();
                    runtime->poll(clock, wall);
                    if (runtime->state() != RuntimeState::ControlRecovering)
                        failed = done = true;
                } else {
                    MqttConfig fresh;
                    std::string fresh_identity;
                    if (runtime->state() != RuntimeState::ControlRecovering ||
                        !configuration(fresh, fresh_identity, instance) ||
                        fresh_identity != identity ||
                        runtime->resume_connection(fresh, clock) != Result::Applied)
                        failed = done = true;
                }
                if (done)
                    break;
                continue;
            }
            const auto vision = vision_local(event->body, use_webrtc, bool(runtime), webrtc_media);
            if (vision != VisionLocalResult::Other) {
                if (vision == VisionLocalResult::Rejected)
                    failed = done = true;
                if (done)
                    break;
                continue;
            }
            // 本地主题票据只在认证桥内传递；不把平台身份或自由工具参数送进网页。
            {
                std::string canonical;
                if (canonical_json_object(event->body, canonical) == DecodeResult::Ok) {
                    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> value(
                        ur::device::parse_json(canonical.c_str()), cJSON_Delete);
                    const auto* kind = cJSON_GetObjectItemCaseSensitive(value.get(), "kind");
                    if (cJSON_IsString(kind) &&
                        std::string_view(kind->valuestring) == "clientThemeReady") {
                        if (runtime || !client_display.receive_ready(event->body)) {
                            failed = done = true;
                            break;
                        }
                        continue;
                    }
                    if (cJSON_IsString(kind) &&
                        std::string_view(kind->valuestring) == "clientThemeActual") {
                        client_display.receive(
                            event->body);  // 错票据、终态后重复与迟到事实仅隔离，不制造成功。
                        continue;
                    }
                }
            }
            if (use_live) {
                const auto kind = local_live_kind(event->body);
                if (!kind.empty()) {
                    if (kind == "liveReady") {
                        const auto actual = live_readiness(event->body);
                        if (!actual || live_ready || (!actual->audio && !actual->video)) {
                            failed = done = true;
                            break;
                        }
                        live_ready = actual;
                        live_volume.ready(true);
                        const auto deadline = now() + 3600000;
                        live_factory = std::make_unique<BrowserLiveFactory>(
                            std::array<BrowserLiveLane*, 3>{&live_lanes[0], &live_lanes[1],
                                                            &live_lanes[2]},
                            *actual,
                            [&]() -> std::optional<std::string> {
                                if (!runtime || runtime->state() != RuntimeState::Running ||
                                    runtime->voice().state() != VoiceState::Established)
                                    return {};
                                return runtime->voice().session_id();
                            },
                            deadline);
                        live_endpoint = std::make_unique<BrowserLiveEndpoint>(
                            mqtt, *live_factory, *actual, "$thing/up/signaling/" + identity,
                            "$thing/down/signaling/" + identity, deadline);
                    } else if (kind == "liveLaneGated") {
                        for (auto& lane : live_lanes)
                            lane.receive(event->body, clock);
                    } else if (kind == "liveVolumeApplied")
                        live_volume.receive(event->body);
                    else if (live_endpoint)
                        live_endpoint->receive_local(event->body);
                    continue;  // 迟到原instance由严格端口隔离，不落到AI解析器变成另一条控制。
                }
            }
            if (use_files) {
                bool consumed = false;  // 各实际事实独立严格匹配原路径，不进入语音控制解析器。
                for (auto& lane : file_lanes)
                    consumed = lane.receive(event->body, clock) || consumed;
                auto* player = file_admission->active_media();
                if (consumed || (player && player->receive(event->body)))
                    continue;
            }
            BrowserRequest request;
            if (!decode_browser_request(event->body, request)) {
                failed = done = true;
                break;
            }
            if (use_none && request.kind != "textReady" && request.kind != "sendText" &&
                request.kind != "cancel" && request.kind != "end") {
                // 配对不授媒体权限；伪造音频ready/采集/播放/SDP拒绝，不能驱动未装配端口。
                failed = done = true;
                break;
            }
            if (request.kind == "textReady") {
                if (!use_none || text_ready || runtime || !client_display.available()) {
                    failed = done = true;
                    break;
                }
                text_ready = true;
                continue;
            }
            if (request.kind == "ready") {
                if (browser_aec || runtime || request.format.sample_rate != format.sample_rate ||
                    request.format.channels != format.channels ||
                    request.format.frame_ms != format.frame_ms ||
                    (!use_webrtc && !pcm.ready(request.format, request.aec))) {
                    failed = done = true;
                    break;
                }
                browser_aec = request.aec;
                if (use_webrtc && (use_focus || endpoint_listen) &&
                    !webrtc_media.enable_output_gate()) {
                    failed = done = true;
                    break;
                }
                if (endpoint_hal) {
                    endpoint_source_live = true;
                    if (!endpoint_hal->initialize(clock) ||
                        (endpoint_idle && !endpoint_hal->receive(*endpoint_idle, clock))) {
                        failed = done = true;
                        break;
                    }
                    endpoint_idle.reset();
                }
                if (output_driver && !output_driver->initialize(clock)) {
                    failed = done = true;
                    break;
                }
            } else if (request.kind == "outputGated" && browser_aec) {
                // 首次实际false必须能在MQTT运行时创建之前回流；仍核验原已交付票据。
                if (!(use_webrtc
                          ? webrtc_media.output_gated(request.gate_ticket, request.output_enabled,
                                                      clock)
                          : pcm.output_gated(request.gate_ticket, request.output_enabled, clock)))
                    failed = done = true;
            } else if (!runtime) {
                failed = done = true;
                break;
            } else if (request.kind == "webrtcOffer" && use_webrtc) {
                if (!webrtc_media.offer_received(request.sdp, request.peer_revision))
                    failed = done = true;
            } else if (request.kind == "webrtcConnected" && use_webrtc) {
                if (!webrtc_media.connected_received(request.peer_revision))
                    failed = done = true;
            } else if (request.kind == "webrtcDisconnected" && use_webrtc) {
                if (!webrtc_media.disconnected_received(request.peer_revision))
                    failed = done = true;
            } else if (request.kind == "webrtcDrained" && use_webrtc) {
                if (!webrtc_media.drained_received(request.epoch, request.peer_revision))
                    failed = done = true;
            } else if (request.kind == "webrtcVolumeApplied" && use_webrtc) {
                if (!webrtc_media.volume_applied(request.id, request.volume))
                    failed = done = true;
            } else if (request.kind == "captured") {
                if (use_webrtc) {
                    failed = done = true;
                    break;
                }
                // 停止后的迟到采集可丢弃，当前输入故障由PCM返回Failed使运行时收敛。
                if (request.capture_generation)
                    pcm.captured_route(request.capture_session, request.capture_generation,
                                       request.id, request.pcm);
                else
                    pcm.captured(request.id, request.pcm);
            } else if (request.kind == "played") {
                if (use_webrtc)
                    failed = done = true;
                else
                    pcm.played(request.epoch, request.ticket);
            } else if (request.kind == "start")
                runtime->start_input(clock);
            else if (request.kind == "stop")
                runtime->stop_input(clock);
            else if (request.kind == "end")
                runtime->end(clock);
            else if (request.kind == "sendText") {
                const auto result = runtime->send_text(request.text, request.audio, clock);
                const char* outcome = result == Result::Applied       ? "submitted"
                                      : result == Result::Busy        ? "busy"
                                      : result == Result::Unsupported ? "unsupported"
                                                                      : "rejected";
                if (!bridge.send(std::string("{\"kind\":\"textSubmitted\",\"result\":\"") +
                                 outcome + "\"}"))
                    failed = done = true;
            } else if (request.kind == "cancel") {
                // 所有媒体复用原核心控制目标；UDP等待工具尚未发声时也能取消，旧播放ID不能控制新回复。
                if (!request.id.empty() && request.id == runtime->response_control_target())
                    runtime->cancel_response(clock);
            }
        }
        if (done)
            break;
        if (use_files) {
            for (auto& lane : file_lanes) {
                if (!lane.poll(clock))
                    failed = done = true;
                while (auto command = lane.take_command())
                    if (!bridge.send(*command))
                        failed = done = true;
            }
            if (auto* player = file_admission->active_media()) {
                if (!player->tick(clock) && !player->quiescent())
                    player->stop_all();  // 业务撤权仍持续泵实际关闭，不把失败当资源已释放。
                while (auto command = player->take_command())
                    if (!bridge.send(*command))
                        failed = done = true;
            }
        }
        if (use_live) {
            live_volume.poll(clock);
            if (auto command = live_volume.take_command())
                if (!bridge.send(*command))
                    failed = done = true;
            if (browser_aec)
                for (std::size_t index = 0; index < live_lanes.size(); ++index) {
                    auto& lane = live_lanes[index];
                    if (!lane.poll(clock) && (!live_refresh || *live_refresh != index + 1))
                        failed = done = true;  // 原新node刷新失败局部封闭，不关掉无关已经运行的AI。
                    while (auto command = lane.take_command())
                        if (!bridge.send(*command))
                            failed = done = true;
                }
            if (live_endpoint) {
                if (auto* player = live_endpoint->active_media())
                    while (auto command = player->take_command())
                        if (!bridge.send(*command))
                            failed = done = true;
                if (const auto lane = live_endpoint->rearm_lane()) {
                    const auto index = *lane == "audio" ? 1U : (*lane == "video" ? 2U : 3U);
                    if (!live_refresh && output_driver->refresh_idle(index, clock))
                        live_refresh = index;
                    if (live_refresh && output_driver->idle_refresh_ready(*live_refresh)) {
                        live_endpoint->rearmed(*lane);
                        live_refresh.reset();
                    }
                    // 原租约未释放/异常终态清理未收录时保持安全Busy，绝不本地假released或重置仲裁器。
                }
            }
        }
        if (progress)
            progress->host(HostStage::OutputGate);
        if (browser_aec &&
            ((use_webrtc ? !webrtc_media.poll(clock) : !pcm.poll_output_gate(clock)) ||
             (output_driver && !output_driver->poll(clock)) ||
             (endpoint_hal && !endpoint_hal->poll(clock, wall)))) {
            failed = true;
            break;
        }
        // 唯一实际输出路径首次关闭后才连接及声明基线；不以构造或命令受理假报idle。
        if ((use_none ? text_ready : browser_aec.has_value()) && !runtime &&
            (!vision_config || webrtc_media.vision_profile()) &&
            (!use_live || (live_ready && live_endpoint)) &&
            (!output_driver || output_driver->initial_idle()) &&
            (!endpoint_hal || endpoint_hal->initial_idle())) {
            if (progress)
                progress->host(HostStage::RuntimeStart);
            VoiceCapability capability;
            if (use_none) {
                capability.transport = VoiceTransport::None;
                capability.text_input = true;
                capability.client_display_theme = client_display.available();
            } else {
                capability =
                    VoiceCapability{{true, *browser_aec, *browser_aec},
                                    format,
                                    1,
                                    true,
                                    use_webrtc ? VoiceTransport::WebRTC : VoiceTransport::Udp,
                                    use_files,
                                    use_files,
                                    use_live && live_ready->audio,
                                    use_live && live_ready->video,
                                    client_display.available()};
            }
            if (!vision_capability(vision_config, webrtc_media, capability)) {
                failed = true;
                break;
            }
            // 实际BrowserPcm音频Ready、原输出idle和同步UDP HAL齐全才广告；getter不产生安装事实。
            if (reply_receipt_requested && !udp_media.udp_reply_start_receipt_ready()) {
                failed = true;
                break;
            }
            capability.udp_reply_start_receipt = reply_receipt_requested && !use_webrtc &&
                                                 !use_none &&
                                                 udp_media.udp_reply_start_receipt_ready();
            capability.session_sync = control_sync;  // 仅显式装配成功的本机恢复入口才广告能力。
            capability.ai_endpoint_handoff = endpoint_listen; // 独立本人接听+实际完整组，不隐含sessionSync。
            capability.ai_endpoint_control_sync = endpoint_sync && endpoint_hal &&
                endpoint_hal->control_recovery_supported(); // 仅真实有限重绑定及正式release证明链装配后声明。
            if (use_none)
                runtime = std::make_unique<VoiceRuntime>(mqtt, identity, capability, instance,
                                                         nullptr, &client_display);
            else
                runtime = std::make_unique<VoiceRuntime>(
                    mqtt, media, identity, capability, instance, &actions, file_admission.get(),
                    focus_endpoint.get(), live_endpoint.get(),
                    client_display.available() ? &client_display : nullptr);
            // 默认不安装诊断；开关只由实际宿主环境启用，非法值不改变业务或装配能力。
            const auto* diagnostic_switch = std::getenv("UR_DEVICE_MESSAGE_DIAGNOSTICS");
            if (diagnostic_switch && std::string_view(diagnostic_switch) == "1") {
                runtime_failure_diagnostics = true;
                runtime->enable_message_diagnostics();
                if (!use_webrtc && !use_none) {
                    pcm.enable_failure_diagnostics();
                    udp_media.enable_failure_diagnostics();
                }
            }
            if ((endpoint_hal && runtime->set_endpoint_focus_prepare(*endpoint_hal) != Result::Applied) ||
                (endpoint_listen ? runtime->start_listening(config, clock)
                                 : runtime->start(config, clock)) != Result::Applied) {
                emit_runtime_failure();  // 同步connect失败也保留首因，不等下一轮poll。
                failed = true;
                break;
            }
        }
        if (runtime) {
            if (progress) {
                progress->host(HostStage::RuntimePoll);
                progress->runtime_entered.increment();
            }
            runtime->poll(clock, wall);
            if (endpoint_source) {
                const auto projection =
                    endpoint_source_state(runtime->endpoint_source_receipt(),
                                          runtime->endpoint_source_intent_available());
                if (projection.empty() || (projection != previous_endpoint_source && !bridge.send(projection))) {
                    failed = true;
                    break;
                }
                previous_endpoint_source = projection;
            }
            if (endpoint_hal) {
                const auto projection = endpoint_local_state(runtime->voice().endpoint_phase());
                if (projection.empty() || (projection != previous_endpoint_state && !bridge.send(projection))) {
                    failed = true;
                    break;
                }
                previous_endpoint_state = projection;
                if (const auto scope = runtime->voice().candidate_focus_scope()) {
                    // 原实际完整组取得及回执交付齐全后，只解除原输入围栏；不自行开始或恢复用户输入。
                    if (focus_endpoint->scoped_permitted(scope->session_id, {"microphone", "speaker"},
                                                        clock, wall)) {
                        if (!(use_webrtc ? webrtc_media.enable_capture_route(scope->session_id, 1)
                                         : pcm.enable_capture_route(scope->session_id, 1))) {
                            failed = true;
                            break;
                        }
                    } else if (!endpoint_pcm_session.empty()) {
                        if (use_webrtc)
                            webrtc_media.fence_capture_route(endpoint_pcm_session, 1);
                        else
                            pcm.fence_capture_route(endpoint_pcm_session, 1);
                    }
                }
                // 原逻辑终态先实际清原Sender/输出整组，清理齐备后才退休同一Peer。
                if (use_webrtc && !endpoint_pcm_session.empty() && endpoint_hal->settled() &&
                    !webrtc_media.finish_capture_route_close(endpoint_pcm_session, 1)) {
                    failed = true;
                    break;
                }
            }
            if (control_sync) {
                const auto current = browser_ai_control_state(*runtime);
                if (!current.empty() && current != previous_control_sync) {
                    if (!bridge.send(current)) {
                        failed = true;
                        break;
                    }
                    previous_control_sync = current;
                }
            }
            emit_runtime_failure();  // 后续桥发送可能提前退出；先领取已发生且锁存的事实。
            if (presignal_diagnostics && !use_webrtc) {
                const auto snapshot = udp_media.presignal_diagnostic();
                if (snapshot && snapshot->observation != last_presignal_observation) {
                    const auto fixed = browser_presignal_diagnostic(*snapshot);
                    if (!fixed.empty()) {
                        std::cout << "host_udp_presignal=" << fixed << '\n' << std::flush;
                        last_presignal_observation = snapshot->observation;
                    }
                }
            }
            emit_lifecycle();  // Failed/Idle提前退出前同样交付原最后事实，不能只在正常分支取证。
            if (progress) {
                progress->runtime_exited.increment();
                progress->host(HostStage::Receipts);
            }
            while (const auto receipt = runtime->take_client_tool_receipt()) {
                const auto fixed = browser_client_tool_submitted(*receipt);
                if (fixed.empty() || !bridge.send(fixed)) {
                    failed = done = true;
                    break;
                }
            }
            if (done)
                break;
            if (progress)
                progress->host(HostStage::ToolCommand);
            if (const auto command = client_display.take_command()) {
                if (!bridge.send(*command)) {
                    failed = true;
                    break;
                }
            }
            if (progress)
                progress->host(HostStage::TextResult);
            while (auto text_result = runtime->take_text_result()) {
                const auto message = std::string("{\"kind\":\"textStatus\",\"result\":\"") +
                                     text_result->result + "\"}";
                if (!bridge.send(message))
                    failed = done = true;
            }
            if (runtime->voice().interrupted_playbacks() != previous_interruptions) {
                previous_interruptions = runtime->voice().interrupted_playbacks();
                if (!bridge.send("{\"kind\":\"playbackInterrupted\",\"count\":" +
                                 std::to_string(previous_interruptions) + "}"))
                    failed = done = true;
            }
            if (action_driver.completed_count() != previous_actions) {
                previous_actions = action_driver.completed_count();
                const auto action = "{\"kind\":\"actionCompleted\",\"code\":" +
                                    std::to_string(action_driver.last_code()) +
                                    ",\"volume\":" + std::to_string(volume.volume()) + "}";
                if (!bridge.send(action))
                    failed = done = true;
            }
            const auto state = runtime->voice().state();
            if (runtime->state() == RuntimeState::Failed || state == VoiceState::Failed) {
                // 仅输出本端方法、状态码及能力403已关联的固定原因，不能打印云端原文或媒体凭据。
                if (const auto& rejection = runtime->voice().last_rejection()) {
                    std::cerr << "request_rejected method=" << rejection->method
                              << " code=" << rejection->code;
                    if (!rejection->reason.empty())
                        std::cerr << " reason=" << rejection->reason;
                    std::cerr << '\n';
                } else {
                    std::cerr << "runtime_failed state=" << static_cast<int>(runtime->state())
                              << " voice=" << static_cast<int>(state) << '\n';
                }
                failed = done = true;
            }
            if (progress)
                progress->host(HostStage::Content);
            while (auto content = runtime->take_content()) {
                std::string display;  // 只展示已验证正文，不转发工具参数或云端信封。
                const auto& method = content->envelope.method;
                if (method == "respSttDone")
                    // 原ASR唯一正文只附本条已验证内容的回复ID；不从播放/网页状态猜测、不重复发送text。
                    display =
                        browser_text("\n用户：" + content->text + "\n助手：", content->response_id);
                else if (method == "respTextDelta") {
                    if (!content->response_id.empty() && first_text_response != content->response_id) {
                        first_text_response = content->response_id;
                        first_text_at = clock;
                    }
                    display = browser_text(content->text);
                }
                else if (method == "respTextDone")
                    display = browser_text("\n");
                else if (method == "respEmotion")
                    display = browser_text(content->emoji);
                else if (method == "toolCallStart" || method == "toolCallResult")
                    display = browser_tool_status(*content);
                else if (method == "respDone" && content->status == "completed")
                    display = browser_completion(
                        content->response_id, content->audio_state,
                        use_webrtc || use_none ? AudioTiming{} : udp_media.timing());
                else if (method == "respDone" && content->status == "cancelled")
                    display = R"({"kind":"responseCancelled"})";
                const bool projected = !display.empty();  // 原宿主过滤与网页展示确认必须分开。
                const bool sent = projected && bridge.send(display);  // 原业务发送只调用一次。
                runtime->observe_content_display(*content, projected, sent);
                if (projected && !sent)
                    failed = done = true;
            }
            // 先收录原内容终态再投影，防止同轮respDone后又从核心最近ID复活取消目标。
            const std::string control_response = std::string(runtime->response_control_target());
            if (state != previous || control_response != previous_control_response) {
                if (state != previous)
                    std::cout << "voice_state=" << static_cast<int>(state) << '\n' << std::flush;
                previous = state;
                previous_control_response = control_response;
                // 本地配对桥统一投影核心控制事实，不改变云端信令或UDP音频包。
                const auto message =
                    state == VoiceState::Established
                        ? (use_none
                               ? R"({"kind":"state","established":true,"transport":"none","text":"文字会话已建立，可以发送文字。"})"
                           : runtime->voice().mode() == Mode::FullDuplex
                               ? R"({"kind":"state","established":true,"interactionMode":"fullDuplex","text":"实时全双工会话已建立，可以开始输入。"})"
                               : R"({"kind":"state","established":true,"interactionMode":"halfDuplex","text":"半双工会话已建立，可以开始输入。"})")
                        : (state == VoiceState::Recovering &&
                                   (use_webrtc || runtime->voice().control_recovering())
                               ? R"({"kind":"state","established":false,"recovering":true,"text":"媒体连接恢复中，可以停止输入、取消回复或结束会话。"})"
                               : R"({"kind":"state","established":false,"text":"正在协商或释放会话。"})");
                const std::string projected =
                    browser_response_control_state(message, control_response);
                if (projected.empty() || !bridge.send(projected))
                    failed = done = true;
            }
            if (progress)
                progress->host(HostStage::MessageDiagnostic);
            if (const auto diagnostic = runtime->take_message_diagnostics()) {
                const auto projection = browser_message_diagnostics(*diagnostic);
                if (!projection.empty()) {
                    if (progress)
                        progress->diagnostic_attempts.increment();
                    const bool sent =
                        bridge.send(projection);  // 原发送仍只有一次，诊断失败无业务副作用。
                    if (progress) {
                        if (sent)
                            progress->diagnostic_sent.increment();
                        else
                            progress->diagnostic_failed.increment();
                    }
                }
            }
            if (runtime->state() == RuntimeState::Idle) {
                // Idle只证明宿主已进入原正常退出分支，不冒认平台结束原因或工具已执行。
                const auto* diagnostic_switch = std::getenv("UR_DEVICE_MESSAGE_DIAGNOSTICS");
                if (diagnostic_switch && std::string_view(diagnostic_switch) == "1")
                    std::cout << "host_exit_branch=runtime_idle\n" << std::flush;
                done = true;
            }
        }
        if (progress)
            progress->host(HostStage::MediaCommands);
        while (!use_webrtc && !use_none) {
            auto command = pcm.take_command();
            if (!command)
                break;
            if (command->kind == BrowserAudioCommandKind::PlaybackStart)
                response = command->id;
            if (command->kind == BrowserAudioCommandKind::PlaybackStop) {
                // 先发送最终媒体计数、再发停播指令；浏览器观察到停播时已持有该回复的最终事实。
                // 领取停播代次的冻结统计；不能读取可能已被下一回复重置的当前计数。
                // 断连/关会话可能已释放媒体，此时不能把空统计冒充最终计数。
                if (response != command->id) {
                    failed = done = true;
                    break;
                }
                if (udp_media.ready()) {
                    const auto stopped = udp_media.take_stopped_timing(command->epoch);
                    if (!stopped || !bridge.send(browser_media_final(response, *stopped))) {
                        failed = done = true;
                        break;
                    }
                    // 只有原停止代次的冻结统计可以投影首音；页面时钟和模型终态都不能补造。
                    const auto measurement = browser_reply_timing(
                        response, *stopped, udp_media.last_non_silent_sent_ms(),
                        first_text_response == response ? first_text_at : std::nullopt);
                    if (measurement.empty() || !bridge.send(measurement)) {
                        failed = done = true;
                        break;
                    }
                }
                response.clear();
            }
            if (!bridge.send(browser_command(*command))) {
                failed = done = true;
                break;
            }
        }
        if (use_webrtc) {
            response = webrtc_media.current_response();
            while (auto command = webrtc_media.take_command()) {
                if (!bridge.send(*command)) {
                    failed = done = true;
                    break;
                }
            }
        }
        // 每轮最多发8帧，避免回复积压饿死控制与上行采集。
        if (progress)
            progress->host(HostStage::PlaybackFrames);
        for (unsigned count = 0; count < 8 && !done && !use_webrtc && !use_none; ++count) {
            auto frame = pcm.take_playback();
            if (!frame)
                break;
            if (!bridge.send(browser_frame(*frame)))
                failed = done = true;
        }
        if (progress)
            progress->host(HostStage::Sleep);
        if (!done)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (progress)
        progress->host(HostStage::Shutdown);
    client_display.close();  // 先退休本地票据，不把页面清理冒称原工具实际成功。
    emit_runtime_failure();  // 未被正常轮询消费的原首因最后交付；正常close不会制造故障。
    if (runtime)
        runtime->close(now());
    if (endpoint_hal)
        endpoint_hal->cancel(); // 只启动实际关闭，断桥不冒称HAL已释放。
    endpoint_source_live = false;
    emit_lifecycle();  // 真正finally关闭后才观察explicit；不在结束前预先推断退出原因。
    runtime.reset();   // 焦点端点与授权器销毁前释放全部借用，网页断开仍不伪造实际关闭事实。
    live_endpoint.reset();
    live_volume.close();  // 未actual释放仍不重建旧节目或伪造原租约Released。
    pcm.disconnect();
    bridge.send(failed ? R"({"kind":"error"})" : R"({"kind":"ended"})");
    bridge.stop();
    if (progress)
        progress->host(HostStage::Stopped);
    return failed ? 1 : 0;
}
}  // namespace
}  // namespace ur::device
// main只接受显式本地端口，不开放公网监听或在参数中接收密钥。
int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: device_browser <loopback-port>\n";
        return 2;
    }
    const auto port = ur::device::port_number(argv[1]);
    if (port < 1024)
        return 2;
    return ur::device::run(port);
}
