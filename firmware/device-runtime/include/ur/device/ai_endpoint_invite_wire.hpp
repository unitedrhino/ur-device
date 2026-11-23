// 文件说明：原AI模型会话的候选端邀请边界；解析不接听、不启动媒体或迁移授权。
#pragma once
#include "ur/device/voice_wire.hpp"

namespace ur::device {
// AIEndpointInvite 保存同一接续事务与原音频选择；held媒体就绪仍不是提交事实。
struct AIEndpointInvite {
    Envelope envelope;             // 原下行sessionInvite及事件水位。
    std::string source_session;    // 原模型anchor的公开业务会话标识。
    std::string handoff_id;        // 原prepare事务；不得跨会话复用。
    std::uint64_t expires_at = 0;  // 原固定候选期限，绝对Unix毫秒。
    VoiceMedia media;              // 当前候选已选媒体，仅UDP携带短期密钥。
};
// decode_ai_endpoint_invite
// 严格解析原signaling邀请及能力交集；失败清空输出，不开启设备采集。
// local_accept必须来自真实本地确认驱动声明，不能从AI音频能力自动推导。
WireResult decode_ai_endpoint_invite(WireRoute route, std::string_view body,
                                     const VoiceCapability& capability, bool local_accept,
                                     AIEndpointInvite& out);
}  // namespace ur::device
