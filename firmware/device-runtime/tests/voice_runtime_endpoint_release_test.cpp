// 文件说明：候选成为新源后正常转出，唯一运行时必须保留原清理通道；虚拟驱动不代替真实媒体验收。
#include <gtest/gtest.h>
#include <algorithm>
#include <deque>
#include "ur/device/voice_runtime.hpp"

namespace ur::device {
namespace {
// ReleaseNetwork只保存正式公共报文及网络事件，不制造焦点或媒体完成事实。
struct ReleaseNetwork final : MqttPort {
    std::deque<MqttEvent> events;
    std::vector<Envelope> sent;
    std::vector<Envelope> delivered; // 只记录发布端实际受理，不能把背压尝试当回执已交付。
    std::string blocked_method; // 只对指定原method注入背压，不改变网络连通或其他请求。
    bool online = true; // 仅模拟发布背压，仍保存每次实际尝试。
    int subscription = 10;
    unsigned disconnects = 0;
    bool connect(const MqttConfig&) override { return true; }
    bool poll() override { return true; }
    bool publications_drained() override { return true; }
    int subscribe(std::string_view) override { return subscription++; }
    void disconnect() override { ++disconnects; }
    std::optional<MqttEvent> take_event() override {
        if (events.empty())
            return {};
        auto value = std::move(events.front());
        events.pop_front();
        return value;
    }
    bool publish(std::string_view topic, std::string_view body) override {
        EXPECT_EQ(topic, "$thing/up/signaling/p/d");
        Envelope value;
        EXPECT_EQ(decode_envelope(body, value), DecodeResult::Ok);
        sent.push_back(std::move(value));
        const bool accepted = online && sent.back().method != blocked_method;
        if (accepted) delivered.push_back(sent.back());
        return accepted;
    }
};
// ReleaseMedia仅由测试独立设置连通，不凭配置或SDP推断实际媒体可用。
struct ReleaseMedia final : PumpedVoiceMediaPort {
    bool connected = false;
    bool capture = false;
    unsigned configurations = 0;
    bool configure(const VoiceMedia&) override { ++configurations; return true; }
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
// ReleaseAuthority只认可原candidate完整组；关闭后只清理原租约，不授新控制权。
struct ReleaseAuthority final : FocusAuthority {
    bool closing = false;
    mutable unsigned validations = 0;  // 只观察原valid调用次数，界面读取不得触发额外权限检查。
    std::string allowed_focus = "candidate-focus"; // 当前真实准备对应的原租约，不授旧focus下一代。
    std::optional<FocusPermit> authorize(const FocusCommand& command, std::uint64_t now,
                                         std::uint64_t) override {
        if (now >= 10000 || command.envelope.session != "candidate" ||
            command.focus_id != allowed_focus || command.resources.size() != 2 ||
            (closing && command.operation != "release"))
            return {};
        return FocusPermit{command, 10000, 1};
    }
    bool valid(const FocusPermit& permit, std::uint64_t now, std::uint64_t) const override {
        ++validations;
        return now < permit.deadline && permit.scope.envelope.session == "candidate" &&
               permit.scope.focus_id == allowed_focus &&
               (!closing || permit.scope.operation == "release");
    }
    bool closed_release_valid(const FocusPermit& permit, std::uint64_t now,
                              std::uint64_t wall) const override {
        return closing && permit.scope.operation == "release" && valid(permit, now, wall);
    }
};
// ReleaseDriver整组完成由complete显式交付；stop_scope双false本身不递增CAS或创造release回执。
struct ReleaseDriver final : FocusDriver {
    FocusCommand original;
    std::uint64_t current = 0, generation = 0;
    unsigned begins = 0;
    mutable unsigned ownership_checks = 0;  // 只计原owns次数，不改变驱动事实或授权结果。
    bool held = false;
    std::deque<FocusFact> facts;
    std::uint64_t revision() const override { return current; }
    bool begin(const FocusCommand& command, std::uint64_t value) override {
        original = command;
        generation = value;
        ++begins;
        return true;
    }
    std::optional<FocusFact> take_fact(std::string_view session, std::string_view focus) override {
        if (facts.empty() || session != original.envelope.session || focus != original.focus_id)
            return {};
        auto value = facts.front();
        facts.pop_front();
        return value;
    }
    bool owns(const FocusCommand& command) const override {
        ++ownership_checks;
        return held && command.envelope.session == "candidate" &&
               command.focus_id == original.focus_id;
    }
    bool stop_scope(std::string_view, std::string_view, const std::vector<std::string>&) override {
        held = false;
        return true;
    }
    // complete只为原已受理操作提供真实整组结果，不从下行文字推断执行成功。
    void complete() {
        held = original.operation != "release";
        facts.push_back(
            {generation, true, held ? "acquired" : "released", original.resources, ++current});
    }
};
// ReleasePrepare将可信HAL取消与实际释放分离，便于核验运行时收尾期间仍能收到正式release。
struct ReleasePrepare final : EndpointFocusPreparePort {
    ReleaseAuthority& authority;
    ReleaseDriver& driver;
    bool released = true;
    bool recovering = false;
    unsigned cancels = 0;
    ReleasePrepare(ReleaseAuthority& value, ReleaseDriver& actual) : authority(value), driver(actual) {}
    EndpointFocusPreparation prepare(const AIEndpointFocusScope& scope, const FocusCommand& command,
                                     std::uint64_t, std::uint64_t) override {
        if (recovering) {
            if (scope.endpoint_epoch != 5 || scope.capability_revision != 1 ||
                command.focus_id == "candidate-focus" || !control_idle_observation())
                return EndpointFocusPreparation::Failed;
            authority.allowed_focus = command.focus_id;
            authority.closing = recovering = false;
        }
        return EndpointFocusPreparation::Ready;
    }
    void cancel() override {
        ++cancels;
        authority.closing = true;
    }
    bool settled() const override { return released; }
    bool control_recovery_supported() const override { return true; } // 独立后续回归仍须实际关闭事实。
    bool suspend_control(const AIEndpointFocusScope& scope, std::uint64_t, std::uint64_t) override {
        if (scope.session_id != "candidate" || scope.endpoint_epoch != 4 ||
            scope.capability_revision != 1 || !driver.held) return false;
        recovering = true;
        cancel();
        driver.stop_scope("candidate", "candidate-focus", {"microphone", "speaker"});
        return true;
    }
    std::optional<FocusIdleObservation> control_idle_observation() const override {
        if (!recovering || !released || driver.held || !driver.facts.empty() ||
            driver.original.operation != "release") return {};
        return FocusIdleObservation{{"microphone", "speaker"}, driver.current};
    }
};
// release_capability仅声明已装配的候选HAL；不自动声明控制重连或其他资源。
VoiceCapability release_capability(bool control = false) {
    VoiceCapability value;
    value.audio = {16000, 1, 20};
    value.modes = {true, true, true};
    value.ai_endpoint_handoff = true;
    value.session_sync = value.ai_endpoint_control_sync = control;
    return value;
}
// RuntimeEndpointReleaseTest从原MQTT事件队列完整建立候选，不跳过焦点引擎或运行时分派。
class RuntimeEndpointReleaseTest : public testing::Test {
    protected:
    ReleaseNetwork network;
    ReleaseMedia media;
    ReleaseAuthority authority;
    ReleaseDriver driver;
    ReleasePrepare prepare{authority, driver};
    FocusEndpoint focus{network,
                        driver,
                        authority,
                        {"microphone", "speaker"},
                        "$thing/up/signaling/p/d",
                        "$thing/down/signaling/p/d",
                        20000};
    const bool control; // 夹具明确选择新目标恢复，不给原三个旧候选用例隐式升级。
    VoiceRuntime runtime;
    explicit RuntimeEndpointReleaseTest(bool enabled = false)
        : control(enabled), runtime(network, media, "p/d", release_capability(enabled),
                                    "instance", nullptr, nullptr, &focus) {}
    std::uint64_t now = 1;
    // drive只推进独立可控时钟，既不sleep也不替驱动生成任何完成事实。
    void drive() {
        ++now;
        runtime.poll(now, 1000 + now);
    }
    void body(std::string value) {
        network.events.push_back(
            {MqttEvent::Kind::Message, 0, 0, "$thing/down/signaling/p/d", std::move(value)});
        drive();
    }
    // ack必须关联实际上行原token和原method，不能伪造另一请求已完成。
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
    // finish为false仅停在原媒体已确认但尚未commit的Held候选；committed_revision为平台原CAS水位。
    void active(bool finish = true, std::uint64_t committed_revision = 2) {
        ASSERT_EQ(runtime.set_endpoint_focus_prepare(prepare), Result::Applied);
        ASSERT_EQ(runtime.start_listening({}, now), Result::Applied);
        network.events.push_back({MqttEvent::Kind::Connected, 0, 0, {}, {}});
        drive();
        for (int mid = 10; mid < 12; ++mid)
            network.events.push_back({MqttEvent::Kind::Subscribed, mid, 0, {}, {}});
        drive();
        ASSERT_EQ(network.sent.back().method, "capabilityReport");
        ack(R"({"acceptedRevision":1})");
        body(
            R"({"msgToken":"invite","method":"sessionInvite","sessionId":"candidate","eventSeq":1,"params":{"purpose":"aiConversation","admission":"accept","expiresAt":31000,"handoff":{"sourceSessionId":"old-source","handoffId":"move"},"selected":{"transport":"udp","interactionMode":"fullDuplex","audioParams":{"format":"opus","sampleRate":16000,"channels":1,"frameDuration":20}},"tracks":[{"trackId":"mic-1","kind":"audio","sourceId":"mic","direction":"sendonly","codec":"opus","state":"requested"},{"trackId":"speaker-1","kind":"audio","sourceId":"speaker","direction":"recvonly","codec":"opus","state":"requested"}],"media":{"udp":{"server":"127.0.0.1","port":12345,"key":"000102030405060708090a0b0c0d0e0f","nonce":"01000000112233445566778800000000"}}}})");
        ASSERT_EQ(runtime.accept_endpoint_invite(now, 1000 + now), Result::Applied);
        ack(R"({"state":"negotiating"})");
        media.connected = true;
        drive();
        ASSERT_EQ(network.sent.back().method, "mediaState");
        ack(R"({"state":"connected","mediaRevision":1})");
        if (!finish)
            return;
        body(
            R"({"msgToken":"committed","method":"endpointState","sessionId":"old-source","eventSeq":42,"data":{"state":"committed","revision":)" +
            std::to_string(committed_revision) +
            R"(,"handoffId":"move","candidateSessionId":"candidate"}})");
        body(control ?
            R"({"msgToken":"established","method":"sessionEstablished","sessionId":"candidate","eventSeq":2,"data":{"state":"established","mediaRevision":1,"endpointEpoch":4,"capabilityRevision":1}})" :
            R"({"msgToken":"established","method":"sessionEstablished","sessionId":"candidate","eventSeq":2,"data":{"state":"established","mediaRevision":1}})");
        body(
            R"({"msgToken":"acquire","method":"focusControl","sessionId":"candidate","eventSeq":3,"params":{"operation":"acquire","expectedRevision":0,"focusId":"candidate-focus","resources":["microphone","speaker"],"priority":"conversation","resumePolicy":"explicit","expiresAt":20000}})");
        drive();
        ASSERT_EQ(driver.begins, 1U);
        driver.complete();
        drive();
        ASSERT_EQ(runtime.voice().endpoint_phase(), AIEndpointPhase::Active);
        prepare.released = false; // 启动前空HAL已释放；取得后才注入异步实际收尾等待。
    }
};
TEST_F(RuntimeEndpointReleaseTest,
       TransferredCandidateConsumesOnlyOriginalCompleteReleaseWhileDraining) {
    ASSERT_NO_FATAL_FAILURE(active());
    const auto original_disconnects = network.disconnects;
    body(
        R"({"msgToken":"source-ended","method":"sessionEnded","sessionId":"candidate","eventSeq":4,"data":{"state":"ended","reason":"transferred"}})");
    ASSERT_EQ(runtime.state(), RuntimeState::Draining);
    ASSERT_GT(prepare.cancels, 0U);
    EXPECT_FALSE(driver.held);
    body(
        R"({"msgToken":"wrong-leg","method":"focusControl","sessionId":"other","eventSeq":5,"params":{"operation":"release","expectedRevision":1,"focusId":"candidate-focus","resources":["microphone","speaker"],"priority":"conversation","resumePolicy":"explicit"}})");
    EXPECT_EQ(driver.begins, 1U);
    body(
        R"({"msgToken":"reacquire","method":"focusControl","sessionId":"candidate","eventSeq":5,"params":{"operation":"acquire","expectedRevision":1,"focusId":"candidate-focus","resources":["microphone","speaker"],"priority":"conversation","resumePolicy":"explicit","expiresAt":20000}})");
    EXPECT_EQ(driver.begins, 1U);
    body(
        R"({"msgToken":"partial","method":"focusControl","sessionId":"candidate","eventSeq":5,"params":{"operation":"release","expectedRevision":1,"focusId":"candidate-focus","resources":["speaker"],"priority":"conversation","resumePolicy":"explicit"}})");
    EXPECT_EQ(driver.begins, 1U);
    body(
        R"({"msgToken":"release","method":"focusControl","sessionId":"candidate","eventSeq":5,"params":{"operation":"release","expectedRevision":1,"focusId":"candidate-focus","resources":["microphone","speaker"],"priority":"conversation","resumePolicy":"explicit"}})");
    ASSERT_EQ(driver.begins, 2U); // RED必须是原运行时丢弃release，不是手造驱动完成。
    EXPECT_EQ(network.disconnects, original_disconnects);
    driver.complete();
    drive();
    const auto released =
        std::find_if(network.sent.begin(), network.sent.end(), [](const auto& value) {
            return value.method == "focusState" && value.token == "release" && value.code == 200;
        });
    ASSERT_NE(released, network.sent.end());
    EXPECT_NE(released->payload.find("released"), std::string::npos);
    EXPECT_EQ(runtime.state(), RuntimeState::Draining);
    prepare.released = true;
    drive();
    EXPECT_EQ(runtime.state(), RuntimeState::Idle);
    EXPECT_EQ(network.disconnects, original_disconnects + 1);
}
TEST_F(RuntimeEndpointReleaseTest, ActualStoppedWithoutFormalReleaseDoesNotCloseOriginalMqtt) {
    ASSERT_NO_FATAL_FAILURE(active());
    const auto original_disconnects = network.disconnects;
    prepare.released = true;
    body(R"({"msgToken":"source-ended","method":"sessionEnded","sessionId":"candidate","eventSeq":4,"data":{"state":"ended","reason":"transferred"}})");
    EXPECT_EQ(runtime.state(), RuntimeState::Draining);
    EXPECT_EQ(network.disconnects, original_disconnects);
    EXPECT_FALSE(driver.held);
    drive();
    EXPECT_EQ(runtime.state(), RuntimeState::Draining);
    EXPECT_EQ(network.disconnects, original_disconnects);
}
TEST_F(RuntimeEndpointReleaseTest, ActualAcquiredCandidateCanPrepareAnotherHopWithoutControlSync) {
    ASSERT_NO_FATAL_FAILURE(active(true, 4));
    ++now;
    EXPECT_EQ(runtime.prepare_endpoint_source("device:p:next", now, 1000 + now), Result::Applied);
    ASSERT_FALSE(network.sent.empty());
    EXPECT_EQ(network.sent.back().method, "endpointControl");
    EXPECT_EQ(network.sent.back().session, "candidate");
    std::string encoded;  // 验证实际提交原字节，不以本地receipt投影替代expectedRevision。
    EndpointCommand command;
    ASSERT_EQ(encode_envelope(network.sent.back(), encoded), DecodeResult::Ok);
    ASSERT_EQ(decode_endpoint_control(WireRoute::Signaling, encoded, command), WireResult::Ok);
    EXPECT_EQ(command.expected_revision, 4U);
    EXPECT_TRUE(driver.held);
    EXPECT_EQ(runtime.voice().endpoint_phase(), AIEndpointPhase::Active);
}
// 候选只继承完整原提交，旧source水位或错误归属不能写入新公开腿的CAS。
TEST_F(RuntimeEndpointReleaseTest, CandidateRevisionIgnoresWrongBindingAndLateSourceFacts) {
    ASSERT_NO_FATAL_FAILURE(active(true, 4));
    body(
        R"({"msgToken":"wrong-source","method":"endpointState","sessionId":"other-source","eventSeq":43,"data":{"state":"committed","revision":99,"handoffId":"move","candidateSessionId":"candidate"}})");
    body(
        R"({"msgToken":"wrong-handoff","method":"endpointState","sessionId":"old-source","eventSeq":43,"data":{"state":"committed","revision":99,"handoffId":"other-move","candidateSessionId":"candidate"}})");
    body(
        R"({"msgToken":"wrong-candidate","method":"endpointState","sessionId":"old-source","eventSeq":43,"data":{"state":"committed","revision":99,"handoffId":"move","candidateSessionId":"other-candidate"}})");
    body(
        R"({"msgToken":"late-source","method":"endpointState","sessionId":"old-source","eventSeq":41,"data":{"state":"committed","revision":99,"handoffId":"move","candidateSessionId":"candidate"}})");
    const auto begins = driver.begins;
    ++now;
    ASSERT_EQ(runtime.prepare_endpoint_source("device:p:next", now, 1000 + now), Result::Applied);
    ASSERT_NE(runtime.endpoint_source_receipt(), nullptr);
    EXPECT_EQ(runtime.endpoint_source_receipt()->revision, 4U);
    EXPECT_EQ(driver.begins, begins);
    EXPECT_TRUE(driver.held);
}
// 旧committed仅为初始出处；同公开腿原prepare/abort的实际更高ACK绝不能被首次水位覆盖。
TEST_F(RuntimeEndpointReleaseTest,
       CandidateNextIntentRetainsHigherRevisionAndOriginalRequestFacts) {
    ASSERT_NO_FATAL_FAILURE(active(true, 4));
    ++now;
    ASSERT_EQ(runtime.prepare_endpoint_source("device:p:next", now, 1000 + now), Result::Applied);
    const auto original = *runtime.endpoint_source_receipt();
    ++now;
    EXPECT_EQ(runtime.prepare_endpoint_source("device:p:other", now, 1000 + now), Result::Busy);
    EXPECT_EQ(runtime.endpoint_source_receipt()->token, original.token);
    EXPECT_EQ(runtime.endpoint_source_receipt()->revision, 4U);
    // 两份回复沿实际本机原token/handoff，不借上一次source事件补造原请求应答。
    Envelope reply;
    reply.request = false;
    reply.method = "endpointState";
    reply.token = original.token;
    reply.session = original.session;
    reply.code = 200;
    reply.payload =
        R"({"state":"preparing","revision":5,"handoffId":")" + original.handoff + R"("})";
    std::string encoded;
    ASSERT_EQ(encode_envelope(reply, encoded), DecodeResult::Ok);
    body(encoded);
    EXPECT_EQ(runtime.endpoint_source_receipt()->revision, 5U);
    ++now;
    ASSERT_EQ(runtime.abort_endpoint_source(now, 1000 + now), Result::Applied);
    const auto abort = *runtime.endpoint_source_receipt();
    reply.token = abort.token;
    reply.payload = R"({"state":"aborted","revision":6,"handoffId":")" + abort.handoff +
                    R"(","reason":"userCancelled"})";
    ASSERT_EQ(encode_envelope(reply, encoded), DecodeResult::Ok);
    body(encoded);
    EXPECT_EQ(runtime.endpoint_source_receipt()->revision, 6U);
    body(
        R"({"msgToken":"later-old-commit","method":"endpointState","sessionId":"old-source","eventSeq":43,"data":{"state":"committed","revision":4,"handoffId":"move","candidateSessionId":"candidate"}})");
    ++now;
    ASSERT_EQ(runtime.prepare_endpoint_source("device:p:other", now, 1000 + now), Result::Applied);
    ASSERT_EQ(encode_envelope(network.sent.back(), encoded), DecodeResult::Ok);
    EndpointCommand command;
    ASSERT_EQ(decode_endpoint_control(WireRoute::Signaling, encoded, command), WireResult::Ok);
    EXPECT_EQ(command.expected_revision, 6U);
    EXPECT_NE(runtime.endpoint_source_receipt()->token, original.token);
    EXPECT_EQ(driver.begins, 1U);
    EXPECT_TRUE(driver.held);
}
TEST_F(RuntimeEndpointReleaseTest, HeldCandidateDoesNotBecomeSourceBeforeActualAcquisition) {
    ASSERT_NO_FATAL_FAILURE(active(false));
    const auto sent = network.sent.size();
    ++now;
    EXPECT_EQ(runtime.prepare_endpoint_source("device:p:next", now, 1000 + now), Result::Stale);
    EXPECT_EQ(network.sent.size(), sent);
    EXPECT_EQ(driver.begins, 0U);
    EXPECT_FALSE(driver.held);
}
TEST_F(RuntimeEndpointReleaseTest, ZeroCommittedRevisionCannotSeedAnotherHop) {
    ASSERT_NO_FATAL_FAILURE(active(true, 0));
    const auto sent = network.sent.size();
    ++now;
    EXPECT_EQ(runtime.prepare_endpoint_source("device:p:next", now, 1000 + now), Result::Stale);
    EXPECT_EQ(network.sent.size(), sent);
    EXPECT_TRUE(runtime.endpoint_source_receipt()->session.empty());
    EXPECT_TRUE(driver.held);
}
TEST_F(RuntimeEndpointReleaseTest, ClosedCandidateCannotReuseItsCommittedSourceRevision) {
    ASSERT_NO_FATAL_FAILURE(active(true, 4));
    ++now;
    ASSERT_EQ(runtime.prepare_endpoint_source("device:p:next", now, 1000 + now), Result::Applied);
    runtime.close(++now);
    const auto sent = network.sent.size();
    ++now;
    EXPECT_EQ(runtime.prepare_endpoint_source("device:p:other", now, 1000 + now), Result::Stale);
    EXPECT_EQ(network.sent.size(), sent);
    EXPECT_FALSE(driver.held);
    EXPECT_FALSE(media.capture);
}
TEST_F(RuntimeEndpointReleaseTest,
       SourceIntentProjectionIncludesActualActiveWithoutCheckingAuthorityAgain) {
    ASSERT_NO_FATAL_FAILURE(active());
    ASSERT_FALSE(runtime.voice().control_sync_enabled());
    const auto validations = authority.validations;
    const auto checks = driver.ownership_checks;
    for (unsigned count = 0; count < 32; ++count)
        EXPECT_TRUE(runtime.endpoint_source_intent_available());
    EXPECT_EQ(authority.validations, validations);
    EXPECT_EQ(driver.ownership_checks, checks);
    EXPECT_TRUE(driver.held);
    ++now;
    EXPECT_EQ(runtime.prepare_endpoint_source("device:p:next", now, 1000 + now), Result::Applied);
    EXPECT_EQ(network.sent.back().method, "endpointControl");
}
TEST_F(RuntimeEndpointReleaseTest,
       SourceIntentProjectionRejectsInactiveAndUncommittedHeldCandidate) {
    EXPECT_FALSE(runtime.endpoint_source_intent_available());
    ASSERT_NO_FATAL_FAILURE(active(false));
    EXPECT_NE(runtime.voice().endpoint_phase(), AIEndpointPhase::Active);
    EXPECT_FALSE(runtime.endpoint_source_intent_available());
    EXPECT_EQ(driver.begins, 0U);
    EXPECT_FALSE(driver.held);
}
TEST_F(RuntimeEndpointReleaseTest, SourceIntentProjectionDoesNotGrantEndingOrRepeatScopeChecks) {
    ASSERT_NO_FATAL_FAILURE(active());
    ++now;
    ASSERT_EQ(runtime.end(now), Result::Applied);
    ASSERT_EQ(runtime.voice().state(), VoiceState::Ending);
    const auto validations = authority.validations;
    const auto checks = driver.ownership_checks;
    for (unsigned count = 0; count < 32; ++count)
        EXPECT_FALSE(runtime.endpoint_source_intent_available());
    EXPECT_EQ(authority.validations, validations);
    EXPECT_EQ(driver.ownership_checks, checks);
    EXPECT_FALSE(driver.held);
}
TEST_F(RuntimeEndpointReleaseTest,
       SourceIntentProjectionCannotAuthorizeRevokedScopeAndFailedRuntime) {
    ASSERT_NO_FATAL_FAILURE(active());
    authority.closing = true;  // 原许可实际失效；界面本机状态还未被下一原poll更新。
    const auto validations = authority.validations;
    const auto checks = driver.ownership_checks;
    EXPECT_TRUE(runtime.endpoint_source_intent_available());
    EXPECT_EQ(authority.validations, validations);
    EXPECT_EQ(driver.ownership_checks, checks);
    const auto sent = network.sent.size();
    const auto begins = driver.begins;
    ++now;
    EXPECT_EQ(runtime.prepare_endpoint_source("device:p:next", now, 1000 + now), Result::Stale);
    EXPECT_GT(authority.validations, validations);
    EXPECT_EQ(network.sent.size(), sent);
    EXPECT_EQ(driver.begins, begins);
    drive();
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
    EXPECT_FALSE(runtime.endpoint_source_intent_available());
    EXPECT_FALSE(driver.held);
}
// 真实候选流程必须从目标公开腿正式建立字段安装绑定，不向sourceSessionId同步。
class RuntimeTargetControlSyncTest : public RuntimeEndpointReleaseTest {
    protected:
    RuntimeTargetControlSyncTest() : RuntimeEndpointReleaseTest(true) {}
    std::optional<Envelope> retired_start; // 原已确认输入请求，仅用于验证迟到原ACK不能复活。
    std::optional<Envelope> retired_stop; // 原本人停止请求，区别离线前已交付与未确认两序。
    // snapshot沿原目标公开腿/原轨，当前新socket能力确认后才恢复控制；不伪造焦点或开麦事实。
    void restored_snapshot(bool user_stop = false, bool stopped_before_loss = false,
                           bool stop_ack_pending = false) {
        ASSERT_NO_FATAL_FAILURE(active());
        ASSERT_EQ(runtime.start_input(++now), Result::Applied);
        retired_start = network.sent.back();
        ack(network.sent.back().payload);
        ASSERT_TRUE(media.capture);
        if (stopped_before_loss) {
            ASSERT_EQ(runtime.stop_input(++now), Result::Applied);
            EXPECT_FALSE(media.capture);
            retired_stop = network.sent.back();
            if (!stop_ack_pending) ack(network.sent.back().payload);
            EXPECT_FALSE(media.capture);
        }
        network.events.push_back({MqttEvent::Kind::Disconnected, 0, 0, {}, {}});
        drive();
        ASSERT_EQ(runtime.state(), RuntimeState::ControlRecovering);
        ASSERT_FALSE(media.capture);
        if (user_stop) runtime.stop_input(++now);
        ASSERT_EQ(runtime.resume_connection({}, ++now), Result::Applied);
        network.events.push_back({MqttEvent::Kind::Connected, 0, 0, {}, {}});
        drive();
        for (int mid = 12; mid < 14; ++mid)
            network.events.push_back({MqttEvent::Kind::Subscribed, mid, 0, {}, {}});
        drive();
        ASSERT_EQ(network.sent.back().method, "capabilityReport");
        EXPECT_EQ(network.sent.back().payload.find("\"baseline\""), std::string::npos);
        ack(R"({"acceptedRevision":1})");
        ASSERT_EQ(network.sent.back().method, "sessionSync");
        EXPECT_EQ(network.sent.back().session, "candidate");
        EXPECT_NE(network.sent.back().payload.find("\"endpointEpoch\":4"), std::string::npos);
        Envelope snapshot;
        snapshot.request = false;
        snapshot.token = network.sent.back().token;
        snapshot.method = "sessionSnapshot";
        snapshot.session = "candidate";
        snapshot.code = 200;
        snapshot.event_sequence = 9;
        snapshot.payload = R"({"state":"established","revision":8,"eventWatermark":9,"messageWatermark":0,"endpointEpoch":5,"capabilityRevision":1,"mediaRevision":1,"expiresAt":20000,"resumeAllowed":true,"participants":[],"tracks":[{"trackId":"mic-1","kind":"audio","sourceId":"mic","direction":"sendonly","codec":"opus","state":"paused"},{"trackId":"speaker-1","kind":"audio","sourceId":"speaker","direction":"recvonly","codec":"opus","state":"paused"}],"controls":{"call":null,"focus":null,"floor":null,"recordings":[],"playbacks":[],"processors":[],"broadcasts":[],"handoffs":[],"mediaPlan":{"revision":1,"tracks":[{"trackId":"mic-1","kind":"audio","sourceId":"mic","direction":"sendonly","codec":"opus","state":"paused"},{"trackId":"speaker-1","kind":"audio","sourceId":"speaker","direction":"recvonly","codec":"opus","state":"paused"}]},"ai":{"inputId":"retired-input","inputState":"paused","activeRespIds":[]}}})";
        std::string encoded;
        ASSERT_EQ(encode_envelope(snapshot, encoded), DecodeResult::Ok);
        body(encoded);
        ASSERT_EQ(runtime.state(), RuntimeState::Running);
        ASSERT_EQ(runtime.voice().control_endpoint_epoch(), 5U);
        EXPECT_EQ(runtime.voice().endpoint_phase(), AIEndpointPhase::AwaitingFocus);
        EXPECT_FALSE(media.capture);
        EXPECT_EQ(media.configurations, 1U);
        EXPECT_EQ(network.sent.back().method, "sessionSync");
    }
    // finish_rebind独立交付原release、真实基线和新整组完成；不提交本人新的audioStart。
    void finish_rebind() {
        body(R"({"msgToken":"recovery-release","method":"focusControl","sessionId":"candidate","eventSeq":10,"params":{"operation":"release","expectedRevision":1,"focusId":"candidate-focus","resources":["microphone","speaker"],"priority":"conversation","resumePolicy":"explicit"}})");
        ASSERT_EQ(driver.begins, 2U);
        driver.complete();
        prepare.released = true;
        drive();
        ASSERT_EQ(network.sent.back().method, "capabilityReport");
        ack(R"({"acceptedRevision":1})");
        body(R"({"msgToken":"fresh-acquire","method":"focusControl","sessionId":"candidate","eventSeq":11,"params":{"operation":"acquire","expectedRevision":2,"focusId":"new-focus","resources":["microphone","speaker"],"priority":"conversation","resumePolicy":"explicit","expiresAt":19000}})");
        ASSERT_EQ(driver.begins, 3U);
        driver.complete();
        drive();
        ASSERT_EQ(runtime.voice().endpoint_phase(), AIEndpointPhase::Active);
        EXPECT_FALSE(media.capture);
    }
    // acknowledge_retired_stop重投原停止ACK，不换token或把旧停止补成新连接权限。
    void acknowledge_retired_stop() {
        ASSERT_TRUE(retired_stop);
        Envelope reply;
        reply.request = false;
        reply.method = "requestAck";
        reply.token = retired_stop->token;
        reply.session = retired_stop->session;
        reply.code = 200;
        reply.payload = "{\"requestMethod\":\"audioStop\",\"result\":" + retired_stop->payload + "}";
        std::string encoded;
        ASSERT_EQ(encode_envelope(reply, encoded), DecodeResult::Ok);
        body(encoded);
    }
};
// 正常已接入但从未恢复时，公共poll不得借逻辑候选或完整组事实发送新的恢复基线。
TEST_F(RuntimeTargetControlSyncTest, NoRecoveryStartDoesNotPublishControlBaseline) {
    ASSERT_NO_FATAL_FAILURE(active());
    ASSERT_EQ(runtime.state(), RuntimeState::Running);
    ASSERT_EQ(runtime.voice().endpoint_phase(), AIEndpointPhase::Active);
    ASSERT_TRUE(runtime.voice().control_sync_enabled());
    ASSERT_TRUE(runtime.voice().control_original_session_preserved());
    ASSERT_TRUE(driver.held);
    const auto published = network.sent.size();  // 原所有上行已实际记录，不能以capability关闭冒当拒绝。
    now += 200;
    runtime.poll(now, 1000 + now);
    EXPECT_EQ(network.sent.size(), published);
    EXPECT_EQ(runtime.state(), RuntimeState::Running);
    EXPECT_EQ(runtime.voice().endpoint_phase(), AIEndpointPhase::Active);
    EXPECT_EQ(runtime.voice().session_id(), "candidate");
    EXPECT_TRUE(driver.held);
    EXPECT_FALSE(media.capture);
}
TEST_F(RuntimeTargetControlSyncTest, EstablishedTargetInstallsItsOwnPublicControlBinding) {
    ASSERT_NO_FATAL_FAILURE(active());
    EXPECT_TRUE(runtime.voice().control_sync_enabled());
    EXPECT_EQ(runtime.voice().control_endpoint_epoch(), 4U);
    EXPECT_TRUE(runtime.voice().control_original_session_preserved());
    EXPECT_EQ(runtime.voice().session_id(), "candidate");
}
TEST_F(RuntimeTargetControlSyncTest,
       DisconnectPreservesUnknownInheritedIntentWithoutGrantingNewHop) {
    ASSERT_NO_FATAL_FAILURE(active(true, 4));
    ++now;
    ASSERT_EQ(runtime.prepare_endpoint_source("device:p:next", now, 1000 + now), Result::Applied);
    const auto original = *runtime.endpoint_source_receipt();
    network.events.push_back({MqttEvent::Kind::Disconnected, 0, 0, {}, {}});
    drive();
    ASSERT_EQ(runtime.state(), RuntimeState::ControlRecovering);
    EXPECT_EQ(runtime.endpoint_source_receipt()->revision, 4U);
    EXPECT_EQ(runtime.endpoint_source_receipt()->token, original.token);
    EXPECT_TRUE(runtime.endpoint_source_receipt()->unknown);
    const auto sent = network.sent.size();
    ++now;
    EXPECT_EQ(runtime.prepare_endpoint_source("device:p:other", now, 1000 + now), Result::Stale);
    EXPECT_EQ(network.sent.size(), sent);
    EXPECT_FALSE(driver.held);
    EXPECT_FALSE(media.capture);
}
TEST_F(RuntimeTargetControlSyncTest,
       SourceIntentProjectionPreservesConfirmedControlAndRejectsRecovery) {
    ASSERT_NO_FATAL_FAILURE(active());
    ASSERT_TRUE(runtime.voice().control_sync_enabled());
    EXPECT_TRUE(runtime.endpoint_source_intent_available());
    network.events.push_back({MqttEvent::Kind::Disconnected, 0, 0, {}, {}});
    drive();
    ASSERT_EQ(runtime.state(), RuntimeState::ControlRecovering);
    EXPECT_FALSE(runtime.endpoint_source_intent_available());
    EXPECT_FALSE(driver.held);
    EXPECT_FALSE(media.capture);
}
// 能力本身不代表HAL装配；没有有限重绑定端口不能开始联网并对外声明该能力。
TEST_F(RuntimeTargetControlSyncTest, UnassembledRecoveryCapabilityCannotStartConnection) {
    const auto before = network.disconnects;
    EXPECT_EQ(runtime.start({}, now), Result::Unsupported);
    EXPECT_EQ(runtime.state(), RuntimeState::Idle);
    EXPECT_EQ(network.disconnects, before);
    EXPECT_TRUE(network.sent.empty());
}
TEST_F(RuntimeTargetControlSyncTest, DisconnectPreservesTargetButActuallyStopsOriginalFullGroup) {
    ASSERT_NO_FATAL_FAILURE(active());
    const auto original = network.disconnects;
    network.events.push_back({MqttEvent::Kind::Disconnected, 0, 0, {}, {}});
    drive();
    ASSERT_EQ(runtime.state(), RuntimeState::ControlRecovering);
    EXPECT_EQ(runtime.voice().session_id(), "candidate");
    EXPECT_EQ(runtime.voice().control_endpoint_epoch(), 4U);
    EXPECT_FALSE(driver.held);
    EXPECT_GT(prepare.cancels, 0U);
    EXPECT_EQ(network.disconnects, original + 1);
    EXPECT_NE(runtime.start_input(++now), Result::Applied);
}
TEST_F(RuntimeTargetControlSyncTest, SnapshotAndUserStopDoNotAutomaticallyResumeInput) {
    ASSERT_NO_FATAL_FAILURE(restored_snapshot(true));
    EXPECT_EQ(runtime.start_input(++now), Result::Stale);
    EXPECT_FALSE(media.capture);
    EXPECT_EQ(runtime.voice().session_id(), "candidate");
}
// 本人离线前已停与恢复期间已停都不能被新整组许可或原输入ACK自动开麦。
TEST_F(RuntimeTargetControlSyncTest, StoppedInputRemainsActuallyStoppedAfterFreshFocusAndLateOriginalAck) {
    ASSERT_NO_FATAL_FAILURE(restored_snapshot(false, true));
    ASSERT_NO_FATAL_FAILURE(finish_rebind());
    ASSERT_TRUE(retired_start);
    Envelope acknowledgement;
    acknowledgement.request = false;
    acknowledgement.token = retired_start->token;
    acknowledgement.session = retired_start->session;
    acknowledgement.method = "requestAck";
    acknowledgement.code = 200;
    acknowledgement.payload = "{\"requestMethod\":\"audioStart\",\"result\":" + retired_start->payload + "}";
    std::string encoded;
    ASSERT_EQ(encode_envelope(acknowledgement, encoded), DecodeResult::Ok);
    body(encoded);
    EXPECT_EQ(runtime.state(), RuntimeState::Running);
    EXPECT_EQ(runtime.voice().endpoint_phase(), AIEndpointPhase::Active);
    EXPECT_FALSE(media.capture);
    EXPECT_FALSE(runtime.voice().capturing());
    EXPECT_EQ(driver.current, 3U);
}
// 本人停止未确认即失联时，旧ACK不能替代原正式release或新全组取得。
TEST_F(RuntimeTargetControlSyncTest, PendingStopAckBeforeReleaseCannotPublishBaselineOrRestartInput) {
    ASSERT_NO_FATAL_FAILURE(restored_snapshot(false, true, true));
    ASSERT_NO_FATAL_FAILURE(acknowledge_retired_stop());
    EXPECT_EQ(runtime.state(), RuntimeState::Running);
    EXPECT_FALSE(media.capture);
    EXPECT_EQ(network.sent.back().method, "sessionSync");
    ASSERT_NO_FATAL_FAILURE(finish_rebind());
    EXPECT_FALSE(runtime.voice().capturing());
}
// 已取得新完整组后原未确认stop才到达，仍只保留本人停止意图，不重开或再次发布基线。
TEST_F(RuntimeTargetControlSyncTest, PendingStopAckAfterFreshFocusKeepsStoppedAndDoesNotRebindAgain) {
    ASSERT_NO_FATAL_FAILURE(restored_snapshot(false, true, true));
    ASSERT_NO_FATAL_FAILURE(finish_rebind());
    const auto sent = network.sent.size();
    ASSERT_NO_FATAL_FAILURE(acknowledge_retired_stop());
    EXPECT_EQ(runtime.state(), RuntimeState::Running);
    EXPECT_EQ(runtime.voice().endpoint_phase(), AIEndpointPhase::Active);
    EXPECT_EQ(network.sent.size(), sent);
    EXPECT_FALSE(media.capture);
    EXPECT_FALSE(runtime.voice().capturing());
}
// 完整release实际完成仍须原focusState发布成功；只有物理false不能发第二baseline。
TEST_F(RuntimeTargetControlSyncTest, StoppedReleaseReceiptBackpressureDelaysBaselineUntilOriginalReceiptDrained) {
    ASSERT_NO_FATAL_FAILURE(restored_snapshot(false, true));
    network.blocked_method = "focusState";
    body(R"({"msgToken":"recovery-release","method":"focusControl","sessionId":"candidate","eventSeq":10,"params":{"operation":"release","expectedRevision":1,"focusId":"candidate-focus","resources":["microphone","speaker"],"priority":"conversation","resumePolicy":"explicit"}})");
    ASSERT_EQ(driver.begins, 2U);
    driver.complete();
    prepare.released = true;
    drive();
    drive();
    EXPECT_FALSE(media.capture);
    EXPECT_TRUE(std::none_of(network.sent.begin(), network.sent.end(), [](const auto& value) {
        return value.method == "capabilityReport" && value.payload.find("\"baseline\"") != std::string::npos;
    }));
    EXPECT_TRUE(std::none_of(network.delivered.begin(), network.delivered.end(), [](const auto& value) {
        return value.method == "focusState" && value.token == "recovery-release";
    }));
    network.blocked_method.clear();
    drive();
    ASSERT_EQ(network.sent.back().method, "capabilityReport");
    EXPECT_NE(network.sent.back().payload.find("\"revision\":2"), std::string::npos);
    const auto received = std::find_if(network.delivered.begin(), network.delivered.end(), [](const auto& value) {
        return value.method == "focusState" && value.token == "recovery-release" && value.code == 200;
    });
    ASSERT_NE(received, network.delivered.end());
    EXPECT_NE(received->payload.find("released"), std::string::npos);
    EXPECT_EQ(std::count_if(network.delivered.begin(), network.delivered.end(), [](const auto& value) {
        return value.method == "capabilityReport" && value.payload.find("\"baseline\"") != std::string::npos;
    }), 1);
    EXPECT_FALSE(media.capture);
    EXPECT_EQ(runtime.voice().endpoint_phase(), AIEndpointPhase::AwaitingFocus);
}
TEST_F(RuntimeTargetControlSyncTest, UserStopDuringRecoveryStaysStoppedThroughFreshCompleteFocus) {
    ASSERT_NO_FATAL_FAILURE(restored_snapshot(true));
    ASSERT_NO_FATAL_FAILURE(finish_rebind());
    EXPECT_FALSE(media.capture);
    EXPECT_FALSE(runtime.voice().capturing());
    EXPECT_EQ(media.configurations, 1U);
}
// 原快照不代替正式release，也不能令新scope收到完整资源取得；等待仍固定首次两秒。
TEST_F(RuntimeTargetControlSyncTest, SnapshotWithoutFormalReleaseCannotAcquireAndExpires) {
    ASSERT_NO_FATAL_FAILURE(restored_snapshot());
    body(R"({"msgToken":"too-early","method":"focusControl","sessionId":"candidate","eventSeq":11,"params":{"operation":"acquire","expectedRevision":1,"focusId":"new-focus","resources":["microphone","speaker"],"priority":"conversation","resumePolicy":"explicit","expiresAt":19000}})");
    EXPECT_EQ(driver.begins, 1U);
    EXPECT_FALSE(media.capture);
    now += 2000;
    drive();
    EXPECT_EQ(runtime.state(), RuntimeState::Failed); // 固定准备超时是失败，不是正常终态收尾。
    EXPECT_FALSE(media.capture);
    EXPECT_EQ(driver.begins, 1U);
}
// 平台基线应答与新原子组操作独立；收到acquire可有界缓存，但不能提前执行HAL。
TEST_F(RuntimeTargetControlSyncTest, FreshAcquireWaitsForMatchingBaselineAcknowledgement) {
    ASSERT_NO_FATAL_FAILURE(restored_snapshot());
    body(R"({"msgToken":"recovery-release","method":"focusControl","sessionId":"candidate","eventSeq":10,"params":{"operation":"release","expectedRevision":1,"focusId":"candidate-focus","resources":["microphone","speaker"],"priority":"conversation","resumePolicy":"explicit"}})");
    ASSERT_EQ(driver.begins, 2U);
    driver.complete();
    prepare.released = true;
    drive();
    ASSERT_EQ(network.sent.back().method, "capabilityReport");
    body(R"({"msgToken":"fresh-acquire","method":"focusControl","sessionId":"candidate","eventSeq":11,"params":{"operation":"acquire","expectedRevision":2,"focusId":"new-focus","resources":["microphone","speaker"],"priority":"conversation","resumePolicy":"explicit","expiresAt":19000}})");
    EXPECT_EQ(driver.begins, 2U);
    EXPECT_FALSE(media.capture);
    ASSERT_EQ(network.sent.back().method, "capabilityReport");
    ack(R"({"acceptedRevision":1})");
    EXPECT_EQ(runtime.state(), RuntimeState::Running);
    EXPECT_EQ(runtime.voice().state(), VoiceState::Established);
    drive(); // ACK后的缓存准入发生在本轮Focus.poll之后，真实驱动由下一次事件泵推进。
    EXPECT_EQ(driver.begins, 3U);
    EXPECT_EQ(runtime.voice().endpoint_phase(), AIEndpointPhase::AwaitingFocus);
    EXPECT_FALSE(media.capture);
}
TEST_F(RuntimeTargetControlSyncTest, FormalReleaseBaselineFreshAcquireAndNewStartAreIndependentBarriers) {
    ASSERT_NO_FATAL_FAILURE(restored_snapshot());
    EXPECT_EQ(runtime.start_input(++now), Result::Stale);
    body(R"({"msgToken":"recovery-release","method":"focusControl","sessionId":"candidate","eventSeq":10,"params":{"operation":"release","expectedRevision":1,"focusId":"candidate-focus","resources":["microphone","speaker"],"priority":"conversation","resumePolicy":"explicit"}})");
    ASSERT_EQ(driver.begins, 2U);
    EXPECT_EQ(network.sent.back().method, "sessionSync");
    driver.complete();
    prepare.released = true;
    drive();
    ASSERT_EQ(network.sent.back().method, "capabilityReport");
    EXPECT_NE(network.sent.back().payload.find("\"baseline\""), std::string::npos);
    EXPECT_NE(network.sent.back().payload.find("\"revision\":2"), std::string::npos);
    const auto released = std::find_if(network.sent.begin(), network.sent.end(), [](const auto& value) {
        return value.method == "focusState" && value.token == "recovery-release" && value.code == 200;
    });
    ASSERT_NE(released, network.sent.end());
    ack(R"({"acceptedRevision":1})");
    EXPECT_EQ(runtime.voice().state(), VoiceState::Established);
    EXPECT_EQ(runtime.voice().endpoint_phase(), AIEndpointPhase::AwaitingFocus);
    body(R"({"msgToken":"fresh-acquire","method":"focusControl","sessionId":"candidate","eventSeq":11,"params":{"operation":"acquire","expectedRevision":2,"focusId":"new-focus","resources":["microphone","speaker"],"priority":"conversation","resumePolicy":"explicit","expiresAt":19000}})");
    ASSERT_EQ(driver.begins, 3U);
    EXPECT_FALSE(media.capture);
    EXPECT_EQ(runtime.voice().endpoint_phase(), AIEndpointPhase::AwaitingFocus);
    driver.complete();
    drive();
    ASSERT_EQ(runtime.voice().endpoint_phase(), AIEndpointPhase::Active);
    EXPECT_FALSE(media.capture);
    ASSERT_EQ(runtime.start_input(++now), Result::Applied);
    EXPECT_FALSE(media.capture);
    ack(network.sent.back().payload);
    EXPECT_TRUE(media.capture);
    EXPECT_EQ(driver.current, 3U);
    EXPECT_EQ(media.configurations, 1U);
}
// BackpressureConsumesFastBudgetWithoutRetiringOriginal
// 背压也消耗12次专属预算，之后原ACK仍可收敛新完整组。
TEST_F(RuntimeTargetControlSyncTest, BackpressureConsumesFastBudgetWithoutRetiringOriginal) {
    ASSERT_NO_FATAL_FAILURE(restored_snapshot());
    body(
        R"({"msgToken":"recovery-release","method":"focusControl","sessionId":"candidate","eventSeq":10,"params":{"operation":"release","expectedRevision":1,"focusId":"candidate-focus","resources":["microphone","speaker"],"priority":"conversation","resumePolicy":"explicit"}})");
    driver.complete();
    prepare.released = true;
    drive();
    ASSERT_EQ(network.sent.back().method, "capabilityReport");
    const auto started = now;
    const auto count = network.sent.size();
    network.online = false;
    for (unsigned i = 1; i <= 12; ++i) {
        now = started + 200 * i;
        runtime.poll(now, 1000 + now);
        ASSERT_EQ(network.sent.size(), count + i);
    }
    now = started + 2600;
    runtime.poll(now, 1000 + now);
    EXPECT_EQ(network.sent.size(), count + 12U);
    network.online = true;
    ack(R"({"acceptedRevision":1})");
    EXPECT_EQ(runtime.state(), RuntimeState::Running);
    body(
        R"({"msgToken":"fresh-acquire","method":"focusControl","sessionId":"candidate","eventSeq":11,"params":{"operation":"acquire","expectedRevision":2,"focusId":"new-focus","resources":["microphone","speaker"],"priority":"conversation","resumePolicy":"explicit","expiresAt":19000}})");
    ASSERT_EQ(driver.begins, 3U);
    driver.complete();
    drive();
    EXPECT_EQ(runtime.voice().endpoint_phase(), AIEndpointPhase::Active);
    EXPECT_FALSE(media.capture);
}
// FastBudgetDoesNotRetireOriginalRequest
// 12次预算耗尽不退休原请求，迟到原ACK与真实驱动取得仍能恢复。
TEST_F(RuntimeTargetControlSyncTest, FastBudgetDoesNotRetireOriginalRequest) {
    ASSERT_NO_FATAL_FAILURE(restored_snapshot());
    body(
        R"({"msgToken":"recovery-release","method":"focusControl","sessionId":"candidate","eventSeq":10,"params":{"operation":"release","expectedRevision":1,"focusId":"candidate-focus","resources":["microphone","speaker"],"priority":"conversation","resumePolicy":"explicit"}})");
    driver.complete();
    prepare.released = true;
    drive();
    ASSERT_EQ(network.sent.back().method, "capabilityReport");
    const auto started = now;
    const auto count = network.sent.size();
    for (unsigned i = 1; i <= 12; ++i) {
        now = started + 200 * i;
        runtime.poll(now, 1000 + now);
        ASSERT_EQ(network.sent.size(), count + i);
    }
    now = started + 2600;
    runtime.poll(now, 1000 + now);
    ASSERT_EQ(network.sent.size(), count + 12U);
    EXPECT_EQ(runtime.state(), RuntimeState::Running);
    ack(R"({"acceptedRevision":1})");
    EXPECT_EQ(runtime.state(), RuntimeState::Running);
    EXPECT_EQ(runtime.voice().endpoint_phase(), AIEndpointPhase::AwaitingFocus);
    body(
        R"({"msgToken":"fresh-acquire","method":"focusControl","sessionId":"candidate","eventSeq":11,"params":{"operation":"acquire","expectedRevision":2,"focusId":"new-focus","resources":["microphone","speaker"],"priority":"conversation","resumePolicy":"explicit","expiresAt":19000}})");
    ASSERT_EQ(driver.begins, 3U);
    driver.complete();
    drive();
    EXPECT_EQ(runtime.voice().endpoint_phase(), AIEndpointPhase::Active);
    EXPECT_FALSE(media.capture);
}
// DuplicateTickAndDelayedTickDoNotBurst 同now不多发，迟到tick只发一条且超出2.4秒不补发。
TEST_F(RuntimeTargetControlSyncTest, DuplicateTickAndDelayedTickDoNotBurst) {
    ASSERT_NO_FATAL_FAILURE(restored_snapshot());
    body(
        R"({"msgToken":"recovery-release","method":"focusControl","sessionId":"candidate","eventSeq":10,"params":{"operation":"release","expectedRevision":1,"focusId":"candidate-focus","resources":["microphone","speaker"],"priority":"conversation","resumePolicy":"explicit"}})");
    driver.complete();
    prepare.released = true;
    drive();
    ASSERT_EQ(network.sent.back().method, "capabilityReport");
    const auto started = now;
    const auto count = network.sent.size();
    now = started + 200;
    runtime.poll(now, 1000 + now);
    runtime.poll(now, 1000 + now);
    EXPECT_EQ(network.sent.size(), count + 1U);
    now = started + 2200;
    runtime.poll(now, 1000 + now);
    EXPECT_EQ(network.sent.size(), count + 2U);
    runtime.poll(now, 1000 + now);
    EXPECT_EQ(network.sent.size(), count + 2U);
    now = started + 2401;
    runtime.poll(now, 1000 + now);
    EXPECT_EQ(network.sent.size(), count + 2U);
}
// SuccessAckStopsOnlyFastSchedule 匹配原成功ACK后停止快调度，不自行初始化焦点。
TEST_F(RuntimeTargetControlSyncTest, SuccessAckStopsOnlyFastSchedule) {
    ASSERT_NO_FATAL_FAILURE(restored_snapshot());
    body(
        R"({"msgToken":"recovery-release","method":"focusControl","sessionId":"candidate","eventSeq":10,"params":{"operation":"release","expectedRevision":1,"focusId":"candidate-focus","resources":["microphone","speaker"],"priority":"conversation","resumePolicy":"explicit"}})");
    driver.complete();
    prepare.released = true;
    drive();
    ASSERT_EQ(network.sent.back().method, "capabilityReport");
    const auto started = now;
    const auto count = network.sent.size();
    ack(R"({"acceptedRevision":1})");
    now = started + 200;
    runtime.poll(now, 1000 + now);
    EXPECT_EQ(network.sent.size(), count);
    EXPECT_EQ(runtime.state(), RuntimeState::Running);
}
// DisconnectStopsOldSocketSchedule 未知新租约再次断线保持原硬失败，旧socket不得再快发。
TEST_F(RuntimeTargetControlSyncTest, DisconnectStopsOldSocketSchedule) {
    ASSERT_NO_FATAL_FAILURE(restored_snapshot());
    body(
        R"({"msgToken":"recovery-release","method":"focusControl","sessionId":"candidate","eventSeq":10,"params":{"operation":"release","expectedRevision":1,"focusId":"candidate-focus","resources":["microphone","speaker"],"priority":"conversation","resumePolicy":"explicit"}})");
    driver.complete();
    prepare.released = true;
    drive();
    ASSERT_EQ(network.sent.back().method, "capabilityReport");
    const auto started = now;
    const auto count = network.sent.size();
    network.events.push_back({MqttEvent::Kind::Disconnected, 0, 0, {}, {}});
    drive();
    ASSERT_EQ(runtime.state(), RuntimeState::Failed);
    now = started + 200;
    runtime.poll(now, 1000 + now);
    EXPECT_EQ(network.sent.size(), count);
    EXPECT_FALSE(media.capture);
}
// PhysicalClockRollbackStillFailsClosed 真实monotonic回拨仍按原规则失败关闭。
TEST_F(RuntimeTargetControlSyncTest, PhysicalClockRollbackStillFailsClosed) {
    ASSERT_NO_FATAL_FAILURE(restored_snapshot());
    body(
        R"({"msgToken":"recovery-release","method":"focusControl","sessionId":"candidate","eventSeq":10,"params":{"operation":"release","expectedRevision":1,"focusId":"candidate-focus","resources":["microphone","speaker"],"priority":"conversation","resumePolicy":"explicit"}})");
    driver.complete();
    prepare.released = true;
    drive();
    ASSERT_EQ(network.sent.back().method, "capabilityReport");
    const auto started = now;
    const auto count = network.sent.size();
    now = started - 1;
    runtime.poll(now, 1000 + now);
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
    EXPECT_EQ(network.sent.size(), count);
    EXPECT_FALSE(media.capture);
}
// WallClockRollbackStillFailsClosed 真实Unix墙钟回拨仍失败关闭，不取max吞回拨。
TEST_F(RuntimeTargetControlSyncTest, WallClockRollbackStillFailsClosed) {
    ASSERT_NO_FATAL_FAILURE(restored_snapshot());
    body(
        R"({"msgToken":"recovery-release","method":"focusControl","sessionId":"candidate","eventSeq":10,"params":{"operation":"release","expectedRevision":1,"focusId":"candidate-focus","resources":["microphone","speaker"],"priority":"conversation","resumePolicy":"explicit"}})");
    driver.complete();
    prepare.released = true;
    drive();
    ASSERT_EQ(network.sent.back().method, "capabilityReport");
    const auto started = now;
    const auto count = network.sent.size();
    now = started + 200;
    runtime.poll(now, 999 + started);
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
    EXPECT_EQ(network.sent.size(), count);
    EXPECT_FALSE(media.capture);
}
// OriginalRecoveryDeadlineStillFailsClosed 原15秒恢复截止不因快速发送延长，届满保持停采。
TEST_F(RuntimeTargetControlSyncTest, OriginalRecoveryDeadlineStillFailsClosed) {
    ASSERT_NO_FATAL_FAILURE(restored_snapshot());
    body(
        R"({"msgToken":"recovery-release","method":"focusControl","sessionId":"candidate","eventSeq":10,"params":{"operation":"release","expectedRevision":1,"focusId":"candidate-focus","resources":["microphone","speaker"],"priority":"conversation","resumePolicy":"explicit"}})");
    driver.complete();
    prepare.released = true;
    drive();
    ASSERT_EQ(network.sent.back().method, "capabilityReport");
    const auto started = now;
    now = started + 15000;
    runtime.poll(now, 1000 + now);
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
    EXPECT_FALSE(media.capture);
}

// BaselineSameOriginalRequestRetransmitsAfterTwoHundredMilliseconds
// 原合法release本机完成且未收到基线ACK时，仅原请求可快重发。
TEST_F(RuntimeTargetControlSyncTest,
       BaselineSameOriginalRequestRetransmitsAfterTwoHundredMilliseconds) {
    ASSERT_NO_FATAL_FAILURE(restored_snapshot());
    body(
        R"({"msgToken":"recovery-release","method":"focusControl","sessionId":"candidate","eventSeq":10,"params":{"operation":"release","expectedRevision":1,"focusId":"candidate-focus","resources":["microphone","speaker"],"priority":"conversation","resumePolicy":"explicit"}})");
    ASSERT_EQ(driver.begins, 2U);
    driver.complete();
    prepare.released = true;
    drive();
    ASSERT_EQ(network.sent.back().method, "capabilityReport");
    ASSERT_NE(network.sent.back().payload.find("\"baseline\""), std::string::npos);
    const auto original = network.sent.back();
    const auto started = now;
    const auto count = network.sent.size();
    now = started + 199;
    runtime.poll(now, 1000 + now);
    ASSERT_EQ(network.sent.size(), count);
    now = started + 200;
    runtime.poll(now, 1000 + now);
    ASSERT_EQ(runtime.state(), RuntimeState::Running);
    ASSERT_EQ(network.sent.size(), count + 1U);
    EXPECT_EQ(network.sent.back().token, original.token);
    std::string before, after;
    ASSERT_EQ(encode_envelope(original, before), DecodeResult::Ok);
    ASSERT_EQ(encode_envelope(network.sent.back(), after), DecodeResult::Ok);
    EXPECT_EQ(before, after);
    EXPECT_FALSE(media.capture);
    EXPECT_EQ(driver.begins, 2U);
}

// FatalBaselineAckStopsFastSchedule 匹配原409致命拒绝仍失败关闭，不重投或重开采集。
TEST_F(RuntimeTargetControlSyncTest, FatalBaselineAckStopsFastSchedule) {
    ASSERT_NO_FATAL_FAILURE(restored_snapshot());
    body(
        R"({"msgToken":"recovery-release","method":"focusControl","sessionId":"candidate","eventSeq":10,"params":{"operation":"release","expectedRevision":1,"focusId":"candidate-focus","resources":["microphone","speaker"],"priority":"conversation","resumePolicy":"explicit"}})");
    driver.complete();
    prepare.released = true;
    drive();
    ASSERT_EQ(network.sent.back().method, "capabilityReport");
    const auto original = network.sent.back();
    const auto started = now;
    Envelope rejected;
    rejected.request = false;
    rejected.token = original.token;
    rejected.session = original.session;
    rejected.method = "requestAck";
    rejected.code = 409;
    rejected.payload = R"({"requestMethod":"capabilityReport","reason":"requestConflict"})";
    std::string encoded;
    ASSERT_EQ(encode_envelope(rejected, encoded), DecodeResult::Ok);
    body(encoded);
    ASSERT_EQ(runtime.state(), RuntimeState::Failed);
    const auto count = network.sent.size();
    now = started + 200;
    runtime.poll(now, 1000 + now);
    EXPECT_EQ(network.sent.size(), count);
    EXPECT_FALSE(media.capture);
}
} // namespace
} // namespace ur::device
