// 文件说明：候选端唯一运行时的可选焦点准备状态；不扩大普通会话常驻队列。
#pragma once
#include "ur/device/voice_runtime.hpp"

namespace ur::device {
// EndpointRuntime只在可信宿主显式装配时分配，原HAL寿命必须长于运行时。
struct VoiceRuntime::EndpointRuntime {
    // Pending只保存首次严格原请求，重复不能续两秒或替换token/资源/截止。
    struct Pending {
        std::string topic, body;    // 原字节仅私有回放，不外发身份或自由正文。
        FocusCommand command;       // HAL绑定所需原focusId/完整资源/token/截止。
        std::uint64_t started = 0;  // 首次单调固定准备起点。
    };
    EndpointFocusPreparePort* port = nullptr;  // 实际可信HAL，不提供默认成功对象。
    bool listening = false;                    // 由start_listening明确选择，不从平台报文开启。
    std::optional<Pending> pending;            // 最多一条，背压/异步准备不能积累无界原报文。
    std::string cleanup_session; // 仅已实际取得候选转出时冻结原腿，关闭后仍等待该原租约正式release。
};
}  // namespace ur::device
