// 文件说明：真实Runtime和候选HAL共享当前双时钟；受控网络及门控票据用于核验迟到意图与物理回拨边界，不冒实际媒体。
#include "browser_ai_endpoint_focus.hpp"
#include "ur/device/voice_runtime.hpp"
#include <gtest/gtest.h>
#include <deque>
#include <map>

namespace ur::device {
namespace {
// ClockNetwork 保存原MQTT公共报文和显式事件，不制造焦点或媒体完成事实。
struct ClockNetwork final : MqttPort {
    std::deque<MqttEvent> events;
    std::vector<Envelope> sent;
    int subscription = 10;
    bool connect(const MqttConfig&) override { return true; }
    bool poll() override { return true; }
    bool publications_drained() override { return true; }
    int subscribe(std::string_view) override { return subscription++; }
    void disconnect() override {}
    std::optional<MqttEvent> take_event() override {
        if (events.empty()) return {};
        auto value = std::move(events.front());
        events.pop_front();
        return value;
    }
    bool publish(std::string_view topic, std::string_view body) override {
        EXPECT_EQ(topic, "$thing/up/signaling/p/d");
        Envelope value;
        EXPECT_EQ(decode_envelope(body, value), DecodeResult::Ok);
        sent.push_back(std::move(value));
        return true;
    }
};
// ClockMedia 连通和采集事实由夹具单独推进，configure成功不形成实际connected。
struct ClockMedia final : PumpedVoiceMediaPort {
    bool connected = false, capture = false;
    bool configure(const VoiceMedia&) override { return true; }
    bool poll(std::uint64_t) override { return true; }
    bool ready() const override { return connected; }
    std::optional<std::uint64_t> drained_epoch() const override { return {}; }
    std::uint32_t next_input_sequence() const override { return 128; }
    bool prepare_reply(const std::array<std::uint8_t, 8>&, std::uint32_t) override { return true; }
    bool finish_reply(std::uint32_t) override { return true; }
    bool start_capture(std::string_view) override { capture = true; return true; }
    void stop_capture() override { capture = false; }
    bool start_playback(std::string_view, std::uint64_t) override { return true; }
    void stop_playback(std::uint64_t) override {}
    void close() override { connected = capture = false; }
};
// clock_capability 仅广告本夹具已装配的双向候选HAL，不隐式授控制恢复权限。
VoiceCapability clock_capability() {
    VoiceCapability value;
    value.audio = {16000, 1, 20};
    value.modes = {true, true, true};
    value.ai_endpoint_handoff = true;
    return value;
}
// ClockOutput 只确认实际已发出的原输出票据，受理本身不形成完成。
struct ClockOutput final : OutputGroupGatePort {
    std::uint64_t next = 0;
    std::map<std::uint64_t, bool> pending, actual;
    std::optional<std::uint64_t> request_output_gate(bool enabled, std::uint64_t) override {
        pending[++next] = enabled;
        return next;
    }
    bool output_gate_confirmed(std::uint64_t ticket, bool enabled) const override {
        auto found = actual.find(ticket);
        return found != actual.end() && found->second == enabled;
    }
};
// ActualScopeClockRig 以原消息队列建立候选和真实FocusEngine完整组；HAL回调读取当前真实VoiceClient作用域。
class ActualScopeClockRig : public testing::Test {
protected:
    ClockNetwork network;
    ClockMedia media;
    ClockOutput output;
    std::vector<std::string> commands;
    BrowserAIEndpointFocus hal{output, 7, 3600000, 3601000, [] { return true; },
        [](std::string_view session, std::uint64_t generation) {
            return session == "candidate" && generation == 1;
        }, [this] { return runtime.voice().candidate_focus_scope(); },
        [this](const std::string& body) { commands.push_back(body); return true; }};
    FocusEndpoint focus{network, hal.driver(), hal, {"microphone", "speaker"},
        "$thing/up/signaling/p/d", "$thing/down/signaling/p/d", 3600000};
    VoiceRuntime runtime{network, media, "p/d", clock_capability(), "instance", nullptr, nullptr, &focus};
    std::uint64_t now = 2;
    // drive 推进同一受控双时钟，并显式确认已经发出的门控票据，不用sleep或生产钩子。
    void drive() {
        ++now;
        output.actual = output.pending;
        const auto pending = std::move(commands);
        commands.clear();
        for (auto body : pending) {
            const auto at = body.find("inputOwnershipGate\"");
            if (at == std::string::npos) continue;
            body.replace(at, 18, "inputOwnershipGated");
            ASSERT_TRUE(hal.receive(body, now));
        }
        ASSERT_TRUE(hal.poll(now, 1000 + now));
        runtime.poll(now, 1000 + now);
    }
    // body 仅把原合法报文交MQTT队列，全部解析和状态推进仍由真实Runtime执行。
    void body(std::string value) {
        network.events.push_back({MqttEvent::Kind::Message, 0, 0,
            "$thing/down/signaling/p/d", std::move(value)});
        drive();
    }
    // ack 严格匹配本轮实际上行token与method，不合成安装、焦点或媒体准入。
    void ack(std::string result) {
        const auto original = network.sent.back();
        Envelope value;
        value.request = false;
        value.token = original.token;
        value.session = original.session;
        value.method = "requestAck";
        value.code = 200;
        value.payload = "{\"requestMethod\":\"" + original.method + "\",\"result\":" + result + "}";
        std::string encoded;
        ASSERT_EQ(encode_envelope(value, encoded), DecodeResult::Ok);
        body(encoded);
    }
    // active 通过原候选邀请、接听、媒体事实与FocusControl取得完整组，确认由真实HAL发出的双向票据。
    void active() {
        ASSERT_TRUE(hal.initialize(1));
        ASSERT_TRUE(hal.receive(R"({"kind":"inputOwnershipIdle","bootGeneration":"7"})", 2));
        output.actual = output.pending;
        ASSERT_TRUE(hal.poll(2, 1002));
        ASSERT_EQ(runtime.set_endpoint_focus_prepare(hal), Result::Applied);
        runtime.enable_message_diagnostics();
        ASSERT_EQ(runtime.start_listening({}, now), Result::Applied);
        network.events.push_back({MqttEvent::Kind::Connected, 0, 0, {}, {}});
        drive();
        for (int mid = 10; mid < 12; ++mid)
            network.events.push_back({MqttEvent::Kind::Subscribed, mid, 0, {}, {}});
        drive();
        ASSERT_EQ(network.sent.back().method, "capabilityReport");
        ack(R"({"acceptedRevision":1})");
        body(R"({"msgToken":"invite","method":"sessionInvite","sessionId":"candidate","eventSeq":1,"params":{"purpose":"aiConversation","admission":"accept","expiresAt":31000,"handoff":{"sourceSessionId":"old-source","handoffId":"move"},"selected":{"transport":"udp","interactionMode":"fullDuplex","audioParams":{"format":"opus","sampleRate":16000,"channels":1,"frameDuration":20}},"tracks":[{"trackId":"mic-1","kind":"audio","sourceId":"mic","direction":"sendonly","codec":"opus","state":"requested"},{"trackId":"speaker-1","kind":"audio","sourceId":"speaker","direction":"recvonly","codec":"opus","state":"requested"}],"media":{"udp":{"server":"127.0.0.1","port":12345,"key":"000102030405060708090a0b0c0d0e0f","nonce":"01000000112233445566778800000000"}}}})");
        ASSERT_EQ(runtime.accept_endpoint_invite(now, 1000 + now), Result::Applied);
        ack(R"({"state":"negotiating"})");
        media.connected = true;
        drive();
        ASSERT_EQ(network.sent.back().method, "mediaState");
        ack(R"({"state":"connected","mediaRevision":1})");
        body(R"({"msgToken":"committed","method":"endpointState","sessionId":"old-source","eventSeq":42,"data":{"state":"committed","revision":4,"handoffId":"move","candidateSessionId":"candidate"}})");
        body(R"({"msgToken":"established","method":"sessionEstablished","sessionId":"candidate","eventSeq":2,"data":{"state":"established","mediaRevision":1}})");
        body(R"({"msgToken":"acquire","method":"focusControl","sessionId":"candidate","eventSeq":3,"params":{"operation":"acquire","expectedRevision":0,"focusId":"candidate-focus","resources":["microphone","speaker"],"priority":"conversation","resumePolicy":"explicit","expiresAt":20000}})");
        for (unsigned i = 0; i < 10; ++i) drive();
        ASSERT_EQ(runtime.voice().endpoint_phase(), AIEndpointPhase::Active);
        ASSERT_EQ(runtime.state(), RuntimeState::Running);
        ASSERT_TRUE(focus.scoped_permitted("candidate", {"microphone", "speaker"}, now, 1000 + now));
    }
    // source_fact 只生成实际上行原token对应的受控平台应答，不造焦点或媒体完成。
    void source_fact(std::string state, unsigned revision, unsigned sequence = 0) {
        const auto* receipt = runtime.endpoint_source_receipt();
        ASSERT_NE(receipt, nullptr);
        Envelope reply;
        reply.request = false;
        reply.method = "endpointState";
        reply.token = sequence ? "next-ready" : receipt->token;
        reply.session = receipt->session;
        if (sequence) reply.event_sequence = sequence;
        else reply.code = 200;
        reply.payload = "{\"state\":\"" + state + "\",\"revision\":" + std::to_string(revision) +
            ",\"handoffId\":\"" + receipt->handoff + "\"";
        if (state == "ready") reply.payload += ",\"candidateSessionId\":\"next\"";
        reply.payload += "}";
        std::string encoded;
        ASSERT_EQ(encode_envelope(reply, encoded), DecodeResult::Ok);
        body(encoded);
    }
    // source_ready 从真实已取得候选提交原prepare，再按原请求/事件水位确认ready。
    void source_ready() {
        ASSERT_NO_FATAL_FAILURE(active());
        ++now;
        ASSERT_EQ(runtime.prepare_endpoint_source("device:p:next", now, 1000 + now), Result::Applied);
        source_fact("preparing", 5);
        source_fact("ready", 6, 4);
        ASSERT_EQ(runtime.endpoint_source_receipt()->state, "ready");
    }
};
// 外部宿主只读合法查询后，本人下一跳原prepare不得被缓存的较旧墙钟误判成物理回拨。
TEST_F(ActualScopeClockRig, FreshHostQueryThenSourcePrepareKeepsOriginalAuthority) {
    ASSERT_NO_FATAL_FAILURE(active());
    ASSERT_TRUE(focus.scoped_permitted("candidate", {"microphone", "speaker"}, now, 1001 + now));
    EXPECT_EQ(runtime.prepare_endpoint_source("device:p:next", now + 1, 1001 + now), Result::Applied);
    drive();
    EXPECT_EQ(runtime.state(), RuntimeState::Running);
    EXPECT_EQ(runtime.voice().endpoint_phase(), AIEndpointPhase::Active);
}
// 后续commit/abort同样使用本次权威时间，不以缓存wall误撤原完整组。
TEST_F(ActualScopeClockRig, FreshHostQueryThenSourceCommitKeepsOriginalAuthority) {
    ASSERT_NO_FATAL_FAILURE(source_ready());
    ASSERT_TRUE(focus.scoped_permitted("candidate", {"microphone", "speaker"}, now, 1001 + now));
    EXPECT_EQ(runtime.commit_endpoint_source(now + 1, 1001 + now), Result::Applied);
    drive();
    EXPECT_EQ(runtime.state(), RuntimeState::Running);
    EXPECT_EQ(runtime.voice().endpoint_phase(), AIEndpointPhase::Active);
}
TEST_F(ActualScopeClockRig, FreshHostQueryThenSourceAbortKeepsOriginalAuthority) {
    ASSERT_NO_FATAL_FAILURE(source_ready());
    ASSERT_TRUE(focus.scoped_permitted("candidate", {"microphone", "speaker"}, now, 1001 + now));
    EXPECT_EQ(runtime.abort_endpoint_source(now + 1, 1001 + now), Result::Applied);
    drive();
    EXPECT_EQ(runtime.state(), RuntimeState::Running);
    EXPECT_EQ(runtime.voice().endpoint_phase(), AIEndpointPhase::Active);
}
// 旧排队意图与新的物理时钟不同；拒旧请求不能把有效authority永久毒化。
TEST_F(ActualScopeClockRig, QueuedOldIntentSnapshotDoesNotRevokeCurrentAuthority) {
    ASSERT_NO_FATAL_FAILURE(active());
    const auto cached_now = now, cached_wall = 1000 + now;
    ASSERT_TRUE(focus.scoped_permitted("candidate", {"microphone", "speaker"}, now, cached_wall + 1));
    ASSERT_EQ(runtime.prepare_endpoint_source("device:p:next", now + 1, cached_wall + 1), Result::Applied);
    const auto sent = network.sent.size();
    EXPECT_EQ(runtime.prepare_endpoint_source("device:p:other", cached_now, cached_wall), Result::Stale);
    EXPECT_EQ(runtime.commit_endpoint_source(cached_now, cached_wall), Result::Stale);
    EXPECT_EQ(runtime.abort_endpoint_source(cached_now, cached_wall), Result::Stale);
    EXPECT_EQ(network.sent.size(), sent);
    drive();
    EXPECT_EQ(runtime.state(), RuntimeState::Running);
    EXPECT_TRUE(focus.scoped_permitted("candidate", {"microphone", "speaker"}, now, 1000 + now));
}
// 真正poll时钟逆跳仍由原核心hard-fail，不把显式API守卫当作回拨兜底。
TEST_F(ActualScopeClockRig, PhysicalMonotonicRollbackStillClosesOriginalScope) {
    ASSERT_NO_FATAL_FAILURE(active());
    ++now;
    ASSERT_EQ(runtime.prepare_endpoint_source("device:p:next", now, 1000 + now), Result::Applied);
    runtime.poll(now - 1, 1000 + now + 1);
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
    EXPECT_NE(runtime.voice().endpoint_phase(), AIEndpointPhase::Active);
}
TEST_F(ActualScopeClockRig, PhysicalWallRollbackStillClosesOriginalScope) {
    ASSERT_NO_FATAL_FAILURE(active());
    ++now;
    ASSERT_EQ(runtime.prepare_endpoint_source("device:p:next", now, 1000 + now), Result::Applied);
    runtime.poll(now + 1, 1000 + now - 1);
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
    EXPECT_NE(runtime.voice().endpoint_phase(), AIEndpointPhase::Active);
}
// 新当前时间不能续原焦点期限，实际到期仍拒请求并按原路径关闭。
TEST_F(ActualScopeClockRig, OriginalLeaseDeadlineCannotBeExtendedByIntentClock) {
    ASSERT_NO_FATAL_FAILURE(active());
    EXPECT_EQ(runtime.prepare_endpoint_source("device:p:next", now + 1, 20000), Result::Stale);
    runtime.poll(now + 1, 20000);
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
    EXPECT_NE(runtime.voice().endpoint_phase(), AIEndpointPhase::Active);
}
// 本人原结束实际撤回scope后，新接口不能复活旧取得或签新候选。
TEST_F(ActualScopeClockRig, ExplicitOriginalEndStillRejectsSourceIntent) {
    ASSERT_NO_FATAL_FAILURE(active());
    ASSERT_EQ(runtime.end(++now), Result::Applied);
    const auto sent = network.sent.size();
    EXPECT_EQ(runtime.prepare_endpoint_source("device:p:next", now, 1000 + now), Result::Stale);
    EXPECT_EQ(runtime.commit_endpoint_source(now, 1000 + now), Result::Stale);
    EXPECT_EQ(runtime.abort_endpoint_source(now, 1000 + now), Result::Stale);
    EXPECT_EQ(network.sent.size(), sent);
    EXPECT_FALSE(runtime.endpoint_source_intent_available());
}

} // namespace
} // namespace ur::device
