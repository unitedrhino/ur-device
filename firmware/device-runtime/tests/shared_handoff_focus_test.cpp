// 文件说明：使用正式FocusEndpoint／FocusEngine验证共享初始租约与协调实际回执；网络、权限及资源事实显式模拟，不代替端云E2E。
#include "ur/device/shared_handoff_focus.hpp"
#include <gtest/gtest.h>
#include <deque>

namespace ur::device {
namespace {
// Network 明确区分发布背压与本地接纳，不能从true推断Broker或后端已提交。
struct Network final : PublishPort {
    bool writable = true;           // 用例控制原唯一网络发布接纳。
    std::vector<std::string> sent;  // 原接纳字节，不保存认证凭据。
    bool publish(std::string_view topic, std::string_view bytes) override {
        EXPECT_EQ(topic, "up");
        if (!writable)
            return false;
        sent.emplace_back(bytes);
        return true;
    }
};
// Driver
// 只由complete提供整组完成事实，记录执行代次以证明初始AI执行器没有被重建。
struct Driver final : FocusDriver {
    std::uint64_t current = 0, generation = 0;  // 实际水位及原驱动操作代次。
    bool held = false, start_ok = true,
         idle = true;                        // 明确模拟占用、启动拒绝及独立初始探针。
    unsigned starts = 0, stops = 0;          // 断言未因重复请求执行第二次。
    FocusCommand last;                       // 原驱动接纳请求。
    std::vector<std::uint64_t> generations;  // 原AI执行器沿同租约递增。
    std::deque<FocusFact> facts;             // 实际完成由测试独立注入。
    std::vector<std::string> idle_resources{"microphone", "speaker"};  // 完整首次探针资源。
    std::uint64_t revision() const override { return current; }
    std::optional<FocusIdleObservation> initial_idle_observation() const override {
        return idle ? std::optional<FocusIdleObservation>({idle_resources, current}) : std::nullopt;
    }
    bool begin(const FocusCommand& command, std::uint64_t value) override {
        ++starts;
        last = command;
        generation = value;
        generations.push_back(value);
        return start_ok;
    }
    std::optional<FocusFact> take_fact(std::string_view session, std::string_view focus) override {
        if (facts.empty() || session != last.envelope.session || focus != last.focus_id)
            return {};
        auto result = std::move(facts.front());
        facts.pop_front();
        return result;
    }
    bool owns(const FocusCommand& command) const override {
        return held && command.envelope.session == last.envelope.session &&
               command.focus_id == last.focus_id;
    }
    bool stop_scope(std::string_view, std::string_view,
                    const std::vector<std::string>& resources) override {
        EXPECT_EQ(resources, (std::vector<std::string>{"microphone", "speaker"}));
        ++stops;
        held = false;
        return true;
    }
    // complete 模拟完整实际结果；begin本身不会获得或暂停任何资源。
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
// Authority 明确模拟独立可信业务授权；测试可以撤权，产品必须装配真实权限来源。
struct Authority final : FocusAuthority {
    bool allow = true;      // 用例明确拒绝不允许的当前业务。
    unsigned admitted = 0;  // 旧请求重放不能再次签发许可。
    std::optional<FocusPermit> authorize(const FocusCommand& command, std::uint64_t,
                                         std::uint64_t) override {
        ++admitted;
        return allow ? std::optional<FocusPermit>(FocusPermit{command, 900, 1}) : std::nullopt;
    }
    bool valid(const FocusPermit&, std::uint64_t, std::uint64_t) const override { return allow; }
};
// command 由正式报文decoder生成结构体，不绕过完整组及控制枚举校验。
FocusCommand command(std::string token = "initial", std::string operation = "acquire",
                     std::uint64_t revision = 0, std::uint64_t sequence = 1,
                     std::string session = "ai", std::string focus = "ai-focus") {
    const auto body =
        "{\"msgToken\":\"" + token + "\",\"method\":\"focusControl\",\"sessionId\":\"" + session +
        "\",\"eventSeq\":" + std::to_string(sequence) + ",\"params\":{\"operation\":\"" +
        operation + "\",\"expectedRevision\":" + std::to_string(revision) + ",\"focusId\":\"" +
        focus +
        "\",\"resources\":[\"microphone\",\"speaker\"],\"priority\":"
        "\"conversation\",\"resumePolicy\":\"explicit\"" +
        ((operation == "acquire" || operation == "restore") ? ",\"expiresAt\":2000" : "") + "}}";
    FocusCommand result;
    EXPECT_EQ(decode_focus_control(WireRoute::Signaling, body, result), WireResult::Ok);
    return result;
}
// bytes 保持原控制的规范信封，独立校验结构体冲突另有用例。
std::string bytes(const FocusCommand& value) {
    std::string result;
    EXPECT_EQ(encode_envelope(value.envelope, result), DecodeResult::Ok);
    return result;
}
// SharedFocusFixture 借同一原发布器、驱动与正式执行器，所有时钟由用例推进。
struct SharedFocusFixture : testing::Test {
    Network network;      // 原共享网络模拟。
    Driver driver;        // 原整组驱动模拟。
    Authority authority;  // 独立业务许可模拟。
    SharedHandoffFocusEndpoint focus{network, driver, authority,
                                     "up",    "down", 1000};  // 正式被测装配。
    // initial 通过宿主receive建立原AI租约，完成结果只能交宿主而不能冒移交结果。
    void initial() {
        ASSERT_EQ(focus.receive("down", bytes(command()), 10, 1010), FocusEndpointResult::Handled);
        focus.poll(11, 1011);
        ASSERT_EQ(driver.starts, 1U);
        EXPECT_FALSE(focus.take_receipt());
        EXPECT_FALSE(focus.take_result());
        driver.complete();
        focus.poll(12, 1012);
        ASSERT_TRUE(focus.take_receipt());
        EXPECT_FALSE(focus.take_result());
    }
};
TEST_F(SharedFocusFixture, InitialAndHandoffUseTheSameOriginalAiEngine) {
    initial();
    const auto suspend = command("suspend", "suspend", 1, 2);
    ASSERT_EQ(focus.submit(suspend, 13, 1013).admission, FocusAdmission::Accepted);
    focus.poll(14, 1014);
    EXPECT_FALSE(focus.take_result());
    driver.complete();
    focus.poll(15, 1015);
    const auto actual = focus.take_result();
    ASSERT_TRUE(actual);
    EXPECT_TRUE(focus_reply_matches(suspend, actual->actual));
    EXPECT_EQ(actual->actual.state, "suspended");
    EXPECT_EQ(actual->actual.revision, 2U);
    EXPECT_EQ(actual->actual.envelope.code, 200);
    EXPECT_FALSE(actual->definitely_not_applied);
    EXPECT_EQ(driver.generations, (std::vector<std::uint64_t>{1, 2}));
}
TEST_F(SharedFocusFixture, NetworkBackpressureCannotCreateLocalCompletion) {
    initial();
    network.writable = false;
    ASSERT_EQ(focus.submit(command("suspend", "suspend", 1, 2), 13, 1013).admission,
              FocusAdmission::Accepted);
    focus.poll(14, 1014);
    driver.complete();
    focus.poll(15, 1015);
    EXPECT_FALSE(focus.take_result());
    EXPECT_FALSE(focus.take_receipt());
    const auto starts = driver.starts;
    network.writable = true;
    focus.poll(16, 1016);
    ASSERT_TRUE(focus.take_result());
    ASSERT_TRUE(focus.take_receipt());
    EXPECT_EQ(driver.starts, starts);
}
TEST_F(SharedFocusFixture, ReplayingAnOperationDoesNotRepeatDriverOrPermit) {
    initial();
    const auto original = command("suspend", "suspend", 1, 2);
    ASSERT_EQ(focus.submit(original, 13, 1013).admission, FocusAdmission::Accepted);
    focus.poll(14, 1014);
    driver.complete();
    focus.poll(15, 1015);
    const auto first = focus.take_result();
    ASSERT_TRUE(first);
    ASSERT_TRUE(focus.take_receipt());
    const auto sent = network.sent.back();
    ASSERT_EQ(focus.submit(original, 16, 1016).admission, FocusAdmission::Accepted);
    focus.poll(17, 1017);
    const auto replay = focus.take_result();
    ASSERT_TRUE(replay);
    EXPECT_EQ(replay->actual.revision, first->actual.revision);
    EXPECT_EQ(network.sent.back(), sent);
    EXPECT_EQ(driver.starts, 2U);
    EXPECT_EQ(authority.admitted, 2U);
}
TEST_F(SharedFocusFixture, MutatedStructureCannotOverrideOriginalEnvelope) {
    auto original = command();
    original.expected_revision = 7;
    EXPECT_EQ(focus.submit(original, 10, 1010).admission, FocusAdmission::Invalid);
    original = command();
    original.operation = "release";
    EXPECT_EQ(focus.submit(original, 10, 1010).admission, FocusAdmission::Invalid);
    focus.poll(11, 1011);
    EXPECT_EQ(driver.starts, 0U);
}
TEST_F(SharedFocusFixture, ConflictingOriginalTokenCannotChangeCas) {
    initial();
    ASSERT_EQ(focus.submit(command("suspend", "suspend", 1, 2), 13, 1013).admission,
              FocusAdmission::Accepted);
    EXPECT_EQ(focus.submit(command("suspend", "suspend", 2, 2), 14, 1014).admission,
              FocusAdmission::Conflict);
    focus.poll(15, 1015);
    EXPECT_EQ(driver.starts, 2U);
}
TEST_F(SharedFocusFixture, NetworkDownlinkFocusStateIsNotActualDriverResult) {
    Envelope reply;
    ASSERT_EQ(focus_reply(command(), "acquired", 1, 200, {}, reply), WireResult::Ok);
    std::string body;
    ASSERT_EQ(encode_envelope(reply, body), DecodeResult::Ok);
    EXPECT_EQ(focus.receive("down", body, 10, 1010), FocusEndpointResult::Ignored);
    EXPECT_FALSE(focus.take_result());
    EXPECT_FALSE(focus.take_receipt());
    EXPECT_EQ(driver.starts, 0U);
}
TEST_F(SharedFocusFixture, WrongTopicDoesNotExecuteControl) {
    EXPECT_EQ(focus.receive("other", bytes(command()), 10, 1010), FocusEndpointResult::Ignored);
    focus.poll(11, 1011);
    EXPECT_EQ(driver.starts, 0U);
}
TEST_F(SharedFocusFixture, PartialGroupCannotEnterTheSharedInitialEndpoint) {
    auto partial = bytes(command());
    const auto microphone = partial.find("\"microphone\",");
    ASSERT_NE(microphone, std::string::npos);
    partial.erase(microphone, std::string("\"microphone\",").size());
    EXPECT_EQ(focus.receive("down", partial, 10, 1010), FocusEndpointResult::Invalid);
    focus.poll(11, 1011);
    EXPECT_EQ(driver.starts, 0U);
    EXPECT_TRUE(network.sent.empty());
}
TEST_F(SharedFocusFixture, DenialDoesNotPromisePhysicalNonExecution) {
    authority.allow = false;
    ASSERT_EQ(focus.submit(command(), 10, 1010).admission, FocusAdmission::Accepted);
    focus.poll(11, 1011);
    const auto result = focus.take_result();
    ASSERT_TRUE(result);
    EXPECT_EQ(result->actual.state, "denied");
    EXPECT_EQ(result->actual.envelope.code, 403);
    EXPECT_FALSE(result->definitely_not_applied);
    EXPECT_EQ(driver.starts, 0U);
}
TEST_F(SharedFocusFixture, FailedDriverStartNeverBecomesAcquired) {
    driver.start_ok = false;
    ASSERT_EQ(focus.submit(command(), 10, 1010).admission, FocusAdmission::Accepted);
    focus.poll(11, 1011);
    const auto result = focus.take_result();
    ASSERT_TRUE(result);
    EXPECT_EQ(result->actual.state, "denied");
    EXPECT_FALSE(result->definitely_not_applied);
    EXPECT_FALSE(driver.held);
}
TEST_F(SharedFocusFixture, HostQueueBackpressurePreservesOriginalUnpublishedResult) {
    for (unsigned index = 0; index < 9; ++index) {
        const auto operation = index == 0 ? "acquire" : (index % 2 ? "suspend" : "restore");
        ASSERT_EQ(
            focus.receive(
                "down", bytes(command("op-" + std::to_string(index), operation, index, index + 1)),
                10 + index * 3, 1010 + index * 3),
            FocusEndpointResult::Handled);
        focus.poll(11 + index * 3, 1011 + index * 3);
        driver.complete();
        focus.poll(12 + index * 3, 1012 + index * 3);
    }
    ASSERT_EQ(network.sent.size(), 8U);
    auto first = focus.take_receipt();
    ASSERT_TRUE(first);
    EXPECT_EQ(first->envelope.token, "op-0");
    focus.poll(40, 1040);
    ASSERT_EQ(network.sent.size(), 9U);
    for (unsigned index = 1; index < 9; ++index) {
        auto original = focus.take_receipt();
        ASSERT_TRUE(original);
        EXPECT_EQ(original->envelope.token, "op-" + std::to_string(index));
    }
    EXPECT_FALSE(focus.take_receipt());
    EXPECT_FALSE(focus.take_result());
    EXPECT_EQ(driver.starts, 9U);
}
TEST_F(SharedFocusFixture, CloseCannotRecreateTheOriginalLease) {
    initial();
    focus.close();
    EXPECT_EQ(focus.submit(command("later", "restore", 1, 2), 13, 1013).admission,
              FocusAdmission::Closed);
    EXPECT_EQ(driver.starts, 1U);
    EXPECT_GE(driver.stops, 1U);
}
TEST_F(SharedFocusFixture, ClockRollbackClosesOriginalScope) {
    initial();
    focus.poll(11, 1013);
    EXPECT_EQ(focus.submit(command("suspend", "suspend", 1, 2), 13, 1014).admission,
              FocusAdmission::Closed);
    EXPECT_FALSE(driver.held);
}
TEST(SharedFocusConfiguration, MissingOrPartialIdleDoesNotAdvertiseFullGroup) {
    Network network;
    Authority authority;
    Driver unknown;
    unknown.idle = false;
    SharedHandoffFocusEndpoint absent(network, unknown, authority, "up", "down", 1000);
    EXPECT_FALSE(absent.supports_microphone_speaker());
    EXPECT_EQ(absent.submit(command(), 10, 1010).admission, FocusAdmission::Invalid);
    Driver partial;
    partial.idle_resources = {"speaker"};
    SharedHandoffFocusEndpoint incomplete(network, partial, authority, "up", "down", 1000);
    EXPECT_FALSE(incomplete.supports_microphone_speaker());
    EXPECT_EQ(incomplete.receive("down", bytes(command()), 10, 1010), FocusEndpointResult::Closed);
}
TEST(SharedFocusConfiguration, InvalidTopicsAndDeadlineDoNotExecute) {
    Network network;
    Authority authority;
    Driver driver;
    SharedHandoffFocusEndpoint same(network, driver, authority, "same", "same", 1000);
    SharedHandoffFocusEndpoint empty(network, driver, authority, "up", "", 1000);
    SharedHandoffFocusEndpoint zero(network, driver, authority, "up", "down", 0);
    EXPECT_FALSE(same.supports_microphone_speaker());
    EXPECT_FALSE(empty.supports_microphone_speaker());
    EXPECT_FALSE(zero.supports_microphone_speaker());
}
}  // namespace
}  // namespace ur::device
