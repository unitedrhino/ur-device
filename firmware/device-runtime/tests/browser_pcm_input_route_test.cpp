// 文件说明：验证原AI输入路由围栏真实清理PCM积压，迟到帧和他腿不能重新取得麦克风。
#include <gtest/gtest.h>
#include "browser_pcm.hpp"
#include "browser_wire.hpp"

namespace ur::device {
// BrowserPcmInputRoute
// 使用正式PCM队列与显式原scope，不以假采集完成替代输入围栏。
class BrowserPcmInputRoute : public ::testing::Test {
protected:
    BrowserPcm pcm{{16000, 1, 20}};  // 固定主要输入格式，与现有UDP媒体泵一致。
    void SetUp() override { ASSERT_TRUE(pcm.ready({16000, 1, 20}, true)); }
};
TEST_F(BrowserPcmInputRoute, BoundRouteStartsClosedAndCannotBeRebound) {
    ASSERT_TRUE(pcm.bind_capture_route("ai", 10));
    EXPECT_FALSE(pcm.begin_capture("input"));
    EXPECT_FALSE(pcm.bind_capture_route("human", 11));
    EXPECT_FALSE(pcm.enable_capture_route("human", 10));
    EXPECT_FALSE(pcm.enable_capture_route("ai", 11));
    ASSERT_TRUE(pcm.enable_capture_route("ai", 10));
    EXPECT_TRUE(pcm.begin_capture("input"));
}
TEST_F(BrowserPcmInputRoute, FenceActuallyClearsQueuedFramesAndBlocksLateCapture) {
    ASSERT_TRUE(pcm.bind_capture_route("ai", 10));
    ASSERT_TRUE(pcm.enable_capture_route("ai", 10));
    ASSERT_TRUE(pcm.begin_capture("input"));
    const std::vector<std::int16_t> samples(320, 1000);
    for (unsigned i = 0; i < 16; ++i)
        ASSERT_TRUE(pcm.captured_route("ai", 10, "input", samples));
    ASSERT_TRUE(pcm.fence_capture_route("ai", 10));
    EXPECT_FALSE(pcm.captured_route("ai", 10, "input", samples));
    EXPECT_FALSE(pcm.captured("input", samples));
    std::vector<std::int16_t> output{1};
    EXPECT_EQ(pcm.read_frame(output), PcmResult::Empty);
    EXPECT_TRUE(output.empty());
    ASSERT_TRUE(pcm.enable_capture_route("ai", 10));
    EXPECT_EQ(pcm.read_frame(output), PcmResult::Empty);
    ASSERT_TRUE(pcm.captured_route("ai", 10, "input", samples));
    EXPECT_EQ(pcm.read_frame(output), PcmResult::Frame);
    EXPECT_EQ(output, samples);
}
TEST_F(BrowserPcmInputRoute, WrongLegCannotFenceCurrentRouteOrInjectAudio) {
    ASSERT_TRUE(pcm.bind_capture_route("ai", 10));
    ASSERT_TRUE(pcm.enable_capture_route("ai", 10));
    ASSERT_TRUE(pcm.begin_capture("input"));
    const std::vector<std::int16_t> samples(320, 1000);
    ASSERT_TRUE(pcm.captured_route("ai", 10, "input", samples));
    EXPECT_FALSE(pcm.fence_capture_route("human", 10));
    EXPECT_FALSE(pcm.fence_capture_route("ai", 11));
    EXPECT_FALSE(pcm.captured_route("human", 10, "input", samples));
    EXPECT_FALSE(pcm.captured_route("ai", 11, "input", samples));
    std::vector<std::int16_t> output;
    EXPECT_EQ(pcm.read_frame(output), PcmResult::Frame);
    EXPECT_EQ(output, samples);
}
TEST_F(BrowserPcmInputRoute, FenceRemovesQueuedCaptureCommandsButPreservesPlayback) {
    ASSERT_TRUE(pcm.bind_capture_route("ai", 10));
    ASSERT_TRUE(pcm.enable_capture_route("ai", 10));
    ASSERT_TRUE(pcm.begin_capture("input"));
    ASSERT_TRUE(pcm.begin_playback("reply", 1));
    ASSERT_TRUE(pcm.fence_capture_route("ai", 10));
    const auto command = pcm.take_command();
    ASSERT_TRUE(command);
    EXPECT_EQ(command->kind, BrowserAudioCommandKind::PlaybackStart);
    EXPECT_FALSE(pcm.take_command());
    ASSERT_TRUE(pcm.write_frame(std::vector<std::int16_t>(320, 1000), 1));
    EXPECT_TRUE(pcm.take_playback());
}
TEST_F(BrowserPcmInputRoute, RestoreUsesOriginalCaptureAndCancelledInputCannotReturn) {
    ASSERT_TRUE(pcm.bind_capture_route("ai", 10));
    ASSERT_TRUE(pcm.enable_capture_route("ai", 10));
    ASSERT_TRUE(pcm.begin_capture("input"));
    ASSERT_TRUE(pcm.fence_capture_route("ai", 10));
    ASSERT_TRUE(pcm.enable_capture_route("ai", 10));
    auto command = pcm.take_command();
    ASSERT_TRUE(command);
    EXPECT_EQ(command->kind, BrowserAudioCommandKind::CaptureStart);
    EXPECT_EQ(command->id, "input");
    ASSERT_TRUE(pcm.fence_capture_route("ai", 10));
    pcm.end_capture();
    ASSERT_TRUE(pcm.enable_capture_route("ai", 10));
    while ((command = pcm.take_command()))
        EXPECT_NE(command->kind, BrowserAudioCommandKind::CaptureStart);
    EXPECT_FALSE(pcm.captured_route("ai", 10, "input", std::vector<std::int16_t>(320, 1000)));
    EXPECT_TRUE(pcm.begin_capture("next"));
}
TEST_F(BrowserPcmInputRoute, InvalidBindingAndActiveLegacyCaptureCannotBeConverted) {
    EXPECT_FALSE(pcm.bind_capture_route("", 10));
    EXPECT_FALSE(pcm.bind_capture_route("a\n", 10));
    EXPECT_FALSE(pcm.bind_capture_route(std::string(65, 'a'), 10));
    EXPECT_FALSE(pcm.bind_capture_route("ai", 0));
    EXPECT_FALSE(pcm.bind_capture_route("ai", 9007199254740992ULL));
    ASSERT_TRUE(pcm.begin_capture("legacy"));
    EXPECT_FALSE(pcm.bind_capture_route("ai", 10));
    EXPECT_TRUE(pcm.captured("legacy", std::vector<std::int16_t>(320, 1000)));
    pcm.end_capture();
    EXPECT_FALSE(pcm.bind_capture_route("ai", 10));
}
TEST_F(BrowserPcmInputRoute, DisconnectCannotRestoreRouteOrForwardAudio) {
    ASSERT_TRUE(pcm.bind_capture_route("ai", 10));
    ASSERT_TRUE(pcm.enable_capture_route("ai", 10));
    ASSERT_TRUE(pcm.begin_capture("input"));
    pcm.disconnect();
    EXPECT_FALSE(pcm.enable_capture_route("ai", 10));
    EXPECT_FALSE(pcm.fence_capture_route("ai", 10));
    EXPECT_FALSE(pcm.captured_route("ai", 10, "input", std::vector<std::int16_t>(320, 1000)));
    std::vector<std::int16_t> output{1};
    EXPECT_EQ(pcm.read_frame(output), PcmResult::Failed);
    EXPECT_TRUE(output.empty());
}
// ScopedCaptureWire
// 验证原路由必须在命令产生时绑定，不给迟到采集帧贴当前会话身份。
TEST(ScopedCaptureWire, OriginalCaptureCommandRetainsScopeAcrossFence) {
    BrowserPcm pcm({16000, 1, 20});
    ASSERT_TRUE(pcm.ready({16000, 1, 20}, true));
    ASSERT_TRUE(pcm.bind_capture_route("ai", 10));
    ASSERT_TRUE(pcm.enable_capture_route("ai", 10));
    ASSERT_TRUE(pcm.begin_capture("input"));
    const auto command = pcm.take_command();
    ASSERT_TRUE(command);
    const auto body = browser_command(*command);
    EXPECT_NE(body.find(R"("sessionId":"ai")"), std::string::npos);
    EXPECT_NE(body.find(R"("routeGeneration":"10")"), std::string::npos);
    ASSERT_TRUE(pcm.fence_capture_route("ai", 10));
    EXPECT_EQ(browser_command(*command), body);
}
TEST(ScopedCaptureWire, ScopedFrameUsesStrictFieldsAndNeverInfersMissingIdentity) {
    BrowserRequest request;
    ASSERT_TRUE(decode_browser_request(
        R"({"kind":"captured","id":"input","samples":[1],"sessionId":"ai","routeGeneration":"10"})",
        request));
    for (
        const auto* body :
        {R"({"kind":"captured","id":"input","samples":[1],"sessionId":"ai"})",
         R"({"kind":"captured","id":"input","samples":[1],"routeGeneration":"10"})",
         R"({"kind":"captured","id":"input","samples":[1],"sessionId":"ai","routeGeneration":10})",
         R"({"kind":"captured","id":"input","samples":[1],"sessionId":"ai","routeGeneration":"01"})",
         R"({"kind":"captured","id":"input","samples":[1],"sessionId":"ai","routeGeneration":"9007199254740992"})",
         R"({"kind":"captured","id":"input","samples":[1],"sessionId":"a\n","routeGeneration":"10"})",
         R"({"kind":"captured","id":"input","samples":[1],"sessionId":"ai","routeGeneration":"10","method":"sessionEnd"})"}) {
        EXPECT_FALSE(decode_browser_request(body, request));
        EXPECT_TRUE(request.kind.empty());
    }
    EXPECT_TRUE(
        decode_browser_request(R"({"kind":"captured","id":"input","samples":[1]})", request));
}
}  // namespace ur::device
