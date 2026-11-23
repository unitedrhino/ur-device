// 文件说明：AI跨端邀请只解析原模型候选选择，不以邀请替代接听或真实媒体建立。
#include "ur/device/ai_endpoint_invite_wire.hpp"
#include <gtest/gtest.h>
#include <utility>
#include <vector>

namespace ur::device {
namespace {
// invite_body 使用公开占位媒体参数，不引用真实环境或供应商秘密。
constexpr std::string_view invite_body =
    R"({"msgToken":"ai-invite","method":"sessionInvite","sessionId":"candidate-ai-leg","eventSeq":1,"params":{"purpose":"aiConversation","admission":"accept","expiresAt":1900000000000,"handoff":{"sourceSessionId":"original-ai-session","handoffId":"handoff-ai-1"},"selected":{"transport":"udp","interactionMode":"fullDuplex","audioParams":{"format":"opus","sampleRate":16000,"channels":1,"frameDuration":20}},"tracks":[{"trackId":"mic","kind":"audio","sourceId":"mic","direction":"sendonly","codec":"opus","state":"requested"},{"trackId":"speaker","kind":"audio","sourceId":"speaker","direction":"recvonly","codec":"opus","state":"requested"}],"media":{"udp":{"server":"media.example.invalid","port":32000,"key":"00112233445566778899aabbccddeeff","nonce":"01000000000000000000000000000000"}}}})";

// invite_capability 表示真实16k双工AEC音频端口；本地接听是另一个显式参数。
VoiceCapability invite_capability() {
    VoiceCapability capability;
    capability.modes = {true, true, true};
    capability.audio = {16000, 1, 20};
    capability.ai_endpoint_handoff = true;
    return capability;
}
// changed 修改公开夹具的一个局部字段，保留其他字段避免靠多重错误凑拒绝。
std::string changed(std::string_view original, std::string_view replacement) {
    std::string body(invite_body);
    const auto position = body.find(original);
    EXPECT_NE(position, std::string::npos);
    if (position == std::string::npos)
        return "{}";
    body.replace(position, original.size(), replacement);
    return body;
}
}  // namespace

TEST(AIEndpointInviteWire, SelectedAudioIsHeldAndNotHumanCall) {
    AIEndpointInvite invite;
    EXPECT_EQ(decode_ai_endpoint_invite(WireRoute::Signaling, invite_body, invite_capability(),
                                        true, invite),
              WireResult::Ok);
    EXPECT_EQ(invite.source_session, "original-ai-session");
    EXPECT_EQ(invite.handoff_id, "handoff-ai-1");
    EXPECT_EQ(invite.envelope.session, "candidate-ai-leg");
    EXPECT_EQ(invite.media.mode, Mode::FullDuplex);
    EXPECT_EQ(invite.media.input_track, "mic");
    EXPECT_EQ(invite.media.output_track, "speaker");
}

TEST(AIEndpointInviteWire, StrictEnvelopeAndConditionalFields) {
    const std::vector<std::pair<std::string, std::string>> mutations = {
        {R"("purpose":"aiConversation")", R"("purpose":"humanCall")"},
        {R"("admission":"accept")", R"("admission":"direct")"},
        {R"("admission":"accept")", R"("admission":"accept","ready":true)"},
        {R"("admission":"accept")", R"("admission":"accept","admission":"accept")"},
        {R"("expiresAt":1900000000000)", R"("expiresAt":0)"},
        {R"("expiresAt":1900000000000)", R"("expiresAt":1.5)"},
        {R"("sourceSessionId":"original-ai-session")", R"("sourceSessionId":"candidate-ai-leg")"},
        {R"("handoffId":"handoff-ai-1")", R"("handoffId":null)"},
        {R"("interactionMode":"fullDuplex")",
         R"("interactionMode":"fullDuplex","pipeline":"realtime")"},
        {R"("channels":1)", R"("channels":2)"},
        {R"("channels":1)", R"("channels":1,"aecMode":"device")"},
        {R"("sourceId":"mic")", R"("sourceId":"camera")"},
        {R"("sourceId":"mic")", R"("sourceId":"mic","sourceType":"microphone")"},
        {R"("trackId":"speaker")", R"("trackId":"mic")"},
        {R"("server":"media.example.invalid")", R"("server":"https://media.example.invalid")"},
        {R"("server":"media.example.invalid")", R"("server":"0.0.0.0")"},
        {R"("port":32000)", R"("port":65536)"},
        {R"("key":"00112233445566778899aabbccddeeff")", R"("key":"00")"},
        {R"("nonce":"01000000000000000000000000000000")",
         R"("nonce":"01000000000000000000000000000080")"},
        {R"("method":"sessionInvite")", R"("method":"sessionCreated")"},
        {R"("eventSeq":1,)", ""},
        {R"("eventSeq":1)", R"("eventSeq":1,"code":200)"},
    };
    for (const auto& mutation : mutations) {
        AIEndpointInvite invite;
        invite.source_session = "must-clear";
        EXPECT_EQ(decode_ai_endpoint_invite(WireRoute::Signaling,
                                            changed(mutation.first, mutation.second),
                                            invite_capability(), true, invite),
                  WireResult::Invalid)
            << mutation.first;
        EXPECT_TRUE(invite.source_session.empty());
    }
    AIEndpointInvite invite;
    EXPECT_EQ(decode_ai_endpoint_invite(WireRoute::Message, invite_body, invite_capability(), true,
                                        invite),
              WireResult::WrongRoute);
}

TEST(AIEndpointInviteWire, CapabilityDoesNotGrantAcceptOrAEC) {
    AIEndpointInvite invite;
    auto capability = invite_capability();
    capability.ai_endpoint_handoff = false;
    EXPECT_EQ(
        decode_ai_endpoint_invite(WireRoute::Signaling, invite_body, capability, true, invite),
        WireResult::Invalid);
    capability = invite_capability();
    EXPECT_EQ(
        decode_ai_endpoint_invite(WireRoute::Signaling, invite_body, capability, false, invite),
        WireResult::Invalid);
    capability.modes = {true, false, false};
    EXPECT_EQ(
        decode_ai_endpoint_invite(WireRoute::Signaling, invite_body, capability, true, invite),
        WireResult::Invalid);
    capability = invite_capability();
    capability.audio.sample_rate = 24000;
    EXPECT_EQ(
        decode_ai_endpoint_invite(WireRoute::Signaling, invite_body, capability, true, invite),
        WireResult::Invalid);
    capability = invite_capability();
    capability.ai_vision = true;
    EXPECT_EQ(
        decode_ai_endpoint_invite(WireRoute::Signaling, invite_body, capability, true, invite),
        WireResult::Invalid);
}

TEST(AIEndpointInviteWire, LegacyMicrophoneSourceRemainsExplicit) {
    AIEndpointInvite invite;
    EXPECT_EQ(
        decode_ai_endpoint_invite(WireRoute::Signaling,
                                  changed(R"("sourceId":"mic")", R"("sourceId":"microphone")"),
                                  invite_capability(), true, invite),
        WireResult::Ok);
    EXPECT_EQ(invite.media.input_track, "mic");
    EXPECT_NE(invite.envelope.payload.find(R"("sourceId":"microphone")"), std::string::npos);
}

TEST(AIEndpointInviteWire, WebRTCUsesOriginalNegotiationWithoutUDP) {
    std::string body = changed(R"("transport":"udp")", R"("transport":"webrtc")");
    const auto media = body.find(R"(,"media":)");
    ASSERT_NE(media, std::string::npos);
    body.erase(media);
    body += "}}";
    const auto rate = body.find("16000");
    ASSERT_NE(rate, std::string::npos);
    body.replace(rate, 5, "48000");
    auto capability = invite_capability();
    capability.transport = VoiceTransport::WebRTC;
    capability.audio = {48000, 1, 20};
    AIEndpointInvite invite;
    EXPECT_EQ(decode_ai_endpoint_invite(WireRoute::Signaling, body, capability, true, invite),
              WireResult::Ok);
    EXPECT_EQ(invite.media.transport, VoiceTransport::WebRTC);
    EXPECT_TRUE(invite.media.host.empty());
    EXPECT_EQ(invite.media.port, 0);
}
}  // namespace ur::device
