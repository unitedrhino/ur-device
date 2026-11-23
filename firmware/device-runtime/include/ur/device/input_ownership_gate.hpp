// 文件说明：唯一采集源的实际路由门控契约；初始化探针不带会话、不授予采集许可。
#pragma once
#include <cstdint>
#include <optional>
#include <string>

namespace ur::device {
// InputGateRequest
// 由可信宿主绑定原会话及路由代次，不能从当前页面或网络正文猜测身份。
struct InputGateRequest {
    std::string session;                 // 已创建的原会话，不能为空。
    std::uint64_t route_generation = 0;  // 唯一源的原路由代次，零无效且不能跨腿复用。
    bool enabled = false;                // 开启原路由或围栏其所有迟到采集，不关闭其他合法路由。
};
// InputOwnershipGatePort
// 不申请第二路物理源；宿主必须把实际采集线程/原Sender完成关联到票据。
class InputOwnershipGatePort {
public:
    virtual ~InputOwnershipGatePort() = default;
    // initial_idle_observation
    // 仅实际全部输入路由关闭、没有候选Sender时返回原boot配对代次。
    // 默认未知；不允许构造、能力声明、空session命令或只读UI指标代替实际探针。
    virtual std::optional<std::uint64_t> initial_idle_observation() const { return {}; }
    // request_input_gate
    // 非阻塞接纳；返回单调非零票据，空值保证本次未执行任何路由动作。
    // disabled须阻断该原路由及其缓冲旧帧，enabled仅在上层独立许可下使用同一物理源。
    virtual std::optional<std::uint64_t> request_input_gate(const InputGateRequest& request,
                                                            std::uint64_t now) = 0;
    // input_gate_confirmed
    // 只核当前原票据/会话/代次/目标的实际线程事实；断连或替换后必须false。
    virtual bool input_gate_confirmed(std::uint64_t ticket,
                                      const InputGateRequest& request) const = 0;
};
}  // namespace ur::device
