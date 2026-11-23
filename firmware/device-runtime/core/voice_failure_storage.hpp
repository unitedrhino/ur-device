// 文件说明：客户端只读诊断的紧凑私有字节；不参与协议序列化、状态转移或权限判断。
#pragma once
#include "ur/device/voice_client.hpp"

namespace ur::device {
// scope_failure_base 使用客户端未用的prepare编码区；Runtime的12..15原因不得写入该存储。
inline constexpr unsigned scope_failure_base = 0xC0;
// encode_voice_failure 保留原客户端1..11的原因/方法字节，Runtime独立来源或非法值返回空观察。
inline std::uint8_t encode_voice_failure(VoiceFailureCause cause, VoiceFailureMethod method) {
    const auto c = static_cast<unsigned>(cause), m = static_cast<unsigned>(method);
    return c > 0 && c <= static_cast<unsigned>(VoiceFailureCause::NegotiatingDeadline) &&
                   m <= static_cast<unsigned>(VoiceFailureMethod::EndpointControl)
               ? static_cast<std::uint8_t>((c << 4U) | m)
               : 0;
}
// encode_scope_failure 只为已实际拒绝的23个固定谓词生成编码；None/未知值不能补造首因。
inline std::uint8_t encode_scope_failure(ScopeFailureCode code) {
    const auto value = static_cast<unsigned>(code);
    return value > 0 && value <= static_cast<unsigned>(ScopeFailureCode::DriverOther)
               ? static_cast<std::uint8_t>(scope_failure_base + value)
               : 0;
}
// decode_scope_failure 只读固定子码；两边界及其他客户端诊断字节都不是焦点首因。
inline std::optional<ScopeFailureCode> decode_scope_failure(std::uint8_t value) {
    return value > scope_failure_base &&
                   value <=
                       scope_failure_base + static_cast<unsigned>(ScopeFailureCode::DriverOther)
               ? std::optional<ScopeFailureCode>(
                     static_cast<ScopeFailureCode>(value - scope_failure_base))
               : std::nullopt;
}
// decode_voice_failure 焦点子码仍对旧消费者投影为ActiveScopeLost/none，不改变原五字段含义。
inline std::optional<VoiceFailureDiagnostic> decode_voice_failure(std::uint8_t value) {
    if (decode_scope_failure(value))
        return VoiceFailureDiagnostic{VoiceFailureCause::ActiveScopeLost, VoiceFailureMethod::None};
    const auto cause = static_cast<VoiceFailureCause>(value >> 4U);
    const auto method = static_cast<VoiceFailureMethod>(value & 15U);
    if (!value || encode_voice_failure(cause, method) != value)
        return {};
    return VoiceFailureDiagnostic{cause, method};
}
}  // namespace ur::device
