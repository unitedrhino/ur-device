// 文件说明：历史录像报文解析回归；纯协议用例不读取档案、不连接媒体、不伪造驱动支持精确clip。
#include "ur/device/record_playback_wire.hpp"
#include <gtest/gtest.h>
#include <utility>
#include <vector>

namespace ur::device {
namespace {
// record_invite 保留历史十一字段及标准档案双轨，不借用实时相机或文件输出许可。
std::string record_invite() {
    return R"({"msgToken":"record-invite","method":"sessionInvite","sessionId":"record-session","eventSeq":1,"params":{"purpose":"recordPlayback","admission":"direct","transport":"webrtc","expiresAt":1900000060000,"cameraId":"entry","recordingId":"record-1","recordingVersion":"fixed-1","playbackId":"player-1","mediaSource":{"contentRef":"recording:fixed-clip-1"},"clip":{"startMs":0,"endMs":30000},"tracks":[{"trackId":"video-1","kind":"video","sourceId":"record-video","direction":"sendonly","codec":"H264","state":"requested"},{"trackId":"audio-1","kind":"audio","sourceId":"record-audio","direction":"sendonly","codec":"opus","state":"requested"}]}})";
}
// alter 修改固定测试片段，不在解析器内提供默认值或容错能力。
std::string alter(std::string value, std::string_view before, std::string_view after) {
    const auto offset = value.find(before);
    EXPECT_NE(offset, std::string::npos);
    if (offset == std::string::npos)
        return {};
    value.replace(offset, before.size(), after);
    return value;
}
// record_control 生成当前媒体代次操作；位置与引用仅用于对应条件字段。
std::string record_control(std::string_view operation, std::string_view extra = {}) {
    return std::string(
               R"({"msgToken":"control-1","method":"playbackControl","sessionId":"record-session","eventSeq":2,"params":{"operation":")") +
           std::string(operation) +
           R"(","expectedRevision":0,"playbackId":"player-1","mediaRevision":1)" +
           std::string(extra) + "}}";
}
// actual_state 生成实际状态应答；去掉code才是自主事件，不用eventSeq冒充实际revision。
std::string actual_state(std::string_view state = "playing") {
    return std::string(
               R"({"msgToken":"fact-1","method":"playbackState","sessionId":"record-session","code":200,"data":{"state":")") +
           std::string(state) +
           R"(","revision":2,"playbackId":"player-1","positionMs":0,"rate":1,"durationMs":30000,"mediaRevision":1}})";
}
// failed_state 是自主失败事实，带媒体代次但没有虚构实际播放位置或时长。
std::string failed_state() {
    return R"({"msgToken":"failed-1","method":"playbackState","sessionId":"record-session","data":{"state":"failed","revision":3,"playbackId":"player-1","reason":"decodeFailed","mediaRevision":1}})";
}
// released_state 只关联原终态整组退出，不把当前媒体代次当释放范围。
std::string released_state() {
    return R"({"msgToken":"ended-original","method":"playbackState","sessionId":"record-session","data":{"playbackId":"player-1","released":true,"endEventSeq":9}})";
}
// invalid_invites 对全部非法输入核对返回值及失败清空，不仅检查是否崩溃。
void invalid_invites(const std::vector<std::string>& values) {
    for (const auto& value : values) {
        SCOPED_TRACE(value);
        RecordPlaybackInvite out;
        out.content_ref = "sentinel";
        EXPECT_EQ(decode_record_playback_invite(WireRoute::Signaling, value, out),
                  WireResult::Invalid);
        EXPECT_TRUE(out.content_ref.empty());
        EXPECT_TRUE(out.envelope.session.empty());
    }
}
// invalid_states 保证非法旧字段或混合事实不能在失败出口残留为有效Actual。
void invalid_states(const std::vector<std::string>& values) {
    for (const auto& value : values) {
        SCOPED_TRACE(value);
        RecordPlaybackState out;
        out.playback_id = "sentinel";
        EXPECT_EQ(decode_record_playback_state(WireRoute::Signaling, value, out),
                  WireResult::Invalid);
        EXPECT_TRUE(out.playback_id.empty());
        EXPECT_TRUE(out.envelope.session.empty());
    }
}
}  // namespace

TEST(RecordPlaybackInviteWire, FixedVersionClipAndArchiveTracks) {
    RecordPlaybackInvite out;
    ASSERT_EQ(decode_record_playback_invite(WireRoute::Signaling, record_invite(), out),
              WireResult::Ok);
    EXPECT_EQ(out.envelope.event_sequence, 1U);
    EXPECT_EQ(out.camera_id, "entry");
    EXPECT_EQ(out.recording_id, "record-1");
    EXPECT_EQ(out.recording_version, "fixed-1");
    EXPECT_EQ(out.playback_id, "player-1");
    EXPECT_EQ(out.content_ref, "recording:fixed-clip-1");
    EXPECT_EQ(out.clip.start_ms, 0U);
    EXPECT_EQ(out.clip.end_ms, 30000U);
    EXPECT_EQ(out.expires_at_ms, 1900000060000ULL);
    EXPECT_TRUE(out.audio);
}
TEST(RecordPlaybackInviteWire, VideoOnlyDoesNotAcquireAudio) {
    const auto value = alter(
        record_invite(),
        R"(,{"trackId":"audio-1","kind":"audio","sourceId":"record-audio","direction":"sendonly","codec":"opus","state":"requested"})",
        "");
    RecordPlaybackInvite out;
    ASSERT_EQ(decode_record_playback_invite(WireRoute::Signaling, value, out), WireResult::Ok);
    EXPECT_FALSE(out.audio);
}
TEST(RecordPlaybackInviteWire, PartialClipIsExpressibleButNotDriverAcceptance) {
    RecordPlaybackInvite out;
    ASSERT_EQ(
        decode_record_playback_invite(
            WireRoute::Signaling, alter(record_invite(), "\"startMs\":0", "\"startMs\":1000"), out),
        WireResult::Ok);
    EXPECT_EQ(out.clip.start_ms, 1000U);  // 后续初轮驱动应明确Unsupported，不由解析器假装精确裁剪。
}
TEST(RecordPlaybackInviteWire, WrongPurposeTransportIdentityAndFieldsRejected) {
    invalid_invites({
        alter(record_invite(), "recordPlayback", "ipcLiveView"),
        alter(record_invite(), "recordPlayback", "mediaPlayback"),
        alter(record_invite(), "\"direct\"", "\"accept\""),
        alter(record_invite(), "\"webrtc\"", "\"none\""),
        alter(record_invite(), "\"recordingVersion\":\"fixed-1\",", ""),
        alter(record_invite(), "\"fixed-1\"", "null"),
        alter(record_invite(), "\"fixed-1\"", "\"\""),
        alter(record_invite(), "\"fixed-1\"", "\"" + std::string(65, 'v') + "\""),
        alter(record_invite(), "\"record-1\"", "\"" + std::string(65, 'r') + "\""),
        alter(record_invite(), "\"cameraId\":\"entry\"",
              "\"cameraId\":\"entry\",\"ownerUserId\":42"),
        alter(record_invite(), "\"mediaSource\":{", "\"inputModalities\":[],\"mediaSource\":{"),
        alter(record_invite(), "\"clip\":{", "\"outputModalities\":[],\"clip\":{"),
        alter(record_invite(), "\"cameraId\":\"entry\"", "\"cameraId\":\"entry\\n\""),
    });
}
TEST(RecordPlaybackInviteWire, URLPathAndSourceExtraFieldsRejected) {
    std::vector<std::string> values;
    for (const auto* ref :
         {"https://example.invalid/file", "/tmp/file", "../file", "C:/file", ".", "..", " ref",
          "ref ", "folder/file", "FiLe:record", "data:payload", "javascript:record", "C:\\file"})
        values.push_back(alter(record_invite(), "recording:fixed-clip-1", ref));
    values.push_back(
        alter(record_invite(), "\"mediaSource\":{", "\"mediaSource\":{\"url\":\"example\","));
    invalid_invites(values);
}
TEST(RecordPlaybackInviteWire, NumericBoundsClipOrderAndDuplicateKeysRejected) {
    invalid_invites({
        alter(record_invite(), "\"startMs\":0", "\"startMs\":-1"),
        alter(record_invite(), "\"startMs\":0", "\"startMs\":0.5"),
        alter(record_invite(), "\"startMs\":0", "\"startMs\":\"0\""),
        alter(record_invite(), "\"startMs\":0", "\"startMs\":null"),
        alter(record_invite(), "\"endMs\":30000", "\"endMs\":0"),
        alter(record_invite(), "\"endMs\":30000", "\"endMs\":9007199254740992"),
        alter(record_invite(), "\"endMs\":30000", "\"endMs\":30000,\"endMs\":30001"),
        alter(record_invite(), "\"expiresAt\":1900000060000", "\"expiresAt\":0"),
        alter(record_invite(), "\"expiresAt\":1900000060000", "\"expiresAt\":\"1900000060000\""),
    });
    RecordPlaybackInvite out;
    ASSERT_EQ(decode_record_playback_invite(
                  WireRoute::Signaling,
                  alter(record_invite(), "\"endMs\":30000", "\"endMs\":9007199254740991"), out),
              WireResult::Ok);
    EXPECT_EQ(out.clip.end_ms, 9007199254740991ULL);
}
TEST(RecordPlaybackInviteWire, RealTimeSourcesDirectionsAndTrackExtrasRejected) {
    invalid_invites({
        alter(record_invite(), "record-video", "entry"),
        alter(record_invite(), "record-audio", "mic"),
        alter(record_invite(), "\"sendonly\"", "\"sendrecv\""),
        alter(record_invite(), "\"H264\"", "\"VP8\""),
        alter(record_invite(), "\"opus\"", "\"pcm\""),
        alter(record_invite(), "\"trackId\":\"video-1\"",
              "\"trackId\":\"video-1\",\"sourceType\":\"camera\""),
        alter(record_invite(), "\"trackId\":\"audio-1\"", "\"trackId\":\"video-1\""),
        alter(record_invite(), "\"requested\"", "\"active\""),
    });
}
TEST(RecordPlaybackInviteWire, WrongRouteMalformedAndWrongEnvelopeClearOutput) {
    RecordPlaybackInvite out;
    out.recording_id = "sentinel";
    EXPECT_EQ(decode_record_playback_invite(WireRoute::Message, record_invite(), out),
              WireResult::WrongRoute);
    EXPECT_TRUE(out.recording_id.empty());
    invalid_invites({
        "null",
        "[]",
        "invalid",
        record_invite() + "{}",
        alter(record_invite(), "\"eventSeq\":1,", ""),
        alter(record_invite(), "\"eventSeq\":1", "\"eventSeq\":0"),
        alter(record_invite(), "\"eventSeq\":1", "\"eventSeq\":1,\"messageSeq\":1"),
        alter(record_invite(), "\"record-session\"", "\"\""),
        alter(record_invite(), "\"record-session\"", "\"" + std::string(65, 's') + "\""),
        alter(record_invite(), "\"params\":{", "\"code\":200,\"params\":{"),
        alter(record_invite(), "\"recordingId\":\"record-1\"",
              "\"recordingId\":\"record\\u0000-1\""),
    });
}
TEST(RecordPlaybackControlWire, RequiredMediaRevisionAndConditionalOperations) {
    for (const auto& item : std::vector<std::pair<std::string, std::string>>{
             {"load", R"(,"contentRef":"recording:fixed-clip-1")"},
             {"play", ""},
             {"pause", ""},
             {"seek", R"(,"positionMs":0)"},
             {"stop", ""}}) {
        PlaybackCommand out;
        ASSERT_EQ(decode_record_playback_control(WireRoute::Signaling,
                                                 record_control(item.first, item.second), out),
                  WireResult::Ok);
        EXPECT_EQ(out.media_revision, 1U);
        EXPECT_EQ(out.operation, item.first);
    }
}
TEST(RecordPlaybackControlWire, MissingWrongMediaRevisionCannotUseFileDefaults) {
    for (const auto* replacement :
         {"\"mediaRevision\":0", "\"mediaRevision\":-1", "\"mediaRevision\":1.5",
          "\"mediaRevision\":null", "\"mediaRevision\":\"1\"",
          "\"mediaRevision\":9007199254740992"}) {
        PlaybackCommand out;
        out.playback_id = "sentinel";
        EXPECT_EQ(decode_record_playback_control(
                      WireRoute::Signaling,
                      alter(record_control("play"), "\"mediaRevision\":1", replacement), out),
                  WireResult::Invalid);
        EXPECT_TRUE(out.playback_id.empty());
    }
    PlaybackCommand out;
    EXPECT_EQ(
        decode_record_playback_control(
            WireRoute::Signaling, alter(record_control("play"), ",\"mediaRevision\":1", ""), out),
        WireResult::Invalid);
    EXPECT_EQ(decode_record_playback_control(WireRoute::Message, record_control("play"), out),
              WireResult::WrongRoute);
}
TEST(RecordPlaybackControlWire, FixedRateUnsupportedAndFieldsStillStrict) {
    PlaybackCommand out;
    EXPECT_EQ(decode_record_playback_control(WireRoute::Signaling,
                                             record_control("setRate", R"(,"rate":1)"), out),
              WireResult::Unsupported);
    EXPECT_TRUE(out.envelope.token.empty());
    for (const auto& value :
         {record_control("seek"), record_control("play", R"(,"positionMs":0)"),
          record_control("load"), record_control("stop", R"(,"rate":1)"),
          record_control("load", R"(,"contentRef":"folder/file")"),
          record_control("load", R"(,"contentRef":"https://example.invalid/file")")})
        EXPECT_EQ(decode_record_playback_control(WireRoute::Signaling, value, out),
                  WireResult::Invalid);
}
TEST(RecordPlaybackStateWire, ActualReplyAndSpontaneousStatesPreserveMediaRevision) {
    for (const auto* state : {"loaded", "playing", "paused", "seeking", "stopped", "ended"}) {
        RecordPlaybackState out;
        ASSERT_EQ(decode_record_playback_state(WireRoute::Signaling, actual_state(state), out),
                  WireResult::Ok);
        EXPECT_EQ(out.kind, RecordPlaybackStateKind::Actual);
        EXPECT_EQ(out.state, state);
        EXPECT_EQ(out.media_revision, 1U);
        EXPECT_EQ(out.position_ms, 0U);
        EXPECT_EQ(out.duration_ms, 30000U);
        ASSERT_EQ(decode_record_playback_state(
                      WireRoute::Signaling, alter(actual_state(state), "\"code\":200,", ""), out),
                  WireResult::Ok);
        EXPECT_FALSE(out.envelope.code);
    }
    RecordPlaybackState out;
    EXPECT_EQ(
        decode_record_playback_state(
            WireRoute::Signaling,
            alter(actual_state("seeking"), "\"mediaRevision\":1", "\"mediaRevision\":2"), out),
        WireResult::Ok);
    EXPECT_EQ(out.media_revision, 2U);  // 当前m+1是否获原seek授权由client判断，不由parser增加权限。
}
TEST(RecordPlaybackStateWire, OptionalDurationAndIntegerBoundaryRemainExact) {
    RecordPlaybackState out;
    ASSERT_EQ(decode_record_playback_state(WireRoute::Signaling,
                                           alter(actual_state(), "\"durationMs\":30000,", ""), out),
              WireResult::Ok);
    EXPECT_FALSE(out.duration_ms);
    const auto value =
        alter(alter(actual_state(), "\"durationMs\":30000", "\"durationMs\":9007199254740991"),
              "\"positionMs\":0", "\"positionMs\":9007199254740991");
    ASSERT_EQ(decode_record_playback_state(WireRoute::Signaling, value, out), WireResult::Ok);
    EXPECT_EQ(out.position_ms, 9007199254740991ULL);
}
// 播放器revision是必填非负水位，零不表示缺省，也不放宽必须为正的媒体代次。
TEST(RecordPlaybackStateWire, ActualRevisionZeroIsPresentAndValid) {
    RecordPlaybackState out;
    ASSERT_EQ(
        decode_record_playback_state(
            WireRoute::Signaling, alter(actual_state(), "\"revision\":2", "\"revision\":0"), out),
        WireResult::Ok);
    EXPECT_EQ(out.kind, RecordPlaybackStateKind::Actual);
    EXPECT_EQ(out.revision, 0U);
    EXPECT_EQ(out.media_revision, 1U);
}
// 初始实例自主失败也可携带实际零revision；失败ACK和自主失败正文仍严格区分。
TEST(RecordPlaybackStateWire, AutonomousFailureRevisionZeroIsPresentAndValid) {
    RecordPlaybackState out;
    ASSERT_EQ(
        decode_record_playback_state(
            WireRoute::Signaling, alter(failed_state(), "\"revision\":3", "\"revision\":0"), out),
        WireResult::Ok);
    EXPECT_EQ(out.kind, RecordPlaybackStateKind::Actual);
    EXPECT_EQ(out.state, "failed");
    EXPECT_EQ(out.revision, 0U);
    EXPECT_EQ(out.media_revision, 1U);
}
TEST(RecordPlaybackStateWire, FailureAckHasOnlyReasonNotSuccessfulActual) {
    const std::string value =
        R"({"msgToken":"control-1","method":"playbackState","sessionId":"record-session","code":503,"data":{"reason":"sourceUnavailable"}})";
    RecordPlaybackState out;
    ASSERT_EQ(decode_record_playback_state(WireRoute::Signaling, value, out), WireResult::Ok);
    EXPECT_EQ(out.kind, RecordPlaybackStateKind::Failure);
    EXPECT_TRUE(out.playback_id.empty());
    EXPECT_EQ(out.media_revision, 0U);
    invalid_states({alter(value, "503", "201"), alter(value, "503", "600"),
                    alter(value, "\"sourceUnavailable\"", "null"),
                    alter(value, "\"reason\":", "\"mediaRevision\":1,\"reason\":"),
                    alter(value, "\"reason\":", "\"state\":\"failed\",\"reason\":")});
}
TEST(RecordPlaybackStateWire, AutonomousFailureCarriesRevisionButNoFakePosition) {
    RecordPlaybackState out;
    ASSERT_EQ(decode_record_playback_state(WireRoute::Signaling, failed_state(), out),
              WireResult::Ok);
    EXPECT_EQ(out.kind, RecordPlaybackStateKind::Actual);
    EXPECT_EQ(out.state, "failed");
    EXPECT_EQ(out.media_revision, 1U);
    invalid_states({alter(failed_state(), "\"data\":{", "\"code\":200,\"data\":{"),
                    alter(failed_state(), ",\"mediaRevision\":1", ""),
                    alter(failed_state(), "\"reason\":", "\"positionMs\":0,\"reason\":")});
}
TEST(RecordPlaybackStateWire, EntireGroupReleaseNeverIncludesMediaRevision) {
    RecordPlaybackState out;
    ASSERT_EQ(decode_record_playback_state(WireRoute::Signaling, released_state(), out),
              WireResult::Ok);
    EXPECT_EQ(out.kind, RecordPlaybackStateKind::Released);
    EXPECT_EQ(out.playback_id, "player-1");
    EXPECT_EQ(out.end_event_sequence, 9U);
    EXPECT_EQ(out.media_revision, 0U);
    invalid_states(
        {alter(released_state(), "true", "false"), alter(released_state(), "true", "1"),
         alter(released_state(), "\"endEventSeq\":9", "\"endEventSeq\":0"),
         alter(released_state(), "\"data\":{", "\"code\":200,\"data\":{"),
         alter(released_state(), "\"released\":true", "\"released\":true,\"mediaRevision\":1"),
         alter(released_state(), "\"released\":true", "\"released\":true,\"positionMs\":0")});
}
TEST(RecordPlaybackStateWire, MissingInvalidOrMixedActualFieldsRejected) {
    invalid_states({
        alter(actual_state(), ",\"mediaRevision\":1", ""),
        alter(actual_state(), "\"mediaRevision\":1", "\"mediaRevision\":0"),
        alter(actual_state(), "\"revision\":2", "\"revision\":-1"),
        alter(actual_state(), "\"rate\":1", "\"rate\":2"),
        alter(actual_state(), "\"rate\":1", "\"rate\":\"1\""),
        alter(actual_state(), "\"positionMs\":0", "\"positionMs\":30001"),
        alter(actual_state(), "\"positionMs\":0", "\"positionMs\":-1"),
        alter(actual_state(), "\"durationMs\":30000", "\"durationMs\":null"),
        alter(actual_state(), "\"playing\"", "\"connected\""),
        alter(actual_state(), "\"positionMs\":0", "\"positionMs\":0,\"released\":true"),
        alter(actual_state(), "\"positionMs\":0", "\"positionMs\":0,\"url\":\"example\""),
        alter(actual_state(), "\"revision\":2", "\"revision\":2,\"revision\":3"),
    });
}
TEST(RecordPlaybackStateWire, InvalidEnvelopeAndWrongRouteClearOutput) {
    RecordPlaybackState out;
    out.reason = "sentinel";
    EXPECT_EQ(decode_record_playback_state(WireRoute::Message, actual_state(), out),
              WireResult::WrongRoute);
    EXPECT_TRUE(out.reason.empty());
    invalid_states({"null", "invalid", actual_state() + "{}",
                    alter(actual_state(), "\"code\":200", "\"eventSeq\":1,\"code\":200"),
                    alter(actual_state(), "\"code\":200", "\"messageSeq\":1,\"code\":200"),
                    alter(actual_state(), "\"playbackState\"", "\"trackState\""),
                    alter(actual_state(), "\"record-session\"", "\"\""),
                    alter(actual_state(), "\"data\":", "\"params\":")});
}
}  // namespace ur::device
