// 文件说明：AI同步报文拒绝缺省、错轨、浮点整数和残留媒体许可；使用占位数据，不代表现场授权。
#include "ur/device/ai_control_wire.hpp"
#include <gtest/gtest.h>

namespace ur::device {
namespace {
// recovery_media 冻结原两条UDP音轨；不安装地址、密钥或真实网络。
VoiceMedia recovery_media() {
    VoiceMedia media;
    media.transport = VoiceTransport::Udp;
    media.input_track = "mic-1";
    media.output_track = "speaker-1";
    return media;
}
// snapshot_body 完整固定AI同步响应，所有无关子对象明确为空，不省略controls字段。
std::string snapshot_body() {
    return R"({"msgToken":"sync","method":"sessionSnapshot","sessionId":"original-ai","eventSeq":9,"code":200,"data":{"state":"established","revision":8,"eventWatermark":9,"messageWatermark":7,"endpointEpoch":2,"capabilityRevision":3,"mediaRevision":1,"expiresAt":20000,"resumeAllowed":true,"participants":[],"tracks":[{"trackId":"mic-1","kind":"audio","sourceId":"mic","direction":"sendonly","codec":"opus","state":"paused"},{"trackId":"speaker-1","kind":"audio","sourceId":"speaker","direction":"recvonly","codec":"opus","state":"paused"}],"controls":{"call":null,"focus":null,"floor":null,"recordings":[],"playbacks":[],"processors":[],"broadcasts":[],"handoffs":[],"mediaPlan":{"revision":1,"tracks":[{"trackId":"mic-1","kind":"audio","sourceId":"mic","direction":"sendonly","codec":"opus","state":"paused"},{"trackId":"speaker-1","kind":"audio","sourceId":"speaker","direction":"recvonly","codec":"opus","state":"paused"}]},"ai":{"inputId":"retired-input","inputState":"paused","activeRespIds":["old-reply"]}}}})";
}
// replaced 只替换一次明确片段，断言测试故障真的命中原字段。
std::string replaced(std::string body, std::string_view from, std::string_view to) {
    const auto position = body.find(from);
    EXPECT_NE(position, std::string::npos);
    if (position != std::string::npos)
        body.replace(position, from.size(), to);
    return body;
}

TEST(AIControlWire, EncodesFourMandatoryFieldsWithExactSafeIntegers) {
    Envelope out;
    ASSERT_EQ(ai_session_sync_command("sync", "original-ai", 9007199254740991ULL, 0, {1, 2}, 1,
                                      VoiceTransport::Udp, out),
              WireResult::Ok);
    EXPECT_EQ(out.method, "sessionSync");
    EXPECT_EQ(
        out.payload,
        R"({"endpointEpoch":1,"lastEventSeq":9007199254740991,"lastMessageSeq":0,"mediaRevision":1})");
    EXPECT_EQ(
        ai_session_sync_command("sync", "original-ai", 0, 0, {1, 2}, 0, VoiceTransport::None, out),
        WireResult::Ok);
    EXPECT_EQ(
        ai_session_sync_command("sync", "original-ai", 0, 0, {1, 2}, 0, VoiceTransport::Udp, out),
        WireResult::Invalid);
    EXPECT_TRUE(out.method.empty());
    EXPECT_EQ(ai_session_sync_command("sync", "original-ai", 0, 0, {}, 1, VoiceTransport::Udp, out),
              WireResult::Invalid);
    EXPECT_EQ(ai_session_sync_command("sync", "original-ai", 9007199254740992ULL, 0, {1, 2}, 1,
                                      VoiceTransport::Udp, out),
              WireResult::Invalid);
}

TEST(AIControlWire, CompleteSnapshotKeepsOriginalTombstonesAndIndependentWatermarks) {
    AIControlSnapshot out;
    ASSERT_EQ(
        decode_ai_control_snapshot(WireRoute::Signaling, snapshot_body(), recovery_media(), out),
        WireResult::Ok);
    EXPECT_EQ(out.binding.endpoint_epoch, 2U);
    EXPECT_EQ(out.binding.capability_revision, 3U);
    EXPECT_EQ(out.event_watermark, 9U);
    EXPECT_EQ(out.message_watermark, 7U);
    EXPECT_EQ(out.input_id, "retired-input");
    EXPECT_EQ(out.active_responses, std::vector<std::string>{"old-reply"});
    EXPECT_FALSE(out.input_active);
    EXPECT_TRUE(out.resume_allowed);
    EXPECT_FALSE(out.ended);
    EXPECT_EQ(
        decode_ai_control_snapshot(WireRoute::Message, snapshot_body(), recovery_media(), out),
        WireResult::WrongRoute);
    EXPECT_TRUE(out.input_id.empty());
}

TEST(AIControlWire, InvalidSnapshotNeverReturnsPartialFacts) {
    const std::vector<std::pair<std::string, std::string>> mutations = {
        {R"("eventWatermark":9)", R"("eventWatermark":8)"},
        {R"("messageWatermark":7)", R"("messageWatermark":7.0)"},
        {R"("endpointEpoch":2)", R"("endpointEpoch":2e0)"},
        {R"("endpointEpoch":2)", R"("endpointEpoch":0)"},
        {R"("capabilityRevision":3)", R"("capabilityRevision":9007199254740992)"},
        {R"("participants":[])", R"("participants":null)"},
        {R"("handoffs":[])", R"("handoffs":null)"},
        {R"("handoffs":[])", R"("handoffs":[{}])"},
        {R"("ai":{"inputId")", R"("ai":{"unknown":true,"inputId")"},
        {R"("inputState":"paused")", R"("inputState":"stopped")"},
        {R"("activeRespIds":["old-reply"])", R"("activeRespIds":["old-reply","old-reply"])"},
        {R"("sourceId":"mic")", R"("sourceId":"another-camera")"},
        {R"("state":"paused")", R"("state":"requested")"},
        {R"("call":null)", R"("call":{})"},
        {R"("expiresAt":20000)", R"("expiresAt":null)"},
        {R"("code":200)", R"("code":202)"},
        {R"("resumeAllowed":true)", R"("resumeAllowed":true,"reason":"snapshotOnly")"}};
    for (const auto& mutation : mutations) {
        SCOPED_TRACE(mutation.first);
        AIControlSnapshot out;
        out.input_id = "must-be-cleared";
        EXPECT_EQ(
            decode_ai_control_snapshot(WireRoute::Signaling,
                                       replaced(snapshot_body(), mutation.first, mutation.second),
                                       recovery_media(), out),
            WireResult::Invalid);
        EXPECT_TRUE(out.input_id.empty());
    }
}

TEST(AIControlWire, FocusProjectionIsStrictReadOnlyState) {
    AIControlSnapshot out;
    const auto body = replaced(
        snapshot_body(), R"("focus":null)",
        R"("focus":{"state":"suspended","revision":4,"focusId":"original-focus","resources":["microphone","speaker"]})");
    ASSERT_EQ(decode_ai_control_snapshot(WireRoute::Signaling, body, recovery_media(), out),
              WireResult::Ok);
    ASSERT_TRUE(out.focus);
    EXPECT_EQ(out.focus->state, "suspended");
    EXPECT_EQ(out.focus->resources.size(), 2U);
    EXPECT_EQ(decode_ai_control_snapshot(WireRoute::Signaling,
                                         replaced(body, R"("speaker"])", R"("microphone"])"),
                                         recovery_media(), out),
              WireResult::Invalid);
}

TEST(AIControlWire, FocusRevisionRetainsMaximumSafeIntegerExactly) {
    AIControlSnapshot out;
    const auto body = replaced(
        snapshot_body(), R"("focus":null)",
        R"("focus":{"state":"suspended","revision":9007199254740991,"focusId":"original-focus","resources":["microphone","speaker"]})");
    ASSERT_EQ(decode_ai_control_snapshot(WireRoute::Signaling, body, recovery_media(), out),
              WireResult::Ok);
    ASSERT_TRUE(out.focus);
    EXPECT_EQ(out.focus->revision, 9007199254740991ULL);
}

TEST(AIControlWire, InitialControlBindingUsesExactIntegerLiteralsAndBothFields) {
    const std::string created =
        R"({"msgToken":"create","method":"sessionCreated","sessionId":"original-ai","code":200,"data":{"state":"negotiating","endpointEpoch":1,"capabilityRevision":1,"selected":{"transport":"none","inputModalities":["text"],"outputModalities":["text"]},"tracks":[]}})";
    VoiceEvent out;
    ASSERT_EQ(decode_voice_event(WireRoute::Signaling, created, out), WireResult::Ok);
    EXPECT_EQ(out.endpoint_epoch, 1U);
    for (const auto* value : {"1.0", "1e0", "1E0", "-1", "0", "9007199254740992", "null"}) {
        SCOPED_TRACE(value);
        EXPECT_EQ(decode_voice_event(WireRoute::Signaling,
                                     replaced(created, "\"endpointEpoch\":1",
                                              std::string("\"endpointEpoch\":") + value),
                                     out),
                  WireResult::Invalid);
        EXPECT_EQ(out.endpoint_epoch, 0U);
    }
    EXPECT_EQ(decode_voice_event(WireRoute::Signaling,
                                 replaced(created, "\"endpointEpoch\":1,", ""), out),
              WireResult::Invalid);
    const auto legacy =
        replaced(replaced(created, "\"endpointEpoch\":1,", ""), "\"capabilityRevision\":1,", "");
    ASSERT_EQ(decode_voice_event(WireRoute::Signaling, legacy, out), WireResult::Ok);
    EXPECT_EQ(out.endpoint_epoch, 0U);
    EXPECT_EQ(out.capability_revision, 0U);
}

TEST(AIControlWire, NoneSnapshotExplicitlyHasNoMediaAndDoesNotInventAnInput) {
    const std::string body =
        R"({"msgToken":"sync","method":"sessionSnapshot","sessionId":"original-ai","eventSeq":9,"code":200,"data":{"state":"established","revision":8,"eventWatermark":9,"messageWatermark":7,"endpointEpoch":2,"capabilityRevision":3,"mediaRevision":0,"expiresAt":20000,"resumeAllowed":true,"participants":[],"tracks":[],"controls":{"call":null,"focus":null,"floor":null,"recordings":[],"playbacks":[],"processors":[],"broadcasts":[],"handoffs":[],"mediaPlan":{"revision":0,"tracks":[]},"ai":{"inputId":"","inputState":"paused","activeRespIds":[]}}}})";
    VoiceMedia selected;
    selected.transport = VoiceTransport::None;
    AIControlSnapshot out;
    ASSERT_EQ(decode_ai_control_snapshot(WireRoute::Signaling, body, selected, out),
              WireResult::Ok);
    EXPECT_EQ(out.media_revision, 0U);
    EXPECT_TRUE(out.input_id.empty());
    EXPECT_FALSE(out.input_active);
    EXPECT_EQ(decode_ai_control_snapshot(
                  WireRoute::Signaling,
                  replaced(body, "\"inputState\":\"paused\"", "\"inputState\":\"active\""),
                  selected, out),
              WireResult::Invalid);
    EXPECT_EQ(decode_ai_control_snapshot(WireRoute::Signaling, body, recovery_media(), out),
              WireResult::Invalid);
}

TEST(AIControlWire, TerminalSnapshotOnlyClosesOriginalScopeAndHasNoActiveAiOrTracks) {
    const std::string body =
        R"({"msgToken":"sync","method":"sessionSnapshot","sessionId":"original-ai","eventSeq":9,"code":200,"data":{"state":"ended","reason":"expired","revision":8,"eventWatermark":9,"messageWatermark":7,"endpointEpoch":1,"capabilityRevision":1,"mediaRevision":1,"expiresAt":20000,"resumeAllowed":false,"participants":[],"tracks":[],"controls":{"call":null,"focus":null,"floor":null,"recordings":[],"playbacks":[],"processors":[],"broadcasts":[],"handoffs":[],"mediaPlan":{"revision":1,"tracks":[]},"ai":null}}})";
    AIControlSnapshot out;
    ASSERT_EQ(decode_ai_control_snapshot(WireRoute::Signaling, body, recovery_media(), out),
              WireResult::Ok);
    EXPECT_TRUE(out.ended);
    EXPECT_FALSE(out.resume_allowed);
    EXPECT_EQ(out.binding.endpoint_epoch, 1U);
    EXPECT_TRUE(out.input_id.empty());
    for (const auto& mutation : std::vector<std::pair<std::string, std::string>>{
             {"\"resumeAllowed\":false", "\"resumeAllowed\":true"},
             {"\"reason\":\"expired\",", ""},
             {"\"ai\":null",
              "\"ai\":{\"inputId\":\"old\",\"inputState\":\"paused\",\"activeRespIds\":[]}"},
             {"\"tracks\":[]", "\"tracks\":[{}]"},
             {"\"playbacks\":[]", "\"playbacks\":[{}]"}})
        EXPECT_EQ(decode_ai_control_snapshot(WireRoute::Signaling,
                                             replaced(body, mutation.first, mutation.second),
                                             recovery_media(), out),
                  WireResult::Invalid);
}
}  // namespace
}  // namespace ur::device
