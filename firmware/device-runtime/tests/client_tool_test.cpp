// 文件说明：真实客户端工具核心的离线合同测试；虚拟时钟和末端驱动观察不代替真实网络/E2E。
#include "ur/device/client_tool.hpp"
#include <gtest/gtest.h>
#include <vector>

namespace ur::device {
namespace {
// ThemeDisplay 保存实际驱动票据，测试必须显式提供同代完成，不能把接纳当完成。
struct ThemeDisplay final : ClientToolDisplayPort {
    int requests = 0;                    // 真实副作用请求次数。
    std::uint64_t generation = 0;        // 最近原驱动票据。
    std::optional<ClientToolFact> fact;  // 可控原末端观察。
    bool accepted = true;                // 真实驱动可拒绝执行，不产生确定副作用。
    std::vector<std::uint64_t> retired;  // 记录唯一终态是否退休原代次，不能误取消下一轮。
    bool request_theme(std::string_view, std::uint64_t ticket) override {
        ++requests;
        generation = ticket;
        return accepted;
    }
    std::optional<ClientToolFact> take_theme_fact(std::uint64_t) override {
        auto result = fact;
        fact.reset();
        return result;
    }
    // retire_theme只观察原执行器通知，不将取消通知冒充实际DOM回滚。
    void retire_theme(std::uint64_t ticket) override { retired.push_back(ticket); }
};
// theme_command 构造真正公共信封，参数不借用内容展示解析器。
std::string theme_command() {
    Envelope event;
    event.token = "execute";
    event.method = "toolCallExecute";
    event.session = "session";
    event.request = false;
    event.message_sequence = 3;
    event.payload =
        R"({"respId":"original","toolCallId":"call","name":"set_local_display_theme","arguments":{"theme":"dark"},"isFrontend":true,"interactionType":"action","executionToken":"synthetic-execution-token","expiresAt":11000})";
    std::string body;
    EXPECT_EQ(encode_envelope(event, body), DecodeResult::Ok);
    return body;
}
// decoded_command 取得真正解析结果，宿主仍须另提供已建立原回复scope。
ClientToolCommand decoded_command() {
    ClientToolCommand command;
    EXPECT_EQ(decode_client_tool_execute(WireRoute::Message, theme_command(), command),
              WireResult::Ok);
    return command;
}
// replace_field
// 只用于合成边界报文，修改之后重新经过实际解析器，不能直接伪造类型化权限。
std::string replace_field(std::string body, std::string_view from, std::string_view to) {
    const auto position = body.find(from);
    EXPECT_NE(position, std::string::npos);
    if (position != std::string::npos)
        body.replace(position, from.size(), to);
    return body;
}
// ClientToolTest.OriginalActualCompletion
// 只在同原票据实际完成后产生succeeded及关联Submit。
TEST(ClientToolTest, OriginalActualCompletion) {
    ClientToolCommand command;
    ASSERT_EQ(decode_client_tool_execute(WireRoute::Message, theme_command(), command),
              WireResult::Ok);
    ThemeDisplay display;
    ClientToolExecutor executor(display, "instance");
    ASSERT_EQ(executor.accept(command, {"session", "original", true}, 100, 1000),
              ClientToolDecision::Accepted);
    EXPECT_EQ(display.requests, 1);
    executor.poll(101);
    EXPECT_FALSE(executor.take_submission());
    display.fact = ClientToolFact{display.generation, "succeeded", "dark"};
    executor.poll(102);
    const auto submission = executor.take_submission();
    ASSERT_TRUE(submission);
    EXPECT_EQ(submission->status, "succeeded");
    EXPECT_EQ(submission->envelope.method, "toolCallSubmit");
    EXPECT_EQ(submission->envelope.session, "session");
    EXPECT_NE(submission->envelope.payload.find(R"("respId":"original")"), std::string::npos);
    EXPECT_NE(submission->envelope.payload.find(R"("toolCallId":"call")"), std::string::npos);
    EXPECT_FALSE(executor.take_submission());
}
// ClientToolTest.ClosedSchemaAndDedicatedRoute
// 拒绝脚本、未知工具、任意schema/null及展示事件执行。
TEST(ClientToolTest, ClosedSchemaAndDedicatedRoute) {
    ClientToolCommand command;
    EXPECT_EQ(decode_client_tool_execute(WireRoute::Signaling, theme_command(), command),
              WireResult::WrongRoute);
    EXPECT_EQ(decode_client_tool_execute(
                  WireRoute::Message,
                  replace_field(theme_command(), "toolCallExecute", "toolCallStart"), command),
              WireResult::Unsupported);
    for (const auto& body :
         {replace_field(theme_command(), "set_local_display_theme", "arbitrary_script"),
          replace_field(theme_command(), R"("theme":"dark")",
                        R"("theme":"dark","script":"synthetic")"),
          replace_field(theme_command(), R"("theme":"dark")", R"("theme":null)"),
          replace_field(theme_command(), R"("theme":"dark")", R"("theme":"dark","theme":"light")"),
          replace_field(theme_command(), R"("isFrontend":true)", R"("isFrontend":false)"),
          replace_field(theme_command(), R"("interactionType":"action")",
                        R"("interactionType":"form")"),
          replace_field(theme_command(), "11000", "11000.5")}) {
        EXPECT_EQ(decode_client_tool_execute(WireRoute::Message, body, command),
                  WireResult::Invalid);
        EXPECT_TRUE(command.call.empty());
    }
}
// ClientToolTest.OriginalScopeAndDeadline
// 错原会话/回复或未建立及已到期不得产生任何驱动请求。
TEST(ClientToolTest, OriginalScopeAndDeadline) {
    ThemeDisplay display;
    ClientToolExecutor executor(display, "instance");
    const auto command = decoded_command();
    EXPECT_EQ(executor.accept(command, {"session", "current", true}, 100, 1000),
              ClientToolDecision::Denied);
    EXPECT_EQ(executor.accept(command, {"other", "original", true}, 100, 1000),
              ClientToolDecision::Denied);
    EXPECT_EQ(executor.accept(command, {"session", "original", false}, 100, 1000),
              ClientToolDecision::Denied);
    EXPECT_EQ(executor.accept(command, {"session", "original", true}, 100, 11000),
              ClientToolDecision::Denied);
    EXPECT_EQ(display.requests, 0);
}
// ClientToolTest.DuplicateAndConflictReplay
// 同调用跨msgToken只重放原结果，错token或换call同意图不能补发。
TEST(ClientToolTest, DuplicateAndConflictReplay) {
    ThemeDisplay display;
    ClientToolExecutor executor(display, "instance");
    auto command = decoded_command();
    const ClientToolScope scope{"session", "original", true};
    ASSERT_EQ(executor.accept(command, scope, 100, 1000), ClientToolDecision::Accepted);
    command.envelope.token = "new-delivery-token";
    EXPECT_EQ(executor.accept(command, scope, 101, 1001), ClientToolDecision::Replay);
    EXPECT_EQ(display.requests, 1);
    display.fact = ClientToolFact{display.generation, "succeeded", "dark"};
    executor.poll(102);
    const auto original = executor.take_submission();
    ASSERT_TRUE(original);
    EXPECT_EQ(executor.accept(command, scope, 103, 1003), ClientToolDecision::Replay);
    const auto replay = executor.take_submission();
    ASSERT_TRUE(replay);
    EXPECT_EQ(replay->envelope.token, original->envelope.token);
    EXPECT_EQ(replay->envelope.payload, original->envelope.payload);
    ClientToolCommand changed;
    ASSERT_EQ(decode_client_tool_execute(WireRoute::Message,
                                         replace_field(theme_command(), "synthetic-execution-token",
                                                       "different-execution-token"),
                                         changed),
              WireResult::Ok);
    EXPECT_EQ(executor.accept(changed, scope, 104, 1004), ClientToolDecision::Denied);
    ASSERT_EQ(decode_client_tool_execute(WireRoute::Message,
                                         replace_field(theme_command(), R"("toolCallId":"call")",
                                                       R"("toolCallId":"new-call")"),
                                         changed),
              WireResult::Ok);
    EXPECT_EQ(executor.accept(changed, scope, 104, 1004), ClientToolDecision::Denied);
    EXPECT_EQ(display.requests, 1);
}
// ClientToolTest.TimeoutOriginalBudgetAndLateFact
// 原两秒/更早期限固定unknown，迟到成功不能重写或再次请求。
TEST(ClientToolTest, TimeoutOriginalBudgetAndLateFact) {
    for (const auto expiry : {"11000", "1500"}) {
        ThemeDisplay display;
        ClientToolExecutor executor(display, "instance");
        ClientToolCommand command;
        ASSERT_EQ(decode_client_tool_execute(
                      WireRoute::Message, replace_field(theme_command(), "11000", expiry), command),
                  WireResult::Ok);
        const auto deadline = std::string_view(expiry) == "1500" ? 600U : 2100U;
        ASSERT_EQ(executor.accept(command, {"session", "original", true}, 100, 1000),
                  ClientToolDecision::Accepted);
        executor.poll(deadline - 1);
        EXPECT_FALSE(executor.take_submission());
        display.fact = ClientToolFact{display.generation, "succeeded", "dark"};
        executor.poll(deadline);
        const auto result = executor.take_submission();
        ASSERT_TRUE(result);
        EXPECT_EQ(result->status, "unknown");
        EXPECT_EQ(result->envelope.payload.find(R"("theme")"), std::string::npos);
        executor.poll(deadline + 1);
        EXPECT_FALSE(executor.take_submission());
        EXPECT_EQ(display.requests, 1);
    }
}
// ClientToolTest.WrongGenerationAndActualMismatch
// 错票据保持等待，实际主题不匹配只得到unknown而非success。
TEST(ClientToolTest, WrongGenerationAndActualMismatch) {
    ThemeDisplay display;
    ClientToolExecutor executor(display, "instance");
    ASSERT_EQ(executor.accept(decoded_command(), {"session", "original", true}, 100, 1000),
              ClientToolDecision::Accepted);
    display.fact = ClientToolFact{display.generation + 1, "succeeded", "dark"};
    executor.poll(101);
    EXPECT_FALSE(executor.take_submission());
    display.fact = ClientToolFact{display.generation, "succeeded", "light"};
    executor.poll(102);
    const auto result = executor.take_submission();
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status, "unknown");
}
// ClientToolTest.CancelOnlyOriginalResponse
// 只撤销原等待，不借迟到实际确认恢复取消的调用。
TEST(ClientToolTest, CancelOnlyOriginalResponse) {
    ThemeDisplay display;
    ClientToolExecutor executor(display, "instance");
    ASSERT_EQ(executor.accept(decoded_command(), {"session", "original", true}, 100, 1000),
              ClientToolDecision::Accepted);
    executor.cancel_response("session", "other");
    EXPECT_FALSE(executor.take_submission());
    executor.cancel_response("session", "original");
    const auto result = executor.take_submission();
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status, "unknown");
    display.fact = ClientToolFact{display.generation, "succeeded", "dark"};
    executor.poll(101);
    EXPECT_FALSE(executor.take_submission());
}
// ClientToolTest.ClockRollbackClosesOriginalWait
// 时钟回退不得恢复新预算或接纳另一次副作用。
TEST(ClientToolTest, ClockRollbackClosesOriginalWait) {
    ThemeDisplay display;
    ClientToolExecutor executor(display, "instance");
    const auto command = decoded_command();
    ASSERT_EQ(executor.accept(command, {"session", "original", true}, 100, 1000),
              ClientToolDecision::Accepted);
    executor.poll(99);
    const auto result = executor.take_submission();
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status, "unknown");
    EXPECT_EQ(executor.accept(command, {"session", "original", true}, 101, 1001),
              ClientToolDecision::Invalid);
    EXPECT_EQ(display.requests, 1);
}
// ClientToolTest.ActualDriverReject
// 明确驱动拒绝得到failed，但不会调用其他动作或伪造实际主题。
TEST(ClientToolTest, ActualDriverReject) {
    ThemeDisplay display;
    display.accepted = false;
    ClientToolExecutor executor(display, "instance");
    ASSERT_EQ(executor.accept(decoded_command(), {"session", "original", true}, 100, 1000),
              ClientToolDecision::Accepted);
    const auto result = executor.take_submission();
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status, "failed");
    EXPECT_EQ(display.requests, 1);
}
// ClientToolTest.EveryFixedTerminalRetiresOriginalGenerationOnce
// 取消/会话关闭/截止/坏时钟/拒绝及实际终态都释放原代次；重放和重复取消不再次退休。
TEST(ClientToolTest, EveryFixedTerminalRetiresOriginalGenerationOnce) {
    for (const auto scenario :
         {"succeeded", "failed", "unknown", "cancel", "close", "timeout", "rollback", "reject"}) {
        SCOPED_TRACE(scenario);
        ThemeDisplay display;  // 各分支独占原末端，副作用始终只请求一次。
        display.accepted = std::string_view(scenario) != "reject";
        ClientToolExecutor executor(display, "instance");
        const auto command = decoded_command();  // 真正Execute编解码，不伪造类型化授权。
        ASSERT_EQ(executor.accept(command, {"session", "original", true}, 100, 1000),
                  ClientToolDecision::Accepted);
        if (std::string_view(scenario) == "cancel") {
            executor.cancel_response("session", "other");
            ASSERT_TRUE(display.retired.empty());
            executor.cancel_response("session", "original");
        } else if (std::string_view(scenario) == "close") {
            executor.close_session("other");
            ASSERT_TRUE(display.retired.empty());
            executor.close_session("session");
        } else if (std::string_view(scenario) == "timeout") {
            executor.poll(2100);
        } else if (std::string_view(scenario) == "rollback") {
            executor.poll(99);
        } else if (std::string_view(scenario) != "reject") {
            display.fact = ClientToolFact{display.generation, scenario,
                                          std::string_view(scenario) == "succeeded" ? "dark" : ""};
            executor.poll(101);
        }
        const auto original = executor.take_submission();  // 保留原固定状态，不把退休当成功。
        ASSERT_TRUE(original);
        EXPECT_EQ(original->status,
                  std::string_view(scenario) == "succeeded" ? "succeeded"
                  : std::string_view(scenario) == "failed" || std::string_view(scenario) == "reject"
                      ? "failed"
                      : "unknown");
        ASSERT_EQ(display.retired.size(), 1U);
        EXPECT_EQ(display.retired.front(), display.generation);
        executor.cancel_response("session", "original");
        executor.close_session("session");
        executor.poll(2200);
        EXPECT_FALSE(executor.take_submission());
        if (std::string_view(scenario) != "rollback") {
            ASSERT_EQ(executor.accept(command, {"session", "original", true}, 2201, 3101),
                      ClientToolDecision::Replay);
            const auto replay = executor.take_submission();  // 重放原结果字节，不重发副作用。
            ASSERT_TRUE(replay);
            EXPECT_EQ(replay->envelope.token, original->envelope.token);
            EXPECT_EQ(replay->envelope.payload, original->envelope.payload);
        }
        EXPECT_EQ(display.retired.size(), 1U);
        EXPECT_EQ(display.requests, 1);
    }
}
}  // namespace
}  // namespace ur::device
