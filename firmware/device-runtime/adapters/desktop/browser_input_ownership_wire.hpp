// 文件说明：本地输入所有权票据严格编解码；不承载设备凭据，不据JSON授予资源。
#pragma once
#include <string_view>
#include "ur/device/input_ownership_gate.hpp"

namespace ur::device {
// BrowserInputOwnershipEvent区分独立无session探针和原会话实际门控事实。
struct BrowserInputOwnershipEvent {
    bool idle = false, enabled = false, sender = false;  // sender区分原Sender与Worklet实际事实。
    std::uint64_t boot_generation = 0, ticket = 0;
    InputGateRequest scope;  // 实际门控必须原session/路由代次，探针禁止带这些字段。
};
// decode_browser_input_ownership拒重复键、额外字段、不安全代次及非uint64票据，失败清空。
bool decode_browser_input_ownership(std::string_view body, BrowserInputOwnershipEvent& out);
// browser_input_ownership_command编码原已受理票据；不能把编码成功解释为线程已应用。
std::string browser_input_ownership_command(const InputGateRequest& request, std::uint64_t ticket);
}  // namespace ur::device
