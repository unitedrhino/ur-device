// 文件说明：门铃本地按钮只传无参意图，不能从网页扩展为物模型身份、平台命令或媒体授权。
#include <gtest/gtest.h>
#include "browser_ipc_media.hpp"

namespace ur::device {
namespace {
// 隐私桥意图只允许显式布尔值；身份、时间和关联键不能由网页覆盖。
TEST(PrivacyBrowser, BooleanIntentDoesNotCarryMediaOrAuthority) {
    IPCBrowserRequest request;
    for (const auto* body : {R"({"kind":"privacyReport","enabled":true})",
                             R"({"kind":"privacyReport","enabled":false})"}) {
        ASSERT_TRUE(decode_ipc_browser(body, request));
        EXPECT_EQ(request.kind, "privacyReport");
        EXPECT_TRUE(request.session.empty());
        EXPECT_TRUE(request.token.empty());
        EXPECT_FALSE(request.success);
    }
    EXPECT_FALSE(request.enabled);
    for (const auto* body :
         {R"({"kind":"privacyReport"})", R"({"kind":"privacyReport","enabled":1})",
          R"({"kind":"privacyReport","enabled":"true"})",
          R"({"kind":"privacyReport","enabled":null})",
          R"({"kind":"privacyReport","enabled":true,"sessionId":"s"})",
          R"({"kind":"privacyReport","enabled":true,"msgToken":"t"})",
          R"({"kind":"privacyReport","enabled":true,"timestamp":1})",
          R"({"kind":"privacyReport","enabled":true,"enabled":false})"}) {
        request.enabled = true;
        EXPECT_FALSE(decode_ipc_browser(body, request));
        EXPECT_TRUE(request.kind.empty());
        EXPECT_FALSE(request.enabled);
    }
}

// 合法按钮没有会话、配置事件、token或媒体内容；原意图在C++设备事件线程生成。
TEST(DoorbellBrowser, PlainButtonIntentHasNoIdentityOrMediaFields) {
    IPCBrowserRequest request;
    ASSERT_TRUE(decode_ipc_browser(R"({"kind":"doorbellPress"})", request));
    EXPECT_EQ(request.kind, "doorbellPress");
    EXPECT_TRUE(request.session.empty());
    EXPECT_TRUE(request.token.empty());
    EXPECT_TRUE(request.sdp.empty());
    EXPECT_TRUE(request.state.empty());
    EXPECT_FALSE(request.success);
}

// 网页不能替换原鉴权身份、选择事件或被叫，不能携带token伪造另一次重试。
TEST(DoorbellBrowser, ExtraAuthorityAndCorrelationFieldsAreRejected) {
    IPCBrowserRequest request;
    for (const auto* body :
         {R"({"kind":"doorbellPress","sessionId":"call"})",
          R"({"kind":"doorbellPress","eventID":"doorbellPressed"})",
          R"({"kind":"doorbellPress","params":{}})", R"({"kind":"doorbellPress","token":"retry"})",
          R"({"kind":"doorbellPress","msgToken":"retry"})",
          R"({"kind":"doorbellPress","timestamp":1})",
          R"({"kind":"doorbellPress","cameraId":"front"})",
          R"({"kind":"doorbellPress","userID":1})",
          R"({"kind":"doorbellPress","calleeId":"owner"})",
          R"({"kind":"doorbellPress","url":"https://example.invalid"})",
          R"({"kind":"doorbellPress","method":"eventPost"})",
          R"({"kind":"doorbellPress","secret":"credential"})",
          R"({"kind":"doorbellPress","sdp":"v=0\r\n"})"}) {
        request.kind = "previous";
        request.session = "previous";
        EXPECT_FALSE(decode_ipc_browser(body, request));
        EXPECT_TRUE(request.kind.empty());
        EXPECT_TRUE(request.session.empty());
        EXPECT_TRUE(request.token.empty());
    }
}

// 大小写、重复键和JSON类型都遵循现有本地桥安全入口，不放宽已有驱动回调契约。
TEST(DoorbellBrowser, MalformedButtonsCannotBecomeOtherCommands) {
    IPCBrowserRequest request;
    for (const auto* body :
         {"{", "[]", "null", "{} trailing", R"({"kind":"doorbellPress","kind":"doorbellPress"})",
          R"({"kind":"doorbellpress"})", R"({"kind":true})", R"({"kind":"doorbellPress\u0000"})"}) {
        EXPECT_FALSE(decode_ipc_browser(body, request));
        EXPECT_TRUE(request.kind.empty());
    }
    EXPECT_TRUE(decode_ipc_browser(R"({"kind":"ready"})", request));
    EXPECT_TRUE(decode_ipc_browser(R"({"kind":"shutdown"})", request));
    EXPECT_TRUE(decode_ipc_browser(R"({"kind":"ipcConnected","sessionId":"view"})", request));
}
}  // namespace
}  // namespace ur::device
