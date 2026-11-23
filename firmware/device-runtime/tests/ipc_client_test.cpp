// IPC独立观看状态机回归；可控媒体端口验证授权、事件乱序、超时和资源释放，不代替实际媒体E2E。
#include "ur/device/ipc_client.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

namespace ur::device {
namespace {
// IPCPublish 记录真实编码器生成的信封，测试不旁路请求关联。
struct IPCPublish final : PublishPort {
    std::vector<Envelope> sent;
    bool publish(std::string_view, std::string_view body) override {
        Envelope envelope;
        if (decode_envelope(body, envelope) != DecodeResult::Ok)
            return false;
        sent.push_back(std::move(envelope));
        return true;
    }
};
// IPCMediaFacts 独立保存驱动事实，关闭后还能安全检查，避免测试悬空指针。
struct IPCMediaFacts {
    bool connected = false;
    bool started = false;
    bool allow = true;
    int closed = 0;
    int track_controls = 0;                    // 驱动执行次数，重复控制不得增加。
    std::vector<IPCQualityCommand> qualities;  // 实际接纳的质量命令，平台应答不等于已执行。
    bool quality_supported = true;             // 显式测试驱动；未适配生产端口默认拒绝。
    std::uint64_t recovery_revision = 0;       // 真实驱动收到的下一代请求，不重新创建采集。
    bool recovery_supported = true;            // 可控恢复拒绝路径，不代表硬件适配完成。
    IPCInvite invite;  // 记录驱动实际收到的授权轨，不能偷偷添加视频或省略扬声器。
};
// IPCFakeMedia 只按显式驱动事件变连通，不因SDP成功自行建立。
struct IPCFakeMedia final : IPCMediaPort {
    std::shared_ptr<IPCMediaFacts> facts;
    explicit IPCFakeMedia(std::shared_ptr<IPCMediaFacts> value) : facts(std::move(value)) {}
    bool begin(const IPCInvite& invite) override {
        facts->invite = invite;
        facts->started = facts->allow;
        return facts->allow;
    }
    bool apply_answer(std::string_view) override { return facts->allow; }
    bool connected() const override { return facts->connected; }
    bool begin_recovery(std::uint64_t revision) override {
        facts->recovery_revision = revision;
        facts->connected = false;
        return facts->recovery_supported;
    }
    bool control_track(const IPCTrackCommand&) override {
        ++facts->track_controls;
        return facts->allow;
    }
    bool apply_quality(const IPCQualityCommand& command) override {
        if (!facts->quality_supported)
            return false;
        facts->qualities.push_back(command);
        return facts->allow;
    }
    void close() override {
        facts->started = facts->connected = false;
        ++facts->closed;
    }
};
// IPCFactory 为各观看者分配独立事实，模拟物理隐私拒绝而非共享“当前Peer”。
struct IPCFactory final : IPCMediaFactory {
    std::vector<std::shared_ptr<IPCMediaFacts>> facts;
    bool allow = true;
    bool talk_supported = false;  // 显式双向驱动许可，不从设备能力声明推断。
    bool supports_talk() const override { return talk_supported; }
    std::unique_ptr<IPCMediaPort> create() override {
        auto value = std::make_shared<IPCMediaFacts>();
        value->allow = allow;
        facts.push_back(value);
        return std::make_unique<IPCFakeMedia>(value);
    }
};
// ipc_body 使用公共编码器生成完整测试报文。
std::string ipc_body(const Envelope& message) {
    std::string body;
    EXPECT_EQ(encode_envelope(message, body), DecodeResult::Ok);
    return body;
}
// ipc_invite 固定能力源与期限，观看者只通过独立sessionId区分。
Envelope ipc_invite(std::string session, bool consent = false) {
    Envelope message;
    message.method = "sessionInvite";
    message.session = std::move(session);
    message.token = "invite-" + message.session;
    message.event_sequence = 1;
    message.payload =
        std::string(
            R"({"purpose":"ipcLiveView","cameraId":"front","transport":"webrtc","expiresAt":1020000,"admission":")") +
        (consent ? "accept" : "direct") +
        R"(","tracks":[{"trackId":"video-1","kind":"video","sourceId":"front","direction":"sendonly","codec":"H264","state":"requested"}]})";
    return message;
}
// ipc_reply 关联原请求，不伪造当前会话或当前token。
Envelope ipc_reply(const Envelope& command, std::string method, std::string payload) {
    Envelope reply;
    reply.token = command.token;
    reply.session = command.session;
    reply.request = false;
    reply.code = 200;
    reply.method = std::move(method);
    reply.payload = std::move(payload);
    return reply;
}
// ipc_accept_ack 验证受理之前绝不创建采集端口。
void ipc_accept_ack(IPCClient& client, IPCPublish& mqtt, IPCFactory& factory, std::string session,
                    const Envelope* invitation = nullptr) {
    const auto count = factory.facts.size();
    ASSERT_TRUE(client.receive("down", ipc_body(invitation ? *invitation : ipc_invite(session)),
                               100, 1000000));
    ASSERT_EQ(factory.facts.size(), count);
    ASSERT_EQ(client.phase(session), IPCPhase::Accepting);
    ASSERT_TRUE(client.receive(
        "down",
        ipc_body(
            ipc_reply(mqtt.sent.back(), "requestAck",
                      R"({"requestMethod":"sessionAccept","result":{"state":"negotiating"}})")),
        101, 1000000));
    ASSERT_EQ(client.phase(session), IPCPhase::Negotiating);
    ASSERT_EQ(factory.facts.size(), count + 1);
}
// ipc_activate
// 通过真实编码及全部ACK建立指定观看者，供源驱动失败与超时测试使用。
void ipc_activate(IPCClient& client, IPCPublish& mqtt, IPCFactory& factory,
                  const std::string& session, const Envelope* invitation = nullptr) {
    ipc_accept_ack(client, mqtt, factory, session, invitation);
    ASSERT_TRUE(client.offer_ready(session, "v=0\r\ns=test\r\n", 102));
    ASSERT_TRUE(client.receive(
        "down",
        ipc_body(ipc_reply(
            mqtt.sent.back(), "mediaAnswer",
            R"({"mediaRevision":1,"webrtc":{"iceMode":"complete","sdp":"v=0\r\ns=test\r\n"}})")),
        103, 1000000));
    factory.facts.back()->connected = true;
    ASSERT_TRUE(client.media_connected(session, 104));
    const auto report = mqtt.sent.back();
    ASSERT_TRUE(client.receive(
        "down",
        ipc_body(ipc_reply(
            report, "requestAck",
            R"({"requestMethod":"mediaState","result":{"state":"connected","mediaRevision":1}})")),
        105, 1000000));
    Envelope established;
    established.request = false;
    established.token = "established";
    established.method = "sessionEstablished";
    established.session = session;
    established.event_sequence = 2;
    established.payload = R"({"state":"established","mediaRevision":1})";
    ASSERT_TRUE(client.receive("down", ipc_body(established), 106, 1000000));
    ASSERT_EQ(client.phase(session), IPCPhase::Active);
}
// ipc_source_control 使用原契约编码授权轨控制，不直接触碰驱动测试对象。
Envelope ipc_source_control(std::string token, std::string operation, std::uint64_t sequence) {
    Envelope command;
    command.token = std::move(token);
    command.method = "trackControl";
    command.session = "a";
    command.event_sequence = sequence;
    command.payload =
        R"({"trackId":"video-1","operation":")" + operation + R"(","mediaRevision":1})";
    return command;
}
// ipc_recover
// 只推进真实失联确认、新代次SDP和连通确认；不补新会话建立事件或重新开始采集。
void ipc_recover(IPCClient& client, IPCPublish& mqtt, IPCFactory& factory) {
    const auto lost = mqtt.sent.back();
    ASSERT_TRUE(client.receive(
        "down",
        ipc_body(ipc_reply(
            lost, "requestAck",
            R"({"requestMethod":"mediaState","result":{"state":"disconnected","mediaRevision":1}})")),
        202, 1000000));
    ASSERT_EQ(factory.facts.front()->recovery_revision, 2U);
    ASSERT_FALSE(client.offer_ready("a", "v=0\r\ns=late\r\n", 203, 1));
    ASSERT_TRUE(client.offer_ready("a", "v=0\r\ns=new\r\n", 203, 2));
    const auto offer = mqtt.sent.back();
    ASSERT_FALSE(client.receive(
        "down",
        ipc_body(ipc_reply(
            offer, "mediaAnswer",
            R"({"mediaRevision":1,"webrtc":{"iceMode":"complete","sdp":"v=0\r\ns=old\r\n"}})")),
        204, 1000000));
    ASSERT_TRUE(client.receive(
        "down",
        ipc_body(ipc_reply(
            offer, "mediaAnswer",
            R"({"mediaRevision":2,"webrtc":{"iceMode":"complete","sdp":"v=0\r\ns=new\r\n"}})")),
        204, 1000000));
    factory.facts.front()->connected = true;
    ASSERT_FALSE(client.media_connected("a", 205, 1));
    ASSERT_TRUE(client.media_connected("a", 205, 2));
    ASSERT_TRUE(client.receive(
        "down",
        ipc_body(ipc_reply(
            mqtt.sent.back(), "requestAck",
            R"({"requestMethod":"mediaState","result":{"state":"connected","mediaRevision":2}})")),
        206, 1000000));
    ASSERT_EQ(client.phase("a"), IPCPhase::Active);
}
// 单腿暂断不影响另一观看者；原失联重复不续期，不重新分配媒体、邀请或采集。
TEST(IPCClient, PreviewRecoveryUsesOriginalPeerAndFencesOldRevision) {
    IPCPublish mqtt;
    IPCFactory factory;
    IPCCapability capability;
    capability.max_publishers = 2;
    IPCClient client(mqtt, factory, "up", "down", "instance", capability);
    ipc_activate(client, mqtt, factory, "a");
    ipc_activate(client, mqtt, factory, "b");
    factory.facts.front()->connected = false;
    ASSERT_TRUE(client.media_disconnected("a", 1, 200));
    const auto count = mqtt.sent.size();
    EXPECT_TRUE(client.media_disconnected("a", 1, 201));
    EXPECT_EQ(mqtt.sent.size(), count);
    EXPECT_EQ(factory.facts.front()->recovery_revision, 0U);
    ipc_recover(client, mqtt, factory);
    EXPECT_EQ(factory.facts.size(), 2U);
    EXPECT_EQ(factory.facts.front()->closed, 0);
    EXPECT_EQ(client.phase("b"), IPCPhase::Active);
    EXPECT_EQ(client.media_revision("a"), 2U);
    EXPECT_FALSE(
        client.receive("down", ipc_body(ipc_source_control("old", "pause", 3)), 207, 1000000));
    EXPECT_EQ(factory.facts.front()->track_controls, 0);
}
// 暂停和停止事实保持不变；恢复不能调用任何轨控制或伪造恢复采集。
TEST(IPCClient, RecoveryPreservesStoppedCaptureAndRejectsImplicitResume) {
    IPCPublish mqtt;
    IPCFactory factory;
    IPCClient client(mqtt, factory, "up", "down", "instance", {});
    ipc_activate(client, mqtt, factory, "a");
    const auto stopped = ipc_source_control("stopped", "stop", 3);
    ASSERT_TRUE(client.receive("down", ipc_body(stopped), 180, 1000000));
    ASSERT_TRUE(client.track_applied("a", "stopped", "stopped", true, 181));
    ASSERT_TRUE(client.media_disconnected("a", 1, 200));
    ipc_recover(client, mqtt, factory);
    auto resume = ipc_source_control("resume", "resume", 4);
    resume.payload = R"({"trackId":"video-1","operation":"resume","mediaRevision":2})";
    EXPECT_FALSE(client.receive("down", ipc_body(resume), 207, 1000000));
    EXPECT_EQ(factory.facts.front()->track_controls, 1);
}
// 原15秒硬截止涵盖ACK与SDP所有阶段；重复、迟到和时钟回退都不能复活原媒体。
TEST(IPCClient, RecoveryDeadlineAndLateCallbacksReleaseOnlyOriginalView) {
    for (const bool rollback : {false, true}) {
        IPCPublish mqtt;
        IPCFactory factory;
        IPCClient client(mqtt, factory, "up", "down", "instance", {});
        ipc_activate(client, mqtt, factory, "a");
        ASSERT_TRUE(client.media_disconnected("a", 1, 200));
        EXPECT_TRUE(client.media_disconnected("a", 1, 15199));
        client.tick(rollback ? 199 : 15200, 1000000);
        EXPECT_EQ(client.phase("a"), IPCPhase::Ended);
        EXPECT_EQ(factory.facts.front()->closed, 1);
        EXPECT_FALSE(client.offer_ready("a", "v=0\r\ns=late\r\n", 15201, 2));
        EXPECT_FALSE(client.media_disconnected("a", 1, 15201));
    }
}
// 恢复驱动不支持时必须释放原腿，不能把ACK包装成成功重协商。
TEST(IPCClient, RecoveryDriverRejectsWithoutRestartingCapture) {
    IPCPublish mqtt;
    IPCFactory factory;
    IPCClient client(mqtt, factory, "up", "down", "instance", {});
    ipc_activate(client, mqtt, factory, "a");
    factory.facts.front()->recovery_supported = false;
    ASSERT_TRUE(client.media_disconnected("a", 1, 200));
    ASSERT_TRUE(client.receive(
        "down",
        ipc_body(ipc_reply(
            mqtt.sent.back(), "requestAck",
            R"({"requestMethod":"mediaState","result":{"state":"disconnected","mediaRevision":1}})")),
        201, 1000000));
    EXPECT_EQ(client.phase("a"), IPCPhase::Ending);
    EXPECT_EQ(factory.facts.front()->closed, 1);
    EXPECT_EQ(factory.facts.size(), 1U);
}
// 十秒驱动超时只结束对应观看者；原重试不延长截止，迟到成功不得复活。
TEST(IPCClient, SourceDriverTimeoutIsolatedAndLateCompletionRejected) {
    IPCPublish mqtt;
    IPCFactory factory;
    IPCCapability capability;
    capability.max_publishers = 2;
    IPCClient client(mqtt, factory, "up", "down", "instance", capability);
    ipc_activate(client, mqtt, factory, "a");
    ipc_activate(client, mqtt, factory, "b");
    const auto control = ipc_source_control("pause", "pause", 3);
    ASSERT_TRUE(client.receive("down", ipc_body(control), 200, 1000000));
    ASSERT_TRUE(client.receive("down", ipc_body(control), 10199, 1000000));
    client.tick(10199, 1000000);
    EXPECT_EQ(factory.facts[0]->closed, 0);
    client.tick(10200, 1000000);
    EXPECT_EQ(factory.facts[0]->closed, 1);
    EXPECT_EQ(factory.facts[1]->closed, 0);
    EXPECT_EQ(client.phase("b"), IPCPhase::Active);
    EXPECT_FALSE(client.track_applied("a", "pause", "paused", true, 10201));
    EXPECT_EQ(factory.facts[0]->track_controls, 1);
}
// 驱动拒绝立即停止单观看者；失败不能被包装为成功trackState。
TEST(IPCClient, SourceDriverRejectsWithoutSuccessReceipt) {
    IPCPublish mqtt;
    IPCFactory factory;
    IPCClient client(mqtt, factory, "up", "down", "instance", {});
    ipc_activate(client, mqtt, factory, "a");
    factory.facts[0]->allow = false;
    ASSERT_TRUE(
        client.receive("down", ipc_body(ipc_source_control("pause", "pause", 3)), 200, 1000000));
    EXPECT_EQ(factory.facts[0]->closed, 1);
    const auto receipt =
        std::find_if(mqtt.sent.begin(), mqtt.sent.end(),
                     [](const Envelope& value) { return value.method == "trackState"; });
    ASSERT_NE(receipt, mqtt.sent.end());
    EXPECT_EQ(receipt->code, 500);
    EXPECT_EQ(receipt->payload, R"({"reason":"mediaFailed"})");
}
// 有界回执淘汰不能拒绝新控制；旧水位不得重新执行，停止后resume不能隐式采集。
TEST(IPCClient, SourceReplyCacheBoundedWithoutBlockingNewControls) {
    IPCPublish mqtt;
    IPCFactory factory;
    IPCClient client(mqtt, factory, "up", "down", "instance", {});
    ipc_activate(client, mqtt, factory, "a");
    for (std::uint64_t i = 0; i < 40; ++i) {
        const std::string operation = i % 2 == 0 ? "pause" : "resume";
        const std::string state = i % 2 == 0 ? "paused" : "active";
        const auto command = ipc_source_control("control-" + std::to_string(i), operation, i + 3);
        ASSERT_TRUE(client.receive("down", ipc_body(command), 200 + i, 1000000));
        ASSERT_TRUE(client.track_applied("a", command.token, state, true, 200 + i));
    }
    EXPECT_EQ(factory.facts[0]->track_controls, 40);
    EXPECT_FALSE(client.receive("down", ipc_body(ipc_source_control("control-0", "pause", 3)), 300,
                                1000000));
    EXPECT_TRUE(client.receive("down", ipc_body(ipc_source_control("control-39", "resume", 42)),
                               300, 1000000));
    EXPECT_EQ(factory.facts[0]->track_controls, 40);
    ASSERT_TRUE(
        client.receive("down", ipc_body(ipc_source_control("stop", "stop", 43)), 301, 1000000));
    ASSERT_TRUE(client.track_applied("a", "stop", "stopped", true, 301));
    EXPECT_FALSE(client.receive(
        "down", ipc_body(ipc_source_control("resume-stopped", "resume", 44)), 302, 1000000));
    EXPECT_EQ(factory.facts[0]->track_controls, 41);
}
TEST(IPCClient, ThreeViewersHaveIndependentMediaAndTermination) {
    IPCPublish mqtt;
    IPCFactory factory;
    IPCCapability capability;
    capability.max_publishers = 3;
    IPCClient client(mqtt, factory, "up", "down", "instance", capability);
    for (const auto* session : {"a", "b", "c"})
        ipc_accept_ack(client, mqtt, factory, session);
    EXPECT_EQ(client.live_sessions(), 3U);
    EXPECT_FALSE(client.receive("down", ipc_body(ipc_invite("d")), 102, 1000000));
    ASSERT_TRUE(client.end("a", 103));
    EXPECT_EQ(factory.facts[0]->closed, 1);
    EXPECT_TRUE(factory.facts[1]->started);
    EXPECT_TRUE(factory.facts[2]->started);
    EXPECT_EQ(client.live_sessions(), 2U);
    ipc_accept_ack(client, mqtt, factory, "d");
    client.close_all(104);
    EXPECT_EQ(client.live_sessions(), 0U);
    for (const auto& facts : factory.facts)
        EXPECT_EQ(facts->closed, 1);
    client.close_all(105);
    for (const auto& facts : factory.facts)
        EXPECT_EQ(facts->closed, 1);
}
TEST(IPCClient, ConsentAndDeadlineDoNotStartCameraEarly) {
    IPCPublish mqtt;
    IPCFactory factory;
    IPCCapability capability;
    capability.local_accept = true;
    IPCClient client(mqtt, factory, "up", "down", "instance", capability);
    ASSERT_TRUE(client.receive("down", ipc_body(ipc_invite("a", true)), 100, 1000000));
    EXPECT_EQ(client.phase("a"), IPCPhase::Invited);
    EXPECT_TRUE(mqtt.sent.empty());
    EXPECT_FALSE(client.accept("a", 101, 1020000));
    EXPECT_TRUE(factory.facts.empty());
    client.tick(102, 1020000);
    EXPECT_EQ(client.phase("a"), IPCPhase::Ended);
    EXPECT_FALSE(client.accept("a", 103, 1000000));
    EXPECT_FALSE(client.receive("down", ipc_body(ipc_invite("expired")), 104, 1020000));
}
TEST(IPCClient, LocalPrivacyBlocksDirectInvitationAfterPlatformAdmission) {
    IPCPublish mqtt;
    IPCFactory factory;
    factory.allow = false;
    IPCClient client(mqtt, factory, "up", "down", "instance", {});
    ASSERT_TRUE(client.receive("down", ipc_body(ipc_invite("a")), 100, 1000000));
    ASSERT_TRUE(client.receive(
        "down",
        ipc_body(
            ipc_reply(mqtt.sent.back(), "requestAck",
                      R"({"requestMethod":"sessionAccept","result":{"state":"negotiating"}})")),
        101, 1000000));
    EXPECT_EQ(client.phase("a"), IPCPhase::Ending);
    EXPECT_FALSE(factory.facts[0]->started);
    EXPECT_EQ(factory.facts[0]->closed, 1);
    EXPECT_EQ(mqtt.sent.back().method, "sessionEnd");
}
TEST(IPCClient, InviteRetryDoesNotAllocateOrResetDeadline) {
    IPCPublish mqtt;
    IPCFactory factory;
    IPCClient client(mqtt, factory, "up", "down", "instance", {});
    const auto invite = ipc_invite("a");
    ASSERT_TRUE(client.receive("down", ipc_body(invite), 100, 1000000));
    EXPECT_TRUE(client.receive("down", ipc_body(invite), 101, 1000000));
    EXPECT_EQ(mqtt.sent.size(), 1U);
    auto changed = invite;
    changed.payload.replace(changed.payload.find("1020000"), 7, "1020001");
    EXPECT_FALSE(client.receive("down", ipc_body(changed), 102, 1000000));
    client.tick(30100, 1000000);
    EXPECT_EQ(client.phase("a"), IPCPhase::Ended);
    EXPECT_TRUE(factory.facts.empty());
    EXPECT_TRUE(client.receive("down", ipc_body(invite), 30101, 1000000));
    EXPECT_EQ(client.phase("a"), IPCPhase::Ended);
}
TEST(IPCClient, ActualMediaAndAckAndEstablishedAreAllRequired) {
    IPCPublish mqtt;
    IPCFactory factory;
    IPCClient client(mqtt, factory, "up", "down", "instance", {});
    ipc_accept_ack(client, mqtt, factory, "a");
    ASSERT_TRUE(client.offer_ready("a", "v=0\r\ns=test\r\n", 102));
    EXPECT_FALSE(client.offer_ready("a", "v=0\r\ns=other\r\n", 103));
    EXPECT_FALSE(client.media_connected("a", 103));
    const auto offer = mqtt.sent.back();
    auto answer = ipc_reply(
        offer, "mediaAnswer",
        R"({"mediaRevision":1,"webrtc":{"iceMode":"complete","sdp":"v=0\r\ns=test\r\n"}})");
    auto stale_answer = answer;
    stale_answer.payload.replace(stale_answer.payload.find("Revision\":1"), 11, "Revision\":2");
    EXPECT_FALSE(client.receive("down", ipc_body(stale_answer), 104, 1000000));
    ASSERT_TRUE(client.receive("down", ipc_body(answer), 104, 1000000));
    EXPECT_FALSE(client.media_connected("a", 105));
    factory.facts[0]->connected = true;
    ASSERT_TRUE(client.media_connected("a", 106));
    const auto report = mqtt.sent.back();
    Envelope established;
    established.request = false;
    established.token = "established";
    established.method = "sessionEstablished";
    established.session = "a";
    established.event_sequence = 2;
    established.payload = R"({"state":"established","mediaRevision":1})";
    ASSERT_TRUE(client.receive("down", ipc_body(established), 107, 1000000));
    EXPECT_EQ(client.phase("a"), IPCPhase::WaitingEstablished);
    ASSERT_TRUE(client.receive(
        "down",
        ipc_body(ipc_reply(
            report, "requestAck",
            R"({"requestMethod":"mediaState","result":{"state":"connected","mediaRevision":1}})")),
        108, 1000000));
    EXPECT_EQ(client.phase("a"), IPCPhase::Active);
    Envelope pause;
    pause.token = "pause-track";
    pause.method = "trackControl";
    pause.session = "a";
    pause.event_sequence = 3;
    pause.payload = R"({"trackId":"video-1","operation":"pause","mediaRevision":1})";
    const auto before_control = mqtt.sent.size();
    ASSERT_TRUE(client.receive("down", ipc_body(pause), 108, 1000000));
    EXPECT_EQ(mqtt.sent.size(), before_control) << "驱动受理不能当作实际暂停回执";
    ASSERT_TRUE(client.receive("down", ipc_body(pause), 108, 1000000));
    EXPECT_EQ(factory.facts[0]->track_controls, 1);
    EXPECT_FALSE(client.track_applied("a", "other-token", "paused", true, 108));
    EXPECT_FALSE(client.track_applied("a", pause.token, "active", true, 108));
    ASSERT_TRUE(client.track_applied("a", pause.token, "paused", true, 108));
    EXPECT_EQ(mqtt.sent.back().method, "trackState");
    EXPECT_EQ(mqtt.sent.back().token, pause.token);
    ASSERT_TRUE(client.receive("down", ipc_body(pause), 108, 1000000));
    EXPECT_EQ(factory.facts[0]->track_controls, 1) << "实际完成后重放原回执不重做驱动";
    auto conflict = pause;
    conflict.payload = R"({"trackId":"video-1","operation":"stop","mediaRevision":1})";
    EXPECT_FALSE(client.receive("down", ipc_body(conflict), 108, 1000000));
    established.method = "sessionEnded";
    established.event_sequence = 4;
    established.payload = R"({"state":"ended","reason":"permissionRevoked"})";
    ASSERT_TRUE(client.receive("down", ipc_body(established), 109, 1000000));
    EXPECT_EQ(client.phase("a"), IPCPhase::Ended);
    EXPECT_EQ(factory.facts[0]->closed, 1);
    EXPECT_TRUE(client.receive("down", ipc_body(answer), 110, 1000000));
    EXPECT_FALSE(client.media_connected("a", 111));
}
TEST(IPCClient, WrongTopicAndCrossSessionAckCannotOpenCamera) {
    IPCPublish mqtt;
    IPCFactory factory;
    IPCClient client(mqtt, factory, "up", "down", "instance", {});
    EXPECT_FALSE(client.receive("other", ipc_body(ipc_invite("a")), 100, 1000000));
    ASSERT_TRUE(client.receive("down", ipc_body(ipc_invite("a")), 100, 1000000));
    auto reply = ipc_reply(mqtt.sent.back(), "requestAck",
                           R"({"requestMethod":"sessionAccept","result":{"state":"negotiating"}})");
    reply.session = "other";
    EXPECT_FALSE(client.receive("down", ipc_body(reply), 101, 1000000));
    reply.session = "a";
    reply.token = "wrong";
    EXPECT_FALSE(client.receive("down", ipc_body(reply), 102, 1000000));
    EXPECT_TRUE(factory.facts.empty());
}
TEST(IPCClient, LateAcceptAcknowledgementCannotStartExpiredCamera) {
    IPCPublish mqtt;
    IPCFactory factory;
    IPCClient client(mqtt, factory, "up", "down", "instance", {});
    ASSERT_TRUE(client.receive("down", ipc_body(ipc_invite("a")), 100, 1000000));
    auto reply = ipc_reply(mqtt.sent.back(), "requestAck",
                           R"({"requestMethod":"sessionAccept","result":{"state":"negotiating"}})");
    EXPECT_FALSE(client.receive("down", ipc_body(reply), 101, 1020000));
    EXPECT_EQ(client.phase("a"), IPCPhase::Ended);
    EXPECT_TRUE(factory.facts.empty());
}
TEST(IPCClient, RetryKeepsTokenAndMediaTimeoutReleasesOnlyOwnPeer) {
    IPCPublish mqtt;
    IPCFactory factory;
    IPCCapability capability;
    capability.max_publishers = 2;
    IPCClient client(mqtt, factory, "up", "down", "instance", capability);
    ASSERT_TRUE(client.receive("down", ipc_body(ipc_invite("a")), 100, 1000000));
    const auto initial = ipc_body(mqtt.sent.back());
    client.tick(10100, 1010000);
    ASSERT_EQ(mqtt.sent.size(), 2U);
    EXPECT_EQ(ipc_body(mqtt.sent.back()), initial);
    ASSERT_TRUE(client.receive(
        "down",
        ipc_body(
            ipc_reply(mqtt.sent.back(), "requestAck",
                      R"({"requestMethod":"sessionAccept","result":{"state":"negotiating"}})")),
        10101, 1010000));
    ipc_accept_ack(client, mqtt, factory, "b");
    // b的单调阶段起点重新设置为较晚时刻，a超时时仍保留b。
    ASSERT_TRUE(client.offer_ready("b", "v=0\r\ns=test\r\n", 20000));
    client.tick(25101, 1011000);
    EXPECT_EQ(client.phase("a"), IPCPhase::Ended);
    EXPECT_EQ(factory.facts[0]->closed, 1);
    EXPECT_EQ(client.phase("b"), IPCPhase::Connecting);
    EXPECT_EQ(factory.facts[1]->closed, 0);
}

// talk_invite
// 使用S07相同的独立对讲用途，轨方向相对设备，不带会议成员或AI交互模式。
Envelope talk_invite() {
    Envelope message;
    message.method = "sessionInvite";
    message.session = "conference-leg";
    message.token = "talk-invite";
    message.event_sequence = 1;
    message.payload =
        R"({"purpose":"ipcTalk","admission":"direct","cameraId":"front","transport":"webrtc","expiresAt":1900000000000,"tracks":[{"trackId":"mic-1","kind":"audio","sourceId":"mic","direction":"sendonly","codec":"opus","state":"requested"},{"trackId":"speaker-1","kind":"audio","sourceId":"speaker","direction":"recvonly","codec":"opus","state":"requested"}]})";
    return message;
}

TEST(IPCTalkWire, IndependentCapabilityAndTracks) {
    IPCCapability capability;
    Envelope report;
    ASSERT_EQ(ipc_capability_command("cap", capability, report), WireResult::Ok);
    EXPECT_EQ(report.payload.find("ipcTalk"), std::string::npos);
    IPCTalkInvite invite;
    const auto body = ipc_body(talk_invite());
    EXPECT_EQ(decode_ipc_talk_invite(WireRoute::Signaling, body, capability, invite),
              WireResult::Invalid);
    capability.conference_talk = true;
    EXPECT_EQ(ipc_capability_command("cap", capability, report), WireResult::Invalid);
    EXPECT_TRUE(report.token.empty());
    capability.microphone = true;
    ASSERT_EQ(ipc_capability_command("cap", capability, report), WireResult::Ok);
    EXPECT_NE(report.payload.find("ipcTalk"), std::string::npos);
    ASSERT_EQ(decode_ipc_talk_invite(WireRoute::Signaling, body, capability, invite),
              WireResult::Ok);
    EXPECT_EQ(invite.microphone_track, "mic-1");
    EXPECT_EQ(invite.speaker_track, "speaker-1");
    EXPECT_EQ(invite.camera_id, "front");
    EXPECT_EQ(invite.expires_at_ms, 1900000000000ULL);
    EXPECT_FALSE(invite.needs_accept);
    IPCInvite view;
    EXPECT_EQ(decode_ipc_invite(WireRoute::Signaling, body, capability, view), WireResult::Invalid);
    EXPECT_EQ(decode_ipc_talk_invite(WireRoute::Signaling, ipc_body(ipc_invite("view")), capability,
                                     invite),
              WireResult::Invalid);
    EXPECT_TRUE(invite.envelope.session.empty());
}

TEST(IPCTalkWire, StrictFieldsDirectionsAndRoute) {
    IPCCapability capability;
    capability.microphone = capability.conference_talk = true;
    const auto initial = ipc_body(talk_invite());
    for (const auto& mutation : std::vector<std::pair<std::string, std::string>>{
             {R"("purpose":"ipcTalk")", R"("purpose":"ipcTalk","purpose":"ipcLiveView")"},
             {R"("admission":"direct")", R"("admission":"direct","callGroupId":"untrusted")"},
             {R"("admission":"direct")", R"("admission":"auto")"},
             {R"("cameraId":"front")", R"("cameraId":"rear")"},
             {R"("transport":"webrtc")", R"("transport":"udp")"},
             {R"("expiresAt":1900000000000)", R"("expiresAt":0)"},
             {R"("expiresAt":1900000000000)", R"("expiresAt":9007199254740992)"},
             {R"("expiresAt":1900000000000)", R"("expiresAt":1.5)"},
             {R"("trackId":"speaker-1")", R"("trackId":"mic-1")"},
             {R"("sourceId":"speaker")", R"("sourceId":"mic")"},
             {R"("direction":"recvonly")", R"("direction":"sendonly")"},
             {R"("kind":"audio")", R"("kind":"video")"},
             {R"("state":"requested")", R"("state":"active")"},
             {R"("codec":"opus")", R"("codec":"PCMU")"}}) {
        auto body = initial;
        const auto position = body.find(mutation.first);
        ASSERT_NE(position, std::string::npos);
        body.replace(position, mutation.first.size(), mutation.second);
        IPCTalkInvite invite;
        invite.speaker_track = "must-clear";
        EXPECT_EQ(decode_ipc_talk_invite(WireRoute::Signaling, body, capability, invite),
                  WireResult::Invalid);
        EXPECT_TRUE(invite.speaker_track.empty());
    }
    IPCTalkInvite invite;
    EXPECT_EQ(decode_ipc_talk_invite(WireRoute::Message, initial, capability, invite),
              WireResult::WrongRoute);
    auto command = talk_invite();
    command.event_sequence.reset();
    EXPECT_EQ(decode_ipc_talk_invite(WireRoute::Signaling, ipc_body(command), capability, invite),
              WireResult::Invalid);
    command = talk_invite();
    command.method = "sessionCreate";
    EXPECT_EQ(decode_ipc_talk_invite(WireRoute::Signaling, ipc_body(command), capability, invite),
              WireResult::Invalid);
}

TEST(IPCTalkWire, LocalConsentAndPreviewDriverIsolation) {
    IPCCapability capability;
    capability.microphone = capability.conference_talk = true;
    auto command = talk_invite();
    const auto pos = command.payload.find("direct");
    ASSERT_NE(pos, std::string::npos);
    command.payload.replace(pos, 6, "accept");
    IPCTalkInvite invite;
    EXPECT_EQ(decode_ipc_talk_invite(WireRoute::Signaling, ipc_body(command), capability, invite),
              WireResult::Invalid);
    capability.local_accept = true;
    ASSERT_EQ(decode_ipc_talk_invite(WireRoute::Signaling, ipc_body(command), capability, invite),
              WireResult::Ok);
    EXPECT_TRUE(invite.needs_accept);
    // 现有预览控制器没有会议驱动，不能因为新解析器存在就启动采集或谎报对讲已支持。
    IPCPublish mqtt;
    IPCFactory factory;
    IPCClient preview(mqtt, factory, "up", "down", "instance", capability);
    EXPECT_FALSE(preview.receive("down", ipc_body(command), 0, 1899999999000ULL));
    EXPECT_TRUE(factory.facts.empty());
    EXPECT_TRUE(mqtt.sent.empty());
}

// runtime_talk 将协议样本期限绑定可控时钟，不放宽运行时三十秒邀请上限。
Envelope runtime_talk(std::string session = "conference-leg", bool consent = false) {
    auto invite = talk_invite();
    invite.session = std::move(session);
    invite.token = "invite-" + invite.session;
    auto position = invite.payload.find("1900000000000");
    invite.payload.replace(position, 13, "1020000");
    if (consent) {
        position = invite.payload.find("direct");
        invite.payload.replace(position, 6, "accept");
    }
    return invite;
}

TEST(IPCClient, TalkUsesSharedLifecycleWithoutAllocatingVideo) {
    IPCPublish mqtt;
    IPCFactory factory;
    factory.talk_supported = true;
    IPCCapability capability;
    capability.microphone = capability.conference_talk = true;
    IPCClient client(mqtt, factory, "up", "down", "instance", capability);
    const auto invite = runtime_talk();
    ipc_activate(client, mqtt, factory, invite.session, &invite);
    ASSERT_EQ(factory.facts.size(), 1U);
    EXPECT_TRUE(factory.facts[0]->invite.video_track.empty());
    EXPECT_EQ(factory.facts[0]->invite.audio_track, "mic-1");
    EXPECT_EQ(factory.facts[0]->invite.speaker_track, "speaker-1");
    const auto accept = std::find_if(mqtt.sent.begin(), mqtt.sent.end(), [](const Envelope& value) {
        return value.method == "sessionAccept";
    });
    ASSERT_NE(accept, mqtt.sent.end());
    EXPECT_EQ(accept->payload, R"({"acceptedTracks":["mic-1","speaker-1"]})");
    EXPECT_TRUE(client.receive("down", ipc_body(invite), 110, 1000000));
    EXPECT_EQ(factory.facts.size(), 1U);
    EXPECT_TRUE(client.end(invite.session, 111));
    EXPECT_EQ(factory.facts[0]->closed, 1);
    EXPECT_FALSE(client.media_connected(invite.session, 112));
    EXPECT_TRUE(client.receive("down", ipc_body(invite), 113, 1000000));
    EXPECT_EQ(factory.facts.size(), 1U);
}

// 扬声器暂停恢复使用原轨控制回执，不能重新采集、分配Peer或改变麦克风授权。
TEST(IPCClient, TalkSpeakerPauseResumeUsesOriginalReceiptAndRejectsReplayConflict) {
    IPCPublish mqtt;
    IPCFactory factory;
    factory.talk_supported = true;
    IPCCapability capability;
    capability.microphone = capability.conference_talk = true;
    IPCClient client(mqtt, factory, "up", "down", "instance", capability);
    const auto invite = runtime_talk();
    ipc_activate(client, mqtt, factory, invite.session, &invite);
    auto command = ipc_source_control("speaker-pause", "pause", 3);
    command.session = invite.session;
    command.payload = R"({"trackId":"speaker-1","operation":"pause","mediaRevision":1})";
    ASSERT_TRUE(client.receive("down", ipc_body(command), 200, 1000000));
    ASSERT_EQ(factory.facts[0]->track_controls, 1);
    ASSERT_TRUE(client.track_applied(invite.session, command.token, "paused", true, 201));
    const auto receipt = mqtt.sent.back();
    EXPECT_EQ(receipt.method, "trackState");
    EXPECT_NE(receipt.payload.find(R"("trackId":"speaker-1")"), std::string::npos);
    ASSERT_TRUE(client.receive("down", ipc_body(command), 202, 1000000));
    EXPECT_EQ(ipc_body(mqtt.sent.back()), ipc_body(receipt));
    EXPECT_EQ(factory.facts[0]->track_controls, 1);
    auto conflict = command;
    conflict.payload = R"({"trackId":"speaker-1","operation":"resume","mediaRevision":1})";
    EXPECT_FALSE(client.receive("down", ipc_body(conflict), 203, 1000000));
    conflict.token = "speaker-resume";
    conflict.event_sequence = 4;
    ASSERT_TRUE(client.receive("down", ipc_body(conflict), 204, 1000000));
    ASSERT_TRUE(client.track_applied(invite.session, conflict.token, "active", true, 205));
    EXPECT_EQ(factory.facts[0]->track_controls, 2);
    EXPECT_EQ(factory.facts.size(), 1U);
    EXPECT_EQ(factory.facts[0]->closed, 0);
    EXPECT_EQ(client.phase(invite.session), IPCPhase::Active);
    EXPECT_EQ(factory.facts[0]->invite.audio_track, "mic-1");
    for (const auto* operation : {"publish", "stop", "subscribe", "unsubscribe"}) {
        conflict.token = std::string("forbidden-") + operation;
        conflict.event_sequence = 5;
        conflict.payload = std::string(R"({"trackId":"speaker-1","operation":")") + operation +
                           R"(","mediaRevision":1})";
        EXPECT_FALSE(client.receive("down", ipc_body(conflict), 206, 1000000));
    }
    EXPECT_EQ(factory.facts[0]->track_controls, 2);
    EXPECT_TRUE(client.end(invite.session, 207));
    EXPECT_FALSE(client.track_applied(invite.session, conflict.token, "active", true, 208));
}

// 收音与播放控制共用既有有界请求，超时只结束对讲腿；迟到成功不能影响仍活跃的观看者。
TEST(IPCClient, TalkSpeakerDriverTimeoutDoesNotEndOtherView) {
    IPCPublish mqtt;
    IPCFactory factory;
    factory.talk_supported = true;
    IPCCapability capability;
    capability.microphone = capability.conference_talk = true;
    IPCClient client(mqtt, factory, "up", "down", "instance", capability);
    ipc_activate(client, mqtt, factory, "a");
    const auto invite = runtime_talk();
    ipc_activate(client, mqtt, factory, invite.session, &invite);
    auto command = ipc_source_control("speaker-pause", "pause", 3);
    command.session = invite.session;
    command.payload = R"({"trackId":"speaker-1","operation":"pause","mediaRevision":1})";
    ASSERT_TRUE(client.receive("down", ipc_body(command), 200, 1000000));
    ASSERT_TRUE(client.receive("down", ipc_body(command), 10199, 1000000));
    client.tick(10200, 1000000);
    EXPECT_EQ(factory.facts[1]->track_controls, 1);
    EXPECT_EQ(factory.facts[1]->closed, 1);
    EXPECT_EQ(client.phase(invite.session), IPCPhase::Ending);
    EXPECT_FALSE(client.track_applied(invite.session, command.token, "paused", true, 10201));
    // 本地驱动已释放，但云端终态必须由原结束请求的真实关联应答确认。
    ASSERT_TRUE(client.receive("down",
                               ipc_body(ipc_reply(mqtt.sent.back(), "sessionEnded",
                                                  R"({"state":"ended","reason":"mediaFailed"})")),
                               10202, 1000000));
    EXPECT_EQ(client.phase(invite.session), IPCPhase::Ended);
    EXPECT_EQ(factory.facts[0]->closed, 0);
    EXPECT_EQ(client.phase("a"), IPCPhase::Active);
}

TEST(IPCClient, TalkAndPreviewHaveIndependentBudgetsAndClose) {
    IPCPublish mqtt;
    IPCFactory factory;
    factory.talk_supported = true;
    IPCCapability capability;
    capability.microphone = capability.conference_talk = true;
    capability.max_publishers = 1;
    IPCClient client(mqtt, factory, "up", "down", "instance", capability);
    ipc_activate(client, mqtt, factory, "view");
    const auto invite = runtime_talk();
    ipc_activate(client, mqtt, factory, invite.session, &invite);
    EXPECT_EQ(client.live_sessions(), 2U);
    EXPECT_FALSE(client.receive("down", ipc_body(ipc_invite("second-view")), 110, 1000000));
    EXPECT_FALSE(client.receive("down", ipc_body(runtime_talk("second-talk")), 110, 1000000));
    EXPECT_TRUE(client.end(invite.session, 111));
    EXPECT_EQ(factory.facts[0]->closed, 0);
    EXPECT_EQ(factory.facts[1]->closed, 1);
    EXPECT_EQ(client.phase("view"), IPCPhase::Active);
    EXPECT_TRUE(client.receive("down", ipc_body(runtime_talk("second-talk")), 112, 1000000));
    EXPECT_EQ(factory.facts.size(), 2U);  // 接听ACK前不新建驱动。
}

TEST(IPCClient, TalkConsentWaitsForLocalAcceptanceAndPlatformAck) {
    IPCPublish mqtt;
    IPCFactory factory;
    factory.talk_supported = true;
    IPCCapability capability;
    capability.microphone = capability.conference_talk = capability.local_accept = true;
    IPCClient client(mqtt, factory, "up", "down", "instance", capability);
    const auto invite = runtime_talk("talk", true);
    ASSERT_TRUE(client.receive("down", ipc_body(invite), 100, 1000000));
    EXPECT_EQ(client.phase("talk"), IPCPhase::Invited);
    EXPECT_TRUE(mqtt.sent.empty());
    EXPECT_TRUE(factory.facts.empty());
    ASSERT_TRUE(client.accept("talk", 101, 1000000));
    EXPECT_TRUE(factory.facts.empty());
    auto ack = ipc_reply(mqtt.sent.back(), "requestAck",
                         R"({"requestMethod":"sessionAccept","result":{"state":"negotiating"}})");
    ack.code = 403;
    ASSERT_TRUE(client.receive("down", ipc_body(ack), 102, 1000000));
    EXPECT_EQ(client.phase("talk"), IPCPhase::Ended);
    EXPECT_TRUE(factory.facts.empty());
    EXPECT_FALSE(client.accept("talk", 103, 1000000));
}

TEST(IPCClient, TalkRequiresBothCapabilityAndDriverAndRejectsExpiredInvite) {
    for (int mode = 0; mode < 3; ++mode) {
        IPCPublish mqtt;
        IPCFactory factory;
        factory.talk_supported = mode != 0;
        IPCCapability capability;
        capability.microphone = true;
        capability.conference_talk = mode != 1;
        IPCClient client(mqtt, factory, "up", "down", "instance", capability);
        EXPECT_FALSE(
            client.receive("down", ipc_body(runtime_talk()), 100, mode == 2 ? 1020000 : 1000000));
        EXPECT_TRUE(mqtt.sent.empty());
        EXPECT_TRUE(factory.facts.empty());
        EXPECT_FALSE(client.phase("conference-leg").has_value());
    }
}
// 名册来自原平台设备腿；成功消费不得关闭媒体、重分配轨或扩大会议话权。
TEST(IPCClientRoster, EstablishedTalkConsumesWithoutMediaAction) {
    IPCPublish mqtt;
    IPCFactory factory;
    factory.talk_supported = true;
    IPCCapability capability;
    capability.microphone = capability.conference_talk = capability.participant_roster = true;
    IPCClient client(mqtt, factory, "up", "down", "instance", capability);
    const auto invite = runtime_talk();
    ipc_activate(client, mqtt, factory, "conference-leg", &invite);
    Envelope member;
    member.token = "roster-3";
    member.method = "participantChanged";
    member.session = "conference-leg";
    member.request = false;
    member.event_sequence = 3;
    member.payload = R"({"participantId":"101","change":"joined","tracks":[]})";
    EXPECT_TRUE(client.receive("down", ipc_body(member), 107, 1000000));
    ASSERT_NE(client.participants("conference-leg"), nullptr);
    ASSERT_EQ(client.participants("conference-leg")->size(), 1U);
    EXPECT_EQ(client.participants("conference-leg")->front().participant_id, "101");
    EXPECT_EQ(client.phase("conference-leg"), IPCPhase::Active);
    EXPECT_EQ(factory.facts.size(), 1U);
    EXPECT_EQ(factory.facts.front()->track_controls, 0);
    EXPECT_EQ(factory.facts.front()->closed, 0);
}
// 原共享水位允许其他信令插入；非法正文不能推进水位、迟到不能撤销新事实。
TEST(IPCClientRoster, InvalidDuplicateLateAndEndAreIsolated) {
    IPCPublish mqtt;
    IPCFactory factory;
    factory.talk_supported = true;
    IPCCapability capability;
    capability.microphone = capability.conference_talk = capability.participant_roster = true;
    IPCClient client(mqtt, factory, "up", "down", "instance", capability);
    const auto invite = runtime_talk();
    ipc_activate(client, mqtt, factory, "conference-leg", &invite);
    Envelope event;
    event.token = "roster";
    event.method = "participantChanged";
    event.session = "conference-leg";
    event.request = false;
    event.event_sequence = 8;
    event.payload = R"({"participantId":"101","change":"joined","tracks":[]})";
    ASSERT_TRUE(client.receive("down", ipc_body(event), 107, 1000000));
    const auto* original_members = client.participants("conference-leg");
    ASSERT_NE(original_members, nullptr);
    EXPECT_EQ(client.participant_sequence("conference-leg"), 8U);
    event.payload = R"({"participantId":"101","change":"left","tracks":[]})";
    ASSERT_TRUE(client.receive("down", ipc_body(event), 108, 1000000));
    ASSERT_EQ(client.participants("conference-leg")->size(), 1U);
    EXPECT_EQ(client.participant_sequence("conference-leg"), 8U);
    event.event_sequence = 7;
    ASSERT_TRUE(client.receive("down", ipc_body(event), 109, 1000000));
    event.event_sequence = 9;
    event.payload = R"({"participantId":"101","change":"updated","tracks":[]})";
    EXPECT_FALSE(client.receive("down", ipc_body(event), 110, 1000000));
    event.payload = R"({"participantId":"101","change":"left","tracks":[]})";
    event.session = "other-leg";
    EXPECT_FALSE(client.receive("down", ipc_body(event), 111, 1000000));
    event.session = "conference-leg";
    EXPECT_FALSE(client.receive("wrong-topic", ipc_body(event), 112, 1000000));
    ASSERT_TRUE(client.receive("down", ipc_body(event), 113, 1000000));
    EXPECT_TRUE(client.participants("conference-leg")->empty());
    EXPECT_EQ(factory.facts.front()->closed, 0);
    ASSERT_TRUE(client.end("conference-leg", 114));
    EXPECT_EQ(client.participants("conference-leg"), nullptr);
    EXPECT_TRUE(original_members->empty());
    event.event_sequence = 10;
    EXPECT_TRUE(client.receive("down", ipc_body(event), 115, 1000000));
    EXPECT_EQ(client.participants("conference-leg"), nullptr);
}
// 平台建立先到时保存只读成员；原连通ACK仍决定Active，不提前开放或重建媒体。
TEST(IPCClientRoster, EstablishedBeforeMediaAckKeepsAcceptedMembers) {
    IPCPublish mqtt;
    IPCFactory factory;
    factory.talk_supported = true;
    IPCCapability capability;
    capability.microphone = capability.conference_talk = capability.participant_roster = true;
    IPCClient client(mqtt, factory, "up", "down", "instance", capability);
    const auto invite = runtime_talk();
    ipc_accept_ack(client, mqtt, factory, "conference-leg", &invite);
    ASSERT_TRUE(client.offer_ready("conference-leg", "v=0\r\ns=test\r\n", 102));
    ASSERT_TRUE(client.receive(
        "down",
        ipc_body(ipc_reply(
            mqtt.sent.back(), "mediaAnswer",
            R"({"mediaRevision":1,"webrtc":{"iceMode":"complete","sdp":"v=0\r\ns=test\r\n"}})")),
        103, 1000000));
    factory.facts.front()->connected = true;
    ASSERT_TRUE(client.media_connected("conference-leg", 104));
    const auto report = mqtt.sent.back();
    Envelope established;
    established.token = "established";
    established.method = "sessionEstablished";
    established.session = "conference-leg";
    established.request = false;
    established.event_sequence = 2;
    established.payload = R"({"state":"established","mediaRevision":1})";
    ASSERT_TRUE(client.receive("down", ipc_body(established), 105, 1000000));
    EXPECT_EQ(client.phase("conference-leg"), IPCPhase::WaitingEstablished);
    established.method = "participantChanged";
    established.event_sequence = 3;
    established.payload = R"({"participantId":"101","change":"joined","tracks":[]})";
    ASSERT_TRUE(client.receive("down", ipc_body(established), 106, 1000000));
    EXPECT_EQ(client.phase("conference-leg"), IPCPhase::WaitingEstablished);
    ASSERT_NE(client.participants("conference-leg"), nullptr);
    EXPECT_EQ(client.participants("conference-leg")->size(), 1U);
    ASSERT_TRUE(client.receive(
        "down",
        ipc_body(ipc_reply(
            report, "requestAck",
            R"({"requestMethod":"mediaState","result":{"state":"connected","mediaRevision":1}})")),
        107, 1000000));
    EXPECT_EQ(client.phase("conference-leg"), IPCPhase::Active);
    EXPECT_EQ(client.participants("conference-leg")->size(), 1U);
    EXPECT_EQ(factory.facts.size(), 1U);
    EXPECT_EQ(factory.facts.front()->track_controls, 0);
}
// 能力关闭及纯预览都不消费会议名册，原媒体保持正常；声明不能被用途隐式推导。
TEST(IPCClientRoster, ExplicitCapabilityAndTalkOnly) {
    for (const bool enabled : {false, true}) {
        IPCPublish mqtt;
        IPCFactory factory;
        factory.talk_supported = true;
        IPCCapability capability;
        capability.microphone = capability.conference_talk = true;
        capability.participant_roster = enabled;
        IPCClient client(mqtt, factory, "up", "down", "instance", capability);
        const auto invitation = enabled ? ipc_invite("view") : runtime_talk();
        const auto session = enabled ? "view" : "conference-leg";
        ipc_activate(client, mqtt, factory, session, &invitation);
        Envelope event;
        event.token = "member";
        event.method = "participantChanged";
        event.session = session;
        event.request = false;
        event.event_sequence = 3;
        event.payload = R"({"participantId":"101","change":"joined","tracks":[]})";
        EXPECT_FALSE(client.receive("down", ipc_body(event), 107, 1000000));
        EXPECT_EQ(client.participants(session), nullptr);
        EXPECT_EQ(client.phase(session), IPCPhase::Active);
        EXPECT_EQ(factory.facts.front()->closed, 0);
    }
    IPCCapability capability;
    Envelope command;
    ASSERT_EQ(ipc_capability_command("cap", capability, command), WireResult::Ok);
    EXPECT_EQ(command.payload.find("participantRoster"), std::string::npos);
    capability.participant_roster = true;
    EXPECT_EQ(ipc_capability_command("cap", capability, command), WireResult::Invalid);
    capability.microphone = capability.conference_talk = true;
    ASSERT_EQ(ipc_capability_command("cap", capability, command), WireResult::Ok);
    EXPECT_NE(command.payload.find("participantRoster"), std::string::npos);
}
// 只有原腿建立后的服务端事件可更新名册；坏信封不能关闭尚在协商的合法媒体。
TEST(IPCClientRoster, EventShapeAndPreEstablishedAreRejectedWithoutSideEffects) {
    IPCPublish mqtt;
    IPCFactory factory;
    factory.talk_supported = true;
    IPCCapability capability;
    capability.microphone = capability.conference_talk = capability.participant_roster = true;
    IPCClient client(mqtt, factory, "up", "down", "instance", capability);
    const auto invite = runtime_talk();
    ipc_accept_ack(client, mqtt, factory, "conference-leg", &invite);
    const std::string event =
        R"({"msgToken":"member","method":"participantChanged","sessionId":"conference-leg","eventSeq":3,"data":{"participantId":"101","change":"joined","tracks":[]}})";
    EXPECT_FALSE(client.receive("down", event, 102, 1000000));
    EXPECT_EQ(client.participants("conference-leg"), nullptr);
    EXPECT_EQ(client.phase("conference-leg"), IPCPhase::Negotiating);
    EXPECT_EQ(factory.facts.front()->closed, 0);
    client.close_all(103);
    EXPECT_EQ(client.participants("conference-leg"), nullptr);
    EXPECT_EQ(factory.facts.front()->closed, 1);

    IPCPublish active_mqtt;
    IPCFactory active_factory;
    active_factory.talk_supported = true;
    IPCClient active(active_mqtt, active_factory, "up", "down", "instance", capability);
    ipc_activate(active, active_mqtt, active_factory, "conference-leg", &invite);
    for (const auto* extra : {"\"code\":200,", "\"messageSeq\":1,"}) {
        std::string invalid = event;
        invalid.insert(1, extra);
        EXPECT_FALSE(active.receive("down", invalid, 107, 1000000));
    }
    std::string request = event;
    request.replace(request.find("\"data\""), 6, "\"params\"");
    EXPECT_FALSE(active.receive("down", request, 107, 1000000));
    std::string no_sequence = event;
    no_sequence.erase(no_sequence.find("\"eventSeq\":3,"), 13);
    EXPECT_FALSE(active.receive("down", no_sequence, 107, 1000000));
    EXPECT_EQ(active.participant_sequence("conference-leg"), 0U);
    EXPECT_TRUE(active.receive("down", event, 108, 1000000));
    EXPECT_EQ(active.participant_sequence("conference-leg"), 3U);
    EXPECT_EQ(active_factory.facts.front()->closed, 0);
    EXPECT_EQ(active_factory.facts.front()->track_controls, 0);
}
// ipc_sync_capability 仅合成完整消费能力，生产默认仍关闭。
IPCCapability ipc_sync_capability() {
    IPCCapability capability;
    capability.revision = 3;
    capability.microphone = capability.conference_talk = capability.participant_roster = true;
    capability.participant_roster_sync = true;
    return capability;
}
// ipc_sync_invitation
// 沿原对讲邀请添加可信公开绑定，不把内部连接epoch写入设备报文。
Envelope ipc_sync_invitation() {
    auto message = runtime_talk();
    message.payload.pop_back();
    message.payload += R"(,"endpointEpoch":7,"capabilityRevision":3})";
    return message;
}
// ipc_sync_snapshot
// 使用完整控制对象和原双轨，不能用局部participants冒充完整应答。
Envelope ipc_sync_snapshot(const Envelope& request, std::uint64_t seq = 8,
                           std::uint64_t expires = 1001200) {
    const std::string tracks =
        R"([{"trackId":"mic-1","kind":"audio","sourceId":"mic","direction":"sendonly","codec":"opus","state":"active"},{"trackId":"speaker-1","kind":"audio","sourceId":"speaker","direction":"recvonly","codec":"opus","state":"active"}])";
    auto reply = ipc_reply(request, "sessionSnapshot", "");
    reply.event_sequence = seq;
    reply.payload =
        R"({"state":"established","revision":7,"eventWatermark":)" + std::to_string(seq) +
        R"(,"messageWatermark":0,"endpointEpoch":7,"mediaRevision":1,"tracks":)" + tracks +
        R"(,"controls":{"call":null,"focus":null,"floor":null,"recordings":[],"playbacks":[],"processors":[],"broadcasts":[],"handoffs":[],"mediaPlan":{"revision":1,"tracks":)" +
        tracks +
        R"(},"ai":null},"capabilityRevision":3,"resumeAllowed":false,"reason":"snapshotOnly","participants":[{"participantId":"101","tracks":[]}],"expiresAt":)" +
        std::to_string(expires) + "}";
    return reply;
}
// IPCSyncHarness 通过原全部媒体ACK建立受控端口，未替代真实端云验收。
struct IPCSyncHarness {
    IPCPublish mqtt;                                   // 原完整编码上行观察。
    IPCFactory factory;                                // 只记录独立原Peer/控制次数。
    IPCCapability capability = ipc_sync_capability();  // 显式测试能力，不默认启用宿主。
    IPCClient client{mqtt, factory, "up", "down", "instance", capability};  // 实际生产状态机。
    IPCSyncHarness() {
        factory.talk_supported = true;
        const auto invitation = ipc_sync_invitation();
        ipc_activate(client, mqtt, factory, invitation.session, &invitation);
    }
};

TEST(IPCClientSync, ExplicitBoundTalkAndLegacyPreviewRemainSeparate) {
    auto capability = ipc_sync_capability();
    IPCInvite preview;
    EXPECT_EQ(decode_ipc_invite(WireRoute::Signaling, ipc_body(ipc_invite("preview")), capability,
                                preview),
              WireResult::Ok);
    IPCTalkInvite talk;
    EXPECT_EQ(
        decode_ipc_talk_invite(WireRoute::Signaling, ipc_body(runtime_talk()), capability, talk),
        WireResult::Invalid);
    ASSERT_EQ(decode_ipc_talk_invite(WireRoute::Signaling, ipc_body(ipc_sync_invitation()),
                                     capability, talk),
              WireResult::Ok);
    EXPECT_EQ(talk.endpoint_epoch, 7U);
    EXPECT_EQ(talk.capability_revision, 3U);
    capability.participant_roster_sync = false;
    EXPECT_EQ(decode_ipc_talk_invite(WireRoute::Signaling, ipc_body(ipc_sync_invitation()),
                                     capability, talk),
              WireResult::Invalid);
    EXPECT_EQ(
        decode_ipc_talk_invite(WireRoute::Signaling, ipc_body(runtime_talk()), capability, talk),
        WireResult::Ok);
}

TEST(IPCClientSync, CompleteSnapshotOriginalRequestAtomicallyReplacesWithoutMediaActions) {
    IPCSyncHarness h;
    ASSERT_TRUE(h.client.sync("conference-leg", 110, 1000000));
    const auto original = h.mqtt.sent.back();
    EXPECT_EQ(original.method, "sessionSync");
    EXPECT_NE(original.payload.find("\"endpointEpoch\":7"), std::string::npos);
    ASSERT_TRUE(h.client.sync("conference-leg", 111, 1000000));
    EXPECT_EQ(h.mqtt.sent.back().token, original.token);
    const auto reply = ipc_body(ipc_sync_snapshot(original));
    ASSERT_TRUE(h.client.receive("down", reply, 112, 1000000));
    ASSERT_NE(h.client.participants("conference-leg"), nullptr);
    ASSERT_EQ(h.client.participants("conference-leg")->size(), 1U);
    EXPECT_EQ(h.client.participants("conference-leg")->front().participant_id, "101");
    EXPECT_EQ(h.client.participant_sequence("conference-leg"), 8U);
    EXPECT_EQ(h.client.sync_state("conference-leg"), RequestState::Succeeded);
    EXPECT_TRUE(h.client.receive("down", reply, 113, 1000000));
    EXPECT_EQ(h.factory.facts.size(), 1U);
    EXPECT_EQ(h.factory.facts.front()->closed, 0);
    EXPECT_EQ(h.factory.facts.front()->track_controls, 0);
}

TEST(IPCClientSync, MalformedWrongOriginalAndLateWatermarkNeverReplaceCurrent) {
    IPCSyncHarness h;
    ASSERT_TRUE(h.client.sync("conference-leg", 110, 1000000));
    const auto request = h.mqtt.sent.back();
    auto wrong = ipc_sync_snapshot(request);
    wrong.token = "another-token";
    EXPECT_FALSE(h.client.receive("down", ipc_body(wrong), 111, 1000000));
    wrong = ipc_sync_snapshot(request);
    wrong.payload = R"({"participants":[]})";
    EXPECT_FALSE(h.client.receive("down", ipc_body(wrong), 111, 1000000));
    EXPECT_EQ(h.client.sync_state("conference-leg"), RequestState::Pending);
    Envelope delta;
    delta.token = "delta-9";
    delta.method = "participantChanged";
    delta.session = "conference-leg";
    delta.request = false;
    delta.event_sequence = 9;
    delta.payload = R"({"participantId":"newer","change":"joined","tracks":[]})";
    ASSERT_TRUE(h.client.receive("down", ipc_body(delta), 112, 1000000));
    EXPECT_FALSE(h.client.receive("down", ipc_body(ipc_sync_snapshot(request)), 113, 1000000));
    EXPECT_EQ(h.client.sync_state("conference-leg"), RequestState::TimedOut);
    ASSERT_EQ(h.client.participants("conference-leg")->size(), 1U);
    EXPECT_EQ(h.client.participants("conference-leg")->front().participant_id, "newer");
    EXPECT_EQ(h.client.participant_sequence("conference-leg"), 9U);
    EXPECT_EQ(h.factory.facts.front()->closed, 0);
}

// 真实DMS入口的固定拒绝必须结束查询；不能忽略400后重复请求，主媒体及名册保持不变。
TEST(IPCClientSync, IngressRejectionTerminatesOnlyOriginalQuery) {
    for (const auto* reason : {"invalidRequest", "unsupportedMethod", "signalingNotEnabled"}) {
        IPCSyncHarness h;
        ASSERT_TRUE(h.client.sync("conference-leg", 110, 1000000));
        const auto request = h.mqtt.sent.back();
        auto reply = ipc_reply(
            request, "sessionSnapshot",
            std::string(R"({"requestMethod":"sessionSync","reason":")") + reason + R"("})");
        reply.code = std::string_view(reason) == "invalidRequest"      ? 400
                     : std::string_view(reason) == "unsupportedMethod" ? 422
                                                                       : 503;
        EXPECT_TRUE(h.client.receive("down", ipc_body(reply), 111, 1000000));
        EXPECT_EQ(h.client.sync_state("conference-leg"), RequestState::Failed);
        EXPECT_EQ(h.client.phase("conference-leg"), IPCPhase::Active);
        EXPECT_EQ(h.factory.facts.front()->closed, 0);
        const auto sent = h.mqtt.sent.size();
        h.client.tick(10110, 1010000);
        EXPECT_EQ(h.mqtt.sent.size(), sent);
        EXPECT_TRUE(h.client.receive("down", ipc_body(reply), 10111, 1010000));
    }
}

TEST(IPCClientSync, WallRollbackMonotonicExpiryAndFutureLeaseAreRejected) {
    for (int scenario = 0; scenario < 3; ++scenario) {
        IPCSyncHarness h;
        ASSERT_TRUE(h.client.sync("conference-leg", 110, 1000000));
        const auto request = h.mqtt.sent.back();
        if (scenario == 0)
            h.client.tick(111, 1001500);
        auto reply = ipc_sync_snapshot(request, 8, scenario == 2 ? 1009000 : 1001200);
        EXPECT_FALSE(h.client.receive("down", ipc_body(reply), scenario == 1 ? 1510 : 112, 999000));
        EXPECT_TRUE(h.client.participants("conference-leg")->empty());
        EXPECT_EQ(h.client.phase("conference-leg"), IPCPhase::Active);
        EXPECT_EQ(h.factory.facts.front()->closed, 0);
    }
}

TEST(IPCClientSync, FixedOriginalRetriesTimeoutDenialAndEndDoNotFakeRecovery) {
    IPCSyncHarness h;
    ASSERT_TRUE(h.client.sync("conference-leg", 110, 1000000));
    const auto original = h.mqtt.sent.back();
    h.client.tick(10110, 1010000);
    EXPECT_EQ(ipc_body(h.mqtt.sent.back()), ipc_body(original));
    h.client.tick(20110, 1020000);
    EXPECT_EQ(ipc_body(h.mqtt.sent.back()), ipc_body(original));
    h.client.tick(30110, 1030000);
    EXPECT_EQ(h.client.sync_state("conference-leg"), RequestState::TimedOut);
    EXPECT_EQ(h.client.phase("conference-leg"), IPCPhase::Active);
    EXPECT_EQ(h.factory.facts.front()->closed, 0);
    ASSERT_TRUE(h.client.sync("conference-leg", 30111, 1030000));
    auto denial = ipc_reply(h.mqtt.sent.back(), "sessionSnapshot",
                            R"({"requestMethod":"sessionSync","reason":"requestDenied"})");
    denial.code = 403;
    ASSERT_TRUE(h.client.receive("down", ipc_body(denial), 30112, 1030000));
    EXPECT_EQ(h.client.sync_state("conference-leg"), RequestState::Failed);
    EXPECT_EQ(h.factory.facts.front()->closed, 0);
    ASSERT_TRUE(h.client.sync("conference-leg", 30113, 1030000));
    EXPECT_TRUE(h.client.end("conference-leg", 30114));
    EXPECT_EQ(h.client.sync_state("conference-leg"), RequestState::Cancelled);
    EXPECT_EQ(h.factory.facts.front()->closed, 1);
}

// ipc_resume_snapshot
// 构造established恢复形态快照：resumeAllowed=true且无原因字段，不伪造媒体授权。
Envelope ipc_resume_snapshot(const Envelope& request, std::uint64_t seq = 8,
                             std::uint64_t expires = 1001200) {
    const std::string tracks =
        R"([{"trackId":"mic-1","kind":"audio","sourceId":"mic","direction":"sendonly","codec":"opus","state":"active"},{"trackId":"speaker-1","kind":"audio","sourceId":"speaker","direction":"recvonly","codec":"opus","state":"active"}])";
    auto reply = ipc_reply(request, "sessionSnapshot", "");
    reply.event_sequence = seq;
    reply.payload =
        R"({"state":"established","revision":7,"eventWatermark":)" + std::to_string(seq) +
        R"(,"messageWatermark":0,"endpointEpoch":7,"mediaRevision":1,"tracks":)" + tracks +
        R"(,"controls":{"call":null,"focus":null,"floor":null,"recordings":[],"playbacks":[],"processors":[],"broadcasts":[],"handoffs":[],"mediaPlan":{"revision":1,"tracks":)" +
        tracks +
        R"(},"ai":null},"capabilityRevision":3,"resumeAllowed":true,"participants":[{"participantId":"101","tracks":[]}],"expiresAt":)" +
        std::to_string(expires) + "}";
    return reply;
}

// ipc_ended_snapshot
// 构造会话过期终止投影：撤空授权轨与名册，触发设备R03本地清理。
Envelope ipc_ended_snapshot(const Envelope& request, std::uint64_t seq = 8,
                            std::uint64_t expires = 1001200) {
    auto reply = ipc_reply(request, "sessionSnapshot", "");
    reply.event_sequence = seq;
    reply.payload =
        R"({"state":"ended","revision":8,"eventWatermark":)" + std::to_string(seq) +
        R"(,"messageWatermark":0,"endpointEpoch":7,"mediaRevision":1,"tracks":[],"controls":{"call":null,"focus":null,"floor":null,"recordings":[],"playbacks":[],"processors":[],"broadcasts":[],"handoffs":[],"mediaPlan":{"revision":1,"tracks":[]},"ai":null},"capabilityRevision":3,"resumeAllowed":false,"reason":"sessionExpired","participants":[],"expiresAt":)" +
        std::to_string(expires) + "}";
    return reply;
}

// ipc_media_updated_applied
// 构造applied独立事件；requestToken与水位由调用者定制。
Envelope ipc_media_updated_applied(std::string token, const std::string& request_token,
                                   std::uint64_t sequence) {
    Envelope event;
    event.request = false;
    event.token = std::move(token);
    event.method = "mediaUpdated";
    event.session = "conference-leg";
    event.event_sequence = sequence;
    event.payload =
        R"({"state":"applied","revision":1,"mediaRevision":1,"tracks":[{"trackId":"mic-1","kind":"audio","sourceId":"mic","direction":"sendonly","codec":"opus","state":"active"}],"requestToken":")" +
        request_token + R"("})";
    return event;
}

// ipc_quality_pending 构造原请求的pending，仅模拟云端受理，不代表实际驱动执行。
Envelope ipc_quality_pending(const Envelope& request) {
    return ipc_reply(
        request, "mediaUpdated",
        R"({"state":"pending","revision":1,"mediaRevision":1,"tracks":[{"trackId":"mic-1","kind":"audio","sourceId":"mic","direction":"sendonly","codec":"opus","state":"active"}]})");
}
// ipc_quality_failed 构造原计划失败事件，失败不能借成功字段或恢复主媒体。
Envelope ipc_quality_failed(const Envelope& request, std::uint64_t sequence) {
    auto event = ipc_media_updated_applied("failed", request.token, sequence);
    event.payload = R"({"state":"failed","revision":1,"requestToken":")" + request.token +
                    R"(","reason":"qualityTimeout"})";
    return event;
}
// 原计划超时期间保留异步槽，云端failed后迟到驱动只释放槽，不能补发虚假的成功。
TEST(IPCClientUpdate, TimeoutKeepsCallAndSerializesLateDriver) {
    IPCSyncHarness h;
    ASSERT_TRUE(h.client.media_update("conference-leg", "mic-1", "low", 110));
    const auto request = h.mqtt.sent.back();
    ASSERT_TRUE(h.client.receive("down", ipc_body(ipc_quality_pending(request)), 111, 1000000));
    h.client.tick(10110, 1000000);
    ASSERT_EQ(h.mqtt.sent.back().method, "trackState");
    EXPECT_NE(h.mqtt.sent.back().payload.find("qualityTimeout"), std::string::npos);
    EXPECT_FALSE(h.client.media_update("conference-leg", "mic-1", "high", 10111));
    ASSERT_TRUE(h.client.receive("down", ipc_body(ipc_quality_failed(request, 3)), 10112, 1000000));
    EXPECT_FALSE(h.client.media_update("conference-leg", "mic-1", "high", 10113));
    const auto before = h.mqtt.sent.size();
    ASSERT_TRUE(h.client.quality_applied("conference-leg", request.token, "active", true, 10114));
    EXPECT_EQ(h.mqtt.sent.size(), before);
    EXPECT_EQ(h.client.phase("conference-leg"), IPCPhase::Active);
    EXPECT_EQ(h.factory.facts.front()->closed, 0);
    EXPECT_TRUE(h.client.media_update("conference-leg", "mic-1", "high", 10115));
}
// 不支持的质量调整明确上报失败，等待原计划终态，不关闭对讲。
TEST(IPCClientUpdate, UnsupportedDriverReportsFailureOnly) {
    IPCSyncHarness h;
    h.factory.facts.front()->quality_supported = false;
    ASSERT_TRUE(h.client.media_update("conference-leg", "mic-1", "low", 110));
    const auto request = h.mqtt.sent.back();
    ASSERT_TRUE(h.client.receive("down", ipc_body(ipc_quality_pending(request)), 111, 1000000));
    EXPECT_EQ(h.mqtt.sent.back().method, "trackState");
    EXPECT_NE(h.mqtt.sent.back().payload.find("qualityUnsupported"), std::string::npos);
    EXPECT_EQ(h.factory.facts.front()->closed, 0);
    EXPECT_EQ(h.client.phase("conference-leg"), IPCPhase::Active);
    ASSERT_TRUE(h.client.receive("down", ipc_body(ipc_quality_failed(request, 3)), 112, 1000000));
    EXPECT_TRUE(h.client.media_update("conference-leg", "mic-1", "high", 113));
}
// 云端终态和实态ACK全部丢失仍有本地未知收敛；只释放已完成驱动，不冒称平台applied。
TEST(IPCClientUpdate, LostTerminalReleasesCompletedDriverAsUnknown) {
    IPCSyncHarness h;
    ASSERT_TRUE(h.client.media_update("conference-leg", "mic-1", "low", 110));
    const auto request = h.mqtt.sent.back();
    ASSERT_TRUE(h.client.receive("down", ipc_body(ipc_quality_pending(request)), 111, 1000000));
    ASSERT_TRUE(h.client.quality_applied("conference-leg", request.token, "active", true, 112));
    h.client.tick(40109, 1000000);
    EXPECT_FALSE(h.client.media_update("conference-leg", "mic-1", "high", 40109));
    h.client.tick(40110, 1000000);
    EXPECT_EQ(h.client.phase("conference-leg"), IPCPhase::Active);
    EXPECT_EQ(h.factory.facts.front()->closed, 0);
    ASSERT_TRUE(h.client.media_update("conference-leg", "mic-1", "high", 40111));
    const auto next = h.mqtt.sent.back();
    EXPECT_FALSE(h.client.receive(
        "down", ipc_body(ipc_media_updated_applied("old-terminal", request.token, 10)),
        40112, 1000000));
    EXPECT_FALSE(h.client.media_update("conference-leg", "mic-1", "standard", 40113));
    EXPECT_EQ(h.mqtt.sent.back().token, next.token);
    EXPECT_EQ(h.factory.facts.front()->qualities.size(), 1U);
}
// 未知时限不能绕过仍在执行的旧驱动，必须等真实回调再允许下一计划。
TEST(IPCClientUpdate, LostTerminalNeverOverlapsUnfinishedDriver) {
    IPCSyncHarness h;
    ASSERT_TRUE(h.client.media_update("conference-leg", "mic-1", "low", 110));
    const auto request = h.mqtt.sent.back();
    ASSERT_TRUE(h.client.receive("down", ipc_body(ipc_quality_pending(request)), 111, 1000000));
    h.client.tick(40110, 1000000);
    EXPECT_FALSE(h.client.media_update("conference-leg", "mic-1", "high", 40111));
    const auto before = h.mqtt.sent.size();
    ASSERT_TRUE(h.client.quality_applied("conference-leg", request.token, "active", true, 40112));
    EXPECT_EQ(h.mqtt.sent.size(), before);
    h.client.tick(40113, 1000000);
    EXPECT_TRUE(h.client.media_update("conference-leg", "mic-1", "high", 40114));
}
// 409没有state字段，核心必须仍能关联并释放原槽；不能误判成媒体故障。
TEST(IPCClientUpdate, BusyConflictAlignsRevisionWithoutClosing) {
    IPCSyncHarness h;
    ASSERT_TRUE(h.client.media_update("conference-leg", "mic-1", "low", 110));
    const auto request = h.mqtt.sent.back();
    auto conflict =
        ipc_reply(request, "mediaUpdated", R"({"reason":"updatePending","currentRevision":2})");
    conflict.code = 409;
    ASSERT_TRUE(h.client.receive("down", ipc_body(conflict), 111, 1000000));
    EXPECT_TRUE(h.factory.facts.front()->qualities.empty());
    ASSERT_TRUE(h.client.media_update("conference-leg", "mic-1", "high", 112));
    EXPECT_NE(h.mqtt.sent.back().payload.find(R"("expectedRevision":2)"), std::string::npos);
    EXPECT_EQ(h.client.phase("conference-leg"), IPCPhase::Active);
}
// 不相关的更高事件水位不能使已回报计划永久占槽；原迟到终态只收敛关联，不回滚水位。
TEST(IPCClientUpdate, DelayedTerminalBelowRosterWatermarkReleasesSlot) {
    IPCSyncHarness h;
    ASSERT_TRUE(h.client.media_update("conference-leg", "mic-1", "low", 110));
    const auto request = h.mqtt.sent.back();
    ASSERT_TRUE(h.client.receive("down", ipc_body(ipc_quality_pending(request)), 111, 1000000));
    ASSERT_TRUE(h.client.quality_applied("conference-leg", request.token, "active", true, 112));
    ASSERT_TRUE(h.client.receive(
        "down",
        R"({"msgToken":"roster","method":"participantChanged","sessionId":"conference-leg","eventSeq":9,"data":{"participantId":"101","change":"joined","tracks":[]}})",
        113, 1000000));
    ASSERT_TRUE(h.client.receive(
        "down", ipc_body(ipc_media_updated_applied("delayed", request.token, 3)), 114, 1000000));
    EXPECT_EQ(h.client.participant_sequence("conference-leg"), 9U);
    EXPECT_TRUE(h.client.media_update("conference-leg", "mic-1", "high", 115));
}

// 弱网降质顺序：pending后只启动驱动，实际完成先上报，随后才接受平台applied。
TEST(IPCClientUpdate, DriverFactPrecedesCloudApplied) {
    IPCSyncHarness h;
    ASSERT_TRUE(h.client.media_update("conference-leg", "mic-1", "low", 110));
    const auto update = h.mqtt.sent.back();
    EXPECT_EQ(update.method, "mediaUpdate");
    EXPECT_NE(update.payload.find(R"("operation":"setQuality")"), std::string::npos);
    EXPECT_NE(update.payload.find(R"("expectedRevision":0)"), std::string::npos);
    EXPECT_NE(update.payload.find(R"("trackIds":["mic-1"])"), std::string::npos);
    EXPECT_NE(update.payload.find(R"("profile":"low")"), std::string::npos);
    // 在途修订期间拒绝第二条，避免旧代次副作用串行冲突。
    EXPECT_FALSE(h.client.media_update("conference-leg", "mic-1", "high", 110));
    auto pending = ipc_reply(update, "mediaUpdated",
                             R"({"state":"pending","revision":1,"mediaRevision":1,"tracks":[{"trackId":"mic-1","kind":"audio","sourceId":"mic","direction":"sendonly","codec":"opus","state":"active"}]})");
    ASSERT_TRUE(h.client.receive("down", ipc_body(pending), 111, 1000000));
    ASSERT_TRUE(h.client.receive("down", ipc_body(pending), 111, 1000000));
    ASSERT_EQ(h.factory.facts.front()->qualities.size(), 1U);
    EXPECT_EQ(h.mqtt.sent.back().method, "mediaUpdate") << "驱动尚未完成，不能编造active上报";
    EXPECT_FALSE(h.client.receive(
        "down", ipc_body(ipc_media_updated_applied("too-early", update.token, 3)), 112, 1000000));
    EXPECT_FALSE(h.client.quality_applied("conference-leg", "other-token", "active", true, 112));
    EXPECT_FALSE(h.client.quality_applied(update.token, "conference-leg", "active", true, 112));
    EXPECT_FALSE(h.client.quality_applied("conference-leg", "active", update.token, true, 112));
    ASSERT_TRUE(h.client.quality_applied("conference-leg", update.token, "active", true, 112));
    EXPECT_FALSE(h.client.quality_applied("conference-leg", update.token, "active", true, 112));
    ASSERT_TRUE(h.client.receive(
        "down", ipc_body(ipc_media_updated_applied("applied-1", update.token, 3)), 112, 1000000));
    const auto report = h.mqtt.sent.back();
    EXPECT_EQ(report.method, "trackState");
    EXPECT_FALSE(report.request) << "实态上报是无code独立事件，不占用原token应答";
    EXPECT_NE(report.payload.find(R"("trackId":"mic-1")"), std::string::npos);
    EXPECT_NE(report.payload.find(R"("state":"active")"), std::string::npos);
    EXPECT_NE(report.payload.find(R"("mediaRevision":1)"), std::string::npos);
    EXPECT_NE(report.payload.find(R"("requestToken":")" + update.token + "\""), std::string::npos);
    EXPECT_NE(report.payload.find(R"("revision":1)"), std::string::npos);
    // 计划修订已对齐为1：下一轮修订携带expectedRevision=1，水位继续使用新事件。
    ASSERT_TRUE(h.client.media_update("conference-leg", "mic-1", "standard", 113));
    EXPECT_NE(h.mqtt.sent.back().payload.find(R"("expectedRevision":1)"), std::string::npos);
}

// 迟到低水位applied只忽略不回滚；未关联token或旧代次媒体不能推进本端计划。
TEST(IPCClientUpdate, LateLowWatermarkAndUnknownAssociationNeverRollBack) {
    IPCSyncHarness h;
    ASSERT_TRUE(h.client.media_update("conference-leg", "mic-1", "low", 110));
    const auto update = h.mqtt.sent.back();
    auto pending = ipc_reply(update, "mediaUpdated",
                             R"({"state":"pending","revision":1,"mediaRevision":1,"tracks":[{"trackId":"mic-1","kind":"audio","sourceId":"mic","direction":"sendonly","codec":"opus","state":"active"}]})");
    ASSERT_TRUE(h.client.receive("down", ipc_body(pending), 111, 1000000));
    ASSERT_TRUE(h.client.quality_applied("conference-leg", update.token, "active", true, 112));
    ASSERT_TRUE(h.client.receive(
        "down", ipc_body(ipc_media_updated_applied("applied-9", update.token, 9)), 112, 1000000));
    EXPECT_EQ(h.mqtt.sent.back().method, "trackState");
    auto late = ipc_media_updated_applied("applied-late", update.token, 3);
    ASSERT_TRUE(h.client.receive("down", ipc_body(late), 113, 1000000)) << "低水位迟到只忽略";
    EXPECT_EQ(h.mqtt.sent.back().method, "trackState") << "迟到applied不得重复触发实态上报";
    // 旧代次媒体（mediaRevision=2）整体拒绝：不推进水位、不消费在途关联。
    auto stale = ipc_media_updated_applied("applied-stale", update.token, 10);
    stale.payload.replace(stale.payload.find(R"("mediaRevision":1)"), 16, R"("mediaRevision":2)");
    EXPECT_FALSE(h.client.receive("down", ipc_body(stale), 114, 1000000));
    auto unknown = ipc_media_updated_applied("applied-unknown", "other-request", 11);
    EXPECT_FALSE(h.client.receive("down", ipc_body(unknown), 115, 1000000));
    // 本地计划修订保持1：下一轮修订仍对齐平台已确认值。
    ASSERT_TRUE(h.client.media_update("conference-leg", "mic-1", "high", 116));
    EXPECT_NE(h.mqtt.sent.back().payload.find(R"("expectedRevision":1)"), std::string::npos)
        << "迟到低水位或未知关联不得回滚计划修订";
    EXPECT_FALSE(h.client.media_update("conference-leg", "other-track", "low", 117));
    EXPECT_FALSE(h.client.media_update("conference-leg", "mic-1", "ultra", 118));
}

// 恢复形态快照安装展示但不得触发媒体动作；能力之外仍拒绝。
TEST(IPCClientUpdate, ResumeFormSnapshotInstallsWithoutMediaActions) {
    IPCSyncHarness h;
    ASSERT_TRUE(h.client.sync("conference-leg", 110, 1000000));
    const auto request = h.mqtt.sent.back();
    ASSERT_TRUE(h.client.receive("down", ipc_body(ipc_resume_snapshot(request)), 111, 1000000));
    ASSERT_NE(h.client.participants("conference-leg"), nullptr);
    EXPECT_EQ(h.client.participant_sequence("conference-leg"), 8U);
    EXPECT_EQ(h.client.sync_state("conference-leg"), RequestState::Succeeded);
    EXPECT_EQ(h.client.phase("conference-leg"), IPCPhase::Active);
    EXPECT_EQ(h.factory.facts.front()->closed, 0);
    EXPECT_EQ(h.factory.facts.front()->track_controls, 0) << "恢复许可不等于已恢复媒体";
}

// 平台终态投影触发R03清理：Peer关闭、名册与在途查询全清，重放幂等不重复副作用。
TEST(IPCClientUpdate, EndedSnapshotTriggersR03CleanupIdempotent) {
    IPCSyncHarness h;
    ASSERT_TRUE(h.client.sync("conference-leg", 110, 1000000));
    const auto request = h.mqtt.sent.back();
    const auto ended = ipc_ended_snapshot(request);
    ASSERT_TRUE(h.client.receive("down", ipc_body(ended), 111, 1000000));
    EXPECT_EQ(h.client.phase("conference-leg"), IPCPhase::Ended);
    EXPECT_EQ(h.factory.facts.front()->closed, 1);
    EXPECT_EQ(h.client.participants("conference-leg"), nullptr);
    EXPECT_EQ(h.client.sync_state("conference-leg"), RequestState::Succeeded);
    EXPECT_FALSE(h.client.media_update("conference-leg", "mic-1", "low", 112));
    EXPECT_FALSE(h.client.media_connected("conference-leg", 113));
    ASSERT_TRUE(h.client.receive("down", ipc_body(ended), 114, 1000000));
    EXPECT_EQ(h.factory.facts.front()->closed, 1) << "终态重放不重复执行清理副作用";
    EXPECT_TRUE(h.mqtt.sent.empty() || h.mqtt.sent.back().method != "sessionEnd")
        << "终止快照由平台权威下发，设备不再重复请求结束";
}
}  // namespace
}  // namespace ur::device
