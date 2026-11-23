// 文件说明：AI候选端Held生命周期与原语音/焦点组合反例；虚拟HAL不替代真实豆包或UDP验收。
#include <gtest/gtest.h>

#include <algorithm>
#include <deque>

#include "ur/device/focus_endpoint.hpp"
#include "ur/device/voice_client.hpp"
#include "ur/device/voice_runtime.hpp"

namespace ur::device {
namespace {
// EndpointNetwork记录实际公共编码，背压不得被当成焦点ACK已交付。
struct EndpointNetwork final : MqttPort {
    bool writable = true;          // 用例控制真实发布接纳，不模拟Broker已收到。
    std::vector<Envelope> sent;    // 只存公开合成夹具，不用实际身份或凭据。
    std::deque<MqttEvent> events;  // 用例按实际MQTT队列交给唯一运行时，不直接调用私有on_event。
    int subscription = 10;         // 原网络MID只关联订阅，不当业务ACK。
    unsigned disconnects = 0;      // 实际关闭调用数。
    bool connect(const MqttConfig&) override { return true; }
    bool poll() override { return true; }
    bool publications_drained() override { return true; }
    int subscribe(std::string_view) override { return subscription++; }
    std::optional<MqttEvent> take_event() override {
        if (events.empty()) return {};
        auto event = std::move(events.front());
        events.pop_front();
        return event;
    }
    void disconnect() override {
        ++disconnects;
        events.clear();
    }
    bool publish(std::string_view topic, std::string_view body) override {
        EXPECT_EQ(topic, "$thing/up/signaling/p/d");
        Envelope envelope;
        EXPECT_EQ(decode_envelope(body, envelope), DecodeResult::Ok);
        if (writable) sent.push_back(std::move(envelope));
        return writable;
    }
};
// EndpointMedia只有显式configure和原audioStart ACK才改变实际调用计数。
struct EndpointMedia final : PumpedVoiceMediaPort {
    bool udp_reply_start_receipt_ready() const override {
        return true;
    }  // 仅新夹具显式选择同步安装。
    unsigned configurations = 0, captures = 0, plays = 0, closes = 0;  // 原HAL副作用计数。
    bool configure_ok = true;      // 失败必须阻止候选就绪及提交。
    bool connected = false;        // 独立原驱动连通事实，不由configure伪造。
    unsigned answers = 0;          // 仅收到原媒体协商应答才计数，不借configure授予连通。
    std::uint32_t sequence = 128;  // UDP原包序号；WebRTC用例必须显式0。
    bool poll(std::uint64_t) override { return true; }
    bool ready() const override { return connected; }
    std::optional<std::uint64_t> drained_epoch() const override { return {}; }
    bool configure(const VoiceMedia&) override {
        ++configurations;
        return configure_ok;
    }
    std::uint32_t next_input_sequence() const override { return sequence; }
    bool apply_answer(std::string_view) override {
        ++answers;
        return true;
    }
    bool prepare_reply(const std::array<std::uint8_t, 8>&, std::uint32_t) override { return true; }
    bool finish_reply(std::uint32_t) override { return true; }
    bool start_capture(std::string_view) override {
        ++captures;
        return true;
    }
    void stop_capture() override {}
    bool start_playback(std::string_view, std::uint64_t) override {
        ++plays;
        return true;
    }
    void stop_playback(std::uint64_t) override {}
    void close() override {
        ++closes;
        connected = false;
    }
};
// EndpointAuthority只授权本候选完整双向组；许可变化是独立事实，不借原接续事件默认true。
struct EndpointAuthority final : FocusAuthority {
    bool allowed = true;  // 显式测试许可，生产必须由完整双向宿主证明。
    std::optional<FocusPermit> authorize(const FocusCommand& command, std::uint64_t,
                                         std::uint64_t) override {
        if (!allowed || command.envelope.session != "candidate" || command.resources.size() != 2)
            return {};
        return FocusPermit{command, 10000, 1};
    }
    bool valid(const FocusPermit&, std::uint64_t, std::uint64_t) const override { return allowed; }
};
// EndpointDriver实际两资源完成由用例注入，不把begin受理当取得。
struct EndpointDriver final : FocusDriver {
    bool held = false;                          // 原完整组实际持有事实。
    std::uint64_t generation = 0, current = 0;  // 原实际修订及受理代次。
    FocusCommand last;                          // 原完整固定命令，不由候选端补造focusId。
    std::deque<FocusFact> facts;                // 原异步事实交事件线程。
    std::uint64_t revision() const override { return current; }
    bool begin(const FocusCommand& command, std::uint64_t value) override {
        last = command;
        generation = value;
        return true;
    }
    std::optional<FocusFact> take_fact(std::string_view session, std::string_view focus) override {
        if (facts.empty() || session != last.envelope.session || focus != last.focus_id) return {};
        auto fact = std::move(facts.front());
        facts.pop_front();
        return fact;
    }
    bool owns(const FocusCommand& command) const override {
        return held && command.envelope.session == "candidate" && command.resources.size() == 2;
    }
    bool stop_scope(std::string_view, std::string_view, const std::vector<std::string>&) override {
        held = false;
        return true;
    }
    // complete只确认原驱动整组，publish可能仍背压，不能据此开候选麦克风。
    void complete() {
        held = true;
        ++current;
        facts.push_back({generation, true, "acquired", last.resources, current});
    }
};
// endpoint_capability是明确本地接听/音频能力，不启用sessionSync或借用视觉配置。
VoiceCapability endpoint_capability(bool receipt = false) {
    VoiceCapability cap;
    cap.audio = {16000, 1, 20};
    cap.modes = {true, true, true};
    cap.ai_endpoint_handoff = true;
    cap.udp_reply_start_receipt = receipt;
    return cap;
}
// endpoint_encode生成原公共信封，所有标识/UDP参数均是不可连接的公开测试占位。
std::string endpoint_encode(std::string method, std::string session, std::string payload,
                            std::uint64_t sequence = 0, std::optional<int> code = {},
                            std::string token = "event") {
    Envelope envelope;
    envelope.method = std::move(method);
    envelope.session = std::move(session);
    envelope.payload = std::move(payload);
    envelope.token = std::move(token);
    // 平台邀请沿原契约携params；应答与状态事件才使用data。
    envelope.request = envelope.method == "sessionInvite";
    envelope.code = code;
    if (sequence) envelope.event_sequence = sequence;
    std::string body;
    EXPECT_EQ(encode_envelope(envelope, body), DecodeResult::Ok);
    return body;
}
// endpoint_invite绑定两条完整原轨，不含新模型创建请求或供应商anchor。
std::string endpoint_invite(bool receipt = false) {
    auto out = endpoint_encode(
        "sessionInvite", "candidate",
        R"({"purpose":"aiConversation","admission":"accept","expiresAt":31000,"handoff":{"sourceSessionId":"source-leg","handoffId":"move"},"selected":{"transport":"udp","interactionMode":"fullDuplex","audioParams":{"format":"opus","sampleRate":16000,"channels":1,"frameDuration":20}},"tracks":[{"trackId":"mic-1","kind":"audio","sourceId":"mic","direction":"sendonly","codec":"opus","state":"requested"},{"trackId":"speaker-1","kind":"audio","sourceId":"speaker","direction":"recvonly","codec":"opus","state":"requested"}],"media":{"udp":{"server":"127.0.0.1","port":12345,"key":"000102030405060708090a0b0c0d0e0f","nonce":"01000000112233445566778800000000"}}})",
        1, {}, "invite");
    if (receipt)
        out.insert(out.find("\"interactionMode\""), "\"udpReplyStartReceipt\":true,");
    return out;
}
// endpoint_ack必须关联实际请求token，错误ACK不能抢占后继正确ACK。
std::string endpoint_ack(const Envelope& request, std::string result, int code = 200) {
    return endpoint_encode(
        "requestAck", request.session,
        "{\"requestMethod\":\"" + request.method + "\"," +
            (code == 200 ? "\"result\":" + result : "\"reason\":\"requestDenied\"") + "}",
        0, code, request.token);
}
// endpoint_end_ack沿原sessionEnd的正式sessionEnded应答，不把接听requestAck结构用于挂断。
std::string endpoint_end_ack(const Envelope& request) {
    return endpoint_encode("sessionEnded", request.session,
                           R"({"state":"ended","reason":"userStopped"})", 0, 200, request.token);
}
// VoiceEndpointTest复用真实FocusEngine，媒体尚未建立时任何播放/采集计数都必须保持零。
class VoiceEndpointTest : public testing::Test {
protected:
    EndpointNetwork network;
    EndpointMedia media;
    EndpointDriver driver;
    EndpointAuthority authority;
    FocusEndpoint focus{network,
                        driver,
                        authority,
                        {"microphone", "speaker"},
                        "$thing/up/signaling/p/d",
                        "$thing/down/signaling/p/d",
                        20000};
    const bool receipt;  // 新回归才声明并选择安装确认，旧夹具仍缺省false。
    VoiceClient client;
    explicit VoiceEndpointTest(bool enabled = false)
        : receipt(enabled),
          client{network,
                 media,
                 "$thing/up/signaling/p/d",
                 "$thing/down/signaling/p/d",
                 endpoint_capability(enabled),
                 "endpoint",
                 &focus} {}
    // receive双时钟固定墙钟基线，monotonic推进不把候选绝对期限续长。
    Result receive(std::string body, std::uint64_t now = 10, std::uint64_t wall = 1010) {
        return client.receive({"$thing/down/signaling/p/d", body}, now, wall);
    }
    void listening() {
        ASSERT_EQ(client.connect_listening(1), Result::Applied);
        const auto report = network.sent.back();
        ASSERT_EQ(report.method, "capabilityReport");
        ASSERT_EQ(receive(endpoint_ack(report, R"({"acceptedRevision":1})"), 2, 1002),
                  Result::Applied);
        ASSERT_EQ(client.endpoint_phase(), AIEndpointPhase::Listening);
    }
    void invited() {
        listening();
        ASSERT_EQ(receive(endpoint_invite(receipt), 3, 1003), Result::Applied);
        ASSERT_EQ(client.endpoint_phase(), AIEndpointPhase::Invited);
    }
    void accepting() {
        invited();
        ASSERT_EQ(client.accept_endpoint_invite(4, 1004), Result::Applied);
        const auto request = network.sent.back();
        ASSERT_EQ(request.method, "sessionAccept");
        ASSERT_EQ(request.payload, R"({"acceptedTracks":["mic-1","speaker-1"]})");
        ASSERT_EQ(receive(endpoint_ack(request, R"({"state":"negotiating"})"), 5, 1005),
                  Result::Applied);
    }
    void media_ready() {
        accepting();
        ASSERT_EQ(client.media_ready(6), Result::Applied);
        const auto request = network.sent.back();
        ASSERT_EQ(request.method, "mediaState");
        ASSERT_EQ(
            receive(endpoint_ack(request, R"({"state":"connected","mediaRevision":1})"), 7, 1007),
            Result::Applied);
    }
    void commit(std::uint64_t now = 8) {
        ASSERT_EQ(
            receive(
                endpoint_encode(
                    "endpointState", "source-leg",
                    R"({"state":"committed","revision":2,"handoffId":"move","candidateSessionId":"candidate"})",
                    42, {}, "commit"),
                now, 1000 + now),
            Result::Applied);
    }
    void established() {
        ASSERT_EQ(receive(endpoint_encode("sessionEstablished", "candidate",
                                          R"({"state":"established","mediaRevision":1})", 2, {},
                                          "established"),
                          9, 1009),
                  Result::Applied);
    }
    // acquire明确走真实原FocusEndpoint受理、驱动事实和回执交付，不直接改VoiceClient状态。
    void acquire(bool backpressure = false) {
        const auto body =
            R"({"msgToken":"focus","method":"focusControl","sessionId":"candidate","eventSeq":3,"params":{"operation":"acquire","expectedRevision":0,"focusId":"focus-candidate","resources":["microphone","speaker"],"priority":"conversation","resumePolicy":"explicit","expiresAt":20000}})";
        ASSERT_EQ(focus.receive("$thing/down/signaling/p/d", body, 10, 1010),
                  FocusEndpointResult::Handled);
        focus.poll(11, 1011);
        driver.complete();
        network.writable = !backpressure;
        focus.poll(12, 1012);
        client.tick(13, 1013);
    }
};
TEST_F(VoiceEndpointTest, ListeningAndInviteNeverCreateModelOrTouchMedia) {
    invited();
    EXPECT_EQ(network.sent.size(), 1U);
    EXPECT_EQ(media.configurations, 0U);
    EXPECT_EQ(media.captures, 0U);
    EXPECT_EQ(media.plays, 0U);
    EXPECT_FALSE(client.candidate_focus_scope());
    EXPECT_EQ(client.start_input(4), Result::Stale);
    EXPECT_EQ(receive(endpoint_invite(), 4, 1004), Result::Duplicate);
    EXPECT_EQ(network.sent.size(), 1U);
}
TEST_F(VoiceEndpointTest, AcceptACKThenActualMediaThenTwoFactsThenCompleteFocus) {
    media_ready();
    commit();
    EXPECT_FALSE(client.candidate_focus_scope());
    established();
    ASSERT_TRUE(client.candidate_focus_scope());
    EXPECT_EQ(client.endpoint_phase(), AIEndpointPhase::AwaitingFocus);
    EXPECT_NE(client.state(), VoiceState::Established);
    EXPECT_EQ(media.captures, 0U);
    acquire(true);
    EXPECT_NE(client.state(), VoiceState::Established);
    EXPECT_EQ(media.captures, 0U);
    network.writable = true;
    focus.poll(14, 1014);
    client.tick(15, 1015);
    ASSERT_EQ(client.state(), VoiceState::Established);
    EXPECT_EQ(client.endpoint_phase(), AIEndpointPhase::Active);
    ASSERT_TRUE(client.candidate_focus_scope());
    EXPECT_EQ(media.captures, 0U);
    ASSERT_EQ(client.start_input(16), Result::Applied);
    const auto request = network.sent.back();
    EXPECT_EQ(request.method, "audioStart");
    EXPECT_EQ(media.captures, 0U);
    ASSERT_EQ(receive(endpoint_ack(request, request.payload), 17, 1017), Result::Applied);
    EXPECT_EQ(media.captures, 1U);
    EXPECT_FALSE(client.control_sync_enabled());
}
TEST_F(VoiceEndpointTest, CandidateEstablishedMayPrecedeSourceCommitted) {
    media_ready();
    established();
    EXPECT_FALSE(client.candidate_focus_scope());
    commit(10);
    ASSERT_TRUE(client.candidate_focus_scope());
    acquire();
    EXPECT_EQ(client.state(), VoiceState::Established);
}
TEST_F(VoiceEndpointTest, WrongScopeAndWrongCandidateCannotCommit) {
    media_ready();
    established();
    EXPECT_EQ(
        receive(
            endpoint_encode(
                "endpointState", "other-leg",
                R"({"state":"committed","revision":2,"handoffId":"move","candidateSessionId":"candidate"})",
                42),
            10),
        Result::Stale);
    EXPECT_EQ(
        receive(
            endpoint_encode(
                "endpointState", "source-leg",
                R"({"state":"committed","revision":2,"handoffId":"other","candidateSessionId":"candidate"})",
                42),
            10),
        Result::Stale);
    EXPECT_FALSE(client.candidate_focus_scope());
    EXPECT_EQ(media.captures, 0U);
}
TEST_F(VoiceEndpointTest, ExpiredInviteAndExactDeadlineDoNotConfigureOrResume) {
    listening();
    EXPECT_EQ(receive(endpoint_invite(), 3, 31000), Result::Stale);
    EXPECT_EQ(media.configurations, 0U);
}
TEST_F(VoiceEndpointTest, MediaFailureAndRejectedAcceptRemainSilent) {
    invited();
    ASSERT_EQ(client.accept_endpoint_invite(4, 1004), Result::Applied);
    const auto request = network.sent.back();
    EXPECT_EQ(receive(endpoint_ack(request, "{}", 403), 5, 1005), Result::Unsupported);
    EXPECT_EQ(media.configurations, 0U);
    EXPECT_EQ(media.captures, 0U);
    EXPECT_EQ(media.plays, 0U);
}
// 接听ACK不豁免真实媒体配置失败；失败必须关闭候选，不得上报连通或开启输入。
TEST_F(VoiceEndpointTest, ActualMediaConfigureFailureNeverReportsReady) {
    invited();
    ASSERT_EQ(client.accept_endpoint_invite(4, 1004), Result::Applied);
    const auto request = network.sent.back();
    const auto count = network.sent.size();
    media.configure_ok = false;
    EXPECT_EQ(receive(endpoint_ack(request, R"({"state":"negotiating"})"), 5, 1005),
              Result::DriverFailed);
    EXPECT_EQ(client.state(), VoiceState::Failed);
    EXPECT_EQ(media.configurations, 1U);
    EXPECT_EQ(media.captures, 0U);
    EXPECT_EQ(media.plays, 0U);
    EXPECT_EQ(network.sent.size(), count);
    EXPECT_FALSE(client.candidate_focus_scope());
}
// AI候选拒接使用Go Owner既有枚举，不复用人际通话的declined。
TEST_F(VoiceEndpointTest, LocalRejectUsesCanonicalReasonWithoutSourceEnd) {
    invited();
    ASSERT_EQ(client.reject_endpoint_invite(4, 1004), Result::Applied);
    ASSERT_FALSE(network.sent.empty());
    EXPECT_EQ(network.sent.back().method, "sessionReject");
    EXPECT_EQ(network.sent.back().session, "candidate");
    EXPECT_EQ(network.sent.back().payload, R"({"reason":"userRejected"})");
    EXPECT_EQ(media.configurations, 0U);
    EXPECT_EQ(media.captures, 0U);
}
// Go endpointNoticeEnvelope实际生成的state+reason结构必须结束原候选，而不是被误判非法ACK。
TEST_F(VoiceEndpointTest, CanonicalGoRejectAcknowledgementEndsCandidateAndLateAcceptCannotRevive) {
    invited();
    ASSERT_EQ(client.reject_endpoint_invite(4, 1004), Result::Applied);
    const auto reject = network.sent.back();
    const std::string go_ack = R"({"msgToken":"invite","method":"requestAck","sessionId":"candidate","data":{"requestMethod":"sessionReject","result":{"reason":"userRejected","state":"ended"}},"code":200})";
    ASSERT_EQ(receive(go_ack, 5, 1005), Result::Applied);
    EXPECT_EQ(client.state(), VoiceState::Idle);
    EXPECT_FALSE(client.candidate_focus_scope());
    EXPECT_NE(client.accept_endpoint_invite(6, 1006), Result::Applied);
    auto accept = reject; // 原token/腿不能被迟到接听应答借来反向建立媒体。
    accept.method = "sessionAccept";
    EXPECT_NE(receive(endpoint_ack(accept, R"({"state":"negotiating"})"), 7, 1007), Result::Applied);
    EXPECT_EQ(client.state(), VoiceState::Idle);
    EXPECT_EQ(media.configurations, 0U);
    EXPECT_EQ(media.captures, 0U);
}
// 既有仅state成功应答保留；兼容ACK不代表仍能重新接听旧候选。
TEST_F(VoiceEndpointTest, LegacyStateOnlyRejectAcknowledgementRemainsAccepted) {
    invited();
    ASSERT_EQ(client.reject_endpoint_invite(4, 1004), Result::Applied);
    const auto original = network.sent.back();
    ASSERT_EQ(receive(endpoint_ack(original, R"({"state":"ended"})"), 5, 1005), Result::Applied);
    EXPECT_EQ(client.state(), VoiceState::Idle);
    EXPECT_EQ(media.configurations, 0U);
    EXPECT_EQ(media.captures, 0U);
}
// 未知字段、非字符串与不匹配原用户拒接原因均不得消费pending，随后原正确ACK仍可完成。
TEST_F(VoiceEndpointTest, RejectAckUnknownOrMismatchedFieldsDoNotConsumeOriginalRequest) {
    invited();
    ASSERT_EQ(client.reject_endpoint_invite(4, 1004), Result::Applied);
    const auto original = network.sent.back();
    const std::vector<std::string> invalid{
        R"({"state":"ended","extra":true})",
        R"({"state":"ended","reason":"declined"})",
        R"({"state":"ended","reason":"resourceBusy"})",
        R"({"state":"ended","reason":"privacyEnabled"})",
        R"({"state":"ended","reason":null})",
        R"({"state":"ended","reason":true})",
        R"({"state":"negotiating","reason":"userRejected"})",
        R"({"state":"ended","reason":"userRejected","extra":true})"};
    std::uint64_t now = 5;
    for (const auto& fields : invalid) {
        EXPECT_EQ(receive(endpoint_ack(original, fields), now, 1000 + now), Result::Invalid);
        EXPECT_EQ(client.state(), VoiceState::Ending);
        EXPECT_EQ(media.configurations, 0U);
        EXPECT_EQ(media.captures, 0U);
        ++now;
    }
    EXPECT_EQ(receive(endpoint_ack(original, R"({"state":"ended","reason":"userRejected"})"),
                      now, 1000 + now), Result::Applied);
    EXPECT_EQ(client.state(), VoiceState::Idle);
}
// 拒接ACK的可选reason不适用于接听ACK，避免把专分支修复扩大成宽松bootstrap。
TEST_F(VoiceEndpointTest, AcceptAcknowledgementStillRejectsReasonAndUnknownResultFields) {
    invited();
    ASSERT_EQ(client.accept_endpoint_invite(4, 1004), Result::Applied);
    const auto original = network.sent.back();
    EXPECT_EQ(receive(endpoint_ack(original, R"({"state":"negotiating","reason":"userRejected"})"),
                      5, 1005), Result::Invalid);
    EXPECT_EQ(media.configurations, 0U);
    EXPECT_EQ(receive(endpoint_ack(original, R"({"state":"negotiating","extra":true})"), 6, 1006),
              Result::Invalid);
    EXPECT_EQ(media.configurations, 0U);
    EXPECT_EQ(receive(endpoint_ack(original, R"({"state":"negotiating"})"), 7, 1007), Result::Applied);
    EXPECT_EQ(media.configurations, 1U);
    EXPECT_EQ(media.captures, 0U);
}

// 提交事务的终态不能被随后高事件序号的旧abort倒退；应由本候选sessionEnded明确关闭。
TEST_F(VoiceEndpointTest, CommittedHandoffCannotRegressToAborted) {
    media_ready();
    commit();
    established();
    acquire();
    ASSERT_EQ(client.endpoint_phase(), AIEndpointPhase::Active);
    EXPECT_EQ(
        receive(
            endpoint_encode(
                "endpointState", "source-leg",
                R"({"state":"aborted","revision":3,"handoffId":"move","reason":"userCancelled"})",
                43, {}, "late-abort"),
            14, 1014),
        Result::Stale);
    EXPECT_EQ(client.endpoint_phase(), AIEndpointPhase::Active);
    EXPECT_EQ(client.state(), VoiceState::Established);
}

// 提交早于候选建立时也必须锁住事务终态，不能借尚未Active接受反向abort。
TEST_F(VoiceEndpointTest, CommitBeforeEstablishedAlsoRejectsLateAbort) {
    media_ready();
    commit();
    ASSERT_EQ(client.state(), VoiceState::Negotiating);
    EXPECT_EQ(
        receive(
            endpoint_encode(
                "endpointState", "source-leg",
                R"({"state":"aborted","revision":3,"handoffId":"move","reason":"handoffTimeout"})",
                43, {}, "late-abort"),
            8, 1008),
        Result::Stale);
    EXPECT_EQ(client.state(), VoiceState::Negotiating);
    established();
    acquire();
    EXPECT_EQ(client.endpoint_phase(), AIEndpointPhase::Active);
}

// 非终态ready以及失败也不能覆盖已提交；拒绝消息不应抬高源腿水位或停止原媒体。
TEST_F(VoiceEndpointTest, CommittedHandoffRejectsOtherStatesWithoutConsumingWatermark) {
    media_ready();
    commit();
    established();
    acquire();
    const auto closed = media.closes;
    for (const auto& payload :
         {R"({"state":"ready","revision":3,"handoffId":"move","candidateSessionId":"candidate"})",
          R"({"state":"failed","revision":3,"handoffId":"move","reason":"mediaFailed"})",
          R"({"state":"preparing","revision":3,"handoffId":"move"})"})
        EXPECT_EQ(
            receive(endpoint_encode("endpointState", "source-leg", payload, 43, {}, "late-state"),
                    14, 1014),
            Result::Stale);
    EXPECT_EQ(media.closes, closed);
    EXPECT_EQ(client.endpoint_phase(), AIEndpointPhase::Active);
    EXPECT_EQ(
        receive(
            endpoint_encode(
                "endpointState", "source-leg",
                R"({"state":"committed","revision":2,"handoffId":"move","candidateSessionId":"candidate"})",
                43, {}, "commit-redelivery"),
            15, 1015),
        Result::Applied);
}

// 原源腿终止不能借较高水位关闭候选；候选正式终止仍须实际关媒体，不能被提交围栏屏蔽。
TEST_F(VoiceEndpointTest, CandidateEndedRemainsAuthoritativeAfterCommit) {
    media_ready();
    commit();
    established();
    acquire();
    const auto closed = media.closes;
    EXPECT_EQ(receive(endpoint_encode("sessionEnded", "source-leg",
                                      R"({"state":"ended","reason":"userHangup"})", 999),
                      14, 1014),
              Result::Stale);
    EXPECT_EQ(client.endpoint_phase(), AIEndpointPhase::Active);
    EXPECT_EQ(media.closes, closed);
    EXPECT_EQ(receive(endpoint_encode("sessionEnded", "candidate",
                                      R"({"state":"ended","reason":"userHangup"})", 3),
                      15, 1015),
              Result::Applied);
    EXPECT_EQ(client.state(), VoiceState::Idle);
    EXPECT_EQ(media.closes, closed + 1);
    EXPECT_FALSE(client.candidate_focus_scope());
}

// 已提交不等于无限资源许可；实际焦点撤权仍立即关掉原双向媒体，不复用历史acquire ACK。
TEST_F(VoiceEndpointTest, ActiveLosesActualFocusAndStopsMedia) {
    client.enable_failure_diagnostics();
    media_ready();
    commit();
    established();
    acquire();
    const auto closed = media.closes;
    authority.allowed = false;
    client.tick(14, 1014);
    EXPECT_EQ(client.state(), VoiceState::Failed);
    EXPECT_EQ(media.closes, closed + 1);
    EXPECT_FALSE(client.candidate_focus_scope());
    EXPECT_EQ(client.start_input(15), Result::Stale);
    ASSERT_TRUE(client.failure_diagnostic());
    EXPECT_EQ(client.failure_diagnostic()->cause, VoiceFailureCause::ActiveScopeLost);
    EXPECT_EQ(client.failure_diagnostic()->method, VoiceFailureMethod::None);
    EXPECT_EQ(client.scope_failure(), ScopeFailureCode::AuthorityDenied);
}
// 原Focus.poll先关闭不能令候选gate只剩engineClosed；下一tick仍读取原现场撤权首因。
TEST_F(VoiceEndpointTest, ScopeDiagnosticPollBeforeCandidateTickRetainsAuthorityCause) {
    client.enable_failure_diagnostics();
    media_ready();
    commit();
    established();
    acquire();
    authority.allowed = false;
    focus.poll(14, 1014);
    client.tick(14, 1014);
    EXPECT_EQ(client.state(), VoiceState::Failed);
    EXPECT_EQ(client.scope_failure(), ScopeFailureCode::AuthorityDenied);
    client.disconnected(15);
    EXPECT_EQ(client.scope_failure(), ScopeFailureCode::AuthorityDenied);
}
// 正常等待焦点不观察失败；默认关闭及失败后开启不会推导历史首因。
TEST_F(VoiceEndpointTest, ScopeDiagnosticDefaultOffAndLateEnableRemainEmpty) {
    media_ready();
    commit();
    established();
    acquire();
    authority.allowed = false;
    client.tick(14, 1014);
    EXPECT_EQ(client.state(), VoiceState::Failed);
    EXPECT_FALSE(client.scope_failure());
    client.enable_failure_diagnostics();
    client.disconnected(15);
    EXPECT_FALSE(client.scope_failure());
}
TEST_F(VoiceEndpointTest, ScopeDiagnosticNormalAwaitingFocusDoesNotLatchFailure) {
    client.enable_failure_diagnostics();
    media_ready();
    commit();
    established();
    client.tick(10, 1010);
    EXPECT_EQ(client.endpoint_phase(), AIEndpointPhase::AwaitingFocus);
    EXPECT_FALSE(client.scope_failure());
    acquire();
    EXPECT_EQ(client.endpoint_phase(), AIEndpointPhase::Active);
    EXPECT_FALSE(client.scope_failure());
}
// 普通用户停止采集不撤原完整资源租约，也不制造ActiveScopeLost观察。
TEST_F(VoiceEndpointTest, FailureDiagnosticNormalInputStopKeepsOriginalActiveScope) {
    client.enable_failure_diagnostics();
    media_ready();
    commit();
    established();
    acquire();
    ASSERT_EQ(client.start_input(14), Result::Applied);
    auto request = network.sent.back();
    ASSERT_EQ(receive(endpoint_ack(request, request.payload), 15, 1015), Result::Applied);
    ASSERT_EQ(client.stop_input(16), Result::Applied);
    request = network.sent.back();
    ASSERT_EQ(receive(endpoint_ack(request, request.payload), 17, 1017), Result::Applied);
    client.tick(18, 1018);
    EXPECT_EQ(client.state(), VoiceState::Established);
    EXPECT_EQ(client.endpoint_phase(), AIEndpointPhase::Active);
    EXPECT_TRUE(driver.held);
    EXPECT_FALSE(client.failure_diagnostic());
}
// 本人挂断已停止媒体；等待原结束ACK时不能把Ending误当新端许可丢失。
TEST_F(VoiceEndpointTest, ActiveCandidateUserEndWaitsForOriginalACKWithoutScopeFailure) {
    client.enable_failure_diagnostics();
    media_ready();
    commit();
    established();
    acquire();
    ASSERT_EQ(client.state(), VoiceState::Established);
    ASSERT_EQ(client.end(14), Result::Applied);
    const auto request = network.sent.back();
    ASSERT_EQ(request.method, "sessionEnd");
    ASSERT_EQ(client.state(), VoiceState::Ending);
    client.tick(15, 1015);
    EXPECT_EQ(client.state(), VoiceState::Ending);
    EXPECT_FALSE(client.failure_diagnostic());
    EXPECT_FALSE(client.scope_failure());
    EXPECT_EQ(
        receive(
            endpoint_encode(
                "endpointState", "source-leg",
                R"({"state":"committed","revision":2,"handoffId":"move","candidateSessionId":"candidate"})",
                43, {}, "late-commit"),
            15, 1015),
        Result::Applied);
    EXPECT_EQ(receive(endpoint_encode("endpointState", "source-leg",
                                      R"({"state":"preparing","revision":0,"handoffId":"move"})",
                                      44, {}, "late-prepare"),
                      15, 1015),
              Result::Stale);
    EXPECT_FALSE(client.candidate_focus_scope());
    EXPECT_EQ(client.start_input(15), Result::Stale);
    EXPECT_EQ(media.captures, 0U);
    auto wrong = request;
    wrong.token += "-other";
    EXPECT_EQ(receive(endpoint_end_ack(wrong), 16, 1016), Result::Stale);
    EXPECT_EQ(client.state(), VoiceState::Ending);
    wrong = request;
    wrong.session = "other-candidate";
    EXPECT_EQ(receive(endpoint_end_ack(wrong), 16, 1016), Result::Stale);
    EXPECT_EQ(client.state(), VoiceState::Ending);
    EXPECT_EQ(receive(endpoint_end_ack(request), 16, 1016), Result::Applied);
    EXPECT_EQ(client.state(), VoiceState::Idle);
    EXPECT_EQ(receive(endpoint_end_ack(request), 17, 1017), Result::Stale);
    EXPECT_EQ(client.start_input(18), Result::Stale);
}

// 原30秒结束请求期限不借候选邀请续期；失败后迟到ACK也不能改写终态。
TEST_F(VoiceEndpointTest, ActiveCandidateEndMissingACKUsesOriginalRequestDeadline) {
    client.enable_failure_diagnostics();
    media_ready();
    commit();
    established();
    acquire();
    ASSERT_EQ(client.end(14), Result::Applied);
    const auto request = network.sent.back();
    client.tick(30013, 31013);
    ASSERT_EQ(client.state(), VoiceState::Ending);
    EXPECT_FALSE(client.candidate_focus_scope());
    client.tick(30014, 31014);
    EXPECT_EQ(client.state(), VoiceState::Failed);
    ASSERT_TRUE(client.failure_diagnostic());
    EXPECT_EQ(client.failure_diagnostic()->cause, VoiceFailureCause::RequestsTimeout);
    EXPECT_EQ(client.failure_diagnostic()->method, VoiceFailureMethod::SessionEnd);
    EXPECT_FALSE(client.scope_failure());
    EXPECT_EQ(receive(endpoint_end_ack(request), 30015, 31015), Result::Stale);
    EXPECT_EQ(client.state(), VoiceState::Failed);
}

// 尚未取得焦点的原Negotiating挂断可跨邀请建立期限接收原ACK，但不能取得原双向许可。
TEST_F(VoiceEndpointTest, NegotiatingEndRetiresInvitationDeadlineWithoutAuthorizingMedia) {
    client.enable_failure_diagnostics();
    media_ready();
    ASSERT_EQ(client.end(29999), Result::Applied);
    const auto request = network.sent.back();
    client.tick(30004, 31004);
    ASSERT_EQ(client.state(), VoiceState::Ending);
    EXPECT_FALSE(client.candidate_focus_scope());
    EXPECT_EQ(media.captures, 0U);
    EXPECT_EQ(media.plays, 0U);
    EXPECT_EQ(receive(endpoint_end_ack(request), 30005, 31005), Result::Applied);
    EXPECT_EQ(client.state(), VoiceState::Idle);
    EXPECT_FALSE(client.failure_diagnostic());
}

// 原错误结束ACK仍是请求拒绝，不被Ending早退吞掉或误记Active权限失败。
TEST_F(VoiceEndpointTest, ActiveCandidateEndRejectedACKKeepsOriginalFailure) {
    client.enable_failure_diagnostics();
    media_ready();
    commit();
    established();
    acquire();
    ASSERT_EQ(client.end(14), Result::Applied);
    const auto request = network.sent.back();
    client.tick(15, 1015);
    EXPECT_EQ(receive(endpoint_ack(request, "{}", 403), 16, 1016), Result::Unsupported);
    ASSERT_TRUE(client.failure_diagnostic());
    EXPECT_EQ(client.failure_diagnostic()->cause, VoiceFailureCause::RequestRejected);
    EXPECT_EQ(client.failure_diagnostic()->method, VoiceFailureMethod::SessionEnd);
    EXPECT_FALSE(client.scope_failure());
    EXPECT_EQ(client.state(), VoiceState::Failed);
}

// 已受理接听但尚未建立时可结束，新输入和重复结束都不能穿过本人关闭状态。
TEST_F(VoiceEndpointTest, NegotiatingEndCannotRestartInputOrRepeatEnd) {
    accepting();
    ASSERT_EQ(client.end(6), Result::Applied);
    EXPECT_EQ(client.start_input(7), Result::Stale);
    EXPECT_EQ(client.end(8), Result::Stale);
    EXPECT_EQ(client.state(), VoiceState::Ending);
}

// 尚未本人接听的候选不能用sessionEnd代替sessionReject，也不改变原准备阶段。
TEST_F(VoiceEndpointTest, EndBeforeAcceptedDoesNotChangeOriginalInvitation) {
    invited();
    const auto count = network.sent.size();
    EXPECT_EQ(client.end(4), Result::Stale);
    EXPECT_EQ(client.endpoint_phase(), AIEndpointPhase::Invited);
    EXPECT_EQ(network.sent.size(), count);
    EXPECT_EQ(media.configurations, 0U);
}

// 新监听的独立公开腿不能被旧结束终态关闭；只测原关联与阶段，不冒称第二次HAL已获权。
TEST_F(VoiceEndpointTest, NewInvitationRejectsPreviousEndedAcknowledgement) {
    media_ready();
    commit();
    established();
    acquire();
    ASSERT_EQ(client.end(14), Result::Applied);
    const auto ended = endpoint_end_ack(network.sent.back());
    ASSERT_EQ(receive(ended, 15, 1015), Result::Applied);
    ASSERT_EQ(client.connect_listening(16), Result::Applied);
    ASSERT_EQ(receive(endpoint_ack(network.sent.back(), R"({"acceptedRevision":1})"), 17, 1017),
              Result::Applied);
    Envelope invite;
    ASSERT_EQ(decode_envelope(endpoint_invite(), invite), DecodeResult::Ok);
    invite.session = "candidate-new";
    invite.token = "invite-new";
    std::string body;
    ASSERT_EQ(encode_envelope(invite, body), DecodeResult::Ok);
    ASSERT_EQ(receive(body, 18, 1018), Result::Applied);
    EXPECT_EQ(receive(ended, 19, 1019), Result::Stale);
    EXPECT_EQ(client.endpoint_phase(), AIEndpointPhase::Invited);
    EXPECT_EQ(client.session_id(), "candidate-new");
    EXPECT_FALSE(client.capturing());
}

// 挂断等待不暂停原焦点维护；实际撤权仍停原资源，已关闭的会话绝不重新采集。
TEST_F(VoiceEndpointTest, EndingDoesNotPreventOriginalFocusRevocation) {
    media_ready();
    commit();
    established();
    acquire();
    ASSERT_TRUE(driver.held);
    ASSERT_EQ(client.end(14), Result::Applied);
    authority.allowed = false;
    focus.poll(15, 1015);
    client.tick(15, 1015);
    EXPECT_FALSE(driver.held);
    EXPECT_EQ(client.state(), VoiceState::Ending);
    EXPECT_FALSE(client.candidate_focus_scope());
    EXPECT_FALSE(client.capturing());
    EXPECT_FALSE(client.playing());
    EXPECT_EQ(client.start_input(16), Result::Stale);
}

// 挂断没有放宽原墙钟约束；回拨仍失败关闭，不能靠Ending跳过可信时间校验。
TEST_F(VoiceEndpointTest, EndingStillRejectsOriginalCandidateClockRollback) {
    client.enable_failure_diagnostics();
    media_ready();
    commit();
    established();
    acquire();
    ASSERT_EQ(client.end(14), Result::Applied);
    client.tick(15, 1012);
    EXPECT_EQ(client.state(), VoiceState::Failed);
    ASSERT_TRUE(client.failure_diagnostic());
    EXPECT_EQ(client.failure_diagnostic()->cause, VoiceFailureCause::CandidateClock);
    EXPECT_FALSE(client.scope_failure());
    EXPECT_FALSE(client.capturing());
}

// 原候选时钟和建立截止各有固定观察，不改变邀请原绝对期限。
TEST_F(VoiceEndpointTest, FailureDiagnosticCandidateClockIsNotEstablishTimeout) {
    client.enable_failure_diagnostics();
    media_ready();
    client.tick(8, 1006);
    ASSERT_EQ(client.state(), VoiceState::Failed);
    ASSERT_TRUE(client.failure_diagnostic());
    EXPECT_EQ(client.failure_diagnostic()->cause, VoiceFailureCause::CandidateClock);
}
TEST_F(VoiceEndpointTest, FailureDiagnosticCandidateEstablishKeepsOriginalDeadline) {
    client.enable_failure_diagnostics();
    invited();
    client.tick(30000, 31000);
    ASSERT_EQ(client.state(), VoiceState::Failed);
    ASSERT_TRUE(client.failure_diagnostic());
    EXPECT_EQ(client.failure_diagnostic()->cause, VoiceFailureCause::CandidateEstablishDeadline);
}

// 有效源事实不能被淘汰后重执行；容量满安全关闭，而不是恢复一个失去去重保护的候选。
TEST_F(VoiceEndpointTest, SourceFactCapacityFailsClosedWithoutEvictingDeduplication) {
    media_ready();
    const auto fact = [](std::uint64_t sequence) {
        return endpoint_encode("endpointState", "source-leg",
                               R"({"state":"preparing","revision":0,"handoffId":"move"})", sequence,
                               {}, "fact-" + std::to_string(sequence));
    };
    for (std::uint64_t sequence = 1; sequence <= 128; ++sequence)
        ASSERT_EQ(receive(fact(sequence), 8, 1008), Result::Applied);
    EXPECT_EQ(receive(fact(1), 8, 1008), Result::Duplicate);
    EXPECT_EQ(media.captures, 0U);
    EXPECT_EQ(receive(fact(129), 8, 1008), Result::Busy);
    EXPECT_EQ(client.state(), VoiceState::Failed);
    EXPECT_FALSE(client.candidate_focus_scope());
    EXPECT_EQ(media.captures, 0U);
    EXPECT_EQ(driver.generation, 0U);
}

// EndpointPrepare只代表可信HAL绑定阶段，Ready也不自动授FocusEngine或打开麦克风。
struct EndpointPrepare final : EndpointFocusPreparePort {
    EndpointFocusPreparation result =
        EndpointFocusPreparation::Pending;  // 用例独立交付异步绑定事实。
    unsigned calls = 0, cancels = 0;        // 重投不能创建第二绑定，关闭必须调用原取消。
    bool released = true;                   // 原实际清理事实，不把cancel受理当完成。
    EndpointFocusPreparation prepare(const AIEndpointFocusScope& scope, const FocusCommand& command,
                                     std::uint64_t, std::uint64_t) override {
        ++calls;
        EXPECT_EQ(scope.session_id, "candidate");
        EXPECT_EQ(command.focus_id, "focus-candidate");
        return result;
    }
    void cancel() override { ++cancels; }
    bool settled() const override { return released; }
};
// VoiceEndpointRuntimeTest从真实公共事件队列组合监听/Held屏障，不依赖系统网络或长等待。
class VoiceEndpointRuntimeTest : public testing::Test {
protected:
    EndpointNetwork network;
    EndpointMedia media;
    EndpointDriver driver;
    EndpointAuthority authority;
    FocusEndpoint focus{network,
                        driver,
                        authority,
                        {"microphone", "speaker"},
                        "$thing/up/signaling/p/d",
                        "$thing/down/signaling/p/d",
                        20000};
    EndpointPrepare prepare;
    VoiceRuntime runtime{network,    media,   "p/d",   endpoint_capability(),
                         "endpoint", nullptr, nullptr, &focus};
    std::uint64_t now = 1;  // 单调虚拟时钟，墙钟固定加1000，原邀请不续期。
    void drive() {
        ++now;
        runtime.poll(now, 1000 + now);
    }
    void queue(std::string body, std::string topic = "$thing/down/signaling/p/d") {
        network.events.push_back(
            {MqttEvent::Kind::Message, 0, 0, std::move(topic), std::move(body)});
    }
    void body(std::string value) {
        queue(std::move(value));
        drive();
    }
    void ack(std::string result) {
        const auto request = network.sent.back();
        body(endpoint_ack(request, std::move(result)));
    }
    void listening() {
        ASSERT_EQ(runtime.set_endpoint_focus_prepare(prepare), Result::Applied);
        ASSERT_EQ(runtime.start_listening({}, now), Result::Applied);
        EXPECT_EQ(prepare.cancels, 0U);  // 原start先close不应取消尚未启动的候选HAL。
        network.events.push_back({MqttEvent::Kind::Connected, 0, 0, {}, {}});
        drive();
        network.events.push_back({MqttEvent::Kind::Subscribed, 10, 0, {}, {}});
        network.events.push_back({MqttEvent::Kind::Subscribed, 11, 0, {}, {}});
        drive();
        ASSERT_EQ(network.sent.back().method, "capabilityReport");
        ack(R"({"acceptedRevision":1})");
        ASSERT_EQ(runtime.state(), RuntimeState::Running);
        ASSERT_EQ(runtime.voice().endpoint_phase(), AIEndpointPhase::Listening);
    }
    void media_ready() {
        listening();
        body(endpoint_invite());
        ASSERT_EQ(runtime.accept_endpoint_invite(now, 1000 + now), Result::Applied);
        ack(R"({"state":"negotiating"})");
        ASSERT_EQ(media.configurations, 1U);
        media.connected = true;
        drive();
        ASSERT_EQ(network.sent.back().method, "mediaState");
        ack(R"({"state":"connected","mediaRevision":1})");
    }
    // final_batch故意在一个真实poll批次交付两个不同scope事实与focusControl，宿主不得被赶过绑定屏障。
    void final_batch() {
        queue(endpoint_encode(
            "endpointState", "source-leg",
            R"({"state":"committed","revision":2,"handoffId":"move","candidateSessionId":"candidate"})",
            42, {}, "commit"));
        queue(endpoint_encode("sessionEstablished", "candidate",
                              R"({"state":"established","mediaRevision":1})", 2, {},
                              "established"));
        queue(focus_body());
        drive();
    }
    static std::string focus_body() {
        return R"({"msgToken":"focus","method":"focusControl","sessionId":"candidate","eventSeq":3,"params":{"operation":"acquire","expectedRevision":0,"focusId":"focus-candidate","resources":["microphone","speaker"],"priority":"conversation","resumePolicy":"explicit","expiresAt":20000}})";
    }
};
TEST_F(VoiceEndpointRuntimeTest, ListeningDoesNotCreateModelOrConfigureMedia) {
    listening();
    ASSERT_EQ(network.sent.size(), 1U);
    EXPECT_EQ(media.configurations, 0U);
    EXPECT_EQ(media.captures, 0U);
    EXPECT_EQ(driver.generation, 0U);
}
// 正常结束精确取消原待完成准备一次；迟到Ready不执行acquire，仍等原结束ACK和实际清理。
TEST_F(VoiceEndpointRuntimeTest, UserEndCancelsOriginalPendingPreparationWithoutTimeoutFailure) {
    runtime.enable_failure_diagnostics();
    media_ready();
    final_batch();
    ASSERT_EQ(runtime.state(), RuntimeState::Running);
    ASSERT_GT(prepare.calls, 0U);
    prepare.released = false;
    const auto cancellations = prepare.cancels;
    ASSERT_EQ(runtime.end(now), Result::Applied);
    const auto request = network.sent.back();
    ASSERT_EQ(request.method, "sessionEnd");
    drive();
    EXPECT_EQ(prepare.cancels, cancellations + 1U);
    prepare.result = EndpointFocusPreparation::Ready;
    now += 2001;
    runtime.poll(now, 1000 + now);
    EXPECT_EQ(runtime.state(), RuntimeState::Running);
    EXPECT_EQ(runtime.voice().state(), VoiceState::Ending);
    EXPECT_EQ(prepare.cancels, cancellations + 1U);
    EXPECT_EQ(driver.generation, 0U);
    EXPECT_EQ(media.captures, 0U);
    EXPECT_FALSE(runtime.failure_diagnostic());
    auto late = focus_body();
    const auto at = late.find("\"msgToken\":\"focus\"");
    ASSERT_NE(at, std::string::npos);
    late.replace(at, std::string("\"msgToken\":\"focus\"").size(), "\"msgToken\":\"late-focus\"");
    body(late);
    EXPECT_EQ(prepare.cancels, cancellations + 1U);
    EXPECT_EQ(driver.generation, 0U);
    EXPECT_EQ(runtime.voice().state(), VoiceState::Ending);
    EXPECT_FALSE(runtime.voice().candidate_focus_scope());
    auto wrong = late;
    const auto session_at = wrong.find("\"sessionId\":\"candidate\"");
    ASSERT_NE(session_at, std::string::npos);
    wrong.replace(session_at, std::string("\"sessionId\":\"candidate\"").size(),
                  "\"sessionId\":\"other-leg\"");
    body(wrong);
    EXPECT_EQ(prepare.cancels, cancellations + 1U);
    EXPECT_EQ(driver.generation, 0U);
    body(endpoint_end_ack(request));
    EXPECT_EQ(runtime.state(), RuntimeState::Draining);
    EXPECT_FALSE(runtime.voice().capturing());
    prepare.released = true;
    drive();
    EXPECT_EQ(runtime.state(), RuntimeState::Idle);
    EXPECT_FALSE(runtime.failure_diagnostic());
}

// 拒绝本人END的原邀请阶段不得取消任何宿主准备或生成新结束报文。
TEST_F(VoiceEndpointRuntimeTest, UnacceptedInvitationEndDoesNotCancelPreparation) {
    listening();
    body(endpoint_invite());
    const auto cancellations = prepare.cancels;
    const auto sent = network.sent.size();
    EXPECT_EQ(runtime.end(now), Result::Stale);
    drive();
    EXPECT_EQ(runtime.voice().endpoint_phase(), AIEndpointPhase::Invited);
    EXPECT_EQ(prepare.cancels, cancellations);
    EXPECT_EQ(network.sent.size(), sent);
    EXPECT_EQ(driver.generation, 0U);
}

// 两秒准备超时的原ClientTick与客户端失败分开；原双向资源仍沿cancel关闭。
TEST_F(VoiceEndpointRuntimeTest, FailureDiagnosticPrepareDeadlineRemainsOriginalTwoSeconds) {
    runtime.enable_failure_diagnostics();
    media_ready();
    final_batch();
    ASSERT_EQ(runtime.state(), RuntimeState::Running);
    now += 1999;
    runtime.poll(now, 1000 + now);
    ASSERT_EQ(runtime.state(), RuntimeState::Running);
    ++now;
    runtime.poll(now, 1000 + now);
    ASSERT_EQ(runtime.state(), RuntimeState::Failed);
    EXPECT_EQ(runtime.failure_diagnostic(), RuntimeFailure::ClientTick);
    const auto fact = runtime.take_client_failure_diagnostic();
    ASSERT_TRUE(fact);
    EXPECT_EQ(fact->origin, ClientFailureOrigin::EndpointPrepareFailed);
    EXPECT_EQ(fact->cause, VoiceFailureCause::EndpointPrepareDeadline);
    EXPECT_EQ(fact->method, VoiceFailureMethod::None);
    EXPECT_FALSE(runtime.take_client_failure_diagnostic());
    EXPECT_GT(prepare.cancels, 0U);
    EXPECT_EQ(media.captures, 0U);
}
// 未确认的接听沿原邀请绝对期限结束；迟到接听ACK不得配置UDP或重建候选。
TEST_F(VoiceEndpointRuntimeTest, MissingAcceptAcknowledgementExpiresAndLateAckCannotConfigureMedia) {
    listening();
    body(endpoint_invite());
    ASSERT_EQ(runtime.accept_endpoint_invite(now, 1000 + now), Result::Applied);
    const auto accept = network.sent.back();
    ASSERT_EQ(accept.method, "sessionAccept");
    now = 29999;
    runtime.poll(now, 1000 + now);
    ASSERT_EQ(runtime.state(), RuntimeState::Running);
    EXPECT_EQ(media.configurations, 0U);
    now = 30000; // 原expiresAt=31000；不是从最后重投重新计时。
    runtime.poll(now, 1000 + now);
    ASSERT_EQ(runtime.state(), RuntimeState::Failed);
    EXPECT_GT(prepare.cancels, 0U);
    EXPECT_GT(media.closes, 0U);
    EXPECT_EQ(media.captures, 0U);
    body(endpoint_ack(accept, R"({"state":"negotiating"})"));
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
    EXPECT_EQ(media.configurations, 0U);
    EXPECT_EQ(driver.generation, 0U);
}
// 接听ACK只允许安装媒体；真实连通仍需mediaState ACK，超时后的ACK/双提交都不能复活。
TEST_F(VoiceEndpointRuntimeTest, AcceptedButUnacknowledgedMediaExpiresWithoutReadinessOrRevival) {
    listening();
    body(endpoint_invite());
    ASSERT_EQ(runtime.accept_endpoint_invite(now, 1000 + now), Result::Applied);
    ack(R"({"state":"negotiating"})");
    ASSERT_EQ(media.configurations, 1U);
    media.connected = true; // 独立驱动连通事实不代替平台确认。
    drive();
    const auto media_state = network.sent.back();
    ASSERT_EQ(media_state.method, "mediaState");
    now = 29999;
    runtime.poll(now, 1000 + now);
    ASSERT_EQ(runtime.state(), RuntimeState::Running);
    EXPECT_NE(runtime.voice().endpoint_phase(), AIEndpointPhase::AwaitingCommit);
    EXPECT_EQ(media.captures, 0U);
    EXPECT_EQ(driver.generation, 0U);
    now = 30000;
    runtime.poll(now, 1000 + now);
    ASSERT_EQ(runtime.state(), RuntimeState::Failed);
    EXPECT_FALSE(media.connected);
    EXPECT_GT(prepare.cancels, 0U);
    body(endpoint_ack(media_state, R"({"state":"connected","mediaRevision":1})"));
    final_batch();
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
    EXPECT_EQ(runtime.start_input(now), Result::Stale);
    EXPECT_EQ(media.captures, 0U);
    EXPECT_EQ(driver.generation, 0U);
    EXPECT_EQ(media.configurations, 1U);
}
// 原媒体确认负向ACK必须关闭候选；HAL取消调用是本机停止请求，不代表真实音频线程已释放。
TEST_F(VoiceEndpointRuntimeTest, RejectedMediaStateClosesCandidateAndLateAckCannotRevive) {
    listening();
    body(endpoint_invite());
    ASSERT_EQ(runtime.accept_endpoint_invite(now, 1000 + now), Result::Applied);
    ack(R"({"state":"negotiating"})");
    media.connected = true;
    drive();
    const auto request = network.sent.back(); // 保留正式Runtime发出的原媒体token及公开腿。
    ASSERT_EQ(request.method, "mediaState");
    const auto closed = media.closes;
    const auto cancelled = prepare.cancels;
    body(endpoint_ack(request, "{}", 403));
    ASSERT_EQ(runtime.state(), RuntimeState::Failed);
    EXPECT_FALSE(media.connected);
    EXPECT_GT(media.closes, closed);
    EXPECT_GT(prepare.cancels, cancelled);
    EXPECT_FALSE(runtime.voice().capturing());
    EXPECT_FALSE(runtime.voice().candidate_focus_scope());
    EXPECT_TRUE(runtime.voice().session_id().empty());
    EXPECT_EQ(media.captures, 0U);
    EXPECT_EQ(media.plays, 0U);
    EXPECT_EQ(driver.generation, 0U);
    body(endpoint_ack(request, R"({"state":"connected","mediaRevision":1})"));
    final_batch();
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
    EXPECT_EQ(runtime.start_input(now), Result::Stale);
    EXPECT_FALSE(media.connected);
    EXPECT_EQ(media.configurations, 1U);
    EXPECT_EQ(media.captures, 0U);
    EXPECT_EQ(driver.generation, 0U);
}
// 拒接负向回执沿原请求关联失败关闭，不保留待接听候选，也不影响原源腿。
TEST_F(VoiceEndpointRuntimeTest, RejectedLocalRejectAcknowledgementCancelsCandidateResources) {
    listening();
    body(endpoint_invite());
    ASSERT_EQ(runtime.reject_endpoint_invite(now, 1000 + now), Result::Applied);
    const auto request = network.sent.back(); // 与实际本机userRejected请求完全相同的关联。
    ASSERT_EQ(request.method, "sessionReject");
    const auto closed = media.closes;
    const auto cancelled = prepare.cancels;
    body(endpoint_ack(request, "{}", 403));
    ASSERT_EQ(runtime.state(), RuntimeState::Failed);
    EXPECT_GT(media.closes, closed);
    EXPECT_GT(prepare.cancels, cancelled);
    EXPECT_FALSE(media.connected);
    EXPECT_FALSE(runtime.voice().capturing());
    EXPECT_TRUE(runtime.voice().session_id().empty());
    EXPECT_EQ(media.configurations, 0U);
    EXPECT_EQ(media.captures, 0U);
    EXPECT_EQ(media.plays, 0U);
    body(endpoint_ack(request, R"({"state":"ended","reason":"userRejected"})"));
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
    EXPECT_EQ(runtime.accept_endpoint_invite(now, 1000 + now), Result::Stale);
    EXPECT_EQ(media.configurations, 0U);
    EXPECT_EQ(driver.generation, 0U);
}
// 核心保留非法ACK的pending不等于宿主继续允许候选；正式Runtime严格失败关闭并拒绝迟到复活。
TEST_F(VoiceEndpointRuntimeTest, MalformedLocalRejectAcknowledgementFailsClosedBeforeAnyMedia) {
    listening();
    body(endpoint_invite());
    ASSERT_EQ(runtime.reject_endpoint_invite(now, 1000 + now), Result::Applied);
    const auto request = network.sent.back(); // 只增加未知result字段，不换token伪造其它请求。
    const auto closed = media.closes;
    const auto cancelled = prepare.cancels;
    body(endpoint_ack(request, R"({"state":"ended","reason":"userRejected","unknown":true})"));
    ASSERT_EQ(runtime.state(), RuntimeState::Failed);
    EXPECT_GT(media.closes, closed);
    EXPECT_GT(prepare.cancels, cancelled);
    EXPECT_FALSE(runtime.voice().capturing());
    EXPECT_FALSE(runtime.voice().candidate_focus_scope());
    EXPECT_EQ(media.configurations, 0U);
    EXPECT_EQ(media.captures, 0U);
    EXPECT_EQ(media.plays, 0U);
    body(endpoint_ack(request, R"({"state":"ended","reason":"userRejected"})"));
    final_batch();
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
    EXPECT_EQ(runtime.start_input(now), Result::Stale);
    EXPECT_EQ(media.configurations, 0U);
    EXPECT_EQ(driver.generation, 0U);
}
TEST_F(VoiceEndpointRuntimeTest, SamePollFinalFactsDeferFocusUntilActualPrepareThenActualACK) {
    media_ready();
    final_batch();
    ASSERT_EQ(runtime.state(), RuntimeState::Running);
    ASSERT_GT(prepare.calls, 0U);
    EXPECT_EQ(driver.generation, 0U);
    EXPECT_EQ(media.captures, 0U);
    EXPECT_NE(runtime.voice().state(), VoiceState::Established);
    prepare.result = EndpointFocusPreparation::Ready;
    drive();  // 只交原FocusEndpoint，驱动完成仍未发生。
    drive();  // 原FocusEngine才受理驱动。
    ASSERT_GT(driver.generation, 0U);
    EXPECT_EQ(media.captures, 0U);
    driver.complete();
    drive();
    ASSERT_EQ(runtime.voice().endpoint_phase(), AIEndpointPhase::Active);
    ASSERT_EQ(runtime.start_input(now), Result::Applied);
    EXPECT_EQ(media.captures, 0U);
    const auto request = network.sent.back();
    ack(request.payload);
    EXPECT_EQ(media.captures, 1U);
}
TEST_F(VoiceEndpointRuntimeTest, DuplicateFocusDoesNotExtendFirstPrepareDeadline) {
    media_ready();
    final_batch();
    const auto first = now;
    now = first + 1998;
    body(focus_body());
    ASSERT_EQ(runtime.state(), RuntimeState::Running);
    now = first + 2000;
    runtime.poll(now, 1000 + now);
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
    EXPECT_EQ(media.captures, 0U);
    EXPECT_EQ(driver.generation, 0U);
    EXPECT_GT(prepare.cancels, 0U);
}
TEST_F(VoiceEndpointRuntimeTest, PendingFocusCannotBeReplacedByNewToken) {
    media_ready();
    final_batch();
    auto changed = focus_body();
    const auto pos = changed.find("\"msgToken\":\"focus\"");
    ASSERT_NE(pos, std::string::npos);
    changed.replace(pos, 18, "\"msgToken\":\"other\"");
    body(std::move(changed));
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
    EXPECT_EQ(driver.generation, 0U);
    EXPECT_EQ(media.captures, 0U);
}
// 候选逻辑终态不能当作HAL已实际释放；唯一运行时保留原连接直到原清理事实齐备。
TEST_F(VoiceEndpointRuntimeTest, CandidateEndWaitsForActualPrepareCleanup) {
    media_ready();
    final_batch();
    // 启动前close已断开空连接；这里只验证原候选终态之后没有提前断开实际连接。
    const auto original_disconnects = network.disconnects;
    prepare.released = false;
    body(endpoint_encode("sessionEnded", "candidate", R"({"state":"ended","reason":"userHangup"})",
                         4));
    EXPECT_EQ(runtime.state(), RuntimeState::Draining);
    EXPECT_GT(prepare.cancels, 0U);
    EXPECT_EQ(network.disconnects, original_disconnects);
    EXPECT_EQ(media.captures, 0U);
    drive();
    EXPECT_EQ(runtime.state(), RuntimeState::Draining);
    prepare.released = true;
    drive();
    EXPECT_EQ(runtime.state(), RuntimeState::Idle);
    EXPECT_EQ(network.disconnects, original_disconnects + 1U);
}
// 墙钟回退即使发生在异步准备中也失败关闭，不能借尚未授焦点继续等候。
TEST_F(VoiceEndpointRuntimeTest, WallClockRollbackDuringPrepareFailsClosed) {
    media_ready();
    final_batch();
    ++now;
    runtime.poll(now, 999);
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
    EXPECT_GT(prepare.cancels, 0U);
    EXPECT_EQ(driver.generation, 0U);
    EXPECT_EQ(media.captures, 0U);
}
// WebRTC候选复用原SDP/ICE及双提交屏障，监听和接听不能直接取得媒体或开麦。
TEST(VoiceEndpointWebRTC, RuntimeRequiresOriginalHALAndOnlyReportsCapabilityWhenListening) {
    EndpointNetwork network;
    EndpointMedia media;
    EndpointDriver driver;
    EndpointAuthority authority;
    FocusEndpoint focus{network, driver, authority, {"microphone", "speaker"},
                        "$thing/up/signaling/p/d", "$thing/down/signaling/p/d", 20000};
    EndpointPrepare prepare;
    auto capability = endpoint_capability(); // 明确原实际48k格式，不能从其他媒体能力推导。
    capability.transport = VoiceTransport::WebRTC;
    capability.audio = {48000, 1, 20};
    VoiceRuntime runtime{network, media, "p/d", capability, "webrtc-listener", nullptr,
                         nullptr, &focus};
    EXPECT_EQ(runtime.start_listening({}, 1), Result::Unsupported);
    ASSERT_EQ(runtime.set_endpoint_focus_prepare(prepare), Result::Applied);
    ASSERT_EQ(runtime.start_listening({}, 1), Result::Applied);
    network.events.push_back({MqttEvent::Kind::Connected, 0, 0, {}, {}});
    runtime.poll(2, 1002);
    network.events.push_back({MqttEvent::Kind::Subscribed, 10, 0, {}, {}});
    network.events.push_back({MqttEvent::Kind::Subscribed, 11, 0, {}, {}});
    runtime.poll(3, 1003);
    ASSERT_EQ(network.sent.size(), 1U);
    EXPECT_EQ(network.sent.back().method, "capabilityReport");
    network.events.push_back({MqttEvent::Kind::Message, 0, 0, "$thing/down/signaling/p/d",
                              endpoint_ack(network.sent.back(), R"({"acceptedRevision":1})")});
    runtime.poll(4, 1004);
    EXPECT_EQ(runtime.voice().endpoint_phase(), AIEndpointPhase::Listening);
    EXPECT_EQ(network.sent.size(), 1U);
    EXPECT_EQ(media.configurations, 0U);
    EXPECT_EQ(media.captures, 0U);
}
// 原邀请先明确接听、原SDP和ICE独立成立、双提交后仍保持未取得输入。
TEST(VoiceEndpointWebRTC, UsesOriginalNegotiationAndCommitWithoutStartingInput) {
    EndpointNetwork network;
    EndpointMedia media;
    media.sequence = 0;
    EndpointDriver driver;
    EndpointAuthority authority;
    FocusEndpoint focus{network,
                        driver,
                        authority,
                        {"microphone", "speaker"},
                        "$thing/up/signaling/p/d",
                        "$thing/down/signaling/p/d",
                        20000};
    auto capability = endpoint_capability();  // 原实际格式是WebRTC 48k/20ms，不跨transport转换。
    capability.transport = VoiceTransport::WebRTC;
    capability.audio = {48000, 1, 20};
    VoiceClient client{network,
                       media,
                       "$thing/up/signaling/p/d",
                       "$thing/down/signaling/p/d",
                       capability,
                       "webrtc-listener",
                       &focus};
    // 原合法事件经正式公共编码后交同一VoiceClient，不能跳过会话水位或真实屏障。
    const auto receive = [&](const std::string& body, std::uint64_t now) {
        return client.receive({"$thing/down/signaling/p/d", body}, now, 1000 + now);
    };
    ASSERT_EQ(client.connect_listening(1), Result::Applied);
    ASSERT_EQ(receive(endpoint_ack(network.sent.back(), R"({"acceptedRevision":1})"), 2),
              Result::Applied);
    ASSERT_EQ(network.sent.size(), 1U);
    EXPECT_EQ(media.configurations, 0U);
    ASSERT_EQ(
        receive(
            endpoint_encode(
                "sessionInvite", "candidate",
                R"({"purpose":"aiConversation","admission":"accept","expiresAt":31000,"handoff":{"sourceSessionId":"source-leg","handoffId":"move"},"selected":{"transport":"webrtc","interactionMode":"fullDuplex","audioParams":{"format":"opus","sampleRate":48000,"channels":1,"frameDuration":20}},"tracks":[{"trackId":"mic-1","kind":"audio","sourceId":"mic","direction":"sendonly","codec":"opus","state":"requested"},{"trackId":"speaker-1","kind":"audio","sourceId":"speaker","direction":"recvonly","codec":"opus","state":"requested"}]})",
                1, {}, "invite"),
            3),
        Result::Applied);
    ASSERT_EQ(client.accept_endpoint_invite(4, 1004), Result::Applied);
    EXPECT_EQ(media.configurations, 0U);
    ASSERT_EQ(receive(endpoint_ack(network.sent.back(), R"({"state":"negotiating"})"), 5),
              Result::Applied);
    EXPECT_EQ(media.configurations, 1U);
    EXPECT_EQ(client.media_ready(6), Result::Stale);
    ASSERT_EQ(client.media_offer_ready("v=0\r\no=offer\r\n", 6), Result::Applied);
    const auto offer = network.sent.back();
    EXPECT_EQ(offer.method, "mediaOffer");
    ASSERT_EQ(
        receive(
            endpoint_encode(
                "mediaAnswer", "candidate",
                R"({"mediaRevision":1,"webrtc":{"iceMode":"complete","sdp":"v=0\r\no=answer\r\n"}})",
                0, 200, offer.token),
            7),
        Result::Applied);
    EXPECT_EQ(media.answers, 1U);
    media.connected = true;  // 用例独立交付实际ICE事实，不能让SDP应答冒充驱动连通。
    ASSERT_EQ(client.media_ready(8), Result::Applied);
    ASSERT_EQ(
        receive(endpoint_ack(network.sent.back(), R"({"mediaRevision":1,"state":"connected"})"), 9),
        Result::Applied);
    ASSERT_EQ(
        receive(
            endpoint_encode(
                "endpointState", "source-leg",
                R"({"handoffId":"move","state":"committed","revision":2,"candidateSessionId":"candidate"})",
                3, {}, "commit"),
            10),
        Result::Applied);
    EXPECT_FALSE(client.candidate_focus_scope());
    ASSERT_EQ(receive(endpoint_encode("sessionEstablished", "candidate",
                                      R"({"state":"established","mediaRevision":1})", 2),
                      11),
              Result::Applied);
    ASSERT_TRUE(client.candidate_focus_scope());
    EXPECT_EQ(client.endpoint_phase(), AIEndpointPhase::AwaitingFocus);
    EXPECT_EQ(client.state(), VoiceState::Negotiating);
    EXPECT_EQ(client.start_input(12), Result::Stale);
    EXPECT_EQ(media.captures, 0U);
}
// ReceiptEndpointFixture沿原邀请、双提交和完整焦点实际取得，能力显式selected。
struct ReceiptEndpointFixture : VoiceEndpointTest {
    ReceiptEndpointFixture() : VoiceEndpointTest(true) {}
};
TEST_F(ReceiptEndpointFixture, HandoffOriginalStartCannotPublishBeforeFocusOrAfterFocusLoss) {
    media_ready();
    commit();
    established();
    const auto old_start = endpoint_encode(
        "respAudioStart", "candidate",
        R"({"respId":"old","trackId":"speaker-1","mediaRevision":1,"udpNonce":"0000000000000063","firstPacketSeq":128})",
        4, {}, "old-start");
    EXPECT_EQ(receive(old_start, 10, 1010), Result::Stale);
    acquire();
    EXPECT_EQ(receive(old_start, 14, 1014), Result::Stale);
    const auto input = endpoint_encode(
        "respAudioStart", "candidate",
        R"({"respId":"new","trackId":"speaker-1","mediaRevision":1,"udpNonce":"0000000000000064","firstPacketSeq":128})",
        5, {}, "new-start");
    ASSERT_EQ(receive(input, 15, 1015), Result::Applied);
    const auto receipts = [&] {
        return std::count_if(network.sent.begin(), network.sent.end(), [](const auto& e) {
            return e.method == "respAudioStart" && e.code == 200;
        });
    };
    ASSERT_EQ(receipts(), 1);
    ASSERT_EQ(media.plays, 1U);
    authority.allowed = false;
    client.tick(16, 1016);
    authority.allowed = true;
    EXPECT_EQ(receive(input, 17, 1017), Result::Stale);
    client.tick(18, 1018);
    EXPECT_EQ(receipts(), 1);
    EXPECT_EQ(media.plays, 1U);
}

// 缺原邀请水位时保留监听和零媒体副作用；后续原完整邀请仍能首次受理。
TEST_F(VoiceEndpointTest, MissingInviteSequenceDoesNotInstallCandidateOrConsumeWatermark) {
    listening();
    Envelope missing;
    ASSERT_EQ(decode_envelope(endpoint_invite(), missing), DecodeResult::Ok);
    missing.event_sequence.reset();
    std::string body;
    ASSERT_EQ(encode_envelope(missing, body), DecodeResult::Ok);
    EXPECT_EQ(receive(body, 3, 1003), Result::Invalid);
    EXPECT_EQ(client.endpoint_phase(), AIEndpointPhase::Listening);
    EXPECT_TRUE(client.session_id().empty());
    EXPECT_FALSE(client.candidate_focus_scope());
    EXPECT_EQ(media.configurations, 0U);
    EXPECT_EQ(media.captures, 0U);
    EXPECT_EQ(media.plays, 0U);
    EXPECT_EQ(network.sent.size(), 1U);
    EXPECT_EQ(receive(endpoint_invite(), 4, 1004), Result::Applied);
    EXPECT_EQ(client.endpoint_phase(), AIEndpointPhase::Invited);
    EXPECT_EQ(receive(endpoint_invite(), 5, 1005), Result::Duplicate);
}

// 空准备槽多次泵送不调用HAL；拒绝候选后迟到整组取得也不能创建新准备或媒体许可。
TEST_F(VoiceEndpointRuntimeTest, EmptyPreparationAndRejectedCandidateDoNotStartLateHAL) {
    listening();
    for (unsigned i = 0; i < 3; ++i) drive();
    EXPECT_EQ(prepare.calls, 0U);
    EXPECT_EQ(prepare.cancels, 0U);
    body(endpoint_invite());
    ASSERT_EQ(runtime.reject_endpoint_invite(now, 1000 + now), Result::Applied);
    const auto request = network.sent.back();
    ASSERT_EQ(request.method, "sessionReject");
    body(endpoint_ack(request, R"({"state":"ended","reason":"userRejected"})"));
    ASSERT_EQ(runtime.state(), RuntimeState::Idle);
    body(focus_body());
    for (unsigned i = 0; i < 3; ++i) drive();
    EXPECT_EQ(runtime.state(), RuntimeState::Idle);
    EXPECT_EQ(prepare.calls, 0U);
    EXPECT_EQ(driver.generation, 0U);
    EXPECT_EQ(media.configurations, 0U);
    EXPECT_EQ(media.captures, 0U);
    EXPECT_EQ(media.plays, 0U);
}
}  // namespace
}  // namespace ur::device
