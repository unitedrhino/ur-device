// 驱动事件交接契约：取帧不是播完，旧输入/输出回调不能污染新代次。
#include "ur/device/buffered_pcm.hpp"
#include <gtest/gtest.h>
#include <utility>

namespace ur::device {
// 原容量第33帧实际false，诊断必须反映该调用而不是以take冒充played。
TEST(BufferedPcm, FailureDiagnosticsObserveOriginalCapacityRejection) {
    BufferedPcm pcm({16000, 1, 20});
    const std::vector<std::int16_t> frame(320, 1000);
    pcm.enable_failure_diagnostics();
    ASSERT_TRUE(pcm.begin_playback("r", 1));
    for (unsigned i = 0; i < 32; ++i)
        ASSERT_TRUE(pcm.write_frame(frame, 1));
    EXPECT_FALSE(pcm.write_frame(frame, 1));
    EXPECT_EQ(pcm.last_write_failure(), PcmWriteFailure::BufferFull);
}
// 通话可收紧未完成帧上限，普通实例仍保持默认32帧；上限只能在代次开始前设置。
TEST(BufferedPcm, PlaybackPendingLimitCanBeReducedBeforePlayback) {
    BufferedPcm pcm({16000, 1, 20});
    const std::vector<std::int16_t> frame(320, 1000);
    EXPECT_TRUE(pcm.set_playback_pending_limit(16));
    EXPECT_FALSE(pcm.set_playback_pending_limit(17));
    ASSERT_TRUE(pcm.begin_playback("call", 1));
    for (unsigned i = 0; i < 16; ++i)
        ASSERT_TRUE(pcm.write_frame(frame, 1));
    EXPECT_TRUE(pcm.playback_backpressured(1));
    EXPECT_FALSE(pcm.write_frame(frame, 1));
    EXPECT_FALSE(pcm.set_playback_pending_limit(8));
}
// 不追溯开启前拒绝；成功write清旧失败，而纯getter与end不消费实际容量。
TEST(BufferedPcm, FailureDiagnosticsAreCurrentWriteOnlyAndDefaultOff) {
    BufferedPcm pcm({16000, 1, 20});                   // 同原固定帧格式。
    const std::vector<std::int16_t> frame(320, 1000);  // 一帧有效非静音PCM。
    EXPECT_FALSE(pcm.write_frame(frame, 1));
    EXPECT_EQ(pcm.last_write_failure(), PcmWriteFailure::None);
    pcm.enable_failure_diagnostics();
    EXPECT_EQ(pcm.last_write_failure(), PcmWriteFailure::None);
    EXPECT_FALSE(pcm.write_frame(frame, 1));
    EXPECT_EQ(pcm.last_write_failure(), PcmWriteFailure::PlaybackInactive);
    ASSERT_TRUE(pcm.begin_playback("r", 1));
    EXPECT_FALSE(pcm.write_frame(frame, 2));
    EXPECT_EQ(pcm.last_write_failure(), PcmWriteFailure::EpochMismatch);
    EXPECT_FALSE(pcm.write_frame({1}, 1));
    EXPECT_EQ(pcm.last_write_failure(), PcmWriteFailure::FrameSize);
    ASSERT_TRUE(pcm.write_frame(frame, 1));
    EXPECT_EQ(pcm.last_write_failure(), PcmWriteFailure::None);
    for (unsigned i = 1; i < 32; ++i)
        ASSERT_TRUE(pcm.write_frame(frame, 1));
    EXPECT_FALSE(pcm.write_frame(frame, 1));
    EXPECT_EQ(pcm.last_write_failure(), PcmWriteFailure::BufferFull);
    pcm.enable_failure_diagnostics();
    EXPECT_EQ(pcm.last_write_failure(), PcmWriteFailure::BufferFull);
    ASSERT_TRUE(pcm.take_playback());
    EXPECT_FALSE(pcm.write_frame(frame, 1));  // take不是played，不释放32容量。
    ASSERT_TRUE(pcm.played(1, 1));
    ASSERT_TRUE(pcm.write_frame(frame, 1));
    EXPECT_EQ(pcm.last_write_failure(), PcmWriteFailure::None);
    EXPECT_FALSE(pcm.write_frame(frame, 2));
    pcm.end_playback(1);
    EXPECT_EQ(pcm.last_write_failure(), PcmWriteFailure::EpochMismatch);
}
// 映射仅固定闭集，DriverClosed留给平台驱动，不借未知整数反射正文。
TEST(BufferedPcm, FailureDiagnosticNamesAreClosed) {
    const std::array<std::string_view, 9> names{
        // 对应原枚举定义顺序。
        "none",       "driverClosed",    "playbackInactive", "epochMismatch", "frameSize",
        "bufferFull", "ticketExhausted", "queueRejected",    "unknown"};
    for (std::size_t i = 0; i < names.size(); ++i)
        EXPECT_EQ(pcm_write_failure_name(static_cast<PcmWriteFailure>(i)), names[i]);
    EXPECT_TRUE(pcm_write_failure_name(static_cast<PcmWriteFailure>(-1)).empty());
}
TEST(BufferedPcm, CaptureIsBoundedAndOldCallbacksCannotResumeInput) {
    BufferedPcm pcm({16000, 1, 20});
    const std::vector<std::int16_t> frame(320, 1000);
    EXPECT_TRUE(pcm.valid());
    EXPECT_EQ(pcm.frame_samples(), 320U);
    EXPECT_FALSE(pcm.captured("old", frame));
    EXPECT_FALSE(pcm.begin_capture(""));
    EXPECT_FALSE(pcm.begin_capture(std::string(129, 'x')));
    ASSERT_TRUE(pcm.begin_capture("i1"));
    EXPECT_FALSE(pcm.begin_capture("i2"));
    EXPECT_FALSE(pcm.captured("i1", {1}));
    for (int i = 0; i < 16; ++i)
        ASSERT_TRUE(pcm.captured("i1", frame));
    EXPECT_FALSE(pcm.captured("i1", frame));
    std::vector<std::int16_t> read;
    EXPECT_EQ(pcm.read_frame(read), PcmResult::Frame);
    EXPECT_EQ(read, frame);
    pcm.end_capture();
    pcm.end_capture();
    EXPECT_EQ(pcm.read_frame(read), PcmResult::Empty);
    EXPECT_TRUE(read.empty());
    ASSERT_TRUE(pcm.begin_capture("i2"));
    EXPECT_FALSE(pcm.captured("i1", frame));
    EXPECT_TRUE(pcm.captured("i2", frame));
}
// 验证身份、帧长与满队列拒绝不消费样本，队列恢复后仍按顺序交付。
TEST(BufferedPcm, OwnedCapturePreservesRejectedFramesAndTransfersAcceptedSamples) {
    BufferedPcm pcm({16000, 1, 20});
    std::vector<std::int16_t> frame(320, 1234);
    ASSERT_TRUE(pcm.begin_capture("current"));
    EXPECT_FALSE(pcm.captured_owned("old", std::move(frame)));
    EXPECT_EQ(frame, std::vector<std::int16_t>(320, 1234));
    std::vector<std::int16_t> short_frame(1, 7);
    EXPECT_FALSE(pcm.captured_owned("current", std::move(short_frame)));
    EXPECT_EQ(short_frame, std::vector<std::int16_t>({7}));
    for (int i = 0; i < 16; ++i) {
        std::vector<std::int16_t> queued(320, static_cast<std::int16_t>(i));
        ASSERT_TRUE(pcm.captured_owned("current", std::move(queued)));
    }
    EXPECT_FALSE(pcm.captured_owned("current", std::move(frame)));
    EXPECT_EQ(frame, std::vector<std::int16_t>(320, 1234));
    std::vector<std::int16_t> read;
    ASSERT_EQ(pcm.read_frame(read), PcmResult::Frame);
    ASSERT_TRUE(pcm.captured_owned("current", std::move(frame)));
    for (int i = 1; i < 16; ++i) {
        ASSERT_EQ(pcm.read_frame(read), PcmResult::Frame);
        EXPECT_EQ(read, std::vector<std::int16_t>(320, static_cast<std::int16_t>(i)));
    }
    ASSERT_EQ(pcm.read_frame(read), PcmResult::Frame);
    EXPECT_EQ(read, std::vector<std::int16_t>(320, 1234));
}
TEST(BufferedPcm, TakingFramesDoesNotReleasePlaybackCapacityOrMeanPlayed) {
    BufferedPcm pcm({16000, 1, 20});
    const std::vector<std::int16_t> frame(320, 5000);
    EXPECT_FALSE(pcm.write_frame(frame, 1));
    EXPECT_FALSE(pcm.take_playback());
    EXPECT_FALSE(pcm.begin_playback("", 1));
    EXPECT_FALSE(pcm.begin_playback("r", 0));
    ASSERT_TRUE(pcm.begin_playback("r", 1));
    EXPECT_FALSE(pcm.begin_playback("r2", 2));
    EXPECT_FALSE(pcm.write_frame({1}, 1));
    EXPECT_FALSE(pcm.write_frame(frame, 2));
    for (int i = 0; i < 32; ++i)
        ASSERT_TRUE(pcm.write_frame(frame, 1));
    EXPECT_FALSE(pcm.write_frame(frame, 1));
    EXPECT_FALSE(pcm.played(1, 1));
    for (unsigned i = 1; i <= 32; ++i) {
        auto next = pcm.take_playback();
        ASSERT_TRUE(next);
        EXPECT_EQ(next->ticket, i);
        EXPECT_EQ(next->epoch, 1U);
        EXPECT_EQ(next->samples, frame);
    }
    EXPECT_FALSE(pcm.take_playback());
    EXPECT_FALSE(pcm.write_frame(frame, 1));
    EXPECT_FALSE(pcm.playback_empty(1));
    EXPECT_FALSE(pcm.played(1, 2));
    EXPECT_FALSE(pcm.played(1, 0));
    EXPECT_FALSE(pcm.played(2, 1));
    EXPECT_FALSE(pcm.played(1, 33));
    ASSERT_TRUE(pcm.played(1, 1));
    EXPECT_TRUE(pcm.played(1, 1));
    EXPECT_TRUE(pcm.write_frame(frame, 1));
    for (unsigned i = 2; i <= 32; ++i)
        EXPECT_TRUE(pcm.played(1, i));
    EXPECT_FALSE(pcm.playback_empty(1));
    ASSERT_TRUE(pcm.take_playback());
    EXPECT_TRUE(pcm.played(1, 33));
    EXPECT_TRUE(pcm.playback_empty(1));
    EXPECT_FALSE(pcm.playback_empty(2));
}
TEST(BufferedPcm, CancelIsScopedAndInvalidFormatNeverActivatesDrivers) {
    BufferedPcm pcm({24000, 1, 60});
    const std::vector<std::int16_t> frame(1440, 1000);
    ASSERT_TRUE(pcm.begin_capture("i"));
    ASSERT_TRUE(pcm.captured("i", frame));
    ASSERT_TRUE(pcm.begin_playback("r", 1));
    ASSERT_TRUE(pcm.write_frame(frame, 1));
    pcm.end_playback(2);
    EXPECT_FALSE(pcm.playback_empty(1));
    pcm.end_playback(1);
    pcm.end_playback(1);
    EXPECT_FALSE(pcm.take_playback());
    EXPECT_FALSE(pcm.played(1, 1));
    EXPECT_FALSE(pcm.begin_playback("r", 1));
    ASSERT_TRUE(pcm.begin_playback("new", 2));
    EXPECT_FALSE(pcm.played(1, 1));
    EXPECT_TRUE(pcm.playback_empty(2));
    std::vector<std::int16_t> input;
    EXPECT_EQ(pcm.read_frame(input), PcmResult::Frame);
    for (const AudioFormat bad :
         {AudioFormat{44100, 1, 20}, AudioFormat{16000, 2, 20}, AudioFormat{16000, 1, 10}}) {
        BufferedPcm invalid(bad);
        EXPECT_FALSE(invalid.valid());
        EXPECT_FALSE(invalid.begin_capture("i"));
        EXPECT_FALSE(invalid.begin_playback("r", 1));
    }
}
}  // namespace ur::device
