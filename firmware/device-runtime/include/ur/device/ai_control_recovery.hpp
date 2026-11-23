// 文件说明：AI控制短断连的单事件线程屏障；保留原业务会话，不拥有网络、模型或音频驱动。
#pragma once
#include "ur/device/ai_control_wire.hpp"

namespace ur::device {
// AIControlPhase 只表示控制握手；Bound不能代替媒体Ready、焦点或audioStart确认。
enum class AIControlPhase { Bound, Disconnected, Reporting, ReadyToSync, Syncing, Failed };
// AIControlRecovery 固定首次断连15秒预算，等待当前能力ACK和原会话完整快照，不重放旧业务请求。
class AIControlRecovery {
public:
    // 构造只接已核sessionCreated的公开绑定和原媒体代次；无效值永久Failed。
    AIControlRecovery(std::string session, AIControlBinding binding, std::uint64_t media_revision,
                      VoiceTransport transport);
    // begin 首次断连冻结原水位和用户意图；重复观察不能延长恢复窗口。
    Result begin(std::uint64_t now, std::uint64_t event_watermark, std::uint64_t message_watermark,
                 bool wants_capture);
    // reporting 仅由新的实际连接全部SUBACK后调用；每次新连接用新能力token，但期限仍取首次断连。
    Result reporting(std::string token, std::uint64_t capability_revision, std::uint64_t now);
    // capability_ack 必须精确关联当前完整能力；旧连接ACK不推进同步。
    Result capability_ack(std::string_view token, std::uint64_t revision, std::uint64_t now);
    // sync 生成原四字段请求，等待相同token的完整快照，不创建新模型会话。
    Result sync(std::string token, std::uint64_t now, Envelope& out);
    // accept 核原会话、水位、平台新端代次和双时钟；快照不执行或重播任何旧媒体。
    Result accept(const AIControlSnapshot& snapshot, std::uint64_t now, std::uint64_t wall);
    // stop_capture 撤销恢复前采集意图，不向断网连接发布旧audioStop。
    void stop_capture() { wants_capture_ = false; }
    // expired 逐次核单调时钟，超时或倒退永久失败，不接受迟到快照续期。
    bool expired(std::uint64_t now);
    AIControlPhase phase() const { return phase_; }
    const AIControlBinding& binding() const { return binding_; }
    // original_session 精确比较平台初建scope；只返回布尔，不向HAL或网页泄露会话标识。
    bool original_session(std::string_view session) const { return session == session_; }
    // message_watermark 只返回原已应用或成功完整快照提交的MSG水位，不混用SIG序号。
    std::uint64_t message_watermark() const { return message_watermark_; }
    // wants_capture 只是用户最新意图；必须生成新inputId并等待新ACK后才能启动驱动。
    bool wants_capture() const { return wants_capture_; }
    // recovering 不把Bound或Failed当成临时离线，调用方据此决定是否继续推进原会话。
    bool recovering() const {
        return phase_ != AIControlPhase::Bound && phase_ != AIControlPhase::Failed;
    }
    // retired_response 查询控制断连期间已被快照退休的回复，不让迟到Start重新发声。
    bool retired_response(std::string_view response) const;
    // advance_media_revision 只接原WebRTC ACK已确认的连续代次，不改变公开端绑定或恢复期限。
    Result advance_media_revision(std::uint64_t revision);

private:
    std::string session_;                  // 平台原业务会话，控制迁移不更换。
    std::string token_;                    // 当前能力或同步请求关联，不保存自由正文或凭据。
    AIControlBinding binding_;             // 当前平台公开绑定，只有成功快照才能更新。
    std::uint64_t media_revision_ = 0;     // 原Peer代次；控制恢复不自行推进。
    std::uint64_t started_ = 0;            // 首次断连单调起点，重复握手不刷新。
    std::uint64_t observed_ = 0;           // 最近单调观察，任何回退都failclosed。
    std::uint64_t event_watermark_ = 0;    // 原端已实际应用的SIG水位。
    std::uint64_t message_watermark_ = 0;  // 原端已实际应用的MSG水位。
    std::uint64_t report_revision_ = 0;    // 本次新连接完整能力修订，不默认沿用旧epoch。
    VoiceTransport transport_;             // 原会话选定通道，不在恢复时切换。
    AIControlPhase phase_ = AIControlPhase::Failed;  // 校验成功的构造才变Bound。
    bool wants_capture_ = false;                     // 用户停止优先于恢复前的意图。
    std::vector<std::string>
        retired_responses_;  // 活会话最多512个原回复墓碑，满时失败，不淘汰后复播。
};
}  // namespace ur::device
