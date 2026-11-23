// 文件说明：控制连接暂停不得重放旧输入，也不能启动业务会话幂等记录的驱逐计时。
#include <gtest/gtest.h>
#include <vector>
#include "ur/device/requests.hpp"

namespace ur::device {
namespace {
// RecoveryPublisher 仅记录真实提交次数，不伪造远端执行或应答。
struct RecoveryPublisher final : PublishPort {
    std::vector<std::string> bodies;  // 原始字节用于确认旧输入不会被定时器重发。
    bool publish(std::string_view, std::string_view body) override {
        bodies.emplace_back(body);
        return true;
    }
};
// input_request 建立固定会话内请求；不同token代表不同用户输入，不因重连重复使用。
Request input_request(std::string token = "input-1", std::string session = "original-ai") {
    return {std::move(token), std::move(session), "audioStart", "$thing/up/signaling/p/d",
            R"({"params":{"inputId":"original-input"}})"};
}
// input_key 定位原完整作用域，不能将另一个会话或命令的结果借给此输入。
RequestKey input_key(std::string_view token = "input-1") {
    return {"$thing/up/signaling/p/d", "audioStart", token};
}

TEST(RequestsControlRecovery, SuspendStopsOldRetriesAndRejectsLateAck) {
    RecoveryPublisher port;
    Requests requests(port);
    ASSERT_EQ(requests.submit(input_request(), 100), SubmitResult::Accepted);
    requests.suspend_session("original-ai", 101);
    EXPECT_EQ(requests.state(input_key()), RequestState::Cancelled);
    requests.tick(10100);
    requests.tick(20100);
    requests.tick(30100);
    EXPECT_EQ(port.bodies.size(), 1U);
    EXPECT_FALSE(requests.acknowledge(
        {"input-1", "original-ai", "audioStart", 200, "$thing/up/signaling/p/d"}, 102));
    EXPECT_EQ(requests.submit(input_request(), 103), SubmitResult::Duplicate);
    EXPECT_EQ(port.bodies.size(), 1U);
    EXPECT_EQ(requests.submit(input_request("fresh-input"), 104), SubmitResult::Accepted);
    EXPECT_EQ(port.bodies.size(), 2U);
}

TEST(RequestsControlRecovery, SuspensionDoesNotExpireActiveSessionFacts) {
    RecoveryPublisher port;
    Requests requests(port);
    ASSERT_EQ(requests.submit(input_request(), 100), SubmitResult::Accepted);
    requests.suspend_session("original-ai", 101);
    requests.tick(900000);
    EXPECT_EQ(requests.state(input_key()), RequestState::Cancelled);
    EXPECT_EQ(requests.submit(input_request(), 900001), SubmitResult::Duplicate);
    requests.cancel_session("original-ai", 900002);
    requests.tick(1500001);
    EXPECT_EQ(requests.state(input_key()), RequestState::Cancelled);
    requests.tick(1500002);
    EXPECT_EQ(requests.state(input_key()), RequestState::Missing);
}

TEST(RequestsControlRecovery, SuspensionIsScopedAndKeepsCompletedFacts) {
    RecoveryPublisher port;
    Requests requests(port);
    ASSERT_EQ(requests.submit(input_request("done"), 100), SubmitResult::Accepted);
    ASSERT_TRUE(requests.acknowledge(
        {"done", "original-ai", "audioStart", 200, "$thing/up/signaling/p/d"}, 101));
    ASSERT_EQ(requests.submit(input_request(), 102), SubmitResult::Accepted);
    ASSERT_EQ(requests.submit(input_request("other", "another-ai"), 103), SubmitResult::Accepted);
    requests.suspend_session("original-ai", 104);
    requests.suspend_session("original-ai", 105);
    EXPECT_EQ(requests.state(input_key("done")), RequestState::Succeeded);
    EXPECT_EQ(requests.state(input_key()), RequestState::Cancelled);
    EXPECT_EQ(requests.state(input_key("other")), RequestState::Pending);
    requests.tick(10103);
    EXPECT_EQ(port.bodies.size(), 4U);
}

TEST(RequestsControlRecovery, EmptyOrBackwardSuspensionCannotRetireAnotherScope) {
    RecoveryPublisher port;
    Requests requests(port);
    ASSERT_EQ(requests.submit(input_request(), 100), SubmitResult::Accepted);
    requests.suspend_session({}, 101);
    requests.suspend_session("original-ai", 99);
    EXPECT_EQ(requests.state(input_key()), RequestState::Pending);
    requests.suspend_session("original-ai", 100);
    EXPECT_EQ(requests.state(input_key()), RequestState::Cancelled);
}
}  // namespace
}  // namespace ur::device
