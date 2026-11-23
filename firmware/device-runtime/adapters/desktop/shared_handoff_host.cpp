// 文件说明：原boot真实idle→统一能力ACK→AI整组租约→候选Peer→移交三屏障；未知只围栏，不猜恢复。
#include "shared_handoff_host.hpp"

#include <cJSON.h>

#include <algorithm>
#include <array>
#include <utility>

#include "browser_duplex_focus.hpp"
#include "browser_human_voice_media.hpp"
#include "browser_input_ownership_gate.hpp"
#include "browser_wire.hpp"
#include "ur/device/json_parse.hpp"
#include "ur/device/shared_handoff_focus.hpp"

namespace ur::device {
namespace {
// input_binding_command
// 编码可信宿主的原腿关联，先登记页面路由再允许门控票据；绑定本身不授媒体许可。
std::string input_binding_command(const BrowserDuplexBinding& binding) {
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_CreateObject(), cJSON_Delete);
    if (!root || !cJSON_AddStringToObject(root.get(), "kind", "inputOwnershipBind") ||
        !cJSON_AddStringToObject(root.get(), "sessionId", binding.session.c_str()) ||
        !cJSON_AddStringToObject(root.get(), "routeGeneration",
                                 std::to_string(binding.route_generation).c_str()) ||
        !cJSON_AddStringToObject(root.get(), "routeKind",
                                 binding.role == BrowserDuplexRole::AI ? "udp" : "humanSender"))
        return {};
    char* raw = cJSON_PrintUnformatted(root.get());
    if (!raw)
        return {};
    const std::string bytes(raw);
    cJSON_free(raw);
    std::string canonical;
    return canonical_json_object(bytes, canonical) == DecodeResult::Ok ? canonical : std::string{};
}
// LocalIntent 是唯一本地用户控制合同，不接受session/focus/Peer/权限字段。
struct LocalIntent {
    std::string operation, target_kind, target;
    std::optional<std::string> consent_id; // 已配对本地显式选择的原本人引用，不授本人权限。
};
// local_intent
// 只接纳固定手工操作；目标经过原公共标识校验，不从URL取得设备控制权。
bool local_intent(std::string_view body, LocalIntent& out) {
    out = {};
    std::string canonical;
    if (canonical_json_object(body, canonical) != DecodeResult::Ok)
        return false;
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(ur::device::parse_json(canonical.c_str()),
                                                         cJSON_Delete);
    const auto* kind = cJSON_GetObjectItemCaseSensitive(root.get(), "kind");
    if (cJSON_IsString(kind) && std::string_view(kind->valuestring) == "humanInputControl") {
        const auto* operation = cJSON_GetObjectItemCaseSensitive(root.get(), "operation");
        if (cJSON_GetArraySize(root.get()) != 2 || !cJSON_IsString(operation) ||
            std::string_view(operation->valuestring) != "stop")
            return false;
        out.operation = "humanInputStop";
        return true;
    }
    if (cJSON_IsString(kind) && std::string_view(kind->valuestring) == "endpointSourceControl") {
        const auto* operation = cJSON_GetObjectItemCaseSensitive(root.get(), "operation");
        if (!cJSON_IsString(operation)) return false;
        const std::string op = operation->valuestring;
        const bool prepare = op == "prepare";
        if ((op != "prepare" && op != "commit" && op != "abort") ||
            cJSON_GetArraySize(root.get()) != (prepare ? 3 : 2))
            return false;
        out.operation = prepare          ? "endpointPrepare"
                        : op == "commit" ? "endpointCommit"
                                         : "endpointAbort";
        if (prepare) {
            const auto* target = cJSON_GetObjectItemCaseSensitive(root.get(), "target");
            if (!cJSON_IsString(target)) return false;
            out.target = target->valuestring;
            if (out.target.empty() || out.target.size() > 128 ||
                std::any_of(out.target.begin(), out.target.end(),
                            [](unsigned char value) { return value < 32 || value == 127; }))
                return false;
        }
        return true;
    }
    if (!cJSON_IsString(kind) || std::string_view(kind->valuestring) != "handoffControl")
        return false;
    const auto* op = cJSON_GetObjectItemCaseSensitive(root.get(), "operation");
    if (!cJSON_IsString(op))
        return false;
    out.operation = op->valuestring;
    if (out.operation == "escalate" || out.operation == "dial") {
        const auto* type = cJSON_GetObjectItemCaseSensitive(root.get(), "targetKind");
        const auto* id = cJSON_GetObjectItemCaseSensitive(root.get(), "targetId");
        const auto* share = cJSON_GetObjectItemCaseSensitive(root.get(), "shareContext");
        const auto* consent = cJSON_GetObjectItemCaseSensitive(root.get(), "consentId");
        const bool sharing = share != nullptr || consent != nullptr;
        if (out.operation == "dial" && sharing)
            return false; // 普通临时通话不借永久移交的本人分享引用。
        if (cJSON_GetArraySize(root.get()) != (sharing ? 6 : 4) || !cJSON_IsString(type) ||
            !cJSON_IsString(id))
            return false;
        out.target_kind = type->valuestring;
        out.target = id->valuestring;
        // 本地HAL只承载服务分享意图，不能以空值、false或额外正文冒充本人授权。
        if (sharing) {
            if (!cJSON_IsTrue(share) || !cJSON_IsString(consent) || out.target_kind != "service")
                return false;
            Envelope reference;
            reference.token = consent->valuestring;
            reference.method = "callControl";
            std::string bytes;
            if (encode_envelope(reference, bytes) != DecodeResult::Ok)
                return false;
            out.consent_id = consent->valuestring;
        }
        Envelope check;
        check.token = out.target;
        check.method = "callControl";
        std::string encoded;
        return (out.target_kind == "device" ||
                (out.operation == "escalate" && out.target_kind == "service")) &&
               encode_envelope(check, encoded) == DecodeResult::Ok;
    }
    return cJSON_GetArraySize(root.get()) == 2 &&
           (out.operation == "accept" || out.operation == "reject" || out.operation == "cancel" ||
            out.operation == "hangup" || out.operation == "hold" || out.operation == "resume");
}
// LocalMux 将原已绑定的两个权限域分开，不把一个原腿许可借给另一原腿。
class LocalMux final : public FocusAuthority {
    public:
    // 两个独立原腿许可不可互换，未实际绑定的一腿没有授权器。
    std::array<std::unique_ptr<BrowserDuplexFocusAuthority>, 2> authorities;
    // before_issue只在原完整合同许可签发后清理平台已停的本地AI输入，不接受网页权限字段。
    std::function<bool(const FocusPermit&)> before_issue;
    // authorize 只委托原不可变绑定签发；未匹配任何原腿返回空许可。
    std::optional<FocusPermit> authorize(const FocusCommand& command, std::uint64_t now,
                                         std::uint64_t wall) override {
        for (auto& authority : authorities)
            if (authority) {
                auto permit = authority->authorize(command, now, wall);
                if (permit && (!before_issue || before_issue(*permit)))
                    return permit;
            }
        return {};
    }
    // valid 必须找到签发该许可的原绑定，并持续核真实双时钟及媒体事实。
    bool valid(const FocusPermit& permit, std::uint64_t now, std::uint64_t wall) const override {
        for (const auto& authority : authorities)
            if (authority && authority->valid(permit, now, wall))
                return true;
        return false;
    }
    // closed_release_valid 只允许原已分配腿的精确释放，不复活媒体权限。
    bool closed_release_valid(const FocusPermit& permit, std::uint64_t now,
                              std::uint64_t wall) const override {
        for (const auto& authority : authorities)
            if (authority && authority->closed_release_valid(permit, now, wall))
                return true;
        return false;
    }
};
// BorrowedHumanCandidate
// 将通话专属Peer与宿主boot扬声器探针分清；初始化client.close不能销毁宿主基线。
class BorrowedHumanCandidate final : public CallMediaPort {
    public:
    // HoldProof
    // 只回读唯一端点原同token/操作的完整实际结果；空值为等待，不创建资源事实。
    using HoldProof = std::function<std::optional<bool>(bool, std::string_view)>;
    // ScopeBinder只由HumanCallClient严格接纳原sessionCreated后调用，不从网页提供session。
    using ScopeBinder = std::function<bool(std::string_view)>;
    // media与closing由宿主固定，关闭回调仅决定是否仍须等待正式整组释放。
    BorrowedHumanCandidate(BrowserHumanVoiceMedia& media, std::function<bool()> closing,
                           HoldProof hold_proof, ScopeBinder binder = {})
        : media_(media), closing_(std::move(closing)), hold_proof_(std::move(hold_proof)),
          scope_binder_(std::move(binder)) {}
    bool available() const override { return media_.available(); } // 实际原音频桥仍可预约。
    bool voice_supported() const override { return media_.voice_supported(); } // 不自报视频能力。
    bool prepare() override { return false; } // 该装配只接语音，不借旧视频API预约。
    bool begin() override { return false; }   // 不偷偷建立摄像头或第二采集源。
    // prepare_voice 只有原媒体真实预约成功后本client才取得候选关闭职责。
    bool prepare_voice(const CallVoiceProfile& profile) override {
        if (!media_.prepare_voice(profile)) {
            // 普通主叫尚无云端leg，只核原实际端口与固定格式；原创建应答后才真正绑定预约。
            if (!scope_binder_ || reserved_ || deferred_ || !media_.available() ||
                profile.sample_rate != 48000 || profile.channels != 1 ||
                profile.frame_duration_ms != 20)
                return false;
            deferred_ = profile;
            return true;
        }
        reserved_ = true;
        return true;
    }
    // bind_session_scope只在原创建应答严格校验后固定唯一leg，不取得麦克风或输出。
    bool bind_session_scope(std::string_view session) override {
        if (scope_binder_ && !scope_binder_(session))
            return false;
        if (deferred_) {
            if (!media_.prepare_voice(*deferred_))
                return false;
            reserved_ = true;
            deferred_.reset();
        }
        return reserved_;
    }
    // begin_voice_candidate 原预约后才创建唯一空Sender Peer，不自动替换麦克风轨。
    bool begin_voice_candidate(const CallVoiceProfile& profile) override {
        return reserved_ && media_.begin_voice_candidate(profile);
    }
    // apply_answer
    // 只提交本预约Peer的原答案，成功仍需实际Answered和Connected回读。
    bool apply_answer(std::string_view sdp) override {
        return reserved_ && media_.apply_answer(sdp);
    }
    // connected 必须是原候选实际ICE/DTLS连通，关闭请求后不再宣称可用。
    bool connected() const override { return reserved_ && !close_requested_ && media_.connected(); }
    // 保持只借原共享FocusEngine的双组执行结果，不另建媒体源、驱动或自行提交CAS。
    bool hold_supported() const override { return bool(hold_proof_); }
    bool hold_async_supported() const override { return bool(hold_proof_); }
    // request_hold 的票据仅关联本业务等待；实际停止/恢复必须另有原同token
    // focusState200。
    std::optional<std::uint64_t> request_hold(bool held, std::string_view token,
                                              std::uint64_t revision, std::uint64_t) override {
        if (!connected() || !hold_proof_ || token.empty() || token.size() > 128 || !revision ||
            revision > 9007199254740991ULL || hold_ticket_ >= 32)
            return {};
        if (hold_ticket_ && !hold_completed_)
            return {}; // 原在途或失败请求不得被另一控制偷偷替换。
        hold_token_ = token;
        hold_target_ = held;
        hold_completed_ = false;
        return ++hold_ticket_;
    }
    // hold_result
    // 只接受当前原业务票据，原物理证明来自固定宿主回调而非网络自报状态。
    std::optional<bool> hold_result(std::uint64_t ticket, bool held) const override {
        if (!connected() || !hold_proof_ || !ticket || ticket != hold_ticket_ ||
            held != hold_target_)
            return false;
        if (hold_completed_)
            return true; // 已由原实际完整组证明的历史操作，不因后继控制改写原结果。
        const auto actual = hold_proof_(held, hold_token_);
        if (actual.value_or(false))
            hold_completed_ = true;
        return actual;
    }
    // private_context_allowed
    // 在原保持等待或已保持时关闭私有交付，不结束主通话或伪造静音。
    bool private_context_allowed() const {
        return !hold_ticket_ || (!hold_target_ && hold_result(hold_ticket_, false).value_or(false));
    }
    // close 不销毁boot闲置探针；已取得的候选由宿主先围栏并等待正式lease释放。
    void close() override {
        // 创建被明确拒绝时尚无Human leg或媒体预约，只撤销延后格式，允许下一次独立拨号。
        // 不关闭共享boot端口或原AI输入；发布背压未调用close，仍保留原请求等待。
        if (!reserved_) {
            deferred_.reset();
            return;
        }
        if (close_requested_)
            return;
        close_requested_ = true;
        if (!closing_ || !closing_())
            media_.close();
    }
    void dispose() { media_.close(); } // 只有宿主原整组release后或整个进程关闭时销毁实际原Peer。
    private:
    BrowserHumanVoiceMedia& media_; // 同原候选和输出门控，不增加媒体源、Peer或实际闲置事实。
    bool reserved_ = false;         // 只在原prepare成功后通话拥有候选资源，boot节点始终由宿主管理。
    bool close_requested_ = false;  // 原close不可逆，但实际门控端口保留到正式lease释放完成。
    std::function<bool()> closing_; // 可信宿主决定是否仍持有待原release的完整组，不由页面决定。
    HoldProof hold_proof_;          // 唯一宿主原端点+完整实际组只读证明。
    ScopeBinder scope_binder_;      // 显式临时或纯Human主叫延后原leg绑定，不从网页接收会话身份。
    std::optional<CallVoiceProfile> deferred_; // 未知leg时只保存固定格式，不启动第二媒体。
    std::string hold_token_;              // 原callControl token，仅同method/session操作账本内关联。
    std::uint64_t hold_ticket_ = 0;       // 本业务有界等待关联，不是物理门控或Focus票据。
    bool hold_target_ = false;            // 当前原等待目标，不能用其自身证明保持成功。
    mutable bool hold_completed_ = false; // 只缓存原完整实际成功，不能以ticket存在标记完成。
};
// HandoffPublisher
// 按原方法借用固定AI或Global发布端口；协调器回执不能冒用AI端口越过原Focus来源。
class HandoffPublisher final : public PublishPort {
    public:
    // 固定原唯一连接，回执发布仍由Router核原FocusControl来源。
    explicit HandoffPublisher(SharedDeviceConnection& connection) : connection_(connection) {}
    // publish 只分流既有callControl和focusState，不新增广播或跨腿发布权限。
    bool publish(std::string_view topic, std::string_view body) override {
        Envelope envelope;
        if (decode_envelope(body, envelope) != DecodeResult::Ok)
            return false;
        if (envelope.method == "focusState")
            return connection_.port(SharedMqttOwner::Global).publish(topic, body);
        if (envelope.method == "callControl")
            return connection_.port(SharedMqttOwner::AI).publish(topic, body);
        return false;
    }

    private:
    SharedDeviceConnection& connection_; // 唯一网络所有者，仍由Router核原来源/会话/回执一致性。
};
// HandoffFocusRelay
// 只观察唯一端点的原准入结果，不创建第二执行器或改变操作代次。
class HandoffFocusRelay final : public AiHandoffFocusPort {
    public:
    using Observer = std::function<void(const FocusCommand&, const FocusSubmission&, std::uint64_t,
                                        std::uint64_t)>;
    // 原端点须比借用适配器存活更久；观察者只同步共享信令水位，不授媒体权限。
    HandoffFocusRelay(SharedHandoffFocusEndpoint& endpoint, Observer observer)
        : endpoint_(endpoint), observer_(std::move(observer)) {}
    bool supports_microphone_speaker() const override {
        return endpoint_.supports_microphone_speaker();
    } // 仍取唯一实际驱动的原完整组能力。
    // submit 原封返回真实准入和原重放字节；拒绝也不能由宿主改造成Accepted。
    FocusSubmission submit(const FocusCommand& command, std::uint64_t now,
                           std::uint64_t wall) override {
        const auto actual = endpoint_.submit(command, now, wall);
        if (observer_ && (actual.admission == FocusAdmission::Accepted ||
                          actual.admission == FocusAdmission::Pending ||
                          actual.admission == FocusAdmission::Replay))
            observer_(command, actual, now, wall);
        return actual;
    }
    std::optional<AiHandoffFocusResult> take_result() override {
        return endpoint_.take_result();
    } // 只消费唯一执行器的原实际结果，不从页面或下行回执造事实。
    private:
    SharedHandoffFocusEndpoint& endpoint_; // 同一AI及Human资源执行器。
    Observer observer_;                    // 固定原宿主水位观察器，不得再次提交副作用。
};
} // namespace

// Impl
// 持有唯一网络入口、唯一输入源所有权和唯一FocusEndpoint；各业务借用固定发布端口。
struct SharedHandoffHost::Impl final : AiHandoffSessionPort {
    MqttPort& network;
    PumpedVoiceMediaPort& media;
    BrowserPcm& pcm;
    SharedHandoffHostConfig config;
    Source paired;
    Emit emit;
    ActionEngine* actions;
    bool ready = false, closed = false, failed = false, initialized = false, committed = false;
    SharedHostExitReason exit_reason =
        SharedHostExitReason::Unknown; // 仅显式诊断开启后锁存首个原退出条件。
    bool ai_closed = false, human_closed = false, human_intent = false, human_bound = false;
    bool pure_outbound = false; // 只锁存本人纯主叫来源；未绑定创建不能凭空取消或清请求。
    bool human_ended = false,
         disposing = false;            // 平台业务终态与实际Peer停止分别收录。
    bool human_established = false;    // 纯受话须原实际acquire之后独立接纳平台Established。
    // early_human_established 仅保存当前Human原建立帧一条、至多64KiB；不授予焦点。
    std::optional<MqttEvent> early_human_established;
    bool early_human_observed = false; // 原Core是否已接纳槽内信令；不表示实际资源已取得。
    bool human_restore_intent = false; // 原平台restore意图保持有效，不因实际门控在途误撤销原许可。
    bool human_holdable = false;       // 仅原新初始acquire explicit可保持，旧never租约不改写。
    bool temporary_call = false, temporary_suspended = false, temporary_resume = false,
         temporary_retiring = false;
    // 原单通临时状态独立于永久committed；未收到原实际release及Peer停止不能恢复AI。
    std::uint64_t ai_intent_version = 1, temporary_intent_version = 0;
    // 真正用户stop推进当前版本，暂停时冻结版本永不随stop或新start洗白。
    std::string temporary_suspend_token, temporary_projection;
    // 本机平台暂停清理至多一次；投影不输出原token、会话或目标。
    std::optional<FocusCommand>
        human_hold_command; // 最近原共享端点成功的suspend/restore，不把callState当证明。
    std::optional<std::uint64_t> human_business_deadline,
        human_business_expires; // 目标原首次acquire业务授权期限，不借用主叫AI期限。
    std::optional<std::uint64_t>
        dispose_deadline; // 正式release之后实际Peer停止回执仍有两秒窗口，不能无限等待。
    std::uint64_t now = 0, wall = 0, ai_seq = 0, token_counter = 0, receipt_counter = 0;
    std::string first_text_response; // 仅关联最近原回复的首文本时刻，不作为媒体或控制许可。
    std::optional<std::uint64_t> first_text_at; // 原Host单调时钟，缺失文本或不同回复必须输出null。
    std::string candidate_session; // 协调器已核原local邀请，固定后不能借另一邀请换路由。
    std::optional<std::uint64_t> ai_business_deadline,
        ai_business_expires; // 原首次AI acquire租约绝对期限，候选/Peer不续期。
    BrowserInputOwnershipGate input;
    BrowserHumanVoiceMedia human_media;
    BorrowedHumanCandidate call_media;
    DuplexFocusDriver driver;
    LocalMux authority;
    std::array<std::optional<BrowserDuplexBinding>, 2> bindings;
    std::array<std::string, 2> operation_tokens;
    std::array<std::optional<FocusCommand>, 2> owned;
    std::array<std::optional<FocusCommand>, 2> submitted;
    std::array<bool, 2> formal_released{
        {false, false}}; // 被动实际false不算正式release，必须原完整200 CAS事实。
    std::array<std::string, 2>
        submitted_bytes;             // 实际交原端点的完整控制字节，释放观察不得重编码旧请求。
    // CleanupIntent只由原已鉴权release固定；停止意图及短期限不可由QoS重投续期。
    struct CleanupIntent {
        std::string bytes;       // 原完整下行字节，冲突不能覆盖已开始停止的原指令。
        std::uint64_t deadline;  // 首次接收起最多两秒单调截止。
        std::uint64_t expires;   // 同一首次接收的墙钟短截止。
    };
    std::array<std::optional<CleanupIntent>, 2>
        cleanup_intents;  // 不清业务scope，实际ACK后再收终态。
    std::string relay_control_bytes; // 同步协调器提交窗口的原SIG完整字节，不用canonical报文补水位。
    std::optional<std::uint64_t>
        human_media_revision; // 只录原HumanCall实际接纳答案的严格revision。
    std::optional<std::string>
        early_input_idle; // ready前至多保留原严格boot事实，不伪造源或媒体就绪。
    std::optional<FocusCommand> delayed_focus;
    std::string delayed_focus_bytes; // 异步false窗口保留原完整控制字节，不重新编码或更换token。
    std::optional<std::string> action_reply;
    std::unique_ptr<SharedDeviceConnection> connection;
    std::unique_ptr<HandoffPublisher> handoff_publisher;
    std::unique_ptr<SharedHandoffFocusEndpoint> focus;
    std::unique_ptr<HandoffFocusRelay> handoff_focus; // 同原端点，仅暴露真实准入旁路观察。
    std::unique_ptr<VoiceRuntime> ai;
    std::unique_ptr<HumanCallClient> human;
    std::unique_ptr<AiHumanHandoff> handoff;
    VoiceState previous_voice = VoiceState::Idle;
    bool previous_owned = false;
    std::string previous_projection;       // 固定七字段投影去重，不保存目标或原会话。
    std::string previous_endpoint_projection;  // 接续闭集事实投影，不泄露handoff/session/token。
    bool human_input_stop_requested = false,
         human_input_stop_reported = false;  // 原本人停止意图及实际通知分离。
    std::string control_projection;        // 独立控制可用性投影，不混入原七字段或资源授权。
    unsigned control_receipt_index = 0;    // 有界本地观察水位，不用于CAS或控制授权。
    SharedContextDiagnostic context_facts; // 默认不使用；只记录原native显式开启后的有界事实。
    std::uint64_t retained_expires = 0;    // 原私有留存期限，仅内存判断到期边界，不输出到日志。

    // 构造只登记可信原端口；source同时要求原ready与原配对连接，缺事实保持false。
    Impl(MqttPort& n, PumpedVoiceMediaPort& m, BrowserPcm& p, SharedHandoffHostConfig c,
         Source source, Emit output, ActionEngine* a)
        : network(n), media(m), pcm(p), config(std::move(c)), paired(std::move(source)),
          emit(std::move(output)), actions(a),
          input(
              config.boot, [this] { return source_live(); },
              [this](const InputGateRequest& request) { return route_live(request); },
              [this](std::string_view session, std::uint64_t generation) {
                  if (bindings[0] && session == bindings[0]->session && generation == 1)
                      return pcm.fence_capture_route(session, generation);
                  return human_bound && session == candidate_session && generation == 2;
              }),
          human_media([this] { return source_live(); }, config.boot),
          call_media(
              human_media,
              [this] {
                  human_closed = true;
                  early_human_established.reset(); // 候选关闭不得把原帧带入后继通话。
                  early_human_observed = false;
                  if (!bindings[1] || !submitted[1])
                      return false;
                  // 本地HAL可比上一主poll更新；关闭必须使用当前可信单调时刻，不能向原驱动回退。
                  if (!driver.poll(now)) {
                      mark_failed(SharedHostExitReason::CandidateCloseDriverFailed);
                      return true;
                  }
                  driver.stop_scope(bindings[1]->session, bindings[1]->focus,
                                    {"microphone", "speaker"});
                  return true;
              },
              [this](bool held, std::string_view token) { return human_hold_proof(held, token); },
              (config.temporary_calls && !config.human_receiver) ||
                      (config.human_receiver && config.human_endpoint_source)
                  ? BorrowedHumanCandidate::ScopeBinder([this](std::string_view session) {
                        return bind_ordinary_candidate(session);
                    })
                  : BorrowedHumanCandidate::ScopeBinder{}),
          driver(input, {&pcm, &human_media}, config.boot, 0) {
        if (config.context_diagnostic) driver.enable_failure_diagnostics(); // 原初始化前启用，不回溯或改变票据流程。
        authority.before_issue = [this](const FocusPermit& permit) {
            if (!temporary_call || !bindings[0] || permit.scope.operation != "suspend" ||
                permit.scope.envelope.session != bindings[0]->session)
                return true;
            if (temporary_suspend_token == permit.scope.envelope.token)
                return true;
            // 只在原authority已核完整组/CAS/双截止之后，退休平台已停止的原本地输入与播放。
            // 不发第二个audioStop，也不把平台暂停算成用户stop或改变原用户版本。
            if (!ai || ai->suspend_input_for_focus(permit, now) != Result::Applied)
                return false;
            temporary_suspend_token = permit.scope.envelope.token;
            return true;
        };
    }
    // source_live 只回读原ready与已配对桥，不授任何会话媒体许可。
    bool source_live() const { return ready && !closed && paired && paired(); }
    // mark_exit
    // 只读分支锁存，不查询端口、不改业务状态；后继close不能覆盖原失败原因。
    void mark_exit(SharedHostExitReason reason) {
        if (config.context_diagnostic && exit_reason == SharedHostExitReason::Unknown)
            exit_reason = reason;
    }
    // mark_failed 在原failed赋值点附带固定原因，不提前停止业务或改变原短路判断。
    void mark_failed(SharedHostExitReason reason) {
        mark_exit(reason);
        failed = true;
    }
    // reject_poll 保留原false返回，仅记录原代码已确认的退出条件。
    bool reject_poll(SharedHostExitReason reason) {
        mark_exit(reason);
        return false;
    }
    // sample_context
    // 只回读原Inbox；到期标记表示截止已过且原留存确已清空，不猜唯一清除原因。
    void sample_context() {
        if (!config.context_diagnostic)
            return;
        const ContextSnapshot* retained =
            nullptr;  // 借用同一已验证快照，不复制私有正文或依赖重复getter。
        if (human) {
            const auto& snapshot = human->context_snapshot();
            if (snapshot.has_value())
                retained = &*snapshot;
        }
        if (retained) {
            context_facts.retained = true;
            retained_expires = retained->expires_at;
        } else if (context_facts.retained) {
            context_facts.cleared = true;
            context_facts.expired =
                context_facts.expired || (retained_expires && wall >= retained_expires);
            context_facts.retained = false;
            retained_expires = 0;
        }
    }
    // route_live 原固定绑定是门控关联条件，不把网络事件当授权。
    bool route_live(const InputGateRequest& r) const {
        const auto& binding = bindings[0];  // 原AI绑定，不从门控请求反推授权。
        return source_live() &&
               ((binding.has_value() && r.session == binding->session && r.route_generation == 1) ||
                (human_bound && r.session == candidate_session && r.route_generation == 2));
    }
    // scope 为两个不可变权限适配器提供原用户意图、原媒体连通及操作资格。
    std::optional<BrowserDuplexScope> scope(std::size_t path) const {
        const auto& binding = bindings[path];  // 固定当前路径的原可选绑定，后续只读同一事实。
        if (!binding.has_value() || !source_live())
            return {};
        BrowserDuplexScope result;
        result.binding = *binding;
        const bool is_ai = path == 0;
        result.authorized =
            is_ai
                ? ai && ai->voice().state() == VoiceState::Established && !ai_closed && !committed
                : human_intent && human && human->session_id() == binding->session && !human_closed;
        result.peer_ready = is_ai ? media.ready() : human_media.connected();
        result.handoff_committed = is_ai && committed;
        result.closing =
            (is_ai ? ai_closed || committed : human_closed) || cleanup_intents[path].has_value();
        if (cleanup_intents[path]) {
            result.authorized = false;  // 停止指令一经确认，采集/播放/暂停/恢复都不再获授权。
            result.cleanup_deadline = cleanup_intents[path]->deadline;
            result.cleanup_expires_at = cleanup_intents[path]->expires;
        }
        // 准入阶段由协调器严格校验；持续权限不能随Accepted→Pending阶段变化把原在途许可误撤销。
        result.acquire = result.authorized && (is_ai ? !committed : human_intent);
        result.suspend =
            result.authorized && (is_ai ? (handoff || temporary_call) && human_intent &&
                                              human_media.connected() && !human_ended
                                        : human_established && human_holdable);
        result.restore = result.authorized &&
                         (is_ai ? temporary_call && temporary_resume && human_ended &&
                                      formal_released[1] && human_media.stopped()
                                : human_established && human_holdable && human_restore_intent);
        // 临时AI只在本人新start及人际整组释放/Peer已停后授restore；永久转接仍无AI恢复权。
        result.release = result.closing || (!is_ai && temporary_call && result.authorized);
        result.operation_token = operation_tokens[path];
        if (is_ai && temporary_call)
            result.current_intent_version = ai_intent_version;
        return result;
    }
    // send 本地有界桥失败终态，不打印平台报文、凭据或SDP。
    bool send(const std::string& body) {
        if (body.empty() || !emit || !emit(body)) {
            mark_failed(SharedHostExitReason::EmitFailed);
            return false;
        }
        return true;
    }
    // next_token 使用native固定实例加单调本地计数，不接受页面自报关联标识。
    std::string next_token() { return config.instance + "-h" + std::to_string(++token_counter); }
    // ai_is_owned 原完整实际租约仍存在且未移交，Established或页面状态不能代替它。
    bool ai_is_owned() const {
        const auto& ownership = owned[0];  // 原租约可选事实，只在存在时查询唯一驱动。
        return !committed && !ai_closed && ownership.has_value() && driver.owns(*ownership);
    }
    // human_is_owned 只观察唯一执行器当前完整组，不以ICE或来电代替租约。
    bool human_is_owned() const {
        const auto& ownership = owned[1];  // 不因Peer/页面状态凭空创建人际租约。
        return !human_closed && !cleanup_intents[1] && ownership.has_value() && driver.owns(*ownership);
    }
    // pure_human_dial_allowed 仅显式纯源可主动拨号，旧受话入口不升级，不借AI或未就绪连接。
    bool pure_human_dial_allowed() const {
        return config.human_receiver && config.human_endpoint_source && source_live() &&
               connection && connection->state() == SharedConnectionState::Ready && human &&
               human->state() == HumanCallState::Available && !human_intent && !human_bound &&
               !human_closed && !human_ended && !ai && !handoff && !temporary_call &&
               !bindings[0] && !bindings[1] && now < config.deadline && wall < config.expires_at;
    }
    // pure_human_cancel_allowed 仅原已绑定且尚未建立的本人呼出可取消，不把受话或未知创建当主叫。
    bool pure_human_cancel_allowed() const {
        const bool waiting = human &&
            (human->state() == HumanCallState::Inviting ||
             human->state() == HumanCallState::Negotiating ||
             human->state() == HumanCallState::ConnectingMedia ||
             human->state() == HumanCallState::WaitingEstablished);
        return config.human_receiver && config.human_endpoint_source && pure_outbound &&
               human_intent && human_bound && human && !human_closed && !human_ended &&
               !human_established && !candidate_session.empty() &&
               human->session_id() == candidate_session && source_live() &&
               waiting &&
               now < config.deadline && wall < config.expires_at;
    }
    // bind_ordinary_candidate只固定原严格Human邀请/创建应答的leg；显式纯Human主叫不需要也不创建AI。
    bool bind_ordinary_candidate(std::string_view session) {
        const bool pure = config.human_receiver && config.human_endpoint_source; // 本机显式纯源。
        if ((!pure && (!config.temporary_calls || config.human_receiver || !temporary_call)) ||
            session.empty())
            return false;
        if (human_bound)
            return candidate_session == session;
        if (!connection || !source_live() ||
            (pure ? !human_intent || ai || bindings[0] || handoff || temporary_call : !ai_is_owned()) ||
            !human_media.bind_scope(session, 2, 1))
            return false;
        // 严格创建应答已确定原leg，先固定只读关联，再让输入仲裁器核同一原scope；绑定本身不授权。
        candidate_session = session;
        human_bound = true;
        return driver.refresh_idle_output(1, now) &&
               input.bind_route(std::string(session), 2, BrowserInputRouteKind::HumanSender);
    }
    // temporary_can_resume只读实际原完整释放与原Peer停止；用户stop或源/期限变动不洗白冻结版本。
    bool temporary_can_resume() const {
        const auto& binding = bindings[0];
        return temporary_call && temporary_suspended && !temporary_resume && human_ended &&
               formal_released[1] && human_media.stopped() && ai && !ai_closed && !committed &&
               source_live() && binding.has_value() &&
               ai_intent_version == temporary_intent_version && ai_business_deadline.has_value() &&
               ai_business_expires.has_value() && now < *ai_business_deadline &&
               wall < *ai_business_expires && now < binding->deadline && wall < binding->expires_at;
    }
    // human_hold_proof
    // 核原腿/原token/原操作与唯一驱动实态；焦点修订与呼叫修订严格分开。
    std::optional<bool> human_hold_proof(bool held, std::string_view token) const {
        const auto& binding = bindings[1];
        if (!source_live() || human_closed || !human_established || !human_holdable ||
            !binding.has_value() || !human || !human->active() || now >= binding->deadline ||
            wall >= binding->expires_at)
            return false;
        if (!human_hold_command.has_value() || human_hold_command->envelope.token != token ||
            human_hold_command->envelope.session != binding->session ||
            human_hold_command->focus_id != binding->focus ||
            human_hold_command->operation != (held ? "suspend" : "restore"))
            return {};
        if (!driver.scope_ready(1))
            return {};
        return held ? !human_is_owned() : driver.owns(*human_hold_command);
    }
    // projected_state 将受话真实阶段映射到原七字段，不伪称AI已启动。
    AiHandoffState projected_state() const {
        if (temporary_call) {
            if (temporary_resume && temporary_suspended)
                return AiHandoffState::Restoring;
            if (human_established && !human_ended)
                return human_is_owned() ? AiHandoffState::HumanActive : AiHandoffState::Suspended;
            if (human_ended)
                return ai_is_owned() ? AiHandoffState::AiActive : AiHandoffState::Suspended;
            return temporary_suspended ? AiHandoffState::Suspended : AiHandoffState::Candidate;
        }
        if (!config.human_receiver)
            return handoff ? handoff->state() : AiHandoffState::AiActive;
        if (human_ended)
            return AiHandoffState::Closed;
        if (human_closed)
            return AiHandoffState::Unknown;
        if (config.human_endpoint_source && human_intent && !human_bound)
            return AiHandoffState::Binding; // 原创建在途，不显示为已建立或虚构候选腿。
        if (human_established && human && human->active() &&
            (human_is_owned() || human->call_held()))
            return AiHandoffState::HumanActive;
        return human_is_owned() ? AiHandoffState::Acquired
               : human_bound    ? AiHandoffState::Candidate
                                : AiHandoffState::AiActive;
    }
    // ai_session_matches
    // 正式退役之后仍按原不可变绑定认腿，不能因client清会话而误路由release。
    bool ai_session_matches(std::string_view session) const {
        const auto& binding = bindings[0];  // 已退役时仍保留原不可变关联，不补授媒体权限。
        return (binding.has_value() && binding->session == session) ||
               (ai && !ai->voice().session_id().empty() && ai->voice().session_id() == session);
    }

    // ai_authorized_and_owned 原业务腿/焦点及双时钟同时匹配才允许暂停。
    bool ai_authorized_and_owned(std::string_view session, std::string_view id, std::uint64_t clock,
                                 std::uint64_t unix_ms) const override {
        const auto& binding = bindings[0];  // 原授权仍先核实际租约，绑定本身不是grant。
        return ai_is_owned() && binding.has_value() && session == binding->session &&
               id == binding->focus && clock < binding->deadline && unix_ms < binding->expires_at;
    }
    // prepare_voice_candidate
    // 只处理协调器已核原local/group邀请；原明确escalate意图授权本端接听。
    bool prepare_voice_candidate(const Envelope& invite) override {
        if (!human_intent || !human || human_bound || !connection)
            return false;
        std::string bytes;
        if (encode_envelope(invite, bytes) != DecodeResult::Ok ||
            !human_media.bind_scope(invite.session, 2, 1))
            return false;
        candidate_session = invite.session;
        human_bound = true;
        // 换为原候选scope后旧boot输出证明已经失效；只刷新未分配输出false，不申请麦克风。
        if (!driver.refresh_idle_output(1, now))
            return false;
        if (!input.bind_route(invite.session, 2, BrowserInputRouteKind::HumanSender))
            return false;
        if (!human->receive_shared("$thing/down/signaling/" + config.identity, bytes, now, wall) ||
            human->session_id() != invite.session || human->state() != HumanCallState::Ringing)
            return false;
        if (connection->router().confirm_session(SharedMqttOwner::Human,
                                                 {invite.token, invite.session},
                                                 now) != SharedRouteResult::Routed)
            return false;
        return human->accept(now);
    }
    std::optional<std::uint64_t> candidate_media_revision(std::string_view session) const override {
        // HumanCall現合同严格仅支持mediaRevision=1；必须实际完成原答案及ICE/DTLS，并已发mediaState。
        const bool output_ready = bindings[1] ? driver.scope_ready(1) : driver.idle_output_ready(1);
        if (!human_closed && human && human->session_id() == session && human_media.connected() &&
            output_ready &&
            (human->state() == HumanCallState::WaitingEstablished ||
             human->state() == HumanCallState::Connected))
            return human_media_revision;
        return {};
    }
    bool restore_authorized(std::string_view, std::string_view, std::uint64_t,
                            std::uint64_t) const override {
        return false;
    }
    void close_candidate(std::string_view session) override {
        if (human && human->session_id() == session) {
            early_human_established.reset();
            early_human_observed = false;
            human_closed = true;
            human->close();
        }
    }
    // fence_ai
    // 仅围栏不可变绑定的原AI腿；跨会话Ended可能已清空语音客户端的临时会话号。
    void fence_ai(std::string_view session) override {
        const auto& binding = bindings[0];  // 只围栏已存在的原AI腿。
        if (!ai || !binding.has_value() || binding->session != session)
            return;
        committed = true;
        if (config.udp_reply_start_receipt && ai->voice().state() == VoiceState::Established &&
            ai->shared_reply_install_gate(session, false, true, now) == Result::Stale) {
            mark_failed(SharedHostExitReason::VoiceRuntimeFailed);
            // 原绑定错误锁失败，但原PCM/driver即刻物理围栏仍必须执行，不能等宿主close兜底。
        }
        pcm.fence_capture_route(session, 1);
        driver.stop_scope(session, binding->focus, {"microphone", "speaker"});
    }
    // release_ai
    // 仅关闭已通过三屏障提交的原绑定腿；不将关闭客户端冒充正式焦点释放。
    void release_ai(std::string_view session) override {
        const auto& binding = bindings[0];  // 已提交关闭只消费原绑定，空绑定保持原无动作。
        if (ai && binding.has_value() && binding->session == session && committed) {
            ai_closed = true;
            ai->close(now);
        }
    }
    // bind_focus
    // 只在原核会话、完整资源、原意图和真实媒体均具备时创建不可变原腿绑定。
    bool bind_focus(const FocusCommand& command) {
        const bool is_ai = ai_session_matches(command.envelope.session);
        const std::size_t path = is_ai ? 0 : 1;
        const auto& existing = bindings[path];
        if (existing.has_value())
            return authority.authorities[path] && existing->session == command.envelope.session &&
                   existing->focus == command.focus_id;
        if (command.operation != "acquire" || command.expected_revision != driver.revision() ||
            command.resources.size() != 2 ||
            !std::is_permutation(command.resources.begin(), command.resources.end(),
                                 std::array<std::string, 2>{"microphone", "speaker"}.begin()) ||
            !command.expires_at || *command.expires_at <= wall ||
            *command.expires_at > config.expires_at ||
            (is_ai ? !ai || ai->voice().state() != VoiceState::Established || !media.ready() ||
                         committed || command.priority != "conversation" ||
                         command.resume_policy != "explicit"
                   : !human_intent || !human_bound || !human ||
                         human->session_id() != command.envelope.session ||
                         !human_media.connected() ||
                         (!config.human_receiver && !temporary_call &&
                          (!handoff || handoff->state() != AiHandoffState::Suspended)) ||
                         (temporary_call && (!temporary_suspended || !human_intent)) ||
                         ((config.human_receiver || temporary_call) &&
                          !candidate_media_revision(command.envelope.session)) ||
                         command.priority != "call" ||
                         (command.resume_policy != "never" && command.resume_policy != "explicit")))
            return false;
        if (!is_ai && !config.human_receiver && !temporary_call &&
            (!ai_business_deadline.has_value() || !ai_business_expires.has_value() ||
             *command.expires_at > *ai_business_expires || now >= *ai_business_deadline))
            return false;
        if (is_ai) {
            ai_business_deadline = std::min(config.deadline, now + *command.expires_at - wall);
            ai_business_expires = command.expires_at;
        }
        if (!is_ai && (config.human_receiver || temporary_call)) {
            human_business_deadline = std::min(config.deadline, now + *command.expires_at - wall);
            human_business_expires = command.expires_at;
        }
        // 已核原租约仍须在本函数具备期限对，缺失时不能建出新绑定或驱动权限。
        const auto& deadline = config.human_receiver || temporary_call ? human_business_deadline
                                                                       : ai_business_deadline;
        const auto& expires =
            config.human_receiver || temporary_call ? human_business_expires : ai_business_expires;
        if (!is_ai && (!deadline.has_value() || !expires.has_value()))
            return false;
        BrowserDuplexBinding binding{path,
                                     command.envelope.session,
                                     command.focus_id,
                                     path + 1,
                                     1,
                                     is_ai ? 0U : 1U,
                                     is_ai ? config.deadline : *deadline,
                                     is_ai ? config.expires_at : *expires,
                                     is_ai ? BrowserDuplexRole::AI : BrowserDuplexRole::Human};
        bindings[path] = binding;
        if (!is_ai)
            human_holdable = command.resume_policy == "explicit";
        if (is_ai && (!pcm.bind_capture_route(command.envelope.session, 1) ||
                      !input.bind_route(command.envelope.session, 1, BrowserInputRouteKind::Udp)))
            return false;
        // Human此处已经具备实际连通Peer/Sender；早期邀请只登记CPP关联，不能向空页面Peer绑定。
        // 发送失败进入原failed终态，绝不继续排Gate或把绑定当作已取得；重复原scope不重发绑定。
        if (!send(input_binding_command(binding)))
            return false;
        if (!driver.bind_scope(path, binding.session, binding.focus, binding.route_generation, now))
            return false;
        authority.authorities[path] = std::make_unique<BrowserDuplexFocusAuthority>(
            driver, binding, [this, path] { return scope(path); },
            is_ai ? std::optional<BrowserDuplexCleanupBounds>{}
                  : std::optional<BrowserDuplexCleanupBounds>{
                        BrowserDuplexCleanupBounds{config.deadline, config.expires_at}});
        return true;
    }
    // begin_human_cleanup先核原绑定/真实分配/水位，再固定短停止意图；不清scope或伪造实际false。
    bool begin_human_cleanup(const FocusCommand& command, std::string_view bytes) {
        if (cleanup_intents[1]) return cleanup_intents[1]->bytes == bytes;
        const auto& binding = bindings[1];
        if (!source_live() || now >= config.deadline || wall >= config.expires_at ||
            now > 9007199254740991ULL - 2000 || wall > 9007199254740991ULL - 2000 || !binding ||
            !owned[1] || formal_released[1] || !human || command.operation != "release" ||
            command.expires_at || command.envelope.session != binding->session ||
            command.focus_id != binding->focus || command.expected_revision != driver.revision() ||
            command.priority != "call" || command.resume_policy != owned[1]->resume_policy ||
            command.resources.size() != 2 ||
            !std::is_permutation(command.resources.begin(), command.resources.end(),
                                 std::array<std::string, 2>{"microphone", "speaker"}.begin()))
            return false;
        if (!human_ended && !human->can_observe_shared_focus(
                                "$thing/down/signaling/" + config.identity, bytes, now, wall))
            return false;
        cleanup_intents[1] =
            CleanupIntent{std::string(bytes), std::min(now + 2000, config.deadline),
                          std::min(wall + 2000, config.expires_at)};
        // closing使原Engine沿stop_scope停真实双路；正式release随后仍须新的两张false票据。
        return true;
    }
    // focus_event
    // 保留原控制直到新绑定false票据齐全，期间不伪造deny/成功或改变原token。
    bool focus_event(const FocusCommand& command, std::string_view bytes) {
        const std::size_t path = ai_session_matches(command.envelope.session) ? 0 : 1;
        if (!bind_focus(command))
            return false;
        if (path == 1 && command.operation == "release") {
            if (!begin_human_cleanup(command, bytes)) return false;
            if (formal_released[1])
                return focus->receive("$thing/down/signaling/" + config.identity, bytes, now,
                                      wall) ==
                       FocusEndpointResult::Handled;  // 仅重发同原实际成功，不签新停止期限或操作。
        }
        bool ready = driver.scope_ready(path) ||
                     (command.operation == "release" && driver.stopped_scope_ready(path));
        // 另一原腿若正在实际关闭，必须等两端false；已确认并持有的另一腿仍可保持通话。
        if (ready && command.operation == "release")
            for (std::size_t index = 0; index < bindings.size(); ++index)
                if (bindings[index] && !formal_released[index] && !driver.scope_ready(index) &&
                    !driver.stopped_scope_ready(index))
                    ready = false;
        if (!ready) {
            if (delayed_focus && (delayed_focus->envelope.token != command.envelope.token ||
                                  delayed_focus_bytes != bytes))
                return false;
            delayed_focus = command;
            delayed_focus_bytes = std::string(bytes);
            return true;
        }
        const auto& binding = bindings[path];
        // bind_focus 的成功不能替代此处对借用关联和实际权威端口的检查。
        if (!binding.has_value() || !authority.authorities[path])
            return false;
        if (!authority.authorities[path]->admit_binding(*binding, now, wall) &&
            command.operation != "release")
            return false;
        operation_tokens[path] = command.envelope.token;
        if (path == 1 && command.operation == "restore" && human_established)
            human_restore_intent = true; // 原已鉴权下行意图，不改变原租约期限/身份或实际事实。
        relay_control_bytes = std::string(bytes);
        const bool accepted =
            handoff &&
                    !(path == 1 && human_established &&
                      (command.operation == "suspend" || command.operation == "restore")) &&
                    handoff->state() != AiHandoffState::AiActive && command.operation != "release"
                ? handoff->receive(command.envelope, now, wall) == AiHandoffResult::Applied
                : focus->receive("$thing/down/signaling/" + config.identity, bytes, now, wall) ==
                      FocusEndpointResult::Handled;
        relay_control_bytes.clear();
        if (accepted) {
            submitted[path] = command;
            submitted_bytes[path] = std::string(bytes);
            if (path == 0 && command.envelope.event_sequence)
                ai_seq = std::max(ai_seq, *command.envelope.event_sequence);
        }
        return accepted;
    }
    // route
    // 每条原帧只交一次对应业务；协调器消费控制前仍须唯一宿主关联及严格解析。
    void route(SharedMqttOwner owner, const MqttEvent& event) {
        if (owner == SharedMqttOwner::Global &&
            event.topic == "$thing/down/action/" + config.identity) {
            if (actions) {
                const auto submission = actions->submit(event.body, now);
                if (!submission.reply.empty())
                    action_reply = submission.reply;
            }
            return;
        }
        Envelope envelope;
        if (decode_envelope(event.body, envelope) != DecodeResult::Ok)
            return;
        // 纯受话或显式普通临时通话只认原Human严格邀请；先固定原leg，不以收到邀请授权接听。
        const bool ordinary_invite = config.temporary_calls && !config.human_receiver && !handoff &&
                                     !temporary_call && ai_is_owned();
        if ((config.human_receiver || ordinary_invite) && owner == SharedMqttOwner::Human &&
            envelope.method == "sessionInvite") {
            const auto& binding = bindings[0];
            // 普通临时通话必须保留原 AI 意图版本；缺原绑定不能先接纳来电再补授关联。
            if ((ordinary_invite && !binding.has_value()) || human_bound || !human ||
                !human->receive_shared(event.topic, event.body, now, wall) ||
                human->state() != HumanCallState::Ringing ||
                human->session_id() != envelope.session)
                return;
            if (ordinary_invite) {
                temporary_call = true;
                temporary_intent_version = binding->intent_version;
                if (!bind_ordinary_candidate(envelope.session)) {
                    mark_failed(SharedHostExitReason::CandidateSetupFailed);
                    return;
                }
            } else {
                candidate_session = envelope.session;
                human_bound = true;
                if (!human_media.bind_scope(candidate_session, 2, 1) ||
                    !driver.refresh_idle_output(1, now) ||
                    !input.bind_route(candidate_session, 2, BrowserInputRouteKind::HumanSender)) {
                    mark_failed(SharedHostExitReason::CandidateSetupFailed);
                    return;
                }
            }
            if (connection->router().confirm_session(owner, {envelope.token, envelope.session},
                                                     now) != SharedRouteResult::Routed)
                mark_failed(SharedHostExitReason::CandidateSetupFailed);
            return;
        }
        if (envelope.method == "focusControl") {
            FocusCommand command;
            if (decode_focus_control(WireRoute::Signaling, event.body, command) == WireResult::Ok) {
                // 后台可先发布建立再保留焦点序号；原Core必须按原字节顺序观察，不能重编号。
                if (command.operation == "acquire" && early_human_established &&
                    !early_human_observed && human &&
                    !human_closed && !human_ended && envelope.session == candidate_session) {
                    Envelope original;
                    if (decode_envelope(early_human_established->body, original) ==
                            DecodeResult::Ok &&
                        original.event_sequence && envelope.event_sequence &&
                        *original.event_sequence < *envelope.event_sequence)
                        early_human_observed = human->receive_shared(
                            early_human_established->topic, early_human_established->body, now, wall);
                }
                focus_event(command, event.body);
            }
            return;
        }
        if (handoff &&
            // 移交协调器只消费原AI转接结果；人工保持事件由Human原共享水位严格校验。
            ((envelope.method == "callState" && ai_session_matches(envelope.session)) ||
             envelope.method == "sessionInvite" || envelope.method == "sessionEstablished" ||
             envelope.method == "sessionEnded")) {
            const auto result = handoff->receive(envelope, now, wall);
            if (envelope.method == "sessionInvite" && result != AiHandoffResult::Ignored)
                return;
            if (envelope.method == "callState" && result != AiHandoffResult::Ignored)
                return;
        }
        if (owner == SharedMqttOwner::AI && ai && !ai_closed) {
            const auto& ai_binding = bindings[0];
            // 原AI腿每帧前同步真实完整组；不能等后续tick才拒绝同pump中失权后的Start。
            if (config.udp_reply_start_receipt && ai->voice().state() == VoiceState::Established &&
                ai->shared_reply_install_gate(
                    ai_binding.has_value() ? ai_binding->session : ai->voice().session_id(),
                    ai_is_owned(), committed, now) == Result::Stale) {
                mark_failed(SharedHostExitReason::VoiceRuntimeFailed);
                return;
            }
            // 新鲜且核心可准入的已交付Start才登记反向来源；满额只拒此帧，不占网络retained槽。
            // false仍沿原receive处理失权/永久退休/duplicate，不搬动核心原200重复协议。
            if (ai->shared_reply_start_needs_relation({event.topic, event.body}) &&
                !connection->router().relate_reply_start(event.topic, event.body, now))
                return;
            const bool accepted = ai->receive_shared({event.topic, event.body}, now, wall);
            if (accepted && envelope.method == "sessionCreated" &&
                !ai->voice().session_id().empty())
                connection->router().confirm_session(
                    owner, {envelope.token, ai->voice().session_id()}, now);
            if (accepted && envelope.event_sequence) {
                ai_seq = std::max(ai_seq, *envelope.event_sequence);
                if (handoff)
                    handoff->observe_event(envelope.session, *envelope.event_sequence);
            }
            if (accepted && envelope.method == "sessionEnded" && !committed)
                ai_closed = true;
        } else if (owner == SharedMqttOwner::Human && human && envelope.method != "sessionInvite") {
            // 人工候选网络已连通不等于整组媒体已取得；私有文本只交付当前实际Human所有者。
            if (envelope.method == "contextSnapshot" &&
                (!human_is_owned() || !call_media.private_context_allowed()))
                return;
            // 早到Established先保存当前原帧；后继焦点沿Core原顺序观察，不提前授予实际所有权。
            if ((config.human_receiver || temporary_call) &&
                envelope.method == "sessionEstablished" && !human_is_owned()) {
                std::unique_ptr<cJSON, decltype(&cJSON_Delete)> data(
                    ur::device::parse_json(envelope.payload.c_str()), cJSON_Delete);
                const auto* state =
                    data ? cJSON_GetObjectItemCaseSensitive(data.get(), "state") : nullptr;
                const auto* revision =
                    data ? cJSON_GetObjectItemCaseSensitive(data.get(), "mediaRevision") : nullptr;
                if (!early_human_established && !human_closed && !human_ended && human_bound &&
                    human->state() == HumanCallState::WaitingEstablished &&
                    envelope.session == candidate_session &&
                    human->session_id() == candidate_session &&
                    human_media_revision == 1 && event.body.size() <= 65536 &&
                    !envelope.request && !envelope.code && !envelope.message_sequence &&
                    envelope.event_sequence && *envelope.event_sequence > 0 && data &&
                    cJSON_IsObject(data.get()) && cJSON_GetArraySize(data.get()) == 2 &&
                    cJSON_IsString(state) && std::string_view(state->valuestring) == "established" &&
                    cJSON_IsNumber(revision) && revision->valuedouble == 1)
                    early_human_established = event;
                return;
            }
            // Router仍可交付旧创建的合法重放；后继Inviting只交当前token，否则核心会误退休新腿。
            // 忽略原旧应答不代表当前请求已成功，也不推进当前期限或补授媒体所有权。
            if (human->state() == HumanCallState::Inviting &&
                human->retired_shared_create_rejection(envelope))
                return;
            // 仅原临时AI或显式纯Human主叫的创建请求可关联随后leg。
            const bool ordinary_originator = temporary_call ||
                (config.human_receiver && config.human_endpoint_source && human_intent);
            const bool awaiting_create = ordinary_originator && !human_bound &&
                                         human->state() == HumanCallState::Inviting;
            const bool accepted = human->receive_shared(event.topic, event.body, now, wall);
            // 仅严格关联的创建非200终态可撤销未绑定意图；未受理发布/未知应答/旧重放不清后继拨号。
            // 临时主叫AI仍持原完整组，纯主叫尚无资源；两者都不能借创建拒绝销毁共享媒体。
            if (awaiting_create && accepted && envelope.method == "sessionCreated" &&
                envelope.code && *envelope.code != 200 &&
                human->state() == HumanCallState::Available) {
                temporary_call = human_intent = false;
                pure_outbound = false;
                temporary_intent_version = 0;
            }
            if (accepted && ordinary_originator && envelope.method == "sessionCreated" &&
                envelope.code == 200 && human_bound && human->session_id() == candidate_session &&
                connection->router().confirm_session(owner, {envelope.token, candidate_session},
                                                     now) != SharedRouteResult::Routed)
                mark_failed(SharedHostExitReason::CandidateSetupFailed);
            if (accepted && envelope.method == "mediaAnswer" && envelope.code == 200 &&
                human->state() == HumanCallState::ConnectingMedia) {
                std::unique_ptr<cJSON, decltype(&cJSON_Delete)> data(
                    ur::device::parse_json(envelope.payload.c_str()), cJSON_Delete);
                const auto* revision =
                    cJSON_GetObjectItemCaseSensitive(data.get(), "mediaRevision");
                if (cJSON_IsNumber(revision) && revision->valuedouble == 1)
                    human_media_revision = 1;
            }
            if (accepted && envelope.method == "sessionEnded") {
                early_human_established.reset();
                early_human_observed = false;
                human_closed = human_ended = true;
            }
            const auto& submission = submitted[1];
            if (accepted && envelope.method == "sessionEstablished" && human_is_owned() &&
                submission.has_value() && envelope.event_sequence &&
                submission->envelope.event_sequence &&
                *envelope.event_sequence > *submission->envelope.event_sequence)
                human_established = true;
        }
    }
    // replay_human_established 在完整取得后交付同scope原帧；Core原水位不因焦点授权而重编号。
    void replay_human_established() {
        if (!early_human_established)
            return;
        Envelope original;
        if (closed || human_closed || human_ended || !human ||
            decode_envelope(early_human_established->body, original) != DecodeResult::Ok ||
            original.session != candidate_session || human->session_id() != candidate_session) {
            early_human_established.reset();
            early_human_observed = false;
            return;
        }
        if (!human_is_owned() || !submitted[1])
            return;
        const auto& acquire = *submitted[1];
        if (acquire.operation != "acquire" || !acquire.envelope.event_sequence ||
            !original.event_sequence ||
            *original.event_sequence == *acquire.envelope.event_sequence) {
            early_human_established.reset();
            early_human_observed = false;
            return;
        }
        if (!early_human_observed) {
            // 尚未观察的后继原帧仍须紧邻已实际接纳的focus水位，拒绝未知未来序号而非补洞。
            if (*original.event_sequence != *acquire.envelope.event_sequence + 1) {
                early_human_established.reset();
                return;
            }
            early_human_observed = human->receive_shared(
                early_human_established->topic, early_human_established->body, now, wall);
        }
        // ACK可能晚于焦点实际回执；只等原Core确认，不刷新期限，也不由网络建立推导所有权。
        if (!early_human_observed || human->state() != HumanCallState::Connected)
            return;
        human_established = true;
        early_human_established.reset();
        early_human_observed = false;
    }
    // poll_focus_receipts
    // 先消费真实完整回执再路由最终事件，避免同poll的QoS0事件丢失保持资格。
    // 原内容、token、完整组及driver owns校验不变；不以网络Established授权媒体。
    void poll_focus_receipts() {
        if (!focus)
            return;
        focus->poll(now, wall);
        while (auto receipt = focus->take_receipt()) {
            const auto path =
                bindings[0] && receipt->envelope.session == bindings[0]->session ? 0U : 1U;
            if (!bindings[path] || receipt->focus_id != bindings[path]->focus)
                continue;
            if (submitted[path] && submitted[path]->operation == "release" &&
                receipt->state == "released" && receipt->envelope.code == 200 &&
                focus_reply_matches(*submitted[path], *receipt))
                formal_released[path] = true;
            // 非协调器的正式release只观察已发布的原完整200事实；它不触发第二次驱动操作。
            if (path == 1 && !human_ended && human && submitted[path] &&
                (submitted[path]->operation == "release" ||
                 submitted[path]->operation == "suspend" ||
                 submitted[path]->operation == "restore" ||
                 ((config.human_receiver || temporary_call) &&
                  submitted[path]->operation == "acquire")) &&
                receipt->envelope.code == 200 && focus_reply_matches(*submitted[path], *receipt)) {
                std::string actual;
                if (submitted_bytes[path].empty() ||
                    encode_envelope(receipt->envelope, actual) != DecodeResult::Ok ||
                    !human->observe_shared_focus("$thing/down/signaling/" + config.identity,
                                                 submitted_bytes[path],
                                                 {FocusAdmission::Replay, actual}, now, wall))
                    mark_failed(SharedHostExitReason::HumanFocusReceiptRejected);
            }
            if (receipt->state == "acquired" || receipt->state == "restored") {
                // 保留原acquire控制，仅真实完整回执与原driver owns都匹配才可开始输入。
                if (submitted[path] && focus_reply_matches(*submitted[path], *receipt) &&
                    driver.owns(*submitted[path]))
                    owned[path] = submitted[path];
            }
            // 原AI完整实际暂停才冻结临时阶段；恢复须原完整组重新取得，不能用网络ACK重开采集。
            if (temporary_call && path == 0 && submitted[path] && receipt->envelope.code == 200 &&
                focus_reply_matches(*submitted[path], *receipt)) {
                if (submitted[path]->operation == "suspend" && receipt->state == "suspended" &&
                    !driver.owns(*submitted[path]))
                    temporary_suspended = true;
                if (submitted[path]->operation == "restore" && receipt->state == "restored" &&
                    driver.owns(*submitted[path])) {
                    temporary_suspended = false;
                    if (!pcm.enable_capture_route(bindings[0]->session, 1))
                        mark_failed(SharedHostExitReason::CandidateSetupFailed);
                }
            }
            // 恢复失败保留暂停及旧意图事实，只撤销页面等待提示，不猜模型或媒体已经恢复。
            if (temporary_call && path == 0 && submitted[path] &&
                submitted[path]->operation == "restore" && receipt->envelope.code &&
                receipt->envelope.code != 200 && focus_reply_matches(*submitted[path], *receipt))
                temporary_resume = false;
            if (path == 1 && submitted[path] && receipt->envelope.code == 200 &&
                focus_reply_matches(*submitted[path], *receipt) &&
                (receipt->state == "suspended" || receipt->state == "restored"))
                human_hold_command = submitted[path]; // 仅实际原完整200，网络ACK不能进入证明账本。
            if (submitted[path] && focus_reply_matches(*submitted[path], *receipt) &&
                receipt->envelope.code)
                send("{\"kind\":\"focusReceipt\",\"state\":\"" + receipt->state +
                     "\",\"revision\":" + std::to_string(receipt->revision) + ",\"operation\":\"" +
                     submitted[path]->operation +
                     "\",\"code\":" + std::to_string(*receipt->envelope.code) +
                     ",\"receiptIndex\":" + std::to_string(++receipt_counter) + "}");
        }
        replay_human_established();
    }
    // flush_commands 所有实际回执由原桥回流，发送仅标记原票据已交付。
    void flush_commands() {
        if (auto command = input.take_command())
            send(*command);
        while (auto command = human_media.take_command())
            send(*command);
        while (auto command = pcm.take_command()) {
            if (command->kind == BrowserAudioCommandKind::PlaybackStop && config.stopped_timing) {
                if (const auto timing = config.stopped_timing(command->epoch)) {
                    send(browser_media_final(command->id, *timing));
                    // 与原独立AI入口一致：只取该停止代次冻结值，并在停播指令前投影同钟计时。
                    send(browser_reply_timing(
                        command->id, *timing,
                        config.last_voice_sent_ms ? config.last_voice_sent_ms() : std::nullopt,
                        first_text_response == command->id ? first_text_at : std::nullopt));
                }
            }
            send(browser_command(*command));
        }
        for (unsigned i = 0; i < 8; ++i) {
            auto frame = pcm.take_playback();
            if (!frame)
                break;
            send(browser_frame(*frame));
        }
    }
    // control_name 仅映射核心闭集状态，Unknown不解释为远端未执行。
    static const char* control_name(HumanCallControlState state) {
        switch (state) {
        case HumanCallControlState::Idle:
            return "idle";
        case HumanCallControlState::Pending:
            return "pending";
        case HumanCallControlState::Completed:
            return "completed";
        case HumanCallControlState::Rejected:
            return "rejected";
        case HumanCallControlState::Unknown:
            return "unknown";
        }
        return "unknown";
    }
    // can_control 只开放原保持策略及实际租约资格，界面许可不替代核心严校验。
    bool can_control(bool resume) const {
        const auto& binding = bindings[1];
        if (!source_live() || failed || human_closed || !human_established || !human_holdable ||
            !human || !human->active() || !binding.has_value() || now >= binding->deadline ||
            wall >= binding->expires_at ||
            (!config.human_receiver && !temporary_call && !committed) ||
            human->call_control_state() == HumanCallControlState::Pending ||
            human->call_control_state() == HumanCallControlState::Unknown)
            return false;
        if (!resume)
            return !human->call_held() && human_is_owned() && driver.scope_ready(1);
        return human->call_held() && human_hold_command &&
               human_hold_proof(true, human_hold_command->envelope.token).value_or(false);
    }
    // project_control 独立投影本地实际状态，不追加MQTT字段或复用七字段投影。
    void project_control() {
        const auto state = human ? human->call_control_state() : HumanCallControlState::Idle;
        const auto boolean = [](bool value) { return value ? "true" : "false"; };
        const std::string body =
            std::string("{\"kind\":\"humanControlState\",\"canHold\":") +
            boolean(can_control(false)) + ",\"canResume\":" + boolean(can_control(true)) +
            ",\"held\":" + boolean(human && human->active() && human->call_held()) +
            ",\"controlState\":\"" + control_name(state) + "\"}";
        if (body != control_projection) {
            control_projection = body;
            send(body);
        }
        // 临时恢复投影仅给本人操作提示，不包含云端腿、许可或意图版本，也不自动发送start。
        if (config.temporary_calls) {
            const std::string temporary =
                std::string("{\"kind\":\"temporaryCallState\",\"canResumeAI\":") +
                boolean(temporary_can_resume()) +
                ",\"restoring\":" + boolean(temporary_resume && temporary_suspended) + "}";
            if (temporary != temporary_projection) {
                temporary_projection = temporary;
                send(temporary);
            }
        }
    }
    // observe_control
    // 只编码原核心观察；token仅认证本地页面内存关联，不写普通日志。
    void observe_control(const HumanCallControlReceipt& receipt) {
        if (control_receipt_index >= 512 ||
            (receipt.operation != "hold" && receipt.operation != "resume"))
            return;
        std::unique_ptr<cJSON, decltype(&cJSON_Delete)> value(cJSON_CreateObject(), cJSON_Delete);
        if (!value || !cJSON_AddStringToObject(value.get(), "kind", "humanControlReceipt") ||
            !cJSON_AddStringToObject(value.get(), "operation", receipt.operation.c_str()) ||
            !cJSON_AddStringToObject(value.get(), "controlToken", receipt.token.c_str()) ||
            !cJSON_AddStringToObject(value.get(), "state", control_name(receipt.state)) ||
            !cJSON_AddStringToObject(value.get(), "reason", receipt.reason.c_str()) ||
            !cJSON_AddNumberToObject(value.get(), "revision",
                                     static_cast<double>(receipt.revision)) ||
            !cJSON_AddNumberToObject(value.get(), "code", receipt.code) ||
            !cJSON_AddBoolToObject(value.get(), "actualApplied", receipt.actual_applied) ||
            !cJSON_AddBoolToObject(value.get(), "actualHeld", receipt.actual_held) ||
            !cJSON_AddNumberToObject(value.get(), "receiptIndex", ++control_receipt_index))
            return;
        std::unique_ptr<char, decltype(&cJSON_free)> bytes(cJSON_PrintUnformatted(value.get()),
                                                           cJSON_free);
        if (bytes)
            send(bytes.get());
    }
    // project_handoff 按原C++事实控制页面按钮；页面按钮值不能反向授予会话权限。
    void project_handoff() {
        const auto state = projected_state();
        const bool can_escalate = pure_human_dial_allowed() ||
            (!config.human_receiver && !handoff && !temporary_call && ai_is_owned() && human &&
             human->state() == HumanCallState::Available);
        const bool ringing = (human_intent || config.human_receiver || temporary_call) && human &&
                             human->state() == HumanCallState::Ringing;
        const bool can_cancel = pure_human_cancel_allowed() ||
            (handoff && !handoff->local_session().empty() && !committed &&
             (state == AiHandoffState::Candidate || state == AiHandoffState::Suspended));
        const bool can_hangup =
            (config.human_receiver || temporary_call
                 ? human_established && human && (human_is_owned() || human->call_held())
                 : committed) &&
            human && human->active() && !human_closed;
        const auto boolean = [](bool value) { return value ? "true" : "false"; };
        const std::string body =
            "{\"kind\":\"handoffState\",\"state\":" + std::to_string(static_cast<int>(state)) +
            ",\"canEscalate\":" + boolean(can_escalate) + ",\"canAccept\":" + boolean(ringing) +
            ",\"canReject\":" + boolean(ringing) + ",\"canCancel\":" + boolean(can_cancel) +
            ",\"canHangup\":" + boolean(can_hangup) + "}";
        if (body != previous_projection) {
            previous_projection = body;
            send(body);
        }
        if (config.human_endpoint_source && human && human->endpoint_source_receipt()) {
            const auto& receipt = *human->endpoint_source_receipt();
            const bool terminal = receipt.state == "committed" || receipt.state == "aborted" ||
                                  receipt.state == "failed";
            const bool available =
                human->endpoint_source_available(now, wall) && !receipt.pending && !receipt.unknown;
            const auto state = receipt.unknown          ? "unknown"
                               : !receipt.state.empty() ? receipt.state.c_str()
                               : receipt.pending        ? "pending"
                               : receipt.code >= 400    ? "rejected"
                                                        : "idle";
            const std::string projection =
                std::string("{\"kind\":\"endpointSourceState\",\"state\":\"") + state +
                "\",\"canPrepare\":" + boolean(available && (receipt.handoff.empty() || terminal)) +
                ",\"canCommit\":" +
                boolean(available && receipt.state == "ready" && !receipt.candidate.empty()) +
                ",\"canAbort\":" + boolean(human->endpoint_source_abort_available(now, wall)) +
                ",\"pending\":" + boolean(receipt.pending) +
                ",\"unknown\":" + boolean(receipt.unknown) + "}";
            if (projection != previous_endpoint_projection) {
                previous_endpoint_projection = projection;
                send(projection);
            }
        }
    }
};

SharedHandoffHost::SharedHandoffHost(MqttPort& network, PumpedVoiceMediaPort& media,
                                     BrowserPcm& pcm, SharedHandoffHostConfig config, Source paired,
                                     Emit emit, ActionEngine* actions)
    : impl_(std::make_unique<Impl>(network, media, pcm, std::move(config), std::move(paired),
                                   std::move(emit), actions)) {}
SharedHandoffHost::~SharedHandoffHost() {
    close(impl_->now);
}

// receive_local 驱动事实各自严格匹配；普通操作不接受原session/focus自报授权。
bool SharedHandoffHost::receive_local(std::string_view body, std::uint64_t now,
                                      std::uint64_t wall) {
    auto& h = *impl_;
    // 独立旁证提前消费，不让附加本地帧触发原Input/Media
    // poll、时钟更新或门控出票。
    if (const auto diagnostic = h.human_media.receive_stop_diagnostic(
            body, h.config.context_diagnostic && h.ready && !h.closed && !h.failed))
        return *diagnostic;
    // 显式纯Human源不让回退的宿主时钟提交新请求，也不提前覆盖原已观察时钟；旧受话/AI路径不变。
    if (h.config.human_receiver && h.config.human_endpoint_source &&
        (now < h.now || wall < h.wall))
        return false;
    h.now = now;
    h.wall = wall;
    if (h.closed || h.failed)
        return false;
    if (!h.ready) {
        BrowserInputOwnershipEvent early;
        if (decode_browser_input_ownership(body, early) && early.idle &&
            early.boot_generation == h.config.boot && h.paired && h.paired()) {
            if (h.early_input_idle && *h.early_input_idle != body)
                return false;
            h.early_input_idle = std::string(body);
            return true;
        }
    }
    if (h.ready && (h.input.receive(body, now) || h.human_media.receive(body, now)))
        return true;
    LocalIntent intent;
    if (local_intent(body, intent)) {
        if (!h.human)
            return false;
        if (intent.operation == "humanInputStop") {
            const auto& binding = h.bindings[1];
            if (!h.config.human_endpoint_source || !binding || !h.human_established ||
                !h.human->active() || h.human_closed || h.human_ended || !h.source_live() ||
                now >= binding->deadline || wall >= binding->expires_at)
                return false;
            if (h.human_input_stop_requested)
                return h.driver.stop_input(binding->session, binding->focus,
                                           now);  // 仍核原时钟/已知实际状态，不续票。
            if (!h.driver.stop_input(binding->session, binding->focus, now)) return false;
            h.human_input_stop_requested = true;
            h.flush_commands();  // 排到真实HAL，仍等待原输入false，不把请求当停止完成。
            return true;
        }
        if (intent.operation == "endpointPrepare")
            return h.config.human_endpoint_source &&
                   h.human->prepare_endpoint_source(intent.target, now, wall);
        if (intent.operation == "endpointCommit")
            return h.config.human_endpoint_source && h.human->commit_endpoint_source(now, wall);
        if (intent.operation == "endpointAbort")
            return h.config.human_endpoint_source && h.human->abort_endpoint_source(now, wall);
        if (intent.operation == "dial") {
            // 纯Human源只复用本人dial与sessionCreate；此处不建leg、Focus或采播许可。
            if (h.config.human_receiver) {
                if (!h.pure_human_dial_allowed() || intent.target_kind != "device" ||
                    !h.human->dial(intent.target, now))
                    return false;
                h.human_intent = true;
                h.pure_outbound = true;
                return true;
            }
            const auto& binding = h.bindings[0];
            if (!h.config.temporary_calls || h.config.human_receiver || h.handoff ||
                h.temporary_call || !h.ai_is_owned() || !binding.has_value() || !h.human ||
                h.human->state() != HumanCallState::Available || intent.target_kind != "device" ||
                !h.human->dial(intent.target, now))
                return false;
            h.temporary_call = true;
            h.human_intent = true;
            h.temporary_intent_version = binding->intent_version;
            return true;
        }
        if (intent.operation == "escalate") {
            const auto& binding = h.bindings[0];
            const auto& ownership = h.owned[0];
            // 分享引用仅来自当前已配对HAL；本地配对仍不是本人授权或云端目录证明。
            if (intent.consent_id && (!h.ready || !h.paired || !h.paired()))
                return false;
            if (h.handoff || h.temporary_call || !h.ai_is_owned() || !binding.has_value() ||
                !ownership.has_value() || !h.ai_business_deadline.has_value() ||
                !h.ai_business_expires.has_value())
                return false;
            h.human_intent = true;
            const auto expires = ownership->expires_at.value_or(0);
            if (expires <= wall)
                return false;
            h.handoff = std::make_unique<AiHumanHandoff>(
                *h.handoff_publisher, h, *h.handoff_focus,
                AiHandoffConfig{"$thing/up/signaling/" + h.config.identity, binding->session,
                                binding->focus, "", 0, h.driver.revision(), h.ai_seq,
                                *h.ai_business_deadline, *h.ai_business_expires});
            return h.handoff->begin(h.next_token(), intent.target_kind, intent.target, now, wall,
                                    intent.consent_id
                                        ? std::optional<std::string_view>(*intent.consent_id)
                                        : std::nullopt);
        }
        if (intent.operation == "cancel") {
            if (h.pure_human_cancel_allowed()) {
                h.human_closed = true; // 本人停止先围栏，原end负责撤输入意图与实际Peer停止。
                return h.human->end(now);
            }
            return h.handoff && h.handoff->cancel(h.next_token(), now, wall);
        }
        if (intent.operation == "accept") {
            if ((!h.config.human_receiver && !h.human_intent && !h.temporary_call) ||
                !h.human->accept(now))
                return false;
            h.human_intent = true;
            return true;
        }
        if (intent.operation == "reject")
            return (h.human_intent || h.config.human_receiver || h.temporary_call) &&
                   h.human->state() == HumanCallState::Ringing && h.human->reject(now);
        if (intent.operation == "hold" || intent.operation == "resume")
            return h.can_control(intent.operation == "resume") &&
                   h.human->control_call(intent.operation, now);
        if (intent.operation == "hangup") {
            if (!(h.config.human_receiver || h.temporary_call
                      ? h.human_established && (h.human_is_owned() || h.human->call_held())
                      : h.committed) ||
                h.human_closed || !h.human->active())
                return false;
            h.human_closed = true;
            return h.human->end(now);
        }
    }
    BrowserRequest request;
    if (!decode_browser_request(body, request))
        return false;
    if (request.kind == "ready") {
        if (h.ready || request.format.sample_rate != 48000 || request.format.channels != 1 ||
            request.format.frame_ms != 20 || !h.config.boot || now >= h.config.deadline ||
            wall >= h.config.expires_at || !h.paired || !h.paired() ||
            !h.pcm.ready(request.format, request.aec))
            return false;
        h.ready = true;
        h.initialized = h.driver.initialize(now);
        if (h.early_input_idle) {
            if (!h.input.receive(*h.early_input_idle, now))
                return false;
            h.early_input_idle.reset();
        }
        return h.initialized;
    }
    if (request.kind == "outputGated")
        return h.ready && h.pcm.output_gated(request.gate_ticket, request.output_enabled, now);
    if (!h.ai)
        return false;
    if (request.kind == "captured") {
        h.pcm.captured_route(request.capture_session, request.capture_generation, request.id,
                             request.pcm);
        return true;
    }
    if (request.kind == "played") {
        h.pcm.played(request.epoch, request.ticket);
        return true;
    }
    if (request.kind == "start" && h.temporary_can_resume()) {
        h.temporary_resume = true;
        if (h.ai->start_input(now) == Result::Applied)
            return true; // 只发新输入请求；原restore实际ACK前绝不启用采集路由。
        h.temporary_resume = false;
        return false;
    }
    if (request.kind == "start")
        return h.ai_is_owned() && h.pcm.enable_capture_route(h.ai->voice().session_id(), 1) &&
               h.ai->start_input(now) == Result::Applied;
    if (request.kind == "stop") {
        // 真正本人停止必须使原冻结恢复意图失效；平台暂停清账本不经过此入口。
        if (h.temporary_call && h.ai_intent_version < 9007199254740991ULL)
            ++h.ai_intent_version;
        const auto result = h.ai->stop_input(now);
        return result == Result::Applied || (h.temporary_call && result == Result::Duplicate);
    }
    if (request.kind == "cancel")
        return request.id == h.ai->voice().response_id() &&
               h.ai->cancel_response(now) == Result::Applied;
    if (request.kind == "sendText") {
        // 本地受理和云端ACK分开报告；保持原权限/返回结果，未协商文字不能静默成盲等。
        const auto result = h.committed ? Result::Stale
                                       : h.ai->send_text(request.text, request.audio, now); // 原状态机受理结果，不是模型成功。
        const char* outcome = result == Result::Applied       ? "submitted"
                              : result == Result::Busy        ? "busy"
                              : result == Result::Unsupported ? "unsupported"
                                                              : "rejected"; // 固定界面枚举，不携带输入身份或正文。
        if (!h.send(std::string("{\"kind\":\"textSubmitted\",\"result\":\"") + outcome + "\"}"))
            return false;
        return result == Result::Applied;
    }
    if (request.kind == "end") {
        h.ai_closed = true;
        return h.ai->end(now) == Result::Applied;
    }
    return false;
}

// poll 从真实票据推进原单连接；AI结束只结束AI业务，不摧毁已接通Human。
bool SharedHandoffHost::poll(std::uint64_t now, std::uint64_t wall) {
    auto& h = *impl_;
    h.now = now;
    h.wall = wall;
    // 拆分仅为记录原短路链已命中的条件；顺序及每个原业务调用次数保持不变。
    if (h.closed)
        return h.reject_poll(SharedHostExitReason::Closed);
    if (h.failed)
        return h.reject_poll(SharedHostExitReason::PriorFailure);
    if (now >= h.config.deadline)
        return h.reject_poll(SharedHostExitReason::MonotonicDeadline);
    if (wall >= h.config.expires_at)
        return h.reject_poll(SharedHostExitReason::WallDeadline);
    if (!h.ready)
        return true;
    if (h.disposing) {
        h.flush_commands();
        if (h.human_media.stopped()) {
            h.mark_exit(SharedHostExitReason::DisposalStopped);
            close(now);
            return false;
        }
        if (!h.dispose_deadline || now >= *h.dispose_deadline)
            return h.reject_poll(SharedHostExitReason::DisposalDeadline);
        if (h.failed)
            return h.reject_poll(SharedHostExitReason::PriorFailure);
        if (!h.source_live())
            return h.reject_poll(SharedHostExitReason::DisposalSourceUnavailable);
        return true;
    }
    const bool human_live = h.human_media.poll(now);
    // 尚未请求Human完整组的候选失效只隔离本候选；原AI实租约仍有效时不关闭共享连接。
    // 已暂停AI或已请求Human租约时没有这个保证，仍按未知失败安全退出，绝不猜恢复。
    if (!human_live && !h.human_closed && h.human_bound && !h.bindings[1] && h.ai_is_owned()) {
        h.human_closed = true;
        if (h.human)
            h.human->close();
    }
    if (!h.source_live())
        return h.reject_poll(SharedHostExitReason::SourceUnavailable);
    if (!h.input.poll(now))
        return h.reject_poll(SharedHostExitReason::InputGateFailed);
    if (!human_live && !h.human_closed)
        return h.reject_poll(SharedHostExitReason::HumanMediaFailed);
    if (!h.pcm.poll_output_gate(now))
        return h.reject_poll(SharedHostExitReason::PcmOutputGateFailed);
    if (!h.driver.poll(now))
        return h.reject_poll(SharedHostExitReason::DuplexDriverFailed);
    if (h.human_input_stop_requested && !h.human_input_stop_reported && h.human &&
        h.human->active() && h.bindings[1] && !h.human->track_control_pending() &&
        h.driver.input_stopped(h.bindings[1]->session, h.bindings[1]->focus)) {
        if (!h.human->control_track("voice", true, now))
            return h.reject_poll(SharedHostExitReason::HumanFocusReceiptRejected);
        h.human_input_stop_reported = true;  // 仅实际false后复用原轨状态报告，不发新Focus许可。
    }
    if (!h.connection && h.driver.initial_idle_observation()) {
        if (h.config.udp_reply_start_receipt &&
            (h.config.human_receiver || !h.pcm.reply_install_ready() ||
             !h.media.udp_reply_start_receipt_ready()))
            return h.reject_poll(SharedHostExitReason::CapabilityEncodingFailed);
        VoiceCapability capability{{true, h.pcm.aec_available() && !h.config.half_duplex_only,
                                    h.pcm.aec_available()},
                                   {48000, 1, 20},
                                   1,
                                   !h.config.human_receiver};
        capability.udp_reply_start_receipt = h.config.udp_reply_start_receipt;
        Envelope report;
        if (handoff_capability_command(h.config.instance + "-cap", capability, h.driver, report,
                                       h.config.human_receiver,
                                       h.config.human_endpoint_source &&
                                           (h.config.human_receiver || h.config.temporary_calls)) != WireResult::Ok)
            return h.reject_poll(SharedHostExitReason::CapabilityEncodingFailed);
        h.connection = std::make_unique<SharedDeviceConnection>(
            h.network, h.config.identity, std::move(report), h.config.deadline);
        h.handoff_publisher = std::make_unique<HandoffPublisher>(*h.connection);
        h.focus = std::make_unique<SharedHandoffFocusEndpoint>(
            h.connection->port(SharedMqttOwner::Global), h.driver, h.authority,
            "$thing/up/signaling/" + h.config.identity,
            "$thing/down/signaling/" + h.config.identity, h.config.deadline);
        h.handoff_focus = std::make_unique<HandoffFocusRelay>(
            *h.focus, [&h](const FocusCommand& command, const FocusSubmission& actual,
                           std::uint64_t clock, std::uint64_t unix_ms) {
                if (!h.human || !h.bindings[1] ||
                    command.envelope.session != h.bindings[1]->session)
                    return;
                FocusCommand original;
                if (h.relay_control_bytes.empty() ||
                    decode_focus_control(WireRoute::Signaling, h.relay_control_bytes, original) !=
                        WireResult::Ok ||
                    original.envelope.token != command.envelope.token ||
                    original.envelope.session != command.envelope.session ||
                    original.envelope.event_sequence != command.envelope.event_sequence ||
                    original.operation != command.operation ||
                    original.focus_id != command.focus_id ||
                    original.expected_revision != command.expected_revision ||
                    !h.human->observe_shared_focus("$thing/down/signaling/" + h.config.identity,
                                                   h.relay_control_bytes, actual, clock, unix_ms))
                    h.mark_failed(SharedHostExitReason::SharedFocusRelayRejected);
            });
        if (!h.connection->start(h.config.mqtt, now))
            return h.reject_poll(SharedHostExitReason::ConnectionStartFailed);
    }
    if (h.connection) {
        h.connection->poll(now, wall);
        if (h.connection->state() == SharedConnectionState::Failed) {
            // 原 MQTT 断连立即清除业务私有留存；Human
            // 关闭仍沿原完整组围栏回调，不能伪造正式释放。
            if (h.human)
                h.human->close();
            return h.reject_poll(SharedHostExitReason::ConnectionFailed);
        }
        if (h.connection->state() == SharedConnectionState::Ready && !h.human) {
            if (h.config.udp_reply_start_receipt &&
                (h.config.human_receiver || !h.pcm.reply_install_ready() ||
                 !h.media.udp_reply_start_receipt_ready()))
                return h.reject_poll(SharedHostExitReason::CapabilityEncodingFailed);
            VoiceCapability capability{{true, h.pcm.aec_available() && !h.config.half_duplex_only,
                                        h.pcm.aec_available()},
                                       {48000, 1, 20},
                                       1,
                                       !h.config.human_receiver};
            capability.udp_reply_start_receipt = h.config.udp_reply_start_receipt;
            if (!h.config.human_receiver)
                h.ai = std::make_unique<VoiceRuntime>(h.connection->port(SharedMqttOwner::AI),
                                                      h.media, h.config.identity, capability,
                                                      h.config.instance, BorrowedVoiceConnection{});
            if (h.ai && h.config.context_diagnostic)
                h.ai->enable_failure_diagnostics(); // 在原start前启用，不能在失败后猜测首因。
            h.human = std::make_unique<HumanCallClient>(
                h.connection->port(SharedMqttOwner::Human), h.call_media, h.config.identity,
                h.config.instance + "-human",
                HumanCallConfig{"", true, false, HumanCallMode::Voice});
            if (h.config.human_endpoint_source &&
                !h.human->set_endpoint_source([&h](std::string_view session, std::uint64_t now,
                                                   std::uint64_t wall,
                                                   HumanEndpointSourceOperation operation) {
                    const auto& binding = h.bindings[1];
                    return h.source_live() && !h.closed && !h.failed && h.human_established &&
                           !h.human_closed && !h.human_ended && binding &&
                           binding->session == session && now < binding->deadline &&
                           wall < binding->expires_at && h.owned[1] && !h.formal_released[1] &&
                           !h.cleanup_intents[1] && h.driver.scope_ready(1) &&
                           (operation == HumanEndpointSourceOperation::Abort
                                ? (!h.human_input_stop_requested ||
                                   h.driver.input_stopped(binding->session, binding->focus))
                                : (!h.human_input_stop_requested && h.human_is_owned()));
                }))
                return h.reject_poll(SharedHostExitReason::HumanClientStartFailed);
            h.human->observe_control(
                [&h](const HumanCallControlReceipt& receipt) { h.observe_control(receipt); });
            if (h.config.context_diagnostic) {
                h.human->enable_rejection_diagnostics(); // 必须在原start前开启，拒帧后不猜正文。
                h.human->observe_context_ack([&h](bool accepted) {
                    h.context_facts.ack_attempts =
                        std::min(4096U, h.context_facts.ack_attempts + 1);
                    if (accepted)
                        h.context_facts.ack_accepted =
                            std::min(4096U, h.context_facts.ack_accepted + 1);
                    h.sample_context();
                });
            }
            if (h.ai && h.ai->start_shared(now) != Result::Applied)
                return h.reject_poll(SharedHostExitReason::AiClientStartFailed);
            if (!h.human->start_shared(now))
                return h.reject_poll(SharedHostExitReason::HumanClientStartFailed);
        }
        // 原实际完成先进入协调器，防止平台极快提交事件抢在本地take_result之前被丢弃。
        h.poll_focus_receipts();
        if (h.handoff)
            h.handoff->poll(now, wall);
        if (h.human_media.connected() && h.driver.idle_output_ready(1) && h.human &&
            h.human->state() == HumanCallState::ConnectingMedia)
            h.human->media_connected(now);
        for (auto owner : {SharedMqttOwner::AI, SharedMqttOwner::Human, SharedMqttOwner::Global})
            for (unsigned i = 0; i < 32; ++i) {
                auto event = h.connection->take_message(owner);
                if (!event)
                    break;
                h.route(owner, *event);
            }
    }
    if (h.delayed_focus) {
        const auto command = *h.delayed_focus;
        const auto path = h.ai_session_matches(command.envelope.session) ? 0U : 1U;
        if (h.driver.scope_ready(path) ||
            (command.operation == "release" && h.driver.stopped_scope_ready(path))) {
            const auto bytes = h.delayed_focus_bytes;
            h.delayed_focus.reset();
            h.delayed_focus_bytes.clear();
            if (!h.focus_event(command, bytes))
                return h.reject_poll(SharedHostExitReason::DelayedFocusFailed);
        }
    }
    h.poll_focus_receipts();
    if (h.handoff)
        h.handoff->poll(now, wall);
    if (h.human) {
        if (auto offer = h.human_media.take_offer())
            h.human->offer_ready(*offer, now);
        if (h.human_media.connected() && h.driver.idle_output_ready(1) &&
            h.human->state() == HumanCallState::ConnectingMedia)
            h.human->media_connected(now);
        h.human->poll(now, wall);
    }
    if (h.ai && !h.ai_closed) {
        const auto& binding = h.bindings[0];
        if (h.config.udp_reply_start_receipt && h.ai->voice().state() == VoiceState::Established &&
            h.ai->shared_reply_install_gate(
                binding.has_value() ? binding->session : h.ai->voice().session_id(),
                h.ai_is_owned(), h.committed, now) == Result::Stale)
            return h.reject_poll(SharedHostExitReason::VoiceRuntimeFailed);
        h.ai->poll(now, wall);
        if (h.ai->state() == RuntimeState::Failed)
            return h.reject_poll(SharedHostExitReason::VoiceRuntimeFailed);
        if (h.ai->voice().state() == VoiceState::Failed)
            return h.reject_poll(SharedHostExitReason::VoiceSessionFailed);
        // 原VoiceRuntime仅在关联inputSend ACK后产生固定枚举，不输出原输入ID、正文或供应商信息。
        while (auto text_result = h.ai->take_text_result()) {
            if (!h.send(std::string("{\"kind\":\"textStatus\",\"result\":\"") +
                        text_result->result + "\"}"))
                return h.reject_poll(SharedHostExitReason::EmitFailed);
        }
        while (auto content = h.ai->take_content()) {
            if (h.committed || (h.handoff && !h.handoff->ai_forwarding_allowed()))
                continue;
            if (content->envelope.method == "respCreated") {
                // 首字关联只由正式创建推进；A/B交错后旧A增量不能重新冒充首字。
                // 单槽被新回复替代时旧回复计时明确缺失，不缓存无界正文或猜测旧时间。
                if (h.first_text_response != content->response_id) {
                    h.first_text_response = content->response_id;
                    h.first_text_at.reset();
                }
            } else if (content->envelope.method == "respTextDelta") {
                if (h.first_text_response == content->response_id && !h.first_text_at)
                    h.first_text_at = now;
                h.send(browser_text(content->text));
            } else if (content->envelope.method == "respSttDone")
                h.send(browser_text("\n用户：" + content->text + "\n助手：", content->response_id));
            else if (content->envelope.method == "toolCallStart" ||
                     content->envelope.method == "toolCallResult")
                h.send(browser_tool_status(*content));
            else if (content->envelope.method == "respTextDone")
                h.send(browser_text("\n"));
            else if (content->envelope.method == "respDone" && content->status == "completed" &&
                     h.config.timing)
                h.send(browser_completion(content->response_id, content->audio_state,
                                          h.config.timing()));
            else if (content->envelope.method == "respDone" && content->status == "cancelled")
                h.send("{\"kind\":\"responseCancelled\"}");
        }
        const bool owned = h.ai_is_owned();
        if (h.previous_voice != h.ai->voice().state() || owned != h.previous_owned) {
            h.previous_voice = h.ai->voice().state();
            h.previous_owned = owned;
            h.send(owned ? std::string("{\"kind\":\"state\",\"established\":true,"
                                       "\"interactionMode\":\"") +
                               (h.ai->voice().mode() == Mode::FullDuplex ? "fullDuplex"
                                                                         : "halfDuplex") +
                               "\",\"text\":\"AI完整音频租约已建立，可以输入。\"}"
                         : "{\"kind\":\"state\",\"established\":false,\"text\":"
                           "\"等待真实完整音频租约。\"}");
        }
    }
    if (h.actions && h.connection) {
        h.actions->poll(now);
        if (!h.action_reply)
            h.action_reply = h.actions->take_reply();
        if (h.action_reply &&
            h.connection->port(SharedMqttOwner::Global)
                .publish("$thing/up/action/" + h.config.identity, *h.action_reply))
            h.action_reply.reset();
    }
    h.project_handoff();
    h.project_control();
    h.flush_commands();
    // 临时通话只销毁其原Peer，保留原AI连接和暂停租约，等待本人新start及正式restore。
    if (h.temporary_call && !h.temporary_retiring && h.human_ended && h.formal_released[1] &&
        h.driver.quiescent_scope_ready(1)) {
        h.temporary_retiring = true;
        h.call_media.dispose();
        h.flush_commands();
    }
    // 原业务终态和所有正式租约释放后才销毁Peer；不以sessionEnded或输出静音冒充整组释放。
    if ((h.committed || h.config.human_receiver) && h.human_ended &&
        (!h.bindings[0] || h.formal_released[0]) && (!h.bindings[1] || h.formal_released[1]) &&
        h.focus && h.focus->settled()) {
        h.disposing = true;
        h.dispose_deadline = now + 2000;
        h.call_media.dispose();
        h.flush_commands();
    }
    return h.failed ? h.reject_poll(SharedHostExitReason::PriorFailure) : true;
}
void SharedHandoffHost::close(std::uint64_t now) {
    auto& h = *impl_;
    if (h.closed)
        return;
    h.ai_closed = h.human_closed = true;
    h.early_human_established.reset();
    h.early_human_observed = false;
    if (h.ai)
        h.ai->close(now);
    if (h.human)
        h.human->close();
    h.call_media.dispose(); // 整个宿主终止也释放未预约boot探针，不把client初始close当此权限。
    if (h.focus)
        h.focus->close();
    h.input.disconnect();
    h.pcm.disconnect();
    if (h.actions)
        h.actions->close();
    if (h.connection)
        h.connection->close();
    h.closed = true;
}
SharedConnectionState SharedHandoffHost::connection_state() const {
    return impl_->connection ? impl_->connection->state() : SharedConnectionState::Idle;
}
VoiceState SharedHandoffHost::voice_state() const {
    return impl_->ai ? impl_->ai->voice().state() : VoiceState::Idle;
}
AiHandoffState SharedHandoffHost::handoff_state() const {
    return impl_->projected_state();
}
bool SharedHandoffHost::ai_owned() const {
    return impl_->ai_is_owned();
}
const ContextSnapshot* SharedHandoffHost::context_snapshot() const {
    if (!impl_->human)
        return nullptr;
    const auto& snapshot = impl_->human->context_snapshot();  // 借用同一个已验证原快照。
    return snapshot.has_value() ? &*snapshot : nullptr;
}
SharedContextDiagnostic SharedHandoffHost::context_diagnostic() const {
    impl_->sample_context();
    return impl_->context_facts;
}
std::string SharedHandoffHost::context_diagnostic_line() const {
    if (!impl_->config.context_diagnostic)
        return {};
    const auto facts = context_diagnostic(); // 仅脱敏固定事实，构造不反射任何私有字段。
    const auto boolean = [](bool value) { return value ? "true" : "false"; };
    return std::string("{\"kind\":\"contextDiagnostic\",\"retained\":") + boolean(facts.retained) +
           ",\"cleared\":" + boolean(facts.cleared) + ",\"expired\":" + boolean(facts.expired) +
           ",\"ackAttempts\":" + std::to_string(facts.ack_attempts) +
           ",\"ackAccepted\":" + std::to_string(facts.ack_accepted) + "}";
}
// startup_diagnostic_line
// 只采样原状态，不发送协议、申请资源或用UI就绪替代实际门控。
std::string SharedHandoffHost::startup_diagnostic_line() const {
    if (!impl_->config.context_diagnostic)
        return {};
    // 枚举仅转换为固定标签；未知内存值保留unknown，不反射原报文或自由异常。
    const auto connection_name = [](SharedConnectionState state) {
        switch (state) {
        case SharedConnectionState::Idle:
            return "idle";
        case SharedConnectionState::Connecting:
            return "connecting";
        case SharedConnectionState::Subscribing:
            return "subscribing";
        case SharedConnectionState::Reporting:
            return "reporting";
        case SharedConnectionState::Ready:
            return "ready";
        case SharedConnectionState::Failed:
            return "failed";
        case SharedConnectionState::Closed:
            return "closed";
        }
        return "unknown";
    };
    const auto voice_name = [](VoiceState state) {
        switch (state) {
        case VoiceState::Idle:
            return "idle";
        case VoiceState::Reporting:
            return "reporting";
        case VoiceState::Creating:
            return "creating";
        case VoiceState::Negotiating:
            return "negotiating";
        case VoiceState::Established:
            return "established";
        case VoiceState::Recovering:
            return "recovering";
        case VoiceState::Ending:
            return "ending";
        case VoiceState::Failed:
            return "failed";
        }
        return "unknown";
    };
    const auto boolean = [](bool value) { return value ? "true" : "false"; };
    return std::string("{\"kind\":\"startupDiagnostic\",\"browserReady\":") +
           boolean(impl_->ready) + ",\"initialIdleConfirmed\":" +
           boolean(impl_->driver.initial_idle_observation().has_value()) + ",\"connection\":\"" +
           connection_name(connection_state()) + "\",\"voice\":\"" + voice_name(voice_state()) +
           "\",\"mediaReady\":" + boolean(impl_->media.ready()) +
           ",\"aiOwned\":" + boolean(ai_owned()) + "}";
}
// human_media_failure_diagnostic_line
// 独立于原四字段退出行；只读原本地scope第一条合法旁证，没有旁证仍unknown。
std::string SharedHandoffHost::human_media_failure_diagnostic_line() const {
    if (!impl_->config.context_diagnostic)
        return {};
    return std::string("{\"kind\":\"humanMediaFailureDiagnostic\",\"reason\":\"") +
           std::string(impl_->human_media.failure_reason()) + "\",\"stopCause\":\"" +
           std::string(impl_->human_media.stop_cause()) + "\"}";
}
// ai_runtime_failure_diagnostic_line 只读取原首因；关闭后仍保留事实，不重新调用poll或网络。
std::string SharedHandoffHost::ai_runtime_failure_diagnostic_line() const {
    if (!impl_->config.context_diagnostic)
        return {};
    const auto failure = impl_->ai ? impl_->ai->failure_diagnostic() : std::nullopt;
    const auto reason = failure ? runtime_failure_name(*failure) : std::string_view("unknown");
    return std::string("{\"kind\":\"voiceRuntimeFailureDiagnostic\",\"reason\":\"") +
           std::string(reason) + "\"}";
}
// duplex_driver_failure_diagnostic_line 不补测端口；unknown仅表示未观察到原失败分支。
std::string SharedHandoffHost::duplex_driver_failure_diagnostic_line() const {
    if (!impl_->config.context_diagnostic)
        return {};
    const auto fact = impl_->driver.failure_diagnostic();
    const auto reason =
        fact ? duplex_driver_failure_name(fact->reason) : std::string_view("unknown");
    const auto path = fact && fact->path == 0 ? "0" : fact && fact->path == 1 ? "1" : "unknown";
    return std::string("{\"kind\":\"duplexDriverFailureDiagnostic\",\"reason\":\"") +
           std::string(reason) + "\",\"path\":\"" + path + "\"}";
}
// human_rejection_diagnostic_line
// 只序列化固定枚举和合法状态码，不读取原网络报文。
std::string SharedHandoffHost::human_rejection_diagnostic_line() const {
    if (!impl_->config.context_diagnostic || !impl_->human || !impl_->human->rejection_facts())
        return {};
    const auto& fact = *impl_->human->rejection_facts();
    static constexpr std::string_view states[] = {"idle",
                                                  "connecting",
                                                  "subscribing",
                                                  "reporting",
                                                  "available",
                                                  "inviting",
                                                  "ringing",
                                                  "accepting",
                                                  "negotiating",
                                                  "connectingMedia",
                                                  "waitingEstablished",
                                                  "connected",
                                                  "ending",
                                                  "failed"};
    static_assert(static_cast<unsigned>(HumanCallState::Failed) + 1 ==
                      sizeof(states) / sizeof(states[0]),
                  "HumanCallState首拒帧闭集必须同步枚举");
    const auto index = static_cast<unsigned>(fact.state);
    const auto state = index < sizeof(states) / sizeof(states[0]) ? states[index] : "unknown";
    return std::string("{\"kind\":\"humanSignalRejectionDiagnostic\",\"method\":\"") +
           std::string(human_rejection_method_name(fact.method)) + "\",\"requestMethod\":\"" +
           std::string(human_rejection_method_name(fact.request_method)) +
           "\",\"code\":" + (fact.code ? std::to_string(*fact.code) : "null") + ",\"state\":\"" +
           std::string(state) + "\"}";
}
// human_signal_diagnostic_line 固定三枚举字段；不读取消息、令牌、会话或再次poll。
std::string SharedHandoffHost::human_signal_diagnostic_line() const {
    if (!impl_->config.context_diagnostic)
        return {};
    static constexpr std::string_view states[] = {"idle",
                                                  "connecting",
                                                  "subscribing",
                                                  "reporting",
                                                  "available",
                                                  "inviting",
                                                  "ringing",
                                                  "accepting",
                                                  "negotiating",
                                                  "connectingMedia",
                                                  "waitingEstablished",
                                                  "connected",
                                                  "ending",
                                                  "failed"};
    static_assert(static_cast<unsigned>(HumanCallState::Failed) + 1 ==
                      sizeof(states) / sizeof(states[0]),
                  "HumanCallState诊断闭集必须同步枚举");
    static constexpr std::string_view failures[] = {"none",
                                                    "unknown",
                                                    "clockRollback",
                                                    "networkPoll",
                                                    "trackDeadline",
                                                    "phaseDeadline",
                                                    "invitationDeadline",
                                                    "connectEvent",
                                                    "subscriptionStart",
                                                    "subscriptionEvent",
                                                    "capabilitySubmit",
                                                    "receiveRejected",
                                                    "networkEvent",
                                                    "startConnect",
                                                    "endPublish"};
    static_assert(static_cast<unsigned>(HumanCallFailure::EndPublish) + 1 ==
                      sizeof(failures) / sizeof(failures[0]),
                  "HumanCallFailure诊断闭集必须同步枚举");
    const auto state = impl_->human ? static_cast<unsigned>(impl_->human->state()) : 999U;
    const auto failure =
        impl_->human ? static_cast<unsigned>(impl_->human->failure_facts().failure) : 999U;
    const auto state_name =
        state < sizeof(states) / sizeof(states[0]) ? states[state] : std::string_view("unknown");
    const auto failure_name = failure < sizeof(failures) / sizeof(failures[0])
                                  ? failures[failure]
                                  : std::string_view("unknown");
    return std::string("{\"kind\":\"humanSignalDiagnostic\",\"state\":\"") +
           std::string(state_name) + "\",\"failure\":\"" + std::string(failure_name) + "\"}";
}
// exit_diagnostic_line
// 只序列化闭集literal和原退出布尔；getter不读取源、驱动或消息队列。
std::string SharedHandoffHost::exit_diagnostic_line(SharedHostExitBranch branch,
                                                    bool failed) const {
    if (!impl_->config.context_diagnostic)
        return {};
    const auto branch_name = [](SharedHostExitBranch value) {
        switch (value) {
        case SharedHostExitBranch::BridgeInvalid:
            return "bridgeInvalid";
        case SharedHostExitBranch::BridgeClosed:
            return "bridgeClosed";
        case SharedHostExitBranch::HostPollFalse:
            return "hostPollFalse";
        case SharedHostExitBranch::Unknown:
            return "unknown";
        }
        return "unknown";
    };
    const auto reason_name = [](SharedHostExitReason value) {
        switch (value) {
        case SharedHostExitReason::Unknown:
            return "unknown";
        case SharedHostExitReason::Closed:
            return "closed";
        case SharedHostExitReason::PriorFailure:
            return "priorFailure";
        case SharedHostExitReason::MonotonicDeadline:
            return "monotonicDeadline";
        case SharedHostExitReason::WallDeadline:
            return "wallDeadline";
        case SharedHostExitReason::DisposalStopped:
            return "disposalStopped";
        case SharedHostExitReason::DisposalDeadline:
            return "disposalDeadline";
        case SharedHostExitReason::DisposalSourceUnavailable:
            return "disposalSourceUnavailable";
        case SharedHostExitReason::SourceUnavailable:
            return "sourceUnavailable";
        case SharedHostExitReason::InputGateFailed:
            return "inputGateFailed";
        case SharedHostExitReason::HumanMediaFailed:
            return "humanMediaFailed";
        case SharedHostExitReason::PcmOutputGateFailed:
            return "pcmOutputGateFailed";
        case SharedHostExitReason::DuplexDriverFailed:
            return "duplexDriverFailed";
        case SharedHostExitReason::CapabilityEncodingFailed:
            return "capabilityEncodingFailed";
        case SharedHostExitReason::ConnectionStartFailed:
            return "connectionStartFailed";
        case SharedHostExitReason::ConnectionFailed:
            return "connectionFailed";
        case SharedHostExitReason::AiClientStartFailed:
            return "aiClientStartFailed";
        case SharedHostExitReason::HumanClientStartFailed:
            return "humanClientStartFailed";
        case SharedHostExitReason::DelayedFocusFailed:
            return "delayedFocusFailed";
        case SharedHostExitReason::VoiceRuntimeFailed:
            return "voiceRuntimeFailed";
        case SharedHostExitReason::VoiceSessionFailed:
            return "voiceSessionFailed";
        case SharedHostExitReason::EmitFailed:
            return "emitFailed";
        case SharedHostExitReason::CandidateCloseDriverFailed:
            return "candidateCloseDriverFailed";
        case SharedHostExitReason::CandidateSetupFailed:
            return "candidateSetupFailed";
        case SharedHostExitReason::SharedFocusRelayRejected:
            return "sharedFocusRelayRejected";
        case SharedHostExitReason::HumanFocusReceiptRejected:
            return "humanFocusReceiptRejected";
        }
        return "unknown";
    };
    return std::string("{\"kind\":\"hostExitDiagnostic\",\"branch\":\"") + branch_name(branch) +
           "\",\"reason\":\"" + reason_name(impl_->exit_reason) +
           "\",\"failed\":" + (failed ? "true}" : "false}");
}
} // namespace ur::device
