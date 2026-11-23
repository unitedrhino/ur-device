// 文件说明：本机控制恢复请求解析及固定事实投影；真正重新签名和网络调用由唯一native宿主负责。
#include "browser_ai_control.hpp"
#include <cJSON.h>
#include <memory>
#include "ur/device/json_parse.hpp"

namespace ur::device {
// decode_browser_ai_control_request 不接受URL、密码、sessionId、等待时长等网页自报控制参数。
BrowserAIControlRequest decode_browser_ai_control_request(std::string_view body) {
    if (body.size() > 65536)
        return BrowserAIControlRequest::Invalid;
    std::string canonical;
    if (canonical_json_object(body, canonical) != DecodeResult::Ok)
        return BrowserAIControlRequest::Invalid;
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(parse_json(canonical.c_str()),
                                                         cJSON_Delete);
    const auto* kind = cJSON_GetObjectItemCaseSensitive(root.get(), "kind");
    if (!cJSON_IsString(kind))
        return BrowserAIControlRequest::Other;
    const std::string_view value = kind->valuestring;
    if (value != "controlDisconnect" && value != "controlReconnect")
        return BrowserAIControlRequest::Other;
    if (body.size() > 256 || cJSON_GetArraySize(root.get()) != 1)
        return BrowserAIControlRequest::Invalid;
    return value == "controlDisconnect" ? BrowserAIControlRequest::Disconnect
                                        : BrowserAIControlRequest::Resume;
}

// browser_ai_control_state
// 只反映核心现有实态；connecting不代表恢复完成，resumed需公开代次实际已推进。
std::string browser_ai_control_state(const VoiceRuntime& runtime) {
    const auto epoch = runtime.voice().control_endpoint_epoch();
    if (epoch == 0)
        return {};
    const char* state = runtime.state() == RuntimeState::ControlRecovering ? "disconnected"
                        : runtime.state() == RuntimeState::Connecting ||
                                runtime.state() == RuntimeState::Subscribing
                            ? "connecting"
                        : runtime.voice().control_recovering() ? "synchronizing"
                        : runtime.state() == RuntimeState::Running &&
                                runtime.voice().state() == VoiceState::Established
                            ? "ready"
                            : "inactive";
    return std::string("{\"kind\":\"controlRecovery\",\"state\":\"") + state +
           "\",\"endpointEpoch\":" + std::to_string(epoch) +
           ",\"messageWatermark\":" + std::to_string(runtime.voice().control_message_watermark()) +
           ",\"originalSessionPreserved\":" +
           (runtime.voice().control_original_session_preserved() ? "true}" : "false}");
}
}  // namespace ur::device
