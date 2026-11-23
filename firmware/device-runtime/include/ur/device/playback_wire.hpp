// 文件播放器信令的可移植边界；音乐/视频共用命令，不执行下载、采集或焦点抢占。
#pragma once
#include <vector>
#include "ur/device/voice_wire.hpp"

namespace ur::device {
// PlaybackCapability 来自实际文件输出驱动；默认不支持，不以AI扬声器/摄像头推断文件能力。
struct PlaybackCapability {
    std::uint64_t revision = 1;  // 本设备连接内的完整能力快照版本。
    bool audio_output = false;   // 实际文件音频输出驱动已装配。
    bool video_output = false;   // 实际文件画面输出驱动已装配，不是摄像头。
};
// playback_capability_command 生成纯文件端完整上报；与AI共存需宿主合并全量快照，不能分次覆盖。
WireResult playback_capability_command(std::string token, const PlaybackCapability& capability,
                                       Envelope& out);
// PlaybackInvite 描述独立文件输出会话；解析不采集音视频、不下载，也不申请焦点。
struct PlaybackInvite {
    Envelope envelope;                           // 原邀请关联与平台事件水位。
    std::string playback_id;                     // 子会话内固定实例，不是AI回复ID。
    std::string content_ref;                     // 必须再经可信内容解析器授权。
    std::vector<std::string> output_modalities;  // audio/video；视频输出不等于摄像头。
    std::uint64_t expires_at = 0;                // 邀请截止，时钟有效性由会话层校验。
};
// decode_playback_invite 仅接受SIG文件邀请、none和空输入/实时轨；失败清空输出。
WireResult decode_playback_invite(WireRoute route, std::string_view body, PlaybackInvite& out);
// validate_playback_outputs 按真实驱动求交邀请输出；失败不建立实例或开启采集。
WireResult validate_playback_outputs(const PlaybackCapability& capability,
                                     const PlaybackInvite& invite);
// PlaybackCommand 保存当前子会话的控制；解析成功不代表已获媒体访问或输出许可。
struct PlaybackCommand {
    Envelope envelope;                         // 原token/session/eventSeq；回执不能关联到主AI会话。
    std::string operation;                     // load/play/pause/seek/setRate/stop。
    std::string playback_id;                   // 子会话内播放器实例。
    std::string content_ref;                   // 仅load允许；不由设备解释为任意URL/文件路径。
    std::uint64_t expected_revision = 0;       // 必填，首次load允许0。
    std::optional<std::uint64_t> position_ms;  // 仅seek允许，显式0是合法起点。
    std::optional<double> rate;                // 仅setRate允许，0.5/1/1.5/2。
    std::optional<std::uint64_t> media_revision;  // 历史流代次；用途规则由会话校验。
};
// decode_playback_control
// 严格校验SIG下行和条件字段；失败清空输出，不调用任何驱动。
WireResult decode_playback_control(WireRoute route, std::string_view body, PlaybackCommand& out);
}  // namespace ur::device
