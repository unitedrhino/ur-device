// 文件说明：纯语音桥保留原Peer作用域、实际SDP与输出票据；不操作共享采集源。
#include "ur/device/json_parse.hpp"
#include "browser_human_voice_media.hpp"
#include <cJSON.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <memory>
#include "ur/device/envelope.hpp"
namespace ur::device {
namespace {
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;  // 独占本地严格JSON树。
constexpr std::uint64_t safe = 9007199254740991ULL;            // 跨语言代次安全上限。
// text 仅取原字符串字段，不将数字或null转成身份。
std::string text(const cJSON* value, const char* key) {
    const auto* item = cJSON_GetObjectItemCaseSensitive(value, key);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
}
// number 只接受规范非零十进制字符串，票据本身可完整表达uint64。
bool number(std::string_view value, std::uint64_t& out, std::uint64_t max) {
    if (value.empty() || value.front() == '0')
        return false;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), out);
    return result.ec == std::errc{} && result.ptr == value.data() + value.size() && out &&
           out <= max;
}
// profile 此适配只有固定Opus48k单声道20ms，没有视频或其他参数fallback。
bool profile(const CallVoiceProfile& p) {
    return p.sample_rate == 48000 && p.channels == 1 && p.frame_duration_ms == 20;
}
// sdp 只准单audio媒体段；实际opus的RTP映射/2是RFC约定，不代表采集源双声道。
bool voice_sdp(std::string_view value) {
    if (value.size() > 60000 || value.substr(0, 3) != "v=0" || value.find('\0') != value.npos)
        return false;
    const auto first = value.find("\nm=audio ");
    return first != value.npos && value.find("\nm=", first + 1) == value.npos &&
           value.find("opus/48000/2") != value.npos;
}
// print 返回紧凑JSON或空值，分配失败不发布残缺命令。
std::string print(cJSON* object) {
    char* raw = cJSON_PrintUnformatted(object);
    if (!raw)
        return {};
    std::string out(raw);
    cJSON_free(raw);
    return out;
}
}  // namespace
BrowserHumanVoiceMedia::BrowserHumanVoiceMedia(Source source, std::uint64_t boot)
    : source_(std::move(source)), boot_generation_(boot <= safe ? boot : 0) {}
bool BrowserHumanVoiceMedia::live() const {
    try {
        return source_ && source_();
    } catch (...) {
        return false;
    }
}
bool BrowserHumanVoiceMedia::voice_supported() const { return live(); }
bool BrowserHumanVoiceMedia::available() const { return live() && !started_ && !closing_; }
bool BrowserHumanVoiceMedia::bind_scope(std::string_view session, std::uint64_t generation,
                                        std::uint64_t epoch) {
    if (!live() || !session_.empty() || closing_ || session.empty() || session.size() > 64 ||
        !generation || generation > safe || !epoch || epoch > safe ||
        (boot_generation_ && (!gate_confirmed_ || gate_enabled_ || gate_pending_)))
        return false;
    for (const unsigned char c : session)
        if (c < 32 || c == 127)
            return false;
    session_ = session;
    generation_ = generation;
    epoch_ = epoch;
    // 用公共严格UTF8/JSON校验器拒绝非法字节，不产生无法与JS关联的标识。
    std::string canonical;
    if (canonical_json_object(command("humanVoiceStop"), canonical) != DecodeResult::Ok) {
        session_.clear();
        generation_ = epoch_ = 0;
        return false;
    }
    if (boot_generation_ && !enqueue("humanVoiceOutputBind", command("humanVoiceOutputBind")))
        return false;
    gate_confirmed_ = false;  // boot只证启动静音，正式原scope必须取得新false票据。
    return true;
}
// 保留既有调用签名兼容原宿主；命令种类与SDP各自编码，原媒体关联不变。
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
std::string BrowserHumanVoiceMedia::command(std::string_view kind, std::string_view sdp) const {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    Json json(cJSON_CreateObject(), &cJSON_Delete);
    if (kind == "humanVoiceOutputInit") {
        if (!json || !cJSON_AddStringToObject(json.get(), "kind", "humanVoiceOutputInit") ||
            !cJSON_AddStringToObject(json.get(), "bootGeneration",
                                     std::to_string(boot_generation_).c_str()) ||
            !cJSON_AddStringToObject(json.get(), "ticket", std::to_string(ticket_).c_str()) ||
            !cJSON_AddBoolToObject(json.get(), "enabled", false))
            return {};
        return print(json.get());
    }
    if (!json || !cJSON_AddStringToObject(json.get(), "kind", std::string(kind).c_str()) ||
        !cJSON_AddStringToObject(json.get(), "sessionId", session_.c_str()) ||
        !cJSON_AddStringToObject(json.get(), "routeGeneration",
                                 std::to_string(generation_).c_str()) ||
        !cJSON_AddStringToObject(json.get(), "peerEpoch", std::to_string(epoch_).c_str()))
        return {};
    if (kind == "humanVoiceStart") {
        if (!cJSON_AddNumberToObject(json.get(), "sampleRate", 48000) ||
            !cJSON_AddNumberToObject(json.get(), "channels", 1) ||
            !cJSON_AddNumberToObject(json.get(), "frameMs", 20))
            return {};
    } else if (kind == "humanVoiceAnswer") {
        if (!cJSON_AddStringToObject(json.get(), "sdp", std::string(sdp).c_str()))
            return {};
    } else if (kind == "humanVoiceOutputGate") {
        if (!cJSON_AddStringToObject(json.get(), "ticket", std::to_string(ticket_).c_str()) ||
            !cJSON_AddBoolToObject(json.get(), "enabled", gate_enabled_))
            return {};
    }
    return print(json.get());
}
bool BrowserHumanVoiceMedia::enqueue(std::string kind, std::string body) {
    if (body.empty() || body.size() > 65536 || commands_.size() >= 8)
        return false;
    commands_.push_back({std::move(kind), std::move(body)});
    return true;
}
bool BrowserHumanVoiceMedia::prepare_voice(const CallVoiceProfile& p) {
    if (!available() || session_.empty() || !profile(p))
        return false;
    prepared_ = true;
    return true;
}
bool BrowserHumanVoiceMedia::begin_voice_candidate(const CallVoiceProfile& p) {
    if (!prepared_ || !available() || !profile(p) ||
        !enqueue("humanVoiceStart", command("humanVoiceStart")))
        return false;
    started_ = true;
    return true;
}
std::optional<std::string> BrowserHumanVoiceMedia::take_command() {
    if (commands_.empty())
        return std::nullopt;
    auto value = std::move(commands_.front());
    commands_.pop_front();
    if (value.kind == "humanVoiceStart")
        start_sent_ = true;
    else if (value.kind == "humanVoiceAnswer")
        answer_sent_ = true;
    else if (value.kind == "humanVoiceOutputGate" || value.kind == "humanVoiceOutputInit")
        gate_sent_ = true;
    else if (value.kind == "humanVoiceStop")
        stop_sent_ = true;
    return value.body;
}
std::optional<std::string> BrowserHumanVoiceMedia::take_offer() {
    if (offer_.empty() || offer_taken_)
        return std::nullopt;
    offer_taken_ = true;
    return offer_;
}
bool BrowserHumanVoiceMedia::apply_answer(std::string_view sdp) {
    if (!live() || closing_ || offer_.empty() || answer_queued_ || !voice_sdp(sdp) ||
        !enqueue("humanVoiceAnswer", command("humanVoiceAnswer", sdp)))
        return false;
    answer_queued_ = true;
    return true;
}
bool BrowserHumanVoiceMedia::connected() const { return live() && connected_ && !closing_; }
std::optional<std::uint64_t> BrowserHumanVoiceMedia::request_output_gate(bool enabled,
                                                                         std::uint64_t now) {
    if (!poll(now) || (!started_ && !boot_generation_) || gate_pending_ ||
        (enabled && !connected()) || ticket_ == UINT64_MAX || now > UINT64_MAX - 2000)
        return std::nullopt;
    ++ticket_;
    gate_enabled_ = enabled;
    const std::string kind = session_.empty() ? "humanVoiceOutputInit" : "humanVoiceOutputGate";
    if (!enqueue(kind, command(kind))) {
        --ticket_;
        return std::nullopt;
    }
    gate_pending_ = true;
    gate_sent_ = false;
    gate_confirmed_ = false;
    gate_deadline_ = now + 2000;
    return ticket_;
}
bool BrowserHumanVoiceMedia::output_gate_confirmed(std::uint64_t ticket, bool enabled) const {
    if (!live())
        return false;
    // 退休原腿只保留此前已确认false且原Peer实际Stopped的原票据，未知停止不能补造确认。
    if (closing_)
        return stopped_ && !enabled && retired_false_ticket_ && ticket == *retired_false_ticket_;
    return ticket && ticket == ticket_ && enabled == gate_enabled_ &&
           gate_confirmed_ && !gate_pending_;
}
bool BrowserHumanVoiceMedia::poll(std::uint64_t now) {
    // 保持原clock/source/deadline短路顺序和每次源读取数；只在原停止分支追加首因。
    if (clock_seen_ && now < clock_) {
        latch_stop_reason(StopReason::ClockRollback);
        close();
    } else if (!live()) {
        latch_stop_reason(StopReason::SourceUnavailable);
        close();
    } else if (gate_pending_ && now >= gate_deadline_) {
        latch_stop_reason(StopReason::GateDeadline);
        close();
    }
    clock_seen_ = true;
    clock_ = now;
    return !closing_;
}
void BrowserHumanVoiceMedia::close() {
    if (closing_)
        return;
    latch_stop_reason(StopReason::ExplicitClose);
    if (gate_confirmed_ && !gate_pending_ && !gate_enabled_ && ticket_)
        retired_false_ticket_ = ticket_;
    closing_ = true;
    connected_ = false;
    gate_pending_ = gate_confirmed_ = false;
    commands_.clear();
    if (started_ || !session_.empty())
        (void)enqueue("humanVoiceStop", command("humanVoiceStop"));
}
void BrowserHumanVoiceMedia::latch_stop_reason(StopReason reason) {
    if (stop_reason_ == StopReason::Unknown)
        stop_reason_ = reason;
}
std::string_view BrowserHumanVoiceMedia::failure_reason() const {
    switch (stop_reason_) {
        case StopReason::ClockRollback:
            return "clockRollback";
        case StopReason::SourceUnavailable:
            return "sourceUnavailable";
        case StopReason::GateDeadline:
            return "gateDeadline";
        case StopReason::RemoteStopped:
            return "remoteStopped";
        case StopReason::ExplicitClose:
            return "explicitClose";
        case StopReason::Unknown:
            return "unknown";
    }
    return "unknown";
}
bool BrowserHumanVoiceMedia::stopped() const { return stopped_; }
std::string_view BrowserHumanVoiceMedia::stop_cause() const { return stop_cause_; }
std::optional<bool> BrowserHumanVoiceMedia::receive_stop_diagnostic(std::string_view body,
                                                                    bool enabled) {
    // 自主浏览器发送器固定kind首键；正文中的同名文字不是候选，普通帧不新增JSON解析。
    constexpr std::string_view prefix = R"({"kind":"humanVoiceStopDiagnostic")";
    if (body.substr(0, prefix.size()) != prefix)
        return std::nullopt;
    if (!enabled)
        return false;  // 默认关闭时连旁证候选也不解析，不读取业务源或推进时钟。
    std::string canonical;
    if (body.size() > 1024 || canonical_json_object(body, canonical) != DecodeResult::Ok)
        return false;
    Json json(ur::device::parse_json(canonical.c_str()), &cJSON_Delete);
    if (!json || text(json.get(), "kind") != "humanVoiceStopDiagnostic")
        return std::nullopt;
    std::uint64_t generation = 0, epoch = 0;  // 只核原bind_scope的两个十进制代次，不推新身份。
    const auto* known = cJSON_GetObjectItemCaseSensitive(json.get(), "known");
    const auto cause = text(json.get(), "cause");
    constexpr std::array<std::string_view, 10> causes{
        // 沿原JS close固定分类，未知不是无故障。
        "processorError",     "startOutputUnavailable", "startException",     "answerException",
        "remoteTrackInvalid", "remoteTrackSetupFailed", "remoteSinkRejected", "peerFailed",
        "peerDisconnected",   "explicitClose"};
    if (stopped_ || session_.empty() || cJSON_GetArraySize(json.get()) != 6 ||
        text(json.get(), "sessionId") != session_ ||
        !number(text(json.get(), "routeGeneration"), generation, safe) ||
        generation != generation_ || !number(text(json.get(), "peerEpoch"), epoch, safe) ||
        epoch != epoch_ || !cJSON_IsBool(known) ||
        (cJSON_IsTrue(known) ? std::find(causes.begin(), causes.end(), cause) == causes.end()
                             : cause != "unknown"))
        return false;
    if (stop_diagnostic_seen_)
        return cause == stop_cause_;  // 重放不再留存；冲突不能用新的分类覆盖原未知或首因。
    stop_diagnostic_seen_ = true;
    stop_cause_ = cause;
    return true;
}
bool BrowserHumanVoiceMedia::receive(std::string_view body, std::uint64_t now) {
    (void)poll(now);
    std::string canonical;
    if (body.size() > 65536 || canonical_json_object(body, canonical) != DecodeResult::Ok)
        return false;
    Json json(ur::device::parse_json(canonical.c_str()), &cJSON_Delete);
    if (json && text(json.get(), "kind") == "humanVoiceOutputIdle") {
        std::uint64_t boot = 0, ticket = 0;
        const auto* enabled = cJSON_GetObjectItemCaseSensitive(json.get(), "enabled");
        const auto* frames = cJSON_GetObjectItemCaseSensitive(json.get(), "frames");
        if (closing_ || !session_.empty() || !gate_pending_ || !gate_sent_ || gate_enabled_ ||
            cJSON_GetArraySize(json.get()) != 5 ||
            !number(text(json.get(), "bootGeneration"), boot, safe) || boot != boot_generation_ ||
            !number(text(json.get(), "ticket"), ticket, UINT64_MAX) || ticket != ticket_ ||
            !cJSON_IsFalse(enabled) || !cJSON_IsNumber(frames) || frames->valuedouble < 1 ||
            frames->valuedouble > 4294967295.0 ||
            std::floor(frames->valuedouble) != frames->valuedouble)
            return false;
        gate_pending_ = false;
        gate_confirmed_ = true;
        return true;
    }
    std::uint64_t generation = 0, epoch = 0;
    if (!json || text(json.get(), "sessionId") != session_ ||
        !number(text(json.get(), "routeGeneration"), generation, safe) ||
        generation != generation_ || !number(text(json.get(), "peerEpoch"), epoch, safe) ||
        epoch != epoch_)
        return false;
    const auto kind = text(json.get(), "kind");
    const auto count = cJSON_GetArraySize(json.get());
    if (kind == "humanVoiceStopped") {
        // 原Peer/output也可能因真实浏览器错误自行停止；合法原scope终态不能留下假connected。
        if (count != 4 || (!start_sent_ && !(closing_ && stop_sent_) && !boot_generation_))
            return false;
        latch_stop_reason(StopReason::RemoteStopped);
        close();
        stopped_ = true;
        return true;
    }
    if (closing_ || !live() ||
        (!start_sent_ && !(boot_generation_ && kind == "humanVoiceOutputGated")))
        return false;
    if (kind == "humanVoiceOffer") {
        const auto value = text(json.get(), "sdp");
        if (count != 5 || !voice_sdp(value) || (!offer_.empty() && offer_ != value))
            return false;
        offer_ = value;
        return true;
    }
    if (kind == "humanVoiceAnswered") {
        if (count != 4 || !answer_sent_)
            return false;
        answered_ = true;
        return true;
    }
    if (kind == "humanVoiceConnected") {
        if (count != 4 || !answered_)
            return false;
        connected_ = true;
        return true;
    }
    if (kind == "humanVoiceOutputGated") {
        const auto* enabled = cJSON_GetObjectItemCaseSensitive(json.get(), "enabled");
        const auto* frames = cJSON_GetObjectItemCaseSensitive(json.get(), "frames");
        const auto* non = cJSON_GetObjectItemCaseSensitive(json.get(), "nonSilentFrames");
        std::uint64_t ticket = 0;
        if (count != 8 || !gate_pending_ || !gate_sent_ || !cJSON_IsBool(enabled) ||
            cJSON_IsTrue(enabled) != gate_enabled_ ||
            !number(text(json.get(), "ticket"), ticket, UINT64_MAX) || ticket != ticket_ ||
            !cJSON_IsNumber(frames) || !cJSON_IsNumber(non) || frames->valuedouble < 1 ||
            frames->valuedouble > safe || std::floor(frames->valuedouble) != frames->valuedouble ||
            non->valuedouble < 0 || non->valuedouble > frames->valuedouble ||
            std::floor(non->valuedouble) != non->valuedouble)
            return false;
        gate_pending_ = false;
        gate_confirmed_ = true;
        return true;
    }
    return false;
}
}  // namespace ur::device
