// 文件说明：实时节目末端票据回归；模拟浏览器事实，不称真实扬声器或可见输出验收。
#include "browser_live_lane.hpp"
#include <gtest/gtest.h>

namespace ur::device {
namespace {
// fact 保留原本地组和票据；错误字段仍须生产桥解析拒绝。
std::string fact(std::string ticket, bool enabled, std::string lane = "audioVideo") {
    return "{\"kind\":\"liveLaneGated\",\"lane\":\"" + lane + "\",\"gateTicket\":\"" + ticket +
           "\",\"enabled\":" + (enabled ? "true}" : "false}");
}
}  // namespace
TEST(BrowserLiveLaneTest, InitialActualCloseRequiredBeforePermission) {
    BrowserLiveLane lane("audioVideo");
    EXPECT_FALSE(lane.permitted());
    EXPECT_FALSE(lane.request_output_gate(true, 1));
    ASSERT_EQ(lane.request_output_gate(false, 2), 1U);
    EXPECT_FALSE(lane.receive(fact("1", false), 3));
    ASSERT_TRUE(lane.take_command());
    ASSERT_TRUE(lane.receive(fact("1", false), 4));
    EXPECT_TRUE(lane.output_gate_confirmed(1, false));
    EXPECT_FALSE(lane.permitted());
    ASSERT_EQ(lane.request_output_gate(true, 5), 2U);
    EXPECT_FALSE(lane.permitted());
    ASSERT_TRUE(lane.take_command());
    ASSERT_TRUE(lane.receive(fact("2", true), 6));
    EXPECT_TRUE(lane.permitted());
}
TEST(BrowserLiveLaneTest, SuspendAndRestoreAreMaskOnlyAndEachNeedsOriginalActual) {
    BrowserLiveLane lane("audioVideo");
    ASSERT_TRUE(lane.request_output_gate(false, 1));
    ASSERT_TRUE(lane.take_command());
    ASSERT_TRUE(lane.receive(fact("1", false), 2));
    ASSERT_TRUE(lane.request_output_gate(true, 3));
    ASSERT_TRUE(lane.take_command());
    ASSERT_TRUE(lane.receive(fact("2", true), 4));
    ASSERT_TRUE(lane.request_output_gate(false, 5));
    EXPECT_FALSE(lane.permitted());
    ASSERT_TRUE(lane.take_command());
    EXPECT_TRUE(lane.receive(fact("2", true), 6));
    EXPECT_FALSE(lane.permitted());
    ASSERT_TRUE(lane.receive(fact("3", false), 7));
    ASSERT_TRUE(lane.request_output_gate(true, 8));
    ASSERT_TRUE(lane.take_command());
    EXPECT_FALSE(lane.receive(fact("4", false), 9));
    EXPECT_FALSE(lane.permitted());
    ASSERT_TRUE(lane.receive(fact("4", true), 10));
    EXPECT_TRUE(lane.permitted());
}
TEST(BrowserLiveLaneTest, RejectsUnknownDuplicateMalformedAndOtherMediaFacts) {
    BrowserLiveLane lane("audioVideo");
    ASSERT_TRUE(lane.request_output_gate(false, 1));
    ASSERT_TRUE(lane.take_command());
    EXPECT_FALSE(lane.receive(fact("2", false), 2));
    EXPECT_FALSE(lane.receive(fact("01", false), 3));
    EXPECT_FALSE(lane.receive(fact("18446744073709551616", false), 4));
    EXPECT_FALSE(lane.receive(fact("1", false, "audio"), 5));
    EXPECT_FALSE(lane.receive(
        R"({"kind":"fileLaneGated","lane":"audioVideo","gateTicket":"1","enabled":false})", 6));
    EXPECT_FALSE(lane.receive(
        R"({"kind":"liveLaneGated","lane":"audioVideo","gateTicket":"1","enabled":false,"enabled":true})",
        7));
    EXPECT_FALSE(lane.receive(
        R"({"kind":"liveLaneGated","lane":"audioVideo","gateTicket":"1","enabled":null})", 8));
    ASSERT_TRUE(lane.receive(fact("1", false), 9));
}
TEST(BrowserLiveLaneTest, FullBackpressureCannotReplaceOriginalTicket) {
    BrowserLiveLane lane("audioVideo");
    ASSERT_EQ(lane.request_output_gate(false, 1), 1U);
    EXPECT_FALSE(lane.request_output_gate(false, 2));
    EXPECT_FALSE(lane.request_output_gate(true, 3));
    const auto body = lane.take_command();
    ASSERT_TRUE(body);
    EXPECT_NE(body->find("\"gateTicket\":\"1\""), std::string::npos);
    EXPECT_FALSE(lane.request_output_gate(false, 4));
}
TEST(BrowserLiveLaneTest, TimeoutAndRollbackNeverRebaselineUnknownOutput) {
    BrowserLiveLane timeout("audioVideo");
    ASSERT_TRUE(timeout.request_output_gate(false, 1));
    ASSERT_TRUE(timeout.take_command());
    EXPECT_FALSE(timeout.poll(2001));
    EXPECT_FALSE(timeout.receive(fact("1", false), 2002));
    EXPECT_FALSE(timeout.request_output_gate(false, 2003));
    BrowserLiveLane rollback("audioVideo");
    ASSERT_TRUE(rollback.request_output_gate(false, 10));
    ASSERT_TRUE(rollback.take_command());
    EXPECT_FALSE(rollback.poll(9));
    EXPECT_FALSE(rollback.permitted());
}
TEST(BrowserLiveLaneTest, PureAudioVideoAndInvalidGroupsAreExplicit) {
    for (const auto& name : {"audio", "video"}) {
        BrowserLiveLane lane(name);
        ASSERT_TRUE(lane.request_output_gate(false, 1));
        ASSERT_TRUE(lane.take_command());
        ASSERT_TRUE(lane.receive(fact("1", false, name), 2));
        EXPECT_TRUE(lane.output_gate_confirmed(1, false));
    }
    BrowserLiveLane invalid("speaker");
    EXPECT_FALSE(invalid.poll(1));
    EXPECT_FALSE(invalid.request_output_gate(false, 2));
}
}  // namespace ur::device
