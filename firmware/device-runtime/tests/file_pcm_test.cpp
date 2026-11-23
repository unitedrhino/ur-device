// 文件驱动的可控时钟测试：并发采集/播放、帧节奏、真实排空、取消与边界恢复。
#include "file_pcm.hpp"
#include <gtest/gtest.h>

namespace ur::device {
namespace {
TEST(FilePcm, InputIsPacedAndLastPartialFrameIsZeroPadded) {
    FilePcm pcm({16000, 1, 20});
    ASSERT_TRUE(pcm.queue_input({16000, std::vector<std::int16_t>(321, 7)}));
    EXPECT_FALSE(pcm.queue_input({16000, {1}}));
    ASSERT_TRUE(pcm.begin_capture("i1"));
    std::vector<std::int16_t> frame;
    EXPECT_EQ(pcm.read_frame(frame), PcmResult::Empty);
    ASSERT_TRUE(pcm.poll(100));
    EXPECT_EQ(pcm.read_frame(frame), PcmResult::Frame);
    EXPECT_EQ(frame, std::vector<std::int16_t>(320, 7));
    EXPECT_EQ(pcm.read_frame(frame), PcmResult::Empty);
    ASSERT_TRUE(pcm.poll(119));
    EXPECT_EQ(pcm.read_frame(frame), PcmResult::Empty);
    ASSERT_TRUE(pcm.poll(120));
    EXPECT_EQ(pcm.read_frame(frame), PcmResult::Frame);
    ASSERT_EQ(frame.size(), 320U);
    EXPECT_EQ(frame.front(), 7);
    EXPECT_EQ(frame.back(), 0);
    EXPECT_TRUE(pcm.input_complete());
    EXPECT_EQ(pcm.last_input_ms(), 120U);
}
TEST(FilePcm, PlaybackDoesNotStopInputAndOnlyCompletesAfterSampleClock) {
    FilePcm pcm({16000, 1, 20});
    ASSERT_TRUE(pcm.queue_input({16000, std::vector<std::int16_t>(640, 1)}));
    ASSERT_TRUE(pcm.begin_capture("i"));
    ASSERT_TRUE(pcm.begin_playback("r", 1));
    ASSERT_TRUE(pcm.write_frame(std::vector<std::int16_t>(320, 2), 1));
    ASSERT_TRUE(pcm.poll(0));
    EXPECT_FALSE(pcm.playback_empty(1));
    EXPECT_TRUE(pcm.rendered().samples.empty());
    std::vector<std::int16_t> frame;
    EXPECT_EQ(pcm.read_frame(frame), PcmResult::Frame);
    ASSERT_TRUE(pcm.poll(20));
    EXPECT_TRUE(pcm.playback_empty(1));
    EXPECT_EQ(pcm.rendered().samples, std::vector<std::int16_t>(320, 2));
    EXPECT_EQ(pcm.read_frame(frame), PcmResult::Frame);
}
TEST(FilePcm, CancelDropsUnplayedFramesAndRetiredCallbacksCannotStopNewReply) {
    FilePcm pcm({24000, 1, 60});
    ASSERT_TRUE(pcm.begin_playback("r1", 1));
    ASSERT_TRUE(pcm.write_frame(std::vector<std::int16_t>(1440, 2), 1));
    ASSERT_TRUE(pcm.poll(0));
    pcm.end_playback(1);
    ASSERT_TRUE(pcm.poll(60));
    EXPECT_TRUE(pcm.rendered().samples.empty());
    ASSERT_TRUE(pcm.begin_playback("r2", 2));
    ASSERT_TRUE(pcm.write_frame(std::vector<std::int16_t>(1440, 3), 2));
    pcm.end_playback(1);
    ASSERT_TRUE(pcm.poll(60));
    ASSERT_TRUE(pcm.poll(120));
    EXPECT_EQ(pcm.rendered().samples, std::vector<std::int16_t>(1440, 3));
    EXPECT_EQ(pcm.response_id(), "r2");
}
TEST(FilePcm, PauseDiscardsOldCorpusAndBackwardClockFailsClosed) {
    FilePcm pcm({16000, 1, 20});
    EXPECT_FALSE(pcm.queue_input({24000, {1}}));
    EXPECT_FALSE(pcm.queue_input({16000, {}}));
    ASSERT_TRUE(pcm.queue_input({16000, {1}}));
    ASSERT_TRUE(pcm.begin_capture("i1"));
    pcm.end_capture();
    EXPECT_TRUE(pcm.input_complete());
    ASSERT_TRUE(pcm.begin_capture("i2"));
    ASSERT_TRUE(pcm.poll(100));
    std::vector<std::int16_t> frame;
    EXPECT_EQ(pcm.read_frame(frame), PcmResult::Empty);
    EXPECT_FALSE(pcm.poll(99));
    EXPECT_EQ(pcm.read_frame(frame), PcmResult::Failed);
    EXPECT_FALSE(pcm.begin_capture("i3"));
    EXPECT_FALSE(pcm.begin_playback("r3", 3));
    EXPECT_FALSE(pcm.queue_input({16000, {2}}));
    EXPECT_FALSE(pcm.poll(101));
    FilePcm invalid({0, 0, 0});
    EXPECT_FALSE(invalid.poll(1));
}
}  // namespace
}  // namespace ur::device
