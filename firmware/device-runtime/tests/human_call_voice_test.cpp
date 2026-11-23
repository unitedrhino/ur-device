// 文件说明：纯语音复用人际通话状态机的离线回归；候选Peer事实不代表真实焦点移交或AI转人工已完成。
#include <cJSON.h>
#include <gtest/gtest.h>
#include <algorithm>
#include <deque>
#include <memory>
#include "ur/device/focus_engine.hpp"
#include "ur/device/human_call.hpp"

namespace ur::device {
// HumanEndpointSourceStaticFaultTest 仅观察/注入消息预算，不创建或改变媒体权限。
class HumanEndpointSourceStaticFaultTest {
   public:
    // fill_bytes 只模拟原主动事件账本字节耗尽，允许恢复预算后验证未推进另一份账本。
    static void fill_bytes(HumanCallClient& client, bool full) {
        client.shared_endpoint_bytes_ = full ? 65536 : 0;
    }
    // sequence 只读整腿水位，与endpoint事务revision区分。
    static std::uint64_t sequence(const HumanCallClient& client) { return client.event_seq_; }
    // expire_request 只推进原请求表GC，检查失败原scope已退休，不替代模型或资源实际回执。
    static RequestState expire_request(HumanCallClient& client, std::string_view token,
                                       std::uint64_t now) {
        client.requests_.tick(now);
        return client.requests_.state({client.up_, "mediaState", token});
    }
};
namespace {
// VoiceMqtt 记录实际编码的上行和受控网络事件；不连接Broker或保存凭据。
struct VoiceMqtt final : MqttPort {
    std::vector<Envelope> sent;         // 仅本测试的规范信封，不当作生产交付事实。
    std::deque<MqttEvent> events;       // 原连接及订阅ACK由测试显式交付。
    int connects = 0, disconnects = 0;  // 借用模式不能碰原宿主连接。
    bool publish_ok = true;             // 只模拟提交拒绝，不伪造远端ACK。
    bool connect(const MqttConfig&) override {
        ++connects;
        return true;
    }
    int subscribe(std::string_view) override { return 7; }
    bool publish(std::string_view topic, std::string_view bytes) override {
        if (!publish_ok || topic != "$thing/up/signaling/p/d")
            return false;
        Envelope value;
        if (decode_envelope(bytes, value) != DecodeResult::Ok)
            return false;
        sent.push_back(std::move(value));
        return true;
    }
    bool poll() override { return true; }
    std::optional<MqttEvent> take_event() override {
        if (events.empty())
            return std::nullopt;
        auto value = std::move(events.front());
        events.pop_front();
        return value;
    }
    void disconnect() override { ++disconnects; }
};
// VoiceMedia 明确支持候选配置，任何旧video
// begin／prepare调用都计为错误；始终不采集、不播放、不申请camera。
struct VoiceMedia : CallMediaPort {
    bool supported = true, reserve_ok = true, begin_ok = true, answer_ok = true, ready = false;
    int prepares = 0, candidates = 0, old_prepares = 0, old_begins = 0, answers = 0, closes = 0;
    CallVoiceProfile last_profile;  // 只记录核心提供的固定格式，不从测试参数改核心配置。
    bool voice_supported() const override { return supported; }
    bool prepare_voice(const CallVoiceProfile& profile) override {
        ++prepares;
        last_profile = profile;
        return reserve_ok;
    }
    bool begin_voice_candidate(const CallVoiceProfile& profile) override {
        ++candidates;
        last_profile = profile;
        return begin_ok;
    }
    bool prepare() override {
        ++old_prepares;
        return true;
    }
    bool begin() override {
        ++old_begins;
        return true;
    }
    bool apply_answer(std::string_view) override {
        ++answers;
        return answer_ok;
    }
    bool connected() const override { return ready; }
    void close() override {
        ++closes;
        ready = false;
    }
};
// voice_config 默认借用连接，mode必须显式Voice；共享宿主已核自己的单一能力ACK。
HumanCallConfig voice_config(bool borrowed = true) {
    HumanCallConfig config;
    config.borrowed_connection = borrowed;
    config.mode = HumanCallMode::Voice;
    return config;
}
// event 保留本端独立腿及原请求token；不从call组猜接收腿。
Envelope event(std::string method, std::string payload, std::string token = "invite") {
    Envelope value;
    value.method = std::move(method);
    value.payload = std::move(payload);
    value.session = "voice-leg";
    value.token = std::move(token);
    return value;
}
// invitation 是冻结纯语音公共契约，sourceType仅此模式必填，旧视频不能接收它。
Envelope invitation() {
    auto value = event(
        "sessionInvite",
        R"({"purpose":"humanCall","callType":"voice","admission":"accept","transport":"webrtc","expiresAt":1001000,"callGroupId":"voice-group","tracks":[{"trackId":"voice","kind":"audio","sourceId":"microphone","sourceType":"microphone","direction":"sendrecv","codec":"opus","state":"requested"}]})");
    value.event_sequence = 1;
    return value;
}
// receive 只走正式公共decoder和共享入口，非法信封不能绕过状态机。
bool receive(HumanCallClient& client, const Envelope& value, std::uint64_t now = 101,
             std::uint64_t wall = 1000000) {
    std::string bytes;
    if (encode_envelope(value, bytes) != DecodeResult::Ok)
        return false;
    return client.receive_shared("$thing/down/signaling/p/d", bytes, now, wall);
}
// acknowledgement 只回应原上行请求，不用默认当前token假造匹配。
Envelope acknowledgement(const Envelope& request) {
    auto value = event(
        "requestAck",
        "{\"requestMethod\":\"" + request.method + "\",\"result\":{\"state\":\"negotiating\"}}",
        request.token);
    value.request = false;
    value.code = 200;
    return value;
}
// invited 接通原共享入口后收到合法来电；呈现Ringing本身不能申请任何实际媒体。
void invited(HumanCallClient& client) {
    ASSERT_TRUE(client.start_shared(100));
    ASSERT_TRUE(receive(client, invitation()));
    ASSERT_EQ(client.state(), HumanCallState::Ringing);
}
// mutate 只改变本测试的规范JSON字段；输出仍经过正式信封编码，不能直接改状态。
Envelope mutate(Envelope value, const std::function<void(cJSON*)>& change) {
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_Parse(value.payload.c_str()),
                                                         cJSON_Delete);
    if (!root) {
        value.payload = "null";
        return value;
    }
    change(root.get());
    auto* bytes = cJSON_PrintUnformatted(root.get());
    if (!bytes) {
        value.payload = "null";
        return value;
    }
    value.payload = bytes;
    cJSON_free(bytes);
    return value;
}
// negotiating 使用原接听ACK创建候选；绝不直接构造核心的Negotiating状态。
void negotiating(HumanCallClient& client, VoiceMqtt& mqtt) {
    ASSERT_NO_FATAL_FAILURE(invited(client));
    ASSERT_TRUE(client.accept(102));
    ASSERT_TRUE(receive(client, acknowledgement(mqtt.sent.back()), 103));
    ASSERT_EQ(client.state(), HumanCallState::Negotiating);
}
// connected 分别交付本端真实Peer连通、原mediaState ACK和平台Established，测试两种合法到达次序。
void connected(HumanCallClient& client, VoiceMqtt& mqtt, VoiceMedia& media,
               bool established_first) {
    ASSERT_NO_FATAL_FAILURE(negotiating(client, mqtt));
    ASSERT_TRUE(client.offer_ready("v=0\r\ns=voice\r\n", 104));
    auto answer =
        event("mediaAnswer",
              R"({"mediaRevision":1,"webrtc":{"iceMode":"complete","sdp":"v=0\r\ns=answer\r\n"}})",
              mqtt.sent.back().token);
    answer.request = false;
    answer.code = 200;
    ASSERT_TRUE(receive(client, answer, 105));
    EXPECT_FALSE(client.media_connected(106));  // SDP成功不是ICE／DTLS已经连通。
    media.ready = true;
    ASSERT_TRUE(client.media_connected(107));
    const auto ack = acknowledgement(mqtt.sent.back());
    auto established =
        event("sessionEstablished", R"({"state":"established","mediaRevision":1})", "established");
    established.request = false;
    established.event_sequence = 2;
    ASSERT_TRUE(receive(client, established_first ? established : ack, 108));
    EXPECT_FALSE(client.active());
    ASSERT_TRUE(receive(client, established_first ? ack : established, 109));
    ASSERT_TRUE(client.active());
    EXPECT_EQ(media.old_begins, 0);
    EXPECT_EQ(media.old_prepares, 0);
}

// 主动prepare复用原发布口；平台ACK或候选ready不关闭原实际媒体。
TEST(HumanCallVoice, ExplicitEndpointSourcePrepareKeepsOriginalMedia) {
    VoiceMqtt mqtt;
    VoiceMedia media;
    HumanCallClient client(mqtt, media, "p/d", "voice", voice_config());
    ASSERT_TRUE(client.set_endpoint_source(
        [](std::string_view session, std::uint64_t, std::uint64_t, HumanEndpointSourceOperation) {
            return session == "voice-leg";
        }));
    ASSERT_NO_FATAL_FAILURE(connected(client, mqtt, media, false));
    ASSERT_TRUE(client.endpoint_source_available(110, 1000000));
    const auto closes = media.closes, candidates = media.candidates;
    ASSERT_TRUE(client.prepare_endpoint_source("device:p:target", 110, 1000000));
    EXPECT_EQ(mqtt.sent.back().method, "endpointControl");
    EXPECT_EQ(mqtt.sent.back().session, "voice-leg");
    EXPECT_EQ(media.closes, closes);
    EXPECT_EQ(media.candidates, candidates);
    EXPECT_TRUE(client.active());
}

// HumanEndpointSource 使用真实两套请求/严格编码器；宿主权限仍为可控原事实，不冒认实网媒体。
class HumanEndpointSource : public ::testing::Test {
   protected:
    VoiceMqtt mqtt;
    VoiceMedia media;
    HumanCallClient client{mqtt, media, "p/d", "source", voice_config()};
    bool permitted = true;  // 唯一可信宿主门控事实，网页不传该值。
    void SetUp() override {
        ASSERT_TRUE(client.set_endpoint_source(
            [this](std::string_view session, std::uint64_t, std::uint64_t,
                   HumanEndpointSourceOperation) { return permitted && session == "voice-leg"; }));
        ASSERT_NO_FATAL_FAILURE(connected(client, mqtt, media, false));
    }
    // fact 只生成当前原handoff的严格事实，revision与全腿eventSeq分别给出。
    Envelope fact(std::string state, unsigned revision, bool reply = true, unsigned sequence = 3) {
        const auto& receipt = *client.endpoint_source_receipt();
        auto value = event("endpointState", "{}",
                           reply ? receipt.token : "endpoint-" + std::to_string(sequence));
        value.request = false;
        value.payload = "{\"state\":\"" + state + "\",\"revision\":" + std::to_string(revision) +
                        ",\"handoffId\":\"" + receipt.handoff + "\"";
        if (state == "ready" || state == "committed")
            value.payload += ",\"candidateSessionId\":\"candidate\"";
        if (state == "aborted") value.payload += ",\"reason\":\"userCancelled\"";
        value.payload += "}";
        if (reply)
            value.code = 200;
        else
            value.event_sequence = sequence;
        return value;
    }
    // ready 同一原准备请求ACK与平台主动ready，不创建或启用第二资源。
    void ready() {
        ASSERT_TRUE(client.prepare_endpoint_source("device:p:target", 110, 1000000));
        ASSERT_TRUE(receive(client, fact("preparing", 1), 111));
        ASSERT_TRUE(receive(client, fact("ready", 2, false), 112));
    }
};

// prepare/ready/commit事实不调用HAL close；正式源release由Host另验完整组后才收终态。
TEST_F(HumanEndpointSource, ReadyCommitKeepsOriginalMediaUntilFormalTerminal) {
    const auto closes = media.closes;
    ready();
    ASSERT_TRUE(client.commit_endpoint_source(113, 1000000));
    const auto committed = fact("committed", 3);
    ASSERT_TRUE(receive(client, committed, 114));
    EXPECT_EQ(client.endpoint_source_receipt()->state, "committed");
    EXPECT_EQ(media.closes, closes);
    EXPECT_TRUE(client.active());
    ASSERT_TRUE(receive(client, committed, 115));
    EXPECT_EQ(media.closes, closes);
}

// 先拒全腿跳号，再拒事务版本；两份账本均不得先推进，后续原合法ready仍可提交。
TEST_F(HumanEndpointSource, BothLedgersRejectAtomicallyAndReplayPreservesOriginalWatermark) {
    ASSERT_TRUE(client.prepare_endpoint_source("device:p:target", 110, 1000000));
    ASSERT_TRUE(receive(client, fact("preparing", 1), 111));
    auto jumped = fact("ready", 2, false, 4);
    EXPECT_FALSE(receive(client, jumped, 112));
    EXPECT_EQ(client.endpoint_source_receipt()->revision, 1U);
    auto inconsistent = fact("preparing", 1, false, 3);
    inconsistent.payload.replace(inconsistent.payload.find("preparing"), 9, "ready");
    EXPECT_FALSE(receive(client, inconsistent, 113));
    auto original = fact("ready", 2, false, 3);
    ASSERT_TRUE(receive(client, original, 114));
    ASSERT_TRUE(receive(client, original, 115));
    EXPECT_EQ(client.endpoint_source_receipt()->revision, 2U);
    auto conflict = original;
    conflict.payload.replace(conflict.payload.find("candidate"), 9, "different");
    EXPECT_FALSE(receive(client, conflict, 116));
    EXPECT_TRUE(client.abort_endpoint_source(117, 1000000));
    ASSERT_TRUE(receive(client, fact("aborted", 3), 118));
    EXPECT_EQ(client.endpoint_source_receipt()->state, "aborted");
}

// 用户可在ready后停止输入；commit不可把停止洗成恢复，abort不操作HAL或本人意图。
TEST_F(HumanEndpointSource, UserStoppedInputStillAbortsWithoutRestoringInputOrOutput) {
    ready();
    const auto closes = media.closes;
    EXPECT_FALSE(client.control_call("hold", 113));
    EXPECT_FALSE(client.transfer_call("p:other", 113));
    ASSERT_TRUE(client.control_track("voice", true, 113));
    EXPECT_FALSE(client.input_wanted());
    EXPECT_FALSE(client.commit_endpoint_source(114, 1000000));
    ASSERT_TRUE(client.abort_endpoint_source(115, 1000000));
    ASSERT_TRUE(receive(client, fact("aborted", 3), 116));
    EXPECT_FALSE(client.input_wanted());
    EXPECT_EQ(media.closes, closes);
    EXPECT_TRUE(client.active());
}

// 同一原请求30秒超时仅锁unknown，不换token、不关原源或接纳过期ACK。
TEST_F(HumanEndpointSource, TimeoutPreservesUnknownAndCannotRecreatePrepare) {
    ASSERT_TRUE(client.prepare_endpoint_source("device:p:target", 110, 1000000));
    const auto token = client.endpoint_source_receipt()->token;
    const auto ack = fact("preparing", 1);
    const auto closes = media.closes;
    client.poll(30110, 1000000);
    EXPECT_TRUE(client.endpoint_source_receipt()->unknown);
    EXPECT_FALSE(client.prepare_endpoint_source("device:p:other", 30111, 1000000));
    EXPECT_FALSE(receive(client, ack, 30112));
    EXPECT_EQ(client.endpoint_source_receipt()->token, token);
    EXPECT_EQ(media.closes, closes);
    EXPECT_TRUE(client.active());
}

// 整腿账本背压先于事务消费；不能先把ready存进事务账本而遗失对应全腿水位。
TEST_F(HumanEndpointSource, HumanBudgetRejectionDoesNotAdvanceEitherLedger) {
    ASSERT_TRUE(client.prepare_endpoint_source("device:p:target", 110, 1000000));
    ASSERT_TRUE(receive(client, fact("preparing", 1), 111));
    HumanEndpointSourceStaticFaultTest::fill_bytes(client, true);
    const auto ready = fact("ready", 2, false);
    EXPECT_FALSE(receive(client, ready, 112));
    EXPECT_EQ(client.endpoint_source_receipt()->revision, 1U);
    EXPECT_EQ(HumanEndpointSourceStaticFaultTest::sequence(client), 2U);
    HumanEndpointSourceStaticFaultTest::fill_bytes(client, false);
    ASSERT_TRUE(receive(client, ready, 113));
    EXPECT_EQ(client.endpoint_source_receipt()->revision, 2U);
    EXPECT_EQ(HumanEndpointSourceStaticFaultTest::sequence(client), 3U);
}

// 网络/原scope失效保原未知身份；清源或重复关闭都不伪造远端abort/恢复输入。
TEST_F(HumanEndpointSource, CloseRetainsUnknownOriginalTransactionWithoutRetryOrPermission) {
    ASSERT_TRUE(client.prepare_endpoint_source("device:p:target", 110, 1000000));
    const auto original = *client.endpoint_source_receipt();
    permitted = false;
    EXPECT_FALSE(client.abort_endpoint_source(111, 1000000));
    EXPECT_FALSE(client.commit_endpoint_source(111, 1000000));
    client.close();
    EXPECT_TRUE(client.endpoint_source_receipt()->unknown);
    EXPECT_EQ(client.endpoint_source_receipt()->token, original.token);
    EXPECT_EQ(client.endpoint_source_receipt()->handoff, original.handoff);
    const auto sent = mqtt.sent.size();
    client.poll(31110, 1000000);
    client.close();
    EXPECT_EQ(mqtt.sent.size(), sent);
    EXPECT_FALSE(client.prepare_endpoint_source("device:p:other", 31111, 1000000));
}

// 来源/时间/会话异常不创建source请求；原入口拒绝明确结束受理但不造事务failed。
TEST_F(HumanEndpointSource, TrustedScopeClockAndIngressRefusalNeverAuthorizeMedia) {
    permitted = false;
    EXPECT_FALSE(client.prepare_endpoint_source("device:p:target", 110, 1000000));
    permitted = true;
    EXPECT_FALSE(client.prepare_endpoint_source("device:p:target", 108, 1000000));
    EXPECT_FALSE(client.prepare_endpoint_source("device:p:target", 110, 999999));
    ASSERT_TRUE(client.prepare_endpoint_source("device:p:target", 110, 1000000));
    auto rejection =
        event("requestAck", R"({"requestMethod":"endpointControl","reason":"requestDenied"})",
              client.endpoint_source_receipt()->token);
    rejection.request = false;
    rejection.code = 403;
    ASSERT_TRUE(receive(client, rejection, 111));
    EXPECT_EQ(client.endpoint_source_receipt()->code, 403);
    EXPECT_TRUE(client.endpoint_source_receipt()->state.empty());
    EXPECT_TRUE(client.active());
    EXPECT_TRUE(client.prepare_endpoint_source("device:p:other", 112, 1000000));
}

// 参数/权限拒绝不推进时钟；回退ACK不更新两份账本或业务水位。
TEST_F(HumanEndpointSource, FailedSubmissionPreservesBusinessClocksAndWatermarks) {
    EXPECT_FALSE(client.prepare_endpoint_source("", 10000, 1000100));
    ASSERT_TRUE(client.prepare_endpoint_source("device:p:target", 110, 1000000));
    const auto original = *client.endpoint_source_receipt();
    auto ack = fact("preparing", 1);
    std::string ack_bytes;
    ASSERT_EQ(encode_envelope(ack, ack_bytes), DecodeResult::Ok);
    EXPECT_FALSE(
        client.observe_shared_endpoint("$thing/down/signaling/p/d", ack_bytes, 109, 1000000));
    EXPECT_EQ(client.endpoint_source_receipt()->revision, 0U);
    EXPECT_EQ(HumanEndpointSourceStaticFaultTest::sequence(client), 2U);
    ASSERT_TRUE(receive(client, ack, 111));
    EXPECT_EQ(client.endpoint_source_receipt()->token, original.token);
    client.close();
    EXPECT_FALSE(client.endpoint_source_available(112, 1000000));
}

// publish背压仍为原唯一Pending；实际字节重传不重新prepare，不更新媒体或事务版本。
TEST_F(HumanEndpointSource, PublishBackpressureKeepsOriginalPendingRequest) {
    mqtt.publish_ok = false;
    ASSERT_TRUE(client.prepare_endpoint_source("device:p:target", 110, 1000000));
    const auto original = *client.endpoint_source_receipt();
    EXPECT_TRUE(original.pending);
    EXPECT_EQ(original.revision, 0U);
    EXPECT_EQ(HumanEndpointSourceStaticFaultTest::sequence(client), 2U);
    EXPECT_FALSE(client.prepare_endpoint_source("device:p:other", 111, 1000000));
    const auto closes = media.closes;
    mqtt.publish_ok = true;
    client.poll(10110, 1000000);
    EXPECT_EQ(mqtt.sent.back().token, original.token);
    EXPECT_EQ(client.endpoint_source_receipt()->revision, 0U);
    EXPECT_EQ(HumanEndpointSourceStaticFaultTest::sequence(client), 2U);
    EXPECT_EQ(media.closes, closes);
}

// Ending/关闭的原关联仅保守收口，不靠迟到ACK/ready恢复输入或重新发起接续。
TEST_F(HumanEndpointSource, EndingAndCloseLateFactsCannotRestoreOrCreateAnotherTransaction) {
    ready();
    ASSERT_TRUE(client.commit_endpoint_source(113, 1000000));
    const auto late_ack = fact("committed", 3);
    const auto late_event = fact("committed", 3, false, 4);
    ASSERT_TRUE(client.end(114));
    const auto closes = media.closes;
    EXPECT_FALSE(receive(client, late_ack, 115));
    EXPECT_FALSE(receive(client, late_event, 116));
    EXPECT_EQ(client.state(), HumanCallState::Ending);
    EXPECT_FALSE(client.prepare_endpoint_source("device:p:another", 117, 1000000));
    EXPECT_EQ(media.closes, closes);
    client.close();
    const auto final_closes = media.closes;
    EXPECT_FALSE(receive(client, late_ack, 118));
    std::string late_bytes;
    ASSERT_EQ(encode_envelope(late_event, late_bytes), DecodeResult::Ok);
    EXPECT_FALSE(
        client.observe_shared_endpoint("$thing/down/signaling/p/d", late_bytes, 119, 1000000));
    EXPECT_FALSE(client.input_wanted());
    EXPECT_EQ(media.closes, final_closes);
}

// 未装配入口保持旧行为；待建立与已结束腿不能凭gate返回true取得操作资格。
TEST(HumanCallVoice, EndpointSourceDefaultAndUnestablishedScopeStayClosed) {
    VoiceMqtt mqtt;
    VoiceMedia media;
    HumanCallClient client(mqtt, media, "p/d", "voice", voice_config());
    EXPECT_EQ(client.endpoint_source_receipt(), nullptr);
    EXPECT_FALSE(client.prepare_endpoint_source("device:p:target", 100, 1000000));
    ASSERT_TRUE(client.set_endpoint_source([](std::string_view, std::uint64_t, std::uint64_t,
                                              HumanEndpointSourceOperation) { return true; }));
    ASSERT_NO_FATAL_FAILURE(negotiating(client, mqtt));
    EXPECT_FALSE(client.endpoint_source_available(110, 1000000));
    EXPECT_FALSE(client.prepare_endpoint_source("device:p:target", 110, 1000000));
    EXPECT_FALSE(client.set_endpoint_source([](std::string_view, std::uint64_t, std::uint64_t,
                                               HumanEndpointSourceOperation) { return true; }));
    client.close();
    EXPECT_FALSE(client.prepare_endpoint_source("device:p:target", 111, 1000000));
}

TEST(HumanCallVoice, InviteAcceptUsesOneTrackAndCandidateOnly) {
    VoiceMqtt mqtt;
    VoiceMedia media;
    HumanCallClient client(mqtt, media, "p/d", "voice", voice_config());
    ASSERT_NO_FATAL_FAILURE(invited(client));
    EXPECT_EQ(media.prepares, 0);
    EXPECT_EQ(media.candidates, 0);
    EXPECT_EQ(mqtt.connects, 0);
    ASSERT_TRUE(client.accept(102));
    ASSERT_EQ(mqtt.sent.back().method, "sessionAccept");
    EXPECT_EQ(mqtt.sent.back().payload, R"({"acceptedTracks":["voice"]})");
    EXPECT_EQ(media.prepares, 1);
    EXPECT_EQ(media.candidates, 0);
    ASSERT_TRUE(receive(client, acknowledgement(mqtt.sent.back()), 103));
    EXPECT_EQ(client.state(), HumanCallState::Negotiating);
    EXPECT_EQ(media.candidates, 1);
    EXPECT_TRUE(receive(client, acknowledgement(mqtt.sent.back()), 104));
    EXPECT_EQ(media.candidates, 1);  // 原接听ACK重投不能再建Peer或配置第二采集路径。
    EXPECT_EQ(media.old_prepares, 0);
    EXPECT_EQ(media.old_begins, 0);
    EXPECT_EQ(media.last_profile.sample_rate, 48000U);
    EXPECT_EQ(media.last_profile.channels, 1U);
    EXPECT_EQ(media.last_profile.frame_duration_ms, 20U);
}

// waiting_established 保留原邀请水位1与实际Peer连通，焦点事件由唯一宿主另行提交。
void waiting_established(HumanCallClient& client, VoiceMqtt& mqtt, VoiceMedia& media,
                         bool acknowledged = true) {
    ASSERT_NO_FATAL_FAILURE(negotiating(client, mqtt));
    ASSERT_TRUE(client.offer_ready("v=0\r\ns=voice\r\n", 104));
    auto answer =
        event("mediaAnswer",
              R"({"mediaRevision":1,"webrtc":{"iceMode":"complete","sdp":"v=0\r\ns=answer\r\n"}})",
              mqtt.sent.back().token);
    answer.request = false;
    answer.code = 200;
    ASSERT_TRUE(receive(client, answer, 105));
    media.ready = true;
    ASSERT_TRUE(client.media_connected(106));
    if (acknowledged) {
        ASSERT_TRUE(receive(client, acknowledgement(mqtt.sent.back()), 107));
    }
    ASSERT_EQ(client.state(), HumanCallState::WaitingEstablished);
}

// shared_focus 原focusControl属于Human腿事件2，严格走正式编码而不是改写Established事件3。
Envelope shared_focus(std::uint64_t sequence = 2, std::string token = "original-focus") {
    auto value = event(
        "focusControl",
        R"({"operation":"acquire","expectedRevision":0,"focusId":"human-focus","resources":["microphone","speaker"],"priority":"call","resumePolicy":"never","expiresAt":1001000})",
        std::move(token));
    value.event_sequence = sequence;
    return value;
}
// encoded 生成原合法字节供唯一执行器与Human水位共同观察，不能先归一化另一个报文。
std::string encoded(const Envelope& message) {
    std::string bytes;
    EXPECT_EQ(encode_envelope(message, bytes), DecodeResult::Ok);
    return bytes;
}
// SharedFocusAuthority 仅给原完整测试租约许可，不从网络ACK推导授权。
struct SharedFocusAuthority final : FocusAuthority {
    std::optional<FocusPermit> authorize(const FocusCommand& command, std::uint64_t,
                                         std::uint64_t) override {
        return FocusPermit{command, 1000, 1};
    }
    bool valid(const FocusPermit&, std::uint64_t, std::uint64_t) const override { return true; }
};
// SharedFocusDriver 分开受理与实际完整组事实，单元测试不启动任何采集／播放路径。
struct SharedFocusDriver final : FocusDriver {
    std::uint64_t current_revision = 0, generation = 0;  // 原实际CAS版本和当前操作代次。
    int begins = 0;                                      // 受理前不能出现驱动副作用。
    std::optional<FocusFact> fact;                       // 必须显式交付原完整实际事实。
    bool owned = false;                                  // 入队不能自动报告真实持有。
    std::uint64_t revision() const override { return current_revision; }
    bool begin(const FocusCommand&, std::uint64_t value) override {
        ++begins;
        generation = value;
        return true;
    }
    std::optional<FocusFact> take_fact(std::string_view, std::string_view) override {
        auto result = fact;
        fact.reset();
        return result;
    }
    bool owns(const FocusCommand&) const override { return owned; }
    bool stop_scope(std::string_view, std::string_view, const std::vector<std::string>&) override {
        return true;
    }
};
// SharedFocusProof 通过真实生产FocusEngine返回原准入和实际原结果，禁止手造成功租约。
struct SharedFocusProof {
    SharedFocusDriver driver;        // 真实协议执行器的可控驱动，不冒充硬件验收。
    SharedFocusAuthority authority;  // 显式原许可，寿命长于执行器。
    FocusEngine engine{driver, authority, "voice-leg", "human-focus", {"microphone", "speaker"},
                       1000};
    // completed 仅在生产执行器收录原代次全组事实后返回原Replay。
    FocusSubmission completed(std::string_view body) {
        const auto accepted = engine.submit(body, 108, 1000000);
        EXPECT_EQ(accepted.admission, FocusAdmission::Accepted);
        engine.poll(108, 1000000);
        driver.current_revision = 1;
        driver.owned = true;
        driver.fact = FocusFact{driver.generation, true, "acquired", {"microphone", "speaker"}, 1};
        engine.poll(109, 1000000);
        const auto result = engine.submit(body, 109, 1000000);
        EXPECT_EQ(result.admission, FocusAdmission::Replay);
        return result;
    }
    // released
    // 固定原engine真实停止结果；可延后交付给Core，但不以本地close冒充完整停止。
    FocusSubmission released(std::string_view body) {
        EXPECT_EQ(engine.submit(body, 110, 1000001).admission, FocusAdmission::Accepted);
        engine.poll(110, 1000001);
        driver.current_revision = 2;
        driver.owned = false;
        driver.fact = FocusFact{driver.generation, true, "released", {"microphone", "speaker"}, 2};
        engine.poll(111, 1000001);
        const auto result = engine.submit(body, 112, 1000001);
        EXPECT_EQ(result.admission, FocusAdmission::Replay);
        return result;
    }
};

// 原正式候选出处、ready和release都须成立；错误/跳号aborted不推进水位，不继承源或重启媒体。
TEST(HumanCallVoice, CandidateAbortCleanupRequiresOriginalScopeActualReleaseAndExactWatermark) {
    for (const bool timeout : {false, true}) {
        for (const unsigned invalid : {0U, 1U, 2U, 3U, 4U}) {
            SCOPED_TRACE(timeout);
            SCOPED_TRACE(invalid);
            VoiceMqtt mqtt;
            VoiceMedia media;
            HumanCallClient client(mqtt, media, "p/d", "voice", voice_config());
            ASSERT_TRUE(client.set_endpoint_source([](auto, auto, auto, auto) { return false; }));
            ASSERT_TRUE(client.start_shared(100));
            auto invite = invitation();
            invite.payload.pop_back();
            invite.payload +=
                R"(,"handoff":{"sourceSessionId":"original-source","handoffId":"original-handoff"}})";
            ASSERT_TRUE(receive(client, invite, 101));
            ASSERT_TRUE(client.accept(102));
            ASSERT_TRUE(receive(client, acknowledgement(mqtt.sent.back()), 103));
            ASSERT_TRUE(client.offer_ready("v=0\r\ns=voice\r\n", 104));
            auto answer = acknowledgement(mqtt.sent.back());
            answer.method = "mediaAnswer";
            answer.payload =
                R"({"mediaRevision":1,"webrtc":{"iceMode":"complete","sdp":"v=0\r\ns=answer\r\n"}})";
            ASSERT_TRUE(receive(client, answer, 105));
            media.ready = true;
            ASSERT_TRUE(client.media_connected(106));
            ASSERT_TRUE(receive(client, acknowledgement(mqtt.sent.back()), 107));
            SharedFocusProof proof;
            const auto acquire = encoded(shared_focus());
            ASSERT_TRUE(client.observe_shared_focus("$thing/down/signaling/p/d", acquire,
                                                    proof.completed(acquire), 109, 1000000));
            auto ready = event(
                "endpointState",
                R"({"state":"ready","revision":2,"handoffId":"original-handoff","candidateSessionId":"voice-leg"})",
                "ready");
            ready.request = false;
            ready.event_sequence = 3;
            for (unsigned wrong = 0; wrong < 6; ++wrong) {
                auto bad_ready = ready;
                if (wrong == 0)
                    bad_ready.payload.replace(bad_ready.payload.find("voice-leg"), 9,
                                              "foreign-leg");
                if (wrong == 1)
                    bad_ready.payload.replace(bad_ready.payload.find("original-handoff"), 16,
                                              "foreign-handoff");
                if (wrong == 2)
                    bad_ready.event_sequence = 4;
                if (wrong == 3)
                    bad_ready.payload.insert(bad_ready.payload.size() - 1,
                                             R"(,"sourceSessionId":"foreign-source")");
                if (wrong == 4)
                    bad_ready.payload.replace(bad_ready.payload.find("\"revision\":2"), 12,
                                              "\"revision\":0");
                if (wrong == 5)
                    bad_ready.session = "foreign-session";
                EXPECT_FALSE(receive(client, bad_ready, 110, 1000001));
                EXPECT_EQ(HumanEndpointSourceStaticFaultTest::sequence(client), 2U);
            }
            ASSERT_TRUE(receive(client, ready, 110, 1000001));
            ASSERT_TRUE(receive(client, ready, 110, 1000001));
            ASSERT_EQ(HumanEndpointSourceStaticFaultTest::sequence(client), 3U);
            auto changed_ready = ready;
            changed_ready.token = "changed-ready";
            EXPECT_FALSE(receive(client, changed_ready, 110, 1000001));
            EXPECT_FALSE(client.endpoint_source_available(110, 1000001));
            auto aborted = event(
                "endpointState",
                R"({"state":"aborted","revision":3,"handoffId":"original-handoff","reason":"userCancelled"})",
                "aborted");
            aborted.request = false;
            aborted.event_sequence = 5;
            EXPECT_FALSE(receive(client, aborted, 110, 1000001));
            auto release = shared_focus(4, "release");
            release.payload =
                R"({"operation":"release","expectedRevision":1,"focusId":"human-focus","resources":["microphone","speaker"],"priority":"call","resumePolicy":"never"})";
            const auto stopped = proof.released(encoded(release));
            auto reacquire = shared_focus(5, "forbidden-reacquire");
            reacquire.payload.replace(reacquire.payload.find("\"expectedRevision\":0"), 20,
                                      "\"expectedRevision\":2");
            const auto original_begins = proof.driver.begins;
            EXPECT_EQ(proof.engine.submit(encoded(reacquire), 113, 1000002).admission,
                      FocusAdmission::Closed); // 原实际released终结该scope，不可再取得或走hold/resume。
            EXPECT_EQ(proof.driver.begins, original_begins);
            const auto now = timeout ? 30108U : 113U;
            const auto wall = timeout ? 1030001U : 1000002U;
            if (timeout) {
                client.poll(30107, 1030000);
                ASSERT_EQ(client.state(),
                          HumanCallState::Failed); // ready不得刷新原phase截止。
                EXPECT_FALSE(client.can_observe_shared_focus(
                    "$thing/down/signaling/p/d", encoded(shared_focus(4, "forbidden-new-acquire")),
                    now, wall));
            }
            ASSERT_TRUE(client.observe_shared_focus("$thing/down/signaling/p/d", encoded(release),
                                                    stopped, now, wall));
            // 普通活腿原时钟回退合同会关闭整腿；此处只核停止墓碑的只读拒绝，不制造可恢复的业务回拨。
            if (timeout) {
                EXPECT_FALSE(receive(client, aborted, now - 1, wall));
            }
            auto bad = aborted;
            if (invalid == 0)
                bad.payload.replace(bad.payload.find("original-handoff"), 16, "foreign-handoff");
            if (invalid == 1)
                bad.session = "foreign-session";
            if (invalid == 2)
                bad.event_sequence = 6;
            if (invalid == 3)
                bad.payload =
                    R"({"state":"ready","revision":3,"handoffId":"original-handoff","candidateSessionId":"voice-leg"})";
            if (invalid == 4)
                bad.payload.insert(bad.payload.size() - 1,
                                   R"(,"sourceSessionId":"foreign-source")");
            EXPECT_FALSE(receive(client, bad, now, wall));
            EXPECT_EQ(HumanEndpointSourceStaticFaultTest::sequence(client), 4U);
            ASSERT_TRUE(receive(client, aborted, now, wall));
            ASSERT_TRUE(receive(client, aborted, now, wall));
            EXPECT_EQ(HumanEndpointSourceStaticFaultTest::sequence(client), 5U);
            bad = aborted;
            bad.token = "changed-token";
            EXPECT_FALSE(receive(client, bad, now, wall));
            EXPECT_FALSE(client.active());
            auto ended =
                event("sessionEnded", R"({"state":"ended","reason":"userCancelled"})", "ended");
            ended.request = false;
            ended.event_sequence = 6;
            ASSERT_TRUE(receive(client, ended, now, wall));
            EXPECT_EQ(client.state(), HumanCallState::Available);
            EXPECT_FALSE(client.can_observe_shared_focus(
                "$thing/down/signaling/p/d", encoded(shared_focus(7, "new-acquire")), now, wall));
            EXPECT_FALSE(receive(client, aborted, now, wall));
            client.close();
            ASSERT_TRUE(client.start_shared(now + 1));
            auto next_invite = invite;
            next_invite.session = "next-voice-leg";
            next_invite.token = "next-invite";
            next_invite.payload.replace(next_invite.payload.find("original-handoff"), 16,
                                        "next-handoff");
            // 新来电有自己的新邀请窗口；不能把上一轮已经自然过期的expiresAt当新轮有效凭据。
            next_invite = mutate(next_invite, [wall](cJSON* json) {
                cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(json, "expiresAt"), wall + 1000);
            });
            ASSERT_TRUE(receive(client, next_invite, now + 2, wall));
            EXPECT_EQ(client.state(), HumanCallState::Ringing);
            EXPECT_FALSE(receive(client, aborted, now + 2, wall));
            EXPECT_FALSE(client.can_observe_shared_focus("$thing/down/signaling/p/d",
                encoded(release), now + 2, wall));
            EXPECT_EQ(client.state(), HumanCallState::Ringing);
        }
    }
}

// 原完整组已实际取得却未Established时超阶段；只留下原停止墓碑，不为新取得/恢复/换腿开放资格。
TEST(HumanCallVoice, PhaseTimeoutCleanupTombstoneRejectsNewAuthority) {
    VoiceMqtt mqtt;
    VoiceMedia media;
    HumanCallClient client(mqtt, media, "p/d", "voice", voice_config());
    ASSERT_NO_FATAL_FAILURE(waiting_established(client, mqtt, media));
    const auto acquire = encoded(shared_focus());
    SharedFocusProof proof;
    ASSERT_TRUE(client.observe_shared_focus("$thing/down/signaling/p/d", acquire,
                                            proof.completed(acquire), 109, 1000000));
    client.poll(30107, 1030000);
    ASSERT_EQ(client.state(), HumanCallState::Failed);
    ASSERT_EQ(client.failure_facts().failure, HumanCallFailure::PhaseDeadline);
    EXPECT_FALSE(client.active());
    EXPECT_FALSE(client.input_wanted());
    auto release = shared_focus(3, "original-release");
    release.payload =
        R"({"operation":"release","expectedRevision":1,"focusId":"human-focus","resources":["microphone","speaker"],"priority":"call","resumePolicy":"never"})";
    const auto bytes = encoded(release);
    auto early_end = event("sessionEnded", R"({"reason":"hangup"})", "original-ended");
    early_end.event_sequence = 3;
    EXPECT_FALSE(
        client.receive_shared("$thing/down/signaling/p/d", encoded(early_end), 30108, 1030001));
    EXPECT_TRUE(
        client.can_observe_shared_focus("$thing/down/signaling/p/d", bytes, 30108, 1030001));
    EXPECT_FALSE(
        client.can_observe_shared_focus("$thing/down/signaling/p/d", acquire, 30108, 1030001));
    auto changed = release;
    changed.session = "different-session";
    EXPECT_FALSE(client.can_observe_shared_focus("$thing/down/signaling/p/d", encoded(changed),
                                                 30108, 1030001));
    changed = release;
    changed.payload.replace(changed.payload.find("human-focus"), 11, "other-focus");
    EXPECT_FALSE(client.can_observe_shared_focus("$thing/down/signaling/p/d", encoded(changed),
                                                 30108, 1030001));
    changed = release;
    changed.event_sequence = 4;
    EXPECT_FALSE(client.can_observe_shared_focus("$thing/down/signaling/p/d", encoded(changed),
                                                 30108, 1030001));
    changed = release;
    changed.payload.replace(changed.payload.find("release"), 7, "restore");
    EXPECT_FALSE(client.can_observe_shared_focus("$thing/down/signaling/p/d", encoded(changed),
                                                 30108, 1030001));
    client.close();
    EXPECT_FALSE(
        client.can_observe_shared_focus("$thing/down/signaling/p/d", bytes, 30109, 1030002));
}

// 超时保停止墓碑不能遗漏原在途请求的退休；保留窗口到期后可回收且新一轮不重传旧帧。
TEST(HumanCallVoice, PhaseTimeoutCleanupRetiresOriginalRequestsBeforeMovingScope) {
    VoiceMqtt mqtt;
    VoiceMedia media;
    HumanCallClient client(mqtt, media, "p/d", "voice", voice_config());
    ASSERT_NO_FATAL_FAILURE(waiting_established(client, mqtt, media, false));
    const auto pending = mqtt.sent.back();
    ASSERT_EQ(pending.method, "mediaState");
    const auto acquire = encoded(shared_focus());
    SharedFocusProof proof;
    ASSERT_TRUE(client.observe_shared_focus("$thing/down/signaling/p/d", acquire,
                                            proof.completed(acquire), 109, 1000000));
    client.poll(30107, 1030000);
    ASSERT_EQ(client.state(), HumanCallState::Failed);
    const auto sent = mqtt.sent.size();
    EXPECT_EQ(HumanEndpointSourceStaticFaultTest::expire_request(client, pending.token, 630108),
              RequestState::Missing);
    EXPECT_EQ(mqtt.sent.size(), sent);
    client.close();
    ASSERT_TRUE(client.start_shared(630109));
    client.poll(640110, 1640003);
    EXPECT_EQ(std::count_if(mqtt.sent.begin(), mqtt.sent.end(), [&](const auto& value) {
        return value.method == pending.method && value.token == pending.token;
    }), 1);
}

TEST(HumanCallVoice, SharedFocusWatermarkAllowsOriginalEstablishedThree) {
    for (const bool acknowledged : {false, true}) {
        VoiceMqtt mqtt;
        VoiceMedia media;
        HumanCallClient client(mqtt, media, "p/d", "voice", voice_config());
        ASSERT_NO_FATAL_FAILURE(waiting_established(client, mqtt, media, acknowledged));
        const auto state_ack = acknowledgement(mqtt.sent.back());
        const auto body = encoded(shared_focus());
        SharedFocusProof proof;
        const auto accepted = proof.engine.submit(body, 108, 1000000);
        ASSERT_EQ(accepted.admission, FocusAdmission::Accepted);
        ASSERT_TRUE(
            client.observe_shared_focus("$thing/down/signaling/p/d", body, accepted, 108, 1000000));
        EXPECT_EQ(client.state(), HumanCallState::WaitingEstablished);
        EXPECT_EQ(proof.driver.begins, 0);  // 观察受理既不执行驱动，也不使媒体Connected。
        EXPECT_FALSE(client.active());
        auto established = event("sessionEstablished",
                                 R"({"state":"established","mediaRevision":1})", "established");
        established.request = false;
        established.event_sequence = 3;
        ASSERT_TRUE(receive(client, established, 109));
        EXPECT_EQ(client.active(), acknowledged);
        if (!acknowledged) {
            ASSERT_TRUE(receive(client, state_ack, 110));
        }
        ASSERT_TRUE(client.active());
        EXPECT_EQ(media.candidates, 1);
        ASSERT_TRUE(client.end(111));
        auto ended = event("sessionEnded", R"({"reason":"hangup"})", mqtt.sent.back().token);
        ended.request = false;
        ended.code = 200;
        ASSERT_TRUE(receive(client, ended, 112));
        EXPECT_EQ(client.state(), HumanCallState::Available);
        EXPECT_EQ(mqtt.disconnects, 0);
    }
}

TEST(HumanCallVoice, SharedFocusRejectedObservationPreservesOriginalLegAndWatermark) {
    for (const int invalid : {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17}) {
        SCOPED_TRACE(invalid);
        VoiceMqtt mqtt;
        VoiceMedia media;
        HumanCallClient client(mqtt, media, "p/d", "voice", voice_config());
        ASSERT_NO_FATAL_FAILURE(waiting_established(client, mqtt, media));
        const auto original = encoded(shared_focus());
        SharedFocusProof proof;
        auto submission = proof.engine.submit(original, 108, 1000000);
        const auto accepted = submission;  // 原实际执行器返回的受理，不由测试伪造成功租约。
        auto message = shared_focus();
        std::string topic = "$thing/down/signaling/p/d", body;
        std::uint64_t now = 108, wall = 1000000;
        if (invalid == 0)
            topic = "$thing/down/signaling/p/other";
        if (invalid == 1)
            message.session = "another-leg";
        if (invalid == 2)
            message.event_sequence = 4;  // 合法focus不能越过未观察的原事件。
        if (invalid == 3)
            message.event_sequence = 1;
        if (invalid == 4)
            message = mutate(message, [](cJSON* root) {
                cJSON_DeleteItemFromArray(cJSON_GetObjectItemCaseSensitive(root, "resources"), 0);
            });
        if (invalid == 5)
            message = mutate(message, [](cJSON* root) {
                cJSON_ReplaceItemInArray(cJSON_GetObjectItemCaseSensitive(root, "resources"), 0,
                                         cJSON_CreateString("display"));
            });
        if (invalid == 6)
            message =
                mutate(message, [](cJSON* root) { cJSON_AddBoolToObject(root, "extra", true); });
        if (invalid == 7)
            now = 106;
        if (invalid == 8)
            wall = 999999;
        if (invalid == 9)
            wall = 0;
        if (invalid == 10)
            submission.admission = FocusAdmission::Invalid;
        if (invalid == 11)
            submission.admission = FocusAdmission::Conflict;
        if (invalid == 12)
            submission.admission = FocusAdmission::Busy;
        if (invalid == 13)
            submission.admission = FocusAdmission::Full;
        if (invalid == 14)
            submission.admission = FocusAdmission::Closed;
        if (invalid == 15)
            submission.admission = FocusAdmission::Replay;  // 无原实际结果不能凭Replay标签推进。
        if (invalid == 16)
            submission.reply = R"({"code":200})";  // 普通ACK不是实际端点受理或完整结果。
        if (invalid == 17) {
            message.method = "focusState";
            message.request = false;
            message.code = 200;  // 网络下行焦点事实不属于这个原端点观察入口。
        }
        body = encoded(message);
        const auto sent = mqtt.sent.size();
        const auto closes = media.closes;
        EXPECT_FALSE(client.observe_shared_focus(topic, body, submission, now, wall));
        EXPECT_EQ(client.state(), HumanCallState::WaitingEstablished);
        EXPECT_EQ(client.session_id(), "voice-leg");
        EXPECT_EQ(client.failure_facts().failure, HumanCallFailure::None);
        EXPECT_EQ(mqtt.sent.size(), sent);
        EXPECT_EQ(media.closes, closes);
        EXPECT_EQ(proof.driver.begins, 0);
        // 拒绝观察既不能先推进序号，也不能把失败时刻写入原时钟以阻断合法恢复。
        ASSERT_TRUE(client.observe_shared_focus("$thing/down/signaling/p/d", original, accepted,
                                                108, 1000000));
        auto established = event("sessionEstablished",
                                 R"({"state":"established","mediaRevision":1})", "established");
        established.request = false;
        established.event_sequence = 3;
        ASSERT_TRUE(receive(client, established, 109));
        EXPECT_TRUE(client.active());
    }
}

TEST(HumanCallVoice, SharedFocusReplayRequiresOriginalCompleteActualResult) {
    for (const int invalid : {-1, 0, 1, 2, 3, 4, 5}) {
        SCOPED_TRACE(invalid);
        VoiceMqtt mqtt;
        VoiceMedia media;
        HumanCallClient client(mqtt, media, "p/d", "voice", voice_config());
        ASSERT_NO_FATAL_FAILURE(waiting_established(client, mqtt, media));
        const auto body = encoded(shared_focus());
        SharedFocusProof proof;
        const auto original = proof.completed(body);
        auto submission = original;
        Envelope actual;
        ASSERT_EQ(decode_envelope(submission.reply, actual), DecodeResult::Ok);
        if (invalid == 0)
            actual.token = "another-operation";
        if (invalid == 1)
            actual.session = "another-leg";
        if (invalid == 2)
            actual = mutate(actual, [](cJSON* root) {
                cJSON_DeleteItemFromArray(cJSON_GetObjectItemCaseSensitive(root, "resources"), 0);
            });
        if (invalid == 3)
            actual = mutate(actual, [](cJSON* root) {
                cJSON_ReplaceItemInObjectCaseSensitive(root, "state",
                                                       cJSON_CreateString("released"));
            });
        if (invalid == 4)
            actual = mutate(actual, [](cJSON* root) {
                cJSON_ReplaceItemInObjectCaseSensitive(root, "revision", cJSON_CreateNumber(2));
            });
        if (invalid == 5) {
            actual.code = 403;
            actual = mutate(actual, [](cJSON* root) {
                cJSON_ReplaceItemInObjectCaseSensitive(root, "state", cJSON_CreateString("denied"));
                cJSON_ReplaceItemInObjectCaseSensitive(root, "revision", cJSON_CreateNumber(0));
                cJSON_AddStringToObject(root, "reason", "permissionDenied");
            });
        }
        submission.reply = encoded(actual);
        EXPECT_EQ(client.observe_shared_focus("$thing/down/signaling/p/d", body, submission, 109,
                                              1000000),
                  invalid == -1);
        EXPECT_EQ(client.state(), HumanCallState::WaitingEstablished);
        EXPECT_FALSE(client.active());
        ASSERT_TRUE(
            client.observe_shared_focus("$thing/down/signaling/p/d", body, original, 109, 1000000));
        EXPECT_EQ(proof.driver.begins, 1);  // 观察成功或失败都不能再执行实际焦点命令。
    }
}

TEST(HumanCallVoice, SharedFocusOriginalBytesDuplicateConflictAndBoundedHistory) {
    VoiceMqtt mqtt;
    VoiceMedia media;
    HumanCallClient client(mqtt, media, "p/d", "voice", voice_config());
    ASSERT_NO_FATAL_FAILURE(waiting_established(client, mqtt, media));
    const auto original = encoded(shared_focus());
    SharedFocusProof proof;
    const auto accepted = proof.engine.submit(original, 108, 1000000);
    ASSERT_TRUE(
        client.observe_shared_focus("$thing/down/signaling/p/d", original, accepted, 108, 1000000));
    const auto pending = proof.engine.submit(original, 109, 1000000);
    EXPECT_FALSE(
        client.observe_shared_focus(original, "$thing/down/signaling/p/d", accepted, 109, 1000000));
    ASSERT_EQ(pending.admission, FocusAdmission::Pending);
    EXPECT_TRUE(
        client.observe_shared_focus("$thing/down/signaling/p/d", original, pending, 109, 1000000));
    EXPECT_FALSE(client.observe_shared_focus("$thing/down/signaling/p/d", original + " ", pending,
                                             109, 1000000));
    EXPECT_FALSE(client.observe_shared_focus("$thing/down/signaling/p/d", encoded(shared_focus(3)),
                                             accepted, 109, 1000000));
    EXPECT_FALSE(client.observe_shared_focus("$thing/down/signaling/p/d",
                                             encoded(shared_focus(2, "another-token")), accepted,
                                             109, 1000000));
    EXPECT_FALSE(
        client.observe_shared_focus("$thing/down/signaling/p/d", original, pending, 107, 1000000));
    EXPECT_FALSE(
        client.observe_shared_focus("$thing/down/signaling/p/d", original, pending, 109, 999999));
    auto established =
        event("sessionEstablished", R"({"state":"established","mediaRevision":1})", "established");
    established.request = false;
    established.event_sequence = 3;
    ASSERT_TRUE(receive(client, established, 109));
    EXPECT_TRUE(
        client.observe_shared_focus("$thing/down/signaling/p/d", original, pending, 110,
                                    1000000));  // 更晚已合法Human事件不使原字节重投跳回水位。
    EXPECT_EQ(proof.driver.begins, 0);
    client.close();
    EXPECT_FALSE(
        client.observe_shared_focus("$thing/down/signaling/p/d", original, pending, 111, 1000000));
    EXPECT_TRUE(client.session_id().empty());
    EXPECT_EQ(mqtt.disconnects, 0);
}

TEST(HumanCallVoice, SharedFocusEndingCorrelatesReleaseButNeverTreatsPendingAsActual) {
    VoiceMqtt mqtt;
    VoiceMedia media;
    HumanCallClient client(mqtt, media, "p/d", "voice", voice_config());
    ASSERT_NO_FATAL_FAILURE(waiting_established(client, mqtt, media));
    const auto body = encoded(shared_focus());
    SharedFocusProof proof;
    const auto acquired = proof.completed(body);
    ASSERT_TRUE(
        client.observe_shared_focus("$thing/down/signaling/p/d", body, acquired, 109, 1000000));
    auto established =
        event("sessionEstablished", R"({"state":"established","mediaRevision":1})", "established");
    established.request = false;
    established.event_sequence = 3;
    ASSERT_TRUE(receive(client, established, 110));
    ASSERT_TRUE(client.end(111));
    const auto sent = mqtt.sent.size(), closes = static_cast<std::size_t>(media.closes);
    EXPECT_FALSE(client.observe_shared_focus("$thing/down/signaling/p/d", body, acquired, 112,
                                             1000000));  // Ending不能重放acquire恢复权限。
    auto release = shared_focus(4, "original-release");
    release.payload =
        R"({"operation":"release","expectedRevision":1,"focusId":"human-focus","resources":["microphone","speaker"],"priority":"call","resumePolicy":"never"})";
    const auto release_body = encoded(release);
    const auto accepted = proof.engine.submit(release_body, 112, 1000000);
    ASSERT_EQ(accepted.admission, FocusAdmission::Accepted);
    EXPECT_TRUE(client.observe_shared_focus("$thing/down/signaling/p/d", release_body, accepted,
                                             112, 1000000));  // 仅关联原在途，仍处于Ending不清scope。
    proof.engine.poll(112, 1000000);
    const auto pending = proof.engine.submit(release_body, 112, 1000000);
    ASSERT_EQ(pending.admission, FocusAdmission::Pending);
    EXPECT_TRUE(client.observe_shared_focus("$thing/down/signaling/p/d", release_body, pending,
                                             112, 1000000));
    proof.driver.current_revision = 2;
    proof.driver.owned = false;
    proof.driver.fact =
        FocusFact{proof.driver.generation, true, "released", {"microphone", "speaker"}, 2};
    proof.engine.poll(113, 1000000);
    const auto actual = proof.engine.submit(release_body, 113, 1000000);
    ASSERT_EQ(actual.admission, FocusAdmission::Replay);
    ASSERT_TRUE(client.observe_shared_focus("$thing/down/signaling/p/d", release_body, actual, 113,
                                            1000000));
    EXPECT_EQ(client.state(), HumanCallState::Ending);
    EXPECT_EQ(mqtt.sent.size(), sent);
    EXPECT_EQ(static_cast<std::size_t>(media.closes), closes);
    auto ended = event("sessionEnded", R"({"reason":"hangup"})", "ended");
    ended.request = false;
    ended.event_sequence = 5;
    ASSERT_TRUE(receive(client, ended, 114));
    EXPECT_EQ(client.state(), HumanCallState::Available);
    EXPECT_FALSE(client.observe_shared_focus("$thing/down/signaling/p/d", release_body, actual, 115,
                                             1000000));
}

TEST(HumanCallVoice, SharedFocusLedgerCapacityRejectsWithoutChangingWatermarkOrClock) {
    for (const bool byte_budget : {false, true}) {
        VoiceMqtt mqtt;
        VoiceMedia media;
        HumanCallClient client(mqtt, media, "p/d", "voice", voice_config());
        ASSERT_NO_FATAL_FAILURE(waiting_established(client, mqtt, media));
        auto original = encoded(shared_focus());
        if (byte_budget)
            original.append(58000, ' ');  // 原字节预算含合法JSON空白，不能归一化后规避容量。
        SharedFocusProof proof;
        const auto replay = proof.completed(original);
        ASSERT_TRUE(client.observe_shared_focus("$thing/down/signaling/p/d", original, replay, 109,
                                                1000000));
        std::uint64_t next_sequence = 3, next_now = 110;
        if (!byte_budget) {
            // 实际执行器对31条原CAS过期请求均返回Accepted并生成denied；Accepted不是资源取得。
            // 水位必须仍能消费原事件，但观察本身不能重新执行任何媒体或把denied说成成功租约。
            for (std::uint64_t index = 1; index < 32; ++index) {
                const auto body =
                    encoded(shared_focus(index + 2, "observed-" + std::to_string(index)));
                const auto accepted = proof.engine.submit(body, index + 109, 1000000);
                ASSERT_EQ(accepted.admission, FocusAdmission::Accepted);
                ASSERT_TRUE(client.observe_shared_focus("$thing/down/signaling/p/d", body, accepted,
                                                        index + 109, 1000000));
                ASSERT_TRUE(proof.engine.take_reply().has_value());
            }
            next_sequence = 34;
            next_now = 141;
        }
        auto next = shared_focus(next_sequence, "capacity-request");
        next = mutate(next, [](cJSON* root) {
            cJSON_ReplaceItemInObjectCaseSensitive(root, "focusId",
                                                   cJSON_CreateString("extra-focus"));
            cJSON_ReplaceItemInObjectCaseSensitive(root, "expectedRevision", cJSON_CreateNumber(1));
        });
        auto body = encoded(next);
        FocusEngine other(proof.driver, proof.authority, "voice-leg", "extra-focus",
                          {"microphone", "speaker"}, 1000);
        if (byte_budget)
            body.append(10000, ' ');
        const auto accepted = other.submit(body, next_now, 1000000);
        ASSERT_EQ(accepted.admission, FocusAdmission::Accepted);
        EXPECT_FALSE(client.observe_shared_focus("$thing/down/signaling/p/d", body, accepted,
                                                 next_now + 1, 1000000));
        EXPECT_EQ(client.state(), HumanCallState::WaitingEstablished);
        EXPECT_EQ(proof.driver.begins, 1);
        // 拒绝容量后仍接受原字节重投，不驱逐旧记录让相同原身份再推进。
        ASSERT_TRUE(client.observe_shared_focus("$thing/down/signaling/p/d", original, replay,
                                                next_now, 1000000));
        auto established = event("sessionEstablished",
                                 R"({"state":"established","mediaRevision":1})", "established");
        established.request = false;
        established.event_sequence = next_sequence;
        ASSERT_TRUE(receive(client, established, next_now));
        EXPECT_TRUE(client.active());
    }
}

// 独占连接仍沿真实订阅/能力/来电/Peer事件泵；接续错序不关闭原通话，合法ACK与ready可继续。
TEST(HumanCallVoice, IndependentEndpointSourceUsesOriginalEventPumpAndRejectsScopeWithoutClosing) {
    VoiceMqtt mqtt;
    VoiceMedia media;
    HumanCallClient client(mqtt, media, "p/d", "source", voice_config(false));
    ASSERT_TRUE(client.set_endpoint_source(
        [](std::string_view session, std::uint64_t, std::uint64_t, HumanEndpointSourceOperation) {
            return session == "voice-leg";
        }));
    ASSERT_TRUE(client.start(MqttConfig{}, 100));
    mqtt.events.push_back({MqttEvent::Kind::Connected, 0, 0, {}, {}});
    client.poll(101, 1000000);
    mqtt.events.push_back({MqttEvent::Kind::Subscribed, 7, 0, {}, {}});
    client.poll(102, 1000000);
    // queue只使用正式编码及原MQTT入队，不直接调用Human私有receive。
    const auto queue = [&](Envelope value, unsigned now) {
        std::string bytes;
        ASSERT_EQ(encode_envelope(value, bytes), DecodeResult::Ok);
        mqtt.events.push_back({MqttEvent::Kind::Message, 0, 0, "$thing/down/signaling/p/d", bytes});
        client.poll(now, 1000000);
    };
    auto cap = event("requestAck",
                     R"({"requestMethod":"capabilityReport","result":{"acceptedRevision":1}})",
                     mqtt.sent.back().token);
    cap.session.clear();
    cap.request = false;
    cap.code = 200;
    queue(cap, 103);
    ASSERT_EQ(client.state(), HumanCallState::Available);
    queue(invitation(), 104);
    ASSERT_TRUE(client.accept(105));
    queue(acknowledgement(mqtt.sent.back()), 106);
    ASSERT_TRUE(client.offer_ready("v=0\r\ns=voice\r\n", 107));
    auto answer =
        event("mediaAnswer",
              R"({"mediaRevision":1,"webrtc":{"iceMode":"complete","sdp":"v=0\r\ns=answer\r\n"}})",
              mqtt.sent.back().token);
    answer.request = false;
    answer.code = 200;
    queue(answer, 108);
    media.ready = true;
    ASSERT_TRUE(client.media_connected(109));
    queue(acknowledgement(mqtt.sent.back()), 110);
    auto established =
        event("sessionEstablished", R"({"state":"established","mediaRevision":1})", "established");
    established.request = false;
    established.event_sequence = 2;
    queue(established, 111);
    ASSERT_TRUE(client.active());
    ASSERT_TRUE(client.prepare_endpoint_source("device:p:next", 112, 1000000));
    const auto receipt = *client.endpoint_source_receipt();
    auto ack =
        event("endpointState",
              "{\"state\":\"preparing\",\"revision\":1,\"handoffId\":\"" + receipt.handoff + "\"}",
              receipt.token);
    ack.request = false;
    ack.code = 200;
    queue(ack, 113);
    ASSERT_EQ(client.endpoint_source_receipt()->revision, 1U);
    auto ready = event("endpointState",
                       "{\"state\":\"ready\",\"revision\":2,\"handoffId\":\"" + receipt.handoff +
                           "\",\"candidateSessionId\":\"candidate\"}",
                       "ready");
    ready.request = false;
    ready.event_sequence = 4;
    const auto closes = media.closes;
    queue(ready, 114);
    EXPECT_EQ(client.endpoint_source_receipt()->revision, 1U);
    EXPECT_EQ(media.closes, closes);
    EXPECT_TRUE(client.active());
    ready.event_sequence = 3;
    queue(ready, 115);
    EXPECT_EQ(client.endpoint_source_receipt()->revision, 2U);
    EXPECT_TRUE(client.commit_endpoint_source(116, 1000000));
}

TEST(HumanCallVoice, IndependentCapabilityDoesNotAdvertiseCameraOrVideo) {
    VoiceMqtt mqtt;
    VoiceMedia media;
    HumanCallClient client(mqtt, media, "p/d", "voice", voice_config(false));
    ASSERT_TRUE(client.start(MqttConfig{}, 100));
    mqtt.events.push_back({MqttEvent::Kind::Connected, 0, 0, {}, {}});
    client.poll(101, 1000000);
    mqtt.events.push_back({MqttEvent::Kind::Subscribed, 7, 0, {}, {}});
    client.poll(102, 1000000);
    ASSERT_EQ(mqtt.sent.size(), 1U);
    const auto& payload = mqtt.sent.back().payload;  // 读取真实序列化，不拷贝能力文案来证明输出。
    EXPECT_EQ(payload.find("video"), std::string::npos);
    EXPECT_EQ(payload.find("cameras"), std::string::npos);
    EXPECT_EQ(payload.find("H264"), std::string::npos);
    EXPECT_NE(payload.find("48000"), std::string::npos);
    EXPECT_NE(payload.find("localAccept"), std::string::npos);
}

TEST(HumanCallVoice, DialEncodesOnlyVoiceTrack) {
    VoiceMqtt mqtt;
    VoiceMedia media;
    HumanCallClient client(mqtt, media, "p/d", "voice", voice_config());
    ASSERT_TRUE(client.start_shared(100));
    ASSERT_TRUE(client.dial("p:remote", 101));
    ASSERT_EQ(mqtt.sent.size(), 1U);
    const auto& payload =
        mqtt.sent.back().payload;  // 本端创建仍由平台授权，JSON不能夹带设备自报用户身份。
    EXPECT_NE(payload.find("\"callType\":\"voice\""), std::string::npos);
    EXPECT_NE(payload.find("\"sourceType\":\"microphone\""), std::string::npos);
    EXPECT_EQ(payload.find("video-1"), std::string::npos);
    EXPECT_EQ(payload.find("mic-1"), std::string::npos);
}

// created 从实际Invite轨字段派生主叫应答，保留不同用途的严格根字段合同。
Envelope created(const Envelope& request) {
    auto value = mutate(invitation(), [](cJSON* root) {
        for (const auto* key : {"purpose", "callType", "admission", "transport", "expiresAt"})
            cJSON_DeleteItemFromObjectCaseSensitive(root, key);
        cJSON_AddStringToObject(root, "state", "inviting");
        auto* selected = cJSON_AddObjectToObject(root, "selected");
        cJSON_AddStringToObject(selected, "transport", "webrtc");
    });
    value.method = "sessionCreated";
    value.token = request.token;
    value.request = false;
    value.code = 200;
    value.event_sequence.reset();
    return value;
}
TEST(HumanCallVoice, DialCreationReplaysDoNotCreateAnotherCandidateAndRejectionReleases) {
    for (const bool rejected : {false, true}) {
        VoiceMqtt mqtt;
        VoiceMedia media;
        HumanCallClient client(mqtt, media, "p/d", "voice", voice_config());
        ASSERT_TRUE(client.start_shared(100));
        ASSERT_TRUE(client.dial("p:remote", 101));
        EXPECT_FALSE(client.dial("p:another", 102));
        EXPECT_EQ(media.prepares, 1);
        const auto closes = media.closes;  // 创建失败须释放本次预约，不能遗留Busy。
        auto response = created(mqtt.sent.back());
        if (rejected) {
            response.code = 403;
            response.session.clear();
            response.payload = R"({"reason":"denied"})";
        }
        ASSERT_TRUE(receive(client, response, 103));
        ASSERT_TRUE(receive(client, response, 104));
        EXPECT_EQ(media.old_begins, 0);
        if (rejected) {
            EXPECT_EQ(client.state(), HumanCallState::Available);
            EXPECT_GT(media.closes, closes);
            EXPECT_EQ(media.candidates, 0);
        } else {
            EXPECT_EQ(client.state(), HumanCallState::Negotiating);
            EXPECT_EQ(media.candidates, 1);
            EXPECT_EQ(client.session_id(), "voice-leg");
        }
    }
}

// 退休查询复用原已确认Failed创建账本；未知、当前、成功或格式非法的应答不能获旧拒绝资格。
TEST(HumanCallVoice, RetiredCreationRejectionRequiresOriginalFailedRequest) {
    VoiceMqtt mqtt;
    VoiceMedia media;
    HumanCallClient client(mqtt, media, "p/d", "voice", voice_config());
    ASSERT_TRUE(client.start_shared(100));
    ASSERT_TRUE(client.dial("p:remote", 101));
    auto rejected = created(mqtt.sent.back());
    rejected.code = 403;
    rejected.session.clear();
    rejected.payload = R"({"reason":"denied"})";
    EXPECT_FALSE(client.retired_shared_create_rejection(rejected));
    ASSERT_TRUE(receive(client, rejected, 102));
    EXPECT_TRUE(client.retired_shared_create_rejection(rejected));
    ASSERT_TRUE(client.dial("p:next", 103));
    EXPECT_TRUE(client.retired_shared_create_rejection(rejected));
    auto current = rejected;
    current.token = mqtt.sent.back().token;
    EXPECT_FALSE(client.retired_shared_create_rejection(current));
    auto unknown = rejected;
    unknown.token = "unknown";
    EXPECT_FALSE(client.retired_shared_create_rejection(unknown));
    for (const int code : {199, 200, 600}) {
        auto invalid = rejected;
        invalid.code = code;
        EXPECT_FALSE(client.retired_shared_create_rejection(invalid));
    }
    auto invalid = rejected;
    invalid.code.reset();
    EXPECT_FALSE(client.retired_shared_create_rejection(invalid));
    invalid = rejected;
    invalid.request = true;
    EXPECT_FALSE(client.retired_shared_create_rejection(invalid));
    invalid = rejected;
    invalid.session = "foreign";
    EXPECT_FALSE(client.retired_shared_create_rejection(invalid));
    invalid = rejected;
    invalid.method = "requestAck";
    EXPECT_FALSE(client.retired_shared_create_rejection(invalid));
    invalid = rejected;
    invalid.event_sequence = 1;
    EXPECT_FALSE(client.retired_shared_create_rejection(invalid));
    invalid = rejected;
    invalid.message_sequence = 1;
    EXPECT_FALSE(client.retired_shared_create_rejection(invalid));
    EXPECT_FALSE(receive(client, unknown, 104));
    EXPECT_EQ(client.state(), HumanCallState::Failed); // 未经Router筛选的未知应答仍按原Core失败语义处理。
}
TEST(HumanCallVoice, InvalidSDPAndWrongAssociationCannotAuthorizeCandidate) {
    for (const int invalid : {0, 1, 2, 3}) {
        VoiceMqtt mqtt;
        VoiceMedia media;
        HumanCallClient client(mqtt, media, "p/d", "voice", voice_config());
        ASSERT_NO_FATAL_FAILURE(negotiating(client, mqtt));
        EXPECT_FALSE(client.offer_ready("not-sdp", 104));
        ASSERT_TRUE(client.offer_ready("v=0\r\ns=voice\r\n", 104));
        auto answer = event(
            "mediaAnswer",
            R"({"mediaRevision":1,"webrtc":{"iceMode":"complete","sdp":"v=0\r\ns=answer\r\n"}})",
            mqtt.sent.back().token);
        answer.request = false;
        answer.code = 200;
        if (invalid == 0)
            answer =
                mutate(answer, [](cJSON* root) { cJSON_AddBoolToObject(root, "unknown", true); });
        if (invalid == 1)
            answer = mutate(answer, [](cJSON* root) {
                cJSON_ReplaceItemInObjectCaseSensitive(root, "webrtc", cJSON_CreateNull());
            });
        if (invalid == 2)
            answer.token = "wrong-token";
        if (invalid == 3)
            answer.session = "old-leg";
        EXPECT_EQ(receive(client, answer, 105), invalid == 3);  // 迟到其他腿被隔离，不破坏原协商。
        EXPECT_EQ(media.answers, 0);
        EXPECT_FALSE(client.active());
        EXPECT_EQ(media.candidates, 1);
        EXPECT_EQ(media.old_begins, 0);
    }
}

TEST(HumanCallVoice, BothEstablishmentOrdersKeepCandidateOnlyAndDuplicatesIdempotent) {
    for (const bool established_first : {false, true}) {
        VoiceMqtt mqtt;
        VoiceMedia media;
        HumanCallClient client(mqtt, media, "p/d", "voice", voice_config());
        ASSERT_NO_FATAL_FAILURE(connected(client, mqtt, media, established_first));
        EXPECT_EQ(media.prepares, 1);
        EXPECT_EQ(media.candidates, 1);
        EXPECT_EQ(media.answers, 1);
        const auto size = mqtt.sent.size();
        EXPECT_FALSE(client.accept(110));
        EXPECT_FALSE(client.offer_ready("v=0\r\ns=late\r\n", 110));
        EXPECT_FALSE(client.media_connected(110));
        EXPECT_EQ(mqtt.sent.size(), size);
        EXPECT_TRUE(receive(client, invitation(), 110));
        EXPECT_EQ(media.prepares, 1);
        EXPECT_EQ(media.candidates, 1);
        EXPECT_FALSE(client.control_track("video-1", true, 111));
        EXPECT_FALSE(client.control_track("mic-1", true, 111));
        ASSERT_TRUE(client.control_track("voice", true, 111));
        EXPECT_NE(mqtt.sent.back().payload.find("\"trackId\":\"voice\""), std::string::npos);
    }
}

TEST(HumanCallVoice, InvitationRequiresExactModeTrackAndNoUnknownFields) {
    const std::vector<std::function<void(cJSON*)>> changes = {
        [](cJSON* root) {
            cJSON_ReplaceItemInObjectCaseSensitive(root, "callType", cJSON_CreateString("video"));
        },
        [](cJSON* root) { cJSON_AddStringToObject(root, "unexpected", "value"); },
        [](cJSON* root) {
            cJSON_ReplaceItemInObjectCaseSensitive(root, "tracks", cJSON_CreateNull());
        },
        [](cJSON* root) {
            cJSON_DeleteItemFromArray(cJSON_GetObjectItemCaseSensitive(root, "tracks"), 0);
        },
        [](cJSON* root) {
            auto* tracks = cJSON_GetObjectItemCaseSensitive(root, "tracks");
            cJSON_AddItemToArray(tracks, cJSON_Duplicate(cJSON_GetArrayItem(tracks, 0), true));
        },
        [](cJSON* root) {
            cJSON_AddStringToObject(
                cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(root, "tracks"), 0), "unknown",
                "field");
        },
        [](cJSON* root) {
            cJSON_DeleteItemFromObjectCaseSensitive(
                cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(root, "tracks"), 0),
                "sourceType");
        },
        [](cJSON* root) {
            cJSON_ReplaceItemInObjectCaseSensitive(
                cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(root, "tracks"), 0),
                "sourceType", cJSON_CreateNull());
        }};
    for (const auto& change : changes) {
        VoiceMqtt mqtt;
        VoiceMedia media;
        HumanCallClient client(mqtt, media, "p/d", "voice", voice_config());
        ASSERT_TRUE(client.start_shared(100));
        EXPECT_FALSE(receive(client, mutate(invitation(), change)));
        EXPECT_EQ(media.prepares, 0);
        EXPECT_EQ(media.candidates, 0);
        EXPECT_FALSE(client.active());
    }
    for (const auto& entry :
         std::vector<std::pair<const char*, const char*>>{{"trackId", "mic-1"},
                                                          {"kind", "video"},
                                                          {"sourceId", "speaker"},
                                                          {"sourceType", "camera"},
                                                          {"direction", "sendonly"},
                                                          {"codec", "H264"},
                                                          {"state", "active"}}) {
        VoiceMqtt mqtt;
        VoiceMedia media;
        HumanCallClient client(mqtt, media, "p/d", "voice", voice_config());
        ASSERT_TRUE(client.start_shared(100));
        auto invalid = mutate(invitation(), [&](cJSON* root) {
            cJSON_ReplaceItemInObjectCaseSensitive(
                cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(root, "tracks"), 0),
                entry.first, cJSON_CreateString(entry.second));
        });
        EXPECT_FALSE(receive(client, invalid));
        EXPECT_EQ(media.prepares, 0);
        EXPECT_EQ(media.candidates, 0);
    }
}

TEST(HumanCallVoice, RejectTimeoutAndLateInviteNeverOpenCandidate) {
    for (const bool timeout : {false, true}) {
        VoiceMqtt mqtt;
        VoiceMedia media;
        HumanCallClient client(mqtt, media, "p/d", "voice", voice_config());
        ASSERT_NO_FATAL_FAILURE(invited(client));
        if (timeout) {
            client.poll(110, 1001000);
            EXPECT_EQ(client.state(), HumanCallState::Available);
            EXPECT_EQ(client.failure_facts().failure, HumanCallFailure::None);
            EXPECT_FALSE(client.accept(110));
        } else {
            ASSERT_TRUE(client.reject(102));
            EXPECT_EQ(mqtt.sent.back().method, "sessionReject");
            EXPECT_TRUE(receive(client, invitation(), 103));
            EXPECT_FALSE(client.accept(104));
        }
        EXPECT_EQ(media.prepares, 0);
        EXPECT_EQ(media.candidates, 0);
        EXPECT_EQ(media.old_begins, 0);
        client.close();
        EXPECT_FALSE(receive(client, invitation(), 120));
        EXPECT_EQ(mqtt.disconnects, 0);
    }
}

TEST(HumanCallVoice, UnsupportedAndFailedDriversNeverBorrowVideoBegin) {
    for (const int failure : {0, 1, 2, 3}) {
        VoiceMqtt mqtt;
        VoiceMedia media;
        auto config = voice_config();
        config.camera_id.clear();
        HumanCallClient client(mqtt, media, "p/d", "voice", config);
        if (failure == 0) {
            media.supported = false;
            EXPECT_FALSE(client.start_shared(100));
        } else {
            ASSERT_NO_FATAL_FAILURE(invited(client));
            if (failure == 1)
                media.reserve_ok = false;
            if (failure == 2)
                media.begin_ok = false;
            if (failure == 3)
                media.supported = false;  // 驱动能力失效不能由旧prepare的true兜底。
            ASSERT_TRUE(client.accept(102));
            if (failure == 1 || failure == 3) {
                EXPECT_EQ(mqtt.sent.back().method, "sessionReject");
                EXPECT_EQ(media.candidates, 0);
            } else {
                EXPECT_FALSE(receive(client, acknowledgement(mqtt.sent.back()), 103));
            }
        }
        EXPECT_EQ(media.old_prepares, 0);
        EXPECT_EQ(media.old_begins, 0);
        EXPECT_FALSE(client.active());
    }
}

TEST(HumanCallVoice, PhaseTimeoutAndEndReleaseOnlyThisBorrowedLeg) {
    for (const bool timeout : {false, true}) {
        VoiceMqtt mqtt;
        VoiceMedia media;
        HumanCallClient client(mqtt, media, "p/d", "voice", voice_config());
        ASSERT_NO_FATAL_FAILURE(negotiating(client, mqtt));
        const auto closes = media.closes;
        if (timeout) {
            client.poll(30103, 1001001);
            EXPECT_EQ(client.state(), HumanCallState::Failed);
            EXPECT_EQ(client.failure_facts().failure, HumanCallFailure::PhaseDeadline);
        } else {
            ASSERT_TRUE(client.end(104));
            EXPECT_EQ(client.state(), HumanCallState::Ending);
            auto ended = event("sessionEnded", R"({"reason":"hangup"})", mqtt.sent.back().token);
            ended.request = false;
            ended.code = 200;
            ASSERT_TRUE(receive(client, ended, 105));
            EXPECT_EQ(client.state(), HumanCallState::Available);
            EXPECT_TRUE(client.session_id().empty());
        }
        EXPECT_GT(media.closes, closes);
        EXPECT_EQ(mqtt.disconnects, 0);
        EXPECT_EQ(media.candidates, 1);
        EXPECT_EQ(media.old_begins, 0);
        EXPECT_FALSE(client.active());
        auto late = acknowledgement(mqtt.sent[0]);
        late.session = "old-leg";
        // GoogleTest断言展开含内部else；显式块保证GCC严格警告和实际控制分支一致。
        if (!timeout) {
            EXPECT_TRUE(receive(client, late, 106));  // 已结束旧腿不能杀当前可用宿主。
        }
        EXPECT_EQ(media.candidates, 1);
    }
}

// LegacyMedia 不实现任何新virtual，用于证明默认视频仍可走原接口而不被voice支持门禁破坏。
struct LegacyMedia : CallMediaPort {
    int prepares = 0, begins = 0;  // 只记录原接口调用，不声称真实硬件资源。
    bool prepare() override {
        ++prepares;
        return true;
    }
    bool begin() override {
        ++begins;
        return true;
    }
    bool apply_answer(std::string_view) override { return true; }
    bool connected() const override { return false; }
    void close() override {}
};
// DeclaredVoiceMedia 只声明能力但不实现新端口，缺省拒绝必须真实生效而不是借旧video成功。
struct DeclaredVoiceMedia : LegacyMedia {
    bool voice_supported() const override { return true; }
};
// ReservedVoiceMedia 只实现预约，候选begin仍继承默认false；预约成功不是Peer已建立。
struct ReservedVoiceMedia final : DeclaredVoiceMedia {
    bool prepare_voice(const CallVoiceProfile&) override { return true; }
};
TEST(HumanCallVoice, MissingVoicePortImplementationsRemainExplicitlyUnsupported) {
    for (const bool reserved : {false, true}) {
        VoiceMqtt mqtt;
        std::unique_ptr<LegacyMedia> media =
            reserved ? std::unique_ptr<LegacyMedia>(new ReservedVoiceMedia)
                     : std::unique_ptr<LegacyMedia>(new DeclaredVoiceMedia);
        HumanCallClient client(mqtt, *media, "p/d", "partial", voice_config());
        ASSERT_NO_FATAL_FAILURE(invited(client));
        ASSERT_TRUE(client.accept(102));
        if (reserved) {
            EXPECT_EQ(mqtt.sent.back().method, "sessionAccept");
            EXPECT_FALSE(receive(client, acknowledgement(mqtt.sent.back()), 103));
        } else
            EXPECT_EQ(mqtt.sent.back().method, "sessionReject");
        EXPECT_EQ(media->prepares, 0);
        EXPECT_EQ(media->begins, 0);
        EXPECT_FALSE(client.active());
    }
}
TEST(HumanCallVoice, DefaultVideoKeepsOldPortAndRejectsVoiceSourceType) {
    VoiceMqtt mqtt;
    LegacyMedia media;
    HumanCallConfig config;
    config.borrowed_connection = true;
    HumanCallClient client(mqtt, media, "p/d", "video", config);
    ASSERT_TRUE(client.start_shared(100));
    ASSERT_TRUE(client.dial("p:remote", 101));
    EXPECT_EQ(media.prepares, 1);  // 默认视频主动拨号也预约原媒体端口。
    EXPECT_NE(mqtt.sent.back().payload.find("\"callType\":\"video\""), std::string::npos);
    EXPECT_EQ(mqtt.sent.back().payload.find("sourceType"), std::string::npos);
    client.close();
    ASSERT_TRUE(client.start_shared(102));
    EXPECT_FALSE(receive(client, invitation(), 103));
    EXPECT_EQ(media.prepares, 1);  // 非法语音邀请不得新增预约。
    EXPECT_EQ(media.begins, 0);
    auto unsupported = voice_config();
    HumanCallClient voice(mqtt, media, "p/d", "unsupported", unsupported);
    EXPECT_FALSE(voice.start_shared(104));
    EXPECT_FALSE(voice.start(MqttConfig{}, 104));
}
// video_invitation 冻结旧front／三轨邀请，旧默认用途不接受语音新增sourceType字段。
Envelope video_invitation() {
    auto value = event(
        "sessionInvite",
        R"({"purpose":"humanCall","callType":"video","admission":"accept","transport":"webrtc","expiresAt":1001000,"callGroupId":"video-group","tracks":[{"trackId":"mic-1","kind":"audio","sourceId":"mic","direction":"sendonly","codec":"opus","state":"requested"},{"trackId":"speaker-1","kind":"audio","sourceId":"speaker","direction":"recvonly","codec":"opus","state":"requested"},{"trackId":"video-1","kind":"video","sourceId":"front","direction":"sendonly","codec":"H264","state":"requested"}]})");
    value.event_sequence = 1;
    return value;
}
TEST(HumanCallVoice, LegacyVideoPositiveAndSourceTypeNegativeRemainStrict) {
    for (const bool extra_source_type : {false, true}) {
        VoiceMqtt mqtt;
        LegacyMedia media;
        HumanCallConfig config;
        config.borrowed_connection = true;
        HumanCallClient client(mqtt, media, "p/d", "legacy", config);
        ASSERT_TRUE(client.start_shared(100));
        auto invite = video_invitation();
        if (extra_source_type)
            invite = mutate(invite, [](cJSON* root) {
                cJSON_AddStringToObject(
                    cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(root, "tracks"), 0),
                    "sourceType", "microphone");
            });
        EXPECT_EQ(receive(client, invite), !extra_source_type);
        if (!extra_source_type) {
            ASSERT_TRUE(client.accept(102));
            EXPECT_EQ(media.prepares, 1);
            EXPECT_EQ(mqtt.sent.back().payload,
                      R"({"acceptedTracks":["mic-1","speaker-1","video-1"]})");
            ASSERT_TRUE(receive(client, acknowledgement(mqtt.sent.back()), 103));
            EXPECT_EQ(media.begins, 1);
        } else {
            EXPECT_EQ(media.prepares, 0);
            EXPECT_EQ(media.begins, 0);
        }
    }
}
// MixedVoiceMedia
// 显式装配UDP选路，仍用原voice候选端口记录相机路径是否被误调用。
struct MixedVoiceMedia : VoiceMedia {
  bool udp_selected = false;
  bool configure_udp_av(bool enabled) override {
    udp_selected = enabled;
    return true;
  }
};

// 单协议固件可以接语音和视频，但纯语音只走voice候选且接受轨不包含摄像头。
TEST(HumanCallVoice, MixedHostAcceptsAudioOnlyAndFencesOtherTransport) {
  for (bool udp : {false, true}) {
    VoiceMqtt mqtt;
    MixedVoiceMedia media;
    HumanCallConfig config;
    config.borrowed_connection = true;
    config.voice_and_video = true;
    config.udp_av = udp;
    config.udp_only = udp;
    HumanCallClient client(mqtt, media, "p/d", "mixed-call", config);
    ASSERT_TRUE(client.start_shared(100));
    auto invite = invitation();
    if (udp) {
      invite = mutate(invite, [](cJSON *root) {
        cJSON_SetValuestring(cJSON_GetObjectItem(root, "transport"), "udpAv");
      });
    }
    ASSERT_TRUE(receive(client, invite));
    EXPECT_EQ(client.mode(), HumanCallMode::Voice); // 响铃时即可取得已校验类型，不等接听资源预约。
    EXPECT_EQ(media.prepares, 0);
    ASSERT_TRUE(client.accept(102));
    EXPECT_EQ(media.prepares, 1);
    EXPECT_EQ(media.old_prepares, 0);
    const auto accept = mqtt.sent.back();
    EXPECT_EQ(accept.payload, R"({"acceptedTracks":["voice"]})");
    ASSERT_TRUE(receive(client, acknowledgement(accept), 103));
    EXPECT_EQ(media.candidates, 1);
    EXPECT_EQ(media.old_begins, 0);
    EXPECT_EQ(media.udp_selected, udp);
    ASSERT_TRUE(client.offer_ready(udp ? "" : "v=0\r\ns=voice\r\n", 104));
    EXPECT_EQ(mqtt.sent.back().payload.find("udpAv") != std::string::npos, udp);
    if (udp) {
      VoiceMqtt rejected_mqtt;
      MixedVoiceMedia rejected_media;
      HumanCallClient rejected(rejected_mqtt, rejected_media, "p/d",
                               "wrong-protocol", config);
      ASSERT_TRUE(rejected.start_shared(100));
      EXPECT_FALSE(receive(rejected, invitation()));
      EXPECT_EQ(rejected.mode(), HumanCallMode::Video); // 被拒邀请不得改写UI类型。
      EXPECT_EQ(rejected_media.prepares, 0);
    }
  }
}
}  // namespace
}  // namespace ur::device
