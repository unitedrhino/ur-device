// 文件说明：同一正式VoiceRuntime、Live端点与唯一焦点驱动的路由/关闭回归；模拟网络及实际驱动事实不代表真WebRTC。
#include <gtest/gtest.h>
#include <deque>
#include "browser_live_endpoint.hpp"
#include "ur/device/voice_runtime.hpp"

namespace ur::device {
namespace {
// CleanupMqtt 保留原订阅、请求与PUBACK排空边界；发布受理不能生成业务ACK或实际退出事实。
struct CleanupMqtt final : MqttPort {
    std::deque<MqttEvent> events;  // 受控原网络事件，到达仍通过生产运行时解析。
    std::vector<Envelope> sent;    // 原完整协议发布，不能把它当云端已收录。
    int subscription = 10;         // 原SUBACK必须关联精确独立MID。
    unsigned disconnects = 0;      // 核验原连接关闭时点，禁止物理false直接断连。
    bool drained = true;           // 独立网络确认，清理ACK只本地受理时仍不能断连。
    bool connect(const MqttConfig&) override { return true; }
    int subscribe(std::string_view) override { return subscription++; }
    bool publish(std::string_view, std::string_view bytes) override {
        Envelope message;
        EXPECT_EQ(decode_envelope(bytes, message), DecodeResult::Ok);
        sent.push_back(std::move(message));
        return true;
    }
    bool poll() override { return true; }
    bool publications_drained() override { return drained; }
    std::optional<MqttEvent> take_event() override {
        if (events.empty())
            return {};
        auto event = std::move(events.front());
        events.pop_front();
        return event;
    }
    void disconnect() override { ++disconnects; }
};
// CleanupVoiceMedia 明确交接AI网络事实；原父关闭后不再有麦克风/扬声器输出。
struct CleanupVoiceMedia final : PumpedVoiceMediaPort {
    bool configured = false, network = false;  // 分配与真实驱动连通分别注入。
    bool configure(const VoiceMedia&) override {
        configured = true;
        return true;
    }
    bool ready() const override { return network; }
    bool poll(std::uint64_t) override { return true; }
    std::uint32_t next_input_sequence() const override { return 1; }
    bool prepare_reply(const std::array<std::uint8_t, 8>&, std::uint32_t) override { return true; }
    bool finish_reply(std::uint32_t) override { return true; }
    bool start_capture(std::string_view) override { return true; }
    void stop_capture() override {}
    bool start_playback(std::string_view, std::uint64_t) override { return true; }
    void stop_playback(std::uint64_t) override {}
    void close() override { configured = network = false; }
    std::optional<std::uint64_t> drained_epoch() const override { return {}; }
};
// CleanupGate 每次新整组票据须独立实际确认；stop请求不自动成为Released修订。
struct CleanupGate final : OutputGroupGatePort {
    std::uint64_t ticket = 0;            // 原物理路径单调票据。
    bool enabled = false, done = false;  // 当前请求与实际完成分离。
    std::optional<std::uint64_t> request_output_gate(bool target, std::uint64_t) override {
        enabled = target;
        done = false;
        return ++ticket;
    }
    bool output_gate_confirmed(std::uint64_t original, bool target) const override {
        return done && ticket == original && enabled == target;
    }
};
// BrowserLiveRuntimeCleanupTest 不重建Runtime/设备/原MQTT；只把明确驱动事实交回同一生命周期。
class BrowserLiveRuntimeCleanupTest : public ::testing::Test {
protected:
    CleanupMqtt mqtt;
    CleanupVoiceMedia voice_media;
    std::array<CleanupGate, 4> gates;
    std::array<BrowserLiveLane, 3> lanes{BrowserLiveLane("audio"), BrowserLiveLane("video"),
                                         BrowserLiveLane("audioVideo")};
    std::unique_ptr<OutputFocusDriver> driver;
    std::unique_ptr<BrowserLiveFactory> factory;
    std::unique_ptr<BrowserLiveEndpoint> live;
    std::unique_ptr<BrowserLiveFocusAuthority> authority;
    std::unique_ptr<FocusEndpoint> focus;
    std::unique_ptr<VoiceRuntime> runtime;  // 借用对象最后构造最先销毁，不能悬空读取原权限。
    std::uint64_t now = 10;                 // 可控单调时钟；墙钟1000独立满足原协议截止。
    static constexpr const char* up = "$thing/up/signaling/p/d";
    static constexpr const char* down = "$thing/down/signaling/p/d";

    // tick 只前进同一单调时钟，独立墙钟不被拿来续长原资源截止。
    void tick() { runtime->poll(++now, 1000); }
    // cloud 原严格JSON沿同一个已鉴权Topic进入正式Runtime，不直接改内部状态。
    void cloud(Envelope message) {
        std::string bytes;
        ASSERT_EQ(encode_envelope(message, bytes), DecodeResult::Ok);
        mqtt.events.push_back({MqttEvent::Kind::Message, 0, 0, down, std::move(bytes)});
        tick();
    }
    // ack 只关联原最后发布token；驱动事实另行注入，业务ACK不伪造媒体连通。
    void ack(std::string result) {
        const auto request = mqtt.sent.back();
        Envelope message;
        message.request = false;
        message.code = 200;
        message.method = "requestAck";
        message.token = request.token;
        message.session = request.session;
        message.payload =
            "{\"requestMethod\":\"" + request.method + "\",\"result\":" + result + "}";
        cloud(std::move(message));
    }
    // SetUp 使用正式共享Live/Focus装配，物理路径预先实际false后才创建唯一运行时。
    void SetUp() override {
        for (auto& lane : lanes) {
            const auto ticket = lane.request_output_gate(false, ++now);
            ASSERT_TRUE(ticket);
            ASSERT_TRUE(lane.take_command());
            ASSERT_TRUE(lane.receive("{\"kind\":\"liveLaneGated\",\"lane\":\"" + lane.lane() +
                                         "\",\"gateTicket\":\"" + std::to_string(*ticket) +
                                         "\",\"enabled\":false}",
                                     ++now));
        }
        driver = std::make_unique<OutputFocusDriver>(
            std::vector<FocusOutputBinding>{{"", "", {"speaker"}, &gates[0]},
                                            {"", "", {"speaker"}, &gates[1]},
                                            {"", "", {"display"}, &gates[2]},
                                            {"", "", {"speaker", "display"}, &gates[3]}},
            0, FocusBindingMode::DeferredReleased);
        ASSERT_TRUE(driver->initialize(++now));
        for (auto& gate : gates)
            gate.done = true;
        ASSERT_TRUE(driver->poll(++now));
        const auto parent = [&]() -> std::optional<std::string> {
            if (!runtime || runtime->voice().state() != VoiceState::Established)
                return {};
            return runtime->voice().session_id();  // 只读原核心父身份，不由网页/邀请/夹具自报授权。
        };
        factory = std::make_unique<BrowserLiveFactory>(
            std::array<BrowserLiveLane*, 3>{&lanes[0], &lanes[1], &lanes[2]},
            BrowserLiveReadiness{true, true}, parent, 3600000);
        live = std::make_unique<BrowserLiveEndpoint>(
            mqtt, *factory, BrowserLiveReadiness{true, true}, up, down, 3600000);
        authority = std::make_unique<BrowserLiveFocusAuthority>(
            *driver, 3600000, parent, [&] { return live->scope(); },
            [&](std::string_view session) { return live->release_scope(session); });
        focus = std::make_unique<FocusEndpoint>(mqtt, *driver, *authority,
                                                std::vector<std::string>{"speaker", "display"}, up,
                                                down, 3600000);
        runtime = std::make_unique<VoiceRuntime>(mqtt, voice_media, "p/d",
                                                 VoiceCapability{{true, true, true},
                                                                 {16000, 1, 20},
                                                                 1,
                                                                 true,
                                                                 VoiceTransport::Udp,
                                                                 false,
                                                                 false,
                                                                 true,
                                                                 true},
                                                 "cleanup-boot", nullptr, nullptr, focus.get(),
                                                 live.get());
    }
    // parent_established 完整原MQTT订阅/创建/网络确认/平台建立，不伪造VoiceClient状态。
    void parent_established() {
        ASSERT_EQ(runtime->start({}, ++now), Result::Applied);
        mqtt.events.push_back({MqttEvent::Kind::Connected, 0, 0, {}, {}});
        tick();
        mqtt.events.push_back({MqttEvent::Kind::Subscribed, 10, 0, {}, {}});
        tick();
        mqtt.events.push_back({MqttEvent::Kind::Subscribed, 11, 0, {}, {}});
        tick();
        ASSERT_EQ(mqtt.sent.back().method, "capabilityReport");
        ack(R"({"acceptedRevision":1})");
        ASSERT_EQ(mqtt.sent.back().method, "sessionCreate");
        Envelope created;
        created.request = false;
        created.code = 200;
        created.token = mqtt.sent.back().token;
        created.method = "sessionCreated";
        created.session = "ai-parent";
        created.payload =
            R"({"state":"negotiating","selected":{"transport":"udp","interactionMode":"fullDuplex","audioParams":{"format":"opus","sampleRate":16000,"channels":1,"frameDuration":20},"inputModalities":["text"],"outputModalities":["text","audio"]},"tracks":[{"trackId":"mic-1","kind":"audio","sourceId":"mic","direction":"sendonly","codec":"opus","state":"requested"},{"trackId":"speaker-1","kind":"audio","sourceId":"speaker","direction":"recvonly","codec":"opus","state":"requested"}],"media":{"udp":{"server":"127.0.0.1","port":12345,"key":"000102030405060708090a0b0c0d0e0f","nonce":"01000000112233445566778800000000"}}})";
        cloud(std::move(created));
        ASSERT_TRUE(voice_media.configured);
        voice_media.network = true;
        tick();
        ASSERT_EQ(mqtt.sent.back().method, "mediaState");
        ack(R"({"mediaRevision":1,"state":"connected"})");
        Envelope established;
        established.request = false;
        established.token = "ai-established";
        established.method = "sessionEstablished";
        established.session = "ai-parent";
        established.event_sequence = 1;
        established.payload = R"({"state":"established","mediaRevision":1})";
        cloud(std::move(established));
        ASSERT_EQ(runtime->voice().state(), VoiceState::Established);
    }
    // programme_established 同一网络正式Live邀请/接听/媒体报告；本地端口事实保持原instance/m1。
    void programme_established() {
        parent_established();
        Envelope invitation;
        ASSERT_EQ(
            decode_envelope(
                R"({"method":"sessionInvite","msgToken":"live-invite","sessionId":"programme","eventSeq":1,"params":{"purpose":"liveBroadcast","admission":"direct","transport":"webrtc","expiresAt":100000,"tracks":[{"trackId":"broadcast-out","kind":"audio","sourceId":"broadcast-audio","direction":"recvonly","codec":"opus","state":"requested"},{"trackId":"broadcast-video-out","kind":"video","sourceId":"broadcast-video","direction":"recvonly","codec":"H264","state":"requested"}]}})",
                invitation),
            DecodeResult::Ok);
        cloud(std::move(invitation));
        ASSERT_EQ(mqtt.sent.back().method, "sessionAccept");
        ack(R"({"state":"negotiating"})");
        ASSERT_TRUE(live->active_media());
        ASSERT_TRUE(live->active_media()->take_command());
        ASSERT_TRUE(live->receive_local(
            R"({"kind":"liveOffer","instance":"1","mediaRevision":1,"sdp":"v=0\r\ncontract"})"));
        tick();
        ASSERT_EQ(mqtt.sent.back().method, "mediaOffer");
        Envelope answer;
        answer.request = false;
        answer.code = 200;
        answer.token = mqtt.sent.back().token;
        answer.session = "programme";
        answer.method = "mediaAnswer";
        answer.payload =
            R"({"mediaRevision":1,"webrtc":{"iceMode":"complete","sdp":"v=0\r\nanswer"}})";
        cloud(std::move(answer));
        ASSERT_TRUE(live->active_media()->take_command());
        ASSERT_TRUE(
            live->receive_local(R"({"kind":"liveConnected","instance":"1","mediaRevision":1})"));
        tick();
        ASSERT_EQ(mqtt.sent.back().method, "mediaState");
        ack(R"({"state":"connected","mediaRevision":1})");
        Envelope established;
        established.request = false;
        established.token = "live-established";
        established.session = "programme";
        established.method = "sessionEstablished";
        established.event_sequence = 2;
        established.payload = R"({"state":"established","mediaRevision":1})";
        cloud(std::move(established));
        ASSERT_TRUE(live->scope());
        ASSERT_TRUE(live->scope()->established);
    }
    // command 构造同原session/focus/完整组的独立CAS命令，不自报用户或新的授权。
    Envelope command(std::string operation, unsigned revision) {
        Envelope message;
        const auto bytes =
            "{\"method\":\"focusControl\",\"msgToken\":\"" + operation +
            "\",\"sessionId\":\"programme\",\"eventSeq\":" + std::to_string(revision + 1) +
            ",\"params\":{\"operation\":\"" + operation +
            "\",\"expectedRevision\":" + std::to_string(revision) +
            ",\"focusId\":\"original-focus\",\"resources\":[\"speaker\",\"display\"],\"priority\":"
            "\"background\",\"resumePolicy\":\"explicit\"" +
            ((operation == "acquire" || operation == "restore") ? ",\"expiresAt\":2000" : "") +
            "}}";
        EXPECT_EQ(decode_envelope(bytes, message), DecodeResult::Ok);
        return message;
    }
    // gate_complete 返回新票据对应实际整组完成，再由正式Runtime推进原回执。
    void gate_complete() {
        gates[3].done = true;
        ASSERT_TRUE(driver->poll(++now));
        tick();
    }
    // seed_original_held 隔离父关闭路由缺口；正常准入另例必须全走Runtime，不以此代替正常路由验收。
    void seed_original_held() {
        std::string bytes;
        ASSERT_EQ(encode_envelope(command("acquire", 0), bytes), DecodeResult::Ok);
        ASSERT_EQ(focus->receive(down, bytes, ++now, 1000), FocusEndpointResult::Handled);
        tick();
        gate_complete();
        ASSERT_EQ(driver->revision(), 1U);
    }
    // parent_end 原平台父终态经VoiceClient收录，不直接把Runtime置为Draining。
    void parent_end() {
        Envelope terminal;
        terminal.request = false;
        terminal.token = "parent-terminal";
        terminal.session = "ai-parent";
        terminal.method = "sessionEnded";
        terminal.event_sequence = 2;
        terminal.payload = R"({"state":"ended","reason":"idleTimeout"})";
        cloud(std::move(terminal));
    }
    // child_end 原独立节目平台终态才授有限关闭资格，不能由物理stop替代。
    void child_end() {
        Envelope terminal;
        terminal.request = false;
        terminal.token = "child-terminal";
        terminal.session = "programme";
        terminal.method = "sessionEnded";
        terminal.event_sequence = 3;
        terminal.payload = R"({"state":"ended","reason":"userStopped"})";
        cloud(std::move(terminal));
    }
    // actual_closed 分别交接真实媒体全部退出和仲裁器物理false，不生成focusRelease事实。
    void actual_closed() {
        ASSERT_TRUE(live->active_media());
        ASSERT_TRUE(live->active_media()->take_command());
        ASSERT_TRUE(live->receive_local(
            R"({"kind":"liveStopped","instance":"1","mediaRevision":1,"peerClosed":true,"tracksStopped":true,"outputsDetached":true,"callbacksStopped":true,"transformsStopped":true})"));
        gate_complete();
        ASSERT_TRUE(live->released());
    }
};
}  // namespace

TEST_F(BrowserLiveRuntimeCleanupTest, RunningRoutesLiveFocusAcquireSuspendRestoreAndRelease) {
    programme_established();
    for (const auto& operation : {"acquire", "suspend", "restore", "release"}) {
        const auto original = driver->revision();
        const auto before = mqtt.sent.size();
        cloud(command(operation, static_cast<unsigned>(original)));
        gate_complete();
        ASSERT_EQ(driver->revision(), original + 1) << operation;
        ASSERT_GT(mqtt.sent.size(), before);
        EXPECT_EQ(mqtt.sent.back().method, "focusState");
        EXPECT_EQ(mqtt.sent.back().code, 200);
        EXPECT_EQ(runtime->voice().state(), VoiceState::Established);
    }
}
TEST_F(BrowserLiveRuntimeCleanupTest, ParentEndedKeepsSameConnectionUntilOriginalActualCleanupAck) {
    programme_established();
    seed_original_held();
    const auto disconnects = mqtt.disconnects;
    parent_end();
    ASSERT_EQ(runtime->state(), RuntimeState::Draining);
    child_end();
    actual_closed();
    ASSERT_EQ(runtime->state(), RuntimeState::Draining);
    ASSERT_EQ(mqtt.disconnects,
              disconnects);  // 只有物理false，原租约未actual released，不能断MQTT。
    ASSERT_TRUE(live->release_scope("programme"));
    const auto original = gates[3].ticket;
    auto reopen = command("acquire", 1);
    reopen.token = "fresh-acquire";
    cloud(std::move(reopen));  // Closing不能按新token重获资源。
    EXPECT_EQ(gates[3].ticket, original);
    EXPECT_EQ(driver->revision(), 1U);
    mqtt.drained = false;
    cloud(command("release", 1));
    ASSERT_GT(gates[3].ticket, original);
    EXPECT_EQ(driver->revision(), 1U);
    gate_complete();
    ASSERT_EQ(driver->revision(), 2U);
    EXPECT_EQ(runtime->state(), RuntimeState::Draining);
    EXPECT_EQ(mqtt.sent.back().method, "focusState");
    EXPECT_EQ(mqtt.sent.back().code, 200);
    EXPECT_EQ(mqtt.disconnects, disconnects);
    mqtt.drained = true;
    tick();
    EXPECT_EQ(runtime->state(), RuntimeState::Idle);
    EXPECT_EQ(mqtt.disconnects, disconnects + 1);
}
TEST_F(BrowserLiveRuntimeCleanupTest, UnknownOriginalAcquisitionCannotSettleFromPhysicalStopAlone) {
    programme_established();
    std::string bytes;
    ASSERT_EQ(encode_envelope(command("acquire", 0), bytes), DecodeResult::Ok);
    ASSERT_EQ(focus->receive(down, bytes, ++now, 1000), FocusEndpointResult::Handled);
    tick();  // 原驱动已开始，尚无Core实际acquired回执；取消后不能补造known-held或Released。
    const auto disconnects = mqtt.disconnects;
    parent_end();
    child_end();
    actual_closed();
    EXPECT_EQ(runtime->state(), RuntimeState::Draining);
    EXPECT_EQ(mqtt.disconnects, disconnects);
    cloud(command("release", 0));
    EXPECT_EQ(driver->revision(), 0U);
    EXPECT_EQ(runtime->state(), RuntimeState::Draining);
    EXPECT_EQ(mqtt.disconnects, disconnects);
}
TEST_F(BrowserLiveRuntimeCleanupTest, NoFocusLeaseStillWaitsForOriginalPlatformChildTerminal) {
    programme_established();
    ASSERT_FALSE(live->active_media()->stop_all());
    actual_closed();  // 原真实资源已退出，但没有平台childEnded，更不能由父Ended补造。
    const auto disconnects = mqtt.disconnects;
    parent_end();
    ASSERT_EQ(runtime->state(), RuntimeState::Draining);
    EXPECT_EQ(mqtt.disconnects, disconnects);
    child_end();
    EXPECT_EQ(runtime->state(), RuntimeState::Idle);
    EXPECT_EQ(mqtt.disconnects, disconnects + 1);
}
TEST_F(BrowserLiveRuntimeCleanupTest, ActualClosedBeforeChildTerminalKeepsOriginalCleanupIdentity) {
    programme_established();
    seed_original_held();
    parent_end();
    actual_closed();
    EXPECT_FALSE(live->release_scope("programme"));
    EXPECT_EQ(runtime->state(), RuntimeState::Draining);
    child_end();
    ASSERT_TRUE(live->release_scope("programme"));
    cloud(command("release", 1));
    gate_complete();
    EXPECT_EQ(driver->revision(), 2U);
    EXPECT_EQ(runtime->state(), RuntimeState::Idle);
}
TEST_F(BrowserLiveRuntimeCleanupTest, DrainingRejectsOtherWritesAndUnknownSessionCleanup) {
    programme_established();
    seed_original_held();
    parent_end();
    child_end();
    actual_closed();
    const auto original = gates[3].ticket;
    for (const auto& operation : {"acquire", "restore", "suspend"}) {
        auto denied = command(operation, 1);
        denied.token = std::string("new-") + operation;
        cloud(std::move(denied));
    }
    auto foreign = command("release", 1);
    foreign.session = "unknown-programme";
    cloud(std::move(foreign));
    EXPECT_EQ(gates[3].ticket, original);
    EXPECT_EQ(driver->revision(), 1U);
    EXPECT_EQ(runtime->state(), RuntimeState::Draining);
    EXPECT_EQ(runtime->start({}, ++now), Result::Busy);
    cloud(command("release", 1));
    gate_complete();
    EXPECT_EQ(runtime->state(), RuntimeState::Idle);
}
TEST_F(BrowserLiveRuntimeCleanupTest, ClockRollbackRevokesCleanupWithoutCreatingActualRelease) {
    programme_established();
    seed_original_held();
    parent_end();
    child_end();
    actual_closed();
    ASSERT_TRUE(live->release_scope("programme"));
    runtime->poll(now - 1, 1000);
    EXPECT_EQ(runtime->state(), RuntimeState::Failed);
    EXPECT_FALSE(live->release_scope("programme"));
    EXPECT_EQ(driver->revision(), 1U);
    EXPECT_EQ(runtime->start({}, ++now), Result::Busy);
}
TEST_F(BrowserLiveRuntimeCleanupTest, DrainDeadlineFailsInsteadOfFabricatingLeaseSettlement) {
    programme_established();
    seed_original_held();
    parent_end();
    const auto original_deadline = now + 30000;
    child_end();
    actual_closed();
    child_end();  // 原终态重放不能刷新固定收尾截止。
    now = original_deadline;
    runtime->poll(now, 1000);
    EXPECT_EQ(runtime->state(), RuntimeState::Failed);
    EXPECT_FALSE(live->release_scope("programme"));
    EXPECT_EQ(driver->revision(), 1U);
    EXPECT_EQ(runtime->start({}, ++now), Result::Busy);
}
TEST_F(BrowserLiveRuntimeCleanupTest, LocalEndKeepsOnlyOriginalCleanupReleaseRoute) {
    programme_established();
    seed_original_held();
    ASSERT_EQ(runtime->end(++now), Result::Applied);
    ASSERT_EQ(runtime->voice().state(), VoiceState::Ending);
    ASSERT_EQ(runtime->state(), RuntimeState::Running);
    child_end();
    actual_closed();
    ASSERT_TRUE(live->release_scope("programme"));
    const auto original = gates[3].ticket;
    auto restore = command("restore", 1);
    restore.token = "closing-restore";
    cloud(std::move(restore));
    auto acquire = command("acquire", 1);
    acquire.token = "closing-acquire";
    cloud(std::move(acquire));
    auto foreign = command("release", 1);
    foreign.session = "unknown-programme";
    cloud(std::move(foreign));
    EXPECT_EQ(gates[3].ticket, original);
    EXPECT_EQ(driver->revision(), 1U);
    cloud(command("release", 1));
    ASSERT_GT(gates[3].ticket, original);  // Ending保原同连接，仅受理原平台终态完整组的新关闭。
    gate_complete();
    EXPECT_EQ(driver->revision(), 2U);
    EXPECT_EQ(runtime->voice().state(), VoiceState::Ending);
    parent_end();
    EXPECT_EQ(runtime->state(), RuntimeState::Idle);
}
}  // namespace ur::device
