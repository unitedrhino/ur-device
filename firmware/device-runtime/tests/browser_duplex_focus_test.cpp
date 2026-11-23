// 文件说明：专用宿主duplex许可合同回归；真实driver配可控HAL，不冒认浏览器或端云验收。
#include "browser_duplex_focus.hpp"
#include <algorithm>
#include <gtest/gtest.h>
#include <map>

namespace ur::device {
// BrowserDuplexStaticFaultTest 仅删除原租约墙钟，保留单调截止触发实际配对检查。
class BrowserDuplexStaticFaultTest {
public:
    // remove_lease_wall 不授任何资源或补造 driver 事实。
    static void remove_lease_wall(BrowserDuplexFocusAuthority& authority) {
        authority.lease_wall_.reset();
    }
};
namespace {
// Input 固定原路由票据，只有显式confirm才能提供实际门控事实。
class Input final : public InputOwnershipGatePort {
public:
    std::optional<std::uint64_t> initial_idle_observation() const override { return boot; }
    std::optional<std::uint64_t> request_input_gate(const InputGateRequest& request,
                                                    std::uint64_t) override {
        pending[++next] = request;
        return next;
    }
    bool input_gate_confirmed(std::uint64_t ticket,
                              const InputGateRequest& request) const override {
        if (trace)
            trace->push_back("input");
        const auto found = actual.find(ticket);
        return found != actual.end() && found->second.session == request.session &&
               found->second.route_generation == request.route_generation &&
               found->second.enabled == request.enabled;
    }
    void confirm() { actual = pending; }  // 确认原票据，不新造业务请求。
    std::optional<std::uint64_t> boot;    // 默认无idle，授权不能初始化此值。
    std::uint64_t next = 0;               // 原本地票据序列。
    std::map<std::uint64_t, InputGateRequest> pending,
        actual;  // 分离请求与实际事实。
    std::vector<std::string>* trace = nullptr;  // 可选检查序列，不构造许可。
};
// Output 仅显式确认原完整路径门控，不以request成功当实际输出。
class Output final : public OutputGroupGatePort {
public:
    std::optional<std::uint64_t> request_output_gate(bool enabled, std::uint64_t) override {
        pending[++next] = enabled;
        return next;
    }
    bool output_gate_confirmed(std::uint64_t ticket, bool enabled) const override {
        if (trace)
            trace->push_back("output");
        const auto found = actual.find(ticket);
        return found != actual.end() && found->second == enabled;
    }
    void confirm() { actual = pending; }            // 原票据实际观察由夹具显式控制。
    std::uint64_t next = 0;                         // 每个输出自己的票据序列。
    std::map<std::uint64_t, bool> pending, actual;  // 请求与实际事实相互独立。
    std::vector<std::string>* trace = nullptr;      // 与原scope读取共用单测观察。
};
// DuplexAuthorityFixture 固定原宿主双腿及两份绝对期限，完全不使用墙钟等待。
class DuplexAuthorityFixture : public ::testing::Test {
protected:
    DuplexAuthorityFixture()
        : driver(input, {&ai, &human}, 7, 0),
          binding{0, "ai", "ai-focus", 10, 3, 20, 10000, 20000, BrowserDuplexRole::AI},
          scope{binding, true, true, false, false, true, true, false, true, "control"},
          authority(driver, binding, [this] {
              if (trace)
                  trace->push_back("scope");
              return std::optional<BrowserDuplexScope>(scope);
          }) {}
    void ready() {
        input.boot = 7;
        ASSERT_TRUE(driver.initialize(100));
        ai.confirm();
        human.confirm();
        ASSERT_TRUE(driver.poll(101));
        ASSERT_TRUE(
            driver.bind_scope(0, binding.session, binding.focus, binding.route_generation, 102));
        input.confirm();
        ai.confirm();
        ASSERT_TRUE(driver.poll(103));
        ASSERT_TRUE(driver.bind_scope(1, "human", "human-focus", 11, 104));
        input.confirm();
        human.confirm();
        ASSERT_TRUE(driver.poll(105));
    }
    void admit() {
        ready();
        ASSERT_TRUE(authority.admit_binding(binding, 110, 11000));
    }
    FocusCommand command(std::string operation = "acquire", std::string token = "control") const {
        const auto json =
            "{\"msgToken\":\"" + token +
            "\",\"method\":\"focusControl\",\"sessionId\":\"ai\",\"eventSeq\":1,"
            "\"params\":{"
            "\"operation\":\"" +
            operation + "\",\"expectedRevision\":" + std::to_string(driver.revision()) +
            ",\"focusId\":\"ai-focus\",\"resources\":[\"microphone\",\"speaker\"],"
            "\"priority\":\"conversation\",\"resumePolicy\":\"explicit\"" +
            ((operation == "acquire" || operation == "restore") ? ",\"expiresAt\":18000" : "") +
            "}}";
        FocusCommand value;  // 实际公共解码器核完整契约。
        EXPECT_EQ(decode_focus_control(WireRoute::Signaling, json, value), WireResult::Ok);
        return value;
    }
    void complete(const FocusCommand& value, std::uint64_t now) {
        ASSERT_TRUE(driver.begin(value, ++generation));
        input.confirm();
        ai.confirm();
        human.confirm();
        ASSERT_TRUE(driver.poll(now));
        ASSERT_TRUE(driver.take_fact(value.envelope.session, value.focus_id));
    }
    Input input;                            // 同一物理输入源。
    Output ai, human;                       // 两原输出路由。
    DuplexFocusDriver driver;               // 共用真实整组仲裁器。
    BrowserDuplexBinding binding;           // 宿主原bind入参和原权限。
    BrowserDuplexScope scope;               // 宿主只读权限快照，不从HAL推导授权。
    BrowserDuplexFocusAuthority authority;  // 仅AI原腿authority。
    std::uint64_t generation = 0;           // 原driver操作代次。
    std::vector<std::string>* trace = nullptr;  // 原宿主回调顺序，默认关闭。
};

// 独立宿主清理上界仅接受原实际分配的release，不能扩大业务期限或跨本人意图。
TEST_F(DuplexAuthorityFixture, HumanCleanupBoundsNeverGrantBusinessOrForeignIntent) {
    ready();
    const BrowserDuplexBinding original{1,     "human", "human-focus",           11, 3, 20,
                                        10000, 20000,   BrowserDuplexRole::Human};
    BrowserDuplexScope permission{original, true, true,  false, false,
                                  true,     true, false, true,  "human-acquire"};
    BrowserDuplexFocusAuthority human_authority(
        driver, original, [&permission] { return std::optional<BrowserDuplexScope>(permission); },
        BrowserDuplexCleanupBounds{30000, 40000});
    ASSERT_TRUE(human_authority.admit_binding(original, 110, 11000));
    // 请求经过严格公共解码器，不直接填私有许可绕过资源组与CAS校验。
    const auto request = [&](std::string_view operation, std::string_view token) {
        FocusCommand result;
        const auto body = "{\"msgToken\":\"" + std::string(token) +
                          "\",\"method\":\"focusControl\",\"sessionId\":\"human\",\"eventSeq\":1,"
                          "\"params\":{\"operation\":\"" +
                          std::string(operation) +
                          "\",\"expectedRevision\":" + std::to_string(driver.revision()) +
                          ",\"focusId\":\"human-focus\",\"resources\":[\"microphone\","
                          "\"speaker\"],"
                          "\"priority\":\"call\",\"resumePolicy\":\"never\"" +
                          (operation == "acquire" ? ",\"expiresAt\":18000" : "") + "}}";
        EXPECT_EQ(decode_focus_control(WireRoute::Signaling, body, result), WireResult::Ok);
        return result;
    };
    const auto acquired = request("acquire", "human-acquire");
    ASSERT_TRUE(human_authority.authorize(acquired, 111, 11001));
    complete(acquired, 112);
    // 到期自动停止保留原修订；未正式释放前不能伪造新acquire或重复恢复。
    permission.closing = true;
    permission.authorized = false;
    permission.cleanup_deadline = 12000;
    permission.cleanup_expires_at = 22000;
    permission.operation_token = "human-release";
    const auto release = request("release", "human-release");
    auto permit = human_authority.authorize(release, 10001, 20001);
    ASSERT_TRUE(permit);
    EXPECT_EQ(permit->deadline, 12000U);
    EXPECT_TRUE(human_authority.closed_release_valid(*permit, 10002, 20002));
    EXPECT_FALSE(human_authority.authorize(request("acquire", "new-acquire"), 10003, 20003));
    EXPECT_FALSE(human_authority.authorize(request("suspend", "new-suspend"), 10004, 20004));
    permission.current_intent_version = 4;
    EXPECT_FALSE(human_authority.valid(*permit, 10005, 20005));
    permission.current_intent_version = 3;
    permission.binding.peer_epoch = 21;
    EXPECT_FALSE(human_authority.valid(*permit, 10006, 20006));
    permission.binding = original;
    EXPECT_TRUE(human_authority.valid(*permit, 10007, 20007));
    // 原字节重投不能续清理截止，即使外部快照后来声称更长清理范围。
    permission.cleanup_deadline = 15000;
    permission.cleanup_expires_at = 25000;
    EXPECT_FALSE(human_authority.valid(*permit, 12000, 22000));
    EXPECT_FALSE(human_authority.authorize(release, 12001, 22001));
    EXPECT_EQ(driver.revision(), 1U);
}

// 原租约缺少墙钟时，后继取得不能重建或续长原租约，实际驱动仍无新操作。
TEST_F(DuplexAuthorityFixture, MissingLeaseWallCannotRenewExistingLease) {
    admit();
    const auto request = command();
    ASSERT_TRUE(authority.authorize(request, 110, 11000));
    BrowserDuplexStaticFaultTest::remove_lease_wall(authority);
    scope.operation_token = "new-acquire";  // 原宿主新意图关联，使拒绝必须经过租约配对检查。
    EXPECT_FALSE(authority.authorize(command("acquire", "new-acquire"), 111, 11001));
    EXPECT_EQ(driver.revision(), std::uint64_t{0});
    EXPECT_FALSE(driver.owns(request));
}

// valid的current可推进时钟/移交围栏；开启观察仍一次读取原scope、原拥有和票据。
TEST_F(DuplexAuthorityFixture, ScopeDiagnosticPreservesHostAndTicketEvaluationSequence) {
    admit();
    const auto request = command();
    const auto permit = authority.authorize(request, 110, 11000);
    ASSERT_TRUE(permit);
    complete(request, 111);
    std::vector<std::string> calls;
    trace = input.trace = ai.trace = human.trace = &calls;
    ASSERT_TRUE(authority.valid(*permit, 112, 11002));
    const auto original = calls;
    EXPECT_EQ(std::count(original.begin(), original.end(), "scope"), 1);
    ASSERT_FALSE(original.empty());
    EXPECT_EQ(original.front(), "scope");
    calls.clear();
    ScopeFailure observation;
    EXPECT_TRUE(authority.valid_observed(*permit, 112, 11002, &observation));
    // 原allocation首读会保存事实，下一次无须再读；比较同原持久状态的关/开调用。
    const auto allocated = calls;
    calls.clear();
    EXPECT_TRUE(authority.valid(*permit, 112, 11002));
    EXPECT_EQ(calls, allocated);
    EXPECT_EQ(observation.code, ScopeFailureCode::None);
    scope.authorized = false;
    calls.clear();
    EXPECT_FALSE(authority.valid(*permit, 113, 11003));
    const auto refused = calls;
    calls.clear();
    EXPECT_FALSE(authority.valid_observed(*permit, 113, 11003, &observation));
    EXPECT_EQ(calls, refused);
    EXPECT_EQ(observation.code, ScopeFailureCode::AuthorityPermission);
}

TEST_F(DuplexAuthorityFixture, ConstructorAndDefaultCallbackDoNotInitializeOrBind) {
    BrowserDuplexFocusAuthority denied(driver, binding, {});
    EXPECT_FALSE(denied.admit_binding(binding, 110, 11000));
    EXPECT_FALSE(denied.authorize(command(), 110, 11000));
    EXPECT_EQ(input.next, 0U);
    EXPECT_EQ(ai.next, 0U);
    EXPECT_FALSE(driver.initial_idle_observation());
}
TEST_F(DuplexAuthorityFixture, ChangedNativeIntentIsCarriedOnlyByRestoreNotOldAcquire) {
    admit();
    const auto acquire = command();
    const auto original = authority.authorize(acquire, 111, 11001);
    ASSERT_TRUE(original);
    complete(acquire, 112);
    scope.operation_token = "suspending";
    const auto suspend = command("suspend", "suspending");
    ASSERT_TRUE(authority.authorize(suspend, 113, 11002));
    complete(suspend, 114);
    scope.restore = true;
    scope.operation_token = "restoring";
    scope.current_intent_version = binding.intent_version + 1;
    const auto restored = authority.authorize(command("restore", "restoring"), 115, 11003);
    ASSERT_TRUE(restored);
    EXPECT_EQ(restored->intent_version, binding.intent_version + 1);
    EXPECT_TRUE(authority.valid(*restored, 116, 11004));
    EXPECT_TRUE(authority.valid(*original, 116, 11004)); // 原lease仍可停止／清理，不能借版本变动重开。
    ++scope.current_intent_version.value();
    EXPECT_FALSE(authority.valid(*restored, 117, 11005));
}
TEST_F(DuplexAuthorityFixture, InvalidNativeIntentSnapshotNeverIssuesRestorePermission) {
    admit();
    const auto acquire = command();
    ASSERT_TRUE(authority.authorize(acquire, 111, 11001));
    complete(acquire, 112);
    scope.restore = true;
    scope.operation_token = "restoring";
    scope.current_intent_version = 0;
    EXPECT_FALSE(authority.authorize(command("restore", "restoring"), 113, 11002));
    scope.current_intent_version = binding.intent_version - 1;
    EXPECT_FALSE(authority.authorize(command("restore", "restoring"), 114, 11003));
    scope.current_intent_version = 9007199254740992ULL;
    EXPECT_FALSE(authority.authorize(command("restore", "restoring"), 115, 11004));
}
// 已恢复的整组日常停输入不撤租约，但再暂停后的新restore仍必须暴露真实意图变更。
TEST_F(DuplexAuthorityFixture, CompletedRestoreRetainsLeaseWithoutWashingNextRestoreIntent) {
    admit();
    const auto acquired = command();
    ASSERT_TRUE(authority.authorize(acquired, 111, 11001));
    complete(acquired, 112);
    scope.operation_token = "suspending";
    const auto suspended = command("suspend", "suspending");
    ASSERT_TRUE(authority.authorize(suspended, 113, 11002));
    complete(suspended, 114);
    scope.restore = true;
    scope.current_intent_version = binding.intent_version;
    scope.operation_token = "restoring";
    const auto restoration = command("restore", "restoring");
    const auto original = authority.authorize(restoration, 115, 11003);
    ASSERT_TRUE(original);
    complete(restoration, 116);
    ++scope.current_intent_version.value();
    EXPECT_TRUE(authority.valid(*original, 117, 11004));
    scope.operation_token = "next-suspend";
    const auto next_suspend = command("suspend", "next-suspend");
    ASSERT_TRUE(authority.authorize(next_suspend, 118, 11005));
    complete(next_suspend, 119);
    EXPECT_FALSE(authority.valid(*original, 120, 11006));
    scope.operation_token = "next-restore";
    const auto next = authority.authorize(command("restore", "next-restore"), 121, 11007);
    ASSERT_TRUE(next);
    EXPECT_EQ(next->intent_version, binding.intent_version + 1);
    EXPECT_NE(next->intent_version, original->intent_version);
}
TEST_F(DuplexAuthorityFixture, ExplicitOriginalBindingAndActualReadyRequired) {
    EXPECT_FALSE(authority.admit_binding(binding, 110, 11000));
    ready();
    auto wrong = binding;
    ++wrong.route_generation;
    EXPECT_FALSE(authority.admit_binding(wrong, 110, 11000));
    EXPECT_FALSE(authority.authorize(command(), 110, 11000));
    EXPECT_TRUE(authority.admit_binding(binding, 110, 11000));
    EXPECT_TRUE(authority.authorize(command(), 111, 11001));
}
TEST_F(DuplexAuthorityFixture, ActualCASFullGroupAndOriginalIdentityRequired) {
    admit();
    const auto original = command();
    for (unsigned field = 0; field < 8; ++field) {
        auto bad = original;
        switch (field) {
            case 0:
                bad.resources.pop_back();
                break;
            case 1:
                bad.resources = {"speaker", "speaker"};
                break;
            case 2:
                bad.envelope.session = "human";
                break;
            case 3:
                bad.focus_id = "human-focus";
                break;
            case 4:
                ++bad.expected_revision;
                break;
            case 5:
                bad.priority = "call";
                break;
            case 6:
                bad.resume_policy = "never";
                break;
            default:
                bad.envelope.token = "other";
                break;
        }
        EXPECT_FALSE(authority.authorize(bad, 111, 11001)) << field;
    }
    EXPECT_TRUE(authority.authorize(original, 111, 11001));
    EXPECT_EQ(driver.revision(), 0U);
}
TEST_F(DuplexAuthorityFixture, ActualPendingDoesNotRevokeOriginalIssuedPermit) {
    admit();
    const auto request = command();
    const auto permit = authority.authorize(request, 111, 11001);
    ASSERT_TRUE(permit);
    ASSERT_TRUE(driver.begin(request, 1));
    EXPECT_FALSE(driver.scope_ready(0));
    EXPECT_TRUE(authority.valid(*permit, 112, 11002));
    input.confirm();
    ai.confirm();
    ASSERT_TRUE(driver.poll(113));
    EXPECT_TRUE(driver.owns(request));
    EXPECT_TRUE(authority.valid(*permit, 114, 11003));
}
TEST_F(DuplexAuthorityFixture, BothAbsoluteClocksAdvanceIndependentlyWithinOriginalLimits) {
    admit();
    const auto permit = authority.authorize(command(), 111, 11001);
    ASSERT_TRUE(permit);
    EXPECT_TRUE(authority.valid(*permit, 112, 11001));
    EXPECT_TRUE(authority.valid(*permit, 112, 11002));
    EXPECT_TRUE(authority.valid(*permit, 113, 11002));
    EXPECT_FALSE(authority.valid(*permit, permit->deadline, 11002));
}
// 原许可采用两个独立截止的较早值；墙钟余量大不能推迟原单调截止。
TEST_F(DuplexAuthorityFixture, IndependentClockRemaindersClipPermitAtOriginalMonotonicDeadline) {
    admit();
    const auto permit = authority.authorize(command(), 9990, 11001);
    ASSERT_TRUE(permit);
    EXPECT_EQ(permit->deadline, binding.deadline);
    EXPECT_TRUE(authority.valid(*permit, 9999, 11002));
    EXPECT_FALSE(authority.valid(*permit, binding.deadline, 11002));
}
// 原执行器必须在真实完整组完成后才能放行；重放与恢复都不能延长原单调授权。
TEST_F(DuplexAuthorityFixture, ActualEngineRestoreAndReplayCannotExtendOriginalMonotonicDeadline) {
    admit();
    FocusEngine engine(driver, authority, binding.session, binding.focus, {"microphone", "speaker"},
                       binding.deadline);
    const auto submit = [&](const FocusCommand& original, std::uint64_t now, std::uint64_t wall) {
        std::string body;
        EXPECT_EQ(encode_envelope(original.envelope, body), DecodeResult::Ok);
        return engine.submit(body, now, wall);
    };
    const auto actual = [&](std::uint64_t now, std::uint64_t wall) {
        engine.poll(now, wall);
        input.confirm();
        ai.confirm();
        human.confirm();
        EXPECT_TRUE(driver.poll(now));
        engine.poll(now, wall);
        const auto bytes = engine.take_reply();
        EXPECT_TRUE(bytes);
        if (bytes) {
            FocusState fact;
            EXPECT_EQ(decode_focus_state(WireRoute::Signaling, *bytes, fact), WireResult::Ok);
            EXPECT_EQ(fact.envelope.code, 200);
        }
    };
    const auto acquired = command();
    ASSERT_EQ(submit(acquired, 9990, 11001).admission, FocusAdmission::Accepted);
    actual(9991, 11002);
    ASSERT_TRUE(engine.output_permitted(9992, 11003));
    scope.operation_token = "suspend";
    auto suspended = command("suspend", "suspend");
    suspended.envelope.event_sequence = 2;
    ASSERT_EQ(submit(suspended, 9993, 11004).admission, FocusAdmission::Accepted);
    actual(9994, 11005);
    scope.operation_token = "restore";
    scope.restore = true;
    auto restored = command("restore", "restore");
    restored.envelope.event_sequence = 3;
    ASSERT_EQ(submit(restored, 9995, 11006).admission, FocusAdmission::Accepted);
    actual(9996, 11007);
    ASSERT_TRUE(engine.output_permitted(9997, 11008));
    const auto input_count = input.next;
    const auto output_count = ai.next;
    ASSERT_EQ(submit(restored, 9998, 11008).admission, FocusAdmission::Replay);
    EXPECT_EQ(input.next, input_count);
    EXPECT_EQ(ai.next, output_count);
    EXPECT_FALSE(engine.output_permitted(binding.deadline, 11009));
    EXPECT_FALSE(authority.authorize(command("restore", "later"), binding.deadline, 11009));
}
// 原墙钟截止先到同样关闭实际输出，不能借单调时钟的余量继续采集。
TEST_F(DuplexAuthorityFixture, ActualEngineCannotCommitAtOriginalWallDeadline) {
    admit();
    const auto original = command();
    FocusEngine engine(driver, authority, binding.session, binding.focus, {"microphone", "speaker"},
                       binding.deadline);
    std::string bytes;
    ASSERT_EQ(encode_envelope(original.envelope, bytes), DecodeResult::Ok);
    ASSERT_EQ(engine.submit(bytes, 111, 17999).admission, FocusAdmission::Accepted);
    engine.poll(111, 17999);
    input.confirm();
    ai.confirm();
    human.confirm();
    ASSERT_TRUE(driver.poll(112));
    engine.poll(112, 18000);
    EXPECT_FALSE(engine.output_permitted(112, 18000));
    const auto reply = engine.take_reply();
    ASSERT_TRUE(reply);
    FocusState fact;
    ASSERT_EQ(decode_focus_state(WireRoute::Signaling, *reply, fact), WireResult::Ok);
    EXPECT_EQ(fact.envelope.code, 410);
}
// 原单调截止到达时，即使整组HAL事实同刻齐备也不能补成成功或被重放续活。
TEST_F(DuplexAuthorityFixture, ActualEngineCannotCommitAtOriginalMonotonicDeadline) {
    admit();
    const auto original = command();
    FocusEngine engine(driver, authority, binding.session, binding.focus, {"microphone", "speaker"},
                       binding.deadline);
    std::string bytes;
    ASSERT_EQ(encode_envelope(original.envelope, bytes), DecodeResult::Ok);
    ASSERT_EQ(engine.submit(bytes, 9990, 11001).admission, FocusAdmission::Accepted);
    engine.poll(9991, 11002);
    EXPECT_FALSE(engine.output_permitted(9991, 11002));
    input.confirm();
    ai.confirm();
    human.confirm();
    ASSERT_TRUE(driver.poll(binding.deadline));
    engine.poll(binding.deadline, 11003);
    EXPECT_FALSE(engine.output_permitted(binding.deadline, 11003));
    const auto reply = engine.take_reply();
    ASSERT_TRUE(reply);
    FocusState fact;
    ASSERT_EQ(decode_focus_state(WireRoute::Signaling, *reply, fact), WireResult::Ok);
    EXPECT_EQ(fact.envelope.code, 410);
    const auto repeated = engine.submit(bytes, binding.deadline + 1, 11004);
    EXPECT_EQ(repeated.admission, FocusAdmission::Replay);
    EXPECT_EQ(repeated.reply, *reply);
    EXPECT_FALSE(engine.output_permitted(binding.deadline + 1, 11004));
}
TEST_F(DuplexAuthorityFixture, WallDeadlineCannotBeExtendedByMonotonicSlack) {
    admit();
    const auto permit = authority.authorize(command(), 111, 11001);
    ASSERT_TRUE(permit);
    EXPECT_FALSE(authority.valid(*permit, 112, 18000));
}
TEST_F(DuplexAuthorityFixture, CreationTTLUpperBoundAndHostBoundsAreStrict) {
    admit();
    auto bad = command();
    bad.expires_at = 20900;
    EXPECT_FALSE(authority.authorize(bad, 9999, 11001));
    bad.expires_at = 20001;
    EXPECT_FALSE(authority.authorize(bad, 9999, 11001));
    EXPECT_FALSE(authority.authorize(command(), 10000, 11001));
}
TEST_F(DuplexAuthorityFixture, ClockRollbackDoesNotResurrectOriginalPermit) {
    admit();
    const auto permit = authority.authorize(command(), 111, 11001);
    ASSERT_TRUE(permit);
    EXPECT_FALSE(authority.valid(*permit, 110, 11002));
    EXPECT_FALSE(authority.valid(*permit, 112, 11003));
}
TEST_F(DuplexAuthorityFixture, WallRollbackAlsoFailsClosed) {
    admit();
    const auto permit = authority.authorize(command(), 111, 11001);
    ASSERT_TRUE(permit);
    EXPECT_FALSE(authority.valid(*permit, 112, 11000));
}
TEST_F(DuplexAuthorityFixture, FullIssuedPermitRejectsEveryAlteredField) {
    admit();
    const auto original = authority.authorize(command(), 111, 11001);
    ASSERT_TRUE(original);
    for (unsigned field = 0; field < 17; ++field) {
        auto bad = *original;
        switch (field) {
            case 0:
                ++bad.deadline;
                break;
            case 1:
                ++bad.intent_version;
                break;
            case 2:
                bad.scope.envelope.token += "x";
                break;
            case 3:
                bad.scope.envelope.session += "x";
                break;
            case 4:
                bad.scope.envelope.method += "x";
                break;
            case 5:
                bad.scope.envelope.request = !bad.scope.envelope.request;
                break;
            case 6:
                bad.scope.envelope.payload += " ";
                break;
            case 7:
                bad.scope.envelope.code = 200;
                break;
            case 8:
                bad.scope.envelope.event_sequence = 2;
                break;
            case 9:
                bad.scope.envelope.message_sequence = 1;
                break;
            case 10:
                bad.scope.operation = "restore";
                break;
            case 11:
                ++bad.scope.expected_revision;
                break;
            case 12:
                bad.scope.focus_id += "x";
                break;
            case 13:
                bad.scope.resources.pop_back();
                break;
            case 14:
                bad.scope.priority = "call";
                break;
            case 15:
                bad.scope.resume_policy = "never";
                break;
            default:
                ++*bad.scope.expires_at;
                break;
        }
        EXPECT_FALSE(authority.valid(bad, 112, 11002)) << field;
    }
    EXPECT_TRUE(authority.valid(*original, 112, 11002));
}
TEST_F(DuplexAuthorityFixture, HostIdentityIntentPeerAndDeadlineCannotChange) {
    admit();
    const auto permit = authority.authorize(command(), 111, 11001);
    ASSERT_TRUE(permit);
    const auto original = scope;
    for (unsigned field = 0; field < 10; ++field) {
        scope = original;
        switch (field) {
            case 0:
                ++scope.binding.path;
                break;
            case 1:
                scope.binding.session += "x";
                break;
            case 2:
                scope.binding.focus += "x";
                break;
            case 3:
                ++scope.binding.route_generation;
                break;
            case 4:
                ++scope.binding.intent_version;
                break;
            case 5:
                ++scope.binding.peer_epoch;
                break;
            case 6:
                ++scope.binding.deadline;
                break;
            case 7:
                ++scope.binding.expires_at;
                break;
            case 8:
                scope.authorized = false;
                break;
            default:
                scope.peer_ready = false;
                break;
        }
        EXPECT_FALSE(authority.valid(*permit, 112, 11002)) << field;
    }
}
TEST_F(DuplexAuthorityFixture, SuspendPreservesOriginalLeaseAndRestoreNeedsIndependentPermission) {
    admit();
    const auto acquire = command();
    const auto lease = authority.authorize(acquire, 111, 11001);
    ASSERT_TRUE(lease);
    complete(acquire, 112);
    scope.operation_token = "suspend";
    const auto suspend = command("suspend", "suspend");
    const auto stopped = authority.authorize(suspend, 113, 11002);
    ASSERT_TRUE(stopped);
    EXPECT_TRUE(authority.valid(*lease, 114, 11003));
    complete(suspend, 115);
    scope.operation_token = "restore";
    EXPECT_FALSE(authority.authorize(command("restore", "restore"), 116, 11004));
    scope.restore = true;
    const auto restored = authority.authorize(command("restore", "restore"), 117, 11005);
    ASSERT_TRUE(restored);
    EXPECT_LE(restored->deadline, lease->deadline);
    complete(restored->scope, 118);
    EXPECT_TRUE(driver.owns(restored->scope));
    EXPECT_TRUE(authority.valid(*restored, 119, 11006));
    scope.restore = false;  // 原恢复授权撤销不能被仍有AI基础权限掩盖。
    EXPECT_FALSE(authority.valid(*restored, 120, 11007));
}
TEST_F(DuplexAuthorityFixture,
       OriginalHumanLeaseRestoresOnlyWithIndependentIntentAndActualFullGroup) {
    ready();
    // 原人工腿具有独立许可，不能借AI角色、回复事件或静音布尔恢复输入。
    const BrowserDuplexBinding original{1,     "human", "human-focus",           11, 3, 20,
                                        10000, 20000,   BrowserDuplexRole::Human};
    BrowserDuplexScope permission{original, true, true,  false, false,
                                  true,     true, false, true,  "human-acquire"};
    BrowserDuplexFocusAuthority human_authority(
        driver, original, [&permission] { return std::optional<BrowserDuplexScope>(permission); });
    ASSERT_TRUE(human_authority.admit_binding(original, 110, 11000));
    // request 使用真实公开解码器；expectedRevision始终是原共享资源水位而非call revision。
    const auto request = [&](std::string_view operation, std::string_view token) {
        FocusCommand value;
        const std::string body =
            "{\"msgToken\":\"" + std::string(token) +
            "\",\"method\":\"focusControl\",\"sessionId\":\"human\",\"eventSeq\":1,"
            "\"params\":{\"operation\":\"" +
            std::string(operation) +
            "\",\"expectedRevision\":" + std::to_string(driver.revision()) +
            ",\"focusId\":\"human-focus\",\"resources\":[\"microphone\","
            "\"speaker\"],"
            "\"priority\":\"call\",\"resumePolicy\":\"explicit\"" +
            ((operation == "acquire" || operation == "restore") ? ",\"expiresAt\":18000" : "") +
            "}}";
        EXPECT_EQ(decode_focus_control(WireRoute::Signaling, body, value), WireResult::Ok);
        return value;
    };
    const auto acquired = request("acquire", "human-acquire");
    const auto lease = human_authority.authorize(acquired, 111, 11001);
    ASSERT_TRUE(lease);
    complete(acquired, 112);
    permission.operation_token = "human-hold";
    const auto suspended = request("suspend", "human-hold");
    ASSERT_TRUE(human_authority.authorize(suspended, 113, 11002));
    complete(suspended, 114);
    EXPECT_FALSE(driver.owns(acquired));
    permission.operation_token = "human-resume";
    const auto restored = request("restore", "human-resume");
    EXPECT_FALSE(human_authority.authorize(restored, 115, 11003));
    permission.restore = true;
    const auto permit = human_authority.authorize(restored, 116, 11004);
    ASSERT_TRUE(permit);
    EXPECT_LE(permit->deadline, lease->deadline);
    ASSERT_TRUE(driver.begin(restored, ++generation));
    human.confirm();
    ASSERT_TRUE(driver.poll(117));
    EXPECT_FALSE(driver.owns(restored));
    input.confirm();
    ASSERT_TRUE(driver.poll(118));
    const auto actual = driver.take_fact("human", "human-focus");
    ASSERT_TRUE(actual);
    EXPECT_TRUE(actual->success);
    EXPECT_EQ(actual->state, "restored");
    EXPECT_EQ(actual->revision, 3U);
    EXPECT_TRUE(driver.owns(restored));
    permission.restore = false;
    EXPECT_FALSE(human_authority.valid(*permit, 119, 11005));
}
TEST_F(DuplexAuthorityFixture, OriginalNeverHumanLeaseCannotBeChangedIntoExplicitRestore) {
    ready();
    const BrowserDuplexBinding original{1,     "human", "human-focus",           11, 3, 20,
                                        10000, 20000,   BrowserDuplexRole::Human};
    BrowserDuplexScope permission{original, true, true, false, false,
                                  true,     true, true, true,  "legacy-acquire"};
    BrowserDuplexFocusAuthority human_authority(
        driver, original, [&permission] { return std::optional<BrowserDuplexScope>(permission); });
    ASSERT_TRUE(human_authority.admit_binding(original, 110, 11000));
    FocusCommand acquired;
    ASSERT_EQ(
        decode_focus_control(
            WireRoute::Signaling,
            R"({"msgToken":"legacy-acquire","method":"focusControl","sessionId":"human","eventSeq":1,"params":{"operation":"acquire","expectedRevision":0,"focusId":"human-focus","resources":["microphone","speaker"],"priority":"call","resumePolicy":"never","expiresAt":18000}})",
            acquired),
        WireResult::Ok);
    ASSERT_TRUE(human_authority.authorize(acquired, 111, 11001));
    complete(acquired, 112);
    permission.operation_token = "legacy-suspend";
    FocusCommand suspended;
    ASSERT_EQ(
        decode_focus_control(
            WireRoute::Signaling,
            R"({"msgToken":"legacy-suspend","method":"focusControl","sessionId":"human","eventSeq":2,"params":{"operation":"suspend","expectedRevision":1,"focusId":"human-focus","resources":["microphone","speaker"],"priority":"call","resumePolicy":"never"}})",
            suspended),
        WireResult::Ok);
    ASSERT_TRUE(human_authority.authorize(suspended, 113, 11002));
    complete(suspended, 114);
    permission.operation_token = "legacy-restore";
    // 即使同scope明示restore，旧never租约也不能被后继请求改成可恢复租约。
    for (const std::string policy : {"never", "explicit"}) {
        FocusCommand restored;
        const std::string wire =
            R"({"msgToken":"legacy-restore","method":"focusControl","sessionId":"human","eventSeq":3,"params":{"operation":"restore","expectedRevision":2,"focusId":"human-focus","resources":["microphone","speaker"],"priority":"call","resumePolicy":")" +
            policy + R"(","expiresAt":18000}})";
        ASSERT_EQ(decode_focus_control(WireRoute::Signaling, wire, restored), WireResult::Ok);
        EXPECT_FALSE(human_authority.authorize(restored, 115, 11003));
        EXPECT_EQ(driver.revision(), 2U);
        EXPECT_FALSE(driver.owns(restored));
    }
}
TEST_F(DuplexAuthorityFixture, FinalHandoffFencesAIAndOnlyExplicitClosingReleaseRemains) {
    admit();
    const auto lease = authority.authorize(command(), 111, 11001);
    ASSERT_TRUE(lease);
    complete(lease->scope, 112);
    scope.handoff_committed = true;
    EXPECT_FALSE(authority.valid(*lease, 113, 11002));
    scope.handoff_committed = false;
    EXPECT_FALSE(authority.valid(*lease, 114, 11003));
    scope.operation_token = "release";
    EXPECT_FALSE(authority.authorize(command("release", "release"), 115, 11004));
    scope.closing = true;
    scope.authorized = false;
    scope.peer_ready = false;
    const auto release = authority.authorize(command("release", "release"), 116, 11005);
    ASSERT_TRUE(release);
    EXPECT_TRUE(authority.closed_release_valid(*release, 117, 11006));
    EXPECT_FALSE(authority.closed_release_valid(*lease, 117, 11006));
    complete(release->scope, 118);
    EXPECT_EQ(driver.revision(), 2U);
    EXPECT_FALSE(driver.owns(lease->scope));
}
TEST_F(DuplexAuthorityFixture, ClosingWithoutOriginalLeaseCannotCreateCleanupPermission) {
    admit();
    scope.closing = true;
    scope.operation_token = "release";
    EXPECT_FALSE(authority.authorize(command("release", "release"), 111, 11001));
}
TEST_F(DuplexAuthorityFixture, IssuedButNeverAppliedCannotGrantClosedCleanup) {
    admit();
    ASSERT_TRUE(authority.authorize(command(), 111, 11001));
    scope.closing = true;
    scope.operation_token = "release";
    EXPECT_FALSE(authority.authorize(command("release", "release"), 112, 11002));
    EXPECT_EQ(driver.revision(), 0U);
}
TEST_F(DuplexAuthorityFixture, OriginalLeaseClockExpiryAlsoPreventsRestore) {
    admit();
    const auto lease = authority.authorize(command(), 111, 11001);
    ASSERT_TRUE(lease);
    complete(lease->scope, 112);
    scope.operation_token = "suspend";
    ASSERT_TRUE(authority.authorize(command("suspend", "suspend"), 113, 11002));
    complete(command("suspend", "suspend"), 114);
    scope.operation_token = "restore";
    scope.restore = true;
    EXPECT_FALSE(authority.authorize(command("restore", "restore"), lease->deadline, 11003));
}
TEST_F(DuplexAuthorityFixture, IndependentRestoreCannotExtendOriginalWallDeadline) {
    admit();
    const auto lease = authority.authorize(command(), 111, 11001);
    ASSERT_TRUE(lease);
    complete(lease->scope, 112);
    scope.operation_token = "suspend";
    ASSERT_TRUE(authority.authorize(command("suspend", "suspend"), 113, 11002));
    complete(command("suspend", "suspend"), 114);
    scope.operation_token = "restore";
    scope.restore = true;
    const auto json =
        R"({"msgToken":"restore","method":"focusControl","sessionId":"ai","eventSeq":2,"params":{"operation":"restore","expectedRevision":2,"focusId":"ai-focus","resources":["microphone","speaker"],"priority":"conversation","resumePolicy":"explicit","expiresAt":18001}})";
    FocusCommand request;  // 原期限延长一毫秒也需拒绝。
    ASSERT_EQ(decode_focus_control(WireRoute::Signaling, json, request), WireResult::Ok);
    EXPECT_FALSE(authority.authorize(request, 115, 11003));
    EXPECT_TRUE(authority.valid(*lease, 116, 11004));
}
TEST_F(DuplexAuthorityFixture, HumanPreparedDoesNotGrantAcquireAndCannotRestoreAIIntent) {
    ready();
    const BrowserDuplexBinding human_binding{1,     "human", "human-focus",           11, 4, 21,
                                             10000, 20000,   BrowserDuplexRole::Human};
    BrowserDuplexScope human_scope{human_binding, true,  true,  false, false,
                                   false,         false, false, true,  "human-control"};
    BrowserDuplexFocusAuthority human_authority(
        driver, human_binding, [&] { return std::optional<BrowserDuplexScope>(human_scope); });
    ASSERT_TRUE(human_authority.admit_binding(human_binding, 110, 11000));
    FocusCommand request;  // 原解码器保留Human完整信封，不能只改结构体而留AI payload。
    ASSERT_EQ(
        decode_focus_control(
            WireRoute::Signaling,
            R"({"msgToken":"human-control","method":"focusControl","sessionId":"human","eventSeq":1,"params":{"operation":"acquire","expectedRevision":0,"focusId":"human-focus","resources":["microphone","speaker"],"priority":"call","resumePolicy":"never","expiresAt":18000}})",
            request),
        WireResult::Ok);
    EXPECT_FALSE(human_authority.authorize(request, 111, 11001));
    human_scope.acquire = true;
    const auto permit = human_authority.authorize(request, 112, 11002);
    ASSERT_TRUE(permit);
    EXPECT_TRUE(human_authority.valid(*permit, 113, 11003));
    EXPECT_FALSE(authority.authorize(request, 113, 11003));
    human_scope.restore = true;
    request.operation = "restore";
    EXPECT_FALSE(human_authority.authorize(request, 114, 11004));
}
TEST_F(DuplexAuthorityFixture, TokenReplayDoesNotRefreshDeadlineAndConflictDoesNotReplaceLease) {
    admit();
    const auto request = command();
    const auto permit = authority.authorize(request, 111, 11001);
    ASSERT_TRUE(permit);
    const auto replay = authority.authorize(request, 112, 11002);
    ASSERT_TRUE(replay);
    EXPECT_EQ(replay->deadline, permit->deadline);
    auto conflicting = request;
    --*conflicting.expires_at;
    EXPECT_FALSE(authority.authorize(conflicting, 113, 11003));
    EXPECT_TRUE(authority.valid(*permit, 114, 11004));
}
TEST_F(DuplexAuthorityFixture, BoundedHistoryDoesNotEvictOriginalLease) {
    admit();
    std::optional<FocusPermit> original;  // 首条原许可持续保留供引擎lease校验。
    for (unsigned index = 0; index < 32; ++index) {
        scope.operation_token = "control" + std::to_string(index);
        const auto permit = authority.authorize(command("acquire", scope.operation_token),
                                                111 + index, 11001 + index);
        ASSERT_TRUE(permit);
        if (!index)
            original = permit;
    }
    scope.operation_token = "overflow";
    EXPECT_FALSE(authority.authorize(command("acquire", "overflow"), 143, 11033));
    ASSERT_TRUE(original);
    EXPECT_TRUE(authority.valid(*original, 144, 11034));
}
TEST_F(DuplexAuthorityFixture, OriginalAINoPeerDoesNotFabricateHumanPeer) {
    ready();
    auto ai_binding = binding;
    ai_binding.peer_epoch = 0;  // 原AI可为UDP路径，无实际Peer不能编造代次。
    auto ai_scope = scope;
    ai_scope.binding = ai_binding;
    BrowserDuplexFocusAuthority ai_authority(
        driver, ai_binding, [&] { return std::optional<BrowserDuplexScope>(ai_scope); });
    ASSERT_TRUE(ai_authority.admit_binding(ai_binding, 110, 11000));
    EXPECT_TRUE(ai_authority.authorize(command(), 111, 11001));
    const BrowserDuplexBinding human_binding{1,     "human", "human-focus",           11, 4, 0,
                                             10000, 20000,   BrowserDuplexRole::Human};
    BrowserDuplexScope human_scope;  // 未知实际Peer不能借AI能力准入。
    human_scope.binding = human_binding;
    human_scope.authorized = human_scope.peer_ready = true;
    BrowserDuplexFocusAuthority human_authority(
        driver, human_binding, [&] { return std::optional<BrowserDuplexScope>(human_scope); });
    EXPECT_FALSE(human_authority.admit_binding(human_binding, 110, 11000));
}
TEST_F(DuplexAuthorityFixture, UnknownHostSnapshotCannotAdmitOrKeepOriginalLease) {
    ready();
    bool known = false;  // 原宿主失读不得借最后成功快照。
    BrowserDuplexFocusAuthority selected(
        driver, binding, [&]() -> std::optional<BrowserDuplexScope> {
            return known ? std::optional<BrowserDuplexScope>(scope) : std::nullopt;
        });
    EXPECT_FALSE(selected.admit_binding(binding, 110, 11000));
    known = true;
    ASSERT_TRUE(selected.admit_binding(binding, 111, 11001));
    const auto permit = selected.authorize(command(), 112, 11002);
    ASSERT_TRUE(permit);
    known = false;
    EXPECT_FALSE(selected.valid(*permit, 113, 11003));
}
TEST_F(DuplexAuthorityFixture, RealEnginesSuspendAIThenAcquireHumanWithoutReplacingOldAILease) {
    admit();
    const BrowserDuplexBinding human_binding{1,     "human", "human-focus",           11, 4, 21,
                                             10000, 20000,   BrowserDuplexRole::Human};
    BrowserDuplexScope human_scope{human_binding, true,  true,  false, false,
                                   false,         false, false, true,  "human-control"};
    BrowserDuplexFocusAuthority human_authority(
        driver, human_binding, [&] { return std::optional<BrowserDuplexScope>(human_scope); });
    ASSERT_TRUE(human_authority.admit_binding(human_binding, 110, 11000));
    FocusEngine ai_engine(driver, authority, "ai", "ai-focus", {"microphone", "speaker"}, 10000);
    FocusEngine human_engine(driver, human_authority, "human", "human-focus",
                             {"microphone", "speaker"}, 10000);
    // apply 用公共原编码器及真实引擎/驱动收原整组事实，时间推进由夹具显式控制。
    const auto apply = [&](FocusEngine& engine, const FocusCommand& request, std::uint64_t now) {
        std::string json;  // 原报文只进真实解析器，不直接注入ACK。
        ASSERT_EQ(encode_envelope(request.envelope, json), DecodeResult::Ok);
        ASSERT_EQ(engine.submit(json, now, 11000 + now).admission, FocusAdmission::Accepted);
        engine.poll(now, 11000 + now);
        input.confirm();
        ai.confirm();
        human.confirm();
        ASSERT_TRUE(driver.poll(now + 1));
        engine.poll(now + 1, 11001 + now);
        const auto reply = engine.take_reply();  // 只接受实际driver所产生的原应答。
        ASSERT_TRUE(reply);
        FocusState state;  // 成功应答再次严格解码，不能仅按非空当成功。
        ASSERT_EQ(decode_focus_state(WireRoute::Signaling, *reply, state), WireResult::Ok);
        EXPECT_EQ(state.envelope.code, 200);
        EXPECT_EQ(state.state, request.operation == "suspend" ? "suspended" : "acquired");
    };
    apply(ai_engine, command(), 111);
    ASSERT_TRUE(ai_engine.output_permitted(113, 11113));
    scope.operation_token = "suspend";
    auto suspend = command("suspend", "suspend");
    suspend.envelope.event_sequence = 2;
    apply(ai_engine, suspend, 114);
    EXPECT_FALSE(ai_engine.closed());
    EXPECT_FALSE(ai_engine.output_permitted(116, 11116));
    human_scope.acquire = true;  // 原全组移交独立授权，Peer ready本身不自动置此值。
    FocusCommand acquire;        // 人工原完整请求，AI许可不参与人工签发。
    ASSERT_EQ(
        decode_focus_control(
            WireRoute::Signaling,
            R"({"msgToken":"human-control","method":"focusControl","sessionId":"human","eventSeq":1,"params":{"operation":"acquire","expectedRevision":2,"focusId":"human-focus","resources":["microphone","speaker"],"priority":"call","resumePolicy":"never","expiresAt":18000}})",
            acquire),
        WireResult::Ok);
    apply(human_engine, acquire, 117);
    ASSERT_TRUE(human_engine.output_permitted(119, 11119));
    scope.handoff_committed = true;  // 平台原最终提交后AI旧lease不可复活。
    ai_engine.poll(120, 11120);
    EXPECT_TRUE(ai_engine.closed());
    input.confirm();
    ai.confirm();
    ASSERT_TRUE(driver.poll(121));
    EXPECT_TRUE(human_engine.output_permitted(122, 11122));
    scope.handoff_committed = false;
    EXPECT_FALSE(ai_engine.output_permitted(123, 11123));
    EXPECT_EQ(driver.revision(), 3U);  // 物理关闭不会伪造release/CAS推进。
}
}  // namespace
}  // namespace ur::device
