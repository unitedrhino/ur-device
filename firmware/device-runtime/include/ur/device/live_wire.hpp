// 文件说明：实时节目接收的独立线协议；只表达既有recvonly节目轨，不授予采集、URL或输出许可。
#pragma once
#include "ur/device/voice_wire.hpp"

namespace ur::device {
// LiveReceiveCapability 描述实际接收驱动格式；与AI输入、摄像头及文件输出能力完全独立。
struct LiveReceiveCapability {
    bool audio = false;          // 实际Opus/48k/单声道/20ms接收与播放端口。
    bool video = false;          // 实际H264接收与可见呈现端口。
    std::uint64_t revision = 1;  // 当前能力快照的安全正修订号。
};
// LiveInvite 固定原完整节目轨组；解析成功不意味着平台授权、焦点许可或媒体已播放。
struct LiveInvite {
    Envelope envelope;                // 已鉴权SIG原token、独立session及平台事件水位。
    std::uint64_t expires_at_ms = 0;  // 原Unix毫秒截止，不因重复邀请延期。
    bool audio = false;               // broadcast-out接收轨，不允许麦克风采集。
    bool video = false;               // broadcast-video-out接收轨，不允许摄像头采集。
};
// LiveTrackControl 只控制原组内单轨；暂停声音不得暗中暂停画面，stop后的恢复由核心拒绝。
struct LiveTrackControl {
    Envelope envelope;                 // 原操作token/session/事件水位，重试必须保持原事实。
    std::string track_id;              // 原邀请接收轨，不能指定新的媒体来源。
    std::string operation;             // 固定pause/resume/stop，不支持seek或publish。
    std::uint64_t media_revision = 0;  // 安全正媒体代次，当前代次合法性另由会话核验。
};
// LiveTrackState 是原操作的实际应答；首阶段不接受无code自主事件或把connected冒充active。
struct LiveTrackState {
    Envelope envelope;                 // 原控制token/session及明确结果code，不带事件水位。
    std::string track_id;              // 仅成功实际应答存在，仍须关联原邀请。
    std::string state;                 // 仅active/paused/stopped；active须实际输出另行证明。
    std::uint64_t media_revision = 0;  // 仅成功事实存在的安全正代次。
    std::string reason;                // 失败应答仅此稳定字段，不夹带成功实际状态。
};
// live_capability_command 生成独立纯接收快照，不默认声明AI、AEC、麦克风、相机或自由内容引用。
WireResult live_capability_command(std::string token, const LiveReceiveCapability& capability,
                                   Envelope& out);
// decode_live_invite 严格解析五字段邀请及固定六字段轨；失败清空输出，无媒体或授权副作用。
WireResult decode_live_invite(WireRoute route, std::string_view body, LiveInvite& out);
// live_has_track 只查询已固定原组；不能据轨存在替代当前播放、焦点或源许可。
bool live_has_track(const LiveInvite& invite, std::string_view track_id);
// decode_live_track_control 限制原session及原组，字段严格；实际执行与当前代次由控制器校验。
WireResult decode_live_track_control(WireRoute route, std::string_view body,
                                     const LiveInvite& invite, LiveTrackControl& out);
// decode_live_track_state 区分完整成功三字段与reason-only失败；禁止无code自主消息。
WireResult decode_live_track_state(WireRoute route, std::string_view body, LiveTrackState& out);
}  // namespace ur::device
