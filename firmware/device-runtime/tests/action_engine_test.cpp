// 动作状态机用可控驱动/时钟验证执行事实、重投、取消、背压与有界历史，不连接硬件。
#include "ur/device/action_engine.hpp"
#include <deque>
#include <gtest/gtest.h>
#include <map>

namespace ur::device {
namespace {
// ControlledActions 将执行、完成、取消分别控制，避免把接纳误当成功。
class ControlledActions final : public ActionDriver {
public:
    unsigned begins = 0;                       // 实际驱动启动次数。
    unsigned cancels = 0;                      // 实际取消次数。
    bool accept = true;                        // 是否能够接纳任务。
    bool quiesce = true;                       // 是否能够确认已停止后续副作用。
    std::deque<ActionCompletion> completions;  // 驱动已完成事实。
    bool begin(const ActionRequest&) override {
        ++begins;
        return accept;
    }
    std::optional<ActionCompletion> take_completion() override {
        if (completions.empty())
            return std::nullopt;
        auto result = std::move(completions.front());
        completions.pop_front();
        return result;
    }
    bool cancel(std::string_view) override {
        ++cancels;
        return quiesce;
    }
};
// request 用固定动作及可控Token构造可解析报文。
std::string request(const std::string& token, const std::string& params = R"({"volume":40})") {
    return "{\"method\":\"action\",\"msgToken\":\"" + token +
           "\",\"actionID\":\"setVolume\",\"params\":" + params + "}";
}
}  // namespace
TEST(ActionEngine, StartsOnlyWhenPolledAndReplaysOriginalCompletion) {
    ControlledActions driver;
    ActionEngine engine(driver);
    EXPECT_EQ(engine.submit(request("a"), 10).state, ActionAdmission::Accepted);
    EXPECT_EQ(driver.begins, 0U);
    EXPECT_FALSE(engine.take_reply());
    EXPECT_EQ(engine.submit(request("a"), 10).state, ActionAdmission::Pending);
    EXPECT_EQ(engine.submit(request("a", R"({"volume":41})"), 10).state, ActionAdmission::Conflict);
    EXPECT_EQ(engine.submit(request("b"), 10).state, ActionAdmission::Busy);
    engine.poll(11);
    engine.poll(12);
    EXPECT_EQ(driver.begins, 1U);
    EXPECT_FALSE(engine.take_reply());
    driver.completions.push_back({"a", 200, R"({"actualVolume":40})"});
    engine.poll(13);
    const auto reply = engine.take_reply();
    ASSERT_TRUE(reply);
    EXPECT_NE(reply->find("actualVolume"), std::string::npos);
    EXPECT_EQ(engine.submit(request("a"), 14).reply, *reply);
    EXPECT_EQ(engine.submit(request("a"), 14).state, ActionAdmission::Replay);
    EXPECT_FALSE(engine.active());
    EXPECT_FALSE(engine.take_reply());
    // 不同Token即便同值仍执行，不按参数值吞掉产品明确提出的控制。
    EXPECT_EQ(engine.submit(request("b"), 15).state, ActionAdmission::Accepted);
    engine.poll(15);
    EXPECT_EQ(driver.begins, 2U);
}
TEST(ActionEngine, CanonicalParametersIgnoreKeyOrderButNotChangedAction) {
    ControlledActions driver;
    ActionEngine engine(driver);
    EXPECT_EQ(engine.submit(request("a", R"({"a":1,"b":2})"), 0).state, ActionAdmission::Accepted);
    EXPECT_EQ(engine.submit(request("a", R"({"b":2,"a":1})"), 0).state, ActionAdmission::Pending);
    EXPECT_EQ(
        engine
            .submit(
                R"({"method":"action","msgToken":"a","actionID":"other","params":{"a":1,"b":2}})",
                0)
            .state,
        ActionAdmission::Conflict);
    EXPECT_EQ(engine.submit("{", 0).state, ActionAdmission::Invalid);
}
TEST(ActionEngine, TimeoutRejectsLateResultAndNeverRepeatsSideEffect) {
    ControlledActions driver;
    ActionEngine engine(driver, 10);
    ASSERT_EQ(engine.submit(request("a"), 1).state, ActionAdmission::Accepted);
    engine.poll(1);
    engine.poll(11);
    const auto timed_out = engine.take_reply();
    ASSERT_TRUE(timed_out);
    EXPECT_NE(timed_out->find("504"), std::string::npos);
    EXPECT_NE(timed_out->find("resultUnknown"), std::string::npos);
    EXPECT_EQ(driver.cancels, 1U);
    EXPECT_FALSE(engine.active());
    EXPECT_FALSE(engine.closed());
    EXPECT_EQ(engine.submit(request("a"), 12).state, ActionAdmission::Replay);
    ASSERT_EQ(engine.submit(request("b"), 12).state, ActionAdmission::Accepted);
    driver.completions.push_back({"a", 200, "{}"});
    driver.completions.push_back({"b", 200, "{}"});
    engine.poll(12);
    auto result = engine.take_reply();
    ASSERT_TRUE(result);
    EXPECT_NE(result->find("\"msgToken\":\"b\""), std::string::npos);
    EXPECT_EQ(driver.begins, 2U);
}
TEST(ActionEngine, UnconfirmedCancellationClosesAndClockRollbackFails) {
    ControlledActions driver;
    driver.quiesce = false;
    ActionEngine engine(driver, 10);
    ASSERT_EQ(engine.submit(request("a"), 10).state, ActionAdmission::Accepted);
    engine.poll(11);
    engine.poll(20);
    EXPECT_TRUE(engine.closed());
    EXPECT_EQ(engine.submit(request("b"), 21).state, ActionAdmission::Closed);
    EXPECT_EQ(engine.submit(request("a"), 21).state, ActionAdmission::Replay);
    ControlledActions other;
    ActionEngine rollback(other);
    ASSERT_EQ(rollback.submit(request("a"), 10).state, ActionAdmission::Accepted);
    rollback.poll(10);
    rollback.poll(9);
    EXPECT_TRUE(rollback.closed());
    EXPECT_EQ(other.cancels, 1U);
}
TEST(ActionEngine, DriverFailureInvalidResultsAndClosingAreNotSuccess) {
    ControlledActions driver;
    driver.accept = false;
    ActionEngine engine(driver);
    ASSERT_EQ(engine.submit(request("a"), 0).state, ActionAdmission::Accepted);
    engine.poll(1);
    auto failure = engine.take_reply();
    ASSERT_TRUE(failure);
    EXPECT_NE(failure->find("503"), std::string::npos);
    driver.accept = true;
    ASSERT_EQ(engine.submit(request("b"), 2).state, ActionAdmission::Accepted);
    driver.completions.push_back({"b", 200, "[]"});
    engine.poll(2);
    failure = engine.take_reply();
    ASSERT_TRUE(failure);
    EXPECT_NE(failure->find("invalidDriverResult"), std::string::npos);
    ASSERT_EQ(engine.submit(request("c"), 3).state, ActionAdmission::Accepted);
    engine.close();
    engine.close();
    EXPECT_EQ(driver.cancels, 0U);  // 从未开始的任务不能触发硬件取消。
    EXPECT_TRUE(engine.closed());
    EXPECT_EQ(engine.submit(request("c"), 4).state, ActionAdmission::Replay);
    ActionEngine invalid(driver, 0);
    EXPECT_EQ(invalid.submit(request("d"), 0).state, ActionAdmission::Closed);
}
TEST(ActionEngine, HistoryCapacityNeverEvictsCompletedTokens) {
    ControlledActions driver;
    ActionEngine engine(driver);
    for (unsigned index = 0; index < 32; ++index) {
        const auto token = std::to_string(index);
        ASSERT_EQ(engine.submit(request(token), index).state, ActionAdmission::Accepted);
        driver.completions.push_back({token, 200, "{}"});
        engine.poll(index);
        ASSERT_TRUE(engine.take_reply());
    }
    EXPECT_EQ(engine.submit(request("next"), 33).state, ActionAdmission::Full);
    EXPECT_EQ(engine.submit(request("0"), 33).state, ActionAdmission::Replay);
    EXPECT_EQ(driver.begins, 32U);
}
TEST(ActionEngine, ReplyBackpressureStopsNewActionsUntilDrained) {
    ControlledActions driver;
    ActionEngine engine(driver);
    for (unsigned index = 0; index < 4; ++index) {
        const auto token = std::to_string(index);
        ASSERT_EQ(engine.submit(request(token), index).state, ActionAdmission::Accepted);
        driver.completions.push_back({token, 200, "{}"});
        engine.poll(index);
    }
    EXPECT_EQ(engine.submit(request("next"), 5).state, ActionAdmission::Busy);
    EXPECT_EQ(driver.begins, 4U);
    ASSERT_TRUE(engine.take_reply());
    EXPECT_EQ(engine.submit(request("next"), 5).state, ActionAdmission::Accepted);
}
TEST(ActionEngine, RetainedByteBudgetReservesReplyBeforeStartingDriver) {
    ControlledActions driver;
    ActionEngine engine(driver);
    const auto params = "{\"value\":\"" + std::string(60000, 'x') + "\"}";
    for (unsigned index = 0; index < 3; ++index) {
        const auto token = std::to_string(index);
        ASSERT_EQ(engine.submit(request(token, params), index).state, ActionAdmission::Accepted);
        driver.completions.push_back({token, 200, "{}"});
        engine.poll(index);
        ASSERT_TRUE(engine.take_reply());
    }
    EXPECT_EQ(engine.submit(request("full", params), 4).state, ActionAdmission::Full);
    EXPECT_EQ(driver.begins, 3U);
}
TEST(ActionEngine, LateCompletionFloodIsBoundedPerPoll) {
    ControlledActions driver;
    ActionEngine engine(driver);
    ASSERT_EQ(engine.submit(request("active"), 0).state, ActionAdmission::Accepted);
    for (unsigned index = 0; index < 4; ++index)
        driver.completions.push_back({"retired", 200, "{}"});
    driver.completions.push_back({"active", 422, "{}"});
    engine.poll(1);
    EXPECT_TRUE(engine.active());
    EXPECT_FALSE(engine.take_reply());
    engine.poll(2);
    const auto reply = engine.take_reply();
    ASSERT_TRUE(reply);
    EXPECT_NE(reply->find("422"), std::string::npos);
    EXPECT_FALSE(engine.active());
}
TEST(ActionEngine, AdmissionTimeoutDoesNotStartExpiredHardwareWork) {
    ControlledActions driver;
    ActionEngine engine(driver, 1);
    ASSERT_EQ(engine.submit(request("expired"), 10).state, ActionAdmission::Accepted);
    engine.poll(11);
    EXPECT_EQ(driver.begins, 0U);
    EXPECT_EQ(driver.cancels, 0U);
    EXPECT_TRUE(engine.take_reply());
    EXPECT_EQ(engine.submit(request("expired"), 12).state, ActionAdmission::Replay);
}

// MemoryLedger
// 按clienttool账本模式固化受理与原终态；重启后同Token只重放，不重发副作用。
class MemoryLedger final : public ActionHistoryLedger {
   public:
    bool writable = true;                           // false模拟账本不可写。
    ActionPersistence admission = ActionPersistence::Stored;  // 可控受理固化事实。
    ActionPersistence terminal = ActionPersistence::Stored;   // 可控终态固化事实。
    std::map<std::string, ActionRequest> accepted;  // 原受理登记，重启后仍须比较行为和参数。
    std::map<std::string, std::string> replies;     // 已固化回执（Token→原JSON）。
    bool recorded(std::string_view token) const override {
        return accepted.find(std::string(token)) != accepted.end();
    }
    std::optional<ActionRequest> find_request(std::string_view token) const override {
        const auto entry = accepted.find(std::string(token));
        if (entry == accepted.end())
            return std::nullopt;
        return entry->second;
    }
    std::optional<std::string> find_reply(std::string_view token) const override {
        const auto entry = replies.find(std::string(token));
        if (entry == replies.end())
            return std::nullopt;
        return entry->second;
    }
    bool record(const ActionRequest& req) override {
        if (!writable)
            return false;
        return accepted.emplace(req.token, req).second;
    }
    bool record_reply(std::string_view token, std::string_view reply) override {
        if (!writable)
            return false;
        replies[std::string(token)] = std::string(reply);
        return true;
    }
    ActionPersistence persistence(std::string_view, bool completed) const override {
        return completed ? terminal : admission;
    }
};

TEST(ActionEngine, QueuedAdmissionCannotStartUntilActuallyStored) {
    ControlledActions driver;
    MemoryLedger ledger;
    ledger.admission = ActionPersistence::Pending;
    ActionEngine engine(driver, 5000, &ledger);
    ASSERT_EQ(engine.submit(request("persist"), 100).state, ActionAdmission::Accepted);
    engine.poll(101);
    EXPECT_EQ(driver.begins, 0U);
    EXPECT_FALSE(engine.take_reply());
    ledger.admission = ActionPersistence::Stored;
    engine.poll(102);
    EXPECT_EQ(driver.begins, 1U);
}

TEST(ActionEngine, ActualCompletionWaitsForTerminalPersistenceBeforePublishingOrReplay) {
    ControlledActions driver;
    MemoryLedger ledger;
    ledger.terminal = ActionPersistence::Pending;
    ActionEngine engine(driver, 5000, &ledger);
    ASSERT_EQ(engine.submit(request("persist"), 100).state, ActionAdmission::Accepted);
    driver.completions.push_back({"persist", 200, R"({"volume":40})"});
    engine.poll(101);
    ASSERT_EQ(driver.begins, 1U);
    EXPECT_FALSE(engine.take_reply());
    EXPECT_EQ(engine.submit(request("persist"), 102).state, ActionAdmission::Pending);
    ledger.terminal = ActionPersistence::Stored;
    ASSERT_TRUE(engine.take_reply());
    EXPECT_EQ(engine.submit(request("persist"), 103).state, ActionAdmission::Replay);
    EXPECT_EQ(driver.begins, 1U);
}

TEST(ActionEngine, FailedAdmissionPersistenceStopsWithoutDriverSideEffect) {
    ControlledActions driver;
    MemoryLedger ledger;
    ledger.admission = ActionPersistence::Failed;
    ActionEngine engine(driver, 5000, &ledger);
    ASSERT_EQ(engine.submit(request("persist"), 100).state, ActionAdmission::Accepted);
    engine.poll(101);
    EXPECT_EQ(driver.begins, 0U);
    EXPECT_TRUE(engine.closed());
}

TEST(ActionEngine, AdmissionTimeoutThenLateDiskCompletionCannotStartOldAction) {
    ControlledActions driver;
    MemoryLedger ledger;
    ledger.admission = ActionPersistence::Pending;
    ActionEngine engine(driver, 10, &ledger);
    ASSERT_EQ(engine.submit(request("persist"), 100).state, ActionAdmission::Accepted);
    engine.poll(110);
    ledger.admission = ActionPersistence::Stored;
    engine.poll(111);
    EXPECT_EQ(driver.begins, 0U);
    EXPECT_EQ(driver.cancels, 0U);
    ASSERT_TRUE(engine.take_reply());
    EXPECT_EQ(engine.submit(request("persist"), 112).state, ActionAdmission::Replay);
}

TEST(ActionEngine, FailedTerminalPersistenceNeverPublishesOrReplaysSuccess) {
    ControlledActions driver;
    MemoryLedger ledger;
    ActionEngine engine(driver, 5000, &ledger);
    ASSERT_EQ(engine.submit(request("persist"), 100).state, ActionAdmission::Accepted);
    driver.completions.push_back({"persist", 200, "{}"});
    ledger.terminal = ActionPersistence::Failed;
    engine.poll(101);
    EXPECT_FALSE(engine.take_reply());
    EXPECT_TRUE(engine.closed());
    EXPECT_EQ(engine.submit(request("persist"), 102).state, ActionAdmission::Closed);
    EXPECT_EQ(engine.submit(request("next"), 103).state, ActionAdmission::Closed);
    EXPECT_EQ(driver.begins, 1U);
}

TEST(ActionEngine, TerminalEnqueueFailureDoesNotTrustSynchronousDefaultPersistence) {
    ControlledActions driver;
    MemoryLedger ledger;
    ActionEngine engine(driver, 5000, &ledger);
    ASSERT_EQ(engine.submit(request("persist"), 100).state, ActionAdmission::Accepted);
    driver.completions.push_back({"persist", 200, "{}"});
    ledger.writable = false;
    engine.poll(101);
    EXPECT_FALSE(engine.take_reply());
    EXPECT_TRUE(engine.closed());
    EXPECT_EQ(engine.submit(request("persist"), 102).state, ActionAdmission::Closed);
    EXPECT_EQ(driver.begins, 1U);
}

// 跨重启同Token不重复执行：优雅关闭时在途动作已固化为504未知终态，重投Replay原文；
// 崩溃场景（账本仅受理无终态）重投保持Pending并给出合成未知回执；
// 两条路径都绝不触发驱动begin；账本没有的新Token照常执行。
TEST(ActionEngine, LedgerReplayAndPendingAcrossRestartNeverReexecute) {
    ControlledActions driver;
    MemoryLedger ledger;
    {
        ActionEngine engine(driver, 5000, &ledger);
        ASSERT_EQ(engine.submit(request("done"), 0).state, ActionAdmission::Accepted);
        driver.completions.push_back({"done", 200, R"({"actualVolume":40})"});
        engine.poll(1);
        ASSERT_TRUE(engine.take_reply());
        ASSERT_EQ(engine.submit(request("inflight"), 2).state, ActionAdmission::Accepted);
        engine.poll(2);  // in-flight已启动但驱动未完成，作用域结束按关闭语义固化504未知。
        ASSERT_FALSE(engine.take_reply());
    }
    ControlledActions restarted;  // 重启后新执行器实例，同一账本。
    ActionEngine engine(restarted, 5000, &ledger);
    const auto replay = engine.submit(request("done"), 10);
    EXPECT_EQ(replay.state, ActionAdmission::Replay);
    EXPECT_NE(replay.reply.find("actualVolume"), std::string::npos);
    // 优雅关闭已在账本固化504未知终态：重投只重放原文，不重执行。
    const auto closed = engine.submit(request("inflight"), 11);
    EXPECT_EQ(closed.state, ActionAdmission::Replay);
    EXPECT_NE(closed.reply.find("resultUnknown"), std::string::npos);
    // 崩溃场景：账本只有受理登记、终态缺失，重投保持Pending并携带合成未知回执。
    ActionRequest crashed;
    ASSERT_EQ(decode_action_request(request("crashed"), crashed), DecodeResult::Ok);
    ASSERT_TRUE(ledger.record(crashed));
    const auto pending = engine.submit(request("crashed"), 12);
    EXPECT_EQ(pending.state, ActionAdmission::Pending);
    EXPECT_NE(pending.reply.find("resultUnknown"), std::string::npos);
    EXPECT_EQ(engine.submit(request("crashed", R"({"volume":90})"), 12).state,
              ActionAdmission::Conflict);
    EXPECT_EQ(restarted.begins, 0U);  // 三条恢复路径都不得重复执行副作用。
    ASSERT_EQ(engine.submit(request("fresh"), 13).state, ActionAdmission::Accepted);
    engine.poll(13);
    EXPECT_EQ(restarted.begins, 1U);
}

// 账本兜底下历史满32条时最旧已完成Token可驱逐腾位；被驱逐Token重投从账本Replay，
// 不重新执行；无账本时保持既有Full语义（容量与拒绝行为向后兼容）。
TEST(ActionEngine, LedgerEnablesOldestRetiredEvictionWhenHistoryFull) {
    ControlledActions driver;
    MemoryLedger ledger;
    ActionEngine engine(driver, 5000, &ledger);
    for (unsigned index = 0; index < 32; ++index) {
        const auto token = std::to_string(index);
        ASSERT_EQ(engine.submit(request(token), index).state, ActionAdmission::Accepted);
        driver.completions.push_back({token, 200, "{}"});
        engine.poll(index);
        ASSERT_TRUE(engine.take_reply());
    }
    // 满容量时新Token可受理：最旧已完成记录被驱逐，账本保留其终态。
    EXPECT_EQ(engine.submit(request("next"), 33).state, ActionAdmission::Accepted);
    driver.completions.push_back({"next", 200, "{}"});
    engine.poll(33);
    EXPECT_EQ(driver.begins, 33U);
    // 被驱逐的最旧Token重投：账本Replay原文，不重新执行。
    const auto replay = engine.submit(request("0"), 34);
    EXPECT_EQ(replay.state, ActionAdmission::Replay);
    EXPECT_EQ(engine.submit(request("0", R"({"volume":90})"), 34).state, ActionAdmission::Conflict);
    engine.poll(34);
    EXPECT_EQ(driver.begins, 33U);
    // 无账本引擎在同样满容量时保持Full拒绝，不会驱逐后冒重复执行风险。
    ControlledActions legacyDriver;
    ActionEngine legacy(legacyDriver);
    for (unsigned index = 0; index < 32; ++index) {
        const auto token = std::to_string(index);
        ASSERT_EQ(legacy.submit(request(token), index).state, ActionAdmission::Accepted);
        legacyDriver.completions.push_back({token, 200, "{}"});
        legacy.poll(index);
        ASSERT_TRUE(legacy.take_reply());
    }
    EXPECT_EQ(legacy.submit(request("next"), 33).state, ActionAdmission::Full);
    EXPECT_EQ(legacyDriver.begins, 32U);
}

// 重启恢复也必须核对原请求，不能将旧成功回执套用于同Token的新参数或新动作。
TEST(ActionEngine, RestartedLedgerRejectsChangedRequest) {
    ControlledActions first;
    MemoryLedger ledger;
    {
        ActionEngine engine(first, 5000, &ledger);
        ASSERT_EQ(engine.submit(request("fixed"), 0).state, ActionAdmission::Accepted);
        first.completions.push_back({"fixed", 200, "{}"});
        engine.poll(1);
        ASSERT_TRUE(engine.take_reply());
    }
    ControlledActions driver;
    ActionEngine restored(driver, 5000, &ledger);
    EXPECT_EQ(restored.submit(request("fixed", R"({"volume":90})"), 2).state,
              ActionAdmission::Conflict);
    EXPECT_EQ(
        restored
            .submit(
                R"({"method":"action","msgToken":"fixed","actionID":"other","params":{"volume":40}})",
                3)
            .state,
        ActionAdmission::Conflict);
    EXPECT_EQ(restored.submit(request("fixed"), 4).state, ActionAdmission::Replay);
    EXPECT_EQ(driver.begins, 0U);
}

// 账本不可写时保守停机：持久受理失败意味着重启后无法拒绝重放，不得继续制造副作用。
TEST(ActionEngine, LedgerWriteFailureStopsNewActions) {
    ControlledActions driver;
    MemoryLedger ledger;
    ledger.writable = false;
    ActionEngine engine(driver, 5000, &ledger);
    EXPECT_EQ(engine.submit(request("a"), 0).state, ActionAdmission::Closed);
    EXPECT_EQ(driver.begins, 0U);
    EXPECT_TRUE(engine.closed());
}
}  // namespace ur::device
