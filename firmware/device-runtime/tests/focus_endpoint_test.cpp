// 文件说明：焦点MQTT分派组件测试；明确模拟网络/许可/驱动，不冒充真实媒体E2E。
#include "ur/device/focus_endpoint.hpp"
#include <gtest/gtest.h>
#include <deque>

namespace ur::device {
namespace {
// Receipt 仅保存白名单设备事实；没有原token、会话、资源组或任何授权依据。
struct Receipt {
    std::string state, operation;   // 原结果状态和内部关联的原请求操作，历史结果不代表当前授权。
    std::uint64_t revision, index;  // 原实际修订号与本地交付序号，重放不得伪造新修订。
    int code;                       // 原结果码，失败不能被投影为成功。
};
// collect_receipts 将同步只读参数复制为测试事实，回调不能保留临时string_view。
void collect_receipts(FocusEndpoint& endpoint, std::vector<Receipt>& receipts) {
    endpoint.set_receipt_observer([&](std::string_view state, std::uint64_t revision,
                                      std::string_view operation, int code, std::uint64_t index) {
        receipts.push_back({std::string(state), std::string(operation), revision, index, code});
    });
}
// request 构造严格原SIG消息，身份只通过固定Topic选择。
std::string request(std::string session = "one", std::string token = "a",
                    std::string operation = "acquire", unsigned revision = 0, unsigned seq = 1,
                    std::string resources = "\"speaker\",\"display\"") {
    return "{\"msgToken\":\"" + token + "\",\"method\":\"focusControl\",\"sessionId\":\"" +
           session + "\",\"eventSeq\":" + std::to_string(seq) + ",\"params\":{\"operation\":\"" +
           operation + "\",\"expectedRevision\":" + std::to_string(revision) +
           ",\"focusId\":\"focus\",\"resources\":[" + resources +
           "],"
           "\"priority\":\"conversation\",\"resumePolicy\":\"explicit\"" +
           ((operation == "acquire" || operation == "restore") ? ",\"expiresAt\":2000" : "") + "}}";
}
// Network 明确分离本地接纳和失败尝试，不把publish=true视为Broker确认。
struct Network final : PublishPort {
    bool writable = true;                     // 用例控制本地背压。
    std::vector<std::string> attempts, sent;  // 检查失败后仍重试原字节。
    bool publish(std::string_view topic, std::string_view body) override {
        EXPECT_EQ(topic, "up");
        attempts.emplace_back(body);
        if (writable)
            sent.emplace_back(body);
        return writable;
    }
};
// Authority 是显式用例许可，不用于生产默认授权。
struct Authority final : FocusAuthority {
    bool allow = true;   // 用例撤权控制。
    unsigned calls = 0;  // 重放不能重新许可。
    std::optional<FocusPermit> authorize(const FocusCommand& command, std::uint64_t,
                                         std::uint64_t) override {
        ++calls;
        return allow ? std::optional<FocusPermit>(FocusPermit{command, 900, 1}) : std::nullopt;
    }
    bool valid(const FocusPermit&, std::uint64_t, std::uint64_t) const override { return allow; }
};
// Driver 按原会话分离事实，明确实际释放由用例独立确认，不修改麦克风状态。
struct Driver final : FocusDriver {
    unsigned starts = 0;                        // 请求接受不等于资源取得。
    bool release_known = false;                 // 未知关闭不能伪装idle。
    bool held = false;                          // 用例实际整组占用。
    std::uint64_t current = 0, generation = 0;  // 实际水位与操作代次。
    FocusCommand last;                          // 原驱动命令。
    std::deque<FocusFact> facts;                // 只在complete后出现完成事实。
    std::vector<std::string> stops;             // 只允许原作用域停止。
    std::vector<std::string> expected_resources{"speaker",
                                                "display"};  // 原租约实际组，不是能力全集。
    std::string expected_focus = "focus";  // 独立多租约用例显式核关闭目标，不放宽默认原focus。
    std::uint64_t revision() const override { return current; }
    bool begin(const FocusCommand& command, std::uint64_t value) override {
        ++starts;
        last = command;
        generation = value;
        return true;
    }
    std::optional<FocusFact> take_fact(std::string_view session, std::string_view focus) override {
        if (facts.empty() || session != last.envelope.session || focus != last.focus_id)
            return {};
        auto value = facts.front();
        facts.pop_front();
        return value;
    }
    bool owns(const FocusCommand& command) const override {
        return held && command.envelope.session == last.envelope.session;
    }
    bool stop_scope(std::string_view session, std::string_view focus,
                    const std::vector<std::string>& resources) override {
        EXPECT_EQ(focus, expected_focus);
        EXPECT_EQ(resources, expected_resources);
        stops.emplace_back(session);
        if (release_known)
            held = false;
        return release_known;
    }
    // complete 仅模拟实际整组完成，不由begin自动确认。
    void complete() {
        current = last.expected_revision + 1;
        held = last.operation == "acquire" || last.operation == "restore";
        const auto state = last.operation == "acquire"   ? "acquired"
                           : last.operation == "suspend" ? "suspended"
                           : last.operation == "restore" ? "restored"
                                                         : "released";
        facts.push_back({generation, true, state, last.resources, current});
    }
};
}  // namespace

// 同会话旧Closed槽位先返回false、新租约成功时，不得把旧选择分支当当前拒绝首因。
TEST(FocusEndpointScope, LaterSuccessfulSlotDiscardsEarlierClosedSelection) {
    Network mqtt;
    Driver driver;
    Authority auth;
    driver.release_known = true;
    FocusEndpoint endpoint(mqtt, driver, auth, driver.expected_resources, "up", "down", 1000);
    endpoint.enable_scope_diagnostics();
    ASSERT_EQ(endpoint.receive("down", request(), 10, 1000), FocusEndpointResult::Handled);
    endpoint.poll(10, 1000);
    driver.complete();
    endpoint.poll(11, 1001);
    ASSERT_EQ(endpoint.receive("down", request("one", "release", "release", 1, 2), 12, 1002),
              FocusEndpointResult::Handled);
    endpoint.poll(12, 1002);
    driver.complete();
    endpoint.poll(13, 1003);
    auto fresh = request("one", "next", "acquire", 2, 1);
    const std::string original = "\"focusId\":\"focus\"";
    const auto position = fresh.find(original);
    ASSERT_NE(position, std::string::npos);
    fresh.replace(position, original.size(), "\"focusId\":\"focus-next\"");
    driver.expected_focus = "focus-next";
    ASSERT_EQ(endpoint.receive("down", fresh, 14, 1004), FocusEndpointResult::Handled);
    endpoint.poll(14, 1004);
    driver.complete();
    endpoint.poll(15, 1005);
    ScopeFailure observed;
    EXPECT_TRUE(endpoint.scoped_permitted("one", driver.expected_resources, 16, 1006, &observed));
    EXPECT_EQ(observed.code, ScopeFailureCode::None);
}

// ScopedPermitRequiresActualCompleteGroupAndPublishedReceipt
// 候选端只能读真实引擎许可，受理、半组、背压、撤权或过期均不能打开采播。
TEST(FocusEndpointScope, ScopedPermitRequiresActualCompleteGroupAndPublishedReceipt) {
    Network mqtt;
    Driver driver;
    Authority auth;
    driver.expected_resources = {"microphone", "speaker"};
    FocusEndpoint endpoint(mqtt, driver, auth, driver.expected_resources, "up", "down", 1000);
    const auto allowed = [&](std::uint64_t now = 14, std::uint64_t wall = 1004) {
        return endpoint.scoped_permitted("one", driver.expected_resources, now, wall);
    };
    EXPECT_FALSE(allowed());
    ASSERT_EQ(
        endpoint.receive("down", request("one", "a", "acquire", 0, 1, "\"microphone\",\"speaker\""),
                         10, 1000),
        FocusEndpointResult::Handled);
    endpoint.poll(11, 1001);
    EXPECT_FALSE(allowed());
    driver.complete();
    mqtt.writable = false;
    endpoint.poll(12, 1002);
    EXPECT_FALSE(allowed());
    mqtt.writable = true;
    endpoint.poll(13, 1003);
    EXPECT_TRUE(allowed());
    EXPECT_FALSE(endpoint.scoped_permitted("other", driver.expected_resources, 14, 1004));
    EXPECT_FALSE(endpoint.scoped_permitted("one", {"speaker"}, 14, 1004));
    EXPECT_FALSE(endpoint.scoped_permitted("one", {"microphone", "display"}, 14, 1004));
    EXPECT_FALSE(allowed(12, 1004));
    EXPECT_FALSE(allowed(14, 1002));
    EXPECT_FALSE(allowed(900, 1004));
    EXPECT_FALSE(allowed(14, 2000));
    const auto starts = driver.starts;
    const auto stops = driver.stops.size();
    auth.allow = false;
    EXPECT_FALSE(allowed());
    EXPECT_EQ(driver.starts, starts);
    EXPECT_EQ(driver.stops.size(), stops);  // 只读许可查询不偷偷执行释放或重新授权。
    auth.allow = true;
    EXPECT_TRUE(allowed());
    driver.held = false;
    EXPECT_FALSE(allowed());
    endpoint.close();
    EXPECT_FALSE(allowed());
}

// 已交付原取得回执的QoS重放遇背压，不能撤销仍真实持有的原许可；新restore仍须交付新事实。
TEST(FocusEndpointScope, DuplicateReceiptBackpressureDoesNotRevokeDeliveredLease) {
    Network mqtt;
    Driver driver;
    Authority auth;
    FocusEndpoint endpoint(mqtt, driver, auth, driver.expected_resources, "up", "down", 1000);
    const auto acquire = request();
    ASSERT_EQ(endpoint.receive("down", acquire, 10, 1000), FocusEndpointResult::Handled);
    endpoint.poll(11, 1001);
    driver.complete();
    endpoint.poll(12, 1002);
    ASSERT_TRUE(endpoint.scoped_permitted("one", driver.expected_resources, 13, 1003));
    mqtt.writable = false;
    ASSERT_EQ(endpoint.receive("down", acquire, 14, 1004), FocusEndpointResult::Handled);
    endpoint.poll(15, 1005);
    EXPECT_TRUE(endpoint.scoped_permitted("one", driver.expected_resources, 16, 1006));
    EXPECT_EQ(driver.starts, 1U);
    mqtt.writable = true;
    endpoint.poll(17, 1007);
    ASSERT_EQ(endpoint.receive("down", request("one", "s", "suspend", 1, 2), 18, 1008),
              FocusEndpointResult::Handled);
    endpoint.poll(19, 1009);
    driver.complete();
    endpoint.poll(20, 1010);
    ASSERT_EQ(endpoint.receive("down", request("one", "r", "restore", 2, 3), 21, 1011),
              FocusEndpointResult::Handled);
    endpoint.poll(22, 1012);
    driver.complete();
    mqtt.writable = false;
    endpoint.poll(23, 1013);
    EXPECT_FALSE(endpoint.scoped_permitted("one", driver.expected_resources, 24, 1014));
    mqtt.writable = true;
    endpoint.poll(25, 1015);
    EXPECT_TRUE(endpoint.scoped_permitted("one", driver.expected_resources, 26, 1016));
}

// 每个操作只在原真实回执被本地MQTT接纳后投影，重放不改变实际驱动水位或资源许可。
TEST(FocusEndpointReceipt, FourOperationsAndReplayPreserveActualRevision) {
    Network mqtt;
    Driver driver;
    Authority auth;
    std::vector<Receipt> receipts;
    FocusEndpoint endpoint(mqtt, driver, auth, {"speaker", "display"}, "up", "down", 1000);
    collect_receipts(endpoint, receipts);
    const std::vector<std::string> operations{"acquire", "suspend", "restore", "release"};
    const std::vector<std::string> states{"acquired", "suspended", "restored", "released"};
    for (unsigned index = 0; index < operations.size(); ++index) {
        ASSERT_EQ(
            endpoint.receive(
                "down", request("one", std::to_string(index), operations[index], index, index + 1),
                10 + index * 3, 1000 + index * 3),
            FocusEndpointResult::Handled);
        endpoint.poll(11 + index * 3, 1001 + index * 3);
        EXPECT_EQ(receipts.size(), index);
        driver.complete();
        endpoint.poll(12 + index * 3, 1002 + index * 3);
        ASSERT_EQ(receipts.size(), index + 1);
        EXPECT_EQ(receipts.back().state, states[index]);
        EXPECT_EQ(receipts.back().operation, operations[index]);
        EXPECT_EQ(receipts.back().code, 200);
        EXPECT_EQ(receipts.back().revision, index + 1);
        EXPECT_EQ(receipts.back().index, index + 1);
    }
    ASSERT_EQ(endpoint.receive("down", request("one", "0", "acquire", 0, 1), 30, 1030),
              FocusEndpointResult::Handled);
    endpoint.poll(31, 1031);
    ASSERT_EQ(receipts.size(), 5U);
    EXPECT_EQ(receipts.back().operation, "acquire");
    EXPECT_EQ(receipts.back().revision, 1U);
    EXPECT_EQ(receipts.back().index, 5U);
    EXPECT_EQ(driver.current, 4U);
    EXPECT_EQ(driver.starts, 4U);
    EXPECT_FALSE(driver.held);
}

// 实际完成与发布背压分开；失败发送不消费投影序号，也不重复执行原操作。
TEST(FocusEndpointReceipt, BackpressureDoesNotInventAcceptedReceipt) {
    Network mqtt;
    Driver driver;
    Authority auth;
    std::vector<Receipt> receipts;
    FocusEndpoint endpoint(mqtt, driver, auth, {"speaker", "display"}, "up", "down", 1000);
    collect_receipts(endpoint, receipts);
    ASSERT_EQ(endpoint.receive("down", request(), 10, 1000), FocusEndpointResult::Handled);
    endpoint.poll(11, 1001);
    driver.complete();
    mqtt.writable = false;
    endpoint.poll(12, 1002);
    endpoint.poll(13, 1003);
    EXPECT_TRUE(receipts.empty());
    mqtt.writable = true;
    endpoint.poll(14, 1004);
    ASSERT_EQ(receipts.size(), 1U);
    EXPECT_EQ(receipts.back().index, 1U);
    ASSERT_EQ(endpoint.receive("down", request(), 15, 1005), FocusEndpointResult::Handled);
    mqtt.writable = false;
    endpoint.poll(16, 1006);
    EXPECT_EQ(receipts.size(), 1U);
    mqtt.writable = true;
    endpoint.poll(17, 1007);
    ASSERT_EQ(receipts.size(), 2U);
    EXPECT_EQ(receipts.back().index, 2U);
    EXPECT_EQ(receipts.back().revision, 1U);
    EXPECT_EQ(driver.starts, 1U);
}

// 退役墓碑仍关联原操作与实际修订，不能把原取得重放投影成当前重新取得。
TEST(FocusEndpointReceipt, RetiredReplayPreservesOriginalOperationAndResult) {
    Network mqtt;
    Driver driver;
    Authority auth;
    std::vector<Receipt> receipts;
    FocusEndpoint endpoint(mqtt, driver, auth, {"speaker", "display"}, "up", "down", 1000);
    collect_receipts(endpoint, receipts);
    for (unsigned index = 0; index < 6; ++index) {
        const auto session = "lease-" + std::to_string(index);
        const auto now = 10U + index * 6;
        ASSERT_EQ(endpoint.receive("down", request(session, "a", "acquire", index * 2, 1), now,
                                   1000 + now),
                  FocusEndpointResult::Handled);
        endpoint.poll(now + 1, 1001 + now);
        driver.complete();
        endpoint.poll(now + 2, 1002 + now);
        ASSERT_EQ(endpoint.receive("down", request(session, "r", "release", index * 2 + 1, 2),
                                   now + 3, 1003 + now),
                  FocusEndpointResult::Handled);
        endpoint.poll(now + 4, 1004 + now);
        driver.complete();
        endpoint.poll(now + 5, 1005 + now);
    }
    ASSERT_EQ(receipts.size(), 12U);
    ASSERT_EQ(endpoint.receive("down", request("lease-0", "a", "acquire", 0, 1), 60, 1060),
              FocusEndpointResult::Handled);
    endpoint.poll(61, 1061);
    ASSERT_EQ(receipts.size(), 13U);
    EXPECT_EQ(receipts.back().state, "acquired");
    EXPECT_EQ(receipts.back().operation, "acquire");
    EXPECT_EQ(receipts.back().revision, 1U);
    EXPECT_EQ(receipts.back().index, 13U);
    EXPECT_EQ(driver.current, 12U);
    EXPECT_EQ(driver.starts, 12U);
    EXPECT_FALSE(driver.held);
}

// 拒绝结果保留原错误码，没有驱动取得事实时不能发布成功观察。
TEST(FocusEndpointReceipt, DeniedResultRemainsDeniedWithoutDriverExecution) {
    Network mqtt;
    Driver driver;
    Authority auth;
    auth.allow = false;
    std::vector<Receipt> receipts;
    FocusEndpoint endpoint(mqtt, driver, auth, {"speaker", "display"}, "up", "down", 1000);
    collect_receipts(endpoint, receipts);
    ASSERT_EQ(endpoint.receive("down", request(), 10, 1000), FocusEndpointResult::Handled);
    endpoint.poll(11, 1001);
    ASSERT_EQ(receipts.size(), 1U);
    EXPECT_EQ(receipts.back().state, "denied");
    EXPECT_EQ(receipts.back().operation, "acquire");
    EXPECT_EQ(receipts.back().code, 403);
    EXPECT_EQ(receipts.back().revision, 0U);
    EXPECT_EQ(driver.starts, 0U);
}

// 替换/关闭只读旁路不能改变原请求和执行；重新观察时水位不得复用历史序号。
TEST(FocusEndpointReceipt, OptionalObserverDoesNotChangeAdmissionOrResetWatermark) {
    Network mqtt;
    Driver driver;
    Authority auth;
    std::vector<Receipt> first, second;
    FocusEndpoint endpoint(mqtt, driver, auth, {"speaker", "display"}, "up", "down", 1000);
    collect_receipts(endpoint, first);
    ASSERT_EQ(endpoint.receive("down", request(), 10, 1000), FocusEndpointResult::Handled);
    endpoint.poll(11, 1001);
    driver.complete();
    endpoint.poll(12, 1002);
    ASSERT_EQ(first.size(), 1U);
    endpoint.set_receipt_observer({});
    ASSERT_EQ(endpoint.receive("down", request(), 13, 1003), FocusEndpointResult::Handled);
    endpoint.poll(14, 1004);
    EXPECT_EQ(first.size(), 1U);
    collect_receipts(endpoint, second);
    ASSERT_EQ(endpoint.receive("down", request(), 15, 1005), FocusEndpointResult::Handled);
    endpoint.poll(16, 1006);
    ASSERT_EQ(second.size(), 1U);
    EXPECT_EQ(second.back().index, 2U);
    EXPECT_EQ(second.back().revision, 1U);
    EXPECT_EQ(driver.starts, 1U);
    EXPECT_EQ(auth.calls, 1U);
}

// 原字节查询不解析新请求，不按部分字段、主动事件或相似结果派生操作。
TEST(FocusEndpointReceipt, ReadOnlyQueryMatchesOnlyCompleteKnownReceipt) {
    Driver driver;
    Authority auth;
    FocusEngine engine(driver, auth, "one", "focus", {"speaker", "display"}, 1000);
    EXPECT_FALSE(engine.reply_operation(""));
    EXPECT_FALSE(engine.reply_operation("{}"));
    ASSERT_EQ(engine.submit(request(), 10, 1000).admission, FocusAdmission::Accepted);
    engine.poll(11, 1001);
    driver.complete();
    engine.poll(12, 1002);
    const auto original = engine.take_reply();
    ASSERT_TRUE(original);
    ASSERT_TRUE(engine.reply_operation(*original));
    EXPECT_EQ(*engine.reply_operation(*original), "acquire");
    EXPECT_FALSE(engine.reply_operation(*original + " "));
    EXPECT_FALSE(engine.reply_operation(request()));
    EXPECT_EQ(driver.starts, 1U);
    EXPECT_EQ(auth.calls, 1U);
    EXPECT_EQ(driver.current, 1U);
}

TEST(FocusEndpoint, DriverWithoutPhysicalObservationCannotInventInitialBaseline) {
    Network mqtt;
    Driver driver;  // 旧驱动没有实现首次观察，必须保持未知，不从release_known或revision造idle。
    Authority authority;
    FocusEndpoint endpoint(mqtt, driver, authority, {"speaker"}, "up", "down", 1000, true);
    Envelope source, out;
    ASSERT_EQ(capability_command("original", VoiceCapability{}, source), WireResult::Ok);
    EXPECT_FALSE(driver.initial_idle_observation());
    EXPECT_EQ(endpoint.capability(source, out), WireResult::Invalid);
    EXPECT_TRUE(out.token.empty());
    EXPECT_TRUE(mqtt.attempts.empty());
    EXPECT_TRUE(mqtt.sent.empty());
}

TEST(FocusEndpoint, DeclaredSubsetFormsItsOwnCompleteLease) {
    for (const auto& resource : {std::string("speaker"), std::string("display")}) {
        Network mqtt;
        Driver driver;
        driver.expected_resources = {resource};
        Authority auth;
        FocusEndpoint endpoint(mqtt, driver, auth, {"speaker", "display"}, "up", "down", 1000);
        const auto original = request("one", "a", "acquire", 0, 1, "\"" + resource + "\"");
        ASSERT_EQ(endpoint.receive("down", original, 10, 1000), FocusEndpointResult::Handled);
        endpoint.poll(11, 1001);
        EXPECT_EQ(driver.starts, 1U);
        EXPECT_EQ(driver.last.resources, driver.expected_resources);
        driver.complete();
        endpoint.poll(12, 1002);
        ASSERT_EQ(mqtt.sent.size(), 1U);
        FocusState state;
        ASSERT_EQ(decode_focus_state(WireRoute::Signaling, mqtt.sent.front(), state),
                  WireResult::Ok);
        EXPECT_EQ(state.resources, driver.expected_resources);
        EXPECT_EQ(state.state, "acquired");
        // 同一原租约不能扩大为能力全集，即使新增资源本身是已声明能力。
        EXPECT_EQ(endpoint.receive("down", request("one", "b", "suspend", 1, 2), 13, 1003),
                  FocusEndpointResult::Invalid);
        EXPECT_EQ(auth.calls, 1U);
        endpoint.close();
        EXPECT_EQ(driver.stops.back(), "one");
    }
}

TEST(FocusEndpoint, CompleteLeaseStillRejectsPartialActualFact) {
    Network mqtt;
    Driver driver;
    Authority auth;
    FocusEndpoint endpoint(mqtt, driver, auth, {"speaker", "display"}, "up", "down", 1000);
    ASSERT_EQ(endpoint.receive("down", request(), 10, 1000), FocusEndpointResult::Handled);
    endpoint.poll(11, 1001);
    driver.complete();
    driver.facts.back().resources = {"speaker"};  // 回执缺屏幕不是合法子集申请。
    endpoint.poll(12, 1002);
    ASSERT_EQ(mqtt.sent.size(), 1U);
    FocusState state;
    ASSERT_EQ(decode_focus_state(WireRoute::Signaling, mqtt.sent.front(), state), WireResult::Ok);
    EXPECT_EQ(state.state, "denied");
    EXPECT_EQ(state.reason, "driverFailed");
    EXPECT_EQ(state.resources, (std::vector<std::string>{"speaker", "display"}));
    EXPECT_FALSE(endpoint.released());
}

TEST(FocusEndpoint, StrictTopicAndFieldsNeverReachDriver) {
    Network mqtt;
    Driver driver;
    Authority auth;
    FocusEndpoint endpoint(mqtt, driver, auth, {"speaker", "display"}, "up", "down", 1000);
    EXPECT_EQ(endpoint.receive("other", request(), 10, 1000), FocusEndpointResult::Ignored);
    EXPECT_EQ(endpoint.receive("down", "bad", 10, 1000), FocusEndpointResult::Invalid);
    EXPECT_EQ(endpoint.receive(
                  "down", R"({"msgToken":"a","method":"inputStart","sessionId":"one","params":{}})",
                  10, 1000),
              FocusEndpointResult::Ignored);
    const auto unsupported = request("one", "a", "acquire", 0, 1, "\"microphone\"");
    EXPECT_EQ(endpoint.receive("down", unsupported, 10, 1000), FocusEndpointResult::Invalid);
    EXPECT_EQ(endpoint.receive("down", request("one", "a", "restore"), 10, 1000),
              FocusEndpointResult::Invalid);
    EXPECT_EQ(driver.starts, 0U);
    EXPECT_EQ(auth.calls, 0U);
    EXPECT_TRUE(endpoint.settled());
}

TEST(FocusEndpoint, ActualCompletionBackpressureReplayAndOriginalScope) {
    Network mqtt;
    Driver driver;
    Authority auth;
    FocusEndpoint endpoint(mqtt, driver, auth, {"speaker", "display"}, "up", "down", 1000);
    const auto original = request();
    ASSERT_EQ(endpoint.receive("down", original, 10, 1000), FocusEndpointResult::Handled);
    EXPECT_EQ(driver.starts, 0U);
    EXPECT_FALSE(endpoint.settled());
    endpoint.poll(11, 1001);
    EXPECT_EQ(driver.starts, 1U);
    EXPECT_TRUE(mqtt.sent.empty());
    EXPECT_FALSE(endpoint.released());
    driver.complete();
    mqtt.writable = false;
    endpoint.poll(12, 1002);
    ASSERT_EQ(mqtt.attempts.size(), 1U);
    const auto fact = mqtt.attempts.back();
    endpoint.poll(13, 1003);
    EXPECT_EQ(mqtt.attempts.back(), fact);
    mqtt.writable = true;
    endpoint.poll(14, 1004);
    ASSERT_EQ(mqtt.sent.size(), 1U);
    EXPECT_EQ(mqtt.sent.back(), fact);
    EXPECT_EQ(endpoint.receive("down", original, 15, 1005), FocusEndpointResult::Handled);
    endpoint.poll(16, 1006);
    EXPECT_EQ(mqtt.sent.back(), fact);
    EXPECT_EQ(auth.calls, 1U);
    EXPECT_EQ(driver.starts, 1U);
    EXPECT_EQ(endpoint.receive("down", request("one", "a", "acquire", 0, 2), 17, 1007),
              FocusEndpointResult::Invalid);
    endpoint.close();
    EXPECT_FALSE(endpoint.released());
    ASSERT_FALSE(driver.stops.empty());
    for (const auto& session : driver.stops)
        EXPECT_EQ(session, "one");
    driver.release_known = true;
    endpoint.poll(18, 1008);
    EXPECT_TRUE(endpoint.settled());
    EXPECT_EQ(endpoint.receive("down", request("new"), 19, 1009), FocusEndpointResult::Closed);
}

TEST(FocusEndpoint, DeniedSlotsAreRetainedAndNeverManufactureAllocation) {
    Network mqtt;
    Driver driver;
    Authority auth;
    auth.allow = false;
    FocusEndpoint endpoint(mqtt, driver, auth, {"speaker", "display"}, "up", "down", 1000);
    for (unsigned index = 0; index < 4; ++index) {
        EXPECT_EQ(
            endpoint.receive("down", request(std::to_string(index)), 10 + index, 1000 + index),
            FocusEndpointResult::Handled);
        endpoint.poll(10 + index, 1000 + index);
    }
    EXPECT_EQ(endpoint.receive("down", request("fifth"), 14, 1004), FocusEndpointResult::Full);
    EXPECT_TRUE(endpoint.released());
    EXPECT_TRUE(endpoint.settled());
    EXPECT_EQ(driver.starts, 0U);
    endpoint.close();
    EXPECT_TRUE(driver.stops.empty());
    EXPECT_TRUE(endpoint.settled());
}

TEST(FocusEndpoint, CloseBeforeExecutionStillPublishesOriginalCancellation) {
    Network mqtt;
    Driver driver;
    Authority auth;
    FocusEndpoint endpoint(mqtt, driver, auth, {"speaker", "display"}, "up", "down", 1000);
    ASSERT_EQ(endpoint.receive("down", request(), 10, 1000), FocusEndpointResult::Handled);
    endpoint.close();
    EXPECT_TRUE(endpoint.released());
    EXPECT_FALSE(endpoint.settled());
    endpoint.poll(11, 1001);
    ASSERT_EQ(mqtt.sent.size(), 1U);
    FocusState state;
    ASSERT_EQ(decode_focus_state(WireRoute::Signaling, mqtt.sent.front(), state), WireResult::Ok);
    EXPECT_EQ(state.reason, "sessionEnded");
    EXPECT_EQ(state.envelope.code, 410);
    EXPECT_EQ(driver.starts, 0U);
    EXPECT_TRUE(endpoint.settled());
}

TEST(FocusEndpoint, InvalidConfigurationAndClockRegressionFailClosed) {
    Network mqtt;
    Driver driver;
    Authority auth;
    FocusEndpoint invalid(mqtt, driver, auth, {"speaker", "display"}, "up", "up", 1000);
    EXPECT_EQ(invalid.receive("up", request(), 10, 1000), FocusEndpointResult::Closed);
    for (unsigned condition = 0; condition < 4; ++condition) {
        FocusEndpoint endpoint(mqtt, driver, auth, {"speaker", "display"}, "up", "down", 1000);
        endpoint.poll(10, 1000);
        const auto now = condition == 0 ? 9 : condition == 3 ? 1000 : 11;
        const auto wall = condition == 1 ? 999 : condition == 2 ? 0 : 1001;
        EXPECT_EQ(endpoint.receive("down", request(), now, wall), FocusEndpointResult::Closed);
        EXPECT_TRUE(endpoint.settled());
    }
    EXPECT_EQ(auth.calls, 0U);
}

TEST(FocusEndpoint, SuspendRestoreReleaseUseActualFactsAndRetainOriginalTerminal) {
    Network mqtt;
    Driver driver;
    Authority auth;
    FocusEndpoint endpoint(mqtt, driver, auth, {"speaker", "display"}, "up", "down", 1000);
    const std::vector<std::string> operations = {"acquire", "suspend", "restore", "release"};
    for (unsigned index = 0; index < operations.size(); ++index) {
        const auto json =
            request("one", std::to_string(index), operations[index], index, index + 1);
        ASSERT_EQ(endpoint.receive("down", json, 10 + index * 3, 1000 + index * 3),
                  FocusEndpointResult::Handled);
        endpoint.poll(11 + index * 3, 1001 + index * 3);
        EXPECT_EQ(mqtt.sent.size(), index);
        driver.complete();
        endpoint.poll(12 + index * 3, 1002 + index * 3);
        ASSERT_EQ(mqtt.sent.size(), index + 1);
        FocusState state;
        ASSERT_EQ(decode_focus_state(WireRoute::Signaling, mqtt.sent.back(), state),
                  WireResult::Ok);
        EXPECT_EQ(state.envelope.code, 200);
        EXPECT_EQ(state.revision, index + 1);
    }
    EXPECT_TRUE(endpoint.settled());
    const auto terminal = mqtt.sent.back();
    ASSERT_EQ(endpoint.receive("down", request("one", "3", "release", 3, 4), 30, 1030),
              FocusEndpointResult::Handled);
    endpoint.poll(31, 1031);
    EXPECT_EQ(mqtt.sent.back(), terminal);
    EXPECT_EQ(driver.starts, 4U);
    EXPECT_EQ(auth.calls, 4U);
    EXPECT_TRUE(driver.stops.empty());
}

TEST(FocusEndpoint, DifferentReplayCannotOverwritePendingOriginalBytes) {
    Network mqtt;
    Driver driver;
    Authority auth;
    auth.allow = false;
    mqtt.writable = false;
    FocusEndpoint endpoint(mqtt, driver, auth, {"speaker", "display"}, "up", "down", 1000);
    ASSERT_EQ(endpoint.receive("down", request(), 10, 1000), FocusEndpointResult::Handled);
    endpoint.poll(11, 1001);
    const auto original = mqtt.attempts.back();
    const auto other = request("one", "b", "acquire", 0, 2);
    ASSERT_EQ(endpoint.receive("down", other, 12, 1002), FocusEndpointResult::Handled);
    EXPECT_EQ(endpoint.receive("down", other, 13, 1003), FocusEndpointResult::Full);
    endpoint.poll(14, 1004);
    EXPECT_EQ(mqtt.attempts.back(), original);
    mqtt.writable = true;
    endpoint.poll(15, 1005);
    endpoint.poll(16, 1006);
    ASSERT_EQ(mqtt.sent.size(), 2U);
    EXPECT_EQ(mqtt.sent.front(), original);
    FocusState second;
    ASSERT_EQ(decode_focus_state(WireRoute::Signaling, mqtt.sent.back(), second), WireResult::Ok);
    EXPECT_EQ(second.envelope.token, "b");
    EXPECT_TRUE(endpoint.settled());
    EXPECT_EQ(driver.starts, 0U);
}

// 正常释放不能耗尽四个工作槽；退役租约仍保留原请求事实并拒绝复活。
TEST(FocusEndpoint, ReleasedLeasesRecycleWorkingSlotsWithoutLosingOriginalReplies) {
    Network mqtt;
    Driver driver;
    Authority auth;
    FocusEndpoint endpoint(mqtt, driver, auth, {"speaker", "display"}, "up", "down", 1000);
    std::vector<std::string> originals, replies;
    for (unsigned index = 0; index < 6; ++index) {
        const auto session = "lease-" + std::to_string(index);
        const auto now = 10U + index * 6;
        const auto acquire = request(session, "acquire", "acquire", index * 2, 1);
        ASSERT_EQ(endpoint.receive("down", acquire, now, 1000 + now), FocusEndpointResult::Handled);
        endpoint.poll(now + 1, 1001 + now);
        driver.complete();
        endpoint.poll(now + 2, 1002 + now);
        originals.push_back(acquire);
        replies.push_back(mqtt.sent.back());
        const auto release = request(session, "release", "release", index * 2 + 1, 2);
        ASSERT_EQ(endpoint.receive("down", release, now + 3, 1003 + now),
                  FocusEndpointResult::Handled);
        endpoint.poll(now + 4, 1004 + now);
        driver.complete();
        endpoint.poll(now + 5, 1005 + now);
        EXPECT_TRUE(endpoint.settled());
    }
    const auto starts = driver.starts;
    const auto permits = auth.calls;
    for (std::size_t index = 0; index < originals.size(); ++index) {
        const auto now = 60U + static_cast<unsigned>(index) * 3;
        ASSERT_EQ(endpoint.receive("down", originals[index], now, 1000 + now),
                  FocusEndpointResult::Handled);
        endpoint.poll(now + 1, 1001 + now);
        EXPECT_EQ(mqtt.sent.back(), replies[index]);
        EXPECT_EQ(endpoint.receive("down", request("lease-" + std::to_string(index), "new"),
                                   now + 2, 1002 + now),
                  FocusEndpointResult::Closed);
    }
    EXPECT_EQ(driver.starts, starts);
    EXPECT_EQ(auth.calls, permits);
    EXPECT_TRUE(driver.stops.empty());
    const auto conflicting = request("lease-0", "acquire", "acquire", 0, 2);
    EXPECT_EQ(endpoint.receive("down", conflicting, 90, 1090), FocusEndpointResult::Invalid);
    mqtt.writable = false;
    ASSERT_EQ(endpoint.receive("down", originals.front(), 91, 1091), FocusEndpointResult::Handled);
    endpoint.poll(92, 1092);
    EXPECT_EQ(mqtt.attempts.back(), replies.front());
    EXPECT_FALSE(endpoint.settled());
    mqtt.writable = true;
    endpoint.poll(93, 1093);
    EXPECT_EQ(mqtt.sent.back(), replies.front());
    EXPECT_TRUE(endpoint.settled());
}

// 未发布的整组释放事实仍占工作槽；网络恢复前不能因驱动已空闲删除原关联。
TEST(FocusEndpoint, BlockedReleaseRepliesCannotRetireWorkingSlots) {
    Network mqtt;
    Driver driver;
    Authority auth;
    FocusEndpoint endpoint(mqtt, driver, auth, {"speaker", "display"}, "up", "down", 1000);
    mqtt.writable = false;
    for (unsigned index = 0; index < 4; ++index) {
        const auto session = "blocked-" + std::to_string(index);
        const auto now = 10U + index * 6;
        ASSERT_EQ(endpoint.receive("down", request(session, "acquire", "acquire", index * 2, 1),
                                   now, 1000 + now),
                  FocusEndpointResult::Handled);
        endpoint.poll(now + 1, 1001 + now);
        driver.complete();
        endpoint.poll(now + 2, 1002 + now);
        ASSERT_EQ(endpoint.receive("down", request(session, "release", "release", index * 2 + 1, 2),
                                   now + 3, 1003 + now),
                  FocusEndpointResult::Handled);
        endpoint.poll(now + 4, 1004 + now);
        driver.complete();
        endpoint.poll(now + 5, 1005 + now);
    }
    EXPECT_TRUE(endpoint.released());
    EXPECT_FALSE(endpoint.settled());
    EXPECT_EQ(endpoint.receive("down", request("fifth", "acquire", "acquire", 8, 1), 40, 1040),
              FocusEndpointResult::Full);
    EXPECT_EQ(driver.starts, 8U);
    mqtt.writable = true;
    endpoint.poll(41, 1041);
    endpoint.poll(42, 1042);
    ASSERT_TRUE(endpoint.settled());
    EXPECT_EQ(endpoint.receive("down", request("fifth", "acquire", "acquire", 8, 1), 43, 1043),
              FocusEndpointResult::Handled);
}

// 墓碑预算耗尽时必须保留旧事实并背压；不能淘汰旧租约以重新执行同一原请求。
TEST(FocusEndpoint, RetirementBudgetFailsClosedAndPreservesEarliestLease) {
    Network mqtt;
    Driver driver;
    Authority auth;
    FocusEndpoint endpoint(mqtt, driver, auth, {"speaker", "display"}, "up", "down", 10000);
    std::string first_reply;
    unsigned count = 0;
    for (; count < 200; ++count) {
        const auto session = "budget-" + std::to_string(count);
        const auto now = 10U + count * 4;
        const auto acquire = request(session, "acquire", "acquire", count * 2, 1);
        const auto admitted = endpoint.receive("down", acquire, now, 1000 + now);
        if (admitted == FocusEndpointResult::Full)
            break;
        ASSERT_EQ(admitted, FocusEndpointResult::Handled);
        endpoint.poll(now, 1000 + now);
        driver.complete();
        endpoint.poll(now + 1, 1001 + now);
        if (count == 0)
            first_reply = mqtt.sent.back();
        ASSERT_EQ(endpoint.receive("down", request(session, "release", "release", count * 2 + 1, 2),
                                   now + 2, 1002 + now),
                  FocusEndpointResult::Handled);
        endpoint.poll(now + 2, 1002 + now);
        driver.complete();
        endpoint.poll(now + 3, 1003 + now);
    }
    ASSERT_GT(count, 4U);
    ASSERT_LT(count, 133U);
    const auto starts = driver.starts;
    const auto calls = auth.calls;
    const auto now = 10U + count * 4 + 1;
    ASSERT_EQ(endpoint.receive("down", request("budget-0", "acquire"), now, 1000 + now),
              FocusEndpointResult::Handled);
    endpoint.poll(now + 1, 1001 + now);
    EXPECT_EQ(mqtt.sent.back(), first_reply);
    EXPECT_EQ(driver.starts, starts);
    EXPECT_EQ(auth.calls, calls);
}
}  // namespace ur::device
