// 文件媒体本地桥：可信源绑定→浏览器实际加载/呈现→关联事实；不授予麦克风或摄像头权限。
#include "ur/device/json_parse.hpp"
#include "browser_file_media.hpp"
#include <cJSON.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <memory>
#include "browser_bridge.hpp"

namespace ur::device {
namespace {
// Json 为所有配置/报文错误路径释放解析树。
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
// text 读取固定有界UTF-8标识；公共JSON入口另校验重复键和编码。
bool text(const cJSON* object, const char* key, std::string& value, std::size_t limit = 128) {
    const auto* item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsString(item))
        return false;
    value = item->valuestring;
    return !value.empty() && value.size() <= limit &&
           std::none_of(value.begin(), value.end(),
                        [](unsigned char ch) { return ch < 32 || ch == 127; });
}
// number 只接受0起安全整数，不能让浮点近似或负值当真实媒体计数。
bool number(const cJSON* object, const char* key, std::uint64_t& value) {
    const auto* item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble) || item->valuedouble < 0 ||
        item->valuedouble > 9007199254740991.0 ||
        std::floor(item->valuedouble) != item->valuedouble)
        return false;
    value = static_cast<std::uint64_t>(item->valuedouble);
    return true;
}
// epoch 本地桥代次用十进制字符串精确表达uint64；0仅用于尚未开始的关闭。
bool epoch(const cJSON* object, std::uint64_t& value) {
    std::string raw;
    if (!text(object, "generation", raw, 20) || (raw.size() > 1 && raw.front() == '0'))
        return false;
    const auto parsed = std::from_chars(raw.data(), raw.data() + raw.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == raw.data() + raw.size();
}
// source 固定授权引用不直接解释为URL；测试相对源只接受四个固定资产路径。
bool source(const BrowserFileBinding& binding) {
    if (binding.url.empty() || binding.url.size() > 4096 ||
        std::any_of(binding.url.begin(), binding.url.end(), [](unsigned char ch) {
            return ch <= 32 || ch >= 127 || ch == '\\' || ch == '#';
        }))
        return false;
    if (binding.origin.empty())
        return (binding.mime == "audio/wav" && binding.url == "/media/file.wav") ||
               (binding.mime == "audio/mpeg" && binding.url == "/media/file.mp3") ||
               (binding.mime == "video/mp4" && binding.url == "/media/file.mp4") ||
               (binding.mime == "video/webm" && binding.url == "/media/file.webm");
    return valid_browser_media_origin(binding.origin) &&
           binding.url.rfind(binding.origin + "/", 0) == 0;
}
// print 仅编码小数与有限小整数；内部generation始终字符串，不近似大整数。
std::string print(const Json& object) {
    if (!object)
        return {};
    std::unique_ptr<char, decltype(&cJSON_free)> encoded(cJSON_PrintUnformatted(object.get()),
                                                         cJSON_free);
    return encoded ? std::string(encoded.get()) : std::string{};
}
}  // namespace

bool valid_browser_file_binding(const BrowserFileBinding& binding) {
    const bool video = binding.mime == "video/mp4" || binding.mime == "video/webm";
    const auto valid_identifier = [](const std::string& value, std::size_t limit) {
        std::string canonical;
        return !value.empty() && value.size() <= limit &&
               std::none_of(value.begin(), value.end(),
                            [](unsigned char ch) { return ch < 32 || ch == 127; }) &&
               canonical_json_object("{\"id\":\"" + value + "\"}", canonical) == DecodeResult::Ok &&
               value.find_first_of("\"\\") == std::string::npos;
    };  // 固定绑定也必须满足标识契约，不能用嵌入NUL截断或注入本地命令。
    return valid_identifier(binding.session, 128) && valid_identifier(binding.playback_id, 128) &&
           valid_identifier(binding.content_ref, 512) && binding.expires_at != 0 &&
           binding.max_bytes != 0 && binding.max_bytes <= 67108864 &&
           (video ||
            (binding.audio && (binding.mime == "audio/wav" || binding.mime == "audio/mpeg"))) &&
           source(binding);
}

BrowserFileMedia::BrowserFileMedia(BrowserFileBinding binding, BrowserFilePermission permission)
    : binding_(std::move(binding)), permission_(std::move(permission)) {
    video_ = binding_.mime == "video/mp4" || binding_.mime == "video/webm";
    failed_ = !permission_ || !valid_browser_file_binding(binding_);
    if (!failed_)
        enqueue(command("fileBind", 0));
}
BrowserFileMedia::~BrowserFileMedia() { stop_all(); }
bool BrowserFileMedia::enqueue(std::string body) {
    if (body.empty() || body.size() > 8192 || !commands_.push_back(std::move(body))) {
        failed_ = true;
        return false;
    }
    return true;
}
std::string BrowserFileMedia::command(const char* kind, std::uint64_t generation,
                                      const PlaybackCommand* playback) const {
    // load 的有效期必须来自本实例已观察的单调时钟，缺失时不生成浏览器命令。
    if (playback && playback->operation == "load" && !clock_.has_value())
        return {};
    Json object(cJSON_CreateObject(), cJSON_Delete);
    if (!object || !cJSON_AddStringToObject(object.get(), "kind", kind) ||
        !cJSON_AddStringToObject(object.get(), "sessionId", binding_.session.c_str()) ||
        !cJSON_AddStringToObject(object.get(), "playbackId", binding_.playback_id.c_str()))
        return {};
    if (std::string_view(kind) == "fileBind") {
        if (!cJSON_AddStringToObject(object.get(), "mime", binding_.mime.c_str()) ||
            !cJSON_AddBoolToObject(object.get(), "audio", binding_.audio) ||
            !cJSON_AddStringToObject(object.get(), "origin", binding_.origin.c_str()) ||
            !cJSON_AddNumberToObject(object.get(), "maxBytes", binding_.max_bytes))
            return {};
    } else if (!cJSON_AddStringToObject(object.get(), "generation",
                                        std::to_string(generation).c_str())) {
        return {};
    }
    if (playback) {
        if (!cJSON_AddStringToObject(object.get(), "operation", playback->operation.c_str()))
            return {};
        if (playback->operation == "load" &&
            (!clock_.has_value() ||
             !cJSON_AddStringToObject(object.get(), "url", binding_.url.c_str()) ||
             !cJSON_AddNumberToObject(object.get(), "validForMs",
                                      static_cast<double>(std::min<std::uint64_t>(
                                          10000, binding_.expires_at - *clock_)))))
            return {};
        if (playback->position_ms &&
            !cJSON_AddRawToObject(object.get(), "positionMs",
                                  std::to_string(*playback->position_ms).c_str()))
            return {};
        if (playback->rate && !cJSON_AddNumberToObject(object.get(), "rate", *playback->rate))
            return {};
    }
    return print(object);
}
bool BrowserFileMedia::tick(std::uint64_t now) {
    if ((clock_ && now < *clock_) || now >= binding_.expires_at ||
        (gate_pending_ && now >= gate_deadline_))
        failed_ = true;
    clock_ = now;
    if (failed_)
        stop_all();
    return !failed_ && !closing_;
}
// request_output_gate 与媒体操作互斥；打开不能绕过原内容/资源许可，关闭不授予权限。
std::optional<std::uint64_t> BrowserFileMedia::request_output_gate(bool enabled,
                                                                   std::uint64_t now) {
    if (!tick(now) || !ready() || pending_ || gate_pending_ ||
        (enabled && !preparation_permitted()) ||
        gate_ticket_ == std::numeric_limits<std::uint64_t>::max() ||
        now > std::numeric_limits<std::uint64_t>::max() - 2000)
        return {};
    Json object(cJSON_CreateObject(), cJSON_Delete);
    const auto ticket = gate_ticket_ + 1;
    if (!object || !cJSON_AddStringToObject(object.get(), "kind", "fileOutputGate") ||
        !cJSON_AddStringToObject(object.get(), "sessionId", binding_.session.c_str()) ||
        !cJSON_AddStringToObject(object.get(), "playbackId", binding_.playback_id.c_str()) ||
        !cJSON_AddStringToObject(object.get(), "gateTicket", std::to_string(ticket).c_str()) ||
        !cJSON_AddBoolToObject(object.get(), "enabled", enabled) || !enqueue(print(object)))
        return {};
    gate_ticket_ = ticket;
    gate_confirmed_ = 0;
    gate_deadline_ = now + 2000;
    gate_enabled_ = enabled;
    gate_pending_ = true;
    gate_sent_ = false;
    return ticket;
}
// output_gate_confirmed 为原路径门控事实，不表示全局释放、节目暂停或用户意图改变。
bool BrowserFileMedia::output_gate_confirmed(std::uint64_t ticket, bool enabled) const {
    return ready() && ticket != 0 && ticket == gate_ticket_ && ticket == gate_confirmed_ &&
           enabled == gate_enabled_ && !gate_pending_;
}
bool BrowserFileMedia::output_permitted() const { return ready() && preparation_permitted(); }
// preparation_permitted 不开启媒体；真实输出仍必须同时满足ready与原资源许可。
bool BrowserFileMedia::preparation_permitted() const {
    return !failed_ && !closing_ && clock_ && *clock_ < binding_.expires_at &&
           permission_(binding_.audio, video_, *clock_);
}
bool BrowserFileMedia::begin(const PlaybackCommand& playback, std::uint64_t generation) {
    if (!ready() || !clock_ || *clock_ >= binding_.expires_at || pending_ || gate_pending_ ||
        (!gate_enabled_ && playback.operation != "load" && playback.operation != "pause" &&
         playback.operation != "stop") ||
        generation == 0 || generation <= generation_ ||
        playback.envelope.session != binding_.session ||
        playback.playback_id != binding_.playback_id || playback.media_revision)
        return false;
    if (playback.operation != "load" && playback.operation != "play" &&
        playback.operation != "pause" && playback.operation != "stop" &&
        playback.operation != "seek" && playback.operation != "setRate")
        return false;
    if ((playback.operation == "seek" && !playback.position_ms) ||
        (playback.operation == "setRate" &&
         (!playback.rate || (*playback.rate != 0.5 && *playback.rate != 1 &&
                             *playback.rate != 1.5 && *playback.rate != 2))))
        return false;
    const bool denied = playback.operation == "load"
                            ? playback.content_ref != binding_.content_ref
                            : !loaded_ || (playback.operation == "play" && !output_permitted());
    if (denied) {
        PlaybackFact fact;  // 拒绝是已检查的授权/驱动状态事实，不触发浏览器下载或输出。
        fact.generation = generation;
        fact.code = playback.operation == "load" ? 403 : 409;
        if (!facts_.push_back(std::move(fact))) {
            failed_ = true;
            stop_all();
            return false;
        }
    } else if (!enqueue(command("fileControl", generation, &playback))) {
        stop_all();
        return false;
    }
    generation_ = generation;
    operation_ = playback.operation;
    pending_ = true;
    completion_queued_ = denied;
    return true;
}
std::optional<PlaybackFact> BrowserFileMedia::take_fact() {
    if (facts_.empty())
        return std::nullopt;
    auto fact = std::move(*facts_.front());
    facts_.pop_front();
    if (!fact.spontaneous && fact.generation == generation_) {
        pending_ = false;
        completion_queued_ = false;
    }
    return fact;
}
bool BrowserFileMedia::stop_all() {
    if (!closing_) {
        closing_ = true;
        pending_ = false;
        completion_queued_ = false;
        commands_.clear();  // 尚未发送的load/play不得在关闭后启动媒体。
        facts_.clear();
        enqueue(command("fileClose", generation_));
    }
    return released_;
}
std::optional<std::string> BrowserFileMedia::take_command() {
    if (commands_.empty())
        return std::nullopt;
    auto body = std::move(*commands_.front());
    commands_.pop_front();
    if (gate_pending_ && body.rfind("{\"kind\":\"fileOutputGate\",", 0) == 0)
        gate_sent_ = true;
    return body;
}
bool BrowserFileMedia::receive(std::string_view body) {
    std::string canonical;
    if (canonical_json_object(body, canonical) != DecodeResult::Ok)
        return false;
    Json object(ur::device::parse_json(canonical.c_str()), cJSON_Delete);
    std::string kind, session, playback_id;
    if (!object || !text(object.get(), "kind", kind, 32) ||
        !text(object.get(), "sessionId", session) ||
        !text(object.get(), "playbackId", playback_id) || session != binding_.session ||
        playback_id != binding_.playback_id)
        return false;
    if (kind == "fileReady") {
        const auto* success = cJSON_GetObjectItemCaseSensitive(object.get(), "success");
        if (cJSON_GetArraySize(object.get()) != 4 || !cJSON_IsBool(success))
            return false;
        if (closing_)
            return true;  // 迟到ready不能重新开启旧播放器。
        if (!cJSON_IsTrue(success))
            return false;
        ready_ = true;
        return true;
    }
    if (kind == "fileOutputGated") {
        const auto* ticket = cJSON_GetObjectItemCaseSensitive(object.get(), "gateTicket");
        const auto* enabled = cJSON_GetObjectItemCaseSensitive(object.get(), "enabled");
        std::uint64_t value = 0;
        if (cJSON_GetArraySize(object.get()) != 5 || !cJSON_IsString(ticket) ||
            !cJSON_IsBool(enabled))
            return false;
        const std::string raw = ticket->valuestring;
        const auto parsed = std::from_chars(raw.data(), raw.data() + raw.size(), value);
        if (raw.empty() || raw.front() == '0' || parsed.ec != std::errc{} ||
            parsed.ptr != raw.data() + raw.size() || value == 0 || value > gate_ticket_)
            return false;
        if (closing_ || failed_ || value < gate_ticket_)
            return true;
        if (!gate_sent_ || cJSON_IsTrue(enabled) != gate_enabled_)
            return false;
        gate_confirmed_ = value;
        gate_pending_ = false;
        return true;
    }
    std::uint64_t generation = 0;
    if (!epoch(object.get(), generation))
        return false;
    if (kind == "fileClosed") {
        if (cJSON_GetArraySize(object.get()) != 4)
            return false;
        if (closing_ && generation == generation_)
            released_ = true;
        return true;
    }
    if (kind != "fileFact")
        return false;
    const auto* spontaneous = cJSON_GetObjectItemCaseSensitive(object.get(), "spontaneous");
    std::uint64_t code = 0;
    if (!cJSON_IsBool(spontaneous) || !number(object.get(), "code", code) || generation == 0 ||
        (code != 200 && (code < 400 || code > 599)))
        return false;
    PlaybackFact fact;  // 只保留真实数据，任何原始URL/异常正文均不进入共享核心。
    fact.generation = generation;
    fact.spontaneous = cJSON_IsTrue(spontaneous);
    fact.code = static_cast<int>(code);
    if (code != 200) {
        if (fact.spontaneous || cJSON_GetArraySize(object.get()) != 6)
            return false;
    } else {
        if (!text(object.get(), "state", fact.state, 32))
            return false;
        if (fact.state == "failed") {
            if (!fact.spontaneous || cJSON_GetArraySize(object.get()) != 7)
                return false;
        } else {
            std::uint64_t duration = 0, audio_samples = 0, video_frames = 0;
            const auto* rate = cJSON_GetObjectItemCaseSensitive(object.get(), "rate");
            if (cJSON_GetArraySize(object.get()) != 12 ||
                !number(object.get(), "positionMs", fact.position_ms) ||
                !number(object.get(), "durationMs", duration) ||
                !number(object.get(), "audioSamples", audio_samples) ||
                !number(object.get(), "videoFrames", video_frames) || duration < fact.position_ms ||
                !cJSON_IsNumber(rate) ||
                (rate->valuedouble != 0.5 && rate->valuedouble != 1 && rate->valuedouble != 1.5 &&
                 rate->valuedouble != 2) ||
                (fact.state != "loaded" && fact.state != "playing" && fact.state != "paused" &&
                 fact.state != "seeking" && fact.state != "ended" && fact.state != "stopped") ||
                (fact.state == "playing" &&
                 ((binding_.audio && audio_samples < 256) || (video_ && video_frames == 0))))
                return false;
            fact.duration_ms = duration;
            fact.rate = rate->valuedouble;
        }
    }
    if (closing_ || generation != generation_ ||
        (!fact.spontaneous && (!pending_ || completion_queued_)))
        return true;  // 旧/重复/关闭后事实不产生新完成，仍必须通过严格语法。
    if (fact.state == "loaded" && operation_ == "load")
        loaded_ = true;
    if (!facts_.push_back(std::move(fact))) {
        failed_ = true;
        stop_all();
        return false;
    }
    if (!cJSON_IsTrue(spontaneous))
        completion_queued_ = true;
    return true;
}
}  // namespace ur::device
