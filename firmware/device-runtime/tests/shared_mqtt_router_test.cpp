// 文件说明：唯一MQTT宿主借用路由的作用域、背压、提前事件及迟到围栏回归，不创建网络连接。
#include "ur/device/shared_mqtt_router.hpp"
#include <gtest/gtest.h>
#include <string>
#include <vector>
#include "ur/device/context_snapshot.hpp"
#include "ur/device/envelope.hpp"
#include "ur/device/voice_wire.hpp"

namespace ur::device {
namespace {
// Publisher 只保存实际发布次数；false模拟原宿主背压，不提供任何远端执行事实。
class Publisher final : public PublishPort {
public:
    bool publish(std::string_view topic, std::string_view body) override {
        topics.emplace_back(topic);
        bodies.emplace_back(body);
        return accepted;
    }
    bool accepted = true;             // 本地是否接纳，不是云端ACK。
    std::vector<std::string> topics;  // 原发布Topic，测试不输出设备身份。
    std::vector<std::string> bodies;  // 不可变原发布字节。
};
// wire 使用正式编码器生成请求/应答，不绕过信封校验。
std::string wire(std::string token, std::string method, std::string session = {},
                 bool request = true, std::string payload = "{}", int code = 0,
                 std::uint64_t sequence = 0, bool message = false) {
    Envelope envelope;
    envelope.token = std::move(token);
    envelope.method = std::move(method);
    envelope.session = std::move(session);
    envelope.request = request;
    envelope.payload = std::move(payload);
    if (code != 0)
        envelope.code = code;
    if (sequence != 0) {
        if (message)
            envelope.message_sequence = sequence;
        else
            envelope.event_sequence = sequence;
    }
    std::string result;
    EXPECT_EQ(encode_envelope(envelope, result), DecodeResult::Ok);
    return result;
}
// Fixture 固定公开测试身份；时间与网络结果由本地测试控制。
class RouterTest : public ::testing::Test {
protected:
    Publisher publisher;
    SharedMqttRouter router{publisher, "test-product/test-device"};
    const std::string up = "$thing/up/signaling/test-product/test-device";
    const std::string down = "$thing/down/signaling/test-product/test-device";
    const std::string message_up = "$thing/up/message/test-product/test-device";
    const std::string message_down = "$thing/down/message/test-product/test-device";
    void SetUp() override { ASSERT_TRUE(router.tick(100)); }
    // establish 由真实已消费创建应答之后显式确认，不以原code200自动绑定。
    void establish(SharedMqttOwner owner, const std::string& token, const std::string& session,
                   std::uint64_t now = 101) {
        ASSERT_TRUE(router.port(owner).publish(up, wire(token, "sessionCreate")));
        ASSERT_EQ(
            router.ingest(down, wire(token, "sessionCreated", session, false, "{}", 200), now),
            SharedRouteResult::Routed);
        ASSERT_TRUE(router.take_message(owner));
        ASSERT_EQ(router.confirm_session(owner, SharedSessionBinding{token, session}, now),
                  SharedRouteResult::Routed);
    }
};
TEST_F(RouterTest, BorrowedPortCannotOwnNetwork) {
    auto& port = router.port(SharedMqttOwner::AI);
    EXPECT_FALSE(port.connect({}));
    EXPECT_EQ(port.subscribe(down), 0);
    EXPECT_FALSE(port.poll());
    EXPECT_FALSE(port.take_event());
    EXPECT_FALSE(port.publications_drained());
    port.disconnect();
    EXPECT_FALSE(router.closed());
    EXPECT_TRUE(publisher.bodies.empty());
}
TEST_F(RouterTest, BackpressurePreservesOriginalOwnerAndBytes) {
    const auto body = wire("same", "sessionCreate");
    publisher.accepted = false;
    EXPECT_FALSE(router.port(SharedMqttOwner::AI).publish(up, body));
    EXPECT_FALSE(router.port(SharedMqttOwner::Human).publish(up, body));
    EXPECT_FALSE(router.port(SharedMqttOwner::AI)
                     .publish(up, wire("same", "sessionCreate", {}, true, "{\"changed\":true}")));
    publisher.accepted = true;
    EXPECT_TRUE(router.port(SharedMqttOwner::AI).publish(up, body));
    ASSERT_EQ(publisher.bodies.size(), 2U);
    EXPECT_EQ(publisher.bodies[0], publisher.bodies[1]);
}
TEST_F(RouterTest, CreationDoesNotBindUntilOwnerConsumesAndConfirms) {
    ASSERT_TRUE(router.port(SharedMqttOwner::AI).publish(up, wire("create", "sessionCreate")));
    ASSERT_EQ(router.ingest(down, wire("create", "sessionCreated", "ai", false, "{}", 200), 101),
              SharedRouteResult::Routed);
    EXPECT_EQ(
        router.confirm_session(SharedMqttOwner::AI, SharedSessionBinding{"create", "ai"}, 101),
        SharedRouteResult::Rejected);
    EXPECT_EQ(
        router.ingest(message_down, wire("early", "textDelta", "ai", false, "{}", 0, 1, true), 101),
        SharedRouteResult::Waiting);
    ASSERT_TRUE(router.take_message(SharedMqttOwner::AI));
    EXPECT_EQ(
        router.confirm_session(SharedMqttOwner::Human, SharedSessionBinding{"create", "ai"}, 101),
        SharedRouteResult::Rejected);
    EXPECT_EQ(
        router.confirm_session(SharedMqttOwner::AI, SharedSessionBinding{"create", "other"}, 101),
        SharedRouteResult::Rejected);
    ASSERT_EQ(
        router.confirm_session(SharedMqttOwner::AI, SharedSessionBinding{"create", "ai"}, 101),
        SharedRouteResult::Routed);
    auto early = router.take_message(SharedMqttOwner::AI);
    ASSERT_TRUE(early);
    EXPECT_EQ(early->topic, message_down);
    EXPECT_FALSE(router.take_message(SharedMqttOwner::Human));
}
TEST_F(RouterTest, RepliesRequireOriginalMethodTopicTokenAndSession) {
    establish(SharedMqttOwner::AI, "create", "ai");
    ASSERT_TRUE(
        router.port(SharedMqttOwner::AI).publish(message_up, wire("input", "inputSend", "ai")));
    const auto ack =
        wire("input", "requestAck", "ai", false, "{\"requestMethod\":\"inputSend\"}", 200);
    EXPECT_EQ(router.ingest(down, ack, 102), SharedRouteResult::Rejected);
    EXPECT_EQ(router.ingest(message_down,
                            wire("input", "requestAck", "wrong", false,
                                 "{\"requestMethod\":\"inputSend\"}", 200),
                            102),
              SharedRouteResult::Rejected);
    EXPECT_EQ(router.ingest(message_down,
                            wire("input", "requestAck", "ai", false,
                                 "{\"requestMethod\":\"respCancel\"}", 200),
                            102),
              SharedRouteResult::Rejected);
    EXPECT_EQ(router.ingest(message_down, ack, 102), SharedRouteResult::Routed);
    ASSERT_TRUE(router.take_message(SharedMqttOwner::AI));
    EXPECT_EQ(router.ingest(message_down, ack, 103), SharedRouteResult::Routed);
    EXPECT_EQ(router.ingest(message_down,
                            wire("input", "requestAck", "ai", false,
                                 "{\"requestMethod\":\"inputSend\"}", 409),
                            103),
              SharedRouteResult::Rejected);
}
TEST_F(RouterTest, IndependentLegsAndToolsAreNotBroadcast) {
    establish(SharedMqttOwner::AI, "ai-create", "ai");
    establish(SharedMqttOwner::Human, "human-create", "human");
    EXPECT_EQ(router.ingest(message_down,
                            wire("tool", "toolCallExecute", "ai", false, "{}", 0, 1, true), 102),
              SharedRouteResult::Routed);
    EXPECT_TRUE(router.take_message(SharedMqttOwner::AI));
    EXPECT_FALSE(router.take_message(SharedMqttOwner::Human));
    EXPECT_FALSE(router.take_message(SharedMqttOwner::Global));
    EXPECT_EQ(
        router.ingest(down, wire("member", "participantChanged", "human", false, "{}", 0, 1), 102),
        SharedRouteResult::Routed);
    EXPECT_TRUE(router.take_message(SharedMqttOwner::Human));
    EXPECT_FALSE(router.port(SharedMqttOwner::AI).publish(up, wire("end", "sessionEnd", "human")));
}
TEST_F(RouterTest, HumanInviteRequiresRootPurposeAndExplicitConfirmation) {
    const auto invite =
        wire("invite", "sessionInvite", "human", true, "{\"purpose\":\"humanCall\"}");
    EXPECT_EQ(router.ingest(down,
                            wire("bad", "sessionInvite", "wrong", true,
                                 "{\"nested\":{\"purpose\":\"humanCall\"}}"),
                            101),
              SharedRouteResult::Rejected);
    EXPECT_EQ(
        router.ingest(down, wire("ipc", "sessionInvite", "ipc", true, "{\"purpose\":\"ipcTalk\"}"),
                      101),
        SharedRouteResult::Rejected);
    ASSERT_EQ(router.ingest(down, invite, 101), SharedRouteResult::Routed);
    EXPECT_FALSE(
        router.port(SharedMqttOwner::Human).publish(up, wire("invite", "sessionAccept", "human")));
    ASSERT_TRUE(router.take_message(SharedMqttOwner::Human));
    ASSERT_EQ(router.confirm_session(SharedMqttOwner::Human,
                                     SharedSessionBinding{"invite", "human"}, 101),
              SharedRouteResult::Routed);
    EXPECT_TRUE(
        router.port(SharedMqttOwner::Human).publish(up, wire("invite", "sessionAccept", "human")));
    EXPECT_EQ(router.ingest(down, invite, 102), SharedRouteResult::Routed);
    EXPECT_EQ(router.ingest(down,
                            wire("invite", "sessionInvite", "human", true,
                                 "{\"purpose\":\"humanCall\",\"different\":true}"),
                            102),
              SharedRouteResult::Rejected);
}
TEST_F(RouterTest, ExpiredEarlyEventIsExplicitlyUnroutableNotDelivered) {
    const auto event = wire("early", "sessionEstablished", "unknown", false, "{}", 0, 1);
    EXPECT_EQ(router.ingest(down, event, 100), SharedRouteResult::Waiting);
    ASSERT_TRUE(router.tick(5100));
    auto expired = router.take_unroutable();
    ASSERT_TRUE(expired);
    EXPECT_EQ(expired->reason, SharedUnroutableReason::Expired);
    EXPECT_EQ(expired->message.body, event);
    EXPECT_FALSE(router.take_message(SharedMqttOwner::AI));
}
TEST_F(RouterTest, RetirementAndDisconnectDoNotLeakLateEventsToNewOwner) {
    establish(SharedMqttOwner::AI, "create", "ai");
    EXPECT_EQ(
        router.ingest(message_down, wire("text", "textDelta", "ai", false, "{}", 0, 1, true), 102),
        SharedRouteResult::Routed);
    ASSERT_TRUE(router.retire_session(SharedMqttOwner::AI, "ai", 103));
    ASSERT_TRUE(router.take_unroutable());
    EXPECT_EQ(
        router.ingest(message_down, wire("late", "textDelta", "ai", false, "{}", 0, 2, true), 104),
        SharedRouteResult::Rejected);
    EXPECT_FALSE(router.port(SharedMqttOwner::AI).publish(up, wire("late", "sessionEnd", "ai")));
    router.close();
    EXPECT_EQ(router.ingest(down, "{}", 104), SharedRouteResult::Closed);
    EXPECT_FALSE(router.port(SharedMqttOwner::Human).publish(up, wire("new", "sessionCreate")));
}
TEST_F(RouterTest, ClockRollbackClosesWithoutNetworkMutation) {
    EXPECT_TRUE(router.tick(200));
    EXPECT_FALSE(router.tick(199));
    EXPECT_TRUE(router.closed());
    EXPECT_TRUE(publisher.bodies.empty());
}
TEST_F(RouterTest, StrictIdentityEnvelopeAndFrameBounds) {
    EXPECT_EQ(router.ingest("$thing/down/signaling/other/device", "{}", 101),
              SharedRouteResult::Rejected);
    EXPECT_EQ(router.ingest(down, std::string(65537, 'x'), 101), SharedRouteResult::Rejected);
    EXPECT_EQ(router.ingest(down, "{\"method\":\"x\",\"method\":\"x\"}", 101),
              SharedRouteResult::Rejected);
    EXPECT_FALSE(router.port(SharedMqttOwner::AI)
                     .publish("$thing/up/signaling/test-product/+", wire("x", "sessionCreate")));
    SharedMqttRouter invalid(publisher, "test-product/+");
    EXPECT_TRUE(invalid.closed());
}
TEST_F(RouterTest, EarlyQueueFullDoesNotConsumeUnacceptedFrame) {
    for (unsigned i = 0; i < 16; ++i) {
        ASSERT_EQ(router.ingest(down,
                                wire("e" + std::to_string(i), "sessionEstablished", "new", false,
                                     "{}", 0, i + 1),
                                100),
                  SharedRouteResult::Waiting);
    }
    const auto frame = wire("overflow", "sessionEstablished", "new", false, "{}", 0, 17);
    EXPECT_EQ(router.ingest(down, frame, 100), SharedRouteResult::Full);
    ASSERT_TRUE(router.tick(5100));
    for (unsigned i = 0; i < 16; ++i)
        ASSERT_TRUE(router.take_unroutable());
    EXPECT_FALSE(router.take_unroutable());
    EXPECT_EQ(router.ingest(down, frame, 5100), SharedRouteResult::Waiting);
}
TEST_F(RouterTest, GlobalCapabilityAndActionAreExclusive) {
    EXPECT_FALSE(router.port(SharedMqttOwner::AI).publish(up, wire("cap", "capabilityReport")));
    ASSERT_TRUE(router.port(SharedMqttOwner::Global).publish(up, wire("cap", "capabilityReport")));
    EXPECT_EQ(router.ingest(down,
                            wire("cap", "requestAck", {}, false,
                                 "{\"requestMethod\":\"capabilityReport\"}", 200),
                            101),
              SharedRouteResult::Routed);
    EXPECT_TRUE(router.take_message(SharedMqttOwner::Global));
    const std::string action_down = "$thing/down/action/test-product/test-device";
    const std::string action_up = "$thing/up/action/test-product/test-device";
    const std::string command =
        "{\"method\":\"action\",\"msgToken\":\"action\",\"actionID\":\"test\",\"params\":{}}";
    EXPECT_EQ(router.ingest(action_down, command, 102), SharedRouteResult::Routed);
    EXPECT_TRUE(router.take_message(SharedMqttOwner::Global));
    const std::string reply =
        "{\"method\":\"actionReply\",\"msgToken\":\"action\",\"actionID\":\"test\",\"code\":200,"
        "\"msg\":\"\",\"data\":{}}";
    EXPECT_FALSE(router.port(SharedMqttOwner::AI).publish(action_up, reply));
    EXPECT_TRUE(router.port(SharedMqttOwner::Global).publish(action_up, reply));
    EXPECT_FALSE(router.port(SharedMqttOwner::Global)
                     .publish(action_up,
                              "{\"method\":\"actionReply\",\"msgToken\":\"other\",\"actionID\":"
                              "\"test\",\"code\":200,\"msg\":\"\",\"data\":{}}"));
}
TEST_F(RouterTest, DuplicateCreationReplyAfterBindingStaysWithOriginalOwner) {
    establish(SharedMqttOwner::AI, "create", "ai");
    EXPECT_EQ(router.ingest(down, wire("create", "sessionCreated", "ai", false, "{}", 200), 102),
              SharedRouteResult::Routed);
    EXPECT_TRUE(router.take_message(SharedMqttOwner::AI));
    EXPECT_FALSE(router.take_message(SharedMqttOwner::Human));
}
TEST_F(RouterTest, DuplicateEarlyEventDoesNotConsumeCapacityOrRenewDeadline) {
    const auto early = wire("early", "sessionEstablished", "new", false, "{}", 0, 1);
    ASSERT_EQ(router.ingest(down, early, 100), SharedRouteResult::Waiting);
    for (unsigned i = 0; i < 20; ++i) {
        EXPECT_EQ(router.ingest(down, early, 5099), SharedRouteResult::Waiting);
    }
    ASSERT_TRUE(router.tick(5100));
    ASSERT_TRUE(router.take_unroutable());
    EXPECT_FALSE(router.take_unroutable());
}
TEST_F(RouterTest, BusyRejectCanReplyToDeliveredOffLegInviteWithoutBinding) {
    establish(SharedMqttOwner::Human, "create", "active");
    const auto invite =
        wire("busy", "sessionInvite", "candidate", true, "{\"purpose\":\"humanCall\"}");
    ASSERT_EQ(router.ingest(down, invite, 102), SharedRouteResult::Routed);
    EXPECT_FALSE(router.port(SharedMqttOwner::Human)
                     .publish(up, wire("busy", "sessionReject", "candidate")));
    ASSERT_TRUE(router.take_message(SharedMqttOwner::Human));
    EXPECT_TRUE(router.port(SharedMqttOwner::Human)
                    .publish(up, wire("busy", "sessionReject", "candidate")));
    EXPECT_FALSE(router.port(SharedMqttOwner::Human)
                     .publish(up, wire("other", "sessionReject", "candidate")));
    EXPECT_FALSE(router.port(SharedMqttOwner::Human)
                     .publish(up, wire("busy", "sessionAccept", "candidate")));
    EXPECT_EQ(router.ingest(down,
                            wire("busy", "requestAck", "candidate", false,
                                 "{\"requestMethod\":\"sessionReject\"}", 200),
                            103),
              SharedRouteResult::Routed);
}
TEST_F(RouterTest, InvalidOwnerCannotBorrowGlobalPort) {
    EXPECT_FALSE(
        router.port(static_cast<SharedMqttOwner>(99)).publish(up, wire("cap", "capabilityReport")));
}
TEST_F(RouterTest, GlobalFocusRequiresActualDeliveredOriginalCommand) {
    const auto command = wire(
        "focus", "focusControl", "ai", true,
        "{\"operation\":\"suspend\",\"expectedRevision\":1,\"focusId\":\"lease\",\"resources\":["
        "\"speaker\"],\"priority\":\"conversation\",\"resumePolicy\":\"explicit\"}",
        0, 1);
    ASSERT_EQ(router.ingest(down, command, 101), SharedRouteResult::Routed);
    const auto reply = wire("focus", "focusState", "ai", false,
                            "{\"state\":\"suspended\",\"revision\":2,\"focusId\":\"lease\","
                            "\"resources\":[\"speaker\"]}",
                            200);
    EXPECT_FALSE(router.port(SharedMqttOwner::Global).publish(up, reply));
    EXPECT_FALSE(router.take_message(SharedMqttOwner::AI));
    ASSERT_TRUE(router.take_message(SharedMqttOwner::Global));
    EXPECT_FALSE(router.port(SharedMqttOwner::AI).publish(up, reply));
    EXPECT_TRUE(router.port(SharedMqttOwner::Global).publish(up, reply));
    EXPECT_TRUE(router.port(SharedMqttOwner::Global).publish(up, reply));
    EXPECT_FALSE(router.port(SharedMqttOwner::Global)
                     .publish(up, wire("focus", "focusState", "other", false,
                                       "{\"state\":\"suspended\",\"revision\":2,\"focusId\":"
                                       "\"lease\",\"resources\":[\"speaker\"]}",
                                       200)));
    EXPECT_EQ(router.ingest(message_down, command, 102), SharedRouteResult::Rejected);
}
TEST_F(RouterTest, OwnerQueueBackpressureLeavesOriginalRequestAvailable) {
    establish(SharedMqttOwner::AI, "create", "ai");
    for (unsigned i = 0; i < 32; ++i) {
        ASSERT_EQ(router.ingest(message_down,
                                wire("event" + std::to_string(i), "textDelta", "ai", false, "{}", 0,
                                     i + 1, true),
                                102),
                  SharedRouteResult::Routed);
    }
    ASSERT_TRUE(
        router.port(SharedMqttOwner::AI).publish(message_up, wire("input", "inputSend", "ai")));
    const auto ack =
        wire("input", "requestAck", "ai", false, "{\"requestMethod\":\"inputSend\"}", 200);
    EXPECT_EQ(router.ingest(message_down, ack, 102), SharedRouteResult::Full);
    ASSERT_TRUE(router.take_message(SharedMqttOwner::AI));
    EXPECT_EQ(router.ingest(message_down, ack, 102), SharedRouteResult::Routed);
    for (unsigned i = 0; i < 32; ++i)
        ASSERT_TRUE(router.take_message(SharedMqttOwner::AI));
    EXPECT_FALSE(router.take_message(SharedMqttOwner::AI));
}
TEST_F(RouterTest, RequestAndSessionCapacitiesDoNotEvictLiveAssociations) {
    for (unsigned i = 0; i < 512; ++i) {
        ASSERT_TRUE(router.port(SharedMqttOwner::AI)
                        .publish(up, wire("c" + std::to_string(i), "sessionCreate")));
    }
    EXPECT_FALSE(router.port(SharedMqttOwner::AI).publish(up, wire("extra", "sessionCreate")));
    EXPECT_TRUE(router.port(SharedMqttOwner::AI).publish(up, wire("c0", "sessionCreate")));
    ASSERT_TRUE(router.tick(30000 + 100));
    EXPECT_EQ(router.ingest(down, wire("c0", "sessionCreated", "late", false, "{}", 200), 30100),
              SharedRouteResult::Rejected);
    ASSERT_TRUE(router.tick(600100));
    EXPECT_TRUE(router.port(SharedMqttOwner::Human).publish(up, wire("c0", "sessionCreate")));
}
TEST_F(RouterTest, SessionLimitRetainsRetiredLegForTenMinutes) {
    for (unsigned i = 0; i < 16; ++i)
        establish(SharedMqttOwner::AI, "c" + std::to_string(i), "s" + std::to_string(i));
    ASSERT_TRUE(router.port(SharedMqttOwner::Human).publish(up, wire("extra", "sessionCreate")));
    ASSERT_EQ(router.ingest(down, wire("extra", "sessionCreated", "extra", false, "{}", 200), 101),
              SharedRouteResult::Routed);
    ASSERT_TRUE(router.take_message(SharedMqttOwner::Human));
    EXPECT_EQ(
        router.confirm_session(SharedMqttOwner::Human, SharedSessionBinding{"extra", "extra"}, 101),
        SharedRouteResult::Full);
    ASSERT_TRUE(router.retire_session(SharedMqttOwner::AI, "s0", 101));
    EXPECT_EQ(
        router.confirm_session(SharedMqttOwner::Human, SharedSessionBinding{"extra", "extra"}, 101),
        SharedRouteResult::Full);
    ASSERT_TRUE(router.tick(600101));
    // 原extra已经过30秒，必须新的业务请求，不能续旧候选等待。
    EXPECT_EQ(router.confirm_session(SharedMqttOwner::Human, SharedSessionBinding{"extra", "extra"},
                                     600101),
              SharedRouteResult::Rejected);
    establish(SharedMqttOwner::Human, "fresh", "fresh", 600102);
}
TEST_F(RouterTest, AggregateBytesBackpressureAndClosedFramesRemainReadable) {
    const auto payload = "{\"text\":\"" + std::string(60000, 'x') + "\"}";
    EXPECT_TRUE(router.port(SharedMqttOwner::AI)
                    .publish(up, wire("large0", "sessionCreate", {}, true, payload)));
    EXPECT_TRUE(router.port(SharedMqttOwner::AI)
                    .publish(up, wire("large1", "sessionCreate", {}, true, payload)));
    EXPECT_FALSE(router.port(SharedMqttOwner::AI)
                     .publish(up, wire("large2", "sessionCreate", {}, true, payload)));
    const auto early = wire("e", "sessionEstablished", "new", false, "{}", 0, 1);
    ASSERT_EQ(router.ingest(down, early, 101), SharedRouteResult::Waiting);
    router.close();
    auto closed = router.take_unroutable();
    ASSERT_TRUE(closed);
    EXPECT_EQ(closed->reason, SharedUnroutableReason::Closed);
    EXPECT_EQ(closed->message.body, early);
}
TEST_F(RouterTest, SameTokenDifferentMethodsAndChannelsKeepIndependentOwners) {
    establish(SharedMqttOwner::AI, "a", "ai");
    establish(SharedMqttOwner::Human, "h", "human");
    ASSERT_TRUE(
        router.port(SharedMqttOwner::AI).publish(message_up, wire("shared", "inputSend", "ai")));
    ASSERT_TRUE(
        router.port(SharedMqttOwner::Human).publish(up, wire("shared", "sessionEnd", "human")));
    EXPECT_EQ(router.ingest(message_down,
                            wire("shared", "requestAck", "ai", false,
                                 "{\"requestMethod\":\"inputSend\"}", 200),
                            102),
              SharedRouteResult::Routed);
    EXPECT_EQ(router.ingest(down, wire("shared", "sessionEnded", "human", false, "{}", 200), 102),
              SharedRouteResult::Routed);
    ASSERT_TRUE(router.take_message(SharedMqttOwner::AI));
    ASSERT_TRUE(router.take_message(SharedMqttOwner::Human));
    EXPECT_FALSE(router.take_message(SharedMqttOwner::Global));
}
TEST_F(RouterTest, SpecializedResponsesUseExistingOriginalMethodsOnly) {
    establish(SharedMqttOwner::Human, "create", "human");
    const std::vector<std::pair<std::string, std::string>> methods{
        {"mediaOffer", "mediaAnswer"},
        {"trackControl", "trackState"},
        {"callControl", "callState"},
        {"sessionSync", "sessionSnapshot"},
        {"sessionEnd", "sessionEnded"}};
    unsigned number = 0;
    for (const auto& mapping : methods) {
        const auto token = "method" + std::to_string(number++);
        ASSERT_TRUE(
            router.port(SharedMqttOwner::Human).publish(up, wire(token, mapping.first, "human")));
        EXPECT_EQ(router.ingest(down, wire(token, "unknownReply", "human", false, "{}", 200), 102),
                  SharedRouteResult::Rejected);
        EXPECT_EQ(router.ingest(down, wire(token, mapping.second, "human", false, "{}", 200), 102),
                  SharedRouteResult::Routed);
        EXPECT_TRUE(router.take_message(SharedMqttOwner::Human));
    }
}
TEST_F(RouterTest, RawSuccessfulAckAndFailedCreationCannotBindSession) {
    ASSERT_TRUE(router.port(SharedMqttOwner::AI).publish(up, wire("create", "sessionCreate")));
    EXPECT_EQ(router.ingest(down,
                            wire("create", "requestAck", {}, false,
                                 "{\"requestMethod\":\"sessionCreate\"}", 200),
                            101),
              SharedRouteResult::Routed);
    ASSERT_TRUE(router.take_message(SharedMqttOwner::AI));
    EXPECT_EQ(router.confirm_session(SharedMqttOwner::AI,
                                     SharedSessionBinding{"create", "invented"}, 101),
              SharedRouteResult::Rejected);
    ASSERT_TRUE(router.port(SharedMqttOwner::Human).publish(up, wire("failed", "sessionCreate")));
    EXPECT_EQ(router.ingest(down, wire("failed", "sessionCreated", {}, false, "{}", 422), 101),
              SharedRouteResult::Routed);
    ASSERT_TRUE(router.take_message(SharedMqttOwner::Human));
    EXPECT_EQ(router.confirm_session(SharedMqttOwner::Human,
                                     SharedSessionBinding{"failed", "invented"}, 101),
              SharedRouteResult::Rejected);
}
TEST_F(RouterTest, RealRequestsRetryUsesOriginalOwnerAssociationAndDoesNotOwnNetwork) {
    Requests requests(router.port(SharedMqttOwner::AI));
    const auto original = wire("retry", "sessionCreate");
    publisher.accepted = false;
    EXPECT_EQ(requests.submit(Request{"retry", {}, "sessionCreate", up, original}, 100),
              SubmitResult::Accepted);
    EXPECT_EQ(requests.state(RequestKey{up, "sessionCreate", "retry"}), RequestState::Pending);
    EXPECT_FALSE(router.port(SharedMqttOwner::Human).publish(up, original));
    ASSERT_TRUE(router.tick(10100));
    publisher.accepted = true;
    requests.tick(10100);
    ASSERT_EQ(publisher.bodies.size(), 2U);
    EXPECT_EQ(publisher.bodies[0], publisher.bodies[1]);
    const auto reply = wire("retry", "sessionCreated", "ai", false, "{}", 200);
    ASSERT_EQ(router.ingest(down, reply, 10101), SharedRouteResult::Routed);
    auto received = router.take_message(SharedMqttOwner::AI);
    ASSERT_TRUE(received);
    EXPECT_EQ(received->body, reply);
    EXPECT_FALSE(router.take_message(SharedMqttOwner::Human));
    EXPECT_TRUE(
        requests.acknowledge(Acknowledgement{"retry", {}, "sessionCreate", 200, up}, 10101));
    EXPECT_EQ(requests.state(RequestKey{up, "sessionCreate", "retry"}), RequestState::Succeeded);
    EXPECT_EQ(
        router.confirm_session(SharedMqttOwner::AI, SharedSessionBinding{"retry", "ai"}, 10101),
        SharedRouteResult::Routed);
}
TEST_F(RouterTest, SignalAndMessageWatermarksRemainInOriginalChannels) {
    establish(SharedMqttOwner::AI, "create", "ai");
    EXPECT_EQ(router.ingest(down, wire("wrong", "textDelta", "ai", false, "{}", 0, 1, true), 102),
              SharedRouteResult::Rejected);
    EXPECT_EQ(router.ingest(message_down,
                            wire("wrong", "sessionEstablished", "ai", false, "{}", 0, 1), 102),
              SharedRouteResult::Rejected);
    EXPECT_EQ(router.ingest(down, wire("missing", "sessionEstablished", "ai", false), 102),
              SharedRouteResult::Rejected);
    EXPECT_EQ(router.ingest(down, wire("error", "sessionError", "ai", false, "{}", 503, 1), 102),
              SharedRouteResult::Routed);
    EXPECT_TRUE(router.take_message(SharedMqttOwner::AI));
    EXPECT_FALSE(router.take_message(SharedMqttOwner::Human));
}
TEST_F(RouterTest, ExpiredInviteCannotRenewOrBindOldCandidate) {
    const auto invite =
        wire("invite", "sessionInvite", "human", true, "{\"purpose\":\"humanCall\"}");
    ASSERT_EQ(router.ingest(down, invite, 100), SharedRouteResult::Routed);
    ASSERT_TRUE(router.take_message(SharedMqttOwner::Human));
    EXPECT_EQ(router.confirm_session(SharedMqttOwner::Human,
                                     SharedSessionBinding{"invite", "human"}, 30100),
              SharedRouteResult::Rejected);
    EXPECT_EQ(router.ingest(down, invite, 30100), SharedRouteResult::Rejected);
    EXPECT_FALSE(
        router.port(SharedMqttOwner::Human).publish(up, wire("invite", "sessionReject", "human")));
    EXPECT_FALSE(router.retire_session(SharedMqttOwner::Human, "human", 30100));
}
TEST_F(RouterTest, InvalidAndConflictingGlobalCommandsNeverReachBusinessQueues) {
    const std::string action_down = "$thing/down/action/test-product/test-device";
    const std::string command =
        "{\"method\":\"action\",\"msgToken\":\"action\",\"actionID\":\"test\"}";
    ASSERT_EQ(router.ingest(action_down, command, 101), SharedRouteResult::Routed);
    EXPECT_EQ(router.ingest(
                  action_down,
                  "{\"method\":\"action\",\"msgToken\":\"action\",\"actionID\":\"other\"}", 101),
              SharedRouteResult::Rejected);
    EXPECT_EQ(
        router.ingest(
            action_down,
            "{\"method\":\"action\",\"msgToken\":\"bad\",\"actionID\":\"test\",\"private\":true}",
            101),
        SharedRouteResult::Rejected);
    EXPECT_EQ(router.ingest(down, wire("focus", "focusControl", "ai", true, "{}", 0, 1), 101),
              SharedRouteResult::Rejected);
    EXPECT_EQ(
        router.ingest(message_down,
                      wire("invite", "sessionInvite", "human", true, "{\"purpose\":\"humanCall\"}"),
                      101),
        SharedRouteResult::Rejected);
    ASSERT_TRUE(router.take_message(SharedMqttOwner::Global));
    EXPECT_FALSE(router.take_message(SharedMqttOwner::Global));
}
TEST_F(RouterTest, InvalidIdentityAndUntickedRouterRejectWithoutPublishing) {
    SharedMqttRouter unticked(publisher, "test-product/test-device");
    EXPECT_FALSE(unticked.port(SharedMqttOwner::AI).publish(up, wire("create", "sessionCreate")));
    const std::vector<std::string> invalid{"/device",
                                           "product/",
                                           "product/device/extra",
                                           "product/#",
                                           "product/\n",
                                           std::string("p/d\0x", 5),
                                           std::string("p/\xff", 3)};
    for (const auto& identity : invalid) {
        SharedMqttRouter instance(publisher, identity);
        EXPECT_TRUE(instance.closed());
        EXPECT_FALSE(instance.tick(100));
    }
    SharedMqttRouter two_long_segments(publisher,
                                       std::string(100, 'p') + "/" + std::string(100, 'd'));
    EXPECT_FALSE(two_long_segments.closed());
    EXPECT_TRUE(publisher.bodies.empty());
}
TEST_F(RouterTest, FailedOwnerRetirementAndClosedTakeCannotDestroyAnotherLeg) {
    establish(SharedMqttOwner::AI, "create", "ai");
    EXPECT_FALSE(router.retire_session(SharedMqttOwner::Human, "ai", 102));
    EXPECT_EQ(
        router.ingest(message_down, wire("text", "textDelta", "ai", false, "{}", 0, 1, true), 102),
        SharedRouteResult::Routed);
    EXPECT_FALSE(router.take_message(static_cast<SharedMqttOwner>(77)));
    router.close();
    EXPECT_FALSE(router.take_message(SharedMqttOwner::AI));
    EXPECT_EQ(
        router.confirm_session(SharedMqttOwner::AI, SharedSessionBinding{"create", "ai"}, 102),
        SharedRouteResult::Closed);
    EXPECT_FALSE(router.tick(103));
    EXPECT_FALSE(router.retire_session(SharedMqttOwner::AI, "ai", 103));
    EXPECT_TRUE(router.take_unroutable());
    router.close();
    EXPECT_FALSE(router.take_unroutable());
}
// context_wire 提供严格完整私有下行，路由仍须核当前Human归属和实际交付。
std::string context_wire() {
    return wire(
        std::string(64, 'a'), "contextSnapshot", "human-leg", false,
        R"({"deliveryId":")" + std::string(64, 'a') + R"(","consentId":")" + std::string(64, 'b') +
            R"(","scope":"completedText","sourceSessionId":"ai-source","serviceId":"support","serviceRevision":"r1","snapshotVersion":"v1","snapshotDigest":")" +
            std::string(64, 'c') +
            R"(","firstTurnIndex":1,"lastTurnIndex":1,"expiresAt":2000,"turns":[{"messageId":"u1","sequence":1,"turnIndex":1,"role":"user","text":"私有本人问题"}]})",
        0, 1, true);
}
// context_ack 使用实际严格解析/生成器，不以通用data消息伪造接收确认。
std::string context_ack() {
    ContextSnapshot snapshot;
    EXPECT_EQ(decode_context_snapshot(WireRoute::Message, context_wire(), snapshot),
              WireResult::Ok);
    Envelope ack;
    EXPECT_EQ(context_snapshot_ack(snapshot, ack), WireResult::Ok);
    std::string result;
    EXPECT_EQ(encode_envelope(ack, result), DecodeResult::Ok);
    return result;
}
TEST_F(RouterTest, HumanContextAckRequiresOriginalDeliveredMessageAndKeepsBytesOnBackpressure) {
    router.expire_private_context(1000);
    ASSERT_NO_FATAL_FAILURE(establish(SharedMqttOwner::Human, "human-create", "human-leg"));
    const auto body = context_wire();
    const auto ack = context_ack();
    const auto count = publisher.bodies.size();
    EXPECT_FALSE(router.port(SharedMqttOwner::Human).publish(message_up, ack));
    ASSERT_EQ(router.ingest(message_down, body, 102), SharedRouteResult::Routed);
    EXPECT_FALSE(router.port(SharedMqttOwner::Human).publish(message_up, ack));
    ASSERT_TRUE(router.take_message(SharedMqttOwner::Human));
    publisher.accepted = false;
    EXPECT_FALSE(router.port(SharedMqttOwner::Human).publish(message_up, ack));
    EXPECT_EQ(publisher.bodies.size(), count + 1);
    publisher.accepted = true;
    ASSERT_TRUE(router.port(SharedMqttOwner::Human).publish(message_up, ack));
    EXPECT_EQ(publisher.bodies.back(), ack);
    ASSERT_TRUE(router.retire_session(SharedMqttOwner::Human, "human-leg", 103));
    EXPECT_FALSE(router.port(SharedMqttOwner::Human).publish(message_up, ack));
}
TEST_F(RouterTest, ContextNeverBorrowsAIUnknownOrRetiredHumanScope) {
    router.expire_private_context(1000);
    EXPECT_EQ(router.ingest(message_down, context_wire(), 102), SharedRouteResult::Rejected);
    ASSERT_NO_FATAL_FAILURE(establish(SharedMqttOwner::AI, "ai-create", "human-leg", 103));
    EXPECT_EQ(router.ingest(message_down, context_wire(), 104), SharedRouteResult::Rejected);
    EXPECT_FALSE(router.port(SharedMqttOwner::AI).publish(message_up, context_ack()));
    EXPECT_FALSE(router.port(SharedMqttOwner::Global).publish(message_up, context_ack()));
}
TEST_F(RouterTest, RetiredContextIsNotReturnedAsUnroutablePrivatePlaintext) {
    router.expire_private_context(1000);
    ASSERT_NO_FATAL_FAILURE(establish(SharedMqttOwner::Human, "human-create", "human-leg"));
    ASSERT_EQ(router.ingest(message_down, context_wire(), 102), SharedRouteResult::Routed);
    ASSERT_TRUE(router.retire_session(SharedMqttOwner::Human, "human-leg", 103));
    EXPECT_EQ(router.private_context_bytes(), 0U);
    EXPECT_FALSE(router.take_message(SharedMqttOwner::Human));
    EXPECT_FALSE(router.take_unroutable());
    EXPECT_FALSE(router.closed());
}
TEST_F(RouterTest, DisconnectedContextIsClearedButOrdinaryUnroutableMessageIsPreserved) {
    router.expire_private_context(1000);
    ASSERT_NO_FATAL_FAILURE(establish(SharedMqttOwner::Human, "human-create", "human-leg"));
    ASSERT_EQ(router.ingest(message_down, context_wire(), 102), SharedRouteResult::Routed);
    const auto normal = wire("ordinary", "textDelta", "human-leg", false, "{}", 0, 2, true);
    ASSERT_EQ(router.ingest(message_down, normal, 102), SharedRouteResult::Routed);
    router.close();
    EXPECT_EQ(router.private_context_bytes(), 0U);
    auto released = router.take_unroutable();
    ASSERT_TRUE(released);
    EXPECT_EQ(released->message.body, normal);
    EXPECT_FALSE(router.take_unroutable());
}
TEST_F(RouterTest, ExpiryClearsDeliveredAndQueuedPrivateCopiesWithoutEndingHumanScope) {
    ASSERT_NO_FATAL_FAILURE(establish(SharedMqttOwner::Human, "human-create", "human-leg"));
    EXPECT_EQ(router.ingest(message_down, context_wire(), 102), SharedRouteResult::Rejected);
    router.expire_private_context(1000);
    ASSERT_EQ(router.ingest(message_down, context_wire(), 103), SharedRouteResult::Routed);
    ASSERT_TRUE(router.take_message(SharedMqttOwner::Human));
    ASSERT_TRUE(router.port(SharedMqttOwner::Human).publish(message_up, context_ack()));
    ASSERT_EQ(router.ingest(message_down, context_wire(), 104), SharedRouteResult::Routed);
    EXPECT_GT(router.private_context_bytes(), 0U);
    router.expire_private_context(1999);
    EXPECT_GT(router.private_context_bytes(), 0U);
    router.expire_private_context(2000);
    EXPECT_EQ(router.private_context_bytes(), 0U);
    EXPECT_FALSE(router.take_message(SharedMqttOwner::Human));
    EXPECT_FALSE(router.take_unroutable());
    EXPECT_FALSE(router.port(SharedMqttOwner::Human).publish(message_up, context_ack()));
    auto changed = context_wire();  // 增大水位和截止不能更新原投递墓碑。
    changed.replace(changed.find("\"messageSeq\":1"), 14, "\"messageSeq\":9");
    changed.replace(changed.find("\"expiresAt\":2000"), 16, "\"expiresAt\":9000");
    ContextSnapshot checked;
    ASSERT_EQ(decode_context_snapshot(WireRoute::Message, changed, checked), WireResult::Ok);
    EXPECT_EQ(router.ingest(message_down, changed, 105), SharedRouteResult::Rejected);
    router.expire_private_context(2001);
    EXPECT_EQ(router.ingest(message_down, changed, 600105), SharedRouteResult::Rejected);
    EXPECT_TRUE(router.port(SharedMqttOwner::Human)
                    .publish(up, wire("still-active", "sessionSync", "human-leg")));
    EXPECT_FALSE(router.closed());
}
TEST_F(RouterTest, ClockRollbackClearsPrivateCopiesAndCannotReopenOriginalDelivery) {
    ASSERT_NO_FATAL_FAILURE(establish(SharedMqttOwner::Human, "human-create", "human-leg"));
    router.expire_private_context(1000);
    ASSERT_EQ(router.ingest(message_down, context_wire(), 102), SharedRouteResult::Routed);
    router.expire_private_context(999);
    EXPECT_EQ(router.private_context_bytes(), 0U);
    EXPECT_FALSE(router.take_unroutable());
    EXPECT_EQ(router.ingest(message_down, context_wire(), 103), SharedRouteResult::Rejected);
    router.expire_private_context(1001);
    EXPECT_EQ(router.ingest(message_down, context_wire(), 104), SharedRouteResult::Rejected);
    EXPECT_FALSE(router.closed());
}
}  // namespace
// ReceiptRouterTest只验转交合同，不以PublishPort成功冒充HAL安装；真实安装仍由独立Host用例覆盖。
class ReceiptRouterTest : public ::testing::Test {
protected:
    Publisher publisher;
    SharedMqttRouter router{publisher, "test-product/test-device",
                            true};  // 本机显式选择，不来自下行。
    const std::string up = "$thing/up/signaling/test-product/test-device";
    const std::string down = "$thing/down/signaling/test-product/test-device";
    // relate只包装被友元允许的已交付原Start关联，返回原容量/冲突判断，不授予HAL安装。
    bool relate(const std::string& body, std::uint64_t now = 102) {
        return router.relate_reply_start(down, body, now);
    }
    // bind机械复用原Host合法Created，再沿正式解码/消费/确认；不虚造Router内部来源。
    void bind(SharedMqttRouter& target, bool receipt = true,
              SharedMqttOwner owner = SharedMqttOwner::AI) {
        ASSERT_TRUE(target.tick(100));
        ASSERT_TRUE(target.port(owner).publish(up, wire("create", "sessionCreate")));
        std::string payload =
            R"({"state":"negotiating","selected":{"transport":"udp","interactionMode":"fullDuplex","audioParams":{"format":"opus","sampleRate":48000,"channels":1,"frameDuration":20},"inputModalities":["text"],"outputModalities":["text","audio"]},"tracks":[{"trackId":"mic-1","kind":"audio","sourceId":"mic","direction":"sendonly","codec":"opus","state":"requested"},{"trackId":"speaker-1","kind":"audio","sourceId":"speaker","direction":"recvonly","codec":"opus","state":"requested"}],"media":{"udp":{"server":"127.0.0.1","port":12345,"key":"000102030405060708090a0b0c0d0e0f","nonce":"01000000112233445566778800000000"}}})";
        if (receipt)
            payload.insert(payload.find("\"interactionMode\""), "\"udpReplyStartReceipt\":true,");
        const auto body = wire("create", "sessionCreated", "ai", false, payload, 200);
        VoiceEvent selected;
        ASSERT_EQ(decode_voice_event(WireRoute::Signaling, body, selected), WireResult::Ok);
        ASSERT_EQ(target.ingest(down, body, 101), SharedRouteResult::Routed);
        ASSERT_TRUE(target.take_message(owner));
        ASSERT_EQ(target.confirm_session(owner, {"create", "ai"}, 101), SharedRouteResult::Routed);
    }
    // start用正式五字段UDP Start，序号/票据可选择而不增权限。
    std::string start(std::string token = "start", std::uint64_t seq = 3) const {
        return wire(
            std::move(token), "respAudioStart", "ai", false,
            R"({"respId":"reply","trackId":"speaker-1","mediaRevision":1,"udpNonce":"0000000000000063","firstPacketSeq":128})",
            0, seq);
    }
    // receipt仅用正式反向编码器生成原事件200，不手写替代字段。
    std::string receipt(const std::string& body) const {
        VoiceEvent event;
        Envelope reply;
        std::string result;
        EXPECT_EQ(decode_voice_event(WireRoute::Signaling, body, event), WireResult::Ok);
        EXPECT_EQ(reply_start_receipt(event, reply), WireResult::Ok);
        EXPECT_EQ(encode_envelope(reply, result), DecodeResult::Ok);
        return result;
    }
    // deliver实际消费一次原事件；没有业务授予或媒体Ready替代。
    void deliver(const std::string& body, std::uint64_t now = 102, bool relate = true) {
        ASSERT_EQ(router.ingest(down, body, now), SharedRouteResult::Routed);
        ASSERT_TRUE(router.take_message(SharedMqttOwner::AI));
        Envelope event;
        ASSERT_EQ(decode_envelope(body, event), DecodeResult::Ok);
        if (relate && event.method == "respAudioStart") {
            ASSERT_TRUE(this->relate(body, now));
        }
    }
};
TEST_F(ReceiptRouterTest, OnlyDeliveredOriginalAiStartCanForwardStrictReceipt) {
    bind(router);
    const auto body = start(), reply = receipt(body);
    ASSERT_EQ(router.ingest(down, body, 102), SharedRouteResult::Routed);
    EXPECT_FALSE(router.port(SharedMqttOwner::AI).publish(up, reply));
    ASSERT_TRUE(router.take_message(SharedMqttOwner::AI));
    EXPECT_FALSE(router.port(SharedMqttOwner::AI).publish(up, reply));
    ASSERT_TRUE(relate(body));
    EXPECT_TRUE(router.port(SharedMqttOwner::AI).publish(up, reply));
    EXPECT_FALSE(router.port(SharedMqttOwner::Human).publish(up, reply));
    EXPECT_FALSE(router.port(SharedMqttOwner::Global).publish(up, reply));
    EXPECT_FALSE(router.port(SharedMqttOwner::AI).publish(down, reply));
}
TEST_F(ReceiptRouterTest, DefaultFalseAndOldSelectedKeepRejectingWithoutStartRecords) {
    SharedMqttRouter old(publisher, "test-product/test-device");
    bind(old);
    const auto body = start();
    ASSERT_EQ(old.ingest(down, body, 102), SharedRouteResult::Routed);
    ASSERT_TRUE(old.take_message(SharedMqttOwner::AI));
    EXPECT_FALSE(old.port(SharedMqttOwner::AI).publish(up, receipt(body)));
    bind(router, false);
    for (unsigned i = 0; i < 600; ++i) {
        deliver(start("old-" + std::to_string(i), i + 2), 102, false);
    }
    EXPECT_FALSE(relate(start("old-599", 601)));
    EXPECT_FALSE(router.port(SharedMqttOwner::AI).publish(up, receipt(start("old-599", 601))));
}
TEST_F(ReceiptRouterTest, HumanSelectedNeverGainsAiReceiptRoute) {
    bind(router, true, SharedMqttOwner::Human);
    const auto body = start();
    ASSERT_EQ(router.ingest(down, body, 102), SharedRouteResult::Routed);
    ASSERT_TRUE(router.take_message(SharedMqttOwner::Human));
    EXPECT_FALSE(router.port(SharedMqttOwner::Human).publish(up, receipt(body)));
    EXPECT_FALSE(router.port(SharedMqttOwner::AI).publish(up, receipt(body)));
}
TEST_F(ReceiptRouterTest, TokenSessionFieldsCodesAndOtherMethodsRemainStrict) {
    bind(router);
    const auto body = start();
    deliver(body);
    Envelope good;
    ASSERT_EQ(decode_envelope(receipt(body), good), DecodeResult::Ok);
    for (unsigned i = 0; i < 8; ++i) {
        auto changed = good;
        if (i == 0)
            changed.token = "other";
        if (i == 1)
            changed.session = "other";
        if (i == 2)
            changed.method = "respAudioDone";
        if (i == 3)
            changed.code = 500;
        if (i == 4)
            changed.event_sequence = 3;
        if (i == 5)
            changed.payload = R"({"respId":"other","trackId":"speaker-1","mediaRevision":1})";
        if (i == 6)
            changed.payload = R"({"respId":"reply","trackId":"other","mediaRevision":1})";
        if (i == 7)
            changed.payload = R"({"respId":"reply","trackId":"speaker-1","mediaRevision":2})";
        std::string bytes;
        ASSERT_EQ(encode_envelope(changed, bytes), DecodeResult::Ok);
        EXPECT_FALSE(router.port(SharedMqttOwner::AI).publish(up, bytes));
    }
    EXPECT_TRUE(router.port(SharedMqttOwner::AI).publish(up, receipt(body)));
}
TEST_F(ReceiptRouterTest, PublisherBusyRetryKeepsOriginalBytesAndFirstTwoSecondDeadline) {
    bind(router);
    const auto body = start();
    deliver(body);
    publisher.accepted = false;
    EXPECT_FALSE(router.port(SharedMqttOwner::AI).publish(up, receipt(body)));
    ASSERT_EQ(router.ingest(down, body, 2101), SharedRouteResult::Routed);
    ASSERT_TRUE(router.take_message(SharedMqttOwner::AI));
    ASSERT_TRUE(relate(body, 2101));  // 精确原字节重登记也不能续首次截止。
    publisher.accepted = true;
    EXPECT_TRUE(router.port(SharedMqttOwner::AI).publish(up, receipt(body)));
    ASSERT_TRUE(router.tick(2102));
    EXPECT_FALSE(router.port(SharedMqttOwner::AI).publish(up, receipt(body)));
    ASSERT_GE(publisher.bodies.size(), std::size_t{3});
    EXPECT_EQ(publisher.bodies[publisher.bodies.size() - 1],
              publisher.bodies[publisher.bodies.size() - 2]);
}
TEST_F(ReceiptRouterTest, RetiredAndClockClosedScopesNeverForwardOldReceipts) {
    bind(router);
    const auto body = start();
    deliver(body);
    ASSERT_TRUE(router.retire_session(SharedMqttOwner::AI, "ai", 103));
    EXPECT_FALSE(router.port(SharedMqttOwner::AI).publish(up, receipt(body)));
    EXPECT_FALSE(router.tick(102));
    EXPECT_FALSE(router.port(SharedMqttOwner::AI).publish(up, receipt(body)));
}
// 同token异完整Start不得覆盖nonce/序号/正文；合法原200仍只能关联第一次已交付事实。
TEST_F(ReceiptRouterTest, ConflictingCanonicalStartCannotReplaceOriginalSource) {
    bind(router);
    const auto original = start();
    deliver(original);
    EXPECT_FALSE(relate(start("start", 4), 103));
    auto changed = original;
    changed.replace(changed.find("0000000000000063"), 16, "0000000000000064");
    EXPECT_FALSE(relate(changed, 103));
    EXPECT_TRUE(router.port(SharedMqttOwner::AI).publish(up, receipt(original)));
}
TEST_F(ReceiptRouterTest, UnrelatedOldStartDoesNotBlockExplicitlyRelatedFreshWork) {
    bind(router);
    deliver(wire("established", "sessionEstablished", "ai", false,
                 R"({"state":"established","mediaRevision":1})", 0, 1));
    deliver(start("late", 1), 102, false);  // Router不估算序号，真实Host依核心水位不登记旧帧。
    EXPECT_FALSE(router.port(SharedMqttOwner::AI).publish(up, receipt(start("late", 1))));
    deliver(start("late", 3));
    EXPECT_TRUE(router.port(SharedMqttOwner::AI).publish(up, receipt(start("late", 3))));
}
TEST_F(ReceiptRouterTest, FullStartHistoryDoesNotEvictOrHolBlockOrdinaryAiEvents) {
    bind(router);
    const auto first = start("start-0", 2);
    deliver(first);
    ASSERT_TRUE(router.port(SharedMqttOwner::AI)
                    .publish(up, receipt(first)));  // 先保存原回执，满额后只验不驱逐。
    unsigned accepted = 0;  // 512条与256KiB先到者拒绝，不能假设正文一定先触条数门限。
    for (unsigned i = 0; i < 512; ++i) {
        const auto body = start("start-" + std::to_string(i), i + 2);
        ASSERT_EQ(router.ingest(down, body, 102), SharedRouteResult::Routed);
        ASSERT_TRUE(router.take_message(SharedMqttOwner::AI));
        ++accepted;
        if (!relate(body)) {
            --accepted;
            break;
        }
    }
    ASSERT_GT(accepted, 0U);
    ASSERT_LT(accepted, 512U);
    EXPECT_FALSE(relate(start("over", 513), 103));
    deliver(
        wire("end", "sessionEnded", "ai", false, R"({"state":"ended","mediaRevision":1})", 0, 514),
        103);
    EXPECT_TRUE(router.port(SharedMqttOwner::AI).publish(up, receipt(first)));
}
}  // namespace ur::device
