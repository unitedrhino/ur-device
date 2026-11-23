// 文件说明：真实公共编解码／请求账本／借用业务入口的单连接装配回归；可控网络不冒称端云验收。
#include "ur/device/shared_device_connection.hpp"
#include <cJSON.h>
#include <gtest/gtest.h>
#include <deque>
#include <memory>
#include "ur/device/context_snapshot.hpp"
#include "ur/device/voice_runtime.hpp"

namespace ur::device {
namespace {
// ConnectionNetwork记录唯一网络控制调用，实际ACK由用例显式送达而非publish自动生成。
struct ConnectionNetwork final : MqttPort {
    unsigned connects = 0, disconnects = 0, reads = 0, polls = 0;
    bool connect_ok = true, poll_ok = true, publish_ok = true;
    int next_mid = 11;
    std::vector<std::string> topics, bytes;
    std::deque<MqttEvent> events;
    bool connect(const MqttConfig&) override {
        ++connects;
        return connect_ok;
    }
    int subscribe(std::string_view topic) override {
        topics.emplace_back(topic);
        return next_mid++;
    }
    bool poll() override {
        ++polls;
        return poll_ok;
    }
    std::optional<MqttEvent> take_event() override {
        ++reads;
        if (events.empty())
            return {};
        auto event = std::move(events.front());
        events.pop_front();
        return event;
    }
    bool publish(std::string_view, std::string_view body) override {
        bytes.emplace_back(body);
        return publish_ok;
    }
    void disconnect() override { ++disconnects; }
};
// IdleDriver仅注入用例物理观测；不能执行实际focus，不是生产默认闲置证明。
struct IdleDriver final : FocusDriver {
    std::optional<FocusIdleObservation> idle;
    std::uint64_t actual_revision = 0;
    std::uint64_t revision() const override { return actual_revision; }
    std::optional<FocusIdleObservation> initial_idle_observation() const override { return idle; }
    bool begin(const FocusCommand&, std::uint64_t) override { return false; }
    std::optional<FocusFact> take_fact(std::string_view, std::string_view) override { return {}; }
    bool owns(const FocusCommand&) const override { return false; }
    bool stop_scope(std::string_view, std::string_view, const std::vector<std::string>&) override {
        return false;
    }
};
// text_capability只测试共享网络和生产文字状态机，不假造音频设备或空音频驱动。
VoiceCapability text_capability() {
    VoiceCapability value;
    value.transport = VoiceTransport::None;
    value.text_input = true;
    return value;
}
// capability保留正式编码器生成的固定原快照，禁止手写成功状态替代编码检查。
Envelope capability() {
    Envelope out;
    EXPECT_EQ(capability_command("cap", text_capability(), out), WireResult::Ok);
    return out;
}
// message生成完整公共信封，非法字段另在测试输入注入，不跳过生产decode。
MqttEvent message(std::string method, std::string payload, std::string token = "cap",
                  std::string session = {}, int code = 0, bool request = false,
                  std::optional<std::uint64_t> sequence = {}) {
    Envelope envelope;
    envelope.token = std::move(token);
    envelope.method = std::move(method);
    envelope.session = std::move(session);
    envelope.payload = std::move(payload);
    envelope.request = request;
    envelope.event_sequence = sequence;
    if (code)
        envelope.code = code;
    std::string bytes;
    EXPECT_EQ(encode_envelope(envelope, bytes), DecodeResult::Ok);
    return {MqttEvent::Kind::Message, 0, 0, "$thing/down/signaling/p/d", std::move(bytes)};
}
// ConnectionTest沿真实Connected→三个独立SUBACK→原能力ACK建立共享入口。
class ConnectionTest : public testing::Test {
protected:
    ConnectionNetwork network;
    SharedDeviceConnection host{network, "p/d", capability(), 1000000};
    void subscribe() {
        ASSERT_TRUE(host.start({}, 100));
        network.events.push_back({MqttEvent::Kind::Connected, 0, 0, {}, {}});
        host.poll(101);
        for (int mid : {11, 12, 13})
            network.events.push_back({MqttEvent::Kind::Subscribed, mid, 0, {}, {}});
        host.poll(102);
        ASSERT_EQ(host.state(), SharedConnectionState::Reporting);
    }
    void ready() {
        subscribe();
        network.events.push_back(message(
            "requestAck", R"({"requestMethod":"capabilityReport","result":{"acceptedRevision":1}})",
            "cap", {}, 200));
        host.poll(103);
        ASSERT_EQ(host.state(), SharedConnectionState::Ready);
    }
};
// private_context 构造严格私有原消息；只用于受控网络，不是实际平台授权证明。
MqttEvent private_context() {
    Envelope envelope;
    envelope.token = std::string(64, 'a');
    envelope.method = "contextSnapshot";
    envelope.request = false;
    envelope.session = "human-private";
    envelope.message_sequence = 1;
    envelope.payload =
        R"({"deliveryId":")" + std::string(64, 'a') + R"(","consentId":")" + std::string(64, 'b') +
        R"(","scope":"completedText","sourceSessionId":"ai-source","serviceId":"support","serviceRevision":"r1","snapshotVersion":"v1","snapshotDigest":")" +
        std::string(64, 'c') +
        R"(","firstTurnIndex":1,"lastTurnIndex":1,"expiresAt":2000,"turns":[{"messageId":"u1","sequence":1,"turnIndex":1,"role":"user","text":"仅限本人授权正文"}]})";
    std::string bytes;  // 使用生产编码与解析双验，夹具不能靠非法报文通过拒绝断言。
    EXPECT_EQ(encode_envelope(envelope, bytes), DecodeResult::Ok);
    ContextSnapshot checked;
    EXPECT_EQ(decode_context_snapshot(WireRoute::Message, bytes, checked), WireResult::Ok);
    return {MqttEvent::Kind::Message, 0, 0, "$thing/down/message/p/d", std::move(bytes)};
}
TEST_F(ConnectionTest, PrivateQueuedCopiesExpireWithoutClosingReadyConnection) {
    ready();
    const auto request = message("sessionCreate", "{}", "human-create", {}, 0, true);
    ASSERT_TRUE(host.port(SharedMqttOwner::Human).publish("$thing/up/signaling/p/d", request.body));
    network.events.push_back(message("sessionCreated", "{}", "human-create", "human-private", 200));
    host.poll(104, 1000);
    ASSERT_TRUE(host.take_message(SharedMqttOwner::Human));
    ASSERT_EQ(
        host.router().confirm_session(SharedMqttOwner::Human,
                                      SharedSessionBinding{"human-create", "human-private"}, 104),
        SharedRouteResult::Routed);
    network.events.push_back(private_context());
    host.poll(105, 1001);
    EXPECT_GT(host.router().private_context_bytes(), 0U);
    host.poll(106, 2000);
    EXPECT_EQ(host.router().private_context_bytes(), 0U);
    EXPECT_FALSE(host.take_message(SharedMqttOwner::Human));
    EXPECT_FALSE(host.router().take_unroutable());
    EXPECT_EQ(host.state(), SharedConnectionState::Ready);
    EXPECT_EQ(network.disconnects, 0U);
}
TEST_F(ConnectionTest, PrivateBackpressureExpiryReleasesOriginalFrameBeforeReadingLaterEvent) {
    ready();
    const auto request = message("sessionCreate", "{}", "human-create", {}, 0, true);
    ASSERT_TRUE(host.port(SharedMqttOwner::Human).publish("$thing/up/signaling/p/d", request.body));
    network.events.push_back(message("sessionCreated", "{}", "human-create", "human-private", 200));
    host.poll(104, 1000);
    ASSERT_TRUE(host.take_message(SharedMqttOwner::Human));
    ASSERT_EQ(
        host.router().confirm_session(SharedMqttOwner::Human,
                                      SharedSessionBinding{"human-create", "human-private"}, 104),
        SharedRouteResult::Routed);
    for (unsigned n = 0; n < 32; ++n) {
        auto ordinary = message("sessionEstablished", "{}", "ordinary-" + std::to_string(n),
                                "human-private", 0, false, n + 1);
        ASSERT_EQ(host.router().ingest(ordinary.topic, ordinary.body, 104),
                  SharedRouteResult::Routed);
    }
    network.events.push_back(private_context());
    network.events.push_back({MqttEvent::Kind::Published, 1, 0, {}, {}});
    host.poll(105, 1001);
    ASSERT_EQ(network.events.size(), 1U);  // 原私有帧确实背压，后帧尚未被取走。
    host.poll(106, 2000);
    EXPECT_TRUE(network.events.empty());
    EXPECT_EQ(host.router().private_context_bytes(), 0U);
    EXPECT_EQ(host.state(), SharedConnectionState::Ready);
    EXPECT_EQ(network.disconnects, 0U);
    unsigned normal = 0;  // 普通主通话消息仍全部存在，不能以清全队列掩盖私有清理。
    while (const auto event = host.take_message(SharedMqttOwner::Human)) {
        EXPECT_NE(event->body.find("ordinary-"), std::string::npos);
        ++normal;
    }
    EXPECT_EQ(normal, 32U);
}
TEST_F(ConnectionTest, RequiresAllExactSubscriptionsBeforeSingleCapability) {
    ASSERT_TRUE(host.start({}, 100));
    network.events.push_back({MqttEvent::Kind::Connected, 0, 0, {}, {}});
    host.poll(101);
    EXPECT_EQ(network.topics,
              (std::vector<std::string>{"$thing/down/signaling/p/d", "$thing/down/message/p/d",
                                        "$thing/down/action/p/d"}));
    network.events.push_back({MqttEvent::Kind::Subscribed, 12, 0, {}, {}});
    network.events.push_back({MqttEvent::Kind::Subscribed, 11, 0, {}, {}});
    network.events.push_back({MqttEvent::Kind::Subscribed, 99, 0, {}, {}});
    host.poll(102);
    EXPECT_TRUE(network.bytes.empty());
    EXPECT_EQ(host.state(), SharedConnectionState::Subscribing);
    network.events.push_back({MqttEvent::Kind::Subscribed, 13, 0, {}, {}});
    network.events.push_back({MqttEvent::Kind::Subscribed, 13, 0, {}, {}});
    host.poll(103);
    ASSERT_EQ(network.bytes.size(), 1U);
    EXPECT_EQ(host.state(), SharedConnectionState::Reporting);
    EXPECT_FALSE(host.start({}, 104));
    EXPECT_EQ(network.connects, 1U);
}
TEST_F(ConnectionTest, PubackWrongTopicTokenAndMethodNeverGrantReady) {
    subscribe();
    auto ack = message("requestAck",
                       R"({"requestMethod":"capabilityReport","result":{"acceptedRevision":1}})",
                       "cap", {}, 200);
    auto wrong_topic = ack;
    wrong_topic.topic = "$thing/down/message/p/d";
    network.events.push_back(wrong_topic);
    network.events.push_back(message(
        "requestAck", R"({"requestMethod":"capabilityReport","result":{"acceptedRevision":1}})",
        "other", {}, 200));
    network.events.push_back(message(
        "requestAck", R"({"requestMethod":"sessionCreate","result":{"acceptedRevision":1}})", "cap",
        {}, 200));
    network.events.push_back({MqttEvent::Kind::Published, 1, 0, {}, {}});
    host.poll(103);
    EXPECT_EQ(host.state(), SharedConnectionState::Reporting);
    network.events.push_back(ack);
    host.poll(104);
    EXPECT_EQ(host.state(), SharedConnectionState::Ready);
}
TEST_F(ConnectionTest, RevisionConflictAndExplicitRejectionCloseOnlyOnce) {
    subscribe();
    network.events.push_back(message(
        "requestAck", R"({"requestMethod":"capabilityReport","result":{"acceptedRevision":2}})",
        "cap", {}, 200));
    host.poll(103);
    EXPECT_EQ(host.state(), SharedConnectionState::Failed);
    host.poll(104);
    host.close();
    host.close();
    EXPECT_EQ(network.disconnects, 1U);
}
TEST_F(ConnectionTest, DuplicateOriginalAckDoesNotRecreateBusinessOrPublishSnapshot) {
    ready();
    network.events.push_back(message(
        "requestAck", R"({"requestMethod":"capabilityReport","result":{"acceptedRevision":1}})",
        "cap", {}, 200));
    host.poll(104);
    EXPECT_EQ(host.state(), SharedConnectionState::Ready);
    EXPECT_EQ(network.bytes.size(), 1U);
}
TEST_F(ConnectionTest, CapabilityBackpressureRetriesOriginalBytesWithoutRenewal) {
    network.publish_ok = false;
    subscribe();
    host.poll(10102);
    host.poll(20102);
    ASSERT_EQ(network.bytes.size(), 3U);
    EXPECT_EQ(network.bytes[0], network.bytes[1]);
    EXPECT_EQ(network.bytes[0], network.bytes[2]);
    host.poll(30102);
    EXPECT_EQ(host.state(), SharedConnectionState::Failed);
    EXPECT_EQ(network.connects, 1U);
    EXPECT_EQ(network.disconnects, 1U);
}
TEST_F(ConnectionTest, EarlyInvitationWaitsForUnifiedAckAndExplicitBusinessBinding) {
    subscribe();
    network.events.push_back(
        message("sessionInvite", R"({"purpose":"humanCall"})", "invite", "human", 0, true, 1));
    host.poll(103);
    EXPECT_FALSE(host.take_message(SharedMqttOwner::Human));
    network.events.push_back(message(
        "requestAck", R"({"requestMethod":"capabilityReport","result":{"acceptedRevision":1}})",
        "cap", {}, 200));
    host.poll(104);
    auto invite = host.take_message(SharedMqttOwner::Human);
    ASSERT_TRUE(invite);
    EXPECT_EQ(invite->topic, "$thing/down/signaling/p/d");
    EXPECT_FALSE(host.take_message(SharedMqttOwner::AI));
    EXPECT_EQ(host.router().confirm_session(SharedMqttOwner::Human, {"invite", "human"}, 104),
              SharedRouteResult::Routed);
}
// SIG已收到消息但另两条SUBACK尚未到达时，合法邀请也必须保留原字节，而不是静默丢失。
TEST_F(ConnectionTest, InvitationBetweenIndependentSubacksIsNotDiscarded) {
    ASSERT_TRUE(host.start({}, 100));
    network.events.push_back({MqttEvent::Kind::Connected, 0, 0, {}, {}});
    network.events.push_back({MqttEvent::Kind::Subscribed, 11, 0, {}, {}});
    const auto original =
        message("sessionInvite", R"({"purpose":"humanCall"})", "invite", "human", 0, true, 1);
    network.events.push_back(original);
    host.poll(101);
    EXPECT_EQ(host.state(), SharedConnectionState::Subscribing);
    EXPECT_FALSE(host.take_message(SharedMqttOwner::Human));
    network.events.push_back({MqttEvent::Kind::Subscribed, 12, 0, {}, {}});
    network.events.push_back({MqttEvent::Kind::Subscribed, 13, 0, {}, {}});
    network.events.push_back(message(
        "requestAck", R"({"requestMethod":"capabilityReport","result":{"acceptedRevision":1}})",
        "cap", {}, 200));
    host.poll(102);
    ASSERT_EQ(host.state(), SharedConnectionState::Ready);
    const auto received = host.take_message(SharedMqttOwner::Human);
    ASSERT_TRUE(received);
    EXPECT_EQ(received->body, original.body);
    EXPECT_EQ(received->topic, original.topic);
}
TEST_F(ConnectionTest, ActualVoiceRuntimeUsesBorrowedPublisherNotSecondNetworkOwner) {
    ready();
    const auto original_reads = network.reads, original_polls = network.polls;
    VoiceRuntime ai{host.port(SharedMqttOwner::AI), "p/d", text_capability(), "shared-ai",
                    BorrowedVoiceConnection{}};
    EXPECT_EQ(ai.start_shared(104), Result::Applied);
    ASSERT_EQ(network.bytes.size(), 2U);
    Envelope created;
    ASSERT_EQ(decode_envelope(network.bytes.back(), created), DecodeResult::Ok);
    EXPECT_EQ(created.method, "sessionCreate");
    ai.poll(105, 1000000);
    ai.close(106);
    EXPECT_EQ(network.reads, original_reads);
    EXPECT_EQ(network.polls, original_polls);
    EXPECT_EQ(network.disconnects, 0U);
    EXPECT_EQ(network.connects, 1U);
    EXPECT_EQ(host.state(), SharedConnectionState::Ready);
}
TEST_F(ConnectionTest, FullRoutingQueueRetainsOriginalFrameAndDoesNotReadPastIt) {
    ready();
    for (unsigned i = 0; i < 34; ++i)
        network.events.push_back(message("sessionInvite", R"({"purpose":"humanCall"})",
                                         "invite-" + std::to_string(i),
                                         "human-" + std::to_string(i), 0, true, 1));
    host.poll(104);
    host.poll(105);
    EXPECT_EQ(network.events.size(), 1U);  // 32已入队，原第33保留，未读走第34。
    ASSERT_TRUE(host.take_message(SharedMqttOwner::Human));
    host.poll(106);
    EXPECT_TRUE(network.events.empty());  // 原33入队，第34成为唯一保留帧。
    std::vector<std::string> tokens;
    while (const auto event = host.take_message(SharedMqttOwner::Human)) {
        Envelope value;
        ASSERT_EQ(decode_envelope(event->body, value), DecodeResult::Ok);
        tokens.push_back(value.token);
    }
    host.poll(107);
    auto last = host.take_message(SharedMqttOwner::Human);
    ASSERT_TRUE(last);
    Envelope value;
    ASSERT_EQ(decode_envelope(last->body, value), DecodeResult::Ok);
    EXPECT_EQ(value.token, "invite-33");
    ASSERT_EQ(tokens.size(), 32U);
    EXPECT_EQ(tokens.back(), "invite-32");
}
TEST_F(ConnectionTest, ClockRollbackOrNetworkFailureCannotReopenOldConnection) {
    ready();
    host.poll(102);
    EXPECT_EQ(host.state(), SharedConnectionState::Failed);
    EXPECT_FALSE(host.start({}, 200));
    EXPECT_EQ(network.connects, 1U);
    EXPECT_EQ(network.disconnects, 1U);
}
TEST_F(ConnectionTest, SubscriptionDeadlineAndFailedConnectReleasePartialResources) {
    ASSERT_TRUE(host.start({}, 100));
    host.poll(10100);
    EXPECT_EQ(host.state(), SharedConnectionState::Failed);
    EXPECT_EQ(network.disconnects, 1U);
    ConnectionNetwork partial;
    partial.connect_ok = false;
    SharedDeviceConnection other{partial, "p/d", capability(), 20000};
    EXPECT_FALSE(other.start({}, 100));
    EXPECT_EQ(partial.disconnects, 1U);
}
// 传输异常只终止原网络，既不重连也不伪造任一腿的业务拒绝或媒体释放。
TEST_F(ConnectionTest, PollFailureDisconnectAndFailedSubackAreTerminal) {
    ready();
    network.poll_ok = false;
    host.poll(104);
    EXPECT_EQ(host.state(), SharedConnectionState::Failed);
    EXPECT_EQ(network.disconnects, 1U);
    for (const auto kind : {MqttEvent::Kind::Disconnected, MqttEvent::Kind::Failed}) {
        ConnectionNetwork local;
        SharedDeviceConnection original{local, "p/d", capability(), 10000};
        ASSERT_TRUE(original.start({}, 100));
        local.events.push_back({kind, 0, 1, {}, {}});
        original.poll(101);
        EXPECT_EQ(original.state(), SharedConnectionState::Failed);
        EXPECT_EQ(local.disconnects, 1U);
    }
    ConnectionNetwork local;
    SharedDeviceConnection original{local, "p/d", capability(), 10000};
    ASSERT_TRUE(original.start({}, 100));
    local.events.push_back({MqttEvent::Kind::Connected, 0, 0, {}, {}});
    local.events.push_back({MqttEvent::Kind::Subscribed, 12, 128, {}, {}});
    original.poll(101);
    EXPECT_EQ(original.state(), SharedConnectionState::Failed);
    EXPECT_TRUE(local.bytes.empty());
}
TEST_F(ConnectionTest, CapabilityRejectionAndIndependentHostExpiryNeverBecomeReady) {
    subscribe();
    network.events.push_back(
        message("requestAck", R"({"requestMethod":"capabilityReport","reason":"requestDenied"})",
                "cap", {}, 403));
    host.poll(103);
    EXPECT_EQ(host.state(), SharedConnectionState::Failed);
    ConnectionNetwork local;
    SharedDeviceConnection original{local, "p/d", capability(), 150};
    ASSERT_TRUE(original.start({}, 100));
    original.poll(150);
    EXPECT_EQ(original.state(), SharedConnectionState::Failed);
    EXPECT_FALSE(original.start({}, 151));
}
TEST_F(ConnectionTest, UnknownSubscriptionsAndMalformedPacketsNeverInventConfirmation) {
    subscribe();
    network.events.push_back({MqttEvent::Kind::Subscribed, 0, 0, {}, {}});
    network.events.push_back(
        {MqttEvent::Kind::Message, 0, 0, "$thing/down/signaling/p/d", "malformed"});
    host.poll(103);
    EXPECT_EQ(host.state(), SharedConnectionState::Reporting);
    EXPECT_TRUE(network.bytes.size() == 1);
    EXPECT_FALSE(host.take_message(SharedMqttOwner::Global));
}
TEST_F(ConnectionTest, RetainedFullQueueExpiresWithoutDroppingLaterNetworkFrame) {
    ready();
    for (unsigned i = 0; i < 34; ++i)
        network.events.push_back(message("sessionInvite", R"({"purpose":"humanCall"})",
                                         "invite-" + std::to_string(i),
                                         "human-" + std::to_string(i), 0, true, 1));
    host.poll(104);
    host.poll(105);
    ASSERT_EQ(network.events.size(), 1U);
    host.poll(5105);
    EXPECT_EQ(host.state(), SharedConnectionState::Failed);
    EXPECT_EQ(network.events.size(), 1U);
    EXPECT_EQ(network.disconnects, 1U);
}
TEST(SharedHandoffCapability, ActualInitialIdleAndCommonFormatAreMandatory) {
    IdleDriver driver;
    VoiceCapability voice;
    voice.audio = {48000, 1, 20};
    voice.modes = {true, true, true};
    Envelope out;
    EXPECT_EQ(handoff_capability_command("cap", voice, driver, out), WireResult::Invalid);
    driver.idle = FocusIdleObservation{{"microphone", "speaker"}, 0};
    ASSERT_EQ(handoff_capability_command("cap", voice, driver, out), WireResult::Ok);
    EXPECT_EQ(out.method, "capabilityReport");
    EXPECT_TRUE(out.session.empty());
    EXPECT_NE(out.payload.find("humanCall"), std::string::npos);
    EXPECT_NE(out.payload.find("localAccept"), std::string::npos);
    EXPECT_NE(out.payload.find("webrtc"), std::string::npos);
    voice.audio.sample_rate = 16000;
    EXPECT_EQ(handoff_capability_command("cap", voice, driver, out), WireResult::Invalid);
    EXPECT_TRUE(out.token.empty());
}
TEST(SharedHandoffCapability, PrivateTextOutputNeverInventsActiveTextInput) {
    IdleDriver driver;
    driver.idle = FocusIdleObservation{{"microphone", "speaker"}, 0};
    VoiceCapability voice;
    voice.audio = {48000, 1, 20};
    voice.modes = {true, true, true};
    for (const bool active_text : {false, true}) {
        SCOPED_TRACE(active_text);
        voice.text_input = active_text;
        Envelope out;
        ASSERT_EQ(handoff_capability_command("cap", voice, driver, out, !active_text),
                  WireResult::Ok);
        const std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_Parse(out.payload.c_str()),
                                                                   cJSON_Delete);
        ASSERT_TRUE(root);
        const auto* inputs = cJSON_GetObjectItemCaseSensitive(root.get(), "inputModalities");
        const auto* outputs = cJSON_GetObjectItemCaseSensitive(root.get(), "outputModalities");
        // 次数核验同时拒绝重复声明；被动私有文本能力不能伪装成主动文字输入能力。
        const auto count = [](const cJSON* array, std::string_view modality) {
            unsigned matches = 0;
            if (cJSON_IsArray(array))
                for (const auto* item = array->child; item; item = item->next)
                    if (cJSON_IsString(item) && modality == item->valuestring)
                        ++matches;
            return matches;
        };
        // audio结构才是音频能力；当前inputModalities枚举只有可选text，不能发明audio枚举。
        EXPECT_EQ(count(inputs, "audio"), 0U);
        EXPECT_EQ(count(inputs, "text"), active_text ? 1U : 0U);
        EXPECT_EQ(count(outputs, "audio"), 1U);
        EXPECT_EQ(count(outputs, "text"), 1U);
        const auto* purposes = cJSON_GetObjectItemCaseSensitive(root.get(), "purposes");
        const auto* transports = cJSON_GetObjectItemCaseSensitive(root.get(), "transports");
        EXPECT_EQ(count(purposes, "humanCall"), 1U);
        EXPECT_EQ(count(purposes, "aiConversation"), active_text ? 1U : 0U);
        EXPECT_EQ(count(transports, "webrtc"), 1U);
        EXPECT_EQ(count(transports, "udp"), active_text ? 1U : 0U);
        EXPECT_EQ(cJSON_HasObjectItem(root.get(), "aecMode"), active_text);
        EXPECT_EQ(cJSON_HasObjectItem(root.get(), "supportedInteractionModes"), active_text);
    }
}
TEST(SharedHandoffCapability, HumanOnlyCannotClaimAnActiveTextInput) {
    IdleDriver driver;
    driver.idle = FocusIdleObservation{{"microphone", "speaker"}, 0};
    VoiceCapability voice;
    voice.audio = {48000, 1, 20};
    voice.text_input = true;
    Envelope out;
    EXPECT_EQ(handoff_capability_command("cap", voice, driver, out, true), WireResult::Invalid);
    EXPECT_TRUE(out.token.empty());
}
TEST(SharedHandoffCapability, PartialDuplicateStaleOrExtraResourceCannotAdvertiseBaseline) {
    IdleDriver driver;
    VoiceCapability voice;
    voice.audio = {48000, 1, 20};
    for (const auto& resources : std::vector<std::vector<std::string>>{
             {"speaker"}, {"speaker", "speaker"}, {"microphone", "speaker", "display"}}) {
        driver.idle = FocusIdleObservation{resources, 0};
        Envelope out;
        EXPECT_EQ(handoff_capability_command("cap", voice, driver, out), WireResult::Invalid);
    }
    driver.idle = FocusIdleObservation{{"microphone", "speaker"}, 1};
    Envelope out;
    EXPECT_EQ(handoff_capability_command("cap", voice, driver, out), WireResult::Invalid);
    driver.actual_revision = 1;
    EXPECT_EQ(handoff_capability_command("cap", voice, driver, out), WireResult::Ok);
}
TEST(SharedConnectionValidation, InvalidIdentitySnapshotOrExpiredStartNeverConnects) {
    ConnectionNetwork network;
    SharedDeviceConnection wildcard{network, "p/+", capability(), 10000};
    EXPECT_FALSE(wildcard.start({}, 100));
    auto malformed = capability();
    malformed.payload = R"({"revision":0})";
    SharedDeviceConnection invalid{network, "p/d", malformed, 10000};
    EXPECT_FALSE(invalid.start({}, 100));
    SharedDeviceConnection expired{network, "p/d", capability(), 100};
    EXPECT_FALSE(expired.start({}, 100));
    EXPECT_EQ(network.connects, 0U);
    EXPECT_EQ(network.disconnects, 0U);
}
TEST_F(ConnectionTest, ConnectedErrorAndDuplicateNeverRepeatSubscriptions) {
    ASSERT_TRUE(host.start({}, 100));
    network.events.push_back({MqttEvent::Kind::Connected, 0, 128, {}, {}});
    host.poll(101);
    EXPECT_EQ(host.state(), SharedConnectionState::Failed);
    EXPECT_TRUE(network.topics.empty());
    EXPECT_EQ(network.disconnects, 1U);
    host.close();
    EXPECT_EQ(network.disconnects, 1U);
    ConnectionNetwork original;
    SharedDeviceConnection second{original, "p/d", capability(), 10000};
    ASSERT_TRUE(second.start({}, 100));
    original.events.push_back({MqttEvent::Kind::Connected, 0, 0, {}, {}});
    second.poll(101);
    ASSERT_EQ(original.topics.size(), 3U);
    original.events.push_back({MqttEvent::Kind::Connected, 0, 0, {}, {}});
    second.poll(102);
    EXPECT_EQ(second.state(), SharedConnectionState::Failed);
    EXPECT_EQ(original.topics.size(), 3U);
    EXPECT_EQ(original.disconnects, 1U);
}
TEST_F(ConnectionTest, EarlyMessageBeforeConnectedCannotInventSubscriptionOrConfirmation) {
    ASSERT_TRUE(host.start({}, 100));
    network.events.push_back(message(
        "requestAck", R"({"requestMethod":"capabilityReport","result":{"acceptedRevision":1}})",
        "cap", {}, 200));
    host.poll(101);
    EXPECT_EQ(host.state(), SharedConnectionState::Connecting);
    EXPECT_TRUE(network.bytes.empty());
    EXPECT_FALSE(host.take_message(SharedMqttOwner::Global));
    EXPECT_TRUE(network.topics.empty());
}
TEST_F(ConnectionTest, NonPositiveSubscribeIdFailsWithoutPublishingCapabilities) {
    network.next_mid = 0;
    ASSERT_TRUE(host.start({}, 100));
    network.events.push_back({MqttEvent::Kind::Connected, 0, 0, {}, {}});
    host.poll(101);
    EXPECT_EQ(host.state(), SharedConnectionState::Failed);
    EXPECT_EQ(network.topics.size(), 1U);
    EXPECT_TRUE(network.bytes.empty());
    EXPECT_EQ(network.disconnects, 1U);
}
}  // namespace
}  // namespace ur::device
