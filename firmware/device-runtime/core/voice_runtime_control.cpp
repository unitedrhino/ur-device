// 文件说明：原AI运行时独占网络的显式重鉴权接入；固定15秒、原会话不重建，凭据仅交网络HAL。
#include "ur/device/voice_runtime.hpp"
#include "voice_runtime_endpoint_state.hpp"

namespace ur::device {
// suspend_control 只处理显式已协商的原AI，不能替共享宿主或活附属会话偷偷重开网络。
bool VoiceRuntime::suspend_control(std::uint64_t now, std::uint64_t wall) {
    const bool target = client_.capability_.ai_endpoint_control_sync && endpoint_ &&
                        endpoint_->listening && endpoint_->port &&
                        endpoint_->port->control_recovery_supported();
    if (borrowed_connection_ || !client_.control_sync_enabled() ||
        (state_ != RuntimeState::Running && !client_.control_recovering()) || !files_released() ||
        !live_released() || (!target && !focus_released()))
        return false;
    if (target && !client_.control_recovering()) {
        const auto scope = client_.candidate_focus_scope();
        // 首次目标恢复必须从真实已获权Active进入；快照后未知新租约再次断线不能洗成空闲。
        if (!scope || client_.endpoint_phase() != AIEndpointPhase::Active ||
            !client_.endpoint_establish_gate() || !focus_ ||
            !endpoint_->port->suspend_control(*scope, now, wall)) return false;
        if (!focus_->close_scope(scope->session_id)) return false;
        endpoint_->pending.reset();
    }
    const auto suspended = client_.control_disconnected(now, message_sequence_);
    if (suspended != Result::Applied && suspended != Result::Duplicate)
        return false;
    if (endpoint_source_) endpoint_source_->suspend(now); // 旧接续在途身份不得跨socket恢复重传。
    if (client_tools_)
        client_tools_->close_session(client_.session_id(),
                                     now);  // 旧授权及原结果不能迁移到新socket。
    if (actions_)
        actions_->disconnected();  // 原持久动作账本仍保留，不重复执行已受理副作用。
    mqtt_.disconnect();
    signaling_subscription_ = message_subscription_ = action_subscription_ = 0;
    signaling_ready_ = message_ready_ = action_ready_ = false;
    contents_.clear();
    text_results_.clear();
    content_response_open_ = false;
    content_response_cancelled_ = true;
    state_ = RuntimeState::ControlRecovering;
    return true;
}

// resume_connection
// 不调用start/close；只重新鉴权当前唯一网络，原AI身份、counter、水位和媒体配置不变。
Result VoiceRuntime::resume_connection(const MqttConfig& config, std::uint64_t now) {
    if (borrowed_connection_ || state_ != RuntimeState::ControlRecovering ||
        !client_.control_recovering())
        return Result::Unsupported;
    if (!now_ || now < *now_) {
        fail(now, RuntimeFailure::ClockRollback);
        return Result::Stale;
    }
    client_.tick(now);
    if (!client_.control_recovering()) {
        fail(now, RuntimeFailure::ConnectionDeadline);
        return Result::Stale;
    }
    if (!mqtt_.connect(config)) {
        // 本地新建网络失败仍保留第一次固定截止；不把失败当作业务会话已经终止。
        return Result::DriverFailed;
    }
    now_ = now;
    phase_at_ = now;
    state_ = RuntimeState::Connecting;
    return Result::Applied;
}
}  // namespace ur::device
