// 文件说明：执行正式device_browser同一回环视觉分派，真实适配器+可控本地桥，不代表真实相机/模型E2E。
// 原生产main重命名后直接复用函数，不通过源码正则或指标伪造发布事实。
#define main task143_vision_host_main
#include "../adapters/desktop/browser_main.cpp"
#undef main
#include <gtest/gtest.h>
#include <random>

namespace ur::device {
namespace {
TEST(AIEndpointSourceHostIntent, LocalTargetWishNeverAcceptsSessionRevisionOrCandidate) {
    const auto prepare = endpoint_source_intent(
        R"({"kind":"endpointSourceControl","operation":"prepare","target":"device:p:target"})");
    ASSERT_TRUE(prepare.matched);
    ASSERT_TRUE(prepare.valid);
    EXPECT_EQ(prepare.operation, "prepare");
    EXPECT_EQ(prepare.target, "device:p:target");
    EXPECT_TRUE(endpoint_source_intent(R"({"kind":"endpointSourceControl","operation":"commit"})").valid);
    EXPECT_TRUE(endpoint_source_intent(R"({"kind":"endpointSourceControl","operation":"abort"})").valid);
    EXPECT_FALSE(endpoint_source_intent(R"({"kind":"start"})").matched);
    for (const auto* body : {
        R"({"kind":"endpointSourceControl","operation":"prepare","target":""})",
        R"({"kind":"endpointSourceControl","operation":"prepare","target":"bad\nname"})",
        R"({"kind":"endpointSourceControl","operation":"prepare","target":"device:p:t","sessionId":"alien"})",
        R"({"kind":"endpointSourceControl","operation":"commit","candidateSessionId":"alien"})",
        R"({"kind":"endpointSourceControl","operation":"abort","target":"device:p:t"})",
        R"({"kind":"endpointSourceControl","operation":"restore"})"}) {
        const auto value = endpoint_source_intent(body);
        EXPECT_TRUE(value.matched);
        EXPECT_FALSE(value.valid);
    }
}
TEST(AIEndpointSourceHostIntent, ProjectionHasNoPlatformIdentityAndDoesNotGrantUnreadyCommit) {
    EndpointHandoffReceipt receipt;
    EXPECT_EQ(endpoint_source_state(&receipt, true),
        R"({"kind":"endpointSourceState","state":"idle","canPrepare":true,"canCommit":false,"canAbort":false,"pending":false,"unknown":false})");
    receipt.session = "original-private-session";
    receipt.handoff = "original-private-handoff";
    receipt.candidate = "original-private-candidate";
    receipt.state = "ready";
    EXPECT_NE(endpoint_source_state(&receipt, true).find(R"("canCommit":true)"), std::string::npos);
    receipt.unknown = true;
    const auto unknown = endpoint_source_state(&receipt, true);
    EXPECT_NE(unknown.find(R"("canCommit":false)"), std::string::npos);
    EXPECT_EQ(unknown.find("private"), std::string::npos);
    receipt.state = "untrusted-body";
    EXPECT_TRUE(endpoint_source_state(&receipt, true).empty());
    EXPECT_TRUE(endpoint_source_state(nullptr, true).empty());
}
TEST(AIEndpointHostIntent, ExactLocalAcceptRejectNeverAcceptPlatformParameters) {
    EXPECT_EQ(endpoint_local_intent(R"({"kind":"endpointAccept"})"), EndpointLocalIntent::Accept);
    EXPECT_EQ(endpoint_local_intent(R"({"kind":"endpointReject"})"), EndpointLocalIntent::Reject);
    EXPECT_EQ(endpoint_local_intent(R"({"kind":"start"})"), EndpointLocalIntent::Other);
    for (const auto* bytes : {R"({"kind":"endpointAccept","sessionId":"alien"})",
                             R"({"kind":"endpointReject","expiresAt":999999})",
                             R"({"kind":"endpointAccept","kind":"endpointAccept"})"})
        EXPECT_EQ(endpoint_local_intent(bytes), EndpointLocalIntent::Invalid);
}
TEST(AIEndpointHostIntent, ProjectionOnlyContainsFixedLocalStateAndButtons) {
    EXPECT_EQ(endpoint_local_state(AIEndpointPhase::Invited),
              R"({"kind":"endpointState","phase":2,"canAccept":true,"canReject":true})");
    EXPECT_EQ(endpoint_local_state(AIEndpointPhase::AwaitingFocus),
              R"({"kind":"endpointState","phase":6,"canAccept":false,"canReject":false})");
    EXPECT_TRUE(endpoint_local_state(static_cast<AIEndpointPhase>(99)).empty());
}
TEST(AIEndpointHostIntent, OriginalGeneratedAssetsStartWithoutBroadeningWhitelistOrEnablingHuman) {
    auto assets = browser_assets();
    ASSERT_TRUE(endpoint_assets(assets, 7, false));
    const auto& script = assets.at("/shared-handoff-native.js");
    EXPECT_NE(script.find("UrAIEndpointListen=true"), std::string::npos);
    EXPECT_EQ(script.find("UrHumanVoiceEnabled"), std::string::npos);
    EXPECT_LT(assets.at("/index.html").find("/input-ownership.js"),
              assets.at("/index.html").find("/app.js"));
    std::random_device random; // 仅为本用例选回环端口，不扫描或终止其他服务。
    bool started = false; // 只以正式白名单桥真实监听成功为判据。
    for (unsigned attempt = 0; attempt < 20 && !started; ++attempt) {
        BrowserBridge bridge(40000U + random() % 20000U, std::string(64, 'b'), assets);
        started = bridge.start();
    }
    EXPECT_TRUE(started);
    auto invalid = browser_assets();
    EXPECT_FALSE(endpoint_assets(invalid, 0, false));
    EXPECT_EQ(invalid.count("/shared-handoff-native.js"), 0U);
}
// VisionHost固定原真实适配器scope，控制/回执经原驱动队列；不运行浏览器、MQTT或收费模型。
class VisionHost : public testing::Test {
protected:
    BrowserWebRTCMedia media;  // 实際受测同一适配器，不替换receive_vision_actual实现。
    // SetUp只建立可控SDP/连通fixture；这些驱动事实不作为真实媒体软件E2E证据。
    void SetUp() override {
        const VoiceVisionProfile profile{"front", "vision-1", 1, true};
        ASSERT_TRUE(media.enable_vision(profile));
        VoiceMedia binding;
        binding.transport = VoiceTransport::WebRTC;
        binding.ai_vision = true;
        binding.audio = {48000, 1, 20};
        binding.input_track = "mic-1";
        binding.output_track = "speaker-1";
        ASSERT_TRUE(media.configure_vision(binding, profile));
        ASSERT_TRUE(media.take_command());
        ASSERT_TRUE(media.offer_received("v=0\r\no=offer\r\n", 1));
        ASSERT_TRUE(media.take_offer());
        ASSERT_TRUE(media.apply_answer("v=0\r\no=answer\r\n"));
        ASSERT_TRUE(media.take_command());
        ASSERT_TRUE(media.connected_received(1));
        const VoiceVisionControl command{7, "original-session",           "front", "vision-1",
                                         1, VoiceVisionOperation::Publish};
        ASSERT_EQ(media.begin_vision_control(command), VoiceVisionRequestResult::Accepted);
        ASSERT_TRUE(media.take_command());
    }
    // actual严格保留原九字段，不夹token/指标/自由文本；字段核权由适配器执行。
    std::string actual(std::string_view session = "original-session", unsigned generation = 7) {
        return R"({"kind":"webrtcVisionActual","generation":")" + std::to_string(generation) +
               R"(","sessionId":")" + std::string(session) +
               R"(","cameraId":"front","trackId":"vision-1","mediaRevision":"1","peerRevision":"1","state":"active","error":""})";
    }
};
TEST_F(VisionHost, AuthenticatedOriginalRawFactReachesActualAdapter) {
    EXPECT_EQ(vision_local(actual(), true, true, media), VisionLocalResult::Handled);
    const auto fact = media.take_vision_fact();
    ASSERT_TRUE(fact);
    EXPECT_EQ(fact->generation, 7U);
    EXPECT_EQ(fact->state, VoiceVisionState::Active);
    EXPECT_EQ(fact->failure, VoiceVisionFailure::None);
}
TEST_F(VisionHost, MetricsAndWrongScopeCannotCreateHALFacts) {
    EXPECT_EQ(vision_local(R"({"kind":"metrics","videoFrames":100})", true, true, media),
              VisionLocalResult::Other);
    EXPECT_EQ(vision_local(actual("another-session"), true, true, media),
              VisionLocalResult::Rejected);
    EXPECT_EQ(vision_local(actual(), false, true, media), VisionLocalResult::Rejected);
    EXPECT_EQ(vision_local(actual(), true, false, media), VisionLocalResult::Rejected);
    EXPECT_FALSE(media.take_vision_fact());
}
TEST_F(VisionHost, RetiredOriginalGenerationIsConsumedWithoutRevivingTrack) {
    media.cancel_vision_control(7);
    EXPECT_EQ(vision_local(actual(), true, true, media), VisionLocalResult::Handled);
    EXPECT_FALSE(media.take_vision_fact());
}
// camera_mapping仅用于宿主的显式本地配置，不包含平台身份、许可或远程地址。
std::string camera_mapping(std::string_view camera = "front", std::string_view track = "vision-1",
                           std::string_view device = "explicit-device") {
    return "{\"cameraId\":\"" + std::string(camera) + "\",\"videoTrack\":\"" + std::string(track) +
           "\",\"deviceId\":\"" + std::string(device) + "\"}";
}
// ready_mapping保留原预检五字段；不是采集、发布或模型同意事实。
std::string ready_mapping(std::string_view state = "ready") {
    return "{\"kind\":\"webrtcVisionReady\",\"cameraId\":\"front\",\"videoTrack\":\"vision-1\","
           "\"deviceId\":\"explicit-device\",\"state\":\"" +
           std::string(state) + "\"}";
}
TEST(VisionHostConfiguration, DefaultKeepsOriginalAudioCapabilityAndNoCamera) {
    std::optional<VisionCameraConfig> config;
    BrowserWebRTCMedia media;
    VoiceCapability capability;
    capability.text_input = true;
    EXPECT_TRUE(vision_camera_config("", false, config));
    EXPECT_FALSE(config);
    EXPECT_TRUE(vision_capability(config, media, capability));
    EXPECT_FALSE(capability.ai_vision);
    EXPECT_TRUE(capability.text_input);
    EXPECT_TRUE(vision_camera_config("", true, config));
    EXPECT_FALSE(config);
    EXPECT_FALSE(media.vision_profile());
}
TEST(VisionHostConfiguration, ExactNativeMappingCannotClaimReadyOrUseUDP) {
    std::optional<VisionCameraConfig> config;
    ASSERT_TRUE(vision_camera_config(camera_mapping(), true, config));
    ASSERT_TRUE(config);
    EXPECT_FALSE(config->profile.ready);
    EXPECT_EQ(config->profile.max_publishers, 1U);
    EXPECT_EQ(config->profile.camera_id, "front");
    EXPECT_EQ(config->profile.video_track, "vision-1");
    EXPECT_EQ(config->device_id, "explicit-device");
    EXPECT_FALSE(vision_camera_config(camera_mapping(), false, config));
    EXPECT_FALSE(config);
}
TEST(VisionHostConfiguration, StrictFieldsRejectMalformedDuplicateNullAndControlIdentifiers) {
    for (
        const auto& body :
        {std::string("[]"), std::string("{"),
         std::string(R"({"cameraId":null,"videoTrack":"vision-1","deviceId":"explicit-device"})"),
         std::string(R"({"cameraId":"front","videoTrack":"vision-1","deviceId":1})"),
         std::string(
             R"({"cameraId":"front","videoTrack":"vision-1","deviceId":"explicit-device","ready":true})"),
         std::string(
             R"({"cameraId":"front","camera\u0049d":"other","videoTrack":"vision-1","deviceId":"explicit-device"})"),
         camera_mapping(""), camera_mapping("front", "mic-1"), camera_mapping("front", "speaker-1"),
         camera_mapping("front", "vision-1", ""),
         camera_mapping("front", "vision-1", "bad\\u0000device"),
         camera_mapping("front", "vision-1", "bad\\ndevice"),
         camera_mapping("front", "vision-1", "bad\\u007fdevice"),
         camera_mapping(std::string(65, 'c')), camera_mapping("front", std::string(65, 't')),
         camera_mapping("front", "vision-1", std::string(257, 'd')),
         camera_mapping(std::string(1, static_cast<char>(0xff)))}) {
        std::optional<VisionCameraConfig> config;
        EXPECT_FALSE(vision_camera_config(body, true, config));
        EXPECT_FALSE(config);
    }
}
TEST(VisionHostConfiguration, UTF8ByteBoundariesAndSafeSerializationPreserveOriginalMapping) {
    std::string camera;
    for (unsigned index = 0; index < 21; ++index)
        camera += "相";
    camera += "a";
    std::optional<VisionCameraConfig> config;
    ASSERT_TRUE(vision_camera_config(camera_mapping(camera), true, config));
    EXPECT_FALSE(vision_camera_config(camera_mapping(camera + "b"), true, config));
    ASSERT_TRUE(vision_camera_config(
        R"({"cameraId":"front\"native","videoTrack":"vision-1","deviceId":"physical\\device\"one"})",
        true, config));
    const auto script = vision_camera_script(*config);
    const std::string prefix = "'use strict';globalThis.UrAIVisionCamera=";
    ASSERT_EQ(script.substr(0, prefix.size()), prefix);
    ASSERT_EQ(script.substr(script.size() - 2), ";\n");
    const auto body = script.substr(prefix.size(), script.size() - prefix.size() - 2);
    std::optional<VisionCameraConfig> recovered;
    ASSERT_TRUE(vision_camera_config(body, true, recovered));
    ASSERT_TRUE(recovered);
    EXPECT_EQ(recovered->profile.camera_id, config->profile.camera_id);
    EXPECT_EQ(recovered->device_id, config->device_id);
}
TEST(VisionHostReadiness, ActualPreflightUnlocksOnlyOriginalCapabilityNotCapture) {
    std::optional<VisionCameraConfig> config;
    ASSERT_TRUE(vision_camera_config(camera_mapping(), true, config));
    BrowserWebRTCMedia media;
    ASSERT_TRUE(media.prepare_vision_preflight(config->profile, config->device_id));
    VoiceCapability capability;
    capability.transport = VoiceTransport::WebRTC;
    capability.audio = {48000, 1, 20};
    capability.modes = {true, false, false};
    capability.text_input = true;
    EXPECT_FALSE(vision_capability(config, media, capability));
    EXPECT_FALSE(capability.ai_vision);
    EXPECT_EQ(vision_local(R"({"kind":"ready","aec":true})", true, false, media),
              VisionLocalResult::Other);
    EXPECT_EQ(vision_local(ready_mapping(), true, false, media), VisionLocalResult::Handled);
    ASSERT_TRUE(vision_capability(config, media, capability));
    EXPECT_TRUE(capability.ai_vision);
    EXPECT_FALSE(capability.text_input);
    EXPECT_FALSE(capability.modes.full_duplex);
    EXPECT_FALSE(capability.modes.device_aec);
    EXPECT_FALSE(media.take_command());
    EXPECT_FALSE(media.take_vision_fact());
    Envelope report;
    ASSERT_EQ(capability_command("cap-original", capability, report, media.vision_profile()),
              WireResult::Ok);
    EXPECT_NE(report.payload.find("aiVision"), std::string::npos);
}
TEST(VisionHostReadiness, UnsupportedMismatchedLateOrWrongTransportCannotUnlockCapability) {
    for (unsigned variant = 0; variant < 5; ++variant) {
        BrowserWebRTCMedia media;
        std::optional<VisionCameraConfig> config;
        ASSERT_TRUE(vision_camera_config(camera_mapping(), true, config));
        ASSERT_TRUE(media.prepare_vision_preflight(config->profile, config->device_id));
        auto body = ready_mapping(variant == 0 ? "unsupported" : "ready");
        if (variant == 1)
            body.replace(body.find("front"), 5, "other");
        if (variant == 2)
            body.insert(body.size() - 1, ",\"frames\":10");
        EXPECT_EQ(vision_local(body, variant != 3, variant == 4, media),
                  VisionLocalResult::Rejected);
        EXPECT_FALSE(media.vision_profile());
        VoiceCapability capability;
        capability.transport = VoiceTransport::WebRTC;
        EXPECT_FALSE(vision_capability(config, media, capability));
        EXPECT_FALSE(capability.ai_vision);
        EXPECT_FALSE(media.take_command());
    }
}
TEST(VisionHostReadiness, WrongCapabilityTransportOrNativeBindingCannotReuseReadyProfile) {
    std::optional<VisionCameraConfig> config;
    ASSERT_TRUE(vision_camera_config(camera_mapping(), true, config));
    BrowserWebRTCMedia media;
    ASSERT_TRUE(media.prepare_vision_preflight(config->profile, config->device_id));
    ASSERT_EQ(vision_local(ready_mapping(), true, false, media), VisionLocalResult::Handled);
    VoiceCapability capability;
    EXPECT_FALSE(vision_capability(config, media, capability));
    capability.transport = VoiceTransport::WebRTC;
    config->profile.camera_id = "other";
    EXPECT_FALSE(vision_capability(config, media, capability));
    EXPECT_FALSE(capability.ai_vision);
}
}  // namespace
}  // namespace ur::device
