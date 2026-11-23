// 文件说明：真实无媒体VoiceRuntime与客户端工具端点的原MSG闭环；外部MQTT/主题驱动为可控端口。
#include <cJSON.h>
#include <gtest/gtest.h>
#include <algorithm>
#include <deque>
#include <memory>
#include "ur/device/voice_runtime.hpp"

namespace ur::device {
namespace {
// ToolRuntimeMqtt以真实报文编解码保存网络事实，不把发布受理当端完成或模型成功。
struct ToolRuntimeMqtt final : MqttPort {
    std::vector<Envelope> sent;    // 原规范请求，仅合成数据。
    std::deque<MqttEvent> events;  // 本测试独占到达序列。
    int subscription = 1;          // 两个精确Topic各有原SUBACK。
    bool connect(const MqttConfig&) override { return true; }
    int subscribe(std::string_view) override { return subscription++; }
    bool poll() override { return true; }
    bool publications_drained() override { return true; }
    std::optional<MqttEvent> take_event() override {
        if (events.empty())
            return {};
        auto event = std::move(events.front());
        events.pop_front();
        return event;
    }
    void disconnect() override { events.clear(); }
    bool publish(std::string_view, std::string_view body) override {
        Envelope request;
        EXPECT_EQ(decode_envelope(body, request), DecodeResult::Ok);
        sent.push_back(std::move(request));
        return true;
    }
};
// ToolRuntimeDisplay单次真实副作用请求，原generation实际事实另行显式给出。
struct ToolRuntimeDisplay final : ClientToolDisplayPort {
    unsigned calls = 0;                  // 用来证明重投不重执行。
    std::uint64_t generation = 0;        // 本地原票据。
    bool ready = true;                   // 显式独立主题端口预检，不借音频ready。
    std::optional<ClientToolFact> fact;  // 原末端观察。
    bool available() const override { return ready; }
    bool request_theme(std::string_view, std::uint64_t ticket) override {
        ++calls;
        generation = ticket;
        return true;
    }
    std::optional<ClientToolFact> take_theme_fact(std::uint64_t) override {
        auto result = fact;
        fact.reset();
        return result;
    }
};
// tool_none_capability只允许None文字与审核主题，默认端不广告工具。
VoiceCapability tool_none_capability() {
    VoiceCapability capability;
    capability.transport = VoiceTransport::None;
    capability.text_input = true;
    capability.client_display_theme = true;
    return capability;
}
// ToolRuntimeTest使用原首包ACK/建立事件及原输入准入，不从Execute自行建回复。
class ToolRuntimeTest : public ::testing::Test {
protected:
    ToolRuntimeMqtt mqtt;        // 外部网络端口。
    ToolRuntimeDisplay display;  // 实际主题端口。
    VoiceRuntime runtime{mqtt,      "p/d",   tool_none_capability(),
                         "process", nullptr, &display};  // 真无媒体构造。
    std::uint64_t now = 100;                             // 固定可控单调时钟。
    std::string original_input;                          // 原本地输入ID，不从工具命令自行创建回复。
    // deliver沿实际Runtime MSG/SIG分派，不直接调用Executor。
    void deliver(Envelope event, bool message = false) {
        std::string body;
        ASSERT_EQ(encode_envelope(event, body), DecodeResult::Ok);
        mqtt.events.push_back({MqttEvent::Kind::Message, 0, 0,
                               message ? "$thing/down/message/p/d" : "$thing/down/signaling/p/d",
                               body});
        ++now;
        runtime.poll(now, 1000 + now);
    }
    // event正文必须过公共编码器；水位独立于SIG建立事件。
    void content(std::string method, std::string body, std::uint64_t sequence) {
        Envelope event;
        event.token = "content-" + std::to_string(sequence);
        event.session = "session";
        event.method = std::move(method);
        event.payload = std::move(body);
        event.request = false;
        event.message_sequence = sequence;
        deliver(std::move(event), true);
    }
    // establish只有原创建应答和revision0事件都齐全才推进Established。
    void establish() {
        ASSERT_EQ(runtime.start({}, now), Result::Applied);
        mqtt.events.push_back({MqttEvent::Kind::Connected, 0, 0, {}, {}});
        mqtt.events.push_back({MqttEvent::Kind::Subscribed, 1, 0, {}, {}});
        mqtt.events.push_back({MqttEvent::Kind::Subscribed, 2, 0, {}, {}});
        ++now;
        runtime.poll(now, 1000 + now);
        ASSERT_EQ(mqtt.sent.size(), 1U);
        auto capability = mqtt.sent.back();
        capability.request = false;
        capability.code = 200;
        capability.method = "requestAck";
        capability.payload =
            R"({"requestMethod":"capabilityReport","result":{"acceptedRevision":1}})";
        deliver(std::move(capability));
        ASSERT_EQ(mqtt.sent.size(), 2U);
        auto created = mqtt.sent.back();
        created.request = false;
        created.code = 200;
        created.method = "sessionCreated";
        created.session = "session";
        created.payload =
            R"({"state":"negotiating","selected":{"transport":"none","inputModalities":["text"],"outputModalities":["text"]},"tracks":[]})";
        deliver(std::move(created));
        Envelope established;
        established.token = "established";
        established.method = "sessionEstablished";
        established.session = "session";
        established.request = false;
        established.event_sequence = 1;
        established.payload = R"({"state":"established","mediaRevision":0})";
        deliver(std::move(established));
        ASSERT_EQ(runtime.voice().state(), VoiceState::Established);
    }
    // original_reply原inputSend ACK先行，不能把云端任意respId当工具scope。
    void original_reply() {
        ASSERT_EQ(runtime.send_text("公开合成主题请求", false, ++now), Result::Applied);
        const auto request = mqtt.sent.back();
        std::unique_ptr<cJSON, decltype(&cJSON_Delete)> json(cJSON_Parse(request.payload.c_str()),
                                                             cJSON_Delete);
        const auto* input = cJSON_GetObjectItemCaseSensitive(json.get(), "inputId");
        ASSERT_TRUE(cJSON_IsString(input));
        const std::string input_id = input->valuestring;
        original_input = input_id;
        auto ack = request;
        ack.request = false;
        ack.code = 200;
        ack.method = "requestAck";
        ack.payload = "{\"requestMethod\":\"inputSend\",\"result\":{\"inputId\":\"" + input_id +
                      "\",\"state\":\"accepted\"}}";
        deliver(std::move(ack), true);
        ASSERT_TRUE(runtime.take_text_result());
        content("respCreated", "{\"respId\":\"reply\",\"inputId\":\"" + input_id + "\"}", 1);
        ASSERT_TRUE(runtime.take_content());
        content(
            "toolCallStart",
            R"({"respId":"reply","toolCallId":"call","name":"set_local_display_theme","arguments":{}})",
            2);
        ASSERT_TRUE(runtime.take_content());
    }
    // execute携带原三身份和有界期限，展示事件绝不能产生这条命令。
    void execute(std::uint64_t sequence = 3, std::uint64_t expiry = 20000) {
        content(
            "toolCallExecute",
            R"({"respId":"reply","toolCallId":"call","name":"set_local_display_theme","arguments":{"theme":"dark"},"isFrontend":true,"interactionType":"action","executionToken":"synthetic-token","expiresAt":)" +
                std::to_string(expiry) + "}",
            sequence);
    }
};
// ToolRuntimeTest.OriginalExecuteAndSubmit必须在!media早退之前推进实际工具；入队及展示不构成成功。
TEST_F(ToolRuntimeTest, OriginalExecuteAndSubmit) {
    establish();
    original_reply();
    execute();
    ASSERT_EQ(display.calls, 1U);
    EXPECT_EQ(mqtt.sent.back().method, "inputSend");
    EXPECT_FALSE(runtime.take_content());
    display.fact = ClientToolFact{display.generation, "succeeded", "dark"};
    ++now;
    runtime.poll(now, 1000 + now);
    ASSERT_EQ(mqtt.sent.back().method, "toolCallSubmit");
    const auto original = mqtt.sent.back();
    auto ack = original;
    ack.method = "requestAck";
    ack.request = false;
    ack.code = 200;
    ack.payload =
        R"({"requestMethod":"toolCallSubmit","result":{"respId":"reply","toolCallId":"call","state":"accepted"}})";
    deliver(std::move(ack), true);
    const auto receipt = runtime.take_client_tool_receipt();
    ASSERT_TRUE(receipt);
    EXPECT_EQ(receipt->response, "reply");
    EXPECT_EQ(receipt->call, "call");
    EXPECT_EQ(receipt->continuation, "unknown");
    EXPECT_FALSE(runtime.take_client_tool_receipt());
    EXPECT_EQ(runtime.voice().state(), VoiceState::Established);
    execute(4);
    EXPECT_EQ(display.calls, 1U);
    EXPECT_EQ(mqtt.sent.back().token, original.token);
}
// 原13条MSG全部可观察；Taken/Host只补原项，不挤占Execute/终态记录或多执行主题。
TEST_F(ToolRuntimeTest, MessageDiagnosticsKeepWholeThirteenMessageRound) {
    establish();
    runtime.enable_message_diagnostics();
    original_reply();
    for (std::uint64_t sequence = 3; sequence <= 10; ++sequence) {
        content("respTextDelta", R"({"respId":"reply","contentIndex":0,"delta":"private-text"})",
                sequence);
        auto text = runtime.take_content();
        ASSERT_TRUE(text);
        runtime.observe_content_display(*text, true, true);
    }
    execute(11);
    ASSERT_EQ(display.calls, 1U);
    content("respDone",
            "{\"respId\":\"reply\",\"inputId\":\"" + original_input +
                "\",\"status\":\"failed\",\"reason\":\"incompleteReply\","
                "\"contentCount\":0,\"audioState\":\"none\"}",
            12);
    auto done = runtime.take_content();
    ASSERT_TRUE(done);
    runtime.observe_content_display(*done, false, false);
    const auto facts = runtime.take_message_diagnostics();
    ASSERT_TRUE(facts);
    EXPECT_EQ(facts->received, 13U);
    ASSERT_EQ(facts->size, 13U);
    EXPECT_FALSE(facts->overflow);
    EXPECT_EQ(facts->records[11].outcome, MessageDiagnosticOutcome::ExecuteAccepted);
    EXPECT_EQ(facts->records[11].watermark, 10U);
    EXPECT_EQ(facts->records[12].method, "respDone");
    EXPECT_TRUE(facts->records[12].taken);
    EXPECT_TRUE(facts->records[12].host_observed);
    EXPECT_FALSE(facts->records[12].projected);
    EXPECT_EQ(display.calls, 1U);
}
// 独立Execute真实解码拒绝不混同权限拒绝，诊断不能使无效命令执行或令原会话失败。
TEST_F(ToolRuntimeTest, MessageDiagnosticsDistinguishExecuteParserRejection) {
    establish();
    original_reply();
    runtime.enable_message_diagnostics();
    content("toolCallExecute", R"({"respId":"private-id","arguments":"secret"})", 3);
    const auto facts = runtime.take_message_diagnostics();
    ASSERT_TRUE(facts);
    ASSERT_EQ(facts->size, 1U);
    EXPECT_EQ(facts->records[0].outcome, MessageDiagnosticOutcome::ExecuteInvalid);
    EXPECT_EQ(display.calls, 0U);
    EXPECT_EQ(runtime.voice().state(), VoiceState::Established);
}
// ToolRuntimeTest.CancelDropsOriginalResultRetry取消请求已接纳就封原scope，不等待ACK再重传端结果。
TEST_F(ToolRuntimeTest, CancelDropsOriginalResultRetry) {
    establish();
    original_reply();
    execute();
    display.fact = ClientToolFact{display.generation, "succeeded", "dark"};
    ++now;
    runtime.poll(now, 1000 + now);
    ASSERT_EQ(mqtt.sent.back().method, "toolCallSubmit");
    ASSERT_EQ(runtime.cancel_response(++now), Result::Applied);
    ASSERT_EQ(mqtt.sent.back().method, "respCancel");
    const auto submissions = [&]() {
        return std::count_if(mqtt.sent.begin(), mqtt.sent.end(), [](const auto& request) {
            return request.method == "toolCallSubmit";
        });
    };  // SIG取消自身可沿旧Requests重传，本回归只限制原工具Submit。
    const auto count = submissions();
    now += 10000;
    runtime.poll(now, 1000 + now);
    EXPECT_EQ(submissions(), count);
    execute(4);
    EXPECT_EQ(display.calls, 1U);
    EXPECT_EQ(submissions(), count);
}
// ToolRuntimeTest.OriginalDoneAndEndRetireTool迟到Execute不能复活原终态，关会话不能继续结果重传。
TEST_F(ToolRuntimeTest, OriginalDoneAndEndRetireTool) {
    establish();
    original_reply();
    content("respDone",
            "{\"respId\":\"reply\",\"inputId\":\"" + original_input +
                "\",\"status\":\"completed\",\"contentCount\":1,\"audioState\":"
                "\"none\"}",
            3);
    ASSERT_TRUE(runtime.take_content());
    execute(4);
    EXPECT_EQ(display.calls, 0U);
    ASSERT_EQ(runtime.end(++now), Result::Applied);
    execute(5);
    EXPECT_EQ(display.calls, 0U);
}
// ToolRuntimeTest.LocalDeadlineAndLateActual原驱动没有读回时2s固定unknown，迟到成功不重写端结果。
TEST_F(ToolRuntimeTest, LocalDeadlineAndLateActual) {
    establish();
    original_reply();
    execute();
    ASSERT_EQ(display.calls, 1U);
    now += 1999;
    runtime.poll(now, 1000 + now);
    EXPECT_EQ(mqtt.sent.back().method, "inputSend");
    ++now;
    runtime.poll(now, 1000 + now);
    ASSERT_EQ(mqtt.sent.back().method, "toolCallSubmit");
    EXPECT_NE(mqtt.sent.back().payload.find("\"status\":\"unknown\""), std::string::npos);
    const auto original = mqtt.sent.back();
    display.fact = ClientToolFact{display.generation, "succeeded", "dark"};
    ++now;
    runtime.poll(now, 1000 + now);
    execute(4);
    EXPECT_EQ(display.calls, 1U);
    EXPECT_EQ(mqtt.sent.back().payload, original.payload);
    EXPECT_EQ(mqtt.sent.back().token, original.token);
}
// ToolRuntimeTest.NoCapabilityWithoutActualDriver能力三字段不能凭空广告，真实驱动未ready也不得连接。
TEST_F(ToolRuntimeTest, NoCapabilityWithoutActualDriver) {
    display.ready = false;
    EXPECT_EQ(runtime.start({}, now), Result::Invalid);
    VoiceRuntime absent(mqtt, "p/d", tool_none_capability(), "other");
    EXPECT_EQ(absent.start({}, now), Result::Invalid);
    VoiceCapability plain = tool_none_capability();
    plain.client_display_theme = false;
    Envelope capability;
    ASSERT_EQ(capability_command("plain", plain, capability), WireResult::Ok);
    EXPECT_EQ(capability.payload.find("frontendTools"), std::string::npos);
    plain.client_display_theme = true;
    ASSERT_EQ(capability_command("known", plain, capability), WireResult::Ok);
    EXPECT_NE(
        capability.payload.find(
            R"("frontendTools":[{"interactionType":"action","name":"set_local_display_theme","schemaRevision":1}])"),
        std::string::npos);
    for (const auto transport :
         {VoiceTransport::None, VoiceTransport::Udp, VoiceTransport::WebRTC}) {
        plain.transport = transport;
        plain.audio = {transport == VoiceTransport::WebRTC ? 48000 : 16000, 1, 20};
        plain.modes = {true, false, false};
        for (bool advertised : {false, true}) {
            plain.client_display_theme = advertised;
            ASSERT_EQ(capability_command("cap", plain, capability), WireResult::Ok);
            EXPECT_EQ(capability.payload.find("frontendTools") != std::string::npos, advertised);
            ASSERT_EQ(create_voice_command("create", plain, capability), WireResult::Ok);
            EXPECT_EQ(capability.payload.find("frontendTools") != std::string::npos, advertised);
            EXPECT_EQ(capability.payload.find("schema\""), std::string::npos);
        }
    }
}
}  // namespace
}  // namespace ur::device
