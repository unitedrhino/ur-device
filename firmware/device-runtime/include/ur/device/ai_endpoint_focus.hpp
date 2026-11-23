// 文件说明：AI候选端提交后、原焦点操作之前的可信HAL准备屏障；不以准备完成代替焦点授权。
#pragma once
#include <cstdint>
#include <string_view>

#include "ur/device/focus_wire.hpp"
#include "ur/device/focus_engine.hpp"

namespace ur::device {
// AIEndpointFocusScope只借用当前候选的完整双向轨；仅事件线程即时使用，不跨生命周期缓存view。
// 能取得此scope表示本地明确接听、媒体实际就绪及平台两项提交事实齐全，不表示已获麦克风或扬声器。
struct AIEndpointFocusScope {
    std::string_view session_id;       // 当前候选公开腿；不是原模型anchor或原设备标识。
    std::string_view input_track;      // 本端完整麦克风轨，须和实际媒体配置一致。
    std::string_view output_track;     // 本端完整扬声器轨，不能只绑定单向输出。
    std::uint64_t media_revision = 0;  // 原候选实际媒体代次；准备屏障不得自行递增。
    std::uint64_t endpoint_epoch = 0; // 仅新目标控制恢复协商后有效，旧候选不得补造。
    std::uint64_t capability_revision = 0; // 与原当前完整能力一致；不是媒体或输入路由代次。
};
// EndpointFocusPreparation只描述HAL原scope绑定；Ready仍须原FocusEngine完整取得和回执交付。
enum class EndpointFocusPreparation { Ready, Pending, Failed };
// EndpointFocusPreparePort仅用于显式AI候选端：先实际关闭双向驱动再绑定scope，禁止默认成功实现。
class EndpointFocusPreparePort {
public:
    virtual ~EndpointFocusPreparePort() = default;
    // prepare非阻塞推进同一scope；Pending不能提前调用原FocusEndpoint或启动任何采播。
    // command为严格解析并固定的原focusControl，含focusId/整组/token/截止，不能由HAL补造。
    // now为单调毫秒，wall为Unix毫秒；实现不得靠本方法刷新原候选或原焦点截止。
    virtual EndpointFocusPreparation prepare(const AIEndpointFocusScope& scope,
                                             const FocusCommand& command, std::uint64_t now,
                                             std::uint64_t wall) = 0;
    // cancel只撤销当前绑定意图并开始实际清理；不是驱动已经停流的证明。
    virtual void cancel() = 0;
    // settled仅在原异步双向清理实际完成后为真；运行时收尾须继续泵原HAL事实。
    virtual bool settled() const = 0;
    // control_recovery_supported仅由完整HAL实现声明；默认端口不具备原租约关闭后重绑定能力。
    virtual bool control_recovery_supported() const { return false; }
    // suspend_control仅由原运行时失联分支调用，撤销完整组并保留原关闭租约事实；不授权新连接。
    virtual bool suspend_control(const AIEndpointFocusScope&, std::uint64_t, std::uint64_t) {
        return false;
    }
    // control_idle_observation须有原正式release及回执交付；只投影当前双false/CAS，不制造idle。
    virtual std::optional<FocusIdleObservation> control_idle_observation() const { return {}; }
};
}  // namespace ur::device
