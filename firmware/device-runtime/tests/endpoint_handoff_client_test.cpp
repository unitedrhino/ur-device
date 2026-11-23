// 文件说明：可控时钟验证显式设备接续、原字节重试、乱序ACK与终态；不模拟媒体ready或授权。
#include "ur/device/endpoint_handoff_client.hpp"
#include <gtest/gtest.h>
#include <vector>

namespace ur::device {
namespace {
constexpr auto up = "$thing/up/signaling/p/d";     // 合成已鉴权设备精确上行。
constexpr auto down = "$thing/down/signaling/p/d"; // 同一设备下行，不能跨身份。
// EndpointPublisher 只记录提交字节，不能自动产生业务应答或媒体事实。
struct EndpointPublisher final : PublishPort {
  std::vector<std::string> bodies; // 本用例原发布正文，不输出日志。
  bool publish(std::string_view topic, std::string_view body) override {
    EXPECT_EQ(topic, up);
    bodies.emplace_back(body);
    return true;
  }
};
TEST(EndpointHandoffClient, SocketSuspensionKeepsOriginalUnknownIntentWithoutRetryingIt) {
  EndpointPublisher publisher;
  EndpointHandoffClient client(publisher, up, down, "boot");
  ASSERT_TRUE(client.bind("source", 0));
  ASSERT_TRUE(client.prepare("device:p:target", 1));
  const auto original = client.receipt();
  client.suspend(2);
  EXPECT_FALSE(client.receipt().pending);
  EXPECT_TRUE(client.receipt().unknown);
  EXPECT_EQ(client.receipt().token, original.token);
  EXPECT_EQ(client.receipt().handoff, original.handoff);
  EXPECT_FALSE(client.prepare("device:p:other", 3));
  client.poll(30002);
  EXPECT_EQ(publisher.bodies.size(), 1U);
  EXPECT_EQ(client.receipt().session, "source");
  EXPECT_TRUE(client.receipt().unknown);
}
// fact 生成严格平台状态；reply使用原操作token，事件使用独立全局水位。
std::string fact(const EndpointHandoffReceipt &receipt, std::string state,
                 std::uint64_t revision, bool reply, std::uint64_t sequence = 1,
                 std::string reason = {}) {
  Envelope event;
  event.request = false;
  event.method = "endpointState";
  event.token = reply ? receipt.token : "event-" + std::to_string(sequence);
  event.session = receipt.session;
  event.payload = "{\"state\":\"" + state +
                  "\",\"revision\":" + std::to_string(revision) +
                  ",\"handoffId\":\"" + receipt.handoff + "\"";
  if (state == "ready" || state == "committed")
    event.payload += ",\"candidateSessionId\":\"candidate\"";
  if (!reason.empty())
    event.payload += ",\"reason\":\"" + reason + "\"";
  event.payload += "}";
  if (reply)
    event.code = state == "failed" ? 409 : 200;
  else
    event.event_sequence = sequence;
  std::string body;
  EXPECT_EQ(encode_envelope(event, body), DecodeResult::Ok);
  return body;
}

// ingress_rejection 沿用DMS原受理层负向ACK；它不是endpointState事务事实。
std::string ingress_rejection(const EndpointHandoffReceipt &receipt,
                              int code = 409,
                              std::string reason = "requestConflict",
                              std::string method = "endpointControl") {
  Envelope value;
  value.request = false;
  value.token = receipt.token;
  value.session = receipt.session;
  value.method = "requestAck";
  value.code = code;
  value.payload =
      "{\"requestMethod\":\"" + method + "\",\"reason\":\"" + reason + "\"}";
  std::string body;
  EXPECT_EQ(encode_envelope(value, body), DecodeResult::Ok);
  return body;
}

// 明确受理拒绝不造failed终态；只结束原token，允许用户另行发起并保存旧ACK去重事实。
TEST(EndpointHandoffClient,
     NarrowIngressRecognitionDoesNotRouteOtherResponses) {
  EndpointHandoffReceipt receipt;
  receipt.session = "source";
  receipt.token = "original";
  EXPECT_TRUE(endpoint_handoff_ingress_ack(ingress_rejection(receipt)));
  EXPECT_TRUE(endpoint_handoff_ingress_ack(
      ingress_rejection(receipt, 400, "invalidRequest")));
  EXPECT_TRUE(endpoint_handoff_ingress_ack(
      ingress_rejection(receipt, 403, "requestDenied")));
  EXPECT_TRUE(endpoint_handoff_ingress_ack(
      ingress_rejection(receipt, 422, "unsupportedMethod")));
  EXPECT_FALSE(endpoint_handoff_ingress_ack(ingress_rejection(receipt, 200)));
  EXPECT_FALSE(endpoint_handoff_ingress_ack(
      ingress_rejection(receipt, 409, "requestDenied")));
  EXPECT_FALSE(endpoint_handoff_ingress_ack(
      ingress_rejection(receipt, 409, "requestConflict", "callControl")));
  EXPECT_FALSE(
      endpoint_handoff_ingress_ack(fact(receipt, "preparing", 1, true)));
  EXPECT_FALSE(endpoint_handoff_ingress_ack("{\"method\":\"requestAck\"}"));
  Envelope envelope;
  ASSERT_EQ(decode_envelope(ingress_rejection(receipt), envelope),
            DecodeResult::Ok);
  envelope.event_sequence = 3;
  std::string encoded;
  ASSERT_EQ(encode_envelope(envelope, encoded), DecodeResult::Ok);
  EXPECT_FALSE(endpoint_handoff_ingress_ack(encoded));
  envelope.event_sequence.reset();
  envelope.payload = "{\"requestMethod\":\"endpointControl\",\"reason\":"
                     "\"requestConflict\",\"revision\":1}";
  ASSERT_EQ(encode_envelope(envelope, encoded), DecodeResult::Ok);
  EXPECT_FALSE(endpoint_handoff_ingress_ack(encoded));
}

// commit入口拒绝不撤销已确认ready或关闭原媒体，显式后续意图仍使用真实修订。
TEST(EndpointHandoffClient, CommitIngressRejectionPreservesReadyAndRevision) {
  EndpointPublisher port;
  EndpointHandoffClient client(port, up, down, "commit-rejected");
  ASSERT_TRUE(client.bind("source", 100));
  ASSERT_TRUE(client.prepare("device:p:target", 101));
  ASSERT_TRUE(
      client.receive(down, fact(client.receipt(), "preparing", 1, true), 102));
  ASSERT_TRUE(
      client.receive(down, fact(client.receipt(), "ready", 2, false, 8), 103));
  ASSERT_TRUE(client.commit(104));
  const auto intent = client.receipt();
  ASSERT_TRUE(client.receive(down, ingress_rejection(intent), 105));
  EXPECT_FALSE(client.receipt().pending);
  EXPECT_FALSE(client.receipt().unknown);
  EXPECT_EQ(client.receipt().revision, 2U);
  EXPECT_EQ(client.receipt().state, "ready");
  EXPECT_EQ(client.receipt().handoff, intent.handoff);
  EXPECT_EQ(client.receipt().candidate, "candidate");
  EXPECT_TRUE(client.receipt().reason.empty());
  ASSERT_TRUE(client.abort(106));
}

TEST(EndpointHandoffClient,
     IngressRejectionDoesNotInventTerminalOrRegressNewIntent) {
  EndpointPublisher port;
  EndpointHandoffClient client(port, up, down, "rejected");
  ASSERT_TRUE(client.bind("source", 100));
  ASSERT_TRUE(client.prepare("device:p:target", 101));
  const auto original = client.receipt();
  const auto rejection = ingress_rejection(original);
  EXPECT_FALSE(client.receive("$thing/down/signaling/p/other", rejection, 102));
  EXPECT_FALSE(client.receive(down, ingress_rejection(original, 200), 102));
  EXPECT_FALSE(client.receive(
      down, ingress_rejection(original, 409, "requestDenied"), 102));
  EXPECT_FALSE(client.receive(
      down, ingress_rejection(original, 409, "requestConflict", "callControl"),
      102));
  ASSERT_TRUE(client.receive(down, rejection, 102));
  EXPECT_FALSE(client.receipt().pending);
  EXPECT_FALSE(client.receipt().unknown);
  EXPECT_EQ(client.receipt().code, 409);
  EXPECT_EQ(client.receipt().revision, 0U);
  EXPECT_TRUE(client.receipt().state.empty());
  EXPECT_TRUE(client.receipt().reason.empty());
  EXPECT_TRUE(client.receipt().handoff.empty());
  ASSERT_TRUE(client.prepare("device:p:target", 103));
  const auto next = client.receipt();
  EXPECT_NE(next.token, original.token);
  ASSERT_TRUE(client.receive(down, rejection, 104));
  EXPECT_EQ(client.receipt().token, next.token);
  EXPECT_EQ(client.receipt().handoff, next.handoff);
  EXPECT_TRUE(client.receipt().pending);
}

// 平台已报告ready就证明原prepare已经受理，迟到矛盾拒绝不能撤销该事实或打开新事务。
TEST(EndpointHandoffClient, RejectionCannotContradictObservedProgress) {
  EndpointPublisher port;
  EndpointHandoffClient client(port, up, down, "reject-progress");
  ASSERT_TRUE(client.bind("source", 100));
  ASSERT_TRUE(client.prepare("device:p:target", 101));
  const auto original = client.receipt();
  ASSERT_TRUE(client.receive(down, fact(original, "ready", 2, false, 8), 102));
  EXPECT_FALSE(client.receive(down, ingress_rejection(original), 103));
  EXPECT_EQ(client.receipt().state, "ready");
  EXPECT_TRUE(client.receipt().pending);
}

TEST(EndpointHandoffClient, ExplicitReadyCommitAndFixedOriginalIdentity) {
  EndpointPublisher port;
  EndpointHandoffClient client(port, up, down, "test");
  ASSERT_TRUE(client.bind("source", 100));
  EXPECT_FALSE(client.commit(101));
  ASSERT_TRUE(client.prepare("device:p:target", 102));
  const auto original = port.bodies.back();
  EXPECT_TRUE(client.receipt().pending);
  EXPECT_TRUE(client.receipt().state.empty());
  EXPECT_FALSE(client.prepare("device:p:other", 103));
  client.poll(10102);
  ASSERT_EQ(port.bodies.size(), 2U);
  EXPECT_EQ(original, port.bodies.back());
  const auto prepared = fact(client.receipt(), "preparing", 1, true);
  ASSERT_TRUE(client.receive(down, prepared, 10103));
  EXPECT_FALSE(client.receipt().pending);
  EXPECT_FALSE(client.commit(10104));
  const auto ready = fact(client.receipt(), "ready", 2, false, 8);
  ASSERT_TRUE(client.receive(down, ready, 10105));
  EXPECT_EQ(client.receipt().candidate, "candidate");
  ASSERT_TRUE(client.commit(10106));
  EXPECT_EQ(client.receipt().state, "ready") << "发布不是提交事实";
  const auto committed = fact(client.receipt(), "committed", 3, true);
  ASSERT_TRUE(client.receive(down, committed, 10107));
  EXPECT_EQ(client.receipt().state, "committed");
  EXPECT_FALSE(client.abort(10108));
  EXPECT_TRUE(client.receive(down, committed, 10109));
  EXPECT_EQ(port.bodies.size(), 3U);
  EXPECT_FALSE(client.receive(
      down, fact(client.receipt(), "ready", 4, false, 9), 10110));
}

TEST(EndpointHandoffClient, ReadyBeforePrepareAckDoesNotRegressActualFact) {
  EndpointPublisher port;
  EndpointHandoffClient client(port, up, down, "test");
  ASSERT_TRUE(client.bind("source", 100));
  ASSERT_TRUE(client.prepare("device:p:target", 101));
  const auto prepared = fact(client.receipt(), "preparing", 1, true);
  ASSERT_TRUE(
      client.receive(down, fact(client.receipt(), "ready", 2, false, 6), 102));
  EXPECT_TRUE(client.receipt().pending);
  ASSERT_TRUE(client.receive(down, prepared, 103));
  EXPECT_FALSE(client.receipt().pending);
  EXPECT_EQ(client.receipt().state, "ready");
  EXPECT_EQ(client.receipt().revision, 2U);
  ASSERT_TRUE(client.commit(104));
}

TEST(EndpointHandoffClient, UnknownTimeoutNeverSwitchesOrChangesToken) {
  EndpointPublisher port;
  EndpointHandoffClient client(port, up, down, "test");
  ASSERT_TRUE(client.bind("source", 100));
  ASSERT_TRUE(client.prepare("device:p:target", 101));
  const auto original = port.bodies.front();
  const auto token = client.receipt().token;
  client.poll(10101);
  client.poll(20101);
  client.poll(30101);
  EXPECT_TRUE(client.receipt().unknown);
  EXPECT_FALSE(client.receipt().pending);
  EXPECT_FALSE(client.prepare("device:p:target", 30102));
  EXPECT_FALSE(client.commit(30102));
  EXPECT_FALSE(client.abort(30102));
  EXPECT_EQ(client.receipt().token, token);
  ASSERT_EQ(port.bodies.size(), 3U);
  for (const auto &body : port.bodies)
    EXPECT_EQ(body, original);
  EXPECT_FALSE(client.receive(
      down, fact(client.receipt(), "preparing", 1, true), 30103));
  ASSERT_TRUE(client.receive(
      down, fact(client.receipt(), "failed", 2, false, 9, "handoffTimeout"),
      30104));
  EXPECT_FALSE(client.receipt().unknown);
  EXPECT_EQ(client.receipt().state, "failed");
}

TEST(EndpointHandoffClient,
     WrongIdentityConflictClockAndStaleSessionHaveNoSideEffects) {
  EndpointPublisher port;
  EndpointHandoffClient client(port, up, down, "test");
  ASSERT_TRUE(client.bind("source", 100));
  ASSERT_TRUE(client.prepare("device:p:target", 101));
  const auto prepared = fact(client.receipt(), "preparing", 1, true);
  EXPECT_FALSE(client.receive("$thing/down/signaling/p/other", prepared, 102));
  EXPECT_FALSE(client.receive(down, prepared, 99));
  ASSERT_TRUE(client.receive(down, prepared, 102));
  auto forged = client.receipt();
  forged.handoff = "other";
  EXPECT_FALSE(client.receive(down, fact(forged, "ready", 2, false, 3), 103));
  const auto ready = fact(client.receipt(), "ready", 2, false, 3);
  ASSERT_TRUE(client.receive(down, ready, 104));
  EXPECT_FALSE(client.receive(
      down, fact(client.receipt(), "committed", 3, false, 3), 105));
  EXPECT_EQ(client.receipt().state, "ready");
  client.close(106);
  EXPECT_FALSE(client.receive(down, ready, 107));
  EXPECT_TRUE(client.receipt().session.empty());
  ASSERT_TRUE(client.bind("next-source", 108));
  EXPECT_FALSE(client.receive(down, ready, 109));
  EXPECT_FALSE(client.prepare(std::string(129, 'd'), 110));
}

// 缺code的公开requestAck不能消费原pending；真实原拒绝仍可确认且重放不回退新意图。
// 原prepare总30秒窗口不能在晚到ready后被commit的新请求起点续长。
TEST(EndpointHandoffClient, OriginalHandoffDeadlineDoesNotRestartAtLateCommit) {
    EndpointPublisher publisher;
    EndpointHandoffClient client(publisher, up, down, "fixed-total");
    ASSERT_TRUE(client.bind("source", 100));
    ASSERT_TRUE(client.prepare("device:p:target", 101));
    ASSERT_TRUE(client.receive(down, fact(client.receipt(), "preparing", 1, true), 102));
    ASSERT_TRUE(client.receive(down, fact(client.receipt(), "ready", 2, false, 8), 30000));
    const auto published = publisher.bodies.size();
    EXPECT_FALSE(client.commit(30101));
    EXPECT_EQ(publisher.bodies.size(), published);
    client.poll(30101);
    EXPECT_TRUE(client.receipt().unknown);
    EXPECT_FALSE(client.abort(30102));
    EXPECT_FALSE(client.prepare("device:p:new", 30103));
}

// 只读预检与实际消费共用唯一合同；预检次数不推进事实、发布或刷新时钟/期限。
TEST(EndpointHandoffClient, ReadonlyPreflightMatchesActualAdmissionIncludingHistoricalAck) {
    EndpointPublisher publisher;
    EndpointHandoffClient client(publisher, up, down, "preflight");
    ASSERT_TRUE(client.bind("source", 100));
    ASSERT_TRUE(client.prepare("device:p:target", 101));
    const auto ack = fact(client.receipt(), "preparing", 1, true);
    const auto ready = fact(client.receipt(), "ready", 2, false, 8);
    const auto before = client.receipt();
    ASSERT_TRUE(client.can_receive(down, ready, 102));
    ASSERT_TRUE(client.can_receive(down, ready, 10000));
    EXPECT_EQ(client.receipt().revision, before.revision);
    EXPECT_EQ(client.receipt().pending, before.pending);
    EXPECT_EQ(publisher.bodies.size(), 1U);
    ASSERT_TRUE(client.receive(down, ready, 103));  // 预检不把10000写成时钟水位。
    ASSERT_TRUE(client.can_receive(down, ack, 104));
    ASSERT_TRUE(client.receive(down, ack, 104));
    EXPECT_EQ(client.receipt().state, "ready");
    EXPECT_EQ(client.receipt().revision, 2U);
    ASSERT_TRUE(client.can_receive(down, ack, 105));
    ASSERT_TRUE(client.receive(down, ack, 105));
    Envelope changed;
    ASSERT_EQ(decode_envelope(ready, changed), DecodeResult::Ok);
    changed.token = "another-token-same-sequence";
    std::string conflict;
    ASSERT_EQ(encode_envelope(changed, conflict), DecodeResult::Ok);
    EXPECT_FALSE(client.can_receive(down, conflict, 106));
    EXPECT_FALSE(client.receive(down, conflict, 106));
    changed.token = "event-8";
    changed.payload.replace(changed.payload.find("candidate"), 9, "different");
    ASSERT_EQ(encode_envelope(changed, conflict), DecodeResult::Ok);
    EXPECT_FALSE(client.can_receive(down, conflict, 107));
    EXPECT_FALSE(client.receive(down, conflict, 107));
    ASSERT_TRUE(client.commit(108));
    const auto actual = fact(client.receipt(), "committed", 3, true);
    ASSERT_EQ(decode_envelope(actual, changed), DecodeResult::Ok);
    const auto candidate = changed.payload.find("\"candidate\"");
    ASSERT_NE(candidate, std::string::npos);
    changed.payload.replace(candidate, 11, "\"other-leg\"");
    ASSERT_EQ(encode_envelope(changed, conflict), DecodeResult::Ok);
    EXPECT_FALSE(client.can_receive(down, conflict, 109));
    EXPECT_FALSE(client.receive(down, conflict, 109));
    ASSERT_TRUE(client.can_receive(down, actual, 110));
    ASSERT_TRUE(client.receive(down, actual, 110));
    ASSERT_TRUE(client.can_receive(down, actual, 111));
    ASSERT_TRUE(client.receive(down, actual, 111));
    EXPECT_FALSE(client.can_receive(down, fact(client.receipt(), "ready", 4, false, 9), 112));
    EXPECT_FALSE(client.receive(down, fact(client.receipt(), "ready", 4, false, 9), 112));
}

// 容量耗尽不驱逐原幂等事实；预检与消费拒绝同一新帧，旧ACK仍可原样重放。
TEST(EndpointHandoffClient, ReadonlyPreflightCapacityKeepsOriginalFacts) {
    EndpointPublisher publisher;
    EndpointHandoffClient client(publisher, up, down, "bounded");
    ASSERT_TRUE(client.bind("source", 100));
    ASSERT_TRUE(client.prepare("device:p:target", 101));
    const auto ack = fact(client.receipt(), "preparing", 1, true);
    ASSERT_TRUE(client.receive(down, ack, 102));
    unsigned accepted = 0;
    for (unsigned sequence = 1; sequence <= 128; ++sequence) {
        const auto next = fact(client.receipt(), "preparing", 1, false, sequence);
        const auto allowed = client.can_receive(down, next, 103 + sequence);
        EXPECT_EQ(client.receive(down, next, 103 + sequence), allowed);
        if (!allowed) break;
        ++accepted;
    }
    EXPECT_GT(accepted, 0U);
    EXPECT_LT(accepted, 128U);
    EXPECT_TRUE(client.can_receive(down, ack, 300));
    EXPECT_TRUE(client.receive(down, ack, 300));
    EXPECT_EQ(client.receipt().state, "preparing");
    EXPECT_EQ(client.receipt().revision, 1U);
    EXPECT_EQ(publisher.bodies.size(), 1U);
}

TEST(EndpointHandoffClient, MissingIngressCodeKeepsOriginalPendingAndAllowsExactRejection) {
  EndpointPublisher port;
  EndpointHandoffClient client(port, up, down, "missing-code");
  ASSERT_TRUE(client.bind("source", 100));
  ASSERT_TRUE(client.prepare("device:p:target", 101));
  const auto original = client.receipt();
  Envelope missing;
  ASSERT_EQ(decode_envelope(ingress_rejection(original), missing), DecodeResult::Ok);
  missing.code.reset();
  std::string body;
  ASSERT_EQ(encode_envelope(missing, body), DecodeResult::Ok);
  EXPECT_FALSE(endpoint_handoff_ingress_ack(body));
  EXPECT_FALSE(client.receive(down, body, 102));
  EXPECT_EQ(client.receipt().token, original.token);
  EXPECT_EQ(client.receipt().handoff, original.handoff);
  EXPECT_EQ(client.receipt().revision, original.revision);
  EXPECT_EQ(client.receipt().code, original.code);
  EXPECT_TRUE(client.receipt().pending);
  EXPECT_FALSE(client.receipt().unknown);
  EXPECT_EQ(port.bodies.size(), 1U);
  const auto rejection = ingress_rejection(original);
  ASSERT_TRUE(client.receive(down, rejection, 103));
  EXPECT_EQ(client.receipt().code, 409);
  EXPECT_FALSE(client.receipt().pending);
  ASSERT_TRUE(client.receive(down, rejection, 104));
  EXPECT_EQ(client.receipt().code, 409);
}
} // namespace
} // namespace ur::device
