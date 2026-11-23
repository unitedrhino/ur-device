// 动作线协议测试：正式平台格式、空参数、歧义字段、恶意JSON及执行回执序列化。
#include "ur/device/action_wire.hpp"
#include <gtest/gtest.h>

namespace ur::device {
TEST(ActionWire, AcceptsPlatformActionAndCanonicalParameters) {
    ActionRequest request;
    ASSERT_EQ(
        decode_action_request(
            R"({"method":"action","msgToken":"t-1","actionID":"setVolume","timestamp":0,"params":{"volume":40,"extra":{"z":1,"a":2}}})",
            request),
        DecodeResult::Ok);
    EXPECT_EQ(request.token, "t-1");
    EXPECT_EQ(request.action, "setVolume");
    EXPECT_EQ(request.params, R"({"extra":{"a":2,"z":1},"volume":40})");
}
TEST(ActionWire, AcceptsAbsentParamsAndSingleLegacyAlias) {
    ActionRequest request;
    ASSERT_EQ(decode_action_request(
                  R"({"method":"action","msgToken":"t","actionId":"takePhoto","version":"1.0"})",
                  request),
              DecodeResult::Ok);
    EXPECT_EQ(request.action, "takePhoto");
    EXPECT_EQ(request.params, "{}");
}
TEST(ActionWire, RejectsMalformedOrAmbiguousRequestsAndClearsOutput) {
    for (const auto* input :
         {R"({"method":"actionReply","msgToken":"t","actionID":"a"})",
          R"({"method":"action","msgToken":"t","actionID":"a","actionId":"a"})",
          R"({"method":"action","msgToken":1,"actionID":"a"})",
          R"({"method":"action","msgToken":"t","actionID":null})",
          R"({"method":"action","msgToken":"t","actionID":""})",
          R"({"method":"action","msgToken":"t","actionID":"a","params":null})",
          R"({"method":"action","msgToken":"t","actionID":"a","params":[]})",
          R"({"method":"action","msgToken":"t","actionID":"a","params":{"x":1,"x":2}})",
          R"({"method":"action","msgToken":"t","actionID":"a","timestamp":-1})",
          R"({"method":"action","msgToken":"t","actionID":"a","timestamp":0.5})",
          R"({"method":"action","msgToken":"t","actionID":"a","timestamp":"1"})",
          R"({"method":"action","msgToken":"t","actionID":"a","version":2})",
          R"({"method":"action","msgToken":"t","actionID":"a","version":"2"})",
          R"({"method":"action","msgToken":"t","actionID":"a","sessionId":"s"})",
          R"({"method":"action","msgToken":"t\u0000","actionID":"a"})",
          R"({"method":"action","msgToken":"t","actionID":"a"} {})", "[]", "{"}) {
        ActionRequest request{"previous", "previous", "{}"};
        EXPECT_NE(decode_action_request(input, request), DecodeResult::Ok) << input;
        EXPECT_TRUE(request.token.empty());
        EXPECT_TRUE(request.action.empty());
    }
}
TEST(ActionWire, RejectsOversizeDeepAndInvalidUnicode) {
    ActionRequest request;
    EXPECT_EQ(decode_action_request(std::string(65537, ' '), request), DecodeResult::TooLarge);
    std::string deep(1000, '[');
    deep += std::string(1000, ']');
    EXPECT_EQ(decode_action_request(deep, request), DecodeResult::InvalidJson);
    // 非法UTF-8字节保持0xff，不通过整数窄化触发MSVC严格警告。
    EXPECT_EQ(decode_action_request(std::string("{\"x\":\"") + '\xff' + "\"}", request),
              DecodeResult::InvalidJson);
}
TEST(ActionWire, EncodesOnlyDeviceExecutionReplyFields) {
    std::string json;
    ASSERT_EQ(encode_action_reply({"t-1", "setVolume", 200, "success", R"({"volume":40})"}, json),
              DecodeResult::Ok);
    EXPECT_EQ(
        json,
        R"({"actionID":"setVolume","code":200,"data":{"volume":40},"method":"actionReply","msg":"success","msgToken":"t-1"})");
    ASSERT_EQ(encode_action_reply({"t-2", "takePhoto", 500, "driverFailed", "{}"}, json),
              DecodeResult::Ok);
    EXPECT_NE(json.find("driverFailed"), std::string::npos);
}
TEST(ActionWire, InvalidDriverReplyNeverProducesSuccessJSON) {
    for (const auto& reply :
         {ActionReply{"", "a", 200, "", "{}"}, ActionReply{"t", "", 200, "", "{}"},
          ActionReply{"t", "a", 0, "", "{}"}, ActionReply{"t", "a", 600, "", "{}"},
          ActionReply{"t", "a", 200, std::string(257, 'x'), "{}"},
          ActionReply{"t", "a", 200, std::string("x\0y", 3), "{}"},
          ActionReply{"t", "a", 200, "", "[]"}, ActionReply{"t", "a", 200, "", R"({"x":1,"x":2})"},
          ActionReply{"t", "a", 200, "", "{"}}) {
        std::string json = "old-success";
        EXPECT_NE(encode_action_reply(reply, json), DecodeResult::Ok);
        EXPECT_TRUE(json.empty());
    }
}
}  // namespace ur::device
