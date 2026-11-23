// 文件说明：历史会话可控端口回归；驱动事实由用例注入，不把模拟连通当真实媒体验收。
#include "ur/device/record_playback_client.hpp"
#include <gtest/gtest.h>
#include <deque>
#include <set>

namespace ur::device {
// RecordPlaybackStaticFaultTest 仅调用实际私有事实出口，保留空在途关联供失败关闭回归。
class RecordPlaybackStaticFaultTest {
public:
    // emit_without_operation 返回实际出口结果，不补造平台请求或权限。
    static bool emit_without_operation(RecordPlaybackClient& client) {
        RecordPlaybackFact fact;
        fact.state = "loaded";
        return client.emit_fact(fact);
    }
};
namespace {
// RecordMqtt 记录本地发布接纳和原字节；false只制造背压，不代表远端拒绝或执行。
struct RecordMqtt final : PublishPort {
    bool writable = true;             // 用例可控发布背压。
    std::vector<std::string> bodies;  // 全部实际尝试原字节。
    std::vector<Envelope> sent;       // 已发布消息的协议投影，不含外部凭据。
    bool publish(std::string_view, std::string_view body) override {
        Envelope message;
        if (decode_envelope(body, message) != DecodeResult::Ok)
            return false;
        bodies.emplace_back(body);
        sent.push_back(std::move(message));
        return writable;
    }
    // last 返回最后一次指定方法，不从媒体状态猜原token。
    Envelope last(std::string_view method) const {
        for (auto it = sent.rbegin(); it != sent.rend(); ++it)
            if (it->method == method)
                return *it;
        return {};
    }
};
// RecordPort 模拟原档案所有者；Prepared/Begin/Answer不会自动生成实际事实。
struct RecordPort final : RecordPlaybackPort {
    RecordPreparation preparation = RecordPreparation::Ready;  // 独立可信清单预检结果。
    bool begin_ok = true, peer_ok = true, restore_ok = true;   // 明确无副作用受理失败。
    bool permitted = true, all_stopped = true;                 // 原发布许可和全组实际退出分开。
    unsigned prepares = 0, stops = 0, restores = 0;            // 可观察原副作用次数。
    std::uint64_t generation = 0, next_revision = 0;           // 最近受理操作的两个独立关联。
    std::string restored_state;                                // 驱动恢复指令，不作为实际状态。
    std::vector<PlaybackCommand> controls;             // 真实受理原命令，不从seek目标造回执。
    std::vector<std::uint64_t> peers, closing;         // 实际新建／关闭代次。
    std::set<std::uint64_t> stopped, connected_peers;  // 用例明确交付Stopped/ICE事实。
    std::deque<RecordPlaybackFact> facts;              // 有限输入事实由用例注入。
    RecordPreparation prepare(const RecordPlaybackInvite&, std::uint64_t) override {
        ++prepares;
        return preparation;
    }
    bool begin(const PlaybackCommand& command, std::uint64_t value, std::uint64_t next) override {
        if (!begin_ok)
            return false;
        controls.push_back(command);
        generation = value;
        next_revision = next;
        return true;
    }
    std::optional<RecordPlaybackFact> take_fact() override {
        if (facts.empty())
            return {};
        auto fact = std::move(facts.front());
        facts.pop_front();
        return fact;
    }
    bool begin_peer(std::uint64_t revision) override {
        if (!peer_ok)
            return false;
        peers.push_back(revision);
        return true;
    }
    bool apply_answer(std::uint64_t, std::string_view) override { return true; }
    bool connected(std::uint64_t revision) const override {
        return connected_peers.count(revision) != 0;
    }
    void close_peer(std::uint64_t revision) override { closing.push_back(revision); }
    bool peer_stopped(std::uint64_t revision) const override {
        return stopped.count(revision) != 0;
    }
    bool restore_intent(std::uint64_t, std::string_view state, std::uint64_t) override {
        ++restores;
        restored_state = state;
        return restore_ok;
    }
    bool publishing_permitted(std::uint64_t) const override { return permitted; }
    bool stop_all() override {
        ++stops;
        return all_stopped;
    }
};
// encode 构造可独立解析的线上消息，失败使当前用例直接失败。
std::string encode(Envelope message) {
    std::string body;
    EXPECT_EQ(encode_envelope(message, body), DecodeResult::Ok);
    return body;
}
// invitation 固定全份30秒档案，属于front但不授实时camera/mic权限。
std::string invitation() {
    Envelope message;
    message.method = "sessionInvite";
    message.token = "record-invite";
    message.session = "record-session";
    message.event_sequence = 1;
    message.payload =
        R"({"purpose":"recordPlayback","admission":"direct","transport":"webrtc","expiresAt":1030000,"cameraId":"front","recordingId":"archive","recordingVersion":"v1","playbackId":"record-player","mediaSource":{"contentRef":"record:archive-v1"},"clip":{"startMs":0,"endMs":30000},"tracks":[{"trackId":"video-1","kind":"video","sourceId":"record-video","direction":"sendonly","codec":"H264","state":"requested"},{"trackId":"audio-1","kind":"audio","sourceId":"record-audio","direction":"sendonly","codec":"opus","state":"requested"}]})";
    return encode(std::move(message));
}
// binding 从真实历史wire解码得原不可变绑定，不手造能力或绕过字段验证。
RecordPlaybackInvite binding() {
    RecordPlaybackInvite value;
    EXPECT_EQ(decode_record_playback_invite(WireRoute::Signaling, invitation(), value),
              WireResult::Ok);
    return value;
}
// response 只构造指定原请求的data，不能隐含驱动实际动作。
std::string response(std::string method, std::string token, std::string payload,
                     std::optional<int> code = 200, std::optional<std::uint64_t> sequence = {}) {
    Envelope message;
    message.request = false;
    message.method = std::move(method);
    message.token = std::move(token);
    message.session = "record-session";
    message.code = code;
    message.event_sequence = sequence;
    message.payload = std::move(payload);
    return encode(std::move(message));
}
// control 为每次独立用户意图保留固定token和eventSeq；原m不能被设备自主改成新m。
std::string control(std::string operation, std::string token, std::uint64_t revision,
                    std::uint64_t sequence, std::uint64_t media = 1,
                    std::uint64_t position = 10000) {
    Envelope message;
    message.method = "playbackControl";
    message.session = "record-session";
    message.token = std::move(token);
    message.event_sequence = sequence;
    message.payload =
        "{\"operation\":\"" + operation +
        "\",\"playbackId\":\"record-player\",\"expectedRevision\":" + std::to_string(revision) +
        ",\"mediaRevision\":" + std::to_string(media);
    if (operation == "load")
        message.payload += R"(,"contentRef":"record:archive-v1")";
    if (operation == "seek")
        message.payload += ",\"positionMs\":" + std::to_string(position);
    message.payload += "}";
    return encode(std::move(message));
}
// RecordPlaybackClientTest 只启动一个固定会话；具体驱动事实由每个场景独立指定。
class RecordPlaybackClientTest : public ::testing::Test {
protected:
    RecordMqtt mqtt;  // 同一设备宿主网络端口。
    RecordPort port;  // 可控档案/Peer所有者，不含实时采集。
    RecordPlaybackClient client{mqtt, port, binding(), "up/signaling", "down/signaling", 100000};
    std::uint64_t now = 100;  // 用例单调时间，推进不调用真实sleep。
    bool deliver(std::string body) {
        return client.receive("down/signaling", body, ++now, 1000000);
    }
    void tick() { client.tick(++now); }
    // accept 仅平台接纳原邀请，不能建立Peer或生成loaded。
    void accept() {
        ASSERT_TRUE(deliver(invitation()));
        ASSERT_TRUE(deliver(
            response("requestAck", "record-invite",
                     R"({"requestMethod":"sessionAccept","result":{"state":"negotiating"}})")));
    }
    // fact 注入真实驱动回调的替身；不把此替身当软件E2E证据。
    void fact(std::string state, std::uint64_t media = 1, bool spontaneous = false,
              std::uint64_t position = 0) {
        RecordPlaybackFact value;
        value.generation = port.generation;
        value.media_revision = media;
        value.state = std::move(state);
        value.position_ms = position;
        value.duration_ms = 30000;
        value.spontaneous = spontaneous;
        port.facts.push_back(std::move(value));
        tick();
    }
    // load 直到实际loaded事实到达才允许建源Peer。
    void load() {
        accept();
        ASSERT_TRUE(deliver(control("load", "load-1", 0, 2)));
        tick();
        ASSERT_EQ(port.controls.size(), 1U);
        fact("loaded");
        ASSERT_EQ(client.state(), "loaded");
        ASSERT_EQ(port.peers, std::vector<std::uint64_t>{1});
    }
    // establish 明确分开完整SDP、真实Connected、原受理ACK及平台事件。
    void establish(std::uint64_t media = 1, std::uint64_t sequence = 3) {
        ASSERT_TRUE(client.offer_ready(media, "v=0\r\ns=test\r\n", ++now));
        const auto offer = mqtt.last("mediaOffer");
        std::string expected;
        ASSERT_EQ(canonical_json_object(
                      "{\"mediaRevision\":" + std::to_string(media) +
                          R"(,"webrtc":{"iceMode":"complete","sdp":"v=0\r\ns=test\r\n"}})",
                      expected),
                  DecodeResult::Ok);
        EXPECT_EQ(offer.payload, expected);
        ASSERT_TRUE(deliver(
            response("mediaAnswer", offer.token,
                     "{\"mediaRevision\":" + std::to_string(media) +
                         R"(,"webrtc":{"iceMode":"complete","sdp":"v=0\r\ns=answer\r\n"}})")));
        port.connected_peers.insert(media);
        ASSERT_TRUE(client.media_connected(media, ++now));
        const auto report = mqtt.last("mediaState");
        ASSERT_TRUE(deliver(response("requestAck", report.token,
                                     "{\"requestMethod\":\"mediaState\",\"result\":{\"state\":"
                                     "\"connected\",\"mediaRevision\":" +
                                         std::to_string(media) + "}}")));
        ASSERT_TRUE(deliver(
            response("sessionEstablished", "established-" + std::to_string(media),
                     "{\"state\":\"established\",\"mediaRevision\":" + std::to_string(media) + "}",
                     {}, sequence)));
    }
    // play 需要原建立和实际playing事实，不能仅由ACK改变播放器Actual。
    void play() {
        load();
        establish();
        ASSERT_TRUE(deliver(control("play", "play-1", 1, 4)));
        tick();
        fact("playing");
        ASSERT_EQ(client.state(), "playing");
    }
};

TEST_F(RecordPlaybackClientTest, LoadFactAndBothMediaSignalsAreRequiredBeforePlay) {
    accept();
    EXPECT_EQ(client.phase(), RecordPlaybackPhase::Ready);
    EXPECT_TRUE(port.peers.empty());
    EXPECT_FALSE(deliver(control("play", "early-play", 0, 2)));
    ASSERT_TRUE(deliver(control("load", "load-1", 0, 2)));
    tick();
    EXPECT_TRUE(client.state().empty());
    EXPECT_TRUE(port.peers.empty());
    fact("loaded");
    EXPECT_FALSE(deliver(control("play", "early-play-2", 1, 3)));
    establish();
    EXPECT_EQ(client.phase(), RecordPlaybackPhase::Active);
    EXPECT_EQ(client.state(), "loaded");
    ASSERT_TRUE(deliver(control("play", "play-1", 1, 4)));
    tick();
    EXPECT_EQ(client.state(), "loaded");
    fact("playing");
    EXPECT_EQ(client.revision(), 2U);
}
TEST_F(RecordPlaybackClientTest, SameOriginalLoadReplaysWithoutDriverExecution) {
    load();
    const auto reply = mqtt.bodies.back();
    EXPECT_TRUE(deliver(control("load", "load-1", 0, 2)));
    EXPECT_EQ(mqtt.bodies.back(), reply);
    EXPECT_EQ(port.controls.size(), 1U);
    EXPECT_FALSE(deliver(control("load", "load-1", 0, 9)));
    EXPECT_EQ(port.controls.size(), 1U);
}
TEST_F(RecordPlaybackClientTest, FixedReferenceAndVersionCannotBeChanged) {
    accept();
    Envelope command;
    ASSERT_EQ(decode_envelope(control("load", "bad-load", 0, 2), command), DecodeResult::Ok);
    command.payload =
        R"({"operation":"load","playbackId":"record-player","expectedRevision":0,"mediaRevision":1,"contentRef":"record:other"})";
    EXPECT_FALSE(deliver(encode(command)));
    Envelope invite;
    ASSERT_EQ(decode_envelope(invitation(), invite), DecodeResult::Ok);
    const auto at = invite.payload.find("v1");
    ASSERT_NE(at, std::string::npos);
    invite.payload.replace(at, 2, "v2");
    EXPECT_FALSE(deliver(encode(invite)));
    EXPECT_TRUE(port.controls.empty());
}
TEST_F(RecordPlaybackClientTest, SeekWaitsForOldActualStoppedBeforeNextPeer) {
    play();
    ASSERT_TRUE(deliver(control("seek", "seek-1", 2, 5)));
    EXPECT_EQ(client.media_revision(), 2U);
    EXPECT_EQ(port.closing, std::vector<std::uint64_t>{1});
    tick();
    EXPECT_EQ(*port.controls.back().media_revision, 1U);
    EXPECT_EQ(port.next_revision, 2U);
    fact("seeking", 2, false, 4200);
    EXPECT_EQ(client.revision(), 2U);
    EXPECT_EQ(port.peers.size(), 1U);
    EXPECT_FALSE(client.offer_ready(2, "v=0\r\ns=new\r\n", ++now));
    EXPECT_TRUE(deliver(control("seek", "seek-1", 2, 5)));
    EXPECT_EQ(client.media_revision(), 2U);
    port.stopped.insert(1);
    tick();
    EXPECT_EQ(client.state(), "seeking");
    EXPECT_EQ(client.revision(), 3U);
    EXPECT_EQ(port.peers, (std::vector<std::uint64_t>{1, 2}));
    establish(2, 6);
    EXPECT_EQ(port.restored_state, "playing");
    EXPECT_EQ(client.state(), "seeking");
    fact("playing", 2, true, 9800);
    EXPECT_EQ(client.phase(), RecordPlaybackPhase::Active);
    EXPECT_EQ(client.revision(), 4U);
}
TEST_F(RecordPlaybackClientTest, PausedSeekNeverAutomaticallyPlaysAfterNewPeer) {
    play();
    ASSERT_TRUE(deliver(control("pause", "pause-1", 2, 5)));
    tick();
    fact("paused", 1, false, 4200);
    ASSERT_TRUE(deliver(control("seek", "seek-paused", 3, 6)));
    tick();
    port.stopped.insert(1);
    fact("seeking", 2, false, 4200);
    establish(2, 7);
    EXPECT_EQ(port.restored_state, "paused");
    EXPECT_EQ(port.restores, 1U);
    EXPECT_EQ(client.state(), "seeking");
    port.permitted = false;
    fact("paused", 2, true, 9800);
    EXPECT_EQ(client.state(), "paused");
    EXPECT_EQ(client.phase(), RecordPlaybackPhase::Active);
    EXPECT_FALSE(deliver(control("play", "denied-play", 5, 8, 2)));
}
TEST_F(RecordPlaybackClientTest, OldGenerationMediaAndAnswerDoNotChangeNewActual) {
    play();
    const auto old_offer = mqtt.last("mediaOffer");
    const auto old_generation = port.generation;
    ASSERT_TRUE(deliver(control("seek", "seek-1", 2, 5)));
    tick();
    RecordPlaybackFact late;
    late.generation = old_generation;
    late.media_revision = 1;
    late.state = "ended";
    late.spontaneous = true;
    port.facts.push_back(late);
    tick();
    EXPECT_EQ(client.phase(), RecordPlaybackPhase::Seeking);
    EXPECT_EQ(client.state(), "playing");
    EXPECT_FALSE(deliver(response(
        "mediaAnswer", old_offer.token,
        R"({"mediaRevision":1,"webrtc":{"iceMode":"complete","sdp":"v=0\r\ns=old\r\n"}})")));
    EXPECT_FALSE(client.media_connected(1, ++now));
    EXPECT_FALSE(deliver(control("pause", "old-control", 2, 6, 1)));
}
TEST_F(RecordPlaybackClientTest, PlatformEndDuringSeekRequiresAllGenerationsStopped) {
    play();
    port.all_stopped = false;
    ASSERT_TRUE(deliver(control("seek", "seek-1", 2, 5)));
    tick();
    ASSERT_TRUE(deliver(response("sessionEnded", "original-end",
                                 R"({"state":"ended","reason":"userStopped"})", {}, 6)));
    EXPECT_EQ(client.phase(), RecordPlaybackPhase::Ended);
    EXPECT_FALSE(client.released());
    EXPECT_FALSE(client.release_submitted());
    port.stopped.insert(1);
    fact("seeking", 2);
    EXPECT_EQ(port.peers.size(), 1U);
    port.all_stopped = true;
    tick();
    EXPECT_TRUE(client.release_submitted());
    const auto release = mqtt.last("playbackState");
    EXPECT_EQ(release.token, "original-end");
    EXPECT_FALSE(release.code);
    RecordPlaybackState decoded;
    ASSERT_EQ(decode_record_playback_state(WireRoute::Signaling, mqtt.bodies.back(), decoded),
              WireResult::Ok);
    EXPECT_EQ(decoded.kind, RecordPlaybackStateKind::Released);
    EXPECT_EQ(decoded.media_revision, 0U);
}
TEST_F(RecordPlaybackClientTest, BackpressureKeepsOriginalFactBeforeOriginalRelease) {
    accept();
    ASSERT_TRUE(deliver(control("load", "load-1", 0, 2)));
    tick();
    mqtt.writable = false;
    fact("loaded");
    const auto original = mqtt.bodies.back();
    ASSERT_TRUE(deliver(response("sessionEnded", "original-end",
                                 R"({"state":"ended","reason":"userStopped"})", {}, 3)));
    EXPECT_FALSE(client.release_submitted());
    mqtt.writable = true;
    const auto before = mqtt.bodies.size();
    tick();
    ASSERT_EQ(mqtt.bodies.size(), before + 2);
    EXPECT_EQ(mqtt.bodies[before], original);
    EXPECT_TRUE(client.release_submitted());
}
TEST_F(RecordPlaybackClientTest, WrongTopicInvalidJsonAndUnacceptedLoadHaveNoSideEffects) {
    EXPECT_FALSE(client.receive("wrong", invitation(), ++now, 1000000));
    EXPECT_FALSE(deliver("{"));
    EXPECT_FALSE(deliver(control("load", "unaccepted", 0, 2)));
    EXPECT_EQ(port.prepares, 0U);
    EXPECT_TRUE(port.controls.empty());
    EXPECT_TRUE(port.peers.empty());
}
TEST_F(RecordPlaybackClientTest, ClockRollbackAndOperationTimeoutCloseWithoutSyntheticSuccess) {
    accept();
    ASSERT_TRUE(deliver(control("load", "load-1", 0, 2)));
    tick();
    client.tick(now + 10000);
    EXPECT_EQ(client.phase(), RecordPlaybackPhase::Ended);
    EXPECT_TRUE(client.state().empty());
    EXPECT_EQ(mqtt.last("playbackState").code, 504);
    EXPECT_TRUE(deliver(control("load", "load-1", 0, 2)));
    EXPECT_EQ(port.controls.size(), 1U);
    EXPECT_FALSE(deliver(control("load", "load-new", 0, 3)));
}
TEST_F(RecordPlaybackClientTest, RollbackImmediatelyClosesCurrentPlayingSource) {
    play();
    client.tick(now - 1);
    EXPECT_EQ(client.phase(), RecordPlaybackPhase::Ended);
    EXPECT_GT(port.stops, 0U);
}
TEST_F(RecordPlaybackClientTest, ActualFailureAckDoesNotInventSuccessfulPosition) {
    accept();
    ASSERT_TRUE(deliver(control("load", "load-1", 0, 2)));
    tick();
    RecordPlaybackFact failure;
    failure.generation = port.generation;
    failure.media_revision = 1;
    failure.code = 503;
    port.facts.push_back(failure);
    tick();
    EXPECT_TRUE(client.state().empty());
    EXPECT_EQ(client.phase(), RecordPlaybackPhase::Ended);
    const auto reply = mqtt.last("playbackState");
    EXPECT_EQ(reply.code, 503);
    EXPECT_EQ(reply.payload, R"({"reason":"deviceRejected"})");
}
TEST_F(RecordPlaybackClientTest, NaturalEndAndLateInviteNeverRestoreResources) {
    play();
    fact("ended", 1, true, 30000);
    EXPECT_EQ(client.state(), "ended");
    EXPECT_EQ(client.phase(), RecordPlaybackPhase::Ended);
    EXPECT_FALSE(deliver(invitation()));
    fact("playing", 1, true);
    EXPECT_EQ(client.state(), "ended");
    EXPECT_EQ(port.peers.size(), 1U);
}
TEST_F(RecordPlaybackClientTest, UnsupportedWholeClipDriverRejectsWithoutOpeningSource) {
    port.preparation = RecordPreparation::Unsupported;
    ASSERT_TRUE(deliver(invitation()));
    EXPECT_EQ(client.phase(), RecordPlaybackPhase::Rejecting);
    EXPECT_EQ(mqtt.last("sessionReject").token, "record-invite");
    EXPECT_EQ(mqtt.last("sessionReject").payload, R"({"reason":"unsupportedCapability"})");
    EXPECT_TRUE(port.controls.empty());
    EXPECT_TRUE(port.peers.empty());
    EXPECT_FALSE(deliver(control("load", "load-1", 0, 2)));
    EXPECT_FALSE(deliver(response(
        "requestAck", "record-invite",
        R"({"requestMethod":"sessionReject","result":{"state":"ended","reason":"permissionDenied"}})")));
    EXPECT_TRUE(deliver(response(
        "requestAck", "record-invite",
        R"({"requestMethod":"sessionReject","result":{"state":"ended","reason":"unsupportedCapability"}})")));
    EXPECT_EQ(client.phase(), RecordPlaybackPhase::Ended);
}
TEST_F(RecordPlaybackClientTest, StopCanCancelPendingSeekWithoutReopeningOldPeer) {
    play();
    ASSERT_TRUE(deliver(control("seek", "seek-1", 2, 5)));
    tick();
    fact("seeking", 2, false, 4200);
    ASSERT_TRUE(deliver(control("stop", "stop-seek", 2, 6, 2)));
    tick();
    EXPECT_EQ(port.controls.back().operation, "stop");
    fact("stopped", 2, false, 4200);
    EXPECT_EQ(client.phase(), RecordPlaybackPhase::Ended);
    EXPECT_EQ(client.state(), "stopped");
    EXPECT_EQ(port.peers, std::vector<std::uint64_t>{1});
    EXPECT_EQ(port.restores, 0U);
    const auto count = port.controls.size();
    EXPECT_TRUE(deliver(control("seek", "seek-1", 2, 5)));
    EXPECT_EQ(mqtt.last("playbackState").code, 504);
    EXPECT_EQ(port.controls.size(), count);
}
TEST_F(RecordPlaybackClientTest, ActualPeerLossClosesEvenIfDriverPermissionStaysTrue) {
    play();
    port.connected_peers.clear();
    tick();
    EXPECT_EQ(client.phase(), RecordPlaybackPhase::Ended);
    EXPECT_GT(port.stops, 0U);
}
TEST_F(RecordPlaybackClientTest, PlatformMediaRejectClosesWithoutReportingConnected) {
    load();
    ASSERT_TRUE(client.offer_ready(1, "v=0\r\ns=test\r\n", ++now));
    const auto offer = mqtt.last("mediaOffer");
    EXPECT_TRUE(deliver(response("mediaAnswer", offer.token, R"({"reason":"mediaFailed"})", 503)));
    EXPECT_EQ(client.phase(), RecordPlaybackPhase::Ended);
    EXPECT_FALSE(client.media_connected(1, ++now));
    EXPECT_GT(port.stops, 0U);
}
TEST_F(RecordPlaybackClientTest, OriginalInviteExpiryDoesNotRefreshOnDuplicate) {
    ASSERT_TRUE(client.receive("down/signaling", invitation(), 100, 1029990));
    EXPECT_TRUE(client.receive("down/signaling", invitation(), 105, 1029990));
    client.tick(110);
    EXPECT_EQ(client.phase(), RecordPlaybackPhase::Ended);
    EXPECT_FALSE(client.receive("down/signaling", invitation(), 111, 1000000));
    EXPECT_EQ(port.prepares, 1U);
    EXPECT_TRUE(port.peers.empty());
}
TEST_F(RecordPlaybackClientTest, DriverWholeDurationMismatchNeverBuildsPeer) {
    accept();
    ASSERT_TRUE(deliver(control("load", "load-1", 0, 2)));
    tick();
    RecordPlaybackFact partial;
    partial.generation = port.generation;
    partial.media_revision = 1;
    partial.state = "loaded";
    partial.duration_ms = 20000;
    port.facts.push_back(partial);
    tick();
    EXPECT_EQ(client.phase(), RecordPlaybackPhase::Ended);
    EXPECT_TRUE(port.peers.empty());
    EXPECT_TRUE(client.state().empty());
}
TEST_F(RecordPlaybackClientTest, DuplicateConnectedAndInviteKeepOriginalTokensAndActions) {
    load();
    establish();
    const auto messages = mqtt.bodies.size();
    const auto original = mqtt.last("mediaState").token;
    EXPECT_TRUE(client.media_connected(1, ++now));
    EXPECT_TRUE(deliver(invitation()));
    EXPECT_EQ(mqtt.bodies.size(), messages);
    EXPECT_EQ(mqtt.last("mediaState").token, original);
    EXPECT_EQ(port.prepares, 1U);
    EXPECT_EQ(port.peers.size(), 1U);
}
TEST_F(RecordPlaybackClientTest, TrustedCatalogDenialIsNotReportedAsPrivacyMode) {
    port.preparation = RecordPreparation::Denied;
    ASSERT_TRUE(deliver(invitation()));
    EXPECT_EQ(mqtt.last("sessionReject").payload, R"({"reason":"permissionDenied"})");
    EXPECT_TRUE(deliver(response(
        "requestAck", "record-invite",
        R"({"requestMethod":"sessionReject","result":{"state":"ended","reason":"permissionDenied"}})")));
    EXPECT_EQ(client.phase(), RecordPlaybackPhase::Ended);
    EXPECT_TRUE(port.controls.empty());
    EXPECT_TRUE(port.peers.empty());
}
// 空在途关联直接进入实际私有出口，不得发布回执、推进修订或调用档案/Peer端口。
TEST(RecordPlaybackStaticFault, MissingActiveCannotEmitOrAdvanceRevision) {
    RecordMqtt mqtt;
    RecordPort port;
    RecordPlaybackClient client(mqtt, port, binding(), "up/signaling", "down/signaling", 100000);
    EXPECT_FALSE(RecordPlaybackStaticFaultTest::emit_without_operation(client));
    EXPECT_EQ(client.revision(), std::uint64_t{0});
    EXPECT_TRUE(mqtt.sent.empty());
    EXPECT_TRUE(port.controls.empty());
    EXPECT_TRUE(port.peers.empty());
    EXPECT_EQ(port.prepares, 0U);
    EXPECT_EQ(port.stops, 0U);
}
}  // namespace
}  // namespace ur::device
