// 文件说明：门铃物模型事件离线回归；虚拟时钟验证原意图重试、平台ACK和连接隔离，不伪造媒体成功。
#include "ur/device/doorbell_event.hpp"
#include <gtest/gtest.h>
#include <string>
#include <utility>
#include <vector>

namespace ur::device {
namespace {
// EventPublisher 仅记录发布尝试，writable模拟本地背压，不制造平台业务确认。
struct EventPublisher final : PublishPort {
    std::vector<std::string> topics;  // 原精确设备路由，不包含媒体或信令Topic。
    std::vector<std::string> bodies;  // 各次尝试的原始字节，可核对重试是否保持原意图。
    bool writable = true;             // false不意味着远端已执行或未执行。
    bool publish(std::string_view topic, std::string_view body) override {
        topics.emplace_back(topic);
        bodies.emplace_back(body);
        return writable;
    }
};
// config 由宿主明确启用单产品无参门铃事件，设备侧不提供被叫或相机选择。
DoorbellEventConfig config() { return {true, "product", "device", "doorbellPressed"}; }
// press 为一次原按压，调用者必须在重试时复用整个对象。
DoorbellPress press(std::string token = "press-1") {
    return {std::move(token), 1700000000123ULL, "{}"};
}
// reply 构造平台实际CommonMsg确认，eventID在当前平台应答中不保证出现。
std::string reply(std::string token = "press-1", int code = 200) {
    return "{\"method\":\"eventReply\",\"msgToken\":\"" + token +
           "\",\"code\":" + std::to_string(code) + "}";
}
// topic 固定同一已鉴权设备的下行事件路由。
constexpr auto topic = "$thing/down/event/product/device";

// 未启用、缺事件或非法Topic不能通过按钮动作隐式启用功能。
TEST(DoorbellEvent, DisabledOrInvalidConfigurationNeverPublishes) {
    EventPublisher mqtt;
    for (unsigned variant = 0; variant < 5; ++variant) {
        auto value = config();
        if (variant == 0)
            value.enabled = false;
        if (variant == 1)
            value.event_id.clear();
        if (variant == 2)
            value.product_id = "other/+";
        if (variant == 3)
            value.device_name = "#";
        if (variant == 4)
            value.event_id = std::string(65, 'a');
        DoorbellEvent event(mqtt, value);
        EXPECT_FALSE(event.enabled());
        EXPECT_TRUE(event.reply_topic().empty());
        EXPECT_FALSE(event.begin_connection(1));
        EXPECT_EQ(event.submit(press(), 2), SubmitResult::Invalid);
        EXPECT_FALSE(event.receive(topic, reply(), 3));
        event.tick(30000);
    }
    EXPECT_TRUE(mqtt.bodies.empty());
}

// 只有宿主声明原连接和精确下行SUBACK就绪后准入，业务ACK不带隐式会话或媒体字段。
TEST(DoorbellEvent, OriginalTopicAndExactWireAwaitBusinessAcknowledgement) {
    EventPublisher mqtt;
    DoorbellEvent event(mqtt, config());
    ASSERT_TRUE(event.enabled());
    EXPECT_EQ(event.submit(press(), 1), SubmitResult::Invalid);
    ASSERT_TRUE(event.begin_connection(2));
    EXPECT_FALSE(event.begin_connection(3));
    ASSERT_EQ(event.submit(press(), 4), SubmitResult::Accepted);
    ASSERT_EQ(mqtt.bodies.size(), 1U);
    EXPECT_EQ(mqtt.topics[0], "$thing/up/event/product/device");
    EXPECT_EQ(
        mqtt.bodies[0],
        R"({"eventID":"doorbellPressed","method":"eventPost","msgToken":"press-1","params":{},"timestamp":1700000000123})");
    EXPECT_EQ(event.state("press-1"), RequestState::Pending);
    EXPECT_FALSE(event.receive("$thing/down/event/product/other", reply(), 5));
    EXPECT_TRUE(event.receive(topic, reply(), 6));
    EXPECT_EQ(event.state("press-1"), RequestState::Succeeded);
    EXPECT_FALSE(event.receive(topic, reply("press-1", 100007), 7));
    EXPECT_EQ(event.state("press-1"), RequestState::Succeeded);
}

// 背压和结果未知只保留原字节，不根据当前墙钟重新生成时间戳或换token。
TEST(DoorbellEvent, BackpressureRetriesOriginalBytesAndTimesOutWithoutReplacement) {
    EventPublisher mqtt;
    mqtt.writable = false;
    DoorbellEvent event(mqtt, config());
    ASSERT_TRUE(event.begin_connection(100));
    ASSERT_EQ(event.submit(press(), 101), SubmitResult::Accepted);
    event.tick(10100);
    EXPECT_EQ(mqtt.bodies.size(), 1U);
    event.tick(10101);
    event.tick(20101);
    ASSERT_EQ(mqtt.bodies.size(), 3U);
    EXPECT_EQ(mqtt.bodies[0], mqtt.bodies[1]);
    EXPECT_EQ(mqtt.bodies[1], mqtt.bodies[2]);
    event.tick(30101);
    EXPECT_EQ(event.state("press-1"), RequestState::TimedOut);
    EXPECT_FALSE(event.receive(topic, reply(), 30102));
    EXPECT_EQ(event.submit(press(), 30103), SubmitResult::Duplicate);
    EXPECT_EQ(mqtt.bodies.size(), 3U);
}

// 同token同值是同意图，不重复发布；同值的新按压用新token，可以独立上报。
TEST(DoorbellEvent, DuplicatePressAndNewSameValuePressRemainDistinct) {
    EventPublisher mqtt;
    DoorbellEvent event(mqtt, config());
    ASSERT_TRUE(event.begin_connection(1));
    ASSERT_EQ(event.submit(press(), 2), SubmitResult::Accepted);
    EXPECT_EQ(event.submit(press(), 3), SubmitResult::Duplicate);
    auto changed = press();
    ++changed.timestamp;
    EXPECT_EQ(event.submit(changed, 4), SubmitResult::Conflict);
    changed = press();
    changed.params = R"({"cameraId":"front"})";
    EXPECT_EQ(event.submit(changed, 5), SubmitResult::Invalid);
    ASSERT_EQ(event.submit(press("press-2"), 6), SubmitResult::Accepted);
    ASSERT_EQ(mqtt.bodies.size(), 2U);
    EXPECT_TRUE(event.receive(topic, reply("press-2"), 7));
    EXPECT_EQ(event.state("press-1"), RequestState::Pending);
    EXPECT_EQ(event.state("press-2"), RequestState::Succeeded);
}

// 参数类型、非法JSON、控制字符和越界时间均不得进入发布队列。
TEST(DoorbellEvent, InvalidPressAndIdentityParametersNeverPublish) {
    EventPublisher mqtt;
    DoorbellEvent event(mqtt, config());
    ASSERT_TRUE(event.begin_connection(1));
    unsigned now = 2;
    for (const auto& params : {"[]", "null", "{", "{} trailing", "{\"x\":1,\"x\":2}",
                               "{\"userID\":1}", "{\"calleeId\":\"x\"}", "{\"cameraId\":\"front\"}",
                               "{\"url\":\"https://example.invalid\"}"}) {
        auto invalid = press();
        invalid.params = params;
        EXPECT_EQ(event.submit(invalid, now++), SubmitResult::Invalid);
    }
    for (const auto& token :
         {std::string{}, std::string(129, 'x'), std::string("a\n"), std::string("a\0b", 3)}) {
        auto invalid = press(token);
        EXPECT_EQ(event.submit(invalid, now++), SubmitResult::Invalid);
    }
    for (const auto timestamp : {0ULL, 9007199254740992ULL}) {
        auto invalid = press();
        invalid.timestamp = timestamp;
        EXPECT_EQ(event.submit(invalid, now++), SubmitResult::Invalid);
    }
    EXPECT_TRUE(mqtt.bodies.empty());
}

// 最大安全整数墙钟必须精确往返，不能由cJSON默认15位打印丢失原按压事实。
TEST(DoorbellEvent, SafeIntegerTimestampIsPreservedExactly) {
    EventPublisher mqtt;
    DoorbellEvent event(mqtt, config());
    ASSERT_TRUE(event.begin_connection(1));
    auto original = press();
    original.timestamp = 9007199254740991ULL;
    ASSERT_EQ(event.submit(original, 2), SubmitResult::Accepted);
    EXPECT_NE(mqtt.bodies[0].find("\"timestamp\":9007199254740991"), std::string::npos);
}

// 物模型错误码可超过HTTP范围，平台错误是Failed而非假成功或一直等待。
TEST(DoorbellEvent, ActualPlatformErrorCodesAreTerminalFailures) {
    EventPublisher mqtt;
    DoorbellEvent event(mqtt, config());
    ASSERT_TRUE(event.begin_connection(1));
    ASSERT_EQ(event.submit(press(), 2), SubmitResult::Accepted);
    EXPECT_TRUE(event.receive(topic, reply("press-1", 100007), 3));
    EXPECT_EQ(event.state("press-1"), RequestState::Failed);
    EXPECT_FALSE(event.receive(topic, reply(), 4));
    EXPECT_EQ(event.state("press-1"), RequestState::Failed);
}

// 大小写、身份作用域、关联键和可选eventID必须精确匹配；恶意字段不得消耗原在途事件。
TEST(DoorbellEvent, MalformedOrUnassociatedRepliesCannotAcknowledge) {
    EventPublisher mqtt;
    DoorbellEvent event(mqtt, config());
    ASSERT_TRUE(event.begin_connection(1));
    ASSERT_EQ(event.submit(press(), 2), SubmitResult::Accepted);
    unsigned now = 3;
    // 交换既有两个string_view参数必须拒绝，不确认或删除原待应答事件。
    EXPECT_FALSE(
        event.receive(R"({"method":"eventReply","msgToken":"press-1","code":200})", topic, now));
    EXPECT_EQ(event.state("press-1"), RequestState::Pending);
    for (const auto& body :
         {"{", "[]", "{} trailing",
          R"({"method":"eventReply","method":"eventReply","msgToken":"press-1","code":200})",
          R"({"method":"eventreply","msgToken":"press-1","code":200})",
          R"({"method":"eventReply","msgToken":"other","code":200})",
          R"({"method":"eventReply","msgToken":"press-1","code":200,"eventID":"other"})",
          R"({"method":"eventReply","msgToken":"press-1","code":200,"eventId":"doorbellPressed"})",
          R"({"method":"eventReply","msgToken":"press-1","code":"200"})",
          R"({"method":"eventReply","msgToken":"press-1","code":200.5})",
          R"({"method":"eventReply","msgToken":"press-1","code":0})",
          R"({"method":"eventReply","msgToken":"press-1","code":2147483648})",
          R"({"method":"eventReply","msgToken":"press-1","code":200,"msg":null})",
          R"({"method":"eventReply","msgToken":"press-1","code":200,"timestamp":-1})",
          R"({"method":"eventReply","msgToken":"press-1","code":200,"sessionId":"call"})"}) {
        EXPECT_FALSE(event.receive(topic, body, now++));
        EXPECT_EQ(event.state("press-1"), RequestState::Pending);
    }
    EXPECT_TRUE(event.receive(
        topic,
        R"({"method":"eventReply","msgToken":"press-1","code":200,"eventID":"doorbellPressed","timestamp":1,"msg":"ok","data":{}})",
        now));
}

// 大包、过深数据、NUL和错误字段类型必须被安全解析器拒绝，不能改变原Pending状态。
TEST(DoorbellEvent, OversizedDeepAndInvalidTypedRepliesRemainPending) {
    EventPublisher mqtt;
    DoorbellEvent event(mqtt, config());
    ASSERT_TRUE(event.begin_connection(1));
    ASSERT_EQ(event.submit(press(), 2), SubmitResult::Accepted);
    const std::string deep = R"({"method":"eventReply","msgToken":"press-1","code":200,"data":)" +
                             std::string(33, '[') + "0" + std::string(33, ']') + "}";
    EXPECT_FALSE(event.receive(topic, deep, 3));
    EXPECT_FALSE(event.receive(topic, std::string(65537, ' '), 4));
    EXPECT_FALSE(event.receive(
        topic, R"({"method":"eventReply","msgToken":"press-1\u0000","code":200})", 5));
    EXPECT_FALSE(event.receive(
        topic, R"({"method":"eventReply","msgToken":"press-1","code":200,"eventID":null})", 6));
    EXPECT_FALSE(event.receive(
        topic, R"({"method":"eventReply","msgToken":"press-1","code":200,"timestamp":1.5})", 7));
    EXPECT_EQ(event.state("press-1"), RequestState::Pending);
    EXPECT_TRUE(event.receive(topic, reply(), 8));
}

// 本地取消不撤销远端事件，晚到ACK和同token重提不得复活已取消等待。
TEST(DoorbellEvent, CancelPreservesUnknownRemoteExecutionAndAcceptsNewIntent) {
    EventPublisher mqtt;
    DoorbellEvent event(mqtt, config());
    ASSERT_TRUE(event.begin_connection(1));
    ASSERT_EQ(event.submit(press(), 2), SubmitResult::Accepted);
    event.cancel(3);
    EXPECT_EQ(event.state("press-1"), RequestState::Cancelled);
    EXPECT_FALSE(event.receive(topic, reply(), 4));
    EXPECT_EQ(event.submit(press(), 5), SubmitResult::Duplicate);
    EXPECT_EQ(event.submit(press("new"), 6), SubmitResult::Accepted);
    EXPECT_TRUE(event.receive(topic, reply("new"), 7));
    event.tick(30000);
    EXPECT_EQ(mqtt.bodies.size(), 2U);
}

// 原连接断开后保持幂等事实；下一连接不能凭迟到ACK或相同token恢复原事件。
TEST(DoorbellEvent, DisconnectAndReconnectFenceLateReplies) {
    EventPublisher mqtt;
    DoorbellEvent event(mqtt, config());
    ASSERT_TRUE(event.begin_connection(1));
    ASSERT_EQ(event.submit(press(), 2), SubmitResult::Accepted);
    event.disconnect(3);
    event.disconnect(4);
    EXPECT_FALSE(event.receive(topic, reply(), 5));
    EXPECT_EQ(event.submit(press("offline"), 6), SubmitResult::Invalid);
    event.tick(10003);
    EXPECT_EQ(mqtt.bodies.size(), 1U);
    ASSERT_TRUE(event.begin_connection(10004));
    EXPECT_FALSE(event.receive(topic, reply(), 10005));
    EXPECT_EQ(event.submit(press(), 10006), SubmitResult::Conflict);
    EXPECT_EQ(event.submit(press("next"), 10007), SubmitResult::Accepted);
    EXPECT_TRUE(event.receive(topic, reply("next"), 10008));
    EXPECT_EQ(event.state("press-1"), RequestState::Cancelled);
}

// 截止边界上的ACK不得恢复未知事件，也不能通过重复提交延长原期限。
TEST(DoorbellEvent, AckAtDeadlineCannotResurrectEvent) {
    EventPublisher mqtt;
    DoorbellEvent event(mqtt, config());
    ASSERT_TRUE(event.begin_connection(0));
    ASSERT_EQ(event.submit(press(), 1), SubmitResult::Accepted);
    EXPECT_FALSE(event.receive(topic, reply(), 30001));
    EXPECT_EQ(event.state("press-1"), RequestState::TimedOut);
    EXPECT_EQ(event.submit(press(), 30002), SubmitResult::Duplicate);
    EXPECT_EQ(mqtt.bodies.size(), 1U);
}

// 单调时钟回退先取消并关闭准入，不因错误计时继续上报或接受旧应答。
TEST(DoorbellEvent, ClockRollbackCancelsAndRequiresExplicitNewConnection) {
    EventPublisher mqtt;
    DoorbellEvent event(mqtt, config());
    ASSERT_TRUE(event.begin_connection(100));
    ASSERT_EQ(event.submit(press(), 101), SubmitResult::Accepted);
    event.tick(99);
    EXPECT_EQ(event.state("press-1"), RequestState::Cancelled);
    EXPECT_FALSE(event.receive(topic, reply(), 102));
    EXPECT_FALSE(event.begin_connection(100));
    EXPECT_TRUE(event.begin_connection(103));
    EXPECT_EQ(event.submit(press("next"), 104), SubmitResult::Accepted);
    EXPECT_EQ(mqtt.bodies.size(), 2U);
}

// 有界在途上限拒绝第三十三个按压，不能丢弃原事件身份腾位或自动换token。
TEST(DoorbellEvent, PendingCapacityBackpressuresWithoutLosingOriginalEvent) {
    EventPublisher mqtt;
    DoorbellEvent event(mqtt, config());
    ASSERT_TRUE(event.begin_connection(1));
    for (unsigned index = 0; index < 32; ++index)
        ASSERT_EQ(event.submit(press("press-" + std::to_string(index)), index + 2),
                  SubmitResult::Accepted);
    EXPECT_EQ(event.submit(press("overflow"), 34), SubmitResult::Full);
    EXPECT_EQ(event.submit(press("press-0"), 35), SubmitResult::Duplicate);
    EXPECT_EQ(event.state("press-0"), RequestState::Pending);
    EXPECT_EQ(mqtt.bodies.size(), 32U);
}
}  // namespace
}  // namespace ur::device
