// 文件说明：实时节目独立接收契约的纯C++回归，不使用网络、不采集、不把协议解析当播放验收。
#include "ur/device/live_wire.hpp"
#include <gtest/gtest.h>
#include <utility>
#include <vector>

namespace ur::device {
namespace {
// audio_track 保留节目标准六字段，与AI输出或设备麦克风轨没有隐式映射。
std::string audio_track() {
    return R"({"trackId":"broadcast-out","kind":"audio","sourceId":"broadcast-audio","direction":"recvonly","codec":"opus","state":"requested"})";
}
// video_track 是独立可见接收轨，不能改写成摄像头发布轨。
std::string video_track() {
    return R"({"trackId":"broadcast-video-out","kind":"video","sourceId":"broadcast-video","direction":"recvonly","codec":"H264","state":"requested"})";
}
// invitation 构造原完整组；五键params不携带私有源引用或用户身份。
std::string invitation(bool audio = true, bool video = true) {
    const auto tracks = (audio ? audio_track() : std::string{}) + (audio && video ? "," : "") +
                        (video ? video_track() : std::string{});
    return R"({"msgToken":"live-original","method":"sessionInvite","sessionId":"live-session","eventSeq":1,"params":{"purpose":"liveBroadcast","admission":"direct","transport":"webrtc","expiresAt":1900000060000,"tracks":[)" +
           tracks + "]}}";
}
// alter 修改固定测试片段，失败即暴露夹具错误，不在解析器提供容错默认值。
std::string alter(std::string value, std::string_view before, std::string_view after) {
    const auto offset = value.find(before);
    EXPECT_NE(offset, std::string::npos);
    if (offset == std::string::npos)
        return {};
    value.replace(offset, before.size(), after);
    return value;
}
// control 使用当前原代次单轨操作，不携带其他轨或追加自由来源。
std::string control(std::string_view operation = "resume", bool video = false) {
    return std::string(
               R"({"msgToken":"live-control","method":"trackControl","sessionId":"live-session","eventSeq":2,"params":{"trackId":")") +
           (video ? "broadcast-video-out" : "broadcast-out") + R"(","operation":")" +
           std::string(operation) + R"(","mediaRevision":1}})";
}
// actual 只构造成功实际三字段ACK，不能把无code自主事件带入第一阶段。
std::string actual(std::string_view state = "active") {
    return std::string(
               R"({"msgToken":"live-control","method":"trackState","sessionId":"live-session","code":200,"data":{"trackId":"broadcast-out","state":")") +
           std::string(state) + R"(","mediaRevision":1}})";
}
// invalid_invites 同时验证失败值清空，防止错误出口残留可运行邀请。
void invalid_invites(const std::vector<std::string>& values) {
    for (const auto& value : values) {
        SCOPED_TRACE(value);
        LiveInvite invite;
        invite.audio = invite.video = true;
        EXPECT_EQ(decode_live_invite(WireRoute::Signaling, value, invite), WireResult::Invalid);
        EXPECT_FALSE(invite.audio);
        EXPECT_FALSE(invite.video);
        EXPECT_TRUE(invite.envelope.session.empty());
    }
}
}  // namespace

TEST(LiveInviteWire, AudioVideoAndIndependentGroupsAreExplicit) {
    for (const auto& flags :
         {std::pair{true, false}, std::pair{false, true}, std::pair{true, true}}) {
        LiveInvite invite;
        ASSERT_EQ(
            decode_live_invite(WireRoute::Signaling, invitation(flags.first, flags.second), invite),
            WireResult::Ok);
        EXPECT_EQ(invite.audio, flags.first);
        EXPECT_EQ(invite.video, flags.second);
        EXPECT_EQ(invite.expires_at_ms, 1900000060000ULL);
        EXPECT_EQ(invite.envelope.event_sequence, 1U);
        EXPECT_EQ(live_has_track(invite, "broadcast-out"), flags.first);
        EXPECT_EQ(live_has_track(invite, "broadcast-video-out"), flags.second);
        EXPECT_FALSE(live_has_track(invite, "mic-1"));
    }
}
TEST(LiveInviteWire, FixedPurposeAdmissionTransportAndFiveFields) {
    invalid_invites({
        alter(invitation(), "liveBroadcast", "mediaPlayback"),
        alter(invitation(), "\"direct\"", "\"accept\""),
        alter(invitation(), "\"webrtc\"", "\"udp\""),
        alter(invitation(), "\"purpose\":", "\"url\":\"https://example.invalid\",\"purpose\":"),
        alter(invitation(), "\"tracks\":", "\"cameraId\":\"front\",\"tracks\":"),
        alter(invitation(), "\"tracks\":", "\"inputModalities\":[],\"tracks\":"),
        alter(invitation(), "\"tracks\":", "\"outputModalities\":[],\"tracks\":"),
        alter(invitation(), "\"admission\":\"direct\",", ""),
        alter(invitation(), "\"transport\":\"webrtc\",", ""),
    });
}
TEST(LiveInviteWire, TrackShapeSourceAndCodecCannotGrantCapture) {
    invalid_invites({
        alter(invitation(), "broadcast-audio", "mic"),
        alter(invitation(), "broadcast-video\"", "front\""),
        alter(invitation(), "\"recvonly\"", "\"sendrecv\""),
        alter(invitation(), "\"opus\"", "\"pcm\""),
        alter(invitation(), "\"H264\"", "\"VP8\""),
        alter(invitation(), "\"requested\"", "\"active\""),
        alter(invitation(), "\"kind\":\"audio\"", "\"kind\":\"audio\",\"sourceType\":\"url\""),
        alter(invitation(), "\"sourceId\":\"broadcast-audio\",", ""),
        alter(invitation(), audio_track() + "," + video_track(),
              video_track() + "," + audio_track()),
        alter(invitation(), audio_track() + "," + video_track(),
              audio_track() + "," + audio_track()),
        alter(invitation(), audio_track() + "," + video_track(),
              audio_track() + "," + video_track() + "," + video_track()),
        invitation(false, false),
    });
}
TEST(LiveInviteWire, DeadlinePositiveSafeAndDuplicateMalformedJSONRejected) {
    for (const auto* deadline : {"0", "-1", "1.5", "null", "\"1\"", "9007199254740992"})
        invalid_invites({alter(invitation(), "1900000060000", deadline)});
    invalid_invites(
        {"null", "[]", "broken", invitation() + "{}",
         alter(invitation(), "\"purpose\":", "\"purpose\":\"liveBroadcast\",\"purpose\":"),
         alter(invitation(), "\"broadcast-audio\"", "\"broadcast\\u0000-audio\"")});
    LiveInvite invite;
    ASSERT_EQ(decode_live_invite(WireRoute::Signaling,
                                 alter(invitation(), "1900000060000", "9007199254740991"), invite),
              WireResult::Ok);
    EXPECT_EQ(invite.expires_at_ms, 9007199254740991ULL);
}
TEST(LiveInviteWire, WrongRouteAndEnvelopeCannotLeakPreviousInvite) {
    LiveInvite invite;
    invite.audio = true;
    EXPECT_EQ(decode_live_invite(WireRoute::Message, invitation(), invite), WireResult::WrongRoute);
    EXPECT_FALSE(invite.audio);
    invalid_invites({
        alter(invitation(), "\"eventSeq\":1,", ""),
        alter(invitation(), "\"eventSeq\":1", "\"eventSeq\":0"),
        alter(invitation(), "\"eventSeq\":1", "\"eventSeq\":1,\"messageSeq\":1"),
        alter(invitation(), "\"live-session\"", "\"\""),
        alter(invitation(), "\"live-session\"", "\"" + std::string(65, 's') + "\""),
        alter(invitation(), "\"params\":", "\"code\":200,\"params\":"),
    });
}
TEST(LiveTrackWire, IndependentPauseResumeStopKeepOriginalTrackAndRevision) {
    LiveInvite invite;
    ASSERT_EQ(decode_live_invite(WireRoute::Signaling, invitation(), invite), WireResult::Ok);
    for (const auto* operation : {"pause", "resume", "stop"}) {
        for (const bool video : {false, true}) {
            LiveTrackControl command;
            ASSERT_EQ(decode_live_track_control(WireRoute::Signaling, control(operation, video),
                                                invite, command),
                      WireResult::Ok);
            EXPECT_EQ(command.operation, operation);
            EXPECT_EQ(command.track_id, video ? "broadcast-video-out" : "broadcast-out");
            EXPECT_EQ(command.media_revision, 1U);
        }
    }
}
TEST(LiveTrackWire, UnknownTrackDifferentSessionOrExtraCommandsRejected) {
    LiveInvite invite;
    ASSERT_EQ(decode_live_invite(WireRoute::Signaling, invitation(true, false), invite),
              WireResult::Ok);
    for (const auto& value : std::vector<std::string>{
             control("resume", true),
             control("publish"),
             control("seek"),
             control("subscribe"),
             alter(control(), "\"live-session\"", "\"another-session\""),
             alter(control(), "\"broadcast-out\"", "\"speaker-1\""),
             alter(control(), "\"operation\":", "\"reason\":\"extra\",\"operation\":"),
             alter(control(), "\"eventSeq\":2,", ""),
             alter(control(), "\"mediaRevision\":1", "\"mediaRevision\":null"),
             alter(control(), "\"mediaRevision\":1", "\"mediaRevision\":0"),
             alter(control(), "\"mediaRevision\":1", "\"mediaRevision\":9007199254740992"),
             alter(control(), "\"mediaRevision\":1", "\"mediaRevision\":1.5"),
             alter(control(), "\"mediaRevision\":1", "\"mediaRevision\":\"1\""),
         }) {
        SCOPED_TRACE(value);
        LiveTrackControl command;
        command.operation = "sentinel";
        EXPECT_EQ(decode_live_track_control(WireRoute::Signaling, value, invite, command),
                  WireResult::Invalid);
        EXPECT_TRUE(command.operation.empty());
    }
    LiveTrackControl command;
    EXPECT_EQ(decode_live_track_control(WireRoute::Message, control(), invite, command),
              WireResult::WrongRoute);
}
TEST(LiveTrackWire, SuccessfulActualAndReasonOnlyFailureAreDistinct) {
    for (const auto* state : {"active", "paused", "stopped"}) {
        LiveTrackState fact;
        ASSERT_EQ(decode_live_track_state(WireRoute::Signaling, actual(state), fact),
                  WireResult::Ok);
        EXPECT_EQ(fact.state, state);
        EXPECT_EQ(fact.track_id, "broadcast-out");
        EXPECT_EQ(fact.media_revision, 1U);
        EXPECT_TRUE(fact.reason.empty());
    }
    LiveTrackState fact;
    ASSERT_EQ(
        decode_live_track_state(
            WireRoute::Signaling,
            R"({"msgToken":"live-control","method":"trackState","sessionId":"live-session","code":503,"data":{"reason":"outputFailed"}})",
            fact),
        WireResult::Ok);
    EXPECT_EQ(fact.reason, "outputFailed");
    EXPECT_TRUE(fact.track_id.empty());
    EXPECT_TRUE(fact.state.empty());
    EXPECT_EQ(fact.media_revision, 0U);
}
TEST(LiveTrackWire, ConnectedHiddenDecodeAndAutonomousEventsAreNotActualActive) {
    for (const auto& value : std::vector<std::string>{
             actual("connected"),
             actual("decoded"),
             actual("failed"),
             alter(actual(), "\"code\":200,", ""),
             alter(actual(), "\"code\":200", "\"code\":202"),
             alter(actual(), "\"code\":200", "\"code\":503"),
             alter(actual(), "\"state\":", "\"reason\":\"mixed\",\"state\":"),
             alter(actual(), "\"mediaRevision\":1", "\"mediaRevision\":0"),
             alter(actual(), "\"mediaRevision\":1", "\"mediaRevision\":1.5"),
             alter(actual(), "\"code\":200", "\"code\":200,\"eventSeq\":3"),
             alter(actual(), "\"trackId\":\"broadcast-out\",", ""),
         }) {
        SCOPED_TRACE(value);
        LiveTrackState fact;
        fact.state = "sentinel";
        EXPECT_EQ(decode_live_track_state(WireRoute::Signaling, value, fact), WireResult::Invalid);
        EXPECT_TRUE(fact.state.empty());
        EXPECT_TRUE(fact.envelope.session.empty());
    }
    LiveTrackState fact;
    EXPECT_EQ(decode_live_track_state(WireRoute::Message, actual(), fact), WireResult::WrongRoute);
}
TEST(LiveCapabilityWire, PureReceiveDoesNotAdvertiseCaptureOrAIAEC) {
    for (const auto& flags :
         {std::pair{true, false}, std::pair{false, true}, std::pair{true, true}}) {
        Envelope command;
        ASSERT_EQ(
            live_capability_command("live-capability", {flags.first, flags.second, 1}, command),
            WireResult::Ok);
        EXPECT_EQ(command.method, "capabilityReport");
        EXPECT_TRUE(command.session.empty());
        EXPECT_NE(command.payload.find("\"features\":[]"), std::string::npos);
        EXPECT_NE(command.payload.find("\"aecMode\":\"\""), std::string::npos);
        EXPECT_EQ(command.payload.find("camera"), std::string::npos);
        EXPECT_EQ(command.payload.find("mic"), std::string::npos);
        EXPECT_EQ(command.payload.find("48000") != std::string::npos, flags.first);
        EXPECT_EQ(command.payload.find("H264") != std::string::npos, flags.second);
    }
}
TEST(LiveCapabilityWire, EmptyUnsafeAndInvalidTokenClearOutput) {
    for (const auto capability :
         {LiveReceiveCapability{false, false, 1}, LiveReceiveCapability{true, true, 0},
          LiveReceiveCapability{true, true, 9007199254740992ULL}}) {
        Envelope command;
        command.method = "sentinel";
        EXPECT_EQ(live_capability_command("original", capability, command), WireResult::Invalid);
        EXPECT_TRUE(command.method.empty());
    }
    Envelope command;
    EXPECT_EQ(live_capability_command("", {true, false, 1}, command), WireResult::Invalid);
    EXPECT_EQ(live_capability_command(std::string(129, 't'), {true, false, 1}, command),
              WireResult::Invalid);
}
}  // namespace ur::device
