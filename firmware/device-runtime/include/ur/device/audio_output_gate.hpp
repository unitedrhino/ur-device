// 文件说明：平台无关的扬声器路径门控；确认必须来自实际输出线程/DMA，不授予业务焦点或停止采集。
#pragma once
#include <cstdint>
#include <optional>

namespace ur::device {
// AudioOutputGatePort 将输出静音证明与输入生命周期隔离，物理固件可用音频线程确认实现同一接口。
class AudioOutputGatePort {
public:
    virtual ~AudioOutputGatePort() = default;
    // request_output_gate 返回独立单调票据；空值为未受理，入队不能视为实际门控完成。
    virtual std::optional<std::uint64_t> request_output_gate(bool enabled, std::uint64_t now) = 0;
    // output_gate_confirmed 仅在当前原票据的实际确认收录后为true，迟到/断连/新请求使旧证明无效。
    virtual bool output_gate_confirmed(std::uint64_t ticket, bool enabled) const = 0;
    // poll_output_gate 使用单调时钟收敛超时/回退，false要求宿主安全停止且不得假报释放。
    virtual bool poll_output_gate(std::uint64_t now) = 0;
};
}  // namespace ur::device
