// 文件说明：真实PCM与三个Live输出组的首次关闭合同；仅模拟网络和浏览器实际回执，不代表真机验收。
#include <gtest/gtest.h>
#include <array>
#include "browser_live_endpoint.hpp"
#include "browser_pcm.hpp"
#include "ur/device/voice_runtime.hpp"

namespace ur::device {
namespace {
// StartupMqtt 只计正式Runtime的连接调用，不建立网络或产生云端业务事实。
struct StartupMqtt final : MqttPort {
    unsigned connects = 0;  // 初始实际关闭不齐时必须保持零次连接。
    // connect 接纳离线连接请求；不伪造CONNACK或已建立会话。
    bool connect(const MqttConfig&) override {
        ++connects;
        return true;
    }
    // subscribe 不用于首次连接前的门控证明。
    int subscribe(std::string_view) override { return 1; }
    // publish 不注入平台ACK或媒体回执。
    bool publish(std::string_view, std::string_view) override { return true; }
    // poll 无网络事件，不启动实际传输。
    bool poll() override { return true; }
    // take_event 保持离线，Runtime只能停在Connecting。
    std::optional<MqttEvent> take_event() override { return {}; }
    // disconnect 无外部资源可释放。
    void disconnect() override {}
};
// StartupVoiceMedia 永不提供媒体连通或实际输入输出，测试只验证Runtime首次准入。
struct StartupVoiceMedia final : PumpedVoiceMediaPort {
    // configure 拒绝测试范围以外的媒体分配。
    bool configure(const VoiceMedia&) override { return false; }
    // ready 不把初始关闭当作媒体连通。
    bool ready() const override { return false; }
    // poll 不执行浏览器或音频设备操作。
    bool poll(std::uint64_t) override { return true; }
    // next_input_sequence 返回固定合法基线，不提交采集。
    std::uint32_t next_input_sequence() const override { return 1; }
    // prepare_reply 拒绝测试范围以外的回复输出。
    bool prepare_reply(const std::array<std::uint8_t, 8>&, std::uint32_t) override { return false; }
    // finish_reply 没有在途媒体可确认。
    bool finish_reply(std::uint32_t) override { return false; }
    // start_capture 禁止初始关闭测试启动采集。
    bool start_capture(std::string_view) override { return false; }
    // stop_capture 无实际采集资源。
    void stop_capture() override {}
    // start_playback 禁止初始关闭测试启动播放。
    bool start_playback(std::string_view, std::uint64_t) override { return false; }
    // stop_playback 无实际播放资源。
    void stop_playback(std::uint64_t) override {}
    // close 无网络或音频设备资源。
    void close() override {}
    // drained_epoch 不伪造实际排空事实。
    std::optional<std::uint64_t> drained_epoch() const override { return {}; }
};

// BrowserLiveStartupTest 复用宿主四条真实门控路径和正式Runtime；时钟推进代替等待。
class BrowserLiveStartupTest : public ::testing::Test {
protected:
    BrowserPcm pcm{{16000, 1, 20}};  // 与宿主相同的独立AI扬声器端口。
    std::array<BrowserLiveLane, 3> lanes{BrowserLiveLane("audio"), BrowserLiveLane("video"),
                                         BrowserLiveLane("audioVideo")};  // 三个固定节目完整组。
    OutputFocusDriver driver{{{"", "", {"speaker"}, &pcm},
                              {"", "", {"speaker"}, &lanes[0]},
                              {"", "", {"display"}, &lanes[1]},
                              {"", "", {"speaker", "display"}, &lanes[2]}},
                             0,
                             FocusBindingMode::DeferredReleased};  // 唯一全局仲裁器。
    StartupMqtt mqtt;                                      // 只核验是否过早进入正式Runtime连接。
    StartupVoiceMedia media;                               // 不产生媒体事实的离线端口。
    std::unique_ptr<BrowserLiveFactory> factory;           // 初始liveReady后的正式节目装配。
    std::unique_ptr<BrowserLiveEndpoint> live;             // 原唯一MQTT上的Live端点。
    std::unique_ptr<BrowserLiveFocusAuthority> authority;  // 读取原C++父与子范围。
    std::unique_ptr<FocusEndpoint> focus;                  // 首次基线来自真实四路径关闭证明。
    std::unique_ptr<VoiceRuntime> runtime;                 // 最后构造、最先销毁，不能借悬空端口。
    std::array<std::uint64_t, 4> tickets{};  // 实际交付的四条原票据，不以队列受理代替确认。
    bool healthy = true;                     // 与宿主相同，初始化失败不能重建旧路径或继续准入。
    static constexpr std::uint64_t started = 100, deadline = started + 2000;  // 固定单调截止。

    // SetUp 注入合法ready/liveReady，尚未交付任何关闭命令或浏览器实际ACK。
    void SetUp() override {
        ASSERT_TRUE(pcm.ready({16000, 1, 20}, true));
        ASSERT_TRUE(driver.initialize(started));
        const auto parent = [&]() -> std::optional<std::string> {
            if (!runtime || runtime->voice().state() != VoiceState::Established)
                return {};
            return runtime->voice().session_id();
        };
        factory = std::make_unique<BrowserLiveFactory>(
            std::array<BrowserLiveLane*, 3>{&lanes[0], &lanes[1], &lanes[2]},
            BrowserLiveReadiness{true, true}, parent, 10000);
        live = std::make_unique<BrowserLiveEndpoint>(
            mqtt, *factory, BrowserLiveReadiness{true, true}, "$thing/up/signaling/p/d",
            "$thing/down/signaling/p/d", 10000);
        authority = std::make_unique<BrowserLiveFocusAuthority>(
            driver, 10000, parent, [&] { return live->scope(); },
            [&](std::string_view session) { return live->release_scope(session); });
        focus = std::make_unique<FocusEndpoint>(
            mqtt, driver, *authority, std::vector<std::string>{"speaker", "display"},
            "$thing/up/signaling/p/d", "$thing/down/signaling/p/d", 10000, true);
        EXPECT_FALSE(driver.initial_idle());
        EXPECT_FALSE(driver.initial_idle_observation());
    }
    // deliver 领取宿主唯一原false命令；这里只交付，不证明浏览器实际关闭。
    void deliver() {
        const auto command = pcm.take_command();
        ASSERT_TRUE(command);
        ASSERT_EQ(command->kind, BrowserAudioCommandKind::OutputGate);
        ASSERT_FALSE(command->output_enabled);
        tickets[0] = command->gate_ticket;
        ASSERT_EQ(tickets[0], 1U);
        EXPECT_FALSE(pcm.take_command());
        for (std::size_t index = 0; index < lanes.size(); ++index) {
            const auto body = lanes[index].take_command();
            ASSERT_TRUE(body);
            EXPECT_EQ(*body, "{\"kind\":\"liveLaneGate\",\"lane\":\"" + lanes[index].lane() +
                                 "\",\"gateTicket\":\"1\",\"enabled\":false}");
            tickets[index + 1] = 1;
            EXPECT_FALSE(lanes[index].take_command());
        }
        EXPECT_FALSE(driver.initial_idle());
    }
    // ack 只给指定原路径回原false票据，经过真实端口解析或PCM实际确认入口。
    bool ack(std::size_t path, std::uint64_t now) {
        if (path == 0)
            return pcm.output_gated(tickets[0], false, now);
        auto& lane = lanes[path - 1];
        return lane.receive("{\"kind\":\"liveLaneGated\",\"lane\":\"" + lane.lane() +
                                "\",\"gateTicket\":\"" + std::to_string(tickets[path]) +
                                "\",\"enabled\":false}",
                            now);
    }
    // tick 泵真实端口和仲裁器；与browser_main相同，仅初始实际关闭齐全才创建正式Runtime。
    void tick(std::uint64_t now) {
        for (auto& lane : lanes)
            healthy = lane.poll(now) && healthy;
        healthy = pcm.poll_output_gate(now) && healthy;
        healthy = driver.poll(now) && healthy;
        if (healthy && !runtime && driver.initial_idle()) {
            runtime = std::make_unique<VoiceRuntime>(mqtt, media, "p/d",
                                                     VoiceCapability{{true, true, true},
                                                                     {16000, 1, 20},
                                                                     1,
                                                                     true,
                                                                     VoiceTransport::Udp,
                                                                     false,
                                                                     false,
                                                                     true,
                                                                     true},
                                                     "startup-boot", nullptr, nullptr, focus.get(),
                                                     live.get());
            EXPECT_EQ(runtime->start(MqttConfig{}, now), Result::Applied);
        }
    }
};

// 四条原实际ACK乱序到达，只能由最后一条补齐首次关闭并启动Runtime。
TEST_F(BrowserLiveStartupTest, OutOfOrderOriginalActualsStartRuntimeOnlyWhenAllFourArrive) {
    deliver();
    const std::array<std::size_t, 4> order{3, 1, 0, 2};  // 完整组先确认，PCM并非最后到达。
    for (std::size_t index = 0; index < order.size(); ++index) {
        const auto now = started + index + 1;  // 各事实共享同一单调时钟。
        ASSERT_TRUE(ack(order[index], now));
        tick(now);
        EXPECT_EQ(static_cast<bool>(runtime), index == order.size() - 1);
        EXPECT_EQ(mqtt.connects, index == order.size() - 1 ? 1U : 0U);
    }
    ASSERT_TRUE(runtime);
    EXPECT_EQ(runtime->state(), RuntimeState::Connecting);
    EXPECT_TRUE(driver.initial_idle_observation());
    EXPECT_EQ(driver.revision(), 0U);
}

// BrowserLiveMissingStartupAckTest 逐条遗漏四条实际关闭，不能由其他组或默认静音补齐。
class BrowserLiveMissingStartupAckTest : public BrowserLiveStartupTest,
                                         public ::testing::WithParamInterface<std::size_t> {};

// 任一路未知都必须在原两秒截止失败，迟到ACK不能重新准入。
TEST_P(BrowserLiveMissingStartupAckTest, MissingOneActualFailsAtOriginalTwoSecondDeadline) {
    deliver();
    for (std::size_t path = 0; path < tickets.size(); ++path) {
        if (path != GetParam()) {
            ASSERT_TRUE(ack(path, started + 1));
        }
    }
    tick(deadline - 1);
    EXPECT_TRUE(healthy);
    EXPECT_FALSE(runtime);
    EXPECT_EQ(mqtt.connects, 0U);
    EXPECT_FALSE(driver.initial_idle_observation());
    tick(deadline);
    EXPECT_FALSE(healthy);
    EXPECT_FALSE(runtime);
    EXPECT_EQ(mqtt.connects, 0U);
    EXPECT_FALSE(driver.initial_idle());
    EXPECT_FALSE(ack(GetParam(), deadline + 1));
    tick(deadline + 1);
    EXPECT_FALSE(runtime);
    EXPECT_EQ(mqtt.connects, 0U);
    EXPECT_FALSE(driver.initial_idle_observation());
}
INSTANTIATE_TEST_SUITE_P(EachRealOutput, BrowserLiveMissingStartupAckTest,
                         ::testing::Values(std::size_t{0}, std::size_t{1}, std::size_t{2},
                                           std::size_t{3}));

// 重复已确认事实不能补另一组或刷新另一组原门控的截止。
TEST_F(BrowserLiveStartupTest, DuplicatesCannotFillMissingGroupOrExtendItsDeadline) {
    deliver();
    for (std::size_t path = 0; path < 3; ++path)
        ASSERT_TRUE(ack(path, started + 1));
    for (const auto now : {deadline - 100, deadline - 1}) {
        for (std::size_t path = 0; path < 3; ++path)
            ASSERT_TRUE(ack(path, now));
        tick(now);
        EXPECT_TRUE(healthy);
        EXPECT_FALSE(runtime);
        EXPECT_EQ(mqtt.connects, 0U);
        EXPECT_FALSE(driver.initial_idle());
    }
    tick(deadline);
    EXPECT_FALSE(healthy);
    EXPECT_FALSE(ack(3, deadline + 1));
    tick(deadline + 1);
    EXPECT_FALSE(driver.initial_idle_observation());
    EXPECT_FALSE(runtime);
    EXPECT_EQ(mqtt.connects, 0U);
}

// 未交付的Live票据不能先确认；正常交付后仍需重新收到原实际ACK。
TEST_F(BrowserLiveStartupTest, UnsentLiveActualCannotConfirmInitialClose) {
    for (auto& lane : lanes) {
        EXPECT_FALSE(lane.receive("{\"kind\":\"liveLaneGated\",\"lane\":\"" + lane.lane() +
                                      "\",\"gateTicket\":\"1\",\"enabled\":false}",
                                  started + 1));
        EXPECT_FALSE(lane.idle());
    }
    tick(started + 1);
    EXPECT_FALSE(runtime);
    EXPECT_EQ(mqtt.connects, 0U);
    deliver();
    for (std::size_t path = 0; path < tickets.size(); ++path)
        ASSERT_TRUE(ack(path, started + 2));
    tick(started + 2);
    EXPECT_TRUE(healthy);
    EXPECT_TRUE(runtime);
    EXPECT_EQ(mqtt.connects, 1U);
}
}  // namespace
}  // namespace ur::device
