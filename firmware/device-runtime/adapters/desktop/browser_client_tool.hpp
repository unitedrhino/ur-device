// 文件说明：审核主题工具的实际网页末端；本地票据与平台执行令牌分离，不给网页平台身份或参数。
#pragma once
#include "ur/device/client_tool.hpp"

namespace ur::device {
struct ClientToolReceipt;  // 原端点只读ACK类型，不把结果正文传给网页。
// browser_client_tool_submitted序列化固定原ACK观察，非法事实返回空串，不新增平台线协议method。
std::string browser_client_tool_submitted(const ClientToolReceipt& receipt);
// BrowserClientToolDisplay
// 只在宿主事件线程操作；命令实际发出后才能收录原网页票据。
class BrowserClientToolDisplay final : public ClientToolDisplayPort {
public:
    // ready
    // 仅原已认证页面的明确客户端主题驱动预检；音频/节目ready不自动授予此能力。
    void ready(bool actual);
    // receive_ready只收录配对页一次规范预检；额外字段、未知修订或已退休页不能扩大能力。
    bool receive_ready(std::string_view body);
    bool request_theme(std::string_view theme, std::uint64_t generation) override;
    std::optional<ClientToolFact> take_theme_fact(std::uint64_t generation) override;
    // retire_theme只清匹配原代次的待发命令/事实/占位；旧取消不能撤销下一轮票据。
    void retire_theme(std::uint64_t generation) override;
    // take_command
    // 只含本地generation及固定theme，平台session/resp/token永不进入网页。
    std::optional<std::string> take_command();
    // receive 严格核原已发出票据的实际状态，返回false不制造结果或续期。
    bool receive(std::string_view body);
    // close 撤销连接并将原未确认末端置unknown；不假称已回滚可见主题。
    void close();
    // available 只报告当前页面实际端口已预检，不能代替平台原scope授权。
    bool available() const override { return ready_ && !closed_; }

private:
    std::optional<std::string> command_;       // 本地一次待发命令，不能覆盖原票据。
    std::optional<ClientToolFact> fact_;       // 原实际观察最多一条。
    std::optional<std::uint64_t> generation_;  // 当前原驱动票据。
    std::string theme_;                        // 原闭合参数，不读取可变的当前工具。
    bool ready_ = false, closed_ = false;      // 明确预检和连接退休，不由构造猜就绪。
};
}  // namespace ur::device
