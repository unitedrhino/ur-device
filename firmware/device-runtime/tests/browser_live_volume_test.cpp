// 文件说明：音量聚合实际读回与原动作关联回归；不表示扬声器声学或跨端MCP验收。
#include "browser_live_volume.hpp"
#include <gtest/gtest.h>
namespace ur::device {
namespace {
// DeferredActualVolume通过原异步队列给真实读回；未提交事实时不改变实际增益。
struct DeferredActualVolume final : VolumePort, DeferredVolumePort {
    unsigned actual = 100;
    std::optional<VolumeApplied> request, fact;
    bool set_volume(unsigned) override { return false; }
    unsigned volume() const override { return actual; }
    bool request_volume(std::string_view token, unsigned percent) override {
        if (request || fact)
            return false;
        request = VolumeApplied{std::string(token), percent};
        return true;
    }
    std::optional<VolumeApplied> take_volume_applied() override {
        auto result = std::move(fact);
        fact.reset();
        return result;
    }
    // apply仅测试驱动的独立实际回调，不从被测请求返回值造成功。
    void apply() {
        actual = request->percent;
        fact = std::move(request);
        request.reset();
    }
};
// ActualVolume 明确模拟实际驱动设置/失败，不能从请求百分比构造成功事实。
struct ActualVolume final : VolumePort {
    unsigned actual = 100, calls = 0;
    bool refuse = false;
    bool set_volume(unsigned percent) override {
        ++calls;
        if (refuse)
            return false;
        actual = percent;
        return true;
    }
    unsigned volume() const override { return actual; }
};
// receipt 本地票据与动作token分离，不允许网页回传平台原token。
std::string receipt(unsigned percent = 40, std::string ticket = "1") {
    return "{\"kind\":\"liveVolumeApplied\",\"volumeTicket\":\"" + ticket +
           "\",\"percent\":" + std::to_string(percent) + "}";
}
}  // namespace
TEST(BrowserLiveVolumeTest, ActualLiveAndPcmConfirmationPrecedeOriginalActionCompletion) {
    ActualVolume pcm;
    BrowserLiveVolume volume(pcm);
    volume.poll(10);
    EXPECT_FALSE(volume.request_volume("original", 40));
    volume.ready(true);
    ASSERT_TRUE(volume.request_volume("original", 40));
    EXPECT_EQ(volume.volume(), 100U);
    EXPECT_FALSE(volume.take_volume_applied());
    ASSERT_TRUE(volume.take_command());
    EXPECT_EQ(pcm.calls, 0U);
    ASSERT_TRUE(volume.receive(receipt()));
    EXPECT_EQ(volume.volume(), 40U);
    const auto result = volume.take_volume_applied();
    ASSERT_TRUE(result);
    EXPECT_EQ(result->token, "original");
    EXPECT_EQ(result->percent, 40U);
    EXPECT_FALSE(volume.take_volume_applied());
    EXPECT_FALSE(volume.receive(receipt()));
}

TEST(BrowserLiveVolumeTest, DeferredAIAndLiveRequireBothActualOrders) {
    for (bool ai_first : {false, true}) {
        DeferredActualVolume ai;
        BrowserLiveVolume volume(ai, &ai);
        volume.poll(10);
        volume.ready(true);
        ASSERT_TRUE(volume.request_volume("original-action", 40));
        ASSERT_EQ(ai.request->token, "live-volume-1");
        ASSERT_TRUE(volume.take_command());
        if (ai_first) {
            ai.apply();
            volume.poll(11);
            EXPECT_FALSE(volume.take_volume_applied());
            ASSERT_TRUE(volume.receive(receipt()));
        } else {
            ASSERT_TRUE(volume.receive(receipt()));
            EXPECT_FALSE(volume.take_volume_applied());
            ai.apply();
            volume.poll(11);
        }
        const auto complete = volume.take_volume_applied();
        ASSERT_TRUE(complete);
        EXPECT_EQ(complete->token, "original-action");
        EXPECT_EQ(complete->percent, 40U);
        EXPECT_FALSE(volume.take_volume_applied());
    }
}
TEST(BrowserLiveVolumeTest, DeferredUnknownOrWrongOriginalCannotClaimSuccess) {
    for (bool late : {false, true}) {
        DeferredActualVolume ai;
        BrowserLiveVolume volume(ai, &ai);
        volume.poll(10);
        volume.ready(true);
        ASSERT_TRUE(volume.request_volume("original-action", 40));
        ASSERT_TRUE(volume.take_command());
        ASSERT_TRUE(volume.receive(receipt()));
        ai.apply();
        if (!late)
            ai.fact->token = "different";
        volume.poll(late ? 2010 : 11);
        EXPECT_FALSE(volume.take_volume_applied());
        EXPECT_FALSE(volume.request_volume("new-action", 40));
    }
}
// 原AI拒绝受理时不能仍向Live发另一副作用，也不能借当前音量等于目标值完成原动作。
TEST(BrowserLiveVolumeTest, DeferredAIRejectCannotEmitLiveOrCreateOriginalSuccess) {
    DeferredActualVolume ai;
    ai.request = VolumeApplied{"other-existing", 40};
    BrowserLiveVolume volume(ai, &ai);
    volume.poll(10);
    volume.ready(true);
    EXPECT_FALSE(volume.request_volume("original-action", 40));
    EXPECT_FALSE(volume.take_command());
    EXPECT_FALSE(volume.take_volume_applied());
    EXPECT_FALSE(volume.receive(receipt()));
    EXPECT_EQ(ai.actual, 100U);
}
TEST(BrowserLiveVolumeTest, BrowserCannotAcknowledgeBeforeCommandDispatchOrAlterParameters) {
    ActualVolume pcm;
    BrowserLiveVolume volume(pcm);
    volume.poll(10);
    volume.ready(true);
    ASSERT_TRUE(volume.request_volume("private-action", 40));
    EXPECT_FALSE(volume.receive(receipt()));
    const auto command = volume.take_command();
    ASSERT_TRUE(command);
    EXPECT_EQ(command->find("private-action"), std::string::npos);
    EXPECT_FALSE(volume.receive(receipt(41)));
    EXPECT_FALSE(volume.receive(receipt(40, "01")));
    EXPECT_FALSE(volume.receive(receipt(40, "2")));
    EXPECT_EQ(pcm.calls, 0U);
    EXPECT_TRUE(volume.receive(receipt()));
}
TEST(BrowserLiveVolumeTest, StrictMalformedReceiptsNeverSetPcm) {
    ActualVolume pcm;
    BrowserLiveVolume volume(pcm);
    volume.poll(10);
    volume.ready(true);
    ASSERT_TRUE(volume.request_volume("original", 40));
    ASSERT_TRUE(volume.take_command());
    EXPECT_FALSE(volume.receive("{}"));
    EXPECT_FALSE(volume.receive(
        R"({"kind":"liveVolumeApplied","volumeTicket":"1","percent":40,"token":"original"})"));
    EXPECT_FALSE(volume.receive(
        R"({"kind":"liveVolumeApplied","volumeTicket":"1","percent":40,"percent":40})"));
    EXPECT_FALSE(
        volume.receive(R"({"kind":"liveVolumeApplied","volumeTicket":"1","percent":null})"));
    EXPECT_EQ(pcm.calls, 0U);
    EXPECT_FALSE(volume.take_volume_applied());
}
TEST(BrowserLiveVolumeTest, PcmFailureOrLateCloseCannotInventFortyPercentSuccess) {
    ActualVolume pcm;
    BrowserLiveVolume volume(pcm);
    volume.poll(10);
    volume.ready(true);
    ASSERT_TRUE(volume.request_volume("original", 40));
    ASSERT_TRUE(volume.take_command());
    pcm.refuse = true;
    EXPECT_FALSE(volume.receive(receipt()));
    EXPECT_EQ(volume.volume(), 100U);
    EXPECT_FALSE(volume.take_volume_applied());
    volume.close();
    pcm.refuse = false;
    EXPECT_FALSE(volume.receive(receipt()));
    EXPECT_FALSE(volume.request_volume("other", 40));
}
TEST(BrowserLiveVolumeTest, UnknownTimeoutAndClockRegressionArePermanentNotRetryableSuccess) {
    ActualVolume pcm;
    BrowserLiveVolume volume(pcm);
    volume.poll(10);
    volume.ready(true);
    ASSERT_TRUE(volume.request_volume("original", 40));
    ASSERT_TRUE(volume.take_command());
    volume.poll(2010);
    EXPECT_FALSE(volume.receive(receipt()));
    volume.ready(true);
    EXPECT_FALSE(volume.request_volume("new", 40));
    EXPECT_EQ(pcm.calls, 0U);
    EXPECT_FALSE(volume.take_volume_applied());
    BrowserLiveVolume regressed(pcm);
    regressed.poll(10);
    regressed.ready(true);
    regressed.poll(9);
    EXPECT_FALSE(regressed.request_volume("new", 40));
}
TEST(BrowserLiveVolumeTest, SameValueNewActionGetsNewTicketAndPendingFactsRemainBounded) {
    ActualVolume pcm;
    BrowserLiveVolume volume(pcm);
    volume.poll(10);
    volume.ready(true);
    EXPECT_FALSE(volume.set_volume(40));
    EXPECT_FALSE(volume.request_volume("", 40));
    EXPECT_FALSE(volume.request_volume("original", 101));
    ASSERT_TRUE(volume.request_volume("first", 40));
    EXPECT_FALSE(volume.request_volume("second", 40));
    ASSERT_TRUE(volume.take_command());
    ASSERT_TRUE(volume.receive(receipt()));
    EXPECT_FALSE(volume.request_volume("second", 40));
    ASSERT_TRUE(volume.take_volume_applied());
    ASSERT_TRUE(volume.request_volume("second", 40));
    ASSERT_TRUE(volume.take_command());
    EXPECT_FALSE(volume.receive(receipt()));
    ASSERT_TRUE(volume.receive(receipt(40, "2")));
    EXPECT_EQ(volume.take_volume_applied()->token, "second");
}
}  // namespace ur::device
