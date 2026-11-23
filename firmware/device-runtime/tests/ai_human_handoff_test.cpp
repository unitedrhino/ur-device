// 文件说明：AI转人工原会话/两腿关联与三屏障回归，假端口只给显式事实、不冒真实端云验收。
#include "ur/device/ai_human_handoff.hpp"
#include <gtest/gtest.h>
#include <array>
#include <deque>
namespace ur::device {
namespace {
// Publish 记录实际共享上行，失败不触发业务重试。
struct Publish : PublishPort {
    bool ok = true;
    std::vector<Envelope> sent;
    bool publish(std::string_view, std::string_view body) override {
        Envelope e;
        if (decode_envelope(body, e) != DecodeResult::Ok)
            return false;
        sent.push_back(e);
        return ok;
    }
};
// Sessions 区分候选Peer准备与整组资源移交，任何fence/release都单独可断言。
struct Sessions : AiHandoffSessionPort {
    bool owned = true, prepare_ok = true, restore_ok = true;
    std::optional<std::uint64_t> media = 1;
    unsigned prepared = 0, closed = 0, fenced = 0, released = 0;
    bool ai_authorized_and_owned(std::string_view, std::string_view, std::uint64_t,
                                 std::uint64_t) const override {
        return owned;
    }
    bool prepare_voice_candidate(const Envelope&) override {
        ++prepared;
        return prepare_ok;
    }
    std::optional<std::uint64_t> candidate_media_revision(std::string_view) const override {
        return media;
    }
    bool restore_authorized(std::string_view, std::string_view, std::uint64_t,
                            std::uint64_t) const override {
        return restore_ok;
    }
    void close_candidate(std::string_view) override { ++closed; }
    void fence_ai(std::string_view) override { ++fenced; }
    void release_ai(std::string_view) override { ++released; }
};
// Focus 受理与实际结果分开；只有complete才提供原关联整组回执。
struct Focus : AiHandoffFocusPort {
    bool supported = true;
    FocusAdmission admission = FocusAdmission::Accepted;
    std::vector<FocusCommand> commands;
    std::deque<AiHandoffFocusResult> facts;
    bool supports_microphone_speaker() const override { return supported; }
    FocusSubmission submit(const FocusCommand& c, std::uint64_t, std::uint64_t) override {
        commands.push_back(c);
        return {admission, {}};
    }
    std::optional<AiHandoffFocusResult> take_result() override {
        if (facts.empty())
            return {};
        auto f = facts.front();
        facts.pop_front();
        return f;
    }
    void complete(std::string state, int code = 200, std::string reason = {},
                  bool not_applied = false) {
        Envelope e;
        const auto& c = commands.back();
        ASSERT_EQ(focus_reply(c, state, c.expected_revision + (state == "denied" ? 0 : 1), code,
                              reason, e),
                  WireResult::Ok);
        std::string body;
        ASSERT_EQ(encode_envelope(e, body), DecodeResult::Ok);
        FocusState f;
        ASSERT_EQ(decode_focus_state(WireRoute::Signaling, body, f), WireResult::Ok);
        facts.push_back({f, not_applied});
    }
};
// Event 使用正式信封decoder，不绕过公共参数/应答边界。
Envelope event(std::string body) {
    Envelope e;
    EXPECT_EQ(decode_envelope(body, e), DecodeResult::Ok);
    return e;
}
// HandoffFixture 固定测试身份与双时钟，不访问模型/网络/硬件。
struct HandoffFixture : testing::Test {
    Publish p;
    Sessions s;
    Focus f;
    AiHandoffConfig c{"up", "ai", "ai-focus", "human-focus", 0, 10, 100, 50000, 100000};
    AiHumanHandoff h{p, s, f, c};
    Envelope ack = event(
        R"({"msgToken":"escalate","method":"callState","sessionId":"ai","code":200,"data":{"state":"transferring","revision":1,"candidateSessionId":"remote","localSessionId":"local","callGroupId":"group"}})");
    Envelope invitation = event(
        R"({"msgToken":"invite","method":"sessionInvite","sessionId":"local","eventSeq":1,"params":{"purpose":"humanCall","transport":"webrtc","admission":"accept","tracks":[{"trackId":"voice","kind":"audio","sourceId":"microphone","sourceType":"microphone","direction":"sendrecv","codec":"opus","state":"requested"}],"expiresAt":31000,"callType":"voice","callGroupId":"group"}})");
    Envelope suspend = event(
        R"({"msgToken":"suspend","method":"focusControl","sessionId":"ai","eventSeq":101,"params":{"operation":"suspend","expectedRevision":10,"focusId":"ai-focus","resources":["microphone","speaker"],"priority":"conversation","resumePolicy":"explicit"}})");
    Envelope acquire = event(
        R"({"msgToken":"acquire","method":"focusControl","sessionId":"local","eventSeq":2,"params":{"operation":"acquire","expectedRevision":11,"focusId":"human-focus","resources":["microphone","speaker"],"priority":"call","resumePolicy":"never","expiresAt":44000}})");
    Envelope restore = event(
        R"({"msgToken":"restore","method":"focusControl","sessionId":"ai","eventSeq":103,"params":{"operation":"restore","expectedRevision":11,"focusId":"ai-focus","resources":["microphone","speaker"],"priority":"conversation","resumePolicy":"explicit","expiresAt":44000}})");
    Envelope established = event(
        R"({"msgToken":"established","method":"sessionEstablished","sessionId":"local","eventSeq":3,"data":{"state":"established","mediaRevision":1}})");
    Envelope transferred = event(
        R"({"msgToken":"transferred","method":"callState","sessionId":"ai","eventSeq":102,"data":{"state":"transferred","revision":2,"candidateSessionId":"remote","localSessionId":"local","callGroupId":"group","requestToken":"escalate"}})");
    Envelope ended = event(
        R"({"msgToken":"ended","method":"sessionEnded","sessionId":"ai","eventSeq":103,"data":{"state":"ended","reason":"transferred"}})");
    Envelope failed = event(
        R"({"msgToken":"failed","method":"callState","sessionId":"ai","eventSeq":102,"data":{"state":"failed","revision":2,"candidateSessionId":"remote","localSessionId":"local","callGroupId":"group","requestToken":"escalate","reason":"transferRejected"}})");
    void bind() {
        ASSERT_TRUE(h.begin("escalate", "service", "human-support", 0, 1000));
        ASSERT_EQ(h.receive(ack, 1, 1001), AiHandoffResult::Applied);
        ASSERT_EQ(h.receive(invitation, 2, 1002), AiHandoffResult::Applied);
    }
    // suspend_actual/acquire_actual 分开原受理与实际完成，不借begin成功伪造焦点。
    void suspend_actual() {
        bind();
        ASSERT_EQ(h.receive(suspend, 3, 1003), AiHandoffResult::Applied);
        f.complete("suspended");
        h.poll(4, 1004);
        ASSERT_EQ(h.state(), AiHandoffState::Suspended);
    }
    void acquire_actual() {
        suspend_actual();
        ASSERT_EQ(h.receive(acquire, 5, 1005), AiHandoffResult::Applied);
        f.complete("acquired");
        h.poll(6, 1006);
        ASSERT_EQ(h.state(), AiHandoffState::Acquired);
    }
    // human_lease只修改原公开expiresAt；不改变原两腿、资源CAS、候选意图或45秒操作截止。
    Envelope human_lease(std::uint64_t expires) const {
        auto result = acquire;
        const auto offset = result.payload.find("44000");
        if (offset != std::string::npos)
            result.payload.replace(offset, 5, std::to_string(expires));
        return result;
    }
};
TEST_F(HandoffFixture, HumanBusinessLeaseCanOutliveCandidateOperationWindow) {
    suspend_actual();
    const auto business = human_lease(48000);
    ASSERT_EQ(h.receive(business, 5, 1005), AiHandoffResult::Applied);
    ASSERT_EQ(f.commands.size(), 2U);
    EXPECT_EQ(f.commands.back().expires_at, 48000U);
    f.complete("acquired");
    h.poll(6, 1006);
    ASSERT_EQ(h.receive(established, 7, 1007), AiHandoffResult::Applied);
    ASSERT_EQ(h.receive(transferred, 8, 1008), AiHandoffResult::Applied);
    h.poll(46000, 47000);
    EXPECT_EQ(h.state(), AiHandoffState::HumanActive);
    EXPECT_EQ(s.fenced, 1U);
}
TEST_F(HandoffFixture, BusinessLeaseCannotExtendCandidateAdmissionDeadline) {
    suspend_actual();
    EXPECT_EQ(h.receive(human_lease(48000), 45000, 46000), AiHandoffResult::Rejected);
    // 从未接纳acquire时仍有独立补偿资格；拒绝迟到取得不应捏造未知副作用或自动恢复。
    EXPECT_EQ(h.state(), AiHandoffState::Suspended);
    EXPECT_EQ(f.commands.size(), 1U);
    EXPECT_EQ(s.fenced, 0U);
}
TEST_F(HandoffFixture, HumanLeaseCannotExceedTrustedAiWallDeadline) {
    suspend_actual();
    EXPECT_EQ(h.receive(human_lease(100001), 5, 1005), AiHandoffResult::Rejected);
    EXPECT_EQ(f.commands.size(), 1U);
    EXPECT_EQ(h.state(), AiHandoffState::Suspended);
}
TEST_F(HandoffFixture, HumanLeaseCannotExceedTrustedAiMonotonicDeadline) {
    // 两钟域余量不必相等；真实授权器/Engine另验permit较早截止，此处核协调器不能迟到提交。
    auto bounded = c;
    bounded.deadline = 20;
    AiHumanHandoff original(p, s, f, bounded);
    auto bounded_invitation = invitation;  // 邀请本身仍位于原独立短截止内，不拿非法邀请制造RED。
    bounded_invitation.payload.replace(bounded_invitation.payload.find("31000"), 5, "1015");
    ASSERT_TRUE(original.begin("escalate", "service", "human-support", 0, 1000));
    ASSERT_EQ(original.receive(ack, 1, 1001), AiHandoffResult::Applied);
    ASSERT_EQ(original.receive(bounded_invitation, 2, 1002), AiHandoffResult::Applied);
    ASSERT_EQ(original.receive(suspend, 3, 1003), AiHandoffResult::Applied);
    f.complete("suspended");
    original.poll(4, 1004);
    ASSERT_EQ(original.receive(human_lease(51001), 5, 1005), AiHandoffResult::Applied);
    f.complete("acquired");
    original.poll(bounded.deadline, 1006);
    EXPECT_EQ(original.state(), AiHandoffState::Unknown);
    original.receive(established, 21, 1007);
    original.receive(transferred, 22, 1008);
    EXPECT_NE(original.state(), AiHandoffState::HumanActive);
    EXPECT_EQ(s.fenced, 0U);
    EXPECT_EQ(s.released, 0U);
}
TEST_F(HandoffFixture, ExplicitIntentPublishesOnceAndKeepsAiBeforeActualBarriers) {
    ASSERT_TRUE(h.begin("escalate", "service", "human-support", 0, 1000));
    ASSERT_EQ(p.sent.size(), 1U);
    EXPECT_EQ(p.sent[0].method, "callControl");
    EXPECT_EQ(p.sent[0].session, "ai");
    EXPECT_TRUE(h.ai_forwarding_allowed());
    EXPECT_FALSE(h.begin("again", "device", "target", 1, 1001));
    EXPECT_EQ(p.sent.size(), 1U);
    EXPECT_EQ(s.fenced, 0U);
}
// 分享只编码原本人引用，不本地授予权限、触焦点或重复发布原请求。
TEST_F(HandoffFixture, ExplicitSingleConsentKeepsOriginalRequestAndAi) {
    ASSERT_TRUE(h.begin("escalate", "service", "human-support", 0, 1000, "owner-consent"));
    ASSERT_EQ(p.sent.size(), 1U);
    EXPECT_EQ(p.sent[0].token, "escalate");
    EXPECT_EQ(p.sent[0].session, "ai");
    EXPECT_EQ(
        p.sent[0].payload,
        R"({"consentIds":["owner-consent"],"expectedRevision":0,"operation":"escalate","shareContext":true,"target":{"id":"human-support","kind":"service"}})");
    const auto original = p.sent[0].payload;
    EXPECT_FALSE(h.begin("escalate", "service", "human-support", 1, 1001, "owner-consent"));
    EXPECT_FALSE(h.begin("escalate", "service", "human-support", 2, 1002, "replacement"));
    h.poll(3, 1003);
    ASSERT_EQ(p.sent.size(), 1U);
    EXPECT_EQ(p.sent[0].payload, original);
    EXPECT_TRUE(h.ai_forwarding_allowed());
    EXPECT_TRUE(f.commands.empty());
    EXPECT_EQ(s.released, 0U);
    EXPECT_EQ(s.fenced, 0U);
}
// 空/越界/控制符及device目标分享均在原状态和发布副作用前拒绝。
TEST_F(HandoffFixture, InvalidOrNonServiceConsentDoesNotConsumeOriginalIntent) {
    for (const auto& reference : std::vector<std::string>{"", std::string(129, 'x'), "bad\nref",
                                                          std::string("bad\0ref", 7)}) {
        EXPECT_FALSE(h.begin("escalate", "service", "human-support", 0, 1000, reference));
        EXPECT_EQ(h.state(), AiHandoffState::AiActive);
        EXPECT_TRUE(p.sent.empty());
    }
    EXPECT_FALSE(h.begin("escalate", "device", "p:other", 0, 1000, "owner-consent"));
    EXPECT_TRUE(p.sent.empty());
    ASSERT_TRUE(h.begin("escalate", "service", "human-support", 0, 1000));
    EXPECT_EQ(
        p.sent[0].payload,
        R"({"expectedRevision":0,"operation":"escalate","shareContext":false,"target":{"id":"human-support","kind":"service"}})");
}
// 网络受理失败保持Unknown且原AI未释放，不以分享授权重试副作用。
TEST_F(HandoffFixture, FailedSharePublishDoesNotClearOrAutomaticallyRetryAi) {
    p.ok = false;
    EXPECT_FALSE(h.begin("escalate", "service", "human-support", 0, 1000, "owner-consent"));
    EXPECT_EQ(h.state(), AiHandoffState::Unknown);
    EXPECT_FALSE(h.begin("escalate", "service", "human-support", 1, 1001, "owner-consent"));
    h.poll(2, 1002);
    EXPECT_EQ(p.sent.size(), 1U);
    EXPECT_EQ(s.released, 0U);
    EXPECT_EQ(s.fenced, 0U);
    EXPECT_TRUE(f.commands.empty());
}
TEST_F(HandoffFixture, BoundVoiceInviteOnlyPreparesCandidate) {
    bind();
    EXPECT_EQ(s.prepared, 1U);
    EXPECT_TRUE(h.ai_forwarding_allowed());
    EXPECT_TRUE(f.commands.empty());
    EXPECT_EQ(s.fenced, 0U);
}
TEST_F(HandoffFixture, FinalCommitRequiresBothActualGroupsAndBothPlatformFacts) {
    acquire_actual();
    EXPECT_FALSE(h.ai_forwarding_allowed());
    EXPECT_EQ(h.receive(established, 7, 1007), AiHandoffResult::Applied);
    EXPECT_EQ(s.fenced, 0U);
    EXPECT_EQ(s.released, 0U);
    EXPECT_EQ(h.receive(transferred, 8, 1008), AiHandoffResult::Applied);
    EXPECT_EQ(h.state(), AiHandoffState::HumanActive);
    EXPECT_EQ(s.fenced, 1U);
    EXPECT_EQ(h.receive(ended, 9, 1009), AiHandoffResult::Applied);
    EXPECT_EQ(s.released, 1U);
    EXPECT_EQ(h.receive(ended, 10, 1010), AiHandoffResult::Duplicate);
    EXPECT_EQ(s.released, 1U);
    EXPECT_EQ(s.fenced, 1U);
    EXPECT_EQ(s.closed, 0U);
}
TEST_F(HandoffFixture, OtherLegCommitMayPrecedeLocalEstablishedWithoutEarlyRelease) {
    acquire_actual();
    ASSERT_EQ(h.receive(transferred, 7, 1007), AiHandoffResult::Applied);
    ASSERT_EQ(h.receive(ended, 8, 1008), AiHandoffResult::Applied);
    EXPECT_EQ(s.fenced, 0U);
    EXPECT_EQ(s.released, 0U);
    ASSERT_EQ(h.receive(established, 9, 1009), AiHandoffResult::Applied);
    EXPECT_EQ(s.fenced, 1U);
    EXPECT_EQ(s.released, 1U);
}
TEST_F(HandoffFixture, EarlyCommitAndEndedCannotImpersonateFocusCompletion) {
    bind();
    EXPECT_EQ(h.receive(established, 3, 1003), AiHandoffResult::Invalid);
    EXPECT_EQ(h.receive(transferred, 4, 1004), AiHandoffResult::Invalid);
    EXPECT_EQ(h.receive(ended, 5, 1005), AiHandoffResult::Invalid);
    EXPECT_TRUE(h.ai_forwarding_allowed());
    EXPECT_EQ(s.fenced, 0U);
    EXPECT_EQ(s.closed, 0U);
}
TEST_F(HandoffFixture, InviteBeforeBindingDoesNotPrepareOrStealAi) {
    ASSERT_TRUE(h.begin("escalate", "device", "target", 0, 1000));
    ASSERT_EQ(h.receive(invitation, 1, 1001), AiHandoffResult::Applied);
    EXPECT_EQ(s.prepared, 0U);
    EXPECT_TRUE(h.ai_forwarding_allowed());
    EXPECT_EQ(h.receive(invitation, 2, 1002), AiHandoffResult::Duplicate);
    ASSERT_EQ(h.receive(ack, 3, 1003), AiHandoffResult::Applied);
    EXPECT_EQ(s.prepared, 1U);
    EXPECT_EQ(h.receive(ack, 4, 1004), AiHandoffResult::Duplicate);
}
TEST_F(HandoffFixture, WrongCachedLegCannotBindOrPrepare) {
    ASSERT_TRUE(h.begin("escalate", "device", "target", 0, 1000));
    invitation.session = "other";
    ASSERT_EQ(h.receive(invitation, 1, 1001), AiHandoffResult::Applied);
    EXPECT_EQ(h.receive(ack, 2, 1002), AiHandoffResult::Invalid);
    EXPECT_EQ(s.prepared, 0U);
    EXPECT_TRUE(h.ai_forwarding_allowed());
}
TEST_F(HandoffFixture, RejectTimeoutAndCancelledCandidateKeepAiWithoutFocus) {
    bind();
    EXPECT_EQ(h.receive(failed, 3, 1003), AiHandoffResult::Applied);
    EXPECT_EQ(s.closed, 1U);
    EXPECT_TRUE(h.ai_forwarding_allowed());
    EXPECT_TRUE(f.commands.empty());
    EXPECT_EQ(h.receive(invitation, 4, 1004), AiHandoffResult::Rejected);
}
TEST_F(HandoffFixture, FixedCandidateDeadlineDoesNotRenewFromAckDuplicate) {
    bind();
    EXPECT_EQ(h.receive(ack, 44000, 45000), AiHandoffResult::Duplicate);
    h.poll(45000, 46000);
    EXPECT_EQ(h.state(), AiHandoffState::AiActive);
    EXPECT_EQ(s.closed, 1U);
    EXPECT_FALSE(h.begin("new", "device", "target", 45001, 46001));
}
TEST_F(HandoffFixture, CancellationAckIsNotResourceRestore) {
    bind();
    ASSERT_TRUE(h.cancel("cancel", 3, 1003));
    EXPECT_FALSE(h.cancel("again", 4, 1004));
    EXPECT_EQ(p.sent.size(), 2U);
    EXPECT_EQ(p.sent.back().token, "cancel");
    auto cancelled = event(
        R"({"msgToken":"cancel","method":"callState","sessionId":"ai","code":200,"data":{"state":"active","revision":2,"candidateSessionId":"remote","localSessionId":"local","callGroupId":"group","reason":"candidateCancelled"}})");
    EXPECT_EQ(h.receive(cancelled, 5, 1005), AiHandoffResult::Applied);
    EXPECT_TRUE(h.ai_forwarding_allowed());
    EXPECT_EQ(s.closed, 1U);
}
TEST_F(HandoffFixture, DeniedWithoutPhysicalNonExecutionProofRemainsUnknown) {
    suspend_actual();
    ASSERT_EQ(h.receive(acquire, 5, 1005), AiHandoffResult::Applied);
    f.complete("denied", 503, "driverFailed");
    h.poll(6, 1006);
    EXPECT_EQ(h.state(), AiHandoffState::Unknown);
    EXPECT_EQ(h.receive(restore, 7, 1007), AiHandoffResult::Rejected);
    EXPECT_EQ(f.commands.size(), 2U);
    EXPECT_FALSE(h.ai_forwarding_allowed());
}
TEST_F(HandoffFixture, ExplicitNonAcquiredRequiresNewAuthorizedRestoreAndActualRestored) {
    suspend_actual();
    ASSERT_EQ(h.receive(acquire, 5, 1005), AiHandoffResult::Applied);
    f.complete("denied", 503, "driverFailed", true);
    h.poll(6, 1006);
    ASSERT_EQ(h.state(), AiHandoffState::Suspended);
    EXPECT_EQ(h.receive(failed, 7, 1007), AiHandoffResult::Applied);
    EXPECT_EQ(h.receive(restore, 8, 1008), AiHandoffResult::Applied);
    EXPECT_FALSE(h.ai_forwarding_allowed());
    EXPECT_EQ(s.closed, 0U);
    f.complete("restored");
    h.poll(9, 1009);
    EXPECT_TRUE(h.ai_forwarding_allowed());
    EXPECT_EQ(s.closed, 1U);
    EXPECT_EQ(s.fenced, 0U);
}
TEST_F(HandoffFixture, ExpiredOrUnauthorizedRestoreCannotReactivateAi) {
    suspend_actual();
    ASSERT_EQ(h.receive(acquire, 5, 1005), AiHandoffResult::Applied);
    f.complete("denied", 503, "driverFailed", true);
    h.poll(6, 1006);
    s.restore_ok = false;
    EXPECT_EQ(h.receive(restore, 7, 1007), AiHandoffResult::Rejected);
    EXPECT_EQ(f.commands.size(), 2U);
    EXPECT_FALSE(h.ai_forwarding_allowed());
}
TEST_F(HandoffFixture, PartialWrongOrLateActualFactsDoNotAcquireOrCommit) {
    suspend_actual();
    ASSERT_EQ(h.receive(acquire, 5, 1005), AiHandoffResult::Applied);
    f.complete("acquired");
    f.facts.back().actual.envelope.token = "old";
    h.poll(6, 1006);
    EXPECT_EQ(h.state(), AiHandoffState::Acquiring);
    f.complete("acquired");
    f.facts.back().actual.resources = {"speaker"};
    h.poll(7, 1007);
    EXPECT_EQ(h.state(), AiHandoffState::Unknown);
    EXPECT_EQ(h.receive(established, 8, 1008), AiHandoffResult::Invalid);
    EXPECT_EQ(s.fenced, 0U);
}
TEST_F(HandoffFixture, PendingTimeoutIsUnknownAndCannotBlindRestore) {
    bind();
    ASSERT_EQ(h.receive(suspend, 3, 1003), AiHandoffResult::Applied);
    h.poll(45000, 46000);
    EXPECT_EQ(h.state(), AiHandoffState::Unknown);
    EXPECT_FALSE(h.ai_forwarding_allowed());
    EXPECT_EQ(h.receive(restore, 45001, 46001), AiHandoffResult::Rejected);
    EXPECT_EQ(f.commands.size(), 1U);
    EXPECT_EQ(s.closed, 0U);
}
TEST_F(HandoffFixture, SharedWatermarkDoesNotRequireConsecutiveHandoffEvents) {
    bind();
    EXPECT_TRUE(h.observe_event("ai", 150));
    suspend.event_sequence = 151;
    EXPECT_EQ(h.receive(suspend, 3, 1003), AiHandoffResult::Applied);
    EXPECT_EQ(h.receive(suspend, 4, 1004), AiHandoffResult::Duplicate);
    EXPECT_EQ(f.commands.size(), 1U);
    EXPECT_FALSE(h.observe_event("ai", 151));
    EXPECT_FALSE(h.observe_event("other", 1));
}
TEST_F(HandoffFixture, UnsupportedDriverOrLostOriginalLeaseCannotStart) {
    f.supported = false;
    EXPECT_FALSE(h.begin("escalate", "device", "target", 0, 1000));
    f.supported = true;
    s.owned = false;
    EXPECT_FALSE(h.begin("escalate", "device", "target", 0, 1000));
    EXPECT_TRUE(p.sent.empty());
}
TEST_F(HandoffFixture, NoInviteOrActualMediaCannotAuthorizeSuspend) {
    ASSERT_TRUE(h.begin("escalate", "device", "target", 0, 1000));
    ASSERT_EQ(h.receive(ack, 1, 1001), AiHandoffResult::Applied);
    EXPECT_EQ(h.receive(suspend, 2, 1002), AiHandoffResult::Rejected);
    ASSERT_EQ(h.receive(invitation, 3, 1003), AiHandoffResult::Applied);
    s.media.reset();
    EXPECT_EQ(h.receive(suspend, 4, 1004), AiHandoffResult::Rejected);
    EXPECT_TRUE(f.commands.empty());
}
TEST_F(HandoffFixture, SharedPublishBackpressureNeverRetriesIntentOrFocus) {
    p.ok = false;
    EXPECT_FALSE(h.begin("escalate", "device", "target", 0, 1000));
    h.poll(1, 1001);
    EXPECT_EQ(p.sent.size(), 1U);
    EXPECT_EQ(h.state(), AiHandoffState::Unknown);
}
TEST_F(HandoffFixture, CancelledPendingAcquireCannotBeRevivedByLateActualFact) {
    suspend_actual();
    ASSERT_EQ(h.receive(acquire, 5, 1005), AiHandoffResult::Applied);
    ASSERT_TRUE(h.cancel("cancel", 6, 1006));
    auto cancelled = event(
        R"({"msgToken":"cancel","method":"callState","sessionId":"ai","code":200,"data":{"state":"active","revision":2,"candidateSessionId":"remote","localSessionId":"local","callGroupId":"group","reason":"candidateCancelled"}})");
    ASSERT_EQ(h.receive(cancelled, 7, 1007), AiHandoffResult::Applied);
    ASSERT_EQ(h.state(), AiHandoffState::Unknown);
    f.complete("acquired");
    h.poll(8, 1008);
    EXPECT_EQ(h.state(), AiHandoffState::Unknown);
    EXPECT_EQ(h.receive(transferred, 9, 1009), AiHandoffResult::Invalid);
    EXPECT_EQ(h.receive(established, 10, 1010), AiHandoffResult::Invalid);
    EXPECT_EQ(s.fenced, 0U);
    EXPECT_FALSE(h.ai_forwarding_allowed());
}
TEST_F(HandoffFixture, NonVoiceNullTrackAndUnknownInviteFieldCannotPrepare) {
    ASSERT_TRUE(h.begin("escalate", "device", "target", 0, 1000));
    ASSERT_EQ(h.receive(ack, 1, 1001), AiHandoffResult::Applied);
    for (
        const auto& body :
        {R"({"msgToken":"i","method":"sessionInvite","sessionId":"local","eventSeq":1,"params":{"purpose":"humanCall","transport":"webrtc","admission":"accept","tracks":null,"expiresAt":31000,"callType":"voice","callGroupId":"group"}})",
         R"({"msgToken":"i","method":"sessionInvite","sessionId":"local","eventSeq":1,"params":{"purpose":"humanCall","transport":"webrtc","admission":"accept","tracks":[],"expiresAt":31000,"callType":"video","callGroupId":"group"}})"}) {
        EXPECT_EQ(h.receive(event(body), 2, 1002), AiHandoffResult::Invalid);
    }
    auto invalid = invitation;
    invalid.payload.insert(1, "\"implicitActivate\":true,");
    EXPECT_EQ(h.receive(invalid, 3, 1003), AiHandoffResult::Invalid);
    EXPECT_EQ(s.prepared, 0U);
    EXPECT_TRUE(h.ai_forwarding_allowed());
}
TEST_F(HandoffFixture, WrongAckScopeRevisionOrMissingLegDoesNotCreateCandidate) {
    ASSERT_TRUE(h.begin("escalate", "device", "target", 0, 1000));
    auto wrong = ack;
    wrong.token = "other";
    EXPECT_EQ(h.receive(wrong, 1, 1001), AiHandoffResult::Invalid);
    wrong = ack;
    wrong.payload =
        R"({"state":"transferring","revision":0,"candidateSessionId":"remote","localSessionId":"local","callGroupId":"group"})";
    EXPECT_EQ(h.receive(wrong, 2, 1002), AiHandoffResult::Invalid);
    wrong.payload =
        R"({"state":"transferring","revision":1,"candidateSessionId":"remote","callGroupId":"group"})";
    EXPECT_EQ(h.receive(wrong, 3, 1003), AiHandoffResult::Invalid);
    EXPECT_TRUE(h.local_session().empty());
    EXPECT_TRUE(h.ai_forwarding_allowed());
}
TEST_F(HandoffFixture, PartialResourceOrWrongFocusCasDoesNotReachDriver) {
    bind();
    auto wrong = suspend;
    wrong.payload =
        R"({"operation":"suspend","expectedRevision":10,"focusId":"ai-focus","resources":["speaker"],"priority":"conversation","resumePolicy":"explicit"})";
    EXPECT_EQ(h.receive(wrong, 3, 1003), AiHandoffResult::Invalid);
    wrong.payload =
        R"({"operation":"suspend","expectedRevision":9,"focusId":"ai-focus","resources":["microphone","speaker"],"priority":"conversation","resumePolicy":"explicit"})";
    EXPECT_EQ(h.receive(wrong, 4, 1004), AiHandoffResult::Rejected);
    EXPECT_TRUE(f.commands.empty());
    EXPECT_TRUE(h.ai_forwarding_allowed());
}
TEST_F(HandoffFixture, CandidatePrepareFailureAndTypedConflictKeepAi) {
    ASSERT_TRUE(h.begin("escalate", "device", "target", 0, 1000));
    auto denied = event(
        R"({"msgToken":"escalate","method":"callState","sessionId":"ai","code":409,"data":{"currentRevision":1,"reason":"staleRevision"}})");
    EXPECT_EQ(h.receive(denied, 1, 1001), AiHandoffResult::Applied);
    EXPECT_EQ(h.state(), AiHandoffState::AiActive);
    EXPECT_TRUE(h.local_session().empty());
    EXPECT_EQ(h.receive(ack, 2, 1002), AiHandoffResult::Invalid);
}
TEST_F(HandoffFixture, IndependentMediaReadyMustMatchFinalMediaRevision) {
    acquire_actual();
    s.media = 2;
    EXPECT_EQ(h.receive(established, 7, 1007), AiHandoffResult::Invalid);
    EXPECT_EQ(s.fenced, 0U);
    EXPECT_EQ(h.state(), AiHandoffState::Acquired);
}
TEST_F(HandoffFixture, ExplicitSuspendNeverAppliedRetainsAiButTimeoutDoesNot) {
    bind();
    ASSERT_EQ(h.receive(suspend, 3, 1003), AiHandoffResult::Applied);
    f.complete("denied", 409, "resourceBusy", true);
    h.poll(4, 1004);
    EXPECT_EQ(h.state(), AiHandoffState::AiActive);
    EXPECT_TRUE(h.ai_forwarding_allowed());
    EXPECT_EQ(s.closed, 1U);
    EXPECT_EQ(s.fenced, 0U);
}
TEST_F(HandoffFixture, BackpressureOnActualReplyDoesNotRepeatDriverOrClaimCommit) {
    bind();
    ASSERT_EQ(h.receive(suspend, 3, 1003), AiHandoffResult::Applied);
    f.complete("suspended");
    p.ok = false;
    h.poll(4, 1004);
    ASSERT_EQ(h.state(), AiHandoffState::Unknown);
    p.ok = true;
    h.poll(5, 1005);
    EXPECT_EQ(f.commands.size(), 1U);
    EXPECT_EQ(p.sent.size(), 2U);
    EXPECT_FALSE(h.ai_forwarding_allowed());
}
TEST_F(HandoffFixture, ClockRollbackCannotGenerateRestoreOrHumanActivation) {
    suspend_actual();
    h.poll(3, 1003);
    EXPECT_EQ(h.state(), AiHandoffState::Unknown);
    EXPECT_FALSE(h.ai_forwarding_allowed());
    EXPECT_EQ(s.fenced, 0U);
    EXPECT_EQ(f.commands.size(), 1U);
}
TEST_F(HandoffFixture, CandidateExpiryBeforeAnyAcquireDoesNotBlockAuthorizedAiCompensation) {
    suspend_actual();
    restore.payload =
        R"({"operation":"restore","expectedRevision":11,"focusId":"ai-focus","resources":["microphone","speaker"],"priority":"conversation","resumePolicy":"explicit","expiresAt":49000})";
    EXPECT_EQ(h.receive(acquire, 46000, 47000), AiHandoffResult::Rejected);
    ASSERT_EQ(h.receive(restore, 46000, 47000), AiHandoffResult::Applied);
    EXPECT_FALSE(h.ai_forwarding_allowed());
    f.complete("restored");
    h.poll(46001, 47001);
    EXPECT_TRUE(h.ai_forwarding_allowed());
    EXPECT_EQ(f.commands.size(), 2U);
    EXPECT_EQ(s.fenced, 0U);
}
TEST_F(HandoffFixture, ExplicitNonAcquiredCompensationUsesOriginalLeaseNotCandidateDeadline) {
    suspend_actual();
    ASSERT_EQ(h.receive(acquire, 5, 1005), AiHandoffResult::Applied);
    f.complete("denied", 503, "driverFailed", true);
    h.poll(6, 1006);
    restore.payload =
        R"({"operation":"restore","expectedRevision":11,"focusId":"ai-focus","resources":["microphone","speaker"],"priority":"conversation","resumePolicy":"explicit","expiresAt":49000})";
    ASSERT_EQ(h.receive(restore, 46000, 47000), AiHandoffResult::Applied);
    EXPECT_FALSE(h.ai_forwarding_allowed());
    f.complete("restored");
    h.poll(46001, 47001);
    EXPECT_TRUE(h.ai_forwarding_allowed());
    EXPECT_EQ(s.closed, 1U);
}
TEST_F(HandoffFixture, HumanFocusMayBindOnlyFromAuthorizedOriginalAcquireAfterSuspend) {
    c.human_focus.clear();
    AiHumanHandoff fresh(p, s, f, c);
    ASSERT_TRUE(fresh.begin("escalate", "device", "target", 0, 1000));
    ASSERT_EQ(fresh.receive(ack, 1, 1001), AiHandoffResult::Applied);
    ASSERT_EQ(fresh.receive(invitation, 2, 1002), AiHandoffResult::Applied);
    EXPECT_EQ(fresh.receive(acquire, 3, 1003), AiHandoffResult::Rejected);
    EXPECT_TRUE(f.commands.empty());
    ASSERT_EQ(fresh.receive(suspend, 4, 1004), AiHandoffResult::Applied);
    f.complete("suspended");
    fresh.poll(5, 1005);
    ASSERT_EQ(fresh.receive(acquire, 6, 1006), AiHandoffResult::Applied);
    EXPECT_EQ(f.commands.back().focus_id, "human-focus");
    f.complete("acquired");
    fresh.poll(7, 1007);
    EXPECT_EQ(fresh.state(), AiHandoffState::Acquired);
    EXPECT_EQ(s.fenced, 0U);
}
TEST_F(HandoffFixture, RejectedFirstFocusCannotBindOrGrantASecondLease) {
    c.human_focus.clear();
    AiHumanHandoff fresh(p, s, f, c);
    ASSERT_TRUE(fresh.begin("escalate", "device", "target", 0, 1000));
    ASSERT_EQ(fresh.receive(ack, 1, 1001), AiHandoffResult::Applied);
    ASSERT_EQ(fresh.receive(invitation, 2, 1002), AiHandoffResult::Applied);
    ASSERT_EQ(fresh.receive(suspend, 3, 1003), AiHandoffResult::Applied);
    f.complete("suspended");
    fresh.poll(4, 1004);
    auto denied = acquire;
    denied.payload =
        R"({"operation":"acquire","expectedRevision":11,"focusId":"unauthorized-focus","resources":["microphone","speaker"],"priority":"call","resumePolicy":"never","expiresAt":44000})";
    f.admission = FocusAdmission::Conflict;
    ASSERT_EQ(fresh.receive(denied, 5, 1005), AiHandoffResult::Rejected);
    EXPECT_EQ(fresh.state(), AiHandoffState::Suspended);
    f.admission = FocusAdmission::Accepted;
    ASSERT_EQ(fresh.receive(acquire, 6, 1006), AiHandoffResult::Applied);
    EXPECT_EQ(f.commands.back().focus_id, "human-focus");
    EXPECT_EQ(s.fenced, 0U);
}
TEST_F(HandoffFixture, OriginalFocusReplayReturnsFixedActualBytesWithoutSideEffectsOrRenewal) {
    suspend_actual();
    const auto original = p.sent.back();
    ASSERT_TRUE(h.observe_event("ai", 150));
    EXPECT_EQ(h.receive(suspend, 5, 1005), AiHandoffResult::Duplicate);
    ASSERT_EQ(p.sent.size(), 3U);
    EXPECT_EQ(p.sent.back().payload, original.payload);
    EXPECT_EQ(p.sent.back().token, original.token);
    EXPECT_EQ(f.commands.size(), 1U);
    auto conflicting = suspend;
    conflicting.payload =
        R"({"operation":"suspend","expectedRevision":10,"focusId":"ai-focus","resources":["microphone","speaker"],"priority":"call","resumePolicy":"explicit"})";
    EXPECT_EQ(h.receive(conflicting, 6, 1006), AiHandoffResult::Invalid);
    EXPECT_EQ(f.commands.size(), 1U);
    EXPECT_EQ(p.sent.size(), 3U);
}
// replace_field 保留完整原信封，只改变明确的单个业务字段以定位拒绝分支。
Envelope replace_field(Envelope original, std::string_view before, std::string_view after) {
    const auto offset = original.payload.find(before);
    EXPECT_NE(offset, std::string::npos);
    if (offset != std::string::npos)
        original.payload.replace(offset, before.size(), after);
    return original;
}
// 短父租约邀请仍按各自绝对截止和30秒合同准入，接纳只准备Peer而不取得任何焦点。
TEST_F(HandoffFixture, ShortParentInviteUsesAbsoluteDeadlinesWithIndependentClockRounding) {
    auto bounded = c;
    bounded.deadline = 30000;
    bounded.expires_at = 31000;
    AiHumanHandoff original(p, s, f, bounded);
    ASSERT_TRUE(original.begin("escalate", "service", "human-support", 0, 1000));
    ASSERT_EQ(original.receive(ack, 1, 1001), AiHandoffResult::Applied);
    ASSERT_EQ(original.receive(invitation, 3, 1002), AiHandoffResult::Applied);
    EXPECT_EQ(s.prepared, 1U);
    EXPECT_TRUE(f.commands.empty());
    EXPECT_EQ(s.fenced, 0U);
    EXPECT_EQ(s.released, 0U);
    EXPECT_EQ(original.receive(invitation, 4, 1003), AiHandoffResult::Ignored);
    EXPECT_EQ(original.receive(ack, 29999, 2000), AiHandoffResult::Duplicate);
    original.poll(bounded.deadline, 2001);
    EXPECT_EQ(original.state(), AiHandoffState::AiActive);
    EXPECT_EQ(s.closed, 1U);
    EXPECT_EQ(s.prepared, 1U);
    EXPECT_EQ(original.receive(invitation, bounded.deadline, 2001), AiHandoffResult::Rejected);
    EXPECT_EQ(original.receive(acquire, bounded.deadline, 2001), AiHandoffResult::Rejected);
    EXPECT_FALSE(original.begin("another", "service", "human-support", 30001, 2002));
    EXPECT_TRUE(f.commands.empty());
}
// 原短期限内早到与重放只保留原邀请；ACK不能重算候选截止或提前授予采集权。
TEST_F(HandoffFixture, EarlyShortParentInviteAndAckReplayCannotRenewCandidateDeadline) {
    auto bounded = c;
    bounded.deadline = 30000;
    bounded.expires_at = 31000;
    AiHumanHandoff original(p, s, f, bounded);
    ASSERT_TRUE(original.begin("escalate", "service", "human-support", 0, 1000));
    ASSERT_EQ(original.receive(invitation, 3, 1002), AiHandoffResult::Applied);
    EXPECT_EQ(s.prepared, 0U);
    EXPECT_EQ(original.receive(invitation, 4, 1003), AiHandoffResult::Duplicate);
    ASSERT_EQ(original.receive(ack, 5, 1004), AiHandoffResult::Applied);
    EXPECT_EQ(s.prepared, 1U);
    EXPECT_EQ(original.receive(ack, 29999, 2000), AiHandoffResult::Duplicate);
    original.poll(bounded.deadline, 2001);
    EXPECT_EQ(original.state(), AiHandoffState::AiActive);
    EXPECT_EQ(s.closed, 1U);
    EXPECT_TRUE(f.commands.empty());
    EXPECT_EQ(s.fenced, 0U);
    EXPECT_EQ(s.released, 0U);
}
// 墙钟到原邀请截止即拒绝，不借父单调余量或已受理ACK准备候选媒体。
TEST_F(HandoffFixture, InviteAtOriginalWallDeadlineCannotPrepareCandidate) {
    ASSERT_TRUE(h.begin("escalate", "service", "human-support", 0, 1000));
    ASSERT_EQ(h.receive(ack, 1, 1001), AiHandoffResult::Applied);
    EXPECT_EQ(h.receive(invitation, 3, 31000), AiHandoffResult::Invalid);
    EXPECT_EQ(s.prepared, 0U);
    EXPECT_TRUE(f.commands.empty());
    EXPECT_EQ(s.fenced, 0U);
    EXPECT_TRUE(h.ai_forwarding_allowed());
}
TEST_F(HandoffFixture, EveryVoiceInviteFieldRejectsInvalidValueWithoutPreparingMedia) {
    // 每个样本独立恢复真实Candidate，拒绝必须保留AI且没有Focus或Peer副作用。
    const std::vector<std::pair<std::string, std::string>> mutations{
        {"\"purpose\":\"humanCall\"", "\"purpose\":\"ai\""},
        {"\"transport\":\"webrtc\"", "\"transport\":\"udp\""},
        {"\"admission\":\"accept\"", "\"admission\":\"direct\""},
        {"\"callType\":\"voice\"", "\"callType\":\"video\""},
        {"\"callGroupId\":\"group\"", "\"callGroupId\":\"\""},
        {"\"expiresAt\":31000", "\"expiresAt\":0"},
        {"\"expiresAt\":31000", "\"expiresAt\":1002"},
        {"\"expiresAt\":31000", "\"expiresAt\":31003"},
        {"\"expiresAt\":31000", "\"expiresAt\":100001"},
        {"\"trackId\":\"voice\"", "\"trackId\":\"other\""},
        {"\"kind\":\"audio\"", "\"kind\":\"video\""},
        {"\"sourceId\":\"microphone\"", "\"sourceId\":\"camera\""},
        {"\"sourceType\":\"microphone\"", "\"sourceType\":\"camera\""},
        {"\"direction\":\"sendrecv\"", "\"direction\":\"recvonly\""},
        {"\"codec\":\"opus\"", "\"codec\":\"pcm\""},
        {"\"state\":\"requested\"", "\"state\":\"active\""}};
    for (const auto& mutation : mutations) {
        SCOPED_TRACE(mutation.first + " => " + mutation.second);
        Publish pub;
        Sessions sessions;
        Focus focus;
        AiHumanHandoff original(pub, sessions, focus, c);
        ASSERT_TRUE(original.begin("escalate", "device", "target", 0, 1000));
        ASSERT_EQ(original.receive(ack, 1, 1001), AiHandoffResult::Applied);
        EXPECT_EQ(
            original.receive(replace_field(invitation, mutation.first, mutation.second), 2, 1002),
            AiHandoffResult::Invalid);
        EXPECT_EQ(original.state(), AiHandoffState::Candidate);
        EXPECT_TRUE(original.ai_forwarding_allowed());
        EXPECT_EQ(sessions.prepared, 0U);
        EXPECT_TRUE(focus.commands.empty());
    }
}
TEST_F(HandoffFixture, EarlyInviteDuplicateAndConflictKeepExactlyOneOriginalCandidate) {
    ASSERT_TRUE(h.begin("escalate", "device", "target", 0, 1000));
    EXPECT_EQ(h.receive(invitation, 1, 1001), AiHandoffResult::Applied);
    EXPECT_EQ(h.receive(invitation, 2, 1002), AiHandoffResult::Duplicate);
    auto conflict = invitation;
    conflict.token = "another-invite";
    EXPECT_EQ(h.receive(conflict, 3, 1003), AiHandoffResult::Rejected);
    ASSERT_EQ(h.receive(ack, 4, 1004), AiHandoffResult::Applied);
    EXPECT_EQ(s.prepared, 1U);
    EXPECT_EQ(h.local_session(), "local");
    EXPECT_TRUE(f.commands.empty());
}
TEST_F(HandoffFixture, EveryTransferFailureReasonPreservesOrFencesAccordingToActualResourceStage) {
    const std::array<const char*, 6> reasons{
        {"transferRejected", "transferTimeout", "transferMediaFailed", "transferUnavailable",
         "transferRevisionChanged", "transferPermissionRevoked"}};
    for (const auto* reason : reasons) {
        SCOPED_TRACE(reason);
        Publish pub;
        Sessions sessions;
        Focus focus;
        AiHumanHandoff original(pub, sessions, focus, c);
        ASSERT_TRUE(original.begin("escalate", "device", "target", 0, 1000));
        ASSERT_EQ(original.receive(ack, 1, 1001), AiHandoffResult::Applied);
        ASSERT_EQ(original.receive(invitation, 2, 1002), AiHandoffResult::Applied);
        const auto failure = replace_field(failed, "transferRejected", reason);
        EXPECT_EQ(original.receive(failure, 3, 1003), AiHandoffResult::Applied);
        EXPECT_EQ(original.state(), AiHandoffState::AiActive);
        EXPECT_EQ(sessions.closed, 1U);
        EXPECT_EQ(sessions.fenced, 0U);
        EXPECT_TRUE(focus.commands.empty());
    }
    acquire_actual();
    EXPECT_EQ(h.receive(replace_field(failed, "transferRejected", "transferMediaFailed"), 7, 1007),
              AiHandoffResult::Applied);
    EXPECT_EQ(h.state(), AiHandoffState::Unknown);
    EXPECT_FALSE(h.ai_forwarding_allowed());
    EXPECT_EQ(s.fenced, 0U);
}
TEST_F(HandoffFixture, CancelPublishFailureCannotClaimRestoredOrRepeatControl) {
    bind();
    p.ok = false;
    EXPECT_FALSE(h.cancel("cancel", 3, 1003));
    EXPECT_EQ(h.state(), AiHandoffState::Unknown);
    EXPECT_FALSE(h.cancel("cancel-again", 4, 1004));
    EXPECT_TRUE(f.commands.empty());
    EXPECT_EQ(p.sent.back().method, "callControl");
    EXPECT_EQ(s.fenced, 0U);
}
TEST_F(HandoffFixture, OriginalAiEndingForNonTransferReasonClosesOnlyCandidate) {
    bind();
    auto closed = ended;
    closed.event_sequence = 101;
    closed = replace_field(closed, "transferred", "hangup");
    EXPECT_EQ(h.receive(closed, 3, 1003), AiHandoffResult::Applied);
    EXPECT_EQ(h.state(), AiHandoffState::Closed);
    EXPECT_EQ(s.closed, 1U);
    EXPECT_EQ(s.fenced, 0U);
    EXPECT_EQ(s.released, 0U);
    EXPECT_EQ(h.receive(invitation, 4, 1004), AiHandoffResult::Ignored);
}
TEST_F(HandoffFixture, InvalidNativeConfigurationAndIntentNeverPublishesOrPrepares) {
    // 独立原配置样本验证各前置边界，不用一次早返回覆盖后续样本。
    constexpr std::uint64_t safe = 9007199254740991ULL;
    for (unsigned sample = 0; sample < 11; ++sample) {
        SCOPED_TRACE(sample);
        auto invalid = c;
        std::string token = "escalate", kind = "device", target = "target";
        switch (sample) {
            case 0:
                invalid.topic.clear();
                break;
            case 1:
                invalid.ai_session.clear();
                break;
            case 2:
                invalid.ai_focus.clear();
                break;
            case 3:
                invalid.human_focus = std::string(129, 'x');
                break;
            case 4:
                invalid.call_revision = safe - 1;
                break;
            case 5:
                invalid.resource_revision = safe - 2;
                break;
            case 6:
                token.clear();
                break;
            case 7:
                target.clear();
                break;
            case 8:
                kind = "unknown";
                break;
            case 9:
                invalid.deadline = 0;
                break;
            case 10:
                invalid.expires_at = 1000;
                break;
        }
        Publish pub;
        Sessions sessions;
        Focus focus;
        AiHumanHandoff original(pub, sessions, focus, invalid);
        EXPECT_FALSE(original.begin(token, kind, target, 0, 1000));
        EXPECT_TRUE(pub.sent.empty());
        EXPECT_TRUE(focus.commands.empty());
        EXPECT_EQ(sessions.prepared, 0U);
        EXPECT_EQ(sessions.fenced, 0U);
    }
}
TEST_F(HandoffFixture, EveryCancelAckScopeFieldPreservesCandidateUntilOriginalConfirmation) {
    const auto confirmation = event(
        R"({"msgToken":"cancel","method":"callState","sessionId":"ai","code":200,"data":{"state":"active","revision":2,"candidateSessionId":"remote","localSessionId":"local","callGroupId":"group","reason":"candidateCancelled"}})");
    const std::vector<std::pair<std::string, std::string>> mutations{
        {"\"state\":\"active\"", "\"state\":\"failed\""},
        {"\"revision\":2", "\"revision\":3"},
        {"\"candidateSessionId\":\"remote\"", "\"candidateSessionId\":\"wrong\""},
        {"\"localSessionId\":\"local\"", "\"localSessionId\":\"wrong\""},
        {"\"callGroupId\":\"group\"", "\"callGroupId\":\"wrong\""},
        {"\"reason\":\"candidateCancelled\"", "\"reason\":\"transferRejected\""},
        {"\"reason\":\"candidateCancelled\"", "\"reason\":\"candidateCancelled\",\"extra\":true"}};
    for (unsigned sample = 0; sample < mutations.size() + 3; ++sample) {
        SCOPED_TRACE(sample);
        Publish pub;
        Sessions sessions;
        Focus focus;
        AiHumanHandoff original(pub, sessions, focus, c);
        ASSERT_TRUE(original.begin("escalate", "device", "target", 0, 1000));
        ASSERT_EQ(original.receive(ack, 1, 1001), AiHandoffResult::Applied);
        ASSERT_EQ(original.receive(invitation, 2, 1002), AiHandoffResult::Applied);
        ASSERT_TRUE(original.cancel("cancel", 3, 1003));
        auto wrong = confirmation;
        if (sample < mutations.size())
            wrong = replace_field(wrong, mutations[sample].first, mutations[sample].second);
        else if (sample == mutations.size())
            wrong.session = "local";
        else if (sample == mutations.size() + 1)
            wrong.code = 403;
        else
            wrong.event_sequence = 101;
        EXPECT_EQ(original.receive(wrong, 4, 1004), AiHandoffResult::Invalid);
        EXPECT_EQ(original.state(), AiHandoffState::Candidate);
        EXPECT_TRUE(focus.commands.empty());
        EXPECT_EQ(sessions.closed, 0U);
        EXPECT_EQ(original.receive(confirmation, 5, 1005), AiHandoffResult::Applied);
        EXPECT_EQ(original.state(), AiHandoffState::AiActive);
        EXPECT_EQ(sessions.closed, 1U);
        EXPECT_EQ(sessions.fenced, 0U);
    }
}
TEST_F(HandoffFixture, PendingFocusAndUnknownMethodsNeverCreateSecondOperation) {
    bind();
    ASSERT_EQ(h.receive(suspend, 3, 1003), AiHandoffResult::Applied);
    auto another = suspend;
    another.token = "another-suspend";
    another.event_sequence = 102;
    EXPECT_EQ(h.receive(another, 4, 1004), AiHandoffResult::Rejected);
    auto unknown = invitation;
    unknown.method = "unknownMethod";
    EXPECT_EQ(h.receive(unknown, 5, 1005), AiHandoffResult::Ignored);
    EXPECT_EQ(f.commands.size(), 1U);
    EXPECT_EQ(s.fenced, 0U);
    EXPECT_EQ(s.released, 0U);
}
}  // namespace
}  // namespace ur::device
