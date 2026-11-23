// 文件说明：纯成员名册解析与有界展示投影，不持有网络、会话、媒体驱动或IM话权。
#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace ur::device {
// ParticipantTrack 仅保存公共轨元数据，不是实际媒体轨或发布、订阅许可。
struct ParticipantTrack {
    std::string track_id;   // 会话内轨标识，不能据此新建驱动轨。
    std::string kind;       // audio/video/subtitle。
    std::string source_id;  // 平台源标识，不作为设备身份或授权。
    std::string direction;  // sendonly/recvonly/sendrecv，相对接收端。
    std::string codec;      // 公开编码描述，不自动配置驱动。
    std::string state;      // requested/active/paused/stopped，仅展示事实。
};
// RosterParticipant 是同一独立设备腿内的完整成员展示条目。
struct RosterParticipant {
    std::string participant_id;            // 原IM成员标识，与sessionId分开。
    std::vector<ParticipantTrack> tracks;  // 完整条目而非轨增量，不授予媒体权限。
};
// ParticipantRoster 在设备事件线程投影joined/left；调用者独立核验会话、能力与共享水位。
class ParticipantRoster {
public:
    // apply 先完整校验增量正文与资源边界再提交，非法输入保留原名册。
    bool apply(std::string_view payload);
    // replace 校验完整participants数组后原子替换展示，不能授予成员媒体、MAC或会话恢复权限。
    bool replace(std::string_view participants);
    // members 返回当前已消费条目，不承诺已经取得会议完整名册或成员媒体连通。
    const std::vector<RosterParticipant>& members() const { return members_; }
    // clear 用于原腿终态或断连，仅清除展示事实，不操作媒体或网络。
    void clear() { members_.clear(); }

private:
    std::vector<RosterParticipant> members_;  // 最多32条、每条8轨、逻辑预算64KiB。
};
}  // namespace ur::device
