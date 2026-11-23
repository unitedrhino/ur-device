// 文件说明：共享人际驱动预约与原实际Stopped票据测试；不替代浏览器真实采集。
#include <gtest/gtest.h>
#include "browser_call_media.hpp"

namespace ur::device {
// 原租约失联不能触发重建或重开静音；精确五秒之后即使JS定时器尚未执行也拒绝恢复。
TEST(BrowserCallSharedTest, NaturalWindowKeepsLeaseAndRejectsLateOrInitialCallbacks) {
    CaptureGate gate(3);
    BrowserCallMedia media(&gate);
    ASSERT_TRUE(media.bind_session("source"));
    ASSERT_TRUE(media.begin());
    ASSERT_TRUE(media.take_command());
    ASSERT_TRUE(media.apply_answer("v=0\r\nanswer"));
    ASSERT_TRUE(media.take_command());
    EXPECT_EQ(media.report_connectivity("source", "1", false, 0), Result::Invalid);
    ASSERT_TRUE(media.report_connected());
    EXPECT_EQ(media.report_connectivity("old", "1", false, 0), Result::Stale);
    ASSERT_EQ(media.report_connectivity("source", "1", false, 0), Result::Applied);
    EXPECT_FALSE(media.connected());
    EXPECT_FALSE(media.report_connected());
    EXPECT_FALSE(gate.available(true, true, false));
    EXPECT_EQ(media.report_connectivity("source", "1", false, 4000), Result::Duplicate);
    EXPECT_FALSE(media.connectivity_expired(4999));
    EXPECT_EQ(media.report_connectivity("source", "1", true, 4999), Result::Applied);
    EXPECT_TRUE(media.connected());
    EXPECT_FALSE(media.connectivity_expired(5000));
    ASSERT_EQ(media.report_connectivity("source", "1", false, 6000), Result::Applied);
    EXPECT_EQ(media.report_connectivity("source", "1", true, 11000), Result::DriverFailed);
    EXPECT_FALSE(media.report_connected());
    EXPECT_TRUE(media.connectivity_expired(11000));
    EXPECT_FALSE(media.connectivity_expired(11001));
    media.close();
    EXPECT_EQ(media.report_connectivity("source", "1", true, 11002), Result::Stale);
    EXPECT_TRUE(media.report_stopped("source", "1"));
}
// 本端单调时钟倒退按失效处理，轮询截止不依赖页面回执且只通知一次。
TEST(BrowserCallSharedTest, NaturalWindowFailsClosedOnClockRollbackAndDeadline) {
    for (const auto now : {99ULL, 5100ULL}) {
        CaptureGate gate(3);
        BrowserCallMedia media(&gate);
        ASSERT_TRUE(media.bind_session("source"));
        ASSERT_TRUE(media.begin());
        ASSERT_TRUE(media.take_command());
        ASSERT_TRUE(media.apply_answer("v=0\r\nanswer"));
        ASSERT_TRUE(media.take_command());
        ASSERT_TRUE(media.report_connected());
        ASSERT_EQ(media.report_connectivity("source", "1", false, 100), Result::Applied);
        EXPECT_TRUE(media.connectivity_expired(now));
        EXPECT_FALSE(media.connectivity_expired(now));
        EXPECT_EQ(media.report_connectivity("source", "1", true, 101), Result::DriverFailed);
    }
}
// 接听前预约阻止另一条麦克风业务，未开始驱动时取消可立即退还预约。
TEST(BrowserCallSharedTest, ReservationBeforeAckAndUnstartedCancel) {
    CaptureGate gate(3);
    BrowserCallMedia media(&gate, "entry");
    EXPECT_FALSE(media.prepare());
    ASSERT_TRUE(media.bind_session("source-leg"));
    ASSERT_TRUE(media.prepare());
    EXPECT_FALSE(gate.acquire(true, true, false));
    EXPECT_FALSE(media.bind_session("other-leg"));
    EXPECT_FALSE(media.take_command());
    media.close();
    EXPECT_TRUE(gate.available(true, true, true));
    EXPECT_TRUE(media.bind_session("other-leg"));
}
// close只请求停止，实际回执之前busy；原租约迟到不能释放下一条腿。
TEST(BrowserCallSharedTest, ActualStopFencesNextSessionAndLateReceipt) {
    CaptureGate gate(3);
    BrowserCallMedia media(&gate, "entry");
    ASSERT_TRUE(media.bind_session("source-leg"));
    ASSERT_TRUE(media.begin());
    const auto configure = media.take_command();
    ASSERT_TRUE(configure);
    EXPECT_NE(configure->find("\"cameraId\":\"entry\""), std::string::npos);
    EXPECT_TRUE(media.matches("source-leg", "1"));
    media.close();
    ASSERT_TRUE(media.take_command());
    EXPECT_FALSE(media.available());
    EXPECT_FALSE(media.begin());
    EXPECT_FALSE(media.report_stopped("other", "1"));
    EXPECT_FALSE(media.report_stopped("source-leg", "2"));
    EXPECT_FALSE(gate.available(true, true, false));
    ASSERT_TRUE(media.report_stopped("source-leg", "1"));
    ASSERT_TRUE(media.bind_session("next-leg"));
    ASSERT_TRUE(media.begin());
    EXPECT_FALSE(media.report_stopped("source-leg", "1"));
    EXPECT_TRUE(media.matches("next-leg", "2"));
    media.close();
    EXPECT_TRUE(media.report_stopped("next-leg", "2"));
}
// 未知本地状态及非法原腿不能编码出可执行浏览器命令。
TEST(BrowserCallSharedTest, StrictBindingAndStateNotice) {
    CaptureGate gate(1);
    BrowserCallMedia media(&gate);
    EXPECT_FALSE(media.bind_session(""));
    EXPECT_FALSE(media.bind_session("bad\nidentity"));
    EXPECT_TRUE(browser_call_state("unknown", "session").empty());
    EXPECT_TRUE(browser_call_state("ringing", "bad\n").empty());
    EXPECT_FALSE(browser_call_state("available", "").empty());
}
}  // namespace ur::device
