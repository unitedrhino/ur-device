/* 联犀 AI 可移植协议核心的主机单元测试与 devicesim 时序回放测试。 */
#include "ur_ai_runtime.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
/* Check 在条件不成立时输出具体断言名称并累计失败数。 */
void Check(bool condition, std::string_view name, int* failures) {
    if (!condition) {
        std::cerr << "FAIL: " << name << '\n';
        ++*failures;
    }
}

/* TestEmotionMap 验证 devicesim 的 21 类映射及未知值兜底。 */
void TestEmotionMap(int* failures) {
    constexpr std::array<std::pair<std::string_view, std::string_view>, 21> kOracle = {{
        {"😶", "neutral"},     {"🙂", "happy"},     {"😆", "laughing"}, {"😂", "funny"},
        {"😔", "sad"},         {"😠", "angry"},     {"😭", "crying"},   {"😍", "loving"},
        {"😳", "embarrassed"}, {"😲", "surprised"}, {"😱", "shocked"},  {"🤔", "thinking"},
        {"😉", "winking"},     {"😎", "cool"},      {"😌", "relaxed"},  {"🤤", "delicious"},
        {"😘", "kissy"},       {"😏", "confident"}, {"😴", "sleepy"},   {"😜", "silly"},
        {"🙄", "confused"},
    }};
    for (const auto& [emoji, emotion] : kOracle) {
        Check(UrAiNormalizeEmotion(emoji, "") == emotion,
              std::string("emoji-") + std::string(emotion), failures);
        Check(UrAiNormalizeEmotion("", emotion) == emotion,
              std::string("emotion-") + std::string(emotion), failures);
    }
    Check(UrAiNormalizeEmotion("unknown", "unknown") == "neutral", "emotion-fallback", failures);
}

/* TestCorrelation 验证 session/respId 的缺省兼容、延后与过期拒绝规则。 */
void TestCorrelation(int* failures) {
    Check(UrAiMatchesSession("session-1", ""), "session-missing-compatible", failures);
    Check(UrAiMatchesSession("session-1", "session-1"), "session-match", failures);
    Check(!UrAiMatchesSession("session-1", "session-old"), "session-old-rejected", failures);
    Check(!UrAiMatchesSession("", "session-1"), "session-without-current-rejected", failures);

    Check(UrAiClassifyResponse("", "resp-1", "respAudioDone") == UrAiResponseDisposition::kDefer,
          "response-terminal-deferred", failures);
    Check(UrAiClassifyResponse("", "resp-1", "respTextDelta") == UrAiResponseDisposition::kReject,
          "response-delta-dropped", failures);
    Check(UrAiClassifyResponse("resp-1", "resp-1", "respTextDone") ==
              UrAiResponseDisposition::kAccept,
          "response-match", failures);
    Check(UrAiClassifyResponse("resp-1", "resp-old", "respTextDone") ==
              UrAiResponseDisposition::kReject,
          "response-old-rejected", failures);
    Check(UrAiClassifyResponse("resp-1", "", "respTextDone") == UrAiResponseDisposition::kReject,
          "response-missing-rejected", failures);
}

/* TestTerminalTracker 验证可靠终态首次消费、重复抑制和跨轮重置。 */
void TestTerminalTracker(int* failures) {
    UrAiTerminalTracker tracker;
    Check(tracker.Mark(UrAiTerminalEvent::kSttDone), "terminal-stt-first", failures);
    Check(!tracker.Mark(UrAiTerminalEvent::kSttDone), "terminal-stt-duplicate", failures);
    Check(tracker.Mark(UrAiTerminalEvent::kTextDone), "terminal-text-first", failures);
    Check(tracker.Mark(UrAiTerminalEvent::kAudioDone), "terminal-audio-first", failures);
    Check(tracker.Mark(UrAiTerminalEvent::kEmotion), "terminal-emotion-first", failures);
    tracker.Reset();
    Check(!tracker.Seen(UrAiTerminalEvent::kSttDone), "terminal-reset-stt", failures);
    Check(tracker.Mark(UrAiTerminalEvent::kAudioDone), "terminal-next-turn", failures);
}

/* TestUdpHeader 验证 devicesim nonce 布局、网络字节序和畸形数据报拒绝。 */
void TestUdpHeader(int* failures) {
    constexpr std::array<uint8_t, kUrAiUdpHeaderSize> kNonce = {0x01, 0x00, 0x00, 0x00, 0x11, 0x22,
                                                                0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
                                                                0x00, 0x00, 0x00, 0x00};
    std::array<uint8_t, kUrAiUdpHeaderSize> header = {};
    Check(UrAiBuildUdpHeader(kNonce.data(), kNonce.size(), 0x0123, 0xaabbccdd, 0x01020304, true,
                             header.data(), header.size()),
          "udp-build-preserved", failures);
    Check(header[2] == 0x01 && header[3] == 0x23, "udp-payload-network-order", failures);
    Check(header[8] == 0x55 && header[11] == 0x88, "udp-nonce-timestamp-preserved", failures);
    Check(header[12] == 0x01 && header[15] == 0x04, "udp-sequence-network-order", failures);

    Check(UrAiBuildUdpHeader(kNonce.data(), kNonce.size(), 4, 0xaabbccdd, 2, false, header.data(),
                             header.size()),
          "udp-build-timestamp", failures);
    Check(header[8] == 0xaa && header[11] == 0xdd, "udp-timestamp-network-order", failures);
    std::vector<uint8_t> datagram(header.begin(), header.end());
    datagram.insert(datagram.end(), {0xde, 0xad, 0xbe, 0xef});
    UrAiUdpHeader parsed;
    Check(UrAiParseUdpHeader(datagram.data(), datagram.size(), &parsed), "udp-parse", failures);
    Check(parsed.payload_size == 4 && parsed.timestamp == 0xaabbccdd && parsed.sequence == 2,
          "udp-parsed-fields", failures);
    datagram.pop_back();
    Check(!UrAiParseUdpHeader(datagram.data(), datagram.size(), &parsed),
          "udp-length-mismatch-rejected", failures);
    Check(!UrAiBuildUdpHeader(kNonce.data(), kNonce.size(), 0, 0, 1, true, header.data(),
                              header.size()),
          "udp-empty-payload-rejected", failures);
}

/* TestVadEndpoint 回放现场短尾音、句中停顿与打断后的新一轮有效语音。 */
void TestVadEndpoint(int* failures) {
    UrAiVadEndpoint endpoint;
    endpoint.Begin(1000000);
    endpoint.Update(true, 1100000);
    endpoint.Update(false, 1200000);
    Check(!endpoint.ShouldStop(5000000), "vad-wake-tail-does-not-close-listening", failures);
    endpoint.Update(true, 5000000);
    endpoint.Update(false, 5400000);
    Check(!endpoint.ShouldStop(5500000), "vad-100ms-pause-is-not-end", failures);
    endpoint.Update(true, 5600000);
    Check(!endpoint.ShouldStop(6500000), "vad-resumed-speech-keeps-listening", failures);
    endpoint.Update(false, 6600000);
    Check(!endpoint.ShouldStop(7299999), "vad-silence-boundary-before", failures);
    Check(endpoint.ShouldStop(7300000), "vad-stable-silence-ends-turn", failures);
    endpoint.Begin(8000000);
    Check(!endpoint.ShouldStop(10000000), "vad-new-turn-forgets-old-silence", failures);
    endpoint.Update(true, 10100000);
    endpoint.Update(false, 10150000);
    Check(!endpoint.ShouldStop(12000000), "vad-click-noise-does-not-end-turn", failures);
    endpoint.Update(true, 12100000);
    endpoint.Update(false, 12500000);
    Check(endpoint.ShouldStop(13200000), "vad-post-interrupt-real-speech-ends", failures);

    // Watcher 的半双工自动监听需要跨越较长的弱音漏检。
    UrAiVadEndpoint watcher_endpoint(2200000);
    watcher_endpoint.Begin(1000000);
    watcher_endpoint.Update(true, 1400000);
    watcher_endpoint.Update(false, 1700000);
    Check(!watcher_endpoint.ShouldStop(2400000), "vad-watcher-keeps-capturing-after-short-silence",
          failures);
    watcher_endpoint.Update(true, 3200000);
    watcher_endpoint.Update(false, 3500000);
    Check(!watcher_endpoint.ShouldStop(5699999), "vad-watcher-restarts-silence-window", failures);
    Check(watcher_endpoint.ShouldStop(5700000), "vad-watcher-silence-boundary", failures);
}

/* RunUnitSuite 执行不依赖设备与网络的生产协议核心单元测试。 */
int RunUnitSuite() {
    int failures = 0;
    TestEmotionMap(&failures);
    TestCorrelation(&failures);
    TestTerminalTracker(&failures);
    TestUdpHeader(&failures);
    TestVadEndpoint(&failures);
    if (failures == 0) {
        std::cout << "PASS unit: emotion correlation terminal udp\n";
    }
    return failures;
}

/* RunE2eSuite 按 devicesim 顺序回放两轮、乱序、重复、打断与旧消息场景。 */
int RunE2eSuite() {
    int failures = 0;
    const std::string session_id = "session-a";
    std::string current_response;
    std::vector<std::pair<std::string, std::string>> deferred;
    UrAiTerminalTracker terminals;

    Check(UrAiMatchesSession(session_id, session_id), "e2e-session-created", &failures);

    constexpr std::array<uint8_t, kUrAiUdpHeaderSize> kNonce = {0x01, 0x00, 0x00, 0x00, 0x11, 0x22,
                                                                0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
                                                                0x00, 0x00, 0x00, 0x00};
    std::array<uint8_t, kUrAiUdpHeaderSize> header = {};
    for (uint32_t sequence = 1; sequence <= 3; ++sequence) {
        Check(UrAiBuildUdpHeader(kNonce.data(), kNonce.size(), 20, sequence * 960, sequence, true,
                                 header.data(), header.size()),
              "e2e-preheat-and-audio-frame", &failures);
        Check(header[15] == sequence, "e2e-monotonic-uplink-sequence", &failures);
    }

    const auto DeferIfNeeded = [&](std::string method, std::string resp_id) {
        const auto disposition = UrAiClassifyResponse(current_response, resp_id, method);
        if (disposition == UrAiResponseDisposition::kDefer) {
            deferred.emplace_back(std::move(method), std::move(resp_id));
        }
        return disposition;
    };
    Check(DeferIfNeeded("respEmotion", "resp-1") == UrAiResponseDisposition::kDefer,
          "e2e-emotion-before-created-deferred", &failures);
    Check(DeferIfNeeded("respAudioDone", "resp-1") == UrAiResponseDisposition::kDefer,
          "e2e-audio-done-before-created-deferred", &failures);
    Check(DeferIfNeeded("respTextDelta", "resp-1") == UrAiResponseDisposition::kReject,
          "e2e-congested-delta-dropped", &failures);

    current_response = "resp-1";
    for (const auto& [method, resp_id] : deferred) {
        Check(UrAiClassifyResponse(current_response, resp_id, method) ==
                  UrAiResponseDisposition::kAccept,
              "e2e-deferred-replayed", &failures);
    }
    Check(terminals.Mark(UrAiTerminalEvent::kEmotion), "e2e-emotion-applied", &failures);
    Check(UrAiNormalizeEmotion("🤔", "") == "thinking", "e2e-emotion-rendered", &failures);
    Check(terminals.Mark(UrAiTerminalEvent::kAudioDone), "e2e-audio-done-applied", &failures);
    Check(!terminals.Mark(UrAiTerminalEvent::kAudioDone), "e2e-audio-done-deduplicated", &failures);

    terminals.Reset();
    current_response = "resp-2";
    Check(UrAiClassifyResponse(current_response, "resp-1", "respTextDone") ==
              UrAiResponseDisposition::kReject,
          "e2e-old-turn-rejected", &failures);
    Check(terminals.Mark(UrAiTerminalEvent::kSttDone), "e2e-second-turn-stt", &failures);
    Check(terminals.Mark(UrAiTerminalEvent::kTextDone), "e2e-second-turn-text", &failures);

    /* 模拟播报中 respCancel 后的新 audioStart：旧 respId 失效，新轮终态重新可消费。 */
    terminals.Reset();
    current_response.clear();
    Check(UrAiClassifyResponse(current_response, "resp-2", "respAudioDone") ==
              UrAiResponseDisposition::kDefer,
          "e2e-interrupt-tail-deferred-until-new-response", &failures);
    current_response = "resp-3";
    Check(UrAiClassifyResponse(current_response, "resp-2", "respAudioDone") ==
              UrAiResponseDisposition::kReject,
          "e2e-interrupted-tail-rejected", &failures);
    Check(terminals.Mark(UrAiTerminalEvent::kAudioDone), "e2e-post-interrupt-audio-done",
          &failures);
    Check(!UrAiMatchesSession(session_id, "session-old"), "e2e-disconnected-session-rejected",
          &failures);

    if (failures == 0) {
        std::cout << "PASS e2e: preheat two-turn out-of-order duplicate interrupt reconnect\n";
    }
    return failures;
}
}  // namespace

/* main 按参数选择单元或协议 E2E 套件，并以退出码返回失败数量。 */
int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: ur_ai_runtime_test <unit|e2e>\n";
        return 2;
    }
    const std::string_view suite = argv[1];
    if (suite == "unit") {
        return RunUnitSuite();
    }
    if (suite == "e2e") {
        return RunE2eSuite();
    }
    std::cerr << "unknown suite: " << suite << '\n';
    return 2;
}
