// IPC多观看者设备状态机：接听、媒体和终态分别关联；所有调用在宿主事件线程串行。
#include "ur/device/ipc_client.hpp"
#include "ur/device/json_parse.hpp"

#include <cJSON.h>

#include <algorithm>
#include <utility>

namespace ur::device {
namespace {
// same 判断已通过公共解析的字段，拒绝数字和null冒充成功状态。
// key是cJSON必需的字段名指针，expected是调用点固定枚举；保留原查找接口，不改变契约。
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
bool same(const cJSON* object, const char* key, const char* expected) {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    const auto* value = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(value) && std::string_view(value->valuestring) == expected;
}
}  // namespace

// IPCClient 借用宿主端口，不创建线程或网络连接，能力来自已确认的当前连接快照。
IPCClient::IPCClient(PublishPort& mqtt, IPCMediaFactory& factory, std::string up_topic,
                     std::string down_topic, std::string instance, IPCCapability capability)
    : factory_(factory),
      mqtt_(mqtt),
      requests_(mqtt),
      up_(std::move(up_topic)),
      down_(std::move(down_topic)),
      instance_(std::move(instance)),
      capability_(std::move(capability)) {}
// 析构保证所有独立源引用和Peer均释放，不等待云端ACK。
IPCClient::~IPCClient() { close_all(0); }

// live_sessions 返回仍持有观看预算的会话数；已本地停流的Ending不占预算。
std::size_t IPCClient::live_sessions() const {
    std::size_t count = 0;
    for (const auto& item : views_)
        if (item.second.phase != IPCPhase::Ended && item.second.phase != IPCPhase::Ending)
            ++count;
    return count;
}
// phase 查询指定会话，不将未知会话映射到当前活动观看者。
std::optional<IPCPhase> IPCClient::phase(std::string_view session) const {
    const auto view = views_.find(std::string(session));
    return view == views_.end() ? std::optional<IPCPhase>{} : view->second.phase;
}
// media_revision 用于宿主丢弃旧驱动回调，不把旧代次失败用于结束新的Peer。
std::optional<std::uint64_t> IPCClient::media_revision(std::string_view session) const {
    const auto found = views_.find(std::string(session));
    return found == views_.end() ? std::optional<std::uint64_t>{} : found->second.media_revision;
}
// participants 不将纯预览、未建立或已结束腿投影成已获得会议完整成员信息。
const std::vector<RosterParticipant>* IPCClient::participants(std::string_view session) const {
    const auto found = views_.find(std::string(session));
    if (!capability_.participant_roster || found == views_.end() ||
        found->second.invite.speaker_track.empty() || !found->second.established ||
        (found->second.phase != IPCPhase::Active &&
         found->second.phase != IPCPhase::WaitingEstablished))
        return nullptr;
    return &found->second.roster.members();
}
// participant_sequence 避免宿主把receive的迟到幂等结果误说成新成员已消费。
std::optional<std::uint64_t> IPCClient::participant_sequence(std::string_view session) const {
    if (!participants(session))
        return std::nullopt;
    return views_.find(std::string(session))->second.roster_sequence;
}
// sync_state 不将未知会话或未同步腿映射为成功，也不查全局当前观看者。
RequestState IPCClient::sync_state(std::string_view session) const {
    const auto found = views_.find(std::string(session));
    return found == views_.end() ? RequestState::Missing : found->second.sync_state;
}
// sync
// 保留原会话/端腿绑定与水位；展示查询独立于媒体控制，不创建新的Peer或输入段。
// now是单调计时，wall_ms验证平台绝对到期；保留与其余IPC端口一致的公开签名。
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
bool IPCClient::sync(std::string_view session, std::uint64_t now, std::uint64_t wall_ms) {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    wall_high_water_ = std::max(wall_high_water_, wall_ms);
    const auto found = views_.find(std::string(session));
    if (!capability_.participant_roster_sync || !capability_.participant_roster ||
        found == views_.end())
        return false;
    auto& view = found->second;
    if (view.phase != IPCPhase::Active || !view.established || !view.media ||
        !view.media->connected() || view.track_pending || !view.pending_token.empty() ||
        view.invite.endpoint_epoch == 0 ||
        view.invite.capability_revision != capability_.revision ||
        view.invite.speaker_track.empty())
        return false;
    if (view.sync_state == RequestState::Pending) {
        if (now < view.sync_started)
            return false;
        if (now - view.sync_started >= 30000) {
            view.sync_state = RequestState::TimedOut;
            return false;
        }
        return true;
    }
    Envelope command;
    command.token = instance_ + "-sync-" + std::to_string(++serial_);
    command.session = std::string(session);
    command.method = "sessionSync";
    command.payload =
        "{\"lastEventSeq\":" + std::to_string(view.event_sequence) +
        ",\"lastMessageSeq\":0,\"endpointEpoch\":" + std::to_string(view.invite.endpoint_epoch) +
        ",\"mediaRevision\":1}";
    std::string body;
    if (encode_envelope(command, body) != DecodeResult::Ok)
        return false;
    view.sync_token = command.token;
    view.sync_body = std::move(body);
    view.sync_reply.clear();
    view.sync_started = now;
    view.sync_wall = wall_high_water_;
    view.sync_retries = 0;
    view.sync_state = RequestState::Pending;
    return mqtt_.publish(up_, view.sync_body);
}
// media_update
// 弱网降质入口；只在已建立且无其他在途命令时受理，冲突修订由平台409对齐。
// now是单调计时；保留与sync一致的公开签名（wall_ms不参与，质量修订不依赖墙钟截止）。
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
bool IPCClient::media_update(std::string_view session, std::string_view track_id,
                             std::string_view profile, std::uint64_t now) {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    const auto found = views_.find(std::string(session));
    if (found == views_.end())
        return false;
    auto& view = found->second;
    // 与媒体控制在途严格互斥：驱动请求或未完成修订期间不投新计划，避免旧代次副作用。
    if (view.phase != IPCPhase::Active || !view.established || !view.media ||
        !view.media->connected() || view.track_pending || !view.pending_token.empty() ||
        !view.update_token.empty())
        return false;
    Envelope command;
    if (ipc_media_update_command(instance_ + "-upd-" + std::to_string(++serial_),
                                 std::string(session), track_id, view.media_plan_revision, profile,
                                 view.invite,
                                 command) != WireResult::Ok)
        return false;
    std::string body;
    if (encode_envelope(command, body) != DecodeResult::Ok)
        return false;
    const auto result =
        requests_.submit({command.token, command.session, command.method, up_, body}, now);
    if (result != SubmitResult::Accepted && result != SubmitResult::Duplicate)
        return false;
    view.update_token = std::move(command.token);
    view.update_track = std::string(track_id);
    view.update_profile = std::string(profile);
    view.update_report_token.clear();
    view.update_deferred_report.reset();
    view.update_accepted = view.update_driver_pending = view.update_reported =
        view.update_terminal = false;
    view.update_started = now;
    return true;
}
// publish_track_state
// 保留原计划关联且重试同字节；不是原质量命令的应答，不猜测实际驱动结果。
bool IPCClient::publish_track_state(View& view, std::string_view state, std::string_view reason,
                                    std::uint64_t now) {
    if (!view.update_deferred_report) {
        Envelope report;
        if (track_state_report_command(
                instance_ + "-obs-" + std::to_string(++serial_), view.invite.envelope.session,
                view.update_track, state, view.invite, report, view.update_token,
                view.media_plan_revision, reason, view.media_revision) != WireResult::Ok)
            return false;
        view.update_deferred_report = std::move(report);
    }
    const auto& report = *view.update_deferred_report;
    std::string body;
    if (encode_envelope(report, body) != DecodeResult::Ok)
        return false;
    const auto result =
        requests_.submit({report.token, report.session, report.method, up_, body}, now);
    if (result != SubmitResult::Accepted && result != SubmitResult::Duplicate)
        return false;
    view.update_report_token = report.token;
    view.update_reported = true;
    view.update_deferred_report.reset();
    return true;
}
// quality_applied
// 不借平台applied制造驱动事实；超时后实际完成仅释放旧命令，不补发成功投影。
// 保留 session/token/state 实际完成回调；session 索引原视图，token 匹配原 update，state 校验成功实态。
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
bool IPCClient::quality_applied(std::string_view session, std::string_view token,
                                std::string_view state, bool success, std::uint64_t now) {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    const auto found = views_.find(std::string(session));
    if (found == views_.end())
        return false;
    auto& view = found->second;
    if (view.phase != IPCPhase::Active || token != view.update_token || !view.update_accepted ||
        !view.update_driver_pending || now < view.update_started ||
        (success && state != "active" && state != "paused" && state != "stopped"))
        return false;
    view.update_driver_pending = false;
    if (view.update_reported || view.update_terminal) {
        if (view.update_terminal)
            view.update_token.clear();
        return true;
    }
    if (now - view.update_started >= 10000)
        return publish_track_state(view, "failed", "qualityTimeout", now);
    if (success)
        view.track_states[view.update_track] = std::string(state);
    return publish_track_state(view, success ? state : "failed", success ? "" : "qualityFailed",
                               now);
}
// send 保留原正文进行有界重试；成功只表示进入本地在途表。
bool IPCClient::send(View& view, Envelope command, std::uint64_t now) {
    std::string body;
    if (encode_envelope(command, body) != DecodeResult::Ok)
        return false;
    const auto result =
        requests_.submit({command.token, command.session, command.method, up_, body}, now);
    if (result != SubmitResult::Accepted && result != SubmitResult::Duplicate)
        return false;
    view.pending_method = std::move(command.method);
    view.pending_token = std::move(command.token);
    view.changed_at = now;
    return true;
}
// finish 结束指定观看者并保留终态去重，不能关闭其它观看者的共享采集引用。
// R03清理在本地同步完成：旧播放队列（Peer输出）、Peer与本地输入随media释放，
// 在途质量修订与原字节重试一并取消，不因终态保留继续请求平台。
void IPCClient::finish(View& view, std::uint64_t now) {
    if (view.phase == IPCPhase::Ended)
        return;
    if (view.media) {
        view.media->close();
        view.media.reset();
    }
    requests_.cancel_session(view.invite.envelope.session, now);
    view.track_pending.reset();
    view.recovering = false;
    view.update_token.clear();
    view.update_track.clear();
    view.update_deferred_report.reset();
    view.roster.clear();  // 原腿终态立即清名册，不等网络确认或保留墓碑结束才移除。
    view.roster_sequence = 0;
    if (view.sync_state == RequestState::Pending)
        view.sync_state = RequestState::Cancelled;
    view.phase = IPCPhase::Ended;
    view.changed_at = now;
}
// accept 只确认当前邀请的明确轨集合，平台受理之前不创建采集端口。
// 保留公共双时钟顺序：now用于本地状态迁移，wall_ms只用于原邀请绝对到期。
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
bool IPCClient::accept(std::string_view session, std::uint64_t now, std::uint64_t wall_ms) {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    const auto found = views_.find(std::string(session));
    if (found == views_.end())
        return false;
    auto& view = found->second;
    if (view.phase != IPCPhase::Invited || wall_ms >= view.invite.expires_at_ms)
        return false;
    using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
    Json root(cJSON_CreateObject(), cJSON_Delete);
    auto* tracks = root ? cJSON_AddArrayToObject(root.get(), "acceptedTracks") : nullptr;
    if (!tracks ||
        (!view.invite.video_track.empty() &&
         !cJSON_AddItemToArray(tracks, cJSON_CreateString(view.invite.video_track.c_str()))) ||
        (!view.invite.audio_track.empty() &&
         !cJSON_AddItemToArray(tracks, cJSON_CreateString(view.invite.audio_track.c_str()))) ||
        (!view.invite.speaker_track.empty() &&
         !cJSON_AddItemToArray(tracks, cJSON_CreateString(view.invite.speaker_track.c_str()))))
        return false;
    char* payload = cJSON_PrintUnformatted(root.get());
    if (!payload)
        return false;
    Envelope command;
    command.token = view.invite.envelope.token;
    command.session = view.invite.envelope.session;
    command.method = "sessionAccept";
    command.payload = payload;
    cJSON_free(payload);
    if (!send(view, std::move(command), now)) {
        finish(view, now);
        return false;
    }
    view.phase = IPCPhase::Accepting;
    return true;
}
// offer_ready 只在采集驱动开始后接纳本次Peer生成的完整offer。
bool IPCClient::offer_ready(std::string_view session, std::string_view sdp, std::uint64_t now,
                            std::uint64_t revision) {
    const auto found = views_.find(std::string(session));
    if (found == views_.end() || found->second.phase != IPCPhase::Negotiating)
        return false;
    auto& view = found->second;
    if (revision != view.media_revision ||
        (view.recovering && (now < view.recovery_started || now - view.recovery_started >= 15000)))
        return false;
    Envelope command;
    if (media_offer_command(instance_ + "-" + std::to_string(++serial_), std::string(session), sdp,
                            view.media_revision, command) != WireResult::Ok ||
        !send(view, std::move(command), now))
        return false;
    view.phase = IPCPhase::Connecting;
    return true;
}
// media_connected 将实际驱动事实报告平台，不能仅凭SDP答复声称已连通。
bool IPCClient::media_connected(std::string_view session, std::uint64_t now,
                                std::uint64_t revision) {
    const auto found = views_.find(std::string(session));
    if (found == views_.end())
        return false;
    auto& view = found->second;
    if (revision != view.media_revision || view.phase != IPCPhase::Connecting ||
        (view.recovering &&
         (now < view.recovery_started || now - view.recovery_started >= 15000)) ||
        !view.pending_token.empty() || !view.media || !view.media->connected())
        return false;
    Envelope command;
    if (voice_control(instance_ + "-" + std::to_string(++serial_), std::string(session),
                      "mediaState", "connected", view.media_revision, command, 0,
                      VoiceTransport::WebRTC) != WireResult::Ok ||
        !send(view, std::move(command), now))
        return false;
    view.phase = IPCPhase::WaitingEstablished;
    return true;
}
// media_disconnected
// 保留原采集资源及意图，仅在平台原代次失联确认后允许驱动重协商。
bool IPCClient::media_disconnected(std::string_view session, std::uint64_t revision,
                                   std::uint64_t now) {
    const auto found = views_.find(std::string(session));
    if (found == views_.end())
        return false;
    auto& view = found->second;
    if (view.recovering)
        return revision == view.media_revision && now >= view.recovery_started &&
               now - view.recovery_started < 15000;
    if (revision != view.media_revision || revision >= 9007199254740991ULL ||
        view.phase != IPCPhase::Active || !view.established || !view.media ||
        !view.invite.speaker_track.empty() || view.track_pending || !view.pending_token.empty() ||
        !view.update_token.empty())
        return false;
    Envelope command;
    if (voice_control(instance_ + "-lost-" + std::to_string(++serial_), std::string(session),
                      "mediaState", "disconnected", revision, command, 0,
                      VoiceTransport::WebRTC) != WireResult::Ok ||
        !send(view, std::move(command), now))
        return false;
    view.recovering = true;
    view.recovery_started = now;
    view.media_acked = false;
    view.phase = IPCPhase::Recovering;
    return true;
}
// end 先停止当前观看者再提交结束命令；等待期间其它观看者继续工作。
bool IPCClient::end(std::string_view session, std::uint64_t now) {
    const auto found = views_.find(std::string(session));
    if (found == views_.end() || found->second.phase == IPCPhase::Ended ||
        found->second.phase == IPCPhase::Ending)
        return false;
    auto& view = found->second;
    view.roster.clear();  // 用户挂断即清本地展示，不等待远端ACK让旧引用继续显示成员。
    view.recovering = false;
    view.roster_sequence = 0;
    if (view.sync_state == RequestState::Pending)
        view.sync_state = RequestState::Cancelled;
    if (view.media) {
        view.media->close();
        view.media.reset();
    }
    requests_.cancel_session(session, now);
    Envelope command;
    if (voice_control(instance_ + "-" + std::to_string(++serial_), std::string(session),
                      "sessionEnd", "userStopped", 0, command) != WireResult::Ok ||
        !send(view, std::move(command), now)) {
        finish(view, now);
        return false;
    }
    view.phase = IPCPhase::Ending;
    return true;
}
// receive
// 顺序校验Topic、契约、独立会话、token及媒体代次；有效建立事件与ACK可乱序到达。
// direct仍需平台受理和本地驱动许可；结束保留去重，迟到包不重新分配媒体。
// 沿用平台端口的Topic／正文和双时钟签名，各参数在对应门禁分别核验。
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
bool IPCClient::receive(std::string_view topic, std::string_view body, std::uint64_t now,
                        std::uint64_t wall_ms) {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    wall_high_water_ = std::max(wall_high_water_, wall_ms);
    if (topic != down_)
        return false;
    Envelope message;
    if (decode_envelope(body, message) != DecodeResult::Ok)
        return false;
    if (message.method == "sessionInvite") {
        IPCInvite invite;
        if (decode_ipc_invite(WireRoute::Signaling, body, capability_, invite) != WireResult::Ok) {
            // 对讲先严格解析独立契约，再复用生命周期；不把预览Peer变成会议麦克风。
            IPCTalkInvite talk;
            if (!factory_.supports_talk() ||
                decode_ipc_talk_invite(WireRoute::Signaling, body, capability_, talk) !=
                    WireResult::Ok)
                return false;
            invite.envelope = std::move(talk.envelope);
            invite.camera_id = std::move(talk.camera_id);
            invite.audio_track = std::move(talk.microphone_track);
            invite.speaker_track = std::move(talk.speaker_track);
            invite.expires_at_ms = talk.expires_at_ms;
            invite.needs_accept = talk.needs_accept;
            invite.endpoint_epoch = talk.endpoint_epoch;
            invite.capability_revision = talk.capability_revision;
        }
        if (!message.event_sequence || wall_ms >= invite.expires_at_ms ||
            invite.expires_at_ms - wall_ms > 30000)
            return false;
        const auto old = views_.find(message.session);
        if (old != views_.end())
            return old->second.invite.envelope.token == message.token &&
                   old->second.invite.envelope.payload == message.payload;
        // 预览发布预算与扬声器占用分开：允许预览并行一个对讲腿，拒绝第二个输出拥有者。
        std::size_t previews = 0, talks = 0;
        for (const auto& item : views_) {
            if (item.second.phase == IPCPhase::Ending || item.second.phase == IPCPhase::Ended)
                continue;
            if (item.second.invite.speaker_track.empty())
                ++previews;
            else
                ++talks;
        }
        if (views_.size() >= 64 ||
            (invite.speaker_track.empty() ? previews >= capability_.max_publishers : talks >= 1))
            return false;
        View view;
        view.event_sequence = *message.event_sequence;
        view.changed_at = now;
        view.invite = std::move(invite);
        const bool direct = !view.invite.needs_accept;
        views_.emplace(message.session, std::move(view));
        return !direct || accept(message.session, now, wall_ms);
    }
    const auto found = views_.find(message.session);
    if (found == views_.end())
        return false;
    auto& view = found->second;
    if (view.phase == IPCPhase::Ended)
        return true;
    if (message.method == "sessionSnapshot") {
        if (!capability_.participant_roster_sync || view.phase != IPCPhase::Active ||
            message.token != view.sync_token || message.request || !message.code)
            return false;
        if (view.sync_state != RequestState::Pending)
            return !view.sync_reply.empty() && view.sync_reply == body;
        if (now < view.sync_started)
            return false;
        const auto elapsed = now - view.sync_started;
        if (elapsed >= 30000) {
            view.sync_state = RequestState::TimedOut;
            return false;
        }
        if (*message.code != 200) {
            using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
            Json data(ur::device::parse_json(message.payload.c_str()), cJSON_Delete);
            if (*message.code < 400 || *message.code > 599 || message.event_sequence ||
                message.message_sequence || !data || cJSON_GetArraySize(data.get()) != 2 ||
                !same(data.get(), "requestMethod", "sessionSync") ||
                !(same(data.get(), "reason", "requestConflict") ||
                  same(data.get(), "reason", "requestDenied") ||
                  same(data.get(), "reason", "invalidRequest") ||
                  same(data.get(), "reason", "unsupportedMethod") ||
                  same(data.get(), "reason", "signalingNotEnabled")))
                return false;
            view.sync_state = RequestState::Failed;
            view.sync_reply = std::string(body);
            return true;  // 同步失败不取消已建立主媒体，不将失败当作撤权后的停流回执。
        }
        IPCRosterSnapshot snapshot;
        if (decode_ipc_roster_snapshot(WireRoute::Signaling, body, view.invite, capability_,
                                       snapshot) != WireResult::Ok)
            return false;
        if (snapshot.ended) {
            // 平台权威终态投影：安装空名册并执行R03清理（旧播放队列、Peer与本地输入），
            // 不恢复媒体、不重试旧操作；同token重放进入Ended分支幂等返回。
            view.sync_state = RequestState::Succeeded;
            view.sync_reply = std::string(body);
            finish(view, now);
            return true;
        }
        const auto effective_wall = std::max(wall_high_water_, view.sync_wall + elapsed);
        if (snapshot.expires_at_ms <= effective_wall) {
            view.sync_state = RequestState::TimedOut;
            return false;
        }
        if (snapshot.expires_at_ms - effective_wall > 2000)
            return false;
        if (snapshot.event_watermark <= view.event_sequence ||
            snapshot.revision <= view.snapshot_revision || view.track_pending || !view.media ||
            !view.media->connected()) {
            view.sync_state = RequestState::TimedOut;
            return false;  // 更高原事件已应用时旧全量只能为未知，不能退回水位或覆盖新展示。
        }
        for (const auto& track : snapshot.track_states) {
            const auto actual = view.track_states.find(track.first);
            if (track.second != (actual == view.track_states.end() ? "active" : actual->second))
                return false;  // 平台展示不能覆盖尚未核实的本地实际轨状态，也不能代替驱动控制。
        }
        if (!view.roster.replace(snapshot.participants))
            return false;
        view.event_sequence = view.roster_sequence = snapshot.event_watermark;
        view.snapshot_revision = snapshot.revision;
        view.sync_state = RequestState::Succeeded;
        view.sync_reply = std::string(body);
        return true;
    }
    if (message.method == "participantChanged") {
        if (!capability_.participant_roster || view.invite.speaker_track.empty() ||
            message.request || message.code || message.message_sequence || !message.event_sequence)
            return false;
        if (view.phase == IPCPhase::Ending)
            return true;
        // 平台建立可先于连通ACK到达；只读名册可缓存，不将缓存视为媒体已Active。
        if (!view.established ||
            (view.phase != IPCPhase::Active && view.phase != IPCPhase::WaitingEstablished))
            return false;
        if (*message.event_sequence <= view.event_sequence)
            return true;
        if (!view.roster.apply(message.payload))
            return false;  // 坏名册不推进水位、不关主媒体，后续原合法事件仍可消费。
        view.event_sequence = *message.event_sequence;
        view.roster_sequence = *message.event_sequence;
        return true;
    }
    if (message.method == "requestAck" && !view.update_report_token.empty() &&
        message.token == view.update_report_token) {
        if (message.request || !message.code || message.event_sequence || message.message_sequence)
            return false;
        using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
        Json data(ur::device::parse_json(message.payload.c_str()), cJSON_Delete);
        if (!same(data.get(), "requestMethod", "trackState"))
            return false;
        return requests_.acknowledge(
            {message.token, message.session, "trackState", *message.code, up_}, now);
    }
    if (message.method == "mediaUpdated") {
        MediaUpdatedEvent event;
        if (decode_media_updated(WireRoute::Signaling, body, event, view.media_revision) !=
            WireResult::Ok)
            return false;
        if (view.phase == IPCPhase::Ended)
            return true;  // 终态后迟到投影不再作用，也不能复活已清理的修订状态。
        if (view.phase != IPCPhase::Active)
            return false;
        if (message.code) {
            // 同token应答只关联在途修订：pending对齐计划修订；409冲突用平台当前修订对齐本地值。
            if (message.token != view.update_token)
                return false;
            if (*message.code == 200) {
                if (event.state != "pending" || event.tracks.size() != 1 ||
                    !event.tracks.count(view.update_track) ||
                    (event.request_token != "" && event.request_token != view.update_token) ||
                    now < view.update_started || now - view.update_started >= 10000)
                    return false;
                if (view.update_accepted)
                    return event.revision ==
                           view.media_plan_revision;  // 原pending重投不再次调用驱动。
                if (event.revision != view.media_plan_revision + 1 ||
                    !requests_.acknowledge(
                        {message.token, message.session, "mediaUpdate", 200, up_}, now))
                    return false;
                view.media_plan_revision = event.revision;
                view.update_accepted = view.update_driver_pending = true;
                if (!view.media->apply_quality({message.session, view.update_token,
                                                view.update_track, view.update_profile,
                                                event.revision})) {
                    view.update_driver_pending = false;
                    return publish_track_state(view, "failed", "qualityUnsupported", now);
                }
                return true;  // 只受理实际驱动工作，完成事实由quality_applied上报。
            }
            if (*message.code != 409 ||
                (event.reason != "staleRevision" && event.reason != "updatePending") ||
                view.update_accepted ||
                !requests_.acknowledge(
                    {message.token, message.session, "mediaUpdate", *message.code, up_}, now))
                return false;
            view.media_plan_revision = event.current_revision;
            view.update_token.clear();
            view.update_track.clear();
            return true;
        }
        if (event.state == "applied") {
            // 迟到低水位不回滚已应用实态；未知关联或无水位事件不能推进本端计划。
            if (!message.event_sequence)
                return false;
            if (*message.event_sequence <= view.event_sequence &&
                event.request_token != view.update_token)
                return true;
            if (event.request_token != view.update_token ||
                event.revision != view.media_plan_revision || !view.update_reported ||
                view.update_driver_pending || event.tracks.size() != 1 ||
                !event.tracks.count(view.update_track) ||
                event.tracks.at(view.update_track) != view.track_states[view.update_track])
                return false;
            view.event_sequence = std::max(view.event_sequence, *message.event_sequence);
            requests_.acknowledge(
                {view.update_report_token, message.session, "trackState", 200, up_}, now);
            view.media_plan_revision = event.revision;
            view.update_token.clear();
            view.update_track.clear();
            return true;  // 平台只确认已上报的实际状态，不能反过来触发实态上报。
        }
        if (event.state == "failed") {
            if (!message.event_sequence)
                return false;
            if (*message.event_sequence <= view.event_sequence &&
                event.request_token != view.update_token)
                return true;
            if (event.request_token != view.update_token ||
                event.revision != view.media_plan_revision)
                return false;
            view.event_sequence = std::max(view.event_sequence, *message.event_sequence);
            view.update_deferred_report.reset();
            view.update_terminal = true;
            if (!view.update_driver_pending) {
                view.update_token.clear();
                view.update_track.clear();
            }  // 尚未完成的驱动继续占质量槽，防止旧异步操作覆盖下一计划；主对讲不受影响。
            return true;  // failed投影不修改计划修订，等待端侧下一轮新修订对齐。
        }
        return false;  // pending必须占用原token应答，不作为独立事件出现。
    }
    if (message.method == "trackControl") {
        IPCTrackCommand command;
        if (decode_ipc_track(WireRoute::Signaling, body, view.invite, command,
                             view.media_revision) != WireResult::Ok)
            return false;
        std::string canonical;
        if (encode_envelope(command.envelope, canonical) != DecodeResult::Ok)
            return false;
        const auto cached = view.track_replies.find(message.token);
        if (cached != view.track_replies.end())
            return cached->second.first == canonical && mqtt_.publish(up_, cached->second.second);
        if (view.track_pending) {
            std::string pending;
            return encode_envelope(view.track_pending->envelope, pending) == DecodeResult::Ok &&
                   pending == canonical;
        }
        if (view.phase != IPCPhase::Active || !view.media || view.update_driver_pending ||
            *message.event_sequence <= view.event_sequence || command.operation == "subscribe" ||
            command.operation == "unsubscribe")
            return false;
        const auto prior = view.track_states.find(command.track_id);
        const std::string current = prior == view.track_states.end() ? "active" : prior->second;
        if (current == "stopped" && (command.operation == "pause" || command.operation == "resume"))
            return false;
        view.track_pending = command;
        view.track_started = now;
        view.event_sequence = *message.event_sequence;
        const std::string target = command.operation == "pause"  ? "paused"
                                   : command.operation == "stop" ? "stopped"
                                                                 : "active";
        if (current == target)
            return track_applied(message.session, message.token, target, true, now);
        if (!view.media->control_track(command))
            return track_applied(message.session, message.token, {}, false, now);
        return true;
    }
    VoiceEvent event;
    if (message.method == "requestAck" && view.pending_method == "sessionAccept") {
        if (wall_ms >= view.invite.expires_at_ms) {
            finish(view, now);
            return false;
        }
        if (message.request || !message.code || message.event_sequence ||
            message.message_sequence || message.token != view.pending_token)
            return false;
        using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
        Json data(ur::device::parse_json(message.payload.c_str()), cJSON_Delete);
        const auto* result = cJSON_GetObjectItemCaseSensitive(data.get(), "result");
        if (!same(data.get(), "requestMethod", "sessionAccept") ||
            (*message.code == 200 && !same(result, "state", "negotiating")))
            return false;
        if (!requests_.acknowledge(
                {message.token, message.session, "sessionAccept", *message.code, up_}, now))
            return false;
        if (*message.code != 200) {
            finish(view, now);
            return true;
        }
        view.pending_token.clear();
        view.pending_method.clear();
        view.media = factory_.create();
        if (!view.media || !view.media->begin(view.invite)) {
            end(message.session, now);
            return true;
        }
        view.phase = IPCPhase::Negotiating;
        view.changed_at = now;
        return true;
    }
    if (decode_voice_event(WireRoute::Signaling, body, event) != WireResult::Ok)
        return false;
    if (message.event_sequence) {
        if (*message.event_sequence <= view.event_sequence)
            return true;
        if (message.method == "sessionEnded") {
            view.event_sequence = *message.event_sequence;
            finish(view, now);
            return true;
        }
        if (message.method != "sessionEstablished" || event.revision != view.media_revision ||
            view.phase != IPCPhase::WaitingEstablished || !view.media || !view.media->connected())
            return false;
        view.event_sequence = *message.event_sequence;
        view.established = true;
        if (view.media_acked)
            view.phase = IPCPhase::Active;
        return true;
    }
    const std::string method = message.method == "mediaAnswer"    ? "mediaOffer"
                               : message.method == "sessionEnded" ? "sessionEnd"
                                                                  : event.request_method;
    // 媒体代次及成功结果须在消耗在途关联之前校验；错误代次不能吃掉合法应答。
    if (message.code && *message.code == 200 &&
        ((method == "mediaOffer" && event.revision != view.media_revision) ||
         (method == "mediaState" &&
          (event.revision != view.media_revision ||
           event.state != (view.phase == IPCPhase::Recovering ? "disconnected" : "connected")))))
        return false;
    if (view.recovering && (now < view.recovery_started || now - view.recovery_started >= 15000)) {
        finish(view, now);
        return false;
    }
    if (!message.code || message.token != view.pending_token || method != view.pending_method ||
        !requests_.acknowledge({message.token, message.session, method, *message.code, up_}, now))
        return false;
    view.pending_token.clear();
    view.pending_method.clear();
    if (*message.code != 200) {
        end(message.session, now);
        return true;
    }
    if (method == "mediaOffer") {
        if (!view.media || !view.media->apply_answer(event.answer_sdp))
            end(message.session, now);
    } else if (method == "mediaState") {
        if (view.phase == IPCPhase::Recovering) {
            // 原平台受理后才递增本端代次；驱动不得重新getUserMedia或恢复用户已停轨。
            if (!view.media || !view.media->begin_recovery(view.media_revision + 1)) {
                end(message.session, now);
                return true;
            }
            ++view.media_revision;
            view.phase = IPCPhase::Negotiating;
            return true;
        }
        view.media_acked = true;
        if (view.established) {
            view.phase = IPCPhase::Active;
            view.recovering = false;
        }
    } else if (method == "sessionEnd")
        finish(view, now);
    return true;
}
// track_applied
// 在真实驱动完成后生成原token回执；排队成功不算生效，错误只结束当前观看者。
// 保留公共实态回执签名；session找原腿、token匹配原请求、state匹配原目标，不能互换授权。
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
bool IPCClient::track_applied(std::string_view session, std::string_view token,
                              std::string_view state, bool success, std::uint64_t now) {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    const auto found = views_.find(std::string(session));
    if (found == views_.end())
        return false;
    auto& view = found->second;
    if (view.phase != IPCPhase::Active || !view.track_pending ||
        view.track_pending->envelope.token != token)
        return false;
    const auto command = *view.track_pending;
    const std::string target = command.operation == "pause"  ? "paused"
                               : command.operation == "stop" ? "stopped"
                                                             : "active";
    if (success && state != target)
        return false;
    Envelope reply;
    reply.token = command.envelope.token;
    reply.session = command.envelope.session;
    reply.method = "trackState";
    reply.request = false;
    reply.code = success ? 200 : 500;
    // 标识来自已严格校验的请求，仍用JSON编码器转义，不拼接不可信轨名。
    using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
    Json data(cJSON_CreateObject(), cJSON_Delete);
    if (!data)
        return false;
    if (success) {
        cJSON_AddStringToObject(data.get(), "trackId", command.track_id.c_str());
        cJSON_AddStringToObject(data.get(), "state", target.c_str());
        cJSON_AddRawToObject(data.get(), "mediaRevision",
                             std::to_string(view.media_revision).c_str());
    } else
        cJSON_AddStringToObject(data.get(), "reason", "mediaFailed");
    char* raw = cJSON_PrintUnformatted(data.get());
    if (!raw)
        return false;
    reply.payload = raw;
    cJSON_free(raw);
    std::string request_body, reply_body;
    if (encode_envelope(command.envelope, request_body) != DecodeResult::Ok ||
        encode_envelope(reply, reply_body) != DecodeResult::Ok)
        return false;
    if (view.track_reply_order.size() >= 32) {
        view.track_replies.erase(view.track_reply_order.front());
        view.track_reply_order.pop_front();
    }
    view.track_replies.emplace(reply.token, std::make_pair(request_body, reply_body));
    view.track_reply_order.push_back(reply.token);
    view.track_pending.reset();
    if (success)
        view.track_states[command.track_id] = target;
    const bool sent = mqtt_.publish(up_, reply_body);
    if (!success)
        end(session, now);
    return sent;
}
// tick
// 分别执行邀请墙钟截止与协商单调时钟截止，过期后取消原请求，防止重试复活资源。
void IPCClient::tick(std::uint64_t now, std::uint64_t wall_ms) {
    wall_high_water_ = std::max(wall_high_water_, wall_ms);
    for (auto item = views_.begin(); item != views_.end();) {
        auto& view = item->second;
        if (view.recovering &&
            (now < view.recovery_started || now - view.recovery_started >= 15000)) {
            finish(view, now);
            ++item;
            continue;
        }
        if (view.sync_state == RequestState::Pending && now >= view.sync_started) {
            const auto elapsed = now - view.sync_started;
            if (elapsed >= 30000)
                view.sync_state = RequestState::TimedOut;
            else if (view.phase == IPCPhase::Active &&
                     elapsed >= 10000ULL * (view.sync_retries + 1U) && view.sync_retries < 2) {
                ++view.sync_retries;
                mqtt_.publish(up_, view.sync_body);  // 原字节与token，不延长原快照授权或主会话。
            }
        }
        if (view.track_pending && now >= view.track_started && now - view.track_started >= 10000)
            track_applied(item->first, view.track_pending->envelope.token, {}, false, now);
        if (view.update_deferred_report && !view.update_terminal)
            publish_track_state(view, {}, {}, now);  // 保留实际完成事实，背压恢复不生成新token。
        if (!view.update_token.empty() && now >= view.update_started &&
            now - view.update_started >= 10000) {
            if (view.update_accepted && !view.update_reported)
                publish_track_state(view, "failed", "qualityTimeout", now);
            else if (!view.update_accepted) {
                view.update_token.clear();
                view.update_track.clear();
            }
        }
        // 接纳后10秒驱动窗口加30秒原事实重试都耗尽，只能认定平台结果未知。
        // 仅已退出的驱动可释放本地槽；仍执行的旧操作继续串行，不能借超时覆盖下一计划。
        // 不推进事件水位或伪造applied；下一新意图仍须让平台按当前revision重新准入。
        if (!view.update_token.empty() && view.update_accepted && !view.update_driver_pending &&
            now >= view.update_started && now - view.update_started >= 40000) {
            view.update_terminal = true;
            view.update_token.clear();
            view.update_track.clear();
            view.update_deferred_report.reset();
        }
        if (view.phase == IPCPhase::Ended && now >= view.changed_at &&
            now - view.changed_at >= 600000) {
            item = views_.erase(item);
            continue;
        }
        // 邀请以墙钟截止；普通应答允许两次同正文重试，媒体协商阶段最长15秒。
        const bool inviting = view.phase == IPCPhase::Invited || view.phase == IPCPhase::Accepting;
        const bool media_phase = view.phase == IPCPhase::Negotiating ||
                                 view.phase == IPCPhase::Connecting ||
                                 view.phase == IPCPhase::WaitingEstablished;
        const auto deadline = media_phase ? 15000U : 30000U;
        if ((inviting && wall_ms >= view.invite.expires_at_ms) ||
            (view.phase != IPCPhase::Invited && view.phase != IPCPhase::Active &&
             view.phase != IPCPhase::Ended && now >= view.changed_at &&
             now - view.changed_at >= deadline))
            finish(view, now);
        ++item;
    }
    requests_.tick(now);
}
// close_all 由宿主在物理隐私、失联或退出时调用，幂等释放每个观看者。
void IPCClient::close_all(std::uint64_t now) {
    for (auto& item : views_)
        finish(item.second, now);
}
}  // namespace ur::device
