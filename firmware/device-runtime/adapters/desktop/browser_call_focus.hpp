// 文件说明：独立人际呼叫宿主的完整输出许可；不借AI焦点或声称跨进程系统仲裁。
#pragma once
#include <functional>
#include "ur/device/output_focus_driver.hpp"

namespace ur::device {
// BrowserCallFocusScope 来自C++原腿和实际Peer代次，页面回报不能自授恢复许可。
struct BrowserCallFocusScope {
    std::string session;                 // 原IM端腿，不用callGroup替代。
    std::uint64_t peer_epoch = 0;        // 原实际Peer代次，重建不继承旧许可。
    bool established = false;            // 原核心ACK/建立事件已经齐全。
    std::string control_token;           // 核心已验证的原控制，不是本地门控票据。
    std::uint64_t control_revision = 0;  // 原call组版本，独立于输出CAS版本。
    bool explicit_restore = false;       // 原合法resume完成事件，不能由held=false推导。
};
// BrowserCallScope 只读可信宿主原核心事实，不访问平台或从浏览器JSON恢复授权。
using BrowserCallScope = std::function<std::optional<BrowserCallFocusScope>()>;

// BrowserCallFocusAuthority 将原已建立humanCall绑定至唯一speaker/display路径。
class BrowserCallFocusAuthority final : public FocusAuthority {
public:
    // deadline 是原宿主独立单调截止；重复restore不能续期或更换Peer。
    BrowserCallFocusAuthority(OutputFocusDriver& driver, std::uint64_t deadline,
                              BrowserCallScope scope);
    std::optional<FocusPermit> authorize(const FocusCommand& command, std::uint64_t now,
                                         std::uint64_t wall) override;
    bool valid(const FocusPermit& permit, std::uint64_t now, std::uint64_t wall) const override;

private:
    // eligible 持续核原业务资格与绝对截止；creation_only仅签发时检查TTL不超过宿主剩余。
    // 双时钟各自取整，持续比较剩余长度会错误撤销仍在两份原绝对期限内的许可。
    bool eligible(const FocusCommand& command, std::uint64_t now, std::uint64_t wall,
                  bool creation_only) const;
    OutputFocusDriver& driver_;     // 仅本宿主唯一实际仲裁器，绝非OS全局焦点。
    std::uint64_t deadline_;        // 原固定独立期限，不从新请求延长。
    BrowserCallScope scope_;        // C++业务与Peer事实，空作用域不能打开输出。
    std::string session_, focus_;   // 最近原租约；新腿须先实际release。
    std::uint64_t peer_epoch_ = 0;  // 最近绑定的原Peer，不继承另一实际端点。
    // issued_ 有界保存唯一最近成功签发的完整许可；失败签发不覆盖，调用者副本不可续期或换意图。
    std::optional<FocusPermit> issued_;
};
}  // namespace ur::device
