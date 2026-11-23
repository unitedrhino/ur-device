// 文件说明：历史桌面端口契约回归；原清单／实际事实／停止分开，不替代浏览器解码或真实媒体E2E。
#include "browser_record_playback.hpp"
#include <gtest/gtest.h>

namespace ur::device {
namespace {
// asset 只提供受控固定清单及本地桥路由，不提供任意宿主路径或外部URL。
BrowserRecordAsset asset() {
    BrowserRecordAsset value;
    value.camera_id = "front";
    value.recording_id = "archive";
    value.recording_version = "v1";
    value.content_ref = "record:archive-v1";
    value.route = "/record-assets/archive.mp4";
    value.duration_ms = 30000;
    value.max_bytes = 1048576;
    value.audio = true;
    return value;
}
// invite 经共享wire获得正式原绑定，端口仍应检查可信清单。
RecordPlaybackInvite invite() {
    RecordPlaybackInvite value;
    EXPECT_EQ(
        decode_record_playback_invite(
            WireRoute::Signaling,
            R"({"msgToken":"inv","method":"sessionInvite","sessionId":"s","eventSeq":1,"params":{"purpose":"recordPlayback","admission":"direct","transport":"webrtc","expiresAt":1030000,"cameraId":"front","recordingId":"archive","recordingVersion":"v1","playbackId":"p","mediaSource":{"contentRef":"record:archive-v1"},"clip":{"startMs":0,"endMs":30000},"tracks":[{"trackId":"video-1","kind":"video","sourceId":"record-video","direction":"sendonly","codec":"H264","state":"requested"},{"trackId":"audio-1","kind":"audio","sourceId":"record-audio","direction":"sendonly","codec":"opus","state":"requested"}]}})",
            value),
        WireResult::Ok);
    return value;
}
// command 仅构造固定绑定的实际控制，不从测试端任意挑设备身份。
PlaybackCommand command(std::string operation, std::uint64_t media = 1) {
    PlaybackCommand value;
    value.envelope.session = "s";
    value.playback_id = "p";
    value.operation = std::move(operation);
    value.content_ref = "record:archive-v1";
    value.media_revision = media;
    if (value.operation == "seek")
        value.position_ms = 10000;
    return value;
}
// BrowserRecordPortTest 明确控制页面Ready、实际load及Stopped，不自动从意图制造成功。
class BrowserRecordPortTest : public ::testing::Test {
protected:
    bool permitted = true;
    BrowserRecordPlaybackPort port{asset(), [this](std::uint64_t) { return permitted; }};
    void ready() {
        port.tick(100);
        ASSERT_TRUE(port.receive(
            R"({"kind":"recordBridgeReady","mime":"video/mp4; codecs=\"avc1.42E01E,opus\"","supported":true})"));
    }
    void bind() {
        ready();
        ASSERT_EQ(port.prepare(invite(), 100000), RecordPreparation::Ready);
        while (port.take_command()) {
        }  // 明确宿主已取原Bind，后续需实际网页关闭回执。
    }
    void load() {
        bind();
        ASSERT_TRUE(port.begin(command("load"), 1, 1));
        ASSERT_TRUE(port.receive(
            R"({"kind":"recordFact","sessionId":"s","playbackId":"p","generation":"1","mediaRevision":1,"code":200,"state":"loaded","positionMs":0,"durationMs":30000,"spontaneous":false})"));
        ASSERT_TRUE(port.take_fact());
    }
};
TEST_F(BrowserRecordPortTest, ExplicitProbeAndActualLoadAreSeparate) {
    port.tick(100);
    EXPECT_FALSE(port.ready());
    EXPECT_EQ(port.prepare(invite(), 100000), RecordPreparation::Unsupported);
    auto probe = port.take_command();
    ASSERT_TRUE(probe);
    EXPECT_NE(probe->find("recordProbe"), std::string::npos);
    ready();
    EXPECT_TRUE(port.ready());
    EXPECT_EQ(port.prepare(invite(), 100000), RecordPreparation::Ready);
    EXPECT_FALSE(port.begin_peer(1));
    EXPECT_FALSE(port.publishing_permitted(1));
}
TEST_F(BrowserRecordPortTest, FixedVersionAndFullClipRequired) {
    ready();
    auto binding = invite();
    binding.recording_version = "v2";
    EXPECT_EQ(port.prepare(binding, 100000), RecordPreparation::Denied);
    binding = invite();
    binding.clip.start_ms = 100;
    EXPECT_EQ(port.prepare(binding, 100000), RecordPreparation::Unsupported);
    EXPECT_EQ(port.prepare(invite(), 100000), RecordPreparation::Ready);
}
TEST_F(BrowserRecordPortTest, ActualFactDuplicateNeverRunsOrAdvancesTwice) {
    bind();
    ASSERT_TRUE(port.begin(command("load"), 1, 1));
    const auto fact =
        R"({"kind":"recordFact","sessionId":"s","playbackId":"p","generation":"1","mediaRevision":1,"code":200,"state":"loaded","positionMs":0,"durationMs":30000,"spontaneous":false})";
    ASSERT_TRUE(port.receive(fact));
    ASSERT_TRUE(port.take_fact());
    EXPECT_TRUE(port.receive(fact));
    EXPECT_FALSE(port.take_fact());
    EXPECT_FALSE(port.begin(command("load"), 1, 1));
}
TEST_F(BrowserRecordPortTest, NewPeerNeedsActualOldStoppedAndOriginalAnswerBeforeConnected) {
    load();
    ASSERT_TRUE(port.begin_peer(1));
    EXPECT_FALSE(port.receive(
        R"({"kind":"recordConnected","sessionId":"s","playbackId":"p","mediaRevision":1})"));
    ASSERT_TRUE(port.apply_answer(1, "v=0\r\ns=answer\r\n"));
    ASSERT_TRUE(port.receive(
        R"({"kind":"recordConnected","sessionId":"s","playbackId":"p","mediaRevision":1})"));
    EXPECT_TRUE(port.publishing_permitted(1));
    port.close_peer(1);
    EXPECT_FALSE(port.peer_stopped(1));
    EXPECT_FALSE(port.connected(1));
    ASSERT_TRUE(port.begin(command("seek"), 2, 2));
    ASSERT_TRUE(port.receive(
        R"({"kind":"recordFact","sessionId":"s","playbackId":"p","generation":"2","mediaRevision":2,"code":200,"state":"seeking","positionMs":9800,"durationMs":30000,"spontaneous":false})"));
    ASSERT_TRUE(port.take_fact());
    EXPECT_FALSE(port.begin_peer(2));
    ASSERT_TRUE(port.receive(
        R"({"kind":"recordPeerStopped","sessionId":"s","playbackId":"p","mediaRevision":1})"));
    EXPECT_TRUE(port.peer_stopped(1));
    EXPECT_TRUE(port.begin_peer(2));
    EXPECT_FALSE(port.apply_answer(1, "v=0\r\ns=late\r\n"));
    EXPECT_FALSE(port.publishing_permitted(2));
}
TEST_F(BrowserRecordPortTest, ClosedNeedsActualOriginalWholeGroupReceipt) {
    load();
    ASSERT_TRUE(port.begin_peer(1));
    EXPECT_FALSE(port.stop_all());
    EXPECT_FALSE(port.receive(
        R"({"kind":"recordClosed","sessionId":"s","playbackId":"p","releaseId":"2","success":true})"));
    EXPECT_FALSE(port.stop_all());
    ASSERT_TRUE(port.receive(
        R"({"kind":"recordClosed","sessionId":"s","playbackId":"p","releaseId":"1","success":true})"));
    EXPECT_TRUE(port.stop_all());
    EXPECT_FALSE(port.begin_peer(1));
    EXPECT_TRUE(port.receive(
        R"({"kind":"recordFact","sessionId":"s","playbackId":"p","generation":"1","mediaRevision":1,"code":200,"state":"playing","positionMs":0,"durationMs":30000,"spontaneous":true})"));
    EXPECT_FALSE(port.take_fact());
}
TEST_F(BrowserRecordPortTest, StrictLocalJsonCannotReplaceOriginalSessionOrDuration) {
    bind();
    ASSERT_TRUE(port.begin(command("load"), 1, 1));
    EXPECT_FALSE(port.receive("{"));
    EXPECT_FALSE(port.receive(
        R"({"kind":"recordFact","sessionId":"other","playbackId":"p","generation":"1","mediaRevision":1,"code":200,"state":"loaded","positionMs":0,"durationMs":30000,"spontaneous":false})"));
    EXPECT_FALSE(port.receive(
        R"({"kind":"recordFact","sessionId":"s","playbackId":"p","generation":"01","mediaRevision":1,"code":200,"state":"loaded","positionMs":0,"durationMs":30000,"spontaneous":false})"));
    EXPECT_FALSE(port.receive(
        R"({"kind":"recordFact","sessionId":"s","playbackId":"p","generation":"1","mediaRevision":1,"code":200,"state":"loaded","positionMs":0,"durationMs":29999,"spontaneous":false})"));
    EXPECT_FALSE(port.take_fact());
    EXPECT_FALSE(port.begin_peer(1));
}
TEST_F(BrowserRecordPortTest, PermissionLossAndRollbackDoNotRestoreOldPublishing) {
    load();
    ASSERT_TRUE(port.begin_peer(1));
    ASSERT_TRUE(port.apply_answer(1, "v=0\r\ns=answer\r\n"));
    ASSERT_TRUE(port.receive(
        R"({"kind":"recordConnected","sessionId":"s","playbackId":"p","mediaRevision":1})"));
    permitted = false;
    port.tick(101);
    EXPECT_FALSE(port.publishing_permitted(1));
    permitted = true;
    port.tick(99);
    EXPECT_FALSE(port.ready());
    EXPECT_FALSE(port.begin(command("play"), 2, 1));
}
TEST_F(BrowserRecordPortTest, UnsentBindingHasNoBrowserResourcesButDeliveredBindingNeedsReceipt) {
    ready();
    ASSERT_EQ(port.prepare(invite(), 100000), RecordPreparation::Ready);
    EXPECT_TRUE(port.stop_all());
    EXPECT_FALSE(port.take_command());
    EXPECT_FALSE(port.begin(command("load"), 1, 1));
}
TEST_F(BrowserRecordPortTest, StopCanInterruptOnlyPendingSeekAndIgnoresItsLateFact) {
    load();
    ASSERT_TRUE(port.begin(command("seek"), 2, 2));
    EXPECT_FALSE(port.begin(command("pause", 2), 3, 2));
    ASSERT_TRUE(port.begin(command("stop", 2), 3, 2));
    EXPECT_TRUE(port.receive(
        R"({"kind":"recordFact","sessionId":"s","playbackId":"p","generation":"2","mediaRevision":2,"code":200,"state":"seeking","positionMs":9800,"durationMs":30000,"spontaneous":false})"));
    EXPECT_FALSE(port.take_fact());
    ASSERT_TRUE(port.receive(
        R"({"kind":"recordFact","sessionId":"s","playbackId":"p","generation":"3","mediaRevision":2,"code":200,"state":"stopped","positionMs":9800,"durationMs":30000,"spontaneous":false})"));
    auto fact = port.take_fact();
    ASSERT_TRUE(fact);
    EXPECT_EQ(fact->state, "stopped");
}
TEST_F(BrowserRecordPortTest, UnknownLateKindDoesNotBypassLocalWhitelist) {
    load();
    EXPECT_FALSE(port.receive(
        R"({"kind":"recordAnything","sessionId":"s","playbackId":"p","mediaRevision":999})"));
    EXPECT_FALSE(port.take_event());
}
TEST(BrowserRecordAssetTest, ArbitraryUrlsAndMissingConfigurationNeverEnableSource) {
    auto fixed = asset();
    fixed.route = "https://elsewhere.invalid/archive.mp4";
    BrowserRecordPlaybackPort denied(fixed, [](std::uint64_t) { return true; });
    EXPECT_FALSE(denied.ready());
    EXPECT_FALSE(denied.take_command());
    BrowserRecordPlaybackPort missing({}, {});
    EXPECT_FALSE(missing.ready());
    EXPECT_FALSE(missing.take_command());
}
}  // namespace
}  // namespace ur::device
