// 文件说明：视频桌面媒体适配与本地桥格式测试，云端连接不由此伪造。
#include "browser_call_media.hpp"
#include <gtest/gtest.h>
#include "browser_call_wire.hpp"

namespace ur::device {
// SelectedVideoReceive 旧驱动配置不扩权，显式所选接收只能在Peer启动前绑定。
TEST(BrowserCallMediaTest, SelectedVideoReceive) {
    BrowserCallMedia media;
    EXPECT_TRUE(media.video_receive_supported());
    ASSERT_TRUE(media.begin());
    ASSERT_TRUE(media.take_command());
    EXPECT_FALSE(media.configure_video_receive(true));
    media.close();
    ASSERT_TRUE(media.take_command());
    ASSERT_TRUE(media.configure_video_receive(true));
    ASSERT_TRUE(media.begin());
    const auto command = media.take_command();
    ASSERT_TRUE(command);
    EXPECT_NE(command->find(R"("videoReceive":true)"), std::string::npos);
    media.close();
    ASSERT_TRUE(media.take_command());
    ASSERT_TRUE(media.begin());
    const auto legacy = media.take_command();
    ASSERT_TRUE(legacy);
    EXPECT_EQ(legacy->find("videoReceive"), std::string::npos);
}
// TransferIntentOnly 固定本地意图不能自授会话、版本或候选腿。
TEST(BrowserCallWireTest, TransferIntentOnly) {
    CallBrowserRequest out;
    ASSERT_TRUE(decode_call_browser(
        R"({"kind":"callControl","operation":"transfer","target":"p1:camera-03"})", out));
    EXPECT_EQ(out.operation, "transfer");
    EXPECT_EQ(out.target, "p1:camera-03");
    ASSERT_TRUE(decode_call_browser(R"({"kind":"callControl","operation":"cancelTransfer"})", out));
    EXPECT_EQ(out.operation, "cancelTransfer");
    EXPECT_TRUE(out.target.empty());
}

// TransferCannotChooseAuthority 新本地操作只允许规范设备目标，版本和候选由原核心取得。
TEST(BrowserCallWireTest, TransferCannotChooseAuthority) {
    CallBrowserRequest out;
    for (const auto* body :
         {R"({"kind":"callControl","operation":"transfer"})",
          R"({"kind":"callControl","operation":"transfer","target":{"kind":"device","id":"p:d"}})",
          R"({"kind":"callControl","operation":"transfer","target":"p:d","expectedRevision":1})",
          R"({"kind":"callControl","operation":"transfer","target":"p:d","sessionId":"other"})",
          R"({"kind":"callControl","operation":"transfer","target":"p:d","controlToken":"chosen"})",
          R"({"kind":"callControl","operation":"cancelTransfer","candidateSessionId":"chosen"})",
          R"({"kind":"callControl","operation":"cancelTransfer","target":"p:d"})",
          R"({"kind":"callControl","operation":"transfer","target":"p:"})",
          R"({"kind":"callControl","operation":"transfer","target":":d"})",
          R"({"kind":"callControl","operation":"transfer","target":"p:d/x"})",
          R"({"kind":"callControl","operation":"transfer","target":"p:d+"})",
          R"({"kind":"callControl","operation":"transfer","target":"p:d#"})",
          R"({"kind":"callControl","operation":"transfer","target":"p:d\n"})",
          R"({"kind":"callControl","operation":"transfer","target":"p:d\u0000other"})"}) {
        EXPECT_FALSE(decode_call_browser(body, out)) << body;
        EXPECT_TRUE(out.kind.empty());
        EXPECT_TRUE(out.target.empty());
    }
    EXPECT_FALSE(
        decode_call_browser("{\"kind\":\"callControl\",\"operation\":\"transfer\",\"target\":\"p:" +
                                std::string(127, 'd') + "\"}",
                            out));
    for (const auto* operation : {"cancel", "hold", "resume"}) {
        ASSERT_TRUE(decode_call_browser(
            "{\"kind\":\"callControl\",\"operation\":\"" + std::string(operation) + "\"}", out));
        EXPECT_EQ(out.operation, operation);
    }
}

// TransferReceiptSeparatesAcceptanceAndCancellation 编码只投影原事实，不授予新腿或改变原媒体。
TEST(BrowserCallMediaTest, TransferReceiptSeparatesAcceptanceAndCancellation) {
    BrowserCallMedia media;
    ASSERT_TRUE(media.begin());
    ASSERT_TRUE(media.take_command());
    ASSERT_TRUE(media.apply_answer("v=0\r\no=server\r\n"));
    ASSERT_TRUE(media.take_command());
    ASSERT_TRUE(media.report_connected());
    HumanCallTransferReceipt receipt;
    receipt.session = "original-leg";
    receipt.token = "original-transfer";
    receipt.target = "must-not-project-target";
    receipt.candidate = "must-not-project-candidate";
    receipt.state = HumanCallTransferState::Transferring;
    receipt.ack_code = 200;
    receipt.ack_revision = 1;
    receipt.cancel_token = "original-cancel";
    receipt.cancel_state = HumanCallControlState::Rejected;
    receipt.cancel_code = 409;
    receipt.cancel_reason = "invalidState";
    EXPECT_EQ(
        browser_call_transfer_receipt(receipt),
        R"({"kind":"callTransferReceipt","sessionId":"original-leg","controlToken":"original-transfer","state":"transferring","code":200,"ackRevision":1,"resultRevision":0,"reason":"","candidatePresent":true,"cancelToken":"original-cancel","cancelState":"rejected","cancelCode":409,"cancelReason":"invalidState"})");
    // 独立结果先到，不可补造200受理；completed只表示撤候选原ACK，不借此关闭原Peer。
    receipt.state = HumanCallTransferState::Cancelled;
    receipt.ack_code = 0;
    receipt.ack_revision = 0;
    receipt.result_revision = 2;
    receipt.reason = "candidateCancelled";
    receipt.cancel_state = HumanCallControlState::Completed;
    receipt.cancel_code = 200;
    receipt.cancel_reason.clear();
    const auto body = browser_call_transfer_receipt(receipt);
    EXPECT_NE(body.find(R"("state":"cancelled","code":0,"ackRevision":0,"resultRevision":2)"),
              std::string::npos);
    EXPECT_NE(body.find(R"("cancelState":"completed","cancelCode":200)"), std::string::npos);
    EXPECT_EQ(body.find(receipt.target), std::string::npos);
    EXPECT_EQ(body.find(receipt.candidate), std::string::npos);
    EXPECT_TRUE(media.connected());
    EXPECT_FALSE(media.take_command());
}

// TransferReceiptFixedEnumsAndUnknown 各合法状态不借当前媒体实态补齐ACK，非法字段不透传自由正文。
TEST(BrowserCallMediaTest, TransferReceiptFixedEnumsAndUnknown) {
    HumanCallTransferReceipt receipt;
    receipt.session = "original-leg";
    receipt.token = "original-transfer";
    for (const auto state : {HumanCallTransferState::Pending, HumanCallTransferState::Transferring,
                             HumanCallTransferState::Transferred, HumanCallTransferState::Failed,
                             HumanCallTransferState::Cancelled, HumanCallTransferState::Rejected,
                             HumanCallTransferState::Unknown}) {
        receipt.state = state;
        for (const auto cancel : {HumanCallControlState::Idle, HumanCallControlState::Pending,
                                  HumanCallControlState::Completed, HumanCallControlState::Rejected,
                                  HumanCallControlState::Unknown}) {
            receipt.cancel_state = cancel;
            EXPECT_FALSE(browser_call_transfer_receipt(receipt).empty());
        }
    }
    receipt.state = HumanCallTransferState::Idle;
    EXPECT_TRUE(browser_call_transfer_receipt(receipt).empty());
    receipt.state = HumanCallTransferState::Unknown;
    for (const auto code : {-1, 201, 399, 600}) {
        receipt.ack_code = code;
        EXPECT_TRUE(browser_call_transfer_receipt(receipt).empty());
    }
    receipt.ack_code = 0;
    receipt.cancel_code = 201;
    EXPECT_TRUE(browser_call_transfer_receipt(receipt).empty());
    receipt.cancel_code = 0;
    receipt.reason = "free-platform-error";
    EXPECT_TRUE(browser_call_transfer_receipt(receipt).empty());
    receipt.reason.clear();
    receipt.cancel_reason = "free-platform-error";
    EXPECT_TRUE(browser_call_transfer_receipt(receipt).empty());
    receipt.cancel_reason.clear();
    receipt.ack_revision = 9007199254740992ULL;
    EXPECT_TRUE(browser_call_transfer_receipt(receipt).empty());
    receipt.ack_revision = 0;
    receipt.result_revision = 9007199254740992ULL;
    EXPECT_TRUE(browser_call_transfer_receipt(receipt).empty());
    receipt.result_revision = 0;
    receipt.token = "bad\n";
    EXPECT_TRUE(browser_call_transfer_receipt(receipt).empty());
    receipt.token = "original-transfer";
    receipt.cancel_token = std::string(129, 'x');
    EXPECT_TRUE(browser_call_transfer_receipt(receipt).empty());
    receipt.cancel_token.clear();
    receipt.session.clear();
    EXPECT_TRUE(browser_call_transfer_receipt(receipt).empty());
}

// BrowserCallMediaTest 验证answer前连通回调无效、结束时可重建Peer。
TEST(BrowserCallMediaTest, HandshakeAndRelease) {
    BrowserCallMedia media;
    EXPECT_FALSE(media.report_connected());
    ASSERT_TRUE(media.begin());
    ASSERT_TRUE(media.take_command().has_value());
    EXPECT_FALSE(media.report_connected());
    EXPECT_FALSE(media.apply_answer("bad"));
    ASSERT_TRUE(media.apply_answer("v=0\r\no=server\r\n"));
    ASSERT_TRUE(media.report_connected());
    EXPECT_TRUE(media.connected());
    media.close();
    EXPECT_FALSE(media.connected());
    EXPECT_TRUE(media.take_command().has_value());  // answer仍按先入先出。
    EXPECT_TRUE(media.take_command().has_value());  // stop在answer后。
    EXPECT_TRUE(media.begin());
}

// BrowserCallWireTest 本地界面不能携带任意云端命令或额外身份字段。
TEST(BrowserCallWireTest, RejectUnexpectedFields) {
    CallBrowserRequest out;
    EXPECT_TRUE(decode_call_browser(R"({"kind":"callAccept"})", out));
    EXPECT_EQ(out.kind, "callAccept");
    EXPECT_TRUE(decode_call_browser(R"({"kind":"callDial","target":"p1:camera-02"})", out));
    EXPECT_EQ(out.target, "p1:camera-02");
    EXPECT_FALSE(decode_call_browser(R"({"kind":"callDial","target":"p1/camera-02"})", out));
    EXPECT_FALSE(
        decode_call_browser(R"({"kind":"callDial","target":"p1:camera-02","owner":1})", out));
    EXPECT_FALSE(decode_call_browser(R"({"kind":"callAccept","sessionId":"other"})", out));
    EXPECT_TRUE(out.kind.empty());
    EXPECT_FALSE(decode_call_browser(R"({"kind":"callOffer","sdp":"bad"})", out));
    EXPECT_TRUE(decode_call_browser(R"({"kind":"callOffer","sdp":"v=0\r\no=device\r\n"})", out));
    EXPECT_TRUE(
        decode_call_browser(R"({"kind":"callTrack","trackId":"mic-1","state":"paused"})", out));
    EXPECT_EQ(out.track_id, "mic-1");
    EXPECT_FALSE(
        decode_call_browser(R"({"kind":"callTrack","trackId":"speaker-1","state":"active"})", out));
    EXPECT_FALSE(decode_call_browser(
        R"({"kind":"callTrack","trackId":"video-1","state":"active","sessionId":"other"})", out));
    EXPECT_FALSE(decode_call_browser(R"({"kind":"mediaState","params":{}})", out));
}
}  // namespace ur::device
