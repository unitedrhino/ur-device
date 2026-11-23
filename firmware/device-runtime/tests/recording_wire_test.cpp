// 文件说明：云端录制报文构造与平台状态解析回归；纯协议用例不取流、不封装文件、不推断同意。
#include "ur/device/recording_wire.hpp"
#include <gtest/gtest.h>
#include <utility>

namespace ur::device {
namespace {
// start_control 生成start全字段结构；测试按需调整字段后再构造。
RecordingControlCommand start_control() {
    RecordingControlCommand control;
    control.operation = "start";
    control.expected_revision = 0;
    control.recording_id = "rec-1";
    control.source_track_ids = {"mic-a", "mic-b"};
    control.consent_ids = {"consent-a", "consent-b"};
    control.policy_id = "retention-policy";
    return control;
}
// start_body 是start控制的标准上行原文；不能带平台事件水位或应答码。
std::string start_body() {
    return R"({"msgToken":"s15-35","method":"recordingControl","sessionId":"s15-session","params":{"operation":"start","expectedRevision":0,"recordingId":"rec-1","sourceTrackIds":["mic-a","mic-b"],"consentIds":["consent-a","consent-b"],"policyId":"retention-policy"}})";
}
// accepted_state 是受理应答；成功应答不能夹带ready产物或failed原因。
std::string accepted_state(std::string_view state) {
    return std::string(
               R"({"msgToken":"s15-35","method":"recordingState","sessionId":"s15-session","code":200,"data":{"state":")") +
           std::string(state) + R"(","revision":1,"recordingId":"rec-1"}})";
}
// event_state 是自主完成事件；ready/failed/自动paused只允许出现在该形态。
std::string event_state(std::string_view body) {
    return std::string(
               R"({"msgToken":"s15-40","method":"recordingState","sessionId":"s15-session","eventSeq":2,"data":)") +
           std::string(body) + "}";
}
// invalid_controls 保证非法控制不能残留半份输出，同时确认合法原文可往返。
void invalid_controls(std::vector<RecordingControlCommand> values) {
    for (auto& value : values) {
        Envelope out;
        out.method = "sentinel";
        EXPECT_EQ(recording_control_command(value, "s15-35", "s15-session", out), WireResult::Invalid);
        EXPECT_TRUE(out.method.empty());
    }
    Envelope round;
    ASSERT_EQ(recording_control_command(start_control(), "s15-35", "s15-session", round), WireResult::Ok);
    EXPECT_EQ(round.payload,
              R"({"consentIds":["consent-a","consent-b"],"expectedRevision":0,"operation":"start","policyId":"retention-policy","recordingId":"rec-1","sourceTrackIds":["mic-a","mic-b"]})");
}
// invalid_states 保证非法事实在失败出口清空，不把应答或事件形态混淆成有效状态。
void invalid_states(const std::vector<std::string>& values) {
    for (const auto& value : values) {
        SCOPED_TRACE(value);
        RecordingStateFact out;
        out.recording_id = "sentinel";
        EXPECT_EQ(decode_recording_state(WireRoute::Signaling, value, out), WireResult::Invalid);
        EXPECT_TRUE(out.recording_id.empty());
        EXPECT_TRUE(out.envelope.session.empty());
    }
}
}  // namespace

// TestRecordingControlCommand 覆盖四操作构造、条件字段互斥与ID边界；构造成功不代表平台受理。
TEST(RecordingControlCommand, BuildsStrictUplink) {
    Envelope out;
    ASSERT_EQ(recording_control_command(start_control(), "s15-35", "s15-session", out), WireResult::Ok);
    EXPECT_TRUE(out.request);
    EXPECT_FALSE(out.code.has_value());
    EXPECT_FALSE(out.event_sequence.has_value());
    EXPECT_FALSE(out.message_sequence.has_value());
    EXPECT_EQ(out.session, "s15-session");
    // 标准原文与构造输出可互证：重新解析标准原文信封后正文一致。
    Envelope parsed;
    ASSERT_EQ(decode_envelope(start_body(), parsed), DecodeResult::Ok);
    EXPECT_EQ(parsed.method, "recordingControl");
    EXPECT_TRUE(parsed.request);
    EXPECT_EQ(parsed.payload,
              R"({"consentIds":["consent-a","consent-b"],"expectedRevision":0,"operation":"start","policyId":"retention-policy","recordingId":"rec-1","sourceTrackIds":["mic-a","mic-b"]})");

    auto pause = start_control();
    pause.operation = "pause";
    pause.expected_revision = 2;
    pause.source_track_ids.clear();
    pause.consent_ids.clear();
    pause.policy_id.clear();
    Envelope paused;
    EXPECT_EQ(recording_control_command(pause, "s15-37", "s15-session", paused), WireResult::Ok);
    EXPECT_EQ(paused.payload.find("consentIds"), std::string::npos) << "pause不得夹带start字段";

    auto resume = start_control();
    resume.operation = "resume";
    resume.expected_revision = 3;
    resume.source_track_ids.clear();
    resume.policy_id.clear();
    Envelope resumed;
    EXPECT_EQ(recording_control_command(resume, "s15-38", "s15-session", resumed), WireResult::Ok);
    EXPECT_EQ(resumed.payload.find("sourceTrackIds"), std::string::npos);

    auto stop = pause;
    stop.operation = "stop";
    stop.expected_revision = 4;
    Envelope stopped;
    EXPECT_EQ(recording_control_command(stop, "s15-39", "s15-session", stopped), WireResult::Ok);

    auto longId = start_control();
    longId.recording_id = std::string(129, 'x');
    invalid_controls({longId});
}

// TestRecordingControlCommandRejects 保证非法输入清空输出，覆盖条件字段与列表边界。
TEST(RecordingControlCommand, RejectsInvalidFields) {
    auto unknown = start_control();
    unknown.operation = "load";
    auto missingTracks = start_control();
    missingTracks.source_track_ids.clear();
    auto pauseWithConsent = start_control();
    pauseWithConsent.operation = "pause";
    pauseWithConsent.source_track_ids.clear();
    pauseWithConsent.policy_id.clear();
    auto resumeWithoutConsent = start_control();
    resumeWithoutConsent.operation = "resume";
    resumeWithoutConsent.source_track_ids.clear();
    resumeWithoutConsent.policy_id.clear();
    resumeWithoutConsent.consent_ids.clear();
    auto startWithoutPolicy = start_control();
    startWithoutPolicy.policy_id.clear();
    auto emptyTracks = start_control();
    emptyTracks.source_track_ids = {};
    auto duplicateConsent = start_control();
    duplicateConsent.consent_ids = {"consent-a", "consent-a"};
    auto overflow = start_control();
    overflow.expected_revision = 9007199254740992ULL;
    auto badTrack = start_control();
    badTrack.source_track_ids = {"mic\t-a"};  // 控制字符非法；普通空格在ID边界内合法。
    auto emptyId = start_control();
    emptyId.recording_id = "";
    invalid_controls({unknown, missingTracks, pauseWithConsent, resumeWithoutConsent,
                      startWithoutPolicy, emptyTracks, duplicateConsent, overflow, badTrack, emptyId});
}

// TestDecodeRecordingState 覆盖受理应答、失败应答与S15报文索引中的自主事件形态。
TEST(DecodeRecordingState, AcceptsContractShapes) {
    for (const auto* state : {"starting", "active", "paused", "finalizing"}) {
        RecordingStateFact fact;
        ASSERT_EQ(decode_recording_state(WireRoute::Signaling, accepted_state(state), fact),
                  WireResult::Ok);
        EXPECT_EQ(fact.state, state);
        EXPECT_EQ(fact.revision, 1u);
        EXPECT_EQ(fact.recording_id, "rec-1");
        EXPECT_TRUE(fact.envelope.code.has_value());
    }
    RecordingStateFact rejected;
    ASSERT_EQ(decode_recording_state(WireRoute::Signaling,
                                     R"({"msgToken":"s15-41","method":"recordingState","sessionId":"s15-session","code":403,"data":{"reason":"recordingConsentMissing"}})",
                                     rejected),
              WireResult::Ok);
    EXPECT_EQ(rejected.reason, "recordingConsentMissing");
    EXPECT_TRUE(rejected.state.empty());
    EXPECT_EQ(rejected.recording_id, "");

    RecordingStateFact ready;
    ASSERT_EQ(decode_recording_state(WireRoute::Signaling,
                                     event_state(
                                         R"({"state":"ready","revision":6,"recordingId":"rec-1","contentRef":"recording:rec-1","durationMs":30000})"),
                                     ready),
              WireResult::Ok);
    EXPECT_EQ(ready.content_ref, "recording:rec-1");
    EXPECT_EQ(ready.duration_ms, 30000u);
    EXPECT_FALSE(ready.envelope.code.has_value());
    EXPECT_EQ(*ready.envelope.event_sequence, 2u);

    RecordingStateFact failed;
    ASSERT_EQ(decode_recording_state(WireRoute::Signaling,
                                     event_state(
                                         R"({"state":"failed","revision":2,"recordingId":"rec-1","reason":"recordingConsentMissing"})"),
                                     failed),
              WireResult::Ok);
    EXPECT_EQ(failed.reason, "recordingConsentMissing");

    RecordingStateFact paused;
    ASSERT_EQ(decode_recording_state(WireRoute::Signaling,
                                     event_state(
                                         R"({"state":"paused","revision":3,"recordingId":"rec-1","reason":"permissionDenied"})"),
                                     paused),
              WireResult::Ok);
    EXPECT_EQ(paused.reason, "permissionDenied");

    RecordingStateFact correlated;
    ASSERT_EQ(decode_recording_state(WireRoute::Signaling,
                                     event_state(
                                         R"({"state":"active","revision":2,"recordingId":"rec-1","requestToken":"s15-35"})"),
                                     correlated),
              WireResult::Ok);
    EXPECT_EQ(correlated.request_token, "s15-35");
}

// ExplicitPauseHasRequestCorrelationWithoutAutomaticReason 验证主动暂停关联原请求，不伪造自动暂停原因。
TEST(DecodeRecordingState, ExplicitPauseHasRequestCorrelationWithoutAutomaticReason) {
    RecordingStateFact paused;
    ASSERT_EQ(decode_recording_state(
                  WireRoute::Signaling,
                  event_state(R"({"state":"paused","revision":3,"recordingId":"rec-1","requestToken":"pause-1"})"),
                  paused),
              WireResult::Ok);
    EXPECT_EQ(paused.state, "paused");
    EXPECT_EQ(paused.request_token, "pause-1");
    EXPECT_TRUE(paused.reason.empty());
}

// TestDecodeRecordingStateRejects 保证形态混淆、缺条件字段与未知原因全部拒绝且清空输出。
TEST(DecodeRecordingState, RejectsInvalidShapes) {
    invalid_states({
        accepted_state("ready"),   // 成功应答不能代替独立完成状态。
        accepted_state("failed"),
        R"({"msgToken":"s15-35","method":"recordingState","sessionId":"s15-session","code":301,"data":{"state":"active","revision":1,"recordingId":"rec-1"}})",
        R"({"msgToken":"s15-35","method":"recordingState","sessionId":"s15-session","code":200,"data":{"state":"active","revision":1,"recordingId":"rec-1","reason":"timeout"}})",
        R"({"msgToken":"s15-41","method":"recordingState","sessionId":"s15-session","code":403,"data":{"reason":"任意自由文字"}})",
        R"({"msgToken":"s15-41","method":"recordingState","sessionId":"s15-session","code":403,"data":{"reason":"recordingConsentMissing","extra":1}})",
        event_state(R"({"state":"ready","revision":6,"recordingId":"rec-1","contentRef":"recording:rec-1"})"),
        event_state(R"({"state":"ready","revision":6,"recordingId":"rec-1","durationMs":30000})"),
        event_state(R"({"state":"ready","revision":6,"recordingId":"rec-1","contentRef":"https://host/file","durationMs":30000})"),
        event_state(R"({"state":"ready","revision":6,"recordingId":"rec-1","contentRef":"recording:rec-1","durationMs":0})"),
        event_state(R"({"state":"failed","revision":2,"recordingId":"rec-1"})"),
        event_state(R"({"state":"paused","revision":3,"recordingId":"rec-1"})"),
        event_state(R"({"state":"paused","revision":3,"recordingId":"rec-1","requestToken":""})"),
        event_state(R"({"state":"paused","revision":3,"recordingId":"rec-1","requestToken":"pause-1","reason":""})"),
        event_state(R"({"state":"loaded","revision":1,"recordingId":"rec-1"})"),
        event_state(R"({"state":"active","revision":-1,"recordingId":"rec-1"})"),
        event_state(R"({"state":"active","revision":1.5,"recordingId":"rec-1"})"),
        event_state(R"({"state":"active","revision":9007199254740992,"recordingId":"rec-1"})"),
        event_state(R"({"state":"active","revision":1,"recordingId":""})"),
        R"({"msgToken":"s15-40","method":"recordingState","sessionId":"s15-session","messageSeq":2,"data":{"state":"active","revision":1,"recordingId":"rec-1"}})",
        R"({"msgToken":"s15-40","method":"playbackState","sessionId":"s15-session","eventSeq":2,"data":{"state":"active","revision":1,"recordingId":"rec-1"}})",
        R"({"msgToken":"s15-35","method":"recordingState","sessionId":"s15-session","code":200,"eventSeq":2,"data":{"state":"active","revision":1,"recordingId":"rec-1"}})",
    });
    RecordingStateFact route;
    EXPECT_EQ(decode_recording_state(WireRoute::Message, accepted_state("active"), route),
              WireResult::WrongRoute);
}
}  // namespace ur::device
