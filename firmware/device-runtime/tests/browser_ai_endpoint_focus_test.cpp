// 文件说明：候选AI接续使用正式双向HAL与原票据；所有时间和实际回执可控，不冒认端云验收。
#include "browser_ai_endpoint_focus.hpp"
#include <gtest/gtest.h>
#include <map>

namespace ur::device {
namespace {
// Output 将受理与实际门控分离，只有confirm推进同一票据事实。
class Output final : public OutputGroupGatePort {
public:
  std::optional<std::uint64_t> request_output_gate(bool enabled,
                                                   std::uint64_t) override {
    pending[++next] = enabled;
    return next;
  }
  bool output_gate_confirmed(std::uint64_t ticket,
                             bool enabled) const override {
    const auto found = actual.find(ticket);
    return found != actual.end() && found->second == enabled;
  }
  void confirm() { actual = pending; } // 显式夹具实态，不由request回填。
  std::uint64_t next = 0;              // 当前原输出票据。
  std::map<std::uint64_t, bool> pending, actual; // 分离原请求与实际确认。
};
// EndpointFocusRig 固定原候选及原宿主截止，借用view不跨调用存储。
class EndpointFocusRig : public ::testing::Test {
protected:
  explicit EndpointFocusRig(
      BrowserInputRouteKind route = BrowserInputRouteKind::Udp, bool control = false)
      : route_kind(route),
        hal(
            output, 7, 10000, 20000, [this] { return source; },
            [](std::string_view session, std::uint64_t generation) {
              return session == "candidate" && generation == 1;
            },
            [this]() -> std::optional<AIEndpointFocusScope> {
              return allowed ? std::optional<AIEndpointFocusScope>(scope)
                             : std::nullopt;
            },
            [this](const std::string &body) {
              commands.push_back(body);
              return emission;
            },
            route, control ? BrowserAIEndpointFocus::Released([this](std::string_view session) {
              return formal_released && session == scope.session_id;
            }) : BrowserAIEndpointFocus::Released{}) {
    if (control) {
      scope.endpoint_epoch = 4;
      scope.capability_revision = 1;
    }
  }
  FocusCommand command(std::string operation = "acquire",
                       std::string token = "original", std::string focus = "focus",
                       unsigned sequence = 0) {
    const auto body = "{\"msgToken\":\"" + token +
                      "\",\"method\":\"focusControl\",\"sessionId\":"
        "\"candidate\",\"eventSeq\":" + std::to_string(sequence ? sequence :
                    (operation == "acquire" ? 1 : 2)) + ","
                      "\"params\":{\"focusId\":\"" + focus + "\",\"operation\":\"" +
                      operation +
                      "\",\"resources\":[\"microphone\",\"speaker\"],"
                      "\"priority\":\"conversation\","
                      "\"resumePolicy\":\"explicit\",\"expectedRevision\":" +
                      std::to_string(hal.driver().revision()) +
                      (operation == "acquire" || operation == "restore"
                           ? ",\"expiresAt\":18000"
                           : "") +
                      "}}";
    FocusCommand value; // 通过原严格解码器，不手工伪造已验证信封。
    EXPECT_EQ(decode_focus_control(WireRoute::Signaling, body, value),
              WireResult::Ok);
    return value;
  }
  void boot() {
    ASSERT_TRUE(hal.initialize(100));
    EXPECT_FALSE(hal.initial_idle());
    ASSERT_TRUE(hal.receive(
        R"({"kind":"inputOwnershipIdle","bootGeneration":"7"})", 101));
    output.confirm();
    ASSERT_TRUE(hal.poll(102, 11000));
    ASSERT_TRUE(hal.initial_idle());
  }
  void confirm_input(std::uint64_t now) {
    const auto original = commands; // 只确认本轮已真正发出的原门控消息。
    for (auto body : original) {
      const auto at = body.find("inputOwnershipGate\"");
      if (at == std::string::npos)
        continue;
      body.replace(at, 18,
                   route_kind == BrowserInputRouteKind::AISender
                       ? "inputOwnershipSenderGated"
                       : "inputOwnershipGated");
      ASSERT_TRUE(hal.receive(body, now));
      confirmed_inputs.push_back(body); // 保留原真实票据，专门重放旧代次而不改字段伪造新确认。
    }
    commands.clear();
  }
  void bind(const FocusCommand &value) {
    ASSERT_EQ(hal.prepare(scope, value, 103, 11001),
              EndpointFocusPreparation::Pending);
    ASSERT_TRUE(hal.poll(104, 11002));
    confirm_input(105);
    output.confirm();
    ASSERT_TRUE(hal.poll(106, 11003));
    ASSERT_EQ(hal.prepare(scope, value, 107, 11004),
              EndpointFocusPreparation::Ready);
  }
  void acquire(const FocusCommand &value) {
    ASSERT_TRUE(hal.authorize(value, 108, 11005));
    ASSERT_TRUE(hal.driver().begin(value, 1));
    ASSERT_TRUE(hal.poll(109, 11006));
    confirm_input(110);
    output.confirm();
    ASSERT_TRUE(hal.poll(111, 11007));
    ASSERT_TRUE(hal.driver().take_fact("candidate", "focus"));
    ASSERT_TRUE(hal.driver().owns(value));
  }
  Output output; // 同一实际输出端口。
  bool source = true, allowed = true,
       emission = true; // 断连、权限及发送各自独立。
  bool formal_released = false; // 由独立宿主已交付原正式release设置，物理false不能替代它。
  AIEndpointFocusScope scope{"candidate", "mic", "speaker",
                             2};     // 来自核心双提交事实。
  std::vector<std::string> commands; // 实际发布的输入命令，必须回原票据。
  std::vector<std::string> confirmed_inputs; // 仅测试历史；生产票据墓碑不能被清空洗成新确认。
  BrowserInputRouteKind
      route_kind; // 用例显式真实端口类型，不能拿UDP中间回执代替Sender。
  BrowserAIEndpointFocus hal; // 正式HAL，不重建协议或FocusEngine。
};
TEST_F(EndpointFocusRig, NoCandidateOrPartialResourceCannotBind) {
  boot();
  const auto value = command();
  allowed = false;
  EXPECT_EQ(hal.prepare(scope, value, 103, 11001),
            EndpointFocusPreparation::Failed);
  allowed = true;
  auto partial = value;
  partial.resources.pop_back();
  EXPECT_EQ(hal.prepare(scope, partial, 104, 11002),
            EndpointFocusPreparation::Failed);
  EXPECT_TRUE(commands.empty());
}
// 新控制代次必须实际退役原完整组；旧scope/false/replayed acquired不能授权新连接。
class EndpointControlRecoveryRig : public EndpointFocusRig {
protected:
  EndpointControlRecoveryRig() : EndpointFocusRig(BrowserInputRouteKind::Udp, true) {}
  std::optional<FocusCommand> original_release; // 保留实际受理时的旧CAS与原信封，不重签旧释放凭据。
  void stop_and_release() {
    boot();
    const auto original = command();
    bind(original);
    acquire(original);
    ASSERT_TRUE(hal.suspend_control(scope, 112, 11008));
    ASSERT_TRUE(hal.poll(113, 11009));
    confirm_input(114);
    output.confirm();
    ASSERT_TRUE(hal.poll(115, 11010));
    ASSERT_TRUE(hal.settled());
    auto release = command("release", "closed-release");
    original_release = release;
    ASSERT_TRUE(hal.authorize(release, 116, 11011));
    ASSERT_TRUE(hal.driver().begin(release, 2));
    ASSERT_TRUE(hal.poll(117, 11012));
    confirm_input(118);
    output.confirm();
    ASSERT_TRUE(hal.poll(119, 11013));
    ASSERT_TRUE(hal.driver().take_fact("candidate", "focus"));
  }
};
TEST_F(EndpointControlRecoveryRig, FormalReleaseThenNextEpochCanPrepareFreshFocusOnly) {
  ASSERT_NO_FATAL_FAILURE(stop_and_release());
  EXPECT_FALSE(hal.authorize(command(), 120, 11014));
  formal_released = true;
  ++scope.endpoint_epoch;
  const auto fresh = command("acquire", "new-acquire", "new-focus", 4);
  EXPECT_EQ(hal.prepare(scope, fresh, 121, 11015), EndpointFocusPreparation::Pending);
  ASSERT_TRUE(hal.poll(122, 11016));
  confirm_input(123);
  output.confirm();
  ASSERT_TRUE(hal.poll(124, 11017));
  EXPECT_EQ(hal.prepare(scope, fresh, 125, 11018), EndpointFocusPreparation::Ready);
  EXPECT_FALSE(hal.driver().owns(fresh));
  EXPECT_EQ(hal.driver().revision(), 2U);
  EXPECT_FALSE(hal.authorize(command(), 126, 11019));
  EXPECT_TRUE(hal.authorize(fresh, 127, 11020));
}
TEST_F(EndpointControlRecoveryRig, PhysicalFalseWithoutFormalReleaseDoesNotRebind) {
  ASSERT_NO_FATAL_FAILURE(stop_and_release());
  ++scope.endpoint_epoch;
  EXPECT_EQ(hal.prepare(scope, command("acquire", "new", "new-focus", 4), 120, 11014),
            EndpointFocusPreparation::Failed);
  EXPECT_EQ(hal.driver().revision(), 2U);
}
// 已实际双false且无原停止pending仍不等于正式release；新release票据与回执必须独立完成。
TEST_F(EndpointControlRecoveryRig, SettledFalseStillRequiresFreshReleaseTicketsAndFormalReceiptForBaseline) {
  boot();
  const auto original = command();
  bind(original);
  acquire(original);
  ASSERT_TRUE(hal.suspend_control(scope, 112, 11008));
  ASSERT_TRUE(hal.poll(113, 11009));
  confirm_input(114);
  output.confirm();
  ASSERT_TRUE(hal.poll(115, 11010));
  ASSERT_TRUE(hal.settled());
  EXPECT_FALSE(hal.control_idle_observation());
  const auto old_false = confirmed_inputs.back(); // 真实旧false不能确认新release的同值新票据。
  const auto release = command("release", "closed-release");
  ASSERT_TRUE(hal.authorize(release, 116, 11011));
  ASSERT_TRUE(hal.driver().begin(release, 2));
  ASSERT_TRUE(hal.poll(117, 11012));
  ASSERT_FALSE(commands.empty());
  EXPECT_FALSE(hal.receive(old_false, 118));
  EXPECT_FALSE(hal.control_idle_observation());
  confirm_input(119);
  output.confirm();
  ASSERT_TRUE(hal.poll(120, 11013));
  EXPECT_FALSE(hal.control_idle_observation());
  ASSERT_TRUE(hal.driver().take_fact("candidate", "focus"));
  EXPECT_FALSE(hal.control_idle_observation());
  formal_released = true;
  const auto idle = hal.control_idle_observation();
  ASSERT_TRUE(idle);
  EXPECT_EQ(idle->revision, 2U);
  EXPECT_EQ(idle->resources, (std::vector<std::string>{"microphone", "speaker"}));
  EXPECT_FALSE(hal.driver().owns(original));
}
// 旧整组释放回执与旧输入true/false票据都不能完成新focus；必须取得本次新输入和输出实际事实。
TEST_F(EndpointControlRecoveryRig, OldReleaseAndInputTicketsCannotAcquireNextEndpointEpoch) {
  ASSERT_NO_FATAL_FAILURE(stop_and_release());
  const auto old_inputs = confirmed_inputs;
  ASSERT_FALSE(old_inputs.empty());
  ASSERT_TRUE(original_release);
  const auto old_release = *original_release;
  formal_released = true;
  ++scope.endpoint_epoch;
  const auto fresh = command("acquire", "new-acquire", "new-focus", 4);
  ASSERT_EQ(hal.prepare(scope, fresh, 120, 11014), EndpointFocusPreparation::Pending);
  ASSERT_TRUE(hal.poll(121, 11015));
  confirm_input(122);
  output.confirm();
  ASSERT_TRUE(hal.poll(123, 11016));
  ASSERT_EQ(hal.prepare(scope, fresh, 124, 11017), EndpointFocusPreparation::Ready);
  EXPECT_FALSE(hal.authorize(old_release, 125, 11018));
  EXPECT_FALSE(hal.driver().take_fact("candidate", "focus"));
  ASSERT_TRUE(hal.authorize(fresh, 126, 11019));
  ASSERT_TRUE(hal.driver().begin(fresh, 3));
  ASSERT_TRUE(hal.poll(127, 11020));
  for (const auto& old : old_inputs) hal.receive(old, 128);
  output.confirm();
  ASSERT_TRUE(hal.poll(129, 11021));
  EXPECT_FALSE(hal.driver().owns(fresh));
  EXPECT_FALSE(hal.driver().take_fact("candidate", "new-focus"));
  EXPECT_EQ(hal.driver().revision(), 2U);
  confirm_input(130);
  ASSERT_TRUE(hal.poll(131, 11022));
  const auto actual = hal.driver().take_fact("candidate", "new-focus");
  ASSERT_TRUE(actual);
  EXPECT_EQ(actual->state, "acquired");
  EXPECT_EQ(hal.driver().revision(), 3U);
  EXPECT_TRUE(hal.driver().owns(fresh));
}
TEST_F(EndpointControlRecoveryRig, OldSkippedEpochAndSameFocusCannotRebind) {
  ASSERT_NO_FATAL_FAILURE(stop_and_release());
  formal_released = true;
  const auto fresh = command("acquire", "new", "new-focus", 4);
  EXPECT_EQ(hal.prepare(scope, fresh, 120, 11014), EndpointFocusPreparation::Failed);
  scope.endpoint_epoch += 2;
  EXPECT_EQ(hal.prepare(scope, fresh, 121, 11015), EndpointFocusPreparation::Failed);
  --scope.endpoint_epoch;
  EXPECT_EQ(hal.prepare(scope, command("acquire", "new", "focus", 4), 122, 11016),
            EndpointFocusPreparation::Failed);
}
TEST_F(EndpointFocusRig, PendingAndReadyDoNotAcquireOrAuthorizeNewScope) {
  boot();
  const auto value = command();
  bind(value);
  EXPECT_FALSE(hal.driver().owns(value));
  auto wrong = scope;
  ++wrong.media_revision;
  EXPECT_EQ(hal.prepare(wrong, value, 108, 11005),
            EndpointFocusPreparation::Failed);
  EXPECT_FALSE(hal.authorize(command("acquire", "alien"), 108, 11005));
  auto changed = value;
  ++changed.expires_at.value();
  EXPECT_EQ(hal.prepare(scope, changed, 108, 11005),
            EndpointFocusPreparation::Failed);
  EXPECT_FALSE(hal.authorize(changed, 108, 11005));
  EXPECT_FALSE(hal.authorize(command("restore", "restoring"), 108, 11005));
}
TEST_F(EndpointFocusRig, ActualAcquiredScopeIsNotSettled) {
  boot();
  const auto value = command();
  bind(value);
  acquire(value);
  EXPECT_FALSE(hal.settled());
}
TEST_F(EndpointFocusRig,
       ActiveReleaseWaitsBothActualStopsBeforeOriginalEngineReply) {
  boot();
  const auto value = command();
  bind(value);
  FocusEngine engine(
      hal.driver(), hal, "candidate", "focus", {"microphone", "speaker"},
      10000);       // 正式引擎保留原租约和CAS，不能以HAL受理冒认released。
  std::string body; // 每次通过原严格信封编码，不绕过命令合同。
  ASSERT_EQ(encode_envelope(value.envelope, body), DecodeResult::Ok);
  ASSERT_EQ(engine.submit(body, 108, 11005).admission,
            FocusAdmission::Accepted);
  engine.poll(109, 11006);
  ASSERT_TRUE(hal.poll(110, 11007));
  confirm_input(111);
  output.confirm();
  ASSERT_TRUE(hal.poll(112, 11008));
  engine.poll(113, 11009);
  const auto acquired = engine.take_reply();
  ASSERT_TRUE(acquired);
  EXPECT_NE(acquired->find("acquired"), std::string::npos);
  engine.delivered_reply(*acquired);
  ASSERT_TRUE(engine.scoped_permitted({"microphone", "speaker"}, 113, 11009));
  const auto release = command("release", "active-release");
  ASSERT_EQ(encode_envelope(release.envelope, body), DecodeResult::Ok);
  ASSERT_EQ(engine.submit(body, 114, 11010).admission,
            FocusAdmission::Accepted);
  engine.poll(115, 11011);
  ASSERT_TRUE(hal.poll(116, 11012));
  EXPECT_FALSE(engine.take_reply());
  EXPECT_FALSE(hal.settled());
  confirm_input(117);
  ASSERT_TRUE(hal.poll(118, 11013));
  engine.poll(119, 11014);
  EXPECT_FALSE(engine.take_reply()); // 仅输入false仍不能证明原完整组释放。
  output.confirm();
  ASSERT_TRUE(hal.poll(120, 11015));
  engine.poll(121, 11016);
  const auto released = engine.take_reply();
  ASSERT_TRUE(released);
  EXPECT_NE(released->find("released"), std::string::npos);
  EXPECT_NE(released->find("active-release"), std::string::npos);
  EXPECT_TRUE(engine.lease_settled());
  EXPECT_TRUE(hal.settled());
  EXPECT_FALSE(engine.scoped_permitted({"microphone", "speaker"}, 121, 11016));
}
TEST_F(EndpointFocusRig,
       ActiveReleaseRejectsPartialWrongScopeAndStaleCASWithoutRevokingAcquire) {
  boot();
  const auto value = command();
  bind(value);
  const auto permit = hal.authorize(value, 108, 11005);
  ASSERT_TRUE(permit);
  acquire(value);
  auto release =
      command("release", "bad-release"); // 非法释放不能夺走原acquire持续权限。
  release.resources.pop_back();
  EXPECT_FALSE(hal.authorize(release, 112, 11008));
  release = command("release", "bad-scope");
  release.focus_id = "other";
  EXPECT_FALSE(hal.authorize(release, 113, 11009));
  release = command("release", "bad-cas");
  --release.expected_revision;
  EXPECT_FALSE(hal.authorize(release, 114, 11010));
  EXPECT_TRUE(hal.valid(*permit, 115, 11011));
  EXPECT_TRUE(hal.driver().owns(value));
  EXPECT_FALSE(hal.authorize(command("restore", "not-restoring"), 116, 11012));
  EXPECT_FALSE(hal.authorize(command("suspend", "not-suspending"), 117, 11013));
}
TEST_F(EndpointFocusRig, AbortWithoutAcquireWaitsOriginalActualFalse) {
  boot();
  const auto value = command();
  bind(value);
  hal.cancel();
  EXPECT_TRUE(hal.settled()); // 原绑定已实际false，无需造acquire/release。
  EXPECT_FALSE(hal.authorize(value, 108, 11005));
}
TEST_F(EndpointFocusRig, CancelAfterAcquireWaitsTwoActualStops) {
  boot();
  const auto value = command();
  bind(value);
  acquire(value);
  hal.cancel();
  EXPECT_FALSE(hal.settled());
  ASSERT_TRUE(hal.poll(112, 11008));
  confirm_input(113);
  EXPECT_FALSE(hal.settled());
  output.confirm();
  ASSERT_TRUE(hal.poll(114, 11009));
  EXPECT_TRUE(hal.settled());
}
TEST_F(EndpointFocusRig, SourceOrClockFailureCannotReviveAuthorization) {
  boot();
  const auto value = command();
  bind(value);
  const auto permit = hal.authorize(value, 108, 11005);
  ASSERT_TRUE(permit);
  allowed = false;
  EXPECT_FALSE(hal.valid(*permit, 109, 11006));
  EXPECT_FALSE(hal.poll(105, 11007));
  allowed = true;
  EXPECT_FALSE(hal.valid(*permit, 110, 11008));
  EXPECT_FALSE(hal.settled()); // 不把未知释放误报已完成。
}
TEST_F(EndpointFocusRig, WallClockRollbackDuringPollIsTerminal) {
  boot();
  EXPECT_FALSE(
      hal.poll(103, 10999)); // 单调时钟前进也不能掩盖宿主授权时钟倒退。
  EXPECT_FALSE(hal.poll(104, 11001));
  EXPECT_FALSE(hal.settled());
}
TEST_F(EndpointFocusRig, PrepareRejectsWallClockRollbackBeforeBinding) {
  boot();
  const auto value = command();
  EXPECT_EQ(hal.prepare(scope, value, 103, 10999),
            EndpointFocusPreparation::Failed);
  EXPECT_TRUE(commands.empty());
  EXPECT_FALSE(hal.poll(104, 11001)); // 原宿主时钟异常后不重新接受后来的邀请。
}
// WebRTC候选使用同一完整组HAL，仅输入端口的实际回执来自原Sender而非UDP处理块。
class EndpointFocusWebRTC : public EndpointFocusRig {
protected:
  EndpointFocusWebRTC() : EndpointFocusRig(BrowserInputRouteKind::AISender) {}
};
TEST_F(EndpointFocusWebRTC,
       OriginalSenderFactRequiredAfterIntermediateWorkletBarrier) {
  boot();
  const auto value = command();
  ASSERT_EQ(hal.prepare(scope, value, 103, 11001),
            EndpointFocusPreparation::Pending);
  ASSERT_TRUE(hal.poll(104, 11002));
  ASSERT_GE(commands.size(), 2U);
  EXPECT_NE(commands.front().find(R"("routeKind":"aiSender")"),
            std::string::npos);
  auto middle =
      commands.back(); // 相同原票据仅Worklet中间false不得变成Sender已释放。
  const auto at = middle.find("inputOwnershipGate\"");
  ASSERT_NE(at, std::string::npos);
  middle.replace(at, 18, "inputOwnershipGated");
  EXPECT_FALSE(hal.receive(middle, 105));
  output.confirm();
  ASSERT_TRUE(hal.poll(106, 11003));
  EXPECT_EQ(hal.prepare(scope, value, 107, 11004),
            EndpointFocusPreparation::Pending);
  confirm_input(108);
  ASSERT_TRUE(hal.poll(109, 11005));
  EXPECT_EQ(hal.prepare(scope, value, 110, 11006),
            EndpointFocusPreparation::Ready);
  EXPECT_FALSE(hal.driver().owns(value));
}
TEST_F(EndpointFocusWebRTC, CancelNeedsOriginalSenderNullAndActualOutputFalse) {
  boot();
  const auto value = command();
  bind(value);
  acquire(value);
  hal.cancel();
  ASSERT_TRUE(hal.poll(112, 11008));
  confirm_input(113);
  EXPECT_FALSE(hal.settled());
  output.confirm();
  ASSERT_TRUE(hal.poll(114, 11009));
  EXPECT_TRUE(hal.settled());
}
} // namespace
} // namespace ur::device
