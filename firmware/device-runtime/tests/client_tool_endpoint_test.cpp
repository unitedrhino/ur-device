// 文件说明：真实独立工具端点与原Requests组合；虚拟时钟验证原身份、同字节结果重传和取消/截止。
#include "ur/device/client_tool_endpoint.hpp"
#include <gtest/gtest.h>

namespace ur::device {
namespace {
// EndpointMqtt仅替代外部MQTT驱动，记录实际发布；PUBACK不产生业务确认。
struct EndpointMqtt final : MqttPort {
    std::vector<std::string> bodies, topics;  // 原线协议字节，仅合成测试数据。
    bool connect(const MqttConfig&) override { return true; }
    int subscribe(std::string_view) override { return 1; }
    bool poll() override { return true; }
    std::optional<MqttEvent> take_event() override { return {}; }
    void disconnect() override {}
    bool publish(std::string_view topic, std::string_view body) override {
        topics.emplace_back(topic);
        bodies.emplace_back(body);
        return true;
    }
};
// EndpointDisplay实际接纳仍保持pending，只有测试显式给出的原generation读回可完成。
struct EndpointDisplay final : ClientToolDisplayPort {
    unsigned calls = 0;                  // 唯一实际副作用次数。
    std::uint64_t generation = 0;        // 原票据，不借当前工具名匹配。
    std::optional<ClientToolFact> fact;  // 实际观察独立于驱动接纳。
    bool request_theme(std::string_view, std::uint64_t ticket) override {
        ++calls;
        generation = ticket;
        return true;
    }
    std::optional<ClientToolFact> take_theme_fact(std::uint64_t) override {
        const auto original = fact;
        fact.reset();
        return original;
    }
};
// endpoint_content实际解析合成原内容；不能凭Endpoint类型化参数绕过原MSG合同。
ContentEvent endpoint_content(std::string method, std::string data) {
    const std::string body = "{\"msgToken\":\"content\",\"method\":\"" + method +
                             "\",\"sessionId\":\"session\",\"messageSeq\":1,\"data\":" + data + "}";
    ContentEvent content;
    EXPECT_EQ(decode_content_event(WireRoute::Message, body, content), WireResult::Ok);
    return content;
}
// endpoint_scope先建立原回复与原callStart；执行许可另须平台registry、实际端能力及活会话。
void endpoint_scope(ClientToolEndpoint& endpoint) {
    ASSERT_TRUE(endpoint.observe(
        endpoint_content("respCreated", R"({"respId":"reply","inputId":"input"})")));
    ASSERT_TRUE(endpoint.observe(endpoint_content(
        "toolCallStart",
        R"({"respId":"reply","toolCallId":"call","name":"set_local_display_theme","arguments":{}})")));
}
// endpoint_execute只含固定审核工具及合成令牌，expires不从客户端当前时间续期。
std::string endpoint_execute(std::uint64_t expiry = 16000) {
    return R"({"msgToken":"execute","method":"toolCallExecute","sessionId":"session","messageSeq":3,"data":{"respId":"reply","toolCallId":"call","name":"set_local_display_theme","arguments":{"theme":"dark"},"isFrontend":true,"interactionType":"action","executionToken":"synthetic-token","expiresAt":)" +
           std::to_string(expiry) + "}}";
}
// endpoint_ack从实际Submit取原token/session，回复与call仍必须严格同原结果。
std::string endpoint_ack(const std::string& submit, std::string response = "reply",
                         std::string call = "call", std::string continuation = "") {
    Envelope original;
    EXPECT_EQ(decode_envelope(submit, original), DecodeResult::Ok);
    return "{\"msgToken\":\"" + original.token + "\",\"method\":\"requestAck\",\"sessionId\":\"" +
           original.session +
           "\",\"code\":200,\"data\":{\"requestMethod\":\"toolCallSubmit\","
           "\"result\":{\"respId\":\"" +
           response + "\",\"toolCallId\":\"" + call + "\",\"state\":\"accepted\"" + continuation +
           "}}}";
}
// ClientToolEndpointTest.OriginalScopeAndResult只在原回复、开始调用和实际读回齐备时Submit。
TEST(ClientToolEndpointTest, OriginalScopeAndResult) {
    EndpointMqtt mqtt;
    EndpointDisplay display;
    ClientToolEndpoint endpoint(mqtt, display, "p/d", "process");
    EXPECT_EQ(endpoint.receive_execute(endpoint_execute(), "session", true, 100, 1000),
              ClientToolDecision::Denied);
    endpoint_scope(endpoint);
    EXPECT_EQ(endpoint.receive_execute(endpoint_execute(), "other", true, 100, 1000),
              ClientToolDecision::Denied);
    EXPECT_EQ(endpoint.receive_execute(endpoint_execute(), "session", false, 100, 1000),
              ClientToolDecision::Denied);
    ASSERT_EQ(endpoint.receive_execute(endpoint_execute(), "session", true, 100, 1000),
              ClientToolDecision::Accepted);
    ASSERT_TRUE(endpoint.poll(101));
    EXPECT_TRUE(mqtt.bodies.empty());
    display.fact = ClientToolFact{display.generation, "succeeded", "dark"};
    ASSERT_TRUE(endpoint.poll(102));
    ASSERT_EQ(mqtt.bodies.size(), 1U);
    EXPECT_EQ(mqtt.topics[0], "$thing/up/message/p/d");
    EXPECT_EQ(display.calls, 1U);
    EXPECT_FALSE(endpoint.take_receipt());
    EXPECT_FALSE(endpoint.receive_ack(endpoint_ack(mqtt.bodies[0], "other"), 103));
    EXPECT_FALSE(endpoint.receive_ack(endpoint_ack(mqtt.bodies[0], "reply", "other"), 103));
    EXPECT_FALSE(endpoint.take_receipt());
    EXPECT_TRUE(endpoint.receive_ack(endpoint_ack(mqtt.bodies[0]),
                                     104));  // 可选continuation缺省不冒模型成功。
    EXPECT_FALSE(endpoint.receive_ack(endpoint_ack(mqtt.bodies[0]), 105));
    const auto receipt = endpoint.take_receipt();
    ASSERT_TRUE(receipt);
    EXPECT_EQ(receipt->session, "session");
    EXPECT_EQ(receipt->response, "reply");
    EXPECT_EQ(receipt->call, "call");
    EXPECT_EQ(receipt->code, 200);
    EXPECT_EQ(receipt->continuation, "unknown");
    EXPECT_EQ(receipt->index, 1U);
    EXPECT_FALSE(endpoint.take_receipt());
    ASSERT_TRUE(endpoint.poll(10102));
    EXPECT_EQ(mqtt.bodies.size(), 1U);
}
// ClientToolEndpointTest.ResultRetryBytes不会重做主题；原deadline早于第二次重试时丢弃传输。
TEST(ClientToolEndpointTest, ResultRetryBytes) {
    EndpointMqtt mqtt;
    EndpointDisplay display;
    ClientToolEndpoint endpoint(mqtt, display, "p/d", "process");
    endpoint_scope(endpoint);
    ASSERT_EQ(endpoint.receive_execute(endpoint_execute(), "session", true, 100, 1000),
              ClientToolDecision::Accepted);
    display.fact = ClientToolFact{display.generation, "succeeded", "dark"};
    ASSERT_TRUE(endpoint.poll(102));
    ASSERT_TRUE(endpoint.poll(10102));
    ASSERT_EQ(mqtt.bodies.size(), 2U);
    EXPECT_EQ(mqtt.bodies[0], mqtt.bodies[1]);
    EXPECT_EQ(display.calls, 1U);
    ASSERT_TRUE(endpoint.poll(20102));
    EXPECT_EQ(mqtt.bodies.size(), 2U);
    EXPECT_EQ(display.calls, 1U);
}
// ClientToolEndpointTest.DeadlineBeforeFirstRetry原授权先截止，尚排队的第一次重传也不能实际发布。
TEST(ClientToolEndpointTest, DeadlineBeforeFirstRetry) {
    EndpointMqtt mqtt;
    EndpointDisplay display;
    ClientToolEndpoint endpoint(mqtt, display, "p/d", "process");
    endpoint_scope(endpoint);
    ASSERT_EQ(endpoint.receive_execute(endpoint_execute(11000), "session", true, 100, 1000),
              ClientToolDecision::Accepted);
    display.fact = ClientToolFact{display.generation, "succeeded", "dark"};
    ASSERT_TRUE(endpoint.poll(102));
    ASSERT_TRUE(endpoint.poll(10102));
    ASSERT_EQ(mqtt.bodies.size(), 1U);
    EXPECT_FALSE(endpoint.receive_ack(endpoint_ack(mqtt.bodies[0]), 10102));
    EXPECT_FALSE(endpoint.take_receipt());
}
// ClientToolEndpointTest.CancelDropsQueuedRetry仅原回复取消，已排队同字节重传不得越过终态。
TEST(ClientToolEndpointTest, CancelDropsQueuedRetry) {
    EndpointMqtt mqtt;
    EndpointDisplay display;
    ClientToolEndpoint endpoint(mqtt, display, "p/d", "process");
    endpoint_scope(endpoint);
    ASSERT_EQ(endpoint.receive_execute(endpoint_execute(), "session", true, 100, 1000),
              ClientToolDecision::Accepted);
    display.fact = ClientToolFact{display.generation, "succeeded", "dark"};
    ASSERT_TRUE(endpoint.poll(102));
    endpoint.cancel_response("session", "reply");
    ASSERT_TRUE(endpoint.poll(10102));
    ASSERT_EQ(mqtt.bodies.size(), 1U);
    EXPECT_FALSE(endpoint.receive_ack(endpoint_ack(mqtt.bodies[0]), 10102));
    EXPECT_FALSE(endpoint.take_receipt());
    EXPECT_EQ(endpoint.receive_execute(endpoint_execute(), "session", true, 10102, 11002),
              ClientToolDecision::Denied);
    EXPECT_EQ(display.calls, 1U);
}
// ClientToolEndpointTest.CancelTombstoneIsNotNormalCompletion正常终态仅封工具，不拒绝跨通道迟到音频。
TEST(ClientToolEndpointTest, CancelTombstoneIsNotNormalCompletion) {
    EndpointMqtt mqtt;
    EndpointDisplay display;
    ClientToolEndpoint endpoint(mqtt, display, "p/d", "process");
    endpoint_scope(endpoint);
    EXPECT_FALSE(endpoint.response_cancelled("session", "reply"));
    EXPECT_FALSE(endpoint.response_cancelled("session", "missing"));
    ASSERT_TRUE(endpoint.observe(endpoint_content(
        "respDone",
        R"({"respId":"reply","inputId":"input","status":"completed","contentCount":0,"audioState":"none"})")));
    EXPECT_FALSE(endpoint.response_cancelled("session", "reply"));
    endpoint.cancel_response("other", "reply");
    EXPECT_FALSE(endpoint.response_cancelled("session", "reply"));
    endpoint.cancel_response("session", "missing");
    EXPECT_FALSE(endpoint.response_cancelled("session", "missing"));
    endpoint.cancel_response("session", "reply");
    EXPECT_TRUE(endpoint.response_cancelled("session", "reply"));
    EXPECT_FALSE(endpoint.response_cancelled("other", "reply"));
    // 交换session/response不能借已取消scope产生拒绝事实。
    EXPECT_FALSE(endpoint.response_cancelled("reply", "session"));
    ASSERT_TRUE(endpoint.observe(
        endpoint_content("respCreated", R"({"respId":"new-reply","inputId":"new-input"})")));
    EXPECT_TRUE(endpoint.response_cancelled("session", "reply"));
    EXPECT_FALSE(endpoint.response_cancelled("session", "new-reply"));
    endpoint.close_session("session", 100);
    EXPECT_TRUE(endpoint.response_cancelled("session", "reply"));
    EXPECT_FALSE(endpoint.response_cancelled("session", "new-reply"));
    EXPECT_EQ(display.calls, 0U);
}
// ClientToolEndpointTest.ToolDoneAndSessionClosed原工具终态/会话关闭不能借开始墓碑重新执行。
TEST(ClientToolEndpointTest, ToolDoneAndSessionClosed) {
    EndpointMqtt mqtt;
    EndpointDisplay display;
    ClientToolEndpoint endpoint(mqtt, display, "p/d", "process");
    endpoint_scope(endpoint);
    ASSERT_TRUE(endpoint.observe(endpoint_content(
        "toolCallResult",
        R"({"respId":"reply","toolCallId":"call","state":"unknown","result":{}})")));
    EXPECT_EQ(endpoint.receive_execute(endpoint_execute(), "session", true, 100, 1000),
              ClientToolDecision::Denied);
    endpoint.close_session("session", 101);
    EXPECT_EQ(endpoint.receive_execute(endpoint_execute(), "session", true, 102, 1002),
              ClientToolDecision::Denied);
    EXPECT_EQ(display.calls, 0U);
}
// ClientToolEndpointTest.ConnectionCloseRetiresQueuedResult网络/原VoiceClient已清身份时仍退休全部原等待。
TEST(ClientToolEndpointTest, ConnectionCloseRetiresQueuedResult) {
    EndpointMqtt mqtt;
    EndpointDisplay display;
    ClientToolEndpoint endpoint(mqtt, display, "p/d", "process");
    endpoint_scope(endpoint);
    ASSERT_EQ(endpoint.receive_execute(endpoint_execute(), "session", true, 100, 1000),
              ClientToolDecision::Accepted);
    display.fact = ClientToolFact{display.generation, "succeeded", "dark"};
    ASSERT_TRUE(endpoint.poll(102));
    ASSERT_EQ(mqtt.bodies.size(), 1U);
    endpoint.close(103);
    ASSERT_TRUE(endpoint.poll(10102));
    EXPECT_EQ(mqtt.bodies.size(), 1U);
    EXPECT_FALSE(endpoint.receive_ack(endpoint_ack(mqtt.bodies[0]), 10102));
    EXPECT_EQ(endpoint.receive_execute(endpoint_execute(), "session", true, 10103, 11003),
              ClientToolDecision::Denied);
    EXPECT_EQ(display.calls, 1U);
}
// ClientToolEndpointTest.OnlyOriginalAckProducesObservation错token/方法/过期或迟到ACK不计，accepted续答必须显式给出。
TEST(ClientToolEndpointTest, OnlyOriginalAckProducesObservation) {
    EndpointMqtt mqtt;
    EndpointDisplay display;
    ClientToolEndpoint endpoint(mqtt, display, "p/d", "process");
    endpoint_scope(endpoint);
    ASSERT_EQ(endpoint.receive_execute(endpoint_execute(), "session", true, 100, 1000),
              ClientToolDecision::Accepted);
    display.fact = ClientToolFact{display.generation, "succeeded", "dark"};
    ASSERT_TRUE(endpoint.poll(102));
    const auto original =
        endpoint_ack(mqtt.bodies[0], "reply", "call", ",\"continuation\":\"accepted\"");
    auto wrong = original;
    const auto position = wrong.find("\"msgToken\":\"") + 12;
    wrong.insert(position, "wrong-");
    EXPECT_FALSE(endpoint.receive_ack(wrong, 103));
    EXPECT_FALSE(endpoint.take_receipt());
    wrong = original;
    wrong.replace(wrong.find("toolCallSubmit"), 14, "audioStart");
    EXPECT_FALSE(endpoint.receive_ack(wrong, 103));
    EXPECT_FALSE(endpoint.take_receipt());
    ASSERT_TRUE(endpoint.receive_ack(original, 104));
    const auto receipt = endpoint.take_receipt();
    ASSERT_TRUE(receipt);
    EXPECT_EQ(receipt->continuation, "accepted");
    EXPECT_FALSE(endpoint.receive_ack(original, 105));
    EXPECT_FALSE(endpoint.take_receipt());
}
}  // namespace
}  // namespace ur::device
