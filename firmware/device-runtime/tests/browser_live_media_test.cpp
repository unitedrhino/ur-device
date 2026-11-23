// 文件说明：实时节目回环端口确定性契约测试；模拟actual不替代真实WebRTC/PCM/新IDR验收。
#include "browser_live_media.hpp"
#include <gtest/gtest.h>

namespace ur::device {
namespace {
// invite 通过生产解析取得原完整AV接收组，不手填字段绕过信令校验。
LiveInvite invite() {
    LiveInvite value;
    EXPECT_EQ(
        decode_live_invite(
            WireRoute::Signaling,
            R"({"method":"sessionInvite","msgToken":"original","sessionId":"programme","eventSeq":1,"params":{"purpose":"liveBroadcast","admission":"direct","transport":"webrtc","expiresAt":100000,"tracks":[{"trackId":"broadcast-out","kind":"audio","sourceId":"broadcast-audio","direction":"recvonly","codec":"opus","state":"requested"},{"trackId":"broadcast-video-out","kind":"video","sourceId":"broadcast-video","direction":"recvonly","codec":"H264","state":"requested"}]}})",
            value),
        WireResult::Ok);
    return value;
}
// local 仅构造原回环instance/m，原云端session/身份/token不进入浏览器消息。
std::string local(std::string kind, std::string extra = {}, std::string instance = "1") {
    return "{\"kind\":\"" + kind + "\",\"instance\":\"" + instance + "\",\"mediaRevision\":1" +
           extra + "}";
}
// gate 保留原完整组和本地票据，不持平台焦点身份。
std::string gate(std::uint64_t ticket, bool enabled) {
    return "{\"kind\":\"liveLaneGated\",\"lane\":\"audioVideo\",\"gateTicket\":\"" +
           std::to_string(ticket) + "\",\"enabled\":" + (enabled ? "true}" : "false}");
}
// operation 通过生产单轨控制解析构造，不使用任意轨/源扩大本地输出。
LiveTrackControl operation(bool video = false, std::string op = "resume") {
    const auto body =
        "{\"method\":\"trackControl\",\"msgToken\":\"control\",\"sessionId\":"
        "\"programme\",\"eventSeq\":3,\"params\":{\"trackId\":\"" +
        std::string(video ? "broadcast-video-out" : "broadcast-out") + "\",\"operation\":\"" + op +
        "\",\"mediaRevision\":1}}";
    LiveTrackControl value;
    EXPECT_EQ(decode_live_track_control(WireRoute::Signaling, body, invite(), value),
              WireResult::Ok);
    return value;
}
// BrowserLiveMediaTest 借唯一父业务和完整末端组；所有网络/输出/退出facts分别可控。
class BrowserLiveMediaTest : public ::testing::Test {
protected:
    BrowserLiveLane lane{"audioVideo"};                // 固定组，不使用file输出或相机/采集。
    std::optional<std::string> parent{"ai-original"};  // 模拟原C++父会话查询，不是浏览器自报。
    std::unique_ptr<BrowserLiveMediaPort> port;        // 先于原lane销毁。
    void SetUp() override {
        port = std::make_unique<BrowserLiveMediaPort>(
            invite(), lane, BrowserLiveReadiness{true, true}, [&] { return parent; }, "ai-original",
            60000, 1, 10);
    }
    // idle 收录原false实际票据，不把构造或派发当输出关闭。
    void idle() {
        ASSERT_TRUE(lane.request_output_gate(false, 10));
        ASSERT_TRUE(lane.take_command());
        ASSERT_TRUE(lane.receive(gate(1, false), 11));
    }
    // connect 独立推进prepare/Peer/offer/answer/connected；没有任何输出active事实。
    void connect() {
        idle();
        ASSERT_EQ(port->prepare(invite(), 60000), LivePreparation::Ready);
        ASSERT_TRUE(port->begin_peer());
        ASSERT_TRUE(port->take_command());
        ASSERT_TRUE(port->receive(local("liveOffer", R"(,"sdp":"v=0\r\nfixture")")));
        ASSERT_TRUE(port->take_offer());
        ASSERT_TRUE(port->apply_answer("v=0\r\nanswer"));
        ASSERT_TRUE(port->take_command());
        ASSERT_TRUE(port->receive(local("liveConnected")));
        ASSERT_TRUE(port->connected());
    }
    // enable 只确认当前focus末端mask，不生成任何单轨意图/actual。
    void enable() {
        ASSERT_TRUE(lane.request_output_gate(true, 12));
        ASSERT_TRUE(lane.take_command());
        ASSERT_TRUE(lane.receive(gate(2, true), 13));
    }
};
}  // namespace
TEST_F(BrowserLiveMediaTest, PrepareRequiresOriginalActualIdleAndFixedProgramme) {
    EXPECT_EQ(port->prepare(invite(), 60000), LivePreparation::Busy);
    EXPECT_FALSE(port->begin_peer());
    idle();
    auto changed = invite();
    changed.envelope.session = "other";
    EXPECT_EQ(port->prepare(changed, 60000), LivePreparation::Denied);
    EXPECT_EQ(port->prepare(invite(), 60001), LivePreparation::Denied);
    EXPECT_EQ(port->prepare(invite(), 60000), LivePreparation::Ready);
    EXPECT_FALSE(port->take_fact());
    EXPECT_FALSE(port->connected());
}
TEST_F(BrowserLiveMediaTest, PeerOfferAnswerAndConnectionAreSeparateFromPlayback) {
    connect();
    EXPECT_FALSE(port->presentation_permitted("broadcast-out"));
    EXPECT_FALSE(port->take_fact());
    EXPECT_FALSE(port->apply_answer("v=0\r\nchanged"));
    EXPECT_FALSE(port->receive(local("liveOffer", R"(,"sdp":"v=0\r\nold")")));
    EXPECT_FALSE(port->begin_track(operation(), 1));
}
TEST_F(BrowserLiveMediaTest, OriginalIndependentAudioVideoActualFacts) {
    connect();
    enable();
    ASSERT_TRUE(port->begin_track(operation(), 5));
    ASSERT_TRUE(port->take_command());
    ASSERT_TRUE(port->begin_track(operation(true), 6));
    ASSERT_TRUE(port->take_command());
    EXPECT_FALSE(port->take_fact());
    ASSERT_TRUE(port->receive(local(
        "liveTrackFact",
        R"(,"trackId":"broadcast-out","generation":5,"code":200,"state":"active","gateTicket":"2")")));
    auto fact = port->take_fact();
    ASSERT_TRUE(fact);
    EXPECT_EQ(fact->generation, 5U);
    EXPECT_EQ(fact->state, "active");
    EXPECT_FALSE(port->take_fact());
    ASSERT_TRUE(port->receive(local(
        "liveTrackFact",
        R"(,"trackId":"broadcast-video-out","generation":6,"code":200,"state":"active","gateTicket":"2")")));
    fact = port->take_fact();
    ASSERT_TRUE(fact);
    EXPECT_EQ(fact->track_id, "broadcast-video-out");
}
TEST_F(BrowserLiveMediaTest, RejectsWrongInstanceGenerationMediaAndFutureOrExtraFacts) {
    connect();
    enable();
    ASSERT_TRUE(port->begin_track(operation(), 5));
    const auto fields = R"(,"trackId":"broadcast-out","generation":5,"code":200,"state":"active")";
    EXPECT_FALSE(port->receive(local("liveTrackFact", fields, "2")));
    EXPECT_FALSE(port->receive(
        local("liveTrackFact",
              R"(,"trackId":"broadcast-out","generation":6,"code":200,"state":"active")")));
    EXPECT_FALSE(port->receive(local(
        "liveTrackFact",
        R"(,"trackId":"broadcast-out","generation":5,"code":200,"state":"active","source":"camera")")));
    EXPECT_FALSE(port->receive(
        local("liveTrackFact",
              R"(,"trackId":"broadcast-out","generation":5,"code":200,"state":"paused")")));
    EXPECT_FALSE(port->take_fact());
}
TEST_F(BrowserLiveMediaTest, TemporaryFocusCancellationNeedsOriginalActualResourceBusy) {
    connect();
    enable();
    ASSERT_TRUE(port->begin_track(operation(), 5));
    ASSERT_TRUE(lane.request_output_gate(false, 14));
    ASSERT_TRUE(lane.take_command());
    EXPECT_TRUE(port->authorization_valid());
    EXPECT_TRUE(port->connected());
    EXPECT_FALSE(port->presentation_permitted("broadcast-out"));
    EXPECT_FALSE(port->receive(
        local("liveTrackFact",
              R"(,"trackId":"broadcast-out","generation":5,"code":200,"state":"active")")));
    ASSERT_TRUE(lane.receive(gate(3, false), 15));
    ASSERT_TRUE(port->receive(
        local("liveTrackFact",
              R"(,"trackId":"broadcast-out","generation":5,"code":409,"failure":"outputBusy")")));
    const auto fact = port->take_fact();
    ASSERT_TRUE(fact);
    EXPECT_EQ(fact->failure, LiveMediaFailure::OutputBusy);
    EXPECT_TRUE(fact->state.empty());
    ASSERT_TRUE(lane.request_output_gate(true, 16));
    ASSERT_TRUE(lane.take_command());
    ASSERT_TRUE(lane.receive(gate(4, true), 17));
    EXPECT_FALSE(port->take_fact());
    EXPECT_TRUE(port->authorization_valid());
}
TEST_F(BrowserLiveMediaTest, FreeErrorReasonAndInventedFailureCodeCannotBecomeActual) {
    connect();
    enable();
    ASSERT_TRUE(port->begin_track(operation(), 5));
    EXPECT_FALSE(port->receive(
        local("liveTrackFact",
              R"(,"trackId":"broadcast-out","generation":5,"code":503,"failure":"privateError")")));
    EXPECT_FALSE(port->receive(
        local("liveTrackFact",
              R"(,"trackId":"broadcast-out","generation":5,"code":503,"failure":"outputBusy")")));
    ASSERT_TRUE(port->receive(
        local("liveTrackFact",
              R"(,"trackId":"broadcast-out","generation":5,"code":503,"failure":"mediaFailed")")));
    EXPECT_EQ(port->take_fact()->failure, LiveMediaFailure::MediaFailed);
}
TEST_F(BrowserLiveMediaTest, SourceParentChangesCannotBorrowNewParentOrReconnect) {
    connect();
    parent = "new-ai";
    EXPECT_FALSE(port->authorization_valid());
    ASSERT_TRUE(port->poll(20));
    EXPECT_FALSE(port->connected());
    EXPECT_FALSE(port->stop_all());
    const auto close = port->take_command();
    ASSERT_TRUE(close);
    EXPECT_NE(close->find("liveClose"), std::string::npos);
    parent = "ai-original";
    EXPECT_FALSE(port->authorization_valid());
    EXPECT_FALSE(port->receive(local("liveConnected")));
}
TEST_F(BrowserLiveMediaTest, CloseHasReservedSlotAndRequiresAllActualResourceFacts) {
    connect();
    enable();
    ASSERT_TRUE(port->begin_track(operation(), 5));
    EXPECT_FALSE(port->stop_all());
    const auto close = port->take_command();
    ASSERT_TRUE(close);
    EXPECT_NE(close->find("liveClose"), std::string::npos);
    EXPECT_FALSE(port->take_command());
    EXPECT_FALSE(port->receive(local(
        "liveStopped",
        R"(,"peerClosed":true,"tracksStopped":true,"outputsDetached":true,"callbacksStopped":true,"transformsStopped":false)")));
    EXPECT_FALSE(port->stop_all());
    ASSERT_TRUE(port->receive(local(
        "liveStopped",
        R"(,"peerClosed":true,"tracksStopped":true,"outputsDetached":true,"callbacksStopped":true,"transformsStopped":true)")));
    EXPECT_TRUE(port->stop_all());
    EXPECT_FALSE(port->receive(
        local("liveTrackFact",
              R"(,"trackId":"broadcast-out","generation":5,"code":200,"state":"active")")));
}
TEST_F(BrowserLiveMediaTest, FactorySingleReservationAndActualCloseRetirement) {
    idle();
    BrowserLiveLane audio("audio"), video("video");
    BrowserLiveFactory factory(
        {&audio, &video, &lane}, {true, true}, [&] { return parent; }, 60000);
    EXPECT_FALSE(factory.create(invite()));
    ASSERT_TRUE(factory.clock(10));
    auto first = factory.create(invite());
    ASSERT_TRUE(first);
    EXPECT_FALSE(factory.create(invite()));
    ASSERT_EQ(first->prepare(invite(), 60000), LivePreparation::Ready);
    EXPECT_TRUE(first->stop_all());
    first.reset();  // 未分配Peer确无原媒体资源，原预约可回收。
    ASSERT_TRUE(factory.create(invite()));
    EXPECT_FALSE(factory.clock(9));
    EXPECT_FALSE(factory.create(invite()));
}
TEST_F(BrowserLiveMediaTest, PreparedRealFormatAndForgedBindingsAreRejected) {
    idle();
    BrowserLiveMediaPort unsupported(
        invite(), lane, {true, false}, [&] { return parent; }, "ai-original", 60000, 2, 10);
    EXPECT_EQ(unsupported.prepare(invite(), 60000), LivePreparation::Unsupported);
    auto forged = invite();
    forged.video = false;
    BrowserLiveMediaPort invalid(
        forged, lane, {true, true}, [&] { return parent; }, "ai-original", 60000, 3, 10);
    EXPECT_FALSE(invalid.authorization_valid());
    EXPECT_EQ(invalid.prepare(forged, 60000), LivePreparation::Denied);
}
TEST_F(BrowserLiveMediaTest, UnknownActualPeerReleaseCannotRecycleFactoryReservation) {
    idle();
    BrowserLiveLane audio("audio"), video("video");
    BrowserLiveFactory factory(
        {&audio, &video, &lane}, {true, true}, [&] { return parent; }, 60000);
    ASSERT_TRUE(factory.clock(10));
    auto original = factory.create(invite());
    ASSERT_TRUE(original);
    ASSERT_EQ(original->prepare(invite(), 60000), LivePreparation::Ready);
    ASSERT_TRUE(original->begin_peer());
    EXPECT_FALSE(original->stop_all());
    original.reset();  // 错误宿主提前销毁不能把未知媒体退出变成可重用预约。
    EXPECT_FALSE(factory.create(invite()));
}
TEST_F(BrowserLiveMediaTest, CurrentIdleAfterCompleteLeaseCanPrepareLaterProgramme) {
    idle();
    ASSERT_TRUE(lane.request_output_gate(true, 12));
    ASSERT_TRUE(lane.take_command());
    ASSERT_TRUE(lane.receive(gate(2, true), 13));
    ASSERT_TRUE(lane.request_output_gate(false, 14));
    ASSERT_TRUE(lane.take_command());
    ASSERT_TRUE(lane.receive(gate(3, false), 15));
    EXPECT_FALSE(lane.output_gate_confirmed(1, false));
    EXPECT_TRUE(lane.idle());
    EXPECT_EQ(port->prepare(invite(), 60000), LivePreparation::Ready);
}
TEST_F(BrowserLiveMediaTest, QueuedActualActiveUsesOriginalTrueTicketDespiteLaterMaskClosing) {
    connect();
    enable();
    ASSERT_TRUE(port->begin_track(operation(), 5));
    ASSERT_TRUE(port->take_command());
    // 真实驱动在原true票据2下已合法形成实际完成；Bridge晚交，不是新mask下再次输出。
    const auto completed = local(
        "liveTrackFact",
        R"(,"trackId":"broadcast-out","generation":5,"code":200,"state":"active","gateTicket":"2")");
    ASSERT_TRUE(lane.request_output_gate(false, 14));
    ASSERT_TRUE(lane.take_command());
    EXPECT_FALSE(port->presentation_permitted("broadcast-out"));
    ASSERT_TRUE(port->receive(completed));
    const auto fact = port->take_fact();
    ASSERT_TRUE(fact);
    EXPECT_EQ(fact->state, "active");
    ASSERT_TRUE(lane.receive(gate(3, false), 15));
    ASSERT_TRUE(lane.request_output_gate(true, 16));
    ASSERT_TRUE(lane.take_command());
    ASSERT_TRUE(lane.receive(gate(4, true), 17));
    EXPECT_FALSE(port->take_fact());
    EXPECT_FALSE(port->take_command());
}
TEST_F(BrowserLiveMediaTest, ActiveCannotBorrowNewRestoredTicketForOldOperation) {
    connect();
    enable();
    ASSERT_TRUE(port->begin_track(operation(), 5));
    ASSERT_TRUE(lane.request_output_gate(false, 14));
    ASSERT_TRUE(lane.take_command());
    ASSERT_TRUE(lane.receive(gate(3, false), 15));
    ASSERT_TRUE(lane.request_output_gate(true, 16));
    ASSERT_TRUE(lane.take_command());
    ASSERT_TRUE(lane.receive(gate(4, true), 17));
    EXPECT_FALSE(port->receive(local(
        "liveTrackFact",
        R"(,"trackId":"broadcast-out","generation":5,"code":200,"state":"active","gateTicket":"4")")));
    EXPECT_FALSE(port->take_fact());
    ASSERT_TRUE(port->receive(local(
        "liveTrackFact",
        R"(,"trackId":"broadcast-out","generation":5,"code":200,"state":"active","gateTicket":"2")")));
    EXPECT_EQ(port->take_fact()->generation, 5U);
}
TEST_F(BrowserLiveMediaTest, CanceledGenerationOrSourceRevocationCannotUseOldTrueTicket) {
    connect();
    enable();
    ASSERT_TRUE(port->begin_track(operation(), 5));
    ASSERT_TRUE(port->receive(
        local("liveTrackFact",
              R"(,"trackId":"broadcast-out","generation":5,"code":409,"failure":"outputBusy")")));
    ASSERT_TRUE(port->take_fact());
    EXPECT_FALSE(port->receive(local(
        "liveTrackFact",
        R"(,"trackId":"broadcast-out","generation":5,"code":200,"state":"active","gateTicket":"2")")));
    ASSERT_TRUE(port->begin_track(operation(), 6));
    parent = "new-ai";
    EXPECT_FALSE(port->receive(local(
        "liveTrackFact",
        R"(,"trackId":"broadcast-out","generation":6,"code":200,"state":"active","gateTicket":"2")")));
    EXPECT_FALSE(port->take_fact());
}
}  // namespace ur::device
