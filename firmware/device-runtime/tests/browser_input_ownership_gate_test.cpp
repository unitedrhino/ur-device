// 文件说明：桌面原输入票据两屏障回归；可控HAL不等于实际浏览器或硬件采集验收。
#include "browser_input_ownership_gate.hpp"
#include <gtest/gtest.h>

namespace ur::device {
namespace {
// Rig只提供公开固定测试scope；独立计数验证fence与线程事实不可互相替代。
struct Rig {
    bool source = true, authorized = true, fence = true;
    int fences = 0;
    BrowserInputOwnershipGate gate{7, [this] { return source; },
                                   [this](const InputGateRequest&) { return authorized; },
                                   [this](std::string_view, std::uint64_t) {
                                       ++fences;
                                       return fence;
                                   }};
    InputGateRequest request{"ai", 10, false};
    bool bind() { return gate.bind_route("ai", 10, BrowserInputRouteKind::Udp); }
    std::string receipt(std::uint64_t ticket, bool enabled = false) {
        auto body = browser_input_ownership_command({"ai", 10, enabled}, ticket);
        body.replace(body.find("inputOwnershipGate"), 18, "inputOwnershipGated");
        return body;
    }
};
TEST(BrowserInputOwnership, IndependentProbeDoesNotGrantSession) {
    Rig r;
    EXPECT_FALSE(r.gate.initial_idle_observation());
    EXPECT_FALSE(r.gate.receive(R"({"kind":"inputOwnershipIdle","bootGeneration":"8"})", 0));
    ASSERT_TRUE(r.gate.receive(R"({"kind":"inputOwnershipIdle","bootGeneration":"7"})", 0));
    EXPECT_EQ(r.gate.initial_idle_observation(), 7);
    EXPECT_FALSE(r.gate.input_gate_confirmed(7, r.request));
    r.source = false;
    EXPECT_FALSE(r.gate.initial_idle_observation());
}
TEST(BrowserInputOwnership, FenceAndActualProcessBothRequired) {
    Rig r;
    ASSERT_TRUE(r.bind());
    const auto ticket = r.gate.request_input_gate(r.request, 0);
    ASSERT_TRUE(ticket);
    EXPECT_EQ(r.fences, 1);
    EXPECT_FALSE(r.gate.input_gate_confirmed(*ticket, r.request));
    EXPECT_FALSE(r.gate.receive(r.receipt(*ticket), 1));  // 未发送不得接纳自行构造事实。
    ASSERT_TRUE(r.gate.take_command());
    EXPECT_FALSE(r.gate.take_command());
    ASSERT_TRUE(r.gate.receive(r.receipt(*ticket), 2));
    EXPECT_TRUE(r.gate.input_gate_confirmed(*ticket, r.request));
    EXPECT_TRUE(r.gate.receive(r.receipt(*ticket), 3));
    EXPECT_EQ(r.fences, 1);
    r.request.enabled = true;
    ASSERT_TRUE(r.gate.request_input_gate(r.request, 4));
    EXPECT_FALSE(r.gate.input_gate_confirmed(*ticket, {"ai", 10, false}));
}
TEST(BrowserInputOwnership, FailedFenceHasNoCommandOrTicket) {
    Rig r;
    ASSERT_TRUE(r.bind());
    r.fence = false;
    EXPECT_FALSE(r.gate.request_input_gate(r.request, 0));
    EXPECT_FALSE(r.gate.take_command());
    r.fence = true;
    EXPECT_EQ(r.gate.request_input_gate(r.request, 1), 1);
}
TEST(BrowserInputOwnership, WrongScopeAndOldTicketNeverConfirmNewOperation) {
    Rig r;
    ASSERT_TRUE(r.bind());
    ASSERT_TRUE(r.gate.request_input_gate(r.request, 0));
    ASSERT_TRUE(r.gate.take_command());
    EXPECT_FALSE(r.gate.receive(
        R"({"kind":"inputOwnershipGated","sessionId":"other","routeGeneration":"10","ticket":"1","enabled":false})",
        1));
    ASSERT_TRUE(r.gate.receive(r.receipt(1), 2));
    r.request.enabled = true;
    ASSERT_EQ(r.gate.request_input_gate(r.request, 3), 2);
    ASSERT_TRUE(r.gate.take_command());
    EXPECT_FALSE(r.gate.receive(r.receipt(1), 4));
    EXPECT_FALSE(r.gate.input_gate_confirmed(2, r.request));
    ASSERT_TRUE(r.gate.receive(r.receipt(2, true), 5));
    r.authorized = false;
    EXPECT_FALSE(r.gate.input_gate_confirmed(2, r.request));
}
TEST(BrowserInputOwnership, HumanScopeRequiresAuthorizationAndBindingBounded) {
    Rig r;
    ASSERT_TRUE(r.bind());
    EXPECT_FALSE(r.gate.bind_route("ai", 11, BrowserInputRouteKind::Udp));
    ASSERT_TRUE(r.gate.bind_route("human", 11, BrowserInputRouteKind::HumanSender));
    EXPECT_FALSE(r.gate.bind_route("third", 12, BrowserInputRouteKind::Udp));
    r.authorized = false;
    EXPECT_FALSE(r.gate.request_input_gate({"human", 11, true}, 0));
    EXPECT_EQ(r.fences, 0);
    r.authorized = true;
    EXPECT_TRUE(r.gate.request_input_gate({"human", 11, false}, 0));
    EXPECT_EQ(r.fences, 1);
}
TEST(BrowserInputOwnership, PendingTimeoutRollbackAndDisconnectInvalidateProof) {
    for (const auto now : {std::uint64_t{9}, std::uint64_t{2010}}) {
        Rig r;
        ASSERT_TRUE(r.bind());
        ASSERT_TRUE(r.gate.request_input_gate(r.request, 10));
        ASSERT_TRUE(r.gate.take_command());
        EXPECT_FALSE(r.gate.poll(now));
        EXPECT_FALSE(r.gate.receive(r.receipt(1), 2011));
        EXPECT_FALSE(r.gate.input_gate_confirmed(1, r.request));
    }
    Rig r;
    ASSERT_TRUE(r.bind());
    ASSERT_TRUE(r.gate.request_input_gate(r.request, 0));
    ASSERT_TRUE(r.gate.take_command());
    ASSERT_TRUE(r.gate.receive(r.receipt(1), 1));
    r.gate.disconnect();
    EXPECT_FALSE(r.gate.input_gate_confirmed(1, r.request));
}
TEST(BrowserInputOwnership, TombstonesBoundedNoTicketReuse) {
    Rig r;
    ASSERT_TRUE(r.bind());
    for (std::uint64_t n = 1; n <= 128; ++n) {
        ASSERT_EQ(r.gate.request_input_gate(r.request, n), n);
        ASSERT_TRUE(r.gate.take_command());
        ASSERT_TRUE(r.gate.receive(r.receipt(n), n));
    }
    EXPECT_FALSE(r.gate.request_input_gate(r.request, 129));
    EXPECT_FALSE(r.gate.take_command());
}
TEST(BrowserInputOwnershipWire, StrictTypesBoundsAndDuplicateKeys) {
    BrowserInputOwnershipEvent event;
    ASSERT_TRUE(decode_browser_input_ownership(
        R"({"kind":"inputOwnershipGated","sessionId":"ai","routeGeneration":"10","ticket":"18446744073709551615","enabled":false})",
        event));
    EXPECT_EQ(event.ticket, UINT64_MAX);
    for (
        const auto* body :
        {R"({"kind":"inputOwnershipIdle","bootGeneration":"7","private":true})",
         R"({"kind":"inputOwnershipIdle","bootGeneration":"7","bootGeneration":"7"})",
         R"({"kind":"inputOwnershipIdle","bootGeneration":7})",
         R"({"kind":"inputOwnershipIdle","bootGeneration":"9007199254740992"})",
         R"({"kind":"inputOwnershipGated","sessionId":"ai","routeGeneration":"10","ticket":"18446744073709551616","enabled":false})"}) {
        EXPECT_FALSE(decode_browser_input_ownership(body, event));
        EXPECT_EQ(event.ticket, 0);
    }
    EXPECT_TRUE(browser_input_ownership_command({"ai", 10, false}, 1).size() > 0);
    EXPECT_TRUE(browser_input_ownership_command({"bad\n", 10, false}, 1).empty());
    EXPECT_TRUE(browser_input_ownership_command({std::string(1, '\xff'), 10, false}, 1).empty());
}
TEST(BrowserInputOwnership, MissingPortsAndCallbackFailureRemainUnknown) {
    BrowserInputOwnershipGate missing(7, {}, {}, {});
    EXPECT_FALSE(missing.initial_idle_observation());
    EXPECT_FALSE(missing.bind_route("ai", 10, BrowserInputRouteKind::Udp));
    BrowserInputOwnershipGate throwing(
        7, []() -> bool { throw 1; }, [](const InputGateRequest&) { return true; },
        [](std::string_view, std::uint64_t) { return true; });
    EXPECT_FALSE(throwing.receive(R"({"kind":"inputOwnershipIdle","bootGeneration":"7"})", 0));
    EXPECT_FALSE(throwing.request_input_gate({"ai", 10, false}, 0));
    Rig r;
    ASSERT_TRUE(r.bind());
    ASSERT_TRUE(r.gate.request_input_gate(r.request, 0));
    EXPECT_FALSE(r.gate.request_input_gate(r.request, 1));
    EXPECT_EQ(r.fences, 1);
    ASSERT_TRUE(r.gate.take_command());
    r.source = false;
    EXPECT_FALSE(r.gate.receive(r.receipt(1), 2));
    r.source = true;
    EXPECT_FALSE(r.gate.receive(r.receipt(1), 3));  // 原页面断连后恢复bool不能复活票据。
}
TEST(BrowserInputOwnership, HumanActualSenderProofCannotBeReplacedByWorkletProof) {
    Rig r;
    ASSERT_TRUE(r.gate.bind_route("human", 11, BrowserInputRouteKind::HumanSender));
    const InputGateRequest scope{"human", 11, true};
    ASSERT_EQ(r.gate.request_input_gate(scope, 0), 1);
    ASSERT_TRUE(r.gate.take_command());
    EXPECT_FALSE(r.gate.receive(
        R"({"kind":"inputOwnershipGated","sessionId":"human","routeGeneration":"11","ticket":"1","enabled":false})",
        1));
    EXPECT_FALSE(r.gate.receive(
        R"({"kind":"inputOwnershipGated","sessionId":"human","routeGeneration":"11","ticket":"1","enabled":true})",
        2));
    EXPECT_FALSE(r.gate.input_gate_confirmed(1, scope));
    ASSERT_TRUE(r.gate.receive(
        R"({"kind":"inputOwnershipSenderGated","sessionId":"human","routeGeneration":"11","ticket":"1","enabled":true})",
        3));
    EXPECT_TRUE(r.gate.input_gate_confirmed(1, scope));
}
// AI Sender复用原实际替轨票据，Worklet中间false不提供原候选麦克风的取得或释放事实。
TEST(BrowserInputOwnership, AIEndpointRequiresActualSenderAndCannotBorrowOtherScope) {
    Rig r;
    ASSERT_TRUE(r.gate.bind_route("ai-endpoint", 11, BrowserInputRouteKind::AISender));
    const InputGateRequest scope{"ai-endpoint", 11, true};
    ASSERT_EQ(r.gate.request_input_gate(scope, 0), 1);
    ASSERT_TRUE(r.gate.take_command());
    EXPECT_FALSE(r.gate.receive(
        R"({"kind":"inputOwnershipGated","sessionId":"ai-endpoint","routeGeneration":"11","ticket":"1","enabled":false})",
        1));
    EXPECT_FALSE(r.gate.receive(
        R"({"kind":"inputOwnershipSenderGated","sessionId":"other","routeGeneration":"11","ticket":"1","enabled":true})",
        2));
    ASSERT_TRUE(r.gate.receive(
        R"({"kind":"inputOwnershipSenderGated","sessionId":"ai-endpoint","routeGeneration":"11","ticket":"1","enabled":true})",
        3));
    EXPECT_TRUE(r.gate.input_gate_confirmed(1, scope));
    EXPECT_FALSE(r.gate.input_gate_confirmed(1, {"ai-endpoint", 12, true}));
}
TEST(BrowserInputOwnership, HumanSuspendRequiresSenderNullProofAndOriginalFence) {
    Rig r;
    ASSERT_TRUE(r.gate.bind_route("human", 11, BrowserInputRouteKind::HumanSender));
    const InputGateRequest scope{"human", 11, false};
    ASSERT_EQ(r.gate.request_input_gate(scope, 0), 1);
    EXPECT_EQ(r.fences, 1);
    ASSERT_TRUE(r.gate.take_command());
    EXPECT_FALSE(r.gate.receive(
        R"({"kind":"inputOwnershipGated","sessionId":"human","routeGeneration":"11","ticket":"1","enabled":false})",
        1));
    EXPECT_FALSE(r.gate.receive(
        R"({"kind":"inputOwnershipSenderGated","sessionId":"human","routeGeneration":"12","ticket":"1","enabled":false})",
        2));
    EXPECT_FALSE(r.gate.input_gate_confirmed(1, scope));
    ASSERT_TRUE(r.gate.receive(
        R"({"kind":"inputOwnershipSenderGated","sessionId":"human","routeGeneration":"11","ticket":"1","enabled":false})",
        3));
    EXPECT_TRUE(r.gate.input_gate_confirmed(1, scope));
    EXPECT_EQ(r.fences, 1);
}
TEST(BrowserInputOwnership, UDPNeverUsesSenderProofEvenWithExactTicketAndScope) {
    Rig r;
    ASSERT_TRUE(r.bind());
    ASSERT_EQ(r.gate.request_input_gate(r.request, 0), 1);
    ASSERT_TRUE(r.gate.take_command());
    EXPECT_FALSE(r.gate.receive(
        R"({"kind":"inputOwnershipSenderGated","sessionId":"ai","routeGeneration":"10","ticket":"1","enabled":false})",
        1));
    EXPECT_FALSE(r.gate.input_gate_confirmed(1, r.request));
    ASSERT_TRUE(r.gate.receive(r.receipt(1), 2));
    EXPECT_TRUE(r.gate.input_gate_confirmed(1, r.request));
}
}  // namespace
}  // namespace ur::device
