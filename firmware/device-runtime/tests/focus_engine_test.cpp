// 文件说明：固定焦点执行器的状态/全组/许可/时间/未知释放回归；驱动与授权均为明确单测夹具。
#include "ur/device/focus_engine.hpp"
#include <cJSON.h>
#include <gtest/gtest.h>
#include <array>
#include <cstdlib>
#include <deque>
#include <functional>

using namespace ur::device;
namespace {
// command 生成完整原下行，仅用于离线用例，不持设备凭据或授予平台许可。
std::string command(const char* operation = "acquire", unsigned revision = 0, unsigned seq = 1,
                    const char* token = "one", const char* resume = "explicit",
                    const char* priority = "conversation", unsigned expires = 2000) {
    return std::string(R"({"msgToken":")") + token +
           R"(","method":"focusControl","sessionId":"child","eventSeq":)" + std::to_string(seq) +
           R"(,"params":{"operation":")" + operation + R"(","expectedRevision":)" +
           std::to_string(revision) +
           R"(,"focusId":"focus","resources":["speaker","display"],"priority":")" + priority +
           R"(","resumePolicy":")" + resume + "\"" +
           ((std::string(operation) == "acquire" || std::string(operation) == "restore")
                ? std::string(R"(,"expiresAt":)") + std::to_string(expires)
                : "") +
           "}}";
}
// Authority 是显式许可夹具，要求原截止保持不变以验证核心不能篡改授权。
class Authority final : public FocusAuthority {
public:
    unsigned calls = 0;                        // 观察重放不重复授权。
    bool allow = true, validity = true;        // 明确成功/撤权分支，非生产默认许可。
    std::uint64_t limit = 1000, intent = 7;    // 固定独立许可与原用户意图版本。
    std::function<void(FocusPermit&)> change;  // 只用于构造跨作用域等恶意许可。
    mutable std::vector<std::string>* trace = nullptr;  // 原检查顺序旁路，不参与许可判断。
    std::optional<FocusPermit> authorize(const FocusCommand& request, std::uint64_t,
                                         std::uint64_t) override {
        ++calls;
        if (!allow)
            return {};
        FocusPermit value{request, limit, intent};
        if (change)
            change(value);
        return value;
    }
    bool valid(const FocusPermit& value, std::uint64_t now, std::uint64_t) const override {
        if (trace)
            trace->push_back("authority");
        return validity && value.deadline == limit && now < value.deadline;
    }
};
// Driver 只在complete时推进实际全局状态；begin受理和关闭请求均不制造成功事实。
class Driver final : public FocusDriver {
public:
    unsigned starts = 0, stops = 0;             // 观察原动作是否只执行一次且只关闭原租约。
    std::uint64_t current = 0, generation = 0;  // 明确实际全局revision及原驱动代次。
    bool accept = true, held = false,
         release_known = false;   // 配置受理、实际占用和真实关闭三个独立事实。
    FocusCommand last, owner;     // 原操作与最后实际拥有租约的操作不能互相替代。
    std::deque<FocusFact> facts;  // 明确受控有界输入，不进行真实音频或屏幕操作。
    mutable std::vector<std::string>* trace = nullptr;  // 与authority共用原求值序列。
    std::uint64_t revision() const override { return current; }
    bool begin(const FocusCommand& request, std::uint64_t value) override {
        ++starts;
        if (!accept)
            return false;
        last = request;
        generation = value;
        return true;
    }
    std::optional<FocusFact> take_fact(std::string_view session, std::string_view focus) override {
        EXPECT_EQ(session, "child");
        EXPECT_EQ(focus, "focus");
        if (facts.empty())
            return {};
        auto result = std::move(facts.front());
        facts.pop_front();
        return result;
    }
    bool owns(const FocusCommand& applied) const override {
        if (trace)
            trace->push_back("driver");
        return held && applied.envelope.token == owner.envelope.token &&
               applied.envelope.session == owner.envelope.session &&
               applied.focus_id == owner.focus_id && applied.resources == owner.resources;
    }
    bool stop_scope(std::string_view session, std::string_view focus,
                    const std::vector<std::string>& resources) override {
        ++stops;
        EXPECT_EQ(session, "child");
        EXPECT_EQ(focus, "focus");
        EXPECT_EQ(resources, (std::vector<std::string>{"speaker", "display"}));
        if (release_known)
            held = false;
        return release_known;
    }
    // complete 只为离线夹具产生实际完成，用例可变异以验证核心不信任部分或迟到回调。
    void complete() {
        current = last.expected_revision + 1;
        held = last.operation == "acquire" || last.operation == "restore";
        if (held)
            owner = last;
        const auto state = last.operation == "acquire"   ? "acquired"
                           : last.operation == "suspend" ? "suspended"
                           : last.operation == "restore" ? "restored"
                                                         : "released";
        facts.push_back({generation, true, state, last.resources, current});
    }
};
// fact 读取真实执行器原回执，不只检查“不崩溃”或请求已接纳。
FocusState fact(FocusEngine& engine) {
    const auto value = engine.take_reply();
    EXPECT_TRUE(value);
    FocusState result;
    if (value) {
        EXPECT_EQ(decode_focus_state(WireRoute::Signaling, *value, result), WireResult::Ok);
    }
    return result;
}
// establish 通过真实核心两次轮询，夹具驱动在两次间明确提供实际整组完成。
void establish(FocusEngine& engine, Driver& driver) {
    ASSERT_EQ(engine.submit(command(), 10, 1000).admission, FocusAdmission::Accepted);
    engine.poll(10, 1000);
    driver.complete();
    engine.poll(11, 1001);
    EXPECT_EQ(fact(engine).state, "acquired");
}
}  // namespace

// 缺信令水位在解码边界拒绝，不消耗原序号、调用授权或驱动；不冒作私有故障注入。
TEST(FocusEngine, MissingSequenceDoesNotAdmitOrConsumeValidSequence) {
    Driver driver;
    Authority auth;
    FocusEngine engine(driver, auth, "child", "focus", {"speaker", "display"}, 500);
    auto missing = command();
    const auto position = missing.find(",\"eventSeq\":1");
    ASSERT_NE(position, std::string::npos);
    missing.erase(position, std::string(",\"eventSeq\":1").size());
    EXPECT_EQ(engine.submit(missing, 10, 1000).admission, FocusAdmission::Invalid);
    EXPECT_EQ(auth.calls, 0U);
    EXPECT_EQ(driver.starts, 0U);
    EXPECT_EQ(engine.submit(command(), 10, 1000).admission, FocusAdmission::Accepted);
}

// 原poll已经撤销真实核心租约，但旧诊断只知道Closed；新观察必须保留原撤权检查点。
TEST(FocusScopeDiagnostic, PollRetainsActualLeaseAuthorityRejectionBeforeClose) {
    Driver driver;
    Authority auth;
    FocusEngine engine(driver, auth, "child", "focus", {"speaker", "display"}, 500);
    engine.enable_scope_diagnostics();
    establish(engine, driver);
    auth.validity = false;
    engine.poll(12, 1002);
    ASSERT_TRUE(engine.closed());
    EXPECT_FALSE(engine.scoped_permitted({"speaker", "display"}, 12, 1002));
    ASSERT_TRUE(engine.scope_failure());
    EXPECT_EQ(*engine.scope_failure(), ScopeFailureCode::AuthorityDenied);
    ScopeFailure observed;
    EXPECT_FALSE(engine.scoped_permitted({"speaker", "display"}, 12, 1002, &observed));
    EXPECT_EQ(observed.code, ScopeFailureCode::AuthorityDenied);
    engine.close();
    EXPECT_EQ(*engine.scope_failure(), ScopeFailureCode::AuthorityDenied);
}
// 默认关闭和失败后开启都不补造历史；原资源关闭事实仍必须发生。
TEST(FocusScopeDiagnostic, DisabledAndLateEnableCannotInventPollHistory) {
    Driver driver;
    Authority auth;
    FocusEngine engine(driver, auth, "child", "focus", {"speaker", "display"}, 500);
    establish(engine, driver);
    auth.validity = false;
    engine.poll(12, 1002);
    EXPECT_TRUE(engine.closed());
    EXPECT_GT(driver.stops, 0U);
    EXPECT_FALSE(engine.scope_failure());
    engine.enable_scope_diagnostics();
    engine.poll(13, 1003);
    EXPECT_FALSE(engine.scope_failure());
}
// 开关不改变原authority/driver调用次数或顺序；原轮询关闭之后不再额外读取许可。
TEST(FocusScopeDiagnostic, ObservationPreservesOriginalPredicateOrderAndCount) {
    const auto run = [](bool enabled) {
        Driver driver;
        Authority auth;
        std::vector<std::string> trace;
        FocusEngine engine(driver, auth, "child", "focus", {"speaker", "display"}, 500);
        if (enabled)
            engine.enable_scope_diagnostics();
        EXPECT_EQ(engine.submit(command(), 10, 1000).admission, FocusAdmission::Accepted);
        engine.poll(10, 1000);
        driver.complete();
        engine.poll(11, 1001);
        const auto reply = engine.take_reply();
        EXPECT_TRUE(reply);
        auth.trace = &trace;
        driver.trace = &trace;
        ScopeFailure observation;
        EXPECT_FALSE(engine.scoped_permitted({"speaker", "display"}, 12, 1002,
                                             enabled ? &observation : nullptr));
        // 原回执尚未本地接纳，不能为诊断绕过发布条件读authority。
        EXPECT_TRUE(trace.empty());
        if (reply)
            engine.delivered_reply(*reply);
        EXPECT_TRUE(engine.scoped_permitted({"speaker", "display"}, 12, 1002,
                                            enabled ? &observation : nullptr));
        EXPECT_EQ(trace, (std::vector<std::string>{"authority", "driver"}));
        auth.validity = false;
        engine.poll(12, 1002);
        EXPECT_EQ(trace, (std::vector<std::string>{"authority", "driver", "authority"}));
        engine.close();
        EXPECT_EQ(trace.size(), 3U);
        return trace;
    };
    EXPECT_EQ(run(false), run(true));
}
// 在途操作等待本来就关闭输出；尚未超时不能锁存撤权，实际超时关闭原lease才记录。
TEST(FocusScopeDiagnostic, PendingSelectionFalseIsNotLeaseLossButTimeoutClosesLease) {
    Driver driver;
    Authority auth;
    FocusEngine engine(driver, auth, "child", "focus", {"speaker", "display"}, 500, 15);
    engine.enable_scope_diagnostics();
    establish(engine, driver);
    ASSERT_EQ(engine.submit(command("suspend", 1, 2, "pause"), 12, 1002).admission,
              FocusAdmission::Accepted);
    engine.poll(12, 1002);
    EXPECT_FALSE(engine.scoped_permitted({"speaker", "display"}, 13, 1003));
    EXPECT_FALSE(engine.closed());
    EXPECT_FALSE(engine.scope_failure());
    engine.poll(27, 1017);
    EXPECT_TRUE(engine.closed());
    ASSERT_TRUE(engine.scope_failure());
    EXPECT_EQ(*engine.scope_failure(), ScopeFailureCode::EngineBusy);
}
// 原wall截止按原poll现场保存，不把随后closed结果覆盖成通用终态。
TEST(FocusScopeDiagnostic, LeaseDeadlineRemainsFirstCauseAfterCleanup) {
    Driver driver;
    Authority auth;
    FocusEngine engine(driver, auth, "child", "focus", {"speaker", "display"}, 500);
    engine.enable_scope_diagnostics();
    establish(engine, driver);
    engine.poll(12, 2000);
    EXPECT_TRUE(engine.closed());
    engine.close();
    EXPECT_EQ(engine.scope_failure(), ScopeFailureCode::EngineDeadline);
}

TEST(FocusEngine, EntireLifecycleRequiresActualGroupAndKeepsInputIndependent) {
    Driver driver;
    Authority auth;
    FocusEngine engine(driver, auth, "child", "focus", {"speaker", "display"}, 500);
    const std::array<std::pair<const char*, const char*>, 4> operations = {
        {{"acquire", "acquired"},
         {"suspend", "suspended"},
         {"restore", "restored"},
         {"release", "released"}}};
    for (unsigned index = 0; index < operations.size(); ++index) {
        const auto token = "token-" + std::to_string(index);
        EXPECT_EQ(engine
                      .submit(command(operations[index].first, index, index + 1, token.c_str()),
                              10 + index * 3, 1000 + index * 3)
                      .admission,
                  FocusAdmission::Accepted);
        EXPECT_EQ(driver.starts, index);
        EXPECT_FALSE(engine.take_reply());
        engine.poll(10 + index * 3, 1000 + index * 3);
        EXPECT_EQ(driver.starts, index + 1);
        EXPECT_FALSE(engine.take_reply());
        driver.complete();
        engine.poll(11 + index * 3, 1001 + index * 3);
        const auto result = fact(engine);
        EXPECT_EQ(result.state, operations[index].second);
        EXPECT_EQ(result.revision, index + 1);
        EXPECT_EQ(result.resources, (std::vector<std::string>{"speaker", "display"}));
        EXPECT_EQ(engine.output_permitted(11 + index * 3, 1001 + index * 3),
                  index == 0 || index == 2);
    }
    EXPECT_TRUE(engine.closed());
    EXPECT_TRUE(engine.released());
    EXPECT_EQ(driver.stops, 0U);
}

TEST(FocusEngine, DuplicateAndConflictNeverRenewOrExecuteAgain) {
    Driver driver;
    Authority auth;
    FocusEngine engine(driver, auth, "child", "focus", {"speaker", "display"}, 1000);
    const auto input = command();
    EXPECT_EQ(engine.submit(input, 10, 1000).admission, FocusAdmission::Accepted);
    EXPECT_EQ(engine.submit(input, 11, 1001).admission, FocusAdmission::Pending);
    EXPECT_EQ(auth.calls, 1U);
    EXPECT_EQ(engine.submit(command("acquire", 0, 2), 11, 1001).admission,
              FocusAdmission::Conflict);
    engine.poll(12, 1002);
    driver.complete();
    engine.poll(13, 1003);
    const auto original = engine.take_reply();
    ASSERT_TRUE(original);
    const auto replay = engine.submit(input, 14, 1004);
    EXPECT_EQ(replay.admission, FocusAdmission::Replay);
    EXPECT_EQ(replay.reply, *original);
    EXPECT_EQ(auth.calls, 1U);
    EXPECT_EQ(driver.starts, 1U);
}

TEST(FocusEngine, AllPermitScopeMutationsAreRejectedBeforeDriver) {
    const std::vector<std::function<void(FocusPermit&)>> mutations = {
        [](auto& p) { p.scope.envelope.session = "other"; },
        [](auto& p) { p.scope.envelope.token = "other"; },
        [](auto& p) { p.scope.focus_id = "other"; },
        [](auto& p) { p.scope.resources = {"speaker"}; },
        [](auto& p) { p.scope.operation = "restore"; },
        [](auto& p) { p.scope.expected_revision = 1; },
        [](auto& p) { p.scope.priority = "emergency"; },
        [](auto& p) { p.scope.resume_policy = "never"; },
        [](auto& p) { p.scope.expires_at = 4000; },
        [](auto& p) { p.deadline = 10; }};
    for (const auto& change : mutations) {
        Driver driver;
        Authority auth;
        auth.change = change;
        FocusEngine engine(driver, auth, "child", "focus", {"speaker", "display"}, 1000);
        EXPECT_EQ(engine.submit(command(), 10, 1000).admission, FocusAdmission::Accepted);
        const auto result = fact(engine);
        EXPECT_EQ(result.state, "denied");
        EXPECT_EQ(result.reason, "permissionDenied");
        EXPECT_EQ(driver.starts, 0U);
    }
}

TEST(FocusEngine, ActualPartialFutureWrongStateAndOwnershipCloseOnlyOriginal) {
    const std::vector<std::function<void(Driver&)>> mutations = {
        [](auto& d) { d.facts.back().resources = {"speaker"}; },
        [](auto& d) { ++d.facts.back().generation; },
        [](auto& d) { d.facts.back().state = "restored"; },
        [](auto& d) { d.facts.back().revision = 0; },
        [](auto& d) { d.facts.back().success = false; },
        [](auto& d) { d.held = false; },
        [](auto& d) { d.owner.envelope.token = "other-operation"; }};
    for (const auto& change : mutations) {
        Driver driver;
        Authority auth;
        FocusEngine engine(driver, auth, "child", "focus", {"speaker", "display"}, 1000);
        engine.submit(command(), 10, 1000);
        engine.poll(10, 1000);
        driver.complete();
        change(driver);
        engine.poll(11, 1001);
        EXPECT_EQ(fact(engine).reason, "driverFailed");
        EXPECT_TRUE(engine.closed());
        EXPECT_FALSE(engine.released());
        EXPECT_FALSE(engine.output_permitted(12, 1002));
        EXPECT_GT(driver.stops, 0U);
        driver.release_known = true;
        engine.poll(13, 1003);
        EXPECT_TRUE(engine.released());
    }
}

TEST(FocusEngine, LateFactsDoNotCompleteNextOperation) {
    Driver driver;
    Authority auth;
    FocusEngine engine(driver, auth, "child", "focus", {"speaker", "display"}, 1000);
    establish(engine, driver);
    engine.submit(command("suspend", 1, 2, "two"), 12, 1002);
    engine.poll(12, 1002);
    driver.facts.push_back({1, true, "acquired", {"speaker", "display"}, 1});
    engine.poll(13, 1003);
    EXPECT_FALSE(engine.take_reply());
    EXPECT_FALSE(engine.closed());
    driver.complete();
    engine.poll(14, 1004);
    EXPECT_EQ(fact(engine).state, "suspended");
    driver.facts.push_back({2, true, "acquired", {"speaker"}, 1});
    engine.poll(15, 1005);
    EXPECT_FALSE(engine.take_reply());
    EXPECT_FALSE(engine.output_permitted(15, 1005));
}

TEST(FocusEngine, PendingTimeoutRevocationAndClockRegressionAreUnknownRelease) {
    const std::vector<std::function<void(FocusEngine&, Authority&)>> changes = {
        [](auto& e, auto&) { e.poll(20, 1010); }, [](auto& e, auto&) { e.poll(9, 1001); },
        [](auto& e, auto&) { e.poll(11, 999); },
        [](auto& e, auto& a) {
            a.validity = false;
            e.poll(11, 1001);
        }};
    for (const auto& change : changes) {
        Driver driver;
        Authority auth;
        FocusEngine engine(driver, auth, "child", "focus", {"speaker", "display"}, 1000, 10);
        engine.submit(command(), 10, 1000);
        engine.poll(10, 1000);
        change(engine, auth);
        const auto result = fact(engine);
        EXPECT_EQ(result.state, "denied");
        EXPECT_TRUE(engine.closed());
        EXPECT_FALSE(engine.released());
        driver.complete();
        engine.poll(21, 1011);
        EXPECT_FALSE(engine.take_reply());
        EXPECT_FALSE(engine.output_permitted(21, 1011));
        const auto replay = engine.submit(command(), 22, 1012);
        EXPECT_EQ(replay.admission, FocusAdmission::Replay);
        EXPECT_EQ(auth.calls, 1U);
        EXPECT_EQ(driver.starts, 1U);
    }
}

TEST(FocusEngine, RestoreCannotChangeIntentPolicyOrExtendOriginalDeadline) {
    Driver driver;
    Authority auth;
    FocusEngine engine(driver, auth, "child", "focus", {"speaker", "display"}, 1000);
    establish(engine, driver);
    engine.submit(command("suspend", 1, 2, "two"), 12, 1002);
    engine.poll(12, 1002);
    driver.complete();
    engine.poll(13, 1003);
    fact(engine);
    auth.intent = 8;
    engine.submit(command("restore", 2, 3, "bad"), 14, 1004);
    EXPECT_EQ(fact(engine).reason, "userIntentChanged");
    auth.intent = 7;
    engine.submit(command("restore", 2, 4, "three"), 15, 1005);
    engine.poll(15, 1005);
    driver.complete();
    engine.poll(16, 1006);
    EXPECT_EQ(fact(engine).state, "restored");
    EXPECT_TRUE(engine.output_permitted(17, 1007));
    engine.poll(1000, 1990);
    EXPECT_TRUE(engine.closed());
    EXPECT_FALSE(engine.output_permitted(1000, 1990));
}

TEST(FocusEngine, CasChangeBeforeBeginIsRejectedWithoutExecution) {
    Driver driver;
    Authority auth;
    FocusEngine engine(driver, auth, "child", "focus", {"speaker", "display"}, 1000);
    engine.submit(command(), 10, 1000);
    driver.current = 5;
    engine.poll(11, 1001);
    const auto result = fact(engine);
    EXPECT_EQ(result.reason, "staleRevision");
    EXPECT_EQ(result.revision, 5U);
    EXPECT_EQ(driver.starts, 0U);
}

TEST(FocusEngine, BoundedQueueAndHistoryDoNotEvictIdempotency) {
    Driver driver;
    Authority auth;
    FocusEngine engine(driver, auth, "child", "focus", {"speaker", "display"}, 1000);
    for (unsigned index = 0; index < 32; ++index) {
        const auto token = "deny-" + std::to_string(index);
        EXPECT_EQ(
            engine.submit(command("release", 0, index + 1, token.c_str()), 10 + index, 1000 + index)
                .admission,
            FocusAdmission::Accepted);
        if (index % 4 == 3) {
            EXPECT_EQ(engine.submit(command("release", 0, 100, "full"), 10 + index, 1000 + index)
                          .admission,
                      FocusAdmission::Full);
            for (unsigned n = 0; n < 4; ++n)
                EXPECT_EQ(fact(engine).state, "denied");
        }
    }
    EXPECT_EQ(engine.submit(command("release", 0, 100, "new"), 50, 1040).admission,
              FocusAdmission::Full);
    EXPECT_EQ(engine.submit(command("release", 0, 1, "deny-0"), 50, 1040).admission,
              FocusAdmission::Replay);
    EXPECT_EQ(driver.starts, 0U);
}

TEST(FocusEngine, ActualGrantEncodingFailureIsRetainedButNeverCurrentPermission) {
    Driver driver;
    Authority auth;
    FocusEngine engine(driver, auth, "child", "focus", {"speaker", "display"}, 1000);
    engine.submit(command(), 10, 1000);
    engine.poll(10, 1000);
    driver.complete();
    cJSON_Hooks hooks{[](std::size_t) -> void* { return nullptr; }, std::free};
    cJSON_InitHooks(&hooks);
    engine.poll(11, 1001);
    cJSON_InitHooks(nullptr);
    EXPECT_TRUE(engine.closed());
    EXPECT_FALSE(engine.released());
    EXPECT_FALSE(engine.output_permitted(12, 1002));
    driver.current = 8;
    const auto replay = engine.submit(command(), 13, 1003);
    EXPECT_EQ(replay.admission, FocusAdmission::Replay);
    FocusState result;
    ASSERT_EQ(decode_focus_state(WireRoute::Signaling, replay.reply, result), WireResult::Ok);
    EXPECT_EQ(result.state, "acquired");
    EXPECT_EQ(result.revision, 1U);
    EXPECT_EQ(driver.starts, 1U);
    EXPECT_EQ(auth.calls, 1U);
}

TEST(FocusEngine, AbsoluteDeadlineForwardJumpCannotKeepPendingOrOwnedLease) {
    for (bool completed : {false, true}) {
        Driver driver;
        Authority auth;
        FocusEngine engine(driver, auth, "child", "focus", {"speaker", "display"}, 1000);
        engine.submit(command(), 10, 1000);
        engine.poll(10, 1000);
        if (completed) {
            driver.complete();
            engine.poll(11, 1001);
            fact(engine);
        }
        engine.poll(12, 2000);
        EXPECT_TRUE(engine.closed());
        EXPECT_FALSE(engine.output_permitted(13, 2001));
        EXPECT_FALSE(engine.released());
        if (!completed) {
            EXPECT_EQ(fact(engine).reason, "leaseExpired");
        }
    }
}

TEST(FocusEngine, RestoreCannotExtendOriginalAbsoluteDeadline) {
    Driver driver;
    Authority auth;
    FocusEngine engine(driver, auth, "child", "focus", {"speaker", "display"}, 1000);
    establish(engine, driver);
    engine.submit(command("suspend", 1, 2, "pause"), 12, 1002);
    engine.poll(12, 1002);
    driver.complete();
    engine.poll(13, 1003);
    EXPECT_EQ(fact(engine).state, "suspended");
    engine.submit(command("restore", 2, 3, "resume", "explicit", "conversation", 4000), 14, 1004);
    engine.poll(14, 1004);
    driver.complete();
    engine.poll(15, 1005);
    EXPECT_EQ(fact(engine).state, "restored");
    EXPECT_TRUE(engine.output_permitted(16, 1999));
    EXPECT_FALSE(engine.output_permitted(17, 2000));
    EXPECT_TRUE(engine.closed());
    EXPECT_FALSE(engine.released());
}

TEST(FocusEngine, AdmissionErrorsNeverReachDriverOrRenewPermission) {
    Driver driver;
    Authority auth;
    FocusEngine engine(driver, auth, "child", "focus", {"speaker", "display"}, 1000);
    EXPECT_EQ(engine.submit("{}", 10, 1000).admission, FocusAdmission::Invalid);
    auto other = command();
    other.replace(other.find("child"), 5, "other");
    EXPECT_EQ(engine.submit(other, 10, 1000).admission, FocusAdmission::Invalid);
    auth.allow = false;
    engine.submit(command(), 10, 1000);
    EXPECT_EQ(fact(engine).reason, "permissionDenied");
    auth.allow = true;
    engine.submit(command("acquire", 0, 1, "stale-sequence"), 11, 1001);
    EXPECT_EQ(fact(engine).reason, "invalidState");
    engine.submit(command("acquire", 1, 2, "stale-revision"), 12, 1002);
    EXPECT_EQ(fact(engine).reason, "staleRevision");
    engine.submit(command("acquire", 0, 3, "pending"), 13, 1003);
    EXPECT_EQ(engine.submit(command("acquire", 0, 4, "busy"), 14, 1004).admission,
              FocusAdmission::Busy);
    driver.accept = false;
    engine.poll(14, 1004);
    EXPECT_EQ(fact(engine).reason, "resourceBusy");
    EXPECT_FALSE(engine.closed());
    EXPECT_FALSE(engine.output_permitted(15, 1005));
    EXPECT_EQ(driver.starts, 1U);
    EXPECT_EQ(driver.stops, 0U);
    EXPECT_EQ(auth.calls, 2U);
}

TEST(FocusEngine, StoppedFactCannotLeaveOriginalOutputOwned) {
    for (const char* operation : {"suspend", "release"}) {
        Driver driver;
        Authority auth;
        FocusEngine engine(driver, auth, "child", "focus", {"speaker", "display"}, 1000);
        establish(engine, driver);
        engine.submit(command(operation, 1, 2, "stop"), 12, 1002);
        engine.poll(12, 1002);
        EXPECT_FALSE(engine.output_permitted(12, 1002));
        driver.complete();
        driver.held = true;
        engine.poll(13, 1003);
        EXPECT_EQ(fact(engine).reason, "driverFailed");
        EXPECT_TRUE(engine.closed());
        EXPECT_FALSE(engine.released());
    }
}
