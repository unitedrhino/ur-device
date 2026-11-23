// 逐方法验证云端下行，确保通用 JSON 通过不能绕过会话、轨及媒体参数契约。
#include "ur/device/voice_wire.hpp"
#include <gtest/gtest.h>
#include "ur/device/ipc_wire.hpp"

namespace ur::device {
namespace {
// AIEndpointHandoffIsExplicitAndDoesNotEnableSessionSync
// 独立音频接续能力必须显式携localAccept，不能把方法名当feature或给旧/None端补能力。
TEST(VoiceWire, AIEndpointHandoffIsExplicitAndDoesNotEnableSessionSync) {
    VoiceCapability capability;
    capability.audio = {16000, 1, 20};
    capability.modes = {true, true, true};
    Envelope command;
    ASSERT_EQ(capability_command("legacy", capability, command), WireResult::Ok);
    EXPECT_EQ(command.payload.find("aiEndpointHandoff"), std::string::npos);
    EXPECT_EQ(command.payload.find("localAccept"), std::string::npos);
    capability.ai_endpoint_handoff = true;
    ASSERT_EQ(capability_command("handoff", capability, command), WireResult::Ok);
    EXPECT_NE(command.payload.find(R"("localAccept")"), std::string::npos);
    EXPECT_NE(command.payload.find(R"("aiEndpointHandoff")"), std::string::npos);
    EXPECT_EQ(command.payload.find("sessionSync"), std::string::npos);
    capability.session_sync = true;
    ASSERT_EQ(capability_command("both", capability, command), WireResult::Ok);
    EXPECT_NE(command.payload.find(R"("sessionSync")"), std::string::npos);
    capability.transport = VoiceTransport::None;
    capability.text_input = true;
    EXPECT_EQ(capability_command("none", capability, command), WireResult::Invalid);
    EXPECT_TRUE(command.method.empty());
    capability.transport = VoiceTransport::WebRTC;
    capability.audio = {48000, 1, 20};
    capability.ai_vision = true;
    EXPECT_EQ(capability_command("vision", capability, command), WireResult::Invalid);
}
// HumanRemoteVideoRequiresExplicitHardwareCapability
// 人际接收声明独立于相机，并且不能绕过humanCall用途。
TEST(VoiceWire, HumanRemoteVideoRequiresExplicitHardwareCapability) {
  VoiceCapability capability;
  capability.audio = {48000, 1, 20};
  capability.modes = {true, false, false};
  capability.transport = VoiceTransport::WebRTC;
  capability.human_call = true;
  const VoiceVisionProfile profile{"front", "vision-1", 1, true};
  Envelope command;
  ASSERT_EQ(capability_command("legacy", capability, command, &profile),
            WireResult::Ok);
  EXPECT_EQ(command.payload.find("videoReceive"), std::string::npos);
  capability.human_video_receive = true;
  ASSERT_EQ(capability_command("receive", capability, command, &profile),
            WireResult::Ok);
  EXPECT_NE(command.payload.find("videoReceive"), std::string::npos);
  capability.human_call = false;
  EXPECT_EQ(capability_command("invalid", capability, command, &profile),
            WireResult::Invalid);
}
// CancelAckWithoutStateMatchesExistingAudioBackend
// 原UDP后端只确认原respId，不能被误作无效报文关闭会话。
TEST(VoiceWire, CancelAckWithoutStateMatchesExistingAudioBackend) {
    VoiceEvent event;
    EXPECT_EQ(
        decode_voice_event(
            WireRoute::Signaling,
            R"({"msgToken":"cancel","method":"requestAck","sessionId":"session","code":200,"data":{"requestMethod":"respCancel","result":{"respId":"pending"}}})",
            event),
        WireResult::Ok);
    EXPECT_EQ(event.response_id, "pending");
    EXPECT_TRUE(event.state.empty());
    for (const auto* state : {"null", "0", "false", "\"\"", "\"pending\"", "{}"}) {
        const std::string body =
            R"({"msgToken":"cancel","method":"requestAck","sessionId":"session","code":200,"data":{"requestMethod":"respCancel","result":{"respId":"pending","state":)" +
            std::string(state) + "}}}";
        EXPECT_EQ(decode_voice_event(WireRoute::Signaling, body, event), WireResult::Invalid);
    }
}
// MediaOfferPreservesExplicitRevision 验证原会话重协商的实际编码与下行互解，
// 并保留旧入口第一代语义；该报文测试不代替真实Peer恢复验收。
TEST(VoiceWire, MediaOfferPreservesExplicitRevision) {
    Envelope request;
    const std::string sdp = "v=0\r\no=offer\r\n";
    ASSERT_EQ(media_offer_command("initial", "session", sdp, request), WireResult::Ok);
    EXPECT_EQ(
        request.payload,
        R"({"mediaRevision":1,"webrtc":{"iceMode":"complete","sdp":"v=0\r\no=offer\r\n"}})");
    for (const auto revision : {2ULL, 9007199254740991ULL}) {
        ASSERT_EQ(media_offer_command("recover", "session", sdp, revision, request),
                  WireResult::Ok);
        EXPECT_EQ(request.session, "session");
        EXPECT_EQ(request.method, "mediaOffer");
        VoiceEvent answer;
        // JSON数字允许科学记数；直接互解真实编码字段，而不是要求最大整数具有特定打印形式。
        const auto body =
            std::string(
                R"({"msgToken":"recover","method":"mediaAnswer","sessionId":"session","code":200,"data":)") +
            request.payload + "}";
        ASSERT_EQ(decode_voice_event(WireRoute::Signaling, body, answer), WireResult::Ok);
        EXPECT_EQ(answer.revision, revision);
        EXPECT_EQ(answer.answer_sdp, sdp);
    }
    for (const auto revision : {0ULL, 9007199254740992ULL, 18446744073709551615ULL}) {
        request.method = "must-clear";
        EXPECT_EQ(media_offer_command("recover", "session", sdp, revision, request),
                  WireResult::Invalid);
        EXPECT_TRUE(request.method.empty());
    }
}
// MediaStatePreservesExactRevision 保证连接回执与Offer使用同一个数值，不把舍入代次当合法关联。
TEST(VoiceWire, MediaStatePreservesExactRevision) {
    Envelope request;
    for (const auto revision : {1ULL, 2ULL, 9007199254740991ULL}) {
        ASSERT_EQ(voice_control("ready", "session", "mediaState", "connected", revision, request, 0,
                                VoiceTransport::WebRTC),
                  WireResult::Ok);
        const auto body =
            std::string(
                R"({"msgToken":"ready","method":"requestAck","sessionId":"session","code":200,"data":{"requestMethod":"mediaState","result":)") +
            request.payload + "}}";
        VoiceEvent answer;
        ASSERT_EQ(decode_voice_event(WireRoute::Signaling, body, answer), WireResult::Ok);
        EXPECT_EQ(answer.revision, revision);
        EXPECT_EQ(answer.state, "connected");
    }
    for (const auto revision : {0ULL, 9007199254740992ULL, 18446744073709551615ULL}) {
        request.method = "must-clear";
        EXPECT_EQ(voice_control("ready", "session", "mediaState", "connected", revision, request, 0,
                                VoiceTransport::WebRTC),
                  WireResult::Invalid);
        EXPECT_TRUE(request.method.empty());
    }
}
// TypedNoneSelectionUsesExistingContract 真实新Owner可选择已声明图片/文件，不能强制降成纯text。
TEST(VoiceWire, TypedNoneSelectionUsesExistingContract) {
    VoiceEvent event;
    EXPECT_EQ(
        decode_voice_event(
            WireRoute::Signaling,
            R"({"msgToken":"typed-create","method":"sessionCreated","sessionId":"typed-session","code":200,"data":{"state":"negotiating","selected":{"transport":"none","inputModalities":["image","file"],"outputModalities":["text"]},"tracks":[]}})",
            event),
        WireResult::Ok);
    EXPECT_EQ(event.media.transport, VoiceTransport::None);
    EXPECT_TRUE(event.media.text_output);
    EXPECT_FALSE(event.media.audio_output);
    EXPECT_TRUE(event.media.image_input);
    EXPECT_TRUE(event.media.file_input);
}

// TypedCapabilityDoesNotGrantUnspecifiedInputs
// 验证capability/create两正式命令与none/UDP实际离散输入声明。
TEST(VoiceWire, TypedCapabilityDoesNotGrantUnspecifiedInputs) {
    VoiceCapability capability;
    capability.transport = VoiceTransport::None;
    capability.image_input = true;
    Envelope request;
    ASSERT_EQ(capability_command("cap", capability, request), WireResult::Ok);
    EXPECT_NE(request.payload.find(R"("inputModalities":["image"])"), std::string::npos);
    EXPECT_NE(request.payload.find(R"("outputModalities":["text"])"), std::string::npos);
    EXPECT_EQ(request.payload.find("audio"), std::string::npos);
    ASSERT_EQ(create_voice_command("create", capability, request), WireResult::Ok);
    EXPECT_NE(request.payload.find(R"("inputModalities":["image"])"), std::string::npos);
    capability.file_input = capability.text_input = true;
    ASSERT_EQ(create_voice_command("create-all", capability, request), WireResult::Ok);
    EXPECT_NE(request.payload.find(R"("inputModalities":["text","image","file"])"),
              std::string::npos);
    capability.transport = VoiceTransport::Udp;
    capability.modes.half_duplex = true;
    capability.audio = {16000, 1, 20};
    ASSERT_EQ(create_voice_command("voice", capability, request), WireResult::Ok);
    EXPECT_NE(request.payload.find(R"("inputModalities":["text","image","file"])"),
              std::string::npos);
    EXPECT_EQ(request.payload.find(R"("inputModalities":["audio")"), std::string::npos);
}

// TypedSelectionRejectsDuplicatesAndUnsupported
// 保持无声明旧语音不自动授附件，非法平台选择清空输出。
TEST(VoiceWire, TypedSelectionRejectsDuplicatesAndUnsupported) {
    for (const auto* inputs : {R"([])", R"(["image","image"])", R"(["file","audio"])",
                               R"(["video"])", R"([null])", R"(["text","image","file","text"])"}) {
        const std::string body =
            std::string(
                R"({"msgToken":"create","method":"sessionCreated","sessionId":"s","code":200,"data":{"state":"negotiating","selected":{"transport":"none","inputModalities":)") +
            inputs + R"(,"outputModalities":["text"]},"tracks":[]}})";
        VoiceEvent event;
        event.media.image_input = true;
        EXPECT_EQ(decode_voice_event(WireRoute::Signaling, body, event), WireResult::Invalid);
        EXPECT_FALSE(event.media.image_input);
        EXPECT_FALSE(event.media.file_input);
    }
}
// NoneSessionCreatedUsesNoAudioFields
// 纯文字会话不分配媒体；revision=0是明确契约，不可用伪音频握手绕过。
TEST(VoiceWire, NoneSessionCreatedUsesNoAudioFields) {
    VoiceEvent event;
    EXPECT_EQ(
        decode_voice_event(
            WireRoute::Signaling,
            R"({"msgToken":"text-create","method":"sessionCreated","sessionId":"text-session","code":200,"data":{"state":"negotiating","selected":{"transport":"none","inputModalities":["text"],"outputModalities":["text"]},"tracks":[]}})",
            event),
        WireResult::Ok);
    EXPECT_TRUE(event.media.input_track.empty());
    EXPECT_TRUE(event.media.output_track.empty());
    EXPECT_EQ(
        decode_voice_event(
            WireRoute::Signaling,
            R"({"msgToken":"text-ready","method":"sessionEstablished","sessionId":"text-session","eventSeq":1,"data":{"state":"established","mediaRevision":0}})",
            event),
        WireResult::Ok);
    EXPECT_EQ(event.revision, 0U);
}
// ipc_invitation 使用不含真实地址或凭据的独立预览邀请。
const std::string ipc_invitation =
    R"({"msgToken":"ipc","method":"sessionInvite","sessionId":"preview-1","eventSeq":1,"params":{"purpose":"ipcLiveView","admission":"direct","cameraId":"front","transport":"webrtc","expiresAt":1900000000000,"tracks":[{"trackId":"video-1","kind":"video","sourceId":"front","direction":"sendonly","codec":"H264","state":"requested"}]}})";
// created 为公开合成响应，不包含真实设备凭据或可连接地址。
const std::string created =
    R"({"msgToken":"t","method":"sessionCreated","sessionId":"s","code":200,"data":{"state":"negotiating","selected":{"transport":"udp","interactionMode":"fullDuplex","audioParams":{"format":"opus","sampleRate":16000,"channels":1,"frameDuration":20}},"tracks":[{"trackId":"mic-1","kind":"audio","sourceId":"mic","direction":"sendonly","codec":"opus","state":"requested"},{"trackId":"speaker-1","kind":"audio","sourceId":"speaker","direction":"recvonly","codec":"opus","state":"requested"}],"media":{"udp":{"server":"127.0.0.1","port":12345,"key":"000102030405060708090a0b0c0d0e0f","nonce":"01000000112233445566778800000000"}}}})";
// replace_first 仅修改指定语义字段，用于非法配置反例。
std::string replace_first(std::string value, const std::string& from, const std::string& to) {
    const auto position = value.find(from);
    EXPECT_NE(position, std::string::npos);
    if (position != std::string::npos)
        value.replace(position, from.size(), to);
    return value;
}
TEST(IPCWire, PureVideoCapabilityDoesNotClaimMicrophoneOrAI) {
    IPCCapability capability;
    Envelope command;
    ASSERT_EQ(ipc_capability_command("cap", capability, command), WireResult::Ok);
    EXPECT_EQ(command.method, "capabilityReport");
    EXPECT_EQ(command.payload.find("\"audio\""), std::string::npos);
    EXPECT_EQ(command.payload.find("aecMode"), std::string::npos);
    EXPECT_NE(command.payload.find("ipcLiveView"), std::string::npos);
    capability.microphone = true;
    capability.local_accept = true;
    ASSERT_EQ(ipc_capability_command("cap", capability, command), WireResult::Ok);
    EXPECT_NE(command.payload.find("\"audio\""), std::string::npos);
    EXPECT_NE(command.payload.find("localAccept"), std::string::npos);
    capability.max_publishers = 5;
    EXPECT_EQ(ipc_capability_command("cap", capability, command), WireResult::Invalid);
    EXPECT_TRUE(command.token.empty());
    capability.max_publishers = 1;
    capability.camera_id = "\n";
    EXPECT_EQ(ipc_capability_command("cap", capability, command), WireResult::Invalid);
}
TEST(IPCWire, PreviewRequiresExplicitPurposeCameraAndAdmission) {
    IPCCapability capability;
    IPCInvite invitation;
    ASSERT_EQ(decode_ipc_invite(WireRoute::Signaling, ipc_invitation, capability, invitation),
              WireResult::Ok);
    EXPECT_EQ(invitation.envelope.session, "preview-1");
    EXPECT_EQ(invitation.camera_id, "front");
    EXPECT_EQ(invitation.video_track, "video-1");
    EXPECT_TRUE(invitation.audio_track.empty());
    EXPECT_FALSE(invitation.needs_accept);
    const auto confirmed = replace_first(ipc_invitation, "direct", "accept");
    EXPECT_EQ(decode_ipc_invite(WireRoute::Signaling, confirmed, capability, invitation),
              WireResult::Invalid);
    EXPECT_TRUE(invitation.envelope.session.empty());
    capability.local_accept = true;
    ASSERT_EQ(decode_ipc_invite(WireRoute::Signaling, confirmed, capability, invitation),
              WireResult::Ok);
    EXPECT_TRUE(invitation.needs_accept);
    EXPECT_EQ(decode_ipc_invite(WireRoute::Message, ipc_invitation, capability, invitation),
              WireResult::WrongRoute);
    EXPECT_TRUE(invitation.envelope.session.empty());
}
TEST(IPCWire, MalformedAndCrossPurposeInvitationsAreRejected) {
    IPCCapability capability;
    IPCInvite invitation;
    for (const auto& invalid :
         {replace_first(ipc_invitation, "ipcLiveView", "humanCall"),
          replace_first(ipc_invitation, "sendonly", "recvonly"),
          replace_first(ipc_invitation, "requested", "active"),
          replace_first(ipc_invitation, "H264", "VP8"),
          replace_first(ipc_invitation, "webrtc", "udp"),
          replace_first(ipc_invitation, "1900000000000", "0"),
          replace_first(ipc_invitation, "1900000000000", "1.5"),
          replace_first(ipc_invitation, "\"eventSeq\":1,", ""),
          replace_first(ipc_invitation, "\"purpose\":", "\"callGroupId\":\"call\",\"purpose\":"),
          replace_first(ipc_invitation, "\"cameraId\":", "\"cameraId\":\"front\",\"cameraId\":"),
          replace_first(ipc_invitation, "\"sourceId\":\"front\"", "\"sourceId\":\"back\"")}) {
        SCOPED_TRACE(invalid);
        EXPECT_EQ(decode_ipc_invite(WireRoute::Signaling, invalid, capability, invitation),
                  WireResult::Invalid);
        EXPECT_TRUE(invitation.video_track.empty());
    }
}
// DownstreamTrackControl
// 保留原token并限定邀请轨，六种语法不等于源拥有订阅权限。
TEST(IPCWire, DownstreamTrackControl) {
    IPCCapability capability;
    IPCInvite invite;
    ASSERT_EQ(decode_ipc_invite(WireRoute::Signaling, ipc_invitation, capability, invite),
              WireResult::Ok);
    for (const auto* operation :
         {"publish", "pause", "resume", "stop", "subscribe", "unsubscribe"}) {
        const std::string body =
            std::string(
                R"({"msgToken":"control","method":"trackControl","sessionId":"preview-1","eventSeq":2,"params":{"trackId":"video-1","operation":")") +
            operation + R"(","mediaRevision":1,"reason":"userRequested"}})";
        IPCTrackCommand command;
        ASSERT_EQ(decode_ipc_track(WireRoute::Signaling, body, invite, command), WireResult::Ok);
        EXPECT_EQ(command.operation, operation);
        EXPECT_EQ(command.track_id, "video-1");
        EXPECT_EQ(command.envelope.token, "control");
        EXPECT_EQ(command.revision, 1U);
        EXPECT_EQ(command.reason, "userRequested");
    }
}

// TrackStateKeepsExactRecoveryRevision 验证重协商代次在JSON安全整数上界仍能精确互解。
TEST(IPCWire, TrackStateKeepsExactRecoveryRevision) {
    IPCCapability capability;
    IPCInvite invite;
    ASSERT_EQ(decode_ipc_invite(WireRoute::Signaling, ipc_invitation, capability, invite),
              WireResult::Ok);
    Envelope report;
    ASSERT_EQ(track_state_report_command("report", "preview-1", "video-1", "paused", invite,
                                        report, {}, 0, {}, 9007199254740991ULL),
              WireResult::Ok);
    EXPECT_NE(report.payload.find("\"mediaRevision\":9007199254740991"), std::string::npos);
    EXPECT_EQ(track_state_report_command("report", "preview-1", "video-1", "paused", invite,
                                        report, {}, 0, {}, 9007199254740992ULL),
              WireResult::Invalid);
    EXPECT_EQ(report.payload, "{}");
}

// TrackControlRejectsForeignAndMalformedInput
// 覆盖跨会话、未授权音轨、旧代次及非法可选值。
TEST(IPCWire, TrackControlRejectsForeignAndMalformedInput) {
    IPCCapability capability;
    IPCInvite invite;
    ASSERT_EQ(decode_ipc_invite(WireRoute::Signaling, ipc_invitation, capability, invite),
              WireResult::Ok);
    const std::string valid =
        R"({"msgToken":"control","method":"trackControl","sessionId":"preview-1","eventSeq":2,"params":{"trackId":"video-1","operation":"pause","mediaRevision":1}})";
    IPCTrackCommand command;
    ASSERT_EQ(decode_ipc_track(WireRoute::Signaling, valid, invite, command), WireResult::Ok);
    EXPECT_EQ(decode_ipc_track(WireRoute::Message, valid, invite, command), WireResult::WrongRoute);
    EXPECT_TRUE(command.envelope.token.empty());
    for (const auto& invalid :
         {replace_first(valid, "preview-1", "other"), replace_first(valid, "video-1", "audio-1"),
          replace_first(valid, "pause", "mute"),
          replace_first(valid, "\"mediaRevision\":1", "\"mediaRevision\":2"),
          replace_first(valid, "\"mediaRevision\":1", "\"mediaRevision\":null"),
          replace_first(valid, "\"eventSeq\":2,", ""),
          replace_first(valid, "\"mediaRevision\":1", "\"mediaRevision\":1,\"reason\":null"),
          replace_first(valid, "\"mediaRevision\":1", "\"mediaRevision\":1,\"reason\":\"\""),
          replace_first(valid, "\"mediaRevision\":1", "\"mediaRevision\":1,\"owner\":\"fake\"")}) {
        ASSERT_EQ(decode_ipc_track(WireRoute::Signaling, invalid, invite, command),
                  WireResult::Invalid);
        EXPECT_TRUE(command.track_id.empty());
        EXPECT_TRUE(command.envelope.token.empty());
    }
}

TEST(IPCWire, ListeningAudioDoesNotEnableTalkback) {
    IPCCapability capability;
    IPCInvite invitation;
    const auto audio = replace_first(
        ipc_invitation, "\"tracks\":[",
        R"("tracks":[{"trackId":"mic-1","kind":"audio","sourceId":"mic","direction":"sendonly","codec":"opus","state":"requested"},)");
    EXPECT_EQ(decode_ipc_invite(WireRoute::Signaling, audio, capability, invitation),
              WireResult::Invalid);
    capability.microphone = true;
    ASSERT_EQ(decode_ipc_invite(WireRoute::Signaling, audio, capability, invitation),
              WireResult::Ok);
    EXPECT_EQ(invitation.audio_track, "mic-1");
    EXPECT_EQ(
        decode_ipc_invite(WireRoute::Signaling,
                          replace_first(audio, "\"trackId\":\"mic-1\"", "\"trackId\":\"video-1\""),
                          capability, invitation),
        WireResult::Invalid);
}
TEST(VoiceWire, GeneratesOnlyActualCapabilitiesAndNewTopicCommands) {
    VoiceCapability capability;
    Envelope request;
    ASSERT_EQ(capability_command("cap", capability, request), WireResult::Ok);
    EXPECT_EQ(request.method, "capabilityReport");
    EXPECT_TRUE(request.session.empty());
    EXPECT_NE(request.payload.find("halfDuplex"), std::string::npos);
    EXPECT_EQ(request.payload.find("fullDuplex"), std::string::npos);
    ASSERT_EQ(create_voice_command("create", capability, request), WireResult::Ok);
    EXPECT_EQ(request.method, "sessionCreate");
    EXPECT_NE(request.payload.find("speaker-1"), std::string::npos);
    capability.modes = {true, true, true};
    ASSERT_EQ(capability_command("cap", capability, request), WireResult::Ok);
    EXPECT_NE(request.payload.find("fullDuplex"), std::string::npos);
    capability.modes.device_aec = false;
    EXPECT_EQ(capability_command("cap", capability, request), WireResult::Invalid);
    EXPECT_EQ(create_voice_command("create", capability, request), WireResult::Invalid);
    EXPECT_TRUE(request.token.empty());
    capability.modes = {false, false, false};
    EXPECT_EQ(capability_command("cap", capability, request), WireResult::Invalid);
    capability.modes = {};
    capability.audio = {44100, 1, 20};
    EXPECT_EQ(capability_command("cap", capability, request), WireResult::Invalid);
    capability.audio = {16000, 1, 20};
    capability.revision = 0;
    EXPECT_EQ(capability_command("cap", capability, request), WireResult::Invalid);
    capability.revision = 1;
    EXPECT_EQ(capability_command("", capability, request), WireResult::Invalid);
}

// AI与文件一次性合并声明，文件输出不改变AI建会话、AEC或媒体格式，缺省报文保持原样。
TEST(VoiceWire, FileOutputsMergeWithoutExpandingAIRequests) {
    for (bool text : {false, true}) {
        for (const auto transport : {VoiceTransport::Udp, VoiceTransport::WebRTC}) {
            VoiceCapability capability;
            capability.text_input = text;
            capability.transport = transport;
            if (transport == VoiceTransport::WebRTC)
                capability.audio = {48000, 1, 20};
            Envelope original;
            ASSERT_EQ(create_voice_command("create", capability, original), WireResult::Ok);
            for (unsigned mask = 0; mask < 4; ++mask) {
                capability.file_audio_output = (mask & 1) != 0;
                capability.file_video_output = (mask & 2) != 0;
                Envelope report, create;
                ASSERT_EQ(capability_command("cap", capability, report), WireResult::Ok);
                EXPECT_EQ(report.payload.find("mediaPlayback") != std::string::npos, mask != 0);
                const std::string media = transport == VoiceTransport::Udp ? "udp" : "webrtc";
                const auto transports =
                    "\"transports\":[\"" + media + (mask == 0 ? "\"]" : "\",\"none\"]");
                EXPECT_NE(report.payload.find(transports), std::string::npos);
                EXPECT_EQ(report.payload.find("\"video\"") != std::string::npos, (mask & 2) != 0);
                ASSERT_EQ(create_voice_command("create", capability, create), WireResult::Ok);
                EXPECT_EQ(create.payload, original.payload);
                EXPECT_EQ(create.payload.find("mediaPlayback"), std::string::npos);
                EXPECT_EQ(create.payload.find("\"video\""), std::string::npos);
            }
        }
    }
}
// LiveReceiveUsesIndependentFormats
// 验证一次完整快照，不把节目H264变成相机或离散视频权限。
TEST(VoiceWire, LiveReceiveUsesIndependentFormats) {
    VoiceCapability capability{{true, true, true}, {16000, 1, 20}, 1, true};
    Envelope original, live, created;
    ASSERT_EQ(capability_command("cap", capability, original), WireResult::Ok);
    EXPECT_EQ(original.payload.find("liveReceive"), std::string::npos);
    capability.live_audio_receive = capability.live_video_receive = true;
    ASSERT_EQ(capability_command("cap", capability, live), WireResult::Ok);
    EXPECT_NE(live.payload.find("liveBroadcast"), std::string::npos);
    EXPECT_NE(live.payload.find("\"transports\":[\"udp\",\"webrtc\"]"), std::string::npos);
    EXPECT_NE(live.payload.find("\"sampleRate\":48000"), std::string::npos);
    EXPECT_NE(live.payload.find("\"sampleRate\":16000"), std::string::npos);
    EXPECT_NE(live.payload.find("\"videoCodecs\":[\"H264\"]"), std::string::npos);
    EXPECT_EQ(live.payload.find("cameras"), std::string::npos);
    EXPECT_EQ(live.payload.find("mediaPlayback"), std::string::npos);
    EXPECT_EQ(live.payload.find("\"video\""), std::string::npos);
    ASSERT_EQ(create_voice_command("create", capability, created), WireResult::Ok);
    EXPECT_EQ(created.payload.find("liveReceive"), std::string::npos);
    EXPECT_EQ(created.payload.find("liveBroadcast"), std::string::npos);
}
// LiveReceiveAllowsIndependentTrackSubset
// 音视频接收声明独立，不为缺失轨伪造格式或重复传输。
TEST(VoiceWire, LiveReceiveAllowsIndependentTrackSubset) {
    VoiceCapability capability{
        {true, false, false}, {48000, 1, 20}, 1, false, VoiceTransport::WebRTC};
    Envelope command;
    capability.live_video_receive = true;
    ASSERT_EQ(capability_command("cap", capability, command), WireResult::Ok);
    EXPECT_NE(command.payload.find("\"liveReceive\":{\"videoCodecs\":[\"H264\"]}"),
              std::string::npos);
    EXPECT_NE(command.payload.find("\"transports\":[\"webrtc\"]"), std::string::npos);
    capability.live_video_receive = false;
    capability.live_audio_receive = true;
    ASSERT_EQ(capability_command("cap", capability, command), WireResult::Ok);
    EXPECT_NE(command.payload.find("liveReceive"), std::string::npos);
    EXPECT_EQ(command.payload.find("videoCodecs"), std::string::npos);
}
TEST(VoiceWire, ControlRequestsRequireExplicitObjectAndSession) {
    Envelope request;
    for (const auto* method : {"audioStart", "audioStop", "respCancel"}) {
        EXPECT_EQ(voice_control("t", "s", method, "id", 0, request,
                                std::string(method) == "audioStart" ? 128U : 0U),
                  WireResult::Ok);
        EXPECT_EQ(request.session, "s");
        EXPECT_EQ(voice_control("t", "s", method, "", 0, request), WireResult::Invalid);
        EXPECT_EQ(voice_control("t", "s", method, std::string(129, 'x'), 0, request),
                  WireResult::Invalid);
    }
    EXPECT_EQ(voice_control("t", "", "audioStart", "i", 0, request), WireResult::Invalid);
    EXPECT_EQ(voice_control("t", "s", "sessionEnd", "userStopped", 0, request), WireResult::Ok);
    EXPECT_EQ(voice_control("t", "s", "sessionEnd", "userHangup", 0, request), WireResult::Ok);
    EXPECT_EQ(voice_control("t", "s", "sessionEnd", "other", 0, request), WireResult::Unsupported);
    EXPECT_EQ(voice_control("t", "s", "audioClear", "i", 0, request), WireResult::Unsupported);
    EXPECT_EQ(voice_control("t", "s", "mediaState", "connected", 1, request), WireResult::Ok);
    EXPECT_EQ(voice_control("t", "s", "mediaState", "connected", 0, request), WireResult::Invalid);
    EXPECT_EQ(voice_control("t", "s", "mediaState", "connected", 9007199254740992ULL, request),
              WireResult::Invalid);
    EXPECT_EQ(voice_control("t", "s", "mediaState", "other", 1, request), WireResult::Invalid);
}
TEST(VoiceWire, TextInputEncodingAndRouteAreExplicit) {
    Envelope request;
    ASSERT_EQ(text_input_command("t", "s", "typed", "你好\n世界", true, request), WireResult::Ok);
    EXPECT_EQ(request.method, "inputSend");
    EXPECT_NE(request.payload.find("\"outputModalities\":[\"text\",\"audio\"]"), std::string::npos);
    for (const std::string& text : {std::string{}, std::string(" \t\n"), std::string("a\0b", 3),
                                    std::string(60001, 'a'), std::string("\xff")})
        EXPECT_EQ(text_input_command("t", "s", "typed", text, false, request), WireResult::Invalid);
    EXPECT_EQ(text_input_command("t", "", "typed", "hello", false, request), WireResult::Invalid);
    EXPECT_EQ(text_input_command("t", "s", "\n", "hello", false, request), WireResult::Invalid);
    VoiceEvent event;
    const std::string ack =
        R"({"msgToken":"t","sessionId":"s","method":"requestAck","code":200,"data":{"requestMethod":"inputSend","result":{"inputId":"typed","state":"accepted"}}})";
    EXPECT_EQ(decode_voice_event(WireRoute::Message, ack, event), WireResult::Ok);
    EXPECT_EQ(event.input_id, "typed");
    EXPECT_EQ(decode_voice_event(WireRoute::Signaling, ack, event), WireResult::WrongRoute);
    EXPECT_EQ(
        decode_voice_event(WireRoute::Message, replace_first(ack, "accepted", "completed"), event),
        WireResult::Invalid);
}
TEST(VoiceWire, ContentNegotiationRejectsInventedAndDuplicateModes) {
    VoiceCapability capability{{true, false, false}, {16000, 1, 20}, 1, true};
    Envelope request;
    ASSERT_EQ(capability_command("t", capability, request), WireResult::Ok);
    EXPECT_NE(request.payload.find("inputModalities"), std::string::npos);
    ASSERT_EQ(create_voice_command("t", capability, request), WireResult::Ok);
    EXPECT_NE(request.payload.find("inputModalities"), std::string::npos);
    VoiceEvent event;
    const auto negotiated = replace_first(
        created, "\"audioParams\"",
        R"("inputModalities":["text"],"outputModalities":["text","audio"],"audioParams")");
    ASSERT_EQ(decode_voice_event(WireRoute::Signaling, negotiated, event), WireResult::Ok);
    EXPECT_TRUE(event.media.text_input && event.media.text_output && event.media.audio_output);
    EXPECT_EQ(decode_voice_event(
                  WireRoute::Signaling,
                  replace_first(negotiated, "[\"text\",\"audio\"]", "[\"text\",\"text\"]"), event),
              WireResult::Invalid);
    EXPECT_EQ(decode_voice_event(WireRoute::Signaling,
                                 replace_first(negotiated, "[\"text\"]", "[\"video\"]"), event),
              WireResult::Invalid);
}
TEST(VoiceWire, CreatedValidatesBothTracksAndExactMediaCredentials) {
    VoiceEvent event;
    ASSERT_EQ(decode_voice_event(WireRoute::Signaling, created, event), WireResult::Ok);
    EXPECT_EQ(event.media.mode, Mode::FullDuplex);
    EXPECT_EQ(event.media.input_track, "mic-1");
    EXPECT_EQ(event.media.output_track, "speaker-1");
    EXPECT_EQ(event.media.port, 12345);
    EXPECT_EQ(event.media.key[15], 15);
    EXPECT_EQ(event.media.nonce[0], 0x11);
    EXPECT_EQ(event.media.nonce[7], 0x88);
    EXPECT_EQ(decode_voice_event(WireRoute::Message, created, event), WireResult::WrongRoute);
    EXPECT_TRUE(event.media.host.empty());
    const std::pair<std::string, std::string> mutations[] = {
        {"fullDuplex", "auto"},
        {"\"udp\"", "\"webrtc\""},
        {"negotiating", "established"},
        {"16000", "44100"},
        {"\"channels\":1", "\"channels\":2"},
        {"\"frameDuration\":20", "\"frameDuration\":10"},
        {"12345", "0"},
        {"12345", "65536"},
        {"12345", "1.5"},
        {"000102030405060708090a0b0c0d0e0f", "00"},
        {"000102030405060708090a0b0c0d0e0f", "Z00102030405060708090a0b0c0d0e0f"},
        {"01000000112233445566778800000000", "02000000112233445566778800000000"},
        {"speaker-1", "mic-1"},
        {"recvonly", "sendonly"},
        {"recvonly", "sendrecv"},
        {"\"kind\":\"audio\"", "\"kind\":\"video\""},
        {"\"sourceId\":\"mic\"", "\"sourceId\":\"\""},
        {"\"state\":\"requested\"", "\"state\":\"active\""},
        {"\"sessionId\":\"s\",", ""},
    };
    for (const auto& mutation : mutations) {
        EXPECT_EQ(
            decode_voice_event(WireRoute::Signaling,
                               replace_first(created, mutation.first, mutation.second), event),
            WireResult::Invalid)
            << mutation.first;
        EXPECT_TRUE(event.media.host.empty());
    }
    EXPECT_EQ(decode_voice_event(WireRoute::Signaling,
                                 replace_first(created, "fullDuplex", "halfDuplex"), event),
              WireResult::Ok);
}
TEST(VoiceWire, UdpInputAndReplyBoundariesAreRequiredAndBounded) {
    Envelope request;
    for (const auto sequence : {0U, 1U, 127U, 129U, 4294967295U})
        EXPECT_EQ(voice_control("t", "s", "audioStart", "i", 1, request, sequence),
                  WireResult::Invalid);
    EXPECT_EQ(voice_control("t", "s", "audioStop", "i", 1, request, 128), WireResult::Ok);
    EXPECT_NE(request.payload.find("\"lastPacketSeq\":128"), std::string::npos);
    EXPECT_EQ(voice_control("t", "s", "audioStop", "i", 1, request, 129), WireResult::Invalid);
    EXPECT_EQ(voice_control("t", "s", "audioStart", "i", 1, request, 4294967168U), WireResult::Ok);
    VoiceEvent event;
    const std::string start =
        R"({"msgToken":"e","method":"respAudioStart","sessionId":"s","eventSeq":1,"data":{"respId":"r","trackId":"speaker-1","mediaRevision":1,"udpNonce":"0011223344556677","firstPacketSeq":128}})";
    EXPECT_EQ(decode_voice_event(WireRoute::Signaling, start, event), WireResult::Ok);
    EXPECT_EQ(event.first_packet_sequence, 128U);
    for (const auto* nonce : {"", "00", "001122334455667788", "gg11223344556677"})
        EXPECT_EQ(decode_voice_event(WireRoute::Signaling,
                                     replace_first(start, "0011223344556677", nonce), event),
                  WireResult::Invalid);
    EXPECT_EQ(
        decode_voice_event(WireRoute::Signaling,
                           replace_first(start, ",\"udpNonce\":\"0011223344556677\"", ""), event),
        WireResult::Invalid);
    for (const auto* number :
         {"0", "1", "127", "129", "-128", "128.5", "4294967296", "null", "\"128\""})
        EXPECT_EQ(decode_voice_event(WireRoute::Signaling,
                                     replace_first(start, "\"firstPacketSeq\":128",
                                                   std::string("\"firstPacketSeq\":") + number),
                                     event),
                  WireResult::Invalid);
    EXPECT_EQ(decode_voice_event(WireRoute::Signaling,
                                 replace_first(start, ",\"firstPacketSeq\":128", ""), event),
              WireResult::Invalid);
    const std::string done =
        R"({"msgToken":"e","method":"respAudioDone","sessionId":"s","eventSeq":2,"data":{"respId":"r","trackId":"speaker-1","mediaRevision":1,"lastPacketSeq":128}})";
    for (const auto* number : {"0", "256", "4294967168"})
        EXPECT_EQ(decode_voice_event(WireRoute::Signaling,
                                     replace_first(done, "\"lastPacketSeq\":128",
                                                   std::string("\"lastPacketSeq\":") + number),
                                     event),
                  WireResult::Ok);
    for (const auto* number : {"1", "-128", "128.5", "4294967296", "null", "\"128\""})
        EXPECT_EQ(decode_voice_event(WireRoute::Signaling,
                                     replace_first(done, "\"lastPacketSeq\":128",
                                                   std::string("\"lastPacketSeq\":") + number),
                                     event),
                  WireResult::Invalid);
}
// WebRTC不携带UDP密钥/序号，SDP只从本机Peer生成并在完整候选后交换。
TEST(VoiceWire, WebRTCCommandsAndEventsDoNotInventUDPBoundaries) {
    VoiceCapability capability{
        {true, true, true}, {48000, 1, 20}, 1, false, VoiceTransport::WebRTC};
    Envelope request;
    ASSERT_EQ(capability_command("cap", capability, request), WireResult::Ok);
    EXPECT_NE(request.payload.find(R"("transports":["webrtc"])"), std::string::npos);
    ASSERT_EQ(create_voice_command("create", capability, request), WireResult::Ok);
    EXPECT_NE(request.payload.find(R"("transport":"webrtc")"), std::string::npos);
    capability.audio.sample_rate = 16000;
    EXPECT_EQ(create_voice_command("invalid", capability, request), WireResult::Invalid);
    const std::string created_web =
        R"({"msgToken":"t","method":"sessionCreated","sessionId":"s","code":200,"data":{"state":"negotiating","selected":{"transport":"webrtc","interactionMode":"fullDuplex","audioParams":{"format":"opus","sampleRate":48000,"channels":1,"frameDuration":20}},"tracks":[{"trackId":"mic-1","kind":"audio","sourceId":"mic","direction":"sendonly","codec":"opus","state":"requested"},{"trackId":"speaker-1","kind":"audio","sourceId":"speaker","direction":"recvonly","codec":"opus","state":"requested"}]}})";
    VoiceEvent event;
    ASSERT_EQ(decode_voice_event(WireRoute::Signaling, created_web, event), WireResult::Ok);
    EXPECT_EQ(event.media.transport, VoiceTransport::WebRTC);
    EXPECT_EQ(event.media.port, 0);
    EXPECT_TRUE(event.media.host.empty());
    ASSERT_EQ(media_offer_command("offer", "s", "v=0\r\no=offer\r\n", request), WireResult::Ok);
    EXPECT_EQ(request.method, "mediaOffer");
    EXPECT_NE(request.payload.find("iceMode"), std::string::npos);
    EXPECT_EQ(media_offer_command("offer", "s", "v=0\n", request), WireResult::Invalid);
    ASSERT_EQ(
        voice_control("start", "s", "audioStart", "input", 1, request, 0, VoiceTransport::WebRTC),
        WireResult::Ok);
    EXPECT_EQ(request.payload.find("firstPacketSeq"), std::string::npos);
    ASSERT_EQ(
        voice_control("stop", "s", "audioStop", "input", 1, request, 0, VoiceTransport::WebRTC),
        WireResult::Ok);
    EXPECT_EQ(request.payload.find("lastPacketSeq"), std::string::npos);
    EXPECT_EQ(
        voice_control("bad", "s", "audioStart", "input", 1, request, 128, VoiceTransport::WebRTC),
        WireResult::Invalid);
    const auto answer =
        R"({"msgToken":"offer","method":"mediaAnswer","sessionId":"s","code":200,"data":{"mediaRevision":1,"webrtc":{"iceMode":"complete","sdp":"v=0\r\no=answer\r\n"}}})";
    ASSERT_EQ(decode_voice_event(WireRoute::Signaling, answer, event), WireResult::Ok);
    EXPECT_EQ(event.answer_sdp, "v=0\r\no=answer\r\n");
    EXPECT_EQ(decode_voice_event(WireRoute::Message, answer, event), WireResult::WrongRoute);
    const auto start =
        R"({"msgToken":"e1","method":"respAudioStart","sessionId":"s","eventSeq":1,"data":{"respId":"r","trackId":"speaker-1","mediaRevision":1}})";
    ASSERT_EQ(decode_voice_event(WireRoute::Signaling, start, event), WireResult::Ok);
    EXPECT_FALSE(event.udp_boundary);
    const auto done =
        R"({"msgToken":"e2","method":"respAudioDone","sessionId":"s","eventSeq":2,"data":{"respId":"r","trackId":"speaker-1","mediaRevision":1}})";
    ASSERT_EQ(decode_voice_event(WireRoute::Signaling, done, event), WireResult::Ok);
    EXPECT_FALSE(event.udp_boundary);
}
TEST(VoiceWire, RequestAckChecksMethodSpecificFieldsAndFailures) {
    VoiceEvent event;
    const auto ack = [](const std::string& method, const std::string& result,
                        const std::string& session = "s") {
        return "{\"msgToken\":\"t\",\"method\":\"requestAck\"," +
               (session.empty() ? "" : "\"sessionId\":\"" + session + "\",") +
               "\"code\":200,\"data\":{\"requestMethod\":\"" + method + "\",\"result\":" + result +
               "}}";
    };
    EXPECT_EQ(decode_voice_event(WireRoute::Signaling,
                                 ack("capabilityReport", R"({"acceptedRevision":1})", ""), event),
              WireResult::Ok);
    EXPECT_EQ(event.revision, 1U);
    EXPECT_EQ(decode_voice_event(WireRoute::Signaling,
                                 ack("capabilityReport", R"({"acceptedRevision":1})"), event),
              WireResult::Invalid);
    for (const auto* method : {"audioStart", "audioStop"}) {
        EXPECT_EQ(
            decode_voice_event(WireRoute::Signaling, ack(method, R"({"inputId":"i"})"), event),
            WireResult::Ok);
        EXPECT_EQ(event.input_id, "i");
    }
    EXPECT_EQ(decode_voice_event(WireRoute::Signaling, ack("audioStart", R"({"inputId":"i"})", ""),
                                 event),
              WireResult::Invalid);
    EXPECT_EQ(decode_voice_event(WireRoute::Signaling,
                                 ack("respCancel", R"({"respId":"r","state":"cancelled"})"), event),
              WireResult::Ok);
    EXPECT_EQ(
        decode_voice_event(WireRoute::Signaling,
                           ack("respCancel", R"({"respId":"r","state":"alreadyFinished"})"), event),
        WireResult::Ok);
    EXPECT_EQ(
        decode_voice_event(WireRoute::Signaling,
                           ack("mediaState", R"({"mediaRevision":1,"state":"connected"})"), event),
        WireResult::Ok);
    EXPECT_EQ(
        decode_voice_event(WireRoute::Signaling,
                           ack("mediaState", R"({"mediaRevision":1,"state":"failed"})"), event),
        WireResult::Ok);
    EXPECT_EQ(
        decode_voice_event(WireRoute::Signaling,
                           ack("mediaState", R"({"mediaRevision":1,"state":"unknown"})"), event),
        WireResult::Invalid);
    EXPECT_EQ(decode_voice_event(WireRoute::Signaling, ack("unknown", R"({})"), event),
              WireResult::Invalid);
    EXPECT_EQ(decode_voice_event(WireRoute::Signaling, ack("audioStart", R"({})"), event),
              WireResult::Invalid);
    EXPECT_EQ(
        decode_voice_event(
            WireRoute::Signaling,
            R"({"msgToken":"t","method":"requestAck","code":403,"data":{"requestMethod":"capabilityReport","reason":"permissionDenied"}})",
            event),
        WireResult::Ok);
    EXPECT_EQ(event.reason, "permissionDenied");
    EXPECT_EQ(
        decode_voice_event(
            WireRoute::Signaling,
            R"({"msgToken":"t","method":"requestAck","code":403,"data":{"requestMethod":"capabilityReport","result":{},"reason":"permissionDenied"}})",
            event),
        WireResult::Invalid);
}
TEST(VoiceWire, ErrorsRequireScopeTargetAndExplicitRecoveryPolicy) {
    VoiceEvent parsed;
    const std::string prefix =
        R"({"msgToken":"e","method":"sessionError","sessionId":"s","code":502,"eventSeq":1,"data":)";
    for (
        const auto* payload :
        {R"({"reason":"failed","retryable":false})",
         R"({"reason":"failed","retryable":true,"scope":"input","inputId":"i"})",
         R"({"reason":"failed","retryable":true,"scope":"tool","respId":"r","toolCallId":"tool"})"}) {
        EXPECT_EQ(decode_voice_event(WireRoute::Signaling, prefix + payload + "}", parsed),
                  WireResult::Ok);
    }
    for (const auto* payload :
         {R"({"reason":"failed"})", R"({"reason":"failed","retryable":"true"})",
          R"({"reason":"failed","retryable":true,"scope":"unknown"})",
          R"({"reason":"failed","retryable":true,"scope":"response"})",
          R"({"reason":"failed","retryable":true,"scope":"tool","respId":"r"})"}) {
        EXPECT_EQ(decode_voice_event(WireRoute::Signaling, prefix + payload + "}", parsed),
                  WireResult::Invalid);
    }
}
// 恢复沿原mediaState方法报告实际暂断，不能伪装为失败终态或允许UDP重协商。
TEST(VoiceWire, WebRTCDisconnectionUsesOriginalRevisionAndAcknowledgement) {
    Envelope request;
    ASSERT_EQ(voice_control("disconnect", "s", "mediaState", "disconnected", 2, request, 0,
                            VoiceTransport::WebRTC),
              WireResult::Ok);
    EXPECT_EQ(request.payload, R"({"mediaRevision":2,"state":"disconnected"})");
    VoiceEvent reply;
    ASSERT_EQ(
        decode_voice_event(
            WireRoute::Signaling,
            R"({"msgToken":"disconnect","method":"requestAck","sessionId":"s","code":200,"data":{"requestMethod":"mediaState","result":{"mediaRevision":2,"state":"disconnected"}}})",
            reply),
        WireResult::Ok);
    EXPECT_EQ(reply.revision, 2U);
    EXPECT_EQ(reply.state, "disconnected");
    EXPECT_EQ(
        voice_control("udp", "s", "mediaState", "disconnected", 1, request, 0, VoiceTransport::Udp),
        WireResult::Invalid);
    EXPECT_TRUE(request.token.empty());
    EXPECT_TRUE(request.method.empty());
}
TEST(VoiceWire, EventsRequireSequenceSessionTrackAndRevision) {
    VoiceEvent event;
    for (const auto* method : {"respAudioStart", "respAudioDone", "audioSpeechInterrupted"}) {
        const std::string body =
            "{\"msgToken\":\"e\",\"method\":\"" + std::string(method) +
            "\",\"sessionId\":\"s\",\"eventSeq\":2,\"data\":{\"respId\":\"r\","
            "\"trackId\":\"speaker-1\",\"mediaRevision\":1,\"udpNonce\":"
            "\"9988776655443322\",\"firstPacketSeq\":128,\"lastPacketSeq\":128}}";
        EXPECT_EQ(decode_voice_event(WireRoute::Signaling, body, event), WireResult::Ok);
        EXPECT_EQ(event.response_id, "r");
        EXPECT_EQ(decode_voice_event(WireRoute::Signaling,
                                     replace_first(body, "\"eventSeq\":2,", ""), event),
                  WireResult::Invalid);
        EXPECT_EQ(decode_voice_event(
                      WireRoute::Signaling,
                      replace_first(body, "\"mediaRevision\":1", "\"mediaRevision\":0"), event),
                  WireResult::Invalid);
        EXPECT_EQ(decode_voice_event(WireRoute::Signaling,
                                     replace_first(body, "\"trackId\":\"speaker-1\",", ""), event),
                  WireResult::Invalid);
    }
    EXPECT_EQ(
        decode_voice_event(
            WireRoute::Signaling,
            R"({"msgToken":"e","method":"sessionEstablished","sessionId":"s","eventSeq":1,"data":{"state":"established","mediaRevision":1}})",
            event),
        WireResult::Ok);
    EXPECT_EQ(
        decode_voice_event(
            WireRoute::Signaling,
            R"({"msgToken":"e","method":"sessionEnded","sessionId":"s","eventSeq":2,"data":{"state":"ended","reason":"networkTimeout"}})",
            event),
        WireResult::Ok);
    EXPECT_EQ(
        decode_voice_event(
            WireRoute::Signaling,
            R"({"msgToken":"e","method":"sessionEnded","sessionId":"s","code":200,"data":{"state":"ended","reason":"userStopped"}})",
            event),
        WireResult::Ok);
    EXPECT_EQ(
        decode_voice_event(
            WireRoute::Signaling,
            R"({"msgToken":"e","method":"sessionEnded","sessionId":"s","code":409,"data":{"reason":"sessionExpired"}})",
            event),
        WireResult::Ok);
    EXPECT_EQ(
        decode_voice_event(
            WireRoute::Signaling,
            R"({"msgToken":"e","method":"sessionCreated","code":422,"data":{"reason":"unsupportedCapability"}})",
            event),
        WireResult::Ok);
    EXPECT_EQ(decode_voice_event(WireRoute::Signaling,
                                 R"({"msgToken":"e","method":"unknown","data":{}})", event),
              WireResult::Unsupported);
    EXPECT_EQ(decode_voice_event(WireRoute::Signaling,
                                 R"({"msgToken":"e","method":"unknown","params":{}})", event),
              WireResult::Invalid);
    EXPECT_EQ(decode_voice_event(WireRoute::Signaling, "broken", event), WireResult::Invalid);
}
// 目标端恢复的两字段必须成对提供；旧应答无字段仍有效，不能补造初始代次。
TEST(VoiceWire, EndpointEstablishedControlBindingRequiresPairedSafeIntegers) {
    const std::string prefix = R"({"msgToken":"e","method":"sessionEstablished","sessionId":"candidate","eventSeq":2,"data":{"state":"established","mediaRevision":1)";
    VoiceEvent event;
    ASSERT_EQ(decode_voice_event(WireRoute::Signaling,
        prefix + R"(,"endpointEpoch":4,"capabilityRevision":1}})", event), WireResult::Ok);
    EXPECT_EQ(event.endpoint_epoch, 4U);
    EXPECT_EQ(event.capability_revision, 1U);
    for (const auto* invalid : {R"(,"endpointEpoch":4)", R"(,"capabilityRevision":1)",
         R"(,"endpointEpoch":0,"capabilityRevision":1)",
         R"(,"endpointEpoch":4,"capabilityRevision":0)",
         R"(,"endpointEpoch":1.5,"capabilityRevision":1)",
         R"(,"endpointEpoch":9007199254740992,"capabilityRevision":1)"}) {
        EXPECT_EQ(decode_voice_event(WireRoute::Signaling, prefix + invalid + "}}", event),
                  WireResult::Invalid) << invalid;
    }
    EXPECT_EQ(decode_voice_event(WireRoute::Signaling, prefix + "}}", event), WireResult::Ok);
    EXPECT_EQ(event.endpoint_epoch, 0U);
    EXPECT_EQ(event.capability_revision, 0U);
}
// 单一新feature依赖原sessionSync、接续音频和本人接听，不给旧候选隐式升级。
TEST(VoiceWire, EndpointControlSyncCapabilityRequiresItsCompleteDependencies) {
    VoiceCapability capability;
    capability.audio = {16000, 1, 20};
    capability.modes = {true, true, true};
    capability.session_sync = capability.ai_endpoint_handoff =
        capability.ai_endpoint_control_sync = true;
    Envelope report;
    ASSERT_EQ(capability_command("report", capability, report), WireResult::Ok);
    EXPECT_NE(report.payload.find("\"aiEndpointControlSync\""), std::string::npos);
    for (unsigned missing = 0; missing < 2; ++missing) {
        auto invalid = capability;
        if (missing == 0) invalid.session_sync = false;
        else invalid.ai_endpoint_handoff = false;
        EXPECT_EQ(capability_command("invalid", invalid, report), WireResult::Invalid);
    }
    capability.ai_endpoint_control_sync = false;
    ASSERT_EQ(capability_command("legacy", capability, report), WireResult::Ok);
    EXPECT_EQ(report.payload.find("aiEndpointControlSync"), std::string::npos);
}
}  // namespace
}  // namespace ur::device
