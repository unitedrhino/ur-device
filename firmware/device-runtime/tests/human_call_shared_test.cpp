// 文件说明：单宿主借用MQTT的人际腿回归；显式接听与实际媒体事实不能由其他会话代替。
#include <gtest/gtest.h>
#include <deque>
#include <vector>
#include "ur/device/envelope.hpp"
#include "ur/device/human_call.hpp"

namespace ur::device {
namespace {
// 结束事件必须与指定腿一致、是无状态码的事件且序号严格前进。
TEST(HumanCallTerminalMatch, ChecksSessionKindAndSequence) {
    Envelope event;
    event.method = "sessionEnded";
    event.request = false;
    event.session = "leg-1";
    event.event_sequence = 2;
    EXPECT_TRUE(human_call_terminal_matches(event, "leg-1", 1));
    EXPECT_FALSE(human_call_terminal_matches(event, "leg-2", 1));
    EXPECT_FALSE(human_call_terminal_matches(event, "leg-1", 2));
    event.event_sequence.reset();
    EXPECT_FALSE(human_call_terminal_matches(event, "leg-1", 1));
    event.event_sequence = 2;
    event.request = true;
    EXPECT_FALSE(human_call_terminal_matches(event, "leg-1", 1));
    event.request = false;
    event.code = 200;
    EXPECT_FALSE(human_call_terminal_matches(event, "leg-1", 1));
    event.code.reset();
    event.method = "sessionEstablished";
    EXPECT_FALSE(human_call_terminal_matches(event, "leg-1", 1));
}

// 等待阶段的邀请只可使用合法原截止，不能用重投延长重放屏障。
TEST(HumanCallPendingDeadline, ValidatesInvitationIdentityAndExpiry) {
    Envelope invite;
    invite.method = "sessionInvite";
    invite.token = "invite-1";
    invite.request = true;
    invite.session = "leg-1";
    invite.event_sequence = 1;
    invite.payload = R"({"purpose":"humanCall","expiresAt":1030000})";
    EXPECT_EQ(human_call_pending_deadline(invite), 1030000u);
    invite.event_sequence = 2;
    EXPECT_FALSE(human_call_pending_deadline(invite));
    invite.event_sequence = 1;
    invite.session.clear();
    EXPECT_FALSE(human_call_pending_deadline(invite));
    invite.session = "leg-1";
    invite.payload = R"({"purpose":"humanCall","expiresAt":3.5})";
    EXPECT_FALSE(human_call_pending_deadline(invite));
    invite.payload = R"({"purpose":"other","expiresAt":1030000})";
    EXPECT_FALSE(human_call_pending_deadline(invite));
    invite.payload = R"({"purpose":"humanCall","expiresAt":-1})";
    EXPECT_FALSE(human_call_pending_deadline(invite));
}

// SharedMqtt 记录网络所有权调用，队列中的IPC消息不应被人际腿消费。
struct SharedMqtt final : MqttPort {
    unsigned connects = 0, subscriptions = 0, polls = 0, takes = 0, closes = 0;
    std::deque<MqttEvent> events;
    std::vector<Envelope> sent;
    std::vector<std::string> bodies;  // 发布尝试原字节，false受理不能丢原拒绝决策。
    bool writable = true;             // 可控本地背压，不模拟远端执行成功。
    bool connect(const MqttConfig&) override {
        ++connects;
        return true;
    }
    int subscribe(std::string_view) override {
        ++subscriptions;
        return 1;
    }
    bool publish(std::string_view, std::string_view body) override {
        Envelope message;
        if (decode_envelope(body, message) != DecodeResult::Ok)
            return false;
        bodies.emplace_back(body);
        sent.push_back(std::move(message));
        return writable;
    }
    bool poll() override {
        ++polls;
        return true;
    }
    std::optional<MqttEvent> take_event() override {
        ++takes;
        if (events.empty())
            return {};
        auto value = std::move(events.front());
        events.pop_front();
        return value;
    }
    void disconnect() override { ++closes; }
};
// SharedMedia 模拟原物理预约在ACK之前占用，不以prepare为实际采集。
struct SharedMedia final : CallMediaPort {
    bool free = true, reserved = false, started = false;
    bool ready = false;            // 只由用例注入实际驱动连通，不从SDP成功自动推断。
    bool require_stopped = false;  // 新复用夹具显式等待Stopped；不代表真实硬件驱动。
    bool stopping = false;         // 已请求停止但尚未交回原物理预约。
    unsigned preparations = 0;
    bool available() const override { return free && !stopping; }
    bool prepare() override {
        ++preparations;
        reserved = available();
        return reserved;
    }
    bool begin() override {
        started = reserved;
        return started;
    }
    bool apply_answer(std::string_view) override { return started; }
    bool connected() const override { return ready; }
    void close() override {
        if (require_stopped && started)
            stopping = true;
        if (!stopping)
            reserved = false;
        started = ready = false;
    }
    // report_stopped 模拟驱动实际资源交接，核心不可用close命令替它解除忙线。
    bool report_stopped() {
        if (!stopping)
            return false;
        stopping = reserved = false;
        return true;
    }
};
// shared_invite 包含标准六键三逻辑轨，指定camera是平台与宿主共同绑定的物理源。
std::string shared_invite(std::string_view camera = "entry") {
    Envelope message;
    message.method = "sessionInvite";
    message.token = "original-invite";
    message.session = "source-leg";
    message.event_sequence = 1;
    message.payload =
        R"({"purpose":"humanCall","callType":"video","admission":"accept","transport":"webrtc","expiresAt":1030000,"callGroupId":"group","tracks":[{"trackId":"mic-1","kind":"audio","sourceId":"mic","direction":"sendonly","codec":"opus","state":"requested"},{"trackId":"speaker-1","kind":"audio","sourceId":"speaker","direction":"recvonly","codec":"opus","state":"requested"},{"trackId":"video-1","kind":"video","sourceId":")";
    message.payload += camera;
    message.payload += R"(","direction":"sendonly","codec":"H264","state":"requested"}]})";
    std::string body;
    EXPECT_EQ(encode_envelope(message, body), DecodeResult::Ok);
    return body;
}
// shared_ack 只确认原sessionAccept，不能伪装ICE或平台建立事件。
std::string shared_ack() {
    Envelope message;
    message.request = false;
    message.method = "requestAck";
    message.token = "original-invite";
    message.session = "source-leg";
    message.code = 200;
    message.payload = R"({"requestMethod":"sessionAccept","result":{"state":"negotiating"}})";
    std::string body;
    EXPECT_EQ(encode_envelope(message, body), DecodeResult::Ok);
    return body;
}
// shared_end 构造平台原腿终态，未接听及媒体协商中均不得随后被QoS1旧邀请复活。
std::string shared_end() {
    Envelope message;
    message.request = false;
    message.method = "sessionEnded";
    message.token = "original-ended";
    message.session = "source-leg";
    message.event_sequence = 2;
    message.payload = R"({"state":"ended","reason":"hangup"})";
    std::string body;
    EXPECT_EQ(encode_envelope(message, body), DecodeResult::Ok);
    return body;
}
// changed_invite 保留标准业务字段，仅由用例显式改变原token、session或原参数。
std::string changed_invite(std::string token, std::string session, bool change_params = false) {
    Envelope message;
    EXPECT_EQ(decode_envelope(shared_invite(), message), DecodeResult::Ok);
    message.token = std::move(token);
    message.session = std::move(session);
    if (change_params) {
        const auto original = message.payload.find("\"group\"");
        EXPECT_NE(original, std::string::npos);
        message.payload.replace(original, 7, "\"different-group\"");
    }
    std::string body;
    EXPECT_EQ(encode_envelope(message, body), DecodeResult::Ok);
    return body;
}
// later_invite 仅为截止边界用例生成独立新意图，不修改既存决策的原有效期。
std::string later_invite() {
    Envelope message;
    EXPECT_EQ(decode_envelope(changed_invite("later-token", "later-leg"), message),
              DecodeResult::Ok);
    const auto original = message.payload.find("1030000");
    EXPECT_NE(original, std::string::npos);
    message.payload.replace(original, 7, "1050000");
    std::string body;
    EXPECT_EQ(encode_envelope(message, body), DecodeResult::Ok);
    return body;
}
// ReuseReceipts 保存上一轮实际关联应答，重投不能接管下一轮授权或清空原事实。
struct ReuseReceipts {
    Envelope answer;       // 原mediaOffer对应的完整应答。
    Envelope state_ack;    // 原mediaState对应的业务确认。
    Envelope established;  // 原腿seq2建立事件。
};
// reuse_reply 构造指定独立腿的请求结果，不复用新一轮的token替换原身份。
Envelope reuse_reply(std::string token, std::string method, std::string session,
                     std::string payload) {
    Envelope value;
    value.request = false;
    value.token = std::move(token);
    value.method = std::move(method);
    value.session = std::move(session);
    value.code = 200;
    value.payload = std::move(payload);
    return value;
}
// reuse_deliver 只把已编码信令交给同一个借用客户端；单调时间由用例推进。
void reuse_deliver(HumanCallClient& client, const Envelope& value, std::uint64_t& now) {
    std::string body;
    ASSERT_EQ(encode_envelope(value, body), DecodeResult::Ok);
    ASSERT_TRUE(client.receive_shared("$thing/down/signaling/p/d", body, ++now, 1'000'000));
}
// reuse_round 逐一提供接听、完整SDP、实际驱动连通及两个独立建立条件，不能跳过第二轮握手。
void reuse_round(HumanCallClient& client, SharedMqtt& mqtt, SharedMedia& media,
                 const std::string& session, const std::string& token, std::uint64_t& now,
                 ReuseReceipts& receipts) {
    ASSERT_TRUE(client.receive_shared("$thing/down/signaling/p/d", changed_invite(token, session),
                                      ++now, 1'000'000));
    ASSERT_EQ(client.state(), HumanCallState::Ringing);
    ASSERT_TRUE(client.accept(++now));
    auto accepted =
        reuse_reply(token, "requestAck", session,
                    R"({"requestMethod":"sessionAccept","result":{"state":"negotiating"}})");
    ASSERT_NO_FATAL_FAILURE(reuse_deliver(client, accepted, now));
    ASSERT_EQ(client.state(), HumanCallState::Negotiating);
    ASSERT_TRUE(client.offer_ready("v=0\r\no=client\r\n", ++now));
    ASSERT_EQ(mqtt.sent.back().method, "mediaOffer");
    receipts.answer = reuse_reply(
        mqtt.sent.back().token, "mediaAnswer", session,
        R"({"mediaRevision":1,"webrtc":{"iceMode":"complete","sdp":"v=0\r\no=server\r\n"}})");
    ASSERT_NO_FATAL_FAILURE(reuse_deliver(client, receipts.answer, now));
    ASSERT_EQ(client.state(), HumanCallState::ConnectingMedia);
    media.ready = true;
    ASSERT_TRUE(client.media_connected(++now));
    receipts.state_ack = reuse_reply(
        mqtt.sent.back().token, "requestAck", session,
        R"({"requestMethod":"mediaState","result":{"mediaRevision":1,"state":"connected"}})");
    ASSERT_NO_FATAL_FAILURE(reuse_deliver(client, receipts.state_ack, now));
    EXPECT_FALSE(client.active());  // 旧轮established不得让新ACK越过新seq2建立条件。
    receipts.established.request = false;
    receipts.established.token = "established-" + token;
    receipts.established.method = "sessionEstablished";
    receipts.established.session = session;
    receipts.established.event_sequence = 2;
    receipts.established.payload = R"({"state":"established","mediaRevision":1})";
    ASSERT_NO_FATAL_FAILURE(reuse_deliver(client, receipts.established, now));
    ASSERT_TRUE(client.active());
}
// reuse_end 生成无code的原腿平台终态，独立于主动挂断请求ACK。
Envelope reuse_end(const std::string& session, std::uint64_t sequence = 3) {
    Envelope value;
    value.request = false;
    value.token = "ended-" + session;
    value.method = "sessionEnded";
    value.session = session;
    value.event_sequence = sequence;
    value.payload = R"({"state":"ended","reason":"hangup"})";
    return value;
}
// reuse_two_rounds 同客户端同连接完成两轮，并重投旧邀请/应答，禁止重建客户端绕过原栅栏。
void reuse_two_rounds(bool local_hangup) {
    SharedMqtt mqtt;
    SharedMedia media;
    media.require_stopped = true;
    HumanCallClient client(mqtt, media, "p/d", "shared", {"entry", true});
    std::uint64_t now = 100;
    ASSERT_TRUE(client.start_shared(now));
    ReuseReceipts first, second;
    ASSERT_NO_FATAL_FAILURE(
        reuse_round(client, mqtt, media, "first-leg", "first-token", now, first));
    if (local_hangup) {
        ASSERT_TRUE(client.end(++now));
        const auto ack = reuse_reply(mqtt.sent.back().token, "sessionEnded", "first-leg",
                                     R"({"state":"ended","reason":"hangup"})");
        ASSERT_NO_FATAL_FAILURE(reuse_deliver(client, ack, now));
    } else {
        ASSERT_NO_FATAL_FAILURE(reuse_deliver(client, reuse_end("first-leg"), now));
    }
    ASSERT_EQ(client.state(), HumanCallState::Available);
    ASSERT_TRUE(client.session_id().empty());
    ASSERT_TRUE(client.receive_shared(
        "$thing/down/signaling/p/d", changed_invite("early-token", "early-leg"), ++now, 1'000'000));
    ASSERT_EQ(mqtt.sent.back().method, "sessionReject");
    EXPECT_EQ(client.state(), HumanCallState::Available);
    ASSERT_TRUE(media.report_stopped());
    ASSERT_TRUE(client.receive_shared(
        "$thing/down/signaling/p/d", changed_invite("early-token", "early-leg"), ++now, 1'000'000));
    EXPECT_EQ(client.state(),
              HumanCallState::Available);  // 已Busy原意图不能随Stopped变成新接听许可。
    ASSERT_TRUE(client.receive_shared(
        "$thing/down/signaling/p/d", changed_invite("first-token", "first-leg"), ++now, 1'000'000));
    EXPECT_EQ(client.state(), HumanCallState::Available);
    ASSERT_NO_FATAL_FAILURE(
        reuse_round(client, mqtt, media, "second-leg", "second-token", now, second));
    EXPECT_NE(first.answer.token, second.answer.token);
    ASSERT_NO_FATAL_FAILURE(reuse_deliver(client, first.answer, now));
    ASSERT_NO_FATAL_FAILURE(reuse_deliver(client, first.state_ack, now));
    ASSERT_NO_FATAL_FAILURE(reuse_deliver(client, first.established, now));
    ASSERT_NO_FATAL_FAILURE(reuse_deliver(client, reuse_end("first-leg"), now));
    ASSERT_TRUE(client.active());
    EXPECT_EQ(client.session_id(), "second-leg");
    ASSERT_NO_FATAL_FAILURE(reuse_deliver(client, second.answer, now));
    EXPECT_TRUE(client.active());  // 原第二轮Succeeded历史仍可接纳精确重投，不重新申请媒体。
    ASSERT_NO_FATAL_FAILURE(reuse_deliver(client, reuse_end("second-leg"), now));
    EXPECT_EQ(client.state(), HumanCallState::Available);
    EXPECT_TRUE(media.report_stopped());
    EXPECT_EQ(mqtt.connects + mqtt.subscriptions + mqtt.closes, 0U);
}
}  // namespace

// 网络只有宿主拥有，人际poll/close不消费IPC队列也不重连或重报能力。
TEST(HumanCallSharedTest, HostAloneOwnsTransport) {
    SharedMqtt mqtt;
    SharedMedia media;
    HumanCallClient client(mqtt, media, "p/d", "shared", {"entry", true});
    mqtt.events.push_back({MqttEvent::Kind::Message, 0, 0, "ipc", "untouched"});
    EXPECT_FALSE(client.start(MqttConfig{}, 100));
    ASSERT_TRUE(client.start_shared(100));
    client.poll(101, 1'000'000);
    client.close();
    EXPECT_EQ(mqtt.connects + mqtt.subscriptions + mqtt.polls + mqtt.takes + mqtt.closes, 0U);
    EXPECT_EQ(mqtt.events.size(), 1U);
    EXPECT_TRUE(mqtt.sent.empty());
}
// 邀请不采集；用户明确接听先预约，原ACK后才调用begin且仍不称媒体接通。
TEST(HumanCallSharedTest, ExplicitAcceptReservesBeforeAck) {
    SharedMqtt mqtt;
    SharedMedia media;
    HumanCallClient client(mqtt, media, "p/d", "shared", {"entry", true});
    ASSERT_TRUE(client.start_shared(100));
    ASSERT_TRUE(
        client.receive_shared("$thing/down/signaling/p/d", shared_invite(), 101, 1'000'000));
    ASSERT_EQ(client.state(), HumanCallState::Ringing);
    EXPECT_FALSE(media.reserved || media.started);
    ASSERT_TRUE(client.accept(102));
    EXPECT_TRUE(media.reserved);
    EXPECT_FALSE(media.started);
    ASSERT_EQ(mqtt.sent.size(), 1U);
    EXPECT_EQ(mqtt.sent.back().token, "original-invite");
    EXPECT_EQ(mqtt.sent.back().method, "sessionAccept");
    ASSERT_TRUE(client.receive_shared("$thing/down/signaling/p/d", shared_ack(), 103, 1'000'000));
    EXPECT_TRUE(media.started);
    EXPECT_FALSE(client.active());
    client.close();
    EXPECT_EQ(mqtt.closes, 0U);
}
// 用户点击到ACK之间IPC已占用时使用原邀请拒接忙线，不能把旧预览权限升级成人际采集。
TEST(HumanCallSharedTest, BusyAtAcceptUsesOriginalReject) {
    SharedMqtt mqtt;
    SharedMedia media;
    HumanCallClient client(mqtt, media, "p/d", "shared", {"entry", true});
    ASSERT_TRUE(client.start_shared(100));
    ASSERT_TRUE(
        client.receive_shared("$thing/down/signaling/p/d", shared_invite(), 101, 1'000'000));
    media.free = false;
    ASSERT_TRUE(client.accept(102));
    ASSERT_EQ(mqtt.sent.size(), 1U);
    EXPECT_EQ(mqtt.sent.back().method, "sessionReject");
    EXPECT_EQ(mqtt.sent.back().token, "original-invite");
    EXPECT_EQ(mqtt.sent.back().payload, R"({"reason":"busy"})");
    EXPECT_FALSE(media.started);
    EXPECT_EQ(client.state(), HumanCallState::Ending);
}
// 动态相机不接受front假设，非法邀请及单调回退只隔离本腿。
TEST(HumanCallSharedTest, WrongCameraAndClockRollbackDoNotDisconnectHost) {
    SharedMqtt mqtt;
    SharedMedia media;
    HumanCallClient client(mqtt, media, "p/d", "shared", {"entry", true});
    ASSERT_TRUE(client.start_shared(100));
    EXPECT_FALSE(
        client.receive_shared("$thing/down/signaling/p/d", shared_invite("front"), 101, 1'000'000));
    EXPECT_EQ(client.state(), HumanCallState::Failed);
    EXPECT_EQ(mqtt.closes, 0U);
    ASSERT_TRUE(client.start_shared(200));
    client.poll(199, 1'000'000);
    EXPECT_EQ(client.state(), HumanCallState::Failed);
    EXPECT_EQ(mqtt.closes, 0U);
    EXPECT_TRUE(human_call_invitation(shared_invite()));
    EXPECT_FALSE(human_call_invitation(R"({"purpose":"humanCall"})"));
}
// 借用接口也必须有有效宿主绑定，不能以调用者省略身份来发布任意Topic。
TEST(HumanCallSharedTest, InvalidHostBindingFailsClosed) {
    SharedMqtt mqtt;
    SharedMedia media;
    HumanCallClient empty_instance(mqtt, media, "p/d", "", {"entry", true});
    HumanCallClient wildcard(mqtt, media, "p/+", "shared", {"entry", true});
    HumanCallClient empty_camera(mqtt, media, "p/d", "shared", {"", true});
    EXPECT_FALSE(empty_instance.start_shared(100));
    EXPECT_FALSE(wildcard.start_shared(100));
    EXPECT_FALSE(empty_camera.start_shared(100));
    EXPECT_TRUE(mqtt.sent.empty());
    EXPECT_EQ(mqtt.closes, 0U);
}
// 接听已ACK且原平台终态到达后，原有效期内旧邀请只能隔离，不能幽灵振铃或采集。
TEST(HumanCallSharedLateTest, AcceptedEndedOriginalInviteCannotRingAgain) {
    SharedMqtt mqtt;
    SharedMedia media;
    HumanCallClient client(mqtt, media, "p/d", "shared", {"entry", true});
    ASSERT_TRUE(client.start_shared(100));
    ASSERT_TRUE(
        client.receive_shared("$thing/down/signaling/p/d", shared_invite(), 101, 1'000'000));
    ASSERT_TRUE(client.accept(102));
    ASSERT_TRUE(client.receive_shared("$thing/down/signaling/p/d", shared_ack(), 103, 1'000'000));
    ASSERT_TRUE(client.receive_shared("$thing/down/signaling/p/d", shared_end(), 104, 1'000'000));
    ASSERT_EQ(client.state(), HumanCallState::Available);
    ASSERT_TRUE(
        client.receive_shared("$thing/down/signaling/p/d", shared_invite(), 105, 1'000'000));
    EXPECT_EQ(client.state(), HumanCallState::Available);
    EXPECT_TRUE(client.session_id().empty());
    EXPECT_FALSE(media.started || media.reserved);
    EXPECT_EQ(mqtt.sent.size(), 1U);
}
// 原Busy决策不能随资源释放重新解释为接听许可；重复回复只允许原拒绝事实。
TEST(HumanCallSharedLateTest, BusyDecisionStaysBusyAfterResourceRelease) {
    SharedMqtt mqtt;
    SharedMedia media;
    media.free = false;
    HumanCallClient client(mqtt, media, "p/d", "shared", {"entry", true});
    ASSERT_TRUE(client.start_shared(100));
    ASSERT_TRUE(
        client.receive_shared("$thing/down/signaling/p/d", shared_invite(), 101, 1'000'000));
    ASSERT_EQ(client.state(), HumanCallState::Available);
    ASSERT_EQ(mqtt.sent.size(), 1U);
    ASSERT_EQ(mqtt.sent.back().method, "sessionReject");
    media.free = true;
    ASSERT_TRUE(
        client.receive_shared("$thing/down/signaling/p/d", shared_invite(), 102, 1'000'000));
    EXPECT_EQ(client.state(), HumanCallState::Available);
    EXPECT_TRUE(client.session_id().empty());
    EXPECT_EQ(media.preparations, 0U);
    for (const auto& sent : mqtt.sent) {
        EXPECT_EQ(sent.token, "original-invite");
        EXPECT_EQ(sent.payload, R"({"reason":"busy"})");
    }
}
// 未接听的原终态同样关闭原意图，不因尚无sessionAccept请求而丢失邀请生命周期。
TEST(HumanCallSharedLateTest, UnansweredEndedOriginalInviteCannotRingAgain) {
    SharedMqtt mqtt;
    SharedMedia media;
    HumanCallClient client(mqtt, media, "p/d", "shared", {"entry", true});
    ASSERT_TRUE(client.start_shared(100));
    ASSERT_TRUE(
        client.receive_shared("$thing/down/signaling/p/d", shared_invite(), 101, 1'000'000));
    ASSERT_TRUE(client.receive_shared("$thing/down/signaling/p/d", shared_end(), 102, 1'000'000));
    ASSERT_TRUE(
        client.receive_shared("$thing/down/signaling/p/d", shared_invite(), 103, 1'000'000));
    EXPECT_EQ(client.state(), HumanCallState::Available);
    EXPECT_TRUE(mqtt.sent.empty());
    EXPECT_FALSE(media.started || media.reserved);
}
// 拒绝之后旧token改成新session不能绕过原决策；相同token不是新的用户意图。
TEST(HumanCallSharedLateTest, BusyTokenCannotMoveToDifferentSession) {
    SharedMqtt mqtt;
    SharedMedia media;
    media.free = false;
    HumanCallClient client(mqtt, media, "p/d", "shared", {"entry", true});
    ASSERT_TRUE(client.start_shared(100));
    ASSERT_TRUE(
        client.receive_shared("$thing/down/signaling/p/d", shared_invite(), 101, 1'000'000));
    media.free = true;
    EXPECT_FALSE(client.receive_shared("$thing/down/signaling/p/d",
                                       changed_invite("original-invite", "changed-leg"), 102,
                                       1'000'000));
    EXPECT_NE(client.state(), HumanCallState::Ringing);
    EXPECT_EQ(mqtt.sent.size(), 1U);
    EXPECT_FALSE(media.started || media.reserved);
}
// 原参数变更不是合法重复；独立新token与新session仍可在资源真正释放后振铃。
TEST(HumanCallSharedLateTest, BusyOriginalParamsCannotChangeButIndependentIntentWorks) {
    for (const bool change_params : {false, true}) {
        SharedMqtt mqtt;
        SharedMedia media;
        media.free = false;
        HumanCallClient client(mqtt, media, "p/d", "shared", {"entry", true});
        ASSERT_TRUE(client.start_shared(100));
        ASSERT_TRUE(
            client.receive_shared("$thing/down/signaling/p/d", shared_invite(), 101, 1'000'000));
        media.free = true;
        const auto next = change_params ? changed_invite("original-invite", "source-leg", true)
                                        : changed_invite("new-invite", "new-source-leg");
        const auto received =
            client.receive_shared("$thing/down/signaling/p/d", next, 102, 1'000'000);
        EXPECT_EQ(received, !change_params);
        if (change_params)
            EXPECT_NE(client.state(), HumanCallState::Ringing);
        else {
            EXPECT_EQ(client.state(), HumanCallState::Ringing);
            EXPECT_EQ(client.session_id(), "new-source-leg");
        }
        EXPECT_FALSE(media.started || media.reserved);
    }
}
// 原拒绝发布遇背压是结果未知，不允许重建token或在驱动空闲后转为来电许可。
TEST(HumanCallSharedLateTest, BackpressureKeepsOriginalBusyBytesAcrossRestart) {
    SharedMqtt mqtt;
    SharedMedia media;
    media.free = false;
    mqtt.writable = false;
    HumanCallClient client(mqtt, media, "p/d", "shared", {"entry", true});
    ASSERT_TRUE(client.start_shared(100));
    EXPECT_FALSE(
        client.receive_shared("$thing/down/signaling/p/d", shared_invite(), 101, 1'000'000));
    ASSERT_EQ(mqtt.bodies.size(), 1U);
    const auto original = mqtt.bodies.front();
    media.free = true;
    mqtt.writable = true;
    ASSERT_TRUE(client.start_shared(102));
    ASSERT_TRUE(
        client.receive_shared("$thing/down/signaling/p/d", shared_invite(), 103, 1'000'000));
    EXPECT_EQ(client.state(), HumanCallState::Available);
    ASSERT_EQ(mqtt.bodies.size(), 2U);
    EXPECT_EQ(mqtt.bodies.back(), original);
    EXPECT_EQ(media.preparations, 0U);
}
// 未过期32个决策不得被第33个挤掉，满额不能出现无保护的Busy发布或新来电。
TEST(HumanCallSharedLateTest, CapacityDoesNotEvictUnexpiredDecisions) {
    SharedMqtt mqtt;
    SharedMedia media;
    media.free = false;
    HumanCallClient client(mqtt, media, "p/d", "shared", {"entry", true});
    ASSERT_TRUE(client.start_shared(100));
    for (unsigned index = 0; index < 32; ++index)
        ASSERT_TRUE(client.receive_shared(
            "$thing/down/signaling/p/d",
            changed_invite("token-" + std::to_string(index), "leg-" + std::to_string(index)),
            101 + index, 1'000'000));
    ASSERT_EQ(mqtt.sent.size(), 32U);
    EXPECT_FALSE(client.receive_shared("$thing/down/signaling/p/d",
                                       changed_invite("overflow", "overflow-leg"), 133, 1'000'000));
    EXPECT_EQ(mqtt.sent.size(), 32U);
    ASSERT_TRUE(client.start_shared(134));
    media.free = true;
    ASSERT_TRUE(client.receive_shared("$thing/down/signaling/p/d",
                                      changed_invite("token-0", "leg-0"), 135, 1'000'000));
    EXPECT_EQ(client.state(), HumanCallState::Available);
    EXPECT_EQ(mqtt.sent.back().method, "sessionReject");
}
// 精确重投不延长截止，终态过期后独立新意图可复用容量；墙钟回退不能复活原过期意图。
TEST(HumanCallSharedLateTest, OriginalDeadlineAndWallClockRollbackStayClosed) {
    SharedMqtt mqtt;
    SharedMedia media;
    media.free = false;
    HumanCallClient client(mqtt, media, "p/d", "shared", {"entry", true});
    ASSERT_TRUE(client.start_shared(100));
    for (unsigned index = 0; index < 32; ++index)
        ASSERT_TRUE(client.receive_shared(
            "$thing/down/signaling/p/d",
            changed_invite("token-" + std::to_string(index), "leg-" + std::to_string(index)),
            101 + index, 1'000'000));
    ASSERT_TRUE(client.receive_shared("$thing/down/signaling/p/d",
                                      changed_invite("token-0", "leg-0"), 133, 1'029'999));
    media.free = true;
    ASSERT_TRUE(client.receive_shared("$thing/down/signaling/p/d", later_invite(), 134, 1'030'000));
    EXPECT_EQ(client.session_id(), "later-leg");
    client.close();
    ASSERT_TRUE(client.start_shared(135));
    EXPECT_FALSE(client.receive_shared("$thing/down/signaling/p/d",
                                       changed_invite("token-0", "leg-0"), 136, 1'000'000));
    EXPECT_NE(client.state(), HumanCallState::Ringing);
    EXPECT_EQ(media.preparations, 0U);
}
// 单调回退终止本腿但不清原决定，同宿主恢复也不能重新振铃旧意图。
TEST(HumanCallSharedLateTest, MonotonicRollbackDoesNotClearInvitationHistory) {
    SharedMqtt mqtt;
    SharedMedia media;
    HumanCallClient client(mqtt, media, "p/d", "shared", {"entry", true});
    ASSERT_TRUE(client.start_shared(100));
    ASSERT_TRUE(
        client.receive_shared("$thing/down/signaling/p/d", shared_invite(), 101, 1'000'000));
    client.poll(100, 1'000'000);
    ASSERT_EQ(client.state(), HumanCallState::Failed);
    ASSERT_TRUE(client.start_shared(102));
    ASSERT_TRUE(
        client.receive_shared("$thing/down/signaling/p/d", shared_invite(), 103, 1'000'000));
    EXPECT_EQ(client.state(), HumanCallState::Available);
    EXPECT_TRUE(client.session_id().empty());
    EXPECT_EQ(mqtt.closes, 0U);
}
// 平台正常终态必须释放本轮暂存，原历史不清空；第二媒体轮不重启共享连接。
TEST(HumanCallSharedReuseTest, PlatformEndedAllowsSecondCompleteRound) {
    ASSERT_NO_FATAL_FAILURE(reuse_two_rounds(false));
}
// 主动挂断ACK同样清理本轮媒体offer及建立条件，迟到旧结果只作历史隔离。
TEST(HumanCallSharedReuseTest, LocalHangupAckAllowsSecondCompleteRound) {
    ASSERT_NO_FATAL_FAILURE(reuse_two_rounds(true));
}
// 未完成协商时被平台取消，旧offer未知不能阻止另一个独立腿生成自己的offer。
TEST(HumanCallSharedReuseTest, CancelledNegotiationDoesNotBlockNewOffer) {
    SharedMqtt mqtt;
    SharedMedia media;
    HumanCallClient client(mqtt, media, "p/d", "shared", {"entry", true});
    std::uint64_t now = 100;
    ASSERT_TRUE(client.start_shared(now));
    ASSERT_TRUE(
        client.receive_shared("$thing/down/signaling/p/d", shared_invite(), ++now, 1'000'000));
    ASSERT_TRUE(client.accept(++now));
    ASSERT_TRUE(client.receive_shared("$thing/down/signaling/p/d", shared_ack(), ++now, 1'000'000));
    ASSERT_TRUE(client.offer_ready("v=0\r\no=client\r\n", ++now));
    const auto old_token = mqtt.sent.back().token;
    ASSERT_NO_FATAL_FAILURE(reuse_deliver(client, reuse_end("source-leg", 2), now));
    ReuseReceipts next;
    ASSERT_NO_FATAL_FAILURE(reuse_round(client, mqtt, media, "next-leg", "next-token", now, next));
    EXPECT_NE(next.answer.token, old_token);
    ASSERT_NO_FATAL_FAILURE(reuse_deliver(
        client,
        reuse_reply(
            old_token, "mediaAnswer", "source-leg",
            R"({"mediaRevision":1,"webrtc":{"iceMode":"complete","sdp":"v=0\r\no=old\r\n"}})"),
        now));
    EXPECT_TRUE(client.active());
}
// 前轮在途控制已随终态取消，不得其十秒期限让空闲入口失败或使新腿立即超时。
TEST(HumanCallSharedReuseTest, EndedPendingTrackDoesNotTimeoutNextRound) {
    SharedMqtt mqtt;
    SharedMedia media;
    HumanCallClient client(mqtt, media, "p/d", "shared", {"entry", true});
    std::uint64_t now = 100;
    ASSERT_TRUE(client.start_shared(now));
    ReuseReceipts first, next;
    ASSERT_NO_FATAL_FAILURE(
        reuse_round(client, mqtt, media, "first-leg", "first-token", now, first));
    ASSERT_TRUE(client.control_track("mic-1", true, ++now));
    const auto old_control = mqtt.sent.back().token;
    ASSERT_NO_FATAL_FAILURE(reuse_deliver(client, reuse_end("first-leg"), now));
    EXPECT_FALSE(client.track_control_pending());
    now += 10001;
    client.poll(now, 1'000'000);
    ASSERT_EQ(client.state(), HumanCallState::Available);
    ASSERT_NO_FATAL_FAILURE(reuse_round(client, mqtt, media, "next-leg", "next-token", now, next));
    ASSERT_NO_FATAL_FAILURE(
        reuse_deliver(client,
                      reuse_reply(old_control, "trackState", "first-leg",
                                  R"({"trackId":"mic-1","state":"paused","mediaRevision":1})"),
                      now));
    EXPECT_TRUE(client.active());
}
// 正常结束之后的拒接保留原邀请决定，第三条独立腿仍须完整重新协商。
TEST(HumanCallSharedReuseTest, DeclinedIntermediateInviteKeepsHistoryAndAllowsNextRound) {
    SharedMqtt mqtt;
    SharedMedia media;
    HumanCallClient client(mqtt, media, "p/d", "shared", {"entry", true});
    std::uint64_t now = 100;
    ASSERT_TRUE(client.start_shared(now));
    ReuseReceipts first, next;
    ASSERT_NO_FATAL_FAILURE(
        reuse_round(client, mqtt, media, "first-leg", "first-token", now, first));
    ASSERT_NO_FATAL_FAILURE(reuse_deliver(client, reuse_end("first-leg"), now));
    ASSERT_TRUE(client.receive_shared("$thing/down/signaling/p/d",
                                      changed_invite("declined-token", "declined-leg"), ++now,
                                      1'000'000));
    ASSERT_TRUE(client.reject(++now));
    ASSERT_NO_FATAL_FAILURE(reuse_deliver(
        client,
        reuse_reply("declined-token", "requestAck", "declined-leg",
                    R"({"requestMethod":"sessionReject","result":{"state":"ended"}})"),
        now));
    ASSERT_NO_FATAL_FAILURE(reuse_deliver(client, reuse_end("declined-leg", 2), now));
    ASSERT_TRUE(client.receive_shared("$thing/down/signaling/p/d",
                                      changed_invite("declined-token", "declined-leg"), ++now,
                                      1'000'000));
    ASSERT_EQ(client.state(), HumanCallState::Available);
    ASSERT_NO_FATAL_FAILURE(reuse_round(client, mqtt, media, "next-leg", "next-token", now, next));
}
// 协议失败不是正常终态；同宿主不得靠新邀请自动恢复或断开共享MQTT。
TEST(HumanCallSharedReuseTest, ProtocolFailureRemainsClosedWithoutAutomaticRestart) {
    SharedMqtt mqtt;
    SharedMedia media;
    HumanCallClient client(mqtt, media, "p/d", "shared", {"entry", true});
    std::uint64_t now = 100;
    ASSERT_TRUE(client.start_shared(now));
    ReuseReceipts first;
    ASSERT_NO_FATAL_FAILURE(
        reuse_round(client, mqtt, media, "first-leg", "first-token", now, first));
    EXPECT_FALSE(
        client.receive_shared("$thing/down/signaling/p/d", "invalid-json", ++now, 1'000'000));
    ASSERT_EQ(client.state(), HumanCallState::Failed);
    EXPECT_FALSE(client.receive_shared("$thing/down/signaling/p/d",
                                       changed_invite("next-token", "next-leg"), ++now, 1'000'000));
    EXPECT_EQ(client.state(), HumanCallState::Failed);
    EXPECT_EQ(mqtt.closes, 0U);
}
// 完整私有上下文必须经实际人际入口留存再确认，不能把合法message当非法信令断开通话。
TEST(HumanCallShared, CompletedContextMessageKeepsActualCallAndAcknowledgesReceipt) {
    SharedMqtt mqtt;
    SharedMedia media;
    HumanCallClient client(mqtt, media, "p/d", "context", {"entry", true});
    std::uint64_t now = 100;
    ASSERT_TRUE(client.start_shared(now));
    ReuseReceipts receipts;
    ASSERT_NO_FATAL_FAILURE(reuse_round(client, mqtt, media, "human-leg", "invite", now, receipts));
    Envelope snapshot;
    snapshot.request = false;
    snapshot.token = std::string(64, 'a');
    snapshot.method = "contextSnapshot";
    snapshot.session = "human-leg";
    snapshot.message_sequence = 1;
    snapshot.payload =
        R"({"deliveryId":")" + snapshot.token + R"(","consentId":")" + std::string(64, 'b') +
        R"(","scope":"completedText","sourceSessionId":"ai-source","serviceId":"human-support","serviceRevision":"r1","snapshotVersion":"v1","snapshotDigest":")" +
        std::string(64, 'c') +
        R"(","firstTurnIndex":1,"lastTurnIndex":1,"expiresAt":1020000,"turns":[{"messageId":"user-1","sequence":1,"turnIndex":1,"role":"user","text":"请帮我联系人工"},{"messageId":"assistant-1","sequence":2,"turnIndex":1,"role":"assistant","text":"好的"}]})";
    std::string body;
    ASSERT_EQ(encode_envelope(snapshot, body), DecodeResult::Ok);
    ASSERT_TRUE(client.receive_shared("$thing/down/message/p/d", body, ++now, 1'000'000));
    EXPECT_TRUE(client.active());
    EXPECT_TRUE(media.ready);
    ASSERT_EQ(mqtt.sent.back().method, "contextSnapshotAck");
    EXPECT_EQ(mqtt.sent.back().token, snapshot.token);
    EXPECT_EQ(mqtt.sent.back().session, "human-leg");
    ASSERT_TRUE(client.context_snapshot());
    ASSERT_EQ(client.context_snapshot()->turns.size(), 2U);
    EXPECT_EQ(client.context_snapshot()->turns[0].text, "请帮我联系人工");
    const auto original_ack = mqtt.bodies.back();
    mqtt.writable = false;
    EXPECT_FALSE(client.receive_shared("$thing/down/message/p/d", body, ++now, 1'000'001));
    EXPECT_TRUE(client.active());
    ASSERT_TRUE(client.context_snapshot());
    mqtt.writable = true;
    ASSERT_TRUE(client.receive_shared("$thing/down/message/p/d", body, ++now, 1'000'002));
    EXPECT_EQ(mqtt.bodies.back(), original_ack);
    ASSERT_NO_FATAL_FAILURE(reuse_deliver(client, reuse_end("human-leg"), now));
    EXPECT_FALSE(client.context_snapshot());
    EXPECT_EQ(client.state(), HumanCallState::Available);
    const auto sent = mqtt.sent.size();
    EXPECT_FALSE(client.receive_shared("$thing/down/message/p/d", body, ++now, 1'000'003));
    EXPECT_EQ(client.state(), HumanCallState::Available);
    EXPECT_EQ(mqtt.sent.size(), sent);
}
// 主叫创建成功会首次分配新会话ID；不能要求应答先匹配尚不存在的当前会话。
TEST(HumanCallShared, CreatingReplyRoutesOnlyTheOriginalPendingToken) {
    SharedMqtt mqtt;
    SharedMedia media;
    HumanCallClient client(mqtt, media, "p/d", "creating", {"entry", true});
    ASSERT_TRUE(client.start_shared(100));
    ASSERT_TRUE(client.dial("p:target", 101));
    ASSERT_EQ(mqtt.sent.back().method, "sessionCreate");
    ASSERT_TRUE(client.session_id().empty());
    Envelope reply;
    reply.request = false;
    reply.method = "sessionCreated";
    reply.token = mqtt.sent.back().token;
    reply.session = "new-caller-leg";
    reply.code = 200;
    EXPECT_TRUE(client.owns_shared_create_reply(reply));
    EXPECT_EQ(client.state(), HumanCallState::Inviting);
    EXPECT_EQ(media.preparations, 1U);  // 本人拨号已预约资源，应答路由检查不能再次预约或开始采集。
    EXPECT_FALSE(media.started);
    auto other = reply;
    other.token = "other-ipc-token";
    EXPECT_FALSE(client.owns_shared_create_reply(other));
    other = reply;
    other.request = true;
    EXPECT_FALSE(client.owns_shared_create_reply(other));
    other = reply;
    other.method = "sessionInvite";
    EXPECT_FALSE(client.owns_shared_create_reply(other));
    other = reply;
    other.code.reset();
    EXPECT_FALSE(client.owns_shared_create_reply(other));
    reply.session.clear();
    reply.code = 403;
    EXPECT_TRUE(client.owns_shared_create_reply(reply));
    client.close();
    EXPECT_FALSE(client.owns_shared_create_reply(reply));
}
}  // namespace ur::device
