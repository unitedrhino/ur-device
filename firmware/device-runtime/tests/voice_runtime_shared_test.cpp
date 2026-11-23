// 文件说明：AI共享宿主借用合同；能力ACK由宿主验证，AI只创建自己的会话，不消费或断开他腿网络。
#include <gtest/gtest.h>
#include <deque>
#include "ur/device/voice_runtime.hpp"

namespace ur::device {
namespace {
// HostMqtt记录网络所有权调用；这里只验证生产编码及运行时，不把夹具称真实联网。
struct HostMqtt final : MqttPort {
    unsigned connects = 0, subscriptions = 0, polls = 0, reads = 0, disconnects = 0;
    bool publishing = true;           // 显式注入上行背压。
    std::vector<Envelope> sent;       // 原生产编码结果。
    std::deque<MqttEvent> unrelated;  // 宿主尚未分派的人工腿事件。
    bool connect(const MqttConfig&) override {
        ++connects;
        return true;
    }
    int subscribe(std::string_view) override { return static_cast<int>(++subscriptions); }
    bool poll() override {
        ++polls;
        return true;
    }
    bool publications_drained() override { return true; }
    std::optional<MqttEvent> take_event() override {
        ++reads;
        if (unrelated.empty())
            return {};
        auto value = std::move(unrelated.front());
        unrelated.pop_front();
        return value;
    }
    bool publish(std::string_view topic, std::string_view bytes) override {
        EXPECT_EQ(topic, "$thing/up/signaling/p/d");
        Envelope value;
        EXPECT_EQ(decode_envelope(bytes, value), DecodeResult::Ok);
        sent.push_back(value);
        return publishing;
    }
    void disconnect() override {
        ++disconnects;
        unrelated.clear();
    }
    // untouched核唯一网络拥有者未被借用端调用；publish是合法借用，不计此组。
    void untouched() const {
        EXPECT_EQ(connects, 0U);
        EXPECT_EQ(subscriptions, 0U);
        EXPECT_EQ(polls, 0U);
        EXPECT_EQ(reads, 0U);
        EXPECT_EQ(disconnects, 0U);
    }
};
// capability只声明None文字；这些用例检验共享控制面，不伪造麦克风或媒体事实。
VoiceCapability capability() {
    VoiceCapability value;
    value.transport = VoiceTransport::None;
    value.text_input = true;
    return value;
}
// SharedVoiceTest由可信宿主已验证的统一能力阶段启动，后续ACK仍须原请求严格关联。
class SharedVoiceTest : public testing::Test {
protected:
    HostMqtt mqtt;
    VoiceRuntime runtime{mqtt, "p/d", capability(), "shared-ai", BorrowedVoiceConnection{}};
    // deliver只构造原AI定向报文；归属分派由宿主决定，不能从本端bool自造租约。
    bool deliver(Envelope envelope, std::uint64_t now = 101) {
        std::string bytes;
        EXPECT_EQ(encode_envelope(envelope, bytes), DecodeResult::Ok);
        return runtime.receive_shared({"$thing/down/signaling/p/d", bytes}, now, 1000000);
    }
};
TEST_F(SharedVoiceTest, StartsOnlyOriginalSessionWithoutSecondCapabilityOrNetworkOwner) {
    mqtt.unrelated.push_back({MqttEvent::Kind::Message, 0, 0, "other", "unread"});
    ASSERT_EQ(runtime.start_shared(100), Result::Applied);
    ASSERT_EQ(mqtt.sent.size(), 1U);
    EXPECT_EQ(mqtt.sent.front().method, "sessionCreate");
    EXPECT_EQ(runtime.voice().state(), VoiceState::Creating);
    runtime.poll(101, 1000000);
    EXPECT_EQ(mqtt.unrelated.size(), 1U);
    mqtt.untouched();
}
TEST_F(SharedVoiceTest, BorrowedInstanceCannotOpenOwnedConnection) {
    EXPECT_EQ(runtime.start({}, 100), Result::Unsupported);
    EXPECT_TRUE(mqtt.sent.empty());
    mqtt.untouched();
}
TEST_F(SharedVoiceTest, ClosingAndDestroyingOriginalAiNeverDisconnectsHumanHost) {
    mqtt.unrelated.push_back({MqttEvent::Kind::Message, 0, 0, "other", "unread"});
    ASSERT_EQ(runtime.start_shared(100), Result::Applied);
    runtime.close(101);
    runtime.close(102);
    {
        VoiceRuntime local{mqtt, "p/d", capability(), "other-ai", BorrowedVoiceConnection{}};
    }
    EXPECT_EQ(mqtt.unrelated.size(), 1U);
    mqtt.untouched();
}
TEST_F(SharedVoiceTest, DuplicateStartDoesNotCreateSecondAiIntent) {
    ASSERT_EQ(runtime.start_shared(100), Result::Applied);
    EXPECT_EQ(runtime.start_shared(101), Result::Busy);
    EXPECT_EQ(mqtt.sent.size(), 1U);
    mqtt.untouched();
}
TEST_F(SharedVoiceTest, FailedPublishLeavesHostAliveAndDoesNotReportCapability) {
    mqtt.publishing = false;
    // Requests接纳原在途并有界同token重试；Applied不是Broker送达或会话已建立。
    ASSERT_EQ(runtime.start_shared(100), Result::Applied);
    ASSERT_EQ(mqtt.sent.size(), 1U);
    const auto original = mqtt.sent.front();
    EXPECT_EQ(runtime.voice().state(), VoiceState::Creating);
    runtime.poll(10100, 1000000);
    runtime.poll(20100, 1000000);
    ASSERT_EQ(mqtt.sent.size(), 3U);
    for (const auto& sent : mqtt.sent) {
        EXPECT_EQ(sent.method, "sessionCreate");
        EXPECT_EQ(sent.token, original.token);
        EXPECT_EQ(sent.payload, original.payload);
    }
    runtime.poll(30100, 1000000);
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
    mqtt.untouched();
}
TEST_F(SharedVoiceTest, MalformedDirectedMessageClosesOnlyOriginalAi) {
    ASSERT_EQ(runtime.start_shared(100), Result::Applied);
    EXPECT_FALSE(
        runtime.receive_shared({"$thing/down/signaling/p/d", "invalid-json"}, 101, 1000000));
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
    mqtt.untouched();
}
TEST_F(SharedVoiceTest, UnrelatedTopicCannotChangeOriginalAiState) {
    ASSERT_EQ(runtime.start_shared(100), Result::Applied);
    EXPECT_FALSE(
        runtime.receive_shared({"$thing/down/signaling/other/d", "invalid-json"}, 101, 1000000));
    EXPECT_EQ(runtime.state(), RuntimeState::Running);
    mqtt.untouched();
}
TEST_F(SharedVoiceTest, OversizedOriginalMessageClosesOnlyAiBeforeCopyingHostQueue) {
    ASSERT_EQ(runtime.start_shared(100), Result::Applied);
    const std::string oversized(65537, 'x');
    EXPECT_FALSE(runtime.receive_shared({"$thing/down/signaling/p/d", oversized}, 101, 1000000));
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
    mqtt.untouched();
}
TEST_F(SharedVoiceTest, ClockRollbackClosesAiWithoutConsumingSharedEvents) {
    ASSERT_EQ(runtime.start_shared(100), Result::Applied);
    runtime.poll(99, 1000000);
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
    mqtt.untouched();
}
TEST_F(SharedVoiceTest, CapabilityInvalidationDoesNotAutonomouslyRewriteHostSnapshot) {
    ASSERT_EQ(runtime.start_shared(100), Result::Applied);
    Envelope ack;
    ack.token = mqtt.sent.front().token;
    ack.method = "requestAck";
    ack.request = false;
    ack.code = 409;
    ack.payload = R"({"requestMethod":"sessionCreate","reason":"capabilityReportRequired"})";
    deliver(ack);
    EXPECT_EQ(runtime.voice().state(), VoiceState::Failed);
    runtime.poll(102, 1000000);
    EXPECT_EQ(mqtt.sent.size(), 1U);
    mqtt.untouched();
}
TEST_F(SharedVoiceTest, IndependentNextAiUsesNewTokenWithoutNetworkReconnect) {
    ASSERT_EQ(runtime.start_shared(100), Result::Applied);
    const auto original = mqtt.sent.front().token;
    runtime.close(101);
    ASSERT_EQ(runtime.start_shared(102), Result::Applied);
    ASSERT_EQ(mqtt.sent.size(), 2U);
    EXPECT_NE(original, mqtt.sent.back().token);
    EXPECT_EQ(mqtt.sent.back().method, "sessionCreate");
    mqtt.untouched();
}
TEST(SharedVoiceContract, OwnedInstanceCannotConsumeBorrowedHostMessages) {
    HostMqtt mqtt;
    VoiceRuntime runtime{mqtt, "p/d", capability(), "owned-ai"};
    EXPECT_EQ(runtime.start_shared(100), Result::Unsupported);
    EXPECT_FALSE(
        runtime.receive_shared({"$thing/down/signaling/p/d", "invalid-json"}, 101, 1000000));
    EXPECT_EQ(mqtt.connects, 0U);
    EXPECT_EQ(mqtt.reads, 0U);
}
TEST(SharedVoiceContract, WrongIdentityIsRejectedBeforeAnyPublication) {
    HostMqtt mqtt;
    VoiceRuntime runtime{mqtt, "p/+/d", capability(), "shared-ai", BorrowedVoiceConnection{}};
    EXPECT_EQ(runtime.start_shared(100), Result::Invalid);
    EXPECT_TRUE(mqtt.sent.empty());
    mqtt.untouched();
}
}  // namespace
}  // namespace ur::device
