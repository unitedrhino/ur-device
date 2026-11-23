// 状态机测试使用虚拟时间和驱动；不将模拟确认当作真实 MQTT/UDP 端到端验收。
#include "ur/device/voice_client.hpp"
#include "../core/voice_failure_storage.hpp"
#include <algorithm>
#include <gtest/gtest.h>

namespace ur::device {
// 私有字节复用只能改变存储，不改变原11个客户端原因/方法或混入Runtime准备来源。
TEST(VoiceFailureStorage, OriginalElevenCausesAndMethodsRoundTripWithoutByteChange) {
    for (unsigned cause = 1; cause <= 11; ++cause) {
        for (unsigned method = 0; method <= 14; ++method) {
            const auto c = static_cast<VoiceFailureCause>(cause);
            const auto m = static_cast<VoiceFailureMethod>(method);
            const auto encoded = encode_voice_failure(c, m);
            EXPECT_EQ(encoded, (cause << 4U) | method);
            const auto decoded = decode_voice_failure(encoded);
            ASSERT_TRUE(decoded);
            EXPECT_EQ(decoded->cause, c);
            EXPECT_EQ(decoded->method, m);
            EXPECT_FALSE(decode_scope_failure(encoded));
        }
    }
    for (unsigned cause = 12; cause <= 15; ++cause)
        EXPECT_EQ(
            encode_voice_failure(static_cast<VoiceFailureCause>(cause), VoiceFailureMethod::None),
            0);
    EXPECT_EQ(encode_voice_failure(VoiceFailureCause::None, VoiceFailureMethod::None), 0);
}
// 23子码只对旧消费者投影原activeScopeLost/none，两边界及非法输入不能造首因。
TEST(VoiceFailureStorage, AllScopeSubcodesAndBoundariesRemainSeparateFromRuntime) {
    for (unsigned code = 1; code <= 23; ++code) {
        const auto original = static_cast<ScopeFailureCode>(code);
        const auto encoded = encode_scope_failure(original);
        EXPECT_EQ(encoded, 0xC0U + code);
        EXPECT_EQ(decode_scope_failure(encoded), original);
        const auto decoded = decode_voice_failure(encoded);
        ASSERT_TRUE(decoded);
        EXPECT_EQ(decoded->cause, VoiceFailureCause::ActiveScopeLost);
        EXPECT_EQ(decoded->method, VoiceFailureMethod::None);
    }
    for (const auto code : {0U, 24U, 255U})
        EXPECT_EQ(encode_scope_failure(static_cast<ScopeFailureCode>(code)), 0);
    for (const auto value : {0U, 0xC0U, 0xD8U, 0xFFU}) {
        EXPECT_FALSE(decode_scope_failure(static_cast<std::uint8_t>(value)));
        EXPECT_FALSE(decode_voice_failure(static_cast<std::uint8_t>(value)));
    }
}
namespace {
// ClientPublisher 保存真实编码后的请求，测试从中提取实际 token
// 而不假设生成次数。
struct ClientPublisher final : PublishPort {
    std::vector<Envelope> messages;  // 已校验的新信令请求。
    bool publish(std::string_view topic, std::string_view body) override {
        Envelope request;
        EXPECT_EQ(decode_envelope(body, request), DecodeResult::Ok);
        EXPECT_EQ(topic, request.method == "inputSend" ? "$thing/up/message/p/d"
                                                       : "$thing/up/signaling/p/d");
        messages.push_back(std::move(request));
        return true;
    }
};
// ClientMedia 统计真实调用意图，允许逐一注入配置、采集与播放失败。
struct ClientMedia final : VoiceMediaPort {
    bool configure_ok = true;           // 模拟通道配置结果。
    bool capture_ok = true;             // 模拟采集启动结果。
    bool playback_ok = true;            // 模拟播放启动结果。
    bool configured = false;            // 是否持有配置资源。
    bool capturing = false;             // 本地驱动是否采集中。
    bool playing = false;               // 本地驱动是否播放中。
    int captures = 0;                   // 实际采集启动调用次数。
    int plays = 0;                      // 实际播放启动调用次数。
    int preparations = 0;               // 回复 nonce 安装次数，陈旧事件不得产生副作用。
    int finishes = 0;                   // 网络末包边界安装次数。
    int capture_stops = 0;              // 已借用采集资源的停止回调次数。
    int playback_stops = 0;             // 已借用播放资源的停止回调次数。
    int closes = 0;                     // 实际媒体端口的幂等关闭调用次数。
    std::string answer;                 // WebRTC answer必须交给媒体驱动。
    std::string captured_input;         // 恢复必须使用原输入ID，不能生成新输入轮次。
    std::uint64_t recovery_revision = 0;  // 实际请求创建的下一代Peer。
    bool recovery_ok = true;              // 模拟新Peer驱动拒绝，不能继续采集。
    int recoveries = 0;                   // 实际创建下一代Peer的次数；恢复补发不得无限重建。
    bool connected = false;               // 驱动实际验证的双向连通；未连通才允许恢复期补发。
    bool ready() const override { return connected; }
    std::uint32_t next_sequence = 128;  // 模拟实际发送水位，停止报文不能猜测语料长度。
    std::uint32_t next_input_sequence() const override { return next_sequence; }
    bool prepare_reply(const std::array<std::uint8_t, 8>&, std::uint32_t) override {
        ++preparations;
        return true;
    }
    bool finish_reply(std::uint32_t) override {
        ++finishes;
        return true;
    }
    bool apply_answer(std::string_view sdp) override {
        answer = std::string(sdp);
        return true;
    }
    bool begin_recovery(std::uint64_t revision) override {
        recovery_revision = revision;
        ++recoveries;
        answer.clear();
        connected = false;  // 新Peer尚未验证双向连通，须重新等待驱动事实。
        return recovery_ok;
    }
    bool configure(const VoiceMedia&) override {
        configured = configure_ok;
        return configured;
    }
    void close() override {
        ++closes;
        configured = capturing = playing = false;
    }
    bool start_capture(std::string_view input) override {
        captured_input = std::string(input);
        ++captures;
        capturing = capture_ok;
        return capturing;
    }
    void stop_capture() override {
        ++capture_stops;
        capturing = false;
    }
    bool start_playback(std::string_view, std::uint64_t) override {
        ++plays;
        playing = playback_ok;
        return playing;
    }
    void stop_playback(std::uint64_t) override {
        ++playback_stops;
        playing = false;
    }
};
// ack 编码完整 ACK；参数 result 由每条用例提供，错误路径故意不含 result。
std::string ack(const Envelope& request, std::string result, int code = 200) {
    Envelope envelope;
    envelope.token = request.token;
    envelope.session = request.session;
    envelope.method = "requestAck";
    envelope.request = false;
    envelope.code = code;
    envelope.payload = "{\"requestMethod\":\"" + request.method + "\"," +
                       (code == 200 ? "\"result\":" + result : "\"reason\":\"permissionDenied\"") +
                       "}";
    std::string body;
    EXPECT_EQ(encode_envelope(envelope, body), DecodeResult::Ok);
    return body;
}
// created_reply 仅含公开占位密钥，配置成功不等同 UDP 网络连通。
std::string created_reply(const Envelope& request, bool full = true, bool text = false) {
    Envelope envelope;
    envelope.token = request.token;
    envelope.session = "session";
    envelope.method = "sessionCreated";
    envelope.request = false;
    envelope.code = 200;
    envelope.payload =
        std::string(R"({"state":"negotiating","selected":{"transport":"udp","interactionMode":")") +
        (full ? "fullDuplex" : "halfDuplex") +
        R"(","audioParams":{"format":"opus","sampleRate":16000,"channels":1,"frameDuration":20}},"tracks":[{"trackId":"mic-1","kind":"audio","sourceId":"mic","direction":"sendonly","codec":"opus","state":"requested"},{"trackId":"speaker-1","kind":"audio","sourceId":"speaker","direction":"recvonly","codec":"opus","state":"requested"}],"media":{"udp":{"server":"127.0.0.1","port":12345,"key":"000102030405060708090a0b0c0d0e0f","nonce":"01000000112233445566778800000000"}}})";
    if (text) {
        const auto position = envelope.payload.find("\"audioParams\"");
        envelope.payload.insert(
            position, R"("inputModalities":["text"],"outputModalities":["text","audio"],)");
    }
    std::string body;
    EXPECT_EQ(encode_envelope(envelope, body), DecodeResult::Ok);
    return body;
}
// web_created_reply 不包含UDP凭据，只要求真实WebRTC驱动随后提供完整SDP。
std::string web_created_reply(const Envelope& request, bool full) {
    Envelope envelope;
    envelope.token = request.token;
    envelope.session = "session";
    envelope.method = "sessionCreated";
    envelope.request = false;
    envelope.code = 200;
    envelope.payload =
        std::string(
            R"({"state":"negotiating","selected":{"transport":"webrtc","interactionMode":")") +
        (full ? "fullDuplex" : "halfDuplex") +
        R"(","audioParams":{"format":"opus","sampleRate":48000,"channels":1,"frameDuration":20}},"tracks":[{"trackId":"mic-1","kind":"audio","sourceId":"mic","direction":"sendonly","codec":"opus","state":"requested"},{"trackId":"speaker-1","kind":"audio","sourceId":"speaker","direction":"recvonly","codec":"opus","state":"requested"}]})";
    std::string body;
    EXPECT_EQ(encode_envelope(envelope, body), DecodeResult::Ok);
    return body;
}
// event 编码独立事件，明确与请求 ACK 的区别。
std::string event(std::string method, std::string data, std::uint64_t sequence = 2) {
    if (method == "respAudioStart")
        data.insert(data.size() - 1, R"(,"udpNonce":"9988776655443322","firstPacketSeq":128)");
    if (method == "respAudioDone")
        data.insert(data.size() - 1, R"(,"lastPacketSeq":128)");
    Envelope envelope;
    envelope.token = "event-" + std::to_string(sequence);
    envelope.session = "session";
    envelope.method = std::move(method);
    envelope.request = false;
    envelope.event_sequence = sequence;
    envelope.payload = std::move(data);
    std::string body;
    EXPECT_EQ(encode_envelope(envelope, body), DecodeResult::Ok);
    return body;
}
// VoiceClientTest 提供逐步骤握手，不跳过能力与 mediaState 应答。
class VoiceClientTest : public testing::Test {
protected:
    ClientPublisher publisher;  // 生命周期长于客户端。
    ClientMedia media;          // 可控驱动。
    VoiceClient client{publisher,
                       media,
                       "$thing/up/signaling/p/d",
                       "$thing/down/signaling/p/d",
                       {{true, true, true}, {16000, 1, 20}, 1, true},
                       "boot"};
    Result receive(const std::string& body, std::uint64_t now = 1) {
        return client.receive({"$thing/down/signaling/p/d", body}, now);
    }
    void negotiate(bool full = true, bool text = false) {
        ASSERT_EQ(client.connect(0), Result::Applied);
        ASSERT_EQ(receive(ack(publisher.messages.back(), R"({"acceptedRevision":1})")),
                  Result::Applied);
        ASSERT_EQ(client.state(), VoiceState::Creating);
        ASSERT_EQ(receive(created_reply(publisher.messages.back(), full, text)), Result::Applied);
        ASSERT_EQ(client.state(), VoiceState::Negotiating);
    }
    void establish(bool full = true, bool text = false) {
        negotiate(full, text);
        ASSERT_EQ(client.media_ready(2), Result::Applied);
        ASSERT_EQ(
            receive(ack(publisher.messages.back(), R"({"mediaRevision":1,"state":"connected"})"),
                    3),
            Result::Applied);
        ASSERT_EQ(
            receive(event("sessionEstablished", R"({"state":"established","mediaRevision":1})", 1),
                    4),
            Result::Applied);
        ASSERT_EQ(client.state(), VoiceState::Established);
    }
    void capture() {
        ASSERT_EQ(client.start_input(5), Result::Applied);
        const auto request = publisher.messages.back();
        ASSERT_EQ(receive(ack(request, request.payload), 6), Result::Applied);
        ASSERT_TRUE(client.capturing());
    }
    std::string audio_event(std::string method, std::uint64_t sequence = 2) {
        return event(std::move(method),
                     R"({"respId":"reply","trackId":"speaker-1","mediaRevision":1})", sequence);
    }
};
// 首次失败观察必须由已接纳原事件产生，既有失败返回值和实际停资源均不改变。
TEST_F(VoiceClientTest, FailureDiagnosticLatchesAcceptedSessionErrorBeforeCleanup) {
    client.enable_failure_diagnostics();
    establish();
    capture();
    Envelope failure;
    ASSERT_EQ(
        decode_envelope(
            event(
                "sessionError",
                R"({"scope":"session","reason":"internalError","retryable":false,"mediaRevision":1})",
                2),
            failure),
        DecodeResult::Ok);
    failure.code = 502;
    std::string body;
    ASSERT_EQ(encode_envelope(failure, body), DecodeResult::Ok);
    ASSERT_EQ(receive(body, 7), Result::Applied);
    ASSERT_EQ(client.state(), VoiceState::Failed);
    EXPECT_FALSE(media.capturing);
    const auto fact = client.failure_diagnostic();
    ASSERT_TRUE(fact);
    EXPECT_EQ(fact->cause, VoiceFailureCause::SessionError);
    EXPECT_EQ(fact->method, VoiceFailureMethod::None);
    client.disconnected(8);
    client.enable_failure_diagnostics();
    ASSERT_TRUE(client.failure_diagnostic());
    EXPECT_EQ(client.failure_diagnostic()->cause, VoiceFailureCause::SessionError);
}
// 默认关闭及失败后启用均不补造原观察，不影响原请求截止或停媒体。
TEST_F(VoiceClientTest, FailureDiagnosticDefaultOffAndCannotBackfillFailedState) {
    establish();
    capture();
    ASSERT_EQ(client.stop_input(7), Result::Applied);
    client.tick(30007);
    ASSERT_EQ(client.state(), VoiceState::Failed);
    EXPECT_FALSE(client.failure_diagnostic());
    client.enable_failure_diagnostics();
    EXPECT_FALSE(client.failure_diagnostic());
}
// 原匹配失败ACK保留Unsupported与停态，错关联ACK不得生成失败观察。
TEST_F(VoiceClientTest, FailureDiagnosticRejectAndRequestTimeoutHaveFixedMethod) {
    client.enable_failure_diagnostics();
    establish();
    capture();
    ASSERT_EQ(client.stop_input(7), Result::Applied);
    const auto request = publisher.messages.back();
    auto wrong = request;
    wrong.token = "wrong-original-token";
    EXPECT_EQ(receive(ack(wrong, {}, 403), 8), Result::Stale);
    EXPECT_FALSE(client.failure_diagnostic());
    EXPECT_EQ(receive(ack(request, {}, 403), 9), Result::Unsupported);
    ASSERT_EQ(client.state(), VoiceState::Failed);
    ASSERT_TRUE(client.failure_diagnostic());
    EXPECT_EQ(client.failure_diagnostic()->cause, VoiceFailureCause::RequestRejected);
    EXPECT_EQ(client.failure_diagnostic()->method, VoiceFailureMethod::AudioStop);
}
// 原30秒绝对请求截止不放宽，诊断只能说明实际在途方法而不能输出原token。
TEST_F(VoiceClientTest, FailureDiagnosticPendingStopExpiresAtOriginalThirtySeconds) {
    client.enable_failure_diagnostics();
    establish();
    capture();
    ASSERT_EQ(client.stop_input(7), Result::Applied);
    client.tick(30006);
    EXPECT_EQ(client.state(), VoiceState::Established);
    EXPECT_FALSE(client.failure_diagnostic());
    client.tick(30007);
    ASSERT_EQ(client.state(), VoiceState::Failed);
    ASSERT_TRUE(client.failure_diagnostic());
    EXPECT_EQ(client.failure_diagnostic()->cause, VoiceFailureCause::RequestsTimeout);
    EXPECT_EQ(client.failure_diagnostic()->method, VoiceFailureMethod::AudioStop);
}
// 对象预算覆盖当前64位Linux实际ABI的紧凑布局；较小平台仍须独立构建，不声称ABI相同。
TEST(VoiceClientLayout, ScalarStateFitsCompactObjectBudget) {
    constexpr std::size_t maximum_bytes =
        920;  // 合并双方:原888字节+UDP安装账本指针8字节+humanCall能力标志+恢复期offer重试起点8字节+恢复期请求应答预算8字节,按实测上限。
    EXPECT_LE(sizeof(VoiceClient), maximum_bytes);
    RecordProperty("voice_client_bytes", static_cast<int>(sizeof(VoiceClient)));
}
// 原端口始终比客户端长寿；显式断连后析构不能二次停止已释放的采集和播放资源。
TEST(VoiceClientLifetime, AudioConstructionAndDestructionPreserveBorrowedPort) {
    for (const bool disconnect_first : {false, true}) {
        ClientPublisher publisher;  // 真实编码请求的有界测试接收方。
        ClientMedia media;          // 客户端只借用，退出内部作用域后仍可验证资源。
        {
            VoiceClient client(publisher, media, "$thing/up/signaling/p/d",
                               "$thing/down/signaling/p/d",
                               {{true, true, true}, {16000, 1, 20}, 1, true}, "lifetime");
            const auto receive = [&](const std::string& body, std::uint64_t now) {
                return client.receive({"$thing/down/signaling/p/d", body}, now);
            };  // 必须沿原关联、能力确认及建立事件，不绕协议直接置状态。
            ASSERT_EQ(client.state(), VoiceState::Idle);
            ASSERT_EQ(client.connect(0), Result::Applied);
            ASSERT_EQ(receive(ack(publisher.messages.back(), R"({"acceptedRevision":1})"), 1),
                      Result::Applied);
            ASSERT_EQ(receive(created_reply(publisher.messages.back()), 2), Result::Applied);
            ASSERT_EQ(client.media_ready(3), Result::Applied);
            ASSERT_EQ(receive(ack(publisher.messages.back(),
                                  R"({"mediaRevision":1,"state":"connected"})"),
                              4),
                      Result::Applied);
            ASSERT_EQ(receive(event("sessionEstablished",
                                    R"({"state":"established","mediaRevision":1})", 1),
                              5),
                      Result::Applied);
            ASSERT_EQ(client.start_input(6), Result::Applied);
            const auto input = publisher.messages.back();  // 原请求正文与token完整回执。
            ASSERT_EQ(receive(ack(input, input.payload), 7), Result::Applied);
            ASSERT_EQ(
                receive(event("respAudioStart",
                              R"({"respId":"reply","trackId":"speaker-1","mediaRevision":1})", 2),
                        8),
                Result::Applied);
            ASSERT_TRUE(media.configured);
            ASSERT_TRUE(media.capturing);
            ASSERT_TRUE(media.playing);
            if (disconnect_first) {
                client.disconnected(9);
                client.disconnected(10);
                EXPECT_EQ(client.state(), VoiceState::Idle);
            }
        }
        EXPECT_FALSE(media.configured);
        EXPECT_FALSE(media.capturing);
        EXPECT_FALSE(media.playing);
        EXPECT_EQ(media.capture_stops, 1);
        EXPECT_EQ(media.playback_stops, 1);
        EXPECT_EQ(media.closes, disconnect_first ? 3 : 1);
    }
}
// 纯文字构造没有媒体对象；原能力请求可发出，重复断连与析构不能触发虚构音频资源。
TEST(VoiceClientLifetime, TextConstructionKeepsMediaAbsent) {
    ClientPublisher publisher;  // 无任何媒体适配器装配。
    {
        VoiceCapability capability;  // 使用现有None契约，不因构造改变音频能力。
        capability.transport = VoiceTransport::None;
        capability.text_input = true;
        VoiceClient client(publisher, "$thing/up/signaling/p/d", "$thing/down/signaling/p/d",
                           capability, "text-lifetime");
        ASSERT_EQ(client.state(), VoiceState::Idle);
        ASSERT_EQ(client.connect(0), Result::Applied);
        ASSERT_EQ(publisher.messages.size(), 1U);
        EXPECT_EQ(publisher.messages.front().method, "capabilityReport");
        EXPECT_EQ(publisher.messages.front().payload.find("aecMode"), std::string::npos);
        EXPECT_FALSE(client.capturing());
        EXPECT_FALSE(client.playing());
        client.disconnected(1);
        client.disconnected(2);
        EXPECT_EQ(client.state(), VoiceState::Idle);
    }
    EXPECT_EQ(publisher.messages.size(), 1U);
}
// 两种交互模式共用会话状态，WebRTC仅替换媒体协商和无UDP序号的输入控制。
TEST(VoiceClientWebRTC, RequiresAnswerAndActualReadinessBeforeAudio) {
    for (const bool full : {false, true}) {
        ClientPublisher publisher;
        ClientMedia media;
        VoiceCapability capability{
            {true, full, full}, {48000, 1, 20}, 1, false, VoiceTransport::WebRTC};
        VoiceClient client(publisher, media, "$thing/up/signaling/p/d", "$thing/down/signaling/p/d",
                           capability, full ? "full" : "half");
        const auto receive = [&](const std::string& body, std::uint64_t now) {
            return client.receive({"$thing/down/signaling/p/d", body}, now);
        };
        ASSERT_EQ(client.connect(0), Result::Applied);
        ASSERT_EQ(receive(ack(publisher.messages.back(), R"({"acceptedRevision":1})"), 1),
                  Result::Applied);
        EXPECT_NE(publisher.messages.back().payload.find(R"("transport":"webrtc")"),
                  std::string::npos);
        ASSERT_EQ(receive(web_created_reply(publisher.messages.back(), full), 2), Result::Applied);
        EXPECT_EQ(client.media_ready(3), Result::Stale);
        EXPECT_EQ(client.media_offer_ready("v=0\n", 3), Result::Invalid);
        ASSERT_EQ(client.media_offer_ready("v=0\r\no=offer\r\n", 3), Result::Applied);
        EXPECT_EQ(client.media_offer_ready("v=0\r\no=other\r\n", 3), Result::Duplicate);
        const auto offer = publisher.messages.back();
        EXPECT_EQ(offer.method, "mediaOffer");
        Envelope answer;
        answer.token = offer.token;
        answer.session = "session";
        answer.method = "mediaAnswer";
        answer.request = false;
        answer.code = 200;
        answer.payload =
            R"({"mediaRevision":1,"webrtc":{"iceMode":"complete","sdp":"v=0\r\no=answer\r\n"}})";
        std::string body;
        ASSERT_EQ(encode_envelope(answer, body), DecodeResult::Ok);
        ASSERT_EQ(receive(body, 4), Result::Applied);
        EXPECT_EQ(media.answer, "v=0\r\no=answer\r\n");
        ASSERT_EQ(client.media_ready(5), Result::Applied);
        ASSERT_EQ(
            receive(ack(publisher.messages.back(), R"({"mediaRevision":1,"state":"connected"})"),
                    6),
            Result::Applied);
        ASSERT_EQ(
            receive(event("sessionEstablished", R"({"state":"established","mediaRevision":1})", 1),
                    7),
            Result::Applied);
        ASSERT_EQ(client.state(), VoiceState::Established);
        ASSERT_EQ(client.start_input(8), Result::Applied);
        const auto start = publisher.messages.back();
        EXPECT_EQ(start.payload.find("PacketSeq"), std::string::npos);
        ASSERT_EQ(receive(ack(start, start.payload), 9), Result::Applied);
        EXPECT_TRUE(client.capturing());
        Envelope reply;
        reply.token = "reply-start";
        reply.session = "session";
        reply.method = "respAudioStart";
        reply.request = false;
        reply.event_sequence = 2;
        reply.payload = R"({"respId":"reply","trackId":"speaker-1","mediaRevision":1})";
        ASSERT_EQ(encode_envelope(reply, body), DecodeResult::Ok);
        ASSERT_EQ(receive(body, 10), Result::Applied);
        EXPECT_EQ(client.capturing(), full);
        EXPECT_TRUE(client.playing());
        EXPECT_EQ(media.preparations, 1);
        if (full) {
            ASSERT_EQ(client.stop_input(11), Result::Applied);
            EXPECT_EQ(publisher.messages.back().payload.find("PacketSeq"), std::string::npos);
        }
    }
}
TEST_F(VoiceClientTest, TextRequiresNegotiationAndMessageAcknowledgement) {
    establish(true, true);
    ASSERT_EQ(client.send_text("你好", true, 5), Result::Applied);
    const auto request = publisher.messages.back();
    ASSERT_EQ(request.method, "inputSend");
    const auto start = request.payload.find("\"inputId\":\"");
    ASSERT_NE(start, std::string::npos);
    const auto id_start = start + std::string("\"inputId\":\"").size();
    const auto id =
        request.payload.substr(id_start, request.payload.find('"', id_start) - id_start);
    const auto body = ack(request, "{\"inputId\":\"" + id + "\",\"state\":\"accepted\"}");
    EXPECT_EQ(receive(body, 6), Result::Invalid);
    EXPECT_EQ(client.send_text("下一条", false, 6), Result::Busy);
    ASSERT_EQ(client.receive({"$thing/down/message/p/d", body}, 7), Result::Applied);
    EXPECT_EQ(client.receive({"$thing/down/message/p/d", body}, 8), Result::Stale);
    EXPECT_EQ(client.state(), VoiceState::Established);
    EXPECT_FALSE(client.capturing());
    EXPECT_EQ(client.send_text("第二条", false, 9), Result::Applied);
    const auto second = publisher.messages.back();
    EXPECT_EQ(client.receive({"$thing/down/message/p/d", ack(second, "{}", 409)}, 10),
              Result::Busy);
    EXPECT_EQ(client.state(), VoiceState::Established);
    EXPECT_TRUE(media.configured);
    EXPECT_EQ(client.send_text("   ", false, 11), Result::Invalid);
}
TEST_F(VoiceClientTest, TextUnavailableOnUnselectedModel) {
    establish();
    EXPECT_EQ(client.send_text("你好", false, 5), Result::Unsupported);
    EXPECT_FALSE(client.supports_content(InputContentKind::Image));
    EXPECT_FALSE(client.supports_content(InputContentKind::File));
    EXPECT_EQ(client.send_content({{InputContentKind::Image, {}, "artifact_image", "image/png"}},
                                  false, 5),
              Result::Unsupported);
    EXPECT_EQ(publisher.messages.back().method, "mediaState");
}
// DiscreteRejectionKeepsOriginalInputIdentity
// 语音拒绝ACK没有result.inputId，必须保留本端原输入供宿主关联。
TEST_F(VoiceClientTest, DiscreteRejectionKeepsOriginalInputIdentity) {
    establish(true, true);
    ASSERT_EQ(client.send_text("原离散输入", false, 5), Result::Applied);
    const auto request = publisher.messages.back();
    const auto begin = request.payload.find(R"("inputId":")") + 11;
    const auto input = request.payload.substr(begin, request.payload.find('"', begin) - begin);
    ASSERT_FALSE(input.empty());
    EXPECT_EQ(client.text_input_id(), input);
    EXPECT_EQ(client.receive({"$thing/down/message/p/d", ack(request, "{}", 403)}, 6),
              Result::Unsupported);
    EXPECT_EQ(client.text_input_id(), input);
}

// TypedNoneClientTest 共用正式请求、创建和Established流程，无媒体对象也不省略平台确认。
class TypedNoneClientTest : public testing::Test {
protected:
    ClientPublisher publisher;  // 原生产发布端口，断言实际路由及编码结果。
    // capability 仅声明本夹具显式装配的三种输入，平台仍可选其中任意非空子集。
    static VoiceCapability capability() {
        VoiceCapability value;
        value.transport = VoiceTransport::None;
        value.text_input = value.image_input = value.file_input = true;
        return value;
    }
    VoiceClient client{publisher, "$thing/up/signaling/p/d", "$thing/down/signaling/p/d",
                       capability(), "typed-boot"};
    const std::vector<InputContentPart> parts = {
        {InputContentKind::Text, "实际有序文字", {}, {}},
        {InputContentKind::Image, {}, "artifact_image", "image/png"},
        {InputContentKind::File, {}, "artifact_file", "text/plain"}};  // 本轮明确Ready引用。
    // establish 只修改实际selected列表，不从offer自动开放图片或文件。
    void establish(std::string_view inputs = R"(["text","image","file"])") {
        ASSERT_EQ(client.connect(0), Result::Applied);
        ASSERT_EQ(client.receive({"$thing/down/signaling/p/d",
                                  ack(publisher.messages.back(), R"({"acceptedRevision":1})")},
                                 1),
                  Result::Applied);
        Envelope created;
        created.token = publisher.messages.back().token;
        created.session = "session";
        created.method = "sessionCreated";
        created.request = false;
        created.code = 200;
        created.payload =
            std::string(
                R"({"state":"negotiating","selected":{"transport":"none","inputModalities":)") +
            std::string(inputs) + R"(,"outputModalities":["text"]},"tracks":[]})";
        std::string body;
        ASSERT_EQ(encode_envelope(created, body), DecodeResult::Ok);
        ASSERT_EQ(client.receive({"$thing/down/signaling/p/d", body}, 2), Result::Applied);
        EXPECT_FALSE(client.supports_content(InputContentKind::Image));
        ASSERT_EQ(client.receive({"$thing/down/signaling/p/d",
                                  event("sessionEstablished",
                                        R"({"state":"established","mediaRevision":0})", 1)},
                                 3),
                  Result::Applied);
        ASSERT_EQ(client.state(), VoiceState::Established);
    }
    // acknowledge 受理必须使用原token与原inputId，失败不能伪造成功result。
    void acknowledge(std::uint64_t now = 5) {
        const auto input = client.text_input_id();
        ASSERT_FALSE(input.empty());
        ASSERT_EQ(client.receive({"$thing/down/message/p/d",
                                  ack(publisher.messages.back(),
                                      "{\"inputId\":\"" + input + "\",\"state\":\"accepted\"}")},
                                 now),
                  Result::Applied);
    }
    // content 在真实内容解析器之后交付；所有回复事实仍须符合本端当前inputId/respId和message水位。
    Result content(std::string_view method, std::string data, std::uint64_t sequence) {
        const auto body = std::string(R"({"msgToken":"content","sessionId":"session","method":")") +
                          std::string(method) + R"(","messageSeq":)" + std::to_string(sequence) +
                          R"(,"data":)" + data + "}";
        ContentEvent decoded;
        EXPECT_EQ(decode_content_event(WireRoute::Message, body, decoded), WireResult::Ok);
        return client.receive_content(decoded);
    }
};

// SelectedIntersectionControlsUploadAdmission
// getter与最终send共享selected，未选的设备offer不能授权。
TEST_F(TypedNoneClientTest, SelectedIntersectionControlsUploadAdmission) {
    EXPECT_FALSE(client.supports_content(InputContentKind::Image));
    EXPECT_EQ(client.send_content(parts, false, 0), Result::Stale);
    EXPECT_TRUE(publisher.messages.empty());
    establish(R"(["image"])");
    EXPECT_TRUE(client.supports_content(InputContentKind::Image));
    EXPECT_FALSE(client.supports_content(InputContentKind::Text));
    EXPECT_FALSE(client.supports_content(InputContentKind::File));
    EXPECT_FALSE(client.supports_content(static_cast<InputContentKind>(99)));
    const auto count = publisher.messages.size();
    EXPECT_EQ(client.send_content({parts[2]}, false, 4), Result::Unsupported);
    EXPECT_EQ(client.send_content({parts[0]}, false, 4), Result::Unsupported);
    EXPECT_EQ(client.send_content({parts[1]}, true, 4), Result::Unsupported);
    EXPECT_EQ(publisher.messages.size(), count);
    ASSERT_EQ(client.send_content({parts[1]}, false, 4), Result::Applied);
    const auto request = publisher.messages.back();
    acknowledge();
    EXPECT_EQ(client.receive({"$thing/down/message/p/d", ack(request, "{}", 403)}, 6),
              Result::Stale);
    EXPECT_EQ(client.end(7), Result::Applied);
    EXPECT_FALSE(client.supports_content(InputContentKind::Image));
    client.disconnected(8);
    EXPECT_FALSE(client.supports_content(InputContentKind::Image));
}

// TypedInputUsesOriginalRoundAndTerminalGuarantees
// ACK不代表回复结束，旧轮/重复ACK不能解开本轮背压。
TEST_F(TypedNoneClientTest, TypedInputUsesOriginalRoundAndTerminalGuarantees) {
    establish();
    ASSERT_EQ(client.send_content(parts, false, 4), Result::Applied);
    const auto request = publisher.messages.back();
    const auto input = client.text_input_id();
    EXPECT_EQ(client.send_content(parts, false, 5), Result::Busy);
    EXPECT_EQ(client.receive({"$thing/down/message/p/d",
                              ack(request, R"({"inputId":"wrong","state":"accepted"})")},
                             5),
              Result::Invalid);
    acknowledge(6);
    EXPECT_EQ(
        client.receive({"$thing/down/message/p/d",
                        ack(request, "{\"inputId\":\"" + input + "\",\"state\":\"accepted\"}")},
                       7),
        Result::Stale);
    EXPECT_EQ(client.send_content(parts, false, 7), Result::Busy);
    EXPECT_EQ(content("respCreated", "{\"respId\":\"r1\",\"inputId\":\"" + input + "\"}", 1),
              Result::Applied);
    EXPECT_EQ(
        content(
            "respDone",
            R"({"respId":"r1","inputId":"old","contentCount":1,"status":"completed","audioState":"none"})",
            2),
        Result::Invalid);
    EXPECT_EQ(content("respDone",
                      "{\"respId\":\"r1\",\"inputId\":\"" + input +
                          "\",\"contentCount\":1,\"status\":\"completed\",\"audioState\":\"none\"}",
                      2),
              Result::Applied);
    ASSERT_EQ(client.send_content({parts[2]}, false, 8), Result::Applied);
    const auto next_input = client.text_input_id();
    EXPECT_NE(next_input, input);
    EXPECT_EQ(content("respDone",
                      "{\"respId\":\"r1\",\"inputId\":\"" + input +
                          "\",\"contentCount\":1,\"status\":\"completed\",\"audioState\":\"none\"}",
                      3),
              Result::Stale);
    EXPECT_TRUE(client.text_input_pending());
}

// 定向取消墓碑属于当前回复；新恢复标记不得跨None文字/附件轮次阻止合法取消。
TEST_F(TypedNoneClientTest, IndependentRoundsCanEachCancelTheirOwnResponse) {
    establish();
    for (std::uint64_t round = 1; round <= 2; ++round) {
        const auto now = round * 10;
        ASSERT_EQ(client.send_content({parts[0]}, false, now), Result::Applied);
        const auto input = client.text_input_id();
        const auto response = "response-" + std::to_string(round);
        acknowledge(now + 1);
        ASSERT_EQ(content("respCreated",
                          "{\"respId\":\"" + response + "\",\"inputId\":\"" + input + "\"}", round),
                  Result::Applied);
        ASSERT_EQ(client.cancel_response(now + 2), Result::Applied);
        const auto request = publisher.messages.back();
        EXPECT_EQ(client.cancel_response(now + 3), Result::Duplicate);
        ASSERT_EQ(client.receive(
                      {"$thing/down/signaling/p/d",
                       ack(request, "{\"respId\":\"" + response + "\",\"state\":\"cancelled\"}")},
                      now + 4),
                  Result::Applied);
        EXPECT_FALSE(client.text_input_pending());
    }
}
// CancelAckRetainsOnlyOriginalTerminal 取消ACK释放输入背压，但只允许原回复唯一终态抵达，不复活内容或副作用。
TEST_F(TypedNoneClientTest, CancelAckRetainsOnlyOriginalTerminal) {
    establish();
    ASSERT_EQ(client.send_content({parts[0]}, false, 4), Result::Applied);
    const auto input = client.text_input_id();
    acknowledge();
    ASSERT_EQ(content("respCreated", "{\"respId\":\"r1\",\"inputId\":\"" + input + "\"}", 1),
              Result::Applied);
    ASSERT_EQ(client.cancel_response(6), Result::Applied);
    const auto cancellation = publisher.messages.back();
    ASSERT_EQ(client.receive({"$thing/down/signaling/p/d",
                              ack(cancellation, R"({"respId":"r1","state":"cancelled"})")}, 7),
              Result::Applied);
    EXPECT_FALSE(client.text_input_pending());
    EXPECT_EQ(content("respTextDelta", R"({"respId":"r1","contentIndex":0,"delta":"迟到"})", 2),
              Result::Stale);
    EXPECT_EQ(content("toolCallStart", R"({"respId":"r1","toolCallId":"old","name":"old","arguments":{}})", 3),
              Result::Stale);
    const auto terminal = "{\"respId\":\"r1\",\"inputId\":\"" + input +
                          "\",\"contentCount\":0,\"status\":\"cancelled\",\"reason\":\"userCancelled\",\"audioState\":\"none\"}";
    EXPECT_EQ(content("respDone", terminal, 4), Result::Applied);
    EXPECT_EQ(content("respDone", terminal, 5), Result::Stale);
    EXPECT_FALSE(client.text_input_pending());
    EXPECT_EQ(client.send_content({parts[0]}, false, 8), Result::Applied);
}

// CancelTerminalBeforeAckIsNotReopened MQTT跨Topic可先收到Done；后到取消ACK不能重新开放同回复的终态槽。
TEST_F(TypedNoneClientTest, CancelTerminalBeforeAckIsNotReopened) {
    establish();
    ASSERT_EQ(client.send_content({parts[0]}, false, 4), Result::Applied);
    const auto input = client.text_input_id();
    acknowledge();
    ASSERT_EQ(content("respCreated", "{\"respId\":\"r1\",\"inputId\":\"" + input + "\"}", 1),
              Result::Applied);
    ASSERT_EQ(client.cancel_response(6), Result::Applied);
    const auto cancellation = publisher.messages.back();
    const auto terminal = "{\"respId\":\"r1\",\"inputId\":\"" + input +
                          "\",\"contentCount\":0,\"status\":\"cancelled\",\"reason\":\"userCancelled\",\"audioState\":\"none\"}";
    ASSERT_EQ(content("respDone", terminal, 2), Result::Applied);
    ASSERT_EQ(client.receive({"$thing/down/signaling/p/d",
                              ack(cancellation, R"({"respId":"r1","state":"cancelled"})")}, 7),
              Result::Applied);
    EXPECT_EQ(content("respDone", terminal, 3), Result::Stale);
    ASSERT_EQ(client.send_content({parts[0]}, false, 8), Result::Applied);
    EXPECT_EQ(content("respDone", terminal, 4), Result::Stale);
    EXPECT_TRUE(client.text_input_pending());
}

// TypedCancelAndTimeoutDoNotResurrectAttachments 原取消ACK与回复终态分层，断网/超时后能力立即关闭。
TEST_F(TypedNoneClientTest, TypedCancelAndTimeoutDoNotResurrectAttachments) {
    establish();
    ASSERT_EQ(client.send_content({parts[1]}, false, 4), Result::Applied);
    const auto input = client.text_input_id();
    acknowledge();
    ASSERT_EQ(content("respCreated", "{\"respId\":\"r1\",\"inputId\":\"" + input + "\"}", 1),
              Result::Applied);
    ASSERT_EQ(client.cancel_response(6), Result::Applied);
    const auto cancellation = publisher.messages.back();
    EXPECT_EQ(client.cancel_response(7), Result::Duplicate);
    EXPECT_EQ(client.send_content({parts[2]}, false, 7), Result::Busy);
    ASSERT_EQ(client.receive({"$thing/down/signaling/p/d",
                              ack(cancellation, R"({"respId":"r1","state":"cancelled"})")},
                             8),
              Result::Applied);
    ASSERT_EQ(client.send_content({parts[2]}, false, 9), Result::Applied);
    const auto request = publisher.messages.back();
    const auto pending_input = client.text_input_id();  // 终态会清空状态，迟到ACK仍必须携原有效ID。
    const auto retry_count = publisher.messages.size();
    client.tick(10009);
    ASSERT_EQ(publisher.messages.size(), retry_count + 1);
    EXPECT_EQ(publisher.messages.back().token, request.token);
    EXPECT_EQ(publisher.messages.back().payload, request.payload);
    client.tick(30009);
    EXPECT_EQ(client.state(), VoiceState::Failed);
    EXPECT_FALSE(client.supports_content(InputContentKind::File));
    EXPECT_EQ(client.receive(
                  {"$thing/down/message/p/d",
                   ack(request, "{\"inputId\":\"" + pending_input + "\",\"state\":\"accepted\"}")},
                  30010),
              Result::Stale);
}

// TypedInvalidInputHasNoPublish 有能力也不能发送URL/非法元数据，原件授权不由设备声明替代。
TEST_F(TypedNoneClientTest, TypedInvalidInputHasNoPublish) {
    establish();
    const auto count = publisher.messages.size();
    EXPECT_EQ(client.send_content({}, false, 4), Result::Invalid);
    auto invalid = parts[1];
    invalid.content_ref = "https://private.invalid/object";
    EXPECT_EQ(client.send_content({invalid}, false, 4), Result::Invalid);
    EXPECT_EQ(client.send_content({{static_cast<InputContentKind>(99), {}, {}, {}}}, false, 4),
              Result::Unsupported);
    EXPECT_EQ(client.send_content(std::vector<InputContentPart>(17, parts[1]), false, 4),
              Result::Invalid);
    EXPECT_EQ(publisher.messages.size(), count);
    EXPECT_FALSE(client.text_input_pending());
}

// TypedVoiceUsesSelectedCapabilitiesInBothDuplexModes
// 图片/文件复用原语音会话，不开放额外输入或旁路采集。
TEST(VoiceClientTypedSelection, TypedVoiceUsesSelectedCapabilitiesInBothDuplexModes) {
    for (const bool full : {false, true}) {
        SCOPED_TRACE(full ? "fullDuplex" : "halfDuplex");
        ClientPublisher publisher;  // 正式编码发布端口，两个模式独立原连接。
        ClientMedia media;          // 只模拟驱动，不伪造媒体网络或模型回复。
        VoiceCapability capability{{true, true, true}, {16000, 1, 20}, 1, true};
        capability.image_input = capability.file_input = true;
        VoiceClient client(publisher, media, "$thing/up/signaling/p/d", "$thing/down/signaling/p/d",
                           capability, "typed-voice");
        const auto receive = [&](const std::string& body, std::uint64_t now) {
            return client.receive({"$thing/down/signaling/p/d", body}, now);
        };  // selected必须来自真实sessionCreated，不能直接设置客户端内部字段。
        EXPECT_FALSE(client.supports_content(InputContentKind::Image));
        ASSERT_EQ(client.connect(0), Result::Applied);
        ASSERT_EQ(receive(ack(publisher.messages.back(), R"({"acceptedRevision":1})"), 1),
                  Result::Applied);
        Envelope created;
        ASSERT_EQ(decode_envelope(created_reply(publisher.messages.back(), full, true), created),
                  DecodeResult::Ok);
        const auto input = created.payload.find(R"(["text"])");
        ASSERT_NE(input, std::string::npos);
        created.payload.replace(input, 8, R"(["text","image"])");
        std::string body;
        ASSERT_EQ(encode_envelope(created, body), DecodeResult::Ok);
        ASSERT_EQ(receive(body, 2), Result::Applied);
        EXPECT_FALSE(client.supports_content(InputContentKind::Image));
        ASSERT_EQ(client.media_ready(3), Result::Applied);
        ASSERT_EQ(
            receive(ack(publisher.messages.back(), R"({"mediaRevision":1,"state":"connected"})"),
                    4),
            Result::Applied);
        ASSERT_EQ(
            receive(event("sessionEstablished", R"({"state":"established","mediaRevision":1})", 1),
                    5),
            Result::Applied);
        EXPECT_TRUE(client.supports_content(InputContentKind::Text));
        EXPECT_TRUE(client.supports_content(InputContentKind::Image));
        EXPECT_FALSE(client.supports_content(InputContentKind::File));
        const auto count = publisher.messages.size();
        EXPECT_EQ(client.send_content({{InputContentKind::File, {}, "artifact_file", "text/plain"}},
                                      false, 6),
                  Result::Unsupported);
        EXPECT_EQ(publisher.messages.size(), count);
        ASSERT_EQ(client.send_content(
                      {{InputContentKind::Image, {}, "artifact_image", "image/png"}}, true, 6),
                  Result::Applied);
        const auto request = publisher.messages.back();
        const auto original_input = client.text_input_id();
        ASSERT_FALSE(original_input.empty());
        EXPECT_EQ(client.receive({"$thing/down/message/p/d", ack(request, "{}", 422)}, 7),
                  Result::Unsupported);
        EXPECT_EQ(client.text_input_id(), original_input);
        EXPECT_EQ(client.receive({"$thing/down/message/p/d", ack(request, "{}", 422)}, 8),
                  Result::Stale);
        ASSERT_EQ(client.start_input(9), Result::Applied);
        ASSERT_EQ(receive(ack(publisher.messages.back(), publisher.messages.back().payload), 10),
                  Result::Applied);
        EXPECT_TRUE(client.capturing());
        const auto capturing_count = publisher.messages.size();
        EXPECT_EQ(client.send_content(
                      {{InputContentKind::Image, {}, "artifact_image", "image/png"}}, false, 11),
                  Result::Busy);
        EXPECT_EQ(publisher.messages.size(), capturing_count);
        ASSERT_EQ(client.end(12), Result::Applied);
        EXPECT_FALSE(client.supports_content(InputContentKind::Image));
        EXPECT_EQ(client.send_content(
                      {{InputContentKind::Image, {}, "artifact_image", "image/png"}}, false, 13),
                  Result::Stale);
        EXPECT_FALSE(media.capturing);
    }
}

// UnofferedSelectionFailsBeforeMediaInstall 平台不能选择设备未声明附件，旧缺省仍无附件权限。
TEST(VoiceClientTypedSelection, UnofferedSelectionFailsBeforeMediaInstall) {
    ClientPublisher publisher;  // 原设备只声明文字/音频，附件缺省false。
    ClientMedia media;          // 验证非法选择没有安装媒体或发Ready。
    VoiceClient client(publisher, media, "$thing/up/signaling/p/d", "$thing/down/signaling/p/d",
                       {{true, true, true}, {16000, 1, 20}, 1, true}, "unoffered");
    ASSERT_EQ(client.connect(0), Result::Applied);
    ASSERT_EQ(client.receive({"$thing/down/signaling/p/d",
                              ack(publisher.messages.back(), R"({"acceptedRevision":1})")},
                             1),
              Result::Applied);
    Envelope created;
    ASSERT_EQ(decode_envelope(created_reply(publisher.messages.back(), true, true), created),
              DecodeResult::Ok);
    const auto input = created.payload.find(R"(["text"])");
    ASSERT_NE(input, std::string::npos);
    created.payload.replace(input, 8, R"(["text","image"])");
    std::string body;
    ASSERT_EQ(encode_envelope(created, body), DecodeResult::Ok);
    const auto count = publisher.messages.size();
    EXPECT_EQ(client.receive({"$thing/down/signaling/p/d", body}, 2), Result::Invalid);
    EXPECT_FALSE(media.configured);
    EXPECT_FALSE(client.supports_content(InputContentKind::Image));
    EXPECT_EQ(publisher.messages.size(), count);
}
TEST_F(VoiceClientTest, DoesNotStartMediaBeforeBothReadinessConfirmations) {
    negotiate();
    EXPECT_TRUE(media.configured);
    EXPECT_FALSE(media.capturing);
    EXPECT_EQ(client.start_input(2), Result::Stale);
    const auto established =
        event("sessionEstablished", R"({"state":"established","mediaRevision":1})", 1);
    EXPECT_EQ(receive(established), Result::Stale);
    ASSERT_EQ(client.media_ready(2), Result::Applied);
    EXPECT_EQ(client.media_ready(2), Result::Duplicate);
    ASSERT_EQ(receive(established, 3), Result::Applied);
    EXPECT_EQ(client.state(), VoiceState::Negotiating);
    ASSERT_EQ(
        receive(ack(publisher.messages.back(), R"({"mediaRevision":1,"state":"connected"})"), 4),
        Result::Applied);
    EXPECT_EQ(client.state(), VoiceState::Established);
    EXPECT_FALSE(media.capturing);
    EXPECT_EQ(receive(established, 5), Result::Stale);
}
TEST_F(VoiceClientTest, StopCarriesActualLastPacketOrZeroForEmptyInput) {
    establish();
    capture();
    ASSERT_EQ(client.stop_input(7), Result::Applied);
    EXPECT_NE(publisher.messages.back().payload.find("\"lastPacketSeq\":0"), std::string::npos);
    ASSERT_EQ(receive(ack(publisher.messages.back(), publisher.messages.back().payload), 8),
              Result::Applied);
    ASSERT_EQ(client.start_input(9), Result::Applied);
    ASSERT_EQ(receive(ack(publisher.messages.back(), publisher.messages.back().payload), 10),
              Result::Applied);
    media.next_sequence = 512;
    ASSERT_EQ(client.stop_input(11), Result::Applied);
    EXPECT_NE(publisher.messages.back().payload.find("\"lastPacketSeq\":384"), std::string::npos);
}
TEST_F(VoiceClientTest, ExhaustedInputSequenceFailsStopClosed) {
    establish();
    capture();
    media.next_sequence = 0;
    EXPECT_EQ(client.stop_input(7), Result::Busy);
    EXPECT_EQ(client.state(), VoiceState::Failed);
    EXPECT_FALSE(media.capturing);
}
TEST_F(VoiceClientTest, LateStartAckCannotUndoStopAndResumeUsesNewInput) {
    establish();
    ASSERT_EQ(client.start_input(5), Result::Applied);
    const auto start = publisher.messages.back();
    ASSERT_EQ(client.stop_input(6), Result::Applied);
    const auto stop = publisher.messages.back();
    EXPECT_EQ(receive(ack(start, start.payload), 7), Result::Applied);
    EXPECT_FALSE(media.capturing);
    EXPECT_EQ(media.captures, 0);
    EXPECT_EQ(receive(ack(stop, stop.payload), 8), Result::Applied);
    ASSERT_EQ(client.start_input(9), Result::Applied);
    const auto resumed = publisher.messages.back();
    EXPECT_NE(start.payload, resumed.payload);
    EXPECT_EQ(receive(ack(resumed, resumed.payload), 10), Result::Applied);
    EXPECT_TRUE(media.capturing);
    EXPECT_EQ(receive(ack(start, start.payload), 11), Result::Stale);
}
TEST_F(VoiceClientTest, FullDuplexKeepsCaptureAndCancellationIsScoped) {
    establish();
    EXPECT_EQ(client.mode(), Mode::FullDuplex);
    capture();
    ASSERT_EQ(receive(audio_event("respAudioStart"), 7), Result::Applied);
    EXPECT_TRUE(client.capturing());
    EXPECT_TRUE(client.playing());
    EXPECT_EQ(client.playback_drained(client.playback_epoch()), Result::Busy);
    ASSERT_EQ(client.cancel_response(8), Result::Applied);
    EXPECT_TRUE(client.capturing());
    EXPECT_FALSE(client.playing());
    EXPECT_EQ(publisher.messages.back().method, "respCancel");
    EXPECT_EQ(receive(audio_event("respAudioStart"), 9), Result::Stale);
    EXPECT_EQ(media.plays, 1);
    EXPECT_EQ(
        receive(ack(publisher.messages.back(), R"({"respId":"reply","state":"cancelled"})"), 10),
        Result::Applied);
    EXPECT_TRUE(client.capturing());
}
TEST_F(VoiceClientTest, StaleOrOverlappingReplyCannotChangeMediaFence) {
    establish();
    capture();
    ASSERT_EQ(receive(audio_event("respAudioStart")), Result::Applied);
    EXPECT_EQ(receive(audio_event("respAudioStart", 3)), Result::Stale);
    EXPECT_EQ(media.preparations, 1);
    EXPECT_EQ(receive(event("respAudioStart",
                            R"({"respId":"other","trackId":"speaker-1","mediaRevision":1})", 4)),
              Result::Busy);
    EXPECT_EQ(media.preparations, 1);
    ASSERT_EQ(receive(audio_event("respAudioDone", 5)), Result::Applied);
    EXPECT_EQ(receive(audio_event("respAudioDone", 6)), Result::Duplicate);
    EXPECT_EQ(media.finishes, 1);
    ASSERT_EQ(client.playback_drained(client.playback_epoch()), Result::Applied);
    EXPECT_EQ(receive(audio_event("respAudioDone", 7)), Result::Duplicate);
    EXPECT_EQ(media.finishes, 1);
}
// 网络结束后的残留播放可停止，重复和已排空事件不能冒充实际中断。
TEST_F(VoiceClientTest, InterruptAfterNetworkDoneStopsOnlyPendingPlayback) {
    establish();
    capture();
    ASSERT_EQ(receive(audio_event("respAudioStart", 2)), Result::Applied);
    ASSERT_EQ(receive(audio_event("respAudioDone", 3)), Result::Applied);
    EXPECT_TRUE(client.playing());
    EXPECT_EQ(client.interrupted_playbacks(), 0U);
    ASSERT_EQ(receive(audio_event("audioSpeechInterrupted", 4)), Result::Applied);
    EXPECT_FALSE(media.playing);
    EXPECT_TRUE(client.capturing());
    EXPECT_EQ(client.interrupted_playbacks(), 1U);
    EXPECT_EQ(receive(audio_event("audioSpeechInterrupted", 5)), Result::Duplicate);
    EXPECT_EQ(client.interrupted_playbacks(), 1U);
}
// 正常排空后收到定向停止只能确认重复，不能增加真实播放中断指标。
TEST_F(VoiceClientTest, InterruptAfterDrainIsNotCounted) {
    establish();
    ASSERT_EQ(receive(audio_event("respAudioStart", 2)), Result::Applied);
    ASSERT_EQ(receive(audio_event("respAudioDone", 3)), Result::Applied);
    ASSERT_EQ(client.playback_drained(client.playback_epoch()), Result::Applied);
    EXPECT_EQ(receive(audio_event("audioSpeechInterrupted", 4)), Result::Duplicate);
    EXPECT_EQ(client.interrupted_playbacks(), 0U);
}
TEST_F(VoiceClientTest, HalfDuplexStopsInputBeforeOutputAndWaitsForDriverDrain) {
    establish(false);
    capture();
    ASSERT_EQ(receive(audio_event("respAudioStart"), 7), Result::Applied);
    EXPECT_FALSE(client.capturing());
    EXPECT_TRUE(client.playing());
    EXPECT_EQ(publisher.messages.back().method, "audioStop");
    EXPECT_EQ(client.start_input(8), Result::Busy);
    ASSERT_EQ(receive(audio_event("respAudioDone", 3), 9), Result::Applied);
    EXPECT_TRUE(client.playing());
    EXPECT_EQ(client.playback_drained(client.playback_epoch() + 1), Result::Stale);
    EXPECT_EQ(client.playback_drained(client.playback_epoch()), Result::Applied);
    EXPECT_FALSE(client.playing());
    EXPECT_EQ(client.start_input(10), Result::Applied);
}
// 半双工恢复分别等待原回复网络结束、原epoch实际排空和新audioStart
// ACK；不能靠下一轮点击绕过任何屏障。
TEST_F(VoiceClientTest, HalfDuplexResumeWaitsForNetworkDrainAndOriginalStartAck) {
    establish(false);
    ASSERT_EQ(client.mode(), Mode::HalfDuplex);
    ASSERT_EQ(receive(audio_event("respAudioStart", 2), 5), Result::Applied);
    const auto epoch = client.playback_epoch();  // 固定原回复实际播放代次，不用后来当前值补关联。
    const auto published = publisher.messages.size();  // Busy不得发布新的audioStart或留下采集意图。
    ASSERT_NE(epoch, 0U);
    ASSERT_TRUE(client.playing());
    ASSERT_TRUE(media.playing);
    ASSERT_EQ(media.captures, 0);

    EXPECT_EQ(client.start_input(6), Result::Busy);
    EXPECT_EQ(client.playback_drained(epoch), Result::Busy);
    EXPECT_EQ(publisher.messages.size(), published);
    EXPECT_EQ(media.finishes, 0);
    EXPECT_EQ(media.captures, 0);
    EXPECT_FALSE(client.capturing());
    EXPECT_FALSE(media.capturing);

    ASSERT_EQ(receive(audio_event("respAudioDone", 3), 7), Result::Applied);
    ASSERT_EQ(media.finishes, 1);
    EXPECT_EQ(client.start_input(8), Result::Busy);
    EXPECT_EQ(publisher.messages.size(), published);
    EXPECT_TRUE(client.playing());
    EXPECT_TRUE(media.playing);
    EXPECT_EQ(media.captures, 0);
    EXPECT_FALSE(client.capturing());
    EXPECT_FALSE(media.capturing);

    EXPECT_EQ(client.playback_drained(epoch + 1), Result::Stale);
    ASSERT_EQ(client.playback_drained(epoch), Result::Applied);
    ASSERT_FALSE(client.playing());
    ASSERT_FALSE(media.playing);
    ASSERT_EQ(client.start_input(9), Result::Applied);
    ASSERT_EQ(publisher.messages.size(), published + 1);
    const auto start = publisher.messages.back();  // 仅关联实际新audioStart的原token和原inputId。
    ASSERT_EQ(start.method, "audioStart");
    EXPECT_EQ(media.captures, 0);
    EXPECT_FALSE(client.capturing());
    EXPECT_FALSE(media.capturing);
    EXPECT_EQ(client.start_input(10), Result::Busy);
    EXPECT_EQ(publisher.messages.size(), published + 1);
    EXPECT_EQ(media.captures, 0);

    ASSERT_EQ(receive(ack(start, start.payload), 11), Result::Applied);
    EXPECT_EQ(media.captures, 1);
    EXPECT_TRUE(client.capturing());
    EXPECT_TRUE(media.capturing);
}
TEST_F(VoiceClientTest, WrongTopicObjectAndSessionCannotConsumeAck) {
    establish();
    ASSERT_EQ(client.start_input(5), Result::Applied);
    const auto request = publisher.messages.back();
    EXPECT_EQ(client.receive({"$thing/down/signaling/p/other", ack(request, request.payload)}, 6),
              Result::Invalid);
    auto other = request;
    other.session = "other";
    EXPECT_EQ(receive(ack(other, other.payload), 6), Result::Stale);
    EXPECT_EQ(receive(ack(request, R"({"inputId":"other"})"), 6), Result::Invalid);
    EXPECT_FALSE(client.capturing());
    EXPECT_EQ(receive(ack(request, request.payload), 7), Result::Applied);
    EXPECT_TRUE(client.capturing());
}
TEST_F(VoiceClientTest, DisconnectReleasesBothSidesAndRejectsOldEvents) {
    establish();
    capture();
    ASSERT_EQ(receive(audio_event("respAudioStart"), 7), Result::Applied);
    client.disconnected(8);
    EXPECT_FALSE(media.configured);
    EXPECT_FALSE(media.capturing);
    EXPECT_FALSE(media.playing);
    EXPECT_EQ(receive(audio_event("respAudioStart", 3), 9), Result::Stale);
    const auto old = publisher.messages.front();
    ASSERT_EQ(client.connect(10), Result::Applied);
    EXPECT_NE(old.token, publisher.messages.back().token);
    EXPECT_EQ(receive(ack(old, R"({"acceptedRevision":1})"), 11), Result::Stale);
}
TEST_F(VoiceClientTest, TimeoutAndRejectedStartFailClosed) {
    establish();
    capture();
    ASSERT_EQ(client.stop_input(7), Result::Applied);
    client.tick(30007);
    EXPECT_EQ(client.state(), VoiceState::Failed);
    EXPECT_FALSE(media.configured);
    ASSERT_EQ(client.connect(30008), Result::Applied);
    EXPECT_EQ(receive(ack(publisher.messages.back(), "{}", 403), 30009), Result::Unsupported);
    EXPECT_EQ(client.state(), VoiceState::Failed);
}
TEST_F(VoiceClientTest, ConfigureFailureAndNegotiationTimeoutReleaseResources) {
    media.configure_ok = false;
    ASSERT_EQ(client.connect(0), Result::Applied);
    ASSERT_EQ(receive(ack(publisher.messages.back(), R"({"acceptedRevision":1})")),
              Result::Applied);
    EXPECT_EQ(receive(created_reply(publisher.messages.back())), Result::DriverFailed);
    EXPECT_EQ(client.state(), VoiceState::Failed);
    media.configure_ok = true;
    negotiate();
    client.tick(30001);
    EXPECT_EQ(client.state(), VoiceState::Failed);
    EXPECT_FALSE(media.configured);
}
TEST_F(VoiceClientTest, EndStopsImmediatelyAndOnlyMatchingTerminalAckCompletes) {
    establish();
    capture();
    ASSERT_EQ(client.end(7), Result::Applied);
    EXPECT_EQ(client.state(), VoiceState::Ending);
    EXPECT_FALSE(media.capturing);
    EXPECT_EQ(client.start_input(8), Result::Stale);
    const auto request = publisher.messages.back();
    Envelope response;
    response.token = request.token;
    response.session = request.session;
    response.method = "sessionEnded";
    response.request = false;
    response.code = 200;
    response.payload = R"({"state":"ended","reason":"userStopped"})";
    std::string body;
    ASSERT_EQ(encode_envelope(response, body), DecodeResult::Ok);
    EXPECT_EQ(receive(body, 9), Result::Applied);
    EXPECT_EQ(client.state(), VoiceState::Idle);
    EXPECT_EQ(receive(body, 10), Result::Stale);
}
// 独立故障终态无需本地sessionEnd请求，也必须停止双向媒体并拒绝重复及迟到回复。
TEST_F(VoiceClientTest, AsyncMediaFailureTerminalReleasesBothDirections) {
    establish();
    capture();
    ASSERT_EQ(receive(audio_event("respAudioStart"), 7), Result::Applied);
    ASSERT_TRUE(media.capturing);
    ASSERT_TRUE(media.playing);
    const std::string ended =
        R"({"msgToken":"media-failed-event","method":"sessionEnded","sessionId":"session","eventSeq":3,"data":{"state":"ended","reason":"mediaFailed"}})";
    EXPECT_EQ(receive(ended, 8), Result::Applied);
    EXPECT_EQ(client.state(), VoiceState::Idle);
    EXPECT_FALSE(media.configured);
    EXPECT_FALSE(media.capturing);
    EXPECT_FALSE(media.playing);
    EXPECT_EQ(receive(ended, 9), Result::Stale);
    EXPECT_EQ(receive(audio_event("respAudioStart", 4), 10), Result::Stale);
}
TEST_F(VoiceClientTest, DriverFailureNeverLeavesSessionEstablished) {
    establish();
    media.capture_ok = false;
    ASSERT_EQ(client.start_input(5), Result::Applied);
    const auto request = publisher.messages.back();
    EXPECT_EQ(receive(ack(request, request.payload), 6), Result::DriverFailed);
    EXPECT_EQ(client.state(), VoiceState::Failed);
    EXPECT_FALSE(media.configured);
}
TEST_F(VoiceClientTest, ResponseErrorDoesNotCloseInputOrTreatEventAsAck) {
    establish();
    capture();
    ASSERT_EQ(receive(audio_event("respAudioStart"), 7), Result::Applied);
    Envelope failure;
    ASSERT_EQ(
        decode_envelope(
            event(
                "sessionError",
                R"({"scope":"response","respId":"reply","reason":"modelTimeout","retryable":true})",
                3),
            failure),
        DecodeResult::Ok);
    failure.code = 502;
    std::string body;
    ASSERT_EQ(encode_envelope(failure, body), DecodeResult::Ok);
    EXPECT_EQ(receive(body, 8), Result::Applied);
    EXPECT_TRUE(client.capturing());
    EXPECT_FALSE(client.playing());
    EXPECT_EQ(client.state(), VoiceState::Established);
    EXPECT_EQ(receive(body, 9), Result::Stale);
    failure.event_sequence = 4;
    failure.payload = R"({"scope":"session","reason":"permissionRevoked","retryable":false})";
    ASSERT_EQ(encode_envelope(failure, body), DecodeResult::Ok);
    EXPECT_EQ(receive(body, 10), Result::Applied);
    EXPECT_EQ(client.state(), VoiceState::Failed);
    EXPECT_FALSE(media.configured);
}
TEST(VoiceClientConfiguration, RejectsUnboundIdentityWildcardAndRestartToken) {
    ClientPublisher publisher;
    ClientMedia media;
    for (const auto* topic : {"$thing/down/signaling/p/other", "$thing/down/ai/p/d"}) {
        VoiceClient client(publisher, media, "$thing/up/signaling/p/d", topic, {}, "boot");
        EXPECT_EQ(client.connect(0), Result::Invalid);
    }
    for (const auto* id : {"", "bad/id", "bad\n"}) {
        VoiceClient client(publisher, media, "$thing/up/signaling/p/d", "$thing/down/signaling/p/d",
                           {}, id);
        EXPECT_EQ(client.connect(0), Result::Invalid);
    }
    VoiceClient wildcard(publisher, media, "$thing/up/signaling/p/+", "$thing/down/signaling/p/+",
                         {}, "boot");
    EXPECT_EQ(wildcard.connect(0), Result::Invalid);
    EXPECT_TRUE(publisher.messages.empty());
}
// 真实建连诊断回归：云端拒绝不得被后续资源关闭抹成只有Idle的失败。
TEST_F(VoiceClientTest, RejectionSurvivesCloseAndResetsOnNewConnection) {
    ASSERT_EQ(client.connect(0), Result::Applied);
    ASSERT_EQ(receive(ack(publisher.messages.back(), R"({"acceptedRevision":1})")),
              Result::Applied);
    const auto request = publisher.messages.back();
    ASSERT_EQ(receive(ack(request, "{}", 403), 2), Result::Unsupported);
    ASSERT_TRUE(client.last_rejection());
    EXPECT_EQ(client.last_rejection()->method, "sessionCreate");
    EXPECT_EQ(client.last_rejection()->code, 403);
    client.disconnected(3);
    EXPECT_EQ(client.state(), VoiceState::Idle);
    ASSERT_TRUE(client.last_rejection());
    EXPECT_EQ(client.last_rejection()->code, 403);
    ASSERT_EQ(client.connect(4), Result::Applied);
    EXPECT_FALSE(client.last_rejection());
    EXPECT_EQ(receive(ack(request, "{}", 503), 5), Result::Stale);
    EXPECT_FALSE(client.last_rejection());
}
// 原能力403必须来自匹配token/method的ACK，诊断不能改变拒绝或以新token重做。
TEST_F(VoiceClientTest, CapabilityRejectionReasonIsBoundedAndCorrelated) {
    for (const auto* reason : {"focusDeviceUnavailable", "requestDenied", "synthetic-private"}) {
        ASSERT_EQ(client.connect(0), Result::Applied);
        const auto request = publisher.messages.back();
        auto denied = ack(request, "", 403);
        const auto position = denied.find("permissionDenied");
        ASSERT_NE(position, std::string::npos);
        denied.replace(position, std::string("permissionDenied").size(), reason);
        Envelope wrong;
        ASSERT_EQ(decode_envelope(denied, wrong), DecodeResult::Ok);
        wrong.token = "wrong-original-token";
        std::string stale;
        ASSERT_EQ(encode_envelope(wrong, stale), DecodeResult::Ok);
        EXPECT_EQ(receive(stale, 1), Result::Stale);
        EXPECT_FALSE(client.last_rejection());
        const auto count = publisher.messages.size();
        ASSERT_EQ(receive(denied, 2), Result::Unsupported);
        EXPECT_EQ(client.state(), VoiceState::Failed);
        EXPECT_EQ(publisher.messages.size(), count);
        ASSERT_TRUE(client.last_rejection());
        EXPECT_EQ(client.last_rejection()->reason,
                  std::string(reason) == "synthetic-private" ? "" : reason);
        EXPECT_FALSE(media.configured);
        client.disconnected(3);
        EXPECT_EQ(client.last_rejection()->reason,
                  std::string(reason) == "synthetic-private" ? "" : reason);
    }
    ASSERT_EQ(client.connect(4), Result::Applied);
    EXPECT_FALSE(client.last_rejection());
}
// 方法或code不属于基线403时不得借用已知reason枚举，也不得导出自由服务端正文。
TEST_F(VoiceClientTest, CapabilityRejectionReasonDoesNotLeakOtherErrors) {
    for (const bool creating : {false, true}) {
        ASSERT_EQ(client.connect(0), Result::Applied);
        if (creating) {
            ASSERT_EQ(receive(ack(publisher.messages.back(), R"({"acceptedRevision":1})")),
                      Result::Applied);
        }
        auto denied = ack(publisher.messages.back(), "", creating ? 403 : 503);
        const auto position = denied.find("permissionDenied");
        ASSERT_NE(position, std::string::npos);
        denied.replace(position, std::string("permissionDenied").size(), "requestDenied");
        EXPECT_EQ(receive(denied, 2), Result::Unsupported);
        ASSERT_TRUE(client.last_rejection());
        EXPECT_TRUE(client.last_rejection()->reason.empty());
        client.disconnected(3);
    }
}
// 上线状态晚于能力上报时有限退避，能力不篡改，持续拒绝最终失败。
TEST_F(VoiceClientTest, CapabilityReportConflictBackoffIsBounded) {
    ASSERT_EQ(client.connect(0), Result::Applied);
    const auto original = publisher.messages.back();
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        const auto request = publisher.messages.back();
        auto denied = ack(request, "", 409);
        const auto position = denied.find("permissionDenied");
        ASSERT_NE(position, std::string::npos);
        denied.replace(position, std::string("permissionDenied").size(), "capabilityNotAccepted");
        const auto now = std::uint64_t{1} + attempt * 501;
        if (attempt == 2) {
            EXPECT_EQ(receive(denied, now), Result::Unsupported);
            EXPECT_EQ(client.state(), VoiceState::Failed);
            break;
        }
        EXPECT_EQ(receive(denied, now), Result::Applied);
        EXPECT_EQ(client.state(), VoiceState::Reporting);
        const auto count = publisher.messages.size();
        client.tick(now + 499);
        EXPECT_EQ(publisher.messages.size(), count);
        client.tick(now + 500);
        ASSERT_EQ(publisher.messages.size(), count + 1);
        EXPECT_EQ(publisher.messages.back().payload, original.payload);
        EXPECT_NE(publisher.messages.back().token, request.token);
        EXPECT_FALSE(client.capturing());
        EXPECT_FALSE(client.playing());
    }
}

// 退避期间断开不能在未来tick复活连接；新的能力ACK仍需正常创建握手。
TEST_F(VoiceClientTest, CapabilityBackoffCanRecoverOrBeAborted) {
    for (const bool disconnect : {false, true}) {
        client.disconnected(0);
        ASSERT_EQ(client.connect(1), Result::Applied);
        auto denied = ack(publisher.messages.back(), "", 409);
        denied.replace(denied.find("permissionDenied"), std::string("permissionDenied").size(),
                       "capabilityNotAccepted");
        ASSERT_EQ(receive(denied, 2), Result::Applied);
        const auto count = publisher.messages.size();
        EXPECT_EQ(receive(denied, 3), Result::Stale);
        if (disconnect)
            client.disconnected(4);
        client.tick(502);
        if (disconnect) {
            EXPECT_EQ(publisher.messages.size(), count);
            EXPECT_EQ(client.state(), VoiceState::Idle);
        } else {
            ASSERT_EQ(publisher.messages.size(), count + 1);
            EXPECT_EQ(receive(ack(publisher.messages.back(), R"({"acceptedRevision":1})"), 503),
                      Result::Applied);
            EXPECT_EQ(client.state(), VoiceState::Creating);
            EXPECT_EQ(publisher.messages.back().method, "sessionCreate");
        }
    }
}

TEST_F(VoiceClientTest, CapabilityRaceRecoveryIsBoundedAndUsesFreshTokens) {
    ASSERT_EQ(client.connect(0), Result::Applied);
    ASSERT_EQ(receive(ack(publisher.messages.back(), R"({"acceptedRevision":1})")),
              Result::Applied);
    const auto first = publisher.messages.back();
    auto denied = ack(first, "{}", 409);
    const auto position = denied.find("permissionDenied");
    ASSERT_NE(position, std::string::npos);
    denied.replace(position, std::string("permissionDenied").size(), "capabilityReportRequired");
    EXPECT_EQ(receive(denied, 2), Result::Applied);
    EXPECT_EQ(client.state(), VoiceState::Reporting);
    EXPECT_EQ(publisher.messages.back().method, "capabilityReport");
    EXPECT_NE(publisher.messages.back().token, first.token);
    EXPECT_FALSE(client.last_rejection());
    const auto count = publisher.messages.size();
    EXPECT_EQ(receive(denied, 3), Result::Stale);
    EXPECT_EQ(publisher.messages.size(), count);
    ASSERT_EQ(receive(ack(publisher.messages.back(), R"({"acceptedRevision":1})"), 4),
              Result::Applied);
    auto again = ack(publisher.messages.back(), "{}", 409);
    again.replace(again.find("permissionDenied"), std::string("permissionDenied").size(),
                  "capabilityReportRequired");
    EXPECT_EQ(receive(again, 5), Result::Unsupported);
    EXPECT_EQ(client.state(), VoiceState::Failed);
    ASSERT_TRUE(client.last_rejection());
    EXPECT_EQ(client.last_rejection()->code, 409);
    EXPECT_FALSE(media.configured);
}
// 无关失败原因不能触发能力重报，也不能掩盖权限或配置错误。
TEST_F(VoiceClientTest, RejectionRequiresMatchingRequestAndTopic) {
    ASSERT_EQ(client.connect(0), Result::Applied);
    auto request = publisher.messages.back();
    request.token = "unrelated";
    EXPECT_EQ(receive(ack(request, "{}", 403)), Result::Stale);
    EXPECT_FALSE(client.last_rejection());
    request = publisher.messages.back();
    EXPECT_EQ(client.receive({"$thing/down/signaling/p/other", ack(request, "{}", 503)}, 2),
              Result::Invalid);
    EXPECT_FALSE(client.last_rejection());
    ASSERT_EQ(receive(ack(request, "{}", 503), 3), Result::Unsupported);
    ASSERT_TRUE(client.last_rejection());
    EXPECT_EQ(client.last_rejection()->method, "capabilityReport");
    EXPECT_EQ(client.last_rejection()->code, 503);
}
// VoiceClientRecovery分别验证半/全双工，同一原会话通过ACK屏障恢复，不制造第二个Established。
class VoiceClientRecovery : public testing::TestWithParam<bool> {
   protected:
    ClientPublisher publisher;  // 保存实际发送的原请求，不能用预造token替代关联。
    ClientMedia media;          // 可控采集/播放及下一代Peer驱动。
    VoiceClient client{
        publisher,
        media,
        "$thing/up/signaling/p/d",
        "$thing/down/signaling/p/d",
        {{true, GetParam(), GetParam()}, {48000, 1, 20}, 1, false, VoiceTransport::WebRTC},
        "recover"};
    // receive仅走正式解码与关联入口，不直接改客户端状态。
    Result receive(const std::string& body, std::uint64_t now) {
        return client.receive({"$thing/down/signaling/p/d", body}, now);
    }
    // answer使用当前实际Offer及指定代次，测试旧Peer回执不能抢占新请求。
    Result answer(const Envelope& offer, std::uint64_t revision, std::uint64_t now) {
        Envelope reply;
        reply.token = offer.token;
        reply.session = "session";
        reply.method = "mediaAnswer";
        reply.request = false;
        reply.code = 200;
        reply.payload = "{\"mediaRevision\":" + std::to_string(revision) +
                        R"(,"webrtc":{"iceMode":"complete","sdp":"v=0\r\no=answer\r\n"}})";
        std::string body;
        EXPECT_EQ(encode_envelope(reply, body), DecodeResult::Ok);
        return receive(body, now);
    }
    // connected先报告驱动事实，再关联平台ACK；两者缺一不允许恢复采集。
    Result connected(std::uint64_t revision, std::uint64_t now) {
        EXPECT_EQ(client.media_ready(now), Result::Applied);
        return receive(
            ack(publisher.messages.back(),
                "{\"mediaRevision\":" + std::to_string(revision) + R"(,"state":"connected"})"),
            now + 1);
    }
    // establish保留原能力/创建/Offer/Answer/Ready/ACK/Established完整屏障。
    void establish() {
        ASSERT_EQ(client.connect(0), Result::Applied);
        ASSERT_EQ(receive(ack(publisher.messages.back(), R"({"acceptedRevision":1})"), 1),
                  Result::Applied);
        ASSERT_EQ(receive(web_created_reply(publisher.messages.back(), GetParam()), 2),
                  Result::Applied);
        ASSERT_EQ(client.media_offer_ready("v=0\r\no=offer\r\n", 3), Result::Applied);
        ASSERT_EQ(answer(publisher.messages.back(), 1, 4), Result::Applied);
        ASSERT_EQ(connected(1, 5), Result::Applied);
        ASSERT_EQ(
            receive(event("sessionEstablished", R"({"state":"established","mediaRevision":1})", 1),
                    7),
            Result::Applied);
        ASSERT_EQ(client.state(), VoiceState::Established);
    }
    // recover只用原disconnected ACK、新Peer及connected ACK；不补发输入启动或建立事件。
    void recover(std::uint64_t now = 102, std::uint64_t revision = 2) {
        const auto disconnect = publisher.messages.back();
        ASSERT_EQ(receive(ack(disconnect, "{\"mediaRevision\":" + std::to_string(revision - 1) +
                                              R"(,"state":"disconnected"})"),
                          now),
                  Result::Applied);
        ASSERT_EQ(media.recovery_revision, revision);
        ASSERT_EQ(client.media_offer_ready("v=0\r\no=new-peer\r\n", now + 1), Result::Applied);
        ASSERT_EQ(answer(publisher.messages.back(), revision, now + 2), Result::Applied);
        ASSERT_EQ(connected(revision, now + 3), Result::Applied);
        ASSERT_EQ(client.state(), VoiceState::Established);
    }
    // reply编码无UDP边界的真实WebRTC事件，代次与事件序号独立校验。
    Result reply(std::string method, std::string id, std::uint64_t revision, std::uint64_t sequence,
                 std::uint64_t now) {
        Envelope message;
        message.token = "reply-" + std::to_string(sequence);
        message.session = "session";
        message.method = std::move(method);
        message.request = false;
        message.event_sequence = sequence;
        message.payload =
            "{\"respId\":\"" + id +
            "\",\"trackId\":\"speaker-1\",\"mediaRevision\":" + std::to_string(revision) + "}";
        std::string body;
        EXPECT_EQ(encode_envelope(message, body), DecodeResult::Ok);
        return receive(body, now);
    }
};
TEST_P(VoiceClientRecovery, OriginalInputResumesOnlyAfterMatchingConnectedACK) {
    establish();
    ASSERT_EQ(client.start_input(8), Result::Applied);
    const auto start = publisher.messages.back();
    ASSERT_EQ(receive(ack(start, start.payload), 9), Result::Applied);
    const auto input = media.captured_input;
    ASSERT_EQ(client.media_disconnected(100), Result::Applied);
    ASSERT_EQ(client.state(), VoiceState::Recovering);
    EXPECT_FALSE(client.capturing());
    EXPECT_EQ(media.capture_stops, 1);
    EXPECT_EQ(client.media_disconnected(101), Result::Duplicate);
    EXPECT_EQ(client.media_offer_ready("v=0\r\no=early\r\n", 101), Result::Stale);
    EXPECT_EQ(client.media_ready(101), Result::Stale);
    EXPECT_EQ(media.recovery_revision, 0U);
    recover();
    EXPECT_TRUE(client.capturing());
    EXPECT_EQ(media.captures, 2);
    EXPECT_EQ(media.captured_input, input);
    EXPECT_EQ(client.session_id(), "session");
    EXPECT_EQ(std::count_if(publisher.messages.begin(), publisher.messages.end(),
                            [](const Envelope& item) { return item.method == "audioStart"; }),
              1);
    ASSERT_EQ(client.media_disconnected(200), Result::Applied);
    recover(202, 3);
    EXPECT_EQ(media.captures, 3);
    EXPECT_EQ(media.captured_input, input);
}
TEST_P(VoiceClientRecovery, ExplicitPauseDuringRecoveryWinsOverLateStartACK) {
    establish();
    ASSERT_EQ(client.start_input(8), Result::Applied);
    const auto start = publisher.messages.back();
    ASSERT_EQ(client.media_disconnected(100), Result::Applied);
    const auto disconnect = publisher.messages.back();
    ASSERT_EQ(client.stop_input(101), Result::Applied);
    const auto stop = publisher.messages.back();
    ASSERT_EQ(receive(ack(start, start.payload), 102), Result::Applied);
    EXPECT_EQ(media.captures, 0);
    ASSERT_EQ(receive(ack(stop, stop.payload), 103), Result::Applied);
    ASSERT_EQ(receive(ack(disconnect, R"({"mediaRevision":1,"state":"disconnected"})"), 104),
              Result::Applied);
    ASSERT_EQ(client.media_offer_ready("v=0\r\no=new\r\n", 105), Result::Applied);
    ASSERT_EQ(answer(publisher.messages.back(), 2, 106), Result::Applied);
    ASSERT_EQ(connected(2, 107), Result::Applied);
    EXPECT_EQ(client.state(), VoiceState::Established);
    EXPECT_FALSE(client.capturing());
    EXPECT_EQ(media.captures, 0);
    EXPECT_EQ(client.stop_input(109), Result::Duplicate);
    ASSERT_EQ(client.start_input(110), Result::Applied);
    const auto resumed = publisher.messages.back();
    ASSERT_EQ(receive(ack(resumed, resumed.payload), 111), Result::Applied);
    EXPECT_TRUE(client.capturing());
    EXPECT_NE(resumed.payload, start.payload);
}
TEST_P(VoiceClientRecovery, PendingStartACKWaitsForNewPeerReadiness) {
    establish();
    ASSERT_EQ(client.start_input(8), Result::Applied);
    const auto start = publisher.messages.back();
    ASSERT_EQ(client.media_disconnected(100), Result::Applied);
    const auto disconnect = publisher.messages.back();
    ASSERT_EQ(receive(ack(start, start.payload), 101), Result::Applied);
    EXPECT_EQ(media.captures, 0);
    ASSERT_EQ(receive(ack(disconnect, R"({"mediaRevision":1,"state":"disconnected"})"), 102),
              Result::Applied);
    ASSERT_EQ(client.media_offer_ready("v=0\r\no=new\r\n", 103), Result::Applied);
    ASSERT_EQ(answer(publisher.messages.back(), 2, 104), Result::Applied);
    EXPECT_FALSE(client.capturing());
    ASSERT_EQ(connected(2, 105), Result::Applied);
    EXPECT_TRUE(client.capturing());
    EXPECT_EQ(media.captures, 1);
    EXPECT_EQ(start.payload.find(media.captured_input), 12U);
}
TEST_P(VoiceClientRecovery, OldGenerationCannotStartPlaybackOrConsumeCurrentACK) {
    establish();
    ASSERT_EQ(reply("respAudioStart", "old", 1, 2, 8), Result::Applied);
    const auto old_epoch = client.playback_epoch();
    ASSERT_EQ(client.media_disconnected(100), Result::Applied);
    const auto disconnect = publisher.messages.back();
    EXPECT_EQ(media.playback_stops, 1);
    EXPECT_EQ(receive(ack(disconnect, R"({"mediaRevision":2,"state":"disconnected"})"), 101),
              Result::Invalid);
    ASSERT_EQ(receive(ack(disconnect, R"({"mediaRevision":1,"state":"disconnected"})"), 102),
              Result::Applied);
    ASSERT_EQ(client.media_offer_ready("v=0\r\no=new\r\n", 103), Result::Applied);
    const auto offer = publisher.messages.back();
    EXPECT_EQ(answer(offer, 1, 104), Result::Invalid);
    EXPECT_EQ(media.answer, "");
    ASSERT_EQ(answer(offer, 2, 105), Result::Applied);
    EXPECT_EQ(reply("respAudioStart", "gap", 2, 3, 106), Result::Stale);
    ASSERT_EQ(client.media_ready(107), Result::Applied);
    const auto ready = publisher.messages.back();
    EXPECT_EQ(receive(ack(ready, R"({"mediaRevision":1,"state":"connected"})"), 108),
              Result::Invalid);
    ASSERT_EQ(receive(ack(ready, R"({"mediaRevision":2,"state":"connected"})"), 109),
              Result::Applied);
    EXPECT_FALSE(client.playing());
    EXPECT_EQ(reply("respAudioStart", "gap", 2, 3, 109), Result::Stale);
    EXPECT_EQ(reply("respAudioDone", "old", 1, 999, 110), Result::Stale);
    EXPECT_EQ(reply("respAudioStart", "old", 2, 4, 111), Result::Stale);
    ASSERT_EQ(reply("respAudioStart", "new", 2, 5, 112), Result::Applied);
    EXPECT_EQ(client.playback_drained(old_epoch), Result::Stale);
    EXPECT_TRUE(client.playing());
    EXPECT_EQ(media.plays, 2);
}
TEST_P(VoiceClientRecovery, FirstDeadlineCannotBeExtendedByRepeatedDisconnect) {
    establish();
    ASSERT_EQ(client.media_disconnected(100), Result::Applied);
    const auto disconnect = publisher.messages.back();
    EXPECT_EQ(client.media_disconnected(45099), Result::Duplicate);
    client.tick(45100); // 固定45秒窗口到期；与延长后的请求预算（50秒）无关。
    EXPECT_EQ(client.state(), VoiceState::Failed);
    EXPECT_EQ(receive(ack(disconnect, R"({"mediaRevision":1,"state":"disconnected"})"), 45101),
              Result::Stale);
    EXPECT_EQ(media.recovery_revision, 0U);
    EXPECT_FALSE(media.configured);
}
TEST_P(VoiceClientRecovery, ClockRollbackClosesRatherThanRevivesMedia) {
    establish();
    ASSERT_EQ(client.media_disconnected(100), Result::Applied);
    client.tick(200);
    EXPECT_EQ(client.media_ready(199), Result::Stale);
    EXPECT_EQ(client.state(), VoiceState::Failed);
    EXPECT_FALSE(client.capturing());
}
TEST_P(VoiceClientRecovery, DriverRejectsNewPeerWithoutReopeningCapture) {
    establish();
    ASSERT_EQ(client.start_input(8), Result::Applied);
    const auto start = publisher.messages.back();
    ASSERT_EQ(receive(ack(start, start.payload), 9), Result::Applied);
    ASSERT_EQ(client.media_disconnected(100), Result::Applied);
    media.recovery_ok = false;
    EXPECT_EQ(
        receive(ack(publisher.messages.back(), R"({"mediaRevision":1,"state":"disconnected"})"),
                101),
        Result::DriverFailed);
    EXPECT_EQ(client.state(), VoiceState::Failed);
    EXPECT_EQ(media.captures, 1);
    EXPECT_FALSE(client.capturing());
}
TEST_P(VoiceClientRecovery, CancelAndHangupRemainAvailableWithoutReplayingOldAudio) {
    establish();
    ASSERT_EQ(reply("respAudioStart", "old", 1, 2, 8), Result::Applied);
    ASSERT_EQ(client.media_disconnected(100), Result::Applied);
    const auto disconnect = publisher.messages.back();
    ASSERT_EQ(client.cancel_response(101), Result::Applied);
    const auto cancel = publisher.messages.back();
    EXPECT_EQ(cancel.method, "respCancel");
    ASSERT_EQ(receive(ack(cancel, R"({"respId":"old","state":"cancelled"})"), 102),
              Result::Applied);
    EXPECT_EQ(client.cancel_response(103), Result::Duplicate);
    ASSERT_EQ(client.end(104), Result::Applied);
    EXPECT_EQ(client.state(), VoiceState::Ending);
    EXPECT_EQ(receive(ack(disconnect, R"({"mediaRevision":1,"state":"disconnected"})"), 105),
              Result::Stale);
    EXPECT_EQ(media.recovery_revision, 0U);
    EXPECT_FALSE(client.playing());
    EXPECT_EQ(media.playback_stops, 1);
}
// 恢复期offer单发加固：下一代Peer建立后首轮候选残缺或应答迟到时重建一次并推进代次。
TEST_P(VoiceClientRecovery, RecoveryOfferRetriesOnceWithNextGenerationWhenPeerStaysDisconnected) {
    establish();
    ASSERT_EQ(client.media_disconnected(100), Result::Applied);
    ASSERT_EQ(receive(ack(publisher.messages.back(),
                          R"({"mediaRevision":1,"state":"disconnected"})"),
                      101),
              Result::Applied);
    ASSERT_EQ(media.recoveries, 1);
    ASSERT_EQ(media.recovery_revision, 2U);
    ASSERT_EQ(client.media_offer_ready("v=0\r\no=first\r\n", 1000), Result::Applied);
    const auto first = publisher.messages.back();
    EXPECT_NE(first.payload.find("\"mediaRevision\":2"), std::string::npos);
    // 未到补发起点只等待；本代offer不得重复发送。
    client.tick(6999);
    EXPECT_EQ(media.recoveries, 1);
    EXPECT_EQ(client.state(), VoiceState::Recovering);
    EXPECT_EQ(client.media_offer_ready("v=0\r\no=other\r\n", 6999), Result::Duplicate);
    // 到点仍未连通时重建下一代Peer并推进代次，新offer使用新token。
    client.tick(7000);
    EXPECT_EQ(media.recoveries, 2);
    EXPECT_EQ(media.recovery_revision, 3U);
    ASSERT_EQ(client.media_offer_ready("v=0\r\no=retry\r\n", 7001), Result::Applied);
    const auto second = publisher.messages.back();
    EXPECT_NE(second.token, first.token);
    EXPECT_NE(second.payload.find("\"mediaRevision\":3"), std::string::npos);
    // 每个恢复期上限一次：时间继续推进也不再重建。
    client.tick(20000);
    EXPECT_EQ(media.recoveries, 2);
    EXPECT_EQ(client.state(), VoiceState::Recovering);
}
// 重发后旧代次应答迟到：原token已退休，不得交给Peer或使新代次提前连通。
TEST_P(VoiceClientRecovery, LateAnswerForRetiredOfferCannotApplyOrResumeMedia) {
    establish();
    ASSERT_EQ(client.media_disconnected(100), Result::Applied);
    ASSERT_EQ(receive(ack(publisher.messages.back(),
                          R"({"mediaRevision":1,"state":"disconnected"})"),
                      101),
              Result::Applied);
    ASSERT_EQ(client.media_offer_ready("v=0\r\no=stale\r\n", 1000), Result::Applied);
    const auto stale_offer = publisher.messages.back();
    client.tick(7000);
    ASSERT_EQ(media.recoveries, 2);
    EXPECT_EQ(answer(stale_offer, 2, 7001), Result::Stale);
    EXPECT_EQ(media.answer, "");
    EXPECT_EQ(client.media_ready(7002), Result::Stale);
    ASSERT_EQ(client.media_offer_ready("v=0\r\no=fresh\r\n", 7003), Result::Applied);
    const auto fresh = publisher.messages.back();
    EXPECT_EQ(answer(fresh, 2, 7004), Result::Invalid);
    ASSERT_EQ(answer(fresh, 3, 7005), Result::Applied);
    EXPECT_EQ(media.answer, "v=0\r\no=answer\r\n");
    ASSERT_EQ(connected(3, 7006), Result::Applied);
    EXPECT_EQ(client.state(), VoiceState::Established);
}
// 旧代次offer的请求已退休：QoS重发不再补发过时代次，恢复首因仍是窗口截止而非请求超时。
TEST_P(VoiceClientRecovery, RetiredOfferRequestNeitherRepublishesNorTimesOutSession) {
    client.enable_failure_diagnostics();
    establish();
    ASSERT_EQ(client.media_disconnected(100), Result::Applied);
    ASSERT_EQ(receive(ack(publisher.messages.back(),
                          R"({"mediaRevision":1,"state":"disconnected"})"),
                      101),
              Result::Applied);
    ASSERT_EQ(client.media_offer_ready("v=0\r\no=first\r\n", 1000), Result::Applied);
    const auto stale = publisher.messages.back();
    const auto publications = publisher.messages.size();
    client.tick(7000);
    ASSERT_EQ(media.recoveries, 2);
    // 原在途请求的10秒/20秒QoS重发点不得带过时代次再次上行。
    for (const std::uint64_t at : {11000U, 21000U}) {
        client.tick(at);
        EXPECT_EQ(publisher.messages.size(), publications);
    }
    EXPECT_EQ(std::count_if(publisher.messages.begin(), publisher.messages.end(),
                            [&stale](const Envelope& item) { return item.token == stale.token; }),
              1);
    // 旧请求延长预算（50秒）不早于45秒窗口：到期首因是媒体恢复截止，不是请求超时。
    client.tick(45100);
    ASSERT_EQ(client.state(), VoiceState::Failed);
    ASSERT_TRUE(client.failure_diagnostic());
    EXPECT_EQ(client.failure_diagnostic()->cause, VoiceFailureCause::MediaRecoveryDeadline);
}
// 窗口延长后旧15秒边界不再判死，迟到的重协商仍可在同一恢复窗口内完成。
TEST_P(VoiceClientRecovery, RecoveryWindowAllowsLatePeerEstablishmentBeyondOldFifteenSeconds) {
    establish();
    ASSERT_EQ(client.media_disconnected(100), Result::Applied);
    const auto disconnect = publisher.messages.back();
    client.tick(15100);
    ASSERT_EQ(client.state(), VoiceState::Recovering);
    ASSERT_EQ(receive(ack(disconnect, R"({"mediaRevision":1,"state":"disconnected"})"), 15101),
              Result::Applied);
    ASSERT_EQ(client.media_offer_ready("v=0\r\no=late\r\n", 15200), Result::Applied);
    // 15秒后才发出的首轮offer在残余拥塞下同样可能残缺，仍允许补发一次。
    client.tick(21200);
    ASSERT_EQ(media.recoveries, 2);
    ASSERT_EQ(client.media_offer_ready("v=0\r\no=late2\r\n", 21300), Result::Applied);
    const auto retry = publisher.messages.back();
    ASSERT_EQ(answer(retry, 3, 21400), Result::Applied);
    ASSERT_EQ(connected(3, 21500), Result::Applied);
    EXPECT_EQ(client.state(), VoiceState::Established);
    EXPECT_EQ(client.session_id(), "session");
}
// 驱动已实际验证连通时不得重建：ready()事实优先于固定补发起点。
TEST_P(VoiceClientRecovery, DriverVerifiedConnectivitySuppressesOfferRetry) {
    establish();
    ASSERT_EQ(client.media_disconnected(100), Result::Applied);
    ASSERT_EQ(receive(ack(publisher.messages.back(),
                          R"({"mediaRevision":1,"state":"disconnected"})"),
                      101),
              Result::Applied);
    ASSERT_EQ(client.media_offer_ready("v=0\r\no=slow\r\n", 1000), Result::Applied);
    const auto offer = publisher.messages.back();
    media.connected = true;  // 驱动已连通，宿主尚未上报mediaState。
    client.tick(7000);
    EXPECT_EQ(media.recoveries, 1);
    EXPECT_EQ(client.state(), VoiceState::Recovering);
    ASSERT_EQ(answer(offer, 2, 7001), Result::Applied);
    ASSERT_EQ(connected(2, 7002), Result::Applied);
    EXPECT_EQ(client.state(), VoiceState::Established);
    EXPECT_EQ(media.recoveries, 1);
}
// 应答已交Peer但ICE始终未连通仍属单代残缺：同一次恢复内重建下一代并补发。
TEST_P(VoiceClientRecovery, AnswerAppliedWithoutConnectivityStillRetriesNextGeneration) {
    establish();
    ASSERT_EQ(client.media_disconnected(100), Result::Applied);
    ASSERT_EQ(receive(ack(publisher.messages.back(),
                          R"({"mediaRevision":1,"state":"disconnected"})"),
                      101),
              Result::Applied);
    ASSERT_EQ(client.media_offer_ready("v=0\r\no=first\r\n", 1000), Result::Applied);
    const auto first = publisher.messages.back();
    ASSERT_EQ(answer(first, 2, 2000), Result::Applied);
    EXPECT_EQ(media.answer, "v=0\r\no=answer\r\n");
    client.tick(7000);
    EXPECT_EQ(media.recoveries, 2);
    EXPECT_EQ(media.recovery_revision, 3U);
    EXPECT_EQ(media.answer, "");  // 新Peer重建后旧answer不得残留。
    ASSERT_EQ(client.media_offer_ready("v=0\r\no=second\r\n", 7001), Result::Applied);
    const auto second = publisher.messages.back();
    EXPECT_NE(second.payload.find("\"mediaRevision\":3"), std::string::npos);
    EXPECT_EQ(answer(second, 2, 7002), Result::Invalid);
    ASSERT_EQ(answer(second, 3, 7003), Result::Applied);
    ASSERT_EQ(connected(3, 7004), Result::Applied);
    EXPECT_EQ(client.state(), VoiceState::Established);
}
INSTANTIATE_TEST_SUITE_P(HalfAndFullDuplex, VoiceClientRecovery, testing::Bool());
}  // namespace
}  // namespace ur::device
