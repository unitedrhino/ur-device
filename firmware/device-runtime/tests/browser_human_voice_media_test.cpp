// 文件说明：纯语音候选桥作用域和实际输出回执回归；可控端口不代替真实浏览器媒体证明。
#include "browser_human_voice_media.hpp"
#include <gtest/gtest.h>

namespace ur::device {
namespace {
// Fixture 明确提供原宿主源状态；不把这个bool当麦克风取得或实际播放许可。
struct VoiceRig {
    bool source = true;
    BrowserHumanVoiceMedia media{[this] { return source; }};
    const std::string offer =
        "v=0\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\na=rtpmap:111 opus/48000/2\r\na=ptime:20\r\n";
    bool start() {
        return media.bind_scope("human", 11, 7) && media.prepare_voice({}) &&
               media.begin_voice_candidate({});
    }
    std::string event(std::string_view kind, std::string_view extra = {}) {
        return "{\"kind\":\"" + std::string(kind) +
               "\",\"sessionId\":\"human\",\"routeGeneration\":\"11\",\"peerEpoch\":\"7\"" +
               std::string(extra) + "}";
    }
};
TEST(BrowserHumanVoiceMedia, ExplicitFixedVoiceAndNoVideoFallback) {
    VoiceRig r;
    EXPECT_TRUE(r.media.voice_supported());
    EXPECT_FALSE(r.media.prepare());
    EXPECT_FALSE(r.media.begin());
    EXPECT_FALSE(r.media.prepare_voice({}));
    ASSERT_TRUE(r.media.bind_scope("human", 11, 7));
    EXPECT_FALSE(r.media.bind_scope("other", 12, 8));
    EXPECT_FALSE(r.media.prepare_voice({16000, 20, 1}));
    EXPECT_FALSE(r.media.prepare_voice({48000, 60, 1}));
    EXPECT_FALSE(r.media.prepare_voice({48000, 20, 2}));
    EXPECT_TRUE(r.media.prepare_voice({}));
    ASSERT_TRUE(r.media.begin_voice_candidate({}));
    EXPECT_FALSE(r.media.begin_voice_candidate({}));
    auto command = r.media.take_command();
    ASSERT_TRUE(command);
    EXPECT_NE(command->find("humanVoiceStart"), std::string::npos);
    EXPECT_EQ(command->find("camera"), std::string::npos);
    EXPECT_FALSE(r.media.connected());
    EXPECT_FALSE(r.media.output_gate_confirmed(1, true));
}
TEST(BrowserHumanVoiceMedia, OriginalSdpAndActualAnswerPrecedeConnected) {
    VoiceRig r;
    ASSERT_TRUE(r.start());
    EXPECT_FALSE(
        r.media.receive(r.event("humanVoiceOffer",
                                ",\"sdp\":\"v=0\\r\\nm=audio 9 UDP/TLS/RTP/SAVPF "
                                "111\\r\\na=rtpmap:111 opus/48000/2\\r\\na=ptime:20\\r\\n\""),
                        0));
    ASSERT_TRUE(r.media.take_command());
    ASSERT_TRUE(
        r.media.receive(r.event("humanVoiceOffer",
                                ",\"sdp\":\"v=0\\r\\nm=audio 9 UDP/TLS/RTP/SAVPF "
                                "111\\r\\na=rtpmap:111 opus/48000/2\\r\\na=ptime:20\\r\\n\""),
                        1));
    ASSERT_EQ(r.media.take_offer(), r.offer);
    EXPECT_FALSE(r.media.apply_answer("v=0\r\nm=video 9 UDP/TLS/RTP/SAVPF 111\r\n"));
    ASSERT_TRUE(r.media.apply_answer(r.offer));
    EXPECT_FALSE(r.media.receive(r.event("humanVoiceAnswered"), 2));
    ASSERT_TRUE(r.media.take_command());
    EXPECT_FALSE(r.media.receive(r.event("humanVoiceConnected"), 2));
    ASSERT_TRUE(r.media.receive(r.event("humanVoiceAnswered"), 2));
    ASSERT_TRUE(r.media.receive(r.event("humanVoiceConnected"), 3));
    EXPECT_TRUE(r.media.connected());
    EXPECT_FALSE(r.media.output_gate_confirmed(1, true));
}
TEST(BrowserHumanVoiceMedia, GateNeedsOriginalDeliveredTicketAndActualAudioBlock) {
    VoiceRig r;
    ASSERT_TRUE(r.start());
    ASSERT_TRUE(r.media.take_command());
    ASSERT_EQ(r.media.request_output_gate(false, 10), 1);
    const auto actual =
        r.event("humanVoiceOutputGated",
                ",\"ticket\":\"1\",\"enabled\":false,\"frames\":1,\"nonSilentFrames\":0");
    EXPECT_FALSE(r.media.receive(actual, 11));
    ASSERT_TRUE(r.media.take_command());
    EXPECT_FALSE(r.media.receive(
        r.event("humanVoiceOutputGated",
                ",\"ticket\":\"1\",\"enabled\":false,\"frames\":0,\"nonSilentFrames\":0"),
        11));
    ASSERT_TRUE(r.media.receive(actual, 11));
    EXPECT_TRUE(r.media.output_gate_confirmed(1, false));
    EXPECT_FALSE(r.media.request_output_gate(true, 12));
    r.source = false;
    EXPECT_FALSE(r.media.output_gate_confirmed(1, false));
}
TEST(BrowserHumanVoiceMedia, TimeoutWrongScopeAndCloseNeverBecomePlaybackSuccess) {
    VoiceRig r;
    ASSERT_TRUE(r.start());
    ASSERT_TRUE(r.media.take_command());
    ASSERT_TRUE(r.media.request_output_gate(false, 10));
    ASSERT_TRUE(r.media.take_command());
    EXPECT_FALSE(
        r.media.receive("{\"kind\":\"humanVoiceOutputGated\",\"sessionId\":\"other\","
                        "\"routeGeneration\":\"11\",\"peerEpoch\":\"7\",\"ticket\":\"1\","
                        "\"enabled\":false,\"frames\":1,\"nonSilentFrames\":0}",
                        11));
    EXPECT_FALSE(r.media.poll(2010));
    EXPECT_FALSE(r.media.output_gate_confirmed(1, false));
    auto stop = r.media.take_command();
    ASSERT_TRUE(stop);
    EXPECT_NE(stop->find("humanVoiceStop"), std::string::npos);
    EXPECT_FALSE(r.media.stopped());
    EXPECT_FALSE(r.media.receive(r.event("humanVoiceConnected"), 2011));
    ASSERT_TRUE(r.media.receive(r.event("humanVoiceStopped"), 2011));
    EXPECT_TRUE(r.media.stopped());
}
TEST(BrowserHumanVoiceMedia, MissingSourceInvalidBindingAndMalformedCallbackFailClosed) {
    BrowserHumanVoiceMedia missing({});
    EXPECT_FALSE(missing.voice_supported());
    EXPECT_FALSE(missing.bind_scope("human", 11, 7));
    VoiceRig r;
    EXPECT_FALSE(r.media.bind_scope("bad\n", 11, 7));
    EXPECT_FALSE(r.media.bind_scope("human", 0, 7));
    EXPECT_FALSE(r.media.bind_scope("human", 11, 9007199254740992ULL));
    EXPECT_FALSE(r.media.receive("{}", 0));
    ASSERT_TRUE(r.start());
    ASSERT_TRUE(r.media.take_command());
    EXPECT_FALSE(r.media.receive(r.event("humanVoiceStopped", ",\"private\":true"), 1));
    EXPECT_FALSE(
        r.media.receive("{\"kind\":\"humanVoiceStopped\",\"kind\":\"humanVoiceStopped\"}", 1));
    EXPECT_FALSE(r.media.poll(0));
}
TEST(BrowserHumanVoiceMedia, BootActualFalseIsIndependentFromOriginalSessionFalse) {
    BrowserHumanVoiceMedia media([] { return true; }, 9);
    EXPECT_FALSE(media.bind_scope("human", 11, 7));
    EXPECT_FALSE(media.request_output_gate(true, 0));
    ASSERT_EQ(media.request_output_gate(false, 0), 1);
    const std::string idle =
        "{\"kind\":\"humanVoiceOutputIdle\",\"bootGeneration\":\"9\",\"ticket\":\"1\",\"enabled\":"
        "false,\"frames\":1}";
    EXPECT_FALSE(media.receive(idle, 1));
    auto init = media.take_command();
    ASSERT_TRUE(init);
    EXPECT_EQ(init->find("sessionId"), std::string::npos);
    ASSERT_TRUE(media.receive(idle, 1));
    ASSERT_TRUE(media.output_gate_confirmed(1, false));
    ASSERT_TRUE(media.bind_scope("human", 11, 7));
    EXPECT_FALSE(media.output_gate_confirmed(1, false));
    ASSERT_TRUE(media.take_command());  // 原outputBind，不创建Peer。
    ASSERT_EQ(media.request_output_gate(false, 2), 2);
    ASSERT_TRUE(media.take_command());
    EXPECT_FALSE(media.receive(idle, 3));
    ASSERT_TRUE(
        media.receive("{\"kind\":\"humanVoiceOutputGated\",\"sessionId\":\"human\","
                      "\"routeGeneration\":\"11\",\"peerEpoch\":\"7\",\"ticket\":\"2\",\"enabled\":"
                      "false,\"frames\":2,\"nonSilentFrames\":0}",
                      3));
    EXPECT_TRUE(media.output_gate_confirmed(2, false));
    EXPECT_FALSE(media.request_output_gate(true, 4));
    EXPECT_FALSE(media.connected());
}
TEST(BrowserHumanVoiceMedia, BootUnknownWrongGenerationAndUnsafeFramesCannotInstall) {
    BrowserHumanVoiceMedia media([] { return true; }, 9);
    ASSERT_TRUE(media.request_output_gate(false, 0));
    ASSERT_TRUE(media.take_command());
    EXPECT_FALSE(
        media.receive("{\"kind\":\"humanVoiceOutputIdle\",\"bootGeneration\":\"8\",\"ticket\":"
                      "\"1\",\"enabled\":false,\"frames\":1}",
                      1));
    EXPECT_FALSE(
        media.receive("{\"kind\":\"humanVoiceOutputIdle\",\"bootGeneration\":\"9\",\"ticket\":"
                      "\"1\",\"enabled\":false,\"frames\":9007199254740992}",
                      1));
    EXPECT_FALSE(media.output_gate_confirmed(1, false));
    EXPECT_FALSE(media.poll(2000));
}
TEST(BrowserHumanVoiceMedia, ConflictOfferAndOldOutputCannotReplaceOriginalFacts) {
    VoiceRig r;
    ASSERT_TRUE(r.start());
    ASSERT_TRUE(r.media.take_command());
    const auto offer = r.event("humanVoiceOffer",
                               ",\"sdp\":\"v=0\\r\\nm=audio 9 UDP/TLS/RTP/SAVPF "
                               "111\\r\\na=rtpmap:111 opus/48000/2\\r\\n\"");
    ASSERT_TRUE(r.media.receive(offer, 1));
    ASSERT_TRUE(r.media.receive(offer, 1));
    EXPECT_FALSE(r.media.receive(r.event("humanVoiceOffer",
                                         ",\"sdp\":\"v=0\\r\\nm=audio 10 UDP/TLS/RTP/SAVPF "
                                         "111\\r\\na=rtpmap:111 opus/48000/2\\r\\n\""),
                                 1));
    ASSERT_TRUE(r.media.take_offer());
    EXPECT_FALSE(r.media.take_offer());
    ASSERT_TRUE(r.media.request_output_gate(false, 2));
    ASSERT_TRUE(r.media.take_command());
    const auto ack =
        r.event("humanVoiceOutputGated",
                ",\"ticket\":\"1\",\"enabled\":false,\"frames\":2,\"nonSilentFrames\":0");
    ASSERT_TRUE(r.media.receive(ack, 3));
    ASSERT_TRUE(r.media.request_output_gate(false, 4));
    ASSERT_TRUE(r.media.take_command());
    EXPECT_FALSE(r.media.receive(ack, 5));
    EXPECT_FALSE(r.media.output_gate_confirmed(2, false));
    r.media.close();
    r.media.close();
    ASSERT_TRUE(r.media.take_command());
    EXPECT_FALSE(r.media.take_command());
}
TEST(BrowserHumanVoiceMedia, ActualOriginalPeerStoppedCannotLeaveCoreConnected) {
    VoiceRig r;
    ASSERT_TRUE(r.start());
    ASSERT_TRUE(r.media.take_command());
    ASSERT_TRUE(r.media.receive(r.event("humanVoiceStopped"), 1));
    EXPECT_TRUE(r.media.stopped());
    EXPECT_FALSE(r.media.connected());
    EXPECT_FALSE(r.media.poll(2));
}
TEST(BrowserHumanVoiceMedia, ConnectedDoesNotReplaceActualTrueTicketAndSourceUnknownInvalidates) {
    VoiceRig r;
    ASSERT_TRUE(r.start());
    ASSERT_TRUE(r.media.take_command());
    ASSERT_TRUE(r.media.receive(r.event("humanVoiceOffer",
                                        ",\"sdp\":\"v=0\\r\\nm=audio 9 UDP/TLS/RTP/SAVPF "
                                        "111\\r\\na=rtpmap:111 opus/48000/2\\r\\n\""),
                                1));
    ASSERT_TRUE(r.media.apply_answer(r.offer));
    ASSERT_TRUE(r.media.take_command());
    ASSERT_TRUE(r.media.receive(r.event("humanVoiceAnswered"), 2));
    ASSERT_TRUE(r.media.receive(r.event("humanVoiceConnected"), 3));
    EXPECT_FALSE(r.media.output_gate_confirmed(1, true));
    ASSERT_EQ(r.media.request_output_gate(true, 4), 1);
    ASSERT_TRUE(r.media.take_command());
    EXPECT_FALSE(r.media.receive(
        r.event("humanVoiceOutputGated",
                ",\"ticket\":\"1\",\"enabled\":false,\"frames\":1,\"nonSilentFrames\":0"),
        5));
    ASSERT_TRUE(r.media.receive(
        r.event("humanVoiceOutputGated",
                ",\"ticket\":\"1\",\"enabled\":true,\"frames\":1,\"nonSilentFrames\":0"),
        5));
    EXPECT_TRUE(
        r.media.output_gate_confirmed(1, true));  // 输出门控完成，不据静音block声称已经播放音频。
    r.source = false;
    EXPECT_FALSE(r.media.connected());
    EXPECT_FALSE(r.media.output_gate_confirmed(1, true));
    EXPECT_FALSE(r.media.poll(6));
}
TEST(BrowserHumanVoiceMedia, ThrowingSourceAndOversizedOrUnknownLocalBodyCannotAdvance) {
    BrowserHumanVoiceMedia throwing([]() -> bool { throw 1; });
    EXPECT_FALSE(throwing.voice_supported());
    EXPECT_FALSE(throwing.available());
    EXPECT_FALSE(throwing.bind_scope("human", 11, 7));
    VoiceRig r;
    ASSERT_TRUE(r.start());
    ASSERT_TRUE(r.media.take_command());
    EXPECT_FALSE(r.media.receive(std::string(65537, ' '), 1));
    EXPECT_FALSE(r.media.receive(r.event("humanVoiceUnknown"), 1));
    EXPECT_FALSE(
        r.media.receive("{\"kind\":\"humanVoiceConnected\",\"sessionId\":\"human\","
                        "\"routeGeneration\":\"11\",\"peerEpoch\":\"8\"}",
                        1));
    EXPECT_FALSE(r.media.connected());
}
TEST(BrowserHumanVoiceMedia, EscapedAnswerMustStayInsideLocalWireBudget) {
    VoiceRig r;
    ASSERT_TRUE(r.start());
    ASSERT_TRUE(r.media.take_command());
    ASSERT_TRUE(r.media.receive(r.event("humanVoiceOffer",
                                        ",\"sdp\":\"v=0\\r\\nm=audio 9 UDP/TLS/RTP/SAVPF "
                                        "111\\r\\na=rtpmap:111 opus/48000/2\\r\\n\""),
                                1));
    EXPECT_FALSE(r.media.apply_answer(r.offer + std::string(40000, '\\')));
    EXPECT_FALSE(r.media.take_command());
    EXPECT_TRUE(r.media.apply_answer(r.offer));  // 背压失败没有保留答案关联或暗自重发。
}
// 原poll/close均只留下false；闭集旁证必须保留首个实际分支，而非清理时覆盖成close。
TEST(BrowserHumanVoiceMedia, FailureEvidenceDistinguishesFiveOriginalStopBranches) {
    VoiceRig clock;
    ASSERT_TRUE(clock.start());
    ASSERT_TRUE(clock.media.poll(10));
    clock.source = false;  // 同时源失效时仍沿原时钟优先短路，不误贴后继源标签。
    EXPECT_FALSE(clock.media.poll(9));
    EXPECT_EQ(clock.media.failure_reason(), "clockRollback");
    clock.media.close();
    EXPECT_EQ(clock.media.failure_reason(), "clockRollback");

    VoiceRig source;
    ASSERT_TRUE(source.start());
    source.source = false;
    EXPECT_FALSE(source.media.poll(10));
    EXPECT_EQ(source.media.failure_reason(), "sourceUnavailable");

    VoiceRig gate;
    ASSERT_TRUE(gate.start());
    ASSERT_TRUE(gate.media.take_command());
    ASSERT_TRUE(gate.media.request_output_gate(false, 10));
    EXPECT_TRUE(gate.media.poll(2009));
    EXPECT_FALSE(gate.media.poll(2010));
    EXPECT_EQ(gate.media.failure_reason(), "gateDeadline");
    ASSERT_TRUE(gate.media.take_command());
    ASSERT_TRUE(gate.media.receive(gate.event("humanVoiceStopped"), 2011));
    EXPECT_EQ(gate.media.failure_reason(), "gateDeadline");

    VoiceRig remote;
    ASSERT_TRUE(remote.start());
    ASSERT_TRUE(remote.media.take_command());
    EXPECT_FALSE(
        remote.media.receive(remote.event("humanVoiceStopped", ",\"cause\":\"unknown\""), 9));
    EXPECT_EQ(remote.media.failure_reason(), "unknown");  // 仍严格拒绝额外本地字段。
    ASSERT_TRUE(remote.media.receive(remote.event("humanVoiceStopped"), 10));
    remote.source = false;
    EXPECT_FALSE(remote.media.poll(11));
    EXPECT_EQ(remote.media.failure_reason(), "remoteStopped");

    VoiceRig explicit_close;
    ASSERT_TRUE(explicit_close.start());
    explicit_close.media.close();
    explicit_close.source = false;
    EXPECT_FALSE(explicit_close.media.poll(10));
    EXPECT_EQ(explicit_close.media.failure_reason(), "explicitClose");
}
// getter只读闭集首因；既不调用可能有副作用的源回调，也不取走尚未发送的原命令。
TEST(BrowserHumanVoiceMedia, FailureEvidenceGetterDoesNotReadSourceOrConsumeCommands) {
    unsigned reads = 0;
    BrowserHumanVoiceMedia media([&reads] {
        ++reads;
        return true;
    });
    ASSERT_TRUE(media.bind_scope("human", 11, 7));
    ASSERT_TRUE(media.prepare_voice({}));
    ASSERT_TRUE(media.begin_voice_candidate({}));
    const auto before = reads;
    EXPECT_EQ(media.failure_reason(), "unknown");
    EXPECT_EQ(media.failure_reason(), "unknown");
    EXPECT_EQ(reads, before);
    ASSERT_TRUE(media.take_command());
    EXPECT_FALSE(media.take_command());
    ASSERT_TRUE(media.poll(10));
    const auto polled = reads;
    EXPECT_FALSE(media.poll(9));
    EXPECT_EQ(reads, polled);  // 原时钟短路不能额外读取source。
    EXPECT_EQ(media.failure_reason(), "clockRollback");
    EXPECT_EQ(reads, polled);
    const auto stop = media.take_command();
    ASSERT_TRUE(stop);
    EXPECT_NE(stop->find("humanVoiceStop"), std::string::npos);
    EXPECT_FALSE(media.take_command());
    EXPECT_FALSE(media.poll(10));
    EXPECT_EQ(reads, polled + 1);  // 已closing后仍保留原source读取，不能以诊断优化掉。
    EXPECT_EQ(media.failure_reason(), "clockRollback");
}
// 独立旁证匹配原scope且不poll；首条unknown也不能被重复或冲突改成已知原因。
TEST(BrowserHumanVoiceMedia, StopDiagnosticIsScopedDefaultOffAndDoesNotPoll) {
    unsigned reads = 0;
    BrowserHumanVoiceMedia media([&reads] {
        ++reads;
        return true;
    });
    ASSERT_TRUE(media.bind_scope("human", 11, 7));
    ASSERT_TRUE(media.prepare_voice({}));
    ASSERT_TRUE(media.begin_voice_candidate({}));
    ASSERT_TRUE(media.take_command());
    ASSERT_TRUE(media.poll(10));
    const auto before = reads;
    const std::string diagnostic =
        R"({"kind":"humanVoiceStopDiagnostic","sessionId":"human","routeGeneration":"11","peerEpoch":"7","known":true,"cause":"peerFailed"})";
    EXPECT_EQ(media.receive_stop_diagnostic(diagnostic), std::optional<bool>{false});
    EXPECT_EQ(media.stop_cause(), "unknown");
    EXPECT_EQ(media.receive_stop_diagnostic(diagnostic, true), std::optional<bool>{true});
    EXPECT_EQ(media.receive_stop_diagnostic(diagnostic, true), std::optional<bool>{true});
    EXPECT_EQ(media.stop_cause(), "peerFailed");
    EXPECT_EQ(media.failure_reason(), "unknown");
    EXPECT_FALSE(media.stopped());
    EXPECT_EQ(reads, before);
    EXPECT_FALSE(
        media
            .receive_stop_diagnostic(
                R"({"kind":"humanVoiceStopDiagnostic","sessionId":"human","routeGeneration":"11","peerEpoch":"7","known":false,"cause":"unknown"})",
                true)
            .value_or(true));
    EXPECT_EQ(media.stop_cause(), "peerFailed");
    EXPECT_TRUE(media.receive(
        R"({"kind":"humanVoiceStopped","sessionId":"human","routeGeneration":"11","peerEpoch":"7"})",
        11));
    EXPECT_EQ(media.failure_reason(), "remoteStopped");
    EXPECT_EQ(media.stop_cause(), "peerFailed");
    EXPECT_FALSE(media.receive_stop_diagnostic(diagnostic, true).value_or(true));
    media.close();
    EXPECT_EQ(media.stop_cause(), "peerFailed");
}
TEST(BrowserHumanVoiceMedia, StopDiagnosticRejectsMalformedAndConflictingFirstCause) {
    VoiceRig r;
    ASSERT_TRUE(r.start());
    ASSERT_TRUE(r.media.take_command());
    for (
        const auto* body :
        {R"({"kind":"humanVoiceStopDiagnostic","sessionId":"other","routeGeneration":"11","peerEpoch":"7","known":true,"cause":"peerFailed"})",
         R"({"kind":"humanVoiceStopDiagnostic","sessionId":"human","routeGeneration":"12","peerEpoch":"7","known":true,"cause":"peerFailed"})",
         R"({"kind":"humanVoiceStopDiagnostic","sessionId":"human","routeGeneration":"11","peerEpoch":"8","known":true,"cause":"peerFailed"})",
         R"({"kind":"humanVoiceStopDiagnostic","sessionId":"human","routeGeneration":"11","peerEpoch":"7","known":true,"cause":"unknown"})",
         R"({"kind":"humanVoiceStopDiagnostic","sessionId":"human","routeGeneration":"11","peerEpoch":"7","known":false,"cause":"peerFailed"})",
         R"({"kind":"humanVoiceStopDiagnostic","sessionId":"human","routeGeneration":"11","peerEpoch":"7","known":true,"cause":"PRIVATE"})",
         R"({"kind":"humanVoiceStopDiagnostic","sessionId":"human","routeGeneration":"11","peerEpoch":"7","known":true,"cause":"peerFailed","extra":1})",
         R"({"kind":"humanVoiceStopDiagnostic","sessionId":"human","routeGeneration":"11","peerEpoch":"7","known":true,"cause":"peerFailed","cause":"processorError"})"}) {
        EXPECT_EQ(r.media.receive_stop_diagnostic(body, true), std::optional<bool>{false});
        EXPECT_EQ(r.media.stop_cause(), "unknown");
    }
    EXPECT_EQ(r.media.receive_stop_diagnostic(
                  "{\"kind\":\"humanVoiceStopDiagnostic\"" + std::string(1025, 'x'), true),
              std::optional<bool>{false});
    EXPECT_EQ(r.media.receive_stop_diagnostic(R"({"kind":"other"})", true), std::nullopt);
    const auto unknown = r.event("humanVoiceStopDiagnostic", R"(,"known":false,"cause":"unknown")");
    EXPECT_EQ(r.media.receive_stop_diagnostic(unknown, true), std::optional<bool>{true});
    EXPECT_EQ(
        r.media.receive_stop_diagnostic(
            r.event("humanVoiceStopDiagnostic", R"(,"known":true,"cause":"processorError")"), true),
        std::optional<bool>{false});
    EXPECT_EQ(r.media.stop_cause(), "unknown");
}
// 非候选即使不是JSON也直接交还原链；关闭开关的候选不解析、不查询源或消费媒体命令。
TEST(BrowserHumanVoiceMedia, StopDiagnosticNonCandidateAndDefaultOffDoNotParseOrReadSource) {
    unsigned reads = 0;
    BrowserHumanVoiceMedia media([&reads] {
        ++reads;
        return true;
    });
    ASSERT_TRUE(media.bind_scope("human", 11, 7));
    const auto before = reads;
    const std::string invalid_utf8(1, static_cast<char>(0xff));
    EXPECT_EQ(media.receive_stop_diagnostic(invalid_utf8, true), std::nullopt);
    EXPECT_EQ(media.receive_stop_diagnostic(std::string(65537, 'x'), true), std::nullopt);
    EXPECT_EQ(media.receive_stop_diagnostic(R"({"kind":"humanVoiceStopDiagnostic")"),
              std::optional<bool>{false});
    EXPECT_EQ(media.receive_stop_diagnostic(R"({"kind":"humanVoiceStopDiagnostic")", true),
              std::optional<bool>{false});
    // 原普通kind即使正文包含完整旁证种类仍交还业务入口，开关关闭也不能吞text或SDP。
    for (const bool enabled : {false, true}) {
        EXPECT_EQ(
            media.receive_stop_diagnostic(
                R"({"kind":"sendText","text":"humanVoiceStopDiagnostic","audio":false})", enabled),
            std::nullopt);
        EXPECT_EQ(media.receive_stop_diagnostic("not JSON humanVoiceStopDiagnostic", enabled),
                  std::nullopt);
    }
    EXPECT_EQ(reads, before);
    EXPECT_EQ(media.stop_cause(), "unknown");
    EXPECT_FALSE(media.take_command());
}
// 原正式release双false之后关闭Peer，实际Stopped才允许保留该原false证明供另一腿恢复。
TEST(BrowserHumanVoiceMedia, RetiredActualStoppedRetainsOnlyOriginalConfirmedFalseTicket) {
    VoiceRig r;
    ASSERT_TRUE(r.start());
    ASSERT_TRUE(r.media.take_command());
    ASSERT_EQ(r.media.request_output_gate(false, 10), 1);
    ASSERT_TRUE(r.media.take_command());
    ASSERT_TRUE(r.media.receive(
        r.event("humanVoiceOutputGated",
                ",\"ticket\":\"1\",\"enabled\":false,\"frames\":1,\"nonSilentFrames\":0"),
        11));
    r.media.close();
    ASSERT_TRUE(r.media.take_command());
    EXPECT_FALSE(r.media.output_gate_confirmed(1, false));
    ASSERT_TRUE(r.media.receive(r.event("humanVoiceStopped"), 12));
    EXPECT_TRUE(r.media.output_gate_confirmed(1, false));
    EXPECT_FALSE(r.media.output_gate_confirmed(1, true));
    EXPECT_FALSE(r.media.output_gate_confirmed(2, false));
    EXPECT_FALSE(r.media.request_output_gate(true, 13));
    r.source = false;
    EXPECT_FALSE(r.media.output_gate_confirmed(1, false));
}
TEST(BrowserHumanVoiceMedia, StoppedWithoutPriorFalseConfirmationCannotInventReleaseProof) {
    VoiceRig r;
    ASSERT_TRUE(r.start());
    ASSERT_TRUE(r.media.take_command());
    ASSERT_EQ(r.media.request_output_gate(false, 10), 1);
    ASSERT_TRUE(r.media.take_command());
    r.media.close();
    ASSERT_TRUE(r.media.take_command());
    ASSERT_TRUE(r.media.receive(r.event("humanVoiceStopped"), 12));
    EXPECT_FALSE(r.media.output_gate_confirmed(1, false));
}
} // namespace
} // namespace ur::device
