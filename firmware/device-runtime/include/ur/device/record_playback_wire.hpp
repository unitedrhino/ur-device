// 文件说明：独立历史录像报文边界；只解析有限档案引用，不打开文件、不采集或授予媒体许可。
#pragma once
#include "ur/device/playback_wire.hpp"

namespace ur::device {
// RecordPlaybackClip 是相对整份录像起点的半开毫秒区间，驱动必须另验实际时长及裁剪能力。
struct RecordPlaybackClip {
    std::uint64_t start_ms = 0;  // 可为零的授权起点，不是播放器当前实际位置。
    std::uint64_t end_ms = 0;    // 必须大于起点的授权终点，区间外媒体不得发布。
};
// RecordPlaybackInvite 固定单个历史实例；录像/版本/引用须由可信清单再次核验，引用不授用户权。
struct RecordPlaybackInvite {
    Envelope envelope;                // 原邀请token、独立session及平台信令水位。
    std::string camera_id;            // 档案所属相机，不代表允许开启实时相机。
    std::string recording_id;         // 固定档案标识，不作为本地路径使用。
    std::string recording_version;    // 不可变版本，与原档案及有限引用一起匹配。
    std::string playback_id;          // 同一业务会话内固定播放器实例，不因seek更换。
    std::string content_ref;          // 可信清单opaque引用，不接受URL或自由路径。
    RecordPlaybackClip clip;          // 平台已固化的有限区间，解析不证明精确裁剪可用。
    std::uint64_t expires_at_ms = 0;  // 原邀请Unix毫秒截止，时钟判断由会话控制器负责。
    bool audio = false;               // 仅明确的record-audio轨；不是麦克风或speaker许可。
};
// RecordPlaybackStateKind 区分实际播放器状态、操作失败与整组释放，不混用成功事实。
enum class RecordPlaybackStateKind { Actual, Failure, Released };
// RecordPlaybackState 只保存已校验正文；状态修订和媒体代次仍由当前会话关联判断。
struct RecordPlaybackState {
    Envelope envelope;                                               // 原操作应答或独立自主事件。
    RecordPlaybackStateKind kind = RecordPlaybackStateKind::Actual;  // 默认不意味着解析成功。
    std::string playback_id;                                         // 实际或释放事实中的原实例。
    std::string state;                 // 实际loaded/playing/paused/seeking/stopped/ended/failed。
    std::string reason;                // 脱敏失败原因，不能据此推断播放位置。
    std::uint64_t revision = 0;        // 必填非负实际播放器水位，零不是缺省；不是媒体协商代次。
    std::uint64_t media_revision = 0;  // 实际状态的正媒体代次；失败ACK/释放没有此字段。
    std::uint64_t position_ms = 0;     // 相对授权clip的实际位置，零是合法值。
    std::optional<std::uint64_t> duration_ms;  // 可选真实clip时长，不能小于实际位置。
    std::uint64_t end_event_sequence = 0;      // 释放仅关联原sessionEnded水位。
};
// decode_record_playback_invite 只接受SIG历史direct/WebRTC固定轨邀请；失败清空输出，无授权副作用。
WireResult decode_record_playback_invite(WireRoute route, std::string_view body,
                                         RecordPlaybackInvite& out);
// decode_record_playback_control 复用控制解析，但历史必带正媒体代次且固定1倍速、不支持setRate。
// 当前代次、clip内定位及原token合法性由后续控制器判断；失败清空输出。
WireResult decode_record_playback_control(WireRoute route, std::string_view body,
                                          PlaybackCommand& out);
// decode_record_playback_state 严格区分实际状态、仅reason的失败ACK和不带媒体代次的全组释放。
// seeking可表达新代次，不能据解析成功证明当前m+1已授权或媒体已连通。
WireResult decode_record_playback_state(WireRoute route, std::string_view body,
                                        RecordPlaybackState& out);
}  // namespace ur::device
