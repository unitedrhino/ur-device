// 本地JSON复用核心严格解析校验；不接受凭据字段或任意云端method透传。
#include "ur/device/json_parse.hpp"
#include "browser_wire.hpp"
#include <cJSON.h>
#include <charconv>
#include <cmath>
#include <memory>
#include "browser_input_ownership_wire.hpp"
#include "ur/device/envelope.hpp"

namespace ur::device {
namespace {
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;  // 解析树独占所有权。
// text拒绝缺失、空串和过长标识，不从其他类型隐式转换。
std::string text(const cJSON* object, const char* key, std::size_t max = 128) {
    const auto* value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsString(value) || !value->valuestring)
        return {};
    const std::string result(value->valuestring);
    return result.size() <= max ? result : std::string{};
}
// integer校验精确整数范围，拒绝小数、NaN和类型混淆。
bool integer(const cJSON* value, double minimum, double maximum) {
    return cJSON_IsNumber(value) && std::isfinite(value->valuedouble) &&
           std::floor(value->valuedouble) == value->valuedouble && value->valuedouble >= minimum &&
           value->valuedouble <= maximum;
}
// optional_response_id复核原ContentEvent标识边界；空值仅兼容旧展示消息，非空至多128字节且无ASCII控制字符。
bool optional_response_id(std::string_view value) {
    if (value.size() > 128)
        return false;
    for (const auto character : value)
        if (static_cast<unsigned char>(character) < 32 || character == 127)
            return false;
    return true;
}
// print将独立JSON对象转为文本，分配失败不返回部分报文。
std::string print(const Json& object) {
    if (!object)
        return {};
    char* raw = cJSON_PrintUnformatted(object.get());
    if (!raw)
        return {};
    std::string body(raw);
    cJSON_free(raw);
    return body;
}
}  // namespace
// observe_terminal仅锁存同Core会话/回复的已验终态，旧Done不能退休新模型目标。
void BrowserResponseControl::observe_terminal(const ContentEvent& event, const VoiceClient& voice) {
    if (event.envelope.method != "respDone" || event.envelope.session != voice.session_id() ||
        event.response_id.empty() || event.response_id != voice.response_id() ||
        (event.status != "completed" && event.status != "cancelled" && event.status != "failed"))
        return;
    terminal_session_ = event.envelope.session;
    terminal_response_ = event.response_id;
}
// target只读取原核心及宿主资格；暂断可取消，不提供新采集或输出许可。
std::string_view BrowserResponseControl::target(const VoiceClient& voice, RuntimeState runtime) const {
    if (runtime != RuntimeState::Running ||
        (voice.state() != VoiceState::Established && voice.state() != VoiceState::Recovering) ||
        voice.session_id().empty() || voice.response_id().empty() ||
        (voice.session_id() == terminal_session_ && voice.response_id() == terminal_response_))
        return {};
    return voice.response_id();
}
// cancel_allowed拒绝空/旧/异会话/终态目标；真正取消仍由原Core墓碑和请求栅栏决定。
bool BrowserResponseControl::cancel_allowed(std::string_view response_id, const VoiceClient& voice,
                                           RuntimeState runtime) const {
    return !response_id.empty() && response_id == target(voice, runtime);
}
// browser_response_control_state校验固定本地状态并转义回复标识，不改旧UDP/云端信封。
// 保留 state/response_id 控制投影接口；state 严格解析固定本地状态对象，response_id 独立校验回复标识。
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
std::string browser_response_control_state(std::string_view state, std::string_view response_id) {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    if (!optional_response_id(response_id))
        return {};
    std::string canonical;
    if (canonical_json_object(state, canonical) != DecodeResult::Ok)
        return {};
    Json object(ur::device::parse_json(canonical.c_str()), &cJSON_Delete);
    if (!object)
        return {};
    const auto* kind = cJSON_GetObjectItemCaseSensitive(object.get(), "kind");
    const auto* established = cJSON_GetObjectItemCaseSensitive(object.get(), "established");
    const auto* text_value = cJSON_GetObjectItemCaseSensitive(object.get(), "text");
    const auto* mode = cJSON_GetObjectItemCaseSensitive(object.get(), "interactionMode");
    const auto* recovering = cJSON_GetObjectItemCaseSensitive(object.get(), "recovering");
    const auto* transport = cJSON_GetObjectItemCaseSensitive(object.get(), "transport");
    const bool active = cJSON_IsTrue(established) != 0;  // 原建立/恢复只授控制，不授末端输出或采集。
    const bool text_only =
        cJSON_IsString(transport) && std::string_view(transport->valuestring) == "none";
    if (!cJSON_IsString(kind) || std::string_view(kind->valuestring) != "state" ||
        !cJSON_IsBool(established) || !cJSON_IsString(text_value) ||
        (transport && (!active || !text_only || mode || recovering)) ||
        (active ? (!text_only && (!cJSON_IsString(mode) ||
                                  (std::string_view(mode->valuestring) != "halfDuplex" &&
                                   std::string_view(mode->valuestring) != "fullDuplex"))) ||
                      recovering
                : mode || (recovering && !cJSON_IsBool(recovering))) ||
        cJSON_GetArraySize(object.get()) != (active || recovering ? 4 : 3) ||
        (!active && cJSON_IsTrue(recovering) == 0 && !response_id.empty()) ||
        !cJSON_AddStringToObject(object.get(), "responseId", std::string(response_id).c_str()))
        return {};
    const auto encoded = print(object);  // 同一原JSON库负责转义，之后再次核UTF8和重复键边界。
    return canonical_json_object(encoded, canonical) == DecodeResult::Ok ? canonical : "";
}
bool decode_browser_request(std::string_view body, BrowserRequest& result) {
    result = {};
    std::string canonical;
    if (canonical_json_object(body, canonical) != DecodeResult::Ok)
        return false;
    Json object(ur::device::parse_json(canonical.c_str()), &cJSON_Delete);
    if (!object)
        return false;
    BrowserRequest value;
    value.kind = text(object.get(), "kind", 32);
    const bool peer_fact = value.kind == "webrtcOffer" || value.kind == "webrtcConnected" ||
                           value.kind == "webrtcDisconnected" || value.kind == "webrtcDrained";
    if (peer_fact) {
        const auto revision = text(object.get(), "peerRevision", 20);
        if (revision.empty() || revision.front() == '0')
            return false;
        const auto parsed = std::from_chars(revision.data(), revision.data() + revision.size(),
                                            value.peer_revision);
        if (parsed.ec != std::errc{} || parsed.ptr != revision.data() + revision.size() ||
            !value.peer_revision)
            return false;
    }
    if (value.kind == "start" || value.kind == "stop" || value.kind == "end" ||
        value.kind == "textReady") {
        if (cJSON_GetArraySize(object.get()) != 1)
            return false;
    } else if (value.kind == "webrtcConnected" || value.kind == "webrtcDisconnected") {
        if (cJSON_GetArraySize(object.get()) != 2)
            return false;
    } else if (value.kind == "webrtcOffer") {
        value.sdp = text(object.get(), "sdp", 49152);
        if (value.sdp.size() < 8 || value.sdp.compare(0, 5, "v=0\r\n") != 0 ||
            cJSON_GetArraySize(object.get()) != 3)
            return false;
    } else if (value.kind == "sendText") {
        const auto* text_value = cJSON_GetObjectItemCaseSensitive(object.get(), "text");
        const auto* audio = cJSON_GetObjectItemCaseSensitive(object.get(), "audio");
        if (!cJSON_IsString(text_value) || !text_value->valuestring || !cJSON_IsBool(audio) ||
            cJSON_GetArraySize(object.get()) != 3)
            return false;
        value.text = text_value->valuestring;
        value.audio = cJSON_IsTrue(audio);
        if (value.text.empty() || value.text.size() > 16384 ||
            value.text.find_first_not_of(" \t\r\n") == std::string::npos ||
            value.text.find('\0') != std::string::npos)
            return false;
    } else if (value.kind == "cancel" || value.kind == "captured") {
        value.id = text(object.get(), "id");
        if (value.id.empty())
            return false;
        if (value.kind == "captured") {
            const auto* samples = cJSON_GetObjectItemCaseSensitive(object.get(), "samples");
            const auto members = cJSON_GetArraySize(object.get());
            if (!cJSON_IsArray(samples) || cJSON_GetArraySize(samples) < 1 ||
                cJSON_GetArraySize(samples) > 2880 || (members != 3 && members != 5))
                return false;
            if (members == 5) {
                value.capture_session = text(object.get(), "sessionId", 64);
                const auto generation = text(object.get(), "routeGeneration", 20);
                const auto parsed =
                    std::from_chars(generation.data(), generation.data() + generation.size(),
                                    value.capture_generation);
                if (generation.empty() || generation.front() == '0' || parsed.ec != std::errc{} ||
                    parsed.ptr != generation.data() + generation.size() ||
                    browser_input_ownership_command(
                        {value.capture_session, value.capture_generation, false}, 1)
                        .empty())
                    return false;
            }
            for (const auto* sample = samples->child; sample; sample = sample->next) {
                if (!integer(sample, -32768, 32767))
                    return false;
                value.pcm.push_back(static_cast<std::int16_t>(sample->valuedouble));
            }
        } else if (cJSON_GetArraySize(object.get()) != 2)
            return false;
    } else if (value.kind == "ready") {
        const auto* rate = cJSON_GetObjectItemCaseSensitive(object.get(), "sampleRate");
        const auto* duration = cJSON_GetObjectItemCaseSensitive(object.get(), "frameMs");
        const auto* aec = cJSON_GetObjectItemCaseSensitive(object.get(), "aec");
        if (!integer(rate, 16000, 48000) || !integer(duration, 20, 60) || !cJSON_IsBool(aec) ||
            cJSON_GetArraySize(object.get()) != 4)
            return false;
        value.format = {static_cast<int>(rate->valuedouble), 1,
                        static_cast<int>(duration->valuedouble)};
        if (!BufferedPcm(value.format).valid())
            return false;
        value.aec = cJSON_IsTrue(aec);
    } else if (value.kind == "webrtcVolumeApplied") {
        value.id = text(object.get(), "id");
        const auto* volume = cJSON_GetObjectItemCaseSensitive(object.get(), "volume");
        if (value.id.empty() || !integer(volume, 0, 100) || cJSON_GetArraySize(object.get()) != 3)
            return false;
        value.volume = static_cast<unsigned>(volume->valuedouble);
    } else if (value.kind == "outputGated") {
        const auto ticket = text(object.get(), "ticket", 20);
        const auto* enabled = cJSON_GetObjectItemCaseSensitive(object.get(), "enabled");
        if (ticket.empty() || ticket.front() == '0' || !cJSON_IsBool(enabled) ||
            cJSON_GetArraySize(object.get()) != 3)
            return false;
        const auto parsed =
            std::from_chars(ticket.data(), ticket.data() + ticket.size(), value.gate_ticket);
        if (parsed.ec != std::errc{} || parsed.ptr != ticket.data() + ticket.size() ||
            value.gate_ticket == 0)
            return false;
        value.output_enabled = cJSON_IsTrue(enabled);
    } else if (value.kind == "played" || value.kind == "webrtcDrained") {
        const auto epoch = text(object.get(), "epoch", 20);
        if (epoch.empty() || epoch.front() == '0')
            return false;
        const auto parsed = std::from_chars(epoch.data(), epoch.data() + epoch.size(), value.epoch);
        const auto* ticket = cJSON_GetObjectItemCaseSensitive(object.get(), "ticket");
        if (parsed.ec != std::errc{} || parsed.ptr != epoch.data() + epoch.size() ||
            value.epoch == 0 ||
            (value.kind == "played" &&
             (!integer(ticket, 1, 4294967295.0) || cJSON_GetArraySize(object.get()) != 3)) ||
            (value.kind == "webrtcDrained" && cJSON_GetArraySize(object.get()) != 3))
            return false;
        if (value.kind == "played")
            value.ticket = static_cast<std::uint32_t>(ticket->valuedouble);
    } else
        return false;
    result = std::move(value);
    return true;
}
std::string browser_command(const BrowserAudioCommand& command) {
    const bool capture = command.kind == BrowserAudioCommandKind::CaptureStart ||
                         command.kind == BrowserAudioCommandKind::CaptureStop;
    const bool scoped = !command.capture_session.empty() || command.capture_generation != 0;
    if (scoped && (!capture || browser_input_ownership_command(
                                   {command.capture_session, command.capture_generation, false}, 1)
                                   .empty()))
        return {};
    if (command.kind == BrowserAudioCommandKind::OutputGate) {
        if (command.gate_ticket == 0 || !command.id.empty() || command.epoch != 0)
            return {};
        Json object(cJSON_CreateObject(), &cJSON_Delete);
        if (!object || !cJSON_AddStringToObject(object.get(), "kind", "playbackGate") ||
            !cJSON_AddStringToObject(object.get(), "ticket",
                                     std::to_string(command.gate_ticket).c_str()) ||
            !cJSON_AddBoolToObject(object.get(), "enabled", command.output_enabled))
            return {};
        return print(object);
    }
    const char* kind = nullptr;  // 显式映射，不依赖枚举顺序与数组索引。
    switch (command.kind) {
        case BrowserAudioCommandKind::CaptureStart:
            kind = "captureStart";
            break;
        case BrowserAudioCommandKind::CaptureStop:
            kind = "captureStop";
            break;
        case BrowserAudioCommandKind::PlaybackStart:
            kind = "playbackStart";
            break;
        case BrowserAudioCommandKind::PlaybackStop:
            kind = "playbackStop";
            break;
        case BrowserAudioCommandKind::OutputGate:
            return {};  // 独立门控已在上方编码，不能借回复id或epoch路由。
    }
    if (!kind || command.id.empty() || command.id.size() > 128)
        return {};
    Json object(cJSON_CreateObject(), &cJSON_Delete);
    if (!object || !cJSON_AddStringToObject(object.get(), "kind", kind) ||
        !cJSON_AddStringToObject(object.get(), "id", command.id.c_str()) ||
        !cJSON_AddStringToObject(object.get(), "epoch", std::to_string(command.epoch).c_str()))
        return {};
    if (scoped &&
        (!cJSON_AddStringToObject(object.get(), "sessionId", command.capture_session.c_str()) ||
         !cJSON_AddStringToObject(object.get(), "routeGeneration",
                                  std::to_string(command.capture_generation).c_str())))
        return {};
    return print(object);
}
std::string browser_frame(const PlaybackFrame& frame) {
    if (frame.epoch == 0 || frame.ticket == 0 || frame.samples.empty() ||
        frame.samples.size() > 2880)
        return {};
    Json object(cJSON_CreateObject(), &cJSON_Delete);
    if (!object || !cJSON_AddStringToObject(object.get(), "kind", "playbackFrame") ||
        !cJSON_AddStringToObject(object.get(), "epoch", std::to_string(frame.epoch).c_str()) ||
        !cJSON_AddNumberToObject(object.get(), "ticket", frame.ticket))
        return {};
    auto* samples = cJSON_AddArrayToObject(object.get(), "samples");
    if (!samples)
        return {};
    for (const auto sample : frame.samples) {
        auto* number = cJSON_CreateNumber(sample);
        if (!number)
            return {};
        if (!cJSON_AddItemToArray(samples, number)) {
            cJSON_Delete(number);
            return {};
        }
    }
    return print(object);
}
// browser_text保持唯一正文；response_id为空不新增字段，非空只附设备已验证内容事件的原回复标识。
std::string browser_text(std::string_view text, std::string_view response_id) {
    if (text.size() > 16384 || text.find('\0') != std::string_view::npos ||
        !optional_response_id(response_id))
        return {};
    Json object(cJSON_CreateObject(), &cJSON_Delete);
    // cJSON独立复制正文及原回复标识，不持悬空string_view。
    const std::string owned(text);
    const std::string response(response_id);
    if (!object || !cJSON_AddStringToObject(object.get(), "kind", "text") ||
        !cJSON_AddStringToObject(object.get(), "text", owned.c_str()) ||
        (!response.empty() &&
         !cJSON_AddStringToObject(object.get(), "responseId", response.c_str())))
        return {};
    return print(object);
}
// browser_message_diagnostics
// 拒绝未知字段枚举与超界计数，原标识/正文根本没有输出槽。
std::string browser_message_diagnostics(const MessageDiagnostics& diagnostic) {
    constexpr std::uint64_t maximum = 9007199254740991ULL;  // JSON整数必须能被网页精确表示。
    if (diagnostic.size > diagnostic.records.size() || diagnostic.received > maximum ||
        diagnostic.taken > maximum || diagnostic.host_filtered > maximum ||
        diagnostic.host_sent > maximum || diagnostic.host_send_failed > maximum)
        return {};
    Json object(cJSON_CreateObject(), &cJSON_Delete);
    if (!object || !cJSON_AddStringToObject(object.get(), "kind", "messageDiagnostic") ||
        !cJSON_AddNumberToObject(object.get(), "received",
                                 static_cast<double>(diagnostic.received)) ||
        !cJSON_AddNumberToObject(object.get(), "taken", static_cast<double>(diagnostic.taken)) ||
        !cJSON_AddNumberToObject(object.get(), "hostFiltered",
                                 static_cast<double>(diagnostic.host_filtered)) ||
        !cJSON_AddNumberToObject(object.get(), "hostSent",
                                 static_cast<double>(diagnostic.host_sent)) ||
        !cJSON_AddNumberToObject(object.get(), "hostSendFailed",
                                 static_cast<double>(diagnostic.host_send_failed)) ||
        !cJSON_AddBoolToObject(object.get(), "overflow", diagnostic.overflow))
        return {};
    auto* records = cJSON_AddArrayToObject(object.get(), "records");  // 数组归原JSON树所有。
    if (!records)
        return {};
    for (std::size_t index = 0; index < diagnostic.size; ++index) {
        const auto& fact = diagnostic.records[index];
        const auto outcome = message_diagnostic_outcome(fact.outcome);
        if (outcome.empty() || fact.method != message_diagnostic_method(fact.method) ||
            fact.sequence > maximum || fact.watermark > maximum)
            return {};
        Json row(cJSON_CreateObject(),
                 &cJSON_Delete);  // 只有成功附着后才转移树所有权。
        if (!row || !cJSON_AddStringToObject(row.get(), "method", fact.method.c_str()) ||
            !cJSON_AddStringToObject(row.get(), "outcome", std::string(outcome).c_str()) ||
            !cJSON_AddNumberToObject(row.get(), "sequence", static_cast<double>(fact.sequence)) ||
            !cJSON_AddNumberToObject(row.get(), "watermark", static_cast<double>(fact.watermark)) ||
            !cJSON_AddBoolToObject(row.get(), "parsed", fact.parsed) ||
            !cJSON_AddBoolToObject(row.get(), "sessionMatches", fact.session_matches) ||
            !cJSON_AddBoolToObject(row.get(), "taken", fact.taken) ||
            !cJSON_AddBoolToObject(row.get(), "hostObserved", fact.host_observed) ||
            !cJSON_AddBoolToObject(row.get(), "projected", fact.projected) ||
            !cJSON_AddBoolToObject(row.get(), "sent", fact.sent))
            return {};
        auto* transferred = row.release();  // 数组成功接管前，失败路径必须重新取得所有权。
        if (!cJSON_AddItemToArray(records, transferred)) {
            row.reset(transferred);
            return {};
        }
    }
    return print(object);
}
// browser_runtime_failure_diagnostic
// 只编码同次原失败路径，未观察到下层首因用none，未知枚举拒绝。
std::string browser_runtime_failure_diagnostic(RuntimeFailure runtime,
                                               std::optional<UdpFailure> udp, PcmWriteFailure pcm) {
    const auto runtime_name =
        runtime_failure_name(runtime);  // 只接受核心固定枚举，不反射自由文本。
    const auto udp_name = udp ? udp_failure_name(*udp) : std::string_view("none");
    const auto pcm_name = pcm_write_failure_name(pcm);
    if (runtime_name.empty() || udp_name.empty() || pcm_name.empty() ||
        (udp && runtime != RuntimeFailure::MediaPoll) ||
        (pcm != PcmWriteFailure::None && (!udp || *udp != UdpFailure::PcmWrite)) ||
        (udp && *udp == UdpFailure::PcmWrite && pcm == PcmWriteFailure::None))
        return {};
    return std::string("{\"kind\":\"runtimeFailureDiagnostic\",\"runtime\":\"") +
           std::string(runtime_name) + "\",\"udp\":\"" + std::string(udp_name) + "\",\"pcm\":\"" +
           std::string(pcm_name) + "\",\"observation\":1}";
}
// browser_client_failure_diagnostic 与原runtime五字段分开，未知真实入口只映射闭集unknown。
std::string browser_client_failure_diagnostic(const ClientFailureDiagnostic& diagnostic) {
    static constexpr const char* causes[] = {
        "",
        "unknown",
        "sessionError",
        "requestRejected",
        "requestsTimeout",
        "activeScopeLost",
        "candidateClock",
        "candidateEstablishDeadline",
        "candidateRecoveryDeadline",
        "visionProfileInvalid",
        "mediaRecoveryDeadline",
        "negotiatingDeadline",
        "endpointPrepareDeadline",
        "endpointPrepareClock",
        "endpointPrepareDriver",
        "endpointPrepareDispatch"};  // 固定原因，不借服务端reason正文。
    static constexpr const char* methods[] = {
        "none",          "unknown",       "capabilityReport", "sessionCreate",
        "sessionAccept", "sessionReject", "mediaState",       "mediaOffer",
        "audioStart",    "audioStop",     "respCancel",       "sessionEnd",
        "inputSend",     "sessionSync",   "endpointControl"};  // 固定本端方法。
    const auto cause = static_cast<unsigned>(diagnostic.cause);
    const auto method = static_cast<unsigned>(diagnostic.method);
    const bool client = diagnostic.origin == ClientFailureOrigin::ClientFailed;
    const bool prepare = diagnostic.origin == ClientFailureOrigin::EndpointPrepareFailed;
    const bool request = diagnostic.cause == VoiceFailureCause::RequestRejected ||
                         diagnostic.cause == VoiceFailureCause::RequestsTimeout;
    if ((!client && !prepare) || !cause || cause >= std::size(causes) ||
        method >= std::size(methods) || (client && cause >= 12) ||
        (prepare && cause != 1 && cause < 12) ||
        (request ? diagnostic.method == VoiceFailureMethod::None
                 : diagnostic.method != VoiceFailureMethod::None))
        return {};
    return std::string("{\"kind\":\"clientFailureDiagnostic\",\"origin\":\"") +
           (client ? "clientFailed" : "endpointPrepareFailed") + "\",\"cause\":\"" + causes[cause] +
           "\",\"method\":\"" + methods[method] + "\",\"observation\":1}";
}
// browser_presignal_diagnostic保持已冻结关系，不把统计不完整当没有丢包。
std::string browser_presignal_diagnostic(const ReplyPreSignalDiagnostic& diagnostic) {
    if (diagnostic.observation == 0 || diagnostic.retained > 16 ||
        diagnostic.evicted > diagnostic.received ||
        (diagnostic.complete &&
         static_cast<std::uint64_t>(diagnostic.received) !=
             static_cast<std::uint64_t>(diagnostic.evicted) + diagnostic.retained))
        return {};
    return "{\"kind\":\"udpPresignalDiagnostic\",\"observation\":" +
           std::to_string(diagnostic.observation) +
           ",\"received\":" + std::to_string(diagnostic.received) +
           ",\"evicted\":" + std::to_string(diagnostic.evicted) +
           ",\"retained\":" + std::to_string(diagnostic.retained) +
           ",\"complete\":" + (diagnostic.complete ? "true}" : "false}");
}
// browser_scope_failure_diagnostic
// 固定五字段且只有合法层级/谓词组合，旧诊断报文保持不变。
std::string browser_scope_failure_diagnostic(ScopeFailureCode code) {
    static constexpr const char* layers[] = {
        "",
        "candidate",
        "candidate",
        "focusEndpoint",
        "focusEndpoint",
        "focusEndpoint",
        "focusEngine",
        "focusEngine",
        "focusEngine",
        "focusEngine",
        "focusEngine",
        "focusEngine",
        "focusEngine",
        "focusEngine",
        "authority",
        "candidateHAL",
        "candidateHAL",
        "authority",
        "authority",
        "authority",
        "duplexDriver",
        "duplexDriver",
        "duplexDriver",
        "duplexDriver"};  // 固定来源层，不序列化类名或其他动态信息。
    static constexpr const char* predicates[] = {
        "",
        "bindingMissing",
        "endpointMissing",
        "stateUnavailable",
        "clockInvalid",
        "sessionMissing",
        "engineClosed",
        "operationPending",
        "leaseMissing",
        "receiptUndelivered",
        "stateUnavailable",
        "resourcesMismatch",
        "clockInvalid",
        "leaseExpired",
        "permissionDenied",
        "stateUnavailable",
        "clockInvalid",
        "currentUnavailable",
        "permitInvalid",
        "operationDenied",
        "stateUnavailable",
        "outputUnconfirmed",
        "inputUnconfirmed",
        "otherPathUnconfirmed"};  // 仅在原检查点记录，不能从清理终态倒推。
    static_assert(std::size(layers) == std::size(predicates));
    const auto index = static_cast<unsigned>(code);
    if (!index || index >= std::size(layers))
        return {};
    return std::string(
               "{\"kind\":\"scopeFailureDiagnostic\",\"origin\":"
               "\"candidateGate\",\"layer\":\"") +
           layers[index] + "\",\"predicate\":\"" + predicates[index] + "\",\"observation\":1}";
}
// browser_lifecycle_diagnostic
// 只提供实际生命周期旁证，不承诺服务器资源或工具已经完成。
std::string browser_lifecycle_diagnostic(const VoiceLifecycleDiagnostic& diagnostic) {
    const char* accepted = nullptr;  // 枚举映射闭集，未知值不得当成none输出。
    switch (diagnostic.accepted_end) {
        case AcceptedVoiceEnd::None:
            accepted = "none";
            break;
        case AcceptedVoiceEnd::SessionEndAck:
            accepted = "session_end_ack";
            break;
        case AcceptedVoiceEnd::SessionEndedEvent:
            accepted = "session_ended_event";
            break;
    }
    const char* close = nullptr;  // 关闭原因是原入口，不是网络自报的reason正文。
    switch (diagnostic.close_cause) {
        case VoiceCloseCause::None:
            close = "none";
            break;
        case VoiceCloseCause::ExplicitClose:
            close = "explicit_close";
            break;
        case VoiceCloseCause::DrainSettled:
            close = "drain_settled";
            break;
        case VoiceCloseCause::Failure:
            close = "failure";
            break;
    }
    const unsigned facts = (diagnostic.local_end_applied ? 1U : 0U) +
                           (diagnostic.accepted_end != AcceptedVoiceEnd::None ? 1U : 0U) +
                           (diagnostic.close_cause != VoiceCloseCause::None ? 1U : 0U);
    if (!accepted || !close || diagnostic.observation != facts || facts > 3)
        return {};
    Json object(cJSON_CreateObject(), &cJSON_Delete);
    if (!object || !cJSON_AddStringToObject(object.get(), "kind", "lifecycleDiagnostic") ||
        !cJSON_AddBoolToObject(object.get(), "localEndApplied", diagnostic.local_end_applied) ||
        !cJSON_AddStringToObject(object.get(), "acceptedEnd", accepted) ||
        !cJSON_AddStringToObject(object.get(), "closeCause", close) ||
        !cJSON_AddNumberToObject(object.get(), "observation", diagnostic.observation))
        return {};
    return print(object);
}
// browser_tool_status只投影原工具展示事实及同ContentEvent回复标识，不采用网页/当前播放状态推断归属。
std::string browser_tool_status(const ContentEvent& event) {
    // 开始通知才携带工具名；结果只承载状态，避免把模型参数或MCP正文送到页面。
    const auto& call_id = event.tool_call_id;
    const auto& tool_name = event.tool_name;
    const auto state =
        event.envelope.method == "toolCallStart" ? std::string("started") : event.status;
    const bool started = state == "started";
    if ((event.envelope.method != "toolCallStart" && event.envelope.method != "toolCallResult") ||
        call_id.empty() || call_id.size() > 128 || call_id.find('\0') != std::string_view::npos ||
        !optional_response_id(event.response_id) ||
        (started && (tool_name.empty() || tool_name.size() > 128 ||
                     tool_name.find('\0') != std::string_view::npos)) ||
        (!started &&
         (!tool_name.empty() || (state != "succeeded" && state != "failed" && state != "unknown"))))
        return {};
    Json object(cJSON_CreateObject(), &cJSON_Delete);
    const std::string id(call_id), name(tool_name), status(state);
    if (!object || !cJSON_AddStringToObject(object.get(), "kind", "toolStatus") ||
        !cJSON_AddStringToObject(object.get(), "id", id.c_str()) ||
        !cJSON_AddStringToObject(object.get(), "state", status.c_str()) ||
        (started && !cJSON_AddStringToObject(object.get(), "name", name.c_str())) ||
        (!event.response_id.empty() &&
         !cJSON_AddStringToObject(object.get(), "responseId", event.response_id.c_str())))
        return {};
    return print(object);
}
std::string browser_completion(std::string_view response_id, std::string_view audio_state,
                               const AudioTiming& timing) {
    if (response_id.empty() || response_id.size() > 128 ||
        response_id.find('\0') != std::string_view::npos ||
        timing.network_packets > 9007199254740991ULL ||
        timing.decoded_frames > 9007199254740991ULL ||
        timing.non_silent_frames > 9007199254740991ULL ||
        (audio_state != "none" && audio_state != "sent" && audio_state != "cancelled" &&
         audio_state != "failed"))
        return {};
    Json object(cJSON_CreateObject(), &cJSON_Delete);
    const std::string owned(response_id), audio(audio_state);
    if (!object || !cJSON_AddStringToObject(object.get(), "kind", "responseCompleted") ||
        !cJSON_AddStringToObject(object.get(), "id", owned.c_str()) ||
        !cJSON_AddStringToObject(object.get(), "audioState", audio.c_str()) ||
        !cJSON_AddNumberToObject(object.get(), "networkPackets",
                                 static_cast<double>(timing.network_packets)) ||
        !cJSON_AddNumberToObject(object.get(), "decodedFrames",
                                 static_cast<double>(timing.decoded_frames)) ||
        !cJSON_AddNumberToObject(object.get(), "nonSilentFrames",
                                 static_cast<double>(timing.non_silent_frames)))
        return {};
    return print(object);
}
std::string browser_media_final(std::string_view response_id, const AudioTiming& timing) {
    if (response_id.empty() || response_id.size() > 128 ||
        response_id.find('\0') != std::string_view::npos ||
        timing.network_packets > 9007199254740991ULL ||
        timing.missing_packets > 9007199254740991ULL ||
        timing.decoded_frames > timing.network_packets ||
        timing.non_silent_frames > timing.decoded_frames ||
        (timing.observed_complete &&
         (timing.missing_packets != 0 || timing.decoded_frames != timing.network_packets)))
        return {};
    Json object(cJSON_CreateObject(), &cJSON_Delete);
    const std::string owned(response_id);
    if (!object || !cJSON_AddStringToObject(object.get(), "kind", "responseMediaFinal") ||
        !cJSON_AddStringToObject(object.get(), "id", owned.c_str()) ||
        !cJSON_AddNumberToObject(object.get(), "networkPackets",
                                 static_cast<double>(timing.network_packets)) ||
        !cJSON_AddNumberToObject(object.get(), "decodedFrames",
                                 static_cast<double>(timing.decoded_frames)) ||
        !cJSON_AddNumberToObject(object.get(), "nonSilentFrames",
                                 static_cast<double>(timing.non_silent_frames)) ||
        !cJSON_AddNumberToObject(object.get(), "missingPackets",
                                 static_cast<double>(timing.missing_packets)) ||
        !cJSON_AddBoolToObject(object.get(), "observedComplete", timing.observed_complete))
        return {};
    return print(object);
}
// browser_reply_timing保存有界原始时刻；输入尚未结束前已有回复属于合法重叠，不伪造负延迟。
std::string browser_reply_timing(std::string_view response_id, const AudioTiming& timing,
                                 std::optional<std::uint64_t> last_voice_sent_ms,
                                 std::optional<std::uint64_t> first_text_ms) {
    constexpr std::uint64_t maximum = 9007199254740991ULL;  // 本地JS能够精确读取的原整数上界。
    if (response_id.empty() || !optional_response_id(response_id)) return {};
    for (const auto value :
         {last_voice_sent_ms, first_text_ms, timing.first_network_ms, timing.first_playable_ms})
        if (value && *value > maximum) return {};
    if (timing.first_playable_ms &&
        (!timing.first_network_ms || *timing.first_playable_ms < *timing.first_network_ms))
        return {};
    Json object(cJSON_CreateObject(), &cJSON_Delete);
    const std::string owned(response_id);
    // 纯十进制Raw避免cJSON格式化把safe整数近似；缺失不被0冒充，任何分配失败返回空。
    const auto add = [&](const char* name, std::optional<std::uint64_t> value) {
        return value ? cJSON_AddRawToObject(object.get(), name, std::to_string(*value).c_str())
                     : cJSON_AddNullToObject(object.get(), name);
    };
    if (!object || !cJSON_AddStringToObject(object.get(), "kind", "replyTiming") ||
        !cJSON_AddStringToObject(object.get(), "responseId", owned.c_str()) ||
        !add("lastVoiceSentMs", last_voice_sent_ms) || !add("firstTextMs", first_text_ms) ||
        !add("firstNetworkMs", timing.first_network_ms) ||
        !add("firstPlayableMs", timing.first_playable_ms))
        return {};
    const auto body = print(object);
    std::string canonical;  // 仅复用原严格UTF8/JSON校验，不能用其重编码近似原safe整数。
    return canonical_json_object(body, canonical) == DecodeResult::Ok ? body : std::string{};
}
}  // namespace ur::device
