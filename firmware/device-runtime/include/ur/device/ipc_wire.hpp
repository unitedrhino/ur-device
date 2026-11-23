// IPC预览邀请的可移植契约；解析成功不代表隐私、资源或媒体已就绪。
#pragma once
#include "ur/device/voice_wire.hpp"
#include <map>

namespace ur::device {
// IPCCapability 只描述当前IPC摄像头，不借用人际通话或AI的AEC声明。
struct IPCCapability {
    std::string camera_id = "front";  // 已声明的物理画面源。
    std::uint64_t revision = 1;       // 当前连接能力修订号。
    unsigned max_publishers = 1;      // 设备可并行发布的观看会话上限，范围1至4。
    bool local_accept = false;        // 是否具备本地确认入口。
    bool microphone = false;          // 是否允许预览附带监听音频，不代表对讲权限。
    bool conference_talk = false;  // 驱动确实支持双向会议音频后才能声明；要求microphone，默认关闭。
    bool human_call = false;       // 统一宿主有真实三轨人际驱动及共享门控时才声明，默认不启用。
    bool participant_roster = false;  // 宿主实际支持只读名册投影才声明；默认关闭，不授予媒体或MAC。
    bool participant_roster_sync = false;  // 完整原子快照消费能力，依赖增量名册与对讲，默认关闭。
    bool receive_video = false;  // 人际通话独立视频接收能力，不因摄像头发布自动声明。
};
// IPCInvite 保留独立会话及轨身份；资源分配器另行检查过期、隐私和并发配额。
struct IPCInvite {
    Envelope envelope;                // 已严格解析的邀请元数据。
    std::string camera_id;            // 摄像头身份，不用作sessionId。
    std::string video_track;          // 设备sendonly视频轨。
    std::string audio_track;          // 可选设备sendonly麦克风轨。
    std::string speaker_track;        // 对讲专用recvonly轨；非空时没有video_track，不隐式升级预览。
    std::uint64_t expires_at_ms = 0;  // 平台邀请截止的Unix毫秒。
    bool needs_accept = false;        // direct不免除本地隐私及资源检查。
    std::uint64_t endpoint_epoch = 0;  // 同步协商时平台给出的公开端腿代次，不是MQTT内部连接代次。
    std::uint64_t capability_revision = 0;  // 原邀请使用的能力revision；旧邀请两个绑定字段均为0。
};
// IPCTalkInvite 与观看腿分开，只有当前被邀请的独立会话才能打开双向音频。
struct IPCTalkInvite {
    Envelope envelope;                      // 对讲独立信封，不能借用预览sessionId。
    std::string camera_id;                  // 授权设备物理源，不会因此打开视频或允许AI取帧。
    std::string microphone_track;           // 相对设备sendonly音频轨。
    std::string speaker_track;              // 相对设备recvonly会议下行。
    std::uint64_t expires_at_ms = 0;        // 平台绝对截止，调用者还须核对本地时间。
    bool needs_accept = false;              // 是否必须本地同意；direct仍校验隐私和输出资源。
    std::uint64_t endpoint_epoch = 0;       // 只有显式同步能力邀请才附加的公开端腿绑定。
    std::uint64_t capability_revision = 0;  // 与本次能力上报revision严格相同，不默认猜测1。
};
// IPCRosterSnapshot
// 是完整只读同步投影，不创建Peer、不执行轨控制或恢复会议话权。
struct IPCRosterSnapshot {
    Envelope envelope;                  // 原sessionSync请求token及独立腿sessionId。
    std::uint64_t revision = 0;         // 平台持久快照修订号。
    std::uint64_t event_watermark = 0;  // 与本报文eventSeq一致的共享水位。
    std::uint64_t expires_at_ms = 0;    // 当前快照Unix毫秒截止，调用者还须核原单调等待期限。
    std::string participants;           // 已完整校验的显式数组，只用于原子名册替换。
    std::map<std::string, std::string> track_states;  // 原双音轨展示状态，不作为驱动指令。
    bool ended = false;           // 平台终态投影；true触发本地R03清理，不恢复媒体或重试旧操作。
    bool resume_allowed = false;  // 平台允许尝试恢复；仍须独立重协商焦点与媒体，不直接恢复输出。
};
// MediaUpdatedEvent
// 保存质量修订的下行投影；同token应答与applied独立事件共用此类型。
struct MediaUpdatedEvent {
    Envelope envelope;                   // 原关联与事件水位；pending应答code=200，冲突409。
    std::string state;                   // pending/applied/failed。
    std::uint64_t revision = 0;          // 媒体计划修订号；同计划pending到applied保持一致。
    std::uint64_t media_revision = 0;    // pending/applied必填；仅质量修订保持原Peer代次。
    std::string request_token;           // applied事件关联原mediaUpdate请求。
    std::string reason;                  // failed必填；成功投影不得携带。
    std::uint64_t current_revision = 0;  // 仅409冲突应答携带的平台当前计划修订。
    std::map<std::string, std::string>
        tracks;  // 已严格校验的受影响轨实态，控制器还须核原邀请范围。
};
// IPCQualityCommand 为原Peer上的异步质量调整，不新增轨、授权或媒体代次。
struct IPCQualityCommand {
    std::string session;         // 原会话。
    std::string token;           // 原mediaUpdate请求token，不能借新的计划接受旧回调。
    std::string track;           // 原授权目标轨。
    std::string profile;         // audioOnly/low/standard/high，仅在实际驱动支持时应用。
    std::uint64_t revision = 0;  // 平台pending确认的计划修订。
};
// decode_media_updated
// 只解析mediaUpdated正文；路由、关联与状态组合不合法即拒绝，不产生副作用。
WireResult decode_media_updated(WireRoute route, std::string_view body, MediaUpdatedEvent& out,
                                std::uint64_t media_revision = 1);
// ipc_media_update_command
// 编码弱网setQuality修订；目标轨必须是原授权轨，不新增来源或授权。
WireResult ipc_media_update_command(std::string token, std::string session,
                                    std::string_view track_id, std::uint64_t expected_revision,
                                    std::string_view profile, const IPCInvite& invite,
                                    Envelope& out);
// track_state_report_command
// 编码设备主动实态上报；无code独立事件，媒体代次仍须一致。
WireResult track_state_report_command(std::string token, std::string session,
                                      std::string_view track_id, std::string_view state,
                                      const IPCInvite& invite, Envelope& out,
                                      std::string_view request_token = {},
                                      std::uint64_t revision = 0, std::string_view reason = {},
                                      std::uint64_t media_revision = 1);
// decode_ipc_roster_snapshot
// 严格核完整控制、原邀请绑定与原轨，失败清空输出，时间与在途关联由调用者复核。
WireResult decode_ipc_roster_snapshot(WireRoute route, std::string_view body,
                                      const IPCInvite& invite, const IPCCapability& capability,
                                      IPCRosterSnapshot& out);
// decode_ipc_talk_invite
// 只校验对讲契约和设备独立声明，失败清空输出，不执行采集或播放。
WireResult decode_ipc_talk_invite(WireRoute route, std::string_view body,
                                  const IPCCapability& capability, IPCTalkInvite& out);
// IPCTrackCommand 保存当前邀请内轨的下行控制；解析不代表驱动实际生效。
struct IPCTrackCommand {
    Envelope envelope;           // 原请求关联及事件水位，驱动回执必须保留。
    std::string track_id;        // 仅当前邀请已授权的轨，不允许新增麦克风。
    std::string operation;       // 通用六种操作，发布/订阅权限由控制器检查。
    std::string reason;          // 可选有界原因；显式空值拒绝。
    std::uint64_t revision = 0;  // 原Peer媒体代次，重协商后旧代次不得执行。
};
// decode_ipc_track 校验路由、原邀请及字段；失败清空输出，不调用驱动或扩大授权。
WireResult decode_ipc_track(WireRoute route, std::string_view body, const IPCInvite& invite,
                            IPCTrackCommand& out, std::uint64_t media_revision = 1);
// ipc_capability_command 生成纯视频或附带音频的能力上报，失败清空输出。
WireResult ipc_capability_command(std::string token, const IPCCapability& capability,
                                  Envelope& out);
// decode_ipc_invite 校验用途、路由、字段及能力交集；失败清空目标且不输出原文。
WireResult decode_ipc_invite(WireRoute route, std::string_view body,
                             const IPCCapability& capability, IPCInvite& out);
}  // namespace ur::device
