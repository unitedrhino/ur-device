// 文件说明：隐私属性原报告的离线协议／时钟／连接回归；平台ACK不当作摄像头停止证明。
#include "ur/device/privacy_property.hpp"
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace ur::device {
namespace {
// PropertyPublisher 只记录原发布字节和背压，不创建平台回执或操作采集驱动。
struct PropertyPublisher final : PublishPort {
    std::vector<std::string> topics;  // 固定原设备属性路由。
    std::vector<std::string> bodies;  // 原报告字节，用于核对重试与规范编码。
    bool writable = true;             // 本地发送受理状态，不表示远端已处理。
    // publish 将原字节留在测试内存，返回明确的本地背压事实。
    bool publish(std::string_view topic, std::string_view body) override {
        topics.emplace_back(topic);
        bodies.emplace_back(body);
        return writable;
    }
};
// config 表示宿主已明确启用、物模型已核验的原设备身份。
PrivacyPropertyConfig config() { return {true, "product", "device"}; }
// report 构造当前唯一原意图，墙钟不参与本地超时预算。
PrivacyReport report() { return {"privacy-1", 1700000000123ULL, true}; }
// reply 是纯离线平台CommonMsg夹具，不通过模拟ACK证明实际停流。
std::string reply(std::string token = "privacy-1", int code = 200) {
    return "{\"method\":\"reportReply\",\"msgToken\":\"" + token +
           "\",\"code\":" + std::to_string(code) + "}";
}
// down 只属于原设备，不能借另一设备或统一信令路由完成关联。
constexpr auto down = "$thing/down/property/product/device";

TEST(PrivacyProperty, DefaultAndInvalidIdentityCannotPublish) {
    PropertyPublisher mqtt;
    for (const auto &value :
         std::vector<PrivacyPropertyConfig>{{},
                                            {false, "product", "device"},
                                            {true, "", "device"},
                                            {true, "product", ""},
                                            {true, "other/#", "device"},
                                            {true, "product", "+"},
                                            {true, std::string(65, 'p'), "device"}}) {
        PrivacyProperty property(mqtt, value);
        EXPECT_FALSE(property.enabled());
        EXPECT_TRUE(property.reply_topic().empty());
        EXPECT_FALSE(property.begin_connection(0));
        EXPECT_EQ(property.submit(report(), 1), SubmitResult::Invalid);
        EXPECT_FALSE(property.receive(down, reply(), 2));
    }
    EXPECT_TRUE(mqtt.bodies.empty());
}

TEST(PrivacyProperty, OriginalReportUsesFixedNativePropertyAndExactTimestamp) {
    PropertyPublisher mqtt;
    PrivacyProperty property(mqtt, config());
    ASSERT_TRUE(property.enabled());
    EXPECT_EQ(property.reply_topic(), down);
    EXPECT_EQ(property.submit(report(), 1), SubmitResult::Invalid);
    ASSERT_TRUE(property.begin_connection(2));
    EXPECT_FALSE(property.begin_connection(3));
    ASSERT_EQ(property.submit(report(), 4), SubmitResult::Accepted);
    ASSERT_EQ(mqtt.bodies.size(), 1U);
    EXPECT_EQ(mqtt.topics.front(), "$thing/up/property/product/device");
    EXPECT_EQ(
        mqtt.bodies.front(),
        R"({"method":"report","msgToken":"privacy-1","params":{"privacyMode":1},"timestamp":1700000000123})");
    EXPECT_EQ(property.state("privacy-1"), RequestState::Pending);
    EXPECT_FALSE(property.receive("$thing/down/property/product/other", reply(), 5));
    EXPECT_FALSE(property.receive("$thing/down/signaling/product/device", reply(), 6));
    EXPECT_FALSE(property.receive(down, reply("another"), 7));
    EXPECT_TRUE(property.receive(down, reply(), 8));
    EXPECT_EQ(property.state("privacy-1"), RequestState::Succeeded);
    EXPECT_FALSE(property.receive(down, reply("privacy-1", 100007), 9));
    EXPECT_EQ(property.state("privacy-1"), RequestState::Succeeded);
}

TEST(PrivacyProperty, DisableIsIndependentReportAndDoesNotHaveMediaSideEffects) {
    PropertyPublisher mqtt;
    PrivacyProperty property(mqtt, config());
    ASSERT_TRUE(property.begin_connection(1));
    auto original = report();
    original.enabled = false;
    original.timestamp = 9007199254740991ULL;
    ASSERT_EQ(property.submit(original, 2), SubmitResult::Accepted);
    EXPECT_EQ(
        mqtt.bodies.front(),
        R"({"method":"report","msgToken":"privacy-1","params":{"privacyMode":0},"timestamp":9007199254740991})");
    EXPECT_TRUE(property.receive(down, reply(), 3));
    EXPECT_EQ(mqtt.bodies.size(), 1U);
    property.tick(40000);
    EXPECT_EQ(mqtt.bodies.size(), 1U);
}

TEST(PrivacyProperty, InvalidIntentDoesNotReplacePendingOriginal) {
    PropertyPublisher mqtt;
    PrivacyProperty property(mqtt, config());
    ASSERT_TRUE(property.begin_connection(1));
    for (const auto &token : std::vector<std::string>{
             "", std::string(129, 'a'), "bad\n", std::string(1, '\x7f'), std::string(1, '\xff')}) {
        auto bad = report();
        bad.token = token;
        EXPECT_EQ(property.submit(bad, 2), SubmitResult::Invalid);
    }
    for (const auto value : {0ULL, 9007199254740992ULL}) {
        auto bad = report();
        bad.timestamp = value;
        EXPECT_EQ(property.submit(bad, 3), SubmitResult::Invalid);
    }
    EXPECT_TRUE(mqtt.bodies.empty());
    ASSERT_EQ(property.submit(report(), 4), SubmitResult::Accepted);
    EXPECT_EQ(property.submit(report(), 5), SubmitResult::Duplicate);
    auto changed = report();
    changed.enabled = false;
    EXPECT_EQ(property.submit(changed, 6), SubmitResult::Conflict);
    changed = report();
    changed.timestamp++;
    EXPECT_EQ(property.submit(changed, 7), SubmitResult::Conflict);
    changed.token = "privacy-2";
    EXPECT_EQ(property.submit(changed, 8), SubmitResult::Full);
    ASSERT_TRUE(property.receive(down, reply(), 9));
    EXPECT_EQ(property.submit(changed, 10), SubmitResult::Accepted);
    EXPECT_EQ(mqtt.bodies.size(), 2U);
}

TEST(PrivacyProperty, BackpressureRetainsBytesAndOriginalThirtySecondDeadline) {
    PropertyPublisher mqtt;
    mqtt.writable = false;
    PrivacyProperty property(mqtt, config());
    ASSERT_TRUE(property.begin_connection(100));
    ASSERT_EQ(property.submit(report(), 101), SubmitResult::Accepted);
    property.tick(10100);
    EXPECT_EQ(mqtt.bodies.size(), 1U);
    property.tick(10101);
    property.tick(20101);
    ASSERT_EQ(mqtt.bodies.size(), 3U);
    for (const auto &bytes : mqtt.bodies) EXPECT_EQ(bytes, mqtt.bodies.front());
    EXPECT_EQ(property.submit(report(), 30000), SubmitResult::Duplicate);
    property.tick(30101);
    EXPECT_EQ(property.state("privacy-1"), RequestState::TimedOut);
    EXPECT_FALSE(property.receive(down, reply(), 30102));
    property.tick(90000);
    EXPECT_EQ(mqtt.bodies.size(), 3U);
}

TEST(PrivacyProperty, NativeBusinessErrorIsFailureNotTimeoutOrRetry) {
    PropertyPublisher mqtt;
    PrivacyProperty property(mqtt, config());
    ASSERT_TRUE(property.begin_connection(1));
    ASSERT_EQ(property.submit(report(), 2), SubmitResult::Accepted);
    EXPECT_TRUE(property.receive(down, reply("privacy-1", 100007), 3));
    EXPECT_EQ(property.state("privacy-1"), RequestState::Failed);
    property.tick(40000);
    EXPECT_EQ(mqtt.bodies.size(), 1U);
}

TEST(PrivacyProperty, MalformedAndUnrelatedReplyCannotCompleteOriginal) {
    PropertyPublisher mqtt;
    PrivacyProperty property(mqtt, config());
    ASSERT_TRUE(property.begin_connection(1));
    ASSERT_EQ(property.submit(report(), 2), SubmitResult::Accepted);
    // 参数交换不是新身份或新应答；原请求必须保持Pending。
    EXPECT_FALSE(property.receive(reply("privacy-1", 200), down, 3));
    EXPECT_EQ(property.state("privacy-1"), RequestState::Pending);
    for (
        const auto *bad :
        {"{}", "[]", "null", R"({"method":"eventReply","msgToken":"privacy-1","code":200})",
         R"({"method":"reportReply","msgToken":"privacy-1","code":"200"})",
         R"({"method":"reportReply","msgToken":"privacy-1","code":200,"code":200})",
         R"({"method":"reportReply","msgToken":"privacy-1","code":0})",
         R"({"method":"reportReply","msgToken":"privacy-1","code":-1})",
         R"({"method":"reportReply","msgToken":"privacy-1","code":200.5})",
         R"({"method":"reportReply","msgToken":"privacy-1","code":2147483648})",
         R"({"method":"reportReply","msgToken":"privacy-1","code":200,"sessionId":"another"})",
         R"({"method":"reportReply","msgToken":"privacy-1","code":200,"msg":null})",
         R"({"method":"reportReply","msgToken":"privacy-1","code":200,"timestamp":-1})",
         R"({"method":"reportReply","msgToken":"privacy-1","code":200,"timestamp":"1"})",
         R"({"method":"reportReply","msgToken":"privacy-1","code":200,"timestamp":1.5})",
         R"({"method":"reportReply","msgToken":"privacy-1","code":200,"timestamp":9007199254740992})"}) {
        EXPECT_FALSE(property.receive(down, bad, 3));
        EXPECT_EQ(property.state("privacy-1"), RequestState::Pending);
    }
    EXPECT_TRUE(property.receive(
        down,
        R"({"method":"reportReply","msgToken":"privacy-1","code":200,"msg":"success","timestamp":0,"data":null})",
        4));
}

TEST(PrivacyProperty, DisconnectAndClockRollbackDoNotResurrectOldReport) {
    PropertyPublisher mqtt;
    PrivacyProperty property(mqtt, config());
    ASSERT_TRUE(property.begin_connection(100));
    ASSERT_EQ(property.submit(report(), 101), SubmitResult::Accepted);
    property.tick(100);
    EXPECT_EQ(property.state("privacy-1"), RequestState::Cancelled);
    EXPECT_FALSE(property.receive(down, reply(), 102));
    EXPECT_EQ(property.submit(report(), 103), SubmitResult::Invalid);
    property.disconnect(1);
    property.disconnect(104);
    ASSERT_TRUE(property.begin_connection(105));
    EXPECT_FALSE(property.receive(down, reply(), 106));
    EXPECT_EQ(property.submit(report(), 107), SubmitResult::Conflict);
    auto fresh = report();
    fresh.token = "privacy-2";
    ASSERT_EQ(property.submit(fresh, 108), SubmitResult::Accepted);
    property.disconnect(109);
    property.tick(50000);
    EXPECT_EQ(property.state("privacy-2"), RequestState::Cancelled);
    EXPECT_EQ(property.state("absent"), RequestState::Missing);
    EXPECT_EQ(mqtt.bodies.size(), 2U);
}
}  // namespace
}  // namespace ur::device
