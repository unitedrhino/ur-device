// 可控文件播放器回归：实际状态/代次/焦点/超时，不将模拟驱动当真实解码或MCP验收。
#include "ur/device/playback_engine.hpp"
#include <cJSON.h>
#include <gtest/gtest.h>
#include <deque>
#include <limits>

namespace ur::device {
// PlaybackStaticFaultTest 直接触发私有事实出口缺失关联，不能由解析器拒绝代替此故障。
class PlaybackStaticFaultTest {
public:
    // emit_without_operation 保留原空在途状态，返回实际私有出口的接纳结果。
    static bool emit_without_operation(PlaybackEngine& engine) {
        return engine.emit(PlaybackFact{0, false, 200, "loaded", 0, 1000, 1}, false);
    }
    // missing_rate_fact 删除已解析在途命令的倍率，再核真实事实准入；不经过 wire 拒绝。
    static bool missing_rate_fact(PlaybackEngine& engine, const PlaybackFact& fact) {
        engine.history_[*engine.active_].command.rate.reset();
        return engine.valid_fact(fact);
    }
};
namespace {
// reject_json_allocation 精确拒绝本用例中的cJSON分配，不影响GTest或C++容器。
void* reject_json_allocation(std::size_t) { return nullptr; }
// JsonAllocationFailure 用RAII恢复全局钩子；核心用例按事件线程顺序执行，无异步解析线程。
class JsonAllocationFailure {
public:
    JsonAllocationFailure() {
        cJSON_Hooks hooks{reject_json_allocation, nullptr};  // 释放仍沿用库默认free。
        cJSON_InitHooks(&hooks);
    }
    ~JsonAllocationFailure() { cJSON_InitHooks(nullptr); }
};
// PlayerDriver 独立控制受理与实际完成，队列只由测试线程访问。
class PlayerDriver final : public PlaybackDriver {
public:
    bool accept = true;              // 驱动是否可接纳。
    bool permitted = true;           // 实际输出租约状态。
    bool stopped = true;             // 能否确认本实例已切断媒体。
    unsigned begins = 0;             // 实际接纳调用次数。
    unsigned stops = 0;              // 实际媒体切断次数。
    std::uint64_t generation = 0;    // 最后接纳的代次。
    std::deque<PlaybackFact> facts;  // 可控实际事实。
    bool begin(const PlaybackCommand&, std::uint64_t candidate) override {
        ++begins;
        if (accept)
            generation = candidate;
        return accept;
    }
    std::optional<PlaybackFact> take_fact() override {
        if (facts.empty())
            return std::nullopt;
        auto fact = std::move(facts.front());
        facts.pop_front();
        return fact;
    }
    bool output_permitted() const override { return permitted; }
    bool stop_all() override {
        ++stops;
        return stopped;
    }
    // report 可发旧代次事实；真实驱动只能在输出状态确实变化后交付它。
    void report(std::string state, bool spontaneous = false, std::uint64_t position = 0,
                double rate = 1, std::optional<std::uint64_t> epoch = std::nullopt) {
        facts.push_back({epoch.value_or(generation), spontaneous, 200, std::move(state), position,
                         30000, rate});
    }
};
// command 每条平台请求带独立token、修订号和信令水位，引用不是真实媒体地址。
std::string command(const std::string& token, const std::string& operation, std::uint64_t revision,
                    std::uint64_t sequence, const std::string& extra = "") {
    return "{\"msgToken\":\"" + token +
           "\",\"method\":\"playbackControl\",\"sessionId\":\"child\","
           "\"eventSeq\":" +
           std::to_string(sequence) + ",\"params\":{\"operation\":\"" + operation +
           "\",\"playbackId\":\"p\",\"expectedRevision\":" + std::to_string(revision) +
           (operation == "load" ? ",\"contentRef\":\"asset:authorized\"" : "") + extra + "}}";
}
// complete 断言不先产生成功，驱动实际事实后才返回可解析回执。
std::string complete(PlaybackEngine& engine, PlayerDriver& driver, const std::string& json,
                     const std::string& state, std::uint64_t now = 0, double rate = 1,
                     std::uint64_t position = 0) {
    EXPECT_EQ(engine.submit(json, now).admission, PlaybackAdmission::Accepted);
    EXPECT_FALSE(engine.take_reply());
    engine.poll(now);
    driver.report(state, false, position, rate);
    engine.poll(now);
    auto reply = engine.take_reply();
    EXPECT_TRUE(reply);
    return reply.value_or("");
}
// load 准备唯一播放器实例，不自动播放。
void load(PlaybackEngine& engine, PlayerDriver& driver) {
    complete(engine, driver, command("load", "load", 0, 1), "loaded");
}
// play 先加载再通过独立play取得实际playing。
void play(PlaybackEngine& engine, PlayerDriver& driver) {
    load(engine, driver);
    complete(engine, driver, command("play", "play", 1, 2), "playing");
}
}  // namespace

// 缺信令水位不推进播放器修订/代次或调用驱动；随后原合法水位仍可受理。
TEST(PlaybackStaticFault, MissingActiveCannotEmitOrAdvanceRevision) {
    PlayerDriver driver;
    PlaybackEngine engine(driver, "child", "p", 1000);
    EXPECT_FALSE(PlaybackStaticFaultTest::emit_without_operation(engine));
    EXPECT_EQ(engine.revision(), std::uint64_t{0});
    EXPECT_FALSE(engine.take_reply());
    EXPECT_EQ(driver.begins, 0U);
    EXPECT_EQ(engine.submit(command("load", "load", 0, 1), 0).admission,
              PlaybackAdmission::Accepted);
}

// 在途 setRate 丢失倍率前置后不能采纳事实；驱动次数和业务修订均保持原值。
TEST(PlaybackStaticFault, MissingRateCannotConfirmActualFact) {
    PlayerDriver driver;
    PlaybackEngine engine(driver, "child", "p", 1000);
    load(engine, driver);
    ASSERT_EQ(engine.submit(command("rate", "setRate", 1, 2, ",\"rate\":1.5"), 1).admission,
              PlaybackAdmission::Accepted);
    engine.poll(1);
    const auto begins = driver.begins;  // 原驱动已接纳次数，故障检查不得重复驱动。
    EXPECT_FALSE(PlaybackStaticFaultTest::missing_rate_fact(
        engine, PlaybackFact{driver.generation, false, 200, "loaded", 0, 1000, 1.5}));
    EXPECT_EQ(engine.revision(), std::uint64_t{1});
    EXPECT_EQ(driver.begins, begins);
    EXPECT_FALSE(engine.take_reply());
}

// 缺信令水位不推进播放器修订/代次或调用驱动；随后原合法水位仍可受理。
TEST(PlaybackEngine, MissingSequenceDoesNotAdmitOrConsumeValidSequence) {
    PlayerDriver driver;
    PlaybackEngine engine(driver, "child", "p", 1000);
    const auto valid = command("load", "load", 0, 1);
    auto missing = valid;
    const auto position = missing.find(",\"eventSeq\":1");
    ASSERT_NE(position, std::string::npos);
    missing.erase(position, std::string(",\"eventSeq\":1").size());
    EXPECT_EQ(engine.submit(missing, 1).admission, PlaybackAdmission::Invalid);
    EXPECT_EQ(engine.revision(), 0U);
    EXPECT_EQ(driver.begins, 0U);
    EXPECT_EQ(engine.submit(valid, 1).admission, PlaybackAdmission::Accepted);
}

TEST(PlaybackEngine, ActualCompletionAndCanonicalReplay) {
    PlayerDriver driver;
    PlaybackEngine engine(driver, "child", "p", 100000);
    const auto json = command("load", "load", 0, 1);
    EXPECT_EQ(engine.submit(json, 0).admission, PlaybackAdmission::Accepted);
    EXPECT_EQ(driver.begins, 0U);
    EXPECT_EQ(engine.submit(json, 0).admission, PlaybackAdmission::Pending);
    EXPECT_EQ(engine.submit(command("load", "load", 0, 2), 0).admission,
              PlaybackAdmission::Conflict);
    EXPECT_EQ(engine.submit(command("other", "load", 0, 2), 0).admission, PlaybackAdmission::Busy);
    engine.poll(1);
    engine.poll(2);
    EXPECT_EQ(driver.begins, 1U);
    EXPECT_TRUE(engine.state().empty());
    EXPECT_FALSE(engine.take_reply());
    driver.report("loaded");
    engine.poll(3);
    const auto reply = engine.take_reply();
    ASSERT_TRUE(reply);
    Envelope envelope;
    ASSERT_EQ(decode_envelope(*reply, envelope), DecodeResult::Ok);
    EXPECT_EQ(envelope.token, "load");
    EXPECT_EQ(envelope.session, "child");
    EXPECT_EQ(envelope.code, 200);
    EXPECT_FALSE(envelope.request);
    EXPECT_FALSE(envelope.event_sequence);
    EXPECT_NE(envelope.payload.find("\"positionMs\":0"), std::string::npos);
    EXPECT_NE(envelope.payload.find("\"revision\":1"), std::string::npos);
    EXPECT_EQ(engine.state(), "loaded");
    EXPECT_EQ(engine.revision(), 1U);
    EXPECT_EQ(engine.submit(json, 4).admission, PlaybackAdmission::Replay);
    EXPECT_EQ(engine.submit(json, 4).reply, *reply);
    EXPECT_EQ(driver.begins, 1U);
    const auto reordered =
        R"({"params":{"contentRef":"asset:authorized","expectedRevision":0,"playbackId":"p","operation":"load"},"eventSeq":1,"sessionId":"child","method":"playbackControl","msgToken":"load"})";
    EXPECT_EQ(engine.submit(reordered, 4).reply, *reply);
    EXPECT_EQ(engine.submit(command("load", "load", 1, 1), 4).admission,
              PlaybackAdmission::Conflict);
}
// 异步play可能已开始出声但事实尚未返回；撤权不能等待playing回执或整个操作超时。
TEST(PlaybackEngine, RevokedLeaseWhilePlayPendingCutsOutput) {
    PlayerDriver driver;
    PlaybackEngine engine(driver, "child", "p", 100000);
    load(engine, driver);
    ASSERT_EQ(engine.submit(command("play", "play", 1, 2), 1).admission,
              PlaybackAdmission::Accepted);
    engine.poll(1);
    ASSERT_TRUE(engine.active());
    ASSERT_EQ(engine.state(), "loaded");
    driver.permitted = false;
    engine.poll(2);
    EXPECT_TRUE(engine.closed());
    EXPECT_GT(driver.stops, 0U);
    const auto reply = engine.take_reply();
    ASSERT_TRUE(reply);
    EXPECT_NE(reply->find("resourceLost"), std::string::npos);
}
TEST(PlaybackEngine, IndependentOperationsAndTerminalInstance) {
    PlayerDriver driver;
    PlaybackEngine engine(driver, "child", "p", 100000);
    load(engine, driver);
    complete(engine, driver, command("play", "play", 1, 2), "playing");
    complete(engine, driver, command("pause", "pause", 2, 3), "paused", 1);
    complete(engine, driver, command("seek", "seek", 3, 4, ",\"positionMs\":1234"), "paused", 2, 1,
             1200);
    complete(engine, driver, command("rate", "setRate", 4, 5, ",\"rate\":1.5"), "paused", 3, 1.5,
             1200);
    complete(engine, driver, command("resume", "play", 5, 6), "playing", 4, 1.5, 1200);
    const auto stopped =
        complete(engine, driver, command("stop", "stop", 6, 7), "stopped", 5, 1.5, 1300);
    EXPECT_EQ(engine.revision(), 7U);
    EXPECT_EQ(driver.stops, 1U);
    EXPECT_EQ(engine.submit(command("again", "play", 7, 8), 5).admission,
              PlaybackAdmission::StateConflict);
    EXPECT_EQ(engine.submit(command("stop", "stop", 6, 7), 5).reply, stopped);
    driver.report("playing");
    engine.poll(6);
    EXPECT_EQ(engine.state(), "stopped");
    EXPECT_EQ(engine.revision(), 7U);
    EXPECT_FALSE(engine.take_reply());
}
TEST(PlaybackEngine, WrongSessionInstanceRevisionAndWatermark) {
    PlayerDriver driver;
    PlaybackEngine engine(driver, "child", "p", 100000);
    EXPECT_EQ(engine.submit("{}", 0).admission, PlaybackAdmission::Invalid);
    auto wrong = command("load", "load", 0, 1);
    wrong.replace(wrong.find("child"), 5, "other");
    EXPECT_EQ(engine.submit(wrong, 0).admission, PlaybackAdmission::Invalid);
    EXPECT_EQ(engine.submit(command("load", "load", 0, 1, ",\"mediaRevision\":1"), 0).admission,
              PlaybackAdmission::Invalid);
    wrong = command("load", "load", 0, 1);
    wrong.replace(wrong.find("\"p\""), 3, "\"another\"");
    EXPECT_EQ(engine.submit(wrong, 0).admission, PlaybackAdmission::Invalid);
    EXPECT_EQ(engine.submit(command("play", "play", 0, 1), 0).admission,
              PlaybackAdmission::StateConflict);
    load(engine, driver);
    EXPECT_EQ(engine.submit(command("play", "play", 0, 2), 0).admission, PlaybackAdmission::Stale);
    EXPECT_EQ(engine.submit(command("play", "play", 1, 1), 0).admission, PlaybackAdmission::Stale);
    EXPECT_EQ(engine.submit(command("load2", "load", 1, 2), 0).admission,
              PlaybackAdmission::StateConflict);
    EXPECT_EQ(engine.submit(command("pause", "pause", 1, 2), 0).admission,
              PlaybackAdmission::StateConflict);
    EXPECT_EQ(driver.begins, 1U);
}
TEST(PlaybackEngine, MissingOrLostOutputPermissionNeverReportsPlaying) {
    PlayerDriver driver;
    PlaybackEngine engine(driver, "child", "p", 100000);
    load(engine, driver);
    driver.permitted = false;
    const auto json = command("play", "play", 1, 2);
    EXPECT_EQ(engine.submit(json, 0).admission, PlaybackAdmission::ResourceBusy);
    driver.permitted = true;
    EXPECT_EQ(engine.submit(json, 0).admission, PlaybackAdmission::Accepted);
    driver.permitted = false;
    engine.poll(0);
    auto failed = engine.take_reply();
    ASSERT_TRUE(failed);
    EXPECT_NE(failed->find("resourceBusy"), std::string::npos);
    EXPECT_EQ(driver.begins, 1U);
    driver.permitted = true;
    complete(engine, driver, command("play2", "play", 1, 3), "playing");
    driver.permitted = false;
    engine.poll(1);
    EXPECT_TRUE(engine.closed());
    EXPECT_GE(driver.stops, 1U);
}
TEST(PlaybackEngine, FailedBeginPreservesExistingMediaGeneration) {
    PlayerDriver driver;
    PlaybackEngine engine(driver, "child", "p", 100000);
    play(engine, driver);
    const auto playing_generation = driver.generation;
    driver.accept = false;
    const auto json = command("pause", "pause", 2, 3);
    EXPECT_EQ(engine.submit(json, 0).admission, PlaybackAdmission::Accepted);
    engine.poll(0);
    ASSERT_TRUE(engine.take_reply());
    driver.report("ended", true, 30000, 1, playing_generation);
    engine.poll(1);
    EXPECT_EQ(engine.state(), "ended");
    EXPECT_EQ(engine.revision(), 3U);
    EXPECT_TRUE(engine.take_reply());
}
TEST(PlaybackEngine, TimeoutAndUnconfirmedStopNeverReviveInstance) {
    PlayerDriver driver;
    driver.stopped = false;
    PlaybackEngine engine(driver, "child", "p", 100000, 10);
    const auto json = command("load", "load", 0, 1);
    EXPECT_EQ(engine.submit(json, 0).admission, PlaybackAdmission::Accepted);
    engine.poll(9);
    engine.poll(10);
    auto reply = engine.take_reply();
    ASSERT_TRUE(reply);
    EXPECT_NE(reply->find("resultUnknown"), std::string::npos);
    EXPECT_NE(reply->find("504"), std::string::npos);
    EXPECT_TRUE(engine.closed());
    EXPECT_EQ(engine.revision(), 0U);
    driver.report("loaded");
    engine.poll(11);
    EXPECT_FALSE(engine.take_reply());
    EXPECT_EQ(engine.submit(json, 11).reply, *reply);
    EXPECT_EQ(engine.submit(command("new", "load", 0, 2), 11).admission, PlaybackAdmission::Closed);
    engine.close();
    EXPECT_EQ(driver.stops, 1U);
}
TEST(PlaybackEngine, ClockRegressionAndDeadlineCloseActualOutput) {
    for (const bool expiry : {false, true}) {
        PlayerDriver driver;
        PlaybackEngine engine(driver, "child", "p", 20);
        play(engine, driver);
        engine.poll(10);
        engine.poll(expiry ? 20 : 9);
        EXPECT_TRUE(engine.closed());
        EXPECT_EQ(driver.stops, 1U);
        EXPECT_EQ(engine.submit(command("p2", "play", 2, 3), expiry ? 20 : 10).admission,
                  expiry ? PlaybackAdmission::Expired : PlaybackAdmission::Closed);
        EXPECT_EQ(engine.submit(command("load", "load", 0, 1), expiry ? 20 : 10).admission,
                  PlaybackAdmission::Replay);
    }
}
TEST(PlaybackEngine, LateAndDuplicateFactsDoNotAdvanceRevision) {
    PlayerDriver driver;
    PlaybackEngine engine(driver, "child", "p", 100000);
    load(engine, driver);
    const auto old = driver.generation;
    const auto json = command("play", "play", 1, 2);
    EXPECT_EQ(engine.submit(json, 0).admission, PlaybackAdmission::Accepted);
    engine.poll(0);
    driver.report("loaded", false, 0, 1, old);
    driver.report("playing");
    driver.report("playing");
    engine.poll(0);
    EXPECT_EQ(engine.revision(), 2U);
    ASSERT_TRUE(engine.take_reply());
    EXPECT_FALSE(engine.take_reply());
    EXPECT_FALSE(engine.closed());
}
TEST(PlaybackEngine, NaturalEndAndFailureUseIndependentEvents) {
    for (const auto* state : {"ended", "failed"}) {
        PlayerDriver driver;
        PlaybackEngine engine(driver, "child", "p", 100000);
        play(engine, driver);
        driver.report(state, true, 30000);
        engine.poll(1);
        auto reply = engine.take_reply();
        ASSERT_TRUE(reply);
        Envelope envelope;
        ASSERT_EQ(decode_envelope(*reply, envelope), DecodeResult::Ok);
        EXPECT_FALSE(envelope.code);
        EXPECT_NE(envelope.token, "play");
        EXPECT_NE(envelope.token.find("player-event-"), std::string::npos);
        EXPECT_EQ(engine.revision(), 3U);
        EXPECT_EQ(engine.state(), state);
        EXPECT_EQ(driver.stops, 1U);
        EXPECT_EQ(engine.submit(command("replay", "play", 3, 3), 1).admission,
                  PlaybackAdmission::StateConflict);
    }
}
TEST(PlaybackEngine, InvalidDriverFactsAreFailClosedAndRedacted) {
    for (unsigned variant = 0; variant < 7; ++variant) {
        PlayerDriver driver;
        PlaybackEngine engine(driver, "child", "p", 100000);
        const auto json = command("load", "load", 0, 1);
        ASSERT_EQ(engine.submit(json, 0).admission, PlaybackAdmission::Accepted);
        engine.poll(0);
        driver.report("loaded");
        auto& fact = driver.facts.back();
        if (variant == 0)
            fact.state = "playing";
        if (variant == 1)
            fact.rate = 1.25;
        if (variant == 2)
            fact.position_ms = 30001;
        if (variant == 3)
            fact.duration_ms = 9007199254740992ULL;
        if (variant == 4)
            fact.code = 202;
        if (variant == 5)
            fact.spontaneous = true;
        if (variant == 6)
            fact.rate = std::numeric_limits<double>::quiet_NaN();
        engine.poll(1);
        EXPECT_TRUE(engine.closed());
        EXPECT_EQ(engine.revision(), 0U);
        auto reply = engine.take_reply();
        ASSERT_TRUE(reply);
        EXPECT_NE(reply->find("invalidDriverResult"), std::string::npos);
        EXPECT_EQ(reply->find("asset:"), std::string::npos);
        EXPECT_EQ(reply->find("positionMs"), std::string::npos);
    }
}
TEST(PlaybackEngine, DeviceFailureCannotContinueOldOutput) {
    PlayerDriver driver;
    PlaybackEngine engine(driver, "child", "p", 100000);
    play(engine, driver);
    const auto json = command("pause", "pause", 2, 3);
    ASSERT_EQ(engine.submit(json, 0).admission, PlaybackAdmission::Accepted);
    engine.poll(0);
    driver.report("paused");
    driver.facts.back().code = 500;
    engine.poll(1);
    auto reply = engine.take_reply();
    ASSERT_TRUE(reply);
    EXPECT_NE(reply->find("deviceRejected"), std::string::npos);
    EXPECT_EQ(reply->find("state"), std::string::npos);  // 不夹带成功状态。
    EXPECT_TRUE(engine.closed());
    EXPECT_EQ(driver.stops, 1U);
    EXPECT_EQ(engine.submit(json, 1).reply, *reply);
}
TEST(PlaybackEngine, BoundedHistoryAndReplyBackpressure) {
    PlayerDriver driver;
    PlaybackEngine engine(driver, "child", "p", 100000);
    load(engine, driver);
    for (unsigned i = 1; i < 32; ++i)
        complete(engine, driver,
                 command("seek" + std::to_string(i), "seek", i, i + 1, ",\"positionMs\":0"),
                 "loaded");
    EXPECT_EQ(engine.submit(command("full", "seek", 32, 33, ",\"positionMs\":0"), 0).admission,
              PlaybackAdmission::Full);
    EXPECT_EQ(engine.submit(command("load", "load", 0, 1), 0).admission, PlaybackAdmission::Replay);
    PlayerDriver queued_driver;
    PlaybackEngine queued(queued_driver, "child", "p", 100000);
    for (unsigned i = 0; i < 4; ++i) {
        const auto json =
            i == 0 ? command("load", "load", 0, 1)
                   : command("seek" + std::to_string(i), "seek", i, i + 1, ",\"positionMs\":0");
        ASSERT_EQ(queued.submit(json, 0).admission, PlaybackAdmission::Accepted);
        queued.poll(0);
        queued_driver.report("loaded");
        queued.poll(0);
    }
    EXPECT_EQ(queued.submit(command("blocked", "play", 4, 5), 0).admission,
              PlaybackAdmission::Busy);
    EXPECT_EQ(queued_driver.begins, 4U);
    EXPECT_TRUE(queued.take_reply());
    EXPECT_EQ(queued.submit(command("blocked", "play", 4, 5), 0).admission,
              PlaybackAdmission::Accepted);
}
TEST(PlaybackEngine, BoundedFactDrainAndDestruction) {
    PlayerDriver driver;
    {
        PlaybackEngine engine(driver, "child", "p", 100000);
        load(engine, driver);
        for (unsigned i = 0; i < 10; ++i)
            driver.report("loaded", false, 0, 1, 0);
        engine.poll(1);
        EXPECT_EQ(driver.facts.size(), 6U);
        engine.poll(1);
        EXPECT_EQ(driver.facts.size(), 2U);
    }
    EXPECT_EQ(driver.stops, 1U);
    PlayerDriver invalid_driver;
    PlaybackEngine invalid(invalid_driver, "", "p", 1, 0);
    EXPECT_TRUE(invalid.closed());
}

// 输出背压不能吃掉自然结束事件；保留在驱动有界队列，平台消费后继续收敛。
TEST(PlaybackEngine, NaturalEventWaitsForReplyCapacity) {
    PlayerDriver driver;
    PlaybackEngine engine(driver, "child", "p", 100000);
    for (unsigned i = 0; i < 4; ++i) {
        const auto json = i == 0 ? command("load", "load", 0, 1)
                                 : command("play" + std::to_string(i), "play", i, i + 1);
        ASSERT_EQ(engine.submit(json, 0).admission, PlaybackAdmission::Accepted);
        engine.poll(0);
        driver.report(i == 0 ? "loaded" : "playing");
        engine.poll(0);
    }
    driver.report("ended", true, 30000);
    engine.poll(1);
    EXPECT_EQ(driver.facts.size(), 1U);
    EXPECT_FALSE(engine.closed());
    EXPECT_EQ(engine.state(), "playing");
    ASSERT_TRUE(engine.take_reply());
    engine.poll(1);
    EXPECT_TRUE(driver.facts.empty());
    EXPECT_EQ(engine.state(), "ended");
    EXPECT_EQ(engine.revision(), 5U);
    for (unsigned i = 0; i < 3; ++i)
        ASSERT_TRUE(engine.take_reply());
    auto ended = engine.take_reply();
    ASSERT_TRUE(ended);
    EXPECT_NE(ended->find("ended"), std::string::npos);
}

// 长seek原token只确认实际seeking，完成必须是同代次独立状态事件，不能假报已到目标。
TEST(PlaybackEngine, AsyncSeekUsesActualProgressAndIndependentCompletion) {
    for (const auto* original : {"loaded", "playing", "paused"}) {
        PlayerDriver driver;
        PlaybackEngine engine(driver, "child", "p", 100000);
        load(engine, driver);
        if (std::string(original) != "loaded")
            complete(engine, driver, command("play", "play", 1, 2), "playing");
        if (std::string(original) == "paused")
            complete(engine, driver, command("pause", "pause", 2, 3), "paused");
        const auto revision = engine.revision();
        const auto json = command("seek", "seek", revision, revision + 1, ",\"positionMs\":2000");
        const auto progress = complete(engine, driver, json, "seeking", 1, 1, 100);
        EXPECT_EQ(engine.state(), "seeking");
        EXPECT_EQ(engine.submit(command("busy", "play", revision + 1, revision + 2), 1).admission,
                  PlaybackAdmission::StateConflict);
        EXPECT_EQ(engine.submit(json, 1).reply, progress);
        driver.report(original, true, 1980);
        engine.poll(2);
        auto result = engine.take_reply();
        ASSERT_TRUE(result);
        Envelope envelope;
        ASSERT_EQ(decode_envelope(*result, envelope), DecodeResult::Ok);
        EXPECT_FALSE(envelope.code);
        EXPECT_NE(envelope.token, "seek");
        EXPECT_NE(envelope.payload.find("1980"), std::string::npos);
        EXPECT_EQ(engine.state(), original);
        EXPECT_EQ(engine.revision(), revision + 2);
        EXPECT_FALSE(engine.closed());
    }
}
// seeking不能因为原token已回200而无限等待；超时失败不能由迟到完成改回playing。
TEST(PlaybackEngine, AsyncSeekTimeoutStopAndOutputRevocation) {
    for (unsigned variant = 0; variant < 3; ++variant) {
        PlayerDriver driver;
        PlaybackEngine engine(driver, "child", "p", 100000, 10);
        play(engine, driver);
        complete(engine, driver, command("seek", "seek", 2, 3, ",\"positionMs\":5000"), "seeking");
        if (variant == 0)
            engine.poll(10);
        if (variant == 1) {
            driver.permitted = false;
            engine.poll(1);
        }
        if (variant == 2)
            complete(engine, driver, command("stop", "stop", 3, 4), "stopped", 1);
        if (variant != 2) {
            EXPECT_TRUE(engine.closed());
            auto failure = engine.take_reply();
            ASSERT_TRUE(failure);
            EXPECT_NE(failure->find(variant == 0 ? "resultUnknown" : "resourceLost"),
                      std::string::npos);
            EXPECT_EQ(engine.state(), "failed");
        }
        driver.report("playing", true, 4990);
        engine.poll(11);
        EXPECT_EQ(engine.state(), variant == 2 ? "stopped" : "failed");
        EXPECT_FALSE(engine.take_reply());
    }
}
// 即使4条回执占满，关闭事实仍有限保留；排空时只补一次，不丢原成功也不重新启动。
TEST(PlaybackEngine, ClosingWithBackpressureRetainsUniqueFailure) {
    PlayerDriver driver;
    PlaybackEngine engine(driver, "child", "p", 100000);
    for (unsigned i = 0; i < 4; ++i) {
        const auto json = i == 0 ? command("load", "load", 0, 1)
                                 : command("play" + std::to_string(i), "play", i, i + 1);
        ASSERT_EQ(engine.submit(json, 0).admission, PlaybackAdmission::Accepted);
        engine.poll(0);
        driver.report(i == 0 ? "loaded" : "playing");
        engine.poll(0);
    }
    engine.close();
    engine.close();
    EXPECT_TRUE(engine.closed());
    EXPECT_EQ(driver.stops, 1U);
    for (unsigned i = 0; i < 4; ++i) {
        const auto old = engine.take_reply();
        ASSERT_TRUE(old);
        EXPECT_NE(old->find("\"code\":200"), std::string::npos);
    }
    const auto failure = engine.take_reply();
    ASSERT_TRUE(failure);
    Envelope envelope;
    ASSERT_EQ(decode_envelope(*failure, envelope), DecodeResult::Ok);
    EXPECT_FALSE(envelope.code);
    EXPECT_EQ(envelope.payload.find("positionMs"), std::string::npos);
    EXPECT_NE(envelope.payload.find("resultUnknown"), std::string::npos);
    EXPECT_EQ(engine.state(), "failed");
    EXPECT_EQ(engine.revision(), 5U);
    EXPECT_FALSE(engine.take_reply());
}
// 在途关闭保留原失败回执；没有加载事实时不伪造播放器位置或已播放事件。
TEST(PlaybackEngine, ClosePendingAndDeadlinePreserveOriginalFailure) {
    for (bool expiry : {false, true}) {
        PlayerDriver driver;
        PlaybackEngine engine(driver, "child", "p", 10);
        const auto json = command("load", "load", 0, 1);
        ASSERT_EQ(engine.submit(json, 0).admission, PlaybackAdmission::Accepted);
        engine.poll(0);
        if (expiry)
            engine.poll(10);
        else
            engine.close();
        const auto failure = engine.take_reply();
        ASSERT_TRUE(failure);
        EXPECT_NE(failure->find(expiry ? "sessionExpired" : "resultUnknown"), std::string::npos);
        EXPECT_EQ(engine.submit(json, 10).reply, *failure);
        driver.report("loaded");
        engine.poll(10);
        EXPECT_FALSE(engine.take_reply());
        EXPECT_EQ(driver.begins, 1U);
        EXPECT_EQ(driver.stops, 1U);
    }
}

// 低内存不能令已播放媒体泄露，也不能重执行已产生未知结果的原控制。
TEST(PlaybackEngine, AllocationFailureStopsOutputAndRetainsUnknownResult) {
    PlayerDriver driver;
    PlaybackEngine engine(driver, "child", "p", 100000);
    play(engine, driver);
    const auto json = command("pause", "pause", 2, 3);
    ASSERT_EQ(engine.submit(json, 0).admission, PlaybackAdmission::Accepted);
    engine.poll(0);
    driver.report("paused");
    driver.facts.back().code = 500;
    {
        JsonAllocationFailure allocation_failure;  // 原失败回执及终态编码均暂时不可分配。
        engine.poll(1);
        EXPECT_TRUE(engine.closed());
        EXPECT_GE(driver.stops, 1U);
        EXPECT_FALSE(engine.take_reply());
    }
    auto failure = engine.take_reply();  // 内存恢复后补交一次独立失败事实。
    ASSERT_TRUE(failure);
    EXPECT_NE(failure->find("deviceRejected"), std::string::npos);
    EXPECT_EQ(engine.submit(json, 2).admission, PlaybackAdmission::Pending);
    EXPECT_EQ(driver.begins, 3U);
    EXPECT_FALSE(engine.take_reply());
    EXPECT_EQ(engine.submit(command("new", "play", 3, 4), 2).admission, PlaybackAdmission::Closed);
}

// 实际位置与时长也必须安全整数精确往返，不能在构造data时被cJSON先近似再规范化。
TEST(PlaybackEngine, ActualSafeIntegerPositionIsNotRounded) {
    PlayerDriver driver;
    PlaybackEngine engine(driver, "child", "p", 100000);
    ASSERT_EQ(engine.submit(command("load", "load", 0, 1), 0).admission,
              PlaybackAdmission::Accepted);
    engine.poll(0);
    driver.report("loaded", false, 9007199254740991ULL);
    driver.facts.back().duration_ms = 9007199254740991ULL;
    engine.poll(0);
    const auto reply = engine.take_reply();
    ASSERT_TRUE(reply);
    Envelope envelope;
    ASSERT_EQ(decode_envelope(*reply, envelope), DecodeResult::Ok);
    EXPECT_NE(envelope.payload.find("\"positionMs\":9007199254740991"), std::string::npos);
    EXPECT_NE(envelope.payload.find("\"durationMs\":9007199254740991"), std::string::npos);
}
}  // namespace ur::device
