// 文件说明：独立于动态播放器实例的固定桌面输出路径；实际音频块/绘制确认后才提供门控事实。
#pragma once
#include <optional>
#include <string>
#include "ur/device/output_group_gate.hpp"

namespace ur::device {
// BrowserFileLane 固定audio/video/audioVideo之一，供可信宿主连接同一完整资源组的真实播放器。
// 不解析内容或分配云端身份，不从构造、空播放器或命令排队推断实际空闲。
class BrowserFileLane final : public OutputGroupGatePort {
public:
    // BrowserFileLane 构造固定路径；非法枚举永久失效，初始实际状态为未知。
    explicit BrowserFileLane(std::string lane);
    // request_output_gate 首次只允许关闭；独立两秒截止，不在事件线程等待浏览器。
    std::optional<std::uint64_t> request_output_gate(bool enabled, std::uint64_t now) override;
    // output_gate_confirmed 只读原已交付票据事实，不能拿旧关闭证明新请求成功。
    bool output_gate_confirmed(std::uint64_t ticket, bool enabled) const override;
    // poll 时钟回退/原请求超时永久失效；宿主仍须关闭网页，不虚报资源释放。
    bool poll(std::uint64_t now);
    // take_command 交付固定本地门控JSON；没有设备/源凭据，不是MQTT设备信令。
    std::optional<std::string> take_command();
    // receive 严格消费匹配原路径的本地实际事实；未知/未来/错值回执拒绝。
    bool receive(std::string_view body, std::uint64_t now);
    // lane 返回不可改换的固定物理路径选择，不代表租约或播放器身份。
    const std::string& lane() const { return lane_; }

private:
    std::string lane_;                    // 固定物理路径，不能从网络控制切到另一个播放器。
    std::optional<std::uint64_t> clock_;  // 同一设备事件线程的单调观察。
    std::optional<std::string> command_;  // 一条原待交付命令，背压不能改变原票据。
    std::uint64_t ticket_ = 0, confirmed_ = 0, deadline_ = 0;  // 原实际门控代次与固定截止。
    bool enabled_ = false, pending_ = false, sent_ = false;    // 目标/在途/已交付分别记录。
    bool failed_ = false;  // 非法配置或未知输出永远不能重新声明首次基线。
};
}  // namespace ur::device
