// 文件说明：视频通话本地桥报文与云端信封分离，界面不能提交任意平台命令。
#pragma once
#include <string>
#include <string_view>

namespace ur::device {
// CallBrowserRequest 是经严格校验的单条本地操作或媒体事实。
struct CallBrowserRequest {
    std::string kind;          // ready/接听/拒接/SDP/连通/挂断/本地轨控制。
    std::string sdp;           // 仅callOffer携带完整本地SDP。
    std::string target;        // 仅callDial/transfer携带设备目标，不自授目标权限。
    std::string track_id;      // 仅callTrack携带固定会话轨ID。
    std::string state;         // 仅callTrack携带驱动实际active/paused状态。
    std::string operation;     // 固定cancel/hold/resume/transfer/cancelTransfer；撤候选仅本地枚举。
    std::string control_body;  // 严格固定字段的局部实际回执，媒体适配另核原票据身份。
};
// decode_call_browser 失败清空目标，拒绝额外字段和非固定类型。
bool decode_call_browser(std::string_view body, CallBrowserRequest& out);
}  // namespace ur::device
