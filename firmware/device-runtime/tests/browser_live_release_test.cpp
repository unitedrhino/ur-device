// 文件说明：原节目平台终态与实际资源关闭后的release资格红测；受控驱动不代表真实WebRTC验收。
#include <gtest/gtest.h>
#include "browser_live_endpoint.hpp"
#include "ur/device/focus_endpoint.hpp"
namespace ur::device {
namespace {
// ReleaseMqtt 只保存原核心发布供握手关联，不以发布受理假造平台ACK。
struct ReleaseMqtt final : PublishPort {
    std::vector<Envelope> messages;
    bool publish(std::string_view, std::string_view body) override {
        Envelope message;
        EXPECT_EQ(decode_envelope(body, message), DecodeResult::Ok);
        messages.push_back(std::move(message));
        return true;
    }
};
// ReleaseGate 显式交接真实票据完成，不在原请求受理时报告输出已停止。
struct ReleaseGate final : OutputGroupGatePort {
    std::uint64_t ticket = 0;
    bool enabled = false, actual = false;
    std::optional<std::uint64_t> request_output_gate(bool value, std::uint64_t) override {
        enabled = value;
        actual = false;
        return ++ticket;
    }
    bool output_gate_confirmed(std::uint64_t original, bool target) const override {
        return actual && ticket == original && enabled == target;
    }
    void finish() { actual = true; }
};
// OrdinaryAuthority 即使后来恢复常规业务许可，也不能默认开启Closed租约的cleanup入口。
struct OrdinaryAuthority final : FocusAuthority {
    bool permitted = true;  // 模拟原权限撤销；没有Live终态/实际释放/原父证据。
    std::optional<FocusPermit> authorize(const FocusCommand& command, std::uint64_t,
                                         std::uint64_t) override {
        return permitted ? std::optional<FocusPermit>(FocusPermit{command, 3600000, 1})
                         : std::nullopt;
    }
    bool valid(const FocusPermit&, std::uint64_t, std::uint64_t) const override {
        return permitted;
    }
};
// ControlledCleanupAuthority
// 只供核心边界测试；显式关闭资格不代替Browser适配的真实平台/父/连接证明。
struct ControlledCleanupAuthority final : FocusAuthority {
    bool permitted = true;          // 原取得许可撤销后不能再作为当前输出权。
    bool cleanup = false;           // 模拟已验证原终态的有限关闭资格。
    std::uint64_t until = 3600000;  // 固定关闭保留窗，原请求重放不得续期。
    std::optional<FocusPermit> authorize(const FocusCommand& command, std::uint64_t now,
                                         std::uint64_t) override {
        return now < until && (permitted || (cleanup && command.operation == "release"))
                   ? std::optional<FocusPermit>(FocusPermit{command, until, 1})
                   : std::nullopt;
    }
    bool valid(const FocusPermit& permit, std::uint64_t now, std::uint64_t) const override {
        return now < until && (permitted || (cleanup && permit.scope.operation == "release"));
    }
    bool closed_release_valid(const FocusPermit& permit, std::uint64_t now,
                              std::uint64_t wall) const override {
        return cleanup && permit.scope.operation == "release" && valid(permit, now, wall);
    }
};
// focus 原命令通过生产解析器，原CAS/focus/完整资源不能以手拼结构体省略。
FocusCommand focus(std::string operation, unsigned revision, std::string session = "programme",
                   std::string resources = "\"speaker\",\"display\"") {
    const auto body =
        "{\"method\":\"focusControl\",\"msgToken\":\"" + operation + "\",\"sessionId\":\"" +
        session + "\",\"eventSeq\":" + std::to_string(revision + 1) +
        ",\"params\":{\"operation\":\"" + operation +
        "\",\"expectedRevision\":" + std::to_string(revision) +
        ",\"focusId\":\"original-focus\",\"resources\":[" + resources +
        "],\"priority\":\"background\",\"resumePolicy\":\"explicit\"" +
        ((operation == "acquire" || operation == "restore") ? ",\"expiresAt\":2000" : "") + "}}";
    FocusCommand command;
    EXPECT_EQ(decode_focus_control(WireRoute::Signaling, body, command), WireResult::Ok);
    return command;
}
// BrowserLiveReleaseTest 同一原AI父/MQTT/Endpoint，两阶段真实原协议握手后才讨论关闭资格。
class BrowserLiveReleaseTest : public ::testing::Test {
protected:
    ReleaseMqtt mqtt;
    std::array<BrowserLiveLane, 3> lanes{BrowserLiveLane("audio"), BrowserLiveLane("video"),
                                         BrowserLiveLane("audioVideo")};
    std::optional<std::string> parent{"original-ai"};
    BrowserLiveFactory factory{
        {&lanes[0], &lanes[1], &lanes[2]}, {true, true}, [&] { return parent; }, 3600000};
    BrowserLiveEndpoint endpoint{mqtt, factory, {true, true}, "up", "down", 3600000};
    std::uint64_t now = 10;
    std::string invite(std::string session = "programme", std::string token = "invite") {
        return "{\"method\":\"sessionInvite\",\"msgToken\":\"" + token + "\",\"sessionId\":\"" +
               session +
               R"(","eventSeq":1,"params":{"purpose":"liveBroadcast","admission":"direct","transport":"webrtc","expiresAt":100000,"tracks":[{"trackId":"broadcast-out","kind":"audio","sourceId":"broadcast-audio","direction":"recvonly","codec":"opus","state":"requested"},{"trackId":"broadcast-video-out","kind":"video","sourceId":"broadcast-video","direction":"recvonly","codec":"H264","state":"requested"}]}})";
    }
    bool down(std::string token, std::string method, std::string payload,
              std::optional<int> code = 200, std::optional<std::uint64_t> seq = {},
              std::string session = "programme") {
        Envelope message;
        message.request = false;
        message.token = std::move(token);
        message.method = std::move(method);
        message.session = std::move(session);
        message.payload = std::move(payload);
        message.code = code;
        message.event_sequence = seq;
        std::string body;
        EXPECT_EQ(encode_envelope(message, body), DecodeResult::Ok);
        return endpoint.receive("down", body, ++now, 1000);
    }
    std::string local(std::string kind, std::string extra = {}) {
        return "{\"kind\":\"" + kind + "\",\"instance\":\"1\",\"mediaRevision\":1" + extra + "}";
    }
    void SetUp() override {
        for (auto& lane : lanes) {
            const auto ticket = lane.request_output_gate(false, ++now);
            ASSERT_TRUE(ticket);
            ASSERT_TRUE(lane.take_command());
            ASSERT_TRUE(lane.receive("{\"kind\":\"liveLaneGated\",\"lane\":\"" + lane.lane() +
                                         "\",\"gateTicket\":\"" + std::to_string(*ticket) +
                                         "\",\"enabled\":false}",
                                     ++now));
        }
        ASSERT_TRUE(endpoint.begin_connection(++now));
    }
    void established() {
        ASSERT_TRUE(endpoint.receive("down", invite(), ++now, 1000));
        ASSERT_TRUE(down("invite", "requestAck",
                         R"({"requestMethod":"sessionAccept","result":{"state":"negotiating"}})"));
        ASSERT_TRUE(endpoint.active_media()->take_command());
        ASSERT_TRUE(endpoint.receive_local(local("liveOffer", R"(,"sdp":"v=0\r\ncontract")")));
        endpoint.tick(++now, 1000);
        const auto offer = mqtt.messages.back().token;
        ASSERT_TRUE(
            down(offer, "mediaAnswer",
                 R"({"mediaRevision":1,"webrtc":{"iceMode":"complete","sdp":"v=0\r\nanswer"}})"));
        ASSERT_TRUE(endpoint.active_media()->take_command());
        ASSERT_TRUE(endpoint.receive_local(local("liveConnected")));
        endpoint.tick(++now, 1000);
        const auto media = mqtt.messages.back().token;
        ASSERT_TRUE(down(
            media, "requestAck",
            R"({"requestMethod":"mediaState","result":{"state":"connected","mediaRevision":1}})"));
        ASSERT_TRUE(down("established", "sessionEstablished",
                         R"({"state":"established","mediaRevision":1})", {}, 2));
        ASSERT_TRUE(endpoint.scope());
        ASSERT_TRUE(endpoint.scope()->established);
    }
    void platform_end() {
        ASSERT_TRUE(
            down("ended", "sessionEnded", R"({"state":"ended","reason":"userStopped"})", {}, 3));
    }
    void actual_stopped() {
        ASSERT_TRUE(endpoint.active_media()->take_command());
        ASSERT_TRUE(endpoint.receive_local(local(
            "liveStopped",
            R"(,"peerClosed":true,"tracksStopped":true,"outputsDetached":true,"callbacksStopped":true,"transformsStopped":true)")));
        endpoint.tick(++now, 1000);
        ASSERT_TRUE(endpoint.released());
    }
    // cleanup_after_parent_change 用原同对象/同连接的完整Engine验证父不再当前时只有原关闭权。
    void cleanup_after_parent_change(bool new_parent) {
        established();
        std::array<ReleaseGate, 4> gates;
        OutputFocusDriver driver({{"", "", {"speaker"}, &gates[0]},
                                  {"", "", {"speaker"}, &gates[1]},
                                  {"", "", {"display"}, &gates[2]},
                                  {"", "", {"speaker", "display"}, &gates[3]}},
                                 0, FocusBindingMode::DeferredReleased);
        ASSERT_TRUE(driver.initialize(++now));
        for (auto& gate : gates)
            gate.finish();
        ASSERT_TRUE(driver.poll(++now));
        BrowserLiveFocusAuthority authority(
            driver, 3600000, [&] { return parent; }, [&] { return endpoint.scope(); },
            [&](std::string_view session) { return endpoint.release_scope(session); });
        FocusEndpoint focus_endpoint(mqtt, driver, authority, {"speaker", "display"}, "up", "down",
                                     3600000);
        ASSERT_TRUE(focus_endpoint.begin_connection(++now));
        const auto submit = [&](const FocusCommand& command) {
            std::string body;
            EXPECT_EQ(encode_envelope(command.envelope, body), DecodeResult::Ok);
            return focus_endpoint.receive("down", body, ++now, 1000);
        };
        ASSERT_EQ(submit(focus("acquire", 0)), FocusEndpointResult::Handled);
        focus_endpoint.poll(++now, 1000);
        gates[3].finish();
        ASSERT_TRUE(driver.poll(++now));
        focus_endpoint.poll(++now, 1000);
        ASSERT_EQ(driver.revision(), 1U);
        platform_end();
        focus_endpoint.poll(++now, 1000);
        actual_stopped();
        gates[3].finish();
        ASSERT_TRUE(driver.poll(++now));
        focus_endpoint.poll(++now, 1000);
        if (new_parent)
            parent = "next-ai";
        else
            parent.reset();
        const auto original = endpoint.release_scope("programme");
        ASSERT_TRUE(original);
        EXPECT_EQ(original->parent_session, "original-ai");
        EXPECT_FALSE(original->authorized);
        EXPECT_FALSE(original->established);
        EXPECT_FALSE(authority.authorize(focus("acquire", 1), ++now, 1000));
        EXPECT_FALSE(authority.authorize(focus("restore", 1), ++now, 1000));
        EXPECT_FALSE(authority.authorize(focus("suspend", 1), ++now, 1000));
        EXPECT_FALSE(endpoint.active_media());
        ASSERT_EQ(submit(focus("release", 1)), FocusEndpointResult::Handled);
        focus_endpoint.poll(++now, 1000);
        EXPECT_EQ(driver.revision(), 1U);
        gates[3].finish();
        ASSERT_TRUE(driver.poll(++now));
        focus_endpoint.poll(++now, 1000);
        EXPECT_EQ(driver.revision(), 2U);
        EXPECT_EQ(mqtt.messages.back().code, 200);
        EXPECT_NE(mqtt.messages.back().payload.find("released"), std::string::npos);
        EXPECT_FALSE(gates[3].enabled);
    }
};
}  // namespace

TEST_F(BrowserLiveReleaseTest, PlatformEndedRetainsOriginalActualReleasedScopeWithoutNewMedia) {
    established();
    platform_end();
    actual_stopped();
    const auto original = endpoint.scope();
    ASSERT_TRUE(original);
    EXPECT_EQ(original->session, "programme");
    EXPECT_TRUE(original->audio);
    EXPECT_TRUE(original->video);
    EXPECT_FALSE(original->authorized);
    EXPECT_FALSE(original->established);
    EXPECT_FALSE(endpoint.active_media());
    EXPECT_TRUE(endpoint.owns("programme"));
}
TEST_F(BrowserLiveReleaseTest, LocalCloseCannotProducePlatformTerminalCleanupProof) {
    established();
    endpoint.close(++now);
    actual_stopped();
    EXPECT_FALSE(endpoint.scope());
    EXPECT_FALSE(endpoint.active_media());
}
TEST_F(BrowserLiveReleaseTest, OriginalCleanupReleaseSurvivesParentEndingWithoutReopen) {
    cleanup_after_parent_change(false);
}
TEST_F(BrowserLiveReleaseTest,
       OriginalCleanupReleaseSurvivesNewParentWithoutBorrowingItsPermission) {
    cleanup_after_parent_change(true);
}
// 真正原Engine会因许可撤销进入Closed；不能只测Authority/Driver绿色就误称新release可穿越关闭栅栏。
TEST_F(BrowserLiveReleaseTest, ClosedOriginalFocusEndpointAcceptsOnlyActualLiveCleanupRelease) {
    established();
    std::array<ReleaseGate, 4> gates;
    OutputFocusDriver driver({{"", "", {"speaker"}, &gates[0]},
                              {"", "", {"speaker"}, &gates[1]},
                              {"", "", {"display"}, &gates[2]},
                              {"", "", {"speaker", "display"}, &gates[3]}},
                             0, FocusBindingMode::DeferredReleased);
    ASSERT_TRUE(driver.initialize(++now));
    for (auto& gate : gates)
        gate.finish();
    ASSERT_TRUE(driver.poll(++now));
    BrowserLiveFocusAuthority authority(
        driver, 3600000, [&] { return parent; }, [&] { return endpoint.scope(); },
        [&](std::string_view session) { return endpoint.release_scope(session); });
    FocusEndpoint focus_endpoint(mqtt, driver, authority, {"speaker", "display"}, "up", "down",
                                 3600000);
    ASSERT_TRUE(focus_endpoint.begin_connection(++now));
    const auto submit = [&](const FocusCommand& command) {
        std::string body;
        EXPECT_EQ(encode_envelope(command.envelope, body), DecodeResult::Ok);
        return focus_endpoint.receive("down", body, ++now, 1000);
    };
    ASSERT_EQ(submit(focus("acquire", 0)), FocusEndpointResult::Handled);
    focus_endpoint.poll(++now, 1000);
    gates[3].finish();
    ASSERT_TRUE(driver.poll(++now));
    focus_endpoint.poll(++now, 1000);
    ASSERT_EQ(mqtt.messages.back().method, "focusState");
    ASSERT_EQ(mqtt.messages.back().code, 200);
    EXPECT_EQ(driver.revision(), 1U);
    platform_end();
    focus_endpoint.poll(++now, 1000);  // 原acquire真实失效，Engine必须关闭输出而非延长其有效期。
    EXPECT_FALSE(driver.owns(focus("acquire", 0)));
    actual_stopped();
    gates[3].finish();
    ASSERT_TRUE(driver.poll(++now));
    focus_endpoint.poll(++now, 1000);
    ASSERT_EQ(submit(focus("release", 1)), FocusEndpointResult::Handled);
    focus_endpoint.poll(++now, 1000);
    EXPECT_EQ(driver.revision(), 1U);  // 新false实际完成之前，不能公布新Released修订。
    const auto release_ticket = gates[3].ticket;
    ASSERT_EQ(submit(focus("release", 1)), FocusEndpointResult::Handled);
    focus_endpoint.poll(++now, 1000);
    EXPECT_EQ(gates[3].ticket, release_ticket);  // pending同token只等待原票据，不能制造第二次执行。
    EXPECT_EQ(driver.revision(), 1U);
    gates[3].finish();
    ASSERT_TRUE(driver.poll(++now));
    focus_endpoint.poll(++now, 1000);
    const auto actual = mqtt.messages.back();
    EXPECT_EQ(actual.method, "focusState");
    EXPECT_EQ(actual.token, "release");
    EXPECT_EQ(actual.code, 200);
    EXPECT_NE(actual.payload.find("released"), std::string::npos);
    EXPECT_EQ(driver.revision(), 2U);
    const auto ticket = gates[3].ticket;
    ASSERT_EQ(submit(focus("release", 1)), FocusEndpointResult::Handled);
    focus_endpoint.poll(++now, 1000);
    EXPECT_EQ(mqtt.messages.back().payload, actual.payload);
    EXPECT_EQ(mqtt.messages.back().token, actual.token);
    EXPECT_EQ(gates[3].ticket, ticket);
    EXPECT_EQ(driver.revision(), 2U);  // 已完成release只重放原事实，不递增CAS。
    ASSERT_EQ(submit(focus("acquire", 0)), FocusEndpointResult::Handled);
    focus_endpoint.poll(++now, 1000);  // 原token只重放历史成功，不重开旧输出。
    EXPECT_EQ(driver.revision(), 2U);
    EXPECT_EQ(gates[3].ticket, ticket);
    EXPECT_FALSE(gates[3].enabled);
}
// 物理false只证明输出停止，不证明原租约已released；容量压力不得删除尚待原cleanup的执行器。
TEST_F(BrowserLiveReleaseTest, CapacityPressureCannotRetireUnreleasedOriginalCleanupLease) {
    established();
    std::array<ReleaseGate, 4> gates;
    OutputFocusDriver driver({{"", "", {"speaker"}, &gates[0]},
                              {"", "", {"speaker"}, &gates[1]},
                              {"", "", {"display"}, &gates[2]},
                              {"", "", {"speaker", "display"}, &gates[3]}},
                             0, FocusBindingMode::DeferredReleased);
    ASSERT_TRUE(driver.initialize(++now));
    for (auto& gate : gates)
        gate.finish();
    ASSERT_TRUE(driver.poll(++now));
    BrowserLiveFocusAuthority authority(
        driver, 3600000, [&] { return parent; }, [&] { return endpoint.scope(); },
        [&](std::string_view session) { return endpoint.release_scope(session); });
    FocusEndpoint focus_endpoint(mqtt, driver, authority, {"speaker", "display"}, "up", "down",
                                 3600000);
    ASSERT_TRUE(focus_endpoint.begin_connection(++now));
    const auto submit = [&](const FocusCommand& command) {
        std::string body;
        EXPECT_EQ(encode_envelope(command.envelope, body), DecodeResult::Ok);
        return focus_endpoint.receive("down", body, ++now, 1000);
    };
    ASSERT_EQ(submit(focus("acquire", 0)), FocusEndpointResult::Handled);
    focus_endpoint.poll(++now, 1000);
    gates[3].finish();
    ASSERT_TRUE(driver.poll(++now));
    focus_endpoint.poll(++now, 1000);
    ASSERT_EQ(mqtt.messages.back().code, 200);
    ASSERT_EQ(driver.revision(), 1U);
    platform_end();
    focus_endpoint.poll(++now, 1000);
    actual_stopped();
    gates[3].finish();
    ASSERT_TRUE(driver.poll(++now));
    focus_endpoint.poll(++now, 1000);
    // 其它请求只有准入/拒绝事实，不获得原节目资源；前三个占空槽，第四个不能偷原未释放槽。
    for (unsigned index = 0; index < 3; ++index)
        ASSERT_EQ(submit(focus("acquire", 1, "other-" + std::to_string(index))),
                  FocusEndpointResult::Handled);
    EXPECT_EQ(submit(focus("acquire", 1, "capacity-5")), FocusEndpointResult::Full);
    ASSERT_EQ(submit(focus("release", 1)), FocusEndpointResult::Handled);
    focus_endpoint.poll(++now, 1000);
    EXPECT_EQ(driver.revision(), 1U);
    gates[3].finish();
    ASSERT_TRUE(driver.poll(++now));
    focus_endpoint.poll(++now, 1000);
    EXPECT_EQ(driver.revision(), 2U);
    EXPECT_FALSE(gates[3].enabled);
}
// 原已Established/原focus租约/平台Ended/actual退出四种证据必须同时成立；取释放许可不重建媒体。
TEST_F(BrowserLiveReleaseTest,
       OriginalEndedWholeGroupReleaseAuthorizesButCannotReopenOrChangeFocus) {
    established();
    std::array<ReleaseGate, 4> gates;
    OutputFocusDriver driver({{"", "", {"speaker"}, &gates[0]},
                              {"", "", {"speaker"}, &gates[1]},
                              {"", "", {"display"}, &gates[2]},
                              {"", "", {"speaker", "display"}, &gates[3]}},
                             0, FocusBindingMode::DeferredReleased);
    ASSERT_TRUE(driver.initialize(++now));
    for (auto& gate : gates)
        gate.finish();
    ASSERT_TRUE(driver.poll(++now));
    BrowserLiveFocusAuthority authority(
        driver, 3600000, [&] { return parent; }, [&] { return endpoint.scope(); });
    const auto acquire = focus("acquire", 0);
    ASSERT_TRUE(authority.authorize(acquire, ++now, 1000));
    ASSERT_TRUE(driver.begin(acquire, 1));
    gates[3].finish();
    ASSERT_TRUE(driver.poll(++now));
    ASSERT_TRUE(driver.take_fact("programme", "original-focus"));
    platform_end();
    EXPECT_FALSE(authority.authorize(focus("release", 1), ++now, 1000));
    actual_stopped();
    EXPECT_FALSE(driver.stop_scope("programme", "original-focus", {"speaker", "display"}));
    gates[3].finish();
    ASSERT_TRUE(driver.poll(++now));
    const auto release = focus("release", 1);
    const auto permit = authority.authorize(release, ++now, 1000);
    ASSERT_TRUE(permit);
    EXPECT_TRUE(authority.valid(*permit, ++now, 1000));
    EXPECT_FALSE(authority.authorize(focus("acquire", 1), ++now, 1000));
    EXPECT_FALSE(authority.authorize(focus("restore", 1), ++now, 1000));
    EXPECT_FALSE(authority.authorize(focus("suspend", 1), ++now, 1000));
    auto changed = release;
    changed.focus_id = "different-focus";
    EXPECT_FALSE(authority.authorize(changed, ++now, 1000));
    changed = release;
    changed.resources = {"speaker"};
    EXPECT_FALSE(authority.authorize(changed, ++now, 1000));
    parent = "another-ai";
    EXPECT_TRUE(authority.authorize(release, ++now, 1000));
    EXPECT_FALSE(authority.authorize(focus("restore", 1), ++now, 1000));
    parent.reset();
    EXPECT_TRUE(authority.authorize(release, ++now, 1000));
    EXPECT_FALSE(authority.authorize(focus("acquire", 1), ++now, 1000));
    EXPECT_FALSE(endpoint.active_media());
    EXPECT_EQ(driver.revision(), 1U);
}
TEST_F(BrowserLiveReleaseTest, UnreleasedOriginalMediaNeverProducesRetiredCleanupScope) {
    established();
    platform_end();
    EXPECT_FALSE(endpoint.released());
    ASSERT_TRUE(endpoint.active_media());
    const auto original = endpoint.scope();
    ASSERT_TRUE(original);
    EXPECT_FALSE(original->authorized);
    EXPECT_FALSE(original->established);
}
TEST_F(BrowserLiveReleaseTest, OriginalCleanupRetentionNeverRefreshesOnLateInviteOrClockRollback) {
    established();
    platform_end();
    const auto ended_at = now;
    actual_stopped();
    ASSERT_TRUE(endpoint.scope());
    EXPECT_TRUE(endpoint.receive("down", invite(), ended_at + 599999, 1000));
    endpoint.tick(ended_at + 600000, 1000);
    EXPECT_FALSE(endpoint.scope());
    endpoint.tick(ended_at, 1000);
    EXPECT_FALSE(endpoint.scope());
    EXPECT_TRUE(endpoint.owns("programme"));
    EXPECT_FALSE(endpoint.active_media());
}
TEST_F(BrowserLiveReleaseTest, OriginalInvitationFactsStayBoundedNonEvictingNearCapacity) {
    unsigned accepted = 0;
    for (unsigned index = 0; index < 40; ++index) {
        const auto session = "bounded-" + std::to_string(index),
                   token = "token-" + std::to_string(index);
        if (!endpoint.receive("down", invite(session, token), ++now, 1000))
            break;
        ++accepted;
        ASSERT_TRUE(down("ended-" + std::to_string(index), "sessionEnded",
                         R"({"state":"ended","reason":"userStopped"})", {}, 2, session));
        endpoint.tick(++now, 1000);
        ASSERT_TRUE(endpoint.released());
        ASSERT_TRUE(endpoint.settled());
    }
    EXPECT_GT(accepted, 0U);
    EXPECT_LE(accepted, 32U);
    EXPECT_TRUE(endpoint.owns("bounded-0"));
    const auto before = mqtt.messages.size();
    EXPECT_TRUE(endpoint.receive("down", invite("bounded-0", "token-0"), ++now, 1000));
    EXPECT_EQ(mqtt.messages.size(), before);
    EXPECT_FALSE(endpoint.active_media());
}
// 这项必须先实际红：被动stop后的false不是显式release，但新原release仍应走新false实际确认与CAS。
TEST(BrowserLiveReleaseDriverTest, OriginalPassiveStoppedReleaseNeedsNewActualFalseTicketAndCas) {
    ReleaseGate gate;
    OutputFocusDriver driver({{"programme", "original-focus", {"speaker", "display"}, &gate}}, 0);
    ASSERT_TRUE(driver.initialize(10));
    gate.finish();
    ASSERT_TRUE(driver.poll(11));
    const auto acquire = focus("acquire", 0);
    ASSERT_TRUE(driver.begin(acquire, 1));
    gate.finish();
    ASSERT_TRUE(driver.poll(12));
    ASSERT_TRUE(driver.take_fact("programme", "original-focus"));
    EXPECT_FALSE(driver.stop_scope("programme", "original-focus", {"speaker", "display"}));
    gate.finish();
    ASSERT_TRUE(driver.poll(13));
    ASSERT_TRUE(driver.stop_scope("programme", "original-focus", {"speaker", "display"}));
    const auto stopped_ticket = gate.ticket;
    ASSERT_TRUE(driver.begin(focus("release", 1), 2));
    EXPECT_GT(gate.ticket, stopped_ticket);
    EXPECT_EQ(driver.revision(), 1U);
    EXPECT_FALSE(driver.take_fact("programme", "original-focus"));
    gate.finish();
    ASSERT_TRUE(driver.poll(14));
    const auto actual = driver.take_fact("programme", "original-focus");
    ASSERT_TRUE(actual);
    EXPECT_TRUE(actual->success);
    EXPECT_EQ(actual->state, "released");
    EXPECT_EQ(actual->revision, 2U);
    EXPECT_EQ(actual->generation, 2U);
    EXPECT_FALSE(driver.take_fact("programme", "original-focus"));
}
TEST(BrowserLiveReleaseDriverTest, UnconfirmedOrMismatchedOriginalGroupCannotBeCleanupReleased) {
    ReleaseGate gate;
    OutputFocusDriver driver({{"programme", "original-focus", {"speaker", "display"}, &gate}}, 0);
    ASSERT_TRUE(driver.initialize(10));
    gate.finish();
    ASSERT_TRUE(driver.poll(11));
    ASSERT_TRUE(driver.begin(focus("acquire", 0), 1));
    gate.finish();
    ASSERT_TRUE(driver.poll(12));
    ASSERT_TRUE(driver.take_fact("programme", "original-focus"));
    EXPECT_FALSE(driver.stop_scope("programme", "original-focus", {"speaker", "display"}));
    const auto ticket = gate.ticket;
    EXPECT_FALSE(driver.begin(focus("release", 1), 2));
    gate.finish();
    ASSERT_TRUE(driver.poll(13));
    EXPECT_FALSE(driver.begin(focus("release", 1, "other"), 2));
    EXPECT_FALSE(driver.begin(focus("release", 1, "programme", "\"speaker\""), 2));
    EXPECT_FALSE(driver.begin(focus("restore", 1), 2));
    EXPECT_FALSE(driver.begin(focus("suspend", 1), 2));
    EXPECT_EQ(gate.ticket, ticket);
    EXPECT_EQ(driver.revision(), 1U);
}
// 默认契约保持Closed；不得因普通Authority恢复true就允许旧file/voice/general租约重新执行。
TEST(BrowserLiveReleaseDriverTest,
     ClosedOrdinaryAuthorityCannotOptIntoLiveCleanupByRestoringPermission) {
    ReleaseGate gate;
    OrdinaryAuthority authority;
    OutputFocusDriver driver({{"programme", "original-focus", {"speaker", "display"}, &gate}}, 0);
    ASSERT_TRUE(driver.initialize(10));
    gate.finish();
    ASSERT_TRUE(driver.poll(11));
    FocusEngine engine(driver, authority, "programme", "original-focus", {"speaker", "display"},
                       3600000);
    std::string original;
    ASSERT_EQ(encode_envelope(focus("acquire", 0).envelope, original), DecodeResult::Ok);
    ASSERT_EQ(engine.submit(original, 12, 1000).admission, FocusAdmission::Accepted);
    engine.poll(13, 1000);
    gate.finish();
    ASSERT_TRUE(driver.poll(14));
    engine.poll(15, 1000);
    ASSERT_TRUE(engine.take_reply());
    EXPECT_EQ(driver.revision(), 1U);
    authority.permitted = false;
    engine.poll(16, 1000);
    EXPECT_TRUE(engine.closed());
    gate.finish();
    ASSERT_TRUE(driver.poll(17));
    engine.poll(18, 1000);
    authority.permitted = true;
    std::string release;
    ASSERT_EQ(encode_envelope(focus("release", 1).envelope, release), DecodeResult::Ok);
    const auto ticket = gate.ticket;
    EXPECT_EQ(engine.submit(release, 19, 1000).admission, FocusAdmission::Closed);
    EXPECT_EQ(driver.revision(), 1U);
    EXPECT_EQ(gate.ticket, ticket);
    EXPECT_TRUE(engine.closed());
}
// 空租约与受理中但未知actual的取得都不能借显式cleanup资格补造原held历史。
TEST(BrowserLiveReleaseDriverTest, CleanupCannotCreateLeaseWithoutKnownActualAcquisition) {
    for (const bool pending_acquisition : {false, true}) {
        SCOPED_TRACE(pending_acquisition);
        ReleaseGate gate;
        ControlledCleanupAuthority authority;
        OutputFocusDriver driver({{"programme", "original-focus", {"speaker", "display"}, &gate}},
                                 0);
        ASSERT_TRUE(driver.initialize(10));
        gate.finish();
        ASSERT_TRUE(driver.poll(11));
        FocusEngine engine(driver, authority, "programme", "original-focus", {"speaker", "display"},
                           3600000);
        if (pending_acquisition) {
            std::string acquire;
            ASSERT_EQ(encode_envelope(focus("acquire", 0).envelope, acquire), DecodeResult::Ok);
            ASSERT_EQ(engine.submit(acquire, 12, 1000).admission, FocusAdmission::Accepted);
            engine.poll(13, 1000);  // 真正驱动仍pending，不产生Core已取得事实。
        }
        authority.permitted = false;
        authority.cleanup = true;
        engine.close();
        gate.finish();
        ASSERT_TRUE(driver.poll(14));
        engine.poll(15, 1000);
        std::string release;
        ASSERT_EQ(encode_envelope(focus("release", 0).envelope, release), DecodeResult::Ok);
        const auto ticket = gate.ticket;
        EXPECT_EQ(engine.submit(release, 16, 1000).admission, FocusAdmission::Closed);
        EXPECT_EQ(driver.revision(), 0U);
        EXPECT_EQ(gate.ticket, ticket);
        EXPECT_TRUE(engine.closed());
    }
}
// 已知原held也不能越过固定cleanup或宿主期限；不能从请求中制造新的关闭保留窗。
TEST(BrowserLiveReleaseDriverTest, CleanupRefusesExpiredPermitAndOriginalHostDeadline) {
    for (const bool host_expired : {false, true}) {
        SCOPED_TRACE(host_expired);
        ReleaseGate gate;
        ControlledCleanupAuthority authority;
        OutputFocusDriver driver({{"programme", "original-focus", {"speaker", "display"}, &gate}},
                                 0);
        ASSERT_TRUE(driver.initialize(10));
        gate.finish();
        ASSERT_TRUE(driver.poll(11));
        FocusEngine engine(driver, authority, "programme", "original-focus", {"speaker", "display"},
                           host_expired ? 30 : 3600000);
        std::string acquire;
        ASSERT_EQ(encode_envelope(focus("acquire", 0).envelope, acquire), DecodeResult::Ok);
        // 原租约截止必须落在宿主期限内，不把无效取得当原已知持有。
        if (host_expired) {
            auto command = focus("acquire", 0);
            command.envelope.payload =
                R"({"operation":"acquire","expectedRevision":0,"focusId":"original-focus","resources":["speaker","display"],"priority":"background","resumePolicy":"explicit","expiresAt":1015})";
            ASSERT_EQ(encode_envelope(command.envelope, acquire), DecodeResult::Ok);
        }
        ASSERT_EQ(engine.submit(acquire, 12, 1000).admission, FocusAdmission::Accepted);
        engine.poll(13, 1000);
        gate.finish();
        ASSERT_TRUE(driver.poll(14));
        engine.poll(15, 1000);
        ASSERT_TRUE(engine.take_reply());
        ASSERT_EQ(driver.revision(), 1U);
        authority.permitted = false;
        authority.cleanup = true;
        authority.until = host_expired ? 3600000 : 30;
        engine.poll(16, 1000);
        gate.finish();
        ASSERT_TRUE(driver.poll(17));
        engine.poll(18, 1000);
        std::string release;
        ASSERT_EQ(encode_envelope(focus("release", 1).envelope, release), DecodeResult::Ok);
        const auto ticket = gate.ticket;
        EXPECT_EQ(engine.submit(release, 30, 1000).admission, FocusAdmission::Closed);
        EXPECT_EQ(driver.revision(), 1U);
        EXPECT_EQ(gate.ticket, ticket);
        EXPECT_TRUE(engine.closed());
    }
}
}  // namespace ur::device
