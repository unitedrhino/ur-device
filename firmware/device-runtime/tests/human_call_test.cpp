// 文件说明：可控时钟与假媒体验证人际视频邀请、接听、握手、挂断和越权报文。
#include "ur/device/human_call.hpp"
#include <gtest/gtest.h>
#include <deque>
#include "ur/device/envelope.hpp"

namespace ur::device {
// HumanCallStaticFaultTest 只为本TU直接注入私有缺水位消息并读取水位，不授平台或媒体权限。
class HumanCallStaticFaultTest {
public:
    // receive_transfer 绕过外层解码以验证私有方法自己的失败围栏，返回原方法结果。
    static bool receive_transfer(HumanCallClient& client, const Envelope& message,
                                 std::uint64_t now, bool& handled) {
        return client.receive_transfer_state(message, now, handled);
    }
    // sequence 返回故障前后原信令水位，仅读状态，不推进准入。
    static std::uint64_t sequence(const HumanCallClient& client) { return client.event_seq_; }
};
namespace {
// TestCallMqtt 记录精确Topic和已解析信封，网络确认须由测试显式注入。
struct TestCallMqtt final : MqttPort {
    std::deque<MqttEvent> events;
    std::vector<Envelope> sent;
    std::vector<std::string> topics;
    std::string subscription;
    bool online = true;
    int closed = 0;
    bool publish_ok = true;  // 显式发布失败夹具，不从本地受理推断Broker交付。
    bool connect(const MqttConfig&) override { return true; }
    int subscribe(std::string_view topic) override {
        subscription = topic;
        return 7;
    }
    bool publish(std::string_view topic, std::string_view body) override {
        if (!publish_ok)
            return false;
        Envelope decoded;
        if (decode_envelope(body, decoded) != DecodeResult::Ok)
            return false;
        sent.push_back(std::move(decoded));
        topics.emplace_back(topic);
        return true;
    }
    bool poll() override { return online; }
    std::optional<MqttEvent> take_event() override {
        if (events.empty())
            return {};
        auto value = std::move(events.front());
        events.pop_front();
        return value;
    }
    void disconnect() override {
        ++closed;
        events.clear();
    }
    void down(const Envelope& value, std::string topic = "$thing/down/signaling/p/d") {
        std::string body;
        EXPECT_EQ(encode_envelope(value, body), DecodeResult::Ok);
        events.push_back({MqttEvent::Kind::Message, 0, 0, std::move(topic), std::move(body)});
    }
};
// TestCallMedia 只确认实际驱动事件，不从offer/answer自动推断连通。
struct TestCallMedia final : CallMediaPort {
    bool started = false, ready = false, answered = false;
    bool reservation_required = false, prepared = false;  // 模拟Watcher在begin前必须预约硬件。
    bool prepare_ok = true;  // 模拟相机或共享媒体资源不可用。
    bool prepare() override {
        prepared = prepare_ok;
        return prepared;
    }
    bool udp_capable = false, udp_selected = false; // 显式UDP驱动夹具，旧用例仍默认拒绝UDP。
    std::optional<CallUDPAVParameters> udp_answer; // 只保存核心核验后的强类型参数。
    bool configure_udp_av(bool enabled) override {
        udp_selected = enabled;
        return !enabled || udp_capable;
    }
    bool apply_udp_av_answer(const CallUDPAVParameters& params) override {
        if (!udp_selected) return false;
        udp_answer = params;
        return true;
    }

    int closes = 0;
    int begins = 0,
        answers = 0;  // 精确检测转接事件是否重建原媒体端口，不能只看仍为true的started。
    bool hold_capable = true, hold_success = true, held = false;
    int hold_calls = 0;
    bool async_supported = false;       // 默认测试旧同步端口，不将异步默认拒绝当失败。
    std::optional<bool> async_applied;  // 显式实际回执前保持null，不从排队造true。
    std::uint64_t async_ticket = 0, async_revision = 0;
    std::string async_token;
    bool async_target = false;
    bool receive_capable = false, receive_configured = false, receive_config_ok = true;
    int receive_config_calls = 0;  // 验证接收权限只由已选轨落到HAL，而不是广告能力自动开启。
    bool bind_scope_ok = true;
    std::string bound_scope;  // 原创建应答身份须在HAL开始采集前落定，失败不能绕过共享资源租约。
    bool bind_session_scope(std::string_view session) override {
        bound_scope = session;
        return bind_scope_ok;
    }
    bool video_receive_supported() const override { return receive_capable; }
    bool configure_video_receive(bool enabled) override {
        ++receive_config_calls;
        receive_configured = enabled;
        return receive_config_ok && (!enabled || receive_capable);
    }
    bool hold_async_supported() const override { return async_supported; }
    std::optional<std::uint64_t> request_hold(bool target, std::string_view token,
                                              std::uint64_t revision, std::uint64_t) override {
        async_target = target;
        async_token = token;
        async_revision = revision;
        async_applied.reset();
        return ++async_ticket;
    }
    std::optional<bool> hold_result(std::uint64_t ticket, bool target) const override {
        if (ticket != async_ticket || target != async_target)
            return false;
        return async_applied;
    }
    bool hold_supported() const override { return hold_capable; }
    bool set_held(bool value) override {
        ++hold_calls;
        if (!hold_success)
            return false;
        held = value;
        return true;
    }
    bool begin() override {
        ++begins;
        if (reservation_required && !prepared) return false;
        started = true;
        return true;
    }
    bool apply_answer(std::string_view sdp) override {
        ++answers;
        answered = sdp.substr(0, 5) == "v=0\r\n";
        return answered;
    }
    bool connected() const override { return ready; }
    void close() override {
        ++closes;
        started = ready = answered = false;
        prepared = false;
    }
};
// reply 构造云端同token应答；事件水位与请求身份分开。
Envelope reply(std::string token, std::string method, std::string session, std::string data) {
    Envelope value;
    value.request = false;
    value.token = std::move(token);
    value.method = std::move(method);
    value.session = std::move(session);
    value.code = 200;
    value.payload = std::move(data);
    return value;
}
// ready_client 走真实连接/订阅/能力ACK，不能跳过Broker订阅门禁。
void ready_client(HumanCallClient& client, TestCallMqtt& mqtt) {
    ASSERT_TRUE(client.start(MqttConfig{}, 100));
    mqtt.events.push_back({MqttEvent::Kind::Connected, 0, 0, {}, {}});
    client.poll(101, 1'000'000);
    ASSERT_EQ(client.state(), HumanCallState::Subscribing);
    mqtt.events.push_back({MqttEvent::Kind::Subscribed, 7, 0, {}, {}});
    client.poll(102, 1'000'000);
    ASSERT_EQ(mqtt.sent.size(), 1);
    EXPECT_EQ(mqtt.sent[0].method, "capabilityReport");
    mqtt.down(reply(mqtt.sent[0].token, "requestAck", "",
                    R"({"requestMethod":"capabilityReport","result":{"acceptedRevision":1}})"));
    client.poll(103, 1'000'000);
    ASSERT_EQ(client.state(), HumanCallState::Available);
}
// reporting_client 用原连接与SUBACK进入Reporting，故障注入不跳过能力上报。
void reporting_client(HumanCallClient& client, TestCallMqtt& mqtt) {
    ASSERT_TRUE(client.start(MqttConfig{}, 100));
    mqtt.events.push_back({MqttEvent::Kind::Connected, 0, 0, {}, {}});
    client.poll(101, 1'000'000);
    mqtt.events.push_back({MqttEvent::Kind::Subscribed, 7, 0, {}, {}});
    client.poll(102, 1'000'000);
    ASSERT_EQ(client.state(), HumanCallState::Reporting);
}
// capability_conflict 仅构造原能力请求的明确冲突，不猜测服务端CAS具体原因。
Envelope capability_conflict(const std::string& token) {
    auto denied = reply(token, "requestAck", "",
                        R"({"requestMethod":"capabilityReport","reason":"capabilityNotAccepted"})");
    denied.code = 409;
    return denied;
}
// 有限恢复消费原失败ACK，同连接同能力新token；旧ACK不能杀新请求或制造成功。
TEST(HumanCallCapabilityRecovery, BoundedBackoffAndStaleAcknowledgements) {
    TestCallMqtt mqtt;
    TestCallMedia media;
    HumanCallClient client(mqtt, media, "p/d", "cap-recovery");
    ASSERT_NO_FATAL_FAILURE(reporting_client(client, mqtt));
    const auto original = mqtt.sent.back();  // 原能力正文及关联身份，恢复不改修订或权限。
    auto now = std::uint64_t{103};
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        const auto request = mqtt.sent.back();
        const auto denied = capability_conflict(request.token);
        mqtt.down(denied);
        client.poll(now, 1'000'000);
        if (attempt == 2) {
            EXPECT_EQ(client.state(), HumanCallState::Failed);
            EXPECT_EQ(client.failure_facts().failure, HumanCallFailure::ReceiveRejected);
            EXPECT_EQ(client.failure_facts().capability_ack_code, 409);
            EXPECT_EQ(client.failure_facts().capability_reason,
                      HumanCapabilityRejection::CapabilityNotAccepted);
            break;
        }
        ASSERT_EQ(client.state(), HumanCallState::Reporting);
        EXPECT_EQ(client.failure_facts().failure, HumanCallFailure::None);
        EXPECT_EQ(client.failure_facts().capability_ack_code, 0);
        const auto count = mqtt.sent.size();
        mqtt.down(denied);
        client.poll(now + 1, 1'000'000);
        mqtt.down(reply(request.token, "requestAck", "",
                        R"({"requestMethod":"capabilityReport","result":{"acceptedRevision":1}})"));
        client.poll(now + 2, 1'000'000);
        ASSERT_EQ(client.state(), HumanCallState::Reporting);
        client.poll(now + 499, 1'000'000);
        EXPECT_EQ(mqtt.sent.size(), count);
        client.poll(now + 500, 1'000'000);
        ASSERT_EQ(mqtt.sent.size(), count + 1);
        EXPECT_EQ(mqtt.sent.back().payload, original.payload);
        EXPECT_NE(mqtt.sent.back().token, request.token);
        mqtt.down(reply(request.token, "requestAck", "",
                        R"({"requestMethod":"capabilityReport","result":{"acceptedRevision":1}})"));
        client.poll(now + 501, 1'000'000);
        ASSERT_EQ(client.state(), HumanCallState::Reporting);
        EXPECT_EQ(mqtt.sent.size(), count + 1);
        EXPECT_FALSE(media.started);
        now += 502;
    }
}
// 新token确认才能Available；恢复中的历史拒绝不得成为后来网络失败的诊断理由。
TEST(HumanCallCapabilityRecovery, SuccessAndAbortDoNotResurrect) {
    for (const int finish : {0, 1, 2, 3}) {
        TestCallMqtt mqtt;
        TestCallMedia media;
        HumanCallClient client(mqtt, media, "p/d", "cap-recovery");
        ASSERT_NO_FATAL_FAILURE(reporting_client(client, mqtt));
        mqtt.down(capability_conflict(mqtt.sent.back().token));
        client.poll(103, 1'000'000);
        ASSERT_EQ(client.state(), HumanCallState::Reporting);
        if (finish == 0) {
            client.poll(603, 1'000'000);
            mqtt.down(
                reply(mqtt.sent.back().token, "requestAck", "",
                      R"({"requestMethod":"capabilityReport","result":{"acceptedRevision":1}})"));
            client.poll(604, 1'000'000);
            EXPECT_EQ(client.state(), HumanCallState::Available);
        } else if (finish == 1) {
            client.close();
        } else if (finish == 2) {
            mqtt.online = false;
            client.poll(104, 1'000'000);
            EXPECT_EQ(client.failure_facts().failure, HumanCallFailure::NetworkPoll);
        } else {
            client.poll(200, 1'000'000);
            client.poll(199, 1'000'000);
            EXPECT_EQ(client.failure_facts().failure, HumanCallFailure::ClockRollback);
        }
        const auto count = mqtt.sent.size();
        client.poll(1'000, 1'000'000);
        EXPECT_EQ(mqtt.sent.size(), count);
        EXPECT_EQ(client.failure_facts().capability_ack_code, 0);
        EXPECT_FALSE(media.started);
    }
}
// 新请求按实际now计时，但原Reporting绝对30秒不续期，也不能继承旧重传窗口。
TEST(HumanCallCapabilityRecovery, AbsoluteDeadlineAndActualSubmissionTime) {
    TestCallMqtt mqtt;
    TestCallMedia media;
    HumanCallClient client(mqtt, media, "p/d", "cap-recovery");
    ASSERT_NO_FATAL_FAILURE(reporting_client(client, mqtt));
    mqtt.down(capability_conflict(mqtt.sent.back().token));
    client.poll(20'101, 1'000'000);
    ASSERT_EQ(client.state(), HumanCallState::Reporting);
    client.poll(20'601, 1'000'000);
    const auto count = mqtt.sent.size();
    client.poll(20'602, 1'000'000);
    EXPECT_EQ(mqtt.sent.size(), count);
    client.poll(30'101, 1'000'000);
    EXPECT_EQ(client.state(), HumanCallState::Reporting);
    client.poll(30'102, 1'000'000);
    EXPECT_EQ(client.state(), HumanCallState::Failed);
    EXPECT_EQ(client.failure_facts().failure, HumanCallFailure::PhaseDeadline);
    EXPECT_EQ(client.failure_facts().capability_ack_code, 0);
    EXPECT_EQ(mqtt.sent.size(), count);
}
// 能力成功必须确认原revision1；结果缺失、类型错误或其他修订不得提前Available。
TEST(HumanCallCapabilityRecovery, AcceptedRevisionIsExact) {
    for (const auto* result : {R"({})", R"({"acceptedRevision":null})",
                               R"({"acceptedRevision":"1"})", R"({"acceptedRevision":0})",
                               R"({"acceptedRevision":2})", R"({"acceptedRevision":1.5})"}) {
        TestCallMqtt mqtt;
        TestCallMedia media;
        HumanCallClient client(mqtt, media, "p/d", "cap-recovery");
        ASSERT_NO_FATAL_FAILURE(reporting_client(client, mqtt));
        mqtt.down(
            reply(mqtt.sent.back().token, "requestAck", "",
                  std::string(R"({"requestMethod":"capabilityReport","result":)") + result + "}"));
        client.poll(103, 1'000'000);
        EXPECT_EQ(client.state(), HumanCallState::Failed);
        EXPECT_EQ(client.failure_facts().failure, HumanCallFailure::ReceiveRejected);
        EXPECT_FALSE(media.started);
    }
}
// close必须取消本client能力在途请求；下一连接推进旧10秒计时不得重发旧token。
TEST(HumanCallCapabilityRecovery, CloseThenRestartCancelsOldCapability) {
    TestCallMqtt mqtt;
    TestCallMedia media;
    HumanCallClient client(mqtt, media, "p/d", "cap-recovery");
    ASSERT_NO_FATAL_FAILURE(reporting_client(client, mqtt));
    const auto original = mqtt.sent.back().token;
    client.close();
    ASSERT_TRUE(client.start(MqttConfig{}, 20'000));
    mqtt.events.push_back({MqttEvent::Kind::Connected, 0, 0, {}, {}});
    client.poll(20'001, 1'000'000);
    mqtt.events.push_back({MqttEvent::Kind::Subscribed, 7, 0, {}, {}});
    client.poll(20'002, 1'000'000);
    ASSERT_EQ(mqtt.sent.size(), 2);
    EXPECT_NE(mqtt.sent.back().token, original);
}
// 只有固定明确能力冲突可恢复；错误关联不认成功，不改变原其他会话迟到消息的忽略语义。
TEST(HumanCallCapabilityRecovery, UnknownAndMismatchedRejectionsStayFailed) {
    for (const int mismatch : {0, 1, 2, 3, 4, 5}) {
        SCOPED_TRACE(mismatch);  // 仅固定夹具编号，失败定位不输出自由报文。
        TestCallMqtt mqtt;
        TestCallMedia media;
        HumanCallClient client(mqtt, media, "p/d", "cap-recovery");
        ASSERT_NO_FATAL_FAILURE(reporting_client(client, mqtt));
        auto denied = capability_conflict(mqtt.sent.back().token);
        if (mismatch == 0)
            denied.payload = R"({"requestMethod":"capabilityReport","reason":"private-unknown"})";
        else if (mismatch == 1)
            denied.payload =
                R"({"requestMethod":"sessionCreate","reason":"capabilityNotAccepted"})";
        else if (mismatch == 2)
            denied.session = "wrong-session";
        else if (mismatch == 3)
            denied.token = "wrong-token";
        else if (mismatch == 4)
            denied.code = 403;
        mqtt.down(denied,
                  mismatch == 5 ? "$thing/down/signaling/p/other" : "$thing/down/signaling/p/d");
        client.poll(103, 1'000'000);
        if (mismatch == 2) {
            // 原其他session消息被忽略，不能触发恢复或Available；仍服从原30秒截止。
            ASSERT_EQ(client.state(), HumanCallState::Reporting);
            const auto count = mqtt.sent.size();
            client.poll(603, 1'000'000);
            EXPECT_EQ(mqtt.sent.size(), count);
            client.poll(30'102, 1'000'000);
        }
        EXPECT_EQ(client.state(), HumanCallState::Failed);
        const auto count = mqtt.sent.size();
        client.poll(603, 1'000'000);
        EXPECT_EQ(mqtt.sent.size(), count);
        EXPECT_FALSE(media.started);
    }
}
// 迟到冲突不能为恢复续期，未满500ms时达到原30秒必须先失败，不发布新能力。
TEST(HumanCallCapabilityRecovery, DeadlineWinsOverScheduledRetry) {
    TestCallMqtt mqtt;
    TestCallMedia media;
    HumanCallClient client(mqtt, media, "p/d", "cap-recovery");
    ASSERT_NO_FATAL_FAILURE(reporting_client(client, mqtt));
    mqtt.down(capability_conflict(mqtt.sent.back().token));
    client.poll(29'900, 1'000'000);
    ASSERT_EQ(client.state(), HumanCallState::Reporting);
    const auto count = mqtt.sent.size();
    client.poll(30'102, 1'000'000);
    EXPECT_EQ(client.state(), HumanCallState::Failed);
    EXPECT_EQ(client.failure_facts().failure, HumanCallFailure::PhaseDeadline);
    EXPECT_EQ(mqtt.sent.size(), count);
}
// 时钟跳到原Reporting截止时，新请求尚未满自身30秒也不能先重传再失败。
TEST(HumanCallCapabilityRecovery, ReportingDeadlinePrecedesRequestRetransmission) {
    TestCallMqtt mqtt;
    TestCallMedia media;
    HumanCallClient client(mqtt, media, "p/d", "cap-deadline");
    ASSERT_NO_FATAL_FAILURE(reporting_client(client, mqtt));
    mqtt.down(capability_conflict(mqtt.sent.back().token));
    client.poll(103, 1'000'000);
    ASSERT_EQ(client.state(), HumanCallState::Reporting);
    client.poll(603, 1'000'000);
    ASSERT_EQ(mqtt.sent.size(), 2);
    // 不推进中间tick，精确覆盖请求29499ms与原阶段30000ms截止的顺序差异。
    client.poll(30'102, 1'000'000);
    EXPECT_EQ(client.state(), HumanCallState::Failed);
    EXPECT_EQ(client.failure_facts().failure, HumanCallFailure::PhaseDeadline);
    EXPECT_EQ(mqtt.sent.size(), 2);
    EXPECT_FALSE(media.started);
    client.poll(40'000, 1'000'000);
    EXPECT_EQ(mqtt.sent.size(), 2);
}
// 原可控事件分别验证Reporting的失败边界，未知诊断不能改变原失败/关闭行为。
TEST(HumanCallFailureDiagnostic, ReportingPollClockAndDeadline) {
    for (const auto expected : {HumanCallFailure::NetworkPoll, HumanCallFailure::ClockRollback,
                                HumanCallFailure::PhaseDeadline}) {
        TestCallMqtt mqtt;
        TestCallMedia media;
        HumanCallClient client(mqtt, media, "p/d", "diagnostic");
        ASSERT_NO_FATAL_FAILURE(reporting_client(client, mqtt));
        const auto sent = mqtt.sent.size();
        if (expected == HumanCallFailure::NetworkPoll)
            mqtt.online = false;
        client.poll(expected == HumanCallFailure::ClockRollback   ? 101
                    : expected == HumanCallFailure::PhaseDeadline ? 30'102
                                                                  : 103,
                    1'000'000);
        ASSERT_EQ(client.state(), HumanCallState::Failed);
        EXPECT_EQ(client.failure_facts().failure, expected);
        EXPECT_EQ(client.failure_facts().capability_ack_code, 0);
        EXPECT_EQ(mqtt.sent.size(), sent);
        client.close();
        EXPECT_EQ(client.failure_facts().failure, expected);
        mqtt.online = true;
        ASSERT_TRUE(client.start(MqttConfig{}, 40'000));
        EXPECT_EQ(client.failure_facts().failure, HumanCallFailure::None);
    }
}
// 拒绝必须关联原capability请求；私有原因不保留，错身份及成功ACK不得冒非200事实。
TEST(HumanCallFailureDiagnostic, CapabilityRejectedAssociationAndRedaction) {
    for (const auto* reason : {"capabilityNotAccepted", "focusDeviceUnavailable", "requestDenied",
                               "private-secret-text"}) {
        TestCallMqtt mqtt;
        TestCallMedia media;
        HumanCallClient client(mqtt, media, "p/d", "diagnostic");
        ASSERT_NO_FATAL_FAILURE(reporting_client(client, mqtt));
        auto denied = reply(
            mqtt.sent.front().token, "requestAck", "",
            std::string(R"({"requestMethod":"capabilityReport","reason":")") + reason + R"("})");
        denied.code = std::string_view(reason) == "capabilityNotAccepted" ? 409 : 403;
        mqtt.down(denied);
        client.poll(103, 1'000'000);
        if (*denied.code == 409) {
            ASSERT_EQ(client.state(), HumanCallState::Reporting);
            EXPECT_EQ(client.failure_facts().capability_ack_code, 0);
            // 原明确冲突只有耗尽后才是最终拒绝，历史恢复不冒首业务失败。
            for (const auto retry : {603ULL, 1'104ULL}) {
                client.poll(retry, 1'000'000);
                denied.token = mqtt.sent.back().token;
                mqtt.down(denied);
                client.poll(retry + 1, 1'000'000);
            }
        }
        ASSERT_EQ(client.state(), HumanCallState::Failed);
        const auto facts = client.failure_facts();
        EXPECT_EQ(facts.failure, HumanCallFailure::ReceiveRejected);
        EXPECT_EQ(facts.capability_ack_code, *denied.code);
        EXPECT_EQ(facts.capability_reason, std::string_view(reason) == "capabilityNotAccepted"
                                               ? HumanCapabilityRejection::CapabilityNotAccepted
                                           : std::string_view(reason) == "focusDeviceUnavailable"
                                               ? HumanCapabilityRejection::FocusDeviceUnavailable
                                           : std::string_view(reason) == "requestDenied"
                                               ? HumanCapabilityRejection::RequestDenied
                                               : HumanCapabilityRejection::Unknown);
    }
    for (const int mismatch : {0, 1, 2, 3}) {
        TestCallMqtt mqtt;
        TestCallMedia media;
        HumanCallClient client(mqtt, media, "p/d", "diagnostic");
        ASSERT_NO_FATAL_FAILURE(reporting_client(client, mqtt));
        auto denied = reply(
            mismatch == 0 ? "other-token" : mqtt.sent.front().token, "requestAck", "",
            mismatch == 1 ? R"({"requestMethod":"sessionCreate","reason":"requestDenied"})"
                          : R"({"requestMethod":"capabilityReport","reason":"requestDenied"})");
        denied.code = mismatch == 2 ? 200 : 403;
        mqtt.down(denied,
                  mismatch == 3 ? "$thing/down/signaling/p/other" : "$thing/down/signaling/p/d");
        client.poll(103, 1'000'000);
        ASSERT_EQ(client.state(), HumanCallState::Failed);
        EXPECT_EQ(client.failure_facts().failure, HumanCallFailure::ReceiveRejected);
        EXPECT_EQ(client.failure_facts().capability_ack_code, 0);
    }
}
// 原订阅/网络事件错误与坏正文都终止原腿，无重拨、媒体启动或额外消息。
TEST(HumanCallFailureDiagnostic, SubscriptionAndUnexpectedNetworkEvent) {
    for (const bool subscription : {false, true}) {
        TestCallMqtt mqtt;
        TestCallMedia media;
        HumanCallClient client(mqtt, media, "p/d", "diagnostic");
        ASSERT_TRUE(client.start(MqttConfig{}, 100));
        mqtt.events.push_back({MqttEvent::Kind::Connected, 0, 0, {}, {}});
        client.poll(101, 1'000'000);
        mqtt.events.push_back(
            {subscription ? MqttEvent::Kind::Subscribed : MqttEvent::Kind::Disconnected,
             subscription ? 99 : 0,
             0,
             {},
             {}});
        client.poll(102, 1'000'000);
        ASSERT_EQ(client.state(), HumanCallState::Failed);
        EXPECT_EQ(client.failure_facts().failure, subscription ? HumanCallFailure::SubscriptionEvent
                                                               : HumanCallFailure::NetworkEvent);
        EXPECT_FALSE(media.started);
        EXPECT_TRUE(mqtt.sent.empty());
    }
}
// 原发布返回false不等于Requests未受理；诊断不能修正既有协议行为或伪造交付结果。
TEST(HumanCallFailureDiagnostic, PublishFailureStillUsesOriginalDeadline) {
    TestCallMqtt mqtt;
    TestCallMedia media;
    HumanCallClient client(mqtt, media, "p/d", "diagnostic");
    mqtt.publish_ok = false;
    ASSERT_NO_FATAL_FAILURE(reporting_client(client, mqtt));
    client.poll(30'101, 1'000'000);
    EXPECT_EQ(client.state(), HumanCallState::Reporting);
    EXPECT_EQ(client.failure_facts().failure, HumanCallFailure::None);
    client.poll(30'102, 1'000'000);
    EXPECT_EQ(client.state(), HumanCallState::Failed);
    EXPECT_EQ(client.failure_facts().failure, HumanCallFailure::PhaseDeadline);
    EXPECT_EQ(client.failure_facts().capability_ack_code, 0);
}
// 合法ACK保持Available；坏正文仅固定ReceiveRejected，未知原因及错码不保留自由文本。
TEST(HumanCallFailureDiagnostic, SuccessMalformedAndWrongReasonCode) {
    TestCallMqtt mqtt;
    TestCallMedia media;
    HumanCallClient client(mqtt, media, "p/d", "diagnostic");
    ASSERT_NO_FATAL_FAILURE(ready_client(client, mqtt));
    EXPECT_EQ(client.failure_facts().failure, HumanCallFailure::None);
    EXPECT_EQ(client.failure_facts().capability_ack_code, 0);
    client.close();
    ASSERT_NO_FATAL_FAILURE(reporting_client(client, mqtt));
    auto denied = reply(mqtt.sent.back().token, "requestAck", "",
                        R"({"requestMethod":"capabilityReport","reason":"capabilityNotAccepted"})");
    denied.code = 403;
    mqtt.down(denied);
    client.poll(103, 1'000'000);
    EXPECT_EQ(client.failure_facts().capability_reason, HumanCapabilityRejection::Unknown);
    client.close();
    ASSERT_NO_FATAL_FAILURE(reporting_client(client, mqtt));
    mqtt.events.push_back(
        {MqttEvent::Kind::Message, 0, 0, "$thing/down/signaling/p/d", "private-not-json"});
    client.poll(103, 1'000'000);
    EXPECT_EQ(client.state(), HumanCallState::Failed);
    EXPECT_EQ(client.failure_facts().failure, HumanCallFailure::ReceiveRejected);
    EXPECT_EQ(client.failure_facts().capability_ack_code, 0);
    EXPECT_EQ(client.failure_facts().capability_reason, HumanCapabilityRejection::None);
}
// invite 生成包含三轨的人际视频邀请，未知结构不能借用AI会话接入。
Envelope invite() {
    Envelope value;
    value.token = "invite-1";
    value.method = "sessionInvite";
    value.session = "call-1";
    value.event_sequence = 1;
    value.payload =
        R"({"purpose":"humanCall","callType":"video","admission":"accept","transport":"webrtc","expiresAt":1030000,"callGroupId":"im-call-1","tracks":[{"trackId":"mic-1","kind":"audio","sourceId":"mic","direction":"sendonly","codec":"opus","state":"requested"},{"trackId":"speaker-1","kind":"audio","sourceId":"speaker","direction":"recvonly","codec":"opus","state":"requested"},{"trackId":"video-1","kind":"video","sourceId":"front","direction":"sendonly","codec":"H264","state":"requested"}]})";
    return value;
}
// receive_video_invite 在旧三轨后显式增加远端接收轨，不改本地相机轨的发送方向。
Envelope receive_video_invite() {
    auto value = invite();
    value.payload.insert(value.payload.size() - 2,
        R"(,{"trackId":"video-in","kind":"video","sourceId":"remote","direction":"recvonly","codec":"H264","state":"requested"})");
    return value;
}
// 视频接收必须具备真实HAL能力且平台明确选择第四轨；广告能力不是实际开启。
TEST(HumanCallVideoReceive, ExplicitSelectionAndLegacyDowngrade) {
    for (const bool selected : {false, true}) {
        SCOPED_TRACE(selected);
        TestCallMqtt mqtt;
        TestCallMedia media;
        media.receive_capable = true;
        HumanCallClient client(mqtt, media, "p/d", "receive-video",
                               {"front", false, false, HumanCallMode::Video, true});
        ready_client(client, mqtt);
        EXPECT_NE(mqtt.sent.front().payload.find("videoReceive"), std::string::npos);
        EXPECT_EQ(media.receive_config_calls, 0);
        mqtt.down(selected ? receive_video_invite() : invite());
        client.poll(104, 1'000'000);
        ASSERT_EQ(client.state(), HumanCallState::Ringing);
        ASSERT_TRUE(client.accept(105));
        EXPECT_EQ(mqtt.sent.back().payload.find("video-in") != std::string::npos, selected);
        mqtt.down(reply("invite-1", "requestAck", "call-1",
                        R"({"requestMethod":"sessionAccept","result":{"state":"negotiating"}})"));
        client.poll(106, 1'000'000);
        EXPECT_EQ(media.receive_config_calls, 1);
        EXPECT_EQ(media.receive_configured, selected);
        EXPECT_TRUE(media.started);
    }
}
// 旧固件不能接纳第四轨，显式能力缺HAL或HAL拒绝配置时也不能偷偷启动媒体。
TEST(HumanCallVideoReceive, RequiresCapabilityAndActualHAL) {
    {
        TestCallMqtt mqtt;
        TestCallMedia media;
        HumanCallClient client(mqtt, media, "p/d", "legacy-video");
        ready_client(client, mqtt);
        EXPECT_EQ(mqtt.sent.front().payload.find("videoReceive"), std::string::npos);
        mqtt.down(receive_video_invite());
        client.poll(104, 1'000'000);
        EXPECT_EQ(client.state(), HumanCallState::Failed);
        EXPECT_EQ(media.begins, 0);
    }
    {
        TestCallMqtt mqtt;
        TestCallMedia media;
        HumanCallClient client(mqtt, media, "p/d", "missing-hal",
                               {"front", false, false, HumanCallMode::Video, true});
        EXPECT_FALSE(client.start(MqttConfig{}, 100));
        EXPECT_TRUE(mqtt.sent.empty());
    }
    {
        TestCallMqtt mqtt;
        TestCallMedia media;
        media.receive_capable = true;
        media.receive_config_ok = false;
        HumanCallClient client(mqtt, media, "p/d", "failed-hal",
                               {"front", false, false, HumanCallMode::Video, true});
        ready_client(client, mqtt);
        mqtt.down(receive_video_invite());
        client.poll(104, 1'000'000);
        ASSERT_TRUE(client.accept(105));
        mqtt.down(reply("invite-1", "requestAck", "call-1",
                        R"({"requestMethod":"sessionAccept","result":{"state":"negotiating"}})"));
        client.poll(106, 1'000'000);
        EXPECT_EQ(client.state(), HumanCallState::Failed);
        EXPECT_EQ(media.begins, 0);
    }
}
// 第四轨不能用本地相机、改为双向或塞sourceType绕过严格远端授权。
TEST(HumanCallVideoReceive, RejectsMalformedRemoteTrack) {
    for (unsigned variant = 0; variant < 4; ++variant) {
        SCOPED_TRACE(variant);
        TestCallMqtt mqtt;
        TestCallMedia media;
        media.receive_capable = true;
        HumanCallClient client(mqtt, media, "p/d", "malformed-receive",
                               {"front", false, false, HumanCallMode::Video, true});
        ready_client(client, mqtt);
        auto message = receive_video_invite();
        const std::string needle = variant == 0 ? "\"sourceId\":\"remote\"" :
                                   variant == 1 ? "\"direction\":\"recvonly\"" :
                                   variant == 2 ? "\"trackId\":\"video-in\"" :
                                                  "\"trackId\":\"video-in\"";
        const std::string replacement = variant == 0 ? "\"sourceId\":\"front\"" :
                                        variant == 1 ? "\"direction\":\"sendrecv\"" :
                                        variant == 2 ? "\"trackId\":\"video-1\"" :
                                                       "\"trackId\":\"video-in\",\"sourceType\":\"camera\"";
        const auto offset = message.payload.rfind(needle);
        ASSERT_NE(offset, std::string::npos);
        message.payload.replace(offset, needle.size(), replacement);
        mqtt.down(message);
        client.poll(104, 1'000'000);
        EXPECT_EQ(client.state(), HumanCallState::Failed);
        EXPECT_EQ(media.begins, 0);
        EXPECT_EQ(media.receive_config_calls, 0);
    }
}
// 主叫提议四轨后仍只按平台回执启动三轨或四轨，不把自己的提议当选定许可。
TEST(HumanCallVideoReceive, CallerProposalDoesNotGrantRemoteVideo) {
    for (const bool selected : {false, true}) {
        SCOPED_TRACE(selected);
        TestCallMqtt mqtt;
        TestCallMedia media;
        media.receive_capable = true;
        HumanCallClient client(mqtt, media, "p/d", "caller-receive",
                               {"front", false, false, HumanCallMode::Video, true});
        ready_client(client, mqtt);
        ASSERT_TRUE(client.dial("p:other", 104));
        EXPECT_NE(mqtt.sent.back().payload.find("video-in"), std::string::npos);
        EXPECT_EQ(media.receive_config_calls, 0);
        const auto selected_invite = selected ? receive_video_invite() : invite();
        const auto start = selected_invite.payload.find("\"tracks\":");
        ASSERT_NE(start, std::string::npos);
        mqtt.down(reply(mqtt.sent.back().token, "sessionCreated", "caller-1",
                        R"({"state":"inviting","callGroupId":"group-1","selected":{"transport":"webrtc"},)" +
                            selected_invite.payload.substr(start)));
        client.poll(105, 1'000'000);
        EXPECT_EQ(client.state(), HumanCallState::Negotiating);
        EXPECT_EQ(media.receive_configured, selected);
        EXPECT_EQ(media.receive_config_calls, 1);
        EXPECT_EQ(media.bound_scope, "caller-1");
    }
}
// 创建应答已合法也不能绕过HAL会话绑定失败而启动摄像头或远端解码。
TEST(HumanCallVideoReceive, OutgoingSessionBindingFailureStopsBeforeHALStart) {
    TestCallMqtt mqtt;
    TestCallMedia media;
    media.bind_scope_ok = false;
    HumanCallClient client(mqtt, media, "p/d", "bind-failed");
    ready_client(client, mqtt);
    ASSERT_TRUE(client.dial("p:other", 104));
    const auto tracks = invite().payload;
    const auto start = tracks.find("\"tracks\":");
    ASSERT_NE(start, std::string::npos);
    mqtt.down(reply(mqtt.sent.back().token, "sessionCreated", "caller-bind-1",
                    R"({"state":"inviting","callGroupId":"group-1","selected":{"transport":"webrtc"},)" +
                        tracks.substr(start)));
    client.poll(105, 1'000'000);
    EXPECT_EQ(client.state(), HumanCallState::Failed);
    EXPECT_EQ(media.bound_scope, "caller-bind-1");
    EXPECT_EQ(media.receive_config_calls, 0);
    EXPECT_EQ(media.begins, 0);
}
// 候选主动结束的原tokenACK与独立终结事件允许两种顺序；任一先确认终态后，旧腿消息不得影响后来邀请。
TEST(HumanCallTest, CandidateEndAckAndEventBothOrders) {
    for (const bool ack_first : {false, true}) {
        SCOPED_TRACE(ack_first ? "ack_first" : "event_first");
        TestCallMqtt mqtt;
        TestCallMedia media;
        HumanCallClient client(mqtt, media, "p/d", "candidate-end");
        ASSERT_NO_FATAL_FAILURE(ready_client(client, mqtt));
        mqtt.down(invite());
        client.poll(104, 1'000'000);
        ASSERT_TRUE(client.accept(105));
        mqtt.down(reply("invite-1", "requestAck", "call-1",
                        R"({"requestMethod":"sessionAccept","result":{"state":"negotiating"}})"));
        client.poll(106, 1'000'000);
        ASSERT_EQ(client.state(), HumanCallState::Negotiating);
        ASSERT_TRUE(media.started);
        ASSERT_TRUE(client.end(107));
        ASSERT_EQ(client.state(), HumanCallState::Ending);
        ASSERT_FALSE(media.started);
        const auto end_token = mqtt.sent.back().token;
        ASSERT_EQ(mqtt.sent.back().method, "sessionEnd");
        auto ack =
            reply(end_token, "sessionEnded", "call-1", R"({"state":"ended","reason":"hangup"})");
        Envelope event;
        event.request = false;
        event.token = "candidate-independent-end";
        event.method = "sessionEnded";
        event.session = "call-1";
        event.event_sequence = 2;
        event.payload = R"({"state":"ended","reason":"transferRejected"})";
        mqtt.down(ack_first ? ack : event);
        client.poll(108, 1'000'000);
        ASSERT_EQ(client.state(), HumanCallState::Available);
        const auto closes_after_terminal = media.closes;
        const auto sent_after_terminal = mqtt.sent.size();
        mqtt.down(ack_first ? event : ack);
        client.poll(109, 1'000'000);
        EXPECT_EQ(client.state(), HumanCallState::Available);
        EXPECT_EQ(media.closes, closes_after_terminal);
        EXPECT_EQ(mqtt.sent.size(), sent_after_terminal);
        EXPECT_FALSE(media.started);
        auto next = invite();
        next.token = "next-candidate-invite";
        next.session = "next-candidate-leg";
        mqtt.down(next);
        client.poll(110, 1'000'000);
        ASSERT_EQ(client.state(), HumanCallState::Ringing);
        mqtt.down(ack);
        mqtt.down(event);
        client.poll(111, 1'000'000);
        EXPECT_EQ(client.state(), HumanCallState::Ringing);
        EXPECT_EQ(media.closes, closes_after_terminal);
        EXPECT_FALSE(media.started);
    }
}
// connected_client
// 提供原接听、SDP、驱动连通和平台建立事件，名册不能替代任一接通条件。
void connected_client(HumanCallClient& client, TestCallMqtt& mqtt, TestCallMedia& media) {
    ASSERT_NO_FATAL_FAILURE(ready_client(client, mqtt));
    mqtt.down(invite());
    client.poll(104, 1'000'000);
    ASSERT_TRUE(client.accept(105));
    mqtt.down(reply("invite-1", "requestAck", "call-1",
                    R"({"requestMethod":"sessionAccept","result":{"state":"negotiating"}})"));
    client.poll(106, 1'000'000);
    ASSERT_TRUE(client.offer_ready("v=0\r\no=client\r\n", 107));
    mqtt.down(reply(
        mqtt.sent.back().token, "mediaAnswer", "call-1",
        R"({"mediaRevision":1,"webrtc":{"iceMode":"complete","sdp":"v=0\r\no=server\r\n"}})"));
    client.poll(108, 1'000'000);
    media.ready = true;
    ASSERT_TRUE(client.media_connected(109));
    mqtt.down(reply(
        mqtt.sent.back().token, "requestAck", "call-1",
        R"({"requestMethod":"mediaState","result":{"mediaRevision":1,"state":"connected"}})"));
    Envelope established;
    established.request = false;
    established.token = "roster-established";
    established.method = "sessionEstablished";
    established.session = "call-1";
    established.event_sequence = 2;
    established.payload = R"({"state":"established","mediaRevision":1})";
    mqtt.down(established);
    client.poll(110, 1'000'000);
    ASSERT_TRUE(client.active());
}
// 原已建会话的在途结束请求也须取消，close后新连接不得重发已清理的旧腿。
TEST(HumanCallCapabilityRecovery, CloseCancelsPendingSessionRequests) {
    TestCallMqtt mqtt;
    TestCallMedia media;
    HumanCallClient client(mqtt, media, "p/d", "cap-recovery");
    ASSERT_NO_FATAL_FAILURE(connected_client(client, mqtt, media));
    ASSERT_TRUE(client.end(111));
    ASSERT_EQ(mqtt.sent.back().method, "sessionEnd");
    const auto count = mqtt.sent.size();
    client.close();
    ASSERT_TRUE(client.start(MqttConfig{}, 20'000));
    mqtt.events.push_back({MqttEvent::Kind::Connected, 0, 0, {}, {}});
    client.poll(20'001, 1'000'000);
    EXPECT_EQ(mqtt.sent.size(), count);
    EXPECT_EQ(client.state(), HumanCallState::Subscribing);
}
// participant_event
// 只生成平台对原端腿的名册事实，不产生任何媒体请求或话权许可。
Envelope participant_event(std::uint64_t sequence, std::string data) {
    Envelope event;
    event.request = false;
    event.token = "participant-" + std::to_string(sequence);
    event.method = "participantChanged";
    event.session = "call-1";
    event.event_sequence = sequence;
    event.payload = std::move(data);
    return event;
}
// roster_track 只拼接用例内固定标识，覆盖公共Track的全部字段，不产生驱动轨。
std::string roster_track(const std::string& id, std::string kind = "audio",
                         std::string direction = "recvonly", std::string state = "requested") {
    return R"({"trackId":")" + id + R"(","kind":")" + kind +
           R"(","sourceId":"source","direction":")" + direction + R"(","codec":"opus","state":")" +
           state + R"("})";
}
// roster_data 用公开合成标识生成一条成员快照，空轨表示仅成员事实而非媒体授权。
std::string roster_data(const std::string& id, const std::string& tracks = "[]",
                        const std::string& change = "joined") {
    return R"({"participantId":")" + id + R"(","change":")" + change + R"(","tracks":)" + tracks +
           "}";
}
}  // namespace

// HumanCallCapability
// 自定义相机替换使用真实JSON编码器；可选名册不能覆盖设备原相机身份。
TEST(HumanCallCapability, ExplicitCameraSurvivesOptionalRosterSerialization) {
    for (bool roster : {false, true}) {
        TestCallMqtt mqtt;
        TestCallMedia media;
        HumanCallClient client(mqtt, media, "p/d", "camera", {"rear-1", false, roster});
        ASSERT_NO_FATAL_FAILURE(ready_client(client, mqtt));
        ASSERT_EQ(mqtt.sent.size(), 1U);
        EXPECT_NE(mqtt.sent[0].payload.find(R"("cameraId":"rear-1")"), std::string::npos);
        EXPECT_EQ(mqtt.sent[0].payload.find(R"("cameraId":"front")"), std::string::npos);
        EXPECT_EQ(mqtt.sent[0].payload.find("participantRoster") != std::string::npos, roster);
        EXPECT_FALSE(media.started);
        EXPECT_EQ(media.closes,
                  1);  // start先释放旧驱动一次；能力编码不能触发额外媒体动作。
        client.close();
    }
}

// HumanCallRoster
// 合法成员事件不能按未知方法关闭已接通会话，也不能自行启动新媒体。
TEST(HumanCallRoster, DeclaredMemberEventPreservesConnectedMedia) {
    TestCallMqtt mqtt;
    TestCallMedia media;
    HumanCallClient client(mqtt, media, "p/d", "roster", {"front", false, true});
    ASSERT_NO_FATAL_FAILURE(connected_client(client, mqtt, media));
    ASSERT_NE(mqtt.sent.front().payload.find("participantRoster"), std::string::npos);
    const auto closes = media.closes;
    const auto requests = mqtt.sent.size();
    mqtt.down(participant_event(3, R"({"participantId":"user-b","change":"joined","tracks":[]})"));
    client.poll(111, 1'000'000);
    EXPECT_TRUE(client.active());
    EXPECT_TRUE(media.started);
    EXPECT_TRUE(media.ready);
    EXPECT_EQ(media.closes, closes);
    EXPECT_EQ(mqtt.sent.size(), requests);
    ASSERT_EQ(client.participants().size(), 1U);
    EXPECT_EQ(client.participants().front().participant_id, "user-b");
    EXPECT_TRUE(client.participants().front().tracks.empty());
}

// HumanCallRoster
// 成员与轨只投影原腿，重复和迟到水位不改新名册，跳号不等于名册丢失。
TEST(HumanCallRoster, JoinReplaceLeaveAndSharedWatermark) {
    TestCallMqtt mqtt;
    TestCallMedia media;
    HumanCallClient client(mqtt, media, "p/d", "roster", {"front", false, true});
    ASSERT_NO_FATAL_FAILURE(connected_client(client, mqtt, media));
    const auto published = mqtt.sent.size();
    const auto closes = media.closes;
    mqtt.down(participant_event(3, roster_data("user-b", "[" + roster_track("b-audio") + "]")));
    client.poll(111, 1'000'000);
    ASSERT_EQ(client.participants().size(), 1U);
    ASSERT_EQ(client.participants()[0].tracks.size(), 1U);
    EXPECT_EQ(client.participants()[0].tracks[0].track_id, "b-audio");
    EXPECT_EQ(client.participants()[0].tracks[0].direction, "recvonly");
    EXPECT_EQ(client.participants()[0].tracks[0].state, "requested");
    mqtt.down(participant_event(3, roster_data("duplicate")));
    mqtt.down(participant_event(2, roster_data("late")));
    auto other_leg = participant_event(100, roster_data("other"));
    other_leg.session = "other-leg";
    mqtt.down(other_leg);
    client.poll(112, 1'000'000);
    ASSERT_EQ(client.participants().size(), 1U);
    EXPECT_EQ(client.participants()[0].participant_id, "user-b");
    // shared eventSeq允许跨其他方法的水位；名册不另造连续序号或启动新媒体。
    mqtt.down(participant_event(
        7,
        roster_data("user-b", "[" + roster_track("b-video", "video", "sendrecv", "active") + "]")));
    mqtt.down(participant_event(
        8, roster_data("user-c",
                       "[" + roster_track("c-sub", "subtitle", "sendonly", "paused") + "]")));
    client.poll(113, 1'000'000);
    ASSERT_EQ(client.participants().size(), 2U);
    EXPECT_EQ(client.participants()[0].tracks[0].kind, "video");
    EXPECT_EQ(client.participants()[0].tracks[0].state, "active");
    EXPECT_EQ(client.participants()[1].tracks[0].kind, "subtitle");
    mqtt.down(participant_event(9, roster_data("unknown", "[]", "left")));
    mqtt.down(participant_event(
        10,
        roster_data("user-b", "[" + roster_track("b-video", "video", "recvonly", "stopped") + "]",
                    "left")));
    mqtt.down(participant_event(10, roster_data("user-b")));
    client.poll(114, 1'000'000);
    ASSERT_EQ(client.participants().size(), 1U);
    EXPECT_EQ(client.participants()[0].participant_id, "user-c");
    EXPECT_TRUE(client.active());
    EXPECT_TRUE(media.ready);
    EXPECT_EQ(media.closes, closes);
    EXPECT_EQ(mqtt.sent.size(), published);
}

// HumanCallRoster
// 默认设备不广告能力且拒绝本腿成员事件，不能由新固件行为改变旧端契约。
TEST(HumanCallRoster, DefaultClientDoesNotClaimRosterSupport) {
    TestCallMqtt mqtt;
    TestCallMedia media;
    HumanCallClient client(mqtt, media, "p/d", "old-client");
    ASSERT_NO_FATAL_FAILURE(connected_client(client, mqtt, media));
    EXPECT_EQ(mqtt.sent.front().payload.find("participantRoster"), std::string::npos);
    mqtt.down(participant_event(3, roster_data("user-b")));
    client.poll(111, 1'000'000);
    EXPECT_EQ(client.state(), HumanCallState::Failed);
    EXPECT_TRUE(client.participants().empty());
}

// HumanCallRoster
// 借用宿主也必须等待原腿建立，提前成员事件不能夺取宿主网络所有权。
TEST(HumanCallRoster, BorrowedHostRejectsPrematureMembers) {
    TestCallMqtt mqtt;
    TestCallMedia media;
    HumanCallClient client(mqtt, media, "p/d", "borrowed", {"front", true, true});
    ASSERT_TRUE(client.start_shared(100));
    Envelope value = invite();
    std::string body;
    ASSERT_EQ(encode_envelope(value, body), DecodeResult::Ok);
    ASSERT_TRUE(client.receive_shared("$thing/down/signaling/p/d", body, 101, 1'000'000));
    // 邀请不等于建立；宿主提前下发成员事件必须拒绝且不能建立媒体。
    value = participant_event(2, roster_data("premature"));
    ASSERT_EQ(encode_envelope(value, body), DecodeResult::Ok);
    EXPECT_FALSE(client.receive_shared("$thing/down/signaling/p/d", body, 102, 1'000'000));
    EXPECT_EQ(client.state(), HumanCallState::Failed);
    EXPECT_TRUE(client.participants().empty());
    EXPECT_EQ(mqtt.closed, 0);
    EXPECT_TRUE(mqtt.sent.empty());
}

// HumanCallRoster
// 借用模式只消费宿主分派的原腿信令；本单测的配置模拟宿主已广告能力，不冒称宿主接线完成。
TEST(HumanCallRoster, BorrowedHostProjectsWithoutOwningConnection) {
    TestCallMqtt mqtt;
    TestCallMedia media;
    HumanCallClient client(mqtt, media, "p/d", "borrowed", {"front", true, true});
    std::uint64_t now = 100;
    ASSERT_TRUE(client.start_shared(now));
    // 每次分派使用原公共编解码，不向假MQTT队列注入后要求借用客户端消费。
    const auto deliver = [&](const Envelope& event) {
        std::string body;
        ASSERT_EQ(encode_envelope(event, body), DecodeResult::Ok);
        ASSERT_TRUE(client.receive_shared("$thing/down/signaling/p/d", body, ++now, 1'000'000));
    };
    ASSERT_NO_FATAL_FAILURE(deliver(invite()));
    ASSERT_TRUE(client.accept(++now));
    ASSERT_NO_FATAL_FAILURE(
        deliver(reply("invite-1", "requestAck", "call-1",
                      R"({"requestMethod":"sessionAccept","result":{"state":"negotiating"}})")));
    ASSERT_TRUE(client.offer_ready("v=0\r\no=client\r\n", ++now));
    ASSERT_NO_FATAL_FAILURE(deliver(reply(
        mqtt.sent.back().token, "mediaAnswer", "call-1",
        R"({"mediaRevision":1,"webrtc":{"iceMode":"complete","sdp":"v=0\r\no=server\r\n"}})")));
    media.ready = true;
    ASSERT_TRUE(client.media_connected(++now));
    ASSERT_NO_FATAL_FAILURE(deliver(reply(
        mqtt.sent.back().token, "requestAck", "call-1",
        R"({"requestMethod":"mediaState","result":{"state":"connected","mediaRevision":1}})")));
    auto established = participant_event(2, R"({"state":"established","mediaRevision":1})");
    established.method = "sessionEstablished";
    ASSERT_NO_FATAL_FAILURE(deliver(established));
    ASSERT_TRUE(client.active());
    const auto published = mqtt.sent.size();
    ASSERT_NO_FATAL_FAILURE(deliver(participant_event(3, roster_data("member"))));
    ASSERT_EQ(client.participants().size(), 1U);
    EXPECT_EQ(client.participants()[0].participant_id, "member");
    EXPECT_EQ(mqtt.sent.size(), published);
    EXPECT_EQ(mqtt.closed, 0);
    EXPECT_TRUE(mqtt.events.empty());
    client.close();
    EXPECT_TRUE(client.participants().empty());
    EXPECT_EQ(mqtt.closed, 0);
}

// endpointState与原人际会话共用事件水位，但其准备事实不得关闭原驱动或制造接听。
TEST(HumanCallEndpoint, SharedEndpointFactDoesNotFailOriginalLeg) {
    TestCallMqtt mqtt;
    TestCallMedia media;
    HumanCallClient client(mqtt, media, "p/d", "endpoint-shared", {"front", true, false});
    ASSERT_TRUE(client.start_shared(100));
    const auto initial_closes = media.closes; // start_shared清理旧驱动是原生命周期，不算endpoint事实副作用。
    std::string body;
    ASSERT_EQ(encode_envelope(invite(), body), DecodeResult::Ok);
    ASSERT_TRUE(client.receive_shared("$thing/down/signaling/p/d", body, 101, 1'000'000));
    Envelope event;
    event.request = false;
    event.token = "endpoint-ready";
    event.method = "endpointState";
    event.session = "call-1";
    event.event_sequence = 2;
    event.payload = R"({"state":"preparing","revision":1,"handoffId":"handoff"})";
    ASSERT_EQ(encode_envelope(event, body), DecodeResult::Ok);
    ASSERT_TRUE(client.receive_shared("$thing/down/signaling/p/d", body, 102, 1'000'000));
    EXPECT_EQ(client.state(), HumanCallState::Ringing);
    EXPECT_EQ(media.begins, 0);
    EXPECT_EQ(media.closes, initial_closes);
    EXPECT_EQ(mqtt.closed, 0);
    EXPECT_TRUE(client.observe_shared_endpoint("$thing/down/signaling/p/d", body, 102, 1'000'000));
    const auto original = body;
    EXPECT_FALSE(
        client.observe_shared_endpoint(original, "$thing/down/signaling/p/d", 102, 1'000'000));
    auto changed = event;
    changed.payload = R"({"state":"ready","revision":2,"handoffId":"handoff","candidateSessionId":"candidate"})";
    ASSERT_EQ(encode_envelope(changed, body), DecodeResult::Ok);
    EXPECT_FALSE(client.observe_shared_endpoint("$thing/down/signaling/p/d", body, 102, 1'000'000));
    changed.event_sequence = 4;
    changed.token = "endpoint-gap";
    ASSERT_EQ(encode_envelope(changed, body), DecodeResult::Ok);
    EXPECT_FALSE(client.observe_shared_endpoint("$thing/down/signaling/p/d", body, 102, 1'000'000));
    EXPECT_FALSE(client.observe_shared_endpoint("$thing/down/signaling/p/other", original, 102, 1'000'000));
    EXPECT_FALSE(client.observe_shared_endpoint("$thing/down/signaling/p/d", original, 101, 1'000'000));
    EXPECT_FALSE(client.observe_shared_endpoint("$thing/down/signaling/p/d", original, 102, 999'999));
    changed.event_sequence = 3;
    changed.session = "other-call";
    ASSERT_EQ(encode_envelope(changed, body), DecodeResult::Ok);
    EXPECT_FALSE(client.observe_shared_endpoint("$thing/down/signaling/p/d", body, 102, 1'000'000));
    changed.session = "call-1";
    ASSERT_EQ(encode_envelope(changed, body), DecodeResult::Ok);
    ASSERT_TRUE(client.observe_shared_endpoint("$thing/down/signaling/p/d", body, 103, 1'000'000));
    EXPECT_EQ(client.state(), HumanCallState::Ringing);
    EXPECT_EQ(media.closes, initial_closes);
    ASSERT_TRUE(client.accept(104));
    client.close();
    EXPECT_FALSE(client.observe_shared_endpoint("$thing/down/signaling/p/d", body, 105, 1'000'000));
}

// HumanCallRoster 非法方法正文或信封不能部分更新名册，更不能改变当前媒体授权。
TEST(HumanCallRoster, InvalidFieldsFailClosedAndClearRoster) {
    const std::vector<std::string> invalid = {
        R"({"participantId":"","change":"joined","tracks":[]})",
        R"({"participantId":1,"change":"joined","tracks":[]})",
        R"({"participantId":"user\n","change":"joined","tracks":[]})",
        roster_data(std::string(129, 'p')),
        R"({"participantId":"user","change":"updated","tracks":[]})",
        R"({"participantId":"user","change":"joined"})",
        R"({"participantId":"user","change":"joined","tracks":null})",
        R"({"participantId":"user","change":"joined","tracks":[],"permission":"talk"})",
        roster_data("user", "[{}]"),
        roster_data("user", "[" + roster_track("t", "data") + "]"),
        roster_data("user", "[" + roster_track("t", "audio", "inactive") + "]"),
        roster_data("user", "[" + roster_track("t", "audio", "recvonly", "ready") + "]"),
        roster_data("user", "[" + roster_track("") + "]"),
        roster_data("user", "[" + roster_track(std::string(129, 't')) + "]"),
        roster_data(
            "user",
            R"([{"trackId":"t","kind":"audio","sourceId":1,"direction":"recvonly","codec":"opus","state":"requested"}])"),
        roster_data(
            "user",
            R"([{"trackId":"t","kind":"audio","sourceId":"s","direction":"recvonly","codec":"","state":"requested"}])"),
        roster_data(
            "user",
            R"([{"trackId":"t","kind":"audio","sourceId":"s","direction":"recvonly","codec":")" +
                std::string(65, 'c') + R"(","state":"requested"}])"),
        roster_data(
            "user",
            R"([{"trackId":"t","kind":"audio","sourceId":"s","direction":"recvonly","codec":"opus","state":"requested","permission":"talk"}])"),
        roster_data("user", "[" + roster_track("same") + "," + roster_track("same") + "]")};
    for (std::size_t index = 0; index < invalid.size(); ++index) {
        SCOPED_TRACE(index);
        TestCallMqtt mqtt;
        TestCallMedia media;
        HumanCallClient client(mqtt, media, "p/d", "invalid", {"front", false, true});
        ASSERT_NO_FATAL_FAILURE(connected_client(client, mqtt, media));
        mqtt.down(participant_event(3, roster_data("initial")));
        client.poll(111, 1'000'000);
        ASSERT_EQ(client.participants().size(), 1U);
        mqtt.down(participant_event(4, invalid[index]));
        client.poll(112, 1'000'000);
        EXPECT_EQ(client.state(), HumanCallState::Failed);
        EXPECT_TRUE(client.participants().empty());
        EXPECT_FALSE(media.started);
    }
}

// HumanCallRoster
// 长字段仍受逻辑64KiB预算限制，不能利用少于32成员绕过设备内存防护。
TEST(HumanCallRoster, BoundedLogicalBytes) {
    TestCallMqtt mqtt;
    TestCallMedia media;
    HumanCallClient client(mqtt, media, "p/d", "bytes", {"front", false, true});
    ASSERT_NO_FATAL_FAILURE(connected_client(client, mqtt, media));
    const std::size_t member_bytes =
        sizeof(HumanParticipant) + 128 +
        8 * (sizeof(HumanParticipantTrack) + 128 + 5 + 128 + 8 + 64 + 6);
    const std::size_t accepted = 65536 / member_bytes;
    ASSERT_GT(accepted, 0U);
    ASSERT_LT(accepted, 32U);
    for (std::size_t index = 0; index <= accepted; ++index) {
        std::string id = "member-" + std::to_string(index);
        id.resize(128, 'm');
        std::string tracks = "[";
        for (unsigned track = 0; track < 8; ++track) {
            if (track != 0)
                tracks += ",";
            std::string track_id = "track-" + std::to_string(index) + "-" + std::to_string(track);
            track_id.resize(128, 't');
            tracks += R"({"trackId":")" + track_id + R"(","kind":"audio","sourceId":")" +
                      std::string(128, 's') + R"(","direction":"recvonly","codec":")" +
                      std::string(64, 'c') + R"(","state":"active"})";
        }
        mqtt.down(participant_event(3 + index, roster_data(id, tracks + "]")));
        client.poll(111 + index, 1'000'000);
        if (index < accepted) {
            ASSERT_TRUE(client.active());
            EXPECT_EQ(client.participants().size(), index + 1);
        } else {
            EXPECT_EQ(client.state(), HumanCallState::Failed);
            EXPECT_TRUE(client.participants().empty());
        }
    }
}

// HumanCallRoster
// 事件不能混用请求params、ACK或内容水位，也不能脱离原端腿或精确Topic。
TEST(HumanCallRoster, InvalidEnvelopeDoesNotGrantMembership) {
    for (unsigned variant = 0; variant < 6; ++variant) {
        SCOPED_TRACE(variant);
        TestCallMqtt mqtt;
        TestCallMedia media;
        HumanCallClient client(mqtt, media, "p/d", "invalid", {"front", false, true});
        ASSERT_NO_FATAL_FAILURE(connected_client(client, mqtt, media));
        auto event = participant_event(3, roster_data("user-b"));
        if (variant == 0)
            event.request = true;
        if (variant == 1)
            event.code = 200;
        if (variant == 2)
            event.event_sequence.reset();
        if (variant == 3) {
            event.event_sequence.reset();
            event.message_sequence = 3;
        }
        if (variant == 4)
            event.session.clear();
        mqtt.down(event,
                  variant == 5 ? "$thing/down/signaling/p/other" : "$thing/down/signaling/p/d");
        client.poll(111, 1'000'000);
        EXPECT_EQ(client.state(), HumanCallState::Failed);
        EXPECT_TRUE(client.participants().empty());
    }
}

// HumanCallRoster
// 存储人数上限是设备防护，合法替换/退出释放名册槽位，不修改IM准入。
TEST(HumanCallRoster, BoundedMembersAllowReplacementAndReclaim) {
    TestCallMqtt mqtt;
    TestCallMedia media;
    HumanCallClient client(mqtt, media, "p/d", "capacity", {"front", false, true});
    ASSERT_NO_FATAL_FAILURE(connected_client(client, mqtt, media));
    std::uint64_t sequence = 3;
    for (unsigned index = 0; index < 32; ++index) {
        mqtt.down(participant_event(sequence++, roster_data("member-" + std::to_string(index))));
        client.poll(111 + index, 1'000'000);
        ASSERT_TRUE(client.active());
    }
    ASSERT_EQ(client.participants().size(), 32U);
    mqtt.down(participant_event(sequence++,
                                roster_data("member-0", "[" + roster_track("track-0") + "]")));
    mqtt.down(participant_event(sequence++, roster_data("member-1", "[]", "left")));
    mqtt.down(participant_event(sequence++, roster_data("replacement")));
    client.poll(144, 1'000'000);
    ASSERT_TRUE(client.active());
    ASSERT_EQ(client.participants().size(), 32U);
    EXPECT_EQ(client.participants()[0].tracks.size(), 1U);
    mqtt.down(participant_event(sequence, roster_data("overflow")));
    client.poll(145, 1'000'000);
    EXPECT_EQ(client.state(), HumanCallState::Failed);
    EXPECT_TRUE(client.participants().empty());
}

// HumanCallRoster 同一会话轨不能归属两个成员；每成员最多八条只读轨。
TEST(HumanCallRoster, DuplicateTrackAcrossMembersAndTrackBound) {
    for (bool duplicate : {false, true}) {
        TestCallMqtt mqtt;
        TestCallMedia media;
        HumanCallClient client(mqtt, media, "p/d", "tracks", {"front", false, true});
        ASSERT_NO_FATAL_FAILURE(connected_client(client, mqtt, media));
        std::string tracks = "[";
        for (unsigned index = 0; index < 8; ++index) {
            if (index != 0)
                tracks += ",";
            tracks += roster_track("track-" + std::to_string(index));
        }
        mqtt.down(participant_event(3, roster_data("first", tracks + "]")));
        client.poll(111, 1'000'000);
        ASSERT_TRUE(client.active());
        ASSERT_EQ(client.participants()[0].tracks.size(), 8U);
        mqtt.down(participant_event(
            4, duplicate ? roster_data("second", "[" + roster_track("track-0") + "]")
                         : roster_data("first", tracks + "," + roster_track("track-8") + "]")));
        client.poll(112, 1'000'000);
        EXPECT_EQ(client.state(), HumanCallState::Failed);
        EXPECT_TRUE(client.participants().empty());
    }
}

// HumanCallRoster 所有正常结束路径清名册，迟到旧成员不能借新会话复活。
TEST(HumanCallRoster, LocalAndRemoteEndClearRoster) {
    for (bool local : {false, true}) {
        TestCallMqtt mqtt;
        TestCallMedia media;
        HumanCallClient client(mqtt, media, "p/d", "end", {"front", false, true});
        ASSERT_NO_FATAL_FAILURE(connected_client(client, mqtt, media));
        mqtt.down(participant_event(3, roster_data("user-b")));
        client.poll(111, 1'000'000);
        ASSERT_EQ(client.participants().size(), 1U);
        if (local) {
            ASSERT_TRUE(client.end(112));
            EXPECT_TRUE(client.participants().empty());
            mqtt.down(participant_event(4, roster_data("late-while-ending")));
            client.poll(112, 1'000'000);
            EXPECT_EQ(client.state(), HumanCallState::Ending);
            EXPECT_TRUE(client.participants().empty());
            mqtt.down(reply(mqtt.sent.back().token, "sessionEnded", "call-1",
                            R"({"state":"ended","reason":"hangup"})"));
        } else {
            auto ended = participant_event(4, R"({"state":"ended","reason":"hangup"})");
            ended.method = "sessionEnded";
            mqtt.down(ended);
        }
        client.poll(113, 1'000'000);
        ASSERT_EQ(client.state(), HumanCallState::Available);
        EXPECT_TRUE(client.participants().empty());
        mqtt.down(participant_event(5, roster_data("late")));
        client.poll(114, 1'000'000);
        EXPECT_EQ(client.state(), HumanCallState::Available);
        EXPECT_TRUE(client.participants().empty());
        auto next = invite();
        next.token = "next-invite";
        next.session = "next-leg";
        mqtt.down(next);
        client.poll(115, 1'000'000);
        ASSERT_EQ(client.state(), HumanCallState::Ringing);
        mqtt.down(participant_event(500, roster_data("old-member")));
        client.poll(116, 1'000'000);
        EXPECT_EQ(client.state(), HumanCallState::Ringing);
        EXPECT_TRUE(client.participants().empty());
        EXPECT_FALSE(media.started);
        client.close();
        EXPECT_TRUE(client.participants().empty());
        EXPECT_EQ(client.state(), HumanCallState::Idle);
    }
}

// control_event 前向声明复用本文件原固定保持/恢复事件，转接不另造旧控制报文。
Envelope control_event(std::string token, std::string state, std::uint64_t revision,
                       std::uint64_t sequence);
// HumanCallTransfer
// 留存端收到合法转接失败只结束对端操作，不关闭原session或实际媒体。
TEST(HumanCallTransfer, RemoteFailurePreservesOriginalConnectedLeg) {
    TestCallMqtt mqtt;
    TestCallMedia media;
    HumanCallClient client(mqtt, media, "p/d", "transfer-red");
    ASSERT_NO_FATAL_FAILURE(connected_client(client, mqtt, media));
    const auto closes = media.closes;
    auto event = participant_event(
        3,
        R"({"state":"failed","revision":2,"requestToken":"remote-transfer","candidateSessionId":"candidate-1","reason":"transferRejected"})");
    event.method = "callState";
    mqtt.down(event);
    client.poll(111, 1'000'000);
    EXPECT_EQ(client.state(), HumanCallState::Connected);
    EXPECT_EQ(client.session_id(), "call-1");
    EXPECT_TRUE(media.started);
    EXPECT_EQ(media.closes, closes);
    EXPECT_EQ(media.hold_calls, 0);
}
// transfer_event 构造现Go固定转接事件；新事件token与原requestToken严格分开。
Envelope transfer_event(const Envelope& request, std::string_view state, std::uint64_t revision,
                        std::uint64_t sequence, std::string_view reason = {}) {
    Envelope event;
    event.request = false;
    event.method = "callState";
    event.token = "transfer-event-" + std::to_string(sequence);
    event.session = request.session;
    event.event_sequence = sequence;
    event.payload = "{\"state\":\"" + std::string(state) +
                    "\",\"revision\":" + std::to_string(revision) + ",\"requestToken\":\"" +
                    request.token + "\",\"candidateSessionId\":\"candidate-1\"";
    if (!reason.empty())
        event.payload += ",\"reason\":\"" + std::string(reason) + "\"";
    event.payload += "}";
    return event;
}
// transfer_ack 只确认平台已分配原候选，不能代替候选接听、媒体建立或原D退出。
Envelope transfer_ack(const Envelope& request) {
    return reply(request.token, "callState", request.session,
                 R"({"state":"transferring","revision":1,"candidateSessionId":"candidate-1"})");
}
// TransferIntent 固定目标和修订走原Requests，一次受理不重建或保持原Peer。
TEST(HumanCallTransfer, IntentAndAcceptanceKeepOriginalMedia) {
    TestCallMqtt mqtt;
    TestCallMedia media;
    HumanCallClient client(mqtt, media, "p/d", "transfer-intent");
    ASSERT_NO_FATAL_FAILURE(connected_client(client, mqtt, media));
    const auto begins = media.begins, closes = media.closes, answers = media.answers;
    const auto sent = mqtt.sent.size();
    for (const auto* invalid : {"", "p", ":d", "p:", "p/+", "p:#", "p:/d", "p:d\n"})
        EXPECT_FALSE(client.transfer_call(invalid, 111));
    EXPECT_EQ(mqtt.sent.size(), sent);
    ASSERT_TRUE(client.transfer_call("p:third", 111));
    const auto original = mqtt.sent.back();
    EXPECT_EQ(original.method, "callControl");
    EXPECT_EQ(original.session, "call-1");
    EXPECT_EQ(mqtt.topics.back(), "$thing/up/signaling/p/d");
    EXPECT_EQ(
        original.payload,
        R"({"expectedRevision":0,"operation":"transfer","target":{"id":"p:third","kind":"device"}})");
    EXPECT_FALSE(client.transfer_call("p:other", 112));
    EXPECT_FALSE(client.cancel_transfer(112));
    mqtt.down(transfer_ack(original));
    mqtt.down(transfer_ack(original));
    client.poll(112, 1'000'000);
    const auto fact = client.transfer_receipt();
    EXPECT_EQ(fact.state, HumanCallTransferState::Transferring);
    EXPECT_EQ(fact.ack_code, 200);
    EXPECT_EQ(fact.ack_revision, 1U);
    EXPECT_EQ(fact.candidate, "candidate-1");
    EXPECT_EQ(client.call_revision(), 1U);
    client.poll(20'112, 1'000'000);
    EXPECT_EQ(mqtt.sent.size(), sent + 1);
    EXPECT_TRUE(client.active());
    EXPECT_EQ(media.begins, begins);
    EXPECT_EQ(media.answers, answers);
    EXPECT_EQ(media.closes, closes);
    EXPECT_EQ(media.hold_calls, 0);
}
// 私有缺水位是故障注入，不能用正常parser拒绝代替；原合法序号之后仍可完成原操作。
TEST(HumanCallTransferFault, MissingSequenceCannotAdvanceRevisionOrTouchMedia) {
    TestCallMqtt mqtt;
    TestCallMedia media;
    HumanCallClient client(mqtt, media, "p/d", "transfer-fault");
    ASSERT_NO_FATAL_FAILURE(connected_client(client, mqtt, media));
    ASSERT_TRUE(client.transfer_call("p:third", 111));
    const auto original = mqtt.sent.back();
    auto completed = transfer_event(original, "transferred", 2, 3);
    completed.event_sequence.reset();
    const auto sequence = HumanCallStaticFaultTest::sequence(client);
    const auto revision = client.call_revision();
    const auto begins = media.begins, closes = media.closes, holds = media.hold_calls;
    bool handled = false;
    EXPECT_FALSE(HumanCallStaticFaultTest::receive_transfer(client, completed, 112, handled));
    EXPECT_TRUE(handled);
    EXPECT_EQ(HumanCallStaticFaultTest::sequence(client), sequence);
    EXPECT_EQ(client.call_revision(), revision);
    EXPECT_EQ(media.begins, begins);
    EXPECT_EQ(media.closes, closes);
    EXPECT_EQ(media.hold_calls, holds);
    EXPECT_EQ(client.transfer_receipt().state, HumanCallTransferState::Pending);
    mqtt.down(transfer_event(original, "transferred", 2, 3));
    client.poll(112, 1'000'000);
    EXPECT_EQ(client.transfer_receipt().state, HumanCallTransferState::Transferred);
    EXPECT_EQ(client.call_revision(), std::uint64_t{2});
}

// TransferCompletion
// 原完成和ACK可乱序；transferred不是sessionEnded，只有原结束事件释放D。
TEST(HumanCallTransfer, CompletionOrdersWaitForOriginalEnded) {
    for (const bool event_first : {false, true}) {
        TestCallMqtt mqtt;
        TestCallMedia media;
        HumanCallClient client(mqtt, media, "p/d", "transfer-complete");
        ASSERT_NO_FATAL_FAILURE(connected_client(client, mqtt, media));
        ASSERT_TRUE(client.transfer_call("p:third", 111));
        const auto original = mqtt.sent.back();
        const auto ack = transfer_ack(original);
        const auto completed = transfer_event(original, "transferred", 2, 3);
        const auto closes = media.closes;
        mqtt.down(event_first ? completed : ack);
        client.poll(112, 1'000'000);
        EXPECT_TRUE(client.active());
        EXPECT_TRUE(media.started);
        EXPECT_EQ(media.closes, closes);
        if (event_first) {
            EXPECT_EQ(client.transfer_receipt().ack_code, 0);
        }
        mqtt.down(event_first ? ack : completed);
        client.poll(113, 1'000'000);
        EXPECT_EQ(client.transfer_receipt().state, HumanCallTransferState::Transferred);
        EXPECT_EQ(client.transfer_receipt().ack_code, 200);
        EXPECT_EQ(client.transfer_receipt().result_revision, 2U);
        EXPECT_EQ(client.call_revision(), 2U);
        mqtt.down(ack);
        mqtt.down(completed);
        client.poll(114, 1'000'000);
        EXPECT_EQ(media.closes, closes);
        auto ended = participant_event(4, R"({"state":"ended","reason":"transferred"})");
        ended.method = "sessionEnded";
        mqtt.down(ended);
        client.poll(115, 1'000'000);
        EXPECT_EQ(client.state(), HumanCallState::Available);
        EXPECT_EQ(media.closes, closes + 1);
        EXPECT_FALSE(media.started);
        auto next = invite();
        next.token = "after-transfer-invite";
        next.session = "after-transfer-leg";
        mqtt.down(next);
        client.poll(116, 1'000'000);
        ASSERT_EQ(client.state(), HumanCallState::Ringing);
        mqtt.down(ack);
        mqtt.down(completed);
        mqtt.down(ended);
        client.poll(117, 1'000'000);
        EXPECT_EQ(client.state(), HumanCallState::Ringing);
        EXPECT_EQ(client.session_id(), "after-transfer-leg");
        EXPECT_EQ(media.closes, closes + 1);
    }
}
// TransferFailure
// 六种真实固定操作失败保留原端口；原组中途被对端保持时更高revision不隐式恢复。
TEST(HumanCallTransfer, FailureReasonsAndHeldRevisionPreserveOriginal) {
    for (const auto* reason :
         {"transferRejected", "transferTimeout", "transferMediaFailed", "transferUnavailable",
          "transferRevisionChanged", "transferPermissionRevoked"}) {
        SCOPED_TRACE(reason);
        TestCallMqtt mqtt;
        TestCallMedia media;
        HumanCallClient client(mqtt, media, "p/d", "transfer-failure");
        ASSERT_NO_FATAL_FAILURE(connected_client(client, mqtt, media));
        ASSERT_TRUE(client.transfer_call("p:third", 111));
        const auto original = mqtt.sent.back();
        mqtt.down(transfer_ack(original));
        client.poll(112, 1'000'000);
        mqtt.down(control_event("remote-hold", "held", 2, 3));
        client.poll(113, 1'000'000);
        ASSERT_TRUE(client.call_held());
        const auto closes = media.closes, begins = media.begins, holds = media.hold_calls;
        const auto failure = transfer_event(original, "failed", 3, 4, reason);
        mqtt.down(failure);
        mqtt.down(failure);
        client.poll(114, 1'000'000);
        EXPECT_EQ(client.transfer_receipt().state, HumanCallTransferState::Failed);
        EXPECT_EQ(client.transfer_receipt().reason, reason);
        EXPECT_TRUE(client.active());
        EXPECT_TRUE(client.call_held());
        EXPECT_EQ(client.call_revision(), 3U);
        EXPECT_EQ(media.closes, closes);
        EXPECT_EQ(media.begins, begins);
        EXPECT_EQ(media.hold_calls, holds);
    }
}
// TransferCancel 撤原候选的ACK与事件独立，不能冒普通cancel关闭原主通话。
TEST(HumanCallTransfer, CandidateCancelAckAndEventBothOrders) {
    for (const bool event_first : {false, true}) {
        TestCallMqtt mqtt;
        TestCallMedia media;
        HumanCallClient client(mqtt, media, "p/d", "transfer-cancel");
        ASSERT_NO_FATAL_FAILURE(connected_client(client, mqtt, media));
        ASSERT_TRUE(client.transfer_call("p:third", 111));
        const auto transfer = mqtt.sent.back();
        mqtt.down(transfer_ack(transfer));
        client.poll(112, 1'000'000);
        ASSERT_TRUE(client.cancel_transfer(113));
        const auto cancel = mqtt.sent.back();
        EXPECT_NE(cancel.token, transfer.token);
        EXPECT_EQ(cancel.session, transfer.session);
        EXPECT_EQ(
            cancel.payload,
            R"({"candidateSessionId":"candidate-1","expectedRevision":1,"operation":"cancel"})");
        EXPECT_FALSE(client.cancel_transfer(114));
        const auto ack = reply(
            cancel.token, "callState", cancel.session,
            R"({"state":"active","revision":2,"candidateSessionId":"candidate-1","reason":"candidateCancelled"})");
        const auto event = transfer_event(cancel, "active", 2, 3, "candidateCancelled");
        const auto closes = media.closes, begins = media.begins;
        mqtt.down(event_first ? event : ack);
        client.poll(114, 1'000'000);
        EXPECT_EQ(client.transfer_receipt().cancel_state, HumanCallControlState::Pending);
        mqtt.down(event_first ? ack : event);
        client.poll(115, 1'000'000);
        EXPECT_EQ(client.transfer_receipt().state, HumanCallTransferState::Cancelled);
        EXPECT_EQ(client.transfer_receipt().cancel_state, HumanCallControlState::Completed);
        EXPECT_EQ(client.transfer_receipt().cancel_code, 200);
        mqtt.down(ack);
        mqtt.down(event);
        mqtt.down(transfer_ack(transfer));
        client.poll(116, 1'000'000);
        EXPECT_TRUE(client.active());
        EXPECT_EQ(client.session_id(), "call-1");
        EXPECT_EQ(media.begins, begins);
        EXPECT_EQ(media.closes, closes);
        EXPECT_EQ(media.hold_calls, 0);
    }
}
// TransferDeadline
// 10秒未知先于默认重传，45秒从首次意图起算，迟到与撤候选不能延长。
TEST(HumanCallTransfer, OriginalAcceptanceAndCompletionDeadlines) {
    for (const bool accepted : {false, true}) {
        TestCallMqtt mqtt;
        TestCallMedia media;
        HumanCallClient client(mqtt, media, "p/d", "transfer-clock");
        ASSERT_NO_FATAL_FAILURE(connected_client(client, mqtt, media));
        ASSERT_TRUE(client.transfer_call("p:third", 111));
        const auto original = mqtt.sent.back();
        const auto sent = mqtt.sent.size();
        if (accepted) {
            mqtt.down(transfer_ack(original));
            client.poll(10'110, 1'000'000);
            client.poll(45'110, 1'000'000);
            EXPECT_EQ(client.transfer_receipt().state, HumanCallTransferState::Transferring);
            ASSERT_TRUE(client.cancel_transfer(45'110));
            client.poll(45'111, 1'000'000);
            EXPECT_EQ(client.transfer_receipt().cancel_state, HumanCallControlState::Unknown);
        } else {
            client.poll(10'110, 1'000'000);
            EXPECT_EQ(client.transfer_receipt().state, HumanCallTransferState::Pending);
            client.poll(10'111, 1'000'000);
            mqtt.down(transfer_ack(original));
            client.poll(10'112, 1'000'000);
            EXPECT_EQ(client.transfer_receipt().ack_code, 0);
        }
        EXPECT_EQ(client.transfer_receipt().state, HumanCallTransferState::Unknown);
        client.poll(70'111, 1'000'000);
        EXPECT_EQ(mqtt.sent.size(), sent + (accepted ? 1U : 0U));
        EXPECT_TRUE(client.active());
        EXPECT_TRUE(media.started);
        EXPECT_FALSE(client.transfer_call("p:another", 70'112));
        EXPECT_FALSE(client.cancel_transfer(70'112));
    }
}
// TransferRejected
// 原typed拒绝只结束操作，仍可接明确新意图；候选取消409也不清原正在转接候选。
TEST(HumanCallTransfer, TypedRejectionsPreserveOriginalScope) {
    for (const bool cancel : {false, true}) {
        TestCallMqtt mqtt;
        TestCallMedia media;
        HumanCallClient client(mqtt, media, "p/d", "transfer-reject");
        ASSERT_NO_FATAL_FAILURE(connected_client(client, mqtt, media));
        ASSERT_TRUE(client.transfer_call("p:third", 111));
        auto original = mqtt.sent.back();
        if (cancel) {
            mqtt.down(transfer_ack(original));
            client.poll(112, 1'000'000);
            ASSERT_TRUE(client.cancel_transfer(113));
            original = mqtt.sent.back();
        }
        const auto closes = media.closes;
        auto denied = reply(original.token, "callState", original.session,
                            cancel ? R"({"reason":"invalidState","currentRevision":1})"
                                   : R"({"reason":"staleRevision","currentRevision":3})");
        denied.code = 409;
        mqtt.down(denied);
        mqtt.down(denied);
        client.poll(114, 1'000'000);
        EXPECT_TRUE(client.active());
        EXPECT_EQ(media.closes, closes);
        if (cancel) {
            EXPECT_EQ(client.transfer_receipt().cancel_state, HumanCallControlState::Rejected);
            EXPECT_EQ(client.transfer_receipt().cancel_reason, "invalidState");
            EXPECT_EQ(client.transfer_receipt().state, HumanCallTransferState::Transferring);
        } else {
            EXPECT_EQ(client.transfer_receipt().state, HumanCallTransferState::Rejected);
            EXPECT_EQ(client.call_revision(), 3U);
            EXPECT_TRUE(client.transfer_call("p:another", 115));
            EXPECT_NE(mqtt.sent.back().token, original.token);
        }
    }
}
// TransferRetire
// 本端挂断/关闭取消原控制账本，旧腿消息不会重新派发转接或影响新邀请。
TEST(HumanCallTransfer, CloseAndLocalEndRetireOriginalRequests) {
    for (const bool close : {false, true}) {
        TestCallMqtt mqtt;
        TestCallMedia media;
        HumanCallClient client(mqtt, media, "p/d", "transfer-retire");
        ASSERT_NO_FATAL_FAILURE(connected_client(client, mqtt, media));
        ASSERT_TRUE(client.transfer_call("p:third", 111));
        const auto original = mqtt.sent.back();
        if (close)
            client.close();
        else {
            ASSERT_TRUE(client.end(112));
            mqtt.down(reply(mqtt.sent.back().token, "sessionEnded", "call-1",
                            R"({"state":"ended","reason":"hangup"})"));
            client.poll(113, 1'000'000);
        }
        const auto sent = mqtt.sent.size();
        client.poll(30'111, 1'000'000);
        EXPECT_EQ(mqtt.sent.size(), sent);
        EXPECT_FALSE(media.started);
        if (!close) {
            auto next = invite();
            next.session = "new-leg";
            next.token = "new-invite";
            mqtt.down(next);
            client.poll(30'112, 1'000'000);
            mqtt.down(transfer_ack(original));
            mqtt.down(transfer_event(original, "transferred", 2, 3));
            client.poll(30'113, 1'000'000);
            EXPECT_EQ(client.state(), HumanCallState::Ringing);
            EXPECT_EQ(client.session_id(), "new-leg");
        }
    }
}
// TransferRetained
// 同原D2仅收原active，不重建端口，不将非本端requestToken误判为越权控制。
TEST(HumanCallTransfer, RetainedActiveDoesNotRestartOriginalPeer) {
    TestCallMqtt mqtt;
    TestCallMedia media;
    HumanCallClient client(mqtt, media, "p/d", "transfer-retained");
    ASSERT_NO_FATAL_FAILURE(connected_client(client, mqtt, media));
    const auto begins = media.begins, answers = media.answers, closes = media.closes;
    mqtt.down(control_event("remote-transfer", "active", 2, 3));
    client.poll(111, 1'000'000);
    EXPECT_TRUE(client.active());
    EXPECT_EQ(client.session_id(), "call-1");
    EXPECT_EQ(media.begins, begins);
    EXPECT_EQ(media.answers, answers);
    EXPECT_EQ(media.closes, closes);
    EXPECT_EQ(media.hold_calls, 0);
    EXPECT_EQ(client.call_revision(), 2U);
}
// TransferCandidate
// 独立新腿仍需原accept/answer、真实驱动连通与ACK/established，不从转接完成取得采集许可。
TEST(HumanCallTransfer, CandidateRequiresEntireNewLegHandshake) {
    TestCallMqtt mqtt;
    TestCallMedia media;
    HumanCallClient client(mqtt, media, "p/d", "transfer-candidate");
    ASSERT_NO_FATAL_FAILURE(ready_client(client, mqtt));
    auto incoming = invite();
    incoming.session = "candidate-1";
    incoming.token = "candidate-invite";
    mqtt.down(incoming);
    client.poll(104, 1'000'000);
    ASSERT_EQ(client.state(), HumanCallState::Ringing);
    EXPECT_FALSE(media.started);
    ASSERT_TRUE(client.accept(105));
    EXPECT_FALSE(media.started);
    mqtt.down(reply(incoming.token, "requestAck", incoming.session,
                    R"({"requestMethod":"sessionAccept","result":{"state":"negotiating"}})"));
    client.poll(106, 1'000'000);
    ASSERT_TRUE(media.started);
    ASSERT_TRUE(client.offer_ready("v=0\r\no=candidate\r\n", 107));
    mqtt.down(reply(
        mqtt.sent.back().token, "mediaAnswer", incoming.session,
        R"({"mediaRevision":1,"webrtc":{"iceMode":"complete","sdp":"v=0\r\no=server\r\n"}})"));
    client.poll(108, 1'000'000);
    EXPECT_FALSE(client.media_connected(109));
    media.ready = true;
    ASSERT_TRUE(client.media_connected(109));
    mqtt.down(reply(
        mqtt.sent.back().token, "requestAck", incoming.session,
        R"({"requestMethod":"mediaState","result":{"mediaRevision":1,"state":"connected"}})"));
    client.poll(110, 1'000'000);
    EXPECT_FALSE(client.active());
    auto established = participant_event(2, R"({"state":"established","mediaRevision":1})");
    established.session = incoming.session;
    established.method = "sessionEstablished";
    mqtt.down(established);
    auto active = control_event("original-transfer", "active", 2, 3);
    active.session = incoming.session;
    mqtt.down(active);
    client.poll(111, 1'000'000);
    EXPECT_TRUE(client.active());
    EXPECT_EQ(client.session_id(), "candidate-1");
    EXPECT_EQ(media.begins, 1);
    EXPECT_EQ(media.hold_calls, 0);
    EXPECT_EQ(client.transfer_receipt().state, HumanCallTransferState::Idle);
}
// TransferBinding 错候选/版本/方法类型等不得被当作转接成功或触发另一个媒体端点。
TEST(HumanCallTransfer, InvalidBindingsFailBeforeTransferEffect) {
    for (unsigned variant = 0; variant < 9; ++variant) {
        SCOPED_TRACE(variant);
        TestCallMqtt mqtt;
        TestCallMedia media;
        HumanCallClient client(mqtt, media, "p/d", "transfer-binding");
        ASSERT_NO_FATAL_FAILURE(connected_client(client, mqtt, media));
        ASSERT_TRUE(client.transfer_call("p:third", 111));
        const auto original = mqtt.sent.back();
        mqtt.down(transfer_ack(original));
        client.poll(112, 1'000'000);
        auto event = transfer_event(original, "transferred", 2, 3);
        if (variant == 0)
            event.payload =
                R"({"state":"transferred","revision":2,"requestToken":"foreign","candidateSessionId":"candidate-1"})";
        if (variant == 1)
            event.payload = "{\"state\":\"transferred\",\"revision\":2,\"requestToken\":\"" +
                            original.token + "\",\"candidateSessionId\":\"other-candidate\"}";
        if (variant == 2)
            event = transfer_event(original, "transferred", 3, 3);
        if (variant == 3)
            event = transfer_event(original, "failed", 2, 3, "unknownFailure");
        if (variant == 4)
            event.code = 200;
        if (variant == 6)
            event.payload.insert(event.payload.size() - 1, ",\"extra\":true");
        if (variant == 7)
            event = transfer_event(original, "failed", 1, 3, "transferRejected");
        const auto begins = media.begins, answers = media.answers, holds = media.hold_calls;
        if (variant == 5 || variant == 8) {
            // 编码器也拒绝非法序列/大整数；变异合法原报文后由真实poll解码边界拒绝。
            std::string body;
            ASSERT_EQ(encode_envelope(event, body), DecodeResult::Ok);
            const std::string original_field = variant == 5 ? "\"eventSeq\":3" : "\"revision\":2";
            const auto position = body.find(original_field);
            ASSERT_NE(position, std::string::npos);
            body.replace(
                position, original_field.size(),
                variant == 5 ? "\"eventSeq\":3,\"messageSeq\":1" : "\"revision\":9007199254740992");
            mqtt.events.push_back(
                {MqttEvent::Kind::Message, 0, 0, "$thing/down/signaling/p/d", std::move(body)});
        } else {
            mqtt.down(event);
        }
        client.poll(113, 1'000'000);
        EXPECT_EQ(client.state(), HumanCallState::Failed);
        EXPECT_EQ(media.begins, begins);
        EXPECT_EQ(media.answers, answers);
        EXPECT_EQ(media.hold_calls, holds);
    }
}
// TransferGeneration 旧候选ACK/事件和旧45秒到期不能撤新代次，撤候选请求只带新ACK绑定。
TEST(HumanCallTransfer, OldCandidateCannotRetireNextGeneration) {
    TestCallMqtt mqtt;
    TestCallMedia media;
    HumanCallClient client(mqtt, media, "p/d", "transfer-generation");
    ASSERT_NO_FATAL_FAILURE(connected_client(client, mqtt, media));
    ASSERT_TRUE(client.transfer_call("p:third", 111));
    const auto first = mqtt.sent.back();
    mqtt.down(transfer_ack(first));
    client.poll(112, 1'000'000);
    ASSERT_TRUE(client.cancel_transfer(113));
    const auto old_cancel = mqtt.sent.back();
    const auto old_ack = reply(
        old_cancel.token, "callState", old_cancel.session,
        R"({"state":"active","revision":2,"candidateSessionId":"candidate-1","reason":"candidateCancelled"})");
    const auto old_event = transfer_event(old_cancel, "active", 2, 3, "candidateCancelled");
    mqtt.down(old_ack);
    mqtt.down(old_event);
    client.poll(114, 1'000'000);
    ASSERT_TRUE(client.transfer_call("p:fourth", 115));
    const auto next = mqtt.sent.back();
    EXPECT_NE(next.token, first.token);
    mqtt.down(reply(next.token, "callState", next.session,
                    R"({"state":"transferring","revision":3,"candidateSessionId":"candidate-2"})"));
    client.poll(116, 1'000'000);
    mqtt.down(transfer_ack(first));
    mqtt.down(old_ack);
    mqtt.down(old_event);
    client.poll(117, 1'000'000);
    EXPECT_EQ(client.transfer_receipt().token, next.token);
    EXPECT_EQ(client.transfer_receipt().candidate, "candidate-2");
    EXPECT_EQ(client.transfer_receipt().state, HumanCallTransferState::Transferring);
    client.poll(45'112, 1'000'000);
    EXPECT_EQ(client.transfer_receipt().state, HumanCallTransferState::Transferring);
    ASSERT_TRUE(client.cancel_transfer(45'113));
    EXPECT_EQ(mqtt.sent.back().payload,
              R"({"candidateSessionId":"candidate-2","expectedRevision":3,"operation":"cancel"})");
    EXPECT_TRUE(client.active());
}
// TransferLate 原10秒之后的受理或完成不补造成功；独立原sessionEnded仍须结束真实旧腿。
TEST(HumanCallTransfer, LateCompletionDoesNotResurrectUnknown) {
    TestCallMqtt mqtt;
    TestCallMedia media;
    HumanCallClient client(mqtt, media, "p/d", "transfer-late");
    ASSERT_NO_FATAL_FAILURE(connected_client(client, mqtt, media));
    ASSERT_TRUE(client.transfer_call("p:third", 111));
    const auto original = mqtt.sent.back();
    client.poll(10'111, 1'000'000);
    mqtt.down(transfer_ack(original));
    mqtt.down(transfer_event(original, "transferred", 2, 3));
    client.poll(10'112, 1'000'000);
    EXPECT_EQ(client.transfer_receipt().state, HumanCallTransferState::Unknown);
    EXPECT_EQ(client.transfer_receipt().ack_code, 0);
    EXPECT_TRUE(client.active());
    const auto closes = media.closes;
    auto ended = participant_event(4, R"({"state":"ended","reason":"transferred"})");
    ended.method = "sessionEnded";
    mqtt.down(ended);
    client.poll(10'113, 1'000'000);
    EXPECT_EQ(client.state(), HumanCallState::Available);
    EXPECT_EQ(media.closes, closes + 1);
    EXPECT_FALSE(media.started);
}
// TransferScope 非本端旧session保持隔离；留存方合法candidateCancelled也是操作结果，不是媒体resume。
TEST(HumanCallTransfer, ForeignSessionAndRetainedCancelDoNotAffectMedia) {
    TestCallMqtt mqtt;
    TestCallMedia media;
    HumanCallClient client(mqtt, media, "p/d", "transfer-scope");
    ASSERT_NO_FATAL_FAILURE(connected_client(client, mqtt, media));
    const auto begins = media.begins, closes = media.closes;
    Envelope remote;
    remote.session = "call-1";
    remote.token = "remote-cancel";
    auto old = transfer_event(remote, "active", 2, 3, "candidateCancelled");
    old.session = "other-leg";
    mqtt.down(old);
    client.poll(111, 1'000'000);
    EXPECT_EQ(client.call_revision(), 0U);
    mqtt.down(transfer_event(remote, "active", 2, 3, "candidateCancelled"));
    client.poll(112, 1'000'000);
    EXPECT_TRUE(client.active());
    EXPECT_EQ(client.call_revision(), 2U);
    EXPECT_EQ(media.begins, begins);
    EXPECT_EQ(media.closes, closes);
    EXPECT_EQ(media.hold_calls, 0);
}
// TransferObserver 每个原状态观察一次，不从只读回调重发控制；完成后仍等待原腿结束。
TEST(HumanCallTransfer, ObserverAndBusyDoNotPerformOrRestartMedia) {
    TestCallMqtt mqtt;
    TestCallMedia media;
    HumanCallClient client(mqtt, media, "p/d", "transfer-observer");
    std::vector<HumanCallTransferReceipt> observed;
    client.observe_transfer(
        [&](const HumanCallTransferReceipt& value) { observed.push_back(value); });
    ASSERT_NO_FATAL_FAILURE(connected_client(client, mqtt, media));
    const auto begins = media.begins, closes = media.closes;
    ASSERT_TRUE(client.transfer_call("p:third", 111));
    const auto original = mqtt.sent.back();
    ASSERT_EQ(observed.size(), 1U);
    EXPECT_EQ(observed.back().state, HumanCallTransferState::Pending);
    EXPECT_EQ(observed.back().token, original.token);
    EXPECT_FALSE(client.control_call("hold", 112));
    mqtt.down(transfer_ack(original));
    client.poll(113, 1'000'000);
    EXPECT_FALSE(client.transfer_call("p:fourth", 114));
    EXPECT_FALSE(client.control_call("hold", 114));
    mqtt.down(transfer_event(original, "transferred", 2, 3));
    client.poll(115, 1'000'000);
    EXPECT_FALSE(client.transfer_call("p:fourth", 116));
    EXPECT_FALSE(client.control_call("hold", 116));
    ASSERT_EQ(observed.size(), 3U);
    EXPECT_EQ(observed.back().state, HumanCallTransferState::Transferred);
    EXPECT_EQ(observed.back().ack_code, 200);
    EXPECT_EQ(observed.back().result_revision, 2U);
    EXPECT_EQ(media.begins, begins);
    EXPECT_EQ(media.closes, closes);
    EXPECT_EQ(media.hold_calls, 0);
    EXPECT_TRUE(client.active());
}
// TransferEntry 业务入口拒绝保留当前媒体；不以空对象证明没有发送或重建。
TEST(HumanCallTransfer, EntryGatesRejectBeforeOriginalRequests) {
    for (unsigned variant = 0; variant < 9; ++variant) {
        SCOPED_TRACE(variant);
        TestCallMqtt mqtt;
        TestCallMedia media;
        HumanCallClient client(mqtt, media, "p/d", "transfer-entry");
        ASSERT_NO_FATAL_FAILURE(connected_client(client, mqtt, media));
        std::uint64_t now = 113;
        if (variant == 0 || variant == 1) {
            media.async_supported = variant == 1;
            mqtt.down(control_event("remote-hold", "held", 1, 3));
            client.poll(112, 1'000'000);
        } else if (variant == 2 || variant == 3) {
            ASSERT_TRUE(client.control_call("hold", 111));
            if (variant == 3) {
                now = 10'112;
                client.poll(now, 1'000'000);
            }
        } else if (variant == 4) {
            mqtt.down(control_event("remote-version", "active", 9007199254740990ULL, 3));
            client.poll(112, 1'000'000);
        } else if (variant == 5) {
            now = 109;
        } else if (variant == 6) {
            ASSERT_TRUE(client.end(111));
        } else if (variant == 7) {
            client.close();
        }
        const auto sent = mqtt.sent.size();
        const auto begins = media.begins, closes = media.closes, holds = media.hold_calls;
        const auto session = client.session_id();
        EXPECT_FALSE(
            client.transfer_call(variant == 8 ? "p:" + std::string(127, 'x') : "p:third", now));
        EXPECT_FALSE(client.cancel_transfer(now));
        EXPECT_EQ(client.session_id(), session);
        EXPECT_EQ(mqtt.sent.size(), sent);
        EXPECT_EQ(media.begins, begins);
        EXPECT_EQ(media.closes, closes);
        EXPECT_EQ(media.hold_calls, holds);
        EXPECT_EQ(client.transfer_receipt().state, HumanCallTransferState::Idle);
    }
}
// TransferCapacity 原24条控制历史不因转接改变预算，拒绝不驱动媒体或替换原token。
TEST(HumanCallTransfer, RejectionHistoryUsesOriginalBoundedControlBudget) {
    TestCallMqtt mqtt;
    TestCallMedia media;
    HumanCallClient client(mqtt, media, "p/d", "transfer-capacity");
    ASSERT_NO_FATAL_FAILURE(connected_client(client, mqtt, media));
    for (unsigned index = 0; index < 24; ++index) {
        ASSERT_TRUE(client.transfer_call("p:third", 111 + index * 2));
        auto denied = reply(mqtt.sent.back().token, "callState", "call-1",
                            R"({"reason":"staleRevision","currentRevision":0})");
        denied.code = 409;
        mqtt.down(denied);
        client.poll(112 + index * 2, 1'000'000);
        ASSERT_EQ(client.transfer_receipt().state, HumanCallTransferState::Rejected);
        EXPECT_TRUE(client.active());
    }
    const auto sent = mqtt.sent.size();
    const auto token = client.transfer_receipt().token;
    EXPECT_FALSE(client.transfer_call("p:fourth", 160));
    EXPECT_FALSE(client.control_call("hold", 160));
    EXPECT_FALSE(client.cancel_transfer(160));
    EXPECT_EQ(mqtt.sent.size(), sent);
    EXPECT_EQ(client.transfer_receipt().token, token);
    EXPECT_EQ(media.begins, 1);
    EXPECT_EQ(media.hold_calls, 0);
    EXPECT_TRUE(client.active());
}
// TransferAckShape 原transfer与撤候选ACK均严格解码；失败关闭资源但不产生新副作用。
TEST(HumanCallTransfer, MalformedAcceptanceAndCancellationAckFailClosed) {
    for (const bool cancel : {false, true}) {
        for (unsigned variant = 0; variant < 12; ++variant) {
            SCOPED_TRACE(cancel);
            SCOPED_TRACE(variant);
            TestCallMqtt mqtt;
            TestCallMedia media;
            HumanCallClient client(mqtt, media, "p/d", "transfer-ack-shape");
            ASSERT_NO_FATAL_FAILURE(connected_client(client, mqtt, media));
            ASSERT_TRUE(client.transfer_call("p:third", 111));
            auto original = mqtt.sent.back();
            if (cancel) {
                mqtt.down(transfer_ack(original));
                client.poll(112, 1'000'000);
                ASSERT_TRUE(client.cancel_transfer(113));
                original = mqtt.sent.back();
            }
            const std::string state = cancel ? "active" : "transferring";
            const std::string reason = cancel ? ",\"reason\":\"candidateCancelled\"" : "";
            const auto revision = cancel ? 2 : 1;
            std::string data = "{\"state\":\"" + state +
                               "\",\"revision\":" + std::to_string(revision) +
                               ",\"candidateSessionId\":\"candidate-1\"" + reason + "}";
            if (variant == 0)
                data =
                    "{\"state\":\"unknown\",\"revision\":1,\"candidateSessionId\":\"candidate-1\"}";
            if (variant == 1)
                data = "{\"state\":\"" + state +
                       "\",\"revision\":0,\"candidateSessionId\":\"candidate-1\"" + reason + "}";
            if (variant == 2)
                data = "{\"state\":\"" + state +
                       "\",\"revision\":\"1\",\"candidateSessionId\":\"candidate-1\"" + reason +
                       "}";
            if (variant == 3)
                data = "{\"state\":\"" + state +
                       "\",\"revision\":4,\"candidateSessionId\":\"candidate-1\"" + reason + "}";
            if (variant == 4)
                data = "{\"state\":\"" + state + "\",\"revision\":" + std::to_string(revision) +
                       ",\"candidateSessionId\":\"call-1\"" + reason + "}";
            if (variant == 5)
                data = "{\"state\":\"" + state + "\",\"revision\":" + std::to_string(revision) +
                       ",\"candidateSessionId\":\"\"" + reason + "}";
            if (variant == 6)
                data.insert(data.size() - 1, ",\"extra\":true");
            if (variant == 7)
                data = R"({"reason":"otherConflict","currentRevision":0})";
            if (variant == 8)
                data = R"({"reason":"staleRevision","currentRevision":"0"})";
            if (variant == 9)
                data = R"({"state":"rejected"})";
            if (variant == 10)
                data = R"({"reason":"serviceUnavailable","extra":true})";
            if (variant == 11 && cancel)
                data =
                    R"({"state":"active","revision":2,"candidateSessionId":"other","reason":"candidateCancelled"})";
            auto ack = reply(original.token, "callState", original.session, data);
            if (variant == 7 || variant == 8)
                ack.code = 409;
            if (variant == 9 || variant == 10)
                ack.code = 503;
            if (variant == 11 && !cancel)
                ack.code = 201;
            const auto sent = mqtt.sent.size();
            const auto begins = media.begins, holds = media.hold_calls, closes = media.closes;
            mqtt.down(ack);
            client.poll(114, 1'000'000);
            EXPECT_EQ(client.state(), HumanCallState::Failed);
            EXPECT_EQ(media.begins, begins);
            EXPECT_EQ(media.hold_calls, holds);
            EXPECT_EQ(media.closes, closes + 1);
            EXPECT_EQ(mqtt.sent.size(), sent);
        }
    }
}
// TransferGenericReject 原reason-only拒绝投影固定原因，重复ACK不二次回调、不影响原媒体。
TEST(HumanCallTransfer, GenericRejectionAndConflictKeepAcceptanceTruth) {
    for (const bool cancel : {false, true}) {
        TestCallMqtt mqtt;
        TestCallMedia media;
        HumanCallClient client(mqtt, media, "p/d", "transfer-generic");
        std::vector<HumanCallTransferReceipt> observed;
        client.observe_transfer(
            [&](const HumanCallTransferReceipt& value) { observed.push_back(value); });
        ASSERT_NO_FATAL_FAILURE(connected_client(client, mqtt, media));
        ASSERT_TRUE(client.transfer_call("p:third", 111));
        auto original = mqtt.sent.back();
        if (cancel) {
            mqtt.down(transfer_ack(original));
            client.poll(112, 1'000'000);
            ASSERT_TRUE(client.cancel_transfer(113));
            original = mqtt.sent.back();
        }
        auto denied = reply(original.token, "callState", original.session,
                            R"({"reason":"serviceUnavailable"})");
        denied.code = 503;
        mqtt.down(denied);
        client.poll(114, 1'000'000);
        ASSERT_TRUE(client.active());
        const auto count = observed.size();
        const auto fact = client.transfer_receipt();
        EXPECT_EQ(cancel ? fact.cancel_reason : fact.reason, "rejected");
        EXPECT_EQ(cancel ? fact.cancel_code : fact.ack_code, 503);
        mqtt.down(denied);
        client.poll(115, 1'000'000);
        EXPECT_EQ(observed.size(), count);
        EXPECT_TRUE(client.active());
        // 同token不同拒绝正文不是幂等重放，即使状态码相同仍fail closed。
        denied.payload = R"({"reason":"differentFailure"})";
        mqtt.down(denied);
        client.poll(116, 1'000'000);
        EXPECT_EQ(client.state(), HumanCallState::Failed);
        EXPECT_EQ(observed.size(), count);
        EXPECT_EQ(media.hold_calls, 0);
    }
}
// TransferEventBeforeAck 操作失败先到不伪造受理；原ACK截止后显式新意图仍独立。
TEST(HumanCallTransfer, FailedEventBeforeAckKeepsBusyUntilOriginalAckDeadline) {
    TestCallMqtt mqtt;
    TestCallMedia media;
    HumanCallClient client(mqtt, media, "p/d", "transfer-event-first");
    ASSERT_NO_FATAL_FAILURE(connected_client(client, mqtt, media));
    ASSERT_TRUE(client.transfer_call("p:third", 111));
    const auto original = mqtt.sent.back();
    mqtt.down(transfer_event(original, "failed", 2, 3, "transferRejected"));
    client.poll(112, 1'000'000);
    EXPECT_EQ(client.transfer_receipt().state, HumanCallTransferState::Failed);
    EXPECT_EQ(client.transfer_receipt().ack_code, 0);
    EXPECT_FALSE(client.transfer_call("p:fourth", 113));
    EXPECT_FALSE(client.control_call("hold", 113));
    client.poll(10'111, 1'000'000);
    EXPECT_EQ(client.transfer_receipt().state, HumanCallTransferState::Failed);
    EXPECT_EQ(client.transfer_receipt().ack_code, 0);
    ASSERT_TRUE(client.transfer_call("p:fourth", 10'112));
    const auto next = client.transfer_receipt().token;
    mqtt.down(transfer_ack(original));
    client.poll(10'113, 1'000'000);
    EXPECT_EQ(client.transfer_receipt().token, next);
    EXPECT_EQ(client.transfer_receipt().state, HumanCallTransferState::Pending);
    EXPECT_EQ(client.transfer_receipt().ack_code, 0);
    EXPECT_TRUE(client.active());
    EXPECT_EQ(media.begins, 1);
    EXPECT_EQ(media.hold_calls, 0);
}
// HumanCallControl 合法的平台保持事件不是未知方法，不能误关闭已建立原腿。
TEST(HumanCallControl, PlatformHoldPreservesOriginalConnectedLeg) {
    TestCallMqtt mqtt;
    TestCallMedia media;
    HumanCallClient client(mqtt, media, "p/d", "control-red", {"front", false, true});
    ASSERT_NO_FATAL_FAILURE(connected_client(client, mqtt, media));
    mqtt.down(participant_event(3, roster_data("member")));
    client.poll(111, 1'000'000);
    ASSERT_EQ(client.participants().size(), 1U);
    const auto closed = media.closes;
    auto held =
        participant_event(4, R"({"state":"held","revision":1,"requestToken":"remote-hold"})");
    held.method = "callState";
    mqtt.down(held);
    client.poll(112, 1'000'000);
    EXPECT_TRUE(client.active());
    EXPECT_EQ(client.session_id(), "call-1");
    EXPECT_EQ(client.participants().size(), 1U);
    EXPECT_EQ(media.closes, closed);
    EXPECT_TRUE(client.call_held());
    EXPECT_TRUE(media.held);
    EXPECT_EQ(media.hold_calls, 1);
}

// control_event 使用真实完成事件结构；原请求token位于data而不是事件msgToken。
Envelope control_event(std::string token, std::string state, std::uint64_t revision,
                       std::uint64_t sequence) {
    auto event = participant_event(sequence, "{\"state\":\"" + state +
                                                 "\",\"revision\":" + std::to_string(revision) +
                                                 ",\"requestToken\":\"" + token + "\"}");
    event.method = "callState";
    return event;
}
// HumanCallControl 原ACK与独立事件可乱序，保持/恢复必须各执行一次实际媒体端口。
TEST(HumanCallControl, AckAndEventOrderKeepOneActualEffectAndRoster) {
    for (bool event_first : {false, true}) {
        SCOPED_TRACE(event_first);
        TestCallMqtt mqtt;
        TestCallMedia media;
        HumanCallClient client(mqtt, media, "p/d", "control", {"front", false, true});
        ASSERT_NO_FATAL_FAILURE(connected_client(client, mqtt, media));
        mqtt.down(participant_event(3, roster_data("member")));
        client.poll(111, 1'000'000);
        std::uint64_t now = 112;
        for (unsigned index = 0; index < 2; ++index) {
            const bool held = index == 0;
            ASSERT_TRUE(client.control_call(held ? "hold" : "resume", now++));
            const auto original = mqtt.sent.back();
            EXPECT_EQ(original.method, "callControl");
            EXPECT_EQ(original.session, "call-1");
            EXPECT_EQ(original.payload, held ? R"({"expectedRevision":0,"operation":"hold"})"
                                             : R"({"expectedRevision":1,"operation":"resume"})");
            ASSERT_FALSE(client.control_call(held ? "hold" : "resume", now));
            auto ack = reply(
                original.token, "callState", "call-1",
                held ? R"({"state":"held","revision":1})" : R"({"state":"active","revision":2})");
            auto event =
                control_event(original.token, held ? "held" : "active", index + 1, index + 4);
            mqtt.down(event_first ? event : ack);
            client.poll(now++, 1'000'000);
            EXPECT_EQ(client.call_control_state(), HumanCallControlState::Pending);
            EXPECT_EQ(media.hold_calls, event_first ? index + 1 : index);
            mqtt.down(event_first ? ack : event);
            client.poll(now++, 1'000'000);
            EXPECT_EQ(client.call_control_state(), HumanCallControlState::Completed);
            EXPECT_EQ(client.call_held(), held);
            EXPECT_EQ(media.held, held);
            EXPECT_EQ(client.call_revision(), index + 1);
            EXPECT_EQ(media.hold_calls, index + 1);
            mqtt.down(ack);
            mqtt.down(event);
            client.poll(now++, 1'000'000);
            EXPECT_EQ(media.hold_calls, index + 1);
            ASSERT_TRUE(client.active());
            ASSERT_EQ(client.participants().size(), 1U);
        }
    }
}
// HumanCallControl
// 类型化拒绝及入口reason拒绝只结束原控制，不能清名册或停原媒体。
TEST(HumanCallControl, RejectionKeepsLegAndPendingTrackRequest) {
    for (const auto* reason : {"staleRevision", "invalidState", "requestConflict"}) {
        SCOPED_TRACE(reason);
        TestCallMqtt mqtt;
        TestCallMedia media;
        HumanCallClient client(mqtt, media, "p/d", "rejection", {"front", false, true});
        ASSERT_NO_FATAL_FAILURE(connected_client(client, mqtt, media));
        mqtt.down(participant_event(3, roster_data("member")));
        client.poll(111, 1'000'000);
        ASSERT_TRUE(client.control_track("mic-1", true, 112));
        const auto track_token = mqtt.sent.back().token;
        ASSERT_TRUE(client.control_call("hold", 113));
        const auto token = mqtt.sent.back().token;
        const auto closed = media.closes;
        const std::string payload =
            std::string("{\"reason\":\"") + reason +
            (std::string_view(reason) == "requestConflict" ? "\"}" : "\",\"currentRevision\":0}");
        auto denied = reply(token, "callState", "call-1", payload);
        denied.code = 409;
        mqtt.down(denied);
        mqtt.down(denied);
        client.poll(114, 1'000'000);
        EXPECT_EQ(client.call_control_state(), HumanCallControlState::Rejected);
        EXPECT_TRUE(client.active());
        EXPECT_EQ(client.participants().size(), 1U);
        EXPECT_TRUE(client.track_control_pending());
        EXPECT_EQ(media.closes, closed);
        EXPECT_EQ(media.hold_calls, 0);
        mqtt.down(reply(track_token, "trackState", "call-1",
                        R"({"trackId":"mic-1","state":"paused","mediaRevision":1})"));
        client.poll(115, 1'000'000);
        EXPECT_FALSE(client.track_control_pending());
        EXPECT_TRUE(client.active());
    }
}
// HumanCallControl
// 截止先于默认10秒重发，晚到控制不成功、不换token，原媒体请求继续。
TEST(HumanCallControl, ExactDeadlineIsUnknownWithoutRetransmissionOrMediaCancellation) {
    TestCallMqtt mqtt;
    TestCallMedia media;
    HumanCallClient client(mqtt, media, "p/d", "deadline");
    ASSERT_NO_FATAL_FAILURE(connected_client(client, mqtt, media));
    ASSERT_TRUE(client.control_call("hold", 111));
    const auto token = mqtt.sent.back().token;
    client.poll(10110, 1'000'000);
    EXPECT_EQ(client.call_control_state(), HumanCallControlState::Pending);
    ASSERT_TRUE(client.control_track("mic-1", true, 10110));
    const auto track_token = mqtt.sent.back().token;
    const auto sent = mqtt.sent.size();
    client.poll(10111, 1'000'000);
    EXPECT_EQ(mqtt.sent.size(), sent);
    EXPECT_EQ(client.call_control_state(), HumanCallControlState::Unknown);
    EXPECT_TRUE(client.track_control_pending());
    mqtt.down(reply(token, "callState", "call-1", R"({"state":"held","revision":1})"));
    mqtt.down(control_event(token, "held", 1, 3));
    mqtt.down(reply(track_token, "trackState", "call-1",
                    R"({"trackId":"mic-1","state":"paused","mediaRevision":1})"));
    client.poll(10112, 1'000'000);
    EXPECT_TRUE(client.active());
    EXPECT_FALSE(client.track_control_pending());
    EXPECT_EQ(client.call_control_state(), HumanCallControlState::Unknown);
    EXPECT_EQ(media.hold_calls, 0);
    EXPECT_FALSE(client.control_call("hold", 10113));
    client.poll(30111, 1'000'000);
    EXPECT_EQ(mqtt.sent.size(), sent);
    EXPECT_TRUE(client.active());
}
// HumanCallControl
// 默认未实现驱动不得发送控制；实际保持失败安全关闭，恢复失败保留静音。
TEST(HumanCallControl, MediaSupportAndActualFailureAreNotCloudAckSuccess) {
    for (unsigned variant = 0; variant < 3; ++variant) {
        SCOPED_TRACE(variant);
        TestCallMqtt mqtt;
        TestCallMedia media;
        HumanCallClient client(mqtt, media, "p/d", "media");
        ASSERT_NO_FATAL_FAILURE(connected_client(client, mqtt, media));
        if (variant == 0) {
            media.hold_capable = false;
            const auto sent = mqtt.sent.size();
            EXPECT_FALSE(client.control_call("hold", 111));
            EXPECT_EQ(mqtt.sent.size(), sent);
            EXPECT_TRUE(client.active());
            continue;
        }
        if (variant == 2) {
            mqtt.down(control_event("remote-hold", "held", 1, 3));
            client.poll(111, 1'000'000);
            ASSERT_TRUE(client.call_held());
        }
        media.hold_success = false;
        ASSERT_TRUE(client.control_call(variant == 1 ? "hold" : "resume", 112));
        const auto token = mqtt.sent.back().token;
        mqtt.down(reply(token, "callState", "call-1",
                        variant == 1 ? R"({"state":"held","revision":1})"
                                     : R"({"state":"active","revision":2})"));
        client.poll(113, 1'000'000);
        EXPECT_EQ(client.call_control_state(), HumanCallControlState::Pending);
        mqtt.down(control_event(token, variant == 1 ? "held" : "active", variant, variant + 2));
        client.poll(114, 1'000'000);
        if (variant == 1) {
            EXPECT_EQ(client.state(), HumanCallState::Failed);
            EXPECT_FALSE(media.started);
        } else {
            EXPECT_TRUE(client.active());
            EXPECT_TRUE(client.call_held());
            EXPECT_TRUE(media.held);
            EXPECT_EQ(client.call_control_state(), HumanCallControlState::Unknown);
        }
    }
}
// HumanCallControl 未实现操作、错身份/版本或非法信封不能触发本端媒体副作用。
TEST(HumanCallControl, InvalidControlRepliesFailClosedBeforeActualEffect) {
    for (unsigned variant = 0; variant < 7; ++variant) {
        SCOPED_TRACE(variant);
        TestCallMqtt mqtt;
        TestCallMedia media;
        HumanCallClient client(mqtt, media, "p/d", "invalid-control");
        ASSERT_NO_FATAL_FAILURE(connected_client(client, mqtt, media));
        EXPECT_FALSE(client.control_call("transfer", 111));
        EXPECT_FALSE(client.control_call("escalate", 111));
        EXPECT_FALSE(client.control_call("resume", 111));
        EXPECT_FALSE(client.control_call("cancel", 111));
        ASSERT_TRUE(client.control_call("hold", 111));
        const auto token = mqtt.sent.back().token;
        auto ack = reply(token, "callState", "call-1", R"({"state":"held","revision":1})");
        if (variant == 0)
            ack.token = "wrong-token";
        if (variant == 1)
            ack.payload = R"({"state":"held","revision":2})";
        if (variant == 2)
            ack.payload = R"({"state":"active","revision":1})";
        if (variant == 3)
            ack.event_sequence = 3;
        if (variant == 4)
            ack.message_sequence = 3;
        if (variant == 5)
            ack.payload = R"({"state":"held","revision":1,"extra":true})";
        if (variant == 6)
            ack.payload = R"({"state":"held","revision":1.5})";
        mqtt.down(ack);
        client.poll(112, 1'000'000);
        EXPECT_EQ(client.state(), HumanCallState::Failed);
        EXPECT_EQ(media.hold_calls, 0);
    }
}

// caller_client 复用真实创建应答建立未接听主叫腿，不借被叫邀请伪造cancel角色。
void caller_client(HumanCallClient& client, TestCallMqtt& mqtt) {
    ASSERT_NO_FATAL_FAILURE(ready_client(client, mqtt));
    ASSERT_TRUE(client.dial("p:other", 104));
    mqtt.down(reply(
        mqtt.sent.back().token, "sessionCreated", "caller-leg-1",
        R"({"state":"inviting","callGroupId":"group-1","selected":{"transport":"webrtc"},"tracks":[{"trackId":"mic-1","kind":"audio","sourceId":"mic","direction":"sendonly","codec":"opus","state":"requested"},{"trackId":"speaker-1","kind":"audio","sourceId":"speaker","direction":"recvonly","codec":"opus","state":"requested"},{"trackId":"video-1","kind":"video","sourceId":"front","direction":"sendonly","codec":"H264","state":"requested"}]})"));
    client.poll(105, 1'000'000);
    ASSERT_EQ(client.state(), HumanCallState::Negotiating);
}
// HumanCallControl
// cancel的ACK不能替代独立sessionEnded，乱序/旧腿迟到都不复活媒体。
TEST(HumanCallControl, CancelWaitsForOriginalEndedEventAndDropsLateLegMessages) {
    for (bool event_first : {false, true}) {
        SCOPED_TRACE(event_first);
        TestCallMqtt mqtt;
        TestCallMedia media;
        HumanCallClient client(mqtt, media, "p/d", "cancel");
        ASSERT_NO_FATAL_FAILURE(caller_client(client, mqtt));
        ASSERT_TRUE(client.control_call("cancel", 106));
        const auto original = mqtt.sent.back();
        EXPECT_EQ(original.payload, R"({"expectedRevision":0,"operation":"cancel"})");
        const auto closed = media.closes;
        auto ack = reply(original.token, "callState", "caller-leg-1",
                         R"({"state":"cancelled","revision":1,"reason":"callerCancelled"})");
        auto ended = participant_event(1, R"({"state":"ended","reason":"callerCancelled"})");
        ended.session = "caller-leg-1";
        ended.method = "sessionEnded";
        if (!event_first) {
            mqtt.down(ack);
            mqtt.down(ack);
            client.poll(107, 1'000'000);
            EXPECT_EQ(client.state(), HumanCallState::Ending);
            EXPECT_EQ(client.session_id(), "caller-leg-1");
            EXPECT_EQ(client.call_control_state(), HumanCallControlState::Pending);
            EXPECT_TRUE(media.started);
            EXPECT_EQ(media.closes, closed);
        }
        mqtt.down(ended);
        client.poll(108, 1'000'000);
        EXPECT_EQ(client.state(), HumanCallState::Available);
        EXPECT_TRUE(client.session_id().empty());
        EXPECT_FALSE(media.started);
        EXPECT_EQ(media.closes, closed + 1);
        mqtt.down(ack);
        mqtt.down(ended);
        client.poll(109, 1'000'000);
        EXPECT_EQ(media.closes, closed + 1);
        auto next = invite();
        next.session = "new-leg";
        next.token = "new-invite";
        mqtt.down(next);
        client.poll(110, 1'000'000);
        ASSERT_EQ(client.state(), HumanCallState::Ringing);
        mqtt.down(ack);
        mqtt.down(ended);
        auto late = control_event(original.token, "active", 2, 2);
        late.session = "caller-leg-1";
        mqtt.down(late);
        client.poll(111, 1'000'000);
        EXPECT_EQ(client.state(), HumanCallState::Ringing);
        EXPECT_EQ(client.session_id(), "new-leg");
        EXPECT_EQ(media.hold_calls, 0);
        EXPECT_EQ(media.closes, closed + 1);
    }
}
// HumanCallControl
// 接听竞争导致cancel拒绝时，原SDP请求必须继续，不能转成sessionEnd。
TEST(HumanCallControl, CancelInvalidStatePreservesOriginalMediaNegotiation) {
    TestCallMqtt mqtt;
    TestCallMedia media;
    HumanCallClient client(mqtt, media, "p/d", "cancel-race");
    ASSERT_NO_FATAL_FAILURE(caller_client(client, mqtt));
    ASSERT_TRUE(client.offer_ready("v=0\r\no=client\r\n", 106));
    const auto offer_token = mqtt.sent.back().token;
    ASSERT_TRUE(client.control_call("cancel", 107));
    auto denied = reply(mqtt.sent.back().token, "callState", "caller-leg-1",
                        R"({"reason":"invalidState","currentRevision":0})");
    denied.code = 409;
    const auto closed = media.closes;
    mqtt.down(denied);
    client.poll(108, 1'000'000);
    EXPECT_EQ(client.state(), HumanCallState::Negotiating);
    EXPECT_EQ(client.call_control_state(), HumanCallControlState::Rejected);
    EXPECT_EQ(media.closes, closed);
    mqtt.down(reply(
        offer_token, "mediaAnswer", "caller-leg-1",
        R"({"mediaRevision":1,"webrtc":{"iceMode":"complete","sdp":"v=0\r\no=server\r\n"}})"));
    client.poll(109, 1'000'000);
    EXPECT_EQ(client.state(), HumanCallState::ConnectingMedia);
    EXPECT_TRUE(media.answered);
    EXPECT_EQ(mqtt.sent.back().method, "callControl");
}

// HumanCallControl
// 已接通原主叫仍由平台裁决cancel，409不能关闭原Peer或降级为hangup。
TEST(HumanCallControl, ConnectedCallerCancelRejectionPreservesActualMedia) {
    TestCallMqtt mqtt;
    TestCallMedia media;
    HumanCallClient client(mqtt, media, "p/d", "connected-cancel");
    ASSERT_NO_FATAL_FAILURE(caller_client(client, mqtt));
    ASSERT_TRUE(client.offer_ready("v=0\r\no=client\r\n", 106));
    mqtt.down(reply(
        mqtt.sent.back().token, "mediaAnswer", "caller-leg-1",
        R"({"mediaRevision":1,"webrtc":{"iceMode":"complete","sdp":"v=0\r\no=server\r\n"}})"));
    client.poll(107, 1'000'000);
    media.ready = true;
    ASSERT_TRUE(client.media_connected(108));
    mqtt.down(reply(
        mqtt.sent.back().token, "requestAck", "caller-leg-1",
        R"({"requestMethod":"mediaState","result":{"mediaRevision":1,"state":"connected"}})"));
    auto established = participant_event(1, R"({"state":"established","mediaRevision":1})");
    established.method = "sessionEstablished";
    established.session = "caller-leg-1";
    mqtt.down(established);
    client.poll(109, 1'000'000);
    ASSERT_TRUE(client.active());
    ASSERT_TRUE(client.control_call("cancel", 110));
    const auto original = mqtt.sent.back();
    const auto closed = media.closes;
    std::vector<HumanCallControlReceipt> receipts;  // 宿主仅接已校验固定拒绝，不读自由平台正文。
    client.observe_control(
        [&](const HumanCallControlReceipt& value) { receipts.push_back(value); });
    auto denied = reply(original.token, "callState", "caller-leg-1",
                        R"({"reason":"invalidState","currentRevision":0})");
    denied.code = 409;
    mqtt.down(denied);
    mqtt.down(denied);
    client.poll(111, 1'000'000);
    EXPECT_TRUE(client.active());
    EXPECT_TRUE(media.started);
    EXPECT_TRUE(media.ready);
    EXPECT_EQ(media.closes, closed);
    EXPECT_EQ(client.call_control_state(), HumanCallControlState::Rejected);
    EXPECT_EQ(mqtt.sent.back().method, "callControl");
    ASSERT_EQ(receipts.size(), 1U);
    EXPECT_EQ(receipts[0].code, 409);
    EXPECT_EQ(receipts[0].reason, "invalidState");
    EXPECT_EQ(receipts[0].token, original.token);
    EXPECT_FALSE(receipts[0].actual_applied);
}
// HumanCallControl 本地挂断后清除控制重发，旧完成事件不得重新恢复实际媒体。
TEST(HumanCallControl, LocalEndCancelsControlWaitWithoutLateResume) {
    TestCallMqtt mqtt;
    TestCallMedia media;
    HumanCallClient client(mqtt, media, "p/d", "local-end");
    ASSERT_NO_FATAL_FAILURE(connected_client(client, mqtt, media));
    ASSERT_TRUE(client.control_call("hold", 111));
    const auto token = mqtt.sent.back().token;
    ASSERT_TRUE(client.end(112));
    const auto end_token = mqtt.sent.back().token;
    const auto sent = mqtt.sent.size();
    mqtt.down(reply(token, "callState", "call-1", R"({"state":"held","revision":1})"));
    mqtt.down(control_event(token, "held", 1, 3));
    client.poll(113, 1'000'000);
    EXPECT_EQ(client.state(), HumanCallState::Ending);
    EXPECT_EQ(media.hold_calls, 0);
    mqtt.down(reply(end_token, "sessionEnded", "call-1", R"({"state":"ended","reason":"hangup"})"));
    client.poll(114, 1'000'000);
    ASSERT_EQ(client.state(), HumanCallState::Available);
    client.poll(20112, 1'000'000);
    EXPECT_EQ(mqtt.sent.size(), sent);
    EXPECT_FALSE(media.started);
}

// HumanCallControl 单独ACK或事件不能完成等待，已保持也不能伪补另一条平台事实。
TEST(HumanCallControl, MissingAckOrCompletionStaysUnknownAtOriginalDeadline) {
    for (bool event_only : {false, true}) {
        SCOPED_TRACE(event_only);
        TestCallMqtt mqtt;
        TestCallMedia media;
        HumanCallClient client(mqtt, media, "p/d", "partial");
        ASSERT_NO_FATAL_FAILURE(connected_client(client, mqtt, media));
        ASSERT_TRUE(client.control_call("hold", 111));
        const auto token = mqtt.sent.back().token;
        mqtt.down(event_only
                      ? control_event(token, "held", 1, 3)
                      : reply(token, "callState", "call-1", R"({"state":"held","revision":1})"));
        client.poll(112, 1'000'000);
        ASSERT_EQ(client.call_control_state(), HumanCallControlState::Pending);
        EXPECT_EQ(client.call_held(), event_only);
        const auto sent = mqtt.sent.size();
        client.poll(10111, 1'000'000);
        EXPECT_EQ(client.call_control_state(), HumanCallControlState::Unknown);
        EXPECT_EQ(mqtt.sent.size(), sent);
        EXPECT_TRUE(client.active());
        mqtt.down(event_only
                      ? reply(token, "callState", "call-1", R"({"state":"held","revision":1})")
                      : control_event(token, "held", 1, 3));
        client.poll(10112, 1'000'000);
        EXPECT_EQ(client.call_control_state(), HumanCallControlState::Unknown);
        EXPECT_EQ(media.hold_calls, event_only ? 1 : 0);
    }
}
// HumanCallControl
// 拒绝给出的currentRevision不是完成，不能阻挡同版本独立实际保持。
TEST(HumanCallControl, RejectionRevisionDoesNotInventActualPlatformState) {
    TestCallMqtt mqtt;
    TestCallMedia media;
    HumanCallClient client(mqtt, media, "p/d", "revision");
    ASSERT_NO_FATAL_FAILURE(connected_client(client, mqtt, media));
    ASSERT_TRUE(client.control_call("hold", 111));
    auto denied = reply(mqtt.sent.back().token, "callState", "call-1",
                        R"({"reason":"staleRevision","currentRevision":2})");
    denied.code = 409;
    mqtt.down(denied);
    client.poll(112, 1'000'000);
    EXPECT_EQ(client.call_revision(), 2U);
    EXPECT_FALSE(client.call_held());
    EXPECT_EQ(media.hold_calls, 0);
    mqtt.down(control_event("remote-hold", "held", 2, 3));
    client.poll(113, 1'000'000);
    EXPECT_TRUE(client.active());
    EXPECT_TRUE(client.call_held());
    EXPECT_EQ(media.hold_calls, 1);
    ASSERT_TRUE(client.control_call("resume", 114));
    EXPECT_EQ(mqtt.sent.back().payload, R"({"expectedRevision":2,"operation":"resume"})");
}
// HumanCallControl 满24条背压而非淘汰旧token，旧ACK和事件不重复媒体副作用。
TEST(HumanCallControl, ControlHistoryIsBoundedWithoutEvictingOriginalIdentity) {
    TestCallMqtt mqtt;
    TestCallMedia media;
    HumanCallClient client(mqtt, media, "p/d", "bounded");
    ASSERT_NO_FATAL_FAILURE(connected_client(client, mqtt, media));
    std::uint64_t now = 111;
    Envelope first_ack, first_event;
    for (unsigned index = 0; index < 24; ++index) {
        const bool held = index % 2 == 0;
        ASSERT_TRUE(client.control_call(held ? "hold" : "resume", now++));
        const auto token = mqtt.sent.back().token;
        auto event = control_event(token, held ? "held" : "active", index + 1, index + 3);
        auto ack = reply(token, "callState", "call-1",
                         "{\"state\":\"" + std::string(held ? "held" : "active") +
                             "\",\"revision\":" + std::to_string(index + 1) + "}");
        if (index == 0) {
            first_ack = ack;
            first_event = event;
        }
        mqtt.down(ack);
        mqtt.down(event);
        client.poll(now++, 1'000'000);
        ASSERT_EQ(client.call_control_state(), HumanCallControlState::Completed);
    }
    EXPECT_FALSE(client.control_call("hold", now));
    const auto sent = mqtt.sent.size();
    mqtt.down(first_ack);
    mqtt.down(first_event);
    client.poll(now + 1, 1'000'000);
    EXPECT_TRUE(client.active());
    EXPECT_FALSE(client.call_held());
    EXPECT_EQ(client.call_revision(), 24U);
    EXPECT_EQ(media.hold_calls, 24);
    EXPECT_EQ(mqtt.sent.size(), sent);
}

// HumanCallControl
// 异步排队不能关闭原腿或冒保持完成，实际原票据之后才形成一次副作用。
TEST(HumanCallControl, AsyncHoldStaysPendingUntilOriginalActualReceipt) {
    TestCallMqtt mqtt;
    TestCallMedia media;
    media.async_supported = true;
    media.hold_success = false;  // 异步驱动不能被错误转入旧同步失败分支。
    HumanCallClient client(mqtt, media, "p/d", "async");
    std::vector<HumanCallControlReceipt> receipts;  // 只记录已核验的固定原控制投影。
    client.observe_control(
        [&](const HumanCallControlReceipt& value) { receipts.push_back(value); });
    ASSERT_NO_FATAL_FAILURE(connected_client(client, mqtt, media));
    ASSERT_TRUE(client.control_call("hold", 111));
    const auto token = mqtt.sent.back().token;
    mqtt.down(reply(token, "callState", "call-1", R"({"state":"held","revision":1})"));
    mqtt.down(control_event(token, "held", 1, 3));
    client.poll(112, 1'000'000);
    ASSERT_TRUE(client.active());
    EXPECT_EQ(client.call_control_state(), HumanCallControlState::Pending);
    EXPECT_FALSE(client.call_held());
    EXPECT_EQ(media.async_token, token);
    EXPECT_EQ(media.async_revision, 1U);
    EXPECT_EQ(media.async_ticket, 1U);
    EXPECT_EQ(media.hold_calls, 0);
    ASSERT_EQ(receipts.size(), 1U);
    EXPECT_EQ(receipts.front().code, 200);
    EXPECT_EQ(receipts.front().session, "call-1");
    EXPECT_EQ(receipts.front().token, token);
    EXPECT_FALSE(receipts.front().actual_applied);
    client.poll(113, 1'000'000);
    EXPECT_EQ(media.async_ticket, 1U);
    media.async_applied = true;
    media.held = true;  // 测试驱动显式收录实际原停止事实，不由控制点击赋值。
    client.poll(114, 1'000'000);
    EXPECT_TRUE(client.call_held());
    EXPECT_EQ(client.call_control_state(), HumanCallControlState::Completed);
    ASSERT_EQ(receipts.size(), 2U);
    EXPECT_TRUE(receipts.back().actual_held);
    EXPECT_TRUE(receipts.back().actual_applied);
    mqtt.down(control_event(token, "held", 1, 3));
    client.poll(115, 1'000'000);
    EXPECT_EQ(media.async_ticket, 1U);
    EXPECT_EQ(receipts.size(), 2U);
}

// HumanCallControl
// 实际两秒或原十秒先到即未知，迟到票据不能复活原控制或延长预算。
TEST(HumanCallControl, AsyncDeadlineKeepsOriginalCallAndDoesNotReplayLateActual) {
    for (bool overall_first : {false, true}) {
        TestCallMqtt mqtt;
        TestCallMedia media;
        media.async_supported = true;
        HumanCallClient client(mqtt, media, "p/d", "async-deadline");
        ASSERT_NO_FATAL_FAILURE(connected_client(client, mqtt, media));
        ASSERT_TRUE(client.control_call("hold", 111));
        const auto token = mqtt.sent.back().token;
        const auto event_at = overall_first ? 10000U : 112U;
        mqtt.down(reply(token, "callState", "call-1", R"({"state":"held","revision":1})"));
        mqtt.down(control_event(token, "held", 1, 3));
        client.poll(event_at, 1'000'000);
        ASSERT_TRUE(client.active());
        ASSERT_EQ(media.async_ticket, 1U);
        const auto sent = mqtt.sent.size();
        client.poll(overall_first ? 10111 : 2112, 1'000'000);
        EXPECT_TRUE(client.active());
        EXPECT_FALSE(client.call_held());
        EXPECT_EQ(client.call_control_state(), HumanCallControlState::Unknown);
        media.async_applied = true;
        mqtt.down(control_event(token, "held", 1, 4));
        client.poll(overall_first ? 10112 : 2113, 1'000'000);
        EXPECT_TRUE(client.active());
        EXPECT_FALSE(client.call_held());
        EXPECT_EQ(media.async_ticket, 1U);
        EXPECT_EQ(mqtt.sent.size(), sent);
        EXPECT_FALSE(client.control_call("hold", overall_first ? 10113 : 2114));
    }
}

// HumanCallControl 结束腿撤销实际等待，原ACK和驱动迟到不能重新开启已结束资源。
TEST(HumanCallControl, AsyncEndedLegDropsOriginalReceipt) {
    TestCallMqtt mqtt;
    TestCallMedia media;
    media.async_supported = true;
    HumanCallClient client(mqtt, media, "p/d", "async-ended");
    ASSERT_NO_FATAL_FAILURE(connected_client(client, mqtt, media));
    ASSERT_TRUE(client.control_call("hold", 111));
    const auto token = mqtt.sent.back().token;
    mqtt.down(control_event(token, "held", 1, 3));
    client.poll(112, 1'000'000);
    ASSERT_TRUE(client.active());
    auto ended = control_event(token, "held", 1, 4);
    ended.method = "sessionEnded";
    ended.payload = R"({"state":"ended","reason":"hangup"})";
    mqtt.down(ended);
    client.poll(113, 1'000'000);
    EXPECT_EQ(client.state(), HumanCallState::Available);
    media.async_applied = true;
    client.poll(114, 1'000'000);
    EXPECT_EQ(client.state(), HumanCallState::Available);
    EXPECT_FALSE(client.call_held());
    EXPECT_EQ(media.async_ticket, 1U);
}

// HumanCallControl
// 合法对端token只沿原平台事件驱动一次门控，不要求凭空存在本端请求ACK。
TEST(HumanCallControl, RemoteHoldEventAppliesAndLocalResumeKeepsOriginalRevision) {
    TestCallMqtt mqtt;
    TestCallMedia media;
    media.async_supported = true;
    HumanCallClient client(mqtt, media, "p/d", "remote-control");
    ASSERT_NO_FATAL_FAILURE(connected_client(client, mqtt, media));
    const auto sent = mqtt.sent.size();
    mqtt.down(control_event("remote-original-hold", "held", 1, 3));
    client.poll(111, 1'000'000);
    ASSERT_TRUE(client.active());
    EXPECT_EQ(media.async_token, "remote-original-hold");
    EXPECT_EQ(media.async_ticket, 1U);
    EXPECT_EQ(client.call_revision(), 1U);
    EXPECT_FALSE(client.call_held());
    EXPECT_EQ(mqtt.sent.size(), sent);
    media.async_applied = true;
    client.poll(112, 1'000'000);
    EXPECT_TRUE(client.call_held());
    EXPECT_EQ(client.call_control_state(), HumanCallControlState::Idle);
    mqtt.down(control_event("remote-original-hold", "held", 1, 4));
    client.poll(113, 1'000'000);
    EXPECT_EQ(media.async_ticket, 1U);
    ASSERT_TRUE(client.control_call("resume", 114));
    const auto token = mqtt.sent.back().token;
    EXPECT_NE(token, "remote-original-hold");
    EXPECT_EQ(mqtt.sent.back().payload, R"({"expectedRevision":1,"operation":"resume"})");
    mqtt.down(reply(token, "callState", "call-1", R"({"state":"active","revision":2})"));
    mqtt.down(control_event(token, "active", 2, 5));
    client.poll(115, 1'000'000);
    EXPECT_TRUE(client.call_held());
    EXPECT_EQ(media.async_ticket, 2U);
    EXPECT_EQ(client.call_control_state(), HumanCallControlState::Pending);
    media.async_applied = true;
    client.poll(116, 1'000'000);
    EXPECT_FALSE(client.call_held());
    EXPECT_EQ(client.call_control_state(), HumanCallControlState::Completed);
    mqtt.down(control_event("remote-original-hold", "held", 1, 6));
    client.poll(117, 1'000'000);
    EXPECT_TRUE(client.active());
    EXPECT_FALSE(client.call_held());
    EXPECT_EQ(media.async_ticket, 2U);
}

// HumanCallLifecycle 所有接通条件分别到达后才激活媒体，挂断立刻释放本地设备。
TEST(HumanCallLifecycle, InviteAcceptMediaAndEnd) {
    TestCallMqtt mqtt;
    TestCallMedia media;
    HumanCallClient client(mqtt, media, "p/d", "test-instance");
    ready_client(client, mqtt);
    mqtt.down(invite());
    client.poll(104, 1'000'000);
    ASSERT_EQ(client.state(), HumanCallState::Ringing);
    EXPECT_FALSE(media.started);
    ASSERT_TRUE(client.accept(105));
    ASSERT_EQ(mqtt.sent.back().method, "sessionAccept");
    EXPECT_EQ(mqtt.sent.back().token, "invite-1");
    mqtt.down(reply("invite-1", "requestAck", "call-1",
                    R"({"requestMethod":"sessionAccept","result":{"state":"negotiating"}})"));
    client.poll(106, 1'000'000);
    ASSERT_EQ(client.state(), HumanCallState::Negotiating);
    EXPECT_TRUE(media.started);
    ASSERT_TRUE(client.offer_ready("v=0\r\no=client\r\n", 107));
    const auto offer_token = mqtt.sent.back().token;
    mqtt.down(reply(
        offer_token, "mediaAnswer", "call-1",
        R"({"mediaRevision":1,"webrtc":{"iceMode":"complete","sdp":"v=0\r\no=server\r\n"}})"));
    client.poll(108, 1'000'000);
    ASSERT_EQ(client.state(), HumanCallState::ConnectingMedia);
    EXPECT_TRUE(media.answered);
    EXPECT_FALSE(client.active());
    media.ready = true;
    ASSERT_TRUE(client.media_connected(109));
    const auto state_token = mqtt.sent.back().token;
    Envelope established;
    established.request = false;
    established.token = "established-1";
    established.method = "sessionEstablished";
    established.session = "call-1";
    established.event_sequence = 2;
    established.payload = R"({"state":"established","mediaRevision":1})";
    mqtt.down(established);
    client.poll(110, 1'000'000);
    EXPECT_FALSE(client.active());
    mqtt.down(reply(
        state_token, "requestAck", "call-1",
        R"({"requestMethod":"mediaState","result":{"mediaRevision":1,"state":"connected"}})"));
    client.poll(111, 1'000'000);
    ASSERT_TRUE(client.active());
    EXPECT_FALSE(client.control_track("speaker-1", true, 111));
    ASSERT_TRUE(client.control_track("mic-1", true, 111));
    EXPECT_TRUE(client.track_control_pending());
    EXPECT_EQ(mqtt.sent.back().method, "trackControl");
    const auto track_token = mqtt.sent.back().token;
    EXPECT_FALSE(client.control_track("video-1", true, 111));
    auto track_reply = reply(track_token, "trackState", "call-1",
                             R"({"trackId":"mic-1","state":"paused","mediaRevision":1})");
    mqtt.down(track_reply);
    client.poll(111, 1'000'000);
    EXPECT_FALSE(client.track_control_pending());
    mqtt.down(track_reply);
    client.poll(111, 1'000'000);
    EXPECT_TRUE(client.active());
    ASSERT_TRUE(client.end(112));
    EXPECT_FALSE(media.started);
    EXPECT_EQ(client.state(), HumanCallState::Ending);
    const auto end_token = mqtt.sent.back().token;
    mqtt.down(reply(end_token, "sessionEnded", "call-1", R"({"state":"ended","reason":"hangup"})"));
    client.poll(113, 1'000'000);
    EXPECT_EQ(client.state(), HumanCallState::Available);
    EXPECT_TRUE(client.session_id().empty());
}

// HumanCallRejectBadInvite
// 验证错Topic、过期、非法轨和无接听操作不能获得摄像头。
TEST(HumanCallRejectBadInvite, UnauthorizedAndMalformed) {
    TestCallMqtt mqtt;
    TestCallMedia media;
    HumanCallClient client(mqtt, media, "p/d", "test-instance");
    ready_client(client, mqtt);
    mqtt.down(invite(), "$thing/down/signaling/p/other");
    client.poll(104, 1'000'000);
    EXPECT_EQ(client.state(), HumanCallState::Failed);
    EXPECT_FALSE(media.started);
}

// HumanCallTimeout 未接听是普通业务终态；保留网络与原邀请墓碑，不创建媒体，可继续接听下一次邀请。
TEST(HumanCallTimeout, RingingExpiryKeepsDeviceAvailableAndFencesOldInvitation) {
    TestCallMqtt mqtt;
    TestCallMedia media;
    HumanCallClient client(mqtt, media, "p/d", "test-instance");
    ready_client(client, mqtt);
    mqtt.down(invite());
    client.poll(104, 1'000'000);
    ASSERT_EQ(client.state(), HumanCallState::Ringing);
    client.poll(105, 1'030'000);
    ASSERT_EQ(client.state(), HumanCallState::Available);
    EXPECT_FALSE(media.started);
    EXPECT_EQ(client.failure_facts().failure, HumanCallFailure::None);
    EXPECT_EQ(mqtt.closed, 1);  // 仅start初始化的一次close，邀请到期不得断设备连接。
    EXPECT_EQ(media.begins, 0);
    const auto sent = mqtt.sent.size();
    mqtt.down(invite());
    client.poll(106, 1'030'000);
    EXPECT_EQ(client.state(), HumanCallState::Available);
    EXPECT_EQ(mqtt.sent.size(), sent);
    EXPECT_FALSE(client.accept(106));
    auto next = invite();
    next.token = "fresh-invite";
    next.session = "fresh-call";
    const auto expiry = next.payload.find("1030000");
    ASSERT_NE(expiry, std::string::npos);
    next.payload.replace(expiry, 7, "1060000");
    mqtt.down(next);
    client.poll(107, 1'030'000);
    EXPECT_EQ(client.state(), HumanCallState::Ringing);
    EXPECT_EQ(media.begins, 0);
}

// HumanCallDial 使用独立主叫会话腿；目标拒绝后迟到创建应答不能复活媒体。
// UDP主叫也必须使用固定JPEG合同，媒体答案不能回落到另一种传输。
TEST(HumanCallDial, UDPCreateUsesExplicitTransportAndJPEG) {
    TestCallMqtt mqtt;
    TestCallMedia media;
    media.receive_capable = true;
    media.udp_capable = true;
    media.reservation_required = true;
    HumanCallConfig config;  // 模拟Watcher实际单UDP、双向视频装配。
    config.udp_av = true;
    config.udp_only = true;
    config.receive_video = true;
    HumanCallClient client(mqtt, media, "p/d", "udp-dial", config);
    ready_client(client, mqtt);
    ASSERT_TRUE(client.dial("p:other", 104));
    EXPECT_NE(mqtt.sent.back().payload.find(R"("transport":"udpAv")"), std::string::npos);
    EXPECT_NE(mqtt.sent.back().payload.find(R"("codec":"jpeg")"), std::string::npos);
    EXPECT_EQ(mqtt.sent.back().payload.find("H264"), std::string::npos);
    const auto token = mqtt.sent.back().token;  // 仅原创建关联可以安装平台选择。
    mqtt.down(reply(token, "sessionCreated", "udp-caller-leg",
        R"({"state":"inviting","callGroupId":"udp-pair","selected":{"transport":"udpAv"},"tracks":[{"trackId":"mic-1","kind":"audio","sourceId":"mic","direction":"sendonly","codec":"opus","state":"requested"},{"trackId":"speaker-1","kind":"audio","sourceId":"speaker","direction":"recvonly","codec":"opus","state":"requested"},{"trackId":"video-1","kind":"video","sourceId":"front","direction":"sendonly","codec":"jpeg","state":"requested"},{"trackId":"video-in","kind":"video","sourceId":"remote","direction":"recvonly","codec":"jpeg","state":"requested"}]})"));
    client.poll(105, 1'000'000);
    EXPECT_EQ(client.state(), HumanCallState::Negotiating);
    EXPECT_TRUE(media.udp_selected);
    ASSERT_TRUE(client.offer_ready("", 106));
    EXPECT_NE(mqtt.sent.back().payload.find("udpAv"), std::string::npos);
}

TEST(HumanCallDial, CreatesIndependentLegAndDropsLateResults) {
    TestCallMqtt mqtt;
    TestCallMedia media;
    HumanCallClient client(mqtt, media, "p/d", "test-instance");
    ready_client(client, mqtt);
    EXPECT_FALSE(client.dial("", 104));
    EXPECT_FALSE(client.dial("p/other", 104));
    ASSERT_TRUE(client.dial("p:other", 104));
    EXPECT_EQ(client.state(), HumanCallState::Inviting);
    ASSERT_EQ(mqtt.sent.back().method, "sessionCreate");
    EXPECT_EQ(mqtt.sent.back().session, "");
    const auto create_token = mqtt.sent.back().token;
    EXPECT_NE(mqtt.sent.back().payload.find("\"targetParty\""), std::string::npos);
    EXPECT_FALSE(media.started);
    EXPECT_FALSE(client.dial("p:third", 104));
    auto created = reply(
        create_token, "sessionCreated", "caller-leg-1",
        R"({"state":"inviting","callGroupId":"group-1","selected":{"transport":"webrtc"},"tracks":[{"trackId":"mic-1","kind":"audio","sourceId":"mic","direction":"sendonly","codec":"opus","state":"requested"},{"trackId":"speaker-1","kind":"audio","sourceId":"speaker","direction":"recvonly","codec":"opus","state":"requested"},{"trackId":"video-1","kind":"video","sourceId":"front","direction":"sendonly","codec":"H264","state":"requested"}]})");
    mqtt.down(created);
    client.poll(105, 1'000'000);
    ASSERT_EQ(client.state(), HumanCallState::Negotiating);
    EXPECT_EQ(client.session_id(), "caller-leg-1");
    EXPECT_TRUE(media.started);
    mqtt.down(created);
    client.poll(106, 1'000'000);
    EXPECT_EQ(client.state(), HumanCallState::Negotiating);
    Envelope ended;
    ended.request = false;
    ended.token = "ended-1";
    ended.method = "sessionEnded";
    ended.session = "caller-leg-1";
    ended.event_sequence = 1;
    ended.payload = R"({"state":"ended","reason":"rejected"})";
    mqtt.down(ended);
    client.poll(107, 1'000'000);
    EXPECT_EQ(client.state(), HumanCallState::Available);
    EXPECT_FALSE(media.started);
    mqtt.down(created);
    client.poll(108, 1'000'000);
    EXPECT_EQ(client.state(), HumanCallState::Available);
    EXPECT_FALSE(media.started);
    EXPECT_TRUE(client.dial("p:third", 109));
}

// HumanCallDial 创建失败保留MQTT能力，不把权限拒绝当成设备失联。
TEST(HumanCallDial, RejectedCreateKeepsDeviceAvailable) {
    TestCallMqtt mqtt;
    TestCallMedia media;
    HumanCallClient client(mqtt, media, "p/d", "test-instance");
    ready_client(client, mqtt);
    ASSERT_TRUE(client.dial("p:other", 104));
    auto denied = reply(mqtt.sent.back().token, "sessionCreated", "",
                        R"({"requestMethod":"sessionCreate","reason":"requestDenied"})");
    denied.code = 403;
    mqtt.down(denied);
    client.poll(105, 1'000'000);
    EXPECT_EQ(client.state(), HumanCallState::Available);
    EXPECT_FALSE(media.started);
    EXPECT_FALSE(media.prepared);
    mqtt.down(denied);
    client.poll(106, 1'000'000);
    EXPECT_EQ(client.state(), HumanCallState::Available);
}
// 主叫相机不可预约时不发送创建请求，避免让对方响铃后本机才发现无法启动。
TEST(HumanCallDial, VideoReservationFailureDoesNotCreateCall) {
    TestCallMqtt mqtt;
    TestCallMedia media;
    media.prepare_ok = false;
    HumanCallClient client(mqtt, media, "p/d", "reservation");
    ready_client(client, mqtt);
    const auto before = mqtt.sent.size();  // 只允许既有能力上报，不产生呼叫。
    EXPECT_FALSE(client.dial("p:other", 104));
    EXPECT_EQ(mqtt.sent.size(), before);
    EXPECT_EQ(client.state(), HumanCallState::Available);
    EXPECT_FALSE(media.started);
}
// 首拒帧只保存白名单，不保留字段值；合成非法报文仅验证诊断，不冒充现场输入。
TEST(HumanRejectionDiagnostic, UnknownAndNestedMethodsAreRedactedAndFirstCauseSurvivesClose) {
    TestCallMqtt mqtt;
    TestCallMedia media;
    HumanCallClient client(mqtt, media, "p/d", "diagnostic",
                           HumanCallConfig{"front", true, false, HumanCallMode::Video});
    client.enable_rejection_diagnostics();
    ASSERT_TRUE(client.start_shared(100));
    const auto body =
        R"({"msgToken":"private-token","method":"requestAck","code":409,"data":{"requestMethod":"private-nested","result":{},"secret":"private-secret"}})";
    EXPECT_FALSE(client.receive_shared("$thing/down/signaling/p/d", body, 101, 1000000));
    ASSERT_TRUE(client.rejection_facts());
    EXPECT_EQ(client.rejection_facts()->method, HumanRejectionMethod::RequestAck);
    EXPECT_EQ(client.rejection_facts()->request_method, HumanRejectionMethod::Unknown);
    EXPECT_EQ(client.rejection_facts()->code, 409);
    EXPECT_EQ(client.rejection_facts()->state, HumanCallState::Available);
    client.close();
    EXPECT_FALSE(client.receive_shared("$thing/down/signaling/p/d", "private-next", 102, 1000000));
    ASSERT_TRUE(client.rejection_facts());
    EXPECT_EQ(client.rejection_facts()->code, 409);
    ASSERT_TRUE(client.start_shared(103));
    EXPECT_FALSE(client.rejection_facts());
    EXPECT_FALSE(client.receive_shared(
        "$thing/down/signaling/p/d",
        R"({"msgToken":"private-token","method":"private-method","data":{}})", 104, 1000000));
    ASSERT_TRUE(client.rejection_facts());
    EXPECT_EQ(client.rejection_facts()->method, HumanRejectionMethod::Unknown);
    EXPECT_FALSE(client.rejection_facts()->code);
}
// 默认关闭时连畸形报文也不增加诊断解析或保存；原业务拒绝仍照常执行。
TEST(HumanRejectionDiagnostic, DisabledDoesNotRetainMalformedOrValidRejectedFrames) {
    TestCallMqtt mqtt;
    TestCallMedia media;
    HumanCallClient client(mqtt, media, "p/d", "diagnostic",
                           HumanCallConfig{"front", true, false, HumanCallMode::Video});
    ASSERT_TRUE(client.start_shared(100));
    EXPECT_FALSE(
        client.receive_shared("$thing/down/signaling/p/d", "private-invalid", 101, 1000000));
    EXPECT_FALSE(client.rejection_facts());
    ASSERT_TRUE(client.start_shared(102));
    EXPECT_FALSE(client.receive_shared(
        "$thing/down/signaling/p/d",
        R"({"msgToken":"private-token","method":"private-method","code":409,"data":{}})", 103,
        1000000));
    EXPECT_FALSE(client.rejection_facts());
    client.enable_rejection_diagnostics();
    ASSERT_TRUE(client.start_shared(104));
    EXPECT_FALSE(
        client.receive_shared("$thing/down/signaling/p/d", "private-invalid", 105, 1000000));
    ASSERT_TRUE(client.rejection_facts());
    EXPECT_EQ(client.rejection_facts()->method, HumanRejectionMethod::Unknown);
    EXPECT_FALSE(client.rejection_facts()->code);
}

// UDP邀请必须先有驱动能力且本人接听；媒体参数不能提前打开真实采集。
TEST(HumanCallUDPAV, ExplicitLocalAcceptAndStrictAnswer) {
    TestCallMqtt mqtt;
    TestCallMedia media;
    media.udp_capable = true;
    HumanCallConfig config;
    config.udp_av = true;
    HumanCallClient client(mqtt, media, "p/d", "udp-call", config);
    ASSERT_NO_FATAL_FAILURE(ready_client(client, mqtt));
    EXPECT_NE(mqtt.sent[0].payload.find("opus16-jpeg240-ctr128-v1"), std::string::npos);
    Envelope invite;
    invite.token="invite-udp"; invite.method="sessionInvite"; invite.session="udp-session"; invite.event_sequence=1;
    invite.payload=R"({"purpose":"humanCall","callType":"video","admission":"accept","transport":"udpAv","expiresAt":1020000,"callGroupId":"group-udp","tracks":[{"trackId":"mic-1","kind":"audio","sourceId":"mic","direction":"sendonly","codec":"opus","state":"requested"},{"trackId":"speaker-1","kind":"audio","sourceId":"speaker","direction":"recvonly","codec":"opus","state":"requested"},{"trackId":"video-1","kind":"video","sourceId":"front","direction":"sendonly","codec":"jpeg","state":"requested"}]})";
    mqtt.down(invite);client.poll(104,1000000);
    ASSERT_EQ(client.state(),HumanCallState::Ringing);
    EXPECT_FALSE(media.started);EXPECT_FALSE(media.udp_selected);
    ASSERT_TRUE(client.accept(105));EXPECT_FALSE(media.started);
    mqtt.down(reply("invite-udp","requestAck","udp-session",R"({"requestMethod":"sessionAccept","result":{"state":"accepted"}})"));client.poll(106,1000000);
    ASSERT_EQ(client.state(),HumanCallState::Negotiating);EXPECT_TRUE(media.udp_selected);ASSERT_TRUE(client.offer_ready("",107));
    const auto offer=mqtt.sent.back();EXPECT_NE(offer.payload.find("udpAv"),std::string::npos);EXPECT_EQ(offer.payload.find("webrtc"),std::string::npos);
    mqtt.down(reply(offer.token,"mediaAnswer","udp-session",R"({"mediaRevision":1,"udpAv":{"profile":"opus16-jpeg240-ctr128-v1","server":"192.0.2.1","port":8551,"key":"00000000000000000000000000000000","upNonce":"0000000000000001","downNonce":"0000000000000002"}})"));client.poll(108,1000000);
    ASSERT_EQ(client.state(),HumanCallState::ConnectingMedia);ASSERT_TRUE(media.udp_answer);EXPECT_EQ(media.udp_answer->port,8551);EXPECT_FALSE(client.media_connected(109));
    media.ready=true;ASSERT_TRUE(client.media_connected(110));EXPECT_EQ(client.state(),HumanCallState::WaitingEstablished);
}
}  // namespace ur::device
