// 文件说明：把AI控制重鉴权接回原语音核心；先停实际HAL，再清旧连接请求，不重建模型或媒体配置。
#include <algorithm>
#include "ur/device/ai_control_recovery.hpp"
#include "ur/device/voice_client.hpp"
#include "voice_endpoint_state.hpp"

namespace ur::device {
// control_sync_enabled 只认平台创建应答成功安装的公开绑定，不依赖本端意愿。
bool VoiceClient::control_sync_enabled() const {
    return control_ && control_->phase() != AIControlPhase::Failed;
}
// control_recovering 不把WebRTC纯媒体重协商解释为MQTT连接恢复。
bool VoiceClient::control_recovering() const { return control_ && control_->recovering(); }
// control_message_watermark 不把SIG或媒体代次借给MSG；旧设备无状态返回0。
std::uint64_t VoiceClient::control_message_watermark() const {
    return control_ ? control_->message_watermark() : 0;
}
// control_endpoint_epoch 未协商返回0，不能由宿主补为1或改写平台绑定。
std::uint64_t VoiceClient::control_endpoint_epoch() const {
    return control_ ? control_->binding().endpoint_epoch : 0;
}
// control_original_session_preserved 只读真实当前scope，不保存另一份身份或以true代替模型/媒体验收。
bool VoiceClient::control_original_session_preserved() const {
    return control_ && !session_id_.empty() && control_->original_session(session_id_);
}

// control_disconnected 在原会话已建立且显式支持时形成屏障，保留回复/输入墓碑和请求counter。
Result VoiceClient::control_disconnected(std::uint64_t now, std::uint64_t last_message) {
    if (!control_sync_enabled() || capability_.ai_vision)
        return Result::Unsupported;
    if (state_ != VoiceState::Established && !control_recovering())
        return Result::Stale;
    const bool target = capability_.ai_endpoint_control_sync && endpoint_ && endpoint_->candidate;
    const auto begun = control_->begin(now, event_seq_, last_message, target ? false : want_capture_);
    if (begun != Result::Applied && begun != Result::Duplicate)
        return begun;
    if (begun == Result::Applied)
        capability_conflicts_ = 0;  // 一个固定恢复窗口最多重报两次，不按物理重连次数无限刷新。
    if (target)
        endpoint_->baseline_retry
            .reset(); // 原socket失联立即撤本轮调度；重复失联不刷新首次恢复起点。
    if (target && begun == Result::Applied) {
        endpoint_->control_recovery_started = now;
        endpoint_->control_baseline_sent = endpoint_->control_baseline_acked = false;
        endpoint_->phase = AIEndpointPhase::AwaitingFocus;
        recovery_.focus_suspended = true; // 原权限只可关闭；新完整组需快照、真实基线及实际取得。
    }
    phase_observed_.reset();        // 旧socket退避计时不能在真实重鉴权前向离线端口再次publish。
    // stop_input退休原段，suspend_media再停输出；resume_media后不能沿原inputId重新采集。
    if (session_) {
        if (!input_id_.empty())
            session_->stop_input(session_id_, input_id_);
        session_->suspend_media(session_id_);
    }
    requests_.suspend_session(session_id_, now);
    requests_.cancel_session({}, now);  // 会话外能力请求独立终态，不给业务会话设置驱逐期限。
    pending_.clear();
    want_capture_ = false;
    recovery_.input_acked = false;
    state_ = VoiceState::Recovering;
    return begun;
}

// control_reconnected 由宿主真实新连接SUBACK触发，只重报能力；原sessionCreate不会再次发送。
Result VoiceClient::control_reconnected(std::uint64_t now) {
    if (!control_recovering() || recovery_expired(now))
        return Result::Stale;
    Envelope report;
    const auto token = next_id();
    phase_observed_.reset();
    if (capability_report(token, report) != WireResult::Ok ||
        control_->reporting(token, capability_.revision, now) != Result::Applied ||
        !send(report, {}, now)) {
        release(VoiceState::Failed, now);
        return Result::Busy;
    }
    return Result::Applied;
}

// receive_control_snapshot 整批核原在途token后替换水位；只允许新audioStart ACK开启真实采集。
Result VoiceClient::receive_control_snapshot(IncomingMessage message, std::uint64_t now,
                                             std::uint64_t wall) {
    AIControlSnapshot snapshot;
    if (!control_recovering() || message.topic != down_topic_ ||
        decode_ai_control_snapshot(WireRoute::Signaling, message.body, selected_, snapshot) !=
            WireResult::Ok)
        return Result::Invalid;
    const auto pending = std::find_if(pending_.begin(), pending_.end(), [&](const auto& item) {
        return item.method == "sessionSync" && item.token == snapshot.envelope.token;
    });
    if (pending == pending_.end())
        return Result::Stale;
    const auto accepted = control_->accept(snapshot, now, wall);
    if (accepted == Result::Unsupported && snapshot.ended) {
        release(VoiceState::Idle, now);
        return Result::Applied;
    }
    if (accepted != Result::Applied) {
        if (control_->phase() == AIControlPhase::Failed)
            release(VoiceState::Failed, now);
        return accepted;
    }
    if (!requests_.acknowledge(
            {snapshot.envelope.token, session_id_, "sessionSync", 200, up_topic_}, now)) {
        release(VoiceState::Failed, now);
        return Result::Stale;
    }
    pending_.erase(pending);
    if (session_ && session_->resume_media(session_id_) != Result::Applied) {
        release(VoiceState::Failed, now);
        return Result::DriverFailed;
    }
    // 完整快照确认原会话仍有效，但原回复音频不重播；SIG/MSG各自保持独立不回退水位。
    event_seq_ = snapshot.event_watermark;
    if (selected_.transport == VoiceTransport::None && snapshot.active_responses.empty())
        text_pending_ = false;
    state_ = VoiceState::Established;
    if (capability_.ai_endpoint_control_sync && endpoint_ && endpoint_->candidate) {
        endpoint_->binding = snapshot.binding;
        endpoint_->phase = AIEndpointPhase::AwaitingFocus;
        return Result::Applied; // 目标恢复不继承旧采集意图，唯本人明确新start及实际ACK。
    }
    const bool should_capture =
        control_->wants_capture() && selected_.transport != VoiceTransport::None;
    if (should_capture)
        return start_input(now);  // 只发送新ID，真正start_capture仍在原成功ACK路径。
    return Result::Applied;
}
}  // namespace ur::device
