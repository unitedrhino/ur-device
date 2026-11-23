// 文件说明：单节目端点同连接回归；严格原报文及可控actual事实，不代表真实ICE/PCM/视频验收。
#include "browser_live_endpoint.hpp"
#include <gtest/gtest.h>
namespace ur::device {
namespace {
// EndpointPublisher 发布受理与原事实分开；背压时保留每次尝试观察，不假网络ACK或实际媒体完成。
struct EndpointPublisher final : PublishPort {
    bool accepting = true;
    std::vector<Envelope> sent;
    std::vector<std::string> bytes;
    bool publish(std::string_view, std::string_view body) override {
        Envelope message;
        EXPECT_EQ(decode_envelope(body, message), DecodeResult::Ok);
        sent.push_back(message);
        bytes.emplace_back(body);
        return accepting;
    }
};
// invitation 全部轨字段由生产wire校验，设备身份/URL不进入合成夹具。
std::string invitation(std::string session = "programme", std::string token = "invite") {
    return "{\"method\":\"sessionInvite\",\"msgToken\":\"" + token + "\",\"sessionId\":\"" +
           session +
           R"(","eventSeq":1,"params":{"purpose":"liveBroadcast","admission":"direct","transport":"webrtc","expiresAt":100000,"tracks":[{"trackId":"broadcast-out","kind":"audio","sourceId":"broadcast-audio","direction":"recvonly","codec":"opus","state":"requested"},{"trackId":"broadcast-video-out","kind":"video","sourceId":"broadcast-video","direction":"recvonly","codec":"H264","state":"requested"}]}})";
}
// BrowserLiveEndpointTest 使用同一父/工厂/MQTT，原网络与actual停止由用例逐项交付。
class BrowserLiveEndpointTest : public ::testing::Test {
protected:
    EndpointPublisher mqtt;
    std::array<BrowserLiveLane, 3> lanes{BrowserLiveLane("audio"), BrowserLiveLane("video"),
                                         BrowserLiveLane("audioVideo")};
    std::optional<std::string> parent{"original-ai"};
    BrowserLiveFactory factory{
        {&lanes[0], &lanes[1], &lanes[2]}, {true, true}, [&] { return parent; }, 60000};
    BrowserLiveEndpoint endpoint{mqtt, factory, {true, true}, "up", "down", 60000};
    std::uint64_t now = 10;
    // gate 显式实际末端确认，不能从请求受理推导许可；焦点仲裁器专用测试另覆盖。
    void gate(std::size_t index, bool enabled) {
        const auto ticket = lanes[index].request_output_gate(enabled, ++now);
        ASSERT_TRUE(ticket);
        ASSERT_TRUE(lanes[index].take_command());
        ASSERT_TRUE(lanes[index].receive("{\"kind\":\"liveLaneGated\",\"lane\":\"" +
                                             lanes[index].lane() + "\",\"gateTicket\":\"" +
                                             std::to_string(*ticket) +
                                             "\",\"enabled\":" + (enabled ? "true}" : "false}"),
                                         ++now));
    }
    void SetUp() override {
        for (std::size_t index = 0; index < 3; ++index)
            gate(index, false);
        ASSERT_TRUE(endpoint.begin_connection(++now));
    }
    // body 只构造下行标准信封，method字段由生产Client再严格按方法解码。
    bool body(std::string token, std::string method, std::string payload,
              std::optional<int> code = 200, std::optional<std::uint64_t> sequence = {},
              std::string session = "programme") {
        Envelope message;
        message.token = std::move(token);
        message.session = std::move(session);
        message.method = std::move(method);
        message.payload = std::move(payload);
        message.code = code;
        message.event_sequence = sequence;
        message.request = false;
        std::string serialized;
        EXPECT_EQ(encode_envelope(message, serialized), DecodeResult::Ok);
        return endpoint.receive("down", serialized, ++now, 1000);
    }
    // local 仅本地原instance/m，不把云session/token交浏览器驱动。
    std::string local(std::string kind, std::string extra = {}, std::string instance = "1") {
        return "{\"kind\":\"" + kind + "\",\"instance\":\"" + instance + "\",\"mediaRevision\":1" +
               extra + "}";
    }
    // connected 原接听/offer/answer/网络/平台事件均独立推进，无默认播放事实。
    void connected() {
        ASSERT_TRUE(endpoint.receive("down", invitation(), ++now, 1000));
        ASSERT_EQ(mqtt.sent.back().method, "sessionAccept");
        ASSERT_TRUE(body("invite", "requestAck",
                         R"({"requestMethod":"sessionAccept","result":{"state":"negotiating"}})"));
        ASSERT_TRUE(endpoint.active_media());
        ASSERT_TRUE(endpoint.active_media()->take_command());
        ASSERT_TRUE(endpoint.receive_local(local("liveOffer", R"(,"sdp":"v=0\r\ncontract")")));
        endpoint.tick(++now, 1000);
        ASSERT_EQ(mqtt.sent.back().method, "mediaOffer");
        const auto offer = mqtt.sent.back().token;
        ASSERT_TRUE(
            body(offer, "mediaAnswer",
                 R"({"mediaRevision":1,"webrtc":{"iceMode":"complete","sdp":"v=0\r\nanswer"}})"));
        ASSERT_TRUE(endpoint.active_media()->take_command());
        ASSERT_TRUE(endpoint.receive_local(local("liveConnected")));
        endpoint.tick(++now, 1000);
        ASSERT_EQ(mqtt.sent.back().method, "mediaState");
        const auto media = mqtt.sent.back().token;
        ASSERT_TRUE(body(
            media, "requestAck",
            R"({"requestMethod":"mediaState","result":{"state":"connected","mediaRevision":1}})"));
        ASSERT_TRUE(body("established", "sessionEstablished",
                         R"({"state":"established","mediaRevision":1})", {}, 2));
        ASSERT_TRUE(endpoint.scope());
        EXPECT_TRUE(endpoint.scope()->established);
    }
    // original_audio_actual 核原控制/驱动代次，不允许下行trackState冒充实际完成。
    std::string original_audio_actual() {
        gate(2, true);
        const std::string command =
            R"({"msgToken":"audio-original","method":"trackControl","sessionId":"programme","eventSeq":3,"params":{"trackId":"broadcast-out","operation":"resume","mediaRevision":1}})";
        EXPECT_TRUE(endpoint.receive("down", command, ++now, 1000));
        EXPECT_TRUE(endpoint.active_media()->take_command());
        EXPECT_TRUE(endpoint.receive_local(local(
            "liveTrackFact",
            R"(,"trackId":"broadcast-out","generation":1,"code":200,"state":"active","gateTicket":"2")")));
        endpoint.tick(++now, 1000);
        return command;
    }
    // stopped 保持平台终态与原所有实际资源退出分别报告，未齐全前不回收唯一预约。
    void stopped() {
        ASSERT_TRUE(
            body("ended", "sessionEnded", R"({"state":"ended","reason":"userStopped"})", {}, 4));
        ASSERT_TRUE(endpoint.active_media()->take_command());
        ASSERT_FALSE(endpoint.released());
        ASSERT_TRUE(endpoint.receive_local(local(
            "liveStopped",
            R"(,"peerClosed":true,"tracksStopped":true,"outputsDetached":true,"callbacksStopped":true,"transformsStopped":true)")));
        endpoint.tick(++now, 1000);
    }
};
}  // namespace
TEST_F(BrowserLiveEndpointTest, OneCurrentFactoryNetworkAndBoundedOwnedSession) {
    connected();
    EXPECT_TRUE(endpoint.owns("programme"));
    EXPECT_FALSE(endpoint.owns("unknown"));
    EXPECT_TRUE(endpoint.matches(mqtt, "up", "down"));
    EndpointPublisher other;
    EXPECT_FALSE(endpoint.matches(other, "up", "down"));
    EXPECT_FALSE(endpoint.receive("down", invitation("second", "new-token"), ++now, 1000));
    EXPECT_FALSE(endpoint.receive("other", invitation(), ++now, 1000));
    EXPECT_TRUE(endpoint.scope()->established);
    EXPECT_FALSE(endpoint.released());
}
TEST_F(BrowserLiveEndpointTest, OriginalActualHistorySurvivesRetirementWithoutRecreatingMedia) {
    connected();
    const auto original = original_audio_actual();
    const auto reply = mqtt.bytes.back();
    stopped();
    ASSERT_TRUE(endpoint.released());
    ASSERT_TRUE(endpoint.settled());
    ASSERT_FALSE(endpoint.active_media());
    EXPECT_TRUE(endpoint.owns("programme"));
    const auto before = mqtt.bytes.size();
    EXPECT_TRUE(endpoint.receive("down", invitation(), ++now, 1000));
    EXPECT_EQ(mqtt.bytes.size(), before);  // Ended原准入决策不能重发accept或再分配Peer。
    EXPECT_TRUE(endpoint.receive("down", original, ++now, 1000));
    EXPECT_EQ(mqtt.bytes.back(), reply);
    EXPECT_FALSE(endpoint.active_media());
}
TEST_F(BrowserLiveEndpointTest, ChangedOriginalInviteCannotUseNewSessionOrTracks) {
    connected();
    stopped();
    EXPECT_FALSE(endpoint.receive("down", invitation("changed", "invite"), ++now, 1000));
    auto changed = invitation();
    const auto at = changed.find("100000");
    changed.replace(at, 6, "100001");
    EXPECT_FALSE(endpoint.receive("down", changed, ++now, 1000));
    EXPECT_FALSE(endpoint.active_media());
}
TEST_F(BrowserLiveEndpointTest, NextSessionRequiresNewActualFalseNodeAndCannotReuseControlToken) {
    connected();
    const auto original = original_audio_actual();
    stopped();
    ASSERT_TRUE(endpoint.rearm_lane());
    EXPECT_FALSE(endpoint.receive("down", invitation("next", "invite-next"), ++now, 1000));
    gate(2, false);
    ASSERT_TRUE(endpoint.rearmed("audioVideo"));
    EXPECT_TRUE(endpoint.receive("down", invitation("next", "invite-next"), ++now, 1000));
    auto changed = original;
    const auto at = changed.find("programme");
    changed.replace(at, 9, "next");
    EXPECT_FALSE(endpoint.receive("down", changed, ++now, 1000));
}
TEST_F(BrowserLiveEndpointTest, BackpressuredActualReceiptCannotBeDroppedOnTerminal) {
    connected();
    mqtt.accepting = false;
    original_audio_actual();
    stopped();
    EXPECT_FALSE(endpoint.settled());
    EXPECT_TRUE(endpoint.active_media());
    EXPECT_FALSE(endpoint.receive("down", invitation("next", "invite-next"), ++now, 1000));
    mqtt.accepting = true;
    endpoint.tick(++now, 1000);
    EXPECT_TRUE(endpoint.settled());
    EXPECT_FALSE(endpoint.active_media());
    EXPECT_EQ(mqtt.sent.back().method, "trackState");
    EXPECT_EQ(mqtt.sent.back().code, 200);
}
TEST_F(BrowserLiveEndpointTest, ForgedActualWrongRouteAndMalformedInviteNeverCreateReceiver) {
    EXPECT_FALSE(endpoint.receive("down", "{}", ++now, 1000));
    EXPECT_FALSE(endpoint.receive("down", invitation(), ++now, 0));
    EXPECT_FALSE(endpoint.receive_local(local("liveConnected")));
    EXPECT_FALSE(endpoint.active_media());
    EXPECT_TRUE(endpoint.released());
    EXPECT_FALSE(endpoint.receive("down", invitation(), ++now, 100001));
}
TEST_F(BrowserLiveEndpointTest, ClosedConnectionKeepsHistoryAndRequiresActualResourcesToExit) {
    connected();
    endpoint.close(++now);
    EXPECT_FALSE(endpoint.released());
    EXPECT_FALSE(endpoint.begin_connection(++now));
    EXPECT_TRUE(endpoint.receive_local(local(
        "liveStopped",
        R"(,"peerClosed":true,"tracksStopped":true,"outputsDetached":true,"callbacksStopped":true,"transformsStopped":true)")));
    endpoint.tick(++now, 1000);
    EXPECT_TRUE(endpoint.begin_connection(++now));
    EXPECT_TRUE(endpoint.owns("programme"));
    EXPECT_FALSE(endpoint.active_media());
}
TEST_F(BrowserLiveEndpointTest, ClockRollbackNeverClearsOriginalSessionFence) {
    connected();
    stopped();
    EXPECT_FALSE(endpoint.begin_connection(1));
    EXPECT_TRUE(endpoint.owns("programme"));
    EXPECT_FALSE(endpoint.receive("down", invitation("next", "invite-next"), 1, 1000));
}
}  // namespace ur::device
