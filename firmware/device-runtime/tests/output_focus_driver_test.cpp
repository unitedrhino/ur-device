// 文件说明：实际共享仲裁核心与受控门控端口的离线回归；端口确认不是硬件或后端E2E证据。
#include "ur/device/output_focus_driver.hpp"
#include <gtest/gtest.h>
#include "ur/device/focus_endpoint.hpp"

using namespace ur::device;
namespace {
// Gate将受理、在途和实际原票据确认分开，不在请求时自动确认声音/画面已停止。
struct Gate final : OutputGroupGatePort {
    std::uint64_t ticket = 0;  // 每个独占路径的单调原票据。
    bool enabled = false, pending = false, actual = false, refuse = false;  // 受控路径事实及背压。
    unsigned calls = 0;  // 非法参数不得触发端口动作。
    std::optional<std::uint64_t> request_output_gate(bool target, std::uint64_t) override {
        ++calls;
        if (pending || refuse)
            return {};
        enabled = target;
        pending = true;
        actual = false;
        return ++ticket;
    }
    bool output_gate_confirmed(std::uint64_t original, bool target) const override {
        return actual && !pending && original == ticket && target == enabled;
    }
    void finish() {
        pending = false;
        actual = true;
    }  // 显式模拟输出线程实际完成。
};
// command生成合法原下行，资源组为JSON夹具片段，身份不从网络或设备凭据读取。
FocusCommand command(const std::string& session, const std::string& operation = "acquire",
                     unsigned revision = 0, const std::string& resources = "\"speaker\"",
                     const std::string& token = "one") {
    const std::string json =
        "{\"msgToken\":\"" + token + "\",\"method\":\"focusControl\",\"sessionId\":\"" + session +
        "\",\"eventSeq\":1,\"params\":{\"operation\":\"" + operation +
        "\",\"expectedRevision\":" + std::to_string(revision) +
        ",\"focusId\":\"f\",\"resources\":[" + resources +
        "],\"priority\":\"conversation\",\"resumePolicy\":\"explicit\"" +
        ((operation == "acquire" || operation == "restore") ? ",\"expiresAt\":2000" : "") + "}}";
    FocusCommand result;  // 真正公共解析器，测试不直接拼造未校验结构体。
    EXPECT_EQ(decode_focus_control(WireRoute::Signaling, json, result), WireResult::Ok);
    return result;
}
// ready所有端口实际确认后才建立独立首次空闲观察；不能由initialize返回值猜测。
void ready(OutputFocusDriver& driver, std::initializer_list<Gate*> gates) {
    ASSERT_TRUE(driver.initialize(10));
    ASSERT_TRUE(driver.poll(11));
    EXPECT_FALSE(driver.initial_idle());
    for (auto* gate : gates)
        gate->finish();
    ASSERT_TRUE(driver.poll(12));
    EXPECT_TRUE(driver.initial_idle());
}
// complete返回原实际事实，重复取走不得重新推进全局修订号。
FocusFact complete(OutputFocusDriver& driver, Gate& gate, const FocusCommand& request,
                   std::uint64_t generation = 1) {
    EXPECT_TRUE(driver.begin(request, generation));
    EXPECT_FALSE(driver.owns(request));
    gate.finish();
    EXPECT_TRUE(driver.poll(20));
    auto fact = driver.take_fact(request.envelope.session, request.focus_id);
    EXPECT_TRUE(fact.has_value());
    return fact.value_or(FocusFact{});
}
}  // namespace

// 新node只由原仲裁器发新false票据；未实际确认前保持原CAS且不得重用旧票据。
TEST(OutputFocusDriver, IdleRefreshOwnsNewTicketAndNeverCreatesLeaseOrRevision) {
    Gate gate;
    OutputFocusDriver driver({{"", "", {"speaker"}, &gate}}, 7, FocusBindingMode::DeferredReleased);
    ready(driver, {&gate});
    const auto original = gate.ticket;
    ASSERT_TRUE(driver.refresh_idle(0, 13));
    EXPECT_EQ(gate.ticket, original + 1);
    EXPECT_FALSE(driver.idle_refresh_ready(0));
    EXPECT_FALSE(driver.initial_idle_observation());
    EXPECT_FALSE(driver.refresh_idle(0, 14));
    EXPECT_FALSE(driver.bind_scope(0, "new", "f", 14));
    EXPECT_EQ(driver.revision(), 7U);
    gate.finish();
    ASSERT_TRUE(driver.poll(15));
    EXPECT_TRUE(driver.idle_refresh_ready(0));
    EXPECT_FALSE(driver.idle_refresh_failed(0));
    ASSERT_TRUE(driver.bind_scope(0, "new", "f", 16));
    EXPECT_EQ(driver.revision(), 7U);
}
// 已取得/暂停/未收录完成都仍是原lease，不能以false静音状态偷换其身份。
TEST(OutputFocusDriver, IdleRefreshRequiresReleasedAndConsumedOriginalFact) {
    Gate gate;
    OutputFocusDriver driver({{"", "", {"speaker"}, &gate}}, 0, FocusBindingMode::DeferredReleased);
    ready(driver, {&gate});
    ASSERT_TRUE(driver.bind_scope(0, "first", "f", 13));
    EXPECT_TRUE(complete(driver, gate, command("first")).success);
    EXPECT_FALSE(driver.refresh_idle(0, 20));
    ASSERT_TRUE(driver.begin(command("first", "suspend", 1), 2));
    gate.finish();
    ASSERT_TRUE(driver.poll(21));
    EXPECT_FALSE(driver.refresh_idle(0, 21));
    ASSERT_TRUE(driver.take_fact("first", "f"));
    EXPECT_FALSE(driver.refresh_idle(0, 22));
    ASSERT_TRUE(driver.begin(command("first", "release", 2), 3));
    gate.finish();
    ASSERT_TRUE(driver.poll(23));
    EXPECT_FALSE(driver.refresh_idle(0, 23));
    ASSERT_TRUE(driver.take_fact("first", "f"));
    ASSERT_TRUE(driver.refresh_idle(0, 24));
    EXPECT_EQ(driver.revision(), 3U);
    gate.finish();
    ASSERT_TRUE(driver.poll(25));
    ASSERT_TRUE(driver.bind_scope(0, "next", "f", 26));
    EXPECT_FALSE(driver.bind_scope(0, "first", "f", 26));
}
// 刷新期间阻止共享物理资源的新取得，但不得暂停已持有的其他正常输出路径。
TEST(OutputFocusDriver, IdleRefreshBlocksOverlappingAcquireWithoutStoppingCurrentOwner) {
    Gate ai, live;
    OutputFocusDriver driver(
        {{"ai", "f", {"speaker"}, &ai}, {"live", "f", {"speaker", "display"}, &live}}, 0);
    ready(driver, {&ai, &live});
    const auto original = command("ai");
    EXPECT_TRUE(complete(driver, ai, original).success);
    ASSERT_TRUE(driver.refresh_idle(1, 21));
    EXPECT_TRUE(driver.owns(original));
    EXPECT_FALSE(driver.begin(command("live", "acquire", 1, "\"speaker\",\"display\""), 2));
    ASSERT_TRUE(driver.poll(22));
    EXPECT_TRUE(driver.owns(original));
    live.finish();
    ASSERT_TRUE(driver.poll(23));
    EXPECT_TRUE(driver.idle_refresh_ready(1));
    EXPECT_TRUE(driver.owns(original));
    EXPECT_EQ(driver.revision(), 1U);
}
// 局部新node超时只封该路径；迟到false不复活它，也不销毁无关正在工作的原AI。
TEST(OutputFocusDriver, IdleRefreshTimeoutIsLocalAndLateConfirmationCannotRevive) {
    Gate ai, live;
    OutputFocusDriver driver({{"ai", "f", {"speaker"}, &ai}, {"live", "f", {"display"}, &live}}, 0);
    ready(driver, {&ai, &live});
    const auto original = command("ai");
    EXPECT_TRUE(complete(driver, ai, original).success);
    ASSERT_TRUE(driver.refresh_idle(1, 21));
    ASSERT_TRUE(driver.poll(2021));
    EXPECT_TRUE(driver.idle_refresh_failed(1));
    EXPECT_FALSE(driver.idle_refresh_ready(1));
    EXPECT_TRUE(driver.owns(original));
    EXPECT_TRUE(ai.enabled);
    live.finish();
    ASSERT_TRUE(driver.poll(2022));
    EXPECT_TRUE(driver.idle_refresh_failed(1));
    EXPECT_FALSE(driver.refresh_idle(1, 2023));
    EXPECT_FALSE(driver.begin(command("live", "acquire", 1, "\"display\""), 2));
    EXPECT_TRUE(driver.owns(original));
    EXPECT_EQ(driver.revision(), 1U);
}
// 背压、错误索引与回退不得发票据；被动stop永不冒充已Released刷新资格。
TEST(OutputFocusDriver, IdleRefreshRejectsBackpressureRollbackAndPassiveStop) {
    Gate gate;
    OutputFocusDriver driver({{"original", "f", {"speaker"}, &gate}}, 0);
    ready(driver, {&gate});
    const auto calls = gate.calls;
    EXPECT_FALSE(driver.refresh_idle(2, 13));
    EXPECT_FALSE(driver.refresh_idle(0, 1));
    EXPECT_EQ(gate.calls, calls);
    gate.refuse = true;
    EXPECT_FALSE(driver.refresh_idle(0, 13));
    EXPECT_TRUE(driver.idle_refresh_ready(0));
    EXPECT_EQ(gate.ticket, 1U);
    gate.refuse = false;
    ASSERT_TRUE(driver.stop_scope("original", "f", {"speaker"}));
    EXPECT_FALSE(driver.refresh_idle(0, 14));
    EXPECT_EQ(driver.revision(), 0U);
}

TEST(OutputFocusDriver, InitialObservationNeedsWholePhysicalGroupAndCannotReturnAfterUse) {
    Gate ai, file;
    OutputFocusDriver driver({{"", "", {"speaker"}, &ai}, {"", "", {"speaker", "display"}, &file}},
                             7, FocusBindingMode::DeferredOnce);
    EXPECT_FALSE(driver.initial_idle_observation());
    ASSERT_TRUE(driver.initialize(10));
    ai.finish();
    ASSERT_TRUE(driver.poll(11));
    EXPECT_FALSE(driver.initial_idle_observation());
    file.finish();
    ASSERT_TRUE(driver.poll(12));
    auto observed = driver.initial_idle_observation();
    ASSERT_TRUE(observed);
    EXPECT_EQ(observed->resources, (std::vector<std::string>{"speaker", "display"}));
    EXPECT_EQ(observed->revision, 7U);
    ASSERT_TRUE(driver.bind_scope(0, "ai", "f", 13));
    EXPECT_TRUE(driver.initial_idle_observation());
    ASSERT_TRUE(driver.begin(command("ai", "acquire", 7), 1));
    EXPECT_FALSE(driver.initial_idle_observation());
    ai.finish();
    ASSERT_TRUE(driver.poll(14));
    EXPECT_FALSE(driver.initial_idle_observation());
}

TEST(OutputFocusDriver, EndpointBaselineIsExplicitAndRequiresCurrentExactPhysicalObservation) {
    // 以下端口不执行媒体/授权；本用例只测试实际驱动观察经正式能力编码，不是MQTT E2E。
    struct Network final : PublishPort {
        bool publish(std::string_view, std::string_view) override { return false; }
    } network;
    struct Authority final : FocusAuthority {
        std::optional<FocusPermit> authorize(const FocusCommand&, std::uint64_t,
                                             std::uint64_t) override {
            return {};
        }
        bool valid(const FocusPermit&, std::uint64_t, std::uint64_t) const override {
            return false;
        }
    } authority;
    Gate gate;
    OutputFocusDriver driver({{"", "", {"speaker"}, &gate}}, 0, FocusBindingMode::DeferredOnce);
    FocusEndpoint explicit_report(network, driver, authority, {"speaker"}, "up", "down", 1000,
                                  true);
    FocusEndpoint original(network, driver, authority, {"speaker"}, "up", "down", 1000);
    FocusEndpoint wrong_group(network, driver, authority, {"speaker", "display"}, "up", "down",
                              1000, true);
    Envelope source, out;
    ASSERT_EQ(capability_command("original", VoiceCapability{}, source), WireResult::Ok);
    EXPECT_EQ(explicit_report.capability(source, out), WireResult::Invalid);
    EXPECT_TRUE(out.token.empty());
    EXPECT_EQ(original.capability(source, out), WireResult::Ok);
    EXPECT_EQ(out.payload.find("\"baseline\""), std::string::npos);
    ready(driver, {&gate});
    ASSERT_EQ(explicit_report.capability(source, out), WireResult::Ok);
    EXPECT_EQ(out.token, source.token);
    EXPECT_NE(out.payload.find("\"baseline\""), std::string::npos);
    EXPECT_NE(out.payload.find("\"state\":\"idle\""), std::string::npos);
    EXPECT_EQ(wrong_group.capability(source, out), WireResult::Invalid);
    EXPECT_TRUE(out.token.empty());
    gate.actual = false;
    EXPECT_EQ(explicit_report.capability(source, out), WireResult::Invalid);
}

TEST(OutputFocusDriver, DeferredScopeNeedsActualIdleAndNeverEnablesOutput) {
    Gate ai, file;
    OutputFocusDriver driver({{"", "", {"speaker"}, &ai}, {"", "", {"speaker", "display"}, &file}},
                             0, FocusBindingMode::DeferredOnce);
    EXPECT_FALSE(driver.bind_scope(0, "ai", "f", 9));
    ASSERT_TRUE(driver.initialize(10));
    ai.finish();
    EXPECT_FALSE(driver.bind_scope(0, "ai", "f", 11));
    file.finish();
    ASSERT_TRUE(driver.poll(12));
    ASSERT_TRUE(driver.bind_scope(0, "ai", "f", 13));
    ASSERT_TRUE(driver.bind_scope(1, "file", "f", 14));
    EXPECT_TRUE(driver.initial_idle());
    EXPECT_EQ(driver.revision(), 0U);
    EXPECT_EQ(ai.calls, 1U);
    EXPECT_EQ(file.calls, 1U);
    EXPECT_FALSE(ai.enabled);
    EXPECT_FALSE(file.enabled);
    EXPECT_FALSE(driver.bind_scope(0, "different", "f", 15));
    EXPECT_TRUE(complete(driver, ai, command("ai")).success);
    EXPECT_FALSE(driver.begin(command("file", "acquire", 1, "\"speaker\",\"display\""), 1));
}

TEST(OutputFocusDriver, DeferredScopeRejectsDuplicateMalformedAndTerminalReuse) {
    Gate ai, file;
    OutputFocusDriver driver({{"", "", {"speaker"}, &ai}, {"", "", {"speaker"}, &file}}, 0,
                             FocusBindingMode::DeferredOnce);
    ready(driver, {&ai, &file});
    EXPECT_FALSE(driver.bind_scope(2, "ai", "f", 13));
    EXPECT_FALSE(driver.bind_scope(0, "", "f", 13));
    EXPECT_FALSE(driver.bind_scope(0, std::string(65, 's'), "f", 13));
    EXPECT_FALSE(driver.bind_scope(0, "ai", "bad\n", 13));
    ASSERT_TRUE(driver.bind_scope(0, "ai", "f", 13));
    EXPECT_FALSE(driver.bind_scope(1, "ai", "f", 13));
    EXPECT_TRUE(complete(driver, ai, command("ai")).success);
    EXPECT_TRUE(
        complete(driver, ai, command("ai", "release", 1, "\"speaker\"", "release"), 2).success);
    EXPECT_FALSE(driver.bind_scope(0, "new-ai", "new-f", 21));
    EXPECT_EQ(driver.revision(), 2U);
}

// 复用的必要条件是原release实际完成且事实被取走；绑定本身既不启音也不回零水位。
TEST(OutputFocusDriver, ReleasedScopeCanBeReplacedWithoutRevivingOldIdentity) {
    Gate gate;
    OutputFocusDriver driver({{"", "", {"speaker"}, &gate}}, 0, FocusBindingMode::DeferredReleased);
    ready(driver, {&gate});
    ASSERT_TRUE(driver.bind_scope(0, "first", "f", 13));
    EXPECT_TRUE(complete(driver, gate, command("first")).success);
    EXPECT_FALSE(driver.bind_scope(0, "second", "f", 20));
    ASSERT_TRUE(driver.begin(command("first", "release", 1, "\"speaker\"", "stop"), 2));
    EXPECT_FALSE(driver.bind_scope(0, "second", "f", 20));
    gate.finish();
    ASSERT_TRUE(driver.poll(21));
    EXPECT_FALSE(driver.bind_scope(0, "second", "f", 21));
    ASSERT_TRUE(driver.take_fact("first", "f"));
    const auto calls = gate.calls;
    ASSERT_TRUE(driver.bind_scope(0, "second", "f", 22));
    EXPECT_EQ(gate.calls, calls);
    EXPECT_EQ(driver.revision(), 2U);
    EXPECT_FALSE(driver.initial_idle());
    EXPECT_FALSE(driver.begin(command("first", "acquire", 2), 3));
    EXPECT_FALSE(driver.stop_scope("first", "f", {"speaker"}));
    EXPECT_FALSE(driver.bind_scope(0, "first", "f", 22));
    ASSERT_TRUE(driver.begin(command("second", "acquire", 2, "\"speaker\"", "next"), 3));
    gate.finish();
    ASSERT_TRUE(driver.poll(23));
    ASSERT_TRUE(driver.take_fact("second", "f"));
    EXPECT_EQ(driver.revision(), 3U);
    EXPECT_TRUE(driver.owns(command("second", "acquire", 2, "\"speaker\"", "next")));
}

// 被动停止不伪装成显式release成功；失联或许可撤销后的路径必须保持终态关闭。
TEST(OutputFocusDriver, ReusableScopeRejectsPassiveCloseAndUnknownPhysicalState) {
    Gate gate;
    OutputFocusDriver driver({{"", "", {"speaker"}, &gate}}, 0, FocusBindingMode::DeferredReleased);
    ready(driver, {&gate});
    ASSERT_TRUE(driver.bind_scope(0, "first", "f", 13));
    EXPECT_TRUE(complete(driver, gate, command("first")).success);
    EXPECT_FALSE(driver.stop_scope("first", "f", {"speaker"}));
    EXPECT_FALSE(driver.bind_scope(0, "second", "f", 20));
    gate.finish();
    ASSERT_TRUE(driver.poll(21));
    EXPECT_TRUE(driver.stop_scope("first", "f", {"speaker"}));
    EXPECT_FALSE(driver.bind_scope(0, "second", "f", 22));
    EXPECT_EQ(driver.revision(), 1U);
}

// 连接内旧身份记录有界但不可淘汰，容量满时拒绝新节目，不让早期迟到控制重新命中。
TEST(OutputFocusDriver, ReusableScopeKeepsBoundedNonEvictingRetirementFence) {
    Gate gate;
    OutputFocusDriver driver({{"", "", {"speaker"}, &gate}}, 0, FocusBindingMode::DeferredReleased);
    ready(driver, {&gate});
    std::uint64_t now = 13;
    for (unsigned index = 0; index <= 128; ++index) {
        const auto id = "program-" + std::to_string(index);
        ASSERT_TRUE(driver.bind_scope(0, id, "f", now++));
        ASSERT_TRUE(driver.begin(command(id, "acquire", index * 2), index * 2 + 1));
        gate.finish();
        ASSERT_TRUE(driver.poll(now++));
        ASSERT_TRUE(driver.take_fact(id, "f"));
        ASSERT_TRUE(driver.begin(command(id, "release", index * 2 + 1), index * 2 + 2));
        gate.finish();
        ASSERT_TRUE(driver.poll(now++));
        ASSERT_TRUE(driver.take_fact(id, "f"));
    }
    EXPECT_FALSE(driver.bind_scope(0, "program-129", "f", now));
    EXPECT_FALSE(driver.bind_scope(0, "program-0", "f", now));
    EXPECT_FALSE(driver.begin(command("program-0", "acquire", 258), 259));
    EXPECT_EQ(driver.revision(), 258U);
    EXPECT_FALSE(gate.enabled);
}

TEST(OutputFocusDriver, DeferredScopeCannotBindDuringPendingOrAfterClockFailure) {
    Gate ai, file;
    OutputFocusDriver driver({{"", "", {"speaker"}, &ai}, {"", "", {"display"}, &file}}, 0,
                             FocusBindingMode::DeferredOnce);
    ready(driver, {&ai, &file});
    ASSERT_TRUE(driver.bind_scope(0, "ai", "f", 13));
    ASSERT_TRUE(driver.begin(command("ai"), 1));
    EXPECT_FALSE(driver.bind_scope(1, "file", "f", 14));
    ai.finish();
    ASSERT_TRUE(driver.poll(15));
    EXPECT_FALSE(driver.bind_scope(1, "file", "f", 14));
    EXPECT_FALSE(driver.bind_scope(1, "file", "f", 16));
}

TEST(OutputFocusDriver, FixedBindingsCannotOptIntoDeferredBinding) {
    Gate gate;
    OutputFocusDriver fixed({{"ai", "f", {"speaker"}, &gate}}, 0);
    ready(fixed, {&gate});
    EXPECT_FALSE(fixed.bind_scope(0, "new", "f", 13));
    Gate invalid;
    OutputFocusDriver mixed({{"ai", "f", {"speaker"}, &invalid}}, 0,
                            FocusBindingMode::DeferredOnce);
    EXPECT_FALSE(mixed.initialize(1));
    EXPECT_EQ(invalid.calls, 0U);
}

TEST(OutputFocusDriver, InitialIdleNeedsEveryOriginalPathAndCannotRepeat) {
    Gate ai, video;
    OutputFocusDriver driver(
        {{"ai", "f", {"speaker"}, &ai}, {"video", "f", {"speaker", "display"}, &video}}, 17);
    EXPECT_FALSE(driver.poll(1));
    ASSERT_TRUE(driver.initialize(10));
    ai.finish();
    EXPECT_TRUE(driver.poll(11));
    EXPECT_FALSE(driver.initial_idle());
    video.finish();
    EXPECT_TRUE(driver.poll(12));
    EXPECT_TRUE(driver.initial_idle());
    EXPECT_EQ(driver.revision(), 17U);
    EXPECT_FALSE(driver.initialize(13));
    EXPECT_EQ(ai.calls, 1U);
}
TEST(OutputFocusDriver, AcquireWaitsForActualFactAndKeepsOriginalScope) {
    Gate ai;
    OutputFocusDriver driver({{"ai", "f", {"speaker"}, &ai}}, 0);
    ready(driver, {&ai});
    const auto request = command("ai");
    ASSERT_TRUE(driver.begin(request, 7));
    EXPECT_FALSE(driver.initial_idle());
    EXPECT_TRUE(driver.poll(13));
    EXPECT_EQ(driver.revision(), 0U);
    EXPECT_FALSE(driver.take_fact("ai", "f"));
    EXPECT_FALSE(driver.owns(request));
    ai.finish();
    ASSERT_TRUE(driver.poll(14));
    EXPECT_FALSE(driver.take_fact("other", "f"));
    auto fact = driver.take_fact("ai", "f");
    ASSERT_TRUE(fact);
    EXPECT_TRUE(fact->success);
    EXPECT_EQ(fact->generation, 7U);
    EXPECT_EQ(fact->revision, 1U);
    EXPECT_EQ(fact->resources, (std::vector<std::string>{"speaker"}));
    EXPECT_TRUE(driver.owns(request));
    auto other = request;
    other.envelope.token = "other";
    EXPECT_FALSE(driver.owns(other));
    EXPECT_FALSE(driver.take_fact("ai", "f"));
    EXPECT_FALSE(driver.begin(request, 8));
}
TEST(OutputFocusDriver, OverlapNeedsExplicitActualSuspendAndRestoreCannotPreempt) {
    Gate ai, video;
    OutputFocusDriver driver(
        {{"ai", "f", {"speaker"}, &ai}, {"video", "f", {"speaker", "display"}, &video}}, 0);
    ready(driver, {&ai, &video});
    const auto ai_request = command("ai");
    EXPECT_TRUE(complete(driver, ai, ai_request).success);
    const auto video_request = command("video", "acquire", 1, "\"speaker\",\"display\"");
    EXPECT_FALSE(driver.begin(video_request, 1));
    EXPECT_EQ(video.calls, 1U);
    EXPECT_TRUE(
        complete(driver, ai, command("ai", "suspend", 1, "\"speaker\"", "suspend"), 2).success);
    const auto video_active = command("video", "acquire", 2, "\"speaker\",\"display\"");
    EXPECT_TRUE(complete(driver, video, video_active).success);
    EXPECT_TRUE(driver.owns(video_active));
    EXPECT_FALSE(driver.owns(ai_request));
    EXPECT_FALSE(driver.begin(command("ai", "restore", 3, "\"speaker\"", "restore"), 3));
    EXPECT_TRUE(complete(driver, video,
                         command("video", "release", 3, "\"display\",\"speaker\"", "release"), 2)
                    .success);
    EXPECT_TRUE(
        complete(driver, ai, command("ai", "restore", 4, "\"speaker\"", "restore"), 3).success);
    EXPECT_EQ(driver.revision(), 5U);
    EXPECT_FALSE(driver.owns(ai_request));
}
TEST(OutputFocusDriver, NonOverlappingOutputsShareGlobalRevisionWithoutStoppingEachOther) {
    Gate ai, screen;
    OutputFocusDriver driver({{"ai", "f", {"speaker"}, &ai}, {"screen", "f", {"display"}, &screen}},
                             0);
    ready(driver, {&ai, &screen});
    const auto speech = command("ai");
    const auto video = command("screen", "acquire", 1, "\"display\"");
    EXPECT_TRUE(complete(driver, ai, speech).success);
    EXPECT_TRUE(complete(driver, screen, video).success);
    EXPECT_EQ(driver.revision(), 2U);
    EXPECT_TRUE(driver.owns(speech));
    EXPECT_TRUE(driver.owns(video));
    EXPECT_EQ(ai.calls, 2U);
}
TEST(OutputFocusDriver, CancelledInFlightAcquireCannotBecomeSuccessOrReleaseReceipt) {
    Gate ai;
    OutputFocusDriver driver({{"ai", "f", {"speaker"}, &ai}}, 0);
    ready(driver, {&ai});
    const auto request = command("ai");
    ASSERT_TRUE(driver.begin(request, 1));
    EXPECT_FALSE(driver.stop_scope("ai", "f", {"display"}));
    EXPECT_FALSE(driver.stop_scope("ai", "f", {"speaker"}));
    ai.finish();  // 迟到true确认只能供原端口收敛，不能授予已经取消的租约。
    EXPECT_TRUE(driver.poll(14));
    EXPECT_FALSE(ai.enabled);
    EXPECT_FALSE(driver.owns(request));
    EXPECT_FALSE(driver.take_fact("ai", "f"));
    EXPECT_FALSE(driver.stop_scope("ai", "f", {"speaker"}));
    ai.finish();
    EXPECT_TRUE(driver.poll(15));
    EXPECT_TRUE(driver.stop_scope("ai", "f", {"speaker"}));
    EXPECT_EQ(driver.revision(), 0U);
    EXPECT_FALSE(driver.take_fact("ai", "f"));
}
TEST(OutputFocusDriver, TimeoutClosesOriginalOutputsAndNeverRefreshesInitialization) {
    Gate ai;
    OutputFocusDriver driver({{"ai", "f", {"speaker"}, &ai}}, 0);
    ready(driver, {&ai});
    ASSERT_TRUE(driver.begin(command("ai"), 1));
    EXPECT_FALSE(driver.poll(2012));
    auto fact = driver.take_fact("ai", "f");
    ASSERT_TRUE(fact);
    EXPECT_FALSE(fact->success);
    EXPECT_EQ(driver.revision(), 0U);
    EXPECT_FALSE(driver.initialize(2013));
    EXPECT_FALSE(driver.stop_scope("ai", "f", {"speaker"}));
    ai.finish();
    EXPECT_FALSE(driver.poll(2014));
    EXPECT_FALSE(ai.enabled);
    ai.finish();
    EXPECT_TRUE(driver.stop_scope("ai", "f", {"speaker"}));
    EXPECT_FALSE(driver.begin(command("ai"), 2));
}
TEST(OutputFocusDriver, RegressingClockAndExternalGateInvalidationFailClosed) {
    for (bool backwards : {false, true}) {
        Gate ai, screen;
        OutputFocusDriver driver(
            {{"ai", "f", {"speaker"}, &ai}, {"screen", "f", {"display"}, &screen}}, 0);
        ready(driver, {&ai, &screen});
        if (!backwards) {
            ASSERT_TRUE(
                screen.request_output_gate(true, 13));  // 不受本驱动控制的操作破坏旧idle证明。
        }
        EXPECT_FALSE(driver.poll(backwards ? 11 : 13));
        EXPECT_FALSE(driver.initial_idle());
        EXPECT_FALSE(driver.begin(command("ai"), 1));
        EXPECT_EQ(driver.revision(), 0U);
    }
}
TEST(OutputFocusDriver, ExternalOutputCannotBeIgnoredBeforeAdmissionOrDuringCompletion) {
    for (bool admitted : {false, true}) {
        Gate ai, screen;
        OutputFocusDriver driver(
            {{"ai", "f", {"speaker"}, &ai}, {"screen", "f", {"display"}, &screen}}, 0);
        ready(driver, {&ai, &screen});
        const auto request = command("ai");
        if (admitted) {
            ASSERT_TRUE(driver.begin(request, 1));
            ai.finish();
        }
        ASSERT_TRUE(screen.request_output_gate(true, 13));
        if (!admitted) {
            EXPECT_FALSE(driver.begin(request, 1));
        }
        EXPECT_FALSE(driver.poll(13));
        EXPECT_EQ(driver.revision(), 0U);
        EXPECT_FALSE(driver.owns(request));
        screen.finish();  // 收敛外部旧true后，驱动下一轮必须提交自己的false。
        EXPECT_FALSE(driver.poll(14));
        EXPECT_FALSE(screen.enabled);
        screen.finish();
        EXPECT_TRUE(driver.stop_scope("screen", "f", {"display"}));
    }
}

TEST(OutputFocusDriver, InvalidBindingsAndUnsafeRevisionNeverTouchPorts) {
    Gate gate;
    for (const auto& bindings : std::vector<std::vector<FocusOutputBinding>>{
             {},
             {{"", "f", {"speaker"}, &gate}},
             {{"ai", "f", {"microphone"}, &gate}},
             {{"ai", "f", {"speaker", "speaker"}, &gate}},
             {{"ai", "f", {"speaker"}, nullptr}},
             {{"ai", "f", {"speaker"}, &gate}, {"other", "f", {"display"}, &gate}}}) {
        OutputFocusDriver driver(bindings, 0);
        EXPECT_FALSE(driver.initialize(10));
        EXPECT_EQ(gate.calls, 0U);
    }
    OutputFocusDriver unsafe({{"ai", "f", {"speaker"}, &gate}}, 9007199254740992ULL);
    EXPECT_FALSE(unsafe.initialize(10));
    EXPECT_EQ(gate.calls, 0U);
}
TEST(OutputFocusDriver, StrictCommandAndBackpressurePreserveActualRevision) {
    Gate ai;
    OutputFocusDriver driver({{"ai", "f", {"speaker"}, &ai}}, 0);
    ready(driver, {&ai});
    auto request = command("ai");
    request.priority = "free-value";
    EXPECT_FALSE(driver.begin(request, 1));
    EXPECT_FALSE(driver.begin(command("missing"), 1));
    EXPECT_FALSE(driver.begin(command("ai", "acquire", 0, "\"display\""), 1));
    EXPECT_FALSE(driver.begin(command("ai"), 0));
    ai.refuse = true;
    EXPECT_FALSE(driver.begin(command("ai"), 1));
    EXPECT_EQ(driver.revision(), 0U);
    EXPECT_TRUE(driver.initial_idle());
    ai.refuse = false;
    ASSERT_TRUE(driver.begin(command("ai"), 1));
    ai.finish();
    ASSERT_TRUE(driver.poll(13));
    EXPECT_FALSE(driver.begin(command("ai", "suspend", 1), 2));  // 原事实尚未交付不覆盖。
    EXPECT_TRUE(driver.take_fact("ai", "f"));
    EXPECT_TRUE(driver.begin(command("ai", "suspend", 1), 2));
}

TEST(OutputFocusDriver, RealFocusEngineWaitsForSharedDriverAndRevocationStopsOnlyOriginalScope) {
    // Permit仍为显式夹具，不将设备收到命令或拥有输出接口当成平台许可。
    struct Permit final : FocusAuthority {
        bool allowed = true;  // 用例控制可信策略撤回，不提供生产默认授权。
        std::optional<FocusPermit> authorize(const FocusCommand& request, std::uint64_t,
                                             std::uint64_t) override {
            return allowed ? std::optional<FocusPermit>(FocusPermit{request, 900, 1})
                           : std::nullopt;
        }
        bool valid(const FocusPermit&, std::uint64_t, std::uint64_t) const override {
            return allowed;
        }
    } permit;
    Gate ai, screen;
    OutputFocusDriver driver({{"ai", "f", {"speaker"}, &ai}, {"screen", "f", {"display"}, &screen}},
                             0);
    ready(driver, {&ai, &screen});
    FocusEngine engine(driver, permit, "ai", "f", {"speaker"}, 1000);
    const auto request = command("ai");
    std::string wire;  // 真正编码与执行器解析，不能绕过原报文关联。
    ASSERT_EQ(encode_envelope(request.envelope, wire), DecodeResult::Ok);
    ASSERT_EQ(engine.submit(wire, 13, 1000).admission, FocusAdmission::Accepted);
    engine.poll(13, 1000);
    EXPECT_FALSE(engine.take_reply());
    ai.finish();
    ASSERT_TRUE(driver.poll(14));
    engine.poll(14, 1001);
    ASSERT_TRUE(engine.take_reply());
    EXPECT_TRUE(engine.output_permitted(14, 1001));
    const auto screen_calls = screen.calls;  // 撤权不能关闭其他合法路径或操作输入。
    permit.allowed = false;
    EXPECT_FALSE(engine.output_permitted(15, 1002));
    EXPECT_FALSE(engine.resources_released());
    ai.finish();
    ASSERT_TRUE(driver.poll(16));
    engine.poll(16, 1003);
    EXPECT_TRUE(engine.resources_released());
    EXPECT_EQ(screen.calls, screen_calls);
    EXPECT_EQ(driver.revision(), 1U);  // 被动停止不制造原release成功或推进全局水位。
}
