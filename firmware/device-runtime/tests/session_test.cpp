// C++ 会话单元测试：可控驱动，无硬件、网络和实际等待。
#include "ur/device/session.hpp"
#include <gtest/gtest.h>
#include <memory>

namespace ur::device {
namespace {
// FakeAudio 记录驱动调用和失败注入，验证控制器没有额外副作用。
struct FakeAudio final : AudioPort {
    bool capture_ok = true;   // 下次启动采集的接受结果。
    bool playback_ok = true;  // 下次启动播放的接受结果。
    int starts = 0;           // 采集调用次数。
    int stops = 0;            // 采集停止次数。
    int plays = 0;            // 播放启动次数。
    int clears = 0;           // 播放清队列次数。
    std::string last_input;   // 驱动实际接收的输入段，恢复不能另造新段。
    bool start_capture(std::string_view input) override {
        ++starts;
        last_input = std::string(input);
        return capture_ok;
    }
    void stop_capture() override { ++stops; }
    bool start_playback(std::string_view, std::uint64_t) override {
        ++plays;
        return playback_ok;
    }
    void stop_playback(std::uint64_t) override { ++clears; }
};
// SessionTest 每个用例独享驱动及控制器；析构顺序保证驱动仍有效。
class SessionTest : public testing::Test {
protected:
    FakeAudio audio;                           // 用例平台替身。
    Session session{audio};                    // 待验证的生产控制器。
    const Capabilities all{true, true, true};  // 声明完整能力，单独测试缺失条件。
    // open 建立指定模式，默认云端为实时路径。
    void open(Mode mode = Mode::FullDuplex, Pipeline pipeline = Pipeline::Realtime) {
        ASSERT_EQ(session.open("s", mode, pipeline, all), Result::Applied);
    }
};

TEST_F(SessionTest, NegotiationRequiresEveryFullDuplexPrerequisite) {
    EXPECT_EQ(session.open("s", Mode::FullDuplex, Pipeline::Realtime, {true, false, true}),
              Result::Unsupported);
    EXPECT_EQ(session.open("s", Mode::FullDuplex, Pipeline::Realtime, {true, true, false}),
              Result::Unsupported);
    EXPECT_EQ(session.open("s", Mode::FullDuplex, Pipeline::Cascade, all), Result::Unsupported);
    EXPECT_EQ(session.open("s", Mode::HalfDuplex, Pipeline::Realtime, {false, true, true}),
              Result::Unsupported);
    EXPECT_FALSE(session.active());
    EXPECT_EQ(audio.starts + audio.plays, 0);
}
TEST_F(SessionTest, InvalidConfigurationDoesNotOpen) {
    EXPECT_EQ(session.open("", Mode::HalfDuplex, Pipeline::Cascade, all), Result::Invalid);
    EXPECT_EQ(session.open(std::string(129, 's'), Mode::HalfDuplex, Pipeline::Cascade, all),
              Result::Invalid);
    EXPECT_EQ(session.open("s", static_cast<Mode>(99), Pipeline::Cascade, all), Result::Invalid);
    EXPECT_EQ(session.open("s", Mode::HalfDuplex, static_cast<Pipeline>(99), all), Result::Invalid);
    open();
    EXPECT_EQ(session.open("other", Mode::HalfDuplex, Pipeline::Cascade, all), Result::Busy);
}
TEST_F(SessionTest, HalfDuplexSupportsBothPipelinesAndManyTurns) {
    for (auto pipeline : {Pipeline::Cascade, Pipeline::Realtime}) {
        open(Mode::HalfDuplex, pipeline);
        for (int turn = 1; turn <= 10; ++turn) {
            const auto input = "i" + std::to_string(turn);
            const auto reply = "r" + std::to_string(turn);
            ASSERT_EQ(session.start_input("s", input), Result::Applied);
            EXPECT_EQ(session.begin_response("s", reply, static_cast<std::uint64_t>(turn)),
                      Result::Busy);
            ASSERT_EQ(session.stop_input("s", input), Result::Applied);
            ASSERT_EQ(session.begin_response("s", reply, static_cast<std::uint64_t>(turn)),
                      Result::Applied);
            EXPECT_EQ(session.start_input("s", "next"), Result::Busy);
            EXPECT_EQ(session.playback_drained(session.playback_epoch()), Result::Busy);
            EXPECT_EQ(session.network_done("s", reply), Result::Applied);
            EXPECT_TRUE(session.playing());
            EXPECT_EQ(session.playback_drained(session.playback_epoch()), Result::Applied);
            EXPECT_FALSE(session.playing());
        }
        session.close();
    }
    EXPECT_EQ(audio.starts, 20);
    EXPECT_EQ(audio.plays, 20);
}
TEST_F(SessionTest, FullDuplexBargeInKeepsCaptureAndDropsOldReply) {
    open();
    ASSERT_EQ(session.start_input("s", "i1"), Result::Applied);
    ASSERT_EQ(session.begin_response("s", "r1", 1), Result::Applied);
    const auto old_epoch = session.playback_epoch();
    EXPECT_TRUE(session.capturing());
    EXPECT_TRUE(session.playing());
    EXPECT_EQ(session.cancel_response("s", "r1"), Result::Applied);
    EXPECT_TRUE(session.capturing());
    EXPECT_EQ(audio.stops, 0);
    EXPECT_EQ(session.begin_response("s", "r1", 1), Result::Duplicate);
    EXPECT_EQ(session.begin_response("s", "r1", 2), Result::Stale);
    ASSERT_EQ(session.begin_response("s", "r2", 3), Result::Applied);
    EXPECT_EQ(session.cancel_response("s", "r1"), Result::Stale);
    EXPECT_EQ(session.network_done("s", "r1"), Result::Stale);
    EXPECT_EQ(session.playback_drained(old_epoch), Result::Stale);
    EXPECT_EQ(session.begin_response("s", "r1", 2), Result::Stale);
    EXPECT_TRUE(session.playing());
}
TEST_F(SessionTest, FullDuplexCanStartInputWhilePlayingAndPauseIndependently) {
    open();
    ASSERT_EQ(session.begin_response("s", "r", 1), Result::Applied);
    ASSERT_EQ(session.start_input("s", "i1"), Result::Applied);
    EXPECT_EQ(session.stop_input("s", "i1"), Result::Applied);
    EXPECT_TRUE(session.playing());
    EXPECT_EQ(session.start_input("s", "i1"), Result::Stale);
    EXPECT_EQ(session.start_input("s", "i2"), Result::Applied);
    EXPECT_EQ(session.stop_input("s", "i1"), Result::Stale);
    EXPECT_TRUE(session.capturing());
}
TEST_F(SessionTest, DuplicateOperationsHaveNoDriverSideEffects) {
    open();
    ASSERT_EQ(session.start_input("s", "i"), Result::Applied);
    EXPECT_EQ(session.start_input("s", "i"), Result::Duplicate);
    EXPECT_EQ(session.start_input("s", "other"), Result::Busy);
    EXPECT_EQ(session.stop_input("s", "i"), Result::Applied);
    EXPECT_EQ(session.stop_input("s", "i"), Result::Duplicate);
    ASSERT_EQ(session.begin_response("s", "r", 1), Result::Applied);
    EXPECT_EQ(session.begin_response("s", "r", 1), Result::Duplicate);
    EXPECT_EQ(session.begin_response("s", "other", 2), Result::Busy);
    EXPECT_EQ(session.network_done("s", "r"), Result::Applied);
    EXPECT_EQ(session.network_done("s", "r"), Result::Duplicate);
    EXPECT_EQ(session.playback_drained(session.playback_epoch()), Result::Applied);
    EXPECT_EQ(session.playback_drained(session.playback_epoch()), Result::Duplicate);
    EXPECT_EQ(session.cancel_response("s", "r"), Result::Duplicate);
    EXPECT_EQ(session.network_done("s", "r"), Result::Duplicate);
    EXPECT_EQ(audio.starts, 1);
    EXPECT_EQ(audio.stops, 1);
    EXPECT_EQ(audio.plays, 1);
    EXPECT_EQ(audio.clears, 1);
}
TEST_F(SessionTest, InvalidAndCrossSessionEventsAreRejected) {
    open();
    EXPECT_EQ(session.start_input("s", ""), Result::Invalid);
    EXPECT_EQ(session.start_input("s", std::string(129, 'i')), Result::Invalid);
    EXPECT_EQ(session.begin_response("s", "", 1), Result::Invalid);
    EXPECT_EQ(session.begin_response("s", std::string(129, 'r'), 1), Result::Invalid);
    EXPECT_EQ(session.begin_response("s", "r", 0), Result::Invalid);
    EXPECT_EQ(session.start_input("old", "i"), Result::Stale);
    EXPECT_EQ(session.stop_input("old", "i"), Result::Stale);
    EXPECT_EQ(session.stop_input("s", ""), Result::Stale);
    EXPECT_EQ(session.begin_response("old", "r", 1), Result::Stale);
    EXPECT_EQ(session.network_done("old", "r"), Result::Stale);
    EXPECT_EQ(session.network_done("s", ""), Result::Stale);
    EXPECT_EQ(session.cancel_response("old", "r"), Result::Stale);
    EXPECT_EQ(session.cancel_response("s", ""), Result::Stale);
    EXPECT_EQ(audio.starts + audio.plays, 0);
}
TEST_F(SessionTest, DriverFailureDoesNotAdvanceStateAndCanRetry) {
    open();
    audio.capture_ok = false;
    EXPECT_EQ(session.start_input("s", "i"), Result::DriverFailed);
    EXPECT_FALSE(session.capturing());
    audio.capture_ok = true;
    EXPECT_EQ(session.start_input("s", "i"), Result::Applied);
    audio.playback_ok = false;
    EXPECT_EQ(session.begin_response("s", "r", 1), Result::DriverFailed);
    EXPECT_FALSE(session.playing());
    EXPECT_EQ(session.playback_epoch(), 0U);
    audio.playback_ok = true;
    EXPECT_EQ(session.begin_response("s", "r", 1), Result::Applied);
}
TEST_F(SessionTest, CloseReleasesOnceAndReopenRejectsOldCallbacks) {
    open();
    ASSERT_EQ(session.start_input("s", "i"), Result::Applied);
    ASSERT_EQ(session.begin_response("s", "r", 1), Result::Applied);
    const auto old_epoch = session.playback_epoch();
    session.close();
    session.close();
    EXPECT_FALSE(session.active());
    EXPECT_EQ(session.playback_drained(old_epoch), Result::Stale);
    EXPECT_EQ(session.start_input("s", "late"), Result::Stale);
    EXPECT_EQ(audio.stops, 1);
    EXPECT_EQ(audio.clears, 1);
    ASSERT_EQ(session.open("new", Mode::FullDuplex, Pipeline::Realtime, all), Result::Applied);
    EXPECT_EQ(session.begin_response("new", "r2", 1), Result::Applied);
    EXPECT_EQ(session.playback_drained(old_epoch), Result::Stale);
    EXPECT_EQ(session.cancel_response("s", "r2"), Result::Stale);
}
TEST(SessionLifetime, DestructorReleasesResources) {
    FakeAudio audio;
    {
        Session session(audio);
        ASSERT_EQ(session.open("s", Mode::FullDuplex, Pipeline::Realtime, {true, true, true}),
                  Result::Applied);
        ASSERT_EQ(session.start_input("s", "i"), Result::Applied);
        ASSERT_EQ(session.begin_response("s", "r", 1), Result::Applied);
    }
    EXPECT_EQ(audio.stops, 1);
    EXPECT_EQ(audio.clears, 1);
}
TEST_F(SessionTest, MediaRecoveryPreservesOriginalInputAndNeverReplaysReply) {
    open();
    ASSERT_EQ(session.start_input("s", "original-input"), Result::Applied);
    ASSERT_EQ(session.begin_response("s", "old-reply", 1), Result::Applied);
    const auto old_epoch = session.playback_epoch();
    ASSERT_EQ(session.suspend_media("s"), Result::Applied);
    EXPECT_TRUE(session.active());
    EXPECT_FALSE(session.capturing());
    EXPECT_FALSE(session.playing());
    EXPECT_EQ(audio.stops, 1);
    EXPECT_EQ(audio.clears, 1);
    EXPECT_EQ(session.suspend_media("s"), Result::Duplicate);
    EXPECT_EQ(session.start_input("s", "other-input"), Result::Busy);
    EXPECT_EQ(session.begin_response("s", "new-reply", 2), Result::Busy);
    ASSERT_EQ(session.resume_media("s"), Result::Applied);
    EXPECT_TRUE(session.capturing());
    EXPECT_FALSE(session.playing());
    EXPECT_EQ(audio.starts, 2);
    EXPECT_EQ(audio.plays, 1);
    EXPECT_EQ(audio.last_input, "original-input");
    EXPECT_EQ(session.start_input("s", "original-input"), Result::Duplicate);
    EXPECT_EQ(session.begin_response("s", "old-reply", 2), Result::Stale);
    EXPECT_EQ(session.network_done("s", "old-reply"), Result::Duplicate);
    EXPECT_EQ(session.playback_drained(old_epoch), Result::Duplicate);
    EXPECT_EQ(session.resume_media("s"), Result::Duplicate);
    EXPECT_EQ(audio.starts, 2);
    ASSERT_EQ(session.begin_response("s", "new-reply", 2), Result::Applied);
    EXPECT_GT(session.playback_epoch(), old_epoch);
    EXPECT_EQ(session.playback_drained(old_epoch), Result::Stale);
}
TEST_F(SessionTest, ExplicitPauseDuringRecoveryCannotBeUndoneByReadyAck) {
    for (const auto mode : {Mode::HalfDuplex, Mode::FullDuplex}) {
        open(mode);
        ASSERT_EQ(session.start_input("s", "i"), Result::Applied);
        ASSERT_EQ(session.suspend_media("s"), Result::Applied);
        EXPECT_EQ(session.stop_input("wrong-session", "i"), Result::Stale);
        EXPECT_EQ(session.stop_input("s", "wrong-input"), Result::Stale);
        ASSERT_EQ(session.stop_input("s", "i"), Result::Applied);
        EXPECT_EQ(session.stop_input("s", "i"), Result::Duplicate);
        const auto before = audio.starts;
        ASSERT_EQ(session.resume_media("s"), Result::Applied);
        EXPECT_FALSE(session.capturing());
        EXPECT_EQ(audio.starts, before);
        EXPECT_EQ(session.start_input("s", "i"), Result::Stale);
        EXPECT_EQ(session.start_input("s", "new-input"), Result::Applied);
        session.close();
    }
}
TEST_F(SessionTest, MediaRecoveryRejectsClosedAndForeignSessionsWithoutDriverCalls) {
    EXPECT_EQ(session.suspend_media("s"), Result::Stale);
    EXPECT_EQ(session.resume_media("s"), Result::Stale);
    open();
    EXPECT_EQ(session.resume_media("s"), Result::Duplicate);
    ASSERT_EQ(session.start_input("s", "i"), Result::Applied);
    EXPECT_EQ(session.suspend_media("other"), Result::Stale);
    EXPECT_TRUE(session.capturing());
    ASSERT_EQ(session.suspend_media("s"), Result::Applied);
    EXPECT_EQ(session.resume_media("other"), Result::Stale);
    session.close();
    EXPECT_EQ(session.resume_media("s"), Result::Stale);
    EXPECT_EQ(audio.starts, 1);
    EXPECT_EQ(audio.stops, 1);
    ASSERT_EQ(session.open("new", Mode::FullDuplex, Pipeline::Realtime, all), Result::Applied);
    EXPECT_EQ(session.resume_media("new"), Result::Duplicate);
    EXPECT_FALSE(session.capturing());
}
TEST_F(SessionTest, RecoveryCaptureFailureStaysSuspendedAndCloseDoesNotRestart) {
    open();
    ASSERT_EQ(session.start_input("s", "i"), Result::Applied);
    ASSERT_EQ(session.suspend_media("s"), Result::Applied);
    audio.capture_ok = false;
    EXPECT_EQ(session.resume_media("s"), Result::DriverFailed);
    EXPECT_FALSE(session.capturing());
    EXPECT_EQ(session.start_input("s", "new"), Result::Busy);
    EXPECT_EQ(session.suspend_media("s"), Result::Duplicate);
    EXPECT_EQ(audio.stops, 1);
    session.close();
    EXPECT_EQ(session.resume_media("s"), Result::Stale);
    EXPECT_EQ(audio.starts, 2);
}
TEST_F(SessionTest, PausedInputAndHalfDuplexPlaybackStayPausedAcrossRecovery) {
    open(Mode::HalfDuplex);
    ASSERT_EQ(session.start_input("s", "i"), Result::Applied);
    ASSERT_EQ(session.stop_input("s", "i"), Result::Applied);
    ASSERT_EQ(session.begin_response("s", "r", 1), Result::Applied);
    ASSERT_EQ(session.suspend_media("s"), Result::Applied);
    ASSERT_EQ(session.resume_media("s"), Result::Applied);
    EXPECT_FALSE(session.capturing());
    EXPECT_FALSE(session.playing());
    EXPECT_EQ(audio.starts, 1);
    EXPECT_EQ(audio.stops, 1);
    EXPECT_EQ(audio.plays, 1);
    EXPECT_EQ(audio.clears, 1);
    EXPECT_EQ(session.begin_response("s", "r", 2), Result::Stale);
}
}  // namespace
}  // namespace ur::device
