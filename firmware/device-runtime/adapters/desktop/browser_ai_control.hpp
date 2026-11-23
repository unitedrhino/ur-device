// 文件说明：配对本机桥的显式AI控制重连入口，不能携带网络凭据、替换会话或改变平台授权。
#pragma once
#include "ur/device/voice_runtime.hpp"

namespace ur::device {
// BrowserAIControlRequest 区分无关消息和严格控制请求；Invalid不能降级为普通语音命令。
enum class BrowserAIControlRequest { Other, Disconnect, Resume, Invalid };
// decode_browser_ai_control_request 仅接单字段固定kind，未知附加字段、重复键和过长报文拒绝。
BrowserAIControlRequest decode_browser_ai_control_request(std::string_view body);
// browser_ai_control_state 输出固定本机事实，不输出设备身份、会话ID、凭据或供应商正文。
std::string browser_ai_control_state(const VoiceRuntime& runtime);
}  // namespace ur::device
