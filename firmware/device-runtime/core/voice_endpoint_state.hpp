// 文件说明：VoiceClient显式AI接续的私有有界状态；普通语音不分配，不外发原标识或短期媒体参数。
#pragma once
#include "ur/device/ai_control_wire.hpp"
#include "ur/device/ai_endpoint_invite_wire.hpp"
#include "ur/device/voice_client.hpp"

namespace ur::device {
// AIEndpointState分别保留监听意图、候选原邀请和源腿事件账本，不以公开腿ID冒充模型anchor。
struct VoiceClient::AIEndpointState {
    // Fact保留原token/序号/字节，重放不能换候选、延长期限或重新执行媒体。
    struct Fact {
        std::string token, body;     // 私有有界原字节，不进入日志/网页。
        std::uint64_t sequence = 0;  // 源腿独立SIG水位；不是候选sessionEstablished水位。
    };
    AIEndpointPhase phase = AIEndpointPhase::Disabled;  // 固定本地阶段，不是平台授权。
    bool listening = false;  // 只由可信宿主启动选择，不从平台邀请自动打开功能。
    bool candidate = false;  // 当前公开腿为候选；源端仍沿原Voice生命周期。
    bool accept_acked = false, committed = false;  // 原接听ACK与源端最终提交分别核验。
    std::uint64_t committed_revision = 0;  // 首次已核完整提交的接续水位，转源继承而非SIG事件序号。
    AIEndpointInvite invite;                       // 原完整固定邀请，短期媒体只用于本候选。
    std::string invite_body;                       // 原邀请重投只能同字节，不复制旧源Provider信息。
    AIControlBinding binding;                      // 已核公开绑定；仅新目标feature可安装控制恢复。
    std::optional<std::uint64_t> control_recovery_started; // 首次目标失联15秒，快照不提前结束整组再准入。
    bool control_baseline_sent =
        false; // 新socket正式release后仅创建一次真实idle基线请求，原字节可有界重投。
    // BaselineRetry只存原关联和首次恢复绑定；正文仍唯一保存在Requests，不另造token或复制报文。
    struct BaselineRetry {
        std::string token, request_session, public_session; // 原请求key和公开腿，不进入日志。
        AIControlBinding
            binding; // 当前已确认endpoint代次绑定；断连另清调度，不能跨epoch使用旧基线。
        std::uint64_t recovery_started = 0, started = 0,
                      last = 0; // 原15秒起点与专属发送间隔，不续期。
        unsigned attempts = 0;  // 快速调度最多12次，首发及原10/20秒通用重投另计。
    };
    std::optional<BaselineRetry>
        baseline_retry; // 仅本轮目标的endpointControlBaseline分配，不影响普通能力报告。
    bool control_baseline_acked = false; // 平台明确确认本次基线之后才消费新完整组acquire。
    std::uint64_t started = 0, now = 0, wall = 0, source_watermark = 0;  // 原固定本地期限及双时钟。
    std::vector<Fact> facts;     // 最多128原事实及64KiB有效字节，不淘汰仍有效去重事实。
    std::size_t fact_bytes = 0;  // 对象与有效字符串逻辑预算，不声称分配器精确堆开销。
};
}  // namespace ur::device
