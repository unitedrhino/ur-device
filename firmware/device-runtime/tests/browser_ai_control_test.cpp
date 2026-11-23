// 文件说明：配对本机桥只允许固定显式断连/恢复，不接受凭据、目标会话或自由代码。
#include "browser_ai_control.hpp"
#include <gtest/gtest.h>

namespace ur::device {
namespace {
TEST(BrowserAIControl, StrictFixedNativeRequests) {
    EXPECT_EQ(decode_browser_ai_control_request(R"({"kind":"controlDisconnect"})"),
              BrowserAIControlRequest::Disconnect);
    EXPECT_EQ(decode_browser_ai_control_request(R"({"kind":"controlReconnect"})"),
              BrowserAIControlRequest::Resume);
    EXPECT_EQ(decode_browser_ai_control_request(R"({"kind":"start"})"),
              BrowserAIControlRequest::Other);
    EXPECT_EQ(decode_browser_ai_control_request(R"({"kind":"captured","pcm":[1,2]})"),
              BrowserAIControlRequest::Other);
    for (const auto* body : {R"({"kind":"controlReconnect","sessionId":"other"})",
                             R"({"kind":"controlReconnect","password":"not-accepted"})",
                             R"({"kind":"controlReconnect","kind":"controlReconnect"})",
                             R"({"kind":"controlDisconnect","delay":0})", "[]", "null", "{"})
        EXPECT_EQ(decode_browser_ai_control_request(body), BrowserAIControlRequest::Invalid);
}
}  // namespace
}  // namespace ur::device
