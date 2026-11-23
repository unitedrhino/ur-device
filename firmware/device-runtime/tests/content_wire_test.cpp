// 内容解码契约：文本可含换行，工具通知不等于执行授权，失败不得留下半解析数据。
#include "ur/device/content_wire.hpp"
#include <gtest/gtest.h>

namespace ur::device {
namespace {
// TypedInputUsesOriginalMethodAndOrderedReferences 校验真正生产编码结果，而非只检查无崩溃。
TEST(ContentWireTest, TypedInputUsesOriginalMethodAndOrderedReferences) {
    Envelope request;
    ASSERT_EQ(content_input_command("token", "session", "input",
                                    {{InputContentKind::Text, "用户问题\n第二行", {}, {}},
                                     {InputContentKind::Image, {}, "artifact_image", "image/png"},
                                     {InputContentKind::File, {}, "artifact_file", "text/plain"}},
                                    true, request),
              WireResult::Ok);
    EXPECT_EQ(request.method, "inputSend");
    EXPECT_TRUE(request.request);
    // 公共信封规范化对象键，contents数组顺序不得变；不把JSON对象键序误当协议合同。
    EXPECT_EQ(
        request.payload,
        R"({"contents":[{"text":"用户问题\n第二行","type":"text"},{"contentRef":"artifact_image","mimeType":"image/png","type":"image"},{"contentRef":"artifact_file","mimeType":"text/plain","type":"file"}],"inputId":"input","outputModalities":["text","audio"]})");
    std::string body;
    ASSERT_EQ(encode_envelope(request, body), DecodeResult::Ok);
    EXPECT_EQ(body.find("data:"), std::string::npos);
    EXPECT_EQ(body.find("https:"), std::string::npos);
}

// TypedInputRejectsUntrustedFields 每项非法语料重新填脏输出，失败须清除旧token/正文。
TEST(ContentWireTest, TypedInputRejectsUntrustedFields) {
    const std::vector<InputContentPart> invalid = {
        {InputContentKind::Text, "", {}, {}},
        {InputContentKind::Text, "\xc2\xa0\xe3\x80\x80", {}, {}},
        {InputContentKind::Text, std::string("a\0b", 3), {}, {}},
        {InputContentKind::Text, std::string("bad\xff", 4), {}, {}},
        {InputContentKind::Text, "正文", "artifact_file", {}},
        {InputContentKind::Text, "正文", {}, "text/plain"},
        {InputContentKind::Image, "内联正文", "artifact_image", "image/png"},
        {InputContentKind::Image, {}, "https://private.invalid/object", "image/png"},
        {InputContentKind::Image, {}, "data:image/png;base64,AAA=", "image/png"},
        {InputContentKind::Image, {}, "artifact_../object", "image/png"},
        {InputContentKind::Image, {}, "ai-chat-device/artifact_image.png", "image/png"},
        {InputContentKind::Image, {}, "artifact_", "image/png"},
        {InputContentKind::Image, {}, "artifact_" + std::string(56, 'a'), "image/png"},
        {InputContentKind::Image, {}, "artifact_image", "image/tiff"},
        {InputContentKind::Image, {}, "artifact_image", "image/png;charset=utf-8"},
        {InputContentKind::Image, {}, "artifact_image", "text/plain"},
        {InputContentKind::File, {}, "artifact_file", "image/png"},
        {InputContentKind::File, {}, "artifact_file", "audio/opus"},
    };
    for (const auto& part : invalid) {
        Envelope request;
        request.token = "dirty";
        request.payload = "dirty";
        EXPECT_EQ(content_input_command("t", "s", "i", {part}, false, request),
                  WireResult::Invalid);
        EXPECT_TRUE(request.token.empty());
        EXPECT_EQ(request.payload, "{}");  // 原Envelope清空态保留合法空JSON，不保留脏内容。
    }
    Envelope request;
    EXPECT_EQ(content_input_command(
                  "t", "s", "i", {{static_cast<InputContentKind>(99), {}, {}, {}}}, false, request),
              WireResult::Unsupported);
    EXPECT_EQ(content_input_command("t", "s", std::string("i\0d", 3),
                                    {{InputContentKind::Text, "正常", {}, {}}}, false, request),
              WireResult::Invalid);
}

// TypedInputKeepsWholeFrameBudget 原件不进入MQTT；数量、累计正文及JSON转义均受整帧门禁约束。
TEST(ContentWireTest, TypedInputKeepsWholeFrameBudget) {
    Envelope request;
    std::vector<InputContentPart> parts(
        16, {InputContentKind::Image, {}, "artifact_image", "image/png"});
    EXPECT_EQ(content_input_command("t", "s", "i", parts, false, request), WireResult::Ok);
    parts.push_back(parts.front());
    EXPECT_EQ(content_input_command("t", "s", "i", parts, false, request), WireResult::Invalid);
    EXPECT_EQ(content_input_command("t", "s", "i", {}, false, request), WireResult::Invalid);
    EXPECT_EQ(content_input_command("t", "s", "i",
                                    {{InputContentKind::Text, std::string(40000, 'a'), {}, {}},
                                     {InputContentKind::Text, std::string(40000, 'b'), {}, {}}},
                                    false, request),
              WireResult::Invalid);
    EXPECT_EQ(content_input_command("t", "s", "i",
                                    {{InputContentKind::Text, std::string(40000, '\\'), {}, {}}},
                                    false, request),
              WireResult::Invalid);
    EXPECT_EQ(content_input_command("t", "s", "i",
                                    {{InputContentKind::Text, std::string(64000, 'x'), {}, {}}},
                                    false, request),
              WireResult::Ok);
    EXPECT_EQ(text_input_command("t", "s", "i", std::string(60001, 'x'), false, request),
              WireResult::Invalid);
}

// TypedInputMatchesCurrentAttachmentFormats 逐项对齐现行Go上传格式，不借MIME前缀扩大文件解析能力。
TEST(ContentWireTest, TypedInputMatchesCurrentAttachmentFormats) {
    Envelope request;
    for (const auto* mime : {"image/png", "image/jpeg", "image/gif", "image/bmp", "image/webp"})
        EXPECT_EQ(content_input_command("t", "s", "i",
                                        {{InputContentKind::Image, {}, "artifact_image", mime}},
                                        false, request),
                  WireResult::Ok);
    for (const auto* mime :
         {"text/plain", "text/csv", "application/json", "text/markdown", "text/asciidoc",
          "text/html", "message/rfc822", "application/pdf",
          "application/vnd.openxmlformats-officedocument.wordprocessingml.document",
          "application/vnd.openxmlformats-officedocument.presentationml.presentation",
          "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"})
        EXPECT_EQ(content_input_command("t", "s", "i",
                                        {{InputContentKind::File, {}, "artifact_file", mime}},
                                        false, request),
                  WireResult::Ok);
}
// content 构造独立 message 事件，数据参数只来自静态测试语料。
std::string content(std::string_view method, std::string_view data) {
    return "{\"msgToken\":\"m1\",\"sessionId\":\"s1\",\"messageSeq\":1,"
           "\"method\":\"" +
           std::string(method) + "\",\"data\":" + std::string(data) + "}";
}
TEST(ContentWireTest, DecodesEveryPhaseOneContentMethod) {
    ContentEvent event;
    EXPECT_EQ(
        decode_content_event(WireRoute::Message,
                             content("respCreated", R"({"respId":"r","inputId":"i"})"), event),
        WireResult::Ok);
    EXPECT_EQ(event.input_id, "i");
    for (const auto* method : {"respSttDelta", "respSttDone"}) {
        const auto data = std::string(R"({"respId":"r","inputId":"i",")") +
                          (std::string(method) == "respSttDelta" ? "delta" : "text") +
                          R"(":"你好\n世界"})";
        EXPECT_EQ(decode_content_event(WireRoute::Message, content(method, data), event),
                  WireResult::Ok);
        EXPECT_EQ(event.text, "你好\n世界");
    }
    for (const auto* method : {"respTextDelta", "respTextDone"}) {
        const auto data = std::string(R"({"respId":"r","contentIndex":0,")") +
                          (std::string(method) == "respTextDelta" ? "delta" : "text") + R"(":""})";
        EXPECT_EQ(decode_content_event(WireRoute::Message, content(method, data), event),
                  WireResult::Ok);
        EXPECT_EQ(event.content_index, 0U);
        EXPECT_TRUE(event.text.empty());
    }
    EXPECT_EQ(
        decode_content_event(
            WireRoute::Message,
            content(
                "toolCallStart",
                R"({"respId":"r","toolCallId":"t","name":"setVolume","arguments":{"volume":40}})"),
            event),
        WireResult::Ok);
    EXPECT_EQ(event.tool_name, "setVolume");
    EXPECT_EQ(event.object_json, R"({"volume":40})");
    for (const auto* state : {"succeeded", "failed", "unknown"}) {
        EXPECT_EQ(
            decode_content_event(
                WireRoute::Message,
                content("toolCallResult",
                        std::string(R"({"respId":"r","toolCallId":"t","result":{},"state":")") +
                            state + R"("})"),
                event),
            WireResult::Ok);
        EXPECT_EQ(event.status, state);
    }
    for (const auto* state : {"completed", "cancelled", "failed"}) {
        for (const auto* audio : {"none", "sent", "cancelled", "failed"}) {
            EXPECT_EQ(
                decode_content_event(
                    WireRoute::Message,
                    content(
                        "respDone",
                        std::string(R"({"respId":"r","inputId":"i","contentCount":1,"status":")") +
                            state + R"(","audioState":")" + audio + R"(","reason":"finished"})"),
                    event),
                WireResult::Ok);
            EXPECT_EQ(event.content_count, 1U);
            EXPECT_EQ(event.audio_state, audio);
        }
    }
    EXPECT_EQ(
        decode_content_event(
            WireRoute::Message,
            content(
                "respDone",
                R"({"respId":"r","inputId":"i","contentCount":0,"status":"completed","audioState":"none"})"),
            event),
        WireResult::Ok);
}

// 表情属于显示内容，不能触发设备动作；畸形/非白名单输入不得留下半解析结果。
TEST(ContentWireTest, EmotionIsValidatedDisplayContent) {
    ContentEvent event;
    EXPECT_EQ(
        decode_content_event(
            WireRoute::Message,
            content("respEmotion", R"({"respId":"r","emoji":"🤔","emotion":"neutral"})"), event),
        WireResult::Ok);
    EXPECT_EQ(event.emoji, "🤔");
    EXPECT_EQ(event.emotion, "neutral");
    EXPECT_TRUE(event.tool_call_id.empty());
    for (const auto* data :
         {R"({"respId":"r","emoji":"x","emotion":"happy"})",
          R"({"respId":"r","emoji":"🙂","emotion":"thinking"})",
          R"({"respId":"r","emoji":null,"emotion":"happy"})", R"({"respId":"r","emoji":"🙂"})"}) {
        EXPECT_EQ(decode_content_event(WireRoute::Message, content("respEmotion", data), event),
                  WireResult::Invalid);
        EXPECT_TRUE(event.response_id.empty());
    }
}
// 连续开麦产生多个语音条目，设备不能要求每个条目等于当前采集ID。
TEST(ContentWireTest, SeparatesUtteranceFromCapture) {
    ContentEvent event;
    EXPECT_EQ(decode_content_event(
                  WireRoute::Message,
                  content("respCreated", R"({"respId":"r","utteranceId":"speech-1"})"), event),
              WireResult::Ok);
    EXPECT_EQ(event.utterance_id, "speech-1");
    EXPECT_TRUE(event.input_id.empty());
    EXPECT_EQ(
        decode_content_event(
            WireRoute::Message,
            content(
                "respSttDone",
                R"({"respId":"r","utteranceId":"speech-1","inputId":"capture-1","text":"测试"})"),
            event),
        WireResult::Ok);
    EXPECT_EQ(event.input_id, "capture-1");
    EXPECT_EQ(event.utterance_id, "speech-1");
    EXPECT_EQ(
        decode_content_event(
            WireRoute::Message,
            content(
                "respDone",
                R"({"respId":"r","utteranceId":"speech-1","status":"completed","contentCount":0,"audioState":"none"})"),
            event),
        WireResult::Ok);
    for (const auto* source :
         {R"({"respId":"r"})", R"({"respId":"r","utteranceId":null})",
          R"({"respId":"r","utteranceId":""})", R"({"respId":"r","utteranceId":7})",
          R"({"respId":"r","utteranceId":"speech","inputId":null})"}) {
        EXPECT_EQ(decode_content_event(WireRoute::Message, content("respCreated", source), event),
                  WireResult::Invalid);
        EXPECT_TRUE(event.utterance_id.empty());
    }
}
TEST(ContentWireTest, RejectsWrongRouteEnvelopeAndExecutionCapability) {
    ContentEvent event;
    const auto valid = content("respCreated", R"({"respId":"r","inputId":"i"})");
    EXPECT_EQ(decode_content_event(WireRoute::Signaling, valid, event), WireResult::WrongRoute);
    for (const auto* body :
         {"null", "{", R"({"msgToken":"m","method":"respCreated","params":{}})",
          R"({"msgToken":"m","method":"respCreated","messageSeq":1,"data":{}})",
          R"({"msgToken":"m","method":"respCreated","sessionId":"s","eventSeq":1,"data":{}})",
          R"({"msgToken":"m","method":"respCreated","sessionId":"s","code":200,"data":{}})"}) {
        EXPECT_EQ(decode_content_event(WireRoute::Message, body, event), WireResult::Invalid);
        EXPECT_TRUE(event.envelope.token.empty());
    }
    for (const auto* method : {"toolCallExecute", "translationResult", "futureEvent"}) {
        EXPECT_EQ(
            decode_content_event(WireRoute::Message, content(method, R"({"respId":"r"})"), event),
            WireResult::Unsupported);
        EXPECT_TRUE(event.response_id.empty());
    }
}
TEST(ContentWireTest, RejectsMalformedFieldsAndClearsPreviousOutput) {
    // 方法/负载一一对应，覆盖缺失、错误类型、非法枚举和整数边界。
    const std::pair<const char*, const char*> cases[] = {
        {"respCreated", R"({"respId":"","inputId":"i"})"},
        {"respCreated", R"({"respId":"r\n","inputId":"i"})"},
        {"respCreated", R"({"respId":"r"})"},
        {"respSttDelta", R"({"respId":"r","inputId":"i","delta":null})"},
        {"respTextDone", R"({"respId":"r","contentIndex":-1,"text":"t"})"},
        {"respTextDone", R"({"respId":"r","contentIndex":0.5,"text":"t"})"},
        {"respTextDone", R"({"respId":"r","contentIndex":9007199254740992,"text":"t"})"},
        {"respTextDone", R"({"respId":"r","contentIndex":"0","text":"t"})"},
        {"toolCallStart", R"({"respId":"r","toolCallId":"t","name":"x","arguments":"{}"})"},
        {"toolCallResult", R"({"respId":"r","toolCallId":"t","state":"ok","result":{}})"},
        {"toolCallResult", R"({"respId":"r","toolCallId":"t","state":"unknown","result":[]})"},
        {"respDone",
         R"({"respId":"r","inputId":"i","contentCount":0,"status":"cancelled","audioState":"none"})"},
        {"respDone",
         R"({"respId":"r","inputId":"i","contentCount":0,"status":"completed","audioState":"played"})"},
        {"respDone",
         R"({"respId":"r","inputId":"i","contentCount":0,"status":"ok","audioState":"none"})"},
    };
    for (const auto& item : cases) {
        ContentEvent event;
        ASSERT_EQ(
            decode_content_event(WireRoute::Message,
                                 content("respCreated", R"({"respId":"r","inputId":"i"})"), event),
            WireResult::Ok);
        EXPECT_EQ(decode_content_event(WireRoute::Message, content(item.first, item.second), event),
                  WireResult::Invalid)
            << item.first << " " << item.second;
        EXPECT_TRUE(event.response_id.empty());
        EXPECT_TRUE(event.envelope.token.empty());
    }
    ContentEvent event;
    EXPECT_EQ(decode_content_event(WireRoute::Message,
                                   content("respCreated", "{\"respId\":\"" + std::string(129, 'r') +
                                                              "\",\"inputId\":\"i\"}"),
                                   event),
              WireResult::Invalid);
}
}  // namespace
}  // namespace ur::device
