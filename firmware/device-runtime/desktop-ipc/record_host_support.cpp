// 文件说明：桌面档案宿主配置／固定能力／只读状态；不实施用户授权或媒体状态机。
#include "ur/device/json_parse.hpp"
#include "record_host_support.hpp"
#include <cJSON.h>
#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>

namespace ur::device {
namespace {
// Json 所有临时树在本线程释放，不保存私有原配置。
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
// get 只读显式注入值；缺失不从平台报文补充。
std::string get(const std::map<std::string, std::string>& values, const char* key) {
    const auto item = values.find(key);
    return item == values.end() ? std::string{} : item->second;
}
// integer 拒绝前导零、符号和尾缀，不以缺省值开启档案读取。
std::uint64_t integer(std::string_view value, std::uint64_t maximum) {
    std::uint64_t result = 0;
    if (value.empty() || value.front() == '0')
        return 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() &&
                   result <= maximum
               ? result
               : 0;
}
// identifier 与原wire有限标识一致，配置不接受控制字符。
bool identifier(std::string_view value, std::size_t limit) {
    return !value.empty() && value.size() <= limit &&
           std::none_of(value.begin(), value.end(),
                        [](unsigned char ch) { return ch < 32 || ch == 127; });
}
// print 不泄露编码错误或原字段，只返回固定格式结构。
std::string print(const Json& object) {
    std::unique_ptr<char, decltype(&cJSON_free)> raw(cJSON_PrintUnformatted(object.get()),
                                                     cJSON_free);
    return raw ? raw.get() : std::string{};
}
}  // namespace

std::optional<RecordHostConfiguration> record_host_configuration(
    const std::map<std::string, std::string>& values) {
    if (get(values, "UR_RECORD_ENABLED") != "1")
        return {};
    RecordHostConfiguration config;
    config.asset.camera_id = get(values, "UR_RECORD_CAMERA_ID");
    config.asset.recording_id = get(values, "UR_RECORD_ID");
    config.asset.recording_version = get(values, "UR_RECORD_VERSION");
    config.asset.content_ref = get(values, "UR_RECORD_CONTENT_REF");
    config.asset.route = "/record-assets/archive.mp4";  // 固定路由，不接UI指定路径。
    config.asset.duration_ms = integer(get(values, "UR_RECORD_DURATION_MS"), 9007199254740991ULL);
    config.asset.max_bytes = integer(get(values, "UR_RECORD_MAX_BYTES"), 8388608);
    const auto audio = get(values, "UR_RECORD_AUDIO");
    config.asset.audio = audio == "1";
    config.file = get(values, "UR_RECORD_FILE");
    config.lease_ms = integer(get(values, "UR_RECORD_LEASE_MS"), 600000);
    if (!identifier(config.asset.camera_id, 64) || !identifier(config.asset.recording_id, 64) ||
        !identifier(config.asset.recording_version, 64) ||
        !identifier(config.asset.content_ref, 512) || !config.asset.duration_ms ||
        !config.asset.max_bytes || (audio != "0" && audio != "1") ||
        !identifier(config.file, 4096) || !std::filesystem::path(config.file).is_absolute() ||
        config.lease_ms < 1000)
        return {};
    return config;
}

bool read_record_archive(const RecordHostConfiguration& config, std::string& data) {
    data.clear();
    if (config.asset.max_bytes == 0 || config.asset.max_bytes > 8388608 ||
        !std::filesystem::path(config.file).is_absolute())
        return false;
    std::ifstream file(config.file, std::ios::binary | std::ios::ate);
    if (!file)
        return false;
    const auto size = file.tellg();
    if (size <= 0 || static_cast<std::uint64_t>(size) > config.asset.max_bytes)
        return false;
    data.resize(static_cast<std::size_t>(size));
    file.seekg(0);
    if (!file.read(data.data(), static_cast<std::streamsize>(data.size()))) {
        data.clear();
        return false;
    }
    // 同一受控文件读取期间增长亦拒绝，不因初次tellg绕过字节预算。
    if (file.peek() != std::char_traits<char>::eof()) {
        data.clear();
        return false;
    }
    return true;
}

WireResult record_host_capability(std::string token, const BrowserRecordAsset& asset,
                                  Envelope& out) {
    out = {};
    if (!identifier(asset.camera_id, 64))
        return WireResult::Invalid;
    Json object(
        ur::device::parse_json(
            asset.audio
                ? R"({"revision":1,"purposes":["recordPlayback"],"transports":["webrtc"],"supportedInteractionModes":[],"aecMode":"","features":["video","audio"],"audio":{"format":"opus","sampleRate":48000,"channels":1,"frameDuration":20},"cameras":[{"cameraId":"","codecs":["H264"],"maxPublishers":1}]})"
                : R"({"revision":1,"purposes":["recordPlayback"],"transports":["webrtc"],"supportedInteractionModes":[],"aecMode":"","features":["video"],"audio":{"format":"","sampleRate":0,"channels":0,"frameDuration":0},"cameras":[{"cameraId":"","codecs":["H264"],"maxPublishers":1}]})"),
        cJSON_Delete);
    if (!object)
        return WireResult::Invalid;
    auto* camera = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(object.get(), "cameras"), 0);
    if (!cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(camera, "cameraId"),
                              asset.camera_id.c_str()))
        return WireResult::Invalid;
    out.token = std::move(token);
    out.method = "capabilityReport";
    out.payload = print(object);
    std::string body;
    return encode_envelope(out, body) == DecodeResult::Ok ? WireResult::Ok : WireResult::Invalid;
}

bool record_host_local_request(std::string_view body, std::string& kind) {
    kind.clear();
    std::string canonical;
    if (canonical_json_object(body, canonical) != DecodeResult::Ok)
        return false;
    Json object(ur::device::parse_json(canonical.c_str()), cJSON_Delete);
    const auto* field = object ? cJSON_GetObjectItemCaseSensitive(object.get(), "kind") : nullptr;
    if (!field || !cJSON_IsString(field) || cJSON_GetArraySize(object.get()) != 1)
        return false;
    kind = field->valuestring;
    if (kind != "ready" && kind != "shutdown") {
        kind.clear();
        return false;
    }
    return true;
}

std::string record_host_projection(const RecordPlaybackClient& client) {
    const auto phase = client.phase();
    const char* name = phase == RecordPlaybackPhase::Prepared      ? "prepared"
                       : phase == RecordPlaybackPhase::Accepting   ? "accepting"
                       : phase == RecordPlaybackPhase::Rejecting   ? "rejecting"
                       : phase == RecordPlaybackPhase::Ready       ? "ready"
                       : phase == RecordPlaybackPhase::Negotiating ? "negotiating"
                       : phase == RecordPlaybackPhase::Active      ? "active"
                       : phase == RecordPlaybackPhase::Seeking     ? "seeking"
                                                                   : "ended";
    Json object(cJSON_CreateObject(), cJSON_Delete);
    if (!object || !cJSON_AddStringToObject(object.get(), "kind", "recordSessionState") ||
        !cJSON_AddStringToObject(object.get(), "phase", name) ||
        !cJSON_AddStringToObject(object.get(), "state", client.state().c_str()) ||
        !cJSON_AddRawToObject(object.get(), "revision",
                              std::to_string(client.revision()).c_str()) ||
        !cJSON_AddRawToObject(object.get(), "mediaRevision",
                              std::to_string(client.media_revision()).c_str()) ||
        !cJSON_AddBoolToObject(object.get(), "released", client.released()) ||
        !cJSON_AddBoolToObject(object.get(), "releaseSubmitted", client.release_submitted()))
        return {};
    return print(object);
}
}  // namespace ur::device
