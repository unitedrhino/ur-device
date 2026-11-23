// 文件说明：完整输入输出组的可控HAL协同回归；模拟票据不代表实际浏览器或硬件验收。
#include "ur/device/duplex_focus_driver.hpp"
#include <gtest/gtest.h>
#include <map>

namespace ur::device {
namespace {
// Input模拟同一物理源的独立路由事实；默认不提供任何初始化或执行成功。
class Input final : public InputOwnershipGatePort {
public:
    std::optional<std::uint64_t> initial_idle_observation() const override { return boot; }
    std::optional<std::uint64_t> request_input_gate(const InputGateRequest& request,
                                                    std::uint64_t) override {
        requests.push_back(request);
        if (reject)
            return {};
        const auto ticket = ++next;
        pending[ticket] = request;
        return ticket;
    }
    bool input_gate_confirmed(std::uint64_t ticket,
                              const InputGateRequest& request) const override {
        if (trace)
            trace->push_back("input");
        const auto found = actual.find(ticket);
        return connected && found != actual.end() && found->second.session == request.session &&
               found->second.route_generation == request.route_generation &&
               found->second.enabled == request.enabled;
    }
    void confirm() { actual = pending; }
    std::optional<std::uint64_t> boot;  // 独立真实空闲探针的boot配对，缺省未知。
    std::vector<InputGateRequest> requests;
    std::map<std::uint64_t, InputGateRequest> pending, actual;
    std::uint64_t next = 0;
    bool reject = false, connected = true;
    std::vector<std::string>* trace = nullptr;  // 观察原票据读次序，不改变实际确认。
};
// Output只在明确确认原票据后返回事实，不把命令接纳当音频块已门控。
class Output final : public OutputGroupGatePort {
public:
    std::optional<std::uint64_t> request_output_gate(bool enabled, std::uint64_t) override {
        requests.push_back(enabled);
        if (reject)
            return {};
        pending[++next] = enabled;
        return next;
    }
    bool output_gate_confirmed(std::uint64_t ticket, bool enabled) const override {
        if (trace)
            trace->push_back("output");
        const auto found = actual.find(ticket);
        return connected && found != actual.end() && found->second == enabled;
    }
    void confirm() { actual = pending; }
    std::vector<bool> requests;
    std::map<std::uint64_t, bool> pending, actual;
    std::uint64_t next = 0;
    bool reject = false, connected = true;
    std::vector<std::string>* trace = nullptr;  // 与输入共享原调用记录，只供单测。
};
// DuplexFixture共用唯一输入端口和两个真实路由输出端口，固定时钟避免长等待。
class DuplexFixture : public ::testing::Test {
protected:
    DuplexFixture() : driver(input, {&ai, &human}, 7, 0) {}
    void initialize() {
        input.boot = 7;
        ASSERT_TRUE(driver.initialize(100));
        ai.confirm();
        human.confirm();
        ASSERT_TRUE(driver.poll(101));
        ASSERT_TRUE(driver.initial_idle_observation());
    }
    void bind() {
        ASSERT_TRUE(driver.bind_scope(0, "ai", "ai-focus", 10, 102));
        ASSERT_FALSE(driver.scope_ready(0));
        input.confirm();
        ai.confirm();
        ASSERT_TRUE(driver.poll(103));
        ASSERT_TRUE(driver.scope_ready(0));
        ASSERT_TRUE(driver.bind_scope(1, "human", "human-focus", 20, 104));
        input.confirm();
        human.confirm();
        ASSERT_TRUE(driver.poll(105));
        ASSERT_TRUE(driver.scope_ready(1));
    }
    // own_ai_only建立原AI实际完整组；候选仍没有绑定、租约或输入请求。
    void own_ai_only() {
        initialize();
        ASSERT_TRUE(driver.bind_scope(0, "ai", "ai-focus", 10, 102));
        input.confirm();
        ai.confirm();
        ASSERT_TRUE(driver.poll(103));
        initial_ai = command();
        complete(initial_ai);
        ASSERT_TRUE(driver.take_fact("ai", "ai-focus"));
        ASSERT_TRUE(driver.owns(initial_ai));
    }
    FocusCommand command(std::string operation = "acquire", bool local = false) const {
        const std::string session = local ? "human" : "ai";
        const std::string focus = local ? "human-focus" : "ai-focus";
        const auto json =
            "{\"msgToken\":\"control\",\"method\":\"focusControl\",\"sessionId\":"
            "\"" +
            session + "\",\"eventSeq\":1,\"params\":{\"operation\":\"" + operation +
            "\",\"expectedRevision\":" + std::to_string(driver.revision()) + ",\"focusId\":\"" +
            focus +
            "\",\"resources\":[\"microphone\",\"speaker\"],\"priority\":"
            "\"conversation\","
            "\"resumePolicy\":\"explicit\"" +
            ((operation == "acquire" || operation == "restore") ? ",\"expiresAt\":9000" : "") +
            "}}";
        FocusCommand value;  // 原公共解析器验证契约，夹具不猜信封成员。
        EXPECT_EQ(decode_focus_control(WireRoute::Signaling, json, value), WireResult::Ok);
        return value;
    }
    void complete(const FocusCommand& value, std::uint64_t generation = 1) {
        ASSERT_TRUE(driver.begin(value, generation));
        input.confirm();
        ai.confirm();
        human.confirm();
        ASSERT_TRUE(driver.poll(++now));
    }
    // stopped_restore 核两条原路径同一规则：资源完整恢复，只有本人输入停止意图不可撤销。
    void stopped_restore(bool human_path) {
        initialize();
        bind();
        const auto acquire = command("acquire", human_path);
        const auto& session = acquire.envelope.session;
        const auto& focus = acquire.focus_id;
        complete(acquire);
        ASSERT_TRUE(driver.take_fact(session, focus));
        ASSERT_TRUE(driver.stop_input(session, focus, ++now));
        input.confirm();
        ASSERT_TRUE(driver.poll(++now));
        complete(command("suspend", human_path), 2);
        ASSERT_TRUE(driver.take_fact(session, focus));
        const auto restored = command("restore", human_path);
        complete(restored, 3);
        EXPECT_FALSE(input.requests.back().enabled);
        EXPECT_TRUE((human_path ? human : ai).requests.back());
        const auto fact = driver.take_fact(session, focus);
        ASSERT_TRUE(fact);
        EXPECT_EQ(fact->state, "restored");
        EXPECT_TRUE(driver.owns(restored));
        EXPECT_TRUE(driver.input_stopped(session, focus));
        EXPECT_FALSE(driver.begin(command("acquire", human_path), 4));
        complete(command("release", human_path), 4);
        ASSERT_TRUE(driver.take_fact(session, focus));
        EXPECT_FALSE(input.requests.back().enabled);
        EXPECT_FALSE((human_path ? human : ai).requests.back());
    }
    Input input;
    Output ai, human;
    DuplexFocusDriver driver;
    FocusCommand initial_ai;  // 保存实际取得时的原CAS，不用后继水位重新生成历史操作。
    std::uint64_t now = 106;
};

// 驱动本身不区分来源身份；两原path只接受可信本人stop，而非平台恢复替本人新意图。
TEST_F(DuplexFixture, OriginalCallerPathStopLatchSurvivesRestoreAndFormalRelease) {
    stopped_restore(false);
}
TEST_F(DuplexFixture, OriginalReceiverPathStopLatchSurvivesRestoreAndFormalRelease) {
    stopped_restore(true);
}

// 本人仅停输入保持原输出及租约，真实false齐后仍无新Focus事实或CAS推进。
TEST_F(DuplexFixture, UserInputStopKeepsOutputAndOriginalLeaseWithoutRevisionChange) {
    own_ai_only();
    const auto output_requests = ai.requests.size();
    const auto revision = driver.revision();
    ASSERT_TRUE(driver.stop_input("ai", "ai-focus", 110));
    EXPECT_FALSE(input.requests.back().enabled);
    EXPECT_EQ(ai.requests.size(), output_requests);
    EXPECT_FALSE(driver.owns(initial_ai));
    EXPECT_FALSE(driver.input_stopped("ai", "ai-focus"));
    ASSERT_TRUE(driver.poll(111));
    input.confirm();
    ASSERT_TRUE(driver.poll(112));
    EXPECT_TRUE(driver.input_stopped("ai", "ai-focus"));
    EXPECT_TRUE(driver.owns(initial_ai));
    EXPECT_FALSE(driver.quiescent_scope_ready(0));  // 输入停了但输出仍播，不是可重绑的idle。
    EXPECT_EQ(driver.revision(), revision);
    EXPECT_FALSE(driver.take_fact("ai", "ai-focus"));
    ASSERT_TRUE(driver.stop_input("ai", "ai-focus", 113));
    EXPECT_EQ(ai.requests.size(), output_requests);
    now = 113;
    complete(command("release"), 2);
    const auto released = driver.take_fact("ai", "ai-focus");
    ASSERT_TRUE(released);
    EXPECT_EQ(released->state, "released");
    EXPECT_EQ(driver.revision(), revision + 1);
    EXPECT_FALSE(ai.requests.back());
}

// 只停输入的八秒旧组预算不能代替固定两秒本人停止预算，重投或迟到不续。
TEST_F(DuplexFixture, UserInputStopTimeoutAndWrongScopeNeverGrantActualStop) {
    own_ai_only();
    const auto requests = input.requests.size();
    EXPECT_FALSE(driver.stop_input("other", "ai-focus", 110));
    EXPECT_EQ(input.requests.size(), requests);
    ASSERT_TRUE(driver.stop_input("ai", "ai-focus", 110));
    ASSERT_TRUE(driver.stop_input("ai", "ai-focus", 2000));
    EXPECT_EQ(input.requests.size(), requests + 1);
    EXPECT_TRUE(driver.poll(2109));
    EXPECT_FALSE(driver.poll(2110));
    input.confirm();
    ai.confirm();
    human.confirm();
    EXPECT_FALSE(driver.poll(2111));
    EXPECT_FALSE(driver.input_stopped("ai", "ai-focus"));
    EXPECT_FALSE(driver.owns(initial_ai));
    EXPECT_EQ(driver.revision(), 1U);
}

// HAL明确未接受停止时保持旧完整实际票据；失败不能先撤扬声器或冒认输入关闭。
TEST_F(DuplexFixture, RejectedUserInputStopPreservesOriginalWholeLease) {
    own_ai_only();
    const auto output_requests = ai.requests.size();
    input.reject = true;
    EXPECT_FALSE(driver.stop_input("ai", "ai-focus", 110));
    EXPECT_TRUE(driver.owns(initial_ai));
    EXPECT_EQ(ai.requests.size(), output_requests);
    EXPECT_FALSE(driver.input_stopped("ai", "ai-focus"));
    EXPECT_EQ(driver.revision(), 1U);
}

TEST_F(DuplexFixture, DefaultUnknownProbeCannotSupplyBaseline) {
    DuplexFocusDriver swapped(input, {&ai, &human}, 0, 7);  // 交换原非零boot与零revision必须拒绝。
    EXPECT_FALSE(swapped.initialize(100));
    EXPECT_TRUE(input.requests.empty());
    EXPECT_TRUE(ai.pending.empty());
    EXPECT_TRUE(human.pending.empty());
    ASSERT_TRUE(driver.initialize(100));
    ai.confirm();
    human.confirm();
    EXPECT_TRUE(driver.poll(101));
    EXPECT_FALSE(driver.initial_idle_observation());
    EXPECT_TRUE(input.requests.empty());
    EXPECT_FALSE(driver.bind_scope(0, "ai", "ai-focus", 10, 102));
}
// 原owns本来会核当前票据两次；开启观察不能去重、追加或改变短路序列。
TEST_F(DuplexFixture, ScopeDiagnosticKeepsActualTicketCallsAndFirstRejectedPort) {
    own_ai_only();
    std::vector<std::string> calls;
    input.trace = ai.trace = human.trace = &calls;
    ASSERT_TRUE(driver.owns(initial_ai));
    const auto original = calls;
    EXPECT_EQ(original, (std::vector<std::string>{"output", "input", "output", "input"}));
    calls.clear();
    ScopeFailure observed;
    ASSERT_TRUE(driver.owns_observed(initial_ai, &observed));
    EXPECT_EQ(calls, original);
    EXPECT_EQ(observed.code, ScopeFailureCode::None);
    input.connected = false;
    calls.clear();
    EXPECT_FALSE(driver.owns(initial_ai));
    const auto refused = calls;
    calls.clear();
    EXPECT_FALSE(driver.owns_observed(initial_ai, &observed));
    EXPECT_EQ(calls, refused);
    EXPECT_EQ(calls, (std::vector<std::string>{"output", "input"}));
    EXPECT_EQ(observed.code, ScopeFailureCode::DriverInput);
    calls.clear();
    ScopeFailure next;
    ai.connected = false;
    EXPECT_FALSE(driver.owns_observed(initial_ai, &next));
    EXPECT_EQ(calls, (std::vector<std::string>{"output"}));
    EXPECT_EQ(next.code, ScopeFailureCode::DriverOutput);
}
// 控制恢复不是重新初始化驱动：正式release、原事实消费和两端false必须全部存在。
TEST_F(DuplexFixture, ReleasedFocusReplacementPreservesGlobalRevisionAndUsesNewTickets) {
    ASSERT_NO_FATAL_FAILURE(own_ai_only());
    complete(command("release"), 2);
    ASSERT_TRUE(driver.take_fact("ai", "ai-focus"));
    const auto revision = driver.revision();
    const auto ticket = input.next;
    EXPECT_TRUE(driver.replace_released_focus(0, "ai", "ai-focus", "ai-focus-new", ++now));
    EXPECT_EQ(driver.revision(), revision);
    EXPECT_GT(input.next, ticket);
    EXPECT_FALSE(driver.scope_ready(0));
    input.confirm();
    ai.confirm();
    EXPECT_TRUE(driver.poll(++now));
    EXPECT_TRUE(driver.scope_ready(0));
    EXPECT_FALSE(driver.initial_idle_observation());
    EXPECT_FALSE(driver.owns(initial_ai));
    EXPECT_FALSE(driver.begin(initial_ai, 3));
}
TEST_F(DuplexFixture, ReplacementRejectsPassiveFalseAndUnconsumedReleaseFact) {
    ASSERT_NO_FATAL_FAILURE(own_ai_only());
    driver.stop_scope("ai", "ai-focus", {"microphone", "speaker"});
    input.confirm();
    ai.confirm();
    ASSERT_TRUE(driver.poll(++now));
    EXPECT_FALSE(driver.replace_released_focus(0, "ai", "ai-focus", "ai-focus-new", ++now));
    complete(command("release"), 2);
    EXPECT_FALSE(driver.replace_released_focus(0, "ai", "ai-focus", "ai-focus-new", ++now));
}
TEST_F(DuplexFixture, ReplacementRejectsWrongOriginalScopeSameFocusAndMissingActualFalse) {
    ASSERT_NO_FATAL_FAILURE(own_ai_only());
    complete(command("release"), 2);
    ASSERT_TRUE(driver.take_fact("ai", "ai-focus"));
    EXPECT_FALSE(driver.replace_released_focus(0, "alien", "ai-focus", "new", ++now));
    EXPECT_FALSE(driver.replace_released_focus(0, "ai", "alien", "new", ++now));
    EXPECT_FALSE(driver.replace_released_focus(0, "ai", "ai-focus", "ai-focus", ++now));
    ai.actual.clear();
    EXPECT_FALSE(driver.replace_released_focus(0, "ai", "ai-focus", "new", ++now));
}
TEST_F(DuplexFixture, WrongBootAndSpeakerOnlyCannotSupplyBaseline) {
    input.boot = 8;
    ASSERT_TRUE(driver.initialize(100));
    ai.confirm();
    human.confirm();
    EXPECT_TRUE(driver.poll(101));
    EXPECT_FALSE(driver.initial_idle_observation());
    EXPECT_TRUE(driver.poll(2100));
    EXPECT_TRUE(driver.poll(8099));
    EXPECT_FALSE(driver.poll(8100));
    EXPECT_EQ(driver.revision(), 0U);
}
TEST_F(DuplexFixture, ProbeAndOriginalSpeakerFactsSupplyIdleNotPermission) {
    initialize();
    EXPECT_EQ(driver.initial_idle_observation()->resources,
              (std::vector<std::string>{"microphone", "speaker"}));
    EXPECT_FALSE(driver.begin(command(), 1));
    EXPECT_TRUE(input.requests.empty());
}
TEST_F(DuplexFixture, BindingRequiresNewOriginalSessionTickets) {
    initialize();
    ASSERT_TRUE(driver.bind_scope(0, "ai", "ai-focus", 10, 102));
    ai.confirm();
    EXPECT_TRUE(driver.poll(103));
    EXPECT_FALSE(driver.scope_ready(0));
    EXPECT_FALSE(driver.begin(command(), 1));
    ASSERT_EQ(input.requests.size(), 1U);
    EXPECT_EQ(input.requests[0].session, "ai");
    EXPECT_EQ(input.requests[0].route_generation, 10U);
    EXPECT_FALSE(input.requests[0].enabled);
    input.confirm();
    EXPECT_TRUE(driver.poll(104));
    EXPECT_TRUE(driver.scope_ready(0));
    EXPECT_FALSE(driver.initial_idle_observation());
}
TEST_F(DuplexFixture, RetiredUnallocatedBootObservationDoesNotCloseOwnedAi) {
    initialize();
    ASSERT_TRUE(driver.bind_scope(0, "ai", "ai-focus", 10, 102));
    input.confirm();
    ai.confirm();
    ASSERT_TRUE(driver.poll(103));
    const auto acquired = command();
    complete(acquired);
    ASSERT_TRUE(driver.take_fact("ai", "ai-focus"));
    // 实际候选媒体绑定新scope时撤销旧boot观察，但尚未申请任何输入或开启输出。
    human.actual.clear();
    EXPECT_TRUE(driver.poll(108));
    EXPECT_TRUE(driver.owns(acquired));
    EXPECT_FALSE(driver.initial_idle_observation());
    EXPECT_FALSE(driver.begin(command("acquire", true), 1));
}
TEST_F(DuplexFixture, NewCandidateBindingReplacesOnlyItsUnallocatedBootTicket) {
    initialize();
    ASSERT_TRUE(driver.bind_scope(0, "ai", "ai-focus", 10, 102));
    input.confirm();
    ai.confirm();
    ASSERT_TRUE(driver.poll(103));
    const auto acquired = command();
    complete(acquired);
    ASSERT_TRUE(driver.take_fact("ai", "ai-focus"));
    human.actual.clear();
    const auto previous = human.next;
    ASSERT_TRUE(driver.bind_scope(1, "human", "human-focus", 20, 108));
    EXPECT_GT(human.next, previous);
    EXPECT_FALSE(driver.scope_ready(1));
    EXPECT_TRUE(driver.owns(acquired));
    human.confirm();
    EXPECT_TRUE(driver.poll(109));
    EXPECT_FALSE(driver.scope_ready(1));
    EXPECT_EQ(driver.revision(), 1U);
    input.confirm();
    ASSERT_TRUE(driver.poll(110));
    EXPECT_TRUE(driver.scope_ready(1));
    EXPECT_TRUE(driver.owns(acquired));
    EXPECT_EQ(driver.revision(), 1U);
    EXPECT_FALSE(driver.begin(command("acquire", true), 1));
}
TEST_F(DuplexFixture, UnallocatedExceptionCannotForgeTheFirstFullIdle) {
    initialize();
    human.actual.clear();
    EXPECT_FALSE(driver.initial_idle_observation());
    EXPECT_FALSE(driver.bind_scope(0, "ai", "ai-focus", 10, 102));
    EXPECT_TRUE(input.requests.empty());
}
TEST_F(DuplexFixture, CandidateBindingStillRequiresActualCurrentAiFacts) {
    initialize();
    ASSERT_TRUE(driver.bind_scope(0, "ai", "ai-focus", 10, 102));
    input.confirm();
    ai.confirm();
    ASSERT_TRUE(driver.poll(103));
    const auto acquired = command();
    complete(acquired);
    ASSERT_TRUE(driver.take_fact("ai", "ai-focus"));
    human.actual.clear();
    ai.actual.clear();
    const auto requests = input.requests.size();
    EXPECT_FALSE(driver.bind_scope(1, "human", "human-focus", 20, 108));
    EXPECT_EQ(input.requests.size(), requests);
    EXPECT_FALSE(driver.owns(acquired));
}
TEST_F(DuplexFixture, FirstHumanOutputRefreshPreservesOriginalBootUntilActualFalse) {
    initialize();
    human.actual.clear();
    EXPECT_FALSE(driver.initial_idle_observation());
    const auto inputs = input.requests.size();
    ASSERT_TRUE(driver.refresh_idle_output(1, 102));
    EXPECT_FALSE(driver.idle_output_ready(1));
    EXPECT_FALSE(driver.initial_idle_observation());
    EXPECT_EQ(input.requests.size(), inputs);
    EXPECT_EQ(driver.revision(), 0U);
    EXPECT_FALSE(driver.bind_scope(1, "human", "human-focus", 20, 103));
    human.confirm();
    ASSERT_TRUE(driver.poll(104));
    EXPECT_TRUE(driver.idle_output_ready(1));
    EXPECT_TRUE(driver.initial_idle_observation());
    ASSERT_TRUE(driver.bind_scope(1, "human", "human-focus", 20, 105));
    EXPECT_FALSE(driver.scope_ready(1));
    input.confirm();
    human.confirm();
    ASSERT_TRUE(driver.poll(106));
    EXPECT_TRUE(driver.scope_ready(1));
    EXPECT_EQ(driver.revision(), 0U);
    EXPECT_FALSE(driver.owns(command("acquire", true)));
}
TEST_F(DuplexFixture, FirstHumanOutputRefreshRequiresCurrentOriginalInputIdle) {
    initialize();
    human.actual.clear();
    input.boot.reset();
    EXPECT_FALSE(driver.refresh_idle_output(1, 102));
    input.boot = 8;
    EXPECT_FALSE(driver.refresh_idle_output(1, 102));
    input.boot = 7;
    ai.actual.clear();
    EXPECT_FALSE(driver.refresh_idle_output(1, 102));
    EXPECT_TRUE(input.requests.empty());
    EXPECT_EQ(human.requests.size(), 1U);
    EXPECT_EQ(driver.revision(), 0U);
}
TEST_F(DuplexFixture, FirstHumanRefreshCannotCreateTheMissingInitialBootProof) {
    input.boot = 7;
    ASSERT_TRUE(driver.initialize(100));
    ai.confirm();
    ASSERT_TRUE(driver.poll(101));
    EXPECT_FALSE(driver.refresh_idle_output(1, 102));
    EXPECT_FALSE(driver.initial_idle_observation());
    EXPECT_TRUE(input.requests.empty());
    EXPECT_EQ(human.requests.size(), 1U);
}
TEST_F(DuplexFixture, FirstHumanRefreshCannotReuseRevokedBootForInitialBinding) {
    initialize();
    human.actual.clear();
    ASSERT_TRUE(driver.refresh_idle_output(1, 102));
    human.confirm();
    input.boot.reset();
    ASSERT_TRUE(driver.poll(103));
    EXPECT_FALSE(driver.idle_output_ready(1));
    EXPECT_FALSE(driver.initial_idle_observation());
    EXPECT_FALSE(driver.bind_scope(1, "human", "human-focus", 20, 104));
    EXPECT_TRUE(input.requests.empty());
    EXPECT_EQ(driver.revision(), 0U);
}
TEST_F(DuplexFixture, CandidateOutputRefreshWaitsForActualFalseWithoutNewInput) {
    own_ai_only();
    const auto acquired = initial_ai;
    human.actual.clear();
    const auto input_count = input.requests.size();
    const auto old_ticket = human.next;
    ASSERT_TRUE(driver.refresh_idle_output(1, 108));
    EXPECT_GT(human.next, old_ticket);
    EXPECT_FALSE(human.requests.back());
    EXPECT_EQ(input.requests.size(), input_count);
    EXPECT_FALSE(driver.idle_output_ready(1));
    EXPECT_TRUE(driver.owns(acquired));
    EXPECT_FALSE(driver.begin(command("suspend"), 2));
    human.confirm();
    ASSERT_TRUE(driver.poll(109));
    EXPECT_TRUE(driver.idle_output_ready(1));
    EXPECT_FALSE(driver.scope_ready(1));
    EXPECT_FALSE(driver.initial_idle_observation());
    EXPECT_EQ(driver.revision(), 1U);
    ASSERT_TRUE(driver.begin(command("suspend"), 2));
    input.confirm();
    ai.confirm();
    ASSERT_TRUE(driver.poll(110));
    ASSERT_TRUE(driver.take_fact("ai", "ai-focus"));
    ASSERT_TRUE(driver.bind_scope(1, "human", "human-focus", 20, 111));
    EXPECT_FALSE(driver.scope_ready(1));  // 正式绑定仍必须另取新的两端false，不借候选输出观察。
}
TEST_F(DuplexFixture, CandidateOutputRefreshCannotReplaceBaselineOrAllocatedScope) {
    initialize();
    EXPECT_FALSE(driver.refresh_idle_output(1, 102));
    EXPECT_FALSE(driver.idle_output_ready(1));
    ASSERT_TRUE(driver.bind_scope(0, "ai", "ai-focus", 10, 103));
    input.confirm();
    ai.confirm();
    ASSERT_TRUE(driver.poll(104));
    const auto output_count = ai.requests.size();
    EXPECT_FALSE(driver.refresh_idle_output(0, 105));
    EXPECT_FALSE(driver.refresh_idle_output(2, 105));
    EXPECT_EQ(ai.requests.size(), output_count);
}
TEST_F(DuplexFixture, CandidateOutputRefreshRejectsPendingOrRepeatedRequests) {
    own_ai_only();
    const auto acquired = initial_ai;
    human.actual.clear();
    ASSERT_TRUE(driver.refresh_idle_output(1, 108));
    const auto output_count = human.requests.size();
    EXPECT_FALSE(driver.refresh_idle_output(1, 109));
    EXPECT_EQ(human.requests.size(), output_count);
    EXPECT_TRUE(driver.owns(acquired));
    human.confirm();
    ASSERT_TRUE(driver.poll(110));
    ASSERT_TRUE(driver.begin(command("suspend"), 2));
    EXPECT_FALSE(driver.refresh_idle_output(1, 111));
}
TEST_F(DuplexFixture, MissingRefreshedOutputExpiresLocallyAndKeepsOwnedAi) {
    own_ai_only();
    const auto acquired = initial_ai;
    human.actual.clear();
    ASSERT_TRUE(driver.refresh_idle_output(1, 108));
    ASSERT_TRUE(driver.poll(2108));
    EXPECT_FALSE(driver.idle_output_ready(1));
    EXPECT_TRUE(driver.owns(acquired));
    EXPECT_EQ(driver.revision(), 1U);
    EXPECT_FALSE(driver.begin(command("suspend"), 2));
    EXPECT_FALSE(driver.bind_scope(1, "human", "human-focus", 20, 2109));
}
TEST_F(DuplexFixture, RefreshedOutputBackpressureOrMissingAiFactCannotInventIdle) {
    own_ai_only();
    const auto acquired = initial_ai;
    human.actual.clear();
    ai.actual.clear();
    const auto output_count = human.requests.size();
    EXPECT_FALSE(driver.refresh_idle_output(1, 108));
    EXPECT_EQ(human.requests.size(), output_count);
    ai.confirm();
    human.reject = true;
    EXPECT_FALSE(driver.refresh_idle_output(1, 109));
    EXPECT_FALSE(driver.idle_output_ready(1));
    EXPECT_TRUE(driver.owns(acquired));
    EXPECT_FALSE(driver.initial_idle_observation());
}
TEST_F(DuplexFixture, MissingMicrophoneFactNeverAdvancesOrOwns) {
    initialize();
    bind();
    const auto value = command();
    ASSERT_TRUE(driver.begin(value, 1));
    ai.confirm();
    EXPECT_TRUE(driver.poll(106));
    EXPECT_EQ(driver.revision(), 0U);
    EXPECT_FALSE(driver.owns(value));
    EXPECT_FALSE(driver.take_fact("ai", "ai-focus"));
    input.confirm();
    ASSERT_TRUE(driver.poll(107));
    auto fact = driver.take_fact("ai", "ai-focus");
    ASSERT_TRUE(fact);
    EXPECT_TRUE(fact->success);
    EXPECT_EQ(fact->resources, value.resources);
    EXPECT_EQ(fact->generation, 1U);
    EXPECT_EQ(driver.revision(), 1U);
    EXPECT_TRUE(driver.owns(value));
}
TEST_F(DuplexFixture, MissingSpeakerFactNeverAdvances) {
    initialize();
    bind();
    ASSERT_TRUE(driver.begin(command(), 1));
    input.confirm();
    EXPECT_TRUE(driver.poll(106));
    EXPECT_EQ(driver.revision(), 0U);
    EXPECT_FALSE(driver.take_fact("ai", "ai-focus"));
}
TEST_F(DuplexFixture, StaleRouteOrSessionConfirmationCannotSatisfyCurrentTicket) {
    initialize();
    bind();
    ASSERT_TRUE(driver.begin(command(), 1));
    input.confirm();
    ai.confirm();
    input.actual[input.next].session = "old";
    EXPECT_TRUE(driver.poll(106));
    EXPECT_EQ(driver.revision(), 0U);
    input.actual[input.next].session = "ai";
    input.actual[input.next].route_generation = 9;
    EXPECT_TRUE(driver.poll(107));
    EXPECT_EQ(driver.revision(), 0U);
}
TEST_F(DuplexFixture, InputBackpressureGuaranteesNoOtherSideEffect) {
    initialize();
    bind();
    const auto output_count = ai.requests.size();
    input.reject = true;
    EXPECT_FALSE(driver.begin(command(), 1));
    EXPECT_EQ(ai.requests.size(), output_count);
    EXPECT_EQ(driver.revision(), 0U);
}
TEST_F(DuplexFixture, PartialSubmissionIsUnknownNotUntouchedRejection) {
    initialize();
    bind();
    ai.reject = true;
    EXPECT_TRUE(driver.begin(command(), 1));
    const auto fact = driver.take_fact("ai", "ai-focus");
    ASSERT_TRUE(fact);
    EXPECT_FALSE(fact->success);
    EXPECT_EQ(driver.revision(), 0U);
    EXPECT_FALSE(driver.owns(command()));
    EXPECT_FALSE(driver.begin(command(), 2));
}
TEST_F(DuplexFixture, TimeoutAndLateHalfFactsNeverReviveDriver) {
    initialize();
    bind();
    ASSERT_TRUE(driver.begin(command(), 1));
    ai.confirm();
    EXPECT_TRUE(driver.poll(2105));
    EXPECT_TRUE(driver.poll(8099));
    EXPECT_FALSE(driver.poll(8105));
    auto fact = driver.take_fact("ai", "ai-focus");
    ASSERT_TRUE(fact);
    EXPECT_FALSE(fact->success);
    input.confirm();
    ai.confirm();
    human.confirm();
    // 超时已使驱动进入终态Failed；迟到半票据确认不得复活驱动，任何时点poll均为false。
    EXPECT_FALSE(driver.poll(2106));
    EXPECT_FALSE(driver.poll(8099));
    EXPECT_FALSE(driver.poll(8106));
    EXPECT_EQ(driver.revision(), 0U);
}
TEST_F(DuplexFixture, OriginalSuspendThenNewAcquireUsesSingleCAS) {
    initialize();
    bind();
    auto acquired = command();
    complete(acquired);
    ASSERT_TRUE(driver.take_fact("ai", "ai-focus"));
    EXPECT_FALSE(driver.begin(command("acquire", true), 1));
    auto suspended = command("suspend");
    suspended.envelope.token = "suspend";
    complete(suspended, 2);
    ASSERT_TRUE(driver.take_fact("ai", "ai-focus"));
    auto local = command("acquire", true);
    local.envelope.token = "local";
    complete(local, 1);
    auto fact = driver.take_fact("human", "human-focus");
    ASSERT_TRUE(fact);
    EXPECT_EQ(fact->revision, 3U);
    EXPECT_TRUE(driver.owns(local));
    EXPECT_FALSE(driver.owns(acquired));
    EXPECT_FALSE(driver.begin(command("restore"), 3));
}
TEST_F(DuplexFixture, UnsafeCASPartialGroupAndRebindAreRejectedWithoutHALCalls) {
    initialize();
    bind();
    const auto count = input.requests.size();
    auto value = command();
    value.expected_revision = 1;
    EXPECT_FALSE(driver.begin(value, 1));
    value = command();
    value.resources = {"speaker"};
    EXPECT_FALSE(driver.begin(value, 1));
    EXPECT_FALSE(driver.bind_scope(0, "other", "other-focus", 30, 106));
    EXPECT_EQ(input.requests.size(), count);
}
TEST_F(DuplexFixture, DisconnectOrClockRollbackInvalidatesWholeOwnership) {
    initialize();
    bind();
    const auto value = command();
    complete(value);
    ASSERT_TRUE(driver.take_fact("ai", "ai-focus"));
    input.connected = false;
    EXPECT_FALSE(driver.owns(value));
    EXPECT_FALSE(driver.poll(107));
    EXPECT_EQ(driver.revision(), 1U);
}
TEST_F(DuplexFixture, StopRequiresBothOriginalDisabledFactsAndDoesNotForgeRelease) {
    initialize();
    bind();
    const auto value = command();
    complete(value);
    ASSERT_TRUE(driver.take_fact("ai", "ai-focus"));
    EXPECT_FALSE(driver.stop_scope("ai", "ai-focus", value.resources));
    input.confirm();
    EXPECT_FALSE(driver.stop_scope("ai", "ai-focus", value.resources));
    ai.confirm();
    EXPECT_TRUE(driver.stop_scope("ai", "ai-focus", value.resources));
    EXPECT_EQ(driver.revision(), 1U);
    EXPECT_FALSE(driver.take_fact("ai", "ai-focus"));
    EXPECT_FALSE(driver.owns(value));
}
// 候选只提交自身false，不应占用整组或中断实际仍合法的原AI。
TEST_F(DuplexFixture, CandidateBindingPendingAndTimeoutPreserveActualAiLease) {
    initialize();
    ASSERT_TRUE(driver.bind_scope(0, "ai", "ai-focus", 10, 102));
    input.confirm();
    ai.confirm();
    ASSERT_TRUE(driver.poll(103));
    const auto value = command();
    complete(value);
    ASSERT_TRUE(driver.take_fact("ai", "ai-focus"));
    ASSERT_TRUE(driver.bind_scope(1, "human", "human-focus", 20, 110));
    EXPECT_TRUE(driver.owns(value));
    EXPECT_TRUE(driver.poll(111));
    EXPECT_TRUE(driver.owns(value));
    EXPECT_TRUE(driver.poll(2110));
    EXPECT_TRUE(driver.owns(value));
    EXPECT_FALSE(driver.scope_ready(1));
    EXPECT_EQ(driver.revision(), 1U);
    EXPECT_TRUE(ai.requests.back());
    EXPECT_FALSE(driver.begin(command("acquire", true), 2));
}
// 平台提交后旧AI仅退役自身已暂停路由，不能撤销新人际原完整组。
TEST_F(DuplexFixture, RetiringSuspendedAiPreservesNewHumanOwnership) {
    initialize();
    bind();
    complete(command());
    ASSERT_TRUE(driver.take_fact("ai", "ai-focus"));
    complete(command("suspend"), 2);
    ASSERT_TRUE(driver.take_fact("ai", "ai-focus"));
    const auto local = command("acquire", true);
    complete(local);
    ASSERT_TRUE(driver.take_fact("human", "human-focus"));
    EXPECT_TRUE(driver.stop_scope("ai", "ai-focus", local.resources));
    EXPECT_TRUE(driver.owns(local));
    EXPECT_TRUE(driver.poll(++now));
    EXPECT_TRUE(driver.owns(local));
    EXPECT_EQ(driver.revision(), 3U);
}
TEST_F(DuplexFixture, LostPreSessionProbeCannotBindFirstSessionUsingOldBaseline) {
    initialize();
    input.boot.reset();
    const auto count = input.requests.size();
    EXPECT_FALSE(driver.bind_scope(0, "ai", "ai-focus", 10, 102));
    EXPECT_EQ(input.requests.size(), count);
}
TEST_F(DuplexFixture, CandidateBindingBackpressureKeepsAiAndNeverReusesScope) {
    initialize();
    ASSERT_TRUE(driver.bind_scope(0, "ai", "ai-focus", 10, 102));
    input.confirm();
    ai.confirm();
    ASSERT_TRUE(driver.poll(103));
    const auto value = command();
    complete(value);
    ASSERT_TRUE(driver.take_fact("ai", "ai-focus"));
    input.reject = true;
    EXPECT_FALSE(driver.bind_scope(1, "human", "human-focus", 20, 110));
    EXPECT_TRUE(driver.poll(111));
    EXPECT_TRUE(driver.owns(value));
    EXPECT_FALSE(driver.bind_scope(1, "replacement", "other", 30, 112));
    EXPECT_EQ(driver.revision(), 1U);
}
TEST_F(DuplexFixture, DuplicateGenerationsOrSessionsNeverTouchHal) {
    initialize();
    ASSERT_TRUE(driver.bind_scope(0, "ai", "ai-focus", 10, 102));
    input.confirm();
    ai.confirm();
    ASSERT_TRUE(driver.poll(103));
    const auto count = input.requests.size();
    EXPECT_FALSE(driver.bind_scope(1, "human", "human-focus", 10, 104));
    EXPECT_FALSE(driver.bind_scope(1, "ai", "other", 20, 104));
    EXPECT_FALSE(driver.bind_scope(1, "", "human-focus", 20, 104));
    EXPECT_FALSE(driver.bind_scope(1, "human", "human-focus", 0, 104));
    EXPECT_FALSE(driver.bind_scope(2, "human", "human-focus", 20, 104));
    EXPECT_EQ(input.requests.size(), count);
}
TEST_F(DuplexFixture, WrongEnabledFactAndOldTicketsCannotComplete) {
    initialize();
    bind();
    ASSERT_TRUE(driver.begin(command(), 1));
    ai.confirm();
    input.confirm();
    input.actual[input.next].enabled = false;
    EXPECT_TRUE(driver.poll(106));
    EXPECT_FALSE(driver.take_fact("ai", "ai-focus"));
    input.actual.erase(input.next);
    EXPECT_TRUE(driver.poll(107));
    EXPECT_EQ(driver.revision(), 0U);
}
TEST_F(DuplexFixture, RestoreAndReleaseRequireOriginalWholeGroupAndSingleRevision) {
    initialize();
    bind();
    complete(command());
    ASSERT_TRUE(driver.take_fact("ai", "ai-focus"));
    EXPECT_FALSE(driver.begin(command("restore"), 2));
    complete(command("suspend"), 2);
    ASSERT_TRUE(driver.take_fact("ai", "ai-focus"));
    const auto restored = command("restore");
    complete(restored, 3);
    ASSERT_TRUE(driver.take_fact("ai", "ai-focus"));
    EXPECT_TRUE(driver.owns(restored));
    complete(command("release"), 4);
    const auto released = driver.take_fact("ai", "ai-focus");
    ASSERT_TRUE(released);
    EXPECT_EQ(released->state, "released");
    EXPECT_EQ(released->revision, 4U);
    EXPECT_FALSE(driver.owns(restored));
    EXPECT_FALSE(driver.begin(command(), 5));
}
TEST_F(DuplexFixture, PendingAndUnconsumedFactCannotBeOverwrittenOrMisrouted) {
    initialize();
    bind();
    const auto value = command();
    ASSERT_TRUE(driver.begin(value, 1));
    EXPECT_FALSE(driver.begin(value, 2));
    EXPECT_FALSE(driver.take_fact("human", "human-focus"));
    input.confirm();
    ai.confirm();
    ASSERT_TRUE(driver.poll(106));
    EXPECT_FALSE(driver.begin(command("suspend"), 2));
    ASSERT_TRUE(driver.take_fact("ai", "ai-focus"));
    EXPECT_FALSE(driver.take_fact("ai", "ai-focus"));
    EXPECT_TRUE(driver.owns(value));
    auto other = value;
    other.envelope.token = "not-original";
    EXPECT_FALSE(driver.owns(other));
    other = value;
    other.expected_revision = 10;
    EXPECT_FALSE(driver.owns(other));
}
TEST_F(DuplexFixture, ClockRollbackNeverReinitializesOrResetsCas) {
    initialize();
    bind();
    complete(command());
    ASSERT_TRUE(driver.take_fact("ai", "ai-focus"));
    EXPECT_FALSE(driver.poll(1));
    EXPECT_EQ(driver.revision(), 1U);
    EXPECT_FALSE(driver.initialize(1000));
}
TEST_F(DuplexFixture, ReusedInputTicketIsUnknownEvenIfPortClaimsActualSuccess) {
    initialize();
    bind();
    input.next = 0;
    EXPECT_TRUE(driver.begin(command(), 1));
    const auto fact = driver.take_fact("ai", "ai-focus");
    ASSERT_TRUE(fact);
    EXPECT_FALSE(fact->success);
    EXPECT_EQ(driver.revision(), 0U);
    input.confirm();
    ai.confirm();
    EXPECT_FALSE(driver.poll(106));
}
TEST_F(DuplexFixture, StoppedReadinessWaitsForBothActualFalseWithoutNewEffect) {
    own_ai_only();
    EXPECT_FALSE(driver.stopped_scope_ready(0));
    EXPECT_FALSE(driver.stop_scope("ai", "ai-focus", initial_ai.resources));
    EXPECT_FALSE(driver.stopped_scope_ready(0));
    input.confirm();
    EXPECT_FALSE(driver.stopped_scope_ready(0));
    ai.confirm();
    ASSERT_TRUE(driver.poll(++now));
    const auto inputs = input.requests.size();
    const auto outputs = ai.requests.size();
    EXPECT_TRUE(driver.stopped_scope_ready(0));
    EXPECT_FALSE(driver.scope_ready(0));
    EXPECT_FALSE(driver.owns(initial_ai));
    EXPECT_EQ(input.requests.size(), inputs);
    EXPECT_EQ(ai.requests.size(), outputs);
    EXPECT_EQ(driver.revision(), 1U);
    EXPECT_FALSE(driver.take_fact("ai", "ai-focus"));
}
TEST_F(DuplexFixture, StoppedReadinessRejectsUnallocatedAndNeverAcquiredScope) {
    initialize();
    EXPECT_FALSE(driver.stopped_scope_ready(0));
    EXPECT_FALSE(driver.stopped_scope_ready(99));
    bind();
    EXPECT_FALSE(driver.stopped_scope_ready(1));
    ASSERT_TRUE(driver.stop_scope("human", "human-focus", {"microphone", "speaker"}));
    EXPECT_FALSE(driver.stopped_scope_ready(1));
    EXPECT_EQ(driver.revision(), 0U);
}
TEST_F(DuplexFixture, StoppedReadinessCannotReplaceFormalReleaseOrItsNewTickets) {
    own_ai_only();
    EXPECT_FALSE(driver.stop_scope("ai", "ai-focus", initial_ai.resources));
    input.confirm();
    ai.confirm();
    ASSERT_TRUE(driver.poll(++now));
    ASSERT_TRUE(driver.stopped_scope_ready(0));
    ASSERT_TRUE(driver.begin(command("release"), 2));
    EXPECT_FALSE(driver.stopped_scope_ready(0));
    input.confirm();
    EXPECT_FALSE(driver.stopped_scope_ready(0));
    ai.confirm();
    ASSERT_TRUE(driver.poll(++now));
    EXPECT_FALSE(driver.stopped_scope_ready(0));
    const auto fact = driver.take_fact("ai", "ai-focus");
    ASSERT_TRUE(fact);
    EXPECT_TRUE(fact->success);
    EXPECT_EQ(fact->state, "released");
    EXPECT_FALSE(driver.stopped_scope_ready(0));
    EXPECT_EQ(driver.revision(), 2U);
    EXPECT_FALSE(driver.begin(command("restore"), 3));
}
TEST_F(DuplexFixture, StoppedReadinessRejectsLostFactsAndInvalidDriver) {
    own_ai_only();
    EXPECT_FALSE(driver.stop_scope("ai", "ai-focus", initial_ai.resources));
    input.confirm();
    ai.confirm();
    ASSERT_TRUE(driver.poll(++now));
    ASSERT_TRUE(driver.stopped_scope_ready(0));
    input.connected = false;
    EXPECT_FALSE(driver.stopped_scope_ready(0));
    input.connected = true;
    ai.connected = false;
    EXPECT_FALSE(driver.stopped_scope_ready(0));
    ai.connected = true;
    EXPECT_FALSE(driver.poll(now - 1));
    EXPECT_FALSE(driver.stopped_scope_ready(0));
    EXPECT_EQ(driver.revision(), 1U);
}
TEST_F(DuplexFixture, StoppedReadinessWaitsForCancelledOriginalFactConsumption) {
    own_ai_only();
    ASSERT_TRUE(driver.begin(command("suspend"), 2));
    EXPECT_FALSE(driver.stop_scope("ai", "ai-focus", initial_ai.resources));
    input.confirm();
    ai.confirm();
    ASSERT_TRUE(driver.poll(++now));
    EXPECT_FALSE(driver.stopped_scope_ready(0));
    const auto fact = driver.take_fact("ai", "ai-focus");
    ASSERT_TRUE(fact);
    EXPECT_FALSE(fact->success);
    EXPECT_TRUE(driver.stopped_scope_ready(0));
    EXPECT_EQ(driver.revision(), 1U);
}
TEST_F(DuplexFixture, StoppedReadinessCannotBypassOtherPathPendingOperation) {
    initialize();
    bind();
    complete(command());
    ASSERT_TRUE(driver.take_fact("ai", "ai-focus"));
    complete(command("suspend"));
    ASSERT_TRUE(driver.take_fact("ai", "ai-focus"));
    const auto human_command = command("acquire", true);
    ASSERT_TRUE(driver.begin(human_command, 3));
    ASSERT_TRUE(driver.stop_scope("ai", "ai-focus", {"microphone", "speaker"}));
    EXPECT_FALSE(driver.stopped_scope_ready(0));
    EXPECT_EQ(driver.revision(), 2U);
    input.confirm();
    human.confirm();
    ASSERT_TRUE(driver.poll(++now));
    ASSERT_TRUE(driver.take_fact("human", "human-focus"));
    EXPECT_TRUE(driver.stopped_scope_ready(0));
    EXPECT_TRUE(driver.owns(human_command));
    EXPECT_EQ(driver.revision(), 3U);
}
TEST_F(DuplexFixture, InvalidConstructionNeverRequestsPorts) {
    DuplexFocusDriver empty(input, {}, 7, 0);
    DuplexFocusDriver duplicate(input, {&ai, &ai}, 7, 0);
    DuplexFocusDriver missing(input, {nullptr}, 7, 0);
    DuplexFocusDriver invalid_boot(input, {&ai}, 0, 0);
    EXPECT_FALSE(empty.initialize(100));
    EXPECT_FALSE(duplicate.initialize(100));
    EXPECT_FALSE(missing.initialize(100));
    EXPECT_FALSE(invalid_boot.initialize(100));
    EXPECT_TRUE(input.requests.empty());
    EXPECT_TRUE(ai.requests.empty());
}
// 原AI退役已确认false不等于正式release；只有原完整组新操作实际确认后才推进一次CAS。
TEST_F(DuplexFixture, StoppedOriginalAiMayOnlyReleaseWithoutStoppingCurrentHuman) {
    initialize();
    bind();
    complete(command());
    ASSERT_TRUE(driver.take_fact("ai", "ai-focus"));
    complete(command("suspend"));
    ASSERT_TRUE(driver.take_fact("ai", "ai-focus"));
    const auto human_command = command("acquire", true);
    complete(human_command);
    ASSERT_TRUE(driver.take_fact("human", "human-focus"));
    ASSERT_TRUE(driver.owns(human_command));
    ASSERT_TRUE(driver.stop_scope("ai", "ai-focus", {"microphone", "speaker"}));
    EXPECT_FALSE(driver.begin(command("restore"), 4));
    const auto release = command("release");
    ASSERT_TRUE(driver.begin(release, 4));
    EXPECT_EQ(driver.revision(), 3U);
    EXPECT_FALSE(driver.take_fact("ai", "ai-focus"));
    input.confirm();
    ai.confirm();
    ASSERT_TRUE(driver.poll(++now));
    const auto fact = driver.take_fact("ai", "ai-focus");
    ASSERT_TRUE(fact);
    EXPECT_TRUE(fact->success);
    EXPECT_EQ(fact->state, "released");
    EXPECT_EQ(fact->revision, 4U);
    EXPECT_TRUE(driver.owns(human_command));
    EXPECT_FALSE(driver.begin(command(), 5));
    EXPECT_FALSE(driver.begin(command("restore"), 5));
    EXPECT_FALSE(driver.begin(command("release"), 5));
}
// 旧腿正式release及原fact消费后才允许另一当前腿后继保持；围栏/半票据不够。
TEST_F(DuplexFixture, ConsumedFormalReleaseAllowsCurrentHumanHoldAndResumeOnly) {
    initialize();
    bind();
    complete(command());
    ASSERT_TRUE(driver.take_fact("ai", "ai-focus"));
    complete(command("suspend"));
    ASSERT_TRUE(driver.take_fact("ai", "ai-focus"));
    const auto human_command = command("acquire", true);
    complete(human_command);
    ASSERT_TRUE(driver.take_fact("human", "human-focus"));
    ASSERT_TRUE(driver.stop_scope("ai", "ai-focus", {"microphone", "speaker"}));
    EXPECT_FALSE(driver.begin(command("suspend", true), 4));
    ASSERT_TRUE(driver.begin(command("release"), 4));
    input.confirm();
    ASSERT_TRUE(driver.poll(++now));
    EXPECT_FALSE(driver.begin(command("suspend", true), 5));
    ai.confirm();
    ASSERT_TRUE(driver.poll(++now));
    EXPECT_FALSE(driver.begin(command("suspend", true), 5));
    const auto release_fact = driver.take_fact("ai", "ai-focus");
    ASSERT_TRUE(release_fact);
    ASSERT_EQ(release_fact->state, "released");
    ASSERT_EQ(release_fact->revision, 4U);
    ASSERT_TRUE(driver.begin(command("suspend", true), 5));
    input.confirm();
    human.confirm();
    ASSERT_TRUE(driver.poll(++now));
    const auto held_fact = driver.take_fact("human", "human-focus");
    ASSERT_TRUE(held_fact);
    ASSERT_EQ(held_fact->state, "suspended");
    const auto restore_human = command("restore", true);
    complete(restore_human, 6);
    ASSERT_TRUE(driver.take_fact("human", "human-focus"));
    EXPECT_TRUE(driver.owns(restore_human));
    EXPECT_FALSE(driver.begin(command(), 7));
    EXPECT_FALSE(driver.begin(command("restore"), 7));
    input.connected = false;
    EXPECT_FALSE(driver.begin(command("suspend", true), 7));
}
// 被动围栏缺少任何原false事实时，不能用release请求补造已释放状态。
TEST_F(DuplexFixture, StoppedCleanupRequiresOriginalFalseBeforeIndependentReleaseTickets) {
    initialize();
    bind();
    complete(command());
    ASSERT_TRUE(driver.take_fact("ai", "ai-focus"));
    EXPECT_FALSE(driver.stop_scope("ai", "ai-focus", {"microphone", "speaker"}));
    EXPECT_FALSE(driver.begin(command("release"), 2));
    input.confirm();
    EXPECT_FALSE(driver.begin(command("release"), 2));
    ai.confirm();
    ASSERT_TRUE(driver.poll(++now));
    ASSERT_TRUE(driver.begin(command("release"), 2));
    EXPECT_FALSE(driver.take_fact("ai", "ai-focus"));
    input.confirm();
    EXPECT_TRUE(driver.poll(++now));
    EXPECT_FALSE(driver.take_fact("ai", "ai-focus"));
    ai.confirm();
    ASSERT_TRUE(driver.poll(++now));
    ASSERT_TRUE(driver.take_fact("ai", "ai-focus"));
    EXPECT_EQ(driver.revision(), 2U);
}
// 正式关闭仍用原两秒截止，不能因stopping分支绕过等待或自动续期。
TEST_F(DuplexFixture, StoppedCleanupMissingNewFactExpiresWithoutCasAdvance) {
    initialize();
    bind();
    complete(command());
    ASSERT_TRUE(driver.take_fact("ai", "ai-focus"));
    EXPECT_FALSE(driver.stop_scope("ai", "ai-focus", {"microphone", "speaker"}));
    input.confirm();
    ai.confirm();
    ASSERT_TRUE(driver.poll(++now));
    ASSERT_TRUE(driver.begin(command("release"), 2));
    input.confirm();
    // 门控预算八秒（真实部署覆盖MQTT往返与媒体协商），迟到不续期的语义不变。
    EXPECT_TRUE(driver.poll(now + 7999));
    EXPECT_FALSE(driver.poll(now + 8000));
    const auto fact = driver.take_fact("ai", "ai-focus");
    ASSERT_TRUE(fact);
    EXPECT_FALSE(fact->success);
    EXPECT_EQ(driver.revision(), 1U);
    EXPECT_FALSE(driver.begin(command("release"), 3));
}
TEST_F(DuplexFixture, FailureDiagnosticDefaultOffAndLateEnableDoNotInventPastReason) {
    initialize();
    human.connected = false;
    EXPECT_FALSE(driver.poll(102));
    EXPECT_FALSE(driver.failure_diagnostic());
    driver.enable_failure_diagnostics();
    EXPECT_FALSE(driver.poll(103));
    EXPECT_FALSE(driver.failure_diagnostic());
}
TEST_F(DuplexFixture, FailureDiagnosticLocksOutputBeforeCleanupAndReadsNoExtraPorts) {
    driver.enable_failure_diagnostics();
    initialize();
    human.connected = false;
    EXPECT_FALSE(driver.poll(102));
    const auto observed = driver.failure_diagnostic();
    ASSERT_TRUE(observed);
    EXPECT_EQ(observed->reason, DuplexDriverFailureReason::OutputUnconfirmed);
    EXPECT_EQ(observed->path, 1);
    std::vector<std::string> trace;
    input.trace = ai.trace = human.trace = &trace;
    EXPECT_FALSE(driver.poll(103));
    const auto reads = trace.size();
    EXPECT_EQ(driver.failure_diagnostic()->reason, observed->reason);
    EXPECT_EQ(driver.failure_diagnostic()->path, observed->path);
    EXPECT_EQ(trace.size(), reads);
    input.trace = ai.trace = human.trace = nullptr;
}
TEST_F(DuplexFixture, FailureDiagnosticDistinguishesOriginalInputFromOutput) {
    driver.enable_failure_diagnostics();
    initialize();
    bind();
    input.connected = false;
    EXPECT_FALSE(driver.poll(106));
    ASSERT_TRUE(driver.failure_diagnostic());
    EXPECT_EQ(driver.failure_diagnostic()->reason, DuplexDriverFailureReason::InputUnconfirmed);
    EXPECT_EQ(driver.failure_diagnostic()->path, 0);
}
TEST_F(DuplexFixture, WaitingTicketsDoNotLatchFailureUntilOriginalDeadline) {
    driver.enable_failure_diagnostics();
    initialize();
    bind();
    ASSERT_TRUE(driver.begin(command("acquire"), 1));
    EXPECT_TRUE(driver.poll(106));
    EXPECT_FALSE(driver.failure_diagnostic());
    EXPECT_FALSE(driver.poll(8105));
    ASSERT_TRUE(driver.failure_diagnostic());
    EXPECT_EQ(driver.failure_diagnostic()->reason, DuplexDriverFailureReason::PendingDeadline);
    EXPECT_EQ(driver.failure_diagnostic()->path, 0);
}
TEST_F(DuplexFixture, ReusedInputTicketKeepsOriginalReasonAfterCleanupPoll) {
    driver.enable_failure_diagnostics();
    initialize();
    bind();
    input.next = 0;
    EXPECT_TRUE(driver.begin(command(), 1));
    ASSERT_TRUE(driver.failure_diagnostic());
    EXPECT_EQ(driver.failure_diagnostic()->reason, DuplexDriverFailureReason::InputTicketOrder);
    EXPECT_EQ(driver.failure_diagnostic()->path, 0);
    EXPECT_FALSE(driver.poll(106));
    EXPECT_EQ(driver.failure_diagnostic()->reason, DuplexDriverFailureReason::InputTicketOrder);
}
TEST_F(DuplexFixture, ClockRollbackDiagnosticDoesNotTurnCleanupIntoAnotherFirstFailure) {
    driver.enable_failure_diagnostics();
    initialize();
    EXPECT_FALSE(driver.poll(1));
    ASSERT_TRUE(driver.failure_diagnostic());
    EXPECT_EQ(driver.failure_diagnostic()->reason, DuplexDriverFailureReason::ClockRollback);
    EXPECT_EQ(driver.failure_diagnostic()->path, -1);
    EXPECT_FALSE(driver.poll(102));
    EXPECT_EQ(driver.failure_diagnostic()->reason, DuplexDriverFailureReason::ClockRollback);
}
// 相同HAL故障下开关只能多留固定事实，原端口读取的顺序和次数必须完全一致。
TEST(DuplexDiagnostic, EnabledFailureObservationKeepsOriginalPortReadSequence) {
    const auto exercise = [](bool enabled) {
        Input input;
        Output output;
        std::vector<std::string> trace;
        DuplexFocusDriver driver(input, {&output}, 7, 0);
        if (enabled) driver.enable_failure_diagnostics();
        input.boot = 7;
        EXPECT_TRUE(driver.initialize(100));
        output.confirm();
        EXPECT_TRUE(driver.poll(101));
        input.trace = output.trace = &trace;
        output.connected = false;
        EXPECT_FALSE(driver.poll(102));
        const auto before = trace;
        (void)driver.failure_diagnostic();
        EXPECT_EQ(trace, before);
        input.trace = output.trace = nullptr;
        return trace;
    };
    EXPECT_EQ(exercise(false), exercise(true));
}
}  // namespace
}  // namespace ur::device
