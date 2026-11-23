// 文件说明：S21纯文字共用生产运行时回归；夹具只有MQTT，无音频驱动，覆盖逐轮关联、取消、断网及非法媒体扩权。
#include <cJSON.h>
#include <gtest/gtest.h>
#include <deque>
#include "ur/device/voice_runtime.hpp"

namespace ur::device {
namespace {
// TextMqtt提供可控真实类型网络事件；不注入语音、媒体Ready或空音频对象。
struct TextMqtt final : MqttPort {
    std::deque<MqttEvent> events;                              // 单线程明确到达顺序。
    std::vector<Envelope> sent;                                // 生产编码器实际发出的请求。
    std::vector<std::string> topics;                           // 每条请求的真实路由。
    int subscriptions = 0;                                     // 订阅mid基于本实例唯一递增。
    bool connected = true, polling = true, publishing = true;  // 可控传输失败。
    bool connect(const MqttConfig&) override { return connected; }
    bool poll() override { return polling; }
    int subscribe(std::string_view) override { return ++subscriptions; }
    bool publish(std::string_view topic, std::string_view body) override {
        Envelope request;
        EXPECT_EQ(decode_envelope(body, request), DecodeResult::Ok);
        sent.push_back(request);
        topics.emplace_back(topic);
        return publishing;
    }
    std::optional<MqttEvent> take_event() override {
        if (events.empty())
            return {};
        auto event = std::move(events.front());
        events.pop_front();
        return event;
    }
    void disconnect() override { events.clear(); }
};
// text_capability只声明真实文字输入，没有AEC、媒体轨或本地音频硬件。
VoiceCapability text_capability() {
    VoiceCapability capability;
    capability.transport = VoiceTransport::None;
    capability.text_input = true;
    return capability;
}
// input_id读取本端实际编码结果，测试不根据counter猜请求/输入关联。
std::string input_id(const Envelope& request) {
    auto* root = cJSON_Parse(request.payload.c_str());
    if (!root)
        return {};
    const auto* field = cJSON_GetObjectItemCaseSensitive(root, "inputId");
    const std::string result = cJSON_IsString(field) ? field->valuestring : "";
    cJSON_Delete(root);
    return result;
}
// TextOnlyTest复用订阅、能力ACK、建会话和持久建立事件；不跳过任何协议阶段。
class TextOnlyTest : public testing::Test {
protected:
    TextMqtt mqtt;  // 实际核心的唯一平台接口。
    VoiceRuntime runtime{mqtt, "p/d", text_capability(), "text-boot"};
    std::uint64_t now = 0;  // 可控单调时钟，无真实等待。
    // deliver编码公开合成下行并交原运行时消费，方向和水位由生产解析器验证。
    void deliver(Envelope envelope, bool message = false) {
        std::string body;
        ASSERT_EQ(encode_envelope(envelope, body), DecodeResult::Ok);
        mqtt.events.push_back({MqttEvent::Kind::Message, 0, 0,
                               message ? "$thing/down/message/p/d" : "$thing/down/signaling/p/d",
                               body});
        runtime.poll(++now);
    }
    // reply保持原token，失败不带伪成功result。
    void reply(const Envelope& request, std::string result, int code = 200) {
        Envelope envelope;
        envelope.token = request.token;
        envelope.session = request.session;
        envelope.method = "requestAck";
        envelope.request = false;
        envelope.code = code;
        envelope.payload = "{\"requestMethod\":\"" + request.method + "\"," +
                           (code == 200 ? "\"result\":" + result : "\"reason\":\"responseBusy\"") +
                           "}";
        deliver(std::move(envelope), request.method == "inputSend");
    }
    // negotiate在Created后保留等待真实Established的状态，没有mediaState上报。
    void negotiate() {
        ASSERT_EQ(runtime.start({}, now), Result::Applied);
        mqtt.events.push_back({MqttEvent::Kind::Connected, 0, 0, {}, {}});
        runtime.poll(++now);
        for (int mid = 1; mid <= 2; ++mid)
            mqtt.events.push_back({MqttEvent::Kind::Subscribed, mid, 0, {}, {}});
        runtime.poll(++now);
        ASSERT_EQ(mqtt.sent.size(), 1U);
        reply(mqtt.sent.back(), R"({"acceptedRevision":1})");
        ASSERT_EQ(mqtt.sent.back().method, "sessionCreate");
        Envelope created;
        created.token = mqtt.sent.back().token;
        created.session = "text-session";
        created.method = "sessionCreated";
        created.request = false;
        created.code = 200;
        created.payload =
            R"({"state":"negotiating","selected":{"transport":"none","inputModalities":["text"],"outputModalities":["text"]},"tracks":[]})";
        deliver(std::move(created));
        ASSERT_EQ(runtime.voice().state(), VoiceState::Negotiating);
    }
    // established只有原revision0事件推进；网络连接与本地发布不等同会话建立。
    void established(std::uint64_t revision = 0, std::uint64_t sequence = 1) {
        Envelope event;
        event.token = "established";
        event.session = "text-session";
        event.method = "sessionEstablished";
        event.request = false;
        event.event_sequence = sequence;
        event.payload =
            "{\"state\":\"established\",\"mediaRevision\":" + std::to_string(revision) + "}";
        deliver(std::move(event));
    }
    // content发送带原inputId/respId的文字内容，媒体字段只在非法反例显式添加。
    void content(std::string method, std::string data, std::uint64_t sequence,
                 std::string session = "text-session") {
        Envelope event;
        event.token = "content-" + std::to_string(sequence);
        event.session = std::move(session);
        event.method = std::move(method);
        event.request = false;
        event.message_sequence = sequence;
        event.payload = std::move(data);
        deliver(std::move(event), true);
    }
    // create_response绑定当前本地请求，ACK后仍不能把第二轮提前插入。
    std::string create_response(std::uint64_t sequence = 1, std::string resp = "reply-1") {
        EXPECT_EQ(runtime.send_text("公开合成文本", false, ++now), Result::Applied);
        const auto request = mqtt.sent.back();
        const auto input = input_id(request);
        reply(request, "{\"inputId\":\"" + input + "\",\"state\":\"accepted\"}");
        content("respCreated", "{\"respId\":\"" + resp + "\",\"inputId\":\"" + input + "\"}",
                sequence);
        return input;
    }
};
TEST_F(TextOnlyTest, EstablishmentRequiresOriginalRevisionZeroAndNeverCreatesMedia) {
    negotiate();
    EXPECT_EQ(runtime.send_text("尚未建立", false, ++now), Result::Stale);
    established(1);
    EXPECT_EQ(runtime.voice().state(), VoiceState::Negotiating);
    established(0);
    EXPECT_EQ(runtime.voice().state(), VoiceState::Established);
    EXPECT_EQ(mqtt.sent.size(), 2U);
    EXPECT_FALSE(runtime.voice().capturing());
    EXPECT_FALSE(runtime.voice().playing());
    EXPECT_EQ(runtime.start_input(++now), Result::Unsupported);
    EXPECT_EQ(runtime.stop_input(++now), Result::Unsupported);
    EXPECT_EQ(runtime.send_text("不要音频", true, ++now), Result::Unsupported);
    EXPECT_EQ(mqtt.sent.size(), 2U);
}
TEST_F(TextOnlyTest, MultiTurnContentRequiresMatchingInputResponseAndOriginalFinalState) {
    negotiate();
    established();
    const auto input = create_response();
    ASSERT_TRUE(runtime.take_text_result());
    ASSERT_TRUE(runtime.take_content());
    EXPECT_EQ(runtime.send_text("第二轮不能抢跑", false, ++now), Result::Busy);
    content("respDone",
            "{\"respId\":\"other\",\"inputId\":\"" + input +
                "\",\"status\":\"completed\",\"contentCount\":0,\"audioState\":\"none\"}",
            2);
    EXPECT_FALSE(runtime.take_content());
    EXPECT_EQ(runtime.send_text("仍忙", false, ++now), Result::Busy);
    content("respTextDelta", R"({"respId":"reply-1","contentIndex":0,"delta":"公开增量"})", 3);
    auto delta = runtime.take_content();
    ASSERT_TRUE(delta);
    EXPECT_EQ(delta->text, "公开增量");
    content("respTextDone", R"({"respId":"reply-1","contentIndex":0,"text":"公开完成"})", 4);
    ASSERT_TRUE(runtime.take_content());
    content("respDone",
            "{\"respId\":\"reply-1\",\"inputId\":\"" + input +
                "\",\"status\":\"completed\",\"contentCount\":1,\"audioState\":\"none\"}",
            5);
    ASSERT_TRUE(runtime.take_content());
    EXPECT_EQ(runtime.send_text("第二轮", false, ++now), Result::Applied);
    EXPECT_NE(input_id(mqtt.sent.back()), input);
    EXPECT_EQ(mqtt.topics.back(), "$thing/up/message/p/d");
}
TEST_F(TextOnlyTest, BusyAndUnsupportedAcknowledgementsKeepSessionAndOriginalInputAssociation) {
    negotiate();
    established();
    for (int code : {409, 422}) {
        ASSERT_EQ(runtime.send_text("公开请求", false, ++now), Result::Applied);
        const auto request = mqtt.sent.back();
        const auto original_input = input_id(request);
        reply(request, {}, code);
        const auto result = runtime.take_text_result();
        ASSERT_TRUE(result);
        EXPECT_EQ(result->input_id, original_input);
        EXPECT_EQ(result->result, code == 409 ? "busy" : "unsupported");
        EXPECT_EQ(runtime.voice().state(), VoiceState::Established);
    }
}
TEST_F(TextOnlyTest, TargetedCancelDoesNotNeedAudioAndKeepsExactAcknowledgement) {
    negotiate();
    established();
    create_response();
    ASSERT_EQ(runtime.cancel_response(++now), Result::Applied);
    const auto cancel = mqtt.sent.back();
    EXPECT_EQ(cancel.method, "respCancel");
    reply(cancel, R"({"respId":"wrong","state":"cancelled"})");
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
}
// NoneCancelStillRequiresActualState 不借音频ACK兼容放宽无媒体原生产者状态核验。
TEST_F(TextOnlyTest, NoneCancelStillRequiresActualState) {
    negotiate();
    established();
    create_response();
    ASSERT_EQ(runtime.cancel_response(++now), Result::Applied);
    reply(mqtt.sent.back(), R"({"respId":"reply-1"})");
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
}
TEST_F(TextOnlyTest, RepeatedCancelWaitsOriginalAcknowledgementWithoutNewRequest) {
    negotiate();
    established();
    create_response();
    ASSERT_EQ(runtime.cancel_response(++now), Result::Applied);
    const auto count = mqtt.sent.size();
    const auto original = mqtt.sent.back();
    EXPECT_EQ(runtime.cancel_response(++now), Result::Duplicate);
    EXPECT_EQ(mqtt.sent.size(), count);
    EXPECT_EQ(mqtt.sent.back().token, original.token);
    EXPECT_TRUE(runtime.voice().text_input_pending());
    reply(original, R"({"respId":"reply-1","state":"cancelled"})");
    EXPECT_FALSE(runtime.voice().text_input_pending());
}
TEST_F(TextOnlyTest, CancelAckAllowsNextInputAndOldReplyCannotSatisfyIt) {
    negotiate();
    established();
    const auto old_input = create_response();
    ASSERT_EQ(runtime.cancel_response(++now), Result::Applied);
    reply(mqtt.sent.back(), R"({"respId":"reply-1","state":"cancelled"})");
    ASSERT_EQ(runtime.send_text("新的独立输入", false, ++now), Result::Applied);
    while (runtime.take_content()) {
    }
    content("respDone",
            "{\"respId\":\"reply-1\",\"inputId\":\"" + old_input +
                "\",\"status\":\"completed\",\"contentCount\":0,\"audioState\":\"none\"}",
            2);
    EXPECT_FALSE(runtime.take_content());
    EXPECT_EQ(runtime.send_text("新轮仍忙", false, ++now), Result::Busy);
}
TEST_F(TextOnlyTest, DisconnectTimeoutAndEndReleaseSessionWithoutMediaSideEffects) {
    negotiate();
    established();
    EXPECT_EQ(runtime.end(++now), Result::Applied);
    const auto ending = mqtt.sent.back();
    EXPECT_EQ(ending.method, "sessionEnd");
    Envelope ended;
    ended.token = ending.token;
    ended.session = ending.session;
    ended.method = "sessionEnded";
    ended.code = 200;
    ended.request = false;
    ended.payload = R"({"state":"ended","reason":"userStopped"})";
    deliver(std::move(ended));
    EXPECT_EQ(runtime.state(), RuntimeState::Idle);
    EXPECT_EQ(runtime.send_text("旧会话", false, ++now), Result::Stale);
    EXPECT_EQ(runtime.start({}, ++now), Result::Applied);
    mqtt.events.push_back({MqttEvent::Kind::Disconnected, 0, 0, {}, {}});
    runtime.poll(++now);
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
}
TEST_F(TextOnlyTest, TextAcknowledgementTimeoutUsesOriginalByteIdenticalRetries) {
    negotiate();
    established();
    ASSERT_EQ(runtime.send_text("公开超时", false, ++now), Result::Applied);
    const auto request = mqtt.sent.back();
    runtime.poll(now + 10000);
    EXPECT_EQ(mqtt.sent.back().payload, request.payload);
    EXPECT_EQ(mqtt.sent.back().token, request.token);
    runtime.poll(now + 30000);
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
    EXPECT_TRUE(runtime.voice().session_id().empty());
}
TEST_F(TextOnlyTest, UnboundOldSessionDuplicateAndSttDoNotProjectOrReleaseCurrentInput) {
    negotiate();
    established();
    ASSERT_EQ(runtime.send_text("公开文本", false, ++now), Result::Applied);
    const auto request = mqtt.sent.back();
    const auto input = input_id(request);
    content("respTextDelta", R"({"respId":"unbound","contentIndex":0,"delta":"拒绝"})", 1);
    content("respCreated", "{\"respId\":\"reply-1\",\"inputId\":\"" + input + "\"}", 2,
            "old-session");
    content("respCreated", R"({"respId":"reply-1","inputId":"old-input"})", 3);
    EXPECT_FALSE(runtime.take_content());
    content("respCreated", "{\"respId\":\"reply-1\",\"inputId\":\"" + input + "\"}", 4);
    ASSERT_TRUE(runtime.take_content());
    content("respCreated", "{\"respId\":\"reply-2\",\"inputId\":\"" + input + "\"}", 5);
    content("respSttDone",
            "{\"respId\":\"reply-1\",\"inputId\":\"" + input + "\",\"text\":\"无音频识别\"}", 6);
    content("respTextDelta", R"({"respId":"reply-1","contentIndex":0,"delta":"迟到"})", 4);
    EXPECT_FALSE(runtime.take_content());
    EXPECT_TRUE(runtime.voice().text_input_pending());
    reply(request, "{\"inputId\":\"" + input + "\",\"state\":\"accepted\"}");
    ASSERT_TRUE(runtime.take_text_result());
    reply(request, "{\"inputId\":\"" + input + "\",\"state\":\"accepted\"}");
    EXPECT_FALSE(runtime.take_text_result());
}
TEST_F(TextOnlyTest, ReplyTerminalRequiresExactInputAndNoMedia) {
    negotiate();
    established();
    const auto input = create_response();
    while (runtime.take_content()) {
    }
    content("respDone",
            "{\"respId\":\"reply-1\",\"inputId\":\"" + input +
                "\",\"status\":\"completed\",\"contentCount\":0,\"audioState\":\"sent\"}",
            2);
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
}
TEST_F(TextOnlyTest, WrongTerminalInputCannotFreeBackpressure) {
    negotiate();
    established();
    create_response();
    content(
        "respDone",
        R"({"respId":"reply-1","inputId":"wrong","status":"completed","contentCount":0,"audioState":"none"})",
        2);
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
}
TEST_F(TextOnlyTest, ToolNotificationsAndFailedTerminalAreDisplayOnlyAndKeepSession) {
    negotiate();
    established();
    const auto input = create_response();
    while (runtime.take_content()) {
    }
    content(
        "toolCallStart",
        R"({"respId":"reply-1","toolCallId":"tool-1","name":"public_test_tool","arguments":{}})",
        2);
    content("toolCallResult",
            R"({"respId":"reply-1","toolCallId":"tool-1","state":"succeeded","result":{}})", 3);
    ASSERT_TRUE(runtime.take_content());
    ASSERT_TRUE(runtime.take_content());
    EXPECT_TRUE(runtime.voice().text_input_pending());
    content("respDone",
            "{\"respId\":\"reply-1\",\"inputId\":\"" + input +
                "\",\"status\":\"failed\",\"reason\":\"modelUnavailable\",\"contentCount\":0,"
                "\"audioState\":\"none\"}",
            4);
    ASSERT_TRUE(runtime.take_content());
    EXPECT_FALSE(runtime.voice().text_input_pending());
    EXPECT_EQ(runtime.voice().state(), VoiceState::Established);
    EXPECT_EQ(runtime.send_text("恢复后新轮", false, ++now), Result::Applied);
}
TEST_F(TextOnlyTest, ScopedErrorsOnlyReleaseMatchingOriginalTextInput) {
    negotiate();
    established();
    const auto input = create_response();
    auto error = [&](std::string response, bool retryable, std::uint64_t sequence) {
        Envelope event;
        event.token = "scoped-error";
        event.method = "sessionError";
        event.session = "text-session";
        event.request = false;
        event.code = 503;
        event.event_sequence = sequence;
        event.payload = "{\"scope\":\"response\",\"respId\":\"" + response +
                        "\",\"reason\":\"modelUnavailable\",\"mediaRevision\":0,\"retryable\":" +
                        (retryable ? "true}" : "false}");
        deliver(std::move(event));
    };
    error("wrong", false, 2);
    EXPECT_TRUE(runtime.voice().text_input_pending());
    error("reply-1", true, 3);
    EXPECT_TRUE(runtime.voice().text_input_pending());
    error("reply-1", false, 4);
    EXPECT_FALSE(runtime.voice().text_input_pending());
    EXPECT_EQ(runtime.voice().state(), VoiceState::Established);
    EXPECT_EQ(runtime.send_text("新轮", false, ++now), Result::Applied);
    EXPECT_NE(input_id(mqtt.sent.back()), input);
}
TEST_F(TextOnlyTest, NegotiationTimeoutAndTransportFailuresAreNotFakeEstablished) {
    negotiate();
    runtime.poll(now + 30000);
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
    EXPECT_TRUE(runtime.voice().session_id().empty());
    EXPECT_EQ(runtime.start({}, ++now), Result::Applied);
    mqtt.polling = false;
    runtime.poll(++now);
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
}
TEST(TextOnlyWire, NoneSelectionRejectsMediaAndModalitiesEscalation) {
    for (
        const auto* selected :
        {R"({"transport":"none","inputModalities":["audio","text"],"outputModalities":["text"]})",
         R"({"transport":"none","inputModalities":["text"],"outputModalities":["audio","text"]})",
         R"({"transport":"none","inputModalities":["text"],"outputModalities":["text"],"interactionMode":"halfDuplex"})",
         R"({"transport":"none","inputModalities":["text"],"outputModalities":["text"],"aecMode":"device"})",
         R"({"transport":"none","inputModalities":["text"],"outputModalities":["text"],"audioParams":{}})",
         R"({"transport":"none","inputModalities":[],"outputModalities":["text"]})"}) {
        VoiceEvent event;
        const auto body =
            std::string(
                R"({"msgToken":"create","method":"sessionCreated","sessionId":"session","code":200,"data":{"state":"negotiating","selected":)") +
            selected + ",\"tracks\":[]}}";
        EXPECT_EQ(decode_voice_event(WireRoute::Signaling, body, event), WireResult::Invalid);
    }
    for (const auto* extra : {R"(,"media":{})", R"(,"media":null)"}) {
        VoiceEvent event;
        const auto body =
            std::string(
                R"({"msgToken":"create","method":"sessionCreated","sessionId":"session","code":200,"data":{"state":"negotiating","selected":{"transport":"none","inputModalities":["text"],"outputModalities":["text"]},"tracks":[])") +
            extra + "}}";
        EXPECT_EQ(decode_voice_event(WireRoute::Signaling, body, event), WireResult::Invalid);
    }
}
TEST(TextOnlyWire, CapabilityAndCreateExcludeAllAudioFieldsAndRejectMediaCommands) {
    auto capability = text_capability();
    Envelope command;
    using wire_encoder = WireResult (*)(std::string, const VoiceCapability&, Envelope&,
                                        const VoiceVisionProfile*);
    const wire_encoder encoders[] = {capability_command, create_voice_command};
    for (const auto encoder : encoders) {
        ASSERT_EQ(encoder("token", capability, command, nullptr), WireResult::Ok);
        auto* params = cJSON_Parse(command.payload.c_str());
        ASSERT_NE(params, nullptr);
        const auto* features = cJSON_GetObjectItemCaseSensitive(params, "features");
        if (command.method == "capabilityReport") {
            EXPECT_TRUE(cJSON_IsArray(features));
            EXPECT_EQ(cJSON_GetArraySize(features), 0);
        } else {
            EXPECT_EQ(features, nullptr);
            const auto* tracks = cJSON_GetObjectItemCaseSensitive(params, "tracks");
            EXPECT_TRUE(cJSON_IsArray(tracks));
            EXPECT_EQ(cJSON_GetArraySize(tracks), 0);
        }
        for (const auto* name : {"inputModalities", "outputModalities"}) {
            const auto* modalities = cJSON_GetObjectItemCaseSensitive(params, name);
            EXPECT_TRUE(cJSON_IsArray(modalities));
            EXPECT_EQ(cJSON_GetArraySize(modalities), 1);
            const auto* text = cJSON_GetArrayItem(modalities, 0);
            EXPECT_TRUE(cJSON_IsString(text));
            if (cJSON_IsString(text)) {
                EXPECT_STREQ(text->valuestring, "text");
            }
        }
        cJSON_Delete(params);
        for (const auto* absent :
             {"audio", "aecMode", "interactionMode", "supportedInteractionModes", "key", "nonce"})
            EXPECT_EQ(command.payload.find(absent), std::string::npos);
        EXPECT_NE(command.payload.find("none"), std::string::npos);
    }
    for (const auto* method : {"audioStart", "audioStop", "mediaState"})
        EXPECT_EQ(
            voice_control("token", "session", method, "input", 0, command, 0, VoiceTransport::None),
            WireResult::Unsupported);
    capability.text_input = false;
    EXPECT_EQ(capability_command("token", capability, command), WireResult::Invalid);
    capability.text_input = true;
    capability.file_audio_output = true;
    EXPECT_EQ(create_voice_command("token", capability, command), WireResult::Invalid);
}
// NoneSelectionRejectsNullOrNonemptyTracks不能用null或音轨冒充明确的无媒体授权。
TEST(TextOnlyWire, NoneSelectionRejectsNullOrNonemptyTracks) {
    for (const auto* tracks : {"null", R"([{"trackId":"mic-1","kind":"audio"}])"}) {
        VoiceEvent event;
        const auto body =
            std::string(
                R"({"msgToken":"create","method":"sessionCreated","sessionId":"session","code":200,"data":{"state":"negotiating","selected":{"transport":"none","inputModalities":["text"],"outputModalities":["text"]},"tracks":)") +
            tracks + "}}";
        EXPECT_EQ(decode_voice_event(WireRoute::Signaling, body, event), WireResult::Invalid);
    }
}
}  // namespace
}  // namespace ur::device
