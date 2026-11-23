// 文件说明：人际端点候选必须沿正式邀请出处接听；本离线解析回归不授实际焦点或媒体权限。
#include <cJSON.h>
#include <gtest/gtest.h>
#include <memory>
#include "ur/device/human_call.hpp"

namespace ur::device {
namespace {
// CandidateNetwork 只保存真实上行编码，连接由原宿主管理，不伪造平台应答。
struct CandidateNetwork final : MqttPort {
    std::vector<Envelope> sent; // 原上行以完整信封留存，测试应答只引用真实请求。
    bool connect(const MqttConfig&) override { return false; }
    int subscribe(std::string_view) override { return 0; }
    bool publish(std::string_view, std::string_view bytes) override {
        Envelope value;
        if (decode_envelope(bytes, value) != DecodeResult::Ok) return false;
        sent.push_back(std::move(value));
        return true;
    }
    bool poll() override { return true; }
    std::optional<MqttEvent> take_event() override { return {}; }
    void disconnect() override {}
};
// CandidateMedia 在本人接听前不得开始媒体；不提供任何实际Focus授权。
struct CandidateMedia final : CallMediaPort {
    unsigned prepares = 0;
    bool ready = false; // 仅驱动连通事实，完整Focus仍由独立可信宿主核验。
    bool voice_supported() const override { return true; }
    bool prepare_voice(const CallVoiceProfile&) override { ++prepares; return true; }
    bool begin_voice_candidate(const CallVoiceProfile&) override { return true; }
    bool begin() override { return false; }
    bool apply_answer(std::string_view) override { return true; }
    bool connected() const override { return ready; }
    void close() override { ready = false; }
};
// config 使用原Voice借用模式；来源门必须由本机可信宿主配置，而非网页报文。
HumanCallConfig candidate_config() {
    HumanCallConfig config;
    config.mode = HumanCallMode::Voice;
    config.borrowed_connection = true;
    return config;
}

// HumanCandidate 原出处、请求和事件均走真实编码器；权限布尔仅Core单测边界，不冒充HAL验收。
class HumanCandidate : public ::testing::Test {
protected:
    CandidateNetwork network;
    CandidateMedia media;
    HumanCallClient client{network, media, "p/d", "candidate", candidate_config()};
    bool permitted = false;
    std::uint64_t now = 100;
    std::string session = "candidate-human", handoff = "original-handoff";
    // invite 保留每个候选自己的平台冻结出处，不从committed反推来源。
    Envelope invite() const {
        Envelope value;
        value.method = "sessionInvite";
        value.token = "original-invite";
        value.session = session;
        value.event_sequence = 1;
        value.payload = R"({"purpose":"humanCall","callType":"voice","admission":"accept","transport":"webrtc","expiresAt":1001000,"callGroupId":"original-group","tracks":[{"trackId":"voice","kind":"audio","sourceId":"microphone","sourceType":"microphone","direction":"sendrecv","codec":"opus","state":"requested"}],"handoff":{"sourceSessionId":"source-human","handoffId":")" + handoff + "\"}}";
        return value;
    }
    // receive 不改变任何私有状态，失败不得借外部setter修订会话或事务水位。
    bool receive(const Envelope& value) {
        std::string bytes;
        return encode_envelope(value, bytes) == DecodeResult::Ok &&
               client.receive_shared("$thing/down/signaling/p/d", bytes, ++now, 1000000);
    }
    // ack 为真实已发送请求生成最小回执，不能预先伪造本地接听。
    Envelope ack(const Envelope& request) const {
        Envelope value;
        value.request = false;
        value.method = "requestAck";
        value.token = request.token;
        value.session = session;
        value.code = 200;
        value.payload = "{\"requestMethod\":\"" + request.method + "\",\"result\":{}}";
        return value;
    }
    void SetUp() override {
        ASSERT_TRUE(client.set_endpoint_source([this](auto current, auto, auto, auto) {
            return permitted && current == session;
        }));
        ASSERT_TRUE(client.start_shared(now));
    }
    // connected 依本人接听、原SDP、驱动连通、mediaState回执及Established完成，不授Focus。
    void connected() {
        ASSERT_TRUE(receive(invite()));
        ASSERT_TRUE(client.accept(++now));
        ASSERT_TRUE(receive(ack(network.sent.back())));
        ASSERT_TRUE(client.offer_ready("v=0\r\ns=voice\r\n", ++now));
        auto answer = ack(network.sent.back());
        answer.method = "mediaAnswer";
        answer.payload = R"({"mediaRevision":1,"webrtc":{"iceMode":"complete","sdp":"v=0\r\ns=answer\r\n"}})";
        ASSERT_TRUE(receive(answer));
        media.ready = true;
        ASSERT_TRUE(client.media_connected(++now));
        ASSERT_TRUE(receive(ack(network.sent.back())));
        Envelope established;
        established.request = false;
        established.method = "sessionEstablished";
        established.token = "original-established";
        established.session = session;
        established.event_sequence = 2;
        established.payload = R"({"state":"established","mediaRevision":1})";
        ASSERT_TRUE(receive(established));
        ASSERT_TRUE(client.active());
    }
    // committed 只有同邀请handoff、候选腿和原共享水位可赋予私有继承来源。
    Envelope committed(unsigned revision = 3) const {
        Envelope value;
        value.request = false;
        value.method = "endpointState";
        value.token = "original-committed";
        value.session = session;
        value.event_sequence = 3;
        value.payload = "{\"state\":\"committed\",\"revision\":" + std::to_string(revision) +
                        ",\"handoffId\":\"" + handoff + "\",\"candidateSessionId\":\"" + session + "\"}";
        return value;
    }
    // last_control 只读正式新源实际发出的CAS，不修改事务内部状态。
    EndpointCommand last_control() const {
        EndpointCommand result;
        std::string bytes;
        EXPECT_EQ(encode_envelope(network.sent.back(), bytes), DecodeResult::Ok);
        EXPECT_EQ(decode_endpoint_control(WireRoute::Signaling, bytes, result), WireResult::Ok);
        return result;
    }
};

TEST_F(HumanCandidate, ConnectedAndCommittedStillRequireCurrentWholeScopeBeforeInheritance) {
    connected();
    EXPECT_FALSE(client.prepare_endpoint_source("device:p:next", ++now, 1000000));
    EXPECT_FALSE(receive(committed()));
    permitted = true;
    ASSERT_TRUE(receive(committed()));
    ASSERT_TRUE(client.prepare_endpoint_source("device:p:next", ++now, 1000000));
    EXPECT_EQ(last_control().expected_revision, 3U);
    EXPECT_NE(last_control().handoff_id, handoff);
    EXPECT_TRUE(receive(committed())); // 精确历史字节不能回退新事务revision或清其pending。
    EXPECT_TRUE(client.endpoint_source_receipt()->pending);
}

TEST_F(HumanCandidate, WrongHandoffCandidateSessionSequenceAndCanonicalBytesNeverGrantSource) {
    connected();
    permitted = true;
    auto value = committed();
    value.payload.replace(value.payload.find(handoff), handoff.size(), "wrong-handoff");
    EXPECT_FALSE(receive(value));
    value = committed();
    value.session = "foreign-session";
    EXPECT_FALSE(receive(value));
    value = committed();
    value.payload.replace(value.payload.rfind(session), session.size(), "foreign-candidate");
    EXPECT_FALSE(receive(value));
    value = committed();
    value.event_sequence = 4;
    EXPECT_FALSE(receive(value));
    EXPECT_FALSE(client.prepare_endpoint_source("device:p:next", ++now, 1000000));
    ASSERT_TRUE(receive(committed()));
    value = committed();
    value.token = "unrequested-future";
    value.event_sequence = 4;
    value.payload.replace(value.payload.find(handoff), handoff.size(), "unrequested-handoff");
    EXPECT_FALSE(receive(value)); // 已成为新源也不能被动接纳并非本人创建的后继事务。
    value = committed(4);
    EXPECT_FALSE(receive(value)); // 相同token/事件序号不同字节不能改变继承CAS。
    value = committed();
    value.token = "forged-second-commit";
    value.event_sequence = 4;
    EXPECT_FALSE(receive(value));
    ASSERT_TRUE(client.prepare_endpoint_source("device:p:next", ++now, 1000000));
    EXPECT_EQ(last_control().expected_revision, 3U);
}

// 自指来源无法成为新邀请授权，完整解析拒绝后不产生任何媒体。
TEST_F(HumanCandidate, MalformedOriginNeverRingsOrAllocatesCandidateMedia) {
    auto value = invite();
    value.payload.replace(value.payload.find("\"source-human\""), 14, "\"candidate-human\"");
    EXPECT_FALSE(receive(value));
    EXPECT_NE(client.state(), HumanCallState::Ringing);
    EXPECT_EQ(media.prepares, 0U);
    EXPECT_FALSE(client.endpoint_source_available(++now, 1000000));
}

// 每个畸形出处独立实例，避免前例FailClosed掩盖后例解析；旧七字段SDK不接受新八字段。
TEST_F(HumanCandidate, StrictOriginShapeMatrixAndLegacySevenFieldCompatibility) {
    const std::vector<std::string> origins = {
        "null", "[]", "{}",
        R"({"sourceSessionId":"","handoffId":"original-handoff"})",
        R"({"sourceSessionId":"source-human","handoffId":""})",
        R"({"sourceSessionId":42,"handoffId":"original-handoff"})",
        R"({"sourceSessionId":"source-human","handoffId":"original-handoff","extra":true})",
        "{\"sourceSessionId\":\"" + std::string(65, 's') + "\",\"handoffId\":\"original-handoff\"}",
        "{\"sourceSessionId\":\"source-human\",\"handoffId\":\"" + std::string(129, 'h') + "\"}"
    };
    for (const auto& origin : origins) {
        CandidateNetwork local_network;
        CandidateMedia local_media;
        HumanCallClient local(local_network, local_media, "p/d", "isolated", candidate_config());
        ASSERT_TRUE(local.set_endpoint_source([](auto, auto, auto, auto) { return false; }));
        ASSERT_TRUE(local.start_shared(100));
        auto value = invite();
        std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_Parse(value.payload.c_str()), cJSON_Delete);
        ASSERT_NE(root, nullptr);
        cJSON_DeleteItemFromObjectCaseSensitive(root.get(), "handoff");
        ASSERT_TRUE(cJSON_AddItemToObject(root.get(), "handoff", cJSON_Parse(origin.c_str())));
        char* encoded = cJSON_PrintUnformatted(root.get());
        ASSERT_NE(encoded, nullptr);
        value.payload = encoded;
        cJSON_free(encoded);
        std::string bytes;
        ASSERT_EQ(encode_envelope(value, bytes), DecodeResult::Ok);
        EXPECT_FALSE(local.receive_shared("$thing/down/signaling/p/d", bytes, 101, 1000000)) << origin;
        EXPECT_NE(local.state(), HumanCallState::Ringing);
        EXPECT_EQ(local_media.prepares, 0U);
    }
    CandidateNetwork legacy_network;
    CandidateMedia legacy_media;
    HumanCallClient legacy(legacy_network, legacy_media, "p/d", "legacy", candidate_config());
    ASSERT_TRUE(legacy.start_shared(100));
    std::string bytes;
    ASSERT_EQ(encode_envelope(invite(), bytes), DecodeResult::Ok);
    EXPECT_FALSE(legacy.receive_shared("$thing/down/signaling/p/d", bytes, 101, 1000000));
    auto plain = invite();
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_Parse(plain.payload.c_str()), cJSON_Delete);
    cJSON_DeleteItemFromObjectCaseSensitive(root.get(), "handoff");
    char* encoded = cJSON_PrintUnformatted(root.get());
    ASSERT_NE(encoded, nullptr);
    plain.payload = encoded;
    cJSON_free(encoded);
    CandidateNetwork ordinary_network;
    CandidateMedia ordinary_media;
    HumanCallClient ordinary(ordinary_network, ordinary_media, "p/d", "ordinary", candidate_config());
    ASSERT_TRUE(ordinary.start_shared(100));
    ASSERT_EQ(encode_envelope(plain, bytes), DecodeResult::Ok);
    EXPECT_TRUE(ordinary.receive_shared("$thing/down/signaling/p/d", bytes, 101, 1000000));
    EXPECT_EQ(ordinary.state(), HumanCallState::Ringing);
}

// 原业务终止不被候选提交重放复活；相同历史事实仍只是账本重放，绝不能再次prepare。
TEST_F(HumanCandidate, EndingAndCloseNeverReviveInheritedCandidateSource) {
    connected();
    permitted = true;
    ASSERT_TRUE(receive(committed()));
    ASSERT_TRUE(client.end(++now));
    EXPECT_FALSE(client.input_wanted());
    EXPECT_FALSE(client.endpoint_source_available(++now, 1000000));
    EXPECT_TRUE(receive(committed()));
    auto future = committed();
    future.token = "future-commit";
    future.event_sequence = 4;
    EXPECT_FALSE(receive(future));
    EXPECT_FALSE(client.prepare_endpoint_source("device:p:next", ++now, 1000000));
    client.close();
    EXPECT_FALSE(receive(committed()));
    EXPECT_FALSE(client.endpoint_source_available(++now, 1000000));
}

// 回退时钟仍按原核心围栏；保留的候选出处不能越过原时钟或连接生命周期重新获源许可。
TEST_F(HumanCandidate, ClockRollbackAfterInheritedCommitRemainsClosed) {
    connected();
    permitted = true;
    ASSERT_TRUE(receive(committed()));
    std::string bytes;
    ASSERT_EQ(encode_envelope(committed(), bytes), DecodeResult::Ok);
    EXPECT_FALSE(client.receive_shared("$thing/down/signaling/p/d", bytes, now - 1, 1000000));
    EXPECT_FALSE(client.endpoint_source_available(++now, 1000000));
    EXPECT_FALSE(client.prepare_endpoint_source("device:p:next", ++now, 1000000));
}

// 两个独立候选分别沿真实invite/本人accept/committed成为后继新源，不使用revision setter。
TEST_F(HumanCandidate, SecondSuccessiveCandidateInheritsItsOwnOriginalCommittedRevision) {
    session = "second-candidate";
    handoff = "second-original-handoff";
    connected();
    permitted = true;
    ASSERT_TRUE(receive(committed(6)));
    ASSERT_TRUE(client.prepare_endpoint_source("device:p:third", ++now, 1000000));
    EXPECT_EQ(last_control().expected_revision, 6U);
    EXPECT_EQ(last_control().envelope.session, "second-candidate");
    EXPECT_NE(last_control().handoff_id, handoff);
}

// 协议组件串接两代：第二候选邀请只取第一新源实际prepare的handoff，不手填或改内部revision。
// 独立实际HAL三端交接由Host/后端集成另验，本用例不以回执替代实际资源释放。
TEST_F(HumanCandidate, TwoSuccessiveTransfersUseActualRequestOriginsAndIndependentCandidateLedgers) {
    connected();
    permitted = true;
    ASSERT_TRUE(receive(committed(3)));
    ASSERT_TRUE(client.prepare_endpoint_source("device:p:second", ++now, 1000000));
    const auto prepare = network.sent.back();
    const auto original = last_control();
    EXPECT_EQ(original.expected_revision, 3U);

    CandidateNetwork second_network;
    CandidateMedia second_media;
    HumanCallClient second(second_network, second_media, "p/d", "second", candidate_config());
    bool second_permitted = false;
    const std::string second_session = "second-human";
    ASSERT_TRUE(second.set_endpoint_source([&](auto current, auto, auto, auto) {
        return second_permitted && current == second_session;
    }));
    ASSERT_TRUE(second.start_shared(++now));
    const auto second_receive = [&](Envelope value) {
        std::string bytes;
        return encode_envelope(value, bytes) == DecodeResult::Ok &&
               second.receive_shared("$thing/down/signaling/p/d", bytes, ++now, 1000000);
    };
    auto second_invite = invite();
    second_invite.session = second_session;
    second_invite.payload.replace(second_invite.payload.find("source-human"), 12, session);
    second_invite.payload.replace(second_invite.payload.find(handoff), handoff.size(), original.handoff_id);
    ASSERT_TRUE(second_receive(second_invite));
    ASSERT_TRUE(second.accept(++now));
    auto response = ack(second_network.sent.back());
    response.session = second_session;
    ASSERT_TRUE(second_receive(response));
    ASSERT_TRUE(second.offer_ready("v=0\r\ns=voice\r\n", ++now));
    response = ack(second_network.sent.back());
    response.session = second_session;
    response.method = "mediaAnswer";
    response.payload = R"({"mediaRevision":1,"webrtc":{"iceMode":"complete","sdp":"v=0\r\ns=answer\r\n"}})";
    ASSERT_TRUE(second_receive(response));
    second_media.ready = true;
    ASSERT_TRUE(second.media_connected(++now));
    response = ack(second_network.sent.back());
    response.session = second_session;
    ASSERT_TRUE(second_receive(response));
    response.request = false;
    response.method = "sessionEstablished";
    response.token = "second-established";
    response.code.reset();
    response.event_sequence = 2;
    response.payload = R"({"state":"established","mediaRevision":1})";
    ASSERT_TRUE(second_receive(response));
    EXPECT_FALSE(second.prepare_endpoint_source("device:p:third", ++now, 1000000));

    response.request = false;
    response.method = "endpointState";
    response.session = session;
    response.token = prepare.token;
    response.event_sequence.reset();
    response.code = 200;
    response.payload = "{\"state\":\"preparing\",\"revision\":4,\"handoffId\":\"" + original.handoff_id + "\"}";
    ASSERT_TRUE(receive(response));
    response.token = "second-ready";
    response.code.reset();
    response.event_sequence = 4;
    response.payload = "{\"state\":\"ready\",\"revision\":5,\"handoffId\":\"" + original.handoff_id + "\",\"candidateSessionId\":\"" + second_session + "\"}";
    ASSERT_TRUE(receive(response));
    ASSERT_TRUE(client.commit_endpoint_source(++now, 1000000));
    EXPECT_EQ(last_control().expected_revision, 5U);
    response.token = network.sent.back().token;
    response.code = 200;
    response.event_sequence.reset();
    response.payload = "{\"state\":\"committed\",\"revision\":6,\"handoffId\":\"" + original.handoff_id + "\",\"candidateSessionId\":\"" + second_session + "\"}";
    ASSERT_TRUE(receive(response));
    EXPECT_TRUE(media.ready); // endpoint ACK自身不擅自停止原端或开候选；依正式控制器收口。
    response.session = second_session;
    response.token = "second-committed";
    response.code.reset();
    response.event_sequence = 3;
    EXPECT_FALSE(second_receive(response)); // 未有当前本人完整组许可，提交事件仍不授新源。
    second_permitted = true;
    ASSERT_TRUE(second_receive(response));
    ASSERT_TRUE(second.prepare_endpoint_source("device:p:third", ++now, 1000000));
    EndpointCommand next;
    std::string bytes;
    ASSERT_EQ(encode_envelope(second_network.sent.back(), bytes), DecodeResult::Ok);
    ASSERT_EQ(decode_endpoint_control(WireRoute::Signaling, bytes, next), WireResult::Ok);
    EXPECT_EQ(next.expected_revision, 6U);
    EXPECT_NE(next.handoff_id, original.handoff_id);
    EXPECT_EQ(next.envelope.session, second_session);
}
// 真实候选出处可呈现本人接听，但未实际整组/媒体/committed前绝不能作为新源。
TEST(HumanEndpointCandidate, FormalOriginInviteRingsButDoesNotGrantSourceBeforeAccept) {
    CandidateNetwork network;
    CandidateMedia media;
    HumanCallClient client(network, media, "p/d", "candidate", candidate_config());
    ASSERT_TRUE(client.set_endpoint_source([](auto, auto, auto, auto) { return false; }));
    ASSERT_TRUE(client.start_shared(100));
    Envelope invite;
    invite.method = "sessionInvite";
    invite.token = "original-invite";
    invite.session = "candidate-human";
    invite.event_sequence = 1;
    invite.payload = R"({"purpose":"humanCall","callType":"voice","admission":"accept","transport":"webrtc","expiresAt":1001000,"callGroupId":"original-group","tracks":[{"trackId":"voice","kind":"audio","sourceId":"microphone","sourceType":"microphone","direction":"sendrecv","codec":"opus","state":"requested"}],"handoff":{"sourceSessionId":"source-human","handoffId":"original-handoff"}})";
    std::string bytes;
    ASSERT_EQ(encode_envelope(invite, bytes), DecodeResult::Ok);
    EXPECT_TRUE(client.receive_shared("$thing/down/signaling/p/d", bytes, 101, 1000000));
    EXPECT_EQ(client.state(), HumanCallState::Ringing);
    EXPECT_EQ(media.prepares, 0U);
    EXPECT_FALSE(client.endpoint_source_available(102, 1000001));
    EXPECT_FALSE(client.prepare_endpoint_source("device:p:next", 102, 1000001));
}
} // namespace
} // namespace ur::device
