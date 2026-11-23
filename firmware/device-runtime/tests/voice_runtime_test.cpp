// 运行时组合测试：可控时间及平台驱动验证订阅门控、跨通道内容隔离和统一故障回收。
#include "ur/device/voice_runtime.hpp"
#include <gtest/gtest.h>
#include <deque>

namespace ur::device {
namespace {
// RuntimeMqtt 只模拟网络事件，不绕过实际报文编码或 VoiceClient 状态机。
struct RuntimeMqtt final : MqttPort {
    bool connecting = true, polling = true,
         publishing = true;                   // 逐项注入驱动失败。
    int next_subscription = 10;               // 单调网络操作号。
    int disconnects = 0;                      // 资源释放调用计数。
    bool drained = true;                      // 测试明确注入网络PUBACK排空，与本地发布受理独立。
    std::vector<std::string> subscriptions;   // 精确已订阅路由。
    std::vector<Envelope> sent;               // 实际发出的协议请求。
    std::vector<std::string> sent_topics;     // 与协议请求一一对应的发布路由。
    std::vector<std::string> action_replies;  // 独立物模型回执，不按信令信封解析。
    std::deque<MqttEvent> events;             // 测试可控的到达顺序。
    bool connect(const MqttConfig&) override { return connecting; }
    bool poll() override { return polling; }
    // publications_drained 仅交付用例网络事实，实际Mosquitto另有真实Broker回归。
    bool publications_drained() override { return drained; }
    int subscribe(std::string_view topic) override {
        subscriptions.emplace_back(topic);
        return next_subscription > 0 ? next_subscription++ : 0;
    }
    bool publish(std::string_view topic, std::string_view body) override {
        if (topic == "$thing/up/action/p/d") {
            action_replies.emplace_back(body);
            return publishing;
        }
        Envelope event;
        EXPECT_EQ(decode_envelope(body, event), DecodeResult::Ok);
        sent.push_back(event);
        sent_topics.emplace_back(topic);
        return publishing;
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
// RuntimeMedia 保留输入/输出事实，不将 configure 或 poll 当成默认媒体就绪。
struct RuntimeMedia final : PumpedVoiceMediaPort {
    bool configured = false, connected = false,
         polling = true;                     // 分离分配和连通事实。
    bool capture = false, playback = false;  // 实际驱动启停状态。
    int polls = 0;                           // 用于验证媒体未被网络饿死。
    std::optional<std::uint64_t> drained;    // 可注入迟到播放回调。
    bool disconnected = false;               // 已建立Peer的暂断，不等同永久poll失败。
    std::uint64_t peer_revision = 1;         // 每次实际恢复创建下一代Peer。
    std::optional<std::string> offer;        // 候选收集完成后的驱动事实。
    std::string input;                       // 真实采集操作使用的原输入ID。
    bool configure(const VoiceMedia& media) override {
        configured = true;
        if (media.transport == VoiceTransport::WebRTC) offer = "v=0\r\no=first\r\n";
        return true;
    }
    // begin_recovery不伪造连通，测试随后独立交付connected事实。
    bool begin_recovery(std::uint64_t revision) override {
        peer_revision = revision;
        disconnected = connected = false;
        offer = "v=0\r\no=next\r\n";
        return true;
    }
    std::optional<std::string> take_offer() override {
        auto result = std::move(offer);
        offer.reset();
        return result;
    }
    bool apply_answer(std::string_view) override { return true; }
    bool recovery_needed() const override { return disconnected; }
    std::uint32_t next_input_sequence() const override { return 128; }
    bool prepare_reply(const std::array<std::uint8_t, 8>&, std::uint32_t) override { return true; }
    bool finish_reply(std::uint32_t) override { return true; }
    bool start_capture(std::string_view id) override {
        input = std::string(id);
        capture = true;
        return true;
    }
    void stop_capture() override { capture = false; }
    bool start_playback(std::string_view, std::uint64_t) override {
        playback = true;
        return true;
    }
    void stop_playback(std::uint64_t) override { playback = false; }
    void close() override {
        configured = connected = capture = playback = false;
        disconnected = false;
        offer.reset();
        drained.reset();
    }
    bool poll(std::uint64_t) override {
        ++polls;
        return polling;
    }
    bool ready() const override { return connected; }
    std::optional<std::uint64_t> drained_epoch() const override { return drained; }
};
// RuntimeActions 只交付测试驱动完成事实，启动不会自动伪造成功。
struct RuntimeActions final : ActionDriver {
    unsigned begins = 0;                     // 验证动作实际启动次数。
    unsigned cancels = 0;                    // 验证断网取消。
    std::deque<ActionCompletion> completed;  // 由测试显式确认的执行结果。
    bool begin(const ActionRequest&) override {
        ++begins;
        return true;
    }
    std::optional<ActionCompletion> take_completion() override {
        if (completed.empty())
            return std::nullopt;
        auto result = std::move(completed.front());
        completed.pop_front();
        return result;
    }
    bool cancel(std::string_view) override {
        ++cancels;
        return true;
    }
};
// RuntimeFileDriver 的逻辑关闭与实际释放分离；没有就绪或播放的默认成功事实。
struct RuntimeFileDriver final : PlaybackDriver {
    bool* released;  // 借用夹具实际关闭事实，驱动由端点持有。
    // 构造仅绑定测试事实，不能在构造时伪造释放。
    explicit RuntimeFileDriver(bool& value) : released(&value) {}
    // begin 此夹具只测试分派/释放，不模拟实际播放，因此拒绝控制执行。
    bool begin(const PlaybackCommand&, std::uint64_t) override { return false; }
    // take_fact 没有实际解码器就不能产生播放事实。
    std::optional<PlaybackFact> take_fact() override { return {}; }
    // output_permitted 用例显式提供测试许可，不代表业务源/焦点已经授权。
    bool output_permitted() const override { return true; }
    // stop_all 只返回独立测试事实，不把取消受理当实际释放。
    bool stop_all() override { return *released; }
};
// RuntimeFileAdmission 准备异步且仅借用当前测试事实，不能以取消受理假报资源释放。
struct RuntimeFileAdmission final : PlaybackAdmissionPort {
    bool ready = false, released = false;  // 两种独立驱动事实。
    unsigned calls = 0, cancels = 0;       // 重连和重投不能重新准备旧实例。
    // authorize 固定原测试许可，未ready只返回原准备，不创建第二驱动。
    PlaybackGrant authorize(const PlaybackInvite&, std::uint64_t) override {
        ++calls;
        if (!ready)
            return {nullptr, 100000, true};
        return {std::make_unique<RuntimeFileDriver>(released), 100000};
    }
    // cancel 请求原准备取消，实际释放由用例随后独立确认。
    bool cancel(std::string_view, std::uint64_t) override {
        ++cancels;
        return released;
    }
};

// RuntimeFocusAuthority 明确注入离线许可，非真实业务默认授权。
struct RuntimeFocusAuthority final : FocusAuthority {
    unsigned calls = 0;  // 重放不重新许可。
    std::optional<FocusPermit> authorize(const FocusCommand& command, std::uint64_t,
                                         std::uint64_t) override {
        ++calls;
        return FocusPermit{command, 50000, 1};
    }
    bool valid(const FocusPermit&, std::uint64_t, std::uint64_t) const override { return true; }
};
// RuntimeFocusDriver 只在显式complete后确认整组，停止未知保留至用例实际确认。
struct RuntimeFocusDriver final : FocusDriver {
    FocusCommand original;                      // 固定原驱动请求。
    std::uint64_t generation = 0, current = 0;  // 实际代次及修订号。
    unsigned starts = 0, stops = 0;             // 验证控制与采集独立。
    bool held = false, released = false;        // 独立实际占用与停止事实。
    std::deque<FocusFact> facts;                // 显式整组完成，不是网络确认。
    std::uint64_t revision() const override { return current; }
    bool begin(const FocusCommand& command, std::uint64_t value) override {
        ++starts;
        original = command;
        generation = value;
        return true;
    }
    std::optional<FocusFact> take_fact(std::string_view session, std::string_view focus) override {
        if (facts.empty() || session != original.envelope.session || focus != original.focus_id)
            return {};
        auto result = facts.front();
        facts.pop_front();
        return result;
    }
    bool owns(const FocusCommand& command) const override {
        return held && command.envelope.session == original.envelope.session &&
               command.focus_id == original.focus_id;
    }
    bool stop_scope(std::string_view session, std::string_view focus,
                    const std::vector<std::string>& resources) override {
        ++stops;
        EXPECT_EQ(session, "file-child");
        EXPECT_EQ(focus, "lease");
        EXPECT_EQ(resources, (std::vector<std::string>{"speaker", "display"}));
        if (released)
            held = false;
        return released;
    }
    // complete 显式模拟实际全组取得；用例不把该夹具称为真实媒体驱动。
    void complete() {
        held = true;
        current = original.expected_revision + 1;
        facts.push_back({generation, true, "acquired", original.resources, current});
    }
};

// RuntimeLiveEndpoint 仅显式模拟借用端点事实，不创建假Peer或伪造节目实际输出。
struct RuntimeLiveEndpoint final : LiveRuntimePort {
    bool matching = true, released_fact = true,
         settled_fact = true;                         // 用例独立注入原实际关闭事实。
    unsigned connections = 0, ticks = 0, closes = 0;  // 生命周期与同连接推进调用观察。
    std::vector<Envelope> received;  // 仅用例已解码原子会话报文，不保存真实媒体或凭据。
    bool matches(PublishPort&, std::string_view up, std::string_view down) const override {
        return matching && up == "$thing/up/signaling/p/d" && down == "$thing/down/signaling/p/d";
    }
    bool begin_connection(std::uint64_t) override {
        ++connections;
        return released_fact && settled_fact;
    }
    bool owns(std::string_view session) const override { return session == "live-child"; }
    bool receive(std::string_view, std::string_view body, std::uint64_t, std::uint64_t) override {
        Envelope envelope;
        if (decode_envelope(body, envelope) != DecodeResult::Ok)
            return false;
        received.push_back(envelope);
        return true;
    }
    void tick(std::uint64_t, std::uint64_t) override { ++ticks; }
    void close(std::uint64_t) override { ++closes; }
    bool released() const override { return released_fact; }
    bool settled() const override { return settled_fact; }
};
// RuntimeTest 通过同一MQTT事件泵和正式状态机协商，不直接写状态。
class RuntimeTest : public testing::Test {
protected:
    RuntimeMqtt mqtt;
    RuntimeMedia media;
    RuntimeActions action_driver;
    ActionEngine actions{action_driver};
    RuntimeFileAdmission file_admission;            // 生命周期长于运行时及其借用的文件驱动。
    RuntimeFocusDriver focus_driver;                // 明确共享焦点驱动夹具。
    RuntimeFocusAuthority focus_authority;          // 明确许可夹具，不用于生产默认授权。
    RuntimeLiveEndpoint live_endpoint;              // 生命周期长于借用它的VoiceRuntime。
    std::unique_ptr<FocusEndpoint> focus_endpoint;  // 比运行时先构造、后销毁。
    VoiceRuntime runtime;
    bool with_actions;  // 默认语音测试保持两Topic，新动作测试显式启用第三路由。
    bool with_control;  // 源端接续夹具必须明确协商sessionSync及创建绑定，不升级旧用例。
    RuntimeTest() : RuntimeTest(false) {}
    explicit RuntimeTest(bool enabled, bool files = false, bool focus = false, bool live = false,
                         VoiceTransport transport = VoiceTransport::Udp, bool full = true,
                         bool control = false)
        : focus_endpoint(focus ? std::make_unique<FocusEndpoint>(
                                     mqtt, focus_driver, focus_authority,
                                     std::vector<std::string>{"speaker", "display"},
                                     "$thing/up/signaling/p/d", "$thing/down/signaling/p/d", 100000)
                               : nullptr),
          runtime{mqtt,
                  media,
                  "p/d",
                  [&] {
                    VoiceCapability capability{{true, full, full},
                   {transport == VoiceTransport::WebRTC ? 48000 : 16000, 1, 20},
                   1,
                   true,
                   transport,
                   files,
                   files,
                   live,
                   live};
                    capability.session_sync = control;
                    return capability;
                  }(),
                  "boot",
                  enabled ? &actions : nullptr,
                  files ? &file_admission : nullptr,
                  focus_endpoint.get(),
                  live ? &live_endpoint : nullptr},
          with_actions(enabled), with_control(control) {}
    std::uint64_t now = 1;  // 单调虚拟毫秒，不需要等待真实超时。
    // drive 同时提供独立墙钟，不能把Unix截止当单调媒体寿命。
    void drive() {
        ++now;
        runtime.poll(now, 1000000 + now);
    }
    void network(MqttEvent::Kind kind, int operation = 0, int code = 0) {
        mqtt.events.push_back({kind, operation, code, {}, {}});
        drive();
    }
    void body(Envelope event, std::string topic = "$thing/down/signaling/p/d") {
        std::string json;
        ASSERT_EQ(encode_envelope(event, json), DecodeResult::Ok);
        mqtt.events.push_back({MqttEvent::Kind::Message, 0, 0, std::move(topic), json});
        drive();
    }
    void ack_last(std::string result) {
        const auto request = mqtt.sent.back();
        Envelope ack;
        ack.token = request.token;
        ack.method = "requestAck";
        ack.session = request.session;
        ack.request = false;
        ack.code = 200;
        ack.payload = "{\"requestMethod\":\"" + request.method + "\",\"result\":" + result + "}";
        body(ack);
    }
    void subscribed() {
        ASSERT_EQ(runtime.start({}, now), Result::Applied);
        network(MqttEvent::Kind::Connected);
        network(MqttEvent::Kind::Subscribed, 11);
        ASSERT_TRUE(mqtt.sent.empty());
        network(MqttEvent::Kind::Subscribed, 10);
        if (with_actions) {
            ASSERT_TRUE(mqtt.sent.empty());
            network(MqttEvent::Kind::Subscribed, 12);
        }
        ASSERT_EQ(mqtt.sent.back().method, "capabilityReport");
    }
    void established(bool full = true) {
        subscribed();
        ack_last(R"({"acceptedRevision":1})");
        ASSERT_EQ(mqtt.sent.back().method, "sessionCreate");
        Envelope created;
        created.token = mqtt.sent.back().token;
        created.method = "sessionCreated";
        created.session = "session";
        created.request = false;
        created.code = 200;
        created.payload =
            R"({"state":"negotiating","selected":{"transport":"udp","interactionMode":"fullDuplex","audioParams":{"format":"opus","sampleRate":16000,"channels":1,"frameDuration":20},"inputModalities":["text"],"outputModalities":["text","audio"]},"tracks":[{"trackId":"mic-1","kind":"audio","sourceId":"mic","direction":"sendonly","codec":"opus","state":"requested"},{"trackId":"speaker-1","kind":"audio","sourceId":"speaker","direction":"recvonly","codec":"opus","state":"requested"}],"media":{"udp":{"server":"127.0.0.1","port":12345,"key":"000102030405060708090a0b0c0d0e0f","nonce":"01000000112233445566778800000000"}}})";
        if (with_control)
            created.payload.insert(1, R"("endpointEpoch":1,"capabilityRevision":1,)");
        if (!full) created.payload.replace(created.payload.find("fullDuplex"), 10, "halfDuplex");
        body(created);
        ASSERT_TRUE(media.configured);
        ASSERT_FALSE(media.capture);
        media.connected = true;
        drive();
        ASSERT_EQ(mqtt.sent.back().method, "mediaState");
        ack_last(R"({"mediaRevision":1,"state":"connected"})");
        Envelope event;
        event.token = "established";
        event.method = "sessionEstablished";
        event.session = "session";
        event.request = false;
        event.event_sequence = 1;
        event.payload = R"({"state":"established","mediaRevision":1})";
        body(event);
        ASSERT_EQ(runtime.voice().state(), VoiceState::Established);
    }
    Envelope content(std::uint64_t seq, std::string session = "session") {
        Envelope event;
        event.token = "text";
        event.method = "respTextDone";
        event.session = std::move(session);
        event.request = false;
        event.message_sequence = seq;
        event.payload = R"({"respId":"r","contentIndex":0,"text":"你好"})";
        return event;
    }
};
TEST_F(RuntimeTest, ContentReplyCanBeCancelledBeforeAnyUdpAudioWithoutStoppingCapture) {
    established();
    ASSERT_EQ(runtime.start_input(++now), Result::Applied);
    ack_last(mqtt.sent.back().payload);
    ASSERT_TRUE(media.capture);
    auto created = content(1);
    created.method = "respCreated";
    created.payload = R"({"respId":"pending","inputId":"input"})";
    body(created, "$thing/down/message/p/d");
    ASSERT_TRUE(runtime.take_content());
    EXPECT_EQ(runtime.response_control_target(), "pending");
    EXPECT_TRUE(runtime.voice().response_id().empty());
    ASSERT_EQ(runtime.cancel_response(++now), Result::Applied);
    EXPECT_EQ(mqtt.sent.back().method, "respCancel");
    EXPECT_EQ(mqtt.sent.back().payload, R"({"respId":"pending"})");
    EXPECT_TRUE(runtime.response_control_target().empty());
    EXPECT_TRUE(media.capture);
    EXPECT_FALSE(media.playback);
    ack_last(R"({"respId":"pending"})");  // 原音频后端ACK，不把受理推成回复终态。
    // 原取消后的SIG音频开始和MSG文本都不能复活输出，输入仍持续。
    auto audio = created;
    audio.message_sequence.reset();
    audio.event_sequence = 2;
    audio.method = "respAudioStart";
    audio.payload =
        R"({"respId":"pending","trackId":"speaker-1","mediaRevision":1,"udpNonce":"9988776655443322","firstPacketSeq":128})";
    body(audio);
    auto late = content(2);
    late.payload = R"({"respId":"pending","contentIndex":0,"text":"迟到"})";
    body(late, "$thing/down/message/p/d");
    EXPECT_FALSE(runtime.take_content());
    EXPECT_FALSE(media.playback);
    EXPECT_TRUE(media.capture);
    EXPECT_EQ(runtime.voice().state(), VoiceState::Established);
    EXPECT_EQ(runtime.cancel_response(++now), Result::Stale);
}

TEST_F(RuntimeTest, NormalContentTerminalDoesNotPretendUdpAudioWasCancelled) {
    established();
    auto created = content(1);
    created.method = "respCreated";
    created.payload = R"({"respId":"normal","inputId":"input"})";
    body(created, "$thing/down/message/p/d");
    ASSERT_TRUE(runtime.take_content());
    auto done = content(2);
    done.method = "respDone";
    done.payload =
        R"({"respId":"normal","inputId":"input","contentCount":0,"status":"completed","audioState":"sent"})";
    body(done, "$thing/down/message/p/d");
    ASSERT_TRUE(runtime.take_content());
    EXPECT_TRUE(runtime.response_control_target().empty());
    auto audio = created;
    audio.message_sequence.reset();
    audio.event_sequence = 2;
    audio.method = "respAudioStart";
    audio.payload =
        R"({"respId":"normal","trackId":"speaker-1","mediaRevision":1,"udpNonce":"9988776655443322","firstPacketSeq":128})";
    body(audio);
    EXPECT_TRUE(media.playback);  // MQTT跨Topic的正常迟到音频仍须播放，不把发送完成当播放完成。
    EXPECT_EQ(runtime.voice().state(), VoiceState::Established);
}

TEST_F(RuntimeTest, OldContentTerminalAndWrongSessionCannotRetireCurrentControlTarget) {
    established();
    auto created = content(1, "other-session");
    created.method = "respCreated";
    created.payload = R"({"respId":"old","inputId":"input"})";
    body(created, "$thing/down/message/p/d");
    EXPECT_TRUE(runtime.response_control_target().empty());
    created.session = "session";
    body(created, "$thing/down/message/p/d");
    ASSERT_TRUE(runtime.take_content());
    created.message_sequence = 2;
    created.payload = R"({"respId":"new","inputId":"input"})";
    body(created, "$thing/down/message/p/d");
    ASSERT_TRUE(runtime.take_content());
    auto done = content(3);
    done.method = "respDone";
    done.payload =
        R"({"respId":"old","inputId":"input","contentCount":0,"status":"completed","audioState":"none"})";
    body(done, "$thing/down/message/p/d");
    ASSERT_TRUE(runtime.take_content());
    EXPECT_EQ(runtime.response_control_target(), "new");
}

// FocusRuntimeTest 通过真实共享运行时分派夹具事实，不直接调用FocusEngine。
class FocusRuntimeTest : public RuntimeTest {
protected:
    FocusRuntimeTest() : RuntimeTest(false, false, true) {}
    // focus 把原云端命令送入与AI共用的精确MQTT事件队列。
    void focus(std::string topic = "$thing/down/signaling/p/d") {
        mqtt.events.push_back(
            {MqttEvent::Kind::Message, 0, 0, std::move(topic),
             R"({"msgToken":"focus-token","method":"focusControl","sessionId":"file-child","eventSeq":1,"params":{"operation":"acquire","expectedRevision":0,"focusId":"lease","resources":["speaker","display"],"priority":"conversation","resumePolicy":"explicit","expiresAt":1010000}})"});
        drive();
    }
    // terminal 通过原AI会话终态触发共享收尾，焦点子会话不得被当成AI父ID。
    void terminal() {
        Envelope event;
        event.token = "parent-terminal";
        event.method = "sessionEnded";
        event.session = "session";
        event.request = false;
        event.event_sequence = 2;
        event.payload = R"({"state":"ended","reason":"normal"})";
        body(event);
    }
};

TEST_F(FocusRuntimeTest, CombinedCapabilityAndActualReplyDoNotStopCapture) {
    established();
    EXPECT_NE(mqtt.sent.front().payload.find("focusControl"), std::string::npos);
    EXPECT_NE(mqtt.sent.front().payload.find("display"), std::string::npos);
    EXPECT_EQ(mqtt.sent.front().payload.find("baseline"), std::string::npos);
    ASSERT_EQ(runtime.start_input(++now), Result::Applied);
    const auto payload = mqtt.sent.back().payload;
    const auto begin = payload.find("\"inputId\":\"") + 11;
    const auto input = payload.substr(begin, payload.find('"', begin) - begin);
    ack_last("{\"inputId\":\"" + input + "\"}");
    ASSERT_TRUE(media.capture);
    const auto before = mqtt.sent.size();
    focus("$thing/down/signaling/other/device");
    EXPECT_EQ(focus_driver.starts, 0U);
    focus();
    EXPECT_EQ(focus_driver.starts, 1U);
    EXPECT_EQ(mqtt.sent.size(), before);
    EXPECT_TRUE(media.capture);
    focus_driver.complete();
    drive();
    ASSERT_EQ(mqtt.sent.back().method, "focusState");
    EXPECT_EQ(mqtt.sent.back().session, "file-child");
    EXPECT_EQ(mqtt.sent.back().code, 200);
    EXPECT_EQ(runtime.voice().state(), VoiceState::Established);
    EXPECT_TRUE(media.capture);
    focus();
    EXPECT_EQ(focus_driver.starts, 1U);
    EXPECT_EQ(focus_authority.calls, 1U);
}

TEST_F(FocusRuntimeTest, ParentDrainRequiresActualReleaseAndNetworkConfirmation) {
    established();
    focus();
    focus_driver.complete();
    drive();
    mqtt.drained = false;
    terminal();
    EXPECT_EQ(runtime.state(), RuntimeState::Draining);
    EXPECT_FALSE(runtime.focus_released());
    EXPECT_FALSE(media.capture);
    EXPECT_EQ(runtime.start({}, ++now), Result::Busy);
    focus_driver.released = true;
    drive();
    EXPECT_TRUE(runtime.focus_released());
    EXPECT_EQ(runtime.state(), RuntimeState::Draining);
    mqtt.drained = true;
    drive();
    EXPECT_EQ(runtime.state(), RuntimeState::Idle);
}

TEST_F(FocusRuntimeTest, DisconnectionContinuesOriginalReleaseWithoutReopeningInput) {
    established();
    focus();
    focus_driver.complete();
    drive();
    network(MqttEvent::Kind::Disconnected);
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
    EXPECT_FALSE(runtime.focus_released());
    EXPECT_EQ(runtime.start({}, ++now), Result::Busy);
    focus_driver.released = true;
    drive();
    EXPECT_TRUE(runtime.focus_released());
    EXPECT_FALSE(media.capture);
    EXPECT_EQ(runtime.start({}, ++now), Result::Applied);
}

TEST_F(FocusRuntimeTest, MissingWallClockClosesInsteadOfUsingMonotonicTime) {
    established();
    focus();
    runtime.poll(++now);
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
    EXPECT_FALSE(runtime.focus_released());
    EXPECT_FALSE(media.capture);
}

TEST_F(FocusRuntimeTest, CapabilityConflictRecoveryKeepsFullGroupWithoutIdleProof) {
    subscribed();
    const auto original = mqtt.sent.back();
    Envelope denied;
    denied.token = original.token;
    denied.method = "requestAck";
    denied.request = false;
    denied.code = 409;
    denied.payload = R"({"requestMethod":"capabilityReport","reason":"capabilityNotAccepted"})";
    body(denied);
    const auto count = mqtt.sent.size();
    now += 500;
    runtime.poll(now, 1000000 + now);
    ASSERT_EQ(mqtt.sent.size(), count + 1);
    EXPECT_NE(mqtt.sent.back().token, original.token);
    EXPECT_EQ(mqtt.sent.back().payload, original.payload);
    EXPECT_EQ(runtime.state(), RuntimeState::Running);
    EXPECT_FALSE(media.capture);
}

TEST(VoiceRuntimeFocusBinding, WrongDeviceEndpointCannotConnectOrAdvertise) {
    RuntimeMqtt mqtt;
    RuntimeMedia media;
    RuntimeFocusDriver driver;
    RuntimeFocusAuthority authority;
    FocusEndpoint endpoint(mqtt, driver, authority, {"speaker", "display"},
                           "$thing/up/signaling/p/other", "$thing/down/signaling/p/other", 1000);
    VoiceRuntime runtime(mqtt, media, "p/d", {{true, false, false}, {16000, 1, 20}}, "boot",
                         nullptr, nullptr, &endpoint);
    EXPECT_EQ(runtime.start({}, 1), Result::Invalid);
    EXPECT_TRUE(mqtt.sent.empty());
    EXPECT_TRUE(mqtt.subscriptions.empty());
    EXPECT_EQ(driver.starts, 0U);
}

TEST_F(FocusRuntimeTest, FocusBeforeParentEstablishmentCannotAllocate) {
    subscribed();
    focus();
    EXPECT_EQ(focus_driver.starts, 0U);
    EXPECT_EQ(focus_authority.calls, 0U);
    EXPECT_TRUE(runtime.focus_released());
    EXPECT_EQ(runtime.voice().state(), VoiceState::Reporting);
}

TEST(VoiceRuntimeFocusBinding, SameTopicOnDifferentPublisherCannotConnect) {
    RuntimeMqtt mqtt, unrelated;
    RuntimeMedia media;
    RuntimeFocusDriver driver;
    RuntimeFocusAuthority authority;
    FocusEndpoint endpoint(unrelated, driver, authority, {"speaker", "display"},
                           "$thing/up/signaling/p/d", "$thing/down/signaling/p/d", 1000);
    VoiceRuntime runtime(mqtt, media, "p/d", {{true, false, false}, {16000, 1, 20}}, "boot",
                         nullptr, nullptr, &endpoint);
    EXPECT_EQ(runtime.start({}, 1), Result::Invalid);
    EXPECT_TRUE(mqtt.sent.empty());
    EXPECT_TRUE(unrelated.sent.empty());
}
// ActionRuntimeTest 复用相同协商流程，区别仅为注入可移植动作执行器。
class ActionRuntimeTest : public RuntimeTest {
protected:
    ActionRuntimeTest() : RuntimeTest(true) {}
    // action 经精确MQTT路由输入，不绕过运行时的消息分派。
    void action(std::string token, std::string params = R"({"volume":40})",
                std::string topic = "$thing/down/action/p/d") {
        mqtt.events.push_back({MqttEvent::Kind::Message, 0, 0, std::move(topic),
                               "{\"method\":\"action\",\"msgToken\":\"" + token +
                                   "\",\"actionID\":\"setVolume\",\"params\":" + params + "}"});
        runtime.poll(++now);
    }
};

// PlaybackRuntimeTest 复用真实语音协商，并从唯一MQTT事件入口注入独立文件子会话。
class PlaybackRuntimeTest : public RuntimeTest {
protected:
    explicit PlaybackRuntimeTest(bool focus = false) : RuntimeTest(false, true, focus) {}
    void file(std::string session = "file-child") {
        Envelope invite;
        invite.token = "file-invite";
        invite.method = "sessionInvite";
        invite.session = std::move(session);
        invite.event_sequence = 1;
        invite.payload =
            R"({"purpose":"mediaPlayback","admission":"direct","transport":"none","tracks":[],"expiresAt":1005000,"playbackId":"player","mediaSource":{"contentRef":"content"},"inputModalities":[],"outputModalities":["audio","video"]})";
        body(invite);
    }
    void terminal() {
        Envelope event;
        event.token = "file-terminal";
        event.method = "sessionEnded";
        event.session = "file-child";
        event.request = false;
        event.event_sequence = 2;
        event.payload = R"({"state":"ended","reason":"parentEnded"})";
        body(event);
    }
    // parent_terminal 仅结束原AI父，不把文件的终态关联混入父消息。
    void parent_terminal() {
        Envelope parent;
        parent.token = "parent-terminal";
        parent.method = "sessionEnded";
        parent.session = "session";
        parent.request = false;
        parent.event_sequence = 2;
        parent.payload = R"({"state":"ended","reason":"completed"})";
        body(parent);
    }
    void capture() {
        ASSERT_EQ(runtime.start_input(++now), Result::Applied);
        const auto request = mqtt.sent.back();
        const auto marker = request.payload.find("\"inputId\":\"");
        ASSERT_NE(marker, std::string::npos);
        const auto begin = marker + 11;
        const auto id = request.payload.substr(begin, request.payload.find('"', begin) - begin);
        ack_last("{\"inputId\":\"" + id + "\"}");
        ASSERT_TRUE(media.capture);
    }
};

// FocusPlaybackRuntimeTest 同时装配文件与焦点，验证独立两组收尾条件不能互相代替。
class FocusPlaybackRuntimeTest : public PlaybackRuntimeTest {
protected:
    FocusPlaybackRuntimeTest() : PlaybackRuntimeTest(true) {}
};

TEST_F(FocusPlaybackRuntimeTest, FileReleaseCannotReplaceActualFocusRelease) {
    established();
    file_admission.ready = true;
    file();
    mqtt.events.push_back(
        {MqttEvent::Kind::Message, 0, 0, "$thing/down/signaling/p/d",
         R"({"msgToken":"focus-token","method":"focusControl","sessionId":"file-child","eventSeq":1,"params":{"operation":"acquire","expectedRevision":0,"focusId":"lease","resources":["speaker","display"],"priority":"conversation","resumePolicy":"explicit","expiresAt":1010000}})"});
    drive();
    ASSERT_EQ(focus_driver.starts, 1U);
    focus_driver.complete();
    drive();
    parent_terminal();
    terminal();
    file_admission.released = true;
    drive();
    EXPECT_TRUE(runtime.files_released());
    EXPECT_FALSE(runtime.focus_released());
    EXPECT_EQ(runtime.state(), RuntimeState::Draining);
    focus_driver.released = true;
    drive();
    EXPECT_EQ(runtime.state(), RuntimeState::Idle);
}

// 能力只上报一次，文件准备不关闭AI输入，原邀请重复不再授权或建立第二网络连接。
TEST_F(PlaybackRuntimeTest, FilesShareIdentityWithoutReplacingAI) {
    established();
    capture();
    file();
    ASSERT_EQ(runtime.state(), RuntimeState::Running);
    EXPECT_EQ(runtime.voice().state(), VoiceState::Established);
    EXPECT_TRUE(media.capture);
    EXPECT_FALSE(runtime.files_released());
    const auto calls = file_admission.calls;
    file();  // 正常poll会轮询原准备；重复本身不能创建新实例。
    EXPECT_EQ(file_admission.calls, calls + 1);
    file_admission.ready = true;
    drive();
    EXPECT_EQ(mqtt.sent.back().method, "sessionAccept");
    EXPECT_EQ(mqtt.sent.back().session, "file-child");
    EXPECT_EQ(mqtt.sent_topics.back(), "$thing/up/signaling/p/d");
    unsigned reports = 0;
    for (const auto& request : mqtt.sent)
        if (request.method == "capabilityReport")
            ++reports;
    EXPECT_EQ(reports, 1U);
    EXPECT_EQ(mqtt.subscriptions.size(), 2U);
    Envelope malformed;
    malformed.token = "bad-file";
    malformed.session = "malformed-file";
    malformed.method = "sessionInvite";
    malformed.event_sequence = 1;
    malformed.payload = R"({"purpose":"mediaPlayback"})";
    const auto prior = file_admission.calls;
    body(malformed);
    EXPECT_EQ(file_admission.calls, prior);
    EXPECT_EQ(runtime.state(), RuntimeState::Running);
    body(content(1), "$thing/down/message/p/d");
    ASSERT_TRUE(runtime.take_content());
    EXPECT_TRUE(media.capture);
}

// MSG回复序号与SIG文件事件序号独立；高水位不得隔离合法子邀请，也不能提前接听。
TEST_F(PlaybackRuntimeTest, MessageHighWaterDoesNotSuppressFileInvitation) {
    established();
    capture();
    body(content(13), "$thing/down/message/p/d");
    ASSERT_TRUE(runtime.take_content());
    body(content(12), "$thing/down/message/p/d");
    EXPECT_FALSE(runtime.take_content());     // 通过原消费行为证明MSG高水位已是13。
    const auto published = mqtt.sent.size();  // 准备前既有协议请求不计入文件接听。
    file();  // 合法SIG eventSeq=1从同一MQTT队列进入真实Runtime，不能与MSG水位比较。
    EXPECT_EQ(file_admission.calls, 2U);  // 首次准入与同poll准备查询各一次，不代表两个子实例。
    EXPECT_EQ(mqtt.sent.size(), published);
    EXPECT_FALSE(runtime.files_released());
    EXPECT_FALSE(runtime.file_established("file-child"));
    EXPECT_EQ(runtime.state(), RuntimeState::Running);
    EXPECT_EQ(runtime.voice().state(), VoiceState::Established);
    EXPECT_EQ(runtime.voice().session_id(), "session");
    EXPECT_TRUE(media.capture);
    file_admission.ready = true;  // 独立驱动就绪事实之后，才允许原准备完成。
    drive();
    ASSERT_EQ(mqtt.sent.size(), published + 1);
    EXPECT_EQ(mqtt.sent.back().method, "sessionAccept");
    EXPECT_EQ(mqtt.sent.back().session, "file-child");
    EXPECT_EQ(mqtt.sent.back().token, "file-invite");
    EXPECT_EQ(mqtt.sent_topics.back(), "$thing/up/signaling/p/d");
    drive();
    EXPECT_EQ(mqtt.sent.size(), published + 1);  // 后续poll不创建第二条接听或第二个子邀请。
    body(content(12), "$thing/down/message/p/d");
    EXPECT_FALSE(runtime.take_content());  // SIG准入不能重置原MSG水位。
    body(content(14), "$thing/down/message/p/d");
    ASSERT_TRUE(runtime.take_content());
    EXPECT_EQ(runtime.voice().session_id(), "session");
    EXPECT_EQ(runtime.voice().state(), VoiceState::Established);
    EXPECT_TRUE(media.capture);
}

// 当前已排队断连先于准备轮询，旧资源释放前不得重连；重连保留原实例不能复活。
TEST_F(PlaybackRuntimeTest, DisconnectWaitsForReleaseBeforeNewConnection) {
    established();
    file();
    const auto sent = mqtt.sent.size();
    file_admission.ready = true;
    network(MqttEvent::Kind::Disconnected);
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
    EXPECT_EQ(mqtt.sent.size(), sent);
    EXPECT_GT(file_admission.cancels, 0U);
    EXPECT_FALSE(runtime.files_released());
    EXPECT_EQ(runtime.start({}, ++now), Result::Busy);
    file_admission.released = true;
    drive();
    EXPECT_TRUE(runtime.files_released());
    ASSERT_EQ(runtime.start({}, ++now), Result::Applied);
    network(MqttEvent::Kind::Connected);
    network(MqttEvent::Kind::Subscribed, 12);
    network(MqttEvent::Kind::Subscribed, 13);
    const auto calls = file_admission.calls;
    file();
    EXPECT_EQ(file_admission.calls, calls);
    EXPECT_NE(mqtt.sent.back().method, "sessionAccept");
    terminal();
    EXPECT_EQ(mqtt.sent.back().method, "playbackState");
    EXPECT_EQ(mqtt.sent.back().token, "file-terminal");
}

// 文件终态先于同轮准备就绪处理，只结束子资源，不停止父AI采集或替换内容水位。
TEST_F(PlaybackRuntimeTest, FileTerminalDoesNotEndParentAI) {
    established();
    capture();
    file();
    const auto sent = mqtt.sent.size();
    file_admission.ready = true;
    terminal();
    EXPECT_EQ(mqtt.sent.size(), sent);
    EXPECT_EQ(runtime.state(), RuntimeState::Running);
    EXPECT_TRUE(media.capture);
    EXPECT_FALSE(runtime.files_released());
    file_admission.released = true;
    drive();
    EXPECT_TRUE(runtime.files_released());
    EXPECT_EQ(mqtt.sent.back().method, "playbackState");
    EXPECT_EQ(mqtt.sent.back().session, "file-child");
    EXPECT_TRUE(media.capture);
    const auto calls = file_admission.calls;
    file("session");  // 文件邀请不得复用父AI会话标识。
    EXPECT_EQ(file_admission.calls, calls);
    EXPECT_EQ(runtime.voice().state(), VoiceState::Established);
}

// 设备主动挂断立即停止文件，不等待父应答；原文件终态与释放仍通过原连接收尾。
TEST_F(PlaybackRuntimeTest, DeviceEndStopsFilesBeforeParentAcknowledgment) {
    established();
    file();
    const auto disconnected = mqtt.disconnects;
    ASSERT_EQ(runtime.end(++now), Result::Applied);
    EXPECT_GT(file_admission.cancels, 0U);
    EXPECT_FALSE(media.configured);
    EXPECT_EQ(mqtt.disconnects, disconnected);
    Envelope ended;
    ended.token = mqtt.sent.back().token;
    ended.session = "session";
    ended.method = "sessionEnded";
    ended.request = false;
    ended.code = 200;
    ended.payload = R"({"state":"ended","reason":"userHangup"})";
    body(ended);
    EXPECT_EQ(runtime.state(), RuntimeState::Draining);
    file_admission.released = true;
    terminal();
    EXPECT_EQ(mqtt.sent.back().token, "file-terminal");
    EXPECT_EQ(runtime.state(), RuntimeState::Idle);
    EXPECT_EQ(mqtt.disconnects, disconnected + 1);
}

// 父AI结束不能抢先断开共享MQTT；文件实际关闭与原平台终态都齐备后才收尾。
TEST_F(PlaybackRuntimeTest, ParentEndWaitsForOriginalFileTerminalAndRelease) {
    established();
    file();
    const auto disconnected = mqtt.disconnects;
    parent_terminal();
    EXPECT_EQ(runtime.voice().state(), VoiceState::Idle);
    ASSERT_NE(runtime.state(), RuntimeState::Idle);
    EXPECT_EQ(mqtt.disconnects, disconnected);
    EXPECT_FALSE(media.capture);
    file_admission.released = true;
    drive();
    EXPECT_TRUE(runtime.files_released());
    EXPECT_EQ(mqtt.disconnects, disconnected);  // 实际释放不能取代尚未收到的原平台终态。
    terminal();
    EXPECT_EQ(mqtt.sent.back().method, "playbackState");
    EXPECT_EQ(mqtt.sent.back().token, "file-terminal");
    EXPECT_EQ(runtime.state(), RuntimeState::Idle);
    EXPECT_EQ(mqtt.disconnects, disconnected + 1);
}

// 事实被本地MQTT接受不等于网络确认；收尾必须等PUBACK，不能立刻disconnect丢队列。
TEST_F(PlaybackRuntimeTest, ParentDrainWaitsForBrokerConfirmation) {
    established();
    file_admission.ready = true;
    file();  // 已交付会话与准备中的终态使用相同收尾门控。
    parent_terminal();
    EXPECT_EQ(runtime.state(), RuntimeState::Draining);
    file_admission.released = true;
    mqtt.drained = false;
    terminal();
    const auto disconnected = mqtt.disconnects;
    const auto sent = mqtt.sent.size();
    EXPECT_TRUE(runtime.files_released());
    EXPECT_EQ(mqtt.sent.back().method, "playbackState");
    EXPECT_EQ(runtime.state(), RuntimeState::Draining);
    drive();
    EXPECT_EQ(mqtt.sent.size(), sent);
    EXPECT_EQ(mqtt.disconnects, disconnected);
    mqtt.drained = true;
    network(MqttEvent::Kind::Published);
    EXPECT_EQ(runtime.state(), RuntimeState::Idle);
    EXPECT_EQ(mqtt.disconnects, disconnected + 1);
}

// 缺原终态是固定截止失败，不受重复消息续期，也不让新文件邀请继续准入。
// 文件事实已在父终态前排队，仍须等待PUBACK；不能只因端点settled就绕过网络排空。
TEST_F(PlaybackRuntimeTest, ParentEndKeepsAlreadyQueuedFileRelease) {
    established();
    file();
    file_admission.released = true;
    mqtt.drained = false;
    terminal();
    EXPECT_TRUE(runtime.files_released());
    EXPECT_EQ(mqtt.sent.back().method, "playbackState");
    const auto disconnected = mqtt.disconnects;
    parent_terminal();
    EXPECT_EQ(runtime.state(), RuntimeState::Draining);
    EXPECT_EQ(mqtt.disconnects, disconnected);
    mqtt.drained = true;
    drive();
    EXPECT_EQ(runtime.state(), RuntimeState::Idle);
    EXPECT_EQ(mqtt.disconnects, disconnected + 1);
}

// 缺原终态是固定截止失败，不受重复消息续期，也不让新文件邀请继续准入。
TEST_F(PlaybackRuntimeTest, ParentDrainTimeoutDoesNotRefreshOnDuplicate) {
    established();
    file();
    parent_terminal();
    const auto began = now;
    EXPECT_EQ(runtime.state(), RuntimeState::Draining);
    const auto calls = file_admission.calls;
    file("late-new");
    parent_terminal();
    EXPECT_EQ(file_admission.calls, calls);
    EXPECT_EQ(runtime.start({}, ++now), Result::Busy);
    file_admission.released = true;
    now = began + 29999;
    runtime.poll(now, 1000000 + now);
    EXPECT_EQ(runtime.state(), RuntimeState::Draining);
    ++now;
    runtime.poll(now, 1000000 + now);
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
    EXPECT_TRUE(runtime.files_released());  // 文件资源已关仍不能伪造原平台终态或正常收尾。
}

// 原终态已到而驱动未实际关闭，也不能正常收尾或因超时假报已释放。
TEST_F(PlaybackRuntimeTest, ParentDrainUnknownReleaseTimesOut) {
    established();
    file_admission.ready = true;
    file();
    parent_terminal();
    const auto began = now;
    terminal();
    now = began + 30000;
    runtime.poll(now, 1000000 + now);
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
    EXPECT_FALSE(runtime.files_released());
}

// 原终态及实际关闭齐备但本地发布仍被拒绝，事实不能被当成功或越过固定截止。
TEST_F(PlaybackRuntimeTest, ParentDrainPublishBackpressureTimesOut) {
    established();
    file();
    parent_terminal();
    const auto began = now;
    file_admission.released = true;
    mqtt.publishing = false;
    terminal();
    EXPECT_EQ(runtime.state(), RuntimeState::Draining);
    now = began + 30000;
    runtime.poll(now, 1000000 + now);
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
    EXPECT_TRUE(runtime.files_released());
}

// 发布排队已接受但未消费PUBACK时必须报未知失败，不以关闭连接清空队列冒充完成。
TEST_F(PlaybackRuntimeTest, ParentDrainUnconfirmedPublicationTimesOut) {
    established();
    file();
    parent_terminal();
    const auto began = now;
    file_admission.released = true;
    mqtt.drained = false;
    terminal();
    EXPECT_EQ(runtime.state(), RuntimeState::Draining);
    now = began + 30000;
    runtime.poll(now, 1000000 + now);
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
    EXPECT_TRUE(runtime.files_released());
}

// 未校时先关闭准备，不能以单调毫秒猜测Unix截止后先接听；旧语音调用仍可用无墙钟重载。
TEST_F(PlaybackRuntimeTest, MissingWallClockCancelsBeforeAccepting) {
    established();
    file();
    const auto sent = mqtt.sent.size();
    file_admission.ready = true;
    runtime.poll(++now);
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
    EXPECT_EQ(mqtt.sent.size(), sent);
    EXPECT_GT(file_admission.cancels, 0U);
    EXPECT_FALSE(runtime.files_released());
}

// 文件能力必须和真实准入器一起装配，未知适配不得自动变成授权成功。
TEST_F(RuntimeTest, FileCapabilityRequiresAdmission) {
    VoiceCapability capability{{true, false, false}, {16000, 1, 20}, 1,    false,
                               VoiceTransport::Udp,  true,           false};
    VoiceRuntime no_driver(mqtt, media, "p/d", capability, "file-boot");
    EXPECT_EQ(no_driver.start({}, now), Result::Invalid);
    capability.file_audio_output = false;
    VoiceRuntime no_capability(mqtt, media, "p/d", capability, "file-boot", nullptr,
                               &file_admission);
    EXPECT_EQ(no_capability.start({}, now), Result::Invalid);
    EXPECT_TRUE(mqtt.sent.empty());
}
TEST_F(ActionRuntimeTest, ExactRouteExecutionAndReplayDoNotBlockVoiceMedia) {
    established();
    ASSERT_EQ(mqtt.subscriptions.size(), 3U);
    action("other", "{}", "$thing/down/action/p/other");
    EXPECT_EQ(action_driver.begins, 0U);
    const auto polls = media.polls;
    action("a");
    EXPECT_EQ(action_driver.begins, 1U);
    EXPECT_TRUE(mqtt.action_replies.empty());
    action("a");
    EXPECT_EQ(action_driver.begins, 1U);
    EXPECT_GT(media.polls, polls);
    action_driver.completed.push_back({"a", 200, R"({"volume":40})"});
    runtime.poll(++now);
    ASSERT_EQ(mqtt.action_replies.size(), 1U);
    const auto reply = mqtt.action_replies.back();
    action("a");
    ASSERT_EQ(mqtt.action_replies.size(), 2U);
    EXPECT_EQ(mqtt.action_replies.back(), reply);
    EXPECT_EQ(action_driver.begins, 1U);
    action("b");
    EXPECT_EQ(action_driver.begins, 2U);
    EXPECT_EQ(runtime.voice().state(), VoiceState::Established);
}
TEST_F(ActionRuntimeTest, ConflictAndBusyHaveCorrelatedFailureWithoutExecution) {
    established();
    action("a");
    action("a", R"({"volume":50})");
    ASSERT_EQ(mqtt.action_replies.size(), 1U);
    EXPECT_NE(mqtt.action_replies.back().find("409"), std::string::npos);
    action("b");
    ASSERT_EQ(mqtt.action_replies.size(), 2U);
    EXPECT_NE(mqtt.action_replies.back().find("503"), std::string::npos);
    EXPECT_EQ(action_driver.begins, 1U);
}
TEST_F(ActionRuntimeTest, DisconnectKeepsActionHistoryAcrossReconnect) {
    established();
    action("a");
    network(MqttEvent::Kind::Disconnected);
    EXPECT_EQ(action_driver.cancels, 1U);
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
    EXPECT_FALSE(actions.closed());
    const auto result = actions.submit(
        R"({"method":"action","msgToken":"a","actionID":"setVolume","params":{"volume":40}})",
        ++now);
    EXPECT_EQ(result.state, ActionAdmission::Replay);
    EXPECT_NE(result.reply.find("504"), std::string::npos);
    EXPECT_EQ(action_driver.begins, 1U);
    mqtt.sent.clear();  // 模拟新的网络连接，执行器与驱动实例不重建。
    mqtt.next_subscription = 10;
    established();
    action("a");
    ASSERT_FALSE(mqtt.action_replies.empty());
    EXPECT_NE(mqtt.action_replies.back().find("504"), std::string::npos);
    EXPECT_EQ(action_driver.begins, 1U);
    action("b");
    EXPECT_EQ(action_driver.begins, 2U);
}
TEST_F(ActionRuntimeTest, FailedActionSubscriptionNeverStartsSessionOrDriver) {
    ASSERT_EQ(runtime.start({}, now), Result::Applied);
    network(MqttEvent::Kind::Connected);
    network(MqttEvent::Kind::Subscribed, 10);
    network(MqttEvent::Kind::Subscribed, 11);
    EXPECT_TRUE(mqtt.sent.empty());
    network(MqttEvent::Kind::Subscribed, 12, 128);
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
    EXPECT_TRUE(mqtt.sent.empty());
    EXPECT_EQ(action_driver.begins, 0U);
}
TEST_F(ActionRuntimeTest, MalformedActionDoesNotExecuteOrStopAudioPolling) {
    established();
    const auto polls = media.polls;
    action("bad", "[]");
    EXPECT_EQ(action_driver.begins, 0U);
    EXPECT_TRUE(mqtt.action_replies.empty());
    EXPECT_EQ(runtime.state(), RuntimeState::Running);
    EXPECT_GT(media.polls, polls);
}
TEST_F(ActionRuntimeTest, FailedPublishRetainsCompletedResultWithoutReexecution) {
    established();
    action("a");
    action_driver.completed.push_back({"a", 200, "{}"});
    mqtt.publishing = false;
    runtime.poll(++now);
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
    const auto result = actions.submit(
        R"({"method":"action","msgToken":"a","actionID":"setVolume","params":{"volume":40}})",
        ++now);
    EXPECT_EQ(result.state, ActionAdmission::Replay);
    EXPECT_NE(result.reply.find("200"), std::string::npos);
    EXPECT_EQ(action_driver.begins, 1U);
}
TEST_F(RuntimeTest, SubscriptionsGateSessionAndPubackIsNotBusinessAck) {
    subscribed();
    EXPECT_EQ(runtime.state(), RuntimeState::Running);
    EXPECT_EQ(mqtt.subscriptions,
              (std::vector<std::string>{"$thing/down/signaling/p/d", "$thing/down/message/p/d"}));
    network(MqttEvent::Kind::Published, 100);
    network(MqttEvent::Kind::Subscribed, 10);
    EXPECT_EQ(mqtt.sent.size(), 1U);
    EXPECT_FALSE(media.configured);
    EXPECT_EQ(runtime.start({}, now), Result::Busy);
}
TEST_F(RuntimeTest, ActualHandshakeAndMediaPumpRemainIndependentOfContent) {
    established();
    EXPECT_EQ(runtime.start_input(++now), Result::Applied);
    EXPECT_FALSE(media.capture);
    const auto request = mqtt.sent.back();
    // 以实际请求中的 inputId 回复，不能假定客户端计数器步长。
    const auto begin = request.payload.find("\"inputId\":\"") + 11;
    const auto id = request.payload.substr(begin, request.payload.find('"', begin) - begin);
    ack_last("{\"inputId\":\"" + id + "\"}");
    ASSERT_TRUE(media.capture);
    body(content(1), "$thing/down/message/p/d");
    auto text = runtime.take_content();
    ASSERT_TRUE(text);
    EXPECT_EQ(text->text, "你好");
    EXPECT_FALSE(runtime.take_content());
    body(content(1), "$thing/down/message/p/d");
    body(content(100, "old"), "$thing/down/message/p/d");
    body(content(100), "$thing/down/message/p/other");
    EXPECT_FALSE(runtime.take_content());
    body(content(2), "$thing/down/message/p/d");
    EXPECT_TRUE(runtime.take_content());
    const auto polls = media.polls;
    for (int count = 0; count < 64; ++count)
        mqtt.events.push_back({MqttEvent::Kind::Published, count, 0, {}, {}});
    runtime.poll(++now);
    EXPECT_EQ(mqtt.events.size(), 32U);
    EXPECT_EQ(media.polls, polls + 1);
    EXPECT_EQ(runtime.stop_input(++now), Result::Applied);
    EXPECT_FALSE(media.capture);
    runtime.close(++now);
    runtime.close(now);
    EXPECT_FALSE(media.configured);
    EXPECT_FALSE(runtime.take_content());
    EXPECT_EQ(runtime.state(), RuntimeState::Idle);
}
TEST_F(RuntimeTest, TextInputUsesMessageRouteAndSurfacesCloudBusinessAcknowledgement) {
    established();
    ASSERT_EQ(runtime.send_text("你好", true, ++now), Result::Applied);
    ASSERT_EQ(mqtt.sent.back().method, "inputSend");
    EXPECT_EQ(mqtt.sent_topics.back(), "$thing/up/message/p/d");
    const auto request = mqtt.sent.back();
    const auto marker = request.payload.find("\"inputId\":\"");
    ASSERT_NE(marker, std::string::npos);
    const auto begin = marker + std::string("\"inputId\":\"").size();
    const auto input_id = request.payload.substr(begin, request.payload.find('"', begin) - begin);
    Envelope accepted;
    accepted.token = request.token;
    accepted.method = "requestAck";
    accepted.session = "session";
    accepted.request = false;
    accepted.code = 200;
    accepted.payload = "{\"requestMethod\":\"inputSend\",\"result\":{\"inputId\":\"" + input_id +
                       "\",\"state\":\"accepted\"}}";
    body(accepted, "$thing/down/message/p/d");
    const auto cloud_result = runtime.take_text_result();
    ASSERT_TRUE(cloud_result);
    EXPECT_EQ(cloud_result->input_id, input_id);
    EXPECT_EQ(cloud_result->result, "accepted");
    EXPECT_EQ(runtime.voice().state(), VoiceState::Established);

    ASSERT_EQ(runtime.send_text("再问一次", false, ++now), Result::Applied);
    const auto rejected_request = mqtt.sent.back();
    Envelope rejected;
    rejected.token = rejected_request.token;
    rejected.method = "requestAck";
    rejected.session = "session";
    rejected.request = false;
    rejected.code = 422;
    rejected.payload = R"({"requestMethod":"inputSend","reason":"inputUnsupported"})";
    body(rejected, "$thing/down/message/p/d");
    const auto rejection = runtime.take_text_result();
    ASSERT_TRUE(rejection);
    EXPECT_EQ(rejection->result, "unsupported");
    EXPECT_EQ(runtime.voice().state(), VoiceState::Established);
    EXPECT_FALSE(runtime.take_text_result());
}
TEST_F(RuntimeTest, ContentBackpressureAndMalformedMessagesFailClosed) {
    established();
    /* contents_ 容量 32：前 32 条保持运行，第 33 条触发溢出关闭。 */
    for (unsigned seq = 1; seq <= 32; ++seq)
        body(content(seq), "$thing/down/message/p/d");
    EXPECT_EQ(runtime.state(), RuntimeState::Running);
    body(content(33), "$thing/down/message/p/d");
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
    EXPECT_FALSE(media.configured);
    EXPECT_FALSE(runtime.take_content());
}
// 原网络分支诊断不借网页计数，且错会话/旧水位不改变原消费结果。
TEST_F(RuntimeTest, MessageDiagnosticsRecordActualAdmissionWithoutChangingContent) {
    established();
    EXPECT_FALSE(runtime.take_message_diagnostics());
    runtime.enable_message_diagnostics();
    body(content(1), "$thing/down/message/p/d");
    ASSERT_TRUE(runtime.take_content());
    body(content(1), "$thing/down/message/p/d");
    body(content(2, "secret-session"), "$thing/down/message/p/d");
    EXPECT_FALSE(runtime.take_content());
    const auto diagnostic = runtime.take_message_diagnostics();
    ASSERT_TRUE(diagnostic);
    EXPECT_EQ(diagnostic->received, 3U);
    ASSERT_EQ(diagnostic->size, 3U);
    EXPECT_EQ(diagnostic->records[0].outcome, MessageDiagnosticOutcome::ContentQueued);
    EXPECT_TRUE(diagnostic->records[0].taken);
    EXPECT_EQ(diagnostic->records[1].outcome, MessageDiagnosticOutcome::WatermarkDiscarded);
    EXPECT_EQ(diagnostic->records[1].watermark, 1U);
    EXPECT_FALSE(diagnostic->records[2].session_matches);
    EXPECT_EQ(diagnostic->records[2].outcome, MessageDiagnosticOutcome::SessionMismatch);
    EXPECT_EQ(runtime.voice().state(), VoiceState::Established);
}
// 默认关闭不留收包或宿主投影；显式启用后空快照可区分观测已安装但尚未收到MSG。
TEST_F(RuntimeTest, MessageDiagnosticsDefaultOffAndHostProjectionIsNotDisplayAck) {
    established();
    body(content(1), "$thing/down/message/p/d");
    auto first = runtime.take_content();
    ASSERT_TRUE(first);
    runtime.observe_content_display(*first, true, true);
    EXPECT_FALSE(runtime.take_message_diagnostics());
    runtime.enable_message_diagnostics();
    auto empty = runtime.take_message_diagnostics();
    ASSERT_TRUE(empty);
    EXPECT_EQ(empty->received, 0U);
    EXPECT_EQ(empty->size, 0U);
    runtime.observe_content_display(*first, false, false);
    runtime.observe_content_display(*first, true, true);
    runtime.observe_content_display(*first, true, false);
    auto facts = runtime.take_message_diagnostics();
    ASSERT_TRUE(facts);
    EXPECT_EQ(facts->received, 0U);
    ASSERT_EQ(facts->size, 0U);
    EXPECT_EQ(facts->host_filtered, 1U);
    EXPECT_EQ(facts->host_sent, 1U);
    EXPECT_EQ(facts->host_send_failed, 1U);
    EXPECT_EQ(runtime.state(), RuntimeState::Running);
}
// 真实解码失败仍保留分支事实；不能因为原fail关闭而丢掉最后一条诊断。
TEST_F(RuntimeTest, MessageDiagnosticsPreserveEnvelopeFailureAfterOriginalClose) {
    established();
    runtime.enable_message_diagnostics();
    mqtt.events.push_back(
        {MqttEvent::Kind::Message, 0, 0, "$thing/down/message/p/d", "secret-not-json"});
    drive();
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
    auto facts = runtime.take_message_diagnostics();
    ASSERT_TRUE(facts);
    EXPECT_EQ(facts->received, 1U);
    ASSERT_EQ(facts->size, 1U);
    EXPECT_EQ(facts->records[0].method, "unknown");
    EXPECT_FALSE(facts->records[0].parsed);
    EXPECT_EQ(facts->records[0].outcome, MessageDiagnosticOutcome::EnvelopeRejected);
}
// 累计32项预算不因读取或重复启用刷新，后续真实收包计数继续但明确记录溢出。
TEST_F(RuntimeTest, MessageDiagnosticsBoundedAcrossReadsAndUnknownMethodRedacted) {
    established();
    runtime.enable_message_diagnostics();
    auto unknown = content(1);
    unknown.method = "private-unknown-method";
    body(unknown, "$thing/down/message/p/d");
    auto first = runtime.take_message_diagnostics();
    ASSERT_TRUE(first);
    ASSERT_EQ(first->size, 1U);
    EXPECT_EQ(first->records[0].method, "unknown");
    EXPECT_EQ(first->records[0].outcome, MessageDiagnosticOutcome::Unsupported);
    for (unsigned index = 0; index < 40; ++index)
        body(content(1, "private-other-session"), "$thing/down/message/p/d");
    runtime.enable_message_diagnostics();
    auto facts = runtime.take_message_diagnostics();
    ASSERT_TRUE(facts);
    EXPECT_EQ(facts->size, 32U);
    EXPECT_EQ(facts->received, 41U);
    EXPECT_TRUE(facts->overflow);
    EXPECT_EQ(runtime.voice().state(), VoiceState::Established);
    EXPECT_FALSE(runtime.take_content());
}
// 仅复现既有高水位语义，不修乱序：终态抢先后旧文本静默丢弃且诊断指出原水位。
TEST_F(RuntimeTest, MessageDiagnosticsObserveTerminalFirstWithoutRepairingOrdering) {
    established();
    runtime.enable_message_diagnostics();
    auto done = content(13);
    done.method = "respDone";
    done.payload =
        R"({"respId":"private-response","inputId":"original-input","status":"failed","reason":"incompleteReply","contentCount":0,"audioState":"none"})";
    body(done, "$thing/down/message/p/d");
    auto terminal = runtime.take_content();
    ASSERT_TRUE(terminal);
    runtime.observe_content_display(*terminal, false, false);
    body(content(1), "$thing/down/message/p/d");
    EXPECT_FALSE(runtime.take_content());
    auto facts = runtime.take_message_diagnostics();
    ASSERT_TRUE(facts);
    ASSERT_EQ(facts->size, 2U);
    EXPECT_TRUE(facts->records[0].taken);
    EXPECT_TRUE(facts->records[0].host_observed);
    EXPECT_FALSE(facts->records[0].projected);
    EXPECT_EQ(facts->records[1].outcome, MessageDiagnosticOutcome::WatermarkDiscarded);
    EXPECT_EQ(facts->records[1].watermark, 13U);
    EXPECT_EQ(runtime.state(), RuntimeState::Running);
}
// 原SIG终态已被VoiceClient消费，却没有诊断快照；复现退出原因观察缺口，不修改结束准入。
TEST_F(RuntimeTest, MessageDiagnosticsObserveAcceptedParentEnd) {
    established();
    runtime.enable_message_diagnostics();
    ASSERT_TRUE(runtime.take_message_diagnostics());
    Envelope ended;
    ended.token = "parent-terminal";
    ended.method = "sessionEnded";
    ended.session = "session";
    ended.request = false;
    ended.event_sequence = 2;
    ended.payload = R"({"state":"ended","reason":"normal"})";
    body(ended);
    ASSERT_EQ(runtime.state(), RuntimeState::Idle);
    EXPECT_TRUE(runtime.take_message_diagnostics());
    const auto lifecycle = runtime.take_lifecycle_diagnostic();
    ASSERT_TRUE(lifecycle);
    EXPECT_EQ(lifecycle->accepted_end, AcceptedVoiceEnd::SessionEndedEvent);
    EXPECT_EQ(lifecycle->close_cause, VoiceCloseCause::DrainSettled);
    EXPECT_FALSE(lifecycle->local_end_applied);
    EXPECT_EQ(lifecycle->observation, 2U);
    const auto disconnects = mqtt.disconnects;
    EXPECT_FALSE(runtime.take_lifecycle_diagnostic());
    EXPECT_EQ(mqtt.disconnects, disconnects);
    body(ended);
    runtime.close(++now);
    EXPECT_FALSE(runtime.take_lifecycle_diagnostic());
}
// 本地受理与真正ACK分开；不匹配的ACK和他会话终态不得获得acceptedEnd。
TEST_F(RuntimeTest, LifecycleDiagnosticsRequireActualMatchingAckAndKeepLocalFactSeparate) {
    established();
    runtime.enable_message_diagnostics();
    ASSERT_EQ(runtime.end(++now), Result::Applied);
    auto lifecycle = runtime.take_lifecycle_diagnostic();
    ASSERT_TRUE(lifecycle);
    EXPECT_TRUE(lifecycle->local_end_applied);
    EXPECT_EQ(lifecycle->accepted_end, AcceptedVoiceEnd::None);
    Envelope wrong;
    wrong.token = "foreign-token";
    wrong.method = "sessionEnded";
    wrong.session = "session";
    wrong.request = false;
    wrong.code = 200;
    wrong.payload = R"({"state":"ended","reason":"userStopped"})";
    body(wrong);
    EXPECT_EQ(runtime.voice().state(), VoiceState::Ending);
    EXPECT_FALSE(runtime.take_lifecycle_diagnostic());
    wrong.code.reset();
    wrong.session = "other-session";
    wrong.event_sequence = 2;
    body(wrong);
    EXPECT_FALSE(runtime.take_lifecycle_diagnostic());
    wrong.token = mqtt.sent.back().token;
    wrong.session = "session";
    wrong.code = 200;
    wrong.event_sequence.reset();
    body(wrong);  // 正式sessionEnd应答method为sessionEnded，不用不支持的通用requestAck。
    EXPECT_EQ(runtime.state(), RuntimeState::Idle);
    lifecycle = runtime.take_lifecycle_diagnostic();
    ASSERT_TRUE(lifecycle);
    EXPECT_EQ(lifecycle->accepted_end, AcceptedVoiceEnd::SessionEndAck);
    EXPECT_EQ(lifecycle->close_cause, VoiceCloseCause::DrainSettled);
    EXPECT_EQ(lifecycle->observation, 3U);
}
// 诊断默认关闭且启用不刷新；普通close及网络断连是独立关闭入口，不借Idle伪称SIG接纳。
TEST_F(RuntimeTest, LifecycleDiagnosticsDefaultOffAndExplicitCloseDoesNotForgeAcceptedEnd) {
    established();
    ASSERT_EQ(runtime.end(++now), Result::Applied);
    EXPECT_FALSE(runtime.take_lifecycle_diagnostic());
    runtime.enable_message_diagnostics();
    EXPECT_FALSE(runtime.take_lifecycle_diagnostic());
    runtime.close(++now);
    auto lifecycle = runtime.take_lifecycle_diagnostic();
    ASSERT_TRUE(lifecycle);
    EXPECT_EQ(lifecycle->close_cause, VoiceCloseCause::ExplicitClose);
    EXPECT_EQ(lifecycle->accepted_end, AcceptedVoiceEnd::None);
    EXPECT_FALSE(lifecycle->local_end_applied);
    runtime.enable_message_diagnostics();
    runtime.close(++now);
    EXPECT_FALSE(runtime.take_lifecycle_diagnostic());
}
TEST_F(RuntimeTest, LifecycleDiagnosticsActualDisconnectIsFailureNotAcceptedEnd) {
    established();
    runtime.enable_message_diagnostics();
    network(MqttEvent::Kind::Disconnected);
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
    const auto lifecycle = runtime.take_lifecycle_diagnostic();
    ASSERT_TRUE(lifecycle);
    EXPECT_EQ(lifecycle->close_cause, VoiceCloseCause::Failure);
    EXPECT_EQ(lifecycle->accepted_end, AcceptedVoiceEnd::None);
}
TEST_F(RuntimeTest, MediaFailureAndBackwardClockReleaseEveryResource) {
    established();
    media.polling = false;
    runtime.poll(++now);
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
    EXPECT_FALSE(media.configured);
    ASSERT_EQ(runtime.start({}, ++now), Result::Applied);
    runtime.poll(now - 1);
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
}
// 原媒体poll只执行一次且失败照常关闭；新增首因不能把unknown当正常。
TEST_F(RuntimeTest, FailureDiagnosticsObserveOriginalMediaPollFailure) {
    runtime.enable_message_diagnostics();
    established();
    const auto original_polls = media.polls;
    media.polling = false;
    drive();
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
    EXPECT_EQ(media.polls, original_polls + 1);
    EXPECT_FALSE(media.configured);
    EXPECT_EQ(runtime.failure_diagnostic(), RuntimeFailure::MediaPoll);
}
// 已正式接纳sessionError先令VoiceClient Failed，后继ClientTick只报告该原首因不倒推超时。
TEST_F(RuntimeTest, ClientFailureDiagnosticPreservesEarlierSessionErrorThroughClientTick) {
    runtime.enable_failure_diagnostics();
    established();
    Envelope error;
    error.token = "original-error";
    error.method = "sessionError";
    error.session = "session";
    error.request = false;
    error.code = 502;
    error.event_sequence = 2;
    error.payload = R"({"scope":"session","reason":"internalError","retryable":false})";
    body(error);
    ASSERT_EQ(runtime.state(), RuntimeState::Failed);
    EXPECT_EQ(runtime.failure_diagnostic(), RuntimeFailure::ClientTick);
    const auto first = runtime.take_client_failure_diagnostic();
    ASSERT_TRUE(first);
    EXPECT_EQ(first->origin, ClientFailureOrigin::ClientFailed);
    EXPECT_EQ(first->cause, VoiceFailureCause::SessionError);
    EXPECT_EQ(first->method, VoiceFailureMethod::None);
    runtime.close(++now);
    runtime.enable_failure_diagnostics();
    EXPECT_FALSE(runtime.take_client_failure_diagnostic());
    EXPECT_FALSE(media.configured);
}
// 原能力请求30秒到期保持完整失败语义，观察不改变重试数量或本机单次交付。
TEST_F(RuntimeTest, ClientFailureDiagnosticHasOriginalTimedOutMethod) {
    runtime.enable_failure_diagnostics();
    subscribed();
    runtime.poll(now + 30000);
    ASSERT_EQ(runtime.state(), RuntimeState::Failed);
    const auto first = runtime.take_client_failure_diagnostic();
    ASSERT_TRUE(first);
    EXPECT_EQ(first->origin, ClientFailureOrigin::ClientFailed);
    EXPECT_EQ(first->cause, VoiceFailureCause::RequestsTimeout);
    EXPECT_EQ(first->method, VoiceFailureMethod::CapabilityReport);
    EXPECT_FALSE(runtime.take_client_failure_diagnostic());
}
// RuntimeFailureTest每例只通过原事件、公开输入及实际驱动返回值触发原fail。
class RuntimeFailureTest : public RuntimeTest,
                           public testing::WithParamInterface<RuntimeFailure> {};
TEST_P(RuntimeFailureTest, OriginalBranchIsFirstCauseAndTakenOnlyOnce) {
    runtime.enable_message_diagnostics();
    const auto expected = GetParam();  // 本例唯一原失败分支。
    if (expected == RuntimeFailure::MqttConnect) {
        mqtt.connecting = false;
        EXPECT_EQ(runtime.start({}, now), Result::DriverFailed);
    } else if (expected == RuntimeFailure::ConnectionDeadline) {
        ASSERT_EQ(runtime.start({}, now), Result::Applied);
        runtime.poll(now + 30000);
    } else if (expected == RuntimeFailure::ClockRollback) {
        ASSERT_EQ(runtime.start({}, now), Result::Applied);
        runtime.poll(now - 1);
    } else if (expected == RuntimeFailure::MqttSubscription) {
        ASSERT_EQ(runtime.start({}, now), Result::Applied);
        mqtt.next_subscription = 0;
        network(MqttEvent::Kind::Connected);
    } else if (expected == RuntimeFailure::MqttPoll) {
        ASSERT_EQ(runtime.start({}, now), Result::Applied);
        mqtt.polling = false;
        drive();
    } else if (expected == RuntimeFailure::ClientTick) {
        subscribed();
        runtime.poll(now + 30000);
    } else {
        established();
        if (expected == RuntimeFailure::MqttDisconnected)
            network(MqttEvent::Kind::Disconnected);
        else if (expected == RuntimeFailure::MqttEventFailure)
            network(MqttEvent::Kind::Failed);
        else if (expected == RuntimeFailure::MediaPoll) {
            media.polling = false;
            drive();
        } else if (expected == RuntimeFailure::MessageQueueFull) {
            /* contents_ 容量 32：发 33 条有效内容触发溢出。 */
            for (unsigned i = 1; i <= 33; ++i)
                body(content(i), "$thing/down/message/p/d");
        } else if (expected == RuntimeFailure::MessageEnvelope ||
                   expected == RuntimeFailure::SignalClientInvalid) {
            mqtt.events.push_back({MqttEvent::Kind::Message, 0, 0,
                                   expected == RuntimeFailure::MessageEnvelope
                                       ? "$thing/down/message/p/d"
                                       : "$thing/down/signaling/p/d",
                                   "{"});
            drive();
        } else {
            auto invalid = content(1);  // 沿原生产信封编码，非法内容交原解码器判断。
            if (expected == RuntimeFailure::MessageAck) {
                invalid.method = "requestAck";
                invalid.code = 200;
            }
            invalid.payload = "{}";
            body(invalid, "$thing/down/message/p/d");
        }
    }
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
    EXPECT_EQ(runtime.failure_diagnostic(), expected);
    const auto disconnects = mqtt.disconnects;  // 原失败已实际关闭网络次数。
    const auto polls = media.polls;             // 诊断读取不允许额外驱动调用。
    EXPECT_EQ(runtime.take_failure_diagnostic(), expected);
    EXPECT_FALSE(runtime.take_failure_diagnostic());
    runtime.enable_message_diagnostics();
    EXPECT_FALSE(runtime.take_failure_diagnostic());
    EXPECT_EQ(mqtt.disconnects, disconnects);
    EXPECT_EQ(media.polls, polls);
    runtime.close(++now);
    EXPECT_EQ(runtime.failure_diagnostic(), expected);
    EXPECT_FALSE(runtime.take_failure_diagnostic());
}
INSTANTIATE_TEST_SUITE_P(
    OriginalRuntimeBranches, RuntimeFailureTest,
    testing::Values(RuntimeFailure::MqttConnect, RuntimeFailure::MqttDisconnected,
                    RuntimeFailure::MqttEventFailure, RuntimeFailure::MqttSubscription,
                    RuntimeFailure::ClockRollback, RuntimeFailure::ConnectionDeadline,
                    RuntimeFailure::MqttPoll, RuntimeFailure::ClientTick,
                    RuntimeFailure::MessageEnvelope, RuntimeFailure::SignalClientInvalid,
                    RuntimeFailure::MessageAck, RuntimeFailure::MessageContent,
                    RuntimeFailure::MessageQueueFull, RuntimeFailure::MediaPoll));
// 默认off后的原故障不可通过事后启用猜首因；close仍保持原业务退出行为。
TEST_F(RuntimeTest, FailureDiagnosticsDefaultOffAndLateEnableDoNotBackfill) {
    established();
    media.polling = false;
    drive();
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
    EXPECT_FALSE(runtime.failure_diagnostic());
    runtime.enable_failure_diagnostics();
    runtime.enable_message_diagnostics();
    EXPECT_FALSE(runtime.take_failure_diagnostic());
    runtime.poll(++now);
    EXPECT_FALSE(runtime.failure_diagnostic());
}
TEST_F(RuntimeTest, FailureDiagnosticsNormalEndDoesNotForgeFailure) {
    runtime.enable_failure_diagnostics();
    established();
    EXPECT_EQ(runtime.end(++now), Result::Applied);
    Envelope ended;  // 真实接纳原结束ACK，而非人工写诊断状态。
    ended.token = mqtt.sent.back().token;
    ended.session = "session";
    ended.method = "sessionEnded";
    ended.request = false;
    ended.code = 200;
    ended.payload = R"({"state":"ended","reason":"userHangup"})";
    body(ended);
    EXPECT_EQ(runtime.state(), RuntimeState::Idle);
    EXPECT_FALSE(runtime.failure_diagnostic());
    EXPECT_FALSE(runtime.take_failure_diagnostic());
}
TEST(RuntimeFailureNames, FixedNamesAndInvalidValue) {
    const std::array<std::string_view, 25> names{// 24原分支与一个明确unknown。
                                                 "mqttConnect",
                                                 "mqttDisconnected",
                                                 "mqttEventFailure",
                                                 "mqttSubscription",
                                                 "clockRollback",
                                                 "connectionDeadline",
                                                 "mqttPoll",
                                                 "signalEnvelope",
                                                 "signalClientInvalid",
                                                 "signalClientDriver",
                                                 "messageEnvelope",
                                                 "messageAck",
                                                 "messageContent",
                                                 "messageToolScope",
                                                 "messageQueueFull",
                                                 "actionReplyPublish",
                                                 "clientTick",
                                                 "toolPoll",
                                                 "mediaPoll",
                                                 "mediaOffer",
                                                 "mediaReady",
                                                 "missingWallClock",
                                                 "drainDeadline",
                                                 "sharedInputInvalid",
                                                 "unknown"};
    for (std::size_t i = 0; i < names.size(); ++i)
        EXPECT_EQ(runtime_failure_name(static_cast<RuntimeFailure>(i)), names[i]);
    EXPECT_TRUE(runtime_failure_name(static_cast<RuntimeFailure>(-1)).empty());
}
TEST_F(RuntimeTest, ConnectionAndSubscriptionFailuresHaveBoundedLifetime) {
    mqtt.connecting = false;
    EXPECT_EQ(runtime.start({}, now), Result::DriverFailed);
    mqtt.connecting = true;
    ASSERT_EQ(runtime.start({}, ++now), Result::Applied);
    runtime.poll(now + 30000);
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
    ASSERT_EQ(runtime.start({}, ++now), Result::Applied);
    network(MqttEvent::Kind::Connected);
    network(MqttEvent::Kind::Subscribed, 999);  // 无关 MID 不能确认订阅。
    runtime.poll(now + 30000);
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
    ASSERT_EQ(runtime.start({}, ++now), Result::Applied);
    mqtt.next_subscription = 0;
    network(MqttEvent::Kind::Connected);
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
}
TEST_F(RuntimeTest, InvalidIdentityNeverConnectsAndDisconnectDoesNotRestoreSession) {
    VoiceRuntime invalid(mqtt, media, "p/+", {{true, false, false}, {16000, 1, 20}, 1}, "boot");
    EXPECT_EQ(invalid.start({}, now), Result::Invalid);
    runtime.poll(++now);
    EXPECT_EQ(runtime.state(), RuntimeState::Idle);
    subscribed();
    network(MqttEvent::Kind::Disconnected);
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
    const auto sent = mqtt.sent.size();
    runtime.poll(now + 100000);
    EXPECT_EQ(mqtt.sent.size(), sent);
}
TEST_F(RuntimeTest, SessionEndClosesNetworkAndLateContentCannotReviveIt) {
    established();
    EXPECT_EQ(runtime.end(++now), Result::Applied);
    EXPECT_FALSE(media.configured);
    Envelope ended;
    ended.token = mqtt.sent.back().token;
    ended.session = "session";
    ended.method = "sessionEnded";
    ended.request = false;
    ended.code = 200;
    ended.payload = R"({"state":"ended","reason":"userHangup"})";
    body(ended);
    EXPECT_EQ(runtime.state(), RuntimeState::Idle);
    body(content(100), "$thing/down/message/p/d");
    EXPECT_FALSE(runtime.take_content());
    EXPECT_FALSE(media.configured);
}
TEST_F(RuntimeTest, UnsupportedContentDoesNotHideValidContentButMalformedContentFails) {
    established();
    auto future = content(100);
    future.method = "toolCallExecute";
    body(future, "$thing/down/message/p/d");
    EXPECT_FALSE(runtime.take_content());
    body(content(1), "$thing/down/message/p/d");
    ASSERT_TRUE(runtime.take_content());
    auto invalid = content(2);
    invalid.payload = R"({"respId":"r"})";
    body(invalid, "$thing/down/message/p/d");
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
    EXPECT_FALSE(media.configured);
}
TEST_F(RuntimeTest, SubackErrorPublishFailureAndRequestTimeoutStopHandshake) {
    ASSERT_EQ(runtime.start({}, now), Result::Applied);
    network(MqttEvent::Kind::Connected);
    network(MqttEvent::Kind::Subscribed, 10, 128);
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
    ASSERT_EQ(runtime.start({}, ++now), Result::Applied);
    network(MqttEvent::Kind::Connected);
    mqtt.publishing = false;
    network(MqttEvent::Kind::Subscribed, 12);
    network(MqttEvent::Kind::Subscribed, 13);
    // 单次发布失败保留原 token 重试；业务截止而非 PUBACK 决定请求终态。
    EXPECT_EQ(runtime.state(), RuntimeState::Running);
    now += 30000;
    runtime.poll(now);
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
    mqtt.publishing = true;
    ASSERT_EQ(runtime.start({}, ++now), Result::Applied);
    network(MqttEvent::Kind::Connected);
    network(MqttEvent::Kind::Subscribed, 14);
    network(MqttEvent::Kind::Subscribed, 15);
    runtime.poll(now + 30000);
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
}
// RuntimeLiveTest 固定同一原设备连接，不用第二个capabilityReport或设备身份。
class RuntimeLiveTest : public RuntimeTest {
protected:
    RuntimeLiveTest() : RuntimeTest(false, false, false, true) {}
};
TEST_F(RuntimeLiveTest, OneCapabilitySnapshotAndKnownLiveRoutePreserveAIInput) {
    established();
    unsigned reports = 0;
    for (const auto& message : mqtt.sent) {
        if (message.method == "capabilityReport") {
            ++reports;
            EXPECT_NE(message.payload.find("liveReceive"), std::string::npos);
        }
    }
    EXPECT_EQ(reports, 1U);
    EXPECT_EQ(live_endpoint.connections, 1U);
    ASSERT_EQ(runtime.start_input(++now), Result::Applied);
    const auto input_payload = mqtt.sent.back().payload;
    const auto input_start = input_payload.find("\"inputId\":\"") + 11;
    const auto input_id =
        input_payload.substr(input_start, input_payload.find('"', input_start) - input_start);
    ack_last("{\"inputId\":\"" + input_id +
             "\"}");  // 原audioStart业务确认后才actual采集，不靠publish。
    Envelope control;
    control.token = "live-control";
    control.session = "live-child";
    control.method = "trackControl";
    control.event_sequence = 2;
    control.payload = R"({"trackId":"broadcast-out","operation":"resume","mediaRevision":1})";
    body(control);
    EXPECT_EQ(live_endpoint.received.size(), 1U);
    EXPECT_EQ(runtime.state(), RuntimeState::Running);
    EXPECT_TRUE(media.capture);
}
TEST_F(RuntimeLiveTest, OriginalEndWaitsForActualLiveReleaseAndMailboxDrain) {
    established();
    live_endpoint.released_fact = live_endpoint.settled_fact = false;
    Envelope terminal;
    terminal.token = "end";
    terminal.session = "session";
    terminal.method = "sessionEnded";
    terminal.request = false;
    terminal.event_sequence = 2;
    terminal.payload = R"({"state":"ended","reason":"idleTimeout"})";
    body(terminal);
    EXPECT_EQ(runtime.state(), RuntimeState::Draining);
    EXPECT_FALSE(runtime.live_released());
    Envelope child = terminal;
    child.token = "child-end";
    child.session = "live-child";
    body(child);
    EXPECT_EQ(live_endpoint.received.size(), 1U);
    live_endpoint.released_fact = true;
    drive();
    EXPECT_EQ(runtime.state(), RuntimeState::Draining);
    live_endpoint.settled_fact = true;
    mqtt.drained = false;
    drive();
    EXPECT_EQ(runtime.state(), RuntimeState::Draining);
    mqtt.drained = true;
    drive();
    EXPECT_EQ(runtime.state(), RuntimeState::Idle);
}
TEST_F(RuntimeLiveTest, MissingWallClockStopsLiveBeforeAnyNewAdmission) {
    established();
    const auto previous = live_endpoint.closes;
    runtime.poll(++now);
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
    EXPECT_GT(live_endpoint.closes, previous);
    EXPECT_TRUE(live_endpoint.received.empty());
}
TEST_F(RuntimeLiveTest, CapabilityAndActualEndpointMustBePairedAndSameConnection) {
    VoiceCapability capability{{true, false, false}, {16000, 1, 20}};
    capability.live_audio_receive = true;
    VoiceRuntime absent(mqtt, media, "p/d", capability, "live-boot");
    EXPECT_EQ(absent.start({}, now), Result::Invalid);
    capability.live_audio_receive = false;
    VoiceRuntime unadvertised(mqtt, media, "p/d", capability, "live-boot", nullptr, nullptr,
                              nullptr, &live_endpoint);
    EXPECT_EQ(unadvertised.start({}, now), Result::Invalid);
    live_endpoint.matching = false;
    EXPECT_EQ(runtime.start({}, now), Result::Invalid);
}
// RuntimeWebRTCRecoveryTest经真实运行时泵入网络/驱动事件，不跳过请求关联或控制ACK。
class RuntimeWebRTCRecoveryTest : public RuntimeTest, public testing::WithParamInterface<bool> {
   protected:
    RuntimeWebRTCRecoveryTest()
        : RuntimeTest(false, false, false, false, VoiceTransport::WebRTC, GetParam()) {}
    // answer_last使用驱动刚交付的原Offer，为指定代次提供平台Answer。
    void answer_last(std::uint64_t revision) {
        const auto request = mqtt.sent.back();
        ASSERT_EQ(request.method, "mediaOffer");
        Envelope response;
        response.token = request.token;
        response.method = "mediaAnswer";
        response.session = "session";
        response.request = false;
        response.code = 200;
        response.payload = "{\"mediaRevision\":" + std::to_string(revision) +
                           R"(,"webrtc":{"iceMode":"complete","sdp":"v=0\r\no=answer\r\n"}})";
        body(response);
    }
    // establish_rtc实际逐次泵入能力、SDP、连通和建立事件，采集最后单独确认。
    void establish_rtc() {
        subscribed();
        ack_last(R"({"acceptedRevision":1})");
        Envelope created;
        created.token = mqtt.sent.back().token;
        created.method = "sessionCreated";
        created.session = "session";
        created.request = false;
        created.code = 200;
        created.payload =
            std::string(
                R"({"state":"negotiating","selected":{"transport":"webrtc","interactionMode":")") +
            (GetParam() ? "fullDuplex" : "halfDuplex") +
            R"(","audioParams":{"format":"opus","sampleRate":48000,"channels":1,"frameDuration":20}},"tracks":[{"trackId":"mic-1","kind":"audio","sourceId":"mic","direction":"sendonly","codec":"opus","state":"requested"},{"trackId":"speaker-1","kind":"audio","sourceId":"speaker","direction":"recvonly","codec":"opus","state":"requested"}]})";
        body(created);
        ASSERT_EQ(mqtt.sent.back().method, "mediaOffer");
        answer_last(1);
        media.connected = true;
        drive();
        ack_last(R"({"mediaRevision":1,"state":"connected"})");
        Envelope established;
        established.token = "established";
        established.method = "sessionEstablished";
        established.session = "session";
        established.request = false;
        established.event_sequence = 1;
        established.payload = R"({"state":"established","mediaRevision":1})";
        body(established);
        ASSERT_EQ(runtime.voice().state(), VoiceState::Established);
        ASSERT_EQ(runtime.start_input(++now), Result::Applied);
        ack_last(mqtt.sent.back().payload);
        ASSERT_TRUE(media.capture);
    }
};
TEST_P(RuntimeWebRTCRecoveryTest, PumpCreatesFreshPeerAndRestoresOriginalInputAfterACK) {
    establish_rtc();
    const auto input = media.input;
    const auto disconnects = mqtt.disconnects;
    media.disconnected = true;
    media.connected = false;
    drive();
    ASSERT_EQ(runtime.voice().state(), VoiceState::Recovering);
    ASSERT_EQ(mqtt.sent.back().method, "mediaState");
    EXPECT_EQ(media.peer_revision, 1U);
    EXPECT_FALSE(media.capture);
    ack_last(R"({"mediaRevision":1,"state":"disconnected"})");
    EXPECT_EQ(media.peer_revision, 2U);
    ASSERT_EQ(mqtt.sent.back().method, "mediaOffer");
    answer_last(2);
    EXPECT_FALSE(media.capture);
    media.connected = true;
    drive();
    ASSERT_EQ(mqtt.sent.back().method, "mediaState");
    EXPECT_FALSE(media.capture);
    ack_last(R"({"mediaRevision":2,"state":"connected"})");
    EXPECT_EQ(runtime.voice().state(), VoiceState::Established);
    EXPECT_EQ(runtime.state(), RuntimeState::Running);
    EXPECT_TRUE(media.capture);
    EXPECT_EQ(media.input, input);
    EXPECT_EQ(mqtt.disconnects, disconnects);
}
TEST_P(RuntimeWebRTCRecoveryTest, RecoveryPauseAndFailureDoNotResumeCapture) {
    establish_rtc();
    media.disconnected = true;
    media.connected = false;
    drive();
    const auto disconnect = mqtt.sent.back();
    ASSERT_EQ(runtime.stop_input(++now), Result::Applied);
    ack_last(mqtt.sent.back().payload);
    Envelope ack;
    ack.token = disconnect.token;
    ack.method = "requestAck";
    ack.session = "session";
    ack.request = false;
    ack.code = 200;
    ack.payload =
        R"({"requestMethod":"mediaState","result":{"mediaRevision":1,"state":"disconnected"}})";
    body(ack);
    answer_last(2);
    media.connected = true;
    drive();
    ack_last(R"({"mediaRevision":2,"state":"connected"})");
    EXPECT_EQ(runtime.voice().state(), VoiceState::Established);
    EXPECT_FALSE(media.capture);
    media.polling = false;
    drive();
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
    EXPECT_FALSE(media.configured);
    EXPECT_FALSE(media.capture);
}
TEST_P(RuntimeWebRTCRecoveryTest, TemporaryDisconnectUsesFixedDeadlineBeforeRequestRetry) {
    establish_rtc();
    media.disconnected = true;
    media.connected = false;
    drive();
    const auto first = now;
    const auto publications = mqtt.sent.size();
    now = first + 44999;
    runtime.poll(now, 1000000 + now);
    ASSERT_EQ(runtime.state(), RuntimeState::Running);
    ASSERT_EQ(runtime.voice().state(), VoiceState::Recovering);
    EXPECT_FALSE(media.capture);
    const auto retries = mqtt.sent.size();
    EXPECT_GT(retries, publications);
    now = first + 45000;
    runtime.poll(now, 1000000 + now);
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
    EXPECT_EQ(mqtt.sent.size(), retries);
    EXPECT_FALSE(media.configured);
}
INSTANTIATE_TEST_SUITE_P(HalfAndFullDuplex, RuntimeWebRTCRecoveryTest, testing::Bool());

// RuntimeEndpointSourceTest 用正式握手冻结源绑定，不将接听能力假报给仅发起接续的端。
class RuntimeEndpointSourceTest : public RuntimeTest {
protected:
    RuntimeEndpointSourceTest()
        : RuntimeTest(false, false, false, false, VoiceTransport::Udp, true, true) {}
    // fact 使用原请求关联和原源腿，不在测试里向控制器写ready或候选字段。
    Envelope fact(std::string state, unsigned revision, unsigned sequence = 0) {
        const auto* receipt = runtime.endpoint_source_receipt();
        EXPECT_NE(receipt, nullptr);
        Envelope result;
        result.token = sequence ? "source-event-" + std::to_string(sequence) : receipt->token;
        result.session = "session";
        result.method = "endpointState";
        result.request = false;
        if (sequence) result.event_sequence = sequence;
        else result.code = 200;
        result.payload = "{\"state\":\"" + state + "\",\"revision\":" + std::to_string(revision) +
            ",\"handoffId\":\"" + receipt->handoff + "\"";
        if (state == "ready" || state == "committed") result.payload += R"(,"candidateSessionId":"candidate")";
        if (state == "aborted") result.payload += R"(,"reason":"userCancelled")";
        result.payload += "}";
        return result;
    }
};

TEST_F(RuntimeEndpointSourceTest, LocalPrepareUsesOriginalLegAndDoesNotStopItsMedia) {
    established();
    ASSERT_EQ(runtime.start_input(++now), Result::Applied);
    ack_last(mqtt.sent.back().payload);
    ASSERT_TRUE(media.capture);
    const auto original = runtime.voice().session_id();
    const auto configurations = media.configured;
    ++now;
    ASSERT_EQ(runtime.prepare_endpoint_source("device:p:target", now, 1000000 + now), Result::Applied);
    ASSERT_NE(runtime.endpoint_source_receipt(), nullptr);
    EXPECT_EQ(mqtt.sent.back().method, "endpointControl");
    EXPECT_EQ(mqtt.sent.back().session, original);
    EXPECT_TRUE(runtime.endpoint_source_receipt()->pending);
    EXPECT_EQ(media.configured, configurations);
    EXPECT_TRUE(media.capture);
    EXPECT_EQ(runtime.voice().state(), VoiceState::Established);
}
TEST_F(RuntimeEndpointSourceTest, PrepareReadyCommitRequiresOriginalFactsAndRetainsMediaUntilEnd) {
    established();
    ++now;
    EXPECT_EQ(runtime.commit_endpoint_source(now, 1000000 + now), Result::Stale);
    ++now;
    ASSERT_EQ(runtime.prepare_endpoint_source("device:p:target", now, 1000000 + now), Result::Applied);
    ++now;
    EXPECT_EQ(runtime.commit_endpoint_source(now, 1000000 + now), Result::Busy);
    body(fact("preparing", 1));
    EXPECT_FALSE(runtime.endpoint_source_receipt()->pending);
    body(fact("ready", 2, 2));
    ASSERT_EQ(runtime.endpoint_source_receipt()->state, "ready");
    ++now;
    ASSERT_EQ(runtime.commit_endpoint_source(now, 1000000 + now), Result::Applied);
    EXPECT_EQ(mqtt.sent.back().method, "endpointControl");
    EXPECT_NE(mqtt.sent.back().payload.find(R"("operation":"commit")"), std::string::npos);
    body(fact("committed", 3));
    EXPECT_TRUE(media.configured);
    EXPECT_EQ(runtime.voice().state(), VoiceState::Established);
    EXPECT_EQ(runtime.endpoint_source_receipt()->state, "committed");
    Envelope ended;
    ended.token = "source-ended";
    ended.session = "session";
    ended.method = "sessionEnded";
    ended.request = false;
    ended.event_sequence = 3;
    ended.payload = R"({"reason":"sourceTransferred"})";
    body(ended);
    EXPECT_FALSE(media.configured);
    EXPECT_EQ(runtime.endpoint_source_receipt()->state, "committed");
    ++now;
    EXPECT_EQ(runtime.commit_endpoint_source(now, 1000000 + now), Result::Stale);
}
TEST_F(RuntimeEndpointSourceTest, SourceEventsShareOriginalWatermarkAndDoNotConsumeOtherAcks) {
    established();
    ++now;
    ASSERT_EQ(runtime.prepare_endpoint_source("device:p:target", now, 1000000 + now), Result::Applied);
    body(fact("preparing", 1));
    Envelope error;
    error.token = "original-response-error";
    error.session = "session";
    error.method = "sessionError";
    error.request = false;
    error.code = 503;
    error.event_sequence = 5;
    error.payload = R"({"scope":"response","respId":"reply","reason":"modelUnavailable","retryable":true})";
    body(error);
    ASSERT_EQ(runtime.voice().state(), VoiceState::Established);
    body(fact("ready", 2, 4)); // 旧SIG不能借控制器独立revision越过会话事件水位。
    EXPECT_EQ(runtime.endpoint_source_receipt()->state, "preparing");
    body(fact("ready", 2, 6));
    ASSERT_EQ(runtime.endpoint_source_receipt()->state, "ready");
    ASSERT_EQ(runtime.start_input(++now), Result::Applied);
    ack_last(mqtt.sent.back().payload);
    EXPECT_TRUE(media.capture);
    EXPECT_EQ(runtime.voice().state(), VoiceState::Established);
}
TEST_F(RuntimeEndpointSourceTest, OriginalIngressRejectionKeepsAiEstablishedAndAllowsNewIntent) {
    established();
    ++now;
    ASSERT_EQ(runtime.prepare_endpoint_source("device:p:denied", now, 1000000 + now), Result::Applied);
    const auto original = mqtt.sent.back();
    Envelope denied;
    denied.token = original.token;
    denied.session = "session";
    denied.method = "requestAck";
    denied.request = false;
    denied.code = 403;
    denied.payload = R"({"requestMethod":"endpointControl","reason":"requestDenied"})";
    body(denied);
    ASSERT_EQ(runtime.endpoint_source_receipt()->code, 403);
    EXPECT_FALSE(runtime.endpoint_source_receipt()->pending);
    EXPECT_EQ(runtime.voice().state(), VoiceState::Established);
    ++now;
    ASSERT_EQ(runtime.prepare_endpoint_source("device:p:target", now, 1000000 + now), Result::Applied);
    EXPECT_NE(mqtt.sent.back().token, original.token);
    body(denied); // 原拒绝重投不能回退新请求的pending。
    EXPECT_TRUE(runtime.endpoint_source_receipt()->pending);
}
// RuntimeFocusSuspendTest 使用可信HAL端口形式的原许可，不把网络请求本身当本机授权。
class RuntimeFocusSuspendTest : public RuntimeTest {
protected:
    explicit RuntimeFocusSuspendTest(bool full = true)
        : RuntimeTest(false, false, false, false, VoiceTransport::Udp, full) {}
    // permit 只建立离线测试的完整原许可，实际宿主须经唯一FocusAuthority核权。
    FocusPermit permit() const {
        FocusPermit result;
        EXPECT_EQ(decode_focus_control(WireRoute::Signaling,
            R"({"msgToken":"original-suspend","method":"focusControl","sessionId":"session","eventSeq":2,"params":{"operation":"suspend","expectedRevision":1,"focusId":"original-focus","resources":["microphone","speaker"],"priority":"conversation","resumePolicy":"explicit"}})",
            result.scope), WireResult::Ok); // 正式下行params是请求形态，不能手造data回执来冒充许可。
        result.deadline = 1000;
        result.intent_version = 1;
        return result;
    }
};
TEST_F(RuntimeFocusSuspendTest, ExactLocalSuspendRetiresInputAndDoesNotSendAudioStop) {
    established();
    ASSERT_EQ(runtime.start_input(++now), Result::Applied);
    ack_last(mqtt.sent.back().payload);
    ASSERT_TRUE(media.capture);
    const auto publications = mqtt.sent.size();
    ASSERT_EQ(runtime.suspend_input_for_focus(permit(), ++now), Result::Applied);
    EXPECT_FALSE(media.capture);
    EXPECT_EQ(mqtt.sent.size(), publications);
    EXPECT_EQ(runtime.voice().state(), VoiceState::Established);
    EXPECT_EQ(runtime.stop_input(++now), Result::Duplicate);
    EXPECT_EQ(runtime.start_input(++now), Result::Applied);
    EXPECT_FALSE(media.capture);  // 新用户动作仍须等新输入段自己的平台ACK。
    ack_last(mqtt.sent.back().payload);
    EXPECT_TRUE(media.capture);
}
TEST_F(RuntimeFocusSuspendTest, LateOriginalStartAckCannotReopenRetiredInput) {
    established();
    ASSERT_EQ(runtime.start_input(++now), Result::Applied);
    const auto original = mqtt.sent.back();
    ASSERT_EQ(runtime.suspend_input_for_focus(permit(), ++now), Result::Applied);
    ack_last(original.payload);
    EXPECT_FALSE(media.capture);
    EXPECT_EQ(runtime.voice().state(), VoiceState::Established);
    ASSERT_EQ(runtime.start_input(++now), Result::Applied);
    EXPECT_NE(mqtt.sent.back().token, original.token);
}
TEST_F(RuntimeFocusSuspendTest, RetiredStartDoesNotRetryOrFailOriginalSessionAtItsOldDeadline) {
    established();
    ASSERT_EQ(runtime.start_input(++now), Result::Applied);
    ASSERT_EQ(runtime.suspend_input_for_focus(permit(), ++now), Result::Applied);
    const auto publications = mqtt.sent.size();
    now += 30001;
    runtime.poll(now, 1000000 + now);
    EXPECT_EQ(mqtt.sent.size(), publications);
    EXPECT_FALSE(media.capture);
    EXPECT_EQ(runtime.voice().state(), VoiceState::Established);
}
TEST_F(RuntimeFocusSuspendTest, WrongScopePartialGroupRestoreAndExpiredPermitDoNothing) {
    established();
    ASSERT_EQ(runtime.start_input(++now), Result::Applied);
    ack_last(mqtt.sent.back().payload);
    for (unsigned index = 0; index < 7; ++index) {
        auto value = permit();
        if (index == 0) value.scope.envelope.session = "alien";
        if (index == 1) value.scope.resources = {"microphone"};
        if (index == 2) value.scope.operation = "restore";
        if (index == 3) value.deadline = now;
        if (index == 4) value.intent_version = 0;
        if (index == 5) value.scope.resources = {"microphone", "microphone"};
        if (index == 6) value.scope.expires_at = 10000;
        EXPECT_NE(runtime.suspend_input_for_focus(value, ++now), Result::Applied);
        EXPECT_TRUE(media.capture);
    }
}
// RuntimeFocusSuspendHalfTest 核验暂停前原回复仍在播的半双工分支，不等待跨通道取消事件。
class RuntimeFocusSuspendHalfTest : public RuntimeFocusSuspendTest {
protected:
    RuntimeFocusSuspendHalfTest() : RuntimeFocusSuspendTest(false) {}
};
TEST_F(RuntimeFocusSuspendHalfTest, RetiresOldPlaybackWithoutCancelRequestAndNewInputCanResume) {
    established(false);
    Envelope audio;
    audio.token = "original-audio";
    audio.session = "session";
    audio.method = "respAudioStart";
    audio.request = false;
    audio.event_sequence = 2;
    audio.payload = R"({"respId":"old-reply","trackId":"speaker-1","mediaRevision":1,"udpNonce":"9988776655443322","firstPacketSeq":128})";
    body(audio);
    ASSERT_TRUE(media.playback);
    const auto epoch = runtime.voice().playback_epoch();
    const auto publications = mqtt.sent.size();
    ASSERT_EQ(runtime.suspend_input_for_focus(permit(), ++now), Result::Applied);
    EXPECT_FALSE(media.playback);
    EXPECT_EQ(mqtt.sent.size(), publications);
    audio.token = "late-audio";
    audio.event_sequence = 3;
    body(audio);
    EXPECT_FALSE(media.playback);
    ASSERT_EQ(runtime.start_input(++now), Result::Applied);
    EXPECT_FALSE(media.capture);
    ack_last(mqtt.sent.back().payload);
    ASSERT_TRUE(media.capture);
    audio.event_sequence = 4;
    body(audio);
    EXPECT_TRUE(media.capture);
    EXPECT_FALSE(media.playback);
    audio.payload.replace(audio.payload.find("old-reply"), 9, "new-reply");
    audio.token = "new-audio";
    audio.event_sequence = 5;
    body(audio);
    EXPECT_TRUE(media.playback);
    media.drained = epoch; // 原媒体事件泵交旧排空回调，不经const查询接口改变核心状态。
    drive();
    media.drained.reset();
    EXPECT_TRUE(media.playback);
}
}  // namespace
}  // namespace ur::device
