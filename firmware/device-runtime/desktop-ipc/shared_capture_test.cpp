// 文件说明：同一物理预约器下IPC与人际腿的真实停止门控测试，不使用两个设备身份。
#include <gtest/gtest.h>
#include "browser_call_media.hpp"
#include "browser_ipc_media.hpp"

namespace ur::device {
// IPC已关闭但实际Stopped未到时继续拒绝人际腿；原回执释放后允许单一新所有者。
TEST(SharedCaptureTest, TalkStopMustActuallyCompleteBeforeHumanAccept) {
    CaptureGate gate(3);
    BrowserIPCMediaFactory factory(true, &gate);
    BrowserCallMedia human(&gate, "entry");
    auto peer = factory.create();
    IPCInvite invite;
    invite.envelope.session = "talk-leg";
    invite.audio_track = "mic-1";
    invite.speaker_track = "speaker-1";
    ASSERT_TRUE(peer->begin(invite));
    ASSERT_TRUE(factory.take_command());
    ASSERT_TRUE(human.bind_session("source-leg"));
    EXPECT_FALSE(human.available());
    EXPECT_FALSE(human.prepare());
    peer->close();
    ASSERT_TRUE(factory.take_command());
    EXPECT_FALSE(human.available());
    EXPECT_FALSE(factory.report_stopped("other-leg", "1"));
    EXPECT_FALSE(factory.report_stopped("talk-leg", "2"));
    ASSERT_TRUE(factory.report_stopped("talk-leg", "1"));
    ASSERT_TRUE(human.prepare());
    auto second = factory.create();
    invite.envelope.session = "next-talk";
    EXPECT_FALSE(second->begin(invite));
    EXPECT_FALSE(factory.report_stopped("talk-leg", "1"));
    human.close();
    EXPECT_TRUE(second->begin(invite));
    second->close();
    EXPECT_TRUE(factory.report_stopped("next-talk", "3"));
}
// 未证明共享的监听麦克风忙线拒绝，但纯视频观看不占用人际mic/speaker组。
TEST(SharedCaptureTest, PreviewMicrophoneConflictsButPureVideoDoesNot) {
    CaptureGate gate(3);
    BrowserIPCMediaFactory factory(true, &gate);
    BrowserCallMedia human(&gate, "entry");
    ASSERT_TRUE(human.bind_session("source-leg"));
    ASSERT_TRUE(human.prepare());
    auto preview = factory.create();
    IPCInvite invite;
    invite.envelope.session = "view-leg";
    invite.video_track = "video-1";
    invite.audio_track = "mic-1";
    EXPECT_FALSE(preview->begin(invite));
    invite.audio_track.clear();
    EXPECT_TRUE(preview->begin(invite));
    human.close();
    preview->close();
    EXPECT_TRUE(factory.report_stopped("view-leg", "2"));
}
// 本地意图只接受固定scope和原票据，不允许浏览器拼平台method或权限。
TEST(SharedCaptureTest, StrictLocalHumanAndStoppedCallbacks) {
    IPCBrowserRequest request;
    EXPECT_TRUE(decode_ipc_browser(R"({"kind":"callAccept","sessionId":"source-leg"})", request));
    EXPECT_TRUE(decode_ipc_browser(
        R"({"kind":"callStopped","sessionId":"source-leg","releaseId":"1"})", request));
    EXPECT_TRUE(decode_ipc_browser(
        R"({"kind":"ipcStopped","sessionId":"view-leg","releaseId":"2"})", request));
    EXPECT_FALSE(decode_ipc_browser(
        R"({"kind":"callAccept","sessionId":"source-leg","cameraId":"other"})", request));
    EXPECT_FALSE(decode_ipc_browser(
        R"({"kind":"callStopped","sessionId":"source-leg","releaseId":1})", request));
    EXPECT_FALSE(decode_ipc_browser(
        R"({"kind":"callStopped","sessionId":"source-leg","releaseId":"01"})", request));
    EXPECT_FALSE(decode_ipc_browser(
        R"({"kind":"callStopped","sessionId":"source-leg","releaseId":"18446744073709551616"})",
        request));
    EXPECT_FALSE(decode_ipc_browser(
        R"({"kind":"callConnected","sessionId":"source-leg","releaseId":"1","method":"mediaState"})",
        request));
    EXPECT_TRUE(request.kind.empty());
}
// 单一能力快照只有驱动三项均明确启用才声明humanCall，不改变旧IPC缺省能力。
TEST(SharedCaptureTest, CapabilityRequiresActualMicTalkAndLocalAccept) {
    IPCCapability capability;
    Envelope command;
    EXPECT_EQ(ipc_capability_command("cap", capability, command), WireResult::Ok);
    EXPECT_EQ(command.payload.find("humanCall"), std::string::npos);
    capability.human_call = true;
    EXPECT_EQ(ipc_capability_command("cap", capability, command), WireResult::Invalid);
    capability.microphone = true;
    capability.conference_talk = true;
    capability.local_accept = false;
    EXPECT_EQ(ipc_capability_command("cap", capability, command), WireResult::Invalid);
    capability.local_accept = true;
    capability.camera_id = "entry";
    EXPECT_EQ(ipc_capability_command("cap", capability, command), WireResult::Ok);
    EXPECT_NE(command.payload.find("humanCall"), std::string::npos);
    EXPECT_NE(command.payload.find("entry"), std::string::npos);
}
}  // namespace ur::device
