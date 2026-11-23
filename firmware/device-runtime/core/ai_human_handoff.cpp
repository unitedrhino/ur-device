// 文件说明：候选关联→真实整组暂停/取得→平台持久提交三屏障；未知不盲恢复。
// 只借共享MQTT发布及必需的实际端口，不创建Peer、第二麦克风或自己的资源授权。
#include "ur/device/json_parse.hpp"
#include "ur/device/ai_human_handoff.hpp"
#include <cJSON.h>
#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>
namespace ur::device {
namespace {
// Json 管理已通过公共有界校验的临时树，不持久保留原正文。
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
// safe_integer 保持设备/Go/JS公共整数边界一致，不能借双精度舍入证明CAS。
constexpr std::uint64_t safe_integer = 9007199254740991ULL;
// text 只读取字符串，不把null/数字转成关联身份。
std::string text(const cJSON* root, const char* key) {
    const auto* v = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
}
// number 拒绝分数/负值/超安全整数；缺失保持未知。
std::optional<std::uint64_t> number(const cJSON* root, const char* key) {
    const auto* v = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsNumber(v) || !std::isfinite(v->valuedouble) || v->valuedouble < 0 ||
        v->valuedouble > static_cast<double>(safe_integer) ||
        std::floor(v->valuedouble) != v->valuedouble)
        return {};
    return static_cast<std::uint64_t>(v->valuedouble);
}
// keys 限定本分支允许字段，不能把未知扩展解释为授权。
bool keys(const cJSON* root, std::initializer_list<const char*> names) {
    if (!cJSON_IsObject(root))
        return false;
    for (const auto* v = root->child; v; v = v->next)
        if (!v->string || std::none_of(names.begin(), names.end(), [v](const char* k) {
                return std::string_view(v->string) == k;
            }))
            return false;
    return true;
}
// valid_id 借公共信封验证非空/长度/控制字符，服务目标只发意图、不本地解析成管理员。
bool valid_id(std::string_view id) {
    Envelope e;
    e.token = std::string(id);
    e.method = "callControl";
    std::string bytes;
    return encode_envelope(e, bytes) == DecodeResult::Ok;
}
// full_group 全部输入输出整组才可移交，资源排序不是新授权。
bool full_group(const std::vector<std::string>& v) {
    return v.size() == 2 && ((v[0] == "microphone" && v[1] == "speaker") ||
                             (v[1] == "microphone" && v[0] == "speaker"));
}
// voice_track 只准现行固定voice候选单轨，不把video或未知sourceType降级接受。
bool voice_track(const cJSON* root) {
    const auto* a = cJSON_GetObjectItemCaseSensitive(root, "tracks");
    if (!cJSON_IsArray(a) || cJSON_GetArraySize(a) != 1)
        return false;
    const auto* t = cJSON_GetArrayItem(a, 0);
    return keys(t, {"trackId", "kind", "sourceId", "sourceType", "direction", "codec", "state"}) &&
           cJSON_GetArraySize(t) == 7 && text(t, "trackId") == "voice" &&
           text(t, "kind") == "audio" && text(t, "sourceId") == "microphone" &&
           text(t, "sourceType") == "microphone" && text(t, "direction") == "sendrecv" &&
           text(t, "codec") == "opus" && text(t, "state") == "requested";
}
}  // namespace
// AiHumanHandoff 构造不触发连接或资源，端口寿命由唯一宿主保证。
AiHumanHandoff::AiHumanHandoff(PublishPort& p, AiHandoffSessionPort& s, AiHandoffFocusPort& f,
                               AiHandoffConfig c)
    : publish_(p),
      sessions_(s),
      focus_(f),
      config_(std::move(c)),
      ai_seq_(config_.event_watermark) {}

// send 借唯一宿主发布，失败保留未知，不换token重试业务。
bool AiHumanHandoff::send(const Envelope& e) {
    std::string body;
    return encode_envelope(e, body) == DecodeResult::Ok && publish_.publish(config_.topic, body);
}
// clock_valid 两时钟回退或绝对截止后，不用旧事实推进新授权。
bool AiHumanHandoff::clock_valid(std::uint64_t now, std::uint64_t wall) {
    const bool compensation =
        (state_ == AiHandoffState::Suspended && compensation_available()) ||
        state_ == AiHandoffState::Restoring;  // 补偿用原AI独立租约，不续候选期限。
    if ((clock_seen_ && (now < now_ || wall < wall_)) || wall > safe_integer ||
        (state_ != AiHandoffState::HumanActive &&
         (now >= config_.deadline || wall >= config_.expires_at ||
          (!token_.empty() && now >= deadline_ && !compensation)))) {
        if (!suspended_ && !pending_ && !acquired_ && !fenced_)
            fail_before_focus();
        else
            state_ = AiHandoffState::Unknown;
        return false;
    }
    clock_seen_ = true;
    now_ = now;
    wall_ = wall;
    return true;
}
// begin 显式新意图只发送一次，先核真实完整组能力与原AI所有权。
bool AiHumanHandoff::begin(std::string token, std::string_view kind, std::string_view target,
                           std::uint64_t now, std::uint64_t wall,
                           std::optional<std::string_view> consent_id) {
    if (!token_.empty() || state_ != AiHandoffState::AiActive || config_.topic.empty() ||
        !valid_id(config_.ai_session) || !valid_id(config_.ai_focus) ||
        (!config_.human_focus.empty() && !valid_id(config_.human_focus)) ||
        config_.call_revision >= safe_integer - 1 ||
        config_.resource_revision >= safe_integer - 2 || !valid_id(token) || !valid_id(target) ||
        (kind != "device" && kind != "service") ||
        (consent_id && (kind != "service" || !valid_id(*consent_id))) ||
        !focus_.supports_microphone_speaker() || !clock_valid(now, wall) ||
        now > safe_integer - 45000 ||
        !sessions_.ai_authorized_and_owned(config_.ai_session, config_.ai_focus, now, wall))
        return false;
    Json p(cJSON_CreateObject(), cJSON_Delete);
    auto* object = p ? cJSON_AddObjectToObject(p.get(), "target") : nullptr;
    if (!object || !cJSON_AddStringToObject(object, "kind", std::string(kind).c_str()) ||
        !cJSON_AddStringToObject(object, "id", std::string(target).c_str()) ||
        !cJSON_AddStringToObject(p.get(), "operation", "escalate") ||
        !cJSON_AddNumberToObject(p.get(), "expectedRevision",
                                 static_cast<double>(config_.call_revision)) ||
        !cJSON_AddBoolToObject(p.get(), "shareContext", consent_id.has_value()))
        return false;
    // 只转发唯一原引用，不读取私有快照或推断其授权状态；同原请求不追加自动重发。
    if (consent_id) {
        auto* ids = cJSON_AddArrayToObject(p.get(), "consentIds");
        Json id(cJSON_CreateString(std::string(*consent_id).c_str()), cJSON_Delete);
        if (!ids || !id || !cJSON_AddItemToArray(ids, id.get()))
            return false;
        // consentIds接管本项，返回指针仅记录转交，不再次拥有或释放。
        [[maybe_unused]] auto* transferred_id = id.release();
    }
    std::unique_ptr<char, decltype(&cJSON_free)> body(cJSON_PrintUnformatted(p.get()), cJSON_free);
    if (!body)
        return false;
    Envelope e;
    e.token = token;
    e.method = "callControl";
    e.session = config_.ai_session;
    e.payload = body.get();
    token_ = std::move(token);
    deadline_ = std::min(config_.deadline, now + 45000);
    state_ = AiHandoffState::Binding;
    if (!send(e)) {
        state_ = AiHandoffState::Unknown;
        return false;
    }
    return true;
}
// observe_event 接收唯一宿主已严格验证的其他事件水位；不要求相邻序号。
bool AiHumanHandoff::observe_event(std::string_view session, std::uint64_t seq) {
    if (seq == 0 || seq > safe_integer)
        return false;
    auto* watermark = session == config_.ai_session
                          ? &ai_seq_
                          : (!local_.empty() && session == local_ ? &local_seq_ : nullptr);
    if (!watermark || seq <= *watermark)
        return false;
    *watermark = seq;
    (session == config_.ai_session ? ai_last_ : local_last_).clear();
    return true;
}
// binding 原请求受理才绑定两条独立腿，重放不延长原45秒期限。
AiHandoffResult AiHumanHandoff::binding(const Envelope& e, std::uint64_t now, std::uint64_t wall) {
    if (e.token != token_ || e.session != config_.ai_session || e.request || e.event_sequence ||
        !e.code || e.message_sequence)
        return AiHandoffResult::Invalid;
    if (!binding_bytes_.empty())
        return e.payload == binding_bytes_ && *e.code == 200 ? AiHandoffResult::Duplicate
                                                             : AiHandoffResult::Invalid;
    Json p(ur::device::parse_json(e.payload.c_str()), cJSON_Delete);
    if (*e.code != 200) {
        if (!keys(p.get(), {"currentRevision", "reason"}) || !number(p.get(), "currentRevision") ||
            text(p.get(), "reason").empty())
            return AiHandoffResult::Invalid;
        fail_before_focus();
        return AiHandoffResult::Applied;
    }
    if (!keys(p.get(),
              {"state", "revision", "candidateSessionId", "localSessionId", "callGroupId"}) ||
        number(p.get(), "revision") != config_.call_revision + 1 ||
        text(p.get(), "state") != "transferring")
        return AiHandoffResult::Invalid;
    const auto local = text(p.get(), "localSessionId"),
               remote = text(p.get(), "candidateSessionId"), group = text(p.get(), "callGroupId");
    if (!valid_id(local) || !valid_id(remote) || !valid_id(group) || local == remote ||
        local == config_.ai_session || remote == config_.ai_session ||
        state_ != AiHandoffState::Binding)
        return AiHandoffResult::Invalid;
    local_ = local;
    remote_ = remote;
    group_ = group;
    binding_bytes_ = e.payload;
    state_ = AiHandoffState::Candidate;
    if (early_invite_) {
        const auto cached = *early_invite_;
        early_invite_.reset();
        if (invite(cached, now, wall) != AiHandoffResult::Applied) {
            fail_before_focus();
            return AiHandoffResult::Invalid;
        }
    }
    return AiHandoffResult::Applied;
}
// invite 最多暂存一条早到邀请；原单调参数已由接收入口核截止，关联后仍须真实Voice候选准入。
AiHandoffResult AiHumanHandoff::invite(const Envelope& e, std::uint64_t, std::uint64_t wall) {
    if (!e.request || e.code || e.message_sequence || !e.event_sequence || !valid_id(e.session))
        return AiHandoffResult::Invalid;
    Json p(ur::device::parse_json(e.payload.c_str()), cJSON_Delete);
    const auto expires = number(p.get(), "expiresAt");
    // clock_valid已核原固定单调候选截止；墙钟独立核30秒邀请及原父截止，不比较跨钟域余量。
    if (!keys(p.get(), {"purpose", "transport", "admission", "tracks", "expiresAt", "callType",
                        "callGroupId"}) ||
        text(p.get(), "purpose") != "humanCall" || text(p.get(), "transport") != "webrtc" ||
        text(p.get(), "admission") != "accept" || text(p.get(), "callType") != "voice" ||
        !voice_track(p.get()) || !expires || *expires <= wall || *expires > config_.expires_at ||
        *expires - wall > 30000 || !valid_id(text(p.get(), "callGroupId")))
        return AiHandoffResult::Invalid;
    if (state_ == AiHandoffState::Binding) {
        if (early_invite_)
            return early_invite_->session == e.session && early_invite_->token == e.token &&
                           early_invite_->payload == e.payload
                       ? AiHandoffResult::Duplicate
                       : AiHandoffResult::Rejected;
        early_invite_ = e;
        return AiHandoffResult::Applied;
    }
    if (state_ != AiHandoffState::Candidate || e.session != local_ ||
        text(p.get(), "callGroupId") != group_)
        return AiHandoffResult::Rejected;
    if (*e.event_sequence <= local_seq_)
        return AiHandoffResult::Ignored;
    if (!sessions_.prepare_voice_candidate(e)) {
        fail_before_focus();
        return AiHandoffResult::Rejected;
    }
    candidate_prepared_ = true;
    local_seq_ = *e.event_sequence;
    encode_envelope(e, local_last_);
    return AiHandoffResult::Applied;
}
// control 只对当前原租约整组提交授权执行，不能凭平台消息宣告实际完成。
AiHandoffResult AiHumanHandoff::control(const Envelope& e, std::uint64_t now, std::uint64_t wall) {
    std::string bytes;
    if (encode_envelope(e, bytes) != DecodeResult::Ok)
        return AiHandoffResult::Invalid;
    FocusCommand c;
    if (decode_focus_control(WireRoute::Signaling, bytes, c) != WireResult::Ok ||
        !full_group(c.resources))
        return AiHandoffResult::Invalid;
    if (pending_ || controls_.size() >= 3 || !focus_.supports_microphone_speaker())
        return AiHandoffResult::Rejected;
    const auto ready = sessions_.candidate_media_revision(local_);
    const bool suspend =
        c.operation == "suspend" && state_ == AiHandoffState::Candidate && candidate_prepared_ &&
        ready && *ready > 0 && c.envelope.session == config_.ai_session &&
        c.focus_id == config_.ai_focus && c.expected_revision == config_.resource_revision &&
        c.priority == "conversation" && c.resume_policy == "explicit" &&
        sessions_.ai_authorized_and_owned(config_.ai_session, config_.ai_focus, now, wall);
    // 新可保持人工腿允许原明确explicit策略，旧never仍保留且不获得自动恢复权。
    const bool acquire = c.operation == "acquire" && state_ == AiHandoffState::Suspended &&
                         now < deadline_ && suspended_ && !acquire_denied_ && ready && *ready > 0 &&
                         c.envelope.session == local_ &&
                         (config_.human_focus.empty() || c.focus_id == config_.human_focus) &&
                         c.expected_revision == config_.resource_revision && c.priority == "call" &&
                         (c.resume_policy == "never" || c.resume_policy == "explicit");
    const bool restore =
        c.operation == "restore" && state_ == AiHandoffState::Suspended &&
        compensation_available() && !committed_ && !established_ &&
        c.envelope.session == config_.ai_session && c.focus_id == config_.ai_focus &&
        c.expected_revision == config_.resource_revision && c.priority == "conversation" &&
        c.resume_policy == "explicit" &&
        sessions_.restore_authorized(config_.ai_session, config_.ai_focus, now, wall);
    if (!suspend && !acquire && !restore)
        return AiHandoffResult::Rejected;
    // 候选45秒只限定取得的接纳/提交时机，不能把原可信业务租约截成45秒通话。
    // acquire与restore核原墙钟边界，clock_valid另核原单调截止；不能把不同钟域余量相等当权限。
    // 底层可信授权器和Engine按两原截止较早值限制许可，仍核短操作期限和实际两端票据。
    if (!suspend && (!c.expires_at || *c.expires_at <= wall || *c.expires_at > config_.expires_at))
        return AiHandoffResult::Rejected;
    const auto submitted = focus_.submit(c, now, wall);
    if (submitted.admission != FocusAdmission::Accepted &&
        submitted.admission != FocusAdmission::Pending)
        return AiHandoffResult::Rejected;
    // 仅完整原local/group候选且真实授权接纳后绑定；解析成功或拒绝不可占租约名。
    if (acquire && config_.human_focus.empty())
        config_.human_focus = c.focus_id;
    controls_.push_back(c);
    pending_ = c;
    state_ = suspend ? AiHandoffState::Suspending
                     : (acquire ? AiHandoffState::Acquiring : AiHandoffState::Restoring);
    return AiHandoffResult::Applied;
}
// receive 唯一宿主仅分派已鉴权SIG；未知方法或错腿不改变主会话。
AiHandoffResult AiHumanHandoff::receive(const Envelope& input, std::uint64_t now,
                                        std::uint64_t wall) {
    std::string bytes;
    Envelope e;
    if (encode_envelope(input, bytes) != DecodeResult::Ok ||
        decode_envelope(bytes, e) != DecodeResult::Ok)
        return AiHandoffResult::Invalid;
    if (token_.empty() || state_ == AiHandoffState::Closed)
        return AiHandoffResult::Ignored;
    if (e.session != config_.ai_session && (local_.empty() || e.session != local_) &&
        !(e.method == "sessionInvite" && state_ == AiHandoffState::Binding))
        return AiHandoffResult::Ignored;
    // 原token重放先核不可变字节；仅重交原实际回执，不读新权限、不续期或重新执行驱动。
    // 共享水位可能已被其他事件推进，不能据低序号吞掉这条有限幂等应答。
    if (e.method == "focusControl") {
        for (const auto& original : controls_) {
            if (original.envelope.token != e.token)
                continue;
            std::string prior;
            encode_envelope(original.envelope, prior);
            if (prior != bytes)
                return AiHandoffResult::Invalid;
            for (const auto& reply : replies_) {
                if (reply.token == e.token && reply.session == e.session)
                    return send(reply) ? AiHandoffResult::Duplicate : AiHandoffResult::Rejected;
            }
            return AiHandoffResult::Duplicate;  // 在途没有实际结果，不能造成功回执。
        }
    }
    if (!clock_valid(now, wall))
        return AiHandoffResult::Rejected;
    if (e.method == "sessionInvite")
        return invite(e, now, wall);
    if (e.method == "callState" && e.code && e.token == token_)
        return binding(e, now, wall);
    if (e.method != "focusControl" && e.method != "sessionEstablished" && e.method != "callState" &&
        e.method != "sessionEnded")
        return AiHandoffResult::Ignored;
    if (e.method == "callState" && e.code && e.token == cancel_token_) {
        Json p(ur::device::parse_json(e.payload.c_str()), cJSON_Delete);
        if (e.request || e.session != config_.ai_session || *e.code != 200 || e.event_sequence ||
            !keys(p.get(), {"state", "revision", "candidateSessionId", "localSessionId",
                            "callGroupId", "reason"}) ||
            text(p.get(), "state") != "active" ||
            number(p.get(), "revision") != config_.call_revision + 2 ||
            text(p.get(), "candidateSessionId") != remote_ ||
            text(p.get(), "localSessionId") != local_ || text(p.get(), "callGroupId") != group_ ||
            text(p.get(), "reason") != "candidateCancelled")
            return AiHandoffResult::Invalid;
        if (!suspended_ && !pending_)
            fail_before_focus();
        else if (compensation_available())
            state_ = AiHandoffState::Suspended;
        else
            state_ = AiHandoffState::Unknown;
        return AiHandoffResult::Applied;
    }
    if (!e.event_sequence || e.message_sequence || e.code)
        return AiHandoffResult::Invalid;
    auto& seq = e.session == config_.ai_session ? ai_seq_ : local_seq_;
    auto& last = e.session == config_.ai_session ? ai_last_ : local_last_;
    if (*e.event_sequence <= seq)
        return *e.event_sequence == seq && bytes == last ? AiHandoffResult::Duplicate
                                                         : AiHandoffResult::Ignored;
    AiHandoffResult result = AiHandoffResult::Invalid;
    if (e.method == "focusControl")
        result = control(e, now, wall);
    else {
        Json p(ur::device::parse_json(e.payload.c_str()), cJSON_Delete);
        if (e.method == "sessionEstablished") {
            const auto revision = number(p.get(), "mediaRevision");
            if (!e.request && e.session == local_ && keys(p.get(), {"state", "mediaRevision"}) &&
                text(p.get(), "state") == "established" && revision && *revision > 0 &&
                sessions_.candidate_media_revision(local_) == revision && acquired_ &&
                (state_ == AiHandoffState::Acquired || state_ == AiHandoffState::HumanActive)) {
                established_ = true;
                finish_commit();
                result = AiHandoffResult::Applied;
            }
        } else if (e.method == "callState") {
            const auto state = text(p.get(), "state"), reason = text(p.get(), "reason");
            if (!e.request && e.session == config_.ai_session &&
                keys(p.get(), {"state", "revision", "candidateSessionId", "localSessionId",
                               "callGroupId", "requestToken", "reason"}) &&
                number(p.get(), "revision") == config_.call_revision + 2 &&
                text(p.get(), "requestToken") == token_ &&
                text(p.get(), "localSessionId") == local_ &&
                text(p.get(), "candidateSessionId") == remote_ &&
                text(p.get(), "callGroupId") == group_) {
                if (state == "transferred" && reason.empty() && acquired_ &&
                    (state_ == AiHandoffState::Acquired || state_ == AiHandoffState::HumanActive)) {
                    committed_ = true;
                    finish_commit();
                    result = AiHandoffResult::Applied;
                } else if (state == "failed" &&
                           (reason == "transferRejected" || reason == "transferTimeout" ||
                            reason == "transferMediaFailed" || reason == "transferUnavailable" ||
                            reason == "transferRevisionChanged" ||
                            reason == "transferPermissionRevoked")) {
                    if (committed_)
                        return AiHandoffResult::Invalid;
                    if (!suspended_ && !pending_ && !acquired_)
                        fail_before_focus();
                    else if (!compensation_available())
                        state_ = AiHandoffState::Unknown;
                    result = AiHandoffResult::Applied;
                }
            }
        } else if (e.method == "sessionEnded" && !e.request && keys(p.get(), {"state", "reason"}) &&
                   text(p.get(), "state") == "ended") {
            if (e.session == config_.ai_session && text(p.get(), "reason") == "transferred" &&
                committed_) {
                ended_ = true;
                finish_commit();
                result = AiHandoffResult::Applied;
            } else if (e.session == local_ && !acquired_ && !suspended_ && !pending_) {
                fail_before_focus();
                result = AiHandoffResult::Applied;
            } else if (e.session == config_.ai_session &&
                       text(p.get(), "reason") != "transferred") {
                sessions_.close_candidate(local_);
                state_ = AiHandoffState::Closed;
                result = AiHandoffResult::Applied;
            }
        }
    }
    if (result == AiHandoffResult::Applied) {
        seq = *e.event_sequence;
        last = std::move(bytes);
    }
    return result;
}
// finish_commit 整组实际取得与两条平台持久确认缺一不可，Ended只能释放旧上下文。
void AiHumanHandoff::finish_commit() {
    if (!acquired_ || !established_ || !committed_)
        return;
    if (!fenced_) {
        sessions_.fence_ai(config_.ai_session);
        fenced_ = true;
    }
    state_ = AiHandoffState::HumanActive;
    if (ended_ && !released_) {
        sessions_.release_ai(config_.ai_session);
        released_ = true;
    }
}
// fail_before_focus 仅在尚无资源副作用或已有实际恢复证明时使用。
void AiHumanHandoff::fail_before_focus() {
    if (!local_.empty() && state_ != AiHandoffState::AiActive)
        sessions_.close_candidate(local_);
    early_invite_.reset();
    candidate_prepared_ = false;
    state_ = AiHandoffState::AiActive;
}
// compensation_available 未提交acquire或原驱动明确未取得才可补偿；任何在途/已取得都排除。
bool AiHumanHandoff::compensation_available() const {
    return suspended_ && !pending_ && !acquired_ && !committed_ &&
           (acquire_denied_ ||
            std::none_of(controls_.begin(), controls_.end(),
                         [](const FocusCommand& c) { return c.operation == "acquire"; }));
}
// cancel 关联原候选一次提交，不把取消受理解释为已恢复原物理采集。
bool AiHumanHandoff::cancel(std::string token, std::uint64_t now, std::uint64_t wall) {
    if (local_.empty() || remote_.empty() || !cancel_token_.empty() || !valid_id(token) ||
        token == token_ || !clock_valid(now, wall) || state_ == AiHandoffState::HumanActive ||
        state_ == AiHandoffState::Unknown || state_ == AiHandoffState::AiActive ||
        state_ == AiHandoffState::Closed)
        return false;
    Json p(cJSON_CreateObject(), cJSON_Delete);
    if (!p || !cJSON_AddStringToObject(p.get(), "operation", "cancel") ||
        !cJSON_AddStringToObject(p.get(), "candidateSessionId", remote_.c_str()) ||
        !cJSON_AddNumberToObject(p.get(), "expectedRevision",
                                 static_cast<double>(config_.call_revision + 1)))
        return false;
    std::unique_ptr<char, decltype(&cJSON_free)> body(cJSON_PrintUnformatted(p.get()), cJSON_free);
    if (!body)
        return false;
    Envelope e;
    e.token = token;
    e.method = "callControl";
    e.session = config_.ai_session;
    e.payload = body.get();
    cancel_token_ = std::move(token);
    if (!send(e)) {
        state_ = AiHandoffState::Unknown;
        return false;
    }
    return true;
}
// poll 有界消费真实原操作事实；取消/未知后的晚到事实仍回报但不得复活已失效的移交。
void AiHumanHandoff::poll(std::uint64_t now, std::uint64_t wall) {
    if (state_ == AiHandoffState::Closed || state_ == AiHandoffState::HumanActive ||
        token_.empty() || !clock_valid(now, wall))
        return;
    const bool may_advance = state_ != AiHandoffState::Unknown;  // 晚回调不能为失效意图补授权。
    for (unsigned count = 0; count < 4; ++count) {
        const auto result = focus_.take_result();
        if (!result)
            break;
        const auto& fact = result->actual;
        if (!pending_ || fact.envelope.token != pending_->envelope.token ||
            fact.envelope.session != pending_->envelope.session)
            continue;
        if (!focus_reply_matches(*pending_, fact)) {
            state_ = AiHandoffState::Unknown;
            pending_.reset();
            break;
        }
        const auto operation = pending_->operation;
        pending_.reset();
        replies_.push_back(fact.envelope);
        if (!send(replies_.back())) {
            state_ = AiHandoffState::Unknown;
            break;
        }
        if (fact.state == "denied") {
            if (!result->definitely_not_applied || fact.revision != config_.resource_revision ||
                (operation != "suspend" && operation != "acquire"))
                state_ = AiHandoffState::Unknown;
            else if (operation == "suspend")
                fail_before_focus();
            else if (operation == "acquire") {
                acquire_denied_ = true;
                state_ = AiHandoffState::Suspended;
            }
        } else {
            config_.resource_revision = fact.revision;
            if (operation == "suspend") {
                suspended_ = true;
                state_ = AiHandoffState::Suspended;
            } else if (operation == "acquire") {
                acquired_ = true;
                state_ = AiHandoffState::Acquired;
            } else {
                suspended_ = false;
                acquire_denied_ = false;
                fail_before_focus();
            }
        }
        if (!may_advance)
            state_ = AiHandoffState::Unknown;
    }
}
// ai_forwarding_allowed 只给移交阶段围栏，主AI仍须独立核验原会话授权。
bool AiHumanHandoff::ai_forwarding_allowed() const {
    return !suspended_ && !fenced_ &&
           (state_ == AiHandoffState::AiActive || state_ == AiHandoffState::Binding ||
            state_ == AiHandoffState::Candidate);
}
}  // namespace ur::device
