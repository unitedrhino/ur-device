// 软件动作驱动验证：回读真实播放设置、失败不改值、实际PCM增益不影响上行。
#include "file_actions.hpp"
#include <gtest/gtest.h>
#include "file_pcm.hpp"

namespace ur::device {
TEST(FileActions, SetReadAndActualPlaybackGain) {
    FilePcm pcm({16000, 1, 20});
    FileActions driver(pcm);
    ASSERT_TRUE(driver.begin({"set", "setVolume", R"({"volume":40})"}));
    ASSERT_TRUE(driver.take_completion());
    EXPECT_EQ(pcm.volume(), 40U);
    ASSERT_TRUE(driver.begin({"read", "getVolume", "{}"}));
    const auto result = driver.take_completion();
    ASSERT_TRUE(result);
    EXPECT_EQ(result->code, 200);
    EXPECT_EQ(result->data, R"({"volume":40})");
    ASSERT_TRUE(pcm.queue_input({16000, std::vector<std::int16_t>(320, 10000)}));
    ASSERT_TRUE(pcm.begin_capture("input"));
    ASSERT_TRUE(pcm.begin_playback("reply", 1));
    ASSERT_TRUE(pcm.write_frame(std::vector<std::int16_t>(320, -10000), 1));
    ASSERT_TRUE(pcm.poll(0));
    std::vector<std::int16_t> input;
    ASSERT_EQ(pcm.read_frame(input), PcmResult::Frame);
    EXPECT_EQ(input.front(), 10000);
    ASSERT_TRUE(pcm.poll(20));
    EXPECT_EQ(pcm.rendered().samples, std::vector<std::int16_t>(320, -4000));
}
TEST(FileActions, InvalidArgumentsDoNotChangeDriverState) {
    FilePcm pcm({16000, 1, 20});
    FileActions driver(pcm);
    for (const auto* params : {"{}", "[]", R"({"volume":true})", R"({"volume":40.5})",
                               R"({"volume":-1})", R"({"volume":101})", R"({"volume":"40"})",
                               R"({"volume":40,"other":1})", R"({"volume":40,"volume":50})"}) {
        ASSERT_TRUE(driver.begin({"bad", "setVolume", params}));
        auto result = driver.take_completion();
        ASSERT_TRUE(result);
        EXPECT_EQ(result->code, 422) << params;
        EXPECT_EQ(pcm.volume(), 100U);
    }
    EXPECT_FALSE(pcm.set_volume(101));
    ASSERT_TRUE(driver.begin({"unknown", "takePhoto", "{}"}));
    EXPECT_EQ(driver.take_completion()->code, 501);
}
// 增益端点必须支持真正静音和满幅有符号样本，不能溢出或影响下一帧恢复。
TEST(FileActions, MuteAndFullScalePlaybackEndpoints) {
    FilePcm pcm({16000, 1, 20});
    FileActions driver(pcm);
    ASSERT_TRUE(pcm.begin_playback("reply", 1));
    std::vector<std::int16_t> frame(320, -32768);
    frame.back() = 32767;
    ASSERT_TRUE(driver.begin({"mute", "setVolume", R"({"volume":0})"}));
    ASSERT_EQ(driver.take_completion()->code, 200);
    ASSERT_TRUE(pcm.write_frame(frame, 1));
    ASSERT_TRUE(pcm.poll(0));
    ASSERT_TRUE(pcm.poll(20));
    EXPECT_EQ(pcm.rendered().samples, std::vector<std::int16_t>(320, 0));
    ASSERT_TRUE(driver.begin({"restore", "setVolume", R"({"volume":100})"}));
    ASSERT_EQ(driver.take_completion()->code, 200);
    ASSERT_TRUE(pcm.write_frame(frame, 1));
    ASSERT_TRUE(pcm.poll(20));
    ASSERT_TRUE(pcm.poll(40));
    ASSERT_EQ(pcm.rendered().samples.size(), 640U);
    EXPECT_EQ(pcm.rendered().samples[320], -32768);
    EXPECT_EQ(pcm.rendered().samples.back(), 32767);
}
TEST(FileActions, PendingCancellationAndFailedAudioDriver) {
    FilePcm pcm({16000, 1, 20});
    FileActions driver(pcm);
    EXPECT_EQ(driver.completed_count(), 0U);
    EXPECT_EQ(driver.last_code(), 0);
    ASSERT_TRUE(driver.begin({"a", "getVolume", "{}"}));
    EXPECT_EQ(driver.completed_count(), 1U);
    EXPECT_EQ(driver.last_code(), 200);
    EXPECT_FALSE(driver.begin({"b", "getVolume", "{}"}));
    EXPECT_EQ(driver.completed_count(), 1U);
    EXPECT_TRUE(driver.cancel("other"));
    ASSERT_TRUE(driver.take_completion());
    EXPECT_FALSE(driver.take_completion());
    EXPECT_EQ(driver.completed_count(), 1U);
    ASSERT_TRUE(driver.begin({"a", "getVolume", "{}"}));
    EXPECT_TRUE(driver.cancel("a"));
    EXPECT_FALSE(driver.take_completion());
    EXPECT_EQ(driver.completed_count(), 2U);
    ASSERT_TRUE(pcm.poll(10));
    EXPECT_FALSE(pcm.poll(9));
    ASSERT_TRUE(driver.begin({"set", "setVolume", R"({"volume":40})"}));
    EXPECT_EQ(driver.take_completion()->code, 503);
    EXPECT_EQ(driver.last_code(), 503);
    EXPECT_EQ(driver.completed_count(), 3U);
    EXPECT_EQ(pcm.volume(), 100U);
}
// 异步浏览器增益只有收到应用确认后才能形成成功的设备动作回执。
TEST(FileActions, DeferredVolumeRequiresAppliedAck) {
    class Deferred final : public VolumePort, public DeferredVolumePort {
    public:
        bool set_volume(unsigned) override { return false; }
        unsigned volume() const override { return value; }
        bool request_volume(std::string_view token, unsigned percent) override {
            pending = VolumeApplied{std::string(token), percent};
            return true;
        }
        std::optional<VolumeApplied> take_volume_applied() override {
            auto result = ack;
            ack.reset();
            return result;
        }
        unsigned value = 100;
        std::optional<VolumeApplied> pending;
        std::optional<VolumeApplied> ack;
    } port;
    FileActions driver(port, &port);
    ASSERT_TRUE(driver.begin({"tool-1", "setVolume", R"({"volume":40})"}));
    EXPECT_FALSE(driver.take_completion());
    EXPECT_EQ(driver.completed_count(), 0U);
    EXPECT_EQ(port.volume(), 100U);
    EXPECT_FALSE(driver.begin({"tool-2", "getVolume", "{}"}));
    EXPECT_FALSE(driver.cancel("tool-1"));
    port.value = 40;
    port.ack = port.pending;
    const auto result = driver.take_completion();
    ASSERT_TRUE(result);
    EXPECT_EQ(result->token, "tool-1");
    EXPECT_EQ(result->code, 200);
    EXPECT_EQ(result->data, R"({"volume":40})");
    EXPECT_EQ(driver.completed_count(), 1U);
    ASSERT_TRUE(driver.begin({"tool-2", "getVolume", "{}"}));
    EXPECT_EQ(driver.take_completion()->data, R"({"volume":40})");
}
}  // namespace ur::device
