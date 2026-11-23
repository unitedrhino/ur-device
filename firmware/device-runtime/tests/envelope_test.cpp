// 公共信封正反例：规范化、重复键、类型、限长、迟到消息关联字段保真。
#include "ur/device/envelope.hpp"
#include <gtest/gtest.h>

namespace ur::device {
// Envelope.SafeNumbersRoundTrip 精确验证数值而非只验证可解析，覆盖嵌套对象/数组与公共水位。
TEST(Envelope, SafeNumbersRoundTrip) {
    constexpr auto body =
        R"({"msgToken":"t","method":"x","sessionId":"s","eventSeq":9007199254740991,"params":{"revision":9007199254740991,"values":[9007199254740990,-9007199254740991,0.1,{"position":9007199254740991}]}})";
    Envelope result;
    ASSERT_EQ(decode_envelope(body, result), DecodeResult::Ok);
    EXPECT_EQ(result.event_sequence, 9007199254740991ULL);
    EXPECT_EQ(
        result.payload,
        R"({"revision":9007199254740991,"values":[9007199254740990,-9007199254740991,0.1,{"position":9007199254740991}]})");
    std::string encoded;
    ASSERT_EQ(encode_envelope(result, encoded), DecodeResult::Ok);
    Envelope restored;
    ASSERT_EQ(decode_envelope(encoded, restored), DecodeResult::Ok);
    EXPECT_EQ(restored.event_sequence, result.event_sequence);
    EXPECT_EQ(restored.payload, result.payload);
    std::string canonical;
    ASSERT_EQ(canonical_json_object(result.payload, canonical), DecodeResult::Ok);
    EXPECT_EQ(canonical, result.payload);
}
TEST(Envelope, RequestRoundTripPreservesCanonicalPayload) {
    Envelope value;
    ASSERT_EQ(
        decode_envelope(
            R"({"msgToken":"t","method":"audioStart","sessionId":"s","params":{"z":1,"inputId":"i"}})",
            value),
        DecodeResult::Ok);
    EXPECT_TRUE(value.request);
    EXPECT_EQ(value.payload, R"({"inputId":"i","z":1})");
    std::string encoded;
    ASSERT_EQ(encode_envelope(value, encoded), DecodeResult::Ok);
    EXPECT_EQ(
        encoded,
        R"({"method":"audioStart","msgToken":"t","params":{"inputId":"i","z":1},"sessionId":"s"})");
    EXPECT_EQ(encoded.find("version"), std::string::npos);
}
TEST(Envelope, AckAndBothEventChannelsRoundTrip) {
    for (
        auto json :
        {R"({"msgToken":"t","method":"requestAck","code":200,"data":{"requestMethod":"audioStart","result":{}}})",
         R"({"msgToken":"t","method":"respAudioStart","sessionId":"s","eventSeq":1,"data":{}})",
         R"({"msgToken":"t","method":"respText","sessionId":"s","messageSeq":2,"data":{"text":"你好"}})"}) {
        Envelope value;
        ASSERT_EQ(decode_envelope(json, value), DecodeResult::Ok);
        std::string encoded;
        ASSERT_EQ(encode_envelope(value, encoded), DecodeResult::Ok);
        Envelope restored;
        ASSERT_EQ(decode_envelope(encoded, restored), DecodeResult::Ok);
        EXPECT_EQ(restored.code, value.code);
        EXPECT_EQ(restored.event_sequence, value.event_sequence);
        EXPECT_EQ(restored.message_sequence, value.message_sequence);
        EXPECT_EQ(restored.payload, value.payload);
    }
}
TEST(Envelope, RejectsAmbiguousAndMalformedJson) {
    for (auto json : {"", "{", "{} trailing", "{}{}",
                      R"({"msgToken":"t","msgToken":"u","method":"x","params":{}})",
                      R"({"msgToken":"t","method":"x","params":{"a":1,"a":2}})",
                      R"({"msgToken":"t","method":"x","params":{"a":1e999}})",
                      R"({"msgToken":"t\u0000evil","method":"x","params":{}})"}) {
        Envelope value;
        EXPECT_EQ(decode_envelope(json, value), DecodeResult::InvalidJson) << json;
        EXPECT_TRUE(value.token.empty());
    }
    Envelope value;
    EXPECT_EQ(decode_envelope(std::string("{}\0{}", 5), value), DecodeResult::InvalidJson);
    EXPECT_EQ(decode_envelope(std::string(65537, 'x'), value), DecodeResult::TooLarge);
    std::string nested(40, '[');
    nested += std::string(40, ']');
    EXPECT_EQ(decode_envelope(nested, value), DecodeResult::InvalidJson);
}
TEST(Envelope, RejectsInvalidCommonFieldsWithoutDispatch) {
    for (auto json : {"[]",
                      "{}",
                      R"({"msgToken":42,"method":"x","params":{}})",
                      R"({"msgToken":"","method":"x","params":{}})",
                      R"({"msgToken":"t","method":"x","sessionId":"","params":{}})",
                      R"({"msgToken":"t","method":"x","params":{},"version":2})",
                      R"({"msgToken":"t","method":"x","params":{},"data":{}})",
                      R"({"msgToken":"t","method":"x"})",
                      R"({"msgToken":"t","method":"x","params":[]})",
                      R"({"msgToken":"t","method":"x","params":{},"code":200})",
                      R"({"msgToken":"t","method":"x","data":{},"code":"200"})",
                      R"({"msgToken":"t","method":"x","data":{},"code":199})",
                      R"({"msgToken":"t","method":"x","data":{},"code":600})",
                      R"({"msgToken":"t","method":"x","data":{},"code":200.5})",
                      R"({"msgToken":"t","method":"x","data":{},"eventSeq":0})",
                      R"({"msgToken":"t","method":"x","data":{},"eventSeq":-1})",
                      R"({"msgToken":"t","method":"x","data":{},"eventSeq":1.5})",
                      R"({"msgToken":"t","method":"x","data":{},"eventSeq":"1"})",
                      R"({"msgToken":"t","method":"x","data":{},"eventSeq":9007199254740992})",
                      R"({"msgToken":"t","method":"x","data":{},"eventSeq":1,"messageSeq":1})"}) {
        Envelope value;
        EXPECT_EQ(decode_envelope(json, value), DecodeResult::InvalidEnvelope) << json;
    }
}
TEST(Envelope, EncoderCannotBypassValidation) {
    Envelope value;
    std::string encoded = "old";
    EXPECT_EQ(encode_envelope(value, encoded), DecodeResult::InvalidEnvelope);
    EXPECT_TRUE(encoded.empty());
    value.token = "t";
    value.method = "x";
    value.payload = "[]";
    EXPECT_EQ(encode_envelope(value, encoded), DecodeResult::InvalidJson);
    value.payload = "{";
    EXPECT_EQ(encode_envelope(value, encoded), DecodeResult::InvalidJson);
    value.payload = std::string(65537, 'x');
    EXPECT_EQ(encode_envelope(value, encoded), DecodeResult::TooLarge);
    value.payload = "{}";
    value.token = std::string("t\0x", 3);
    EXPECT_EQ(encode_envelope(value, encoded), DecodeResult::InvalidEnvelope);
    value.token = std::string(129, 't');
    EXPECT_EQ(encode_envelope(value, encoded), DecodeResult::InvalidEnvelope);
    value.token = "t";
    value.method = std::string(65, 'm');
    EXPECT_EQ(encode_envelope(value, encoded), DecodeResult::InvalidEnvelope);
    value.method = "x";
    value.session = std::string(129, 's');
    EXPECT_EQ(encode_envelope(value, encoded), DecodeResult::InvalidEnvelope);
}
TEST(Envelope, NestedPayloadCanonicalizationPreservesArrayOrder) {
    Envelope value;
    ASSERT_EQ(
        decode_envelope(
            R"({"msgToken":"t","method":"x","params":{"a":[{"z":0,"b":2},3,1],"b":{}}})", value),
        DecodeResult::Ok);
    EXPECT_EQ(value.payload, R"({"a":[{"b":2,"z":0},3,1],"b":{}})");
}
TEST(Envelope, NestingIsBoundedBeforePlatformJsonParserAndEscapedTextIsNotStructure) {
    Envelope value;
    const std::string nested =
        "{\"a\":" + std::string(10000, '[') + "0" + std::string(10000, ']') + "}";
    EXPECT_EQ(
        decode_envelope("{\"msgToken\":\"t\",\"method\":\"x\",\"params\":" + nested + "}", value),
        DecodeResult::InvalidJson);
    value.token = "t";
    value.method = "x";
    value.payload = nested;
    std::string result;
    EXPECT_EQ(encode_envelope(value, result), DecodeResult::InvalidJson);
    EXPECT_TRUE(result.empty());
    EXPECT_EQ(
        decode_envelope(R"({"msgToken":"t","method":"x","params":{"text":"[{}]\\\""}})", value),
        DecodeResult::Ok);
    for (const auto* malformed : {"}", "{", R"({"x":"unfinished})", "[]]"})
        EXPECT_EQ(decode_envelope(malformed, value), DecodeResult::InvalidJson);
}
TEST(Envelope, RejectsControlIdentifiersAndInvalidUtf8) {
    Envelope value;
    EXPECT_EQ(decode_envelope(R"({"msgToken":"bad\n","method":"x","params":{}})", value),
              DecodeResult::InvalidEnvelope);
    EXPECT_EQ(decode_envelope(
                  R"({"msgToken":"t","method":"x","sessionId":"bad\u007f","params":{}})", value),
              DecodeResult::InvalidEnvelope);
    for (const std::string& bytes :
         {std::string("\xc0\xaf", 2), std::string("\xed\xa0\x80", 3),
          std::string("\xf4\x90\x80\x80", 4), std::string("\x80", 1), std::string("\xe4\xb8", 2)}) {
        EXPECT_EQ(decode_envelope("{\"msgToken\":\"t\",\"method\":\"x\",\"params\":{\"text\":\"" +
                                      bytes + "\"}}",
                                  value),
                  DecodeResult::InvalidJson);
    }
    EXPECT_EQ(
        decode_envelope(R"({"msgToken":"中文😀","method":"x","params":{"text":"你好"}})", value),
        DecodeResult::Ok);
}
TEST(Envelope, LiteralEscapesAndSafePayloadNumbersMatchCloud) {
    Envelope value;
    ASSERT_EQ(
        decode_envelope(R"({"msgToken":"t","method":"x","params":{"text":"\\u0000"}})", value),
        DecodeResult::Ok);
    std::string encoded;
    ASSERT_EQ(encode_envelope(value, encoded), DecodeResult::Ok);
    for (const auto* number : {"9007199254740992", "-9007199254740992", "1e100"}) {
        EXPECT_EQ(decode_envelope(
                      std::string(R"({"msgToken":"t","method":"x","params":{"n":)") + number + "}}",
                      value),
                  DecodeResult::InvalidJson);
    }
    ASSERT_EQ(
        decode_envelope(R"({"msgToken":"t","method":"x","params":{"n":9007199254740991}})", value),
        DecodeResult::Ok);
}
}  // namespace ur::device
