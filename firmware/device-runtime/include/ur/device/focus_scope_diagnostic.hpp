// 文件说明：原焦点检查点的闭集只读首因；不保存身份，不参与权限、期限或驱动动作。
#pragma once
#include <cstdint>
#include <optional>

namespace ur::device {
// ScopeFailureCode
// 仅区分实际拒绝所在层；未知动态错误、资源名和请求正文不进入诊断。
enum class ScopeFailureCode : std::uint8_t {
    None,
    CandidateBinding,
    CandidateEndpoint,
    EndpointState,
    EndpointClock,
    EndpointSession,
    EngineClosed,
    EngineBusy,
    EngineLease,
    EngineReceipt,
    EngineState,
    EngineResources,
    EngineClock,
    EngineDeadline,
    AuthorityDenied,
    HALState,
    HALClock,
    AuthorityCurrent,
    AuthorityPermit,
    AuthorityPermission,
    DriverState,
    DriverOutput,
    DriverInput,
    DriverOther
};
// ScopeFailure
// 在一次原检查调用栈上收集首个实际拒绝；成功的候选分支不得提交该观察。
struct ScopeFailure {
    ScopeFailureCode code = ScopeFailureCode::None;  // 未观察不推断未知原因。
    // reject 返回原false，首因不被后继清理覆盖；空指针由调用者保持诊断默认关闭。
    bool reject(ScopeFailureCode value) {
        if (code == ScopeFailureCode::None)
            code = value;
        return false;
    }
};
// scope_reject
// 保留原判断结果，不触碰时钟、授权器或驱动；可选观察为空时无状态写入。
inline bool scope_reject(ScopeFailure* observation, ScopeFailureCode code) {
    if (observation)
        observation->reject(code);
    return false;
}
}  // namespace ur::device
