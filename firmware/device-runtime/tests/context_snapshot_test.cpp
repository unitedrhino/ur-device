// 文件说明：私有上下文严格wire、完整留存和截止/幂等回归；合成文本不代替实际模型或端云验收。
#include "ur/device/context_snapshot.hpp"
#include <gtest/gtest.h>
#include <functional>
#include <utility>

namespace ur::device {
namespace {
// snapshot_envelope 构造完整两消息一轮，AI来源与人工接收腿分别标识。
Envelope snapshot_envelope() {
    Envelope value;  // 唯一原投递及水位，不在重投中换身份。
    value.request = false;
    value.token = std::string(64, 'a');
    value.method = "contextSnapshot";
    value.session = "human-leg";
    value.message_sequence = 7;
    value.payload =
        R"({"deliveryId":")" + value.token + R"(","consentId":")" + std::string(64, 'b') +
        R"(","scope":"completedText","sourceSessionId":"ai-source","serviceId":"support","serviceRevision":"r1","snapshotVersion":"v1","snapshotDigest":")" +
        std::string(64, 'c') +
        R"(","firstTurnIndex":1,"lastTurnIndex":1,"expiresAt":2000,"turns":[{"messageId":"u1","sequence":1,"turnIndex":1,"role":"user","text":"你好\n人工"},{"messageId":"a1","sequence":2,"turnIndex":1,"role":"assistant","text":"正在转接"}]})";
    return value;
}
// wire
// 使用正式编解码，非法公共结构由单独raw用例验证，不用绕过生产编码器造成功。
std::string wire(const Envelope& value) {
    std::string result;  // 完整独立可解析信封。
    EXPECT_EQ(encode_envelope(value, result), DecodeResult::Ok);
    return result;
}
// replace
// 在已知完整夹具精确替换字段，用例缺前提必须失败，不能悄悄测试原合法数据。
void replace(std::string& value, std::string_view from, std::string_view to) {
    const auto position = value.find(from);  // 原夹具唯一变异位置。
    ASSERT_NE(position, std::string::npos);
    value.replace(position, from.size(), to);
}
// Mutation 每个命名例仅变一个字段，确保失败源于严格契约而非其他预先非法条件。
using Mutation = std::pair<const char*, std::function<void(Envelope&)>>;
// invalid_snapshots
// 覆盖字段、枚举、范围、顺序与嵌套权限扩张，不改变公共解析判据。
std::vector<Mutation> invalid_snapshots() {
    return {
        {"request",
         [](auto& e) {
             e.request = true;
             e.message_sequence.reset();
         }},
        {"code", [](auto& e) { e.code = 200; }},
        {"eventWatermark",
         [](auto& e) {
             e.message_sequence.reset();
             e.event_sequence = 7;
         }},
        {"missingWatermark", [](auto& e) { e.message_sequence.reset(); }},
        {"sessionTooLong", [](auto& e) { e.session = std::string(65, 'h'); }},
        {"missingSession", [](auto& e) { e.session.clear(); }},
        {"method", [](auto& e) { e.method = "respTextDone"; }},
        {"tokenMismatch", [](auto& e) { e.token = std::string(64, 'd'); }},
        {"deliveryDigest",
         [](auto& e) { replace(e.payload, std::string(64, 'a'), std::string(64, 'A')); }},
        {"consentDigest",
         [](auto& e) { replace(e.payload, std::string(64, 'b'), "not-a-digest"); }},
        {"snapshotDigest",
         [](auto& e) { replace(e.payload, std::string(64, 'c'), std::string(64, 'g')); }},
        {"wrongScope", [](auto& e) { replace(e.payload, "completedText", "allContext"); }},
        {"serviceWildcard", [](auto& e) { replace(e.payload, "support", "*"); }},
        {"serviceSlash", [](auto& e) { replace(e.payload, "support", "group/support"); }},
        {"serviceLeadingSpace", [](auto& e) { replace(e.payload, "support", " support"); }},
        {"revisionTrailingUnicodeSpace", [](auto& e) { replace(e.payload, "\"r1\"", "\"r1　\""); }},
        {"revisionWildcard", [](auto& e) { replace(e.payload, "\"r1\"", "\"r+\""); }},
        {"sourceTooLong", [](auto& e) { replace(e.payload, "ai-source", std::string(65, 'x')); }},
        {"snapshotVersionEmpty", [](auto& e) { replace(e.payload, "\"v1\"", "\"\""); }},
        {"identifierControl", [](auto& e) { replace(e.payload, "\"u1\"", "\"u\\n1\""); }},
        {"firstZero",
         [](auto& e) { replace(e.payload, "\"firstTurnIndex\":1", "\"firstTurnIndex\":0"); }},
        {"lastBeforeFirst",
         [](auto& e) { replace(e.payload, "\"firstTurnIndex\":1", "\"firstTurnIndex\":2"); }},
        {"moreThan32Turns",
         [](auto& e) { replace(e.payload, "\"lastTurnIndex\":1", "\"lastTurnIndex\":33"); }},
        {"turnIndexOverflow",
         [](auto& e) {
             replace(e.payload, "\"lastTurnIndex\":1", "\"lastTurnIndex\":2147483648");
         }},
        {"expiresZero",
         [](auto& e) { replace(e.payload, "\"expiresAt\":2000", "\"expiresAt\":0"); }},
        {"expiresFraction",
         [](auto& e) { replace(e.payload, "\"expiresAt\":2000", "\"expiresAt\":2000.5"); }},
        {"expiresUnsafe",
         [](auto& e) {
             replace(e.payload, "\"expiresAt\":2000", "\"expiresAt\":9007199254740992");
         }},
        {"expiresType",
         [](auto& e) { replace(e.payload, "\"expiresAt\":2000", "\"expiresAt\":\"2000\""); }},
        {"firstAssistant",
         [](auto& e) { replace(e.payload, "\"role\":\"user\"", "\"role\":\"assistant\""); }},
        {"invalidRole",
         [](auto& e) { replace(e.payload, "\"role\":\"user\"", "\"role\":\"system\""); }},
        {"repeatedMessage", [](auto& e) { replace(e.payload, "\"a1\"", "\"u1\""); }},
        {"repeatedSequence",
         [](auto& e) { replace(e.payload, "\"sequence\":2", "\"sequence\":1"); }},
        {"sequenceZero", [](auto& e) { replace(e.payload, "\"sequence\":1", "\"sequence\":0"); }},
        {"assistantWrongTurn",
         [](auto& e) {
             replace(e.payload, "\"sequence\":2,\"turnIndex\":1", "\"sequence\":2,\"turnIndex\":2");
         }},
        {"userRepeatedTurn",
         [](auto& e) { replace(e.payload, "\"role\":\"assistant\"", "\"role\":\"user\""); }},
        {"blankUnicode", [](auto& e) { replace(e.payload, "正在转接", "\u3000\u00a0\u2003"); }},
        {"blankAscii", [](auto& e) { replace(e.payload, "正在转接", " \\n\\t"); }},
        {"textTooLong", [](auto& e) { replace(e.payload, "正在转接", std::string(16385, 'x')); }},
        {"unknownRootKey",
         [](auto& e) { replace(e.payload, "\"scope\":", "\"ownerId\":1,\"scope\":"); }},
        {"unknownTurnKey",
         [](auto& e) {
             replace(e.payload, "\"messageId\":\"u1\"", "\"completed\":true,\"messageId\":\"u1\"");
         }},
        {"missingRootKey", [](auto& e) { replace(e.payload, "\"snapshotVersion\":\"v1\",", ""); }},
        {"missingTurnKey", [](auto& e) { replace(e.payload, "\"sequence\":2,", ""); }},
        {"wrongCase", [](auto& e) { replace(e.payload, "snapshotVersion", "SnapshotVersion"); }}};
}
// InvalidSnapshot 每条畸形JSON分别执行，不以单一“不崩溃”代替字段拒绝证据。
class InvalidSnapshot : public ::testing::TestWithParam<Mutation> {};
TEST_P(InvalidSnapshot, RejectsAndClearsAllPrivateFields) {
    auto value = snapshot_envelope();
    GetParam().second(value);
    ContextSnapshot out;
    out.consent_id = "old-private";
    out.turns.push_back({"old", 1, 1, "user", "旧私有内容"});
    std::string body;
    if (std::string_view(GetParam().first) == "expiresUnsafe") {
        // 公共编码层已拒不安全整数，接收负例只能变异原合法wire，不能让生成器绕过校验。
        body = wire(snapshot_envelope());
        replace(body, "\"expiresAt\":2000", "\"expiresAt\":9007199254740992");
    } else {
        body = wire(value);
    }
    EXPECT_EQ(decode_context_snapshot(WireRoute::Message, body, out), WireResult::Invalid);
    EXPECT_TRUE(out.consent_id.empty());
    EXPECT_TRUE(out.turns.empty());
}
INSTANTIATE_TEST_SUITE_P(Contract, InvalidSnapshot, ::testing::ValuesIn(invalid_snapshots()),
                         [](const auto& info) { return info.param.first; });
}  // namespace

TEST(ContextSnapshotWire, FullTextRoundTripAndAckContainsNoPrivateText) {
    ContextSnapshot snapshot;
    ASSERT_EQ(decode_context_snapshot(WireRoute::Message, wire(snapshot_envelope()), snapshot),
              WireResult::Ok);
    ASSERT_EQ(snapshot.turns.size(), 2U);
    EXPECT_EQ(snapshot.turns[0].text, "你好\n人工");
    EXPECT_EQ(snapshot.source_session_id, "ai-source");
    EXPECT_EQ(snapshot.envelope.session, "human-leg");
    Envelope ack;
    ASSERT_EQ(context_snapshot_ack(snapshot, ack), WireResult::Ok);
    ContextSnapshotAck decoded;
    const auto encoded = wire(ack);
    ASSERT_EQ(decode_context_snapshot_ack(WireRoute::Message, encoded, decoded), WireResult::Ok);
    EXPECT_EQ(decoded.snapshot_digest, snapshot.snapshot_digest);
    EXPECT_EQ(encoded.find("你好"), std::string::npos);
    EXPECT_FALSE(ack.message_sequence);
    EXPECT_FALSE(ack.event_sequence);
}
TEST(ContextSnapshotWire, DirectoryLiteralQuestionMarkIsNotAQueryWildcard) {
    auto value = snapshot_envelope();
    replace(value.payload, "support", "support?");
    replace(value.payload, "\"r1\"", "\"r?1\"");
    ContextSnapshot snapshot;
    ASSERT_EQ(decode_context_snapshot(WireRoute::Message, wire(value), snapshot), WireResult::Ok);
    EXPECT_EQ(snapshot.service_id, "support?");
    EXPECT_EQ(snapshot.service_revision, "r?1");
}
TEST(ContextSnapshotWire, WrongRouteDuplicateKeyInvalidUtf8AndOversizeFailClosed) {
    ContextSnapshot snapshot;
    EXPECT_EQ(decode_context_snapshot(WireRoute::Signaling, wire(snapshot_envelope()), snapshot),
              WireResult::WrongRoute);
    auto body = wire(snapshot_envelope());
    replace(body, "\"scope\":", "\"scope\":\"completedText\",\"scope\":");
    EXPECT_EQ(decode_context_snapshot(WireRoute::Message, body, snapshot), WireResult::Invalid);
    body = wire(snapshot_envelope());
    replace(body, "正在转接", std::string(1, static_cast<char>(0xff)));
    EXPECT_EQ(decode_context_snapshot(WireRoute::Message, body, snapshot), WireResult::Invalid);
    EXPECT_EQ(decode_context_snapshot(WireRoute::Message, std::string(65537, 'x'), snapshot),
              WireResult::Invalid);
    EXPECT_TRUE(snapshot.turns.empty());
}
TEST(ContextSnapshotWire, UserOnlyCompletedTurnsAreAllowedButEmptyOr65MessagesAreNot) {
    auto value = snapshot_envelope();
    const auto first = value.payload.find(",{\"messageId\":\"a1\"");
    ASSERT_NE(first, std::string::npos);
    value.payload.erase(first, value.payload.size() - first - 2);
    ContextSnapshot snapshot;
    ASSERT_EQ(decode_context_snapshot(WireRoute::Message, wire(value), snapshot), WireResult::Ok);
    ASSERT_EQ(snapshot.turns.size(), 1U);
    replace(value.payload, "\"lastTurnIndex\":1", "\"lastTurnIndex\":2");
    const auto suffix = value.payload.size() - 2;
    value.payload.insert(
        suffix, R"(,{"messageId":"u2","sequence":2,"turnIndex":2,"role":"user","text":"第二轮"})");
    EXPECT_EQ(decode_context_snapshot(WireRoute::Message, wire(value), snapshot), WireResult::Ok);
    auto empty = snapshot_envelope();
    empty.payload.erase(empty.payload.find("\"turns\":") + 9);
    empty.payload += "]}";
    EXPECT_EQ(decode_context_snapshot(WireRoute::Message, wire(empty), snapshot),
              WireResult::Invalid);
    auto many = snapshot_envelope();
    many.payload.erase(many.payload.find("\"turns\":") + 9);
    for (unsigned n = 0; n < 65; ++n) {
        if (n)
            many.payload += ',';
        many.payload += R"({"messageId":"m)" + std::to_string(n) + R"(","sequence":)" +
                        std::to_string(n + 1) + R"(,"turnIndex":1,"role":"user","text":"x"})";
    }
    many.payload += "]}";
    EXPECT_EQ(decode_context_snapshot(WireRoute::Message, wire(many), snapshot),
              WireResult::Invalid);
}
TEST(ContextSnapshotWire, AckRejectsEveryAlteredIdentityShapeOrStatus) {
    ContextSnapshot snapshot;
    ASSERT_EQ(decode_context_snapshot(WireRoute::Message, wire(snapshot_envelope()), snapshot),
              WireResult::Ok);
    Envelope original;
    ASSERT_EQ(context_snapshot_ack(snapshot, original), WireResult::Ok);
    const std::vector<Mutation> invalid = {
        {"code", [](auto& e) { e.code = 202; }},
        {"noCode", [](auto& e) { e.code.reset(); }},
        {"seq", [](auto& e) { e.message_sequence = 1; }},
        {"eventSeq", [](auto& e) { e.event_sequence = 1; }},
        {"method", [](auto& e) { e.method = "requestAck"; }},
        {"token", [](auto& e) { e.token = "unrelated"; }},
        {"noSession", [](auto& e) { e.session.clear(); }},
        {"sessionLength", [](auto& e) { e.session = std::string(65, 'x'); }},
        {"extra",
         [](auto& e) { replace(e.payload, "\"status\":", "\"text\":\"private\",\"status\":"); }},
        {"status", [](auto& e) { replace(e.payload, "received", "read"); }},
        {"consent", [](auto& e) { replace(e.payload, std::string(64, 'b'), "other"); }},
        {"digest", [](auto& e) { replace(e.payload, std::string(64, 'c'), "other"); }},
        {"version", [](auto& e) { replace(e.payload, "\"v1\"", "\"\""); }}};
    ContextSnapshotAck out;
    for (const auto& mutation : invalid) {
        SCOPED_TRACE(mutation.first);
        auto changed = original;
        mutation.second(changed);
        EXPECT_EQ(decode_context_snapshot_ack(WireRoute::Message, wire(changed), out),
                  WireResult::Invalid);
        EXPECT_TRUE(out.delivery_id.empty());
    }
    EXPECT_EQ(decode_context_snapshot_ack(WireRoute::Signaling, wire(original), out),
              WireResult::WrongRoute);
    auto invalid_params = wire(original);
    replace(invalid_params, "\"data\":", "\"params\":");
    EXPECT_EQ(decode_context_snapshot_ack(WireRoute::Message, invalid_params, out),
              WireResult::Invalid);
    snapshot.envelope.payload = "{}";
    EXPECT_EQ(context_snapshot_ack(snapshot, original), WireResult::Invalid);
    EXPECT_TRUE(original.token.empty());
}
TEST(ContextSnapshotInbox, RetainsFullTextBeforeAckAndReplaysOnlySameImmutableMessage) {
    ContextSnapshotInbox inbox;
    const auto body = wire(snapshot_envelope());
    Envelope ack;
    ASSERT_TRUE(inbox.receive(body, "human-leg", true, false, 1000, ack));
    ASSERT_TRUE(inbox.snapshot());
    ASSERT_EQ(inbox.snapshot()->turns.size(), 2U);
    EXPECT_EQ(inbox.snapshot()->turns[1].text, "正在转接");
    const auto original = wire(ack);
    ASSERT_TRUE(inbox.receive(body, "human-leg", true, false, 1001, ack));
    EXPECT_EQ(wire(ack), original);
    auto changed = snapshot_envelope();
    replace(changed.payload, "正在转接", "被替换正文");
    EXPECT_FALSE(inbox.receive(wire(changed), "human-leg", true, false, 1002, ack));
    EXPECT_TRUE(ack.token.empty());
    EXPECT_EQ(inbox.snapshot()->turns[1].text, "正在转接");
    changed = snapshot_envelope();
    changed.token = std::string(64, 'd');
    replace(changed.payload, std::string(64, 'a'), changed.token);
    changed.message_sequence = 8;
    EXPECT_FALSE(inbox.receive(wire(changed), "human-leg", true, false, 1003, ack));
}
TEST(ContextSnapshotInbox, InvalidStateExpiryAndClockRollbackNeverAcknowledgeOrExtend) {
    ContextSnapshotInbox inbox;
    const auto body = wire(snapshot_envelope());
    Envelope ack;
    EXPECT_FALSE(inbox.receive(body, "other-leg", true, false, 1000, ack));
    EXPECT_FALSE(inbox.receive(body, "human-leg", false, false, 1000, ack));
    EXPECT_FALSE(inbox.receive(body, "human-leg", true, true, 1000, ack));
    EXPECT_FALSE(inbox.receive(body, "human-leg", true, false, 0, ack));
    EXPECT_FALSE(inbox.snapshot());
    ASSERT_TRUE(inbox.receive(body, "human-leg", true, false, 1001, ack));
    EXPECT_FALSE(inbox.receive(body, "human-leg", true, false, 1000, ack));
    EXPECT_FALSE(inbox.receive(body, "human-leg", true, true, 1002, ack));
    EXPECT_TRUE(inbox.snapshot());
    inbox.expire(1999);
    EXPECT_TRUE(inbox.snapshot());
    inbox.expire(2000);
    EXPECT_FALSE(inbox.snapshot());
    EXPECT_FALSE(inbox.receive(body, "human-leg", true, false, 1999, ack));
    auto renewed = snapshot_envelope();
    replace(renewed.payload, "\"expiresAt\":2000", "\"expiresAt\":3000");
    EXPECT_FALSE(inbox.receive(wire(renewed), "human-leg", true, false, 2001, ack));
    renewed.message_sequence = 8;
    replace(renewed.payload, "正在转接", "迟到替换正文");
    EXPECT_FALSE(inbox.receive(wire(renewed), "human-leg", true, false, 2002, ack));
    EXPECT_FALSE(inbox.snapshot());
    EXPECT_TRUE(ack.token.empty());
    inbox.clear();
    ASSERT_FALSE(inbox.snapshot());
    EXPECT_TRUE(inbox.receive(wire(renewed), "human-leg", true, false, 2001, ack));
}
}  // namespace ur::device
