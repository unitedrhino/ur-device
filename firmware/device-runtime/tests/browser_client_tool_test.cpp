// 文件说明：浏览器主题驱动原票据回执回归；不包含真实DOM/渲染或平台模型调用。
#include "browser_client_tool.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <deque>
#include <vector>
#include "ur/device/client_tool_endpoint.hpp"
#include "ur/device/voice_runtime.hpp"

namespace ur::device {
namespace {
// PendingThemeMqtt只替换外部网络；报文仍经真实Runtime/Endpoint/Executor消费，原FIFO不乱序。
class PendingThemeMqtt final : public MqttPort {
public:
    std::deque<MqttEvent> events;  // 单次poll可同时取到原Execute和后续Done。
    std::vector<Envelope> sent;    // 仅合成上行请求，用来核实退休后没有Submit。
    int subscription = 1;          // 原SIG/MSG分别确认SUBACK。
    bool connect(const MqttConfig&) override { return true; }
    int subscribe(std::string_view) override { return subscription++; }
    bool poll() override { return true; }
    bool publications_drained() override { return true; }
    void disconnect() override { events.clear(); }
    std::optional<MqttEvent> take_event() override {
        if (events.empty())
            return std::nullopt;
        auto event = std::move(events.front());
        events.pop_front();
        return event;
    }
    bool publish(std::string_view, std::string_view body) override {
        Envelope request;  // 不绕过正式线协议解析，也不制造设备ACK。
        if (decode_envelope(body, request) != DecodeResult::Ok)
            return false;
        sent.push_back(std::move(request));
        return true;
    }
};
// pending_theme_command构造合成原回复/调用，仍经真正Execute解析授权字段，不借展示内容执行。
ClientToolCommand pending_theme_command(std::string_view response, std::string_view call,
                                        std::uint64_t expiry = 11000) {
    Envelope event;  // 仅固定合成标识，原工具schema保持闭合且不包含脚本。
    event.token = "execute";
    event.method = "toolCallExecute";
    event.session = "session";
    event.request = false;
    event.message_sequence = 3;
    event.payload = "{\"respId\":\"" + std::string(response) + "\",\"toolCallId\":\"" +
                    std::string(call) +
                    "\",\"name\":\"set_local_display_theme\",\"arguments\":{\"theme\":\"dark\"},"
                    "\"isFrontend\":true,\"interactionType\":\"action\","
                    "\"executionToken\":\"synthetic-token\",\"expiresAt\":" +
                    std::to_string(expiry) + "}";
    std::string body;           // 原规范网络信封。
    ClientToolCommand command;  // 只返回正式解析器接纳的执行命令。
    EXPECT_EQ(encode_envelope(event, body), DecodeResult::Ok);
    EXPECT_EQ(decode_client_tool_execute(WireRoute::Message, body, command), WireResult::Ok);
    return command;
}
}  // namespace
// BrowserClientToolTest.FifoDoneRetiresPendingThemeBeforeHostDispatch
// 真实poll先接纳Execute、再退休原回复；尚未交DOM的命令不得在宿主随后take_command时复活副作用。
TEST(BrowserClientToolTest, FifoDoneRetiresPendingThemeBeforeHostDispatch) {
    PendingThemeMqtt mqtt;             // 外部MQTT端口，保持正常FIFO。
    BrowserClientToolDisplay display;  // 生产桌面驱动，不用同步成功假端口。
    ASSERT_TRUE(display.receive_ready(R"({"kind":"clientThemeReady","schemaRevision":1})"));
    VoiceCapability capability;  // 真无媒体文字会话，主题能力必须独立预检。
    capability.transport = VoiceTransport::None;
    capability.text_input = true;
    capability.client_display_theme = true;
    VoiceRuntime runtime(mqtt, "p/d", capability, "process", nullptr, &display);
    std::uint64_t now = 100;  // 固定时钟，不等待墙钟、不借超时取消。
    // enqueue只编码后放入原网络队列；是否调用poll由测试明确决定。
    const auto enqueue = [&](Envelope event, bool message) {
        std::string body;  // 仅合成规范报文，不写身份或响应正文日志。
        ASSERT_EQ(encode_envelope(event, body), DecodeResult::Ok);
        mqtt.events.push_back({MqttEvent::Kind::Message, 0, 0,
                               message ? "$thing/down/message/p/d" : "$thing/down/signaling/p/d",
                               std::move(body)});
    };
    // advance沿原Runtime入口推进，末端命令始终未被宿主提取或发送。
    const auto advance = [&]() {
        ++now;
        runtime.poll(now, 1000 + now);
    };
    ASSERT_EQ(runtime.start({}, now), Result::Applied);
    mqtt.events.push_back({MqttEvent::Kind::Connected, 0, 0, {}, {}});
    mqtt.events.push_back({MqttEvent::Kind::Subscribed, 1, 0, {}, {}});
    mqtt.events.push_back({MqttEvent::Kind::Subscribed, 2, 0, {}, {}});
    advance();
    ASSERT_EQ(mqtt.sent.size(), 1U);
    auto ack = mqtt.sent.back();  // 原能力请求应答，不能由工具命令自行建会话。
    ack.request = false;
    ack.code = 200;
    ack.method = "requestAck";
    ack.payload = R"({"requestMethod":"capabilityReport","result":{"acceptedRevision":1}})";
    enqueue(ack, false);
    advance();
    ASSERT_EQ(mqtt.sent.size(), 2U);
    ack = mqtt.sent.back();
    ack.request = false;
    ack.code = 200;
    ack.method = "sessionCreated";
    ack.session = "session";
    ack.payload =
        R"({"state":"negotiating","selected":{"transport":"none","inputModalities":["text"],"outputModalities":["text"]},"tracks":[]})";
    enqueue(ack, false);
    advance();
    Envelope event;  // 原建立事件及后续内容都保留session，MSG水位单独递增。
    event.token = "established";
    event.session = "session";
    event.method = "sessionEstablished";
    event.request = false;
    event.event_sequence = 1;
    event.payload = R"({"state":"established","mediaRevision":0})";
    enqueue(event, false);
    advance();
    ASSERT_EQ(runtime.voice().state(), VoiceState::Established);
    ASSERT_EQ(runtime.send_text("合成主题请求", false, ++now), Result::Applied);
    const auto input = runtime.voice().text_input_id();  // 原输入关联，不猜当前播放回复。
    ASSERT_FALSE(input.empty());
    ack = mqtt.sent.back();
    ack.request = false;
    ack.code = 200;
    ack.method = "requestAck";
    ack.payload = "{\"requestMethod\":\"inputSend\",\"result\":{\"inputId\":\"" + input +
                  "\",\"state\":\"accepted\"}}";
    enqueue(ack, true);
    advance();
    ASSERT_TRUE(runtime.take_text_result());
    event.event_sequence.reset();
    event.token = "created";
    event.method = "respCreated";
    event.message_sequence = 1;
    event.payload = "{\"respId\":\"reply\",\"inputId\":\"" + input + "\"}";
    enqueue(event, true);
    advance();
    ASSERT_TRUE(runtime.take_content());
    event.token = "start";
    event.method = "toolCallStart";
    event.message_sequence = 2;
    event.payload =
        R"({"respId":"reply","toolCallId":"call","name":"set_local_display_theme","arguments":{}})";
    enqueue(event, true);
    advance();
    ASSERT_TRUE(runtime.take_content());
    runtime.enable_message_diagnostics();
    event.token = "execute";
    event.method = "toolCallExecute";
    event.message_sequence = 3;
    event.payload =
        R"({"respId":"reply","toolCallId":"call","name":"set_local_display_theme","arguments":{"theme":"dark"},"isFrontend":true,"interactionType":"action","executionToken":"synthetic-token","expiresAt":20000})";
    enqueue(event, true);
    event.token = "done";
    event.method = "respDone";
    event.message_sequence = 4;
    event.payload = "{\"respId\":\"reply\",\"inputId\":\"" + input +
                    "\",\"status\":\"failed\",\"reason\":\"incompleteReply\","
                    "\"contentCount\":0,\"audioState\":\"none\"}";
    enqueue(event, true);
    ASSERT_EQ(mqtt.events.size(), 2U);
    advance();  // 同一次生产poll，Execute严格先于Done，不插入DOM回执或宿主派发。
    ASSERT_EQ(runtime.voice().state(), VoiceState::Established);
    ASSERT_EQ(runtime.state(), RuntimeState::Running);
    ASSERT_TRUE(mqtt.events.empty());
    const auto facts =
        runtime.take_message_diagnostics();  // 证明确实接纳Execute而非解析/水位拒绝。
    ASSERT_TRUE(facts);
    ASSERT_EQ(facts->size, 2U);
    ASSERT_EQ(facts->records[0].outcome, MessageDiagnosticOutcome::ExecuteAccepted);
    ASSERT_EQ(facts->records[1].method, "respDone");
    ASSERT_EQ(facts->records[1].outcome, MessageDiagnosticOutcome::ContentQueued);
    EXPECT_EQ(std::count_if(mqtt.sent.begin(), mqtt.sent.end(),
                            [](const auto& request) { return request.method == "toolCallSubmit"; }),
              0);
    EXPECT_FALSE(display.take_command());  // 宿主原take_command边界不得拿到已退休副作用。
}
// BrowserClientToolTest.FixedSubmitReceipt不含参数/结果/schema/令牌；省略续答不能冒成功。
TEST(BrowserClientToolTest, FixedSubmitReceipt) {
    ClientToolReceipt receipt{"session", "reply", "call", "unknown", 200, 1};
    const auto body = browser_client_tool_submitted(receipt);
    EXPECT_NE(body.find("\"kind\":\"clientToolSubmitted\""), std::string::npos);
    EXPECT_NE(body.find("\"requestMethod\":\"toolCallSubmit\""), std::string::npos);
    EXPECT_NE(body.find("\"continuation\":\"unknown\""), std::string::npos);
    EXPECT_EQ(body.find("arguments"), std::string::npos);
    EXPECT_EQ(body.find("executionToken"), std::string::npos);
    receipt.continuation = "succeeded";
    EXPECT_TRUE(browser_client_tool_submitted(receipt).empty());
    receipt.continuation = "accepted";
    receipt.code = 403;
    EXPECT_TRUE(browser_client_tool_submitted(receipt).empty());
    receipt.continuation = "unknown";
    EXPECT_FALSE(browser_client_tool_submitted(receipt).empty());
    receipt.index = 513;
    EXPECT_TRUE(browser_client_tool_submitted(receipt).empty());
}
// BrowserClientToolTest.ExactReadiness只有配对页明确三字段能力的原修订预检能装配端口。
TEST(BrowserClientToolTest, ExactReadiness) {
    BrowserClientToolDisplay display;
    for (const auto* body :
         {R"({"kind":"clientThemeReady","schemaRevision":null})",
          R"({"kind":"clientThemeReady","schemaRevision":2})",
          R"({"kind":"clientThemeReady","schemaRevision":"1"})",
          R"({"kind":"clientThemeReady","schemaRevision":1,"script":"arbitrary"})",
          R"({"kind":"clientThemeReady","schemaRevision":1,"schemaRevision":1})"}) {
        EXPECT_FALSE(display.receive_ready(body));
        EXPECT_FALSE(display.available());
    }
    ASSERT_TRUE(display.receive_ready(R"({"kind":"clientThemeReady","schemaRevision":1})"));
    EXPECT_TRUE(display.available());
    EXPECT_FALSE(display.receive_ready(R"({"kind":"clientThemeReady","schemaRevision":1})"));
    display.close();
    EXPECT_FALSE(display.available());
    EXPECT_FALSE(display.receive_ready(R"({"kind":"clientThemeReady","schemaRevision":1})"));
}
// BrowserClientToolTest.DispatchThenOriginalActual
// 不能在未发命令前确认，错代次/附带平台身份不得确认。
TEST(BrowserClientToolTest, DispatchThenOriginalActual) {
    BrowserClientToolDisplay display;
    EXPECT_FALSE(display.request_theme("dark", 1));
    display.ready(true);
    ASSERT_TRUE(display.request_theme("dark", 1));
    const std::string actual =
        R"({"kind":"clientThemeActual","themeTicket":"1","status":"succeeded","theme":"dark"})";
    EXPECT_FALSE(display.receive(actual));
    const auto command = display.take_command();
    ASSERT_TRUE(command);
    EXPECT_EQ(*command, R"({"kind":"clientThemeSet","themeTicket":"1","theme":"dark"})");
    EXPECT_FALSE(display.receive(
        R"({"kind":"clientThemeActual","themeTicket":"01","status":"succeeded","theme":"dark"})"));
    EXPECT_FALSE(display.receive(
        R"({"kind":"clientThemeActual","themeTicket":"2","status":"succeeded","theme":"dark"})"));
    EXPECT_FALSE(display.receive(
        R"({"kind":"clientThemeActual","themeTicket":"1","status":"succeeded","theme":"dark","executionToken":"synthetic"})"));
    ASSERT_TRUE(display.receive(actual));
    EXPECT_FALSE(display.take_theme_fact(2));
    const auto fact = display.take_theme_fact(1);
    ASSERT_TRUE(fact);
    EXPECT_EQ(fact->status, "succeeded");
    EXPECT_EQ(fact->theme, "dark");
    EXPECT_FALSE(display.receive(actual));
}
// BrowserClientToolTest.ClosedConnectionIsUnknown
// 断连只能unknown，不复活旧驱动，也不把清理当已回滚主题。
TEST(BrowserClientToolTest, ClosedConnectionIsUnknown) {
    BrowserClientToolDisplay display;
    display.ready(true);
    ASSERT_TRUE(display.request_theme("light", 3));
    ASSERT_TRUE(display.take_command());
    display.close();
    const auto fact = display.take_theme_fact(3);
    ASSERT_TRUE(fact);
    EXPECT_EQ(fact->status, "unknown");
    EXPECT_TRUE(fact->theme.empty());
    display.ready(true);
    EXPECT_FALSE(display.request_theme("light", 4));
}
// BrowserClientToolTest.ActualThemeMismatchStaysUnknown
// 完整真实核心组合不能把页面不同主题读回当原请求成功。
TEST(BrowserClientToolTest, ActualThemeMismatchStaysUnknown) {
    BrowserClientToolDisplay display;
    display.ready(true);
    ClientToolExecutor executor(display, "instance");
    const auto body =
        R"({"msgToken":"event","method":"toolCallExecute","sessionId":"session","messageSeq":1,"data":{"respId":"original","toolCallId":"call","name":"set_local_display_theme","arguments":{"theme":"dark"},"isFrontend":true,"interactionType":"action","executionToken":"synthetic","expiresAt":5000}})";
    ClientToolCommand command;
    ASSERT_EQ(decode_client_tool_execute(WireRoute::Message, body, command), WireResult::Ok);
    ASSERT_EQ(executor.accept(command, {"session", "original", true}, 100, 1000),
              ClientToolDecision::Accepted);
    ASSERT_TRUE(display.take_command());
    ASSERT_TRUE(display.receive(
        R"({"kind":"clientThemeActual","themeTicket":"1","status":"succeeded","theme":"light"})"));
    executor.poll(101);
    const auto result = executor.take_submission();
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status, "unknown");
}
// BrowserClientToolTest.RetirementMatchesOriginalGeneration
// 旧取消不能清除新命令/实际事实；匹配退休只清占位，不让迟到票据复活副作用。
TEST(BrowserClientToolTest, RetirementMatchesOriginalGeneration) {
    BrowserClientToolDisplay display;  // 真正桌面异步票据存储。
    display.ready(true);
    ASSERT_TRUE(display.request_theme("dark", 2));
    display.retire_theme(1);
    ASSERT_TRUE(display.take_command());
    ASSERT_TRUE(display.receive(
        R"({"kind":"clientThemeActual","themeTicket":"2","status":"succeeded","theme":"dark"})"));
    display.retire_theme(1);
    ASSERT_FALSE(display.request_theme("light", 3));  // 未退休原事实仍不能被覆盖。
    display.retire_theme(2);
    EXPECT_FALSE(display.take_theme_fact(2));
    EXPECT_FALSE(display.receive(
        R"({"kind":"clientThemeActual","themeTicket":"2","status":"succeeded","theme":"dark"})"));
    ASSERT_TRUE(display.request_theme("light", 3));
    display.retire_theme(2);
    ASSERT_TRUE(display.take_command());
    display.retire_theme(3);
    EXPECT_FALSE(display.take_command());
}
// BrowserClientToolTest.DispatchedCancelRejectsLateActualAndPreservesNextRound
// 已派发不声称回滚；退休旧票据后拒绝迟到事实，下一原回复仍能只执行一次并提交实际成功。
TEST(BrowserClientToolTest, DispatchedCancelRejectsLateActualAndPreservesNextRound) {
    BrowserClientToolDisplay display;  // 原生产驱动，不伪造同步执行成功。
    display.ready(true);
    ClientToolExecutor executor(display, "instance");
    const auto first = pending_theme_command("original", "call");  // 第一轮独立原scope。
    ASSERT_EQ(executor.accept(first, {"session", "original", true}, 100, 1000),
              ClientToolDecision::Accepted);
    ASSERT_TRUE(display.take_command());  // 与宿主一致，旧命令已经交给派发边界。
    executor.cancel_response("session", "original");
    const auto cancelled = executor.take_submission();  // 不冒称已经回滚DOM或原副作用未发生。
    ASSERT_TRUE(cancelled);
    EXPECT_EQ(cancelled->status, "unknown");
    EXPECT_FALSE(display.receive(
        R"({"kind":"clientThemeActual","themeTicket":"1","status":"succeeded","theme":"dark"})"));
    const auto second = pending_theme_command("next", "next-call");  // 下一轮使用新回复/调用。
    ASSERT_EQ(executor.accept(second, {"session", "next", true}, 101, 1001),
              ClientToolDecision::Accepted);
    display.retire_theme(1);                      // 迟到旧取消不能撤销新generation。
    const auto command = display.take_command();  // 生产pending必须保持新票据。
    ASSERT_TRUE(command);
    EXPECT_NE(command->find(R"("themeTicket":"2")"), std::string::npos);
    EXPECT_FALSE(display.take_command());
    EXPECT_FALSE(display.receive(
        R"({"kind":"clientThemeActual","themeTicket":"1","status":"succeeded","theme":"dark"})"));
    ASSERT_TRUE(display.receive(
        R"({"kind":"clientThemeActual","themeTicket":"2","status":"succeeded","theme":"dark"})"));
    executor.poll(102);
    const auto completed = executor.take_submission();  // 只有真实同代读回才succeeded。
    ASSERT_TRUE(completed);
    EXPECT_EQ(completed->status, "succeeded");
    EXPECT_FALSE(executor.take_submission());
    EXPECT_FALSE(display.receive(
        R"({"kind":"clientThemeActual","themeTicket":"2","status":"succeeded","theme":"dark"})"));
    ASSERT_EQ(executor.accept(second, {"session", "next", true}, 103, 1003),
              ClientToolDecision::Replay);
    ASSERT_TRUE(executor.take_submission());
    EXPECT_FALSE(display.take_command());  // 原结果重放不能重新派发主题。
    const auto third = pending_theme_command("third", "third-call");  // 实际成功也释放下一轮占位。
    ASSERT_EQ(executor.accept(third, {"session", "third", true}, 104, 1004),
              ClientToolDecision::Accepted);
    ASSERT_TRUE(display.take_command());
}
// BrowserClientToolTest.OriginalDeadlineRetiresUndispatchedTheme
// 原期限比局部2秒更早时同样停止尚未派发命令，不把unknown当成功或延长下一轮预算。
TEST(BrowserClientToolTest, OriginalDeadlineRetiresUndispatchedTheme) {
    for (const auto expiry : {1500U, 11000U}) {
        SCOPED_TRACE(expiry);
        BrowserClientToolDisplay display;  // 每个期限分支独占生产驱动。
        display.ready(true);
        ClientToolExecutor executor(display, "instance");
        const auto command = pending_theme_command("original", "call", expiry);
        ASSERT_EQ(executor.accept(command, {"session", "original", true}, 100, 1000),
                  ClientToolDecision::Accepted);
        const auto deadline = expiry == 1500U ? 600U : 2100U;  // 固定原最早截止，不等待真实时间。
        executor.poll(deadline - 1);
        EXPECT_FALSE(executor.take_submission());
        EXPECT_FALSE(display.request_theme("light", 9));  // 原待发命令仍在途，不能覆盖。
        executor.poll(deadline);
        const auto result = executor.take_submission();
        ASSERT_TRUE(result);
        EXPECT_EQ(result->status, "unknown");
        EXPECT_FALSE(display.take_command());
        EXPECT_FALSE(display.receive(
            R"({"kind":"clientThemeActual","themeTicket":"1","status":"succeeded","theme":"dark"})"));
        const auto next = pending_theme_command("next", "next-call");  // 旧截止不阻塞新的原回复。
        ASSERT_EQ(
            executor.accept(next, {"session", "next", true}, deadline + 1, 1000 + deadline + 1),
            ClientToolDecision::Accepted);
        ASSERT_TRUE(display.take_command());
    }
}
}  // namespace ur::device
