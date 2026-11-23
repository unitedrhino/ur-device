// 请求计时用虚拟毫秒，不实际睡眠；断言原报文重试、严格关联和幂等保留。
#include "ur/device/requests.hpp"
#include <gtest/gtest.h>
#include <vector>

namespace ur::device {
namespace {
// request_topic 为测试已鉴权身份，ACK 必须显式提供完整作用域。
constexpr auto request_topic = "$thing/up/signaling/p/d";
// FakePublisher 收集原始发布内容，可模拟离线但不伪造云端应答。
struct FakePublisher final : PublishPort {
    std::vector<std::string> bodies;  // 已尝试发布的正文。
    bool online = true;               // 本地发送是否接受。
    bool publish(std::string_view, std::string_view body) override {
        bodies.emplace_back(body);
        return online;
    }
};
// request 创建固定合法测试请求，token 由测试决定。
Request request(std::string token = "t") {
    return {std::move(token), "s", "audioStart", "$thing/up/signaling/p/d", "{\"params\":{}}"};
}
TEST(Requests, TimeoutRetriesSameBodyAndNeverExtendsDeadline) {
    FakePublisher port;
    port.online = false;
    Requests pending(port);
    ASSERT_EQ(pending.submit(request(), 100), SubmitResult::Accepted);
    pending.tick(99);
    pending.tick(10099);
    EXPECT_EQ(port.bodies.size(), 1U);
    pending.tick(10100);
    pending.tick(20100);
    EXPECT_EQ(port.bodies.size(), 3U);
    EXPECT_EQ(port.bodies[0], port.bodies[1]);
    EXPECT_EQ(port.bodies[1], port.bodies[2]);
    EXPECT_EQ(pending.submit(request(), 20101), SubmitResult::Duplicate);
    pending.tick(30100);
    EXPECT_EQ(pending.state({request_topic, "audioStart", "t"}), RequestState::TimedOut);
    EXPECT_FALSE(pending.acknowledge({"t", "s", "audioStart", 200, request_topic}, 30101));
    EXPECT_EQ(port.bodies.size(), 3U);
}
TEST(Requests, AckRequiresExactAssociationAndSuccessCode) {
    FakePublisher port;
    Requests pending(port);
    ASSERT_EQ(pending.submit(request(), 100), SubmitResult::Accepted);
    EXPECT_FALSE(pending.acknowledge({"other", "s", "audioStart", 200, request_topic}, 101));
    EXPECT_FALSE(pending.acknowledge({"t", "other", "audioStart", 200, request_topic}, 101));
    EXPECT_FALSE(pending.acknowledge({"t", "s", "audioStop", 200, request_topic}, 101));
    EXPECT_FALSE(pending.acknowledge({"t", "s", "audioStart", 0, request_topic}, 101));
    EXPECT_FALSE(pending.acknowledge({"t", "s", "audioStart", 600, request_topic}, 101));
    EXPECT_FALSE(pending.acknowledge({"t", "s", "audioStart", 200, request_topic}, 99));
    EXPECT_TRUE(pending.acknowledge({"t", "s", "audioStart", 200, request_topic}, 101));
    EXPECT_EQ(pending.state({request_topic, "audioStart", "t"}), RequestState::Succeeded);
    EXPECT_FALSE(pending.acknowledge({"t", "s", "audioStart", 500, request_topic}, 102));
    ASSERT_EQ(pending.submit(request("t2"), 200), SubmitResult::Accepted);
    EXPECT_TRUE(pending.acknowledge({"t2", "s", "audioStart", 403, request_topic}, 201));
    EXPECT_EQ(pending.state({request_topic, "audioStart", "t2"}), RequestState::Failed);
}
TEST(Requests, LateAckAtBoundaryCannotResurrectRequest) {
    FakePublisher port;
    Requests pending(port);
    ASSERT_EQ(pending.submit(request(), 0), SubmitResult::Accepted);
    EXPECT_FALSE(pending.acknowledge({"t", "s", "audioStart", 200, request_topic}, 30000));
    EXPECT_EQ(pending.state({request_topic, "audioStart", "t"}), RequestState::TimedOut);
}
// 恢复期延长预算：在途与之后提交的请求都按统一截止接受迟到 ACK（下行积压场景），
// 只延长不缩短；清除后恢复常规 30 秒预算。
TEST(Requests, ExtendedDeadlineAcceptsLateRecoveryAckAndIsBounded) {
    FakePublisher port;
    Requests pending(port);
    ASSERT_EQ(pending.submit(request("a"), 0), SubmitResult::Accepted);
    pending.extend_pending(50000);
    pending.extend_pending(1000); // 较小截止不得缩短既有延长。
    pending.tick(30001);
    EXPECT_EQ(pending.state({request_topic, "audioStart", "a"}), RequestState::Pending);
    EXPECT_TRUE(pending.acknowledge({"a", "s", "audioStart", 200, request_topic}, 30900));
    // 延长期间提交的恢复请求同样按延长截止判定，迟到 ACK 仍可关联。
    ASSERT_EQ(pending.submit(request("b"), 1000), SubmitResult::Accepted);
    pending.tick(31000);
    EXPECT_EQ(pending.state({request_topic, "audioStart", "b"}), RequestState::Pending);
    EXPECT_TRUE(pending.acknowledge({"b", "s", "audioStart", 200, request_topic}, 49999));
    // 截止时刻按过期处理；清除后新请求回到常规 30 秒预算。
    ASSERT_EQ(pending.submit(request("c"), 2000), SubmitResult::Accepted);
    EXPECT_FALSE(pending.acknowledge({"c", "s", "audioStart", 200, request_topic}, 50000));
    EXPECT_EQ(pending.state({request_topic, "audioStart", "c"}), RequestState::TimedOut);
    pending.clear_pending_extension();
    ASSERT_EQ(pending.submit(request("d"), 60000), SubmitResult::Accepted);
    pending.tick(90000);
    EXPECT_EQ(pending.state({request_topic, "audioStart", "d"}), RequestState::TimedOut);
}
TEST(Requests, CancelIsScopedAndRetainedForTenMinutes) {
    FakePublisher port;
    Requests pending(port);
    ASSERT_EQ(pending.submit(request(), 0), SubmitResult::Accepted);
    pending.cancel_session("other", 1);
    EXPECT_EQ(pending.state({request_topic, "audioStart", "t"}), RequestState::Pending);
    pending.cancel_session("s", 2);
    pending.cancel_session("s", 3);
    EXPECT_EQ(pending.state({request_topic, "audioStart", "t"}), RequestState::Cancelled);
    pending.tick(1);
    pending.tick(600001);
    EXPECT_EQ(pending.state({request_topic, "audioStart", "t"}), RequestState::Cancelled);
    pending.tick(600002);
    EXPECT_EQ(pending.state({request_topic, "audioStart", "t"}), RequestState::Missing);
    EXPECT_EQ(port.bodies.size(), 1U);
}
TEST(Requests, TokenConflictsDoNotPublishOrReplaceRecords) {
    FakePublisher port;
    Requests pending(port);
    ASSERT_EQ(pending.submit(request(), 0), SubmitResult::Accepted);
    for (int field = 0; field < 4; ++field) {
        auto changed = request();
        if (field == 0)
            changed.session = "other";
        if (field == 1)
            changed.method = "audioStop";
        if (field == 2)
            changed.topic = "other";
        if (field == 3)
            changed.body = "{}";
        EXPECT_EQ(pending.submit(std::move(changed), 1),
                  field == 1 || field == 2 ? SubmitResult::Accepted : SubmitResult::Conflict);
    }
    EXPECT_EQ(port.bodies.size(), 3U);
}
TEST(Requests, SameTokenAcrossMethodsAndTopicsHasIndependentAckScope) {
    FakePublisher port;
    Requests pending(port);
    ASSERT_EQ(pending.submit(request(), 0), SubmitResult::Accepted);
    auto different = request();
    different.topic = "$thing/up/message/p/d";
    ASSERT_EQ(pending.submit(different, 0), SubmitResult::Accepted);
    different.method = "audioStop";
    ASSERT_EQ(pending.submit(different, 0), SubmitResult::Accepted);
    EXPECT_FALSE(pending.acknowledge({"t", "s", "audioStart", 200, ""}, 1));
    EXPECT_TRUE(pending.acknowledge({"t", "s", "audioStop", 200, different.topic}, 1));
    EXPECT_EQ(pending.state({different.topic, "audioStart", "t"}), RequestState::Pending);
    EXPECT_EQ(pending.state({request_topic, "audioStart", "t"}), RequestState::Pending);
    EXPECT_EQ(pending.state({different.topic, "audioStop", "t"}), RequestState::Succeeded);
}
TEST(Requests, CapacityProtectsRecentTerminalRecords) {
    FakePublisher port;
    Requests pending(port);
    for (int index = 0; index < 32; ++index)
        ASSERT_EQ(pending.submit(request(std::to_string(index)), 0), SubmitResult::Accepted);
    EXPECT_EQ(pending.submit(request("overflow"), 0), SubmitResult::Full);
    pending.cancel_session("s", 1);
    for (int index = 32; index < 512; ++index) {
        const auto token = std::to_string(index);
        ASSERT_EQ(pending.submit(request(token), 2), SubmitResult::Accepted);
        ASSERT_TRUE(pending.acknowledge({token, "s", "audioStart", 200, request_topic}, 2));
    }
    EXPECT_EQ(pending.submit(request("overflow"), 3), SubmitResult::Full);
    EXPECT_EQ(pending.submit(request("0"), 3), SubmitResult::Duplicate);
    EXPECT_EQ(pending.submit(request("overflow"), 600001), SubmitResult::Accepted);
}
TEST(Requests, RetainedPayloadBudgetBackpressuresAndExpires) {
    FakePublisher port;
    Requests pending(port);
    auto large = request("large");
    large.body = std::string(65536, 'x');
    ASSERT_EQ(pending.submit(large, 0), SubmitResult::Accepted);
    ASSERT_TRUE(pending.acknowledge({"large", "s", "audioStart", 200, request_topic}, 1));
    pending.cancel_session("s", 1);
    large.token = "second";
    EXPECT_EQ(pending.submit(large, 2), SubmitResult::Full);
    EXPECT_EQ(pending.submit(large, 600001), SubmitResult::Accepted);
    EXPECT_EQ(pending.state({request_topic, "audioStart", "large"}), RequestState::Missing);
}
TEST(Requests, ActiveSessionRetainsCompletedRequestsUntilTenMinutesAfterClose) {
    FakePublisher port;
    Requests pending(port);
    ASSERT_EQ(pending.submit(request(), 0), SubmitResult::Accepted);
    ASSERT_TRUE(pending.acknowledge({"t", "s", "audioStart", 200, request_topic}, 1));
    pending.tick(900000);
    EXPECT_EQ(pending.state({request_topic, "audioStart", "t"}), RequestState::Succeeded);
    EXPECT_EQ(pending.submit(request(), 900000), SubmitResult::Duplicate);
    pending.cancel_session("s", 900001);
    pending.cancel_session("s", 900002);
    pending.tick(1500000);
    EXPECT_EQ(pending.state({request_topic, "audioStart", "t"}), RequestState::Succeeded);
    pending.tick(1500001);
    EXPECT_EQ(pending.state({request_topic, "audioStart", "t"}), RequestState::Missing);
}
TEST(Requests, SessionlessRequestsExpireTenMinutesAfterCompletion) {
    FakePublisher port;
    Requests pending(port);
    auto capability = request();
    capability.session.clear();
    ASSERT_EQ(pending.submit(capability, 0), SubmitResult::Accepted);
    ASSERT_TRUE(pending.acknowledge({"t", "", "audioStart", 200, request_topic}, 1));
    pending.tick(600000);
    EXPECT_EQ(pending.state({request_topic, "audioStart", "t"}), RequestState::Succeeded);
    pending.tick(600001);
    EXPECT_EQ(pending.state({request_topic, "audioStart", "t"}), RequestState::Missing);
}
TEST(Requests, RejectsEmptyAndOversizedFields) {
    FakePublisher port;
    Requests pending(port);
    for (int field = 0; field < 5; ++field) {
        for (bool oversized : {false, true}) {
            if (field == 1 && !oversized)
                continue;
            auto invalid = request();
            auto value = oversized ? std::string(65537, 'x') : std::string{};
            if (field == 0)
                invalid.token = value;
            if (field == 1)
                invalid.session = value;
            if (field == 2)
                invalid.method = value;
            if (field == 3)
                invalid.topic = value;
            if (field == 4)
                invalid.body = value;
            EXPECT_EQ(pending.submit(std::move(invalid), 0), SubmitResult::Invalid);
        }
    }
    EXPECT_TRUE(port.bodies.empty());
}
// 长会话不能因为已完成32条请求而停止；终态仍须保留并阻止重放。
TEST(Requests, OneHundredTurnsRetainTerminalsWithoutExhaustingPendingSlots) {
    FakePublisher port;
    Requests pending(port);
    for (int index = 0; index < 300; ++index) {
        const auto token = std::to_string(index);
        ASSERT_EQ(pending.submit(request(token), static_cast<std::uint64_t>(index)),
                  SubmitResult::Accepted);
        ASSERT_TRUE(pending.acknowledge({token, "s", "audioStart", 200, request_topic},
                                        static_cast<std::uint64_t>(index)));
    }
    EXPECT_EQ(pending.submit(request("0"), 300), SubmitResult::Duplicate);
    EXPECT_EQ(port.bodies.size(), 300U);
}
TEST(Requests, ExactRetirementPreservesOtherRequestsAndOriginalDeduplication) {
    FakePublisher port;
    Requests pending(port);
    ASSERT_EQ(pending.submit(request("retired"), 10), SubmitResult::Accepted);
    ASSERT_EQ(pending.submit(request("other"), 10), SubmitResult::Accepted);
    EXPECT_FALSE(pending.cancel_request({request_topic, "audioStop", "retired"}, 11));
    EXPECT_FALSE(pending.cancel_request({request_topic, "audioStart", "retired"}, 9));
    ASSERT_TRUE(pending.cancel_request({request_topic, "audioStart", "retired"}, 12));
    EXPECT_TRUE(pending.cancel_request({request_topic, "audioStart", "retired"}, 13));
    EXPECT_EQ(pending.state({request_topic, "audioStart", "retired"}), RequestState::Cancelled);
    EXPECT_EQ(pending.submit(request("retired"), 14), SubmitResult::Duplicate);
    EXPECT_FALSE(pending.acknowledge({"retired", "s", "audioStart", 200, request_topic}, 15));
    const auto publications = port.bodies.size();
    pending.tick(10010);
    EXPECT_EQ(port.bodies.size(), publications + 1); // 只重试未退休的另一原请求。
    EXPECT_EQ(pending.state({request_topic, "audioStart", "other"}), RequestState::Pending);
}

// ExactRepublishPreservesOriginalGenericRetryBudget 核原字节、原通用重投和30秒截止独立存在。
TEST(Requests, ExactRepublishPreservesOriginalGenericRetryBudget) {
    FakePublisher port;
    Requests pending(port);
    ASSERT_EQ(pending.submit(request(), 100), SubmitResult::Accepted);
    const RequestKey key{request_topic, "audioStart", "t"};
    EXPECT_FALSE(pending.republish_pending({"wrong", "audioStart", "t"}, "s", 300));
    EXPECT_FALSE(pending.republish_pending({request_topic, "wrong", "t"}, "s", 300));
    EXPECT_FALSE(pending.republish_pending({request_topic, "audioStart", "wrong"}, "s", 300));
    EXPECT_FALSE(pending.republish_pending(key, "wrong", 300));
    EXPECT_FALSE(pending.republish_pending(key, "s", 99));
    EXPECT_EQ(port.bodies.size(), 1U);
    for (unsigned i = 1; i <= 12; ++i)
        EXPECT_TRUE(pending.republish_pending(key, "s", 100 + 200 * i));
    EXPECT_EQ(port.bodies.size(), 13U);
    pending.tick(10099);
    EXPECT_EQ(port.bodies.size(), 13U);
    pending.tick(10100);
    EXPECT_EQ(port.bodies.size(), 14U);
    pending.tick(20100);
    EXPECT_EQ(port.bodies.size(), 15U);
    for (const auto& body : port.bodies)
        EXPECT_EQ(body, port.bodies.front());
    EXPECT_FALSE(pending.republish_pending(key, "s", 30100));
    pending.tick(30100);
    EXPECT_EQ(pending.state(key), RequestState::TimedOut);
}
// RetiredOrBackpressuredRepublishCannotResetLedger 精确退休与背压不新建或延长原对象。
TEST(Requests, RetiredOrBackpressuredRepublishCannotResetLedger) {
    FakePublisher port;
    Requests pending(port);
    ASSERT_EQ(pending.submit(request(), 100), SubmitResult::Accepted);
    const RequestKey key{request_topic, "audioStart", "t"};
    port.online = false;
    EXPECT_FALSE(pending.republish_pending(key, "s", 300));
    EXPECT_EQ(pending.state(key), RequestState::Pending);
    EXPECT_TRUE(pending.acknowledge({"t", "s", "audioStart", 200, request_topic}, 301));
    EXPECT_FALSE(pending.republish_pending(key, "s", 500));
    ASSERT_EQ(pending.submit(request("other"), 600), SubmitResult::Accepted);
    pending.cancel_session("s", 601);
    EXPECT_FALSE(pending.republish_pending({request_topic, "audioStart", "other"}, "s", 800));
}
}  // namespace
}  // namespace ur::device
