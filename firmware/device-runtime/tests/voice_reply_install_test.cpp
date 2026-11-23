// 候选回归：真实VoiceClient和Session执行原HAL，虚拟时间/发布器不冒充MQTT或kernel媒体E2E。
#include "ur/device/voice_client.hpp"
#include "ur/device/ai_endpoint_invite_wire.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <iomanip>
#include <sstream>

namespace ur::device {
namespace {
// Publisher只记录成功交接字节，背压时另存尝试原件以核重试是否原样。
struct ReceiptPublisher final : PublishPort {
    bool blocked = false;                // 只用于原回执发布受理背压，不表示设备安装失败。
    std::vector<Envelope> accepted;      // 当前实际受理的原信封。
    std::vector<std::string> attempted;  // 原字节，不进入正式诊断。
    bool publish(std::string_view, std::string_view body) override {
        attempted.emplace_back(body);
        if (blocked)
            return false;
        Envelope envelope;
        EXPECT_EQ(decode_envelope(body, envelope), DecodeResult::Ok);
        accepted.push_back(std::move(envelope));
        return true;
    }
};
// Media同步安装真实Session调用的模拟HAL事实；Accepted队列不存在，失败不得产安装200。
struct ReceiptMedia final : VoiceMediaPort {
    bool support = true, prepare_ok = true, play_ok = true;  // 可控实际装配和两步同步结果。
    int preparations = 0, plays = 0, stops = 0;              // 必须由正式客户端实际调用产生。
    std::uint32_t input_sequence = 128;  // 控制原停止请求的实际序号准入，0表示媒体边界暂不可用。
    bool udp_reply_start_receipt_ready() const override { return support; }
    bool configure(const VoiceMedia&) override { return true; }
    bool prepare_reply(const std::array<std::uint8_t, 8>&, std::uint32_t) override {
        ++preparations;
        return prepare_ok;
    }
    std::uint32_t next_input_sequence() const override { return input_sequence; }
    bool finish_reply(std::uint32_t) override { return true; }
    bool start_capture(std::string_view) override { return true; }
    void stop_capture() override {}
    bool start_playback(std::string_view, std::uint64_t) override {
        ++plays;
        return play_ok;
    }
    void stop_playback(std::uint64_t) override { ++stops; }
    void close() override {}
};
// bytes使用原公共编码器，防止测试绕过公共JSON/方向校验。
std::string bytes(const Envelope& envelope) {
    std::string out;
    EXPECT_EQ(encode_envelope(envelope, out), DecodeResult::Ok);
    return out;
}
// ack仅构造既有平台原关联成功，不生成新的反向安装应答。
std::string ack(const Envelope& request, std::string data) {
    Envelope out;
    out.token = request.token;
    out.session = request.session;
    out.method = "requestAck";
    out.request = false;
    out.code = 200;
    out.payload = "{\"requestMethod\":\"" + request.method + "\",\"result\":" + data + "}";
    return bytes(out);
}
// capability明确只装配原UDP音频，默认参数仍使旧feature缺省不出现。
VoiceCapability capability(bool selected = true) {
    VoiceCapability out;
    out.modes = {true, true, true};
    out.audio = {16000, 1, 20};
    out.udp_reply_start_receipt = selected;
    return out;
}
// created独立生成显式true/省略选择；UDP密钥仅测试常量，不访问真实身份。
std::string created(const Envelope& request, bool feature, bool full = true) {
    Envelope out;
    out.token = request.token;
    out.session = "session";
    out.method = "sessionCreated";
    out.request = false;
    out.code = 200;
    out.payload =
        std::string(
            R"({"state":"negotiating","selected":{"transport":"udp","interactionMode":"fullDuplex","audioParams":{"format":"opus","sampleRate":16000,"channels":1,"frameDuration":20})") +
        (feature ? R"(,"udpReplyStartReceipt":true)" : "") +
        R"(},"tracks":[{"trackId":"mic-1","kind":"audio","sourceId":"mic","direction":"sendonly","codec":"opus","state":"requested"},{"trackId":"speaker-1","kind":"audio","sourceId":"speaker","direction":"recvonly","codec":"opus","state":"requested"}],"media":{"udp":{"server":"127.0.0.1","port":12345,"key":"000102030405060708090a0b0c0d0e0f","nonce":"01000000112233445566778800000000"}}})";
    if (!full) {
        const auto pos = out.payload.find("fullDuplex");
        out.payload.replace(pos, std::string("fullDuplex").size(), "halfDuplex");
    }
    return bytes(out);
}
// event始终固定原信令方向，newnonce只是测试输入，不当作任何额外媒体许可。
Envelope event(std::string method, std::string token, std::uint64_t seq, std::string data) {
    Envelope out;
    out.method = std::move(method);
    out.token = std::move(token);
    out.session = "session";
    out.request = false;
    out.event_sequence = seq;
    out.payload = std::move(data);
    return out;
}
// start按固定UDP五字段构造每个独立回复，nonce不从其他轨借用。
Envelope start(std::string token = "start", std::string reply = "reply", std::uint64_t seq = 2,
               std::uint64_t nonce = 99) {
    std::ostringstream hex;
    hex << std::hex << std::setfill('0') << std::setw(16) << nonce;
    return event("respAudioStart", std::move(token), seq,
                 "{\"respId\":\"" + reply +
                     "\",\"trackId\":\"speaker-1\",\"mediaRevision\":1,\"udpNonce\":\"" +
                     hex.str() + "\",\"firstPacketSeq\":128}");
}
// ReceiptClientTest沿完整能力/创建/mediaState/Established握手，不直接构造session或插入媒体权限。
class ReceiptClientTest : public testing::Test {
protected:
    ReceiptPublisher publisher;
    ReceiptMedia media;
    VoiceClient client{publisher,    media, "$thing/up/signaling/p/d", "$thing/down/signaling/p/d",
                       capability(), "boot"};
    Result receive(const Envelope& value, std::uint64_t now = 10) {
        return client.receive({"$thing/down/signaling/p/d", bytes(value)}, now);
    }
    void establish(bool feature = true, bool full = true) {
        ASSERT_EQ(client.connect(0), Result::Applied);
        ASSERT_EQ(client.receive({"$thing/down/signaling/p/d",
                                  ack(publisher.accepted.back(), R"({"acceptedRevision":1})")},
                                 1),
                  Result::Applied);
        ASSERT_EQ(client.receive({"$thing/down/signaling/p/d",
                                  created(publisher.accepted.back(), feature, full)},
                                 2),
                  Result::Applied);
        ASSERT_EQ(client.media_ready(3), Result::Applied);
        ASSERT_EQ(client.receive({"$thing/down/signaling/p/d",
                                  ack(publisher.accepted.back(),
                                      R"({"state":"connected","mediaRevision":1})")},
                                 4),
                  Result::Applied);
        ASSERT_EQ(receive(event("sessionEstablished", "established", 1,
                                R"({"state":"established","mediaRevision":1})"),
                          5),
                  Result::Applied);
    }
    std::size_t receipts() const {
        return static_cast<std::size_t>(std::count_if(
            publisher.accepted.begin(), publisher.accepted.end(),
            [](const auto& e) { return e.method == "respAudioStart" && e.code == 200; }));
    }
};
TEST_F(ReceiptClientTest, ActualTwoStepInstallThenOriginalStrictThreeFieldReceipt) {
    establish();
    const auto input = start();
    ASSERT_EQ(receive(input), Result::Applied);
    EXPECT_EQ(media.preparations, 1);
    EXPECT_EQ(media.plays, 1);
    ASSERT_EQ(receipts(), std::size_t{1});
    const auto& actual = publisher.accepted.back();
    EXPECT_EQ(actual.token, input.token);
    EXPECT_EQ(actual.session, input.session);
    EXPECT_FALSE(actual.event_sequence);
    EXPECT_FALSE(actual.message_sequence);
    EXPECT_FALSE(actual.request);
    UDPReplyStartReceipt checked;
    EXPECT_EQ(decode_reply_start_receipt(WireRoute::Signaling, bytes(actual), checked),
              WireResult::Ok);
    EXPECT_EQ(checked.response_id, "reply");
    EXPECT_EQ(checked.track_id, "speaker-1");
    EXPECT_EQ(checked.media_revision, std::uint64_t{1});
    VoiceEvent down;
    EXPECT_EQ(decode_voice_event(WireRoute::Signaling, bytes(actual), down), WireResult::Invalid);
}
TEST_F(ReceiptClientTest, ExactDuplicateReplaysOnlyBytesNoSecondInstallation) {
    establish();
    const auto input = start();
    ASSERT_EQ(receive(input), Result::Applied);
    const auto fact = bytes(publisher.accepted.back());
    EXPECT_EQ(receive(input, 11), Result::Duplicate);
    EXPECT_EQ(bytes(publisher.accepted.back()), fact);
    EXPECT_EQ(media.preparations, 1);
    EXPECT_EQ(media.plays, 1);
    EXPECT_EQ(receipts(), std::size_t{2});
}
TEST_F(ReceiptClientTest, PublisherPressureSingleFactRetriesOriginalBytesWithoutReinstall) {
    establish();
    publisher.blocked = true;
    ASSERT_EQ(receive(start()), Result::Applied);
    const auto original = publisher.attempted.back();
    client.tick(11);
    EXPECT_EQ(publisher.attempted.back(), original);
    EXPECT_EQ(receipts(), std::size_t{0});
    publisher.blocked = false;
    client.tick(12);
    EXPECT_EQ(bytes(publisher.accepted.back()), original);
    EXPECT_EQ(receipts(), std::size_t{1});
    EXPECT_EQ(media.preparations, 1);
    EXPECT_EQ(media.plays, 1);
}
TEST_F(ReceiptClientTest, CancelDiscardsPendingAndLateStartCannotReinstall) {
    establish();
    publisher.blocked = true;
    const auto input = start();
    ASSERT_EQ(receive(input), Result::Applied);
    publisher.blocked = false;
    ASSERT_EQ(client.cancel_response(11), Result::Applied);
    client.tick(12);
    EXPECT_EQ(receipts(), std::size_t{0});
    EXPECT_EQ(receive(input, 13), Result::Stale);
    EXPECT_EQ(media.preparations, 1);
    EXPECT_EQ(media.plays, 1);
}
TEST_F(ReceiptClientTest, OriginalTokenCannotAcquireNewSequenceNonceOrReply) {
    establish();
    ASSERT_EQ(receive(start()), Result::Applied);
    ASSERT_EQ(receive(event("audioSpeechInterrupted", "interrupt", 3,
                            R"({"respId":"reply","trackId":"speaker-1","mediaRevision":1})"),
                      11),
              Result::Applied);
    EXPECT_EQ(receive(start("start", "other", 4, 100), 12), Result::Invalid);
    EXPECT_EQ(media.preparations, 1);
    EXPECT_EQ(media.plays, 1);
    ASSERT_EQ(receive(start("next", "next-reply", 4, 101), 13), Result::Applied);
    ASSERT_EQ(receive(event("audioSpeechInterrupted", "interrupt-next", 5,
                            R"({"respId":"next-reply","trackId":"speaker-1","mediaRevision":1})"),
                      14),
              Result::Applied);
    EXPECT_EQ(receive(start("start", "other", 6, 102), 15), Result::Stale);
    EXPECT_EQ(media.preparations, 2);
    EXPECT_EQ(media.plays, 2);
}
TEST_F(ReceiptClientTest, FixedTwoSecondsNoRefreshAndBackwardsClockRetiresPending) {
    establish();
    publisher.blocked = true;
    ASSERT_EQ(receive(start(), 10), Result::Applied);
    client.tick(2009);
    EXPECT_TRUE(client.playing());
    client.tick(2010);
    EXPECT_FALSE(client.playing());
    publisher.blocked = false;
    client.tick(2011);
    EXPECT_EQ(receipts(), std::size_t{0});
    EXPECT_EQ(receive(start(), 2012), Result::Stale);
}
TEST_F(ReceiptClientTest, ClockRollbackNeverPublishesOrRefreshesOriginalDeadline) {
    establish();
    publisher.blocked = true;
    ASSERT_EQ(receive(start(), 10), Result::Applied);
    publisher.blocked = false;
    client.tick(9);
    client.tick(11);
    EXPECT_EQ(receipts(), std::size_t{0});
    EXPECT_FALSE(client.playing());
    EXPECT_EQ(receive(start(), 12), Result::Stale);
}
TEST_F(ReceiptClientTest, DeliveredReceiptExpiryDoesNotStopNormalReplyOrFullDuplexInput) {
    establish();
    ASSERT_EQ(client.start_input(6), Result::Applied);
    ASSERT_EQ(client.receive({"$thing/down/signaling/p/d",
                              ack(publisher.accepted.back(), publisher.accepted.back().payload)},
                             7),
              Result::Applied);
    ASSERT_TRUE(client.capturing());
    ASSERT_EQ(receive(start(), 10), Result::Applied);
    client.tick(2010);
    EXPECT_TRUE(client.playing());
    EXPECT_TRUE(client.capturing());
    EXPECT_EQ(receive(start(), 2011), Result::Stale);
    EXPECT_EQ(receipts(), std::size_t{1});
}
TEST_F(ReceiptClientTest, DeliveredReceiptDuplicatePressureCannotCancelNormalReply) {
    establish();
    const auto input = start();
    ASSERT_EQ(receive(input, 10), Result::Applied);
    ASSERT_EQ(receipts(), std::size_t{1});
    publisher.blocked = true;
    EXPECT_EQ(receive(input, 2009), Result::Duplicate);
    client.tick(2010);
    EXPECT_TRUE(client.playing());
    EXPECT_EQ(media.stops, 0);
    publisher.blocked = false;
    client.tick(2011);
    EXPECT_EQ(receipts(), std::size_t{1});
    EXPECT_EQ(media.preparations, 1);
    EXPECT_EQ(media.plays, 1);
    EXPECT_EQ(receive(input, 2012), Result::Stale);
}
TEST_F(ReceiptClientTest, HalfDuplexStopsInputBeforeActualInstallReceipt) {
    establish(true, false);
    ASSERT_EQ(client.start_input(6), Result::Applied);
    ASSERT_EQ(client.receive({"$thing/down/signaling/p/d",
                              ack(publisher.accepted.back(), publisher.accepted.back().payload)},
                             7),
              Result::Applied);
    ASSERT_TRUE(client.capturing());
    ASSERT_EQ(receive(start(), 10), Result::Applied);
    EXPECT_FALSE(client.capturing());
    EXPECT_TRUE(client.playing());
    EXPECT_EQ(receipts(), std::size_t{1});
    EXPECT_EQ(media.preparations, 1);
    EXPECT_EQ(media.plays, 1);
}
TEST_F(ReceiptClientTest, HalfDuplexStopAdmissionBusyRetriesOriginalStartWithinFixedDeadline) {
    establish(true, false);
    ASSERT_EQ(client.start_input(6), Result::Applied);
    ASSERT_EQ(client.receive({"$thing/down/signaling/p/d",
                              ack(publisher.accepted.back(), publisher.accepted.back().payload)},
                             7),
              Result::Applied);
    const auto input = start();
    media.input_sequence = 0;
    ASSERT_EQ(receive(input, 10), Result::Busy);
    EXPECT_EQ(client.state(), VoiceState::Established);
    EXPECT_TRUE(client.capturing());
    EXPECT_EQ(media.preparations, 0);
    EXPECT_EQ(media.plays, 0);
    EXPECT_EQ(receipts(), std::size_t{0});
    client.tick(11);
    media.input_sequence = 128;
    EXPECT_EQ(receive(input, 12), Result::Applied);
    EXPECT_FALSE(client.capturing());
    EXPECT_EQ(media.preparations, 1);
    EXPECT_EQ(media.plays, 1);
    EXPECT_EQ(receipts(), std::size_t{1});
    EXPECT_EQ(receive(input, 2010), Result::Stale);
}
TEST_F(ReceiptClientTest, HalfDuplexUninstalledBusyNeverRefreshesOrPublishesReceipt) {
    establish(true, false);
    ASSERT_EQ(client.start_input(6), Result::Applied);
    ASSERT_EQ(client.receive({"$thing/down/signaling/p/d",
                              ack(publisher.accepted.back(), publisher.accepted.back().payload)},
                             7),
              Result::Applied);
    const auto input = start();
    media.input_sequence = 0;
    ASSERT_EQ(receive(input, 10), Result::Busy);
    EXPECT_EQ(receive(input, 2009), Result::Busy);
    client.tick(2010);
    media.input_sequence = 128;
    EXPECT_EQ(receive(input, 2011), Result::Stale);
    EXPECT_TRUE(client.capturing());
    EXPECT_EQ(media.preparations, 0);
    EXPECT_EQ(media.plays, 0);
    EXPECT_EQ(receipts(), std::size_t{0});
}
TEST_F(ReceiptClientTest, HalfDuplexPublisherRefusalRetainsOriginalRequestsAndInstallFact) {
    establish(true, false);
    ASSERT_EQ(client.start_input(6), Result::Applied);
    ASSERT_EQ(client.receive({"$thing/down/signaling/p/d",
                              ack(publisher.accepted.back(), publisher.accepted.back().payload)},
                             7),
              Result::Applied);
    publisher.blocked = true;
    const auto input = start();
    ASSERT_EQ(receive(input, 10),
              Result::Applied);  // 原Requests接收即保留请求，publish=false本身不返回Busy。
    EXPECT_FALSE(client.capturing());
    EXPECT_EQ(receipts(), std::size_t{0});
    EXPECT_TRUE(
        std::any_of(publisher.attempted.begin(), publisher.attempted.end(), [](const auto& body) {
            Envelope e;
            return decode_envelope(body, e) == DecodeResult::Ok && e.method == "audioStop";
        }));
    publisher.blocked = false;
    client.tick(11);
    EXPECT_EQ(receipts(), std::size_t{1});
    EXPECT_EQ(media.preparations, 1);
    EXPECT_EQ(media.plays, 1);
    client.tick(10010);
    EXPECT_TRUE(std::any_of(publisher.accepted.begin(), publisher.accepted.end(),
                            [](const auto& e) { return e.method == "audioStop"; }));
    EXPECT_EQ(media.preparations, 1);
    EXPECT_EQ(media.plays, 1);
}
TEST_F(ReceiptClientTest, HALPrepareFailureDoesNotPublishInstallationReceipt) {
    establish();
    media.prepare_ok = false;
    EXPECT_EQ(receive(start()), Result::DriverFailed);
    EXPECT_EQ(media.plays, 0);
    EXPECT_EQ(receipts(), std::size_t{0});
}
TEST_F(ReceiptClientTest, HALBeginPlaybackFailureDoesNotPublishInstallationReceipt) {
    establish();
    media.play_ok = false;
    EXPECT_EQ(receive(start()), Result::DriverFailed);
    EXPECT_EQ(media.preparations, 1);
    EXPECT_EQ(receipts(), std::size_t{0});
}
TEST_F(ReceiptClientTest, SelectedOmittedPreservesLegacyNoReceipt) {
    establish(false);
    EXPECT_EQ(receive(start()), Result::Applied);
    EXPECT_EQ(media.preparations, 1);
    EXPECT_EQ(media.plays, 1);
    EXPECT_EQ(receipts(), std::size_t{0});
}
TEST_F(ReceiptClientTest, MissingActualHALCannotAdvertise) {
    media.support = false;
    EXPECT_EQ(client.connect(0), Result::Unsupported);
    EXPECT_TRUE(publisher.accepted.empty());
}
TEST_F(ReceiptClientTest, DisconnectDropsPendingWithoutLaterTickRecreatingReceipt) {
    establish();
    publisher.blocked = true;
    ASSERT_EQ(receive(start()), Result::Applied);
    client.disconnected(11);
    publisher.blocked = false;
    client.tick(12);
    EXPECT_EQ(receipts(), std::size_t{0});
    EXPECT_EQ(receive(start(), 13), Result::Stale);
}
TEST_F(ReceiptClientTest, EndedAndWrongTrackRevisionCannotPublishOrInstall) {
    establish();
    auto wrong = start();
    wrong.payload.replace(wrong.payload.find("speaker-1"), 9, "speaker-2");
    EXPECT_EQ(receive(wrong), Result::Stale);
    EXPECT_EQ(media.preparations, 0);
    wrong = start();
    wrong.payload.replace(wrong.payload.find("\"mediaRevision\":1") + 16, 1, "2");
    EXPECT_EQ(receive(wrong), Result::Stale);
    EXPECT_EQ(media.preparations, 0);
    publisher.blocked = true;
    ASSERT_EQ(receive(start()), Result::Applied);
    publisher.blocked = false;
    ASSERT_EQ(client.end(11), Result::Applied);
    client.tick(12);
    EXPECT_EQ(receipts(), std::size_t{0});
    EXPECT_EQ(receive(start(), 13), Result::Stale);
    EXPECT_EQ(media.preparations, 1);
}
TEST_F(ReceiptClientTest, MoreThanThirtyTwoRepliesAnd512BoundNeverEvictsOldToken) {
    establish();
    for (unsigned i = 0; i < 512; ++i) {
        const auto reply = "reply-" + std::to_string(i);
        ASSERT_EQ(
            receive(start("start-" + std::to_string(i), reply, 2 + i * 2, 100 + i), 10 + i * 4),
            Result::Applied);
        ASSERT_EQ(
            receive(event("audioSpeechInterrupted", "interrupt-" + std::to_string(i), 3 + i * 2,
                          "{\"respId\":\"" + reply +
                              "\",\"trackId\":\"speaker-1\",\"mediaRevision\":1}"),
                    11 + i * 4),
            Result::Applied);
    }
    EXPECT_EQ(media.preparations, 512);
    EXPECT_EQ(client.state(), VoiceState::Established);
    EXPECT_EQ(receive(start("start-0", "forged", 1026, 9999), 2060), Result::Stale);
    EXPECT_EQ(receive(start("start-512", "extra", 1026, 9999), 2061), Result::Busy);
    EXPECT_EQ(media.preparations, 512);
    EXPECT_EQ(client.state(), VoiceState::Established);
}
TEST(ReplyInstallWire, OnlyUdpCapabilityAndLiteralTrueSelection) {
    auto cap = capability();
    Envelope out;
    EXPECT_EQ(capability_command("cap", cap, out), WireResult::Ok);
    EXPECT_NE(out.payload.find("udpReplyStartReceipt"), std::string::npos);
    cap.udp_reply_start_receipt = false;
    EXPECT_EQ(capability_command("cap", cap, out), WireResult::Ok);
    EXPECT_EQ(out.payload.find("udpReplyStartReceipt"), std::string::npos);
    cap.udp_reply_start_receipt = true;
    cap.transport = VoiceTransport::WebRTC;
    cap.audio = {48000, 1, 20};
    EXPECT_EQ(capability_command("cap", cap, out), WireResult::Invalid);
    Envelope request;
    request.token = "create";
    request.method = "sessionCreate";
    auto body = created(request, true);
    VoiceEvent parsed;
    ASSERT_EQ(decode_voice_event(WireRoute::Signaling, body, parsed), WireResult::Ok);
    EXPECT_TRUE(parsed.media.udp_reply_start_receipt);
    for (const auto replacement : {"false", "null", "1", "\"true\""}) {
        auto invalid = body;
        invalid.replace(invalid.find("\"udpReplyStartReceipt\":true") + 23, 4, replacement);
        EXPECT_EQ(decode_voice_event(WireRoute::Signaling, invalid, parsed), WireResult::Invalid);
    }
}
TEST(ReplyInstallWire, ReceiptRejectsWrongDirectionCodeExtraFieldsAndMissingOriginalBoundary) {
    VoiceEvent input;
    ASSERT_EQ(decode_voice_event(WireRoute::Signaling, bytes(start()), input), WireResult::Ok);
    Envelope actual;
    ASSERT_EQ(reply_start_receipt(input, actual), WireResult::Ok);
    UDPReplyStartReceipt checked;
    EXPECT_EQ(decode_reply_start_receipt(WireRoute::Message, bytes(actual), checked),
              WireResult::WrongRoute);
    auto invalid = actual;
    invalid.event_sequence = 1;
    EXPECT_EQ(decode_reply_start_receipt(WireRoute::Signaling, bytes(invalid), checked),
              WireResult::Invalid);
    invalid = actual;
    invalid.code = 503;
    EXPECT_EQ(decode_reply_start_receipt(WireRoute::Signaling, bytes(invalid), checked),
              WireResult::Invalid);
    invalid = actual;
    invalid.payload = R"({"respId":"reply","trackId":"speaker-1","mediaRevision":0})";
    EXPECT_EQ(decode_reply_start_receipt(WireRoute::Signaling, bytes(invalid), checked),
              WireResult::Invalid);
    invalid = actual;
    invalid.payload =
        R"({"respId":"reply","trackId":"speaker-1","mediaRevision":1,"udpNonce":"00"})";
    EXPECT_EQ(decode_reply_start_receipt(WireRoute::Signaling, bytes(invalid), checked),
              WireResult::Invalid);
    input.envelope.payload = R"({"respId":"reply","trackId":"speaker-1","mediaRevision":1})";
    EXPECT_EQ(reply_start_receipt(input, actual), WireResult::Invalid);
}
TEST(ReplyInstallWire, HandoffKeepsSourceSelectionAndRejectsTargetWithoutCapability) {
    Envelope request;
    request.token = "create";
    request.method = "sessionCreate";
    Envelope invite;
    ASSERT_EQ(decode_envelope(created(request, true), invite), DecodeResult::Ok);
    invite.method = "sessionInvite";
    invite.session = "candidate";
    invite.request = true;
    invite.code.reset();
    invite.event_sequence = 1;
    const std::string state = R"("state":"negotiating",)";
    const auto position = invite.payload.find(state);
    ASSERT_NE(position, std::string::npos);
    invite.payload.erase(position, state.size());
    invite.payload.insert(
        1,
        R"("purpose":"aiConversation","admission":"accept","expiresAt":1000,"handoff":{"sourceSessionId":"source","handoffId":"handoff"},)");
    auto cap = capability();
    cap.ai_endpoint_handoff = true;
    AIEndpointInvite parsed;
    ASSERT_EQ(decode_ai_endpoint_invite(WireRoute::Signaling, bytes(invite), cap, true, parsed),
              WireResult::Ok);
    EXPECT_TRUE(parsed.media.udp_reply_start_receipt);
    cap.udp_reply_start_receipt = false;
    EXPECT_EQ(decode_ai_endpoint_invite(WireRoute::Signaling, bytes(invite), cap, true, parsed),
              WireResult::Invalid);
    EXPECT_TRUE(parsed.envelope.token.empty());
}
TEST(ReplyInstallWire, PlatformCannotSelectFeatureMissingFromOriginalDeviceCapability) {
    ReceiptPublisher publisher;
    ReceiptMedia media;
    VoiceClient client{
        publisher,         media, "$thing/up/signaling/p/d", "$thing/down/signaling/p/d",
        capability(false), "boot"};
    ASSERT_EQ(client.connect(0), Result::Applied);
    ASSERT_EQ(client.receive({"$thing/down/signaling/p/d",
                              ack(publisher.accepted.back(), R"({"acceptedRevision":1})")},
                             1),
              Result::Applied);
    EXPECT_EQ(
        client.receive({"$thing/down/signaling/p/d", created(publisher.accepted.back(), true)}, 2),
        Result::Invalid);
    EXPECT_EQ(client.state(), VoiceState::Creating);
    EXPECT_EQ(media.preparations, 0);
    EXPECT_EQ(media.plays, 0);
}
}  // namespace
}  // namespace ur::device
