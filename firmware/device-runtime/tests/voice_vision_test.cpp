// AI摄像头核心契约与原语音状态机组合回归；使用可控HAL，不声称摄像头/模型端云验收。
#include <gtest/gtest.h>
#include <utility>
#include "ur/device/voice_client.hpp"

namespace ur::device {
namespace {
// OptionalProfileHasBoundedObjectCost记录实际ABI，仅增加可选配置拥有指针，不扩展原Session状态。
TEST(VoiceVisionLayout, OptionalProfileHasBoundedObjectCost) {
    RecordProperty("voice_client_bytes", static_cast<int>(sizeof(VoiceClient)));
    RecordProperty("voice_capability_bytes", static_cast<int>(sizeof(VoiceCapability)));
    RecordProperty("voice_media_bytes", static_cast<int>(sizeof(VoiceMedia)));
    RecordProperty("vision_profile_bytes", static_cast<int>(sizeof(VoiceVisionProfile)));
    EXPECT_LE(sizeof(VoiceClient),
              920U);  // 合并双方:原888+UDP安装账本指针8+humanCall标志+恢复期offer重试起点8+恢复期请求应答预算8,按实测上限。
    EXPECT_LE(sizeof(VoiceVisionProfile), 80U);
}
// vision_capability只声明实际受限48k/20ms半双工视觉，不补造设备AEC。
VoiceCapability vision_capability() {
    VoiceCapability capability;
    capability.transport = VoiceTransport::WebRTC;
    capability.audio = {48000, 1, 20};
    capability.modes.half_duplex = true;
    capability.ai_vision = true;
    return capability;
}
// vision_data来自正式Creator的selected和原三轨，没有附件模态或摄像头授权字段。
const std::string vision_data =
    R"({"state":"negotiating","selected":{"transport":"webrtc","interactionMode":"halfDuplex","audioParams":{"format":"opus","sampleRate":48000,"channels":1,"frameDuration":20}},"tracks":[{"trackId":"mic-1","kind":"audio","sourceId":"mic","direction":"sendonly","codec":"opus","state":"requested"},{"trackId":"speaker-1","kind":"audio","sourceId":"speaker","direction":"recvonly","codec":"opus","state":"requested"},{"trackId":"vision-1","kind":"video","sourceId":"front","direction":"sendonly","codec":"H264","state":"paused"}]})";
// encoded_reply使用同一公共编码器保留原token和方法关联，避免测试绕过信封。
std::string encoded_reply(const Envelope& request, std::string method, std::string payload,
                          std::string session = "vision-session") {
    Envelope reply;
    reply.token = request.token;
    reply.method = std::move(method);
    reply.session = std::move(session);
    reply.request = false;
    reply.code = 200;
    reply.payload = std::move(payload);
    std::string encoded;
    EXPECT_EQ(encode_envelope(reply, encoded), DecodeResult::Ok);
    return encoded;
}
// vision_created生成原三轨成功应答；不把应答编码当媒体已建立。
std::string vision_created(const Envelope& request) {
    return encoded_reply(request, "sessionCreated", vision_data);
}
// changed只修改一个受控字段，便于验证错误报文不配置媒体。
std::string changed(std::string value, const std::string& from, const std::string& to) {
    const auto position = value.find(from);
    EXPECT_NE(position, std::string::npos);
    if (position != std::string::npos)
        value.replace(position, from.size(), to);
    return value;
}
// VisionPublisher保存实际上行信封；不授权任何摄像头或模型能力。
struct VisionPublisher final : PublishPort {
    std::vector<Envelope> messages;
    bool available = true;
    bool publish(std::string_view topic, std::string_view body) override {
        if (!available)
            return false;
        EXPECT_EQ(topic, "$thing/up/signaling/p/d");
        Envelope envelope;
        EXPECT_EQ(decode_envelope(body, envelope), DecodeResult::Ok);
        messages.push_back(std::move(envelope));
        return true;
    }
};
// VisionMedia仅模拟安装与实际音频驱动调用；视觉配置必须保持视频暂停，不提供同意签发接口。
struct VisionMedia final : VoiceMediaPort {
    VoiceVisionProfile profile{"front", "vision-1", 1, true};
    bool expose_profile = true;
    bool vision_ok = true;
    unsigned ordinary_configs = 0;
    unsigned vision_configs = 0;
    unsigned captures = 0;
    unsigned plays = 0;
    unsigned recoveries = 0;
    unsigned closes = 0;
    bool configured = false;
    bool video_paused = true;
    const VoiceVisionProfile* vision_profile() const override {
        return expose_profile ? &profile : nullptr;
    }
    bool configure(const VoiceMedia&) override {
        ++ordinary_configs;
        return false;
    }
    bool configure_vision(const VoiceMedia& media, const VoiceVisionProfile& original) override {
        ++vision_configs;
        EXPECT_TRUE(media.ai_vision);
        EXPECT_TRUE(media.host.empty());
        EXPECT_EQ(original.camera_id, "front");
        EXPECT_EQ(original.video_track, "vision-1");
        EXPECT_NE(&original, &profile);  // 原配置独立拥有，不借可变能力缓存。
        configured = vision_ok;
        video_paused = true;
        return vision_ok;
    }
    std::uint32_t next_input_sequence() const override { return 128; }
    bool prepare_reply(const std::array<std::uint8_t, 8>&, std::uint32_t) override { return true; }
    bool finish_reply(std::uint32_t) override { return true; }
    bool apply_answer(std::string_view) override { return configured; }
    bool begin_recovery(std::uint64_t) override {
        ++recoveries;
        return true;
    }
    bool start_capture(std::string_view) override {
        ++captures;
        return true;
    }
    void stop_capture() override {}
    bool start_playback(std::string_view, std::uint64_t) override {
        ++plays;
        return true;
    }
    void stop_playback(std::uint64_t) override {}
    void close() override {
        ++closes;
        configured = false;
        video_paused = true;
    }
};
// 人际通话可以声明相机而不启用 AI 视觉；缺少相机描述时不能发出不完整能力。
TEST(VoiceHumanCallCapability, UsesCameraProfileWithoutEnablingAiVision) {
    VisionPublisher publisher;
    VisionMedia media;
    auto capability = vision_capability();
    capability.ai_vision = false;
    capability.human_call = true;
    capability.human_udp_av = true;
    VoiceClient client{publisher, media, "$thing/up/signaling/p/d",
                       "$thing/down/signaling/p/d", capability, "tank-process"};
    ASSERT_EQ(client.connect(0), Result::Applied);
    ASSERT_EQ(publisher.messages.size(), 1U);
    EXPECT_EQ(publisher.messages[0].method, "capabilityReport");
    EXPECT_NE(publisher.messages[0].payload.find("humanCall"), std::string::npos);
    EXPECT_EQ(publisher.messages[0].payload.find("aiVision"), std::string::npos);
    EXPECT_NE(publisher.messages[0].payload.find("front"), std::string::npos);
}
// 人际相机不可用时必须拒绝能力上报，不得创建无法履行的视频能力。
TEST(VoiceHumanCallCapability, MissingCameraProfileFailsClosed) {
    VisionPublisher publisher;
    VisionMedia media;
    media.expose_profile = false;
    auto capability = vision_capability();
    capability.ai_vision = false;
    capability.human_call = true;
    VoiceClient client{publisher, media, "$thing/up/signaling/p/d",
                       "$thing/down/signaling/p/d", capability, "tank-process"};
    EXPECT_EQ(client.connect(0), Result::Unsupported);
    EXPECT_TRUE(publisher.messages.empty());
}
// VisionFixture沿同一VoiceClient/Requests完成三轨创建，所有媒体进展仍来自端口事实。
struct VisionFixture {
    VisionPublisher publisher;
    VisionMedia media;
    VoiceClient client{publisher,
                       media,
                       "$thing/up/signaling/p/d",
                       "$thing/down/signaling/p/d",
                       vision_capability(),
                       "vision-process"};
    /* 本组用例固定aiVision三轨形态：与本地语音会话解耦后需显式请求。 */
    VisionFixture() { client.request_vision_session(true); }
    Result receive(std::string_view body, std::uint64_t now = 1) {
        return client.receive({"$thing/down/signaling/p/d", body}, now);
    }
    void negotiate() {
        ASSERT_EQ(client.connect_shared(0), Result::Applied);
        ASSERT_EQ(receive(vision_created(publisher.messages.back())), Result::Applied);
    }
    void establish() {
        negotiate();
        ASSERT_EQ(client.media_offer_ready("v=0\r\no=offer\r\n", 2), Result::Applied);
        ASSERT_EQ(
            receive(
                encoded_reply(
                    publisher.messages.back(), "mediaAnswer",
                    R"({"mediaRevision":1,"webrtc":{"iceMode":"complete","sdp":"v=0\r\no=answer\r\n"}})"),
                3),
            Result::Applied);
        ASSERT_EQ(client.media_ready(4), Result::Applied);
        ASSERT_EQ(
            receive(
                encoded_reply(
                    publisher.messages.back(), "requestAck",
                    R"({"requestMethod":"mediaState","result":{"state":"connected","mediaRevision":1}})"),
                5),
            Result::Applied);
        ASSERT_EQ(
            receive(
                R"({"msgToken":"established","method":"sessionEstablished","sessionId":"vision-session","eventSeq":1,"data":{"state":"established","mediaRevision":1}})",
                6),
            Result::Applied);
        ASSERT_EQ(client.state(), VoiceState::Established);
    }
};

TEST(VoiceVisionWire, ExplicitActualProfileProducesOriginalSixKeyCreate) {
    const VoiceVisionProfile profile{"front", "vision-1", 1, true};
    Envelope request;
    ASSERT_EQ(capability_command("cap", vision_capability(), request, &profile), WireResult::Ok);
    EXPECT_NE(request.payload.find(R"("purposes":["aiConversation","aiVision"])"),
              std::string::npos);
    EXPECT_NE(request.payload.find(R"("features":["audio","video"])"), std::string::npos);
    EXPECT_NE(request.payload.find(R"("aecMode":"none")"), std::string::npos);
    EXPECT_NE(request.payload.find(R"("cameraId":"front")"), std::string::npos);
    ASSERT_EQ(create_voice_command("create", vision_capability(), request, &profile),
              WireResult::Ok);
    EXPECT_NE(request.payload.find(R"("purpose":"aiVision")"), std::string::npos);
    EXPECT_NE(request.payload.find(R"("state":"paused")"), std::string::npos);
    EXPECT_EQ(request.payload.find("consent"), std::string::npos);
    EXPECT_EQ(request.payload.find("inputModalities"), std::string::npos);
    EXPECT_EQ(request.payload.find("frontendTools"), std::string::npos);
    VoiceEvent selected;
    ASSERT_EQ(decode_voice_event(WireRoute::Signaling, vision_created(request), selected, &profile),
              WireResult::Ok);
    EXPECT_TRUE(selected.media.ai_vision);
    EXPECT_TRUE(selected.media.host.empty());
    EXPECT_EQ(selected.media.input_track, "mic-1");
    EXPECT_EQ(selected.media.output_track, "speaker-1");
    EXPECT_EQ(selected.media.mode, Mode::HalfDuplex);
}
TEST(VoiceVisionWire, NoProfileOrImplicitIPCAndAttachmentCannotAdvertiseVision) {
    Envelope request;
    auto capability = vision_capability();
    EXPECT_EQ(capability_command("cap", capability, request), WireResult::Invalid);
    /* 视觉能力与会话形态解耦：无profile的创建是明确的本地语音会话（aiConversation
     * 两轨），不再拒绝；能力上报仍必须带profile。 */
    ASSERT_EQ(create_voice_command("create", capability, request), WireResult::Ok);
    EXPECT_NE(request.payload.find(R"("purpose":"aiConversation")"), std::string::npos);
    EXPECT_EQ(request.payload.find("aiVision"), std::string::npos);
    EXPECT_EQ(request.payload.find("cameraId"), std::string::npos);
    EXPECT_EQ(request.payload.find(R"("kind":"video")"), std::string::npos);
    const VoiceVisionProfile profile{"front", "vision-1", 1, true};
    capability.ai_vision = false;
    ASSERT_EQ(capability_command("cap", capability, request, &profile), WireResult::Ok);
    EXPECT_EQ(request.payload.find("aiVision"), std::string::npos);
    EXPECT_EQ(request.payload.find("cameras"), std::string::npos);
    VoiceEvent event;
    EXPECT_EQ(decode_voice_event(WireRoute::Signaling, vision_created(request), event),
              WireResult::Invalid);
}
TEST(VoiceVisionWire, RejectsUnavailableMalformedAndAliasedProfiles) {
    for (unsigned index = 0; index < 9; ++index) {
        VoiceVisionProfile profile{"front", "vision-1", 1, true};
        switch (index) {
            case 0:
                profile.ready = false;
                break;
            case 1:
                profile.camera_id.clear();
                break;
            case 2:
                profile.camera_id = "camera\n";
                break;
            case 3:
                profile.camera_id = std::string(129, 'c');
                break;
            case 4:
                profile.camera_id = std::string(1, static_cast<char>(0xff));
                break;
            case 5:
                profile.video_track = "mic-1";
                break;
            case 6:
                profile.video_track = "speaker-1";
                break;
            case 7:
                profile.max_publishers = 0;
                break;
            default:
                profile.max_publishers = 5;
                break;
        }
        Envelope request;
        EXPECT_EQ(capability_command("cap", vision_capability(), request, &profile),
                  WireResult::Invalid);
        EXPECT_EQ(create_voice_command("create", vision_capability(), request, &profile),
                  WireResult::Invalid);
        EXPECT_TRUE(request.method.empty());
    }
}
TEST(VoiceVisionWire, TransportModesAndDiscreteInputsDoNotExpandVision) {
    const VoiceVisionProfile profile{"front", "vision-1", 1, true};
    for (unsigned index = 0; index < 8; ++index) {
        auto capability = vision_capability();
        switch (index) {
            case 0:
                capability.transport = VoiceTransport::Udp;
                break;
            case 1:
                capability.transport = VoiceTransport::None;
                break;
            case 2:
                capability.audio.sample_rate = 16000;
                break;
            case 3:
                capability.audio.frame_ms = 60;
                break;
            case 4:
                capability.modes.full_duplex = true;
                break;
            case 5:
                capability.text_input = true;
                break;
            case 6:
                capability.image_input = true;
                break;
            default:
                capability.file_input = true;
                break;
        }
        Envelope request;
        EXPECT_EQ(create_voice_command("create", capability, request, &profile),
                  WireResult::Invalid);
    }
}
TEST(VoiceVisionWire, StrictOriginalTracksRejectForgedSourceCodecDirectionAndState) {
    const VoiceVisionProfile profile{"front", "vision-1", 1, true};
    Envelope request;
    request.token = "create";
    const auto original = vision_created(request);
    const std::pair<const char*, const char*> cases[] = {
        {R"("sourceId":"front")", R"("sourceId":"back")"},
        {R"("trackId":"vision-1")", R"("trackId":"other")"},
        {R"("trackId":"vision-1")", R"("trackId":"mic-1")"},
        {R"("codec":"H264")", R"("codec":"VP8")"},
        {R"("state":"paused")", R"("state":"active")"},
        {R"("direction":"sendonly","kind":"video")", R"("direction":"recvonly","kind":"video")"},
        {R"("sourceId":"front")", R"("sourceId":"front","sourceType":"camera")"},
        {R"("sourceId":"mic")", R"("sourceId":"camera")"},
        {R"("transport":"webrtc")", R"("transport":"udp")"},
        {R"("sampleRate":48000)", R"("sampleRate":16000)"},
        {R"("selected":{)", R"("selected":{"cameraId":"front",)"},
        {R"("state":"negotiating")", R"("state":"negotiating","media":{})"}};
    for (const auto& entry : cases) {
        VoiceEvent event;
        EXPECT_EQ(decode_voice_event(WireRoute::Signaling,
                                     changed(original, entry.first, entry.second), event, &profile),
                  WireResult::Invalid);
        EXPECT_FALSE(event.media.ai_vision);
    }
    VoiceEvent event;
    EXPECT_EQ(decode_voice_event(WireRoute::Signaling, "{", event, &profile), WireResult::Invalid);
    EXPECT_EQ(decode_voice_event(WireRoute::Message, original, event, &profile),
              WireResult::WrongRoute);
}
TEST(VoiceVisionClient, InstallsPausedMediaOnlyOnceAndRejectsLateCreated) {
    VisionFixture fixture;
    ASSERT_EQ(fixture.client.connect_shared(0), Result::Applied);
    const auto reply = vision_created(fixture.publisher.messages.back());
    ASSERT_EQ(fixture.receive(reply), Result::Applied);
    EXPECT_EQ(fixture.media.ordinary_configs, 0U);
    EXPECT_EQ(fixture.media.vision_configs, 1U);
    EXPECT_TRUE(fixture.media.video_paused);
    EXPECT_EQ(fixture.media.captures, 0U);
    EXPECT_EQ(fixture.media.plays, 0U);
    EXPECT_EQ(fixture.receive(reply), Result::Stale);
    fixture.client.disconnected(2);
    // 终态不保留相机配置；旧三轨报文不再满足当前解析上下文，更不能重装驱动。
    EXPECT_EQ(fixture.receive(reply, 3), Result::Invalid);
    EXPECT_EQ(fixture.media.vision_configs, 1U);
}
TEST(VoiceVisionClient, InvalidSelectedDoesNotConfigureAndValidOriginalStillWorks) {
    VisionFixture fixture;
    ASSERT_EQ(fixture.client.connect_shared(0), Result::Applied);
    const auto reply = vision_created(fixture.publisher.messages.back());
    EXPECT_EQ(fixture.receive(changed(reply, R"("state":"paused")", R"("state":"active")")),
              Result::Invalid);
    EXPECT_EQ(fixture.media.vision_configs, 0U);
    EXPECT_EQ(fixture.receive(reply), Result::Applied);
    EXPECT_EQ(fixture.media.vision_configs, 1U);
}
TEST(VoiceVisionClient, ProfileChangeBeforeCapabilityAckFailsClosed) {
    VisionFixture fixture;
    ASSERT_EQ(fixture.client.connect(0), Result::Applied);
    const auto report = fixture.publisher.messages.back();
    fixture.media.profile.camera_id = "back";
    const auto reply = encoded_reply(
        report, "requestAck",
        R"({"requestMethod":"capabilityReport","result":{"acceptedRevision":1}})", "");
    EXPECT_EQ(fixture.receive(reply), Result::DriverFailed);
    EXPECT_EQ(fixture.client.state(), VoiceState::Failed);
    EXPECT_EQ(fixture.media.vision_configs, 0U);
    EXPECT_EQ(fixture.publisher.messages.size(), 1U);
}
TEST(VoiceVisionClient, ProfileChangeWhileNegotiatingClosesOriginalResources) {
    for (unsigned field = 0; field < 4; ++field) {
        VisionFixture fixture;
        fixture.negotiate();
        if (field == 0)
            fixture.media.profile.video_track = "other";
        if (field == 1)
            fixture.media.profile.max_publishers = 2;
        if (field == 2)
            fixture.media.profile.ready = false;
        if (field == 3)
            fixture.media.expose_profile = false;
        fixture.client.tick(2);
        EXPECT_EQ(fixture.client.state(), VoiceState::Failed);
        EXPECT_FALSE(fixture.media.configured);
        EXPECT_EQ(fixture.media.captures, 0U);
    }
}
TEST(VoiceVisionClient, DriverRejectionClosesWithoutAudioFallback) {
    VisionFixture fixture;
    fixture.media.vision_ok = false;
    ASSERT_EQ(fixture.client.connect_shared(0), Result::Applied);
    EXPECT_EQ(fixture.receive(vision_created(fixture.publisher.messages.back())),
              Result::DriverFailed);
    EXPECT_EQ(fixture.client.state(), VoiceState::Failed);
    EXPECT_EQ(fixture.media.ordinary_configs, 0U);
    EXPECT_FALSE(fixture.media.configured);
}
TEST(VoiceVisionClient, ExistingAudioAckAndPauseCannotStartVideoOrReviveLateCapture) {
    VisionFixture fixture;
    fixture.establish();
    EXPECT_EQ(fixture.media.captures, 0U);
    ASSERT_EQ(fixture.client.start_input(7), Result::Applied);
    const auto start = fixture.publisher.messages.back();
    ASSERT_EQ(fixture.client.stop_input(8), Result::Applied);
    const auto start_ack = encoded_reply(
        start, "requestAck", R"({"requestMethod":"audioStart","result":)" + start.payload + "}");
    EXPECT_EQ(fixture.receive(start_ack, 9), Result::Applied);
    EXPECT_EQ(fixture.media.captures, 0U);
    EXPECT_TRUE(fixture.media.video_paused);
}
TEST(VoiceVisionClient, VisualDisconnectCannotBorrowPureAudioRecoveryPermission) {
    VisionFixture fixture;
    fixture.establish();
    EXPECT_EQ(fixture.client.media_disconnected(7), Result::Unsupported);
    EXPECT_EQ(fixture.client.state(), VoiceState::Failed);
    EXPECT_EQ(fixture.media.recoveries, 0U);
    EXPECT_FALSE(fixture.media.configured);
}
TEST(VoiceVisionClient, ProfileMayChangeOnlyAfterOriginalTerminal) {
    VisionFixture fixture;
    fixture.negotiate();
    fixture.client.disconnected(2);
    fixture.media.profile.camera_id = "back";
    ASSERT_EQ(fixture.client.connect_shared(3), Result::Applied);
    EXPECT_NE(fixture.publisher.messages.back().payload.find(R"("cameraId":"back")"),
              std::string::npos);
    EXPECT_EQ(fixture.client.state(), VoiceState::Creating);
}
TEST(VoiceVisionWire, ExtraMissingDuplicateTracksAndMalformedEnvelopeAreRejected) {
    const VoiceVisionProfile profile{"front", "vision-1", 1, true};
    Envelope request;
    request.token = "create";
    const auto original = vision_created(request);
    const std::string video =
        R"({"codec":"H264","direction":"sendonly","kind":"video","sourceId":"front","state":"paused","trackId":"vision-1"})";
    const std::string cases[] = {changed(original, video, video + "," + video),
                                 changed(original, "," + video, ""),
                                 changed(original, R"("trackId":"vision-1")",
                                         R"("trackId":"vision-1","trackId":"vision-1")"),
                                 changed(original, R"("sourceId":"front")", R"("sourceId":null)"),
                                 changed(original, R"("state":"paused")",
                                         R"("state":"paused","url":"https://example.invalid/")")};
    for (const auto& body : cases) {
        VoiceEvent event;
        EXPECT_EQ(decode_voice_event(WireRoute::Signaling, body, event, &profile),
                  WireResult::Invalid);
        EXPECT_FALSE(event.media.ai_vision);
    }
}
TEST(VoiceVisionClient, UndeclaredFullModeDoesNotConfigureVisionOrClaimAEC) {
    VisionFixture fixture;
    ASSERT_EQ(fixture.client.connect_shared(0), Result::Applied);
    const auto reply = vision_created(fixture.publisher.messages.back());
    EXPECT_EQ(fixture.receive(changed(reply, "halfDuplex", "fullDuplex")), Result::Invalid);
    EXPECT_EQ(fixture.media.vision_configs, 0U);
    EXPECT_EQ(fixture.client.state(), VoiceState::Creating);
    EXPECT_EQ(fixture.receive(reply), Result::Applied);
}
TEST(VoiceVisionClient, MissingProfileCannotPublishEvenSharedCreate) {
    VisionFixture fixture;
    fixture.media.expose_profile = false;
    EXPECT_EQ(fixture.client.connect_shared(0), Result::Unsupported);
    EXPECT_TRUE(fixture.publisher.messages.empty());
    EXPECT_EQ(fixture.media.vision_configs, 0U);
}
TEST(VoiceVisionClient, OriginalCreateTimeoutCannotInstallLateMedia) {
    VisionFixture fixture;
    ASSERT_EQ(fixture.client.connect_shared(0), Result::Applied);
    const auto original = vision_created(fixture.publisher.messages.back());
    // 原设备Requests仍30秒转未知，平台10秒分配预算不等于设备端请求保留期限。
    fixture.client.tick(30001);
    EXPECT_EQ(fixture.client.state(), VoiceState::Failed);
    EXPECT_EQ(fixture.receive(original, 30002), Result::Invalid);
    EXPECT_EQ(fixture.media.vision_configs, 0U);
}
}  // namespace
}  // namespace ur::device
