// 文件说明：固定授权子会话协调器的可控驱动回归，不替代实际媒体或MQTT网络验收。
#include "ur/device/playback_session.hpp"
#include <gtest/gtest.h>
#include <deque>

namespace ur::device {
namespace {
// OutputDriver 只记录实际控制，并显式交付完成事实，不能凭begin成功报告已播放。
struct OutputDriver final : PlaybackDriver {
    bool permitted = true;           // 当前独占输出许可，撤权用例显式关闭。
    bool released = true;            // 实际驱动释放事实，与关闭请求受理分开。
    unsigned begins = 0, stops = 0;  // 驱动操作和精确关闭计数。
    std::uint64_t generation = 0;    // 最近操作内部代次。
    std::deque<PlaybackFact> facts;  // 可控事实队列。
    bool begin(const PlaybackCommand&, std::uint64_t value) override {
        ++begins;
        generation = value;
        return true;
    }
    std::optional<PlaybackFact> take_fact() override {
        if (facts.empty())
            return std::nullopt;
        auto fact = facts.front();
        facts.pop_front();
        return fact;
    }
    bool output_permitted() const override { return permitted; }
    bool stop_all() override {
        ++stops;
        return released;
    }
};
// OutputMQTT 注入发布背压，不将本地接纳当作Broker或设备执行成功。
struct OutputMQTT final : PublishPort {
    bool permitted = true;              // 是否接纳当前发布。
    std::vector<std::string> messages;  // 实际接纳的原字节。
    std::vector<std::string> attempts;  // 包括背压失败，验证重试没有重建或替换原事实。
    bool publish(std::string_view, std::string_view body) override {
        attempts.emplace_back(body);
        if (!permitted)
            return false;
        messages.emplace_back(body);
        return true;
    }
};
// invite 固定独立资源的可信测试夹具，实际宿主需另外验证内容和输出许可。
constexpr auto invitation =
    R"({"msgToken":"invite","method":"sessionInvite","sessionId":"child","eventSeq":1,"params":{"purpose":"mediaPlayback","admission":"direct","transport":"none","tracks":[],"expiresAt":10000,"playbackId":"player","mediaSource":{"contentRef":"asset"},"inputModalities":[],"outputModalities":["audio"]}})";
constexpr auto acknowledgement =
    R"({"msgToken":"invite","method":"requestAck","sessionId":"child","code":200,"data":{"requestMethod":"sessionAccept","result":{"state":"established"}}})";
constexpr auto established =
    R"({"msgToken":"established","method":"sessionEstablished","sessionId":"child","eventSeq":2,"data":{"state":"established","mediaRevision":0}})";
constexpr auto load =
    R"({"msgToken":"load","method":"playbackControl","sessionId":"child","eventSeq":3,"params":{"operation":"load","expectedRevision":0,"playbackId":"player","contentRef":"asset"}})";
// binding 严格解析固定邀请，测试不手造缺字段结构。
PlaybackInvite binding() {
    PlaybackInvite value;
    EXPECT_EQ(decode_playback_invite(WireRoute::Signaling, invitation, value), WireResult::Ok);
    return value;
}

// 两种跨通道可见顺序均需二者确认，重复控制只重放真实回执，背压保持原字节。
TEST(PlaybackSession, OrderingActualFactAndBackpressure) {
    for (const bool ack_first : {true, false}) {
        OutputMQTT mqtt;
        OutputDriver driver;
        PlaybackSession client(mqtt, driver, binding(), {1, true, false}, "up", "down", 30000);
        ASSERT_TRUE(client.receive("down", invitation, 0, 100));
        EXPECT_EQ(mqtt.messages.size(), 1U);
        EXPECT_TRUE(client.receive("down", invitation, 1, 101));
        EXPECT_FALSE(client.receive("down", load, 2, 102));
        ASSERT_TRUE(client.receive("down", ack_first ? acknowledgement : established, 3, 103));
        EXPECT_FALSE(client.receive("down", load, 4, 104));
        ASSERT_TRUE(client.receive("down", ack_first ? established : acknowledgement, 5, 105));
        ASSERT_EQ(client.phase(), PlaybackPhase::Active);
        EXPECT_FALSE(client.receive(
            "down",
            R"({"msgToken":"invite","method":"requestAck","sessionId":"child","code":403,"data":{"requestMethod":"sessionAccept","reason":"requestDenied"}})",
            5, 105));
        EXPECT_EQ(client.phase(), PlaybackPhase::Active);
        ASSERT_TRUE(client.receive("down", load, 6, 106));
        client.tick(7, 107);
        ASSERT_EQ(driver.begins, 1U);
        EXPECT_EQ(mqtt.messages.size(), 1U);
        PlaybackFact fact;
        fact.generation = driver.generation;
        fact.state = "loaded";
        driver.facts.push_back(fact);
        mqtt.permitted = false;
        client.tick(8, 108);
        mqtt.permitted = true;
        client.tick(9, 109);
        ASSERT_EQ(mqtt.messages.size(), 2U);
        EXPECT_NE(mqtt.messages.back().find("playbackState"), std::string::npos);
        ASSERT_TRUE(client.receive("down", load, 10, 110));
        EXPECT_EQ(mqtt.messages.size(), 3U);
        EXPECT_EQ(mqtt.messages[1], mqtt.messages[2]);
        EXPECT_EQ(driver.begins, 1U);
        EXPECT_FALSE(client.receive(
            "down",
            R"({"msgToken":"stale-end","method":"sessionEnded","sessionId":"child","eventSeq":2,"data":{"state":"ended","reason":"parentEnded"}})",
            11, 111));
        EXPECT_TRUE(client.receive(
            "down",
            R"({"msgToken":"end","method":"sessionEnded","sessionId":"child","eventSeq":4,"data":{"state":"ended","reason":"parentEnded"}})",
            12, 112));
        EXPECT_EQ(client.phase(), PlaybackPhase::Ended);
        EXPECT_FALSE(client.receive("down", invitation, 13, 113));
        EXPECT_EQ(driver.stops, 1U);
    }
}

// 固定授权源和完整输出不得被邀请或load替换；非法关联不能提前建立。
TEST(PlaybackSession, FixedBindingAndInvalidControl) {
    OutputMQTT mqtt;
    OutputDriver driver;
    PlaybackSession client(mqtt, driver, binding(), {1, true, false}, "up", "down", 30000);
    std::string changed = invitation;
    const auto offset = changed.find("asset");
    ASSERT_NE(offset, std::string::npos);
    changed.replace(offset, 5, "other");
    EXPECT_FALSE(client.receive("down", changed, 0, 100));
    EXPECT_TRUE(mqtt.messages.empty());
    ASSERT_TRUE(client.receive("down", invitation, 1, 101));
    EXPECT_FALSE(client.receive(
        "down",
        R"({"msgToken":"other","method":"requestAck","sessionId":"child","code":200,"data":{"requestMethod":"sessionAccept","result":{"state":"established"}}})",
        2, 102));
    ASSERT_TRUE(client.receive("down", acknowledgement, 3, 103));
    EXPECT_FALSE(client.receive(
        "down",
        R"({"msgToken":"bad-established","method":"sessionEstablished","sessionId":"child","eventSeq":2,"data":{"state":"established","mediaRevision":1}})",
        4, 104));
    ASSERT_TRUE(client.receive("down", established, 5, 105));
    changed = load;
    changed.replace(changed.find("asset"), 5, "other");
    EXPECT_FALSE(client.receive("down", changed, 6, 106));
    client.tick(7, 107);
    EXPECT_EQ(driver.begins, 0U);
}

// 逻辑结束只请求释放，异步释放确认后才公开资源完成，且不恢复已结束实例。
TEST(PlaybackSession, ActualReleaseAfterLogicalEnd) {
    OutputMQTT mqtt;
    OutputDriver driver;
    driver.released = false;
    PlaybackSession client(mqtt, driver, binding(), {1, true, false}, "up", "down", 30000);
    ASSERT_TRUE(client.receive("down", invitation, 1, 101));
    ASSERT_TRUE(client.receive("down", acknowledgement, 2, 102));
    ASSERT_TRUE(client.receive("down", established, 3, 103));
    EXPECT_FALSE(client.released());
    ASSERT_TRUE(client.receive(
        "down",
        R"({"msgToken":"end","method":"sessionEnded","sessionId":"child","eventSeq":3,"data":{"state":"ended","reason":"parentEnded"}})",
        4, 104));
    EXPECT_EQ(client.phase(), PlaybackPhase::Ended);
    EXPECT_FALSE(client.released());
    EXPECT_EQ(driver.stops, 1U);
    client.tick(5, 105);
    EXPECT_FALSE(client.released());
    EXPECT_EQ(driver.stops, 2U);
    driver.released = true;
    client.tick(30001, 30001);  // 授权到期不应阻止旧实例收敛释放，仍不允许新输出。
    EXPECT_TRUE(client.released());
    const auto stops = driver.stops;
    client.tick(30002, 30002);
    client.close(30003);
    EXPECT_EQ(driver.stops, stops);
    EXPECT_FALSE(client.receive("down", invitation, 30004, 30004));
    EXPECT_EQ(driver.begins, 0U);
    ASSERT_EQ(mqtt.messages.size(), 2U);  // 一条接听与一条独立释放，不能偷写loaded/playing。
    EXPECT_NE(mqtt.messages.back().find("\"released\":true"), std::string::npos);
}

// 本地先关闭不丢失随后平台终态；错误身份、旧水位和重复终态不恢复任何资源。
TEST(PlaybackSession, PlatformEndAfterLocalClosure) {
    OutputMQTT mqtt;
    OutputDriver driver;
    driver.released = false;
    PlaybackSession client(mqtt, driver, binding(), {1, true, false}, "up", "down", 30000);
    ASSERT_TRUE(client.receive("down", invitation, 1, 101));
    ASSERT_TRUE(client.receive("down", acknowledgement, 2, 102));
    ASSERT_TRUE(client.receive("down", established, 3, 103));
    driver.permitted = false;
    client.tick(4, 104);
    ASSERT_EQ(client.phase(), PlaybackPhase::Ended);
    constexpr auto end =
        R"({"msgToken":"end","method":"sessionEnded","sessionId":"child","eventSeq":3,"data":{"state":"ended","reason":"playbackStopped"}})";
    EXPECT_FALSE(client.receive("other", end, 5, 105));
    EXPECT_FALSE(client.receive(
        "down",
        R"({"msgToken":"end","method":"sessionEnded","sessionId":"other-child","eventSeq":3,"data":{"state":"ended","reason":"playbackStopped"}})",
        5, 105));
    EXPECT_FALSE(client.receive(
        "down",
        R"({"msgToken":"end","method":"sessionEnded","sessionId":"child","eventSeq":2,"data":{"state":"ended","reason":"playbackStopped"}})",
        5, 105));
    EXPECT_FALSE(client.receive("down", established, 5, 105));
    EXPECT_TRUE(client.receive("down", end, 30001, 30001));
    EXPECT_FALSE(client.receive("down", end, 30002, 30002));
    // 终态只绑定首次合法原token；较新水位也不能替换已固定的释放关联。
    EXPECT_FALSE(client.receive(
        "down",
        R"({"msgToken":"replacement-end","method":"sessionEnded","sessionId":"child","eventSeq":4,"data":{"state":"ended","reason":"parentEnded"}})",
        30002, 30002));
    EXPECT_FALSE(client.receive("down", load, 30003, 30003));
    EXPECT_FALSE(client.released());
    EXPECT_EQ(driver.stops, 1U);
    EXPECT_EQ(driver.begins, 0U);
    EXPECT_EQ(mqtt.messages.size(), 1U);
}

// 原平台终态与驱动整组释放缺一不可，发布背压不丢失原token或复用播放器。
TEST(PlaybackSession, ReleaseFactAfterActualDriverAndBackpressure) {
    OutputMQTT mqtt;
    OutputDriver driver;
    driver.released = false;
    PlaybackSession client(mqtt, driver, binding(), {1, true, false}, "up", "down", 30000);
    ASSERT_TRUE(client.receive("down", invitation, 1, 101));
    ASSERT_TRUE(client.receive("down", acknowledgement, 2, 102));
    ASSERT_TRUE(client.receive("down", established, 3, 103));
    client.close(4);
    driver.released = true;
    client.tick(5, 105);
    EXPECT_TRUE(client.released());
    EXPECT_EQ(mqtt.messages.size(), 1U);  // 只有本地结束，不能伪造平台终态关联。
    ASSERT_TRUE(client.receive(
        "down",
        R"({"msgToken":"platform-end","method":"sessionEnded","sessionId":"child","eventSeq":3,"data":{"state":"ended","reason":"parentEnded"}})",
        6, 106));
    mqtt.permitted = false;
    client.tick(7, 107);
    EXPECT_EQ(mqtt.messages.size(), 1U);
    const auto blocked = mqtt.attempts.back();
    mqtt.permitted = true;
    client.tick(8, 108);
    ASSERT_EQ(mqtt.messages.size(), 2U);
    EXPECT_EQ(mqtt.messages.back(), blocked);
    Envelope release;
    ASSERT_EQ(decode_envelope(mqtt.messages.back(), release), DecodeResult::Ok);
    EXPECT_EQ(release.token, "platform-end");
    EXPECT_EQ(release.session, "child");
    EXPECT_EQ(release.method, "playbackState");
    EXPECT_FALSE(release.code.has_value());
    EXPECT_FALSE(release.event_sequence.has_value());
    EXPECT_EQ(release.payload, R"({"endEventSeq":3,"playbackId":"player","released":true})");
    client.tick(9, 109);
    EXPECT_EQ(mqtt.messages.size(), 2U);
    EXPECT_EQ(driver.begins, 0U);
}

// 已确认loaded但发布背压时结束，原操作事实必须先交付，不能被独立released事实覆盖。
TEST(PlaybackSession, PlatformEndPreservesBlockedActualReplyBeforeRelease) {
    OutputMQTT mqtt;
    OutputDriver driver;
    PlaybackSession client(mqtt, driver, binding(), {1, true, false}, "up", "down", 30000);
    ASSERT_TRUE(client.receive("down", invitation, 1, 101));
    ASSERT_TRUE(client.receive("down", acknowledgement, 2, 102));
    ASSERT_TRUE(client.receive("down", established, 3, 103));
    ASSERT_TRUE(client.receive("down", load, 4, 104));
    client.tick(5, 105);
    driver.facts.push_back({driver.generation, false, 200, "loaded", 0, {}, 1});
    mqtt.permitted = false;
    client.tick(6, 106);
    const auto original = mqtt.attempts.back();
    ASSERT_NE(original.find("\"msgToken\":\"load\""), std::string::npos);
    ASSERT_TRUE(client.receive(
        "down",
        R"({"msgToken":"end","method":"sessionEnded","sessionId":"child","eventSeq":4,"data":{"state":"ended","reason":"parentEnded"}})",
        7, 107));
    client.tick(8, 108);
    EXPECT_TRUE(client.released());
    EXPECT_FALSE(client.release_submitted());
    EXPECT_EQ(mqtt.attempts.back(), original);
    mqtt.permitted = true;
    client.tick(9, 109);
    ASSERT_GE(mqtt.messages.size(), 2U);
    EXPECT_EQ(mqtt.messages[1], original);
    for (unsigned now = 10; now < 14; ++now)
        client.tick(now, now + 100);
    ASSERT_TRUE(client.release_submitted());
    EXPECT_NE(mqtt.messages.back().find("\"released\":true"), std::string::npos);
    EXPECT_EQ(driver.begins, 1U);
    EXPECT_FALSE(client.receive("down", load, 15, 115));
}

// 非法邀请、错误身份、撤权/时钟倒退/邀请超时或拒绝ACK不得执行任何媒体控制。
TEST(PlaybackSession, AdmissionAndLifecycleGuards) {
    for (unsigned scenario = 0; scenario < 5; ++scenario) {
        OutputMQTT mqtt;
        OutputDriver driver;
        PlaybackSession client(mqtt, driver, binding(), {1, true, false}, "up", "down", 30000);
        EXPECT_FALSE(client.receive("other-topic", invitation, 100, 100));
        EXPECT_FALSE(client.receive("down", invitation, 100, 10000));
        ASSERT_TRUE(client.receive("down", invitation, 100, 100));
        if (scenario == 0) {
            driver.permitted = false;
            client.tick(101, 101);
        } else if (scenario == 1)
            client.tick(99, 101);
        else if (scenario == 2)
            client.tick(101, 10000);
        else if (scenario == 3)
            client.tick(30000, 101);
        else
            EXPECT_TRUE(client.receive(
                "down",
                R"({"msgToken":"invite","method":"requestAck","sessionId":"child","code":403,"data":{"requestMethod":"sessionAccept","reason":"requestDenied"}})",
                101, 101));
        EXPECT_EQ(client.phase(), PlaybackPhase::Ended);
        EXPECT_EQ(driver.begins, 0U);
        EXPECT_EQ(driver.stops, 1U);
    }
}
}  // namespace
}  // namespace ur::device
