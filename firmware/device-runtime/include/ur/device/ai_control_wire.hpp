// 文件说明：AI控制连接同步的严格报文边界；只读快照不能自行授予采集、焦点或恢复旧回复。
#pragma once
#include "ur/device/focus_wire.hpp"

namespace ur::device {
// AIControlBinding 是平台创建结果的公开端绑定，不包含或信任MQTT内部socket身份。
struct AIControlBinding {
    std::uint64_t endpoint_epoch = 0;       // 平台分配，socket迁移提交后递增。
    std::uint64_t capability_revision = 0;  // 当前完整能力的已确认修订号。
};
// AIControlSnapshot 保存完整AI同步事实；原token、会话与本地截止仍由恢复状态机复核。
struct AIControlSnapshot {
    Envelope envelope;                    // 原sessionSync应答，eventSeq与event_watermark一致。
    AIControlBinding binding;             // 平台迁移后的公开绑定，不是设备自报新epoch。
    std::uint64_t revision = 0;           // 本次持久快照修订号。
    std::uint64_t event_watermark = 0;    // 共享SIG已经提交的水位。
    std::uint64_t message_watermark = 0;  // MSG已经提交的水位，不与SIG混用。
    std::uint64_t media_revision = 0;     // 原媒体代次，None明确为0。
    std::uint64_t expires_at = 0;         // Unix毫秒截止，不替代本地固定单调窗口。
    std::string input_id;                 // 原采集段墓碑；恢复不能重复使用。
    std::vector<std::string> active_responses;  // 原回复列表仅用于隔离，不能重播或授权工具。
    std::optional<FocusState> focus;            // 当前真实资源投影；解析不执行restore或开麦。
    bool input_active = false;                  // 纯控制迁移成功必须paused，不隐式恢复旧输入。
    bool ended = false;                         // 终态触发正式清理，不能作为恢复成功。
    bool resume_allowed = false;                // 只解除控制屏障，媒体/输入ACK另行核验。
};
// decode_ai_control_created_binding 核创建结果成对整数原字面量；旧端两字段均省略时返回空绑定。
WireResult decode_ai_control_created_binding(std::string_view body, AIControlBinding& out);
// ai_session_sync_command 编码已有四字段契约；媒体零代次仅适用于None。
WireResult ai_session_sync_command(std::string token, std::string session, std::uint64_t last_event,
                                   std::uint64_t last_message, const AIControlBinding& binding,
                                   std::uint64_t media_revision, VoiceTransport transport,
                                   Envelope& out);
// decode_ai_control_snapshot 核完整controls及原媒体轨，不允许快照新增摄像头或扩大原选择。
WireResult decode_ai_control_snapshot(WireRoute route, std::string_view body,
                                      const VoiceMedia& selected, AIControlSnapshot& out);
}  // namespace ur::device
