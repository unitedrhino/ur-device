// 信令状态机：能力确认→会话分配→真实建立事件；语音须双向媒体探测，none纯文字完全不装配媒体。
// 控制ACK不替代媒体事实，纯文字以原inputId/respId和内容终态控制逐轮背压。
#include "ur/device/voice_client.hpp"
#include "ur/device/ai_control_recovery.hpp"
#include <algorithm>
#include <limits>
#include <utility>
#include "ur/device/connect_trace.hpp"
#include "ur/device/focus_endpoint.hpp"
#include "voice_endpoint_state.hpp"
#include "voice_failure_storage.hpp"
#include "udp_reply_receipt_state.hpp"

namespace ur::device {
namespace {
// 连接提速测量标签：握手/媒体应答阶段的毫秒打点统一使用该 tag。
constexpr const char* kConnectTag = "ur_conn_cli";
}  // namespace
namespace {
// 媒体恢复窗口从首次断连起点固定计时，重复断连或握手不能续期；平台按原会话代次+1
// 接受同一次恢复内的新offer，窗口须覆盖候选重建、再收集与应答往返的真实耗时。
// 取45秒覆盖真实链路恢复：真机定向洪泛（TASK-143）实测设备WiFi抖动后下行TCP积压约30秒，
// 恢复ACK（平台毫秒级生成）只能在积压释放后到达；25秒窗口会让设备在ACK到达前先行失败。
constexpr std::uint64_t kMediaRecoveryWindowMs = 45000;
// 恢复期请求应答预算的额外宽限：让恢复窗口自身的固定截止先于请求超时收口，
// 避免普通30秒请求预算把仍在窗口内的迟到ACK判为过期。
constexpr std::uint64_t kRecoveryRequestGraceMs = 5000;
// 恢复期offer发出后仍未连通的补发起点：残余拥塞下首轮候选收集可能残缺或应答迟到，
// 到点仍无双向连通时重建下一代Peer补发一次，本恢复期上限一次。
constexpr std::uint64_t kRecoveryOfferRetryAfterMs = 6000;
}  // namespace
// voice_failure_method 只按原固定控制方法分类，无身份/正文/未知名称输出。
VoiceFailureMethod voice_failure_method(std::string_view method) {
    if (method == "capabilityReport")
        return VoiceFailureMethod::CapabilityReport;
    if (method == "sessionCreate")
        return VoiceFailureMethod::SessionCreate;
    if (method == "sessionAccept")
        return VoiceFailureMethod::SessionAccept;
    if (method == "sessionReject")
        return VoiceFailureMethod::SessionReject;
    if (method == "mediaState")
        return VoiceFailureMethod::MediaState;
    if (method == "mediaOffer")
        return VoiceFailureMethod::MediaOffer;
    if (method == "audioStart")
        return VoiceFailureMethod::AudioStart;
    if (method == "audioStop")
        return VoiceFailureMethod::AudioStop;
    if (method == "respCancel")
        return VoiceFailureMethod::RespCancel;
    if (method == "sessionEnd")
        return VoiceFailureMethod::SessionEnd;
    if (method == "inputSend")
        return VoiceFailureMethod::InputSend;
    if (method == "sessionSync")
        return VoiceFailureMethod::SessionSync;
    if (method == "endpointControl")
        return VoiceFailureMethod::EndpointControl;
    return method.empty() ? VoiceFailureMethod::None : VoiceFailureMethod::Unknown;
}
// VisionState只为视觉会话分配：原Profile不可变，最多32原命令/回执，不常驻媒体或复制语音状态机。
// 活会话不淘汰原token，满时拒绝新发布；最后一槽保留停止，任何背压都不丢原实际事实。
struct VoiceClient::VisionState {
    // Record保留原规范化命令及实际回执；未完成/已取消不等于摄像头停止。
    struct Record {
        VoiceVisionCommand command; // 原完整请求，重放必须全部相同，不能只按token比较。
        std::string receipt;        // 已确定回执的原字节；背压不改写成功或失败。
        std::uint64_t generation = 0; // 本地HAL原操作映射，不向云端增加字段。
        std::uint64_t started = 0;    // 原受理10秒固定起点，不随重试/tick延长。
        bool pending = false;        // HAL受理尚无实际完成事实。
        bool dirty = false;          // 仅待发布原回执，不再调用HAL。
    };
    const VoiceVisionProfile profile; // 与原创建完全相同的只读绑定。
    std::array<Record, 32> records{};  // 上限含原停止清理槽；终态整体释放，不淘汰仍有效原token。
    std::size_t count = 0;            // 原会话已记录条数，不因失败或重放减少。
    std::uint64_t current = 0;        // 当前HAL意图代次；历史成功不使旧代次恢复。
    std::optional<std::uint64_t> clock; // 单调时钟回退永久禁止本会话再次发布。
    bool clock_failed = false;       // 不以倒退时钟延长原10秒截止。
    // 构造只冻结已校验Profile；不打开相机，不从记录数推断轨active。
    explicit VisionState(const VoiceVisionProfile& value) : profile(value) {}
    // complete保存原事实/有限失败；503deviceRejected这里只表示取消未完成发布，非用户拒绝。
    static bool complete(Record& record, int code, std::string_view state, std::string_view reason) {
        Envelope reply;
        if (voice_vision_receipt(record.command, code, state, reason, reply) != WireResult::Ok ||
            encode_envelope(reply, record.receipt) != DecodeResult::Ok)
            return false;
        record.pending = false;
        record.dirty = true;
        return true;
    }
};
VoiceClient::VoiceClient(PublishPort& publisher, VoiceMediaPort& media, std::string up_topic,
                         std::string down_topic, VoiceCapability capability,
                         std::string instance_id, FocusEndpoint* focus)
    : VoiceClient(publisher, &media, std::move(up_topic), std::move(down_topic), capability,
                  std::move(instance_id), focus) {}
VoiceClient::VoiceClient(PublishPort& publisher, std::string up_topic, std::string down_topic,
                         VoiceCapability capability, std::string instance_id)
    : VoiceClient(publisher, nullptr, std::move(up_topic), std::move(down_topic), capability,
                  std::move(instance_id), nullptr) {}
VoiceClient::VoiceClient(PublishPort& publisher, VoiceMediaPort* media, std::string up_topic,
                         std::string down_topic, VoiceCapability capability,
                         std::string instance_id, FocusEndpoint* focus)
    : publisher_(publisher),
      focus_(focus),
      media_(media),
      requests_(publisher),
      up_topic_(std::move(up_topic)),
      down_topic_(std::move(down_topic)),
      instance_id_(std::move(instance_id)),
      capability_(capability) {
    if (media_)
        session_.emplace(*media_);
}
VoiceClient::~VoiceClient() {
    if (session_)
        session_->close();
    if (media_)
        media_->close();
}
// capability_report 拒绝错设备焦点端点，恢复报文保持同一完整资源声明，不自造idle基线。
WireResult VoiceClient::capability_report(std::string token, Envelope& out) const {
    if ((capability_.udp_reply_start_receipt &&
         (!media_ || !media_->udp_reply_start_receipt_ready())) ||
        !vision_profile_current()) {
        out = {};
        return WireResult::Invalid;
    }
    // 人际通话可独立于 AI 视觉使用相机；两者均有时沿用原 AI 视觉快照。
    const auto* profile = !media_ ? nullptr : capability_.ai_vision ? media_->vision_profile()
                         : capability_.human_call ? media_->call_video_profile() : nullptr;
    const auto result = capability_command(std::move(token), capability_, out, profile);
    if (result != WireResult::Ok || !focus_)
        return result;
    if (!focus_->matches(publisher_, up_topic_, down_topic_)) {
        out = {};
        return WireResult::WrongRoute;
    }
    if (endpoint_ && endpoint_->candidate && capability_.ai_endpoint_control_sync &&
        control_recovering())
        return focus_->recovery_capability(out, session_id_, out);
    return focus_->capability(out, out);
}
// vision_profile_current只接受实际HAL与原不可变配置相同，不从旧camera或附件许可推断取帧权。
bool VoiceClient::vision_profile_current() const {
    if (!capability_.ai_vision)
        return true;
    const auto* current = media_ ? media_->vision_profile() : nullptr;
    if (!vision_)
        return !vision_profile_binding(current).empty();
    // 快照已经完整校验；活会话只比较原字段，避免每帧重复分配JSON验证树。
    return current && current->ready && vision_->profile.camera_id == current->camera_id &&
           vision_->profile.video_track == current->video_track &&
           vision_->profile.max_publishers == current->max_publishers;
}
std::string VoiceClient::next_id() {
    if (counter_ == std::numeric_limits<std::uint64_t>::max())
        return {};
    return instance_id_ + "-" + std::to_string(++counter_);
}
bool VoiceClient::send(const Envelope& envelope, std::string object, std::uint64_t now) {
    std::string body;
    if (pending_.size() >= 32 || encode_envelope(envelope, body) != DecodeResult::Ok)
        return false;
    const auto& topic = envelope.method == "inputSend" ? content_up_ : up_topic_;
    if (requests_.submit({envelope.token, envelope.session, envelope.method, topic, body}, now) !=
        SubmitResult::Accepted)
        return false;
    pending_.push_back({envelope.token, envelope.method, std::move(object)});
    return true;
}
bool VoiceClient::control(std::string method, std::string object, std::uint64_t now) {
    Envelope envelope;
    auto packet_sequence =
        selected_.transport == VoiceTransport::Udp && method == "audioStart" ? input_first_ : 0U;
    if (method == "audioStop" && selected_.transport == VoiceTransport::Udp) {
        const auto next = media_->next_input_sequence();
        if (next == 0 || next < input_first_)
            return false;
        packet_sequence = next == input_first_ ? 0U : next - 128U;
    }
    if (voice_control(next_id(), session_id_, std::move(method), object, stream_watermark_,
                      envelope, packet_sequence, selected_.transport) != WireResult::Ok)
        return false;
    return send(envelope, std::move(object), now);
}
Result VoiceClient::connect(std::uint64_t now) { return connect_mode(now, false); }
// connect_shared沿相同严格身份/能力校验，建会话请求仍保留原token和所有应答关联。
Result VoiceClient::connect_shared(std::uint64_t now) { return connect_mode(now, true); }
// connect_mode校验原设备和媒体装配，shared只借已确认统一能力，不伪造建会话或媒体完成。
Result VoiceClient::connect_mode(std::uint64_t now, bool shared) {
    if (state_ != VoiceState::Idle && state_ != VoiceState::Failed)
        return Result::Busy;
    if ((capability_.transport == VoiceTransport::None) != (media_ == nullptr))
        return Result::Invalid;
    if (instance_id_.empty() || instance_id_.size() > 64 ||
        instance_id_.find_first_not_of(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") !=
            std::string::npos)
        return Result::Invalid;
    constexpr std::string_view up = "$thing/up/signaling/";
    constexpr std::string_view down = "$thing/down/signaling/";
    if (up_topic_.compare(0, up.size(), up) != 0 ||
        down_topic_.compare(0, down.size(), down) != 0 ||
        up_topic_.substr(up.size()) != down_topic_.substr(down.size()))
        return Result::Invalid;
    const auto identity = up_topic_.substr(up.size());
    const auto slash = identity.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 == identity.size() ||
        identity.find('/', slash + 1) != std::string::npos ||
        identity.find_first_of("+#\0", 0, 3) != std::string::npos)
        return Result::Invalid;
    content_up_ = "$thing/up/message/" + identity;
    content_down_ = "$thing/down/message/" + identity;
    // 独立快照在任何上行前冻结；能力更新只能用于下一会话，不换本次三轨的原源。
    // 仅宿主显式请求的会话冻结aiVision三轨；设备本地语音缺省两轨，能力上报不受影响。
    if (capability_.ai_vision && vision_requested_) {
        const auto* profile = media_ ? media_->vision_profile() : nullptr;
        if (vision_profile_binding(profile).empty())
            return Result::Unsupported;
        vision_ = std::make_unique<VisionState>(*profile);
    }
    Envelope envelope;
    if (capability_report(next_id(), envelope) != WireResult::Ok) {
        vision_.reset();
        return Result::Unsupported;
    }
    rejection_.reset();
    capability_recovered_ = false;
    capability_conflicts_ = 0;
    phase_observed_.reset();
    shared_connection_ = shared;
    if (shared &&
        create_voice_command(next_id(), capability_, envelope, vision_ ? &vision_->profile : nullptr) != WireResult::Ok) {
        vision_.reset();
        return Result::Unsupported;
    }
    UR_CONNECT_TRACE(kConnectTag, "stage=%s token=%s t=%llu",
                     shared ? "session_create_sent" : "capability_report_sent",
                     envelope.token.c_str(), static_cast<unsigned long long>(now));
    if (!send(envelope, {}, now)) {
        vision_.reset();
        return Result::Busy;
    }
    state_ = shared ? VoiceState::Creating : VoiceState::Reporting;
    UR_CONNECT_TRACE(kConnectTag, "stage=connect_state state=%d t=%llu", static_cast<int>(state_),
                     static_cast<unsigned long long>(now));
    return Result::Applied;
}
void VoiceClient::release(VoiceState state, std::uint64_t now, VoiceFailureCause cause,
                          VoiceFailureMethod method) {
    if (state == VoiceState::Failed && !recovery_.failure_terminal) {
        if (recovery_.failure_diagnostics_enabled &&
            !(cause == VoiceFailureCause::ActiveScopeLost && scope_failure()))
            failure_diagnostic_ = encode_voice_failure(cause, method);
        recovery_.failure_terminal = true;  // 失败首因只锁存一次，后继清理不覆盖原事实。
    }
    phase_observed_.reset();
    vision_cancel();
    if (session_)
        session_->close();
    if (media_)
        media_->close();
    requests_.cancel_session(session_id_, now);
    requests_.cancel_session({}, now);
    requests_.clear_pending_extension();  // 会话终结不得把恢复期的延长预算带入下一连接。
    pending_.clear();
    udp_reply_receipts_.reset();  // 断线/终态不能带原安装票据进入下一连接。
    selected_ = {};
    control_.reset();
    endpoint_.reset();
    vision_.reset();
    session_id_.clear();
    input_id_.clear();
    text_input_id_.clear();
    text_pending_ = false;
    stream_watermark_ = 0;
    response_id_.clear();
    reply_finished_ = false;
    offer_sent_ = answer_applied_ = false;
    offer_sent_at_ = 0;
    recovery_offer_retries_ = 0;
    recovery_.input_acked = recovery_.disconnected_acked = recovery_.response_cancel_sent = false;
    recovery_.none_terminal_pending = false;
    recovery_.focus_suspended = false;
    want_capture_ = media_reported_ = media_acked_ = established_event_ = false;
    event_seq_ = 0;
    state_ = state;
}
void VoiceClient::disconnected(std::uint64_t now) { release(VoiceState::Idle, now); }
// enable_failure_diagnostics 只开启未来的原失败；失败或清理后的重启不能补造首因。
void VoiceClient::enable_failure_diagnostics() {
    if (!recovery_.failure_terminal && state_ != VoiceState::Failed) {
        recovery_.failure_diagnostics_enabled = true;
        if (focus_)
            focus_->enable_scope_diagnostics();
    }
}
// scope_failure 未观测时为空；失败和清理不会根据终态猜测一个新原因。
std::optional<ScopeFailureCode> VoiceClient::scope_failure() const {
    return decode_scope_failure(failure_diagnostic_);
}
// failure_diagnostic 未观察时为空，不从Failed状态猜测实际入口。
std::optional<VoiceFailureDiagnostic> VoiceClient::failure_diagnostic() const {
    return decode_voice_failure(failure_diagnostic_);
}
Result VoiceClient::establish() {
    if (endpoint_ && endpoint_->candidate) {
        if (endpoint_->accept_acked && endpoint_->committed && media_reported_ && media_acked_ &&
            established_event_ && endpoint_->phase != AIEndpointPhase::Active)
            endpoint_->phase = AIEndpointPhase::AwaitingFocus;
        if (!endpoint_establish_gate())
            return Result::Applied;
    }
    if (state_ == VoiceState::Recovering && recovery_.disconnected_acked && media_reported_ &&
        media_acked_) {
        // 恢复标记不能替代实际Session；异常缺对象时拒绝，不重新开启采集。
        if (!session_)
            return Result::Unsupported;
        // 不重开Session、不重发audioStart；原已暂停输入必须继续暂停，旧回复永不重播。
        auto result = session_->resume_media(session_id_);
        if (result == Result::Applied && want_capture_ && recovery_.input_acked &&
            !session_->capturing())
            result = session_->start_input(session_id_, input_id_);
        if (result == Result::Applied) {
            state_ = VoiceState::Established;
            requests_.clear_pending_extension();  // 恢复完成即回到常规请求预算。
        }
        return result;
    }
    if (selected_.transport == VoiceTransport::None) {
        if (state_ == VoiceState::Negotiating && established_event_)
            state_ = VoiceState::Established;
        return Result::Applied;
    }
    if (state_ != VoiceState::Negotiating || !media_reported_ || !media_acked_ ||
        !established_event_)
        return Result::Applied;
    // None不构造音频会话；音频建立必须持有实际Session，不能由状态标记替代驱动对象。
    if (!session_)
        return Result::Unsupported;
    // 云端算法不影响设备半双工收发；fullDuplex 已由云端及 AEC 能力共同选择。
    const auto result =
        session_->open(session_id_, selected_.mode, Pipeline::Realtime, capability_.modes);
    if (result == Result::Applied) {
        state_ = VoiceState::Established;
        UR_CONNECT_TRACE(kConnectTag, "stage=voice_established transport=%d t=%llu",
                         static_cast<int>(selected_.transport), UR_CONNECT_MS());
    }
    if (result == Result::Applied && endpoint_ && endpoint_->candidate)
        endpoint_->phase = AIEndpointPhase::Active;
    return result;
}
Result VoiceClient::acknowledge(const VoiceEvent& event, std::uint64_t now) {
    const auto& envelope = event.envelope;
    if (!envelope.code)
        return Result::Invalid;
    const std::string method = envelope.method == "sessionCreated" ? "sessionCreate"
                               : envelope.method == "sessionEnded" ? "sessionEnd"
                               : envelope.method == "mediaAnswer"  ? "mediaOffer"
                                                                   : event.request_method;
    const auto found = std::find_if(pending_.begin(), pending_.end(), [&](const Pending& item) {
        return item.token == envelope.token && item.method == method;
    });
    if (found == pending_.end())
        return Result::Stale;
    const bool creating = method == "sessionCreate";
    const bool connection_report = method == "capabilityReport";
    if (!creating && !connection_report && envelope.session != session_id_)
        return Result::Stale;
    const bool success = *envelope.code == 200;
    if (success) {
        if (method == "capabilityReport" && event.revision != capability_.revision)
            return Result::Invalid;
        if ((method == "audioStart" || method == "audioStop" || method == "inputSend") &&
            event.input_id != found->object)
            return Result::Invalid;
        if (method == "respCancel" && event.response_id != found->object)
            return Result::Invalid;
        // None须取得原生产者实际取消状态；音频旧ACK缺省只代表请求受理，不能伪造终态。
        if (method == "respCancel" && selected_.transport == VoiceTransport::None &&
            event.state.empty())
            return Result::Invalid;
        if (method == "mediaState" &&
            (event.revision != stream_watermark_ || event.state != found->object))
            return Result::Invalid;
        if (creating) {
            const auto& chosen = event.media;
            if ((capability_.session_sync || capability_.ai_endpoint_handoff)
                    ? (event.endpoint_epoch == 0 ||
                       event.capability_revision != capability_.revision)
                    : (event.endpoint_epoch != 0 || event.capability_revision != 0))
                return Result::Invalid;
            if (chosen.udp_reply_start_receipt && !capability_.udp_reply_start_receipt)
                return Result::Invalid;
            // 平台只能选本次请求的原形态：请求视觉会话（vision_ 冻结）必须回 aiVision
            // 三轨，本地语音会话必须回两轨；设备能力与会话形态已解耦，不能按能力位强校验。
            if (chosen.ai_vision != (capability_.ai_vision && vision_ != nullptr) ||
                !vision_profile_current())
                return Result::Invalid;
            // 设备只报能力，平台选原交集；任何未声明的离散模态都不得安装或默认放行。
            // 例外：aiVision 会话的 text 模态来自平台模型协商（sessionCreated 回填），
            // 设备能力层与 aiVision 互斥不声明（valid_capability），不能按声明子集拒绝。
            if ((chosen.text_input && !capability_.text_input && !chosen.ai_vision) ||
                (chosen.image_input && !capability_.image_input) ||
                (chosen.file_input && !capability_.file_input))
                return Result::Invalid;
            if (chosen.transport == VoiceTransport::None) {
                if (capability_.transport != VoiceTransport::None ||
                    !(chosen.text_input || chosen.image_input || chosen.file_input) ||
                    !chosen.text_output || chosen.audio_output || !chosen.input_track.empty() ||
                    !chosen.output_track.empty())
                    return Result::Invalid;
            } else if ((chosen.mode == Mode::FullDuplex &&
                        (!capability_.modes.full_duplex || !capability_.modes.device_aec)) ||
                       (chosen.mode == Mode::HalfDuplex && !capability_.modes.half_duplex) ||
                       chosen.audio.sample_rate != capability_.audio.sample_rate ||
                       chosen.audio.channels != capability_.audio.channels ||
                       chosen.audio.frame_ms != capability_.audio.frame_ms ||
                       chosen.transport != capability_.transport || chosen.input_track != "mic-1" ||
                       chosen.output_track != "speaker-1")
                return Result::Invalid;
        }
        if (method == "mediaOffer" &&
            ((state_ != VoiceState::Negotiating && state_ != VoiceState::Recovering) ||
             selected_.transport != VoiceTransport::WebRTC || event.revision != stream_watermark_ ||
             event.answer_sdp.empty()))
            return Result::Invalid;
    }
    if (!requests_.acknowledge(
            {envelope.token,
             creating || connection_report ? std::string_view{} : std::string_view(session_id_),
             method, *envelope.code, method == "inputSend" ? content_up_ : up_topic_},
            now))
        return Result::Stale;
    const auto object = found->object;
    pending_.erase(found);
    if (!success) {
        // Broker上行可能早于平台上线状态更新；只对明确快照冲突有限退避，不修改能力或AEC。
        // 持续冲突仍失败，权限/非法参数及未知失败不得走此恢复路径。
        if (method == "capabilityReport" &&
            (state_ == VoiceState::Reporting ||
             (control_recovering() && control_->phase() == AIControlPhase::Reporting)) &&
            *envelope.code == 409 && event.reason == "capabilityNotAccepted" &&
            capability_conflicts_ < 2) {
            ++capability_conflicts_;
            phase_observed_ = now;
            return Result::Applied;
        }
        // 上线事件异步更新连接代次时平台可明确要求重报；仅恢复尚未分配媒体的创建请求。
        // 必须消费匹配的失败ACK后使用新token，不能重放上一连接能力或无限延长建连。
        if (!shared_connection_ && creating && state_ == VoiceState::Creating &&
            *envelope.code == 409 && event.reason == "capabilityReportRequired" &&
            !capability_recovered_) {
            capability_recovered_ = true;
            Envelope report;
            if (capability_report(next_id(), report) != WireResult::Ok || !send(report, {}, now)) {
                release(VoiceState::Failed, now);
                return Result::Busy;
            }
            state_ = VoiceState::Reporting;
            return Result::Applied;
        }
        rejection_ = VoiceRejection{method, *envelope.code, {}};
        // 只在已匹配原token/method的能力403记录两种稳定原因，不保存自由错误正文或改变权限拒绝行为。
        if (method == "capabilityReport" && *envelope.code == 403 &&
            (event.reason == "focusDeviceUnavailable" || event.reason == "requestDenied")) {
            rejection_->reason = event.reason;
        }
        if (method == "inputSend") {
            // 文字提问被平台暂拒（如视觉处理器未就绪的503）时清pending允许上层重发；
            // 保留会导致send_text永远Busy、视觉提问在attach后无法重试（真机定位）。
            if (*envelope.code == 503 || selected_.transport == VoiceTransport::None)
                text_pending_ = false;
            return *envelope.code == 409 ? Result::Busy : Result::Unsupported;
        }
        release(VoiceState::Failed, now, VoiceFailureCause::RequestRejected,
                voice_failure_method(method));
        return Result::Unsupported;
    }
    if (method == "capabilityReport") {
        UR_CONNECT_TRACE(kConnectTag, "stage=capability_report_ack revision=%llu t=%llu",
                         static_cast<unsigned long long>(event.revision),
                         static_cast<unsigned long long>(now));
        if (control_recovering()) {
            if (control_->capability_ack(envelope.token, event.revision, now) != Result::Applied) {
                release(VoiceState::Failed, now);
                return Result::Stale;
            }
            Envelope sync;
            if (control_->sync(next_id(), now, sync) != Result::Applied || !send(sync, {}, now)) {
                release(VoiceState::Failed, now);
                return Result::Busy;
            }
            return Result::Applied;
        }
        if (object == "endpointControlBaseline" && endpoint_control_rebinding()) {
            endpoint_->control_baseline_acked = true;
            endpoint_->baseline_retry.reset(); // 原成功ACK已关联，停止快调度但不补任何焦点事实。
            return Result::Applied; // 原恢复的能力ACK不能重置Listening/Idle或重开模型。
        }
        if (endpoint_ && endpoint_->listening) {
            state_ = VoiceState::Idle; // 仅监听，不能把能力ACK升级成sessionCreate。
            endpoint_->phase = AIEndpointPhase::Listening;
            return Result::Applied;
        }
        Envelope create;
        if (create_voice_command(next_id(), capability_, create, vision_ ? &vision_->profile : nullptr) != WireResult::Ok ||
            !send(create, {}, now)) {
            release(VoiceState::Failed, now);
            return Result::Busy;
        }
        UR_CONNECT_TRACE(kConnectTag, "stage=session_create_sent token=%s t=%llu",
                         create.token.c_str(), static_cast<unsigned long long>(now));
        state_ = VoiceState::Creating;
    } else if (creating) {
        UR_CONNECT_TRACE(kConnectTag, "stage=session_created session=%s transport=%d t=%llu",
                         envelope.session.c_str(), static_cast<int>(event.media.transport),
                         static_cast<unsigned long long>(now));
        session_id_ = envelope.session;
        selected_ = event.media;
        if (capability_.ai_endpoint_handoff) {
            endpoint_ = std::make_unique<AIEndpointState>();
            endpoint_->binding = {event.endpoint_epoch, event.capability_revision};
        }
        stream_watermark_ = selected_.transport == VoiceTransport::None ? 0 : 1;
        if (capability_.session_sync) {
            control_ = std::make_unique<AIControlRecovery>(
                session_id_, AIControlBinding{event.endpoint_epoch, event.capability_revision},
                stream_watermark_, selected_.transport);
            if (control_->phase() != AIControlPhase::Bound) {
                release(VoiceState::Failed, now);
                return Result::Invalid;
            }
        }
        if (selected_.transport != VoiceTransport::None &&
            !(selected_.ai_vision ? media_->configure_vision(selected_, vision_->profile)
                                  : media_->configure(selected_))) {
            release(VoiceState::Failed, now);
            return Result::DriverFailed;
        }
        phase_started_ = now;
        state_ = VoiceState::Negotiating;
    } else if (method == "mediaState") {
        if (object == "disconnected") {
            // 同一媒体许可内按序递增；ACK确认恢复准入，不替代新Peer连通或采集许可。
            recovery_.disconnected_acked = true;
            ++stream_watermark_;
            if (control_ &&
                control_->advance_media_revision(stream_watermark_) != Result::Applied) {
                release(VoiceState::Failed, now);
                return Result::Stale;
            }
            if (!media_->begin_recovery(stream_watermark_)) {
                release(VoiceState::Failed, now);
                return Result::DriverFailed;
            }
            return Result::Applied;
        }
        media_acked_ = true;
        const auto result = establish();
        if (result == Result::DriverFailed) release(VoiceState::Failed, now);
        return result;
    } else if (method == "mediaOffer") {
        if (!media_->apply_answer(event.answer_sdp)) {
            release(VoiceState::Failed, now);
            return Result::DriverFailed;
        }
        answer_applied_ = true;
        UR_CONNECT_TRACE(kConnectTag, "stage=media_answer_applied revision=%llu t=%llu",
                         static_cast<unsigned long long>(event.revision),
                         static_cast<unsigned long long>(now));
    } else if (method == "audioStart" && session_ && want_capture_ && object == input_id_) {
        recovery_.input_acked = true;
        recovery_.focus_suspended = false; // 只有新用户输入段的原成功ACK可离开本机焦点暂停。
        if (state_ == VoiceState::Recovering) return Result::Applied;
        UR_CONNECT_TRACE(kConnectTag, "stage=audio_start_ack input=%s t=%llu", object.c_str(),
                         static_cast<unsigned long long>(now));
        const auto result = session_->start_input(session_id_, object);
        if (result != Result::Applied)
            release(VoiceState::Failed, now);
        return result;
    } else if (method == "sessionEnd") {
        release(VoiceState::Idle, now);
    } else if (method == "respCancel" && selected_.transport == VoiceTransport::None) {
        // 跨Topic可先ACK后Done；只释放输入背压，保留原input/resp的唯一终态展示资格。
        recovery_.none_terminal_pending = text_pending_;
        text_pending_ = false;
    }
    return Result::Applied;
}
Result VoiceClient::receive(IncomingMessage message, std::uint64_t now) {
    return receive(message, now, 0);
}
// reply_start_admission提取原Start三分支及已有会话轨检查；不读取时钟/改账本/调用HAL。
Result VoiceClient::reply_start_admission(const VoiceEvent& event) const {
    if (!session_ || state_ != VoiceState::Established || event.revision != stream_watermark_ ||
        event.track_id != selected_.output_track || event.response_id == response_id_ ||
        (control_ && control_->retired_response(event.response_id)))
        return Result::Stale;
    if (session_->playing())
        return Result::Busy;
    if (event.udp_boundary != (selected_.transport == VoiceTransport::Udp) ||
        (event.udp_boundary && event.reply_nonce == selected_.nonce))
        return Result::Invalid;
    return Result::Applied;
}
// receive 双时钟只对控制恢复快照必需；旧普通事件不因宿主缺省墙钟改变行为。
Result VoiceClient::receive(IncomingMessage message, std::uint64_t now, std::uint64_t wall) {
    if (recovery_expired(now)) return Result::Stale;
    if (!vision_profile_current() && state_ != VoiceState::Idle && state_ != VoiceState::Failed) {
        release(VoiceState::Failed, now, VoiceFailureCause::VisionProfileInvalid);
        return Result::DriverFailed;
    }
    if (message.topic != down_topic_ && message.topic != content_down_)
        return Result::Invalid;
    if (const auto result = endpoint_receive(message, now, wall))
        return *result;
    VoiceEvent event;
    const auto route = message.topic == content_down_ ? WireRoute::Message : WireRoute::Signaling;
    if (control_recovering()) {
        if (udp_reply_receipts_)
            udp_reply_receipts_->retire();  // 旧socket成功事实不能在快照恢复后重发200。
        Envelope control_event;
        if (route != WireRoute::Signaling ||
            decode_envelope(message.body, control_event) != DecodeResult::Ok)
            return Result::Stale;
        if (control_event.method == "sessionSnapshot")
            return receive_control_snapshot(message, now, wall);
        if (control_event.method != "requestAck" && control_event.method != "sessionEnded" &&
            control_event.method != "sessionError")
            return Result::Stale;
    }
    if (vision_ && route == WireRoute::Signaling) {
        VoiceVisionCommand vision_command;
        if (decode_voice_vision_control(route, message.body, vision_command) == WireResult::Ok)
            return vision_control(vision_command, now);
    }
    if (decode_voice_event(route, message.body, event, vision_ ? &vision_->profile : nullptr) != WireResult::Ok)
        return Result::Invalid;
    if (state_ == VoiceState::Idle || state_ == VoiceState::Failed)
        return Result::Stale;
    if (event.envelope.code && event.envelope.method != "sessionError")
        return acknowledge(event, now);
    if (event.envelope.session != session_id_ || session_id_.empty() ||
        !event.envelope.event_sequence)
        return Result::Stale;
    const auto sequence = *event.envelope.event_sequence;
    if (selected_.udp_reply_start_receipt && event.envelope.method == "respAudioStart" &&
        udp_reply_receipts_ && udp_reply_receipts_->shared_managed &&
        !udp_reply_receipts_->shared_owned && event.revision == stream_watermark_ &&
        event.track_id == selected_.output_track) {
        // 失权拒绝只记录新鲜Start；旧序号晚包不得为未来合法新工作预埋拒绝摘要。
        if (sequence <= event_seq_)
            return Result::Stale;
        udp_reply_receipts_->reject_shared(event, now);
        event_seq_ = std::max(event_seq_,
                              sequence);  // 永久/Held/尚未Owned的合法原Start不可复权后换seq补播。
        return Result::Stale;
    }
    if (selected_.udp_reply_start_receipt && event.envelope.method == "respAudioStart" &&
        udp_reply_receipts_) {
        const auto duplicate = udp_reply_receipts_->inspect(event, now);
        if (duplicate != Result::Applied) {
            if (duplicate == Result::Duplicate)
                reply_receipt_tick(now);
            return duplicate;  // 原token改序号/nonce也不能绕过旧票据或再次安装。
        }
    }
    if (sequence <= event_seq_)
        return Result::Stale;
    const auto& method = event.envelope.method;
    if (method == "sessionError") {
        if (event.revision != 0 && event.revision != stream_watermark_) return Result::Stale;
        if (selected_.transport == VoiceTransport::None && !event.retryable &&
            ((event.scope == "response" && event.response_id == response_id_) ||
             (event.scope == "input" && event.input_id == text_input_id_)))
            text_pending_ = false;
        if (event.scope == "response" && session_) {
            if (udp_reply_receipts_ && udp_reply_receipts_->response == event.response_id)
                udp_reply_receipts_->retire();
            session_->cancel_response(session_id_, event.response_id);
        }
        if (event.scope == "input" && event.input_id == input_id_) {
            want_capture_ = false;
            recovery_.input_acked = false;
            if (session_)
                session_->stop_input(session_id_, input_id_);
        }
        // 工具错误仅属于对应调用；不能使整个会话或其他回复失效。
        event_seq_ = sequence;
        if (event.scope == "session" && !event.retryable)
            release(VoiceState::Failed, now, VoiceFailureCause::SessionError);
        return Result::Applied;
    }
    if (method == "sessionEnded") {
        release(VoiceState::Idle, now);
        return Result::Applied;
    }
    if (method == "sessionEstablished") {
        const bool no_media = selected_.transport == VoiceTransport::None;
        if (state_ != VoiceState::Negotiating || event.revision != (no_media ? 0U : 1U) ||
            (!no_media && !media_reported_))
            return Result::Stale;
        UR_CONNECT_TRACE(kConnectTag, "stage=session_established_recv revision=%llu t=%llu",
                         static_cast<unsigned long long>(event.revision),
                         static_cast<unsigned long long>(now));
        // 候选没有sessionCreated；只在新feature下从本公开腿正式建立事实安装恢复绑定。
        if (endpoint_ && endpoint_->candidate && capability_.ai_endpoint_control_sync) {
            if (!event.endpoint_epoch || event.capability_revision != capability_.revision)
                return Result::Invalid;
            endpoint_->binding = {event.endpoint_epoch, event.capability_revision};
            control_ = std::make_unique<AIControlRecovery>(session_id_, endpoint_->binding,
                                                          stream_watermark_, selected_.transport);
            if (control_->phase() != AIControlPhase::Bound) return Result::Invalid;
        } else if (event.endpoint_epoch || event.capability_revision) {
            return Result::Invalid; // 未协商目标不接受新字段，不能凭下行单方面升级断连行为。
        }
        event_seq_ = sequence;
        established_event_ = true;
        return establish();
    }
    // 新Peer确认前开始的回复整条不播放；记录同代Start水位，QoS重投不能在ACK后复活旧Start。
    if (state_ == VoiceState::Recovering && method == "respAudioStart" &&
        event.revision == stream_watermark_ && event.track_id == selected_.output_track) {
        event_seq_ = sequence;
        return Result::Stale;
    }
    // 新安装合同的候选尚无完整组许可时退休同代Start水位；接管后不能借原QoS字节补播。
    if (selected_.udp_reply_start_receipt && endpoint_ && endpoint_->candidate &&
        method == "respAudioStart" && event.revision == stream_watermark_ &&
        event.track_id == selected_.output_track && !endpoint_establish_gate()) {
        event_seq_ = sequence;
        return Result::Stale;
    }
    if (!session_ || state_ != VoiceState::Established || event.revision != stream_watermark_ ||
        event.track_id != selected_.output_track)
        return Result::Stale;
    Result result = Result::Stale;
    if (method == "respAudioStart") {
        if (recovery_.focus_suspended) {
            event_seq_ = sequence;
            return Result::Stale; // 无原采播许可时只退休迟到事件，不能配置nonce或使旧模型回复复活。
        }
        // 先检查协议状态，再安装 nonce，避免陈旧事件改变仍在播放的回复边界。
        const auto admitted = reply_start_admission(event);
        if (admitted != Result::Applied)
            return admitted;
        if (selected_.udp_reply_start_receipt) {
            if (!udp_reply_receipts_)
                udp_reply_receipts_ = std::make_unique<UDPReplyReceiptState>();
            const auto staged = udp_reply_receipts_->stage(event, now);
            if (staged != Result::Applied)
                return staged;  // 摘要满/背压不安装新回复，不杀原会话。
        }
        if (selected_.mode == Mode::HalfDuplex && (session_->capturing() || want_capture_)) {
            if (selected_.udp_reply_start_receipt) {
                // 原停止请求准入失败保持可重试；不清采集意图、不释放会话、不刷新原Start截止。
                if (!control("audioStop", input_id_, now))
                    return Result::Busy;
                want_capture_ = false;
                recovery_.input_acked = false;
                session_->stop_input(session_id_, input_id_);
            } else {
                const auto stopped = stop_input(now);
                if (stopped != Result::Applied)
                    return stopped;
            }
        }
        if (!media_->prepare_reply(event.reply_nonce, event.first_packet_sequence)) {
            release(VoiceState::Failed, now);
            return Result::DriverFailed;
        }
        result = session_->begin_response(session_id_, event.response_id, sequence);
        if (result == Result::Applied) {
            response_id_ = event.response_id;
            recovery_.response_cancel_sent = false;
            reply_finished_ = false;
            if (udp_reply_receipts_) {
                udp_reply_receipts_->installed = true;  // 同步HAL与Session原播放对象都已实际安装。
                udp_reply_receipts_->dirty = true;
                reply_receipt_tick(now);
            }
        } else if (udp_reply_receipts_)
            udp_reply_receipts_->retire();
    } else if (method == "respAudioDone") {
        if (event.response_id != response_id_)
            return Result::Stale;
        if (!session_->playing() || reply_finished_) {
            event_seq_ = sequence;
            return Result::Duplicate;
        }
        if (event.udp_boundary != (selected_.transport == VoiceTransport::Udp))
            return Result::Invalid;
        if (!media_->finish_reply(event.last_packet_sequence)) {
            release(VoiceState::Failed, now);
            return Result::DriverFailed;
        }
        result = session_->network_done(session_id_, event.response_id);
        if (result == Result::Applied)
            reply_finished_ = true;
    } else if (method == "audioSpeechInterrupted") {
        if (udp_reply_receipts_ && udp_reply_receipts_->response == event.response_id)
            udp_reply_receipts_->retire();
        result = session_->cancel_response(session_id_, event.response_id);
        if (result == Result::Applied)
            ++interrupted_playbacks_;
    }
    if (result == Result::Applied || result == Result::Duplicate)
        event_seq_ = sequence;
    if (result == Result::DriverFailed)
        release(VoiceState::Failed, now);
    return result;
}
Result VoiceClient::media_ready(std::uint64_t now) {
    if (recovery_expired(now)) return Result::Stale;
    if (!vision_profile_current()) {
        release(VoiceState::Failed, now);
        return Result::DriverFailed;
    }
    if (capability_.transport == VoiceTransport::None || !session_)
        return Result::Unsupported;
    if (state_ != VoiceState::Negotiating && state_ != VoiceState::Recovering) return Result::Stale;
    if (state_ == VoiceState::Recovering && !recovery_.disconnected_acked) return Result::Stale;
    if (media_reported_)
        return Result::Duplicate;
    if (selected_.transport == VoiceTransport::WebRTC && !answer_applied_)
        return Result::Stale;
    if (!control("mediaState", "connected", now))
        return Result::Busy;
    UR_CONNECT_TRACE(kConnectTag, "stage=media_ready_sent t=%llu",
                     static_cast<unsigned long long>(now));
    media_reported_ = true;
    return Result::Applied;
}
Result VoiceClient::media_offer_ready(std::string_view sdp, std::uint64_t now) {
    if (recovery_expired(now)) return Result::Stale;
    if (!vision_profile_current()) {
        release(VoiceState::Failed, now);
        return Result::DriverFailed;
    }
    if ((state_ != VoiceState::Negotiating && state_ != VoiceState::Recovering) ||
        selected_.transport != VoiceTransport::WebRTC ||
        (state_ == VoiceState::Recovering && !recovery_.disconnected_acked))
        return Result::Stale;
    if (offer_sent_)
        return Result::Duplicate;
    Envelope offer;
    if (media_offer_command(next_id(), session_id_, sdp, stream_watermark_, offer) !=
        WireResult::Ok)
        return Result::Invalid;
    if (!send(offer, {}, now))
        return Result::Busy;
    offer_sent_ = true;
    offer_sent_at_ = now;
    UR_CONNECT_TRACE(kConnectTag, "stage=media_offer_sent bytes=%d t=%llu",
                     static_cast<int>(sdp.size()), UR_CONNECT_MS());
    return Result::Applied;
}
// recovery_expired所有恢复入口共享首次25秒与单调观察；重复断连或握手不能延长窗口。
bool VoiceClient::recovery_expired(std::uint64_t now) {
    if (control_recovering()) {
        if (control_->expired(now)) {
            release(VoiceState::Failed, now, VoiceFailureCause::MediaRecoveryDeadline);
            return true;
        }
        return false;
    }
    if (state_ != VoiceState::Recovering) return false;
    if (!phase_observed_ || now < *phase_observed_ ||
        now - phase_started_ >= kMediaRecoveryWindowMs) {
        release(VoiceState::Failed, now, VoiceFailureCause::MediaRecoveryDeadline);
        return true;
    }
    phase_observed_ = now;
    return false;
}
// media_disconnected物理停流先于报告；原输入意图保留，显式暂停/取消仍可在恢复窗口生效。
Result VoiceClient::media_disconnected(std::uint64_t now) {
    if (recovery_expired(now)) return Result::Stale;
    if (selected_.transport != VoiceTransport::WebRTC || !session_) return Result::Unsupported;
    if (state_ == VoiceState::Recovering) return Result::Duplicate;
    if (state_ != VoiceState::Established) return Result::Stale;
    // 首阶段未装配视觉三轨恢复；不能把音频Peer恢复许可扩成重新发布摄像头。
    if (selected_.ai_vision) {
        release(VoiceState::Failed, now);
        return Result::Unsupported;
    }
    if (stream_watermark_ >= 9007199254740991ULL) {
        release(VoiceState::Failed, now);
        return Result::Invalid;
    }
    const auto result = session_->suspend_media(session_id_);
    if (result != Result::Applied) return result;
    phase_started_ = now;
    phase_observed_ = now;
    offer_sent_ = answer_applied_ = media_reported_ = media_acked_ = recovery_.disconnected_acked =
        false;
    offer_sent_at_ = 0;
    recovery_offer_retries_ = 0;
    state_ = VoiceState::Recovering;
    // 恢复期请求预算与窗口同源：下行积压（真机实测约30秒）内的迟到ACK必须仍能关联原请求，
    // 否则设备在窗口未耗尽前就被普通30秒预算判死；恢复完成或释放时清除。
    requests_.extend_pending(now + kMediaRecoveryWindowMs + kRecoveryRequestGraceMs);
    if (!control("mediaState", "disconnected", now)) {
        release(VoiceState::Failed, now);
        return Result::Busy;
    }
    return Result::Applied;
}
Result VoiceClient::start_input(std::uint64_t now) {
    if (!vision_profile_current()) {
        release(VoiceState::Failed, now);
        return Result::DriverFailed;
    }
    if (capability_.transport == VoiceTransport::None || !session_)
        return Result::Unsupported;
    if (state_ != VoiceState::Established)
        return Result::Stale;
    if (endpoint_ && endpoint_->candidate && endpoint_->phase != AIEndpointPhase::Active)
        return Result::Stale; // 目标恢复快照/基线不代替新完整组实际取得。
    if (want_capture_ || (selected_.mode == Mode::HalfDuplex && session_->playing()))
        return Result::Busy;
    const auto input = next_id();
    input_first_ = selected_.transport == VoiceTransport::Udp ? media_->next_input_sequence() : 0;
    /* 先提前打开采集队列再发 audioStart：WebRTC 驱动在 arm 阶段只缓冲不上行，
     * ACK 往返期间的用户语音不会丢；不支持提前采集的驱动返回 false，ACK 后仍按
     * 原路径 start_capture。 */
    media_->arm_capture(input);
    if (!control("audioStart", input, now)) {
        media_->disarm_capture();
        return Result::Busy;
    }
    input_id_ = input;
    want_capture_ = true;
    recovery_.input_acked = false;
    return Result::Applied;
}
Result VoiceClient::stop_input(std::uint64_t now) {
    if (recovery_expired(now)) return Result::Stale;
    if (capability_.transport == VoiceTransport::None)
        return Result::Unsupported;
    if (control_recovering()) {
        if (!control_->wants_capture())
            return Result::Duplicate;
        control_->stop_capture();
        want_capture_ = false;
        media_->disarm_capture();
        return Result::Applied;
    }
    if (state_ != VoiceState::Established && state_ != VoiceState::Recovering) return Result::Stale;
    if (!want_capture_)
        return Result::Duplicate;
    want_capture_ = false;
    recovery_.input_acked = false;
    session_->stop_input(session_id_, input_id_);
    /* 提前武装但 ACK 未到（或 Session 未记录采集）时，Session::stop_input 不会
     * 触碰驱动，必须在此撤销武装并清空缓冲，避免迟到 ACK 重新开麦。 */
    media_->disarm_capture();
    if (!control("audioStop", input_id_, now)) {
        release(VoiceState::Failed, now);
        return Result::Busy;
    }
    return Result::Applied;
}
Result VoiceClient::send_text(std::string_view text, bool audio, std::uint64_t now) {
    if (state_ != VoiceState::Established)
        return Result::Stale;
    if (!selected_.text_input || !selected_.text_output || (audio && !selected_.audio_output))
        return Result::Unsupported;
    if (want_capture_ || capturing() || playing() || !pending_.empty() || text_pending_)
        return Result::Busy;
    // 先保留旧输入顺序和长度保护，再复制有界文字；未建立/忙线不能分配任意大正文。
    if (text.size() > 60000)
        return Result::Invalid;
    return send_content({{InputContentKind::Text, std::string(text), {}, {}}}, audio, now);
}

// supports_content 只认当前Established的实际选择，终态和未知枚举不保留上传准入。
bool VoiceClient::supports_content(InputContentKind kind) const {
    if (state_ != VoiceState::Established)
        return false;
    switch (kind) {
        case InputContentKind::Text:
            return selected_.text_input;
        case InputContentKind::Image:
            return selected_.image_input;
        case InputContentKind::File:
            return selected_.file_input;
    }
    return false;
}

// send_content 与原纯文字共享一次发布和请求关联，附件不会创建另一套输入状态机。
Result VoiceClient::send_content(const std::vector<InputContentPart>& parts, bool audio,
                                 std::uint64_t now) {
    if (state_ != VoiceState::Established)
        return Result::Stale;
    if (!selected_.text_output || (audio && !selected_.audio_output))
        return Result::Unsupported;
    for (const auto& part : parts) {
        if (!supports_content(part.kind))
            return Result::Unsupported;
    }
    if (want_capture_ || capturing() || playing() || !pending_.empty() || text_pending_)
        return Result::Busy;
    Envelope envelope;
    const auto input = next_id();
    if (content_input_command(next_id(), session_id_, input, parts, audio, envelope) !=
        WireResult::Ok)
        return Result::Invalid;
    if (!send(envelope, input, now))
        return Result::Busy;
    // 所有离散输入都保留原ID；失败ACK没有result，语音宿主也不得从当前音频段猜关联。
    text_input_id_ = input;
    if (selected_.transport == VoiceTransport::None) {
        response_id_.clear();
        recovery_.none_terminal_pending = false;
        text_pending_ = true;
    }
    return Result::Applied;
}
// receive_content核本轮双关联；取消ACK后仅原终态可展示一次，旧内容不能解锁新轮或复活副作用。
Result VoiceClient::receive_content(const ContentEvent& content) {
    if (selected_.transport != VoiceTransport::None)
        return Result::Unsupported;
    const bool cancelled_terminal = recovery_.none_terminal_pending &&
                                    content.envelope.method == "respDone";
    if (state_ != VoiceState::Established || content.envelope.session != session_id_ ||
        !content.envelope.message_sequence ||
        *content.envelope.message_sequence <= stream_watermark_ ||
        (!text_pending_ && !cancelled_terminal) ||
        content.response_id.empty())
        return Result::Stale;
    const auto& method = content.envelope.method;
    if (method == "respCreated") {
        if (content.input_id != text_input_id_ || !response_id_.empty())
            return Result::Stale;
        response_id_ = content.response_id;
        recovery_.response_cancel_sent = false;
    } else {
        if (response_id_.empty() || content.response_id != response_id_)
            return Result::Stale;
        if (method == "respSttDelta" || method == "respSttDone")
            return Result::Unsupported;
        if (method == "respDone") {
            if (content.input_id != text_input_id_ || content.audio_state != "none")
                return Result::Invalid;
            text_pending_ = false;
            recovery_.none_terminal_pending = false;
        }
    }
    stream_watermark_ = *content.envelope.message_sequence;
    return Result::Applied;
}
// cancel_content_response 取消Runtime已核验但尚未安装音频边界的回复，不停止其他回复或输入。
Result VoiceClient::cancel_content_response(std::string_view response, std::uint64_t now) {
    if (recovery_expired(now) || response.empty() ||
        (state_ != VoiceState::Established && state_ != VoiceState::Recovering))
        return Result::Stale;
    if (std::any_of(pending_.begin(), pending_.end(), [&](const Pending& item) {
            return item.method == "respCancel" && item.object == response;
        }))
        return Result::Duplicate;
    if (!control("respCancel", std::string(response), now)) {
        release(VoiceState::Failed, now);
        return Result::Busy;
    }
    return Result::Applied;
}
Result VoiceClient::cancel_response(std::uint64_t now) {
    if (recovery_expired(now)) return Result::Stale;
    if ((state_ != VoiceState::Established && state_ != VoiceState::Recovering) ||
        response_id_.empty())
        return Result::Stale;
    if (recovery_.response_cancel_sent) return Result::Duplicate;
    // none没有音频Session的本地取消状态；同回复已有原取消请求时等待其ACK，不制造第二个副作用token。
    if (selected_.transport == VoiceTransport::None &&
        std::any_of(pending_.begin(), pending_.end(), [&](const Pending& item) {
            return item.method == "respCancel" && item.object == response_id_;
        }))
        return Result::Duplicate;
    if (udp_reply_receipts_)
        udp_reply_receipts_->retire();  // 定向取消先废待发，不把晚200当新许可。
    const auto result = session_        ? session_->cancel_response(session_id_, response_id_)
                        : text_pending_ ? Result::Applied
                                        : Result::Duplicate;
    if (result != Result::Applied &&
        !(state_ == VoiceState::Recovering && result == Result::Duplicate))
        return result;
    if (!control("respCancel", response_id_, now)) {
        release(VoiceState::Failed, now);
        return Result::Busy;
    }
    recovery_.response_cancel_sent = true;
    return Result::Applied;
}
Result VoiceClient::end(std::uint64_t now) {
    if (recovery_expired(now)) return Result::Stale;
    if (control_recovering()) {
        // 当前连接未获原会话权限时只停止本地；服务器固定失联窗口负责回收原模型，不重放旧结束请求。
        release(VoiceState::Idle, now);
        return Result::Applied;
    }
    if (state_ != VoiceState::Established && state_ != VoiceState::Negotiating &&
        state_ != VoiceState::Recovering)
        return Result::Stale;
    vision_cancel();
    if (udp_reply_receipts_)
        udp_reply_receipts_->retire();
    if (session_)
        session_->close();
    if (media_)
        media_->close();
    want_capture_ = false;
    // 取消所有旧输入/取消请求等待，迟到 ACK 不能在挂断期间改变资源状态。
    requests_.cancel_session(session_id_, now);
    pending_.clear();
    if (!control("sessionEnd", "userStopped", now)) {
        release(VoiceState::Failed, now);
        return Result::Busy;
    }
    state_ = VoiceState::Ending;
    return Result::Applied;
}
// vision_cancel撤销原意图但不声明Stopped；已完成成功不改写，旧事实不得恢复后继控制。
void VoiceClient::vision_cancel() {
    if (!vision_ || !media_) return;
    if (vision_->current) media_->cancel_vision_control(vision_->current);
    for (std::size_t index = 0; index < vision_->count; ++index) {
        auto& record = vision_->records[index];
        if (!record.pending) continue;
        if (record.generation != vision_->current)
            media_->cancel_vision_control(record.generation);
        VisionState::complete(record, 503, {}, "deviceRejected");
    }
    vision_->current = 0;
}
// vision_tick每次至多交接32个事实；原截止及背压与音频请求独立，失败不假关整条模型会话。
void VoiceClient::vision_tick(std::uint64_t now) {
    if (!vision_ || !media_) return;
    auto& state = *vision_;
    if (state.clock && now < *state.clock) {
        state.clock_failed = true;
        vision_cancel();
    } else state.clock = now;
    for (std::size_t count = 0; count < state.records.size(); ++count) {
        const auto fact = media_->take_vision_fact();
        if (!fact) break;
        // 必须在原10秒内形成并交接；截止恰好相等也不能被滞后tick或队列完成绕过。
        auto found = std::find_if(state.records.begin(), state.records.begin() + state.count,
                                 [&fact](const auto& record) { return record.generation == fact->generation; });
        if (found == state.records.begin() + state.count || !found->pending ||
            now < found->started || now - found->started >= 10000) continue;
        const bool success = fact->failure == VoiceVisionFailure::None &&
            ((found->command.operation == VoiceVisionOperation::Publish && fact->state == VoiceVisionState::Active) ||
             (found->command.operation == VoiceVisionOperation::Stop && fact->state == VoiceVisionState::Stopped));
        if (success) {
            VisionState::complete(*found, 200, fact->state == VoiceVisionState::Active ? "active" : "stopped", {});
            if (fact->state == VoiceVisionState::Stopped && state.current == fact->generation)
                state.current = 0;
        } else if (fact->state == VoiceVisionState::Failed && fact->failure != VoiceVisionFailure::None) {
            int code = 503;
            std::string_view reason;
            switch (fact->failure) {
                case VoiceVisionFailure::Unsupported: code = 422; reason = "unsupportedCapability"; break;
                case VoiceVisionFailure::Busy: code = 409; reason = "resourceBusy"; break;
                case VoiceVisionFailure::MediaFailed: reason = "mediaFailed"; break;
                case VoiceVisionFailure::Timeout: code = 504; reason = "operationTimedOut"; break;
                // 原停止/取消撤销未完成发布，不代表用户拒绝，也不改写先前成功事实。
                case VoiceVisionFailure::Cancelled: reason = "deviceRejected"; break;
                default: continue; // 非法枚举不能成为自由网络reason或成功。
            }
            media_->cancel_vision_control(found->generation);
            VisionState::complete(*found, code, {}, reason);
            if (state.current == found->generation) state.current = 0;
        }
    }
    for (std::size_t index = 0; index < state.count; ++index) {
        auto& record = state.records[index];
        if (record.pending && now >= record.started && now - record.started >= 10000) {
            media_->cancel_vision_control(record.generation);
            VisionState::complete(record, 504, {}, "operationTimedOut");
            if (state.current == record.generation) state.current = 0;
        }
        if (record.dirty && publisher_.publish(up_topic_, record.receipt)) record.dirty = false;
    }
}
// vision_control先收原排队事实，再冻结新意图；历史重放只回原字节，不重开相机或延原截止。
Result VoiceClient::vision_control(const VoiceVisionCommand& command, std::uint64_t now) {
    // 直接调用也须持有原事件序号；异常命令不收取排队事实、不推进水位或触发驱动。
    if (!command.envelope.event_sequence.has_value())
        return Result::Invalid;
    if (!vision_ || !selected_.ai_vision || command.envelope.session != session_id_ ||
        session_id_.empty()) return Result::Stale;
    vision_tick(now);
    auto& state = *vision_;
    const auto reply_error = [this, &command](int code, std::string_view reason) {
        Envelope reply;
        std::string body;
        if (voice_vision_receipt(command, code, {}, reason, reply) == WireResult::Ok &&
            encode_envelope(reply, body) == DecodeResult::Ok) publisher_.publish(up_topic_, body);
    };
    for (std::size_t index = 0; index < state.count; ++index) {
        auto& record = state.records[index];
        if (record.command.envelope.token != command.envelope.token) continue;
        if (record.command.envelope.payload != command.envelope.payload ||
            record.command.envelope.event_sequence != command.envelope.event_sequence) {
            reply_error(409, "tokenConflict");
            return Result::Applied; // 业务冲突已明确回执，不让共享Runtime把它当格式故障终止音频。
        }
        if (!record.receipt.empty()) {
            record.dirty = true;
            if (publisher_.publish(up_topic_, record.receipt)) record.dirty = false;
        }
        return Result::Duplicate;
    }
    if (state_ != VoiceState::Established || command.track_id != state.profile.video_track ||
        command.media_revision != stream_watermark_ ||
        *command.envelope.event_sequence <= event_seq_) return Result::Stale;
    const bool stopping = command.operation == VoiceVisionOperation::Stop;
    // 无空槽时绝不驱动新发布；最后槽专供停止。满额停止只撤意图并返回Busy，不虚构成功。
    if (state.count == state.records.size() || (!stopping && state.count + 1 == state.records.size())) {
        if (stopping) vision_cancel();
        reply_error(409, "resourceBusy");
        return Result::Busy;
    }
    auto& record = state.records[state.count++];
    record.command = command;
    record.started = now;
    event_seq_ = *command.envelope.event_sequence;
    bool busy = state.clock_failed && !stopping;
    if (state.current) {
        const auto current = std::find_if(state.records.begin(), state.records.begin() + state.count - 1,
                                        [&state](const auto& value) { return value.generation == state.current; });
        if (!stopping || (current != state.records.begin() + state.count - 1 &&
                         current->pending && current->command.operation == VoiceVisionOperation::Stop)) busy = true;
        else vision_cancel(); // 不改已成功发布历史，取消尚未完成Publish后才受理Stop。
    }
    if (busy || counter_ >= 9007199254740991ULL) {
        VisionState::complete(record, 409, {}, "resourceBusy");
    } else {
        record.generation = ++counter_;
        const VoiceVisionControl control{record.generation, session_id_, state.profile.camera_id,
                                         state.profile.video_track, stream_watermark_, command.operation};
        const auto accepted = media_->begin_vision_control(control);
        if (accepted == VoiceVisionRequestResult::Accepted) {
            record.pending = true;
            state.current = record.generation;
        } else if (accepted == VoiceVisionRequestResult::Busy) {
            VisionState::complete(record, 409, {}, "resourceBusy");
        } else {
            VisionState::complete(record, 422, {}, "unsupportedCapability");
        }
    }
    vision_tick(now);
    return Result::Applied;
}
void VoiceClient::tick(std::uint64_t now) {
    if (!vision_profile_current() && state_ != VoiceState::Idle && state_ != VoiceState::Failed) {
        release(VoiceState::Failed, now, VoiceFailureCause::VisionProfileInvalid);
        return;
    }
    if (recovery_expired(now))
        return;
    // 恢复期offer单发加固：首次候选收集在残余拥塞下可能残缺，或应答虽到而ICE未连通；
    // 发出后kRecoveryOfferRetryAfterMs内仍无双向连通时重建下一代Peer补发一次，
    // 平台按原会话代次+1接受同一次恢复内的新offer，本恢复期不再重发第二次。
    // 控制腿恢复期间上行无法获答，先等其恢复；窗口已在上方统一判死，补发不越窗。
    if (state_ == VoiceState::Recovering && recovery_.disconnected_acked && offer_sent_ &&
        !media_reported_ && recovery_offer_retries_ < 1 && offer_sent_at_ > 0 &&
        now >= offer_sent_at_ && now - offer_sent_at_ >= kRecoveryOfferRetryAfterMs && media_ &&
        !media_->ready() && !control_recovering()) {
        // 旧代次offer的关联必须先退休：其QoS重发会向平台补发过时代次，
        // 请求超时扫描也会把本恢复误判成请求失败。
        const auto stale = std::find_if(pending_.begin(), pending_.end(),
                                        [](const Pending& item) { return item.method == "mediaOffer"; });
        if (stale != pending_.end()) {
            const std::string token = stale->token;
            pending_.erase(stale);
            requests_.cancel_request({up_topic_, "mediaOffer", token}, now);
        }
        recovery_offer_retries_ = 1;
        offer_sent_ = answer_applied_ = false;
        offer_sent_at_ = 0;
        ++stream_watermark_;
        // 控制同步会话须与断连ACK路径一样推进已知代次；否则后继控制快照的mediaRevision
        // 仍停在旧代而拒绝恢复。
        if (control_ && control_->advance_media_revision(stream_watermark_) != Result::Applied)
            release(VoiceState::Failed, now);
        else if (!media_->begin_recovery(stream_watermark_))
            release(VoiceState::Failed, now);
    }
    vision_tick(now);
    reply_receipt_tick(now);
    requests_.tick(now);
    if ((state_ == VoiceState::Reporting ||
         (control_recovering() && control_->phase() == AIControlPhase::Reporting)) &&
        phase_observed_ && now >= *phase_observed_ && now - *phase_observed_ >= 500) {
        phase_observed_.reset();
        if (control_recovering()) {
            // 新上线事实冲突仍只重报原完整能力，原15秒起点和业务会话不变。
            control_reconnected(now);
            return;
        }
        Envelope report;
        if (capability_report(next_id(), report) != WireResult::Ok || !send(report, {}, now)) {
            release(VoiceState::Failed, now);
            return;
        }
    }
    for (const auto& item : pending_) {
        if (requests_.state({item.method == "inputSend" ? content_up_ : up_topic_, item.method,
                             item.token}) == RequestState::TimedOut) {
            release(VoiceState::Failed, now, VoiceFailureCause::RequestsTimeout,
                    voice_failure_method(item.method));
            return;
        }
    }
    if (state_ == VoiceState::Negotiating && now >= phase_started_ && now - phase_started_ >= 30000)
        release(VoiceState::Failed, now, VoiceFailureCause::NegotiatingDeadline);
}
// tick双时钟不将旧单调值当作Unix授权；候选原双向屏障闭合后仍执行原请求/媒体截止。
void VoiceClient::tick(std::uint64_t now, std::uint64_t wall) {
    endpoint_tick(now, wall);
    tick(now);
}

// reply_receipt_tick发布只证明原对象安装；2秒/回退仅退休本回复，不关闭无关输入或会话。
void VoiceClient::reply_receipt_tick(std::uint64_t now) {
    if (!udp_reply_receipts_)
        return;
    auto& value = *udp_reply_receipts_;
    const bool waiting = value.dirty && !value.delivered;  // 已交付后重复背压不得取消原正常回复。
    if (!value.observe(now) || now < value.started || now - value.started >= 2000) {
        value.retire();
        if (waiting && session_ && value.response == response_id_)
            session_->cancel_response(session_id_, response_id_);
        return;
    }
    if (state_ != VoiceState::Established || recovery_.focus_suspended ||
        (value.shared_managed && !value.shared_owned) ||
        (endpoint_ && endpoint_->candidate && !endpoint_establish_gate()) || !session_ ||
        (value.installed && (!session_->playing() || value.response != response_id_))) {
        value.retire();
        return;
    }
    if (value.installed && value.dirty && !value.retired &&
        publisher_.publish(up_topic_, value.receipt)) {
        value.delivered = true;
        value.dirty = false;
    }
}
}  // namespace ur::device
