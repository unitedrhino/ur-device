// 文件说明：实时节目核心确定性状态机测试；模拟端口独立交实际事实，不把离线fixture称真实媒体结果。
#include "ur/device/live_client.hpp"
#include <gtest/gtest.h>
#include <deque>

namespace ur::device {
namespace {
// encode 通过公共生产信封生成用例报文，不手拼错误类型绕过线上安全验证。
std::string encode(Envelope value) {
    std::string body;
    EXPECT_EQ(encode_envelope(value, body), DecodeResult::Ok);
    return body;
}
// invitation 是首轮30秒完整AV节目接收许可，不携带采集或自由源引用。
std::string invitation(bool audio = true, bool video = true) {
    Envelope value;
    value.method = "sessionInvite";
    value.token = "live-original";
    value.session = "live-session";
    value.event_sequence = 1;
    const auto audio_track =
        R"({"trackId":"broadcast-out","kind":"audio","sourceId":"broadcast-audio","direction":"recvonly","codec":"opus","state":"requested"})";
    const auto video_track =
        R"({"trackId":"broadcast-video-out","kind":"video","sourceId":"broadcast-video","direction":"recvonly","codec":"H264","state":"requested"})";
    value.payload =
        std::string(
            R"({"purpose":"liveBroadcast","admission":"direct","transport":"webrtc","expiresAt":1030000,"tracks":[)") +
        (audio ? audio_track : "") + (audio && video ? "," : "") + (video ? video_track : "") +
        "]}";
    return encode(std::move(value));
}
// binding 必须从生产LiveWire取得，不手填布尔字段扩大实际接收组。
LiveInvite binding(bool audio = true, bool video = true) {
    LiveInvite out;
    EXPECT_EQ(decode_live_invite(WireRoute::Signaling, invitation(audio, video), out),
              WireResult::Ok);
    return out;
}
// response 构造原关联下行事实，实际网络与输出仍由独立端口交回。
std::string response(std::string method, std::string token, std::string payload,
                     std::optional<int> code = 200, std::optional<std::uint64_t> sequence = {}) {
    Envelope value;
    value.request = false;
    value.method = std::move(method);
    value.token = std::move(token);
    value.session = "live-session";
    value.payload = std::move(payload);
    value.code = code;
    value.event_sequence = sequence;
    return encode(std::move(value));
}
// control 保留每个原意图token/水位；同值新token仍是独立操作，重复原token不得重做。
std::string control(std::string operation, std::string token, std::uint64_t sequence,
                    bool video = false, std::uint64_t revision = 1) {
    Envelope value;
    value.method = "trackControl";
    value.token = std::move(token);
    value.session = "live-session";
    value.event_sequence = sequence;
    value.payload = "{\"trackId\":\"" +
                    std::string(video ? "broadcast-video-out" : "broadcast-out") +
                    "\",\"operation\":\"" + operation +
                    "\",\"mediaRevision\":" + std::to_string(revision) + "}";
    return encode(std::move(value));
}
// LiveMqtt 记录发布尝试原字节，背压仅本地未接纳，不代表远端实际拒绝或执行。
struct LiveMqtt final : PublishPort {
    bool writable = true;             // 独立可控发布背压。
    std::vector<std::string> bodies;  // 所有尝试原字节，用于一致性回归。
    std::vector<Envelope> sent;       // 协议投影，不保存真实环境凭据。
    bool publish(std::string_view, std::string_view body) override {
        Envelope value;
        if (decode_envelope(body, value) != DecodeResult::Ok)
            return false;
        bodies.emplace_back(body);
        sent.push_back(std::move(value));
        return writable;
    }
    // last 只按原方法查实际尝试，不从驱动状态猜请求关联。
    Envelope last(std::string_view method) const {
        for (auto iterator = sent.rbegin(); iterator != sent.rend(); ++iterator)
            if (iterator->method == method)
                return *iterator;
        return {};
    }
};
// LivePort 受理与actual事实刻意分开，默认不生成PeerConnected、非静音PCM或可见画面。
struct LivePort final : LiveMediaPort {
    LivePreparation preparation = LivePreparation::Ready;      // 本地格式与资源预检。
    bool peer_ok = true, answer_ok = true, command_ok = true;  // 明确无副作用受理失败。
    bool network = false, all_stopped = true;                  // 实际网络与全组真实退出分开。
    bool authorized = true;  // 原节目有限授权，与可暂时关闭的输出mask完全独立。
    bool audio_permitted = true, video_permitted = true;               // 原输出组各轨实际许可投影。
    unsigned prepares = 0, peers = 0, answers = 0, stops = 0;          // 观察真实端口调用次数。
    std::vector<std::pair<LiveTrackControl, std::uint64_t>> commands;  // 原操作及本地代次。
    std::deque<LiveMediaFact> facts;  // 用例独立交付有限实际结果，不由begin自动成功。
    LivePreparation prepare(const LiveInvite&, std::uint64_t) override {
        ++prepares;
        return preparation;
    }
    bool begin_peer() override {
        if (!peer_ok)
            return false;
        ++peers;
        return true;
    }
    bool apply_answer(std::string_view) override {
        if (!answer_ok)
            return false;
        ++answers;
        return true;
    }
    bool connected() const override { return network; }
    bool authorization_valid() const override { return authorized; }
    bool begin_track(const LiveTrackControl& command, std::uint64_t generation) override {
        if (!command_ok)
            return false;
        commands.emplace_back(command, generation);
        return true;
    }
    std::optional<LiveMediaFact> take_fact() override {
        if (facts.empty())
            return {};
        auto value = std::move(facts.front());
        facts.pop_front();
        return value;
    }
    bool presentation_permitted(std::string_view track_id) const override {
        return track_id == "broadcast-out" ? audio_permitted : video_permitted;
    }
    bool stop_all() override {
        ++stops;
        return all_stopped;
    }
    // complete 只交最近指定原轨操作的actual，用于测试正向完成；不是生产媒体伪造接口。
    void complete(bool video, std::string state, int code = 200,
                  LiveMediaFailure failure = LiveMediaFailure::MediaFailed) {
        const auto id = video ? "broadcast-video-out" : "broadcast-out";
        for (auto iterator = commands.rbegin(); iterator != commands.rend(); ++iterator) {
            if (iterator->first.track_id != id)
                continue;
            facts.push_back({iterator->second, 1, id, std::move(state), code, failure});
            return;
        }
        ADD_FAILURE() << "original_driver_command_required";
    }
};
// LiveClientTest 在唯一对象/原端口上协商；不会重建对象绕过原历史、时钟或输出门控。
class LiveClientTest : public ::testing::Test {
protected:
    LiveMqtt mqtt;                       // 原同一发布连接模拟。
    LivePort port;                       // 原同一媒体所有者模拟。
    std::unique_ptr<LiveClient> client;  // 生命周期严格晚于端口、早于端口析构。
    std::uint64_t now = 1000;            // 唯一可控单调时钟，无实际等待。
    void SetUp() override {
        client = std::make_unique<LiveClient>(mqtt, port, binding(), "up", "down", 100000);
    }
    bool receive(const std::string& body) { return client->receive("down", body, now++, 1000000); }
    // negotiate 只完成网络/平台建立，不交任何输出事实；初始actual两轨必须保持为空。
    void negotiate(bool audio = true, bool video = true) {
        ASSERT_TRUE(receive(invitation(audio, video)));
        ASSERT_TRUE(receive(
            response("requestAck", "live-original",
                     R"({"requestMethod":"sessionAccept","result":{"state":"negotiating"}})")));
        ASSERT_TRUE(client->offer_ready("v=0\r\nfixture", now++));
        ASSERT_TRUE(receive(response(
            "mediaAnswer", mqtt.last("mediaOffer").token,
            R"({"mediaRevision":1,"webrtc":{"iceMode":"complete","sdp":"v=0\r\nanswer"}})")));
        port.network = true;
        ASSERT_TRUE(client->media_connected(now++));
        ASSERT_TRUE(receive(response(
            "requestAck", mqtt.last("mediaState").token,
            R"({"requestMethod":"mediaState","result":{"state":"connected","mediaRevision":1}})")));
        ASSERT_TRUE(receive(response("sessionEstablished", "established",
                                     R"({"state":"established","mediaRevision":1})", {}, 2)));
        ASSERT_EQ(client->phase(), LivePhase::Established);
    }
    // tick 推进独立端口actual与背压，不用sleep或同步fake回调替代真实异步边界。
    void tick() { client->tick(now++); }
};
}  // namespace

TEST_F(LiveClientTest, AdmissionOriginalTracksAndNoCaptureOrAutomaticActive) {
    negotiate();
    EXPECT_FALSE(client->input_open());
    EXPECT_TRUE(client->actual_state("broadcast-out").empty());
    EXPECT_TRUE(client->actual_state("broadcast-video-out").empty());
    EXPECT_TRUE(port.commands.empty());
    EXPECT_EQ(port.prepares, 1U);
    EXPECT_EQ(port.peers, 1U);
    EXPECT_NE(mqtt.last("sessionAccept").payload.find("broadcast-video-out"), std::string::npos);
}
TEST_F(LiveClientTest, ExpiredWrongTopicAndHandForgedBindingCannotPrepare) {
    EXPECT_FALSE(client->receive("foreign", invitation(), now, 1000000));
    EXPECT_FALSE(client->receive("down", invitation(), now++, 1030000));
    EXPECT_EQ(port.prepares, 0U);
    auto forged = binding();
    forged.audio = false;
    LiveClient invalid(mqtt, port, std::move(forged), "up", "down", 100000);
    EXPECT_EQ(invalid.phase(), LivePhase::Ended);
    EXPECT_FALSE(invalid.receive("down", invitation(), now, 1000000));
}
TEST_F(LiveClientTest, DuplicateInviteAndAdmissionAckDoNotRepeatPrepareOrPeer) {
    ASSERT_TRUE(receive(invitation()));
    ASSERT_TRUE(receive(invitation()));
    EXPECT_EQ(port.prepares, 1U);
    const auto ack =
        response("requestAck", "live-original",
                 R"({"requestMethod":"sessionAccept","result":{"state":"negotiating"}})");
    ASSERT_TRUE(receive(ack));
    ASSERT_TRUE(receive(ack));
    EXPECT_EQ(port.peers, 1U);
}
TEST_F(LiveClientTest, RejectionUsesOriginalReasonAndNoPeer) {
    port.preparation = LivePreparation::Unsupported;
    ASSERT_TRUE(receive(invitation()));
    EXPECT_EQ(mqtt.last("sessionReject").token, "live-original");
    ASSERT_TRUE(receive(response(
        "requestAck", "live-original",
        R"({"requestMethod":"sessionReject","result":{"state":"ended","reason":"unsupportedCapability"}})")));
    EXPECT_EQ(client->phase(), LivePhase::Ended);
    EXPECT_EQ(port.peers, 0U);
}
TEST_F(LiveClientTest, MediaRequiresOriginalAnswerRealConnectionAckAndEstablished) {
    ASSERT_TRUE(receive(invitation()));
    ASSERT_TRUE(
        receive(response("requestAck", "live-original",
                         R"({"requestMethod":"sessionAccept","result":{"state":"negotiating"}})")));
    ASSERT_TRUE(client->offer_ready("v=0\r\nfixture", now++));
    EXPECT_FALSE(client->offer_ready("v=0\r\nchanged", now++));
    EXPECT_FALSE(client->media_connected(now++));
    EXPECT_FALSE(receive(
        response("mediaAnswer", "different",
                 R"({"mediaRevision":1,"webrtc":{"iceMode":"complete","sdp":"v=0\r\nanswer"}})")));
    ASSERT_TRUE(receive(response("sessionEstablished", "early",
                                 R"({"state":"established","mediaRevision":1})", {}, 2)));
    EXPECT_EQ(client->phase(), LivePhase::Negotiating);
    EXPECT_FALSE(receive(control("resume", "early-control", 3)));
    ASSERT_TRUE(receive(
        response("mediaAnswer", mqtt.last("mediaOffer").token,
                 R"({"mediaRevision":1,"webrtc":{"iceMode":"complete","sdp":"v=0\r\nanswer"}})")));
    EXPECT_FALSE(client->media_connected(now++));
    port.network = true;
    ASSERT_TRUE(client->media_connected(now++));
    EXPECT_EQ(client->phase(), LivePhase::Negotiating);
    ASSERT_TRUE(receive(response(
        "requestAck", mqtt.last("mediaState").token,
        R"({"requestMethod":"mediaState","result":{"state":"connected","mediaRevision":1}})")));
    EXPECT_EQ(client->phase(), LivePhase::Established);
    EXPECT_TRUE(port.commands.empty());
}
TEST_F(LiveClientTest, CloudTrackStateAndDecoderReadyCannotBecomeActualPlayback) {
    negotiate();
    EXPECT_FALSE(
        receive(response("trackState", "cloud-fake",
                         R"({"trackId":"broadcast-out","state":"active","mediaRevision":1})")));
    ASSERT_TRUE(receive(control("resume", "audio-resume", 3)));
    port.complete(false, "decoded");
    tick();
    EXPECT_TRUE(client->actual_state("broadcast-out").empty());
    port.complete(false, "active");
    tick();
    EXPECT_EQ(client->actual_state("broadcast-out"), "active");
}
TEST_F(LiveClientTest, AudioAndVideoResumeIndependentWithoutFullGroupDeadlock) {
    negotiate();
    ASSERT_TRUE(receive(control("resume", "audio-resume", 3)));
    ASSERT_TRUE(receive(control("resume", "video-resume", 4, true)));
    EXPECT_EQ(port.commands.size(), 2U);
    port.complete(false, "active");
    tick();
    EXPECT_EQ(client->actual_state("broadcast-out"), "active");
    EXPECT_TRUE(client->actual_state("broadcast-video-out").empty());
    port.complete(true, "active");
    tick();
    EXPECT_EQ(client->actual_state("broadcast-video-out"), "active");
}
TEST_F(LiveClientTest, AudioPauseDoesNotStopOrPauseVideo) {
    negotiate();
    ASSERT_TRUE(receive(control("resume", "audio-resume", 3)));
    port.complete(false, "active");
    tick();
    ASSERT_TRUE(receive(control("resume", "video-resume", 4, true)));
    port.complete(true, "active");
    tick();
    ASSERT_TRUE(receive(control("pause", "audio-pause", 5)));
    port.complete(false, "paused");
    tick();
    EXPECT_EQ(client->actual_state("broadcast-out"), "paused");
    EXPECT_EQ(client->actual_state("broadcast-video-out"), "active");
    EXPECT_EQ(client->phase(), LivePhase::Established);
    EXPECT_EQ(port.stops, 0U);
}
TEST_F(LiveClientTest, ResumeRequiresCurrentOutputPermissionAndCannotBorrowOtherTrack) {
    negotiate();
    port.audio_permitted = false;
    ASSERT_TRUE(receive(control("resume", "audio-denied", 3)));
    tick();
    EXPECT_TRUE(port.commands.empty());
    EXPECT_EQ(mqtt.last("trackState").code, 409);
    EXPECT_EQ(mqtt.last("trackState").payload, R"({"reason":"resourceBusy"})");
    ASSERT_TRUE(receive(control("resume", "video-allowed", 4, true)));
    port.complete(true, "active");
    tick();
    EXPECT_EQ(client->actual_state("broadcast-video-out"), "active");
}
TEST_F(LiveClientTest, DuplicatePendingAndCompletedPreserveOriginalBytesNoNewDriverCall) {
    negotiate();
    const auto command = control("resume", "original", 3);
    ASSERT_TRUE(receive(command));
    ASSERT_TRUE(receive(command));
    EXPECT_EQ(port.commands.size(), 1U);
    port.complete(false, "active");
    tick();
    const auto bytes = mqtt.bodies.back();
    ASSERT_TRUE(receive(command));
    EXPECT_EQ(port.commands.size(), 1U);
    EXPECT_EQ(mqtt.bodies.back(), bytes);
    EXPECT_FALSE(receive(control("pause", "original", 3)));
    EXPECT_FALSE(receive(control("resume", "original", 4)));
}
TEST_F(LiveClientTest, SameValueNewTokenIsIndependentActualOperation) {
    negotiate();
    ASSERT_TRUE(receive(control("pause", "first", 3)));
    port.complete(false, "paused");
    tick();
    ASSERT_TRUE(receive(control("pause", "second", 4)));
    EXPECT_EQ(port.commands.size(), 2U);
    EXPECT_EQ(mqtt.last("trackState").token, "first");
    port.complete(false, "paused");
    tick();
    EXPECT_EQ(mqtt.last("trackState").token, "second");
}
TEST_F(LiveClientTest, WrongTrackGenerationAndMediaRevisionDoNotConfirmCurrentOperation) {
    negotiate();
    ASSERT_TRUE(receive(control("resume", "original", 3)));
    const auto generation = port.commands.back().second;
    port.facts.push_back({generation, 1, "broadcast-video-out", "active", 200});
    port.facts.push_back({generation + 1, 1, "broadcast-out", "active", 200});
    port.facts.push_back({generation, 2, "broadcast-out", "active", 200});
    tick();
    EXPECT_TRUE(client->actual_state("broadcast-out").empty());
    EXPECT_FALSE(receive(control("resume", "wrong-m", 4, false, 2)));
    port.complete(false, "active");
    tick();
    EXPECT_EQ(client->actual_state("broadcast-out"), "active");
}
TEST_F(LiveClientTest, ActualReceiptsSurviveBackpressureAndTerminalWithoutRevival) {
    negotiate();
    mqtt.writable = false;
    const auto command = control("resume", "original", 3);
    ASSERT_TRUE(receive(command));
    port.complete(false, "active");
    tick();
    const auto bytes = mqtt.bodies.back();
    ASSERT_TRUE(receive(response("sessionEnded", "original-end",
                                 R"({"state":"ended","reason":"parentEnded"})", {}, 4)));
    mqtt.writable = true;
    tick();
    EXPECT_EQ(mqtt.bodies.back(), bytes);
    EXPECT_EQ(client->phase(), LivePhase::Ended);
    ASSERT_TRUE(receive(command));
    EXPECT_EQ(mqtt.bodies.back(), bytes);
    EXPECT_EQ(port.commands.size(), 1U);
    EXPECT_FALSE(receive(control("resume", "new-late", 5)));
    EXPECT_FALSE(receive(invitation()));
}
TEST_F(LiveClientTest, BothPendingOperationsGetOriginalUnknownOnCloseAndLateFactsIgnored) {
    negotiate();
    ASSERT_TRUE(receive(control("resume", "audio-original", 3)));
    ASSERT_TRUE(receive(control("resume", "video-original", 4, true)));
    client->close(now++);
    tick();
    EXPECT_EQ(mqtt.last("trackState").token, "video-original");
    EXPECT_EQ(mqtt.last("trackState").code, 504);
    port.complete(false, "active");
    port.complete(true, "active");
    tick();
    EXPECT_TRUE(client->actual_state("broadcast-out").empty());
    EXPECT_TRUE(client->actual_state("broadcast-video-out").empty());
}
TEST_F(LiveClientTest, SingleStopKeepsOtherTrackAndStoppedTrackCannotResume) {
    negotiate();
    ASSERT_TRUE(receive(control("stop", "audio-stop", 3)));
    port.complete(false, "stopped");
    tick();
    EXPECT_EQ(client->phase(), LivePhase::Established);
    EXPECT_EQ(port.stops, 0U);
    ASSERT_TRUE(receive(control("resume", "audio-revive", 4)));
    tick();
    EXPECT_EQ(mqtt.last("trackState").code, 409);
    EXPECT_EQ(port.commands.size(), 1U);
    ASSERT_TRUE(receive(control("stop", "video-stop", 5, true)));
    port.complete(true, "stopped");
    tick();
    EXPECT_EQ(client->phase(), LivePhase::Ended);
    EXPECT_TRUE(client->released());
}
TEST_F(LiveClientTest, SourceAuthorizationRevokedMidResumeClosesFullGroupNoLateActive) {
    negotiate();
    ASSERT_TRUE(receive(control("resume", "original", 3)));
    port.authorized = false;
    port.complete(false, "active");
    tick();
    EXPECT_EQ(client->phase(), LivePhase::Ended);
    EXPECT_TRUE(client->actual_state("broadcast-out").empty());
    EXPECT_EQ(mqtt.last("trackState").code, 504);
}
TEST_F(LiveClientTest, TimeoutClockRollbackAndRealReleaseAreSeparate) {
    negotiate();
    port.all_stopped = false;
    ASSERT_TRUE(receive(control("resume", "original", 3)));
    now += 10000;
    tick();
    EXPECT_EQ(client->phase(), LivePhase::Ended);
    EXPECT_FALSE(client->released());
    port.all_stopped = true;
    tick();
    EXPECT_TRUE(client->released());
    EXPECT_EQ(mqtt.last("trackState").code, 504);
}
TEST_F(LiveClientTest, ClockRollbackCannotClearOriginalHistoryOrResumeOutput) {
    negotiate();
    const auto original = control("resume", "original", 3);
    ASSERT_TRUE(receive(original));
    now = 1;
    tick();
    EXPECT_EQ(client->phase(), LivePhase::Ended);
    const auto count = port.commands.size();
    ASSERT_TRUE(receive(original));
    EXPECT_EQ(port.commands.size(), count);
    EXPECT_EQ(mqtt.last("trackState").code, 504);
}
TEST_F(LiveClientTest, HistoryFullRefusesNewIntentWithoutEvictingOriginalDecision) {
    negotiate();
    for (std::uint64_t index = 0; index < 32; ++index) {
        ASSERT_TRUE(receive(control("pause", "original-" + std::to_string(index), index + 3)));
        port.complete(false, "paused");
        tick();
    }
    EXPECT_FALSE(receive(control("pause", "overflow", 35)));
    EXPECT_EQ(port.commands.size(), 32U);
    ASSERT_TRUE(receive(control("pause", "original-0", 3)));
    EXPECT_EQ(port.commands.size(), 32U);
}
TEST_F(LiveClientTest, PortFailureHasReasonOnlyAndNoInventedSuccessState) {
    negotiate();
    ASSERT_TRUE(receive(control("resume", "original", 3)));
    port.complete(false, {}, 503);
    tick();
    EXPECT_EQ(mqtt.last("trackState").code, 503);
    EXPECT_EQ(mqtt.last("trackState").payload, R"({"reason":"mediaFailed"})");
    EXPECT_TRUE(client->actual_state("broadcast-out").empty());
    port.command_ok = false;
    ASSERT_TRUE(receive(control("pause", "not-accepted", 4)));
    tick();
    EXPECT_EQ(mqtt.last("trackState").code, 503);
}
TEST_F(LiveClientTest, ExplicitEndIsOriginalIntentAndPlatformAckCannotReopen) {
    negotiate();
    port.all_stopped = false;
    ASSERT_TRUE(client->end("userStopped", now++));
    const auto token = mqtt.last("sessionEnd").token;
    EXPECT_EQ(client->phase(), LivePhase::Ended);
    EXPECT_FALSE(client->released());
    EXPECT_TRUE(client->end("userStopped", now++));
    EXPECT_FALSE(client->end("parentEnded", now++));
    ASSERT_TRUE(
        receive(response("sessionEnded", token, R"({"state":"ended","reason":"userStopped"})")));
    EXPECT_EQ(client->phase(), LivePhase::Ended);
    port.all_stopped = true;
    tick();
    EXPECT_TRUE(client->released());
}
TEST_F(LiveClientTest, EndUnderBackpressureRetriesSameBytesWithoutRevivingOldControls) {
    negotiate();
    ASSERT_TRUE(receive(control("resume", "old-pending", 3)));
    mqtt.writable = false;
    ASSERT_TRUE(client->end("userStopped", now++));
    const auto original = encode(mqtt.last("sessionEnd"));
    const auto token = mqtt.last("sessionEnd").token;
    const auto count = port.commands.size();
    mqtt.writable = true;
    now += 10000;
    tick();
    EXPECT_EQ(encode(mqtt.last("sessionEnd")), original);
    EXPECT_EQ(port.commands.size(), count);
    EXPECT_EQ(client->phase(), LivePhase::Ended);
    ASSERT_TRUE(
        receive(response("sessionEnded", token, R"({"state":"ended","reason":"userStopped"})")));
    const auto published = mqtt.bodies.size();
    now += 20000;
    tick();
    EXPECT_EQ(mqtt.bodies.size(), published);
}
TEST_F(LiveClientTest, AudioOnlyGroupHasNoVideoPermissionOrHiddenCapture) {
    client = std::make_unique<LiveClient>(mqtt, port, binding(true, false), "up", "down", 100000);
    negotiate(true, false);
    EXPECT_EQ(mqtt.last("sessionAccept").payload, R"({"acceptedTracks":["broadcast-out"]})");
    EXPECT_FALSE(receive(control("resume", "foreign-video", 3, true)));
    ASSERT_TRUE(receive(control("resume", "audio", 3)));
    port.complete(false, "active");
    tick();
    EXPECT_FALSE(client->input_open());
    EXPECT_EQ(client->actual_state("broadcast-out"), "active");
}
TEST_F(LiveClientTest, VideoOnlyGroupCompletesAndReleasesWithoutWaitingForAudio) {
    client = std::make_unique<LiveClient>(mqtt, port, binding(false, true), "up", "down", 100000);
    negotiate(false, true);
    EXPECT_EQ(mqtt.last("sessionAccept").payload, R"({"acceptedTracks":["broadcast-video-out"]})");
    ASSERT_TRUE(receive(control("resume", "video", 3, true)));
    port.complete(true, "active");
    tick();
    EXPECT_EQ(client->actual_state("broadcast-video-out"), "active");
    ASSERT_TRUE(receive(control("stop", "video-stop", 4, true)));
    port.complete(true, "stopped");
    tick();
    EXPECT_EQ(client->phase(), LivePhase::Ended);
    EXPECT_TRUE(client->released());
}
TEST_F(LiveClientTest, PlatformTerminalCancelsPendingEndWithoutRetryingClosedIntent) {
    negotiate();
    mqtt.writable = false;
    ASSERT_TRUE(client->end("userStopped", now++));
    ASSERT_TRUE(receive(response("sessionEnded", "platform-terminal",
                                 R"({"state":"ended","reason":"parentEnded"})", {}, 3)));
    mqtt.writable = true;
    const auto attempts = mqtt.bodies.size();
    now += 20000;
    tick();
    EXPECT_EQ(mqtt.bodies.size(), attempts);
}
TEST_F(LiveClientTest, TemporarySpeakerFocusSuspendDoesNotDestroyProgrammeOrVideo) {
    negotiate();
    ASSERT_TRUE(receive(control("resume", "audio", 3)));
    port.complete(false, "active");
    tick();
    ASSERT_TRUE(receive(control("resume", "video", 4, true)));
    port.complete(true, "active");
    tick();
    port.audio_permitted = false;  // 实际末端临时mask关闭，不等同节目源授权撤销。
    tick();
    EXPECT_EQ(client->phase(), LivePhase::Established);
    EXPECT_EQ(port.stops, 0U);
    EXPECT_EQ(client->actual_state("broadcast-video-out"), "active");
    EXPECT_FALSE(client->input_open());
}
TEST_F(LiveClientTest, FocusLossWaitsForOriginalDriverCancellationWithoutInventedPaused) {
    negotiate();
    ASSERT_TRUE(receive(control("resume", "original", 3)));
    const auto original_generation = port.commands.back().second;
    port.audio_permitted = false;
    tick();
    EXPECT_EQ(client->phase(), LivePhase::Established);
    EXPECT_TRUE(mqtt.last("trackState").method.empty());
    port.complete(false, {}, 409, LiveMediaFailure::OutputBusy);  // 实际原输出关闭后交取消失败。
    tick();
    EXPECT_EQ(client->phase(), LivePhase::Established);
    EXPECT_EQ(mqtt.last("trackState").code, 409);
    EXPECT_EQ(mqtt.last("trackState").payload, R"({"reason":"resourceBusy"})");
    EXPECT_TRUE(client->actual_state("broadcast-out").empty());
    port.audio_permitted = true;
    port.facts.push_back({original_generation, 1, "broadcast-out", "active", 200});
    tick();
    EXPECT_TRUE(client->actual_state("broadcast-out").empty());
    EXPECT_EQ(port.commands.size(), 1U);
}
TEST_F(LiveClientTest, FocusRestoreCannotCreateNewTrackResumeOrRestorePlaybackIntent) {
    negotiate();
    ASSERT_TRUE(receive(control("resume", "original", 3)));
    port.audio_permitted = false;
    port.complete(false, {}, 409, LiveMediaFailure::OutputBusy);
    tick();
    const auto count = port.commands.size();
    port.audio_permitted = true;
    tick();
    EXPECT_EQ(client->phase(), LivePhase::Established);
    EXPECT_EQ(port.commands.size(), count);
    EXPECT_TRUE(client->actual_state("broadcast-out").empty());
    ASSERT_TRUE(receive(control("resume", "explicit-new-intent", 4)));
    EXPECT_EQ(port.commands.size(), count + 1);
}
TEST_F(LiveClientTest, ProgrammeAuthorizationLostBeforeAdmissionAckCannotBeginPeer) {
    ASSERT_TRUE(receive(invitation()));
    port.authorized = false;
    EXPECT_FALSE(
        receive(response("requestAck", "live-original",
                         R"({"requestMethod":"sessionAccept","result":{"state":"negotiating"}})")));
    EXPECT_EQ(client->phase(), LivePhase::Ended);
    EXPECT_EQ(port.peers, 0U);
    port.authorized = true;
    EXPECT_FALSE(receive(invitation()));
    EXPECT_EQ(port.prepares, 1U);
}
TEST_F(LiveClientTest, InvalidFailureClassificationCannotForgeOutputBusyOrCompleteOperation) {
    negotiate();
    ASSERT_TRUE(receive(control("resume", "original", 3)));
    port.complete(false, {}, 503, LiveMediaFailure::OutputBusy);
    port.complete(false, {}, 409, static_cast<LiveMediaFailure>(999));
    tick();
    EXPECT_TRUE(mqtt.last("trackState").method.empty());
    EXPECT_TRUE(client->actual_state("broadcast-out").empty());
    port.complete(false, {}, 409, LiveMediaFailure::OutputBusy);
    tick();
    EXPECT_EQ(mqtt.last("trackState").code, 409);
    EXPECT_EQ(mqtt.last("trackState").payload, R"({"reason":"resourceBusy"})");
    EXPECT_EQ(client->phase(), LivePhase::Established);
}
TEST_F(LiveClientTest, LegallyFormedQueuedActiveSurvivesLaterTemporaryFocusLossAsHistoryOnly) {
    negotiate();
    ASSERT_TRUE(receive(control("resume", "original", 3)));
    port.complete(false, "active");  // 实际原授权/mask有效时已合法形成，尚未交Core收录。
    port.audio_permitted = false;    // 实际关闭末端后才交此前原完成事实，不重新打开输出。
    tick();
    EXPECT_EQ(client->phase(), LivePhase::Established);
    EXPECT_EQ(client->actual_state("broadcast-out"), "active");
    EXPECT_EQ(mqtt.last("trackState").token, "original");
    EXPECT_EQ(mqtt.last("trackState").code, 200);
    EXPECT_EQ(port.commands.size(), 1U);
    port.audio_permitted = true;
    tick();
    EXPECT_EQ(port.commands.size(), 1U);  // 历史actual不得成为恢复focus后的新播放意图。
}
TEST_F(LiveClientTest, ProgrammeAuthorizationRevokeStillBlocksQueuedActiveAndNewPlayback) {
    negotiate();
    ASSERT_TRUE(receive(control("resume", "original", 3)));
    port.complete(false, "active");
    port.authorized = false;
    tick();
    EXPECT_EQ(client->phase(), LivePhase::Ended);
    EXPECT_TRUE(client->actual_state("broadcast-out").empty());
    EXPECT_EQ(mqtt.last("trackState").code, 504);
    port.authorized = true;
    port.audio_permitted = true;
    EXPECT_FALSE(receive(control("resume", "new-after-revocation", 4)));
}
TEST_F(LiveClientTest, LateCanceledGenerationCannotBecomeHistorySuccessAfterFocusRestore) {
    negotiate();
    ASSERT_TRUE(receive(control("resume", "original", 3)));
    const auto canceled = port.commands.back().second;
    port.audio_permitted = false;
    port.complete(false, {}, 409, LiveMediaFailure::OutputBusy);
    tick();
    port.audio_permitted = true;
    port.facts.push_back({canceled, 1, "broadcast-out", "active", 200});
    tick();
    EXPECT_TRUE(client->actual_state("broadcast-out").empty());
    EXPECT_EQ(mqtt.last("trackState").code, 409);
    EXPECT_EQ(client->phase(), LivePhase::Established);
    EXPECT_EQ(port.commands.size(), 1U);
}
}  // namespace ur::device
