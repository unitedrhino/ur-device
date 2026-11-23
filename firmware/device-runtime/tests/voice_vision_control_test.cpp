// 文件说明：同一AI视觉会话的发布/停止实际回执回归；模拟HAL只验证协议，不代表摄像头或模型E2E。
#include <gtest/gtest.h>
#include <vector>
#include "ur/device/voice_client.hpp"
#include "voice_client_optional_test_port.hpp"

namespace ur::device {
namespace {
// ControlPublisher 保存实际编码后的SIG上行，便于核原token/code及驱动受理不冒充实际完成。
struct ControlPublisher final : PublishPort {
    std::vector<Envelope> messages; // 原已鉴权设备上行报文。
    std::vector<std::string> bytes; // 原发布尝试字节，背压也须保留不改写。
    bool accepted = true;           // 可控Broker背压，不把发布受理当实际媒体。
    bool publish(std::string_view topic, std::string_view body) override {
        EXPECT_EQ(topic, "$thing/up/signaling/p/d");
        Envelope message;
        EXPECT_EQ(decode_envelope(body, message), DecodeResult::Ok);
        messages.push_back(std::move(message));
        bytes.emplace_back(body);
        return accepted;
    }
};
// ControlMedia 只安装原paused视觉；未覆写发布口时应明确unsupported，而不是假造active。
struct ControlMedia : VoiceMediaPort {
    VoiceVisionProfile profile{"front", "vision-1", 1, true}; // 原不可变实际配置。
    const VoiceVisionProfile* vision_profile() const override { return &profile; }
    bool configure(const VoiceMedia&) override { return false; }
    bool configure_vision(const VoiceMedia&, const VoiceVisionProfile&) override { return true; }
    VoiceVisionRequestResult result = VoiceVisionRequestResult::Unsupported; // 默认无发布驱动。
    std::vector<VoiceVisionControl> controls; // 实际HAL受理调用，重放不得新增。
    std::vector<VoiceVisionFact> facts;       // 可控实际事实；不按请求自动返回成功。
    std::vector<std::uint64_t> cancelled;     // 原撤意图请求，不能作为Stopped回执。
    std::size_t next_fact = 0;                // 只交接一次已形成的原事实。
    VoiceVisionRequestResult begin_vision_control(const VoiceVisionControl& control) override {
        controls.push_back(control);
        return result;
    }
    std::optional<VoiceVisionFact> take_vision_fact() override {
        return next_fact == facts.size() ? std::nullopt
                                         : std::optional<VoiceVisionFact>(facts[next_fact++]);
    }
    void cancel_vision_control(std::uint64_t generation) override {
        cancelled.push_back(generation);
    }
    std::uint32_t next_input_sequence() const override { return 128; }
    bool prepare_reply(const std::array<std::uint8_t, 8>&, std::uint32_t) override { return true; }
    bool finish_reply(std::uint32_t) override { return true; }
    bool apply_answer(std::string_view) override { return true; }
    bool start_capture(std::string_view) override { return true; }
    void stop_capture() override {}
    bool start_playback(std::string_view, std::uint64_t) override { return true; }
    void stop_playback(std::uint64_t) override {}
    void close() override {}
};
// control_capability 保留专用WebRTC48k/20ms半双工，不补造AEC或复用IPC相机许可。
VoiceCapability control_capability() {
    VoiceCapability value;
    value.transport = VoiceTransport::WebRTC;
    value.audio = {48000, 1, 20};
    value.modes.half_duplex = true;
    value.ai_vision = true;
    return value;
}
// ControlFixture 沿现有创建、双SDP与实际媒体确认建立原VoiceClient，不跳过会话状态机。
struct ControlFixture {
    ControlPublisher publisher;
    ControlMedia media;
    VoiceClient client{publisher,
                       media,
                       "$thing/up/signaling/p/d",
                       "$thing/down/signaling/p/d",
                       control_capability(),
                       "vision-control"};
    Result receive(std::string_view body, std::uint64_t now) {
        return client.receive({"$thing/down/signaling/p/d", body}, now);
    }
    // reply 复用公共信封编码，保留实际原请求token与独立下行应答方法。
    void reply(std::string method, std::string payload, std::uint64_t now) {
        Envelope value;
        value.token = publisher.messages.back().token;
        value.session = "vision-session";
        value.method = std::move(method);
        value.payload = std::move(payload);
        value.request = false;
        value.code = 200;
        std::string body;
        ASSERT_EQ(encode_envelope(value, body), DecodeResult::Ok);
        ASSERT_EQ(receive(body, now), Result::Applied);
    }
    void establish() {
        /* 视觉控制测试固定aiVision三轨形态：连接前显式请求，与本地语音会话解耦。 */
        client.request_vision_session(true);
        ASSERT_EQ(client.connect_shared(0), Result::Applied);
        reply(
            "sessionCreated",
            R"({"state":"negotiating","selected":{"transport":"webrtc","interactionMode":"halfDuplex","audioParams":{"format":"opus","sampleRate":48000,"channels":1,"frameDuration":20}},"tracks":[{"trackId":"mic-1","kind":"audio","sourceId":"mic","direction":"sendonly","codec":"opus","state":"requested"},{"trackId":"speaker-1","kind":"audio","sourceId":"speaker","direction":"recvonly","codec":"opus","state":"requested"},{"trackId":"vision-1","kind":"video","sourceId":"front","direction":"sendonly","codec":"H264","state":"paused"}]})",
            1);
        ASSERT_EQ(client.media_offer_ready("v=0\r\no=offer\r\n", 2), Result::Applied);
        reply("mediaAnswer",
              R"({"mediaRevision":1,"webrtc":{"iceMode":"complete","sdp":"v=0\r\no=answer\r\n"}})",
              3);
        ASSERT_EQ(client.media_ready(4), Result::Applied);
        reply("requestAck",
              R"({"requestMethod":"mediaState","result":{"state":"connected","mediaRevision":1}})",
              5);
        ASSERT_EQ(
            receive(
                R"({"msgToken":"established","method":"sessionEstablished","sessionId":"vision-session","eventSeq":1,"data":{"state":"established","mediaRevision":1}})",
                6),
            Result::Applied);
        ASSERT_EQ(client.state(), VoiceState::Established);
    }
};
// track_command 保留现S09原控制字段，不携同意、设备身份或媒体字节。
std::string track_command(const char* operation, const char* token = "original-control",
                          unsigned sequence = 2) {
    return std::string(R"({"msgToken":")") + token +
           R"(","method":"trackControl","sessionId":"vision-session","eventSeq":)" +
           std::to_string(sequence) + R"(,"params":{"trackId":"vision-1","operation":")" +
           operation + R"(","mediaRevision":1}})";
}
// changed使用原公共信封修改一个字段；用于证明同token或陈旧范围不会触发实际驱动。
std::string changed(std::string body, std::string session, std::string payload) {
    Envelope value;
    EXPECT_EQ(decode_envelope(body, value), DecodeResult::Ok);
    value.session = std::move(session);
    value.payload = std::move(payload);
    EXPECT_EQ(encode_envelope(value, body), DecodeResult::Ok);
    return body;
}
// actual将当前驱动原代次的真实结果交接；该测试桩不声称浏览器相机或模型真的完成。
void actual(ControlFixture& fixture, VoiceVisionState state,
            VoiceVisionFailure failure = VoiceVisionFailure::None, std::uint64_t now = 8) {
    ASSERT_FALSE(fixture.media.controls.empty());
    fixture.media.facts.push_back({fixture.media.controls.back().generation, state, failure});
    fixture.client.tick(now);
}

TEST(VoiceVisionControl, DefaultPublisherRejectsOriginalPublishWithoutEndingAudio) {
    ControlFixture fixture;
    fixture.establish();
    ASSERT_EQ(fixture.receive(track_command("publish"), 7), Result::Applied);
    const auto& receipt = fixture.publisher.messages.back();
    EXPECT_EQ(receipt.method, "trackState");
    EXPECT_EQ(receipt.token, "original-control");
    ASSERT_TRUE(receipt.code);
    EXPECT_EQ(*receipt.code, 422);
    EXPECT_EQ(receipt.payload, R"({"reason":"unsupportedCapability"})");
    EXPECT_FALSE(receipt.request);
    EXPECT_FALSE(receipt.event_sequence);
    EXPECT_EQ(fixture.client.state(), VoiceState::Established);
}

TEST(VoiceVisionControl, DirectMissingSequenceCannotDriveOrAdvanceWatermark) {
    ControlFixture fixture;
    fixture.establish();
    fixture.media.result = VoiceVisionRequestResult::Accepted;
    VoiceVisionCommand command;
    ASSERT_EQ(decode_voice_vision_control(WireRoute::Signaling, track_command("publish"), command),
              WireResult::Ok);
    command.envelope.event_sequence.reset();
    const auto published = fixture.publisher.messages.size();
    const auto sequence = VoiceClientOptionalTestPort::event_sequence(fixture.client);
    EXPECT_EQ(VoiceClientOptionalTestPort::vision_control(fixture.client, command, 7),
              Result::Invalid);
    EXPECT_TRUE(fixture.media.controls.empty());
    EXPECT_TRUE(fixture.media.cancelled.empty());
    EXPECT_EQ(fixture.publisher.messages.size(), published);
    EXPECT_EQ(VoiceClientOptionalTestPort::event_sequence(fixture.client), sequence);
    EXPECT_EQ(fixture.client.state(), VoiceState::Established);
    EXPECT_EQ(fixture.receive(track_command("publish"), 8), Result::Applied);
    EXPECT_EQ(fixture.media.controls.size(), std::size_t{1});
}

TEST(VoiceVisionControl, DirectMissingSequenceCannotConsumeQueuedActualFact) {
    ControlFixture fixture;
    fixture.establish();
    fixture.media.result = VoiceVisionRequestResult::Accepted;
    ASSERT_EQ(fixture.receive(track_command("publish"), 7), Result::Applied);
    ASSERT_EQ(fixture.media.controls.size(), std::size_t{1});
    fixture.media.facts.push_back({fixture.media.controls.back().generation,
                                   VoiceVisionState::Active});
    VoiceVisionCommand command;
    ASSERT_EQ(decode_voice_vision_control(WireRoute::Signaling,
                                        track_command("stop", "missing-sequence", 3), command),
              WireResult::Ok);
    command.envelope.event_sequence.reset();
    const auto published = fixture.publisher.messages.size();
    const auto sequence = VoiceClientOptionalTestPort::event_sequence(fixture.client);
    EXPECT_EQ(VoiceClientOptionalTestPort::vision_control(fixture.client, command, 8),
              Result::Invalid);
    EXPECT_EQ(fixture.media.next_fact, std::size_t{0});
    EXPECT_EQ(fixture.media.controls.size(), std::size_t{1});
    EXPECT_TRUE(fixture.media.cancelled.empty());
    EXPECT_EQ(fixture.publisher.messages.size(), published);
    EXPECT_EQ(VoiceClientOptionalTestPort::event_sequence(fixture.client), sequence);
    EXPECT_EQ(fixture.client.state(), VoiceState::Established);
    fixture.client.tick(9);
    EXPECT_EQ(fixture.media.next_fact, std::size_t{1});
    ASSERT_EQ(fixture.publisher.messages.size(), published + 1);
    EXPECT_EQ(fixture.publisher.messages.back().code, 200);
}
TEST(VoiceVisionControl, DefaultPublisherRejectsOriginalStopWithoutFabricatingStopped) {
    ControlFixture fixture;
    fixture.establish();
    ASSERT_EQ(fixture.receive(track_command("stop"), 7), Result::Applied);
    const auto& receipt = fixture.publisher.messages.back();
    EXPECT_EQ(receipt.method, "trackState");
    ASSERT_TRUE(receipt.code);
    EXPECT_EQ(*receipt.code, 422);
    EXPECT_EQ(receipt.payload, R"({"reason":"unsupportedCapability"})");
    EXPECT_EQ(fixture.client.state(), VoiceState::Established);
}
TEST(VoiceVisionControl, AcceptedPublishIsNotActualUntilOriginalFact) {
    ControlFixture fixture;
    fixture.establish();
    fixture.media.result = VoiceVisionRequestResult::Accepted;
    const auto before = fixture.publisher.messages.size();
    ASSERT_EQ(fixture.receive(track_command("publish"), 7), Result::Applied);
    EXPECT_EQ(fixture.publisher.messages.size(), before);
    ASSERT_EQ(fixture.media.controls.size(), 1U);
    const auto& request = fixture.media.controls.front();
    EXPECT_GT(request.generation, 0U);
    EXPECT_EQ(request.session_id, "vision-session");
    EXPECT_EQ(request.camera_id, "front");
    EXPECT_EQ(request.track_id, "vision-1");
    EXPECT_EQ(request.media_revision, 1U);
    EXPECT_EQ(request.operation, VoiceVisionOperation::Publish);
    actual(fixture, VoiceVisionState::Active);
    const auto& receipt = fixture.publisher.messages.back();
    EXPECT_EQ(receipt.method, "trackState");
    EXPECT_EQ(receipt.code, 200);
    EXPECT_EQ(receipt.payload, R"({"mediaRevision":1,"state":"active","trackId":"vision-1"})");
    EXPECT_FALSE(receipt.event_sequence);
    EXPECT_FALSE(receipt.message_sequence);
    EXPECT_EQ(fixture.client.state(), VoiceState::Established);
}
TEST(VoiceVisionControl, StopPreemptsPendingPublishButRequiresActualStopped) {
    ControlFixture fixture;
    fixture.establish();
    fixture.media.result = VoiceVisionRequestResult::Accepted;
    ASSERT_EQ(fixture.receive(track_command("publish"), 7), Result::Applied);
    const auto old = fixture.media.controls.back().generation;
    ASSERT_EQ(fixture.receive(track_command("stop", "stop-control", 3), 8), Result::Applied);
    ASSERT_EQ(fixture.media.controls.size(), 2U);
    ASSERT_EQ(fixture.media.cancelled.size(), 1U);
    EXPECT_EQ(fixture.media.cancelled.front(), old);
    EXPECT_EQ(fixture.publisher.messages.back().token, "original-control");
    EXPECT_EQ(fixture.publisher.messages.back().code, 503);
    EXPECT_EQ(fixture.publisher.messages.back().payload, R"({"reason":"deviceRejected"})");
    const auto before = fixture.publisher.messages.size();
    fixture.media.facts.push_back({old, VoiceVisionState::Active});
    fixture.client.tick(9);
    EXPECT_EQ(fixture.publisher.messages.size(), before);
    actual(fixture, VoiceVisionState::Stopped, VoiceVisionFailure::None, 10);
    EXPECT_EQ(fixture.publisher.messages.back().token, "stop-control");
    EXPECT_EQ(fixture.publisher.messages.back().payload,
              R"({"mediaRevision":1,"state":"stopped","trackId":"vision-1"})");
    EXPECT_EQ(fixture.client.state(), VoiceState::Established);
}
TEST(VoiceVisionControl, QueuedActualPublishSurvivesLaterStopAsHistoricalSuccess) {
    ControlFixture fixture;
    fixture.establish();
    fixture.media.result = VoiceVisionRequestResult::Accepted;
    ASSERT_EQ(fixture.receive(track_command("publish"), 7), Result::Applied);
    fixture.media.facts.push_back(
        {fixture.media.controls.back().generation, VoiceVisionState::Active});
    ASSERT_EQ(fixture.receive(track_command("stop", "stop-control", 3), 8), Result::Applied);
    EXPECT_EQ(fixture.publisher.messages.back().code, 200);
    EXPECT_EQ(fixture.publisher.messages.back().token, "original-control");
    actual(fixture, VoiceVisionState::Stopped, VoiceVisionFailure::None, 9);
    const auto count = fixture.media.controls.size();
    EXPECT_EQ(fixture.receive(track_command("publish"), 10), Result::Duplicate);
    EXPECT_EQ(fixture.publisher.messages.back().token, "original-control");
    EXPECT_EQ(fixture.publisher.messages.back().code, 200);
    EXPECT_EQ(fixture.media.controls.size(), count);
}
TEST(VoiceVisionControl, CompletedReplayKeepsExactBytesAndDoesNotExecuteAgain) {
    ControlFixture fixture;
    fixture.establish();
    fixture.media.result = VoiceVisionRequestResult::Accepted;
    ASSERT_EQ(fixture.receive(track_command("publish"), 7), Result::Applied);
    actual(fixture, VoiceVisionState::Active);
    const auto bytes = fixture.publisher.bytes.back();
    EXPECT_EQ(fixture.receive(track_command("publish"), 9), Result::Duplicate);
    EXPECT_EQ(fixture.publisher.bytes.back(), bytes);
    EXPECT_EQ(fixture.media.controls.size(), 1U);
}
TEST(VoiceVisionControl, PendingReplayDoesNotExtendFixedTenSecondDeadline) {
    ControlFixture fixture;
    fixture.establish();
    fixture.media.result = VoiceVisionRequestResult::Accepted;
    ASSERT_EQ(fixture.receive(track_command("publish"), 7), Result::Applied);
    EXPECT_EQ(fixture.receive(track_command("publish"), 9000), Result::Duplicate);
    EXPECT_EQ(fixture.media.controls.size(), 1U);
    actual(fixture, VoiceVisionState::Active, VoiceVisionFailure::None, 10007);
    EXPECT_EQ(fixture.publisher.messages.back().code, 504);
    EXPECT_EQ(fixture.publisher.messages.back().payload, R"({"reason":"operationTimedOut"})");
    const auto bytes = fixture.publisher.bytes.back();
    actual(fixture, VoiceVisionState::Active, VoiceVisionFailure::None, 10008);
    EXPECT_EQ(fixture.publisher.bytes.back(), bytes);
    EXPECT_EQ(fixture.client.state(), VoiceState::Established);
}
TEST(VoiceVisionControl, SameTokenChangedParamsCannotBecomeNewPublish) {
    ControlFixture fixture;
    fixture.establish();
    fixture.media.result = VoiceVisionRequestResult::Accepted;
    ASSERT_EQ(fixture.receive(track_command("publish"), 7), Result::Applied);
    EXPECT_EQ(fixture.receive(track_command("stop", "original-control", 3), 8), Result::Applied);
    EXPECT_EQ(fixture.media.controls.size(), 1U);
    EXPECT_TRUE(fixture.media.cancelled.empty());
    EXPECT_EQ(fixture.publisher.messages.back().code, 409);
    EXPECT_EQ(fixture.publisher.messages.back().payload, R"({"reason":"tokenConflict"})");
}
TEST(VoiceVisionControl, WrongSessionTrackRevisionAndOldSequenceDoNotTouchDriver) {
    ControlFixture fixture;
    fixture.establish();
    fixture.media.result = VoiceVisionRequestResult::Accepted;
    EXPECT_EQ(fixture.receive(
                  changed(track_command("publish"), "old-session",
                          R"({"trackId":"vision-1","operation":"publish","mediaRevision":1})"),
                  7),
              Result::Stale);
    for (const auto* payload :
         {R"({"trackId":"speaker-1","operation":"publish","mediaRevision":1})",
          R"({"trackId":"vision-1","operation":"publish","mediaRevision":2})"}) {
        EXPECT_EQ(fixture.receive(changed(track_command("publish"), "vision-session", payload), 7),
                  Result::Stale);
    }
    EXPECT_EQ(fixture.receive(track_command("publish", "old-sequence", 1), 7), Result::Stale);
    EXPECT_TRUE(fixture.media.controls.empty());
}
TEST(VoiceVisionControl, BackpressurePreservesOriginalActualAndNeverRestartsCamera) {
    ControlFixture fixture;
    fixture.establish();
    fixture.media.result = VoiceVisionRequestResult::Accepted;
    ASSERT_EQ(fixture.receive(track_command("publish"), 7), Result::Applied);
    fixture.publisher.accepted = false;
    actual(fixture, VoiceVisionState::Active);
    const auto original = fixture.publisher.bytes.back();
    fixture.client.tick(9);
    EXPECT_EQ(fixture.publisher.bytes.back(), original);
    fixture.publisher.accepted = true;
    fixture.client.tick(10);
    EXPECT_EQ(fixture.publisher.bytes.back(), original);
    EXPECT_EQ(fixture.media.controls.size(), 1U);
}
TEST(VoiceVisionControl, MalformedFutureAndWrongOperationFactsCannotReportActive) {
    ControlFixture fixture;
    fixture.establish();
    fixture.media.result = VoiceVisionRequestResult::Accepted;
    ASSERT_EQ(fixture.receive(track_command("stop"), 7), Result::Applied);
    const auto before = fixture.publisher.messages.size();
    fixture.media.facts.push_back(
        {fixture.media.controls.back().generation + 1, VoiceVisionState::Stopped});
    actual(fixture, VoiceVisionState::Active);
    actual(fixture, VoiceVisionState::Stopped, VoiceVisionFailure::Busy, 9);
    actual(fixture, VoiceVisionState::Failed, VoiceVisionFailure::None, 10);
    EXPECT_EQ(fixture.publisher.messages.size(), before);
    actual(fixture, VoiceVisionState::Stopped, VoiceVisionFailure::None, 11);
    EXPECT_EQ(fixture.publisher.messages.back().code, 200);
}
TEST(VoiceVisionControl, DriverFailuresUseOnlyFixedExistingErrorClasses) {
    const std::pair<VoiceVisionFailure, std::string_view> cases[] = {
        {VoiceVisionFailure::Unsupported, "unsupportedCapability"},
        {VoiceVisionFailure::Busy, "resourceBusy"},
        {VoiceVisionFailure::MediaFailed, "mediaFailed"},
        {VoiceVisionFailure::Timeout, "operationTimedOut"},
        {VoiceVisionFailure::Cancelled, "deviceRejected"}};
    for (const auto& value : cases) {
        ControlFixture fixture;
        fixture.establish();
        fixture.media.result = VoiceVisionRequestResult::Accepted;
        ASSERT_EQ(fixture.receive(track_command("publish"), 7), Result::Applied);
        actual(fixture, VoiceVisionState::Failed, value.first);
        EXPECT_EQ(fixture.publisher.messages.back().payload,
                  "{\"reason\":\"" + std::string(value.second) + "\"}");
        EXPECT_EQ(fixture.media.cancelled.size(), 1U);
        EXPECT_EQ(fixture.client.state(), VoiceState::Established);
    }
}
TEST(VoiceVisionControl, BusyDoesNotReplaceOriginalPendingStop) {
    ControlFixture fixture;
    fixture.establish();
    fixture.media.result = VoiceVisionRequestResult::Accepted;
    ASSERT_EQ(fixture.receive(track_command("stop"), 7), Result::Applied);
    EXPECT_EQ(fixture.receive(track_command("stop", "another-stop", 3), 8), Result::Applied);
    EXPECT_EQ(fixture.media.controls.size(), 1U);
    EXPECT_EQ(fixture.publisher.messages.back().code, 409);
    EXPECT_TRUE(fixture.media.cancelled.empty());
    actual(fixture, VoiceVisionState::Stopped, VoiceVisionFailure::None, 9);
    EXPECT_EQ(fixture.publisher.messages.back().token, "original-control");
    EXPECT_EQ(fixture.publisher.messages.back().code, 200);
}
TEST(VoiceVisionControl, ClockRollbackCancelsPendingButDoesNotAuthorizeNewPublish) {
    ControlFixture fixture;
    fixture.establish();
    fixture.media.result = VoiceVisionRequestResult::Accepted;
    ASSERT_EQ(fixture.receive(track_command("publish"), 10), Result::Applied);
    actual(fixture, VoiceVisionState::Active, VoiceVisionFailure::None, 9);
    EXPECT_EQ(fixture.publisher.messages.back().code, 503);
    EXPECT_EQ(fixture.media.cancelled.size(), 1U);
    EXPECT_EQ(fixture.receive(track_command("publish", "new-publish", 3), 11), Result::Applied);
    EXPECT_EQ(fixture.publisher.messages.back().code, 409);
    EXPECT_EQ(fixture.media.controls.size(), 1U);
}
TEST(VoiceVisionControl, FullHistoryPreservesOriginalAndReservesFinalStopSlot) {
    ControlFixture fixture;
    fixture.establish();
    for (unsigned index = 0; index < 31; ++index) {
        const auto token = "publish-" + std::to_string(index);
        EXPECT_EQ(fixture.receive(track_command("publish", token.c_str(), index + 2), index + 7),
                  Result::Applied);
    }
    EXPECT_EQ(fixture.media.controls.size(), 31U);
    const auto first = fixture.publisher.bytes.front();
    EXPECT_EQ(fixture.receive(track_command("publish", "overflow", 33), 38), Result::Busy);
    EXPECT_EQ(fixture.media.controls.size(), 31U);
    EXPECT_EQ(fixture.receive(track_command("stop", "reserved-stop", 34), 39), Result::Applied);
    EXPECT_EQ(fixture.media.controls.size(), 32U);
    EXPECT_EQ(fixture.receive(track_command("publish", "publish-0", 2), 40), Result::Duplicate);
    EXPECT_EQ(fixture.publisher.messages.back().token, "publish-0");
    EXPECT_EQ(fixture.publisher.messages.back().code, 422);
    EXPECT_EQ(fixture.receive(track_command("stop", "overflow-stop", 35), 41), Result::Busy);
    EXPECT_EQ(fixture.media.controls.size(), 32U);
    EXPECT_FALSE(first.empty());
}
TEST(VoiceVisionControl, EndCancelsOriginalIntentAndLateFactsCannotRestartOrAck) {
    ControlFixture fixture;
    fixture.establish();
    fixture.media.result = VoiceVisionRequestResult::Accepted;
    ASSERT_EQ(fixture.receive(track_command("publish"), 7), Result::Applied);
    const auto generation = fixture.media.controls.back().generation;
    ASSERT_EQ(fixture.client.end(8), Result::Applied);
    const auto end_token = fixture.publisher.messages.back().token;
    EXPECT_EQ(fixture.media.cancelled.front(), generation);
    fixture.media.facts.push_back({generation, VoiceVisionState::Active});
    fixture.client.tick(9);
    EXPECT_EQ(fixture.client.state(), VoiceState::Ending);
    EXPECT_EQ(fixture.receive(track_command("publish", "new", 3), 10), Result::Stale);
    EXPECT_EQ(fixture.media.controls.size(), 1U);
    Envelope end_reply;
    end_reply.token = end_token;
    end_reply.session = "vision-session";
    end_reply.method = "sessionEnded";
    end_reply.request = false;
    end_reply.code = 200;
    end_reply.payload = R"({"state":"ended","reason":"userStopped"})";
    std::string end_body;
    ASSERT_EQ(encode_envelope(end_reply, end_body), DecodeResult::Ok);
    ASSERT_EQ(fixture.receive(end_body, 11), Result::Applied);
    const auto before = fixture.publisher.messages.size();
    fixture.client.tick(12);
    EXPECT_EQ(fixture.publisher.messages.size(), before);
    EXPECT_EQ(fixture.receive(track_command("publish"), 13), Result::Invalid);
}
TEST(VoiceVisionWire, RejectsWrongRouteDirectionExtraFieldsAndUnsupportedOperation) {
    VoiceVisionCommand parsed;
    EXPECT_EQ(decode_voice_vision_control(WireRoute::Message, track_command("publish"), parsed),
              WireResult::WrongRoute);
    for (const auto* payload :
         {R"({"trackId":"vision-1","operation":"publish","mediaRevision":0})",
          R"({"trackId":"vision-1","operation":"publish","mediaRevision":1,"consentId":"fake"})",
          R"({"trackId":"vision-1","operation":"publish","mediaRevision":"1"})"}) {
        EXPECT_EQ(decode_voice_vision_control(
                      WireRoute::Signaling,
                      changed(track_command("publish"), "vision-session", payload), parsed),
                  WireResult::Invalid);
    }
    EXPECT_EQ(
        decode_voice_vision_control(
            WireRoute::Signaling,
            R"({"msgToken":"control","method":"trackControl","sessionId":"vision-session","eventSeq":2,"params":{"trackId":"vision-1","operation":"publish","mediaRevision":9007199254740992}})",
            parsed),
        WireResult::Invalid);
    EXPECT_EQ(decode_voice_vision_control(WireRoute::Signaling, track_command("resume"), parsed),
              WireResult::Unsupported);
    Envelope message;
    ASSERT_EQ(decode_envelope(track_command("publish"), message), DecodeResult::Ok);
    message.code = 200;
    std::string body;
    EXPECT_EQ(encode_envelope(message, body), DecodeResult::InvalidEnvelope);
    body =
        R"({"msgToken":"control","method":"trackControl","sessionId":"vision-session","eventSeq":2,"code":200,"params":{"trackId":"vision-1","operation":"publish","mediaRevision":1}})";
    EXPECT_EQ(decode_voice_vision_control(WireRoute::Signaling, body, parsed), WireResult::Invalid);
    message.code.reset();
    message.request = false;
    EXPECT_EQ(encode_envelope(message, body), DecodeResult::Ok);
    body =
        R"({"msgToken":"control","method":"trackControl","sessionId":"vision-session","eventSeq":2,"data":{"trackId":"vision-1","operation":"publish","mediaRevision":1}})";
    EXPECT_EQ(decode_voice_vision_control(WireRoute::Signaling, body, parsed), WireResult::Invalid);
}
TEST(VoiceVisionWire, ReceiptRejectsFabricatedStoppedFreeReasonAndModifiedTypedTrack) {
    VoiceVisionCommand command;
    ASSERT_EQ(decode_voice_vision_control(WireRoute::Signaling, track_command("publish"), command),
              WireResult::Ok);
    Envelope receipt;
    EXPECT_EQ(voice_vision_receipt(command, 200, "stopped", {}, receipt), WireResult::Invalid);
    EXPECT_EQ(voice_vision_receipt(command, 503, {}, "private driver detail", receipt),
              WireResult::Invalid);
    command.track_id = "other";
    EXPECT_EQ(voice_vision_receipt(command, 200, "active", {}, receipt), WireResult::Invalid);
}
} // namespace
} // namespace ur::device
