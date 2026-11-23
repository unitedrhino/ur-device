// 浏览器本地消息边界：不透传云端命令，拒绝重复键、溢出与非法PCM。
#include "browser_wire.hpp"
#include <gtest/gtest.h>
#include <limits>

namespace ur::device {
namespace {
// 预信令观察使用同一严格编码器，uint32饱和/未知统计不能洗成完整包。
TEST(BrowserWire, PresignalDiagnosticStrictShapeAndIncompleteBoundary) {
    EXPECT_EQ(browser_presignal_diagnostic({1, 169, 153, 16, true}),
              "{\"kind\":\"udpPresignalDiagnostic\",\"observation\":1,\"received\":169,\"evicted\":"
              "153,\"retained\":16,\"complete\":true}");
    EXPECT_FALSE(browser_presignal_diagnostic({1, 0, 0, 1, false}).empty());
    EXPECT_FALSE(browser_presignal_diagnostic({1, UINT32_MAX, UINT32_MAX, 16, false}).empty());
    EXPECT_TRUE(browser_presignal_diagnostic({0, 169, 153, 16, true}).empty());
    EXPECT_TRUE(browser_presignal_diagnostic({1, 169, 153, 17, false}).empty());
    EXPECT_TRUE(browser_presignal_diagnostic({1, 169, 170, 0, false}).empty());
    EXPECT_TRUE(browser_presignal_diagnostic({1, 168, 153, 16, true}).empty());
}
// ResponsePublisher保存正式编码请求供原ACK关联；不直写VoiceClient内部状态。
struct ResponsePublisher final : PublishPort {
    std::vector<Envelope> requests;  // 此受控夹具少量请求，不提供网络或模型事实。
    bool publish(std::string_view, std::string_view body) override {
        Envelope request;
        if (decode_envelope(body, request) != DecodeResult::Ok) return false;
        requests.push_back(std::move(request));
        return true;
    }
};
// ResponseMedia仅为状态机提供受控驱动完成；不将其冒充浏览器ICE、实际发声或物理焦点。
struct ResponseMedia final : VoiceMediaPort {
    std::uint32_t next_input_sequence() const override { return 0; } // WebRTC不制造UDP上行包序号。
    bool configure(const VoiceMedia&) override { return true; }
    void close() override {}
    bool prepare_reply(const std::array<std::uint8_t, 8>&, std::uint32_t) override { return true; }
    bool finish_reply(std::uint32_t) override { return true; }
    bool apply_answer(std::string_view) override { return true; }
    bool begin_recovery(std::uint64_t) override { return true; }
    bool start_capture(std::string_view) override { return true; }
    void stop_capture() override {}
    bool start_playback(std::string_view, std::uint64_t) override { return true; }
    void stop_playback(std::uint64_t) override {}
};
// ResponseControlTest经原能力/创建/SDP/连通ACK取得真实Core控制目标；不借DOM或媒体播放ID置状态。
class ResponseControlTest : public testing::Test {
protected:
    ResponsePublisher publisher;  // 生命周期长于Core借用端口。
    ResponseMedia media;          // 仅替代HAL，不替代协议状态转移。
    VoiceClient voice{publisher, media, "$thing/up/signaling/p/d", "$thing/down/signaling/p/d",
        {{true, true, true}, {48000, 1, 20}, 1, false, VoiceTransport::WebRTC}, "response-control"};
    BrowserResponseControl control;  // main使用的同一生产规则，不在测试中重实现。
    // receive按原请求token、会话、事件序号编码后交正式客户端解码。
    Result receive(std::string method, std::string payload, const Envelope* request = nullptr,
                   std::uint64_t sequence = 0, std::uint64_t now = 1) {
        Envelope event;
        event.token = request ? request->token : "event-" + std::to_string(sequence);
        event.session = "session";
        event.method = std::move(method);
        event.payload = std::move(payload);
        event.request = false;
        if (request) event.code = 200;
        else event.event_sequence = sequence;
        std::string body;
        EXPECT_EQ(encode_envelope(event, body), DecodeResult::Ok);
        return voice.receive({"$thing/down/signaling/p/d", body}, now);
    }
    // acknowledge只回当前实际请求，不能猜token或用事件替代ACK。
    Result acknowledge(std::string result, std::uint64_t now = 1) {
        const auto request = publisher.requests.back();
        Envelope reply;
        reply.token = request.token;
        reply.session = request.session;
        reply.method = "requestAck";
        reply.payload = "{\"requestMethod\":\"" + request.method + "\",\"result\":" + result + "}";
        reply.request = false;
        reply.code = 200;
        std::string body;
        EXPECT_EQ(encode_envelope(reply, body), DecodeResult::Ok);
        return voice.receive({"$thing/down/signaling/p/d", body}, now);
    }
    // establish保留原WebRTC完整屏障；媒体HAL成功只使客户端可上报，不直接赋Established。
    void establish() {
        ASSERT_EQ(voice.connect(0), Result::Applied);
        ASSERT_EQ(acknowledge(R"({"acceptedRevision":1})"), Result::Applied);
        const auto create = publisher.requests.back();
        ASSERT_EQ(receive("sessionCreated",
            R"({"state":"negotiating","selected":{"transport":"webrtc","interactionMode":"fullDuplex","audioParams":{"format":"opus","sampleRate":48000,"channels":1,"frameDuration":20}},"tracks":[{"trackId":"mic-1","kind":"audio","sourceId":"mic","direction":"sendonly","codec":"opus","state":"requested"},{"trackId":"speaker-1","kind":"audio","sourceId":"speaker","direction":"recvonly","codec":"opus","state":"requested"}]})",
            &create, 0, 2), Result::Applied);
        ASSERT_EQ(voice.media_offer_ready("v=0\r\no=offer\r\n", 3), Result::Applied);
        const auto offer = publisher.requests.back();
        ASSERT_EQ(receive("mediaAnswer", R"({"mediaRevision":1,"webrtc":{"iceMode":"complete","sdp":"v=0\r\no=answer\r\n"}})",
            &offer, 0, 4), Result::Applied);
        ASSERT_EQ(voice.media_ready(5), Result::Applied);
        ASSERT_EQ(acknowledge(R"({"mediaRevision":1,"state":"connected"})", 6), Result::Applied);
        ASSERT_EQ(receive("sessionEstablished", R"({"state":"established","mediaRevision":1})", nullptr, 1, 7),
            Result::Applied);
    }
    // reply仅使用WebRTC回复事件，不含UDP包边界，不将播放HAL ID当控制授权。
    void reply(std::string id, std::uint64_t sequence = 2) {
        ASSERT_EQ(receive("respAudioStart", "{\"respId\":\"" + id +
            "\",\"trackId\":\"speaker-1\",\"mediaRevision\":1}", nullptr, sequence, 8 + sequence),
            Result::Applied);
    }
    // done为已核同会话的内容终态；错id/会话测试逐项修改副本，生产规则仍自行核原Core。
    ContentEvent done(std::string id) {
        ContentEvent event;
        event.envelope.method = "respDone";
        event.envelope.session = "session";
        event.response_id = std::move(id);
        event.status = "completed";
        return event;
    }
};
}  // namespace

TEST(BrowserResponseSerializer, EscapesAndValidatesUtf8WithoutMediaPermission) {
    const std::string active = R"({"kind":"state","established":true,"interactionMode":"fullDuplex","text":"已建立"})";
    const std::string id = "reply-\"\\😀";
    const auto encoded = browser_response_control_state(active, id);
    EXPECT_TRUE(browser_response_control_state(id, active).empty());
    EXPECT_FALSE(encoded.empty());
    EXPECT_NE(encoded.find(R"(reply-\"\\😀)"), std::string::npos);
    EXPECT_FALSE(browser_response_control_state(active, std::string(128, 'x')).empty());
    EXPECT_TRUE(browser_response_control_state(active, std::string(129, 'x')).empty());
    std::string multibyte;  // 42个三字节字符加ab恰为128UTF8字节，不按JavaScript字符数计界限。
    for (unsigned index = 0; index < 42; ++index) multibyte += "字";
    EXPECT_FALSE(browser_response_control_state(active, multibyte + "ab").empty());
    EXPECT_TRUE(browser_response_control_state(active, multibyte + "字").empty());
    for (const auto& invalid : {std::string("bad\nreply"), std::string("bad\x7f", 4),
                               std::string("\xc0\xaf", 2), std::string("\xed\xa0\x80", 3),
                               std::string("a\0b", 3)})
        EXPECT_TRUE(browser_response_control_state(active, invalid).empty());
    EXPECT_FALSE(browser_response_control_state(active, "").empty());
}
TEST(BrowserResponseSerializer, TerminalAndInvalidStateCannotAdvertiseTarget) {
    const std::string terminal = R"({"kind":"state","established":false,"text":"终态"})";
    EXPECT_TRUE(browser_response_control_state(terminal, "old").empty());
    EXPECT_FALSE(browser_response_control_state(terminal, "").empty());
    EXPECT_FALSE(browser_response_control_state(
        R"({"kind":"state","established":false,"recovering":true,"text":"恢复"})", "old").empty());
    for (const auto* invalid : {R"({"kind":"state","established":true,"text":"缺模式"})",
          R"({"kind":"state","established":false,"recovering":"true","text":"类型错误"})",
          R"({"kind":"state","established":false,"text":"额外字段","id":"forged"})",
          R"({"kind":"state","kind":"state","established":false,"text":"重复"})"})
        EXPECT_TRUE(browser_response_control_state(invalid, "").empty());
}
TEST(BrowserResponseSerializer, TextOnlyStateNeverAdvertisesAudioModeOrMediaRecovery) {
    const std::string active =
        R"({"kind":"state","established":true,"transport":"none","text":"文字"})";
    EXPECT_FALSE(browser_response_control_state(active, "text-reply").empty());
    EXPECT_FALSE(browser_response_control_state(active, "").empty());
    for (
        const auto* invalid :
        {R"({"kind":"state","established":true,"transport":"udp","text":"媒体"})",
         R"({"kind":"state","established":true,"transport":null,"text":"空值"})",
         R"({"kind":"state","established":false,"transport":"none","text":"未建立"})",
         R"({"kind":"state","established":true,"transport":"none","interactionMode":"fullDuplex","text":"扩权"})",
         R"({"kind":"state","established":true,"transport":"none","recovering":false,"text":"扩权"})"})
        EXPECT_TRUE(browser_response_control_state(invalid, "").empty());
}
TEST(BrowserWire, TextReadinessIsStrictLocalMessageWithoutMediaClaims) {
    BrowserRequest request;
    EXPECT_TRUE(decode_browser_request(R"({"kind":"textReady"})", request));
    EXPECT_EQ(request.kind, "textReady");
    for (const auto* invalid :
         {R"({"kind":"textReady","aec":true})", R"({"kind":"textReady","sessionId":"forged"})",
          R"({"kind":"textReady","kind":"ready"})"}) {
        EXPECT_FALSE(decode_browser_request(invalid, request));
        EXPECT_TRUE(request.kind.empty());
    }
}
TEST_F(ResponseControlTest, CurrentCoreTargetSurvivesPeerRetirementButNeverGrantsOutput) {
    EXPECT_TRUE(control.target(voice, RuntimeState::Running).empty());
    establish();
    reply("original");
    EXPECT_TRUE(control.cancel_allowed("original", voice, RuntimeState::Running));
    EXPECT_FALSE(control.cancel_allowed("old-playing", voice, RuntimeState::Running));
    EXPECT_FALSE(control.cancel_allowed("", voice, RuntimeState::Running));
    ASSERT_EQ(voice.media_disconnected(20), Result::Applied);
    ASSERT_EQ(voice.state(), VoiceState::Recovering);
    EXPECT_EQ(control.target(voice, RuntimeState::Running), "original");
    EXPECT_TRUE(control.cancel_allowed("original", voice, RuntimeState::Running));
    EXPECT_FALSE(voice.playing());
    EXPECT_EQ(voice.cancel_response(21), Result::Applied);
    EXPECT_EQ(publisher.requests.back().method, "respCancel");
}
TEST_F(ResponseControlTest, TerminalLatchDoesNotReviveRecentIdOrSuppressNewReply) {
    establish();
    reply("first");
    control.observe_terminal(done("first"), voice);
    EXPECT_EQ(voice.response_id(), "first");
    EXPECT_TRUE(control.target(voice, RuntimeState::Running).empty());
    EXPECT_FALSE(control.cancel_allowed("first", voice, RuntimeState::Running));
    EXPECT_EQ(voice.cancel_response(12), Result::Applied);
    reply("second", 3);
    EXPECT_EQ(control.target(voice, RuntimeState::Running), "second");
    control.observe_terminal(done("first"), voice);
    EXPECT_TRUE(control.cancel_allowed("second", voice, RuntimeState::Running));
    EXPECT_FALSE(control.cancel_allowed("first", voice, RuntimeState::Running));
}
TEST_F(ResponseControlTest, ForeignOrNonterminalContentCannotRetireCurrentTarget) {
    establish();
    reply("current");
    auto event = done("current");
    event.envelope.session = "other";
    control.observe_terminal(event, voice);
    event = done("current"); event.status = "started";
    control.observe_terminal(event, voice);
    event = done("current"); event.envelope.method = "respTextDone";
    control.observe_terminal(event, voice);
    control.observe_terminal(done("other"), voice);
    EXPECT_TRUE(control.cancel_allowed("current", voice, RuntimeState::Running));
    for (const auto* terminal : {"completed", "cancelled", "failed"}) {
        BrowserResponseControl separate;
        event = done("current"); event.status = terminal;
        separate.observe_terminal(event, voice);
        EXPECT_TRUE(separate.target(voice, RuntimeState::Running).empty());
    }
}
TEST_F(ResponseControlTest, NonRunningEndingAndFailedCoreNeverAuthorizeCancel) {
    establish();
    reply("current");
    const std::array<int, 6> states{static_cast<int>(RuntimeState::Idle),
        static_cast<int>(RuntimeState::Connecting), static_cast<int>(RuntimeState::Subscribing),
        static_cast<int>(RuntimeState::Draining), static_cast<int>(RuntimeState::Failed), 999};
    // 数组输入经运行期枚举转换，保留未知值999边界，不触发常量越界转换掩盖业务断言。
    for (const auto state : states)
        EXPECT_FALSE(control.cancel_allowed("current", voice, static_cast<RuntimeState>(state)));
    ASSERT_EQ(voice.end(20), Result::Applied);
    EXPECT_TRUE(control.target(voice, RuntimeState::Running).empty());
    voice.tick(30021);
    EXPECT_EQ(voice.state(), VoiceState::Failed);
    EXPECT_FALSE(control.cancel_allowed("current", voice, RuntimeState::Running));
}
// 首因投影只保留实际层级；不允许跨runtime挪用UDP或把普通故障包装成PCM背压。
TEST(BrowserWire, RuntimeFailureProjectionRejectsInvalidLayerCombinations) {
    EXPECT_EQ(
        browser_runtime_failure_diagnostic(RuntimeFailure::MediaPoll, UdpFailure::PcmWrite,
                                           PcmWriteFailure::BufferFull),
        R"({"kind":"runtimeFailureDiagnostic","runtime":"mediaPoll","udp":"pcmWrite","pcm":"bufferFull","observation":1})");
    EXPECT_EQ(
        browser_runtime_failure_diagnostic(RuntimeFailure::Unknown),
        R"({"kind":"runtimeFailureDiagnostic","runtime":"unknown","udp":"none","pcm":"none","observation":1})");
    EXPECT_TRUE(browser_runtime_failure_diagnostic(RuntimeFailure::Unknown, UdpFailure::PcmWrite,
                                                   PcmWriteFailure::BufferFull)
                    .empty());
    EXPECT_TRUE(browser_runtime_failure_diagnostic(RuntimeFailure::MediaPoll, UdpFailure::PcmWrite)
                    .empty());
    EXPECT_TRUE(browser_runtime_failure_diagnostic(RuntimeFailure::MediaPoll, {},
                                                   PcmWriteFailure::BufferFull)
                    .empty());
    EXPECT_TRUE(browser_runtime_failure_diagnostic(static_cast<RuntimeFailure>(999)).empty());
    EXPECT_TRUE(
        browser_runtime_failure_diagnostic(RuntimeFailure::MediaPoll, static_cast<UdpFailure>(999))
            .empty());
    EXPECT_TRUE(browser_runtime_failure_diagnostic(RuntimeFailure::MediaPoll, UdpFailure::PcmWrite,
                                                   static_cast<PcmWriteFailure>(999))
                    .empty());
}
// 客户端首因与旧runtime五字段分开，非法来源/方法/枚举不进入本机日志。
TEST(BrowserWire, ClientFailureDiagnosticHasIndependentStrictFixedFields) {
    const ClientFailureDiagnostic original{ClientFailureOrigin::ClientFailed,
                                           VoiceFailureCause::RequestsTimeout,
                                           VoiceFailureMethod::AudioStop};
    EXPECT_EQ(
        browser_client_failure_diagnostic(original),
        R"({"kind":"clientFailureDiagnostic","origin":"clientFailed","cause":"requestsTimeout","method":"audioStop","observation":1})");
    auto value = original;
    value.method = VoiceFailureMethod::None;
    EXPECT_TRUE(browser_client_failure_diagnostic(value).empty());
    value = original;
    value.origin = ClientFailureOrigin::EndpointPrepareFailed;
    EXPECT_TRUE(browser_client_failure_diagnostic(value).empty());
    value.cause = VoiceFailureCause::EndpointPrepareDeadline;
    value.method = VoiceFailureMethod::None;
    EXPECT_FALSE(browser_client_failure_diagnostic(value).empty());
    value.origin = ClientFailureOrigin::ClientFailed;
    EXPECT_TRUE(browser_client_failure_diagnostic(value).empty());
    for (const auto cause : {VoiceFailureCause::None, static_cast<VoiceFailureCause>(255)}) {
        value = original;
        value.cause = cause;
        EXPECT_TRUE(browser_client_failure_diagnostic(value).empty());
    }
    value = original;
    value.method = static_cast<VoiceFailureMethod>(255);
    EXPECT_TRUE(browser_client_failure_diagnostic(value).empty());
    value = original;
    value.origin = static_cast<ClientFailureOrigin>(255);
    EXPECT_TRUE(browser_client_failure_diagnostic(value).empty());
    value = {ClientFailureOrigin::ClientFailed, VoiceFailureCause::Unknown,
             VoiceFailureMethod::None};
    EXPECT_FALSE(browser_client_failure_diagnostic(value).empty());
    value.method = VoiceFailureMethod::AudioStart;
    EXPECT_TRUE(browser_client_failure_diagnostic(value).empty());
}
// 原焦点首因独立五字段；非法枚举为空，所有字段均来自编译期固定映射。
TEST(BrowserWire, ScopeDiagnosticsRejectUnknownAndPreserveFixedShape) {
    EXPECT_EQ(
        browser_scope_failure_diagnostic(ScopeFailureCode::EngineDeadline),
        R"({"kind":"scopeFailureDiagnostic","origin":"candidateGate","layer":"focusEngine","predicate":"leaseExpired","observation":1})");
    EXPECT_TRUE(browser_scope_failure_diagnostic(ScopeFailureCode::None).empty());
    EXPECT_TRUE(browser_scope_failure_diagnostic(static_cast<ScopeFailureCode>(255)).empty());
    for (unsigned index = 1; index <= static_cast<unsigned>(ScopeFailureCode::DriverOther);
         ++index) {
        const auto value = browser_scope_failure_diagnostic(static_cast<ScopeFailureCode>(index));
        EXPECT_FALSE(value.empty());
        EXPECT_EQ(value.find("\"sessionId\""), std::string::npos);
        EXPECT_EQ(value.find("\"msgToken\""), std::string::npos);
    }
}
// native关闭旁证只有固定事实，拒绝非法枚举和不自洽计数，旧MSG JSON保持不变。
TEST(BrowserWire, LifecycleDiagnosticsOnlyFixedOriginalFacts) {
    VoiceLifecycleDiagnostic facts;
    EXPECT_EQ(
        browser_lifecycle_diagnostic(facts),
        R"({"kind":"lifecycleDiagnostic","localEndApplied":false,"acceptedEnd":"none","closeCause":"none","observation":0})");
    facts.local_end_applied = true;
    facts.accepted_end = AcceptedVoiceEnd::SessionEndAck;
    facts.close_cause = VoiceCloseCause::DrainSettled;
    facts.observation = 3;
    EXPECT_EQ(
        browser_lifecycle_diagnostic(facts),
        R"({"kind":"lifecycleDiagnostic","localEndApplied":true,"acceptedEnd":"session_end_ack","closeCause":"drain_settled","observation":3})");
    facts.observation = 4;
    EXPECT_TRUE(browser_lifecycle_diagnostic(facts).empty());
}
// 固定诊断JSON没有信封/工具参数/标识槽，未知方法及非法枚举必须拒绝而不反射。
TEST(BrowserWire, MessageDiagnosticsAreBoundedFixedProjection) {
    MessageDiagnostics facts;
    EXPECT_EQ(
        browser_message_diagnostics(facts),
        R"({"kind":"messageDiagnostic","received":0,"taken":0,"hostFiltered":0,"hostSent":0,"hostSendFailed":0,"overflow":false,"records":[]})");
    facts.received = 1;
    facts.size = 1;
    facts.records[0] = {
        "toolCallExecute", MessageDiagnosticOutcome::WatermarkDiscarded, 12, 13, true, true};
    const auto json = browser_message_diagnostics(facts);
    EXPECT_EQ(
        json,
        R"({"kind":"messageDiagnostic","received":1,"taken":0,"hostFiltered":0,"hostSent":0,"hostSendFailed":0,"overflow":false,"records":[{"method":"toolCallExecute","outcome":"watermark_discarded","sequence":12,"watermark":13,"parsed":true,"sessionMatches":true,"taken":false,"hostObserved":false,"projected":false,"sent":false}]})");
    facts.records[0].method = "private-identifier";
    EXPECT_TRUE(browser_message_diagnostics(facts).empty());
    facts.records[0].method = "unknown";
    facts.records[0].outcome = MessageDiagnosticOutcome::ContentRejected;
    facts.size = 33;
    EXPECT_TRUE(browser_message_diagnostics(facts).empty());
    facts.size = 1;
    facts.received = 9007199254740992ULL;
    EXPECT_TRUE(browser_message_diagnostics(facts).empty());
}
// BrowserWireEnumCase 将运行期整数输入与独立固定输出配对；空输出表示未知值必须拒绝。
struct BrowserWireEnumCase {
    int value;               // 原字段边界整数，包含全部合法值及邻接/原远端未知值。
    const char* wire_value;  // 合法枚举的预期固定文本；未知值不提供替代文本。
};
// BrowserWireLifecycleEnumTest 通过同一真实序列化入口检查合法和未知终态枚举。
class BrowserWireLifecycleEnumTest : public testing::TestWithParam<BrowserWireEnumCase> {};
TEST_P(BrowserWireLifecycleEnumTest, RuntimeIntegerBoundary) {
    VoiceLifecycleDiagnostic facts;  // 其余字段保持自洽，拒绝必须来自未知枚举。
    facts.local_end_applied = true;
    facts.accepted_end = static_cast<AcceptedVoiceEnd>(GetParam().value);
    facts.close_cause = VoiceCloseCause::DrainSettled;
    facts.observation = GetParam().value == 0 ? 2U : 3U;
    const auto projection = browser_lifecycle_diagnostic(facts);  // 每个参数只调用实际入口一次。
    if (!GetParam().wire_value) {
        EXPECT_TRUE(projection.empty());
        return;
    }
    const std::string expected =
        std::string(R"({"kind":"lifecycleDiagnostic","localEndApplied":true,"acceptedEnd":")") +
        GetParam().wire_value + R"(","closeCause":"drain_settled","observation":)" +
        std::to_string(facts.observation) + "}";  // 预期映射不调用生产枚举映射函数。
    EXPECT_EQ(projection, expected);
}
INSTANTIATE_TEST_SUITE_P(BrowserWireBoundaries, BrowserWireLifecycleEnumTest,
                         testing::Values(BrowserWireEnumCase{0, "none"},
                                         BrowserWireEnumCase{1, "session_end_ack"},
                                         BrowserWireEnumCase{2, "session_ended_event"},
                                         BrowserWireEnumCase{3, nullptr},
                                         BrowserWireEnumCase{99, nullptr}));
// BrowserWireMessageEnumTest 保留全部合法消费分支及邻接/原999未知值的运行期拒绝覆盖。
class BrowserWireMessageEnumTest : public testing::TestWithParam<BrowserWireEnumCase> {};
TEST_P(BrowserWireMessageEnumTest, RuntimeIntegerBoundary) {
    MessageDiagnostics facts;  // 固定方法和安全计数不干扰原枚举边界判断。
    facts.received = 1;
    facts.size = 1;
    facts.records[0].method = "unknown";
    facts.records[0].outcome = static_cast<MessageDiagnosticOutcome>(GetParam().value);
    const auto projection = browser_message_diagnostics(facts);  // 每个参数只走原入口一次。
    if (!GetParam().wire_value) {
        EXPECT_TRUE(projection.empty());
        return;
    }
    const std::string expected =
        std::string(
            R"({"kind":"messageDiagnostic","received":1,"taken":0,"hostFiltered":0,"hostSent":0,"hostSendFailed":0,"overflow":false,"records":[{"method":"unknown","outcome":")") +
        GetParam().wire_value +
        R"(","sequence":0,"watermark":0,"parsed":false,"sessionMatches":false,"taken":false,"hostObserved":false,"projected":false,"sent":false}]})";
    EXPECT_EQ(projection, expected);
}
INSTANTIATE_TEST_SUITE_P(
    BrowserWireBoundaries, BrowserWireMessageEnumTest,
    testing::Values(
        BrowserWireEnumCase{0, "no_session"}, BrowserWireEnumCase{1, "draining"},
        BrowserWireEnumCase{2, "envelope_rejected"}, BrowserWireEnumCase{3, "unsupported"},
        BrowserWireEnumCase{4, "content_rejected"}, BrowserWireEnumCase{5, "session_mismatch"},
        BrowserWireEnumCase{6, "watermark_discarded"}, BrowserWireEnumCase{7, "tool_unavailable"},
        BrowserWireEnumCase{8, "execute_accepted"}, BrowserWireEnumCase{9, "execute_replay"},
        BrowserWireEnumCase{10, "execute_denied"}, BrowserWireEnumCase{11, "execute_invalid"},
        BrowserWireEnumCase{12, "ack_consumed"}, BrowserWireEnumCase{13, "ack_rejected"},
        BrowserWireEnumCase{14, "scope_rejected"}, BrowserWireEnumCase{15, "queue_full"},
        BrowserWireEnumCase{16, "content_queued"}, BrowserWireEnumCase{17, nullptr},
        BrowserWireEnumCase{999, nullptr}));
TEST(BrowserWire, ReadyAndControlsAreStrict) {
    BrowserRequest request;
    ASSERT_TRUE(decode_browser_request(
        R"({"kind":"ready","sampleRate":16000,"frameMs":20,"aec":true})", request));
    EXPECT_TRUE(request.aec);
    EXPECT_EQ(request.format.sample_rate, 16000);
    for (const auto body : {R"({"kind":"start"})", R"({"kind":"stop"})", R"({"kind":"end"})",
                            R"({"kind":"cancel","id":"r"})"})
        EXPECT_TRUE(decode_browser_request(body, request));
    for (const auto body : {R"({"kind":"start","secret":"no"})", R"({"kind":"arbitraryMethod"})",
                            R"({"kind":"ready","sampleRate":44100,"frameMs":20,"aec":true})",
                            R"({"kind":"ready","sampleRate":16000,"frameMs":20,"aec":"true"})",
                            R"({"kind":"start","kind":"end"})", R"({"kind":"cancel","id":""})"}) {
        EXPECT_FALSE(decode_browser_request(body, request));
        EXPECT_TRUE(request.kind.empty());
    }
}
TEST(BrowserWire, TextInputIsBoundedAndCannotTunnelCloudCommands) {
    BrowserRequest request;
    ASSERT_TRUE(
        decode_browser_request(R"({"kind":"sendText","text":" 你好\n","audio":true})", request));
    EXPECT_EQ(request.text, " 你好\n");
    EXPECT_TRUE(request.audio);
    ASSERT_TRUE(
        decode_browser_request(R"({"kind":"sendText","text":"你好","audio":false})", request));
    EXPECT_EQ(request.text, "你好");
    EXPECT_FALSE(request.audio);
    for (const auto body :
         {R"({"kind":"sendText","text":" ","audio":true})",
          R"({"kind":"sendText","text":"你好","audio":"true"})",
          R"({"kind":"sendText","text":"你好","audio":false,"method":"sessionEnd"})",
          R"({"kind":"sendText","text":"你好","audio":false,"sessionId":"forged"})"}) {
        EXPECT_FALSE(decode_browser_request(body, request));
        EXPECT_TRUE(request.kind.empty());
        EXPECT_TRUE(request.text.empty());
    }
    const std::string too_long =
        R"({"kind":"sendText","text":")" + std::string(16385, 'x') + R"(","audio":false})";
    EXPECT_FALSE(decode_browser_request(too_long, request));
}
TEST(BrowserWire, CaptureAndPlaybackAreBoundedAndEpochLossless) {
    BrowserRequest request;
    ASSERT_TRUE(decode_browser_request(R"({"kind":"captured","id":"i","samples":[-32768,0,32767]})",
                                       request));
    EXPECT_EQ(request.pcm, (std::vector<std::int16_t>{-32768, 0, 32767}));
    ASSERT_TRUE(decode_browser_request(
        R"({"kind":"played","epoch":"18446744073709551615","ticket":4294967295})", request));
    EXPECT_EQ(request.epoch, UINT64_MAX);
    EXPECT_EQ(request.ticket, UINT32_MAX);
    for (const auto body : {R"({"kind":"captured","id":"i","samples":[32768]})",
                            R"({"kind":"captured","id":"i","samples":[0.5]})",
                            R"({"kind":"captured","id":"i","samples":[]})",
                            R"({"kind":"played","epoch":9007199254740993,"ticket":1})",
                            R"({"kind":"played","epoch":"18446744073709551616","ticket":1})",
                            R"({"kind":"played","epoch":"01","ticket":1})",
                            R"({"kind":"played","epoch":"1","ticket":0})"})
        EXPECT_FALSE(decode_browser_request(body, request));
    const auto output = browser_frame({{-32768, 32767}, UINT64_MAX, 1});
    EXPECT_NE(output.find(R"("epoch":"18446744073709551615")"), std::string::npos);
    EXPECT_NE(output.find(R"("samples":[-32768,32767])"), std::string::npos);
    EXPECT_TRUE(browser_frame({{}, 1, 1}).empty());
}
TEST(BrowserWire, WebRTCFactsAreTypedAndCannotTunnelCloudIdentity) {
    BrowserRequest request;
    ASSERT_TRUE(
        decode_browser_request(R"({"kind":"webrtcOffer","peerRevision":"1","sdp":"v=0\r\no=device\r\n"})", request));
    EXPECT_EQ(request.sdp, "v=0\r\no=device\r\n");
    ASSERT_TRUE(decode_browser_request(R"({"kind":"webrtcConnected","peerRevision":"1"})", request));
    ASSERT_TRUE(decode_browser_request(R"({"kind":"webrtcDrained","peerRevision":"1","epoch":"9"})", request));
    EXPECT_EQ(request.epoch, 9U);
    ASSERT_TRUE(decode_browser_request(
        R"({"kind":"webrtcVolumeApplied","id":"tool-1","volume":40})", request));
    EXPECT_EQ(request.id, "tool-1");
    EXPECT_EQ(request.volume, 40U);
    for (const auto body :
         {R"({"kind":"webrtcOffer","peerRevision":"1","sdp":"v=0\n"})",
          R"({"kind":"webrtcOffer","peerRevision":"1","sdp":"v=0\r\no=x\r\n","sessionId":"forged"})",
          R"({"kind":"webrtcConnected","peerRevision":"1","state":"connected"})",
          R"({"kind":"webrtcDrained","peerRevision":"1","epoch":"01"})",
          R"({"kind":"webrtcDrained","peerRevision":"1","epoch":"1","ticket":1})",
          R"({"kind":"webrtcVolumeApplied","id":"","volume":40})",
          R"({"kind":"webrtcVolumeApplied","id":"tool-1","volume":101})",
          R"({"kind":"webrtcVolumeApplied","id":"tool-1","volume":"40"})",
          R"({"kind":"webrtcVolumeApplied","id":"tool-1","volume":40,"secret":"x"})"})
        EXPECT_FALSE(decode_browser_request(body, request));
}
TEST(BrowserWire, CommandsEscapeIdentifiersAndNeverExposeDeviceIdentity) {
    EXPECT_EQ(browser_command({BrowserAudioCommandKind::CaptureStart, "i\"", 0}),
              R"({"kind":"captureStart","id":"i\"","epoch":"0"})");
    EXPECT_EQ(browser_command({BrowserAudioCommandKind::PlaybackStop, "r", 2}),
              R"({"kind":"playbackStop","id":"r","epoch":"2"})");
    EXPECT_TRUE(browser_command({BrowserAudioCommandKind::CaptureStop, "", 0}).empty());
    EXPECT_EQ(browser_text("<script>\n\""), R"({"kind":"text","text":"<script>\n\""})");
    EXPECT_TRUE(browser_text(std::string(16385, 'x')).empty());
    EXPECT_TRUE(browser_text(std::string("x\0y", 3)).empty());
    const AudioTiming timing{10, 20, 3, 2, 1};
    EXPECT_EQ(
        browser_completion("r\"", "sent", timing),
        R"({"kind":"responseCompleted","id":"r\"","audioState":"sent","networkPackets":3,"decodedFrames":2,"nonSilentFrames":1})");
    EXPECT_TRUE(browser_completion("", "sent", timing).empty());
    EXPECT_TRUE(browser_completion(std::string(129, 'x'), "sent", timing).empty());
    EXPECT_TRUE(browser_completion(std::string("r\0x", 3), "sent", timing).empty());
    EXPECT_TRUE(browser_completion("r", "unknown", timing).empty());
    EXPECT_TRUE(browser_completion("r", "sent", {0, 0, 9007199254740992ULL, 0, 0}).empty());
    AudioTiming final = timing;
    final.decoded_frames = 3;
    final.observed_complete = true;
    EXPECT_EQ(
        browser_media_final("r\"", final),
        R"({"kind":"responseMediaFinal","id":"r\"","networkPackets":3,"decodedFrames":3,"nonSilentFrames":1,"missingPackets":0,"observedComplete":true})");
    EXPECT_TRUE(browser_media_final("", timing).empty());
    EXPECT_TRUE(browser_media_final(std::string(129, 'x'), timing).empty());
    EXPECT_TRUE(browser_media_final(std::string("r\0x", 3), timing).empty());
    EXPECT_TRUE(browser_media_final("r", {0, 0, 9007199254740992ULL, 0, 0}).empty());
    EXPECT_TRUE(browser_media_final("r", {0, 0, 1, 2, 0}).empty());
    EXPECT_TRUE(browser_media_final("r", {0, 0, 2, 1, 2}).empty());
    final.missing_packets = 1;
    EXPECT_TRUE(browser_media_final("r", final).empty());
}

// 本机测量只保留同一C++时钟；未观测不是0，也不能从包计数或完成事件补时刻。
TEST(BrowserWire, ReplyTimingPreservesNativeFactsAndMissingValues) {
    EXPECT_EQ(
        browser_reply_timing("reply", {}, {}, {}),
        R"({"kind":"replyTiming","responseId":"reply","lastVoiceSentMs":null,"firstTextMs":null,"firstNetworkMs":null,"firstPlayableMs":null})");
    const AudioTiming timing{120, 140};
    EXPECT_EQ(
        browser_reply_timing("r\"", timing, 100, 110),
        R"({"kind":"replyTiming","responseId":"r\"","lastVoiceSentMs":100,"firstTextMs":110,"firstNetworkMs":120,"firstPlayableMs":140})");
    EXPECT_EQ(
        browser_reply_timing("reply", {0, 0}, 0, 0),
        R"({"kind":"replyTiming","responseId":"reply","lastVoiceSentMs":0,"firstTextMs":0,"firstNetworkMs":0,"firstPlayableMs":0})");
}

// Realtime回复可能与末段用户音频重叠；保存原事实，不换起点或让合法业务失败。
TEST(BrowserWire, ReplyTimingAllowsRealOverlapWithoutComputingNegativeLatency) {
    EXPECT_EQ(
        browser_reply_timing("reply", {90, 95}, 100, 80),
        R"({"kind":"replyTiming","responseId":"reply","lastVoiceSentMs":100,"firstTextMs":80,"firstNetworkMs":90,"firstPlayableMs":95})");
    EXPECT_FALSE(browser_reply_timing("reply", {90, 90}, 100, {}).empty());
}

// 非法标识、精度越界及不可能解码顺序拒绝；safe上界原十进制必须精确保留。
TEST(BrowserWire, ReplyTimingRejectsUnsafeOrImpossibleFacts) {
    constexpr std::uint64_t maximum = 9007199254740991ULL;
    const auto body = browser_reply_timing("reply", {maximum, maximum}, maximum, maximum);
    EXPECT_EQ(
        body,
        R"({"kind":"replyTiming","responseId":"reply","lastVoiceSentMs":9007199254740991,"firstTextMs":9007199254740991,"firstNetworkMs":9007199254740991,"firstPlayableMs":9007199254740991})");
    EXPECT_TRUE(browser_reply_timing("", {}, {}, {}).empty());
    EXPECT_TRUE(browser_reply_timing(std::string(129, 'x'), {}, {}, {}).empty());
    EXPECT_TRUE(browser_reply_timing(std::string("x\0y", 3), {}, {}, {}).empty());
    EXPECT_TRUE(browser_reply_timing("x\ny", {}, {}, {}).empty());
    // 原回复标识可含有效UTF8，但编码边界不能输出过长编码、代理区或截断字节。
    EXPECT_FALSE(browser_reply_timing("回复😀", {}, {}, {}).empty());
    for (const auto& invalid : {std::string("\xc0\xaf", 2), std::string("\xed\xa0\x80", 3),
                                std::string("\xf0\x9f", 2)})
        EXPECT_TRUE(browser_reply_timing(invalid, {}, {}, {}).empty());
    EXPECT_TRUE(browser_reply_timing("reply", {maximum + 1, {}}, {}, {}).empty());
    EXPECT_TRUE(browser_reply_timing("reply", {{}, maximum + 1}, {}, {}).empty());
    EXPECT_TRUE(browser_reply_timing("reply", {}, maximum + 1, {}).empty());
    EXPECT_TRUE(browser_reply_timing("reply", {}, {}, maximum + 1).empty());
    EXPECT_TRUE(browser_reply_timing("reply", {10, 9}, {}, {}).empty());
    EXPECT_TRUE(browser_reply_timing("reply", {{}, 10}, {}, {}).empty());
}

// 本地Peer事实必须有不丢精度的正uint64字符串；不能混入设备身份或未来操作权限。
TEST(BrowserWire, WebRTCPeerFactsRequireStrictRevision) {
    BrowserRequest request;
    ASSERT_TRUE(decode_browser_request(R"({"kind":"webrtcDisconnected","peerRevision":"2"})", request));
    EXPECT_EQ(request.peer_revision, 2U);
    ASSERT_TRUE(decode_browser_request(R"({"kind":"webrtcConnected","peerRevision":"18446744073709551615"})", request));
    EXPECT_EQ(request.peer_revision, std::numeric_limits<std::uint64_t>::max());
    for (const auto body : {R"({"kind":"webrtcConnected"})",
         R"({"kind":"webrtcDisconnected","peerRevision":2})",
         R"({"kind":"webrtcOffer","peerRevision":"0","sdp":"v=0\r\no=x\r\n"})",
         R"({"kind":"webrtcConnected","peerRevision":"01"})",
         R"({"kind":"webrtcConnected","peerRevision":"18446744073709551616"})",
         R"({"kind":"webrtcConnected","peerRevision":"1","sessionId":"forged"})",
         R"({"kind":"webrtcDrained","epoch":"1"})",
         R"({"kind":"webrtcConnected","peerRevision":"1","peerRevision":"2"})"})
        EXPECT_FALSE(decode_browser_request(body, request));
}
TEST(BrowserWire, ToolStatusOmitsArgumentsAndRejectsInvalidStates) {
    ContentEvent event;
    ASSERT_EQ(
        decode_content_event(
            WireRoute::Message,
            R"({"msgToken":"m1","sessionId":"s1","messageSeq":1,"method":"toolCallStart","data":{"respId":"r1","toolCallId":"call-1","name":"tools.set<volume>\"","arguments":{"secret":"must-not-leak"}}})",
            event),
        WireResult::Ok);
    EXPECT_EQ(
        browser_tool_status(event),
        R"({"kind":"toolStatus","id":"call-1","state":"started","name":"tools.set<volume>\"","responseId":"r1"})");
    ASSERT_EQ(
        decode_content_event(
            WireRoute::Message,
            R"({"msgToken":"m2","sessionId":"s1","messageSeq":2,"method":"toolCallResult","data":{"respId":"r1","toolCallId":"call-1","state":"succeeded","result":{"secret":"must-not-leak"}}})",
            event),
        WireResult::Ok);
    EXPECT_EQ(browser_tool_status(event),
              R"({"kind":"toolStatus","id":"call-1","state":"succeeded","responseId":"r1"})");
    event.status = "unknown";
    EXPECT_EQ(browser_tool_status(event),
              R"({"kind":"toolStatus","id":"call-1","state":"unknown","responseId":"r1"})");
    event.tool_call_id.clear();
    EXPECT_TRUE(browser_tool_status(event).empty());
    event.tool_call_id = "call-1";
    event.envelope.method = "toolCallStart";
    EXPECT_TRUE(browser_tool_status(event).empty());
    event.envelope.method = "toolCallResult";
    event.tool_name = "tools.set";
    EXPECT_TRUE(browser_tool_status(event).empty());
    event.tool_name.clear();
    event.status = "done";
    EXPECT_TRUE(browser_tool_status(event).empty());
    event.status = "failed";
    event.tool_call_id = std::string(129, 'x');
    EXPECT_TRUE(browser_tool_status(event).empty());
    event.tool_call_id = "call-1";
    event.envelope.method = "respTextDone";
    EXPECT_TRUE(browser_tool_status(event).empty());
}
// ASR只给原唯一展示text附内容事件的可信回复ID；不生成另一条正文或以当前播放回复代替。
TEST(BrowserWire, AsrTextCarriesVerifiedResponseWithoutDuplicatingText) {
    ContentEvent event;
    ASSERT_EQ(
        decode_content_event(
            WireRoute::Message,
            R"({"msgToken":"asr-1","sessionId":"s1","messageSeq":1,"method":"respSttDone","data":{"respId":"asr-response","inputId":"input-1","text":"你好"}})",
            event),
        WireResult::Ok);
    EXPECT_EQ(browser_text("\n用户：" + event.text + "\n助手：", event.response_id),
              R"({"kind":"text","text":"\n用户：你好\n助手：","responseId":"asr-response"})");
    EXPECT_EQ(browser_text("delta"), R"({"kind":"text","text":"delta"})");
    EXPECT_EQ(browser_text("text", {}), R"({"kind":"text","text":"text"})");
    EXPECT_EQ(browser_text("text", "resp\"id"),
              R"({"kind":"text","text":"text","responseId":"resp\"id"})");
    EXPECT_FALSE(browser_text("text", std::string(128, 'x')).empty());
    EXPECT_TRUE(browser_text("text", std::string(129, 'x')).empty());
    std::string multibyte;  // 用原UTF8字节计数验证边界，而不是按字符个数宽放标识。
    for (unsigned count = 0; count < 42; ++count)
        multibyte += u8"中";
    EXPECT_FALSE(browser_text("text", multibyte + "ab").empty());
    EXPECT_TRUE(browser_text("text", multibyte + u8"中").empty());
    EXPECT_TRUE(browser_text(std::string(16385, 'x'), "r").empty());
    EXPECT_TRUE(browser_text(std::string("x\0y", 3), "r").empty());
    for (unsigned byte = 0; byte <= 127; ++byte) {
        if (byte >= 32 && byte != 127)
            continue;
        const std::string invalid = "r" + std::string(1, static_cast<char>(byte)) + "x";
        EXPECT_TRUE(browser_text("text", invalid).empty());
    }
}
// 工具开始/各终态使用同ContentEvent回复ID；空值保留旧JSON，非法非空不能降级成无关联通知。
TEST(BrowserWire, ToolResponseIdentityIsOptionalBoundedAndPreservedForEveryState) {
    ContentEvent event;
    event.envelope.method = "toolCallStart";
    event.tool_call_id = "call-1";
    event.tool_name = "tools.set";
    EXPECT_EQ(browser_tool_status(event),
              R"({"kind":"toolStatus","id":"call-1","state":"started","name":"tools.set"})");
    event.response_id = "r\"1";
    EXPECT_EQ(
        browser_tool_status(event),
        R"({"kind":"toolStatus","id":"call-1","state":"started","name":"tools.set","responseId":"r\"1"})");
    event.envelope.method = "toolCallResult";
    event.tool_name.clear();
    for (const auto state : {"succeeded", "failed", "unknown"}) {
        event.status = state;
        EXPECT_EQ(browser_tool_status(event),
                  std::string(R"({"kind":"toolStatus","id":"call-1","state":")") + state +
                      R"(","responseId":"r\"1"})");
    }
    event.response_id.clear();
    event.status = "succeeded";
    EXPECT_EQ(browser_tool_status(event),
              R"({"kind":"toolStatus","id":"call-1","state":"succeeded"})");
    event.response_id = std::string(128, 'x');
    EXPECT_FALSE(browser_tool_status(event).empty());
    event.response_id = std::string(129, 'x');
    EXPECT_TRUE(browser_tool_status(event).empty());
    for (unsigned byte = 0; byte <= 127; ++byte) {
        if (byte >= 32 && byte != 127)
            continue;
        event.response_id = "r" + std::string(1, static_cast<char>(byte)) + "x";
        EXPECT_TRUE(browser_tool_status(event).empty());
    }
}
// 门控本地票据保留完整64位，严格独立于云端焦点/会话报文及播放帧号。
TEST(BrowserWire, OutputGateUsesIndependentStrictTicket) {
    const BrowserAudioCommand command{
        BrowserAudioCommandKind::OutputGate, {}, 0, 9007199254740993ULL, false};
    EXPECT_EQ(browser_command(command),
              R"({"kind":"playbackGate","ticket":"9007199254740993","enabled":false})");
    BrowserRequest request;
    ASSERT_TRUE(decode_browser_request(
        R"({"kind":"outputGated","ticket":"18446744073709551615","enabled":false})", request));
    EXPECT_EQ(request.gate_ticket, 18446744073709551615ULL);
    EXPECT_FALSE(request.output_enabled);
    EXPECT_EQ(request.epoch, 0U);
    for (const auto invalid :
         {R"({"kind":"outputGated","ticket":"0","enabled":false})",
          R"({"kind":"outputGated","ticket":"01","enabled":false})",
          R"({"kind":"outputGated","ticket":"18446744073709551616","enabled":false})",
          R"({"kind":"outputGated","ticket":1,"enabled":false})",
          R"({"kind":"outputGated","ticket":"1","enabled":null})",
          R"({"kind":"outputGated","ticket":"1","enabled":false,"epoch":"1"})",
          R"({"kind":"outputGated","ticket":"1","enabled":false,"enabled":true})"}) {
        EXPECT_FALSE(decode_browser_request(invalid, request));
        EXPECT_TRUE(request.kind.empty());
    }
    EXPECT_TRUE(browser_command({BrowserAudioCommandKind::OutputGate, {}, 0, 0, false}).empty());
    EXPECT_TRUE(
        browser_command({BrowserAudioCommandKind::OutputGate, "reply", 0, 1, false}).empty());
    EXPECT_TRUE(browser_command({BrowserAudioCommandKind::OutputGate, {}, 1, 1, false}).empty());
}
}  // namespace ur::device
