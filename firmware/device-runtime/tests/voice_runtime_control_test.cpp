// 文件说明：运行时短MQTT断连必须重新精确SUBACK/能力ACK，且原AI会话、独立水位和期限不被重置。
#include <gtest/gtest.h>
#include <algorithm>
#include <deque>
#include "ur/device/voice_runtime.hpp"

namespace ur::device {
namespace {
// ControlMqtt 各次物理连接返回不同SUBACK操作号，旧连接事件不能推动新阶段。
struct ControlMqtt final : MqttPort {
    std::deque<MqttEvent> events;            // 明确事件到达顺序，不用睡眠猜测。
    std::vector<Envelope> sent;              // 原核心编码的实际控制请求。
    int next_mid = 1;                        // 单端口生命周期内保持唯一。
    unsigned connects = 0, disconnects = 0;  // 网络实际生命周期调用数。
    bool polling = true, connecting = true;  // 注入本地物理失败。
    bool connect(const MqttConfig&) override {
        ++connects;
        return connecting;
    }
    bool poll() override { return polling; }
    int subscribe(std::string_view) override { return next_mid++; }
    bool publish(std::string_view topic, std::string_view body) override {
        if (topic == "$thing/up/action/p/d")
            return true;  // 动作回执不是AI信封；副作用仍由原ActionEngine记录。
        Envelope message;
        EXPECT_EQ(decode_envelope(body, message), DecodeResult::Ok);
        sent.push_back(std::move(message));
        return true;
    }
    std::optional<MqttEvent> take_event() override {
        if (events.empty())
            return {};
        auto event = std::move(events.front());
        events.pop_front();
        return event;
    }
    void disconnect() override {
        ++disconnects;
        events.clear();
    }
};
// control_text 只有明确实现的None离散AI控制恢复，没有虚构音频硬件或媒体Ready。
VoiceCapability control_text() {
    VoiceCapability capability;
    capability.transport = VoiceTransport::None;
    capability.text_input = true;
    capability.session_sync = true;
    return capability;
}
// ControlActions 记录实际驱动调用，恢复快照前的旧连接动作不得进入执行队列。
struct ControlActions final : ActionDriver {
    unsigned begins = 0;  // 实际动作执行次数，不以收到报文当执行。
    bool begin(const ActionRequest&) override {
        ++begins;
        return true;
    }
    std::optional<ActionCompletion> take_completion() override { return {}; }
    bool cancel(std::string_view) override { return true; }
};
// RuntimeControlFixture 复用正式运行时与完整初始握手；恢复不以单独组件成功代替原会话。
struct RuntimeControlFixture : ::testing::Test {
    ControlMqtt mqtt;                     // 网络比借用运行时长寿。
    ControlActions action_driver;         // 长于执行器及运行时。
    ActionEngine actions{action_driver};  // 保留同一原幂等账本，不因重连重建。
    VoiceRuntime runtime;
    bool with_actions;  // 明确安装时增加第三路实际SUBACK。
    explicit RuntimeControlFixture(bool enabled = false)
        : runtime(mqtt, "p/d", control_text(), "control-runtime", enabled ? &actions : nullptr),
          with_actions(enabled) {}
    std::uint64_t now = 0;  // 单调测试毫秒。
    void drive() {
        ++now;
        runtime.poll(now, 10000 + now);
    }
    void event(MqttEvent::Kind kind, int mid = 0) {
        mqtt.events.push_back({kind, mid, 0, {}, {}});
        drive();
    }
    void deliver(Envelope envelope) {
        std::string body;
        ASSERT_EQ(encode_envelope(envelope, body), DecodeResult::Ok);
        mqtt.events.push_back({MqttEvent::Kind::Message, 0, 0, "$thing/down/signaling/p/d", body});
        drive();
    }
    void ack() {
        const auto request = mqtt.sent.back();
        Envelope envelope;
        envelope.token = request.token;
        envelope.session = request.session;
        envelope.method = "requestAck";
        envelope.request = false;
        envelope.code = 200;
        envelope.payload =
            "{\"requestMethod\":\"" + request.method + "\",\"result\":{\"acceptedRevision\":1}}";
        deliver(envelope);
    }
    void establish() {
        ASSERT_EQ(runtime.start({}, now), Result::Applied);
        event(MqttEvent::Kind::Connected);
        event(MqttEvent::Kind::Subscribed, 1);
        event(MqttEvent::Kind::Subscribed, 2);
        if (with_actions)
            event(MqttEvent::Kind::Subscribed, 3);
        ASSERT_EQ(mqtt.sent.back().method, "capabilityReport");
        ack();
        ASSERT_EQ(mqtt.sent.back().method, "sessionCreate");
        Envelope created;
        created.token = mqtt.sent.back().token;
        created.session = "original-ai";
        created.method = "sessionCreated";
        created.request = false;
        created.code = 200;
        created.payload =
            R"({"state":"negotiating","endpointEpoch":1,"capabilityRevision":1,"selected":{"transport":"none","inputModalities":["text"],"outputModalities":["text"]},"tracks":[]})";
        deliver(created);
        Envelope established;
        established.token = "established";
        established.session = "original-ai";
        established.method = "sessionEstablished";
        established.request = false;
        established.event_sequence = 1;
        established.payload = R"({"state":"established","mediaRevision":0})";
        deliver(established);
        ASSERT_EQ(runtime.voice().state(), VoiceState::Established);
    }
    void snapshot() {
        ASSERT_EQ(mqtt.sent.back().method, "sessionSync");
        Envelope snapshot;
        snapshot.token = mqtt.sent.back().token;
        snapshot.session = "original-ai";
        snapshot.method = "sessionSnapshot";
        snapshot.request = false;
        snapshot.code = 200;
        snapshot.event_sequence = 9;
        snapshot.payload =
            R"({"state":"established","revision":8,"eventWatermark":9,"messageWatermark":7,"endpointEpoch":2,"capabilityRevision":1,"mediaRevision":0,"expiresAt":20000,"resumeAllowed":true,"participants":[],"tracks":[],"controls":{"call":null,"focus":null,"floor":null,"recordings":[],"playbacks":[],"processors":[],"broadcasts":[],"handoffs":[],"mediaPlan":{"revision":0,"tracks":[]},"ai":{"inputId":"","inputState":"paused","activeRespIds":[]}}})";
        deliver(snapshot);
    }
};

TEST_F(RuntimeControlFixture, ReauthRequiresFreshSubacksAndDoesNotCreateAnotherSession) {
    establish();
    event(MqttEvent::Kind::Disconnected);
    ASSERT_EQ(runtime.state(), RuntimeState::ControlRecovering);
    EXPECT_EQ(runtime.voice().session_id(), "original-ai");
    ASSERT_EQ(runtime.resume_connection({}, ++now), Result::Applied);
    event(MqttEvent::Kind::Connected);
    const auto sent = mqtt.sent.size();
    event(MqttEvent::Kind::Subscribed, 1);
    event(MqttEvent::Kind::Subscribed, 2);
    EXPECT_EQ(mqtt.sent.size(), sent);
    event(MqttEvent::Kind::Subscribed, 3);
    EXPECT_EQ(mqtt.sent.size(), sent);
    event(MqttEvent::Kind::Subscribed, 4);
    ASSERT_EQ(mqtt.sent.back().method, "capabilityReport");
    ack();
    snapshot();
    EXPECT_EQ(runtime.voice().state(), VoiceState::Established);
    EXPECT_EQ(runtime.voice().session_id(), "original-ai");
    EXPECT_EQ(runtime.voice().control_message_watermark(), 7U);
    EXPECT_EQ(mqtt.connects, 2U);
    EXPECT_EQ(std::count_if(mqtt.sent.begin(), mqtt.sent.end(),
                            [](const auto& message) { return message.method == "sessionCreate"; }),
              1);
    EXPECT_EQ(runtime.send_text("恢复后新输入", false, ++now), Result::Applied);
    EXPECT_EQ(mqtt.sent.back().method, "inputSend");
}

TEST_F(RuntimeControlFixture, PollFailurePreservesOriginalUntilFixedDeadline) {
    establish();
    mqtt.polling = false;
    drive();
    ASSERT_EQ(runtime.state(), RuntimeState::ControlRecovering);
    EXPECT_EQ(runtime.voice().session_id(), "original-ai");
    now += 14999;
    runtime.poll(now, 10000 + now);
    EXPECT_EQ(runtime.state(), RuntimeState::ControlRecovering);
    ++now;
    runtime.poll(now, 10000 + now);
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
    EXPECT_TRUE(runtime.voice().session_id().empty());
    EXPECT_EQ(runtime.resume_connection({}, ++now), Result::Unsupported);
}

TEST_F(RuntimeControlFixture, UnconfirmedCreationCannotEnableRecovery) {
    ASSERT_EQ(runtime.start({}, now), Result::Applied);
    event(MqttEvent::Kind::Connected);
    event(MqttEvent::Kind::Subscribed, 1);
    event(MqttEvent::Kind::Subscribed, 2);
    mqtt.polling = false;
    drive();
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
    EXPECT_EQ(runtime.resume_connection({}, ++now), Result::Unsupported);
}

TEST_F(RuntimeControlFixture, ReconnectAttemptCannotExtendOriginalOfflineDeadline) {
    establish();
    event(MqttEvent::Kind::Disconnected);
    const auto disconnected_at = now;
    mqtt.connecting = false;
    EXPECT_EQ(runtime.resume_connection({}, ++now), Result::DriverFailed);
    EXPECT_EQ(runtime.state(), RuntimeState::ControlRecovering);
    mqtt.connecting = true;
    now = disconnected_at + 14999;
    EXPECT_EQ(runtime.resume_connection({}, now), Result::Applied);
    EXPECT_EQ(runtime.state(), RuntimeState::Connecting);
    ++now;
    runtime.poll(now, 10000 + now);
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
    EXPECT_TRUE(runtime.voice().session_id().empty());
    EXPECT_EQ(std::count_if(mqtt.sent.begin(), mqtt.sent.end(),
                            [](const auto& message) { return message.method == "sessionCreate"; }),
              1);
}

TEST_F(RuntimeControlFixture, UntrustedMessagesBeforeSnapshotCannotPoisonMessageWatermark) {
    establish();
    event(MqttEvent::Kind::Disconnected);
    ASSERT_EQ(runtime.resume_connection({}, ++now), Result::Applied);
    event(MqttEvent::Kind::Connected);
    event(MqttEvent::Kind::Subscribed, 3);
    event(MqttEvent::Kind::Subscribed, 4);
    mqtt.events.push_back({MqttEvent::Kind::Message, 0, 0, "$thing/down/message/p/d", "not-json"});
    drive();  // 未完成控制恢复时不消费旧MSG，也不让它使新握手失败。
    EXPECT_EQ(runtime.state(), RuntimeState::Running);
    EXPECT_TRUE(runtime.voice().control_recovering());
    ack();
    snapshot();
    EXPECT_EQ(runtime.voice().control_message_watermark(), 7U);
    EXPECT_FALSE(runtime.take_content());
    EXPECT_EQ(runtime.send_text("恢复后新输入", false, ++now), Result::Applied);
}

// ActionControlFixture 明确装配原动作通道，验证其不能绕控制恢复屏障。
struct ActionControlFixture : RuntimeControlFixture {
    ActionControlFixture() : RuntimeControlFixture(true) {}
};
TEST_F(ActionControlFixture, OldActionCannotExecuteBeforeAuthoritativeSnapshot) {
    establish();
    event(MqttEvent::Kind::Disconnected);
    ASSERT_EQ(runtime.resume_connection({}, ++now), Result::Applied);
    event(MqttEvent::Kind::Connected);
    event(MqttEvent::Kind::Subscribed, 4);
    event(MqttEvent::Kind::Subscribed, 5);
    event(MqttEvent::Kind::Subscribed, 6);
    const std::string action =
        R"({"method":"action","msgToken":"old-action","actionID":"setVolume","params":{"volume":40}})";
    mqtt.events.push_back({MqttEvent::Kind::Message, 0, 0, "$thing/down/action/p/d", action});
    drive();
    EXPECT_EQ(action_driver.begins, 0U);
    EXPECT_FALSE(actions.active());
    ack();
    snapshot();
    mqtt.events.push_back({MqttEvent::Kind::Message, 0, 0, "$thing/down/action/p/d", action});
    drive();
    EXPECT_EQ(action_driver.begins, 1U);
}
}  // namespace
}  // namespace ur::device
