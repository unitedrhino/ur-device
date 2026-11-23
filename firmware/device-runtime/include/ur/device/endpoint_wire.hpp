// 文件说明：S18跨端接续契约的可移植报文边界；解析不建立连接、不撤销旧端授权、不推进接续账本。
#pragma once
#include <optional>
#include <string>
#include <string_view>
#include "ur/device/voice_wire.hpp"

namespace ur::device {
// EndpointTarget 保存prepare声明的目标端；kind固定endpoint，同一用户归属由平台独立核验。
struct EndpointTarget {
    std::string kind;  // 本阶段固定endpoint，不接受device/service等其他Party目标。
    std::string id;    // 目标端标识，非空且不含控制字符。
};
// EndpointCommand 保留原上行接续请求；字段合法不代表平台已受理或原子转移获准。
struct EndpointCommand {
    Envelope envelope;                    // 原SIG上行请求，身份仍来自已鉴权Topic。
    std::string operation;                // prepare/commit/abort；并非媒体启停指令。
    std::uint64_t expected_revision = 0;  // 端切换修订号，零为有效首值。
    std::string handoff_id;               // 幂等接续事务，不跨会话复用。
    std::optional<EndpointTarget> target;  // 仅prepare必填；其他操作出现即拒绝。
    std::string candidate_session_id;      // 仅commit必填；目标已建立会话腿。
};
// EndpointState 保存平台下发的接续事实；preparing/ready不代表最终完成。
struct EndpointState {
    Envelope envelope;                  // 原应答或主动事件关联；code与事件水位互斥。
    std::string state;                  // preparing/ready/committed/aborted/failed。
    std::uint64_t revision = 0;         // 接续修订号，拒绝与超时可不同。
    std::string handoff_id;             // 原接续事务。
    std::string candidate_session_id;   // 仅ready/committed；目标腿。
    std::string reason;                 // 仅failed/aborted的固定公开原因，不透传内部错误。
};
// decode_endpoint_control
// 严格解析SIG上行；失败清空输出，未知字段/重复键/条件字段错位全部拒绝。
WireResult decode_endpoint_control(WireRoute route, std::string_view json, EndpointCommand& out);
// decode_endpoint_state
// 校验平台下行事实的应答/事件形态与reason白名单；失败清空，不从code200推断接续完成。
WireResult decode_endpoint_state(WireRoute route, std::string_view json, EndpointState& out);
}  // namespace ur::device
