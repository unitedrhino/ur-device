// 文件说明：共享连接文件分派的准入、身份、实例保留及实际释放回归；不冒充真实MQTT E2E。
#include "ur/device/playback_endpoint.hpp"
#include <gtest/gtest.h>
#include <limits>

namespace ur::device {
namespace {
// EndpointMqtt 捕获规范化原发布；不持身份密钥，不把受理当远端执行。
struct EndpointMqtt final : PublishPort {
    std::vector<std::string> sent;  // 按顺序记录原请求/事实。
    bool writable = true;           // 独立注入本地发布背压，不伪造远端接收。
    bool publish(std::string_view topic, std::string_view body) override {
        EXPECT_EQ(topic, "up");
        sent.emplace_back(body);
        return writable;
    }
};
// EndpointDriver 释放由用例独立确认，构造和准入不会启动媒体。
struct EndpointDriver final : PlaybackDriver {
    bool released = false;  // 与逻辑关闭分离的实际释放。
    unsigned starts = 0;    // 验证未建立时不执行媒体。
    unsigned stops = 0;     // 验证关闭仅影响本驱动。
    bool begin(const PlaybackCommand&, std::uint64_t) override {
        ++starts;
        return true;
    }
    std::optional<PlaybackFact> take_fact() override { return {}; }
    bool output_permitted() const override { return true; }
    bool stop_all() override {
        ++stops;
        return released;
    }
};
// EndpointAdmission 明确注入可信准入结果；空授权必须拒绝，不能默认使用邀请期限。
struct EndpointAdmission final : PlaybackAdmissionPort {
    unsigned calls = 0;                // 严格解析/幂等之后的准入调用次数。
    bool permitted = true;             // 用例显式拒绝内容授权。
    EndpointDriver* latest = nullptr;  // 非拥有指针，仅在端点持有驱动时检查。
    PlaybackGrant authorize(const PlaybackInvite&, std::uint64_t now) override {
        ++calls;
        if (!permitted)
            return {};
        auto driver = std::make_unique<EndpointDriver>();
        latest = driver.get();
        return {std::move(driver), now + 1000};
    }
    // cancel 本夹具没有未交付准备资源；已交付驱动由核心独立stop_all确认。
    bool cancel(std::string_view, std::uint64_t) override { return true; }
};

// PreparingAdmission 明确保留一个异步准备；就绪与实际关闭由用例注入，不默认成功。
struct PreparingAdmission final : PlaybackAdmissionPort {
    bool ready = false, released = false;  // 准备及取消实际事实分离。
    std::uint64_t deadline = 20000;        // 固定独立许可，不取原邀请截止。
    unsigned calls = 0, cancels = 0;       // 重复邀请不能触发二次准备。
    PlaybackGrant authorize(const PlaybackInvite&, std::uint64_t) override {
        ++calls;
        if (!ready)
            return {nullptr, deadline, true};
        return {std::make_unique<EndpointDriver>(), deadline};
    }
    bool cancel(std::string_view, std::uint64_t) override {
        ++cancels;
        return released;
    }
};
// invitation 每次保留完整契约，仅改变独立子会话ID用于隔离回归。
std::string invitation(std::string_view session) {
    return std::string(R"({"msgToken":"invite","method":"sessionInvite","sessionId":")") +
           std::string(session) +
           R"(","eventSeq":1,"params":{"purpose":"mediaPlayback","admission":"direct","transport":"none","tracks":[],"expiresAt":10000,"playbackId":"player","mediaSource":{"contentRef":"content"},"inputModalities":[],"outputModalities":["audio"]}})";
}
}  // namespace

// 原邀请先保留而不接听；就绪后只接一次，独立许可不被邀请截止缩短为播放器寿命。
TEST(PlaybackEndpoint, PreparationBeforeAcceptance) {
    EndpointMqtt mqtt;
    PreparingAdmission admission;
    PlaybackEndpoint endpoint(mqtt, admission, {1, true, false}, "up", "down");
    ASSERT_TRUE(endpoint.receive("down", invitation("child"), 1, 1));
    EXPECT_TRUE(mqtt.sent.empty());
    ASSERT_TRUE(endpoint.receive("down", invitation("child"), 2, 2));
    EXPECT_EQ(admission.calls, 1U);
    ASSERT_TRUE(endpoint.receive("down", invitation("other"), 3, 3));
    ASSERT_EQ(mqtt.sent.size(), 1U);
    EXPECT_NE(mqtt.sent.back().find("resourceBusy"), std::string::npos);
    admission.ready = true;
    endpoint.tick(4, 4);
    ASSERT_EQ(mqtt.sent.size(), 2U);
    EXPECT_NE(mqtt.sent.back().find("sessionAccept"), std::string::npos);
    endpoint.tick(5, 5);
    EXPECT_EQ(admission.calls, 2U);
    EXPECT_EQ(mqtt.sent.size(), 2U);
}

// 原邀请超时与异步关闭不同，未实际释放仍阻止第二实例；拒绝背压复用原文且不重授权。
TEST(PlaybackEndpoint, PreparationTimeoutKeepsResourceFence) {
    EndpointMqtt mqtt;
    PreparingAdmission admission;
    PlaybackEndpoint endpoint(mqtt, admission, {1, true, false}, "up", "down");
    ASSERT_TRUE(endpoint.receive("down", invitation("child"), 1, 1));
    mqtt.writable = false;
    endpoint.tick(10000, 10000);
    ASSERT_FALSE(mqtt.sent.empty());
    const auto rejection = mqtt.sent.back();
    EXPECT_NE(rejection.find("sessionReject"), std::string::npos);
    mqtt.writable = true;
    endpoint.tick(10001, 10001);
    EXPECT_EQ(mqtt.sent.back(), rejection);
    ASSERT_TRUE(endpoint.receive("down", invitation("other"), 10002, 1));
    EXPECT_NE(mqtt.sent.back().find("resourceBusy"), std::string::npos);
    EXPECT_EQ(admission.calls, 1U);
    admission.released = true;
    endpoint.tick(10003, 2);
    ASSERT_TRUE(endpoint.receive("down", invitation("third"), 10004, 3));
    EXPECT_EQ(admission.calls, 2U);
}

// 轮询不得续独立授权，关闭/时钟回退不得重新接听，准备截止换算禁止uint64溢出。
TEST(PlaybackEndpoint, PreparationCannotRefreshOrSurviveClose) {
    for (unsigned scenario = 0; scenario < 4; ++scenario) {
        EndpointMqtt mqtt;
        PreparingAdmission admission;
        PlaybackEndpoint endpoint(mqtt, admission, {1, true, false}, "up", "down");
        if (scenario == 3) {
            admission.deadline = std::numeric_limits<std::uint64_t>::max();
            ASSERT_TRUE(endpoint.receive("down", invitation("child"), admission.deadline - 1, 1));
            EXPECT_NE(mqtt.sent.back().find("sessionReject"), std::string::npos);
            continue;
        }
        ASSERT_TRUE(endpoint.receive("down", invitation("child"), 2, 2));
        if (scenario == 0) {
            admission.deadline++;
            admission.ready = true;
            endpoint.tick(3, 3);
            EXPECT_NE(mqtt.sent.back().find("sessionReject"), std::string::npos);
        } else {
            if (scenario == 1)
                endpoint.close(3);
            else
                endpoint.tick(1, 1);
            admission.ready = true;
            endpoint.tick(4, 4);
            EXPECT_TRUE(mqtt.sent.empty());
            EXPECT_FALSE(endpoint.receive("down", invitation("child"), 5, 5));
        }
        if (scenario != 0) {
            EXPECT_GT(admission.cancels,
                      0U);  // 无效已交付驱动由核心直接请求stop_all，不借准入器假报释放。
        }
    }
}

// 准备期间平台终态必须取消原资源；迟到就绪和重复终态不能改写原释放关联。
TEST(PlaybackEndpoint, TerminalDuringPreparationWaitsForActualRelease) {
    EndpointMqtt mqtt;
    PreparingAdmission admission;
    PlaybackEndpoint endpoint(mqtt, admission, {1, true, false}, "up", "down");
    ASSERT_TRUE(endpoint.receive("down", invitation("child"), 1, 1));
    const std::string terminal =
        R"({"msgToken":"terminal","method":"sessionEnded","sessionId":"child","eventSeq":2,"data":{"state":"ended","reason":"parentEnded"}})";
    EXPECT_FALSE(endpoint.receive(
        "down",
        R"({"msgToken":"bad","method":"sessionEnded","sessionId":"child","eventSeq":1,"data":{"state":"ended","reason":"parentEnded"}})",
        2, 2));
    EXPECT_EQ(admission.cancels, 0U);
    EXPECT_FALSE(endpoint.receive(
        "down",
        R"({"msgToken":"bad","method":"sessionEnded","sessionId":"child","code":200,"data":{"state":"ended","reason":"parentEnded"}})",
        2, 2));
    EXPECT_FALSE(endpoint.receive(
        "down",
        R"({"msgToken":"bad","method":"sessionEnded","sessionId":"child","eventSeq":2,"data":{"state":"playing","reason":"parentEnded"}})",
        2, 2));
    ASSERT_TRUE(endpoint.receive("down", terminal, 3, 3));
    EXPECT_GT(admission.cancels, 0U);
    EXPECT_TRUE(mqtt.sent.empty());
    admission.ready = true;
    endpoint.tick(4, 4);
    EXPECT_EQ(admission.calls, 1U);
    EXPECT_FALSE(endpoint.receive("down", terminal, 5, 5));
    EXPECT_FALSE(endpoint.receive(
        "down",
        R"({"msgToken":"replacement","method":"sessionEnded","sessionId":"child","eventSeq":3,"data":{"state":"ended","reason":"parentEnded"}})",
        5, 5));
    EXPECT_FALSE(endpoint.receive("down", invitation("child"), 6, 6));
    admission.released = true;
    mqtt.writable = false;
    endpoint.tick(7, 7);
    ASSERT_EQ(mqtt.sent.size(), 1U);
    const auto fact = mqtt.sent.back();
    Envelope release;
    ASSERT_EQ(decode_envelope(fact, release), DecodeResult::Ok);
    EXPECT_EQ(release.method, "playbackState");
    EXPECT_EQ(release.token, "terminal");
    EXPECT_EQ(release.session, "child");
    EXPECT_EQ(release.payload, R"({"endEventSeq":2,"playbackId":"player","released":true})");
    mqtt.writable = true;
    endpoint.tick(8, 8);
    ASSERT_EQ(mqtt.sent.size(), 2U);
    EXPECT_EQ(mqtt.sent.back(), fact);
    endpoint.tick(9, 9);
    EXPECT_EQ(mqtt.sent.size(), 2U);
}

// 本地准备超时先拒绝不丢平台终态；实际关闭后仍按原平台关联回报，不重授权或恢复。
TEST(PlaybackEndpoint, LateTerminalAfterPreparationTimeoutUsesOriginalRelease) {
    EndpointMqtt mqtt;
    PreparingAdmission admission;
    PlaybackEndpoint endpoint(mqtt, admission, {1, true, false}, "up", "down");
    ASSERT_TRUE(endpoint.receive("down", invitation("child"), 1, 1));
    admission.released = true;
    endpoint.tick(10000, 10000);
    ASSERT_EQ(mqtt.sent.size(), 1U);
    ASSERT_TRUE(endpoint.receive(
        "down",
        R"({"msgToken":"expired","method":"sessionEnded","sessionId":"child","eventSeq":2,"data":{"state":"ended","reason":"inviteTimeout"}})",
        10001, 10001));
    ASSERT_EQ(mqtt.sent.size(), 2U);
    Envelope release;
    ASSERT_EQ(decode_envelope(mqtt.sent.back(), release), DecodeResult::Ok);
    EXPECT_EQ(release.token, "expired");
    EXPECT_EQ(release.payload, R"({"endEventSeq":2,"playbackId":"player","released":true})");
    EXPECT_EQ(admission.calls, 1U);
    EXPECT_FALSE(endpoint.receive("down", invitation("child"), 10002, 1));
}

// 非文件/错Topic/过期/非法输出不调用可信内容解析；重复不替换原固定实例。
// 新连接只在旧整组资源实际释放后开始；保留近期关联，不复活旧准备或刷新其许可。
TEST(PlaybackEndpoint, NewConnectionKeepsOldInstanceFence) {
    EndpointMqtt mqtt;
    PreparingAdmission admission;
    PlaybackEndpoint endpoint(mqtt, admission, {1, true, false}, "up", "down");
    EXPECT_FALSE(endpoint.begin_connection(1));  // 尚未关闭的代次不能被偷偷重置。
    ASSERT_TRUE(endpoint.receive("down", invitation("child"), 2, 2));
    EXPECT_TRUE(endpoint.owns("child"));
    EXPECT_FALSE(endpoint.owns(""));
    EXPECT_FALSE(endpoint.owns("ai-parent"));
    EXPECT_FALSE(endpoint.released());
    endpoint.close(3);
    EXPECT_FALSE(endpoint.begin_connection(4));
    admission.released = true;
    endpoint.tick(5, 5);
    EXPECT_TRUE(endpoint.released());
    EXPECT_FALSE(endpoint.begin_connection(4));  // 时钟回退仍拒绝新连接。
    ASSERT_TRUE(endpoint.begin_connection(6));
    EXPECT_TRUE(endpoint.owns("child"));
    EXPECT_FALSE(endpoint.receive("down", invitation("child"), 7, 7));
    EXPECT_EQ(admission.calls, 1U);
    ASSERT_TRUE(endpoint.receive("down", invitation("next"), 8, 8));
    EXPECT_EQ(admission.calls, 2U);
    EXPECT_FALSE(endpoint.released());
}

// 非文件/错Topic/过期/非法输出不调用可信内容解析；重复不替换原固定实例。
TEST(PlaybackEndpoint, StrictAdmissionAndDuplicate) {
    EndpointMqtt mqtt;
    EndpointAdmission admission;
    PlaybackEndpoint endpoint(mqtt, admission, {1, true, false}, "up", "down");
    EXPECT_FALSE(endpoint.receive("other", invitation("child"), 1, 1));
    EXPECT_FALSE(endpoint.receive("down", "{}", 1, 1));
    EXPECT_FALSE(endpoint.receive("down", invitation("child"), 1, 10000));
    EXPECT_EQ(admission.calls, 0U);
    ASSERT_TRUE(endpoint.receive("down", invitation("child"), 2, 2));
    ASSERT_TRUE(admission.latest);
    EXPECT_EQ(admission.calls, 1U);
    EXPECT_EQ(admission.latest->starts, 0U);
    EXPECT_EQ(mqtt.sent.size(), 1U);
    EXPECT_TRUE(endpoint.receive("down", invitation("child"), 3, 3));
    EXPECT_EQ(admission.calls, 1U);
    EXPECT_EQ(mqtt.sent.size(), 1U);
    EXPECT_FALSE(endpoint.receive(
        "down",
        R"({"msgToken":"unknown","method":"sessionEnded","sessionId":"ai-parent","eventSeq":3,"data":{"state":"ended","reason":"parentEnded"}})",
        4, 4));
    EXPECT_EQ(admission.latest->stops, 0U);
}

// 本地结束尚未实际释放时第二播放器拒绝；拒绝后不能因资源变空重准入原会话。
TEST(PlaybackEndpoint, LogicalEndIsNotReusableOutput) {
    EndpointMqtt mqtt;
    EndpointAdmission admission;
    PlaybackEndpoint endpoint(mqtt, admission, {1, true, false}, "up", "down");
    ASSERT_TRUE(endpoint.receive("down", invitation("first"), 1, 1));
    auto* first = admission.latest;
    endpoint.tick(1001, 1001);  // 独立内容准入截止到期，本地逻辑结束。
    ASSERT_TRUE(endpoint.receive("down", invitation("second"), 1002, 1002));
    EXPECT_EQ(admission.calls, 1U);
    EXPECT_NE(mqtt.sent.back().find("resourceBusy"), std::string::npos);
    const auto rejection = mqtt.sent.back();
    first->released = true;
    endpoint.tick(1003, 1003);
    EXPECT_TRUE(endpoint.receive("down", invitation("second"), 1004, 1004));
    EXPECT_EQ(mqtt.sent.back(), rejection);
    EXPECT_EQ(admission.calls, 1U);
    ASSERT_TRUE(endpoint.receive("down", invitation("third"), 1005, 1005));
    EXPECT_EQ(admission.calls, 2U);
    EXPECT_NE(admission.latest, first);
}

// 空授权拒绝并保留原字节；满槽只背压，关闭与时钟回退不允许新的准入。
TEST(PlaybackEndpoint, DenialCapacityAndClosure) {
    EndpointMqtt mqtt;
    EndpointAdmission admission;
    admission.permitted = false;
    PlaybackEndpoint endpoint(mqtt, admission, {1, true, false}, "up", "down");
    for (unsigned i = 0; i < 4; ++i)
        ASSERT_TRUE(endpoint.receive("down", invitation(std::to_string(i)), 1, 1));
    EXPECT_EQ(admission.calls, 4U);
    EXPECT_FALSE(endpoint.receive("down", invitation("overflow"), 1, 1));
    EXPECT_EQ(mqtt.sent.size(), 4U);
    admission.permitted = true;
    EXPECT_TRUE(endpoint.receive("down", invitation("0"), 2, 2));
    EXPECT_EQ(admission.calls, 4U);
    EXPECT_NE(mqtt.sent.back().find("userRejected"), std::string::npos);
    endpoint.tick(1, 1);  // 时钟回退只收敛关闭。
    endpoint.tick(600002, 600002);
    EXPECT_FALSE(endpoint.receive("down", invitation("new"), 600003, 1));
    EXPECT_EQ(admission.calls, 4U);
}

// 原建立/结束和实际释放沿用会话核心；身份冲突不重授权，结束事实不能串到AI父会话。
TEST(PlaybackEndpoint, SessionRoutingAndActualRelease) {
    EndpointMqtt mqtt;
    EndpointAdmission admission;
    PlaybackEndpoint endpoint(mqtt, admission, {1, true, false}, "up", "down");
    EXPECT_FALSE(endpoint.established("child"));
    ASSERT_TRUE(endpoint.receive("down", invitation("child"), 1, 1));
    Envelope conflicting;
    ASSERT_EQ(decode_envelope(invitation("child"), conflicting), DecodeResult::Ok);
    conflicting.token = "replacement";
    std::string body;
    ASSERT_EQ(encode_envelope(conflicting, body), DecodeResult::Ok);
    EXPECT_FALSE(endpoint.receive("down", body, 2, 2));
    EXPECT_EQ(admission.calls, 1U);
    ASSERT_TRUE(endpoint.receive(
        "down",
        R"({"msgToken":"invite","method":"requestAck","sessionId":"child","code":200,"data":{"requestMethod":"sessionAccept","result":{"state":"established"}}})",
        3, 3));
    EXPECT_FALSE(endpoint.established("child"));
    ASSERT_TRUE(endpoint.receive(
        "down",
        R"({"msgToken":"established","method":"sessionEstablished","sessionId":"child","eventSeq":2,"data":{"state":"established","mediaRevision":0}})",
        4, 4));
    EXPECT_TRUE(endpoint.established("child"));
    EXPECT_FALSE(endpoint.established("ai-parent"));
    ASSERT_TRUE(endpoint.receive(
        "down",
        R"({"msgToken":"terminal","method":"sessionEnded","sessionId":"child","eventSeq":3,"data":{"state":"ended","reason":"parentEnded"}})",
        5, 5));
    EXPECT_FALSE(endpoint.established("child"));
    endpoint.tick(6, 6);
    EXPECT_EQ(mqtt.sent.size(), 1U);
    admission.latest->released = true;
    endpoint.tick(7, 7);
    ASSERT_EQ(mqtt.sent.size(), 2U);
    Envelope release;
    ASSERT_EQ(decode_envelope(mqtt.sent.back(), release), DecodeResult::Ok);
    EXPECT_EQ(release.session, "child");
    EXPECT_EQ(release.token, "terminal");
    EXPECT_EQ(release.method, "playbackState");
    endpoint.close(8);
    EXPECT_FALSE(endpoint.established("child"));
    EXPECT_EQ(release.payload, R"({"endEventSeq":3,"playbackId":"player","released":true})");
    endpoint.tick(8, 8);
    EXPECT_EQ(mqtt.sent.size(), 2U);
}

// 已交付驱动的释放事实发布背压超过保留窗口，仍不能回收原关联或假报settled。
TEST(PlaybackEndpoint, SubmittedReleaseRequiredBeforeRetentionCleanup) {
    EndpointMqtt mqtt;
    EndpointAdmission admission;
    PlaybackEndpoint endpoint(mqtt, admission, {1, true, false}, "up", "down");
    ASSERT_TRUE(endpoint.receive("down", invitation("child"), 1, 1));
    EXPECT_FALSE(endpoint.settled());
    ASSERT_TRUE(endpoint.receive(
        "down",
        R"({"msgToken":"terminal","method":"sessionEnded","sessionId":"child","eventSeq":2,"data":{"state":"ended","reason":"parentEnded"}})",
        2, 2));
    admission.latest->released = true;
    mqtt.writable = false;
    endpoint.tick(3, 3);
    const auto fact = mqtt.sent.back();
    EXPECT_TRUE(endpoint.released());
    EXPECT_FALSE(endpoint.settled());
    endpoint.tick(600004, 4);
    EXPECT_TRUE(endpoint.owns("child"));
    EXPECT_FALSE(endpoint.settled());
    EXPECT_EQ(mqtt.sent.back(), fact);
    mqtt.writable = true;
    endpoint.tick(600005, 5);
    EXPECT_EQ(mqtt.sent.back(), fact);
    EXPECT_TRUE(endpoint.settled());
    EXPECT_FALSE(endpoint.owns("child"));
}

// 收报时的时钟回退也立即关闭当前实例，而非等下一次正常tick才停止输出。
TEST(PlaybackEndpoint, ReceiveClockRollbackClosesDriver) {
    EndpointMqtt mqtt;
    EndpointAdmission admission;
    PlaybackEndpoint endpoint(mqtt, admission, {1, true, false}, "up", "down");
    ASSERT_TRUE(endpoint.receive("down", invitation("child"), 10, 10));
    EXPECT_FALSE(endpoint.receive("down", invitation("other"), 9, 9));
    EXPECT_GT(admission.latest->stops, 0U);
    EXPECT_FALSE(endpoint.receive("down", invitation("other"), 11, 11));
    EXPECT_EQ(admission.calls, 1U);
}

// 平台终态与实际释放均完成后，第五首文件不等待十分钟；墓碑仍挡住旧会话重准入。
TEST(PlaybackEndpoint, FinishedPlayersRecycleWorkingSlotsAndRetainIdentityFence) {
    EndpointMqtt mqtt;
    EndpointAdmission admission;
    PlaybackEndpoint endpoint(mqtt, admission, {1, true, false}, "up", "down");
    for (unsigned index = 0; index < 6; ++index) {
        const auto session = "finished-" + std::to_string(index);
        const auto now = 1U + index * 3;
        ASSERT_TRUE(endpoint.receive("down", invitation(session), now, now));
        ASSERT_TRUE(admission.latest);
        admission.latest->released = true;
        const auto end =
            "{\"msgToken\":\"terminal\",\"method\":\"sessionEnded\",\"sessionId\":\"" + session +
            "\",\"eventSeq\":2,\"data\":{\"state\":\"ended\",\"reason\":\"parentEnded\"}}";
        ASSERT_TRUE(endpoint.receive("down", end, now + 1, now + 1));
        endpoint.tick(now + 2, now + 2);
        EXPECT_TRUE(endpoint.released());
        EXPECT_TRUE(endpoint.settled());
    }
    const auto calls = admission.calls;
    for (unsigned index = 0; index < 6; ++index) {
        const auto session = "finished-" + std::to_string(index);
        EXPECT_TRUE(endpoint.owns(session));
        EXPECT_FALSE(endpoint.established(session));
        EXPECT_FALSE(endpoint.receive("down", invitation(session), 30 + index, 30 + index));
    }
    EXPECT_EQ(admission.calls, calls);
    EXPECT_EQ(calls, 6U);
    endpoint.tick(600100, 600100);
    for (unsigned index = 0; index < 6; ++index)
        EXPECT_FALSE(endpoint.owns("finished-" + std::to_string(index)));
}

// 即使四个驱动实际释放，原平台终态释放回报尚被背压也不可删除工作关联。
TEST(PlaybackEndpoint, BlockedTerminalFactsKeepAllWorkingSlots) {
    EndpointMqtt mqtt;
    EndpointAdmission admission;
    PlaybackEndpoint endpoint(mqtt, admission, {1, true, false}, "up", "down");
    for (unsigned index = 0; index < 4; ++index) {
        const auto session = "blocked-" + std::to_string(index);
        const auto now = 1U + index * 3;
        ASSERT_TRUE(endpoint.receive("down", invitation(session), now, now));
        admission.latest->released = true;
        mqtt.writable = false;
        const auto end =
            "{\"msgToken\":\"terminal\",\"method\":\"sessionEnded\",\"sessionId\":\"" + session +
            "\",\"eventSeq\":2,\"data\":{\"state\":\"ended\",\"reason\":\"parentEnded\"}}";
        ASSERT_TRUE(endpoint.receive("down", end, now + 1, now + 1));
        endpoint.tick(now + 2, now + 2);
        mqtt.writable = true;
    }
    EXPECT_TRUE(endpoint.released());
    EXPECT_FALSE(endpoint.settled());
    EXPECT_FALSE(endpoint.receive("down", invitation("fifth"), 20, 20));
    EXPECT_EQ(admission.calls, 4U);
    mqtt.writable = true;
    endpoint.tick(21, 21);
    ASSERT_TRUE(endpoint.settled());
    EXPECT_TRUE(endpoint.receive("down", invitation("fifth"), 22, 22));
    EXPECT_EQ(admission.calls, 5U);
}

// 墓碑预算满只能背压；保留最早原邀请身份，不能用清空历史的方式重新准入。
TEST(PlaybackEndpoint, RetirementBudgetPreservesEarliestSession) {
    EndpointMqtt mqtt;
    EndpointAdmission admission;
    PlaybackEndpoint endpoint(mqtt, admission, {1, true, false}, "up", "down");
    unsigned count = 0;
    for (; count < 200; ++count) {
        const auto session = "budget-" + std::to_string(count);
        const auto now = 1U + count * 3;
        if (!endpoint.receive("down", invitation(session), now, now))
            break;
        admission.latest->released = true;
        const auto end =
            "{\"msgToken\":\"terminal\",\"method\":\"sessionEnded\",\"sessionId\":\"" + session +
            "\",\"eventSeq\":2,\"data\":{\"state\":\"ended\",\"reason\":\"parentEnded\"}}";
        ASSERT_TRUE(endpoint.receive("down", end, now + 1, now + 1));
        endpoint.tick(now + 2, now + 2);
    }
    ASSERT_GT(count, 4U);
    ASSERT_LT(count, 133U);
    const auto calls = admission.calls;
    EXPECT_TRUE(endpoint.owns("budget-0"));
    EXPECT_FALSE(endpoint.receive("down", invitation("budget-0"), 1000, 1000));
    EXPECT_EQ(admission.calls, calls);
}
}  // namespace ur::device
