// 文件说明：同一AI会话控制重连的真实核心组合回归；虚拟HAL核旧输入不重放及用户停止不复活。
#include <gtest/gtest.h>
#include <algorithm>
#include "ur/device/voice_client.hpp"
#include "voice_client_optional_test_port.hpp"

namespace ur::device {
namespace {
// ControlPublisher 保存实际协议字节的解析结果，不绕过Requests或VoiceClient关联。
struct ControlPublisher final : PublishPort {
    std::vector<Envelope> sent;  // 当前事件线程发出的有界测试请求。
    bool publish(std::string_view, std::string_view bytes) override {
        Envelope envelope;
        EXPECT_EQ(decode_envelope(bytes, envelope), DecodeResult::Ok);
        sent.push_back(std::move(envelope));
        return true;
    }
};
// ControlMedia 核实际采集/播放开关及输入ID；纯控制恢复不能configure第二个媒体会话。
struct ControlMedia final : VoiceMediaPort {
    bool udp_reply_start_receipt_ready() const override {
        return true;
    }  // 新夹具仅声明同步HAL路径。
    bool capture = false, playback = false;                 // HAL已接受的实态。
    unsigned configurations = 0, captures = 0, closes = 0;  // 禁止恢复时重建或重复采集。
    std::string input;                                      // 实际采集使用的新旧ID对照。
    bool configure(const VoiceMedia&) override {
        ++configurations;
        return true;
    }
    std::uint32_t next_input_sequence() const override { return 128; }
    bool prepare_reply(const std::array<std::uint8_t, 8>&, std::uint32_t) override { return true; }
    bool finish_reply(std::uint32_t) override { return true; }
    bool start_capture(std::string_view value) override {
        input = value;
        ++captures;
        capture = true;
        return true;
    }
    void stop_capture() override { capture = false; }
    bool start_playback(std::string_view, std::uint64_t) override {
        playback = true;
        return true;
    }
    void stop_playback(std::uint64_t) override { playback = false; }
    void close() override {
        ++closes;
        capture = playback = false;
    }
};
// control_capability 明确本端真实实现同步；不改变旧构造默认能力。
VoiceCapability control_capability(bool receipt = false) {
    VoiceCapability capability;
    capability.modes = {true, true, true};
    capability.audio = {16000, 1, 20};
    capability.session_sync = true;
    capability.udp_reply_start_receipt = receipt;
    return capability;
}
// response 使用原生产公共编码器构造平台应答，token只取实际已发送请求。
std::string response(const Envelope& request, std::string method, std::string data,
                     std::string session = {}, int code = 200) {
    Envelope reply;
    reply.token = request.token;
    reply.method = std::move(method);
    reply.session = std::move(session);
    reply.request = false;
    reply.code = code;
    reply.payload = std::move(data);
    std::string encoded;
    EXPECT_EQ(encode_envelope(reply, encoded), DecodeResult::Ok);
    return encoded;
}
// ack 使用原字段级关联，不将任意code200当作已执行。
std::string ack(const Envelope& request, std::string result) {
    return response(request, "requestAck",
                    "{\"requestMethod\":\"" + request.method + "\",\"result\":" + result + "}",
                    request.session);
}
// ClientControlFixture 建立一个原UDP全双工会话，所有创建和开麦均走原ACK门控。
struct ClientControlFixture : ::testing::Test {
    ControlPublisher publisher;  // 端口先构造，最后析构。
    ControlMedia media;          // 被VoiceClient借用。
    const bool receipt;          // 默认旧控制恢复矩阵不声明本次新能力。
    VoiceClient client;
    explicit ClientControlFixture(bool enabled = false)
        : receipt(enabled),
          client{publisher,
                 media,
                 "$thing/up/signaling/p/d",
                 "$thing/down/signaling/p/d",
                 control_capability(enabled),
                 "control-instance"} {}
    Result receive(std::string body, std::uint64_t now, std::uint64_t wall = 10000) {
        if (receipt && body.find("\"method\":\"sessionCreated\"") != std::string::npos)
            body.insert(body.find("\"interactionMode\""), "\"udpReplyStartReceipt\":true,");
        return client.receive({"$thing/down/signaling/p/d", body}, now, wall);
    }
    void establish() {
        ASSERT_EQ(client.connect(0), Result::Applied);
        ASSERT_EQ(receive(ack(publisher.sent.back(), R"({"acceptedRevision":1})"), 1),
                  Result::Applied);
        const auto request = publisher.sent.back();
        ASSERT_EQ(
            receive(
                response(
                    request, "sessionCreated",
                    R"({"state":"negotiating","endpointEpoch":1,"capabilityRevision":1,"selected":{"transport":"udp","interactionMode":"fullDuplex","audioParams":{"format":"opus","sampleRate":16000,"channels":1,"frameDuration":20}},"tracks":[{"trackId":"mic-1","kind":"audio","sourceId":"mic","direction":"sendonly","codec":"opus","state":"requested"},{"trackId":"speaker-1","kind":"audio","sourceId":"speaker","direction":"recvonly","codec":"opus","state":"requested"}],"media":{"udp":{"server":"127.0.0.1","port":9000,"key":"00000000000000000000000000000000","nonce":"01000000000000000000000000000000"}}})",
                    "original-ai"),
                2),
            Result::Applied);
        ASSERT_EQ(client.media_ready(3), Result::Applied);
        ASSERT_EQ(
            receive(ack(publisher.sent.back(), R"({"mediaRevision":1,"state":"connected"})"), 4),
            Result::Applied);
        Envelope event;
        event.token = "established";
        event.session = "original-ai";
        event.method = "sessionEstablished";
        event.request = false;
        event.event_sequence = 1;
        event.payload = R"({"state":"established","mediaRevision":1})";
        std::string body;
        ASSERT_EQ(encode_envelope(event, body), DecodeResult::Ok);
        ASSERT_EQ(receive(body, 5), Result::Applied);
    }
    void start() {
        ASSERT_EQ(client.start_input(6), Result::Applied);
        ASSERT_EQ(receive(ack(publisher.sent.back(), publisher.sent.back().payload), 7),
                  Result::Applied);
        ASSERT_TRUE(media.capture);
    }
    std::string snapshot(const Envelope& request) {
        auto body = response(
            request, "sessionSnapshot",
            R"({"state":"established","revision":8,"eventWatermark":9,"messageWatermark":7,"endpointEpoch":2,"capabilityRevision":1,"mediaRevision":1,"expiresAt":20000,"resumeAllowed":true,"participants":[],"tracks":[{"trackId":"mic-1","kind":"audio","sourceId":"mic","direction":"sendonly","codec":"opus","state":"paused"},{"trackId":"speaker-1","kind":"audio","sourceId":"speaker","direction":"recvonly","codec":"opus","state":"paused"}],"controls":{"call":null,"focus":null,"floor":null,"recordings":[],"playbacks":[],"processors":[],"broadcasts":[],"handoffs":[],"mediaPlan":{"revision":1,"tracks":[{"trackId":"mic-1","kind":"audio","sourceId":"mic","direction":"sendonly","codec":"opus","state":"paused"},{"trackId":"speaker-1","kind":"audio","sourceId":"speaker","direction":"recvonly","codec":"opus","state":"paused"}]},"ai":{"inputId":"retired-input","inputState":"paused","activeRespIds":[]}}})",
            "original-ai");
        Envelope envelope;
        EXPECT_EQ(decode_envelope(body, envelope), DecodeResult::Ok);
        envelope.event_sequence = 9;
        EXPECT_EQ(encode_envelope(envelope, body), DecodeResult::Ok);
        return body;
    }
};

TEST_F(ClientControlFixture, KeepsBusinessSessionAndOnlyFreshAcknowledgedInputResumes) {
    establish();
    start();
    ASSERT_TRUE(client.control_sync_enabled());
    const auto original_input = media.input;
    const auto old_start = publisher.sent.back();
    ASSERT_EQ(client.control_disconnected(8, 7), Result::Applied);
    EXPECT_FALSE(media.capture);
    EXPECT_FALSE(media.playback);
    EXPECT_TRUE(client.control_recovering());
    EXPECT_EQ(client.session_id(), "original-ai");
    const auto count = publisher.sent.size();
    client.tick(10008);
    EXPECT_EQ(publisher.sent.size(), count);
    ASSERT_EQ(client.control_reconnected(10009), Result::Applied);
    EXPECT_EQ(publisher.sent.back().method, "capabilityReport");
    ASSERT_EQ(receive(ack(publisher.sent.back(), R"({"acceptedRevision":1})"), 10010),
              Result::Applied);
    ASSERT_EQ(publisher.sent.back().method, "sessionSync");
    ASSERT_EQ(receive(snapshot(publisher.sent.back()), 10011), Result::Applied);
    EXPECT_FALSE(client.control_recovering());
    EXPECT_EQ(client.session_id(), "original-ai");
    EXPECT_EQ(media.configurations, 1U);
    EXPECT_EQ(publisher.sent.back().method, "audioStart");
    EXPECT_FALSE(media.capture);
    EXPECT_EQ(receive(ack(old_start, old_start.payload), 10012), Result::Stale);
    const auto fresh = publisher.sent.back();
    EXPECT_NE(fresh.payload, old_start.payload);
    ASSERT_EQ(receive(ack(fresh, fresh.payload), 10013), Result::Applied);
    EXPECT_TRUE(media.capture);
    EXPECT_NE(media.input, original_input);
    EXPECT_EQ(media.captures, 2U);
}

TEST_F(ClientControlFixture, MissingSessionRecoveryRejectsWithoutRestartingCapture) {
    establish();
    start();
    const auto captures = media.captures;
    const auto configurations = media.configurations;
    const auto published = publisher.sent.size();
    const auto sequence = VoiceClientOptionalTestPort::event_sequence(client);
    EXPECT_EQ(VoiceClientOptionalTestPort::recovering_without_session(client),
              Result::Unsupported);
    EXPECT_EQ(client.state(), VoiceState::Recovering);
    EXPECT_FALSE(client.capturing());
    EXPECT_FALSE(media.capture);
    EXPECT_FALSE(media.playback);
    EXPECT_EQ(media.captures, captures);
    EXPECT_EQ(media.configurations, configurations);
    EXPECT_EQ(publisher.sent.size(), published);
    EXPECT_EQ(VoiceClientOptionalTestPort::event_sequence(client), sequence);
}

TEST_F(ClientControlFixture, ExplicitOnlineSnapshotConflictRetriesWithinOriginalRecoveryWindow) {
    establish();
    ASSERT_EQ(client.control_disconnected(8, 5), Result::Applied);
    ASSERT_EQ(client.control_reconnected(9), Result::Applied);
    const auto original_report = publisher.sent.back();
    auto rejected =
        response(original_report, "requestAck",
                 R"({"requestMethod":"capabilityReport","reason":"capabilityNotAccepted"})");
    const auto code = rejected.find("\"code\":200");
    ASSERT_NE(code, std::string::npos);
    rejected.replace(code, 10, "\"code\":409");
    ASSERT_EQ(receive(rejected, 10), Result::Applied);
    ASSERT_TRUE(client.control_recovering());
    const auto count = publisher.sent.size();
    client.tick(509);
    EXPECT_EQ(publisher.sent.size(), count);
    client.tick(510);
    ASSERT_EQ(publisher.sent.size(), count + 1);
    const auto retried = publisher.sent.back();
    EXPECT_EQ(retried.method, "capabilityReport");
    EXPECT_NE(retried.token, original_report.token);
    EXPECT_EQ(retried.payload, original_report.payload);
    ASSERT_EQ(receive(ack(retried, R"({"acceptedRevision":1})"), 511), Result::Applied);
    ASSERT_EQ(publisher.sent.back().method, "sessionSync");
    EXPECT_EQ(receive(snapshot(publisher.sent.back()), 512), Result::Applied);
    EXPECT_EQ(client.session_id(), "original-ai");
    EXPECT_EQ(std::count_if(publisher.sent.begin(), publisher.sent.end(),
                            [](const auto& request) { return request.method == "sessionCreate"; }),
              1);
}

TEST_F(ClientControlFixture, RecoveryCapabilityConflictHasFixedAttemptsAndCannotSurviveOffline) {
    establish();
    ASSERT_EQ(client.control_disconnected(8, 5), Result::Applied);
    ASSERT_EQ(client.control_reconnected(9), Result::Applied);
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        const auto time = 10U + attempt * 501U;
        const auto rejected = response(
            publisher.sent.back(), "requestAck",
            R"({"requestMethod":"capabilityReport","reason":"capabilityNotAccepted"})", {}, 409);
        EXPECT_EQ(receive(rejected, time), attempt < 2 ? Result::Applied : Result::Unsupported);
        if (attempt < 2)
            client.tick(time + 500);
    }
    EXPECT_EQ(client.state(), VoiceState::Failed);
    EXPECT_TRUE(client.session_id().empty());
    const auto count = publisher.sent.size();
    client.tick(15000);
    EXPECT_EQ(publisher.sent.size(), count);
}

TEST_F(ClientControlFixture, SecondPhysicalLossCancelsOldBackoffWithoutExtendingDeadline) {
    establish();
    ASSERT_EQ(client.control_disconnected(8, 5), Result::Applied);
    ASSERT_EQ(client.control_reconnected(9), Result::Applied);
    const auto rejected = response(
        publisher.sent.back(), "requestAck",
        R"({"requestMethod":"capabilityReport","reason":"capabilityNotAccepted"})", {}, 409);
    ASSERT_EQ(receive(rejected, 10), Result::Applied);
    EXPECT_EQ(client.control_disconnected(11, 5), Result::Duplicate);
    const auto count = publisher.sent.size();
    client.tick(510);
    EXPECT_EQ(publisher.sent.size(), count);  // 尚未实际重新鉴权，旧退避不能离线publish。
    ASSERT_EQ(client.control_reconnected(15007), Result::Applied);
    client.tick(15008);
    EXPECT_EQ(client.state(), VoiceState::Failed);
    EXPECT_TRUE(client.session_id().empty());
}

TEST_F(ClientControlFixture, UserStopDuringRecoveryNeverReopensCapture) {
    establish();
    start();
    ASSERT_EQ(client.control_disconnected(8, 0), Result::Applied);
    EXPECT_EQ(client.stop_input(9), Result::Applied);
    ASSERT_EQ(client.control_reconnected(10), Result::Applied);
    ASSERT_EQ(receive(ack(publisher.sent.back(), R"({"acceptedRevision":1})"), 11),
              Result::Applied);
    ASSERT_EQ(receive(snapshot(publisher.sent.back()), 12), Result::Applied);
    EXPECT_EQ(publisher.sent.back().method, "sessionSync");
    EXPECT_FALSE(client.capturing());
    EXPECT_EQ(media.captures, 1U);
}

TEST_F(ClientControlFixture, FixedDeadlineClosesMediaWithoutCreatingAnotherSession) {
    establish();
    start();
    ASSERT_EQ(client.control_disconnected(8, 0), Result::Applied);
    EXPECT_EQ(client.control_disconnected(10000, 0), Result::Duplicate);
    client.tick(15008);
    EXPECT_EQ(client.state(), VoiceState::Failed);
    EXPECT_FALSE(media.capture);
    EXPECT_TRUE(client.session_id().empty());
    EXPECT_EQ(media.configurations, 1U);
}

TEST_F(ClientControlFixture, SnapshotRetiresReplyNeverSeenBeforeDisconnection) {
    establish();
    ASSERT_EQ(client.control_disconnected(8, 0), Result::Applied);
    ASSERT_EQ(client.control_reconnected(9), Result::Applied);
    ASSERT_EQ(receive(ack(publisher.sent.back(), R"({"acceptedRevision":1})"), 10),
              Result::Applied);
    auto body = snapshot(publisher.sent.back());
    const auto at = body.find(R"("activeRespIds":[])");
    ASSERT_NE(at, std::string::npos);
    body.replace(at, std::string(R"("activeRespIds":[])").size(),
                 R"("activeRespIds":["offline-reply"])");
    ASSERT_EQ(receive(body, 11), Result::Applied);
    Envelope old_audio;
    old_audio.token = "late-audio";
    old_audio.session = "original-ai";
    old_audio.method = "respAudioStart";
    old_audio.request = false;
    old_audio.event_sequence = 10;
    old_audio.payload =
        R"({"respId":"offline-reply","trackId":"speaker-1","mediaRevision":1,"firstPacketSeq":128,"udpNonce":"0000000000000000"})";
    ASSERT_EQ(encode_envelope(old_audio, body), DecodeResult::Ok);
    EXPECT_EQ(receive(body, 12), Result::Stale);
    EXPECT_FALSE(media.playback);
}
// ReceiptControlFixture沿原完整重鉴权/能力ACK/快照恢复，旧Start不能在恢复后再次发200。
struct ReceiptControlFixture : ClientControlFixture {
    ReceiptControlFixture() : ClientControlFixture(true) {}
};
TEST_F(ReceiptControlFixture, OriginalInstalledStartCannotReplayReceiptAcrossControlRecovery) {
    establish();
    Envelope original;
    original.token = "original-start";
    original.method = "respAudioStart";
    original.session = "original-ai";
    original.request = false;
    original.event_sequence = 2;
    original.payload =
        R"({"respId":"reply","trackId":"speaker-1","mediaRevision":1,"udpNonce":"0000000000000063","firstPacketSeq":128})";
    std::string encoded;
    ASSERT_EQ(encode_envelope(original, encoded), DecodeResult::Ok);
    ASSERT_EQ(receive(encoded, 8), Result::Applied);
    const auto receipts = [&] {
        return std::count_if(publisher.sent.begin(), publisher.sent.end(), [](const auto& e) {
            return e.method == "respAudioStart" && e.code == 200;
        });
    };
    ASSERT_EQ(receipts(), 1);
    ASSERT_EQ(client.control_disconnected(9, 7), Result::Applied);
    EXPECT_EQ(receive(encoded, 10), Result::Stale);
    ASSERT_EQ(client.control_reconnected(11), Result::Applied);
    ASSERT_EQ(receive(ack(publisher.sent.back(), R"({"acceptedRevision":1})"), 12),
              Result::Applied);
    ASSERT_EQ(receive(snapshot(publisher.sent.back()), 13), Result::Applied);
    EXPECT_EQ(receive(encoded, 14), Result::Stale);
    client.tick(15);
    EXPECT_EQ(receipts(), 1);
    EXPECT_FALSE(media.playback);
}
}  // namespace
}  // namespace ur::device
