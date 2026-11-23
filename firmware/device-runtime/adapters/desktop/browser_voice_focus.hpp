// 文件说明：桌面可信宿主将当前已建立AI会话绑定到唯一实际speaker路径；不授权文件或显示输出。
#pragma once
#include <functional>
#include "ur/device/output_focus_driver.hpp"

namespace ur::device {
// BrowserVoiceScope 仅由持设备身份的宿主读取已验证会话；网页/消息正文不得选择授权会话。
using BrowserVoiceScope = std::function<std::optional<std::string>()>;
// BrowserVoiceFocusAuthority 仅支持原AI会话conversation/speaker，首次acquire绑定一次。
// 固定宿主截止及已鉴权Topic由调用者提供；不提供其他业务、默认许可或跨实例重绑定。
class BrowserVoiceFocusAuthority final : public FocusAuthority {
public:
    BrowserVoiceFocusAuthority(OutputFocusDriver& driver, std::uint64_t deadline,
                               BrowserVoiceScope scope);
    // authorize 复核当前已建立原会话、完整资源及独立期限；首次身份绑定不启用输出。
    std::optional<FocusPermit> authorize(const FocusCommand& command, std::uint64_t now,
                                         std::uint64_t wall) override;
    // valid 原会话结束/切换或独立期限失效即撤权，不刷新许可或重新绑定路径。
    bool valid(const FocusPermit& permit, std::uint64_t now, std::uint64_t wall) const override;

private:
    // eligible 同步只读核验原实际业务范围，不在设备事件线程访问网络或申请新授权。
    bool eligible(const FocusCommand& command, std::uint64_t now, std::uint64_t wall) const;
    OutputFocusDriver& driver_;    // 固定唯一实际输出驱动，借用寿命长于授权器。
    std::uint64_t deadline_;       // 独立进程期限，不取平台控制期限作为宿主无限续租。
    BrowserVoiceScope scope_;      // 原C++状态机事实，空/非建立状态均拒绝。
    std::string session_, focus_;  // 首次原会话/租约身份，终态不能复用。
};
}  // namespace ur::device
