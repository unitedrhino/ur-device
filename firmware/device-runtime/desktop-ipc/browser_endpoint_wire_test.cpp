// 文件说明：本地窄桥命令与投影的正反例；不调用平台、浏览器或真实媒体。
#include "browser_endpoint_wire.hpp"
#include <gtest/gtest.h>

namespace ur::device {
// 空闲控制器尚无当前原腿，不得提前向尚未配对的本地桥发送状态并使设备退出。
TEST(BrowserEndpointWire, UnboundReceiptDoesNotPublishBeforeBrowserPairing) {
    EXPECT_TRUE(browser_endpoint_receipt(EndpointHandoffReceipt{}).empty());
}
namespace {
TEST(BrowserEndpointWire, ExactLocalCommandsCannotCarryPlatformAuthorization) {
  EndpointBrowserRequest result;
  for (
      const auto *body :
      {R"({"kind":"callDial","target":"p:d"})",
       R"({"kind":"endpointPrepare","sessionId":"source","target":"device:p:d"})",
       R"({"kind":"endpointCommit","sessionId":"source"})",
       R"({"kind":"endpointAbort","sessionId":"source"})"})
    EXPECT_EQ(decode_endpoint_browser(body, result), EndpointBrowserDecode::Ok);
  EXPECT_EQ(decode_endpoint_browser(
                R"({"kind":"callAccept","sessionId":"source"})", result),
            EndpointBrowserDecode::Other);
  for (
      const auto *body :
      {R"({"kind":"callDial","target":"p:d","ownerUserId":1})",
       R"({"kind":"callDial","target":"device:p:d"})",
       R"({"kind":"endpointPrepare","sessionId":"source","target":"p:d"})",
       R"({"kind":"endpointPrepare","sessionId":"source","target":"device:p:d/other"})",
       R"({"kind":"endpointCommit","sessionId":"source","candidateSessionId":"forged"})",
       R"({"kind":"endpointAbort","sessionId":"source","handoffId":"forged"})",
       R"({"kind":"endpointAbort","sessionId":null})",
       R"({"kind":"callDial","kind":"endpointCommit","target":"p:d"})"}) {
    EXPECT_EQ(decode_endpoint_browser(body, result),
              EndpointBrowserDecode::Invalid);
    EXPECT_TRUE(result.kind.empty());
    EXPECT_TRUE(result.session.empty());
  }
}
TEST(BrowserEndpointWire, ReceiptHasBoundedFixedStatesAndNoRawPayload) {
  EndpointHandoffReceipt receipt;
  EXPECT_TRUE(browser_endpoint_receipt(receipt).empty());
  receipt.session = "source";
  EXPECT_NE(browser_endpoint_receipt(receipt).find("endpointStatus"),
            std::string::npos); // 已绑定原腿的idle与完全未绑定状态不同。
  receipt.handoff = "handoff";
  receipt.state = "ready";
  receipt.revision = 2;
  receipt.candidate = "candidate";
  const auto body = browser_endpoint_receipt(receipt);
  EXPECT_NE(body.find("candidateSessionId"), std::string::npos);
  EXPECT_EQ(body.find("payload"), std::string::npos);
  receipt.reason = "internal credential-bearing failure";
  EXPECT_TRUE(browser_endpoint_receipt(receipt).empty());
  receipt.reason.clear();
  receipt.revision = 9007199254740992ULL;
  EXPECT_TRUE(browser_endpoint_receipt(receipt).empty());
}
} // namespace
} // namespace ur::device
