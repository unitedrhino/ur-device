// 文件说明：IPC浏览器驱动仅发送明确的媒体指令；所有方法由设备唯一事件线程调用。
#include "browser_ipc_media.hpp"

#include <cJSON.h>

#include <algorithm>
#include <cmath>
#include <memory>

#include "ur/device/envelope.hpp"
#include "ur/device/json_parse.hpp"

namespace ur::device {
namespace {
// Json 保证解析或序列化失败时释放所有JSON对象。
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
// id 校验有界会话标识，禁止控制字符或内嵌NUL。
bool id(std::string_view value) {
    return !value.empty() && value.size() <= 64 &&
           std::none_of(value.begin(), value.end(),
                        [](unsigned char ch) { return ch < 32 || ch == 127; });
}
// track_id 与共享契约保持128字节边界，拒绝控制字符；媒体端口不接自由扩轨。
bool track_id(std::string_view value) {
    return !value.empty() && value.size() <= 128 &&
           std::none_of(value.begin(), value.end(),
                        [](unsigned char ch) { return ch < 32 || ch == 127; });
}
// release_id 为原本地非零十进制资源票据，不接受任意token或JS不精确数字。
bool release_id(std::string_view value) {
    return !value.empty() && value.size() <= 20 && value.front() != '0' &&
           (value.size() < 20 || value <= "18446744073709551615") &&
           std::all_of(value.begin(), value.end(),
                       [](unsigned char ch) { return ch >= '0' && ch <= '9'; });
}
// string_field 拒绝数字、null及缺失字段，不隐式转换标识。
std::string string_field(const cJSON* object, const char* key) {
    const auto* field = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(field) ? std::string(field->valuestring) : std::string{};
}
// notice 共用标识校验和JSON转义，仅内部固定kind可调用。
std::string notice(const char* kind, std::string_view session) {
    if (!id(session))
        return {};
    Json object(cJSON_CreateObject(), cJSON_Delete);
    const std::string identity(session);
    if (!object || !cJSON_AddStringToObject(object.get(), "kind", kind) ||
        !cJSON_AddStringToObject(object.get(), "sessionId", identity.c_str()))
        return {};
    char* raw = cJSON_PrintUnformatted(object.get());
    if (!raw)
        return {};
    std::string body(raw);
    cJSON_free(raw);
    return body;
}
// IPCPeerState 保存唯一观看Peer的真实本地事实，终态不复活。
struct IPCPeerState {
    std::string session;        // begin时绑定合法邀请，不从浏览器重新绑定。
    bool begun = false;         // 已发送实际创建意图。
    bool answered = false;      // 已成功将answer排入桥队列。
    bool connected = false;     // 真实ICE/DTLS回调事实。
    bool closed = false;        // 关闭后所有迟到回调拒绝。
    std::uint64_t revision = 1;  // 原Peer当前媒体代次，不使用浏览器自由改绑值。
    std::string video_track;    // 原邀请授权的轨，只属于此Peer。
    std::string audio_track;    // 缺省为空，不允许控制时新增监听。
    std::string speaker_track;  // 独立对讲接收轨，空值表示纯发布Peer。
    std::uint64_t release_id = 0;       // 原gate租约或无gate对讲的本机唯一次序，旧回调不得改绑。
    std::uint64_t natural_started = 0;  // 首次同Peer暂断的单调时间，零值由suspended标识区分。
    bool natural_suspended = false;     // 暂断期间actual connected为false，不假报连通。
    bool natural_expired = false;       // 到期永久关闭恢复许可，不能晚连通复活。
    bool natural_notified = false;      // 固定过期只交宿主一次，避免重复结束和通知。
};
}  // namespace

// Shared 只借用弱Peer索引；关闭命令不因端口对象销毁而丢失。
struct BrowserIPCMediaFactory::Shared {
    FixedQueue<std::string, 32> commands;                      // 不无限积压configure/answer/stop。
    std::map<std::string, std::weak_ptr<IPCPeerState>> peers;  // 独立观看会话索引。
    bool failed = false;                                       // 队列故障后拒绝所有副作用。
    bool conference_talk = false;  // 宿主明确开启驱动，不从平台邀请推断。
    std::uint64_t next_talk_release = 0;  // 无gate对讲同进程唯一原驱动租约，不跨端授予许可。
    CaptureGate* gate = nullptr;   // 统一宿主的同一mic/speaker/camera预约器，旧驱动默认无此限制。
    // Reservation 保留至原实际停止，旧Peer析构后迟到Stopped仍须有原身份匹配。
    struct Reservation {
        std::uint64_t lease;
        bool stopping = false;
    };
    std::map<std::string, Reservation> reservations;  // 至多真实有界预约数，不保存已释放历史。
    // emit 序列化固定指令，不将原设备报文或认证信息转交页面。
    bool emit(const char* kind, const IPCPeerState& state, const IPCInvite* invite = nullptr,
              std::string_view sdp = {}, const IPCTrackCommand* control = nullptr,
              const IPCQualityCommand* quality = nullptr) {
        if (failed)
            return false;
        Json object(cJSON_CreateObject(), cJSON_Delete);
        const std::string description(sdp);
        if (!object || !cJSON_AddStringToObject(object.get(), "kind", kind) ||
            !cJSON_AddStringToObject(object.get(), "sessionId", state.session.c_str()) ||
            (invite &&
             (!cJSON_AddStringToObject(object.get(), "videoTrack", invite->video_track.c_str()) ||
              !cJSON_AddBoolToObject(object.get(), "audio", !invite->audio_track.empty()) ||
              !cJSON_AddStringToObject(object.get(), "audioTrack", invite->audio_track.c_str()) ||
              !cJSON_AddStringToObject(object.get(), "speakerTrack",
                                       invite->speaker_track.c_str()))) ||
            (control &&
             (!cJSON_AddStringToObject(object.get(), "token", control->envelope.token.c_str()) ||
              !cJSON_AddStringToObject(object.get(), "trackId", control->track_id.c_str()) ||
              !cJSON_AddStringToObject(object.get(), "operation", control->operation.c_str()))) ||
            (quality &&
             (!cJSON_AddStringToObject(object.get(), "token", quality->token.c_str()) ||
              !cJSON_AddStringToObject(object.get(), "trackId", quality->track.c_str()) ||
              !cJSON_AddStringToObject(object.get(), "profile", quality->profile.c_str()))) ||
            (!sdp.empty() && !cJSON_AddStringToObject(object.get(), "sdp", description.c_str()))) {
            failed = true;
            return false;
        }
        if (gate) {
            const auto reservation = reservations.find(state.session);
            if (reservation == reservations.end() ||
                !cJSON_AddStringToObject(object.get(), "releaseId",
                                         std::to_string(reservation->second.lease).c_str())) {
                failed = true;
                return false;
            }
        } else if (!state.speaker_track.empty() &&
                   (!state.release_id || !cJSON_AddStringToObject(
                       object.get(), "releaseId", std::to_string(state.release_id).c_str()))) {
            failed = true;
            return false;
        }
        if ((state.revision > 1 || std::string_view(kind) == "ipcRecover") &&
            !cJSON_AddRawToObject(object.get(), "mediaRevision",
                                  std::to_string(state.revision).c_str()))
            return false;
        char* raw = cJSON_PrintUnformatted(object.get());
        if (!raw) {
            failed = true;
            return false;
        }
        std::string body(raw);
        cJSON_free(raw);
        if (body.size() > 65536 || !commands.push_back(std::move(body))) {
            failed = true;
            return false;
        }
        return true;
    }
};

namespace {
// BrowserIPCPeer 仅在独立对讲邀请内创建反向音频；不把观看Peer升级为会议。
class BrowserIPCPeer final : public IPCMediaPort {
public:
    explicit BrowserIPCPeer(std::shared_ptr<BrowserIPCMediaFactory::Shared> shared)
        : shared_(std::move(shared)), state_(std::make_shared<IPCPeerState>()) {}
    ~BrowserIPCPeer() override { close(); }
    // begin 只允许一次绑定，预算和重复会话检查先于浏览器采集。
    bool begin(const IPCInvite& invite) override {
        const bool talk = !invite.speaker_track.empty();
        // 最小完整轨计划：对讲两条不同音轨且无视频，观看至少有视频；旧工厂拒绝对讲。
        if (talk ? (!shared_->conference_talk || !invite.video_track.empty() ||
                    !track_id(invite.audio_track) || !track_id(invite.speaker_track) ||
                    invite.audio_track == invite.speaker_track)
                 : (!track_id(invite.video_track) ||
                    (!invite.audio_track.empty() && !track_id(invite.audio_track))))
            return false;
        if (shared_->failed || state_->begun || state_->closed || !id(invite.envelope.session) ||
            shared_->peers.size() >= 4 || shared_->peers.count(invite.envelope.session) != 0 ||
            shared_->reservations.count(invite.envelope.session) != 0)
            return false;
        // 一个设备的扬声器只能被一个会议腿持有；不限制该会议内合法成员同时发言。
        if (talk) {
            for (const auto& item : shared_->peers) {
                auto peer = item.second.lock();
                if (peer && !peer->closed && !peer->speaker_track.empty())
                    return false;
            }
        }
        state_->session = invite.envelope.session;
        state_->video_track = invite.video_track;
        state_->audio_track = invite.audio_track;
        state_->speaker_track = invite.speaker_track;
        if (shared_->gate) {
            const auto lease = shared_->gate->acquire(!invite.audio_track.empty(), talk,
                                                      !invite.video_track.empty());
            if (!lease)
                return false;
            shared_->reservations.emplace(
                state_->session, BrowserIPCMediaFactory::Shared::Reservation{*lease, false});
            state_->release_id = *lease;
        } else if (talk) {
            if (shared_->next_talk_release >= 9007199254740991ULL)
                return false;
            state_->release_id = ++shared_->next_talk_release;
        }
        if (!shared_->emit("ipcConfigure", *state_, &invite)) {
            const auto reservation = shared_->reservations.find(state_->session);
            if (reservation != shared_->reservations.end()) {
                shared_->gate->release(reservation->second.lease);
                shared_->reservations.erase(reservation);
            }
            return false;
        }
        state_->begun = true;
        shared_->peers.emplace(state_->session, state_);
        return true;
    }
    // apply_answer 限定当前Peer一次完整answer；不把排队成功视为媒体连通。
    bool apply_answer(std::string_view sdp) override {
        if (!state_->begun || state_->closed || state_->answered || sdp.size() < 8 ||
            sdp.size() > 49152 || sdp.substr(0, 5) != "v=0\r\n" ||
            sdp.find('\0') != std::string_view::npos ||
            !shared_->emit("ipcAnswer", *state_, nullptr, sdp))
            return false;
        state_->answered = true;
        return true;
    }
    // connected 只读指定观看者的实际驱动事实，不读全局连接状态。
    bool connected() const override {
        return state_->connected && !state_->closed && !shared_->failed;
    }
    // begin_recovery
    // 复用原Peer与原源预约；只递增本端代次，不再begin或申请采集。
    bool begin_recovery(std::uint64_t revision) override {
        if (!state_->begun || state_->closed || !state_->answered ||
            !state_->speaker_track.empty() || revision != state_->revision + 1 ||
            revision > 9007199254740991ULL)
            return false;
        state_->revision = revision;
        state_->connected = state_->answered = false;
        return shared_->emit("ipcRecover", *state_);
    }
    // control_track
    // 只控制原邀请轨；接收扬声器仅暂停恢复，实际完成仍经网页回执关联。
    bool control_track(const IPCTrackCommand& command) override {
        const bool speaker =
            !state_->speaker_track.empty() && command.track_id == state_->speaker_track;
        if (!connected() || command.envelope.session != state_->session ||
            command.revision != state_->revision || !track_id(command.envelope.token) ||
            !track_id(command.track_id) ||
            (speaker
                 ? (command.operation != "pause" && command.operation != "resume")
                 : (command.track_id != state_->video_track &&
                    (state_->audio_track.empty() || command.track_id != state_->audio_track))) ||
            (command.operation != "pause" && command.operation != "resume" &&
             command.operation != "stop" && command.operation != "publish"))
            return false;
        return shared_->emit("ipcTrack", *state_, nullptr, {}, &command);
    }
    // apply_quality 只转交原发布轨的编码档位；实际完成由浏览器setParameters回执确认。
    bool apply_quality(const IPCQualityCommand& command) override {
        if (!connected() || command.session != state_->session || command.revision == 0 ||
            !track_id(command.token) || !track_id(command.track) ||
            (command.track != state_->video_track &&
             (state_->audio_track.empty() || command.track != state_->audio_track)) ||
            (command.profile != "low" && command.profile != "standard" &&
             command.profile != "high" && command.profile != "audioOnly"))
            return false;
        return shared_->emit("ipcQuality", *state_, nullptr, {}, nullptr, &command);
    }
    // close 保留停止命令直到宿主消费；不影响其它观看者的采集引用。
    void close() override {
        if (state_->closed)
            return;
        state_->closed = true;
        state_->connected = false;
        if (state_->begun) {
            shared_->emit("ipcStop", *state_);
            const auto reservation = shared_->reservations.find(state_->session);
            if (reservation != shared_->reservations.end())
                reservation->second.stopping = true;
            shared_->peers.erase(state_->session);
        }
    }

private:
    std::shared_ptr<BrowserIPCMediaFactory::Shared> shared_;  // 队列比当前Peer存活更久。
    std::shared_ptr<IPCPeerState> state_;                     // 不与其它观看者共享协商阶段。
};
}  // namespace

// decode_ipc_browser
// 复用核心的重复键/Unicode校验，再限制本地固定消息类型与字段。
bool decode_ipc_browser(std::string_view body, IPCBrowserRequest& out) {
    out = {};
    std::string canonical;
    if (canonical_json_object(body, canonical) != DecodeResult::Ok)
        return false;
    Json object(ur::device::parse_json(canonical.c_str()), cJSON_Delete);
    if (!object)
        return false;
    IPCBrowserRequest request;
    request.kind = string_field(object.get(), "kind");
    const auto count = cJSON_GetArraySize(object.get());
    // 门铃按钮只传无参本地原意图；身份、token和物模型eventID均由C++宿主固定。
    if (request.kind == "ready" || request.kind == "shutdown" || request.kind == "doorbellPress") {
        if (count != 1)
            return false;
    } else if (request.kind == "privacyReport") {
        // 仅本地物模型值意图；设备身份、请求token和墙钟由宿主固定。
        const auto* enabled = cJSON_GetObjectItemCaseSensitive(object.get(), "enabled");
        if (count != 2 || !cJSON_IsBool(enabled))
            return false;
        request.enabled = cJSON_IsTrue(enabled);
    } else {
        request.session = string_field(object.get(), "sessionId");
        if (!id(request.session))
            return false;
        if (request.kind == "ipcConnectivity" || request.kind == "callConnectivity") {
            // 同Peer暂断不是SDP协商；只接受原本地资源票据和两个固定实态。
            request.token = string_field(object.get(), "releaseId");
            request.state = string_field(object.get(), "state");
            if (count != 4 || !release_id(request.token) ||
                (request.state != "connected" && request.state != "disconnected"))
                return false;
        } else if (request.kind == "ipcStopped" || request.kind == "callStopped") {
            request.token = string_field(object.get(), "releaseId");
            if (count != 3 || !release_id(request.token))
                return false;
        } else if (request.kind == "callOffer") {
            request.sdp = string_field(object.get(), "sdp");
            request.token = string_field(object.get(), "releaseId");
            if (count != 4 || request.sdp.size() < 8 || request.sdp.size() > 49152 ||
                request.sdp.substr(0, 5) != "v=0\r\n" || !release_id(request.token))
                return false;
        } else if (request.kind == "callAccept" || request.kind == "callReject" ||
                   request.kind == "callHangup") {
            if (count != 2)
                return false;
        } else if (request.kind == "callConnected" || request.kind == "callFailure") {
            request.token = string_field(object.get(), "releaseId");
            if (count != 3 || !release_id(request.token))
                return false;
        } else if (request.kind == "ipcSetQuality") {
            request.track = string_field(object.get(), "trackId");
            request.profile = string_field(object.get(), "profile");
            if (count != 4 || !track_id(request.track) ||
                (request.profile != "low" && request.profile != "standard" &&
                 request.profile != "high" && request.profile != "audioOnly"))
                return false;
        } else if (request.kind == "ipcTrackApplied" || request.kind == "ipcQualityApplied") {
            request.token = string_field(object.get(), "token");
            request.state = string_field(object.get(), "state");
            const auto* success = cJSON_GetObjectItemCaseSensitive(object.get(), "success");
            if (count != 5 || request.token.empty() || request.token.size() > 128 ||
                std::any_of(request.token.begin(), request.token.end(),
                            [](unsigned char ch) { return ch < 32 || ch == 127; }) ||
                !cJSON_IsBool(success))
                return false;
            request.success = cJSON_IsTrue(success);
            if ((request.success && request.state != "active" && request.state != "paused" &&
                 request.state != "stopped") ||
                (!request.success && request.state != "unknown"))
                return false;
        } else if (request.kind == "ipcOffer" || request.kind == "ipcConnected" ||
                   request.kind == "ipcDisconnected") {
            const auto* revision = cJSON_GetObjectItemCaseSensitive(object.get(), "mediaRevision");
            if (revision &&
                (!cJSON_IsNumber(revision) || !std::isfinite(revision->valuedouble) ||
                 revision->valuedouble < 1 || revision->valuedouble > 9007199254740991.0 ||
                 std::floor(revision->valuedouble) != revision->valuedouble))
                return false;
            if (revision)
                request.media_revision = static_cast<std::uint64_t>(revision->valuedouble);
            if (request.kind == "ipcDisconnected" && !revision)
                return false;
            if (request.kind != "ipcOffer") {
                if (count != (revision ? 3 : 2))
                    return false;
                out = std::move(request);
                return true;
            }
            request.sdp = string_field(object.get(), "sdp");
            if (count != (revision ? 4 : 3) || request.sdp.size() < 8 ||
                request.sdp.size() > 49152 || request.sdp.substr(0, 5) != "v=0\r\n")
                return false;
        } else if (count != 2 || (request.kind != "ipcConnected" && request.kind != "ipcAccept" &&
                                  request.kind != "ipcEnd" && request.kind != "ipcFailure" &&
                                  request.kind != "ipcSync"))
            return false;
    }
    out = std::move(request);
    return true;
}
// ipc_browser_sync_configuration 不允许纯预览、人际腿或只有名册增量能力的宿主隐式开启查询。
bool ipc_browser_sync_configuration(bool sync, bool roster, bool talk) {
    return sync && roster && talk;
}
// ipc_browser_sync_state 将核心终态映射为固定界面字面量；水位来自原腿实际消费，不取浏览器自报。
std::string ipc_browser_sync_state(const IPCClient& client, std::string_view session, bool enabled,
                                   bool rejected) {
    if (!id(session))
        return {};
    const auto phase = client.phase(session);
    const char* state = "unavailable";
    if (enabled && phase == IPCPhase::Active && client.participants(session)) {
        const auto query_state = client.sync_state(session);
        switch (query_state) {
            case RequestState::Missing:
                state = rejected ? "rejected" : "ready";
                break;
            case RequestState::Pending:
                state = "pending";
                break;
            case RequestState::Succeeded:
                state = "succeeded";
                break;
            case RequestState::Failed:
                state = "failed";
                break;
            case RequestState::TimedOut:
                state = "unknown";
                break;
            case RequestState::Cancelled:
                state = "cancelled";
                break;
        }
        // 本次被核心拒绝不能借前次成功作答；已登记待确认或刚超时仍保持真实未知语义。
        if (rejected && query_state != RequestState::Pending &&
            query_state != RequestState::TimedOut)
            state = "rejected";
    }
    Json object(cJSON_CreateObject(), cJSON_Delete);
    const std::string identity(session);
    const auto sequence = client.participant_sequence(session).value_or(0);
    if (sequence > 9007199254740991ULL || !object ||
        !cJSON_AddStringToObject(object.get(), "kind", "ipcSyncState") ||
        !cJSON_AddStringToObject(object.get(), "sessionId", identity.c_str()) ||
        !cJSON_AddStringToObject(object.get(), "state", state) ||
        !cJSON_AddNumberToObject(object.get(), "eventSeq", static_cast<double>(sequence)))
        return {};
    char* raw = cJSON_PrintUnformatted(object.get());
    if (!raw)
        return {};
    std::string body(raw);
    cJSON_free(raw);
    return body;
}
// ipc_browser_sync 不创建token、Peer或空媒体；查询是否允许及重复Pending均由同一IPCClient判断。
std::string ipc_browser_sync(IPCClient& client, const IPCBrowserRequest& request, bool enabled,
                             std::uint64_t now, std::uint64_t wall_ms) {
    if (request.kind != "ipcSync" || !id(request.session))
        return {};
    const bool accepted = enabled && client.sync(request.session, now, wall_ms);
    return ipc_browser_sync_state(client, request.session, enabled, !accepted);
}
// ipc_browser_invite 编码本地确认控件，标识中的引号由JSON库转义。
std::string ipc_browser_invite(std::string_view session, bool talk) {
    if (!talk)
        return notice("ipcInvite", session);
    if (!id(session))
        return {};
    Json object(cJSON_CreateObject(), cJSON_Delete);
    const std::string identity(session);
    if (!object || !cJSON_AddStringToObject(object.get(), "kind", "ipcInvite") ||
        !cJSON_AddStringToObject(object.get(), "sessionId", identity.c_str()) ||
        !cJSON_AddBoolToObject(object.get(), "talk", true))
        return {};
    char* raw = cJSON_PrintUnformatted(object.get());
    if (!raw)
        return {};
    std::string body(raw);
    cJSON_free(raw);
    return body;
}
// ipc_browser_stop 关闭尚未采集的邀请和已有观看控件，不附带远端报文。
std::string ipc_browser_stop(std::string_view session) { return notice("ipcStop", session); }
// ipc_browser_active 展示共享控制器已经证实的建立事实，不反过来改变协议状态。
std::string ipc_browser_active(std::string_view session) { return notice("ipcActive", session); }
// ipc_browser_participants 共用纯解析器校验外部调用边界，JSON编码身份仅送界面，不送stdout日志。
std::string ipc_browser_participants(std::string_view session, std::uint64_t sequence,
                                     const std::vector<RosterParticipant>& participants) {
    if (!id(session) || sequence == 0 || sequence > 9007199254740991ULL || participants.size() > 32)
        return {};
    Json object(cJSON_CreateObject(), cJSON_Delete);
    const std::string identity(session);
    if (!object || !cJSON_AddStringToObject(object.get(), "kind", "ipcParticipants") ||
        !cJSON_AddStringToObject(object.get(), "sessionId", identity.c_str()) ||
        !cJSON_AddNumberToObject(object.get(), "eventSeq", static_cast<double>(sequence)))
        return {};
    auto* members = cJSON_AddArrayToObject(object.get(), "participants");
    if (!members)
        return {};
    ParticipantRoster checked;  // 同规范校验重复标识、轨、字符串和最终逻辑预算，不维护第二份规范。
    for (const auto& participant : participants) {
        if (participant.participant_id.find('\0') != std::string::npos)
            return {};
        auto* member = cJSON_CreateObject();
        if (!member)
            return {};
        if (!cJSON_AddItemToArray(members, member)) {
            cJSON_Delete(member);
            return {};
        }
        if (!cJSON_AddStringToObject(member, "participantId", participant.participant_id.c_str()) ||
            !cJSON_AddStringToObject(member, "change", "joined"))
            return {};
        auto* tracks = cJSON_AddArrayToObject(member, "tracks");
        if (!tracks)
            return {};
        for (const auto& track : participant.tracks) {
            for (const auto* field : {&track.track_id, &track.kind, &track.source_id,
                                      &track.direction, &track.codec, &track.state})
                if (field->find('\0') != std::string::npos)
                    return {};
            auto* item = cJSON_CreateObject();
            if (!item)
                return {};
            if (!cJSON_AddItemToArray(tracks, item)) {
                cJSON_Delete(item);
                return {};
            }
            if (!cJSON_AddStringToObject(item, "trackId", track.track_id.c_str()) ||
                !cJSON_AddStringToObject(item, "kind", track.kind.c_str()) ||
                !cJSON_AddStringToObject(item, "sourceId", track.source_id.c_str()) ||
                !cJSON_AddStringToObject(item, "direction", track.direction.c_str()) ||
                !cJSON_AddStringToObject(item, "codec", track.codec.c_str()) ||
                !cJSON_AddStringToObject(item, "state", track.state.c_str()))
                return {};
        }
        char* data = cJSON_PrintUnformatted(member);
        if (!data)
            return {};
        const bool valid = checked.apply(data);
        cJSON_free(data);
        if (!valid)
            return {};
        cJSON_DeleteItemFromObjectCaseSensitive(
            member, "change");  // 本地条目无云端变更方法，避免误当新平台命令。
    }
    if (checked.members().size() != participants.size())
        return {};
    char* raw = cJSON_PrintUnformatted(object.get());
    if (!raw)
        return {};
    std::string result(raw);
    cJSON_free(raw);
    return result.size() <= 65536 ? result : std::string{};
}
// 构造创建有界交接对象；Peer可以比工厂存活更久，但不拥有平台身份。
BrowserIPCMediaFactory::BrowserIPCMediaFactory(bool conference_talk, CaptureGate* gate)
    : shared_(std::make_shared<Shared>()) {
    shared_->conference_talk = conference_talk;
    shared_->gate = gate;
}
// report_stopped 只接受实际关闭中的原租约，错会话、提前和重复回执均不能释放资源。
bool BrowserIPCMediaFactory::report_stopped(std::string_view session, std::string_view release_id) {
    const auto found = shared_->reservations.find(std::string(session));
    if (!shared_->gate || found == shared_->reservations.end() || !found->second.stopping ||
        release_id != std::to_string(found->second.lease) ||
        !shared_->gate->release(found->second.lease))
        return false;
    shared_->reservations.erase(found);
    return true;
}
// supports_talk 不因报文到达启用驱动，背压故障后不宣称还能接受新对讲。
bool BrowserIPCMediaFactory::supports_talk() const {
    return shared_->conference_talk && !shared_->failed;
}
// create 不开始采集，调用者仍须以合法邀请执行begin。
std::unique_ptr<IPCMediaPort> BrowserIPCMediaFactory::create() {
    return shared_->failed ? nullptr : std::make_unique<BrowserIPCPeer>(shared_);
}
// report_connected 只接收当前已答复Peer的一次真实连通事实，旧会话拒绝。
bool BrowserIPCMediaFactory::report_connected(std::string_view session, std::uint64_t revision) {
    const auto found = shared_->peers.find(std::string(session));
    if (found == shared_->peers.end() || shared_->failed)
        return false;
    auto state = found->second.lock();
    if (!state || state->closed || !state->answered || state->connected ||
        state->natural_suspended || state->natural_expired || state->revision != revision)
        return false;
    state->connected = true;
    return true;
}
// report_connectivity 只更新原媒体实态；自然恢复不重跑协议握手、不递增代次或改变用户采集意图。
Result BrowserIPCMediaFactory::report_connectivity(std::string_view session,
                                                   std::string_view original_release,
                                                   bool connected, std::uint64_t now) {
    const auto found = shared_->peers.find(std::string(session));
    if (found == shared_->peers.end() || shared_->failed)
        return Result::Stale;
    const auto state = found->second.lock();
    if (!state || state->closed || !state->answered || !state->release_id ||
        original_release != std::to_string(state->release_id))
        return Result::Stale;
    if (state->speaker_track.empty())
        return Result::Unsupported;  // 预览仍必须走原新代次answer，不借自然窗口跳过授权。
    if (state->natural_expired)
        return Result::DriverFailed;
    if (state->natural_suspended &&
        (now < state->natural_started || now - state->natural_started >= 5000)) {
        state->natural_expired = true;
        return Result::DriverFailed;
    }
    if (connected) {
        if (!state->natural_suspended)
            return state->connected ? Result::Duplicate : Result::Invalid;
        state->natural_suspended = false;
        state->connected = true;
        return Result::Applied;
    }
    if (state->natural_suspended)
        return Result::Duplicate;
    if (!state->connected)
        return Result::Invalid;  // 未完成首次真实握手的媒体不能沿本机回调获得建立资格。
    state->natural_started = now;
    state->natural_suspended = true;
    state->connected = false;
    return Result::Applied;
}
// poll_connectivity 即使网页定时器暂停仍按原单调截止交出一次释放意图；旧回执不能取消到期事实。
std::vector<std::string> BrowserIPCMediaFactory::poll_connectivity(std::uint64_t now) {
    std::vector<std::string> expired;
    for (const auto& item : shared_->peers) {
        const auto state = item.second.lock();
        if (!state || state->closed || !state->natural_suspended || state->natural_notified)
            continue;
        if (now >= state->natural_started && now - state->natural_started < 5000)
            continue;
        state->natural_expired = true;
        state->natural_notified = true;
        expired.push_back(state->session);
    }
    return expired;
}
// take_command 交出一条拥有独立正文的控制指令，宿主负责处理桥背压。
std::optional<std::string> BrowserIPCMediaFactory::take_command() {
    auto* command = shared_->commands.front();
    if (!command)
        return {};
    auto result = std::move(*command);
    shared_->commands.pop_front();
    return result;
}
// failed 使不可丢的控制积压变成明确终止条件，不隐式丢弃停止命令。
bool BrowserIPCMediaFactory::failed() const { return shared_->failed; }
}  // namespace ur::device
