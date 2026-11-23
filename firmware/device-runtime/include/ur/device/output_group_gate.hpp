// 文件说明：独占媒体路径的完整输出门控；实际确认不等于业务授权或全局资源取得。
#pragma once
#include <cstdint>
#include <optional>

namespace ur::device {
// OutputGroupGatePort 包含路径实际使用的声音/画面，不操作输入、用户音量或播放意图。
// 全局驱动须等待所有相关路径的原票据确认，不能用某一路静音充当完整组证明。
class OutputGroupGatePort {
public:
    virtual ~OutputGroupGatePort() = default;
    // request_output_gate 非阻塞接纳原票据；打开仍须独立业务许可，不恢复旧播放意图。
    virtual std::optional<std::uint64_t> request_output_gate(bool enabled, std::uint64_t now) = 0;
    // output_gate_confirmed 来自实际音频块及画面隔离的原票据，失效/释放后为false。
    virtual bool output_gate_confirmed(std::uint64_t ticket, bool enabled) const = 0;
};
}  // namespace ur::device
