// 文件说明：统一资源焦点信令的可移植报文边界；解析不授予资源、不触发驱动或改变用户意图。
#pragma once
#include <vector>
#include "ur/device/voice_wire.hpp"

namespace ur::device {
// FocusCommand 保留原完整资源集合及租约条件，不能把部分资源确认解释为整组取得。
struct FocusCommand {
    Envelope envelope;                    // 原SIG下行token、会话及事件水位，身份仍来自已鉴权Topic。
    std::string operation;                // acquire/suspend/restore/release；并非麦克风启停指令。
    std::uint64_t expected_revision = 0;  // 设备资源修订号，零为有效初始值。
    std::string focus_id;                 // 会话绑定的资源租约ID，不是播放实例、轨或cameraId。
    std::vector<std::string> resources;   // 原完整非重复集合，speaker/display与camera:ID独立。
    std::string priority;       // background/conversation/call/emergency，授权由上层独立核验。
    std::string resume_policy;  // never/explicit/ifUnchanged，不自行恢复已停止的用户意图。
    std::optional<std::uint64_t> expires_at;  // acquire/restore的Unix毫秒截止，不充当单调驱动期限。
};
// FocusState 保存实际资源事实；构造或编码成功不证明驱动已经执行。
struct FocusState {
    Envelope envelope;                   // 原应答关联；主动事件可无code，但仍须绑定原会话。
    std::string state;                   // acquired/suspended/restored/released/denied。
    std::uint64_t revision = 0;          // 实际设备资源修订号，不因请求入队推进。
    std::string focus_id;                // 原资源租约ID。
    std::vector<std::string> resources;  // 全部实际确认资源；与请求关联时不允许删减。
    std::string reason;                  // 仅denied有稳定原因，不能携带任意驱动错误、URL或凭据。
};
// decode_focus_control
// 严格解析SIG下行；失败清空输出，未知字段/资源/重复键全部拒绝。
WireResult decode_focus_control(WireRoute route, std::string_view json, FocusCommand& out);
// decode_focus_state 严格解析SIG上行；失败清空，不从code200推断整组资源有效。
WireResult decode_focus_state(WireRoute route, std::string_view json, FocusState& out);
// focus_reply
// 仅编码原命令实际结果，核对全组、原会话/租约与修订号；不调用资源驱动。
WireResult focus_reply(const FocusCommand& command, std::string_view state, std::uint64_t revision,
                       int code, std::string_view reason, Envelope& out);
// focus_reply_matches 验证实际回执是否属于原请求；主动事件不得代替原操作完成。
bool focus_reply_matches(const FocusCommand& command, const FocusState& state);
// add_focus_capability
// 为已有全量能力显式附加唯一focusControl资源组，保留原token/用途/模态。
// 宿主仅在实际唯一驱动已装配时调用；函数不授权、不启用驱动、不接受替换既有控制对象。
WireResult add_focus_capability(const Envelope& source, const std::vector<std::string>& group,
                                Envelope& out);
// add_focus_baseline
// 为已有显式焦点能力附加首次完整idle事实，revision须由唯一实际驱动提供。
// 不接受部分组、未知相机或覆盖旧基线；编码器不证明空闲，默认运行时不得自行调用。
WireResult add_focus_baseline(const Envelope& source,
                              const std::vector<std::string>& idle_resources,
                              std::uint64_t revision, Envelope& out);
}  // namespace ur::device
