// 文件说明：云端录制信令的可移植边界；只构造上行控制并解析平台状态，不取流、不封装文件、不核验被录者同意。
#pragma once
#include <string>
#include <string_view>
#include <vector>
#include "ur/device/voice_wire.hpp"

namespace ur::device {
// RecordingControlCommand 保存本端要上报的原控制；条件字段与操作严格绑定，构造成功不代表平台受理。
struct RecordingControlCommand {
    std::string operation;                      // start/pause/resume/stop。
    std::uint64_t expected_revision = 0;        // 录制实例修订号；start固定0，其余须等于当前事实。
    std::string recording_id;                   // 幂等录制实例ID，不是本地路径。
    std::vector<std::string> source_track_ids;  // 仅start；授权取流轨，非空且有界。
    std::vector<std::string> consent_ids;       // 仅start/resume；全部被录者同意凭证。
    std::string policy_id;                      // 仅start；保留/访问/删除策略引用。
};
// recording_control_command 构造SIG上行原请求；失败清空输出，不推断本地录像能力。
WireResult recording_control_command(const RecordingControlCommand& control, std::string_view token,
                                     std::string_view session, Envelope& out);
// RecordingStateFact 保存已校验的平台事实；受理不等于开始录制，finalizing不等于文件可用。
struct RecordingStateFact {
    Envelope envelope;            // 应答带code；自主完成事件带事件水位。
    std::string state;            // starting/active/paused/finalizing/ready/failed。
    std::uint64_t revision = 0;   // 录制实例修订号，零是合法起点但字段必须存在。
    std::string recording_id;     // 原实例ID。
    std::string content_ref;      // 仅ready；鉴权录制文件引用，不接受URL或本地路径。
    std::uint64_t duration_ms = 0;  // 仅ready；实际可播放时长，必须为正。
    std::string reason;           // 失败应答或failed/自动paused事件的固定公开原因。
    std::string request_token;    // 仅自主事件可选；关联原控制token。
};
// decode_recording_state 严格区分受理应答、失败应答与自主事件；失败清空输出，不因code200推断文件可用。
WireResult decode_recording_state(WireRoute route, std::string_view json, RecordingStateFact& out);
}  // namespace ur::device
