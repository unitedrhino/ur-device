// 文件说明：固定文件路径本地事实关联回归；确认来自显式用例消息，不冒充真实浏览器执行。
#include "browser_file_lane.hpp"
#include <gtest/gtest.h>
#include "ur/device/output_focus_driver.hpp"

namespace ur::device {
namespace {
// fact 固定本地已配对桥报文，云端session/租约不能替代路径票据。
std::string fact(std::string lane, std::string ticket = "1", bool enabled = false) {
    return "{\"kind\":\"fileLaneGated\",\"lane\":\"" + lane + "\",\"gateTicket\":\"" + ticket +
           "\",\"enabled\":" + (enabled ? "true}" : "false}");
}
}  // namespace

TEST(BrowserFileLane, FirstIdleRequiresDeliveredOriginalPhysicalFact) {
    for (const auto& name : {"audio", "video", "audioVideo"}) {
        BrowserFileLane lane(name);
        EXPECT_FALSE(lane.output_gate_confirmed(1, false));
        EXPECT_FALSE(lane.request_output_gate(true, 10));
        ASSERT_EQ(lane.request_output_gate(false, 11), 1U);
        EXPECT_FALSE(lane.receive(fact(name), 12));
        auto command = lane.take_command();
        ASSERT_TRUE(command);
        EXPECT_NE(command->find("\"lane\":\"" + std::string(name) + "\""), std::string::npos);
        EXPECT_FALSE(lane.take_command());
        EXPECT_FALSE(lane.output_gate_confirmed(1, false));
        ASSERT_TRUE(lane.receive(fact(name), 13));
        EXPECT_TRUE(lane.output_gate_confirmed(1, false));
        EXPECT_FALSE(lane.output_gate_confirmed(1, true));
        EXPECT_TRUE(lane.receive(fact(name), 14));
    }
}

TEST(BrowserFileLane, NewGateCannotReuseOldFactOrRefreshDeadline) {
    BrowserFileLane lane("audioVideo");
    ASSERT_EQ(lane.request_output_gate(false, 10), 1U);
    ASSERT_TRUE(lane.take_command());
    ASSERT_TRUE(lane.receive(fact("audioVideo"), 11));
    ASSERT_EQ(lane.request_output_gate(true, 12), 2U);
    EXPECT_FALSE(lane.request_output_gate(true, 13));
    EXPECT_FALSE(lane.output_gate_confirmed(1, false));
    EXPECT_TRUE(lane.receive(fact("audioVideo"), 14));
    EXPECT_FALSE(lane.output_gate_confirmed(2, true));
    ASSERT_TRUE(lane.take_command());
    EXPECT_FALSE(lane.receive(fact("audioVideo", "2", false), 15));
    ASSERT_TRUE(lane.receive(fact("audioVideo", "2", true), 16));
    EXPECT_TRUE(lane.output_gate_confirmed(2, true));
    ASSERT_EQ(lane.request_output_gate(false, 17), 3U);
    ASSERT_TRUE(lane.take_command());
    EXPECT_TRUE(lane.poll(2016));
    EXPECT_FALSE(lane.poll(2017));
    EXPECT_FALSE(lane.receive(fact("audioVideo", "3"), 2018));
    EXPECT_FALSE(lane.request_output_gate(false, 2019));
    EXPECT_FALSE(lane.output_gate_confirmed(2, true));
}

TEST(BrowserFileLane, RejectsWrongScopeAndUntrustedMalformedReceipts) {
    BrowserFileLane lane("audio");
    ASSERT_TRUE(lane.request_output_gate(false, 10));
    ASSERT_TRUE(lane.take_command());
    for (const auto& body :
         {std::string("null"), std::string("[]"), fact("video"), fact("audio", "0"),
          fact("audio", "01"), fact("audio", "-1"), fact("audio", "2"),
          fact("audio", "18446744073709551616"), fact("audio", "1", true),
          fact("audio", "1\\u0000"),
          std::string("{\"kind\":\"fileLaneGated\",\"lane\":\"audio\","
                      "\"gateTicket\":\"1\",\"enabled\":false,\"extra\":0}"),
          std::string("{\"kind\":\"fileLaneGated\",\"lane\":\"audio\","
                      "\"gateTicket\":\"1\",\"enabled\":false,\"enabled\":false}")}) {
        EXPECT_FALSE(lane.receive(body, 11));
        EXPECT_FALSE(lane.output_gate_confirmed(1, false));
    }
    EXPECT_TRUE(lane.receive(fact("audio"), 12));
    EXPECT_FALSE(lane.poll(11));
    EXPECT_FALSE(lane.output_gate_confirmed(1, false));
}

TEST(BrowserFileLane, CompleteThreeGroupsMustAllConfirmBeforeSharedBaseline) {
    BrowserFileLane audio("audio"), video("video"), av("audioVideo");
    OutputFocusDriver driver({{"", "", {"speaker"}, &audio},
                              {"", "", {"display"}, &video},
                              {"", "", {"speaker", "display"}, &av}},
                             0, FocusBindingMode::DeferredOnce);
    ASSERT_TRUE(driver.initialize(10));
    EXPECT_FALSE(driver.initial_idle_observation());
    ASSERT_TRUE(audio.take_command());
    ASSERT_TRUE(video.take_command());
    ASSERT_TRUE(av.take_command());
    ASSERT_TRUE(audio.receive(fact("audio"), 11));
    ASSERT_TRUE(video.receive(fact("video"), 11));
    ASSERT_TRUE(driver.poll(12));
    EXPECT_FALSE(driver.initial_idle_observation());
    ASSERT_TRUE(av.receive(fact("audioVideo"), 13));
    ASSERT_TRUE(driver.poll(14));
    const auto observed = driver.initial_idle_observation();
    ASSERT_TRUE(observed);
    EXPECT_EQ(observed->resources, (std::vector<std::string>{"speaker", "display"}));
    EXPECT_EQ(observed->revision, 0U);
    EXPECT_TRUE(driver.bind_scope(0, "music", "original-music", 15));
    EXPECT_TRUE(driver.bind_scope(1, "silent-video", "original-video", 16));
    EXPECT_TRUE(driver.bind_scope(2, "movie", "original-av", 17));
}

TEST(BrowserFileLane, InvalidConfigurationAndOverflowDoNotAllocateOutput) {
    for (const auto& name : {"", "speaker", "../audio", "unknown"}) {
        BrowserFileLane lane(name);
        EXPECT_FALSE(lane.request_output_gate(false, 0));
        EXPECT_FALSE(lane.take_command());
        EXPECT_FALSE(lane.receive(fact("audio"), 1));
    }
    BrowserFileLane lane("audio");
    EXPECT_FALSE(lane.request_output_gate(false, UINT64_MAX - 1000));
    EXPECT_FALSE(lane.take_command());
}
}  // namespace ur::device
