// 文件说明：实时节目固定末端输出组；原实际音频块/可见绘制确认与节目源授权严格分开。
#pragma once
#include <optional>
#include <string>
#include "ur/device/output_group_gate.hpp"

namespace ur::device {
// BrowserLiveLane 固定audio/video/audioVideo组，不借文件、采集或摄像头许可。
class BrowserLiveLane final : public OutputGroupGatePort {
public:
    // lane 是可信宿主固定配置；非法组永久拒绝，初始actual为未知而非默认关闭成功。
    explicit BrowserLiveLane(std::string lane);
    // request_output_gate 只请求原末端mask；首次仅false，原票据不因重试续期。
    std::optional<std::uint64_t> request_output_gate(bool enabled, std::uint64_t now) override;
    // output_gate_confirmed 仅查询当前原实际票据，不以派发或旧关闭证明本次成功。
    bool output_gate_confirmed(std::uint64_t ticket, bool enabled) const override;
    // poll 校验单调时钟和两秒驱动预算；未知输出永久失败，仍须宿主实际收尾。
    bool poll(std::uint64_t now);
    // take_command 交付独立回环消息；它不是MQTT命令或节目授权。
    std::optional<std::string> take_command();
    // receive 只消费本组原实际票据；迟到原事实不会恢复末端mask。
    bool receive(std::string_view body, std::uint64_t now);
    // permitted 当前mask仅在原true票据已实际确认且无在途时开放，不代表任何播放意图。
    bool permitted() const;
    // idle 只在当前原false票据实际确认时成立，不拿首次旧票据证明新租约安全。
    bool idle() const;
    // permitted_ticket 仅当前已actual true且无在途的原票据，供原resume形成时固定关联。
    std::optional<std::uint64_t> permitted_ticket() const;
    // lane 返回原不可变输出组；不得据此扩展云端完整轨组或租约。
    const std::string& lane() const { return lane_; }

private:
    std::string lane_;                    // 原可信固定物理组，不接网络任意路径。
    std::optional<std::uint64_t> clock_;  // 原事件线程单调高水位。
    std::optional<std::string> command_;  // 一条原未交付门控，背压不改变票据。
    std::uint64_t ticket_ = 0, confirmed_ = 0, deadline_ = 0;  // 原实际票据及固定预算。
    bool enabled_ = false, pending_ = false, sent_ = false;    // 意图、在途与交付分开。
    bool failed_ = false;  // 原未知输出不能重新基线化或提升权限。
};
}  // namespace ur::device
