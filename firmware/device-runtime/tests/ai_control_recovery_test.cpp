// 文件说明：AI控制恢复使用虚拟双时钟验证固定窗口、原会话与来源代次，不替代实际MQTT/HAL联调。
#include "ur/device/ai_control_recovery.hpp"
#include <gtest/gtest.h>

namespace ur::device {
namespace {
// sync_ready 推进精确能力关联，不跳过已定义能力ACK步骤。
Envelope sync_ready(AIControlRecovery& recovery, std::uint64_t now = 100) {
    EXPECT_EQ(recovery.begin(now, 7, 5, true), Result::Applied);
    EXPECT_EQ(recovery.reporting("current-cap", 3, now + 1), Result::Applied);
    EXPECT_EQ(recovery.capability_ack("current-cap", 3, now + 2), Result::Applied);
    Envelope request;
    EXPECT_EQ(recovery.sync("current-sync", now + 3, request), Result::Applied);
    return request;
}
// control_snapshot 仅构造经严格wire解析后的类型化完整事实，状态单测另核关联与截止。
AIControlSnapshot control_snapshot() {
    AIControlSnapshot snapshot;
    snapshot.envelope.token = "current-sync";
    snapshot.envelope.session = "original-ai";
    snapshot.envelope.method = "sessionSnapshot";
    snapshot.envelope.request = false;
    snapshot.envelope.code = 200;
    snapshot.envelope.event_sequence = 9;
    snapshot.binding = {2, 3};
    snapshot.revision = 8;
    snapshot.event_watermark = 9;
    snapshot.message_watermark = 6;
    snapshot.media_revision = 1;
    snapshot.expires_at = 20000;
    snapshot.input_id = "retired-input";
    snapshot.resume_allowed = true;
    return snapshot;
}
TEST(AIControlRecovery, FreshSourceRequiresCurrentCapabilityAndOriginalSessionSync) {
    AIControlRecovery recovery("original-ai", {1, 1}, 1, VoiceTransport::Udp);
    EXPECT_EQ(recovery.phase(), AIControlPhase::Bound);
    const auto request = sync_ready(recovery);
    EXPECT_EQ(request.session, "original-ai");
    EXPECT_EQ(request.method, "sessionSync");
    EXPECT_EQ(request.payload,
              R"({"endpointEpoch":1,"lastEventSeq":7,"lastMessageSeq":5,"mediaRevision":1})");
    EXPECT_EQ(recovery.accept(control_snapshot(), 104, 19000), Result::Applied);
    EXPECT_EQ(recovery.phase(), AIControlPhase::Bound);
    EXPECT_EQ(recovery.binding().endpoint_epoch, 2U);
    EXPECT_EQ(recovery.binding().capability_revision, 3U);
    EXPECT_TRUE(recovery.wants_capture());
    EXPECT_EQ(recovery.accept(control_snapshot(), 105, 19000), Result::Stale);
}
TEST(AIControlRecovery, UserStopDuringDisconnectCannotBeResurrected) {
    AIControlRecovery recovery("original-ai", {1, 1}, 1, VoiceTransport::Udp);
    sync_ready(recovery);
    recovery.stop_capture();
    EXPECT_EQ(recovery.accept(control_snapshot(), 104, 19000), Result::Applied);
    EXPECT_FALSE(recovery.wants_capture());
}
TEST(AIControlRecovery, OldAckOrChangedBindingCannotCompleteRecovery) {
    AIControlRecovery recovery("original-ai", {1, 1}, 1, VoiceTransport::Udp);
    ASSERT_EQ(recovery.begin(100, 7, 5, false), Result::Applied);
    EXPECT_EQ(recovery.capability_ack("not-issued", 3, 101), Result::Stale);
    EXPECT_EQ(recovery.reporting("current-cap", 3, 102), Result::Applied);
    EXPECT_EQ(recovery.capability_ack("old-cap", 3, 103), Result::Stale);
    EXPECT_EQ(recovery.capability_ack("current-cap", 2, 104), Result::Stale);
    EXPECT_EQ(recovery.capability_ack("current-cap", 3, 105), Result::Applied);
    Envelope request;
    EXPECT_EQ(recovery.sync("current-sync", 106, request), Result::Applied);
    for (unsigned field = 0; field < 10; ++field) {
        auto snapshot = control_snapshot();
        if (field == 0)
            snapshot.envelope.token = "old-sync";
        if (field == 1)
            snapshot.envelope.session = "another-ai";
        if (field == 2)
            snapshot.binding.endpoint_epoch = 1;
        if (field == 3)
            snapshot.binding.endpoint_epoch = 3;
        if (field == 4)
            snapshot.binding.capability_revision = 2;
        if (field == 5)
            snapshot.media_revision = 2;
        if (field == 6)
            snapshot.event_watermark = 6;
        if (field == 7)
            snapshot.message_watermark = 4;
        if (field == 8)
            snapshot.input_active = true;
        if (field == 9)
            snapshot.resume_allowed = false;
        EXPECT_EQ(recovery.accept(snapshot, 107, 19000), Result::Stale);
        EXPECT_EQ(recovery.phase(), AIControlPhase::Syncing);
    }
}
TEST(AIControlRecovery, DuplicateReconnectCannotExtendFirstFifteenSeconds) {
    AIControlRecovery recovery("original-ai", {1, 1}, 1, VoiceTransport::Udp);
    sync_ready(recovery);
    EXPECT_EQ(recovery.begin(14999, 8, 8, true), Result::Duplicate);
    EXPECT_EQ(recovery.reporting("new-physical-cap", 4, 15099), Result::Applied);
    EXPECT_TRUE(recovery.expired(15100));
    EXPECT_EQ(recovery.phase(), AIControlPhase::Failed);
    EXPECT_EQ(recovery.capability_ack("new-physical-cap", 4, 15101), Result::Stale);
    EXPECT_EQ(recovery.begin(16000, 0, 0, true), Result::Stale);
}
TEST(AIControlRecovery, WallDeadlineAndMonotonicRollbackFailClosed) {
    AIControlRecovery recovery("original-ai", {1, 1}, 1, VoiceTransport::Udp);
    sync_ready(recovery);
    EXPECT_EQ(recovery.accept(control_snapshot(), 104, 20000), Result::Stale);
    EXPECT_EQ(recovery.accept(control_snapshot(), 103, 19000), Result::Stale);
    EXPECT_EQ(recovery.phase(), AIControlPhase::Failed);
}
TEST(AIControlRecovery, MissingNegotiationNeverInventsAnInitialBinding) {
    for (const auto binding : {AIControlBinding{}, AIControlBinding{1, 0}, AIControlBinding{0, 1},
                               AIControlBinding{9007199254740992ULL, 1}}) {
        AIControlRecovery recovery("original-ai", binding, 1, VoiceTransport::Udp);
        EXPECT_EQ(recovery.phase(), AIControlPhase::Failed);
        EXPECT_EQ(recovery.begin(100, 0, 0, true), Result::Stale);
    }
    AIControlRecovery none("original-ai", {1, 1}, 0, VoiceTransport::None);
    EXPECT_EQ(none.phase(), AIControlPhase::Bound);
    EXPECT_EQ(none.begin(100, 0, 0, false), Result::Applied);
    AIControlRecovery invalid_audio("original-ai", {1, 1}, 0, VoiceTransport::Udp);
    EXPECT_EQ(invalid_audio.phase(), AIControlPhase::Failed);
}

TEST(AIControlRecovery, RetiredRepliesPersistAcrossRecoveryAndNeverEvictToReplay) {
    AIControlRecovery recovery("original-ai", {1, 1}, 1, VoiceTransport::Udp);
    sync_ready(recovery);
    auto snapshot = control_snapshot();
    for (unsigned index = 0; index < 512; ++index)
        snapshot.active_responses.push_back("retired-" + std::to_string(index));
    ASSERT_EQ(recovery.accept(snapshot, 104, 19000), Result::Applied);
    EXPECT_TRUE(recovery.retired_response("retired-0"));
    EXPECT_TRUE(recovery.retired_response("retired-511"));
    EXPECT_FALSE(recovery.retired_response("new-reply"));
    ASSERT_EQ(recovery.begin(200, 9, 6, true), Result::Applied);
    ASSERT_EQ(recovery.reporting("second-cap", 4, 201), Result::Applied);
    ASSERT_EQ(recovery.capability_ack("second-cap", 4, 202), Result::Applied);
    Envelope request;
    ASSERT_EQ(recovery.sync("second-sync", 203, request), Result::Applied);
    snapshot.envelope.token = "second-sync";
    snapshot.binding = {3, 4};
    snapshot.active_responses = {"retired-0", "new-retired-513"};
    EXPECT_EQ(recovery.accept(snapshot, 204, 19000), Result::Busy);
    EXPECT_EQ(recovery.phase(), AIControlPhase::Failed);
    EXPECT_FALSE(recovery.wants_capture());
    EXPECT_EQ(recovery.binding().endpoint_epoch, 2U);
    EXPECT_TRUE(recovery.retired_response("retired-0"));
    EXPECT_FALSE(recovery.retired_response("new-retired-513"));
}

TEST(AIControlRecovery, PriorIceRevisionIsPreservedButControlCannotRenegotiateMedia) {
    AIControlRecovery recovery("original-ai", {1, 1}, 1, VoiceTransport::WebRTC);
    EXPECT_EQ(recovery.advance_media_revision(1), Result::Duplicate);
    EXPECT_EQ(recovery.advance_media_revision(3), Result::Invalid);
    EXPECT_EQ(recovery.advance_media_revision(2), Result::Applied);
    const auto request = sync_ready(recovery);
    EXPECT_NE(request.payload.find("\"mediaRevision\":2"), std::string::npos);
    EXPECT_EQ(recovery.advance_media_revision(3), Result::Stale);
    auto snapshot = control_snapshot();
    EXPECT_EQ(recovery.accept(snapshot, 104, 19000), Result::Stale);
    snapshot.media_revision = 2;
    EXPECT_EQ(recovery.accept(snapshot, 105, 19000), Result::Applied);
    EXPECT_EQ(recovery.advance_media_revision(3), Result::Applied);
    AIControlRecovery udp("original-ai", {1, 1}, 1, VoiceTransport::Udp);
    EXPECT_EQ(udp.advance_media_revision(2), Result::Stale);
}

TEST(AIControlRecovery, ConfirmedTerminalSnapshotCannotResurrectCaptureOrSession) {
    AIControlRecovery recovery("original-ai", {1, 1}, 1, VoiceTransport::Udp);
    sync_ready(recovery);
    auto snapshot = control_snapshot();
    snapshot.ended = true;
    snapshot.resume_allowed = false;
    EXPECT_EQ(recovery.accept(snapshot, 104, 19000), Result::Unsupported);
    EXPECT_EQ(recovery.phase(), AIControlPhase::Failed);
    EXPECT_FALSE(recovery.wants_capture());
    EXPECT_EQ(recovery.begin(105, 9, 6, true), Result::Stale);
    EXPECT_EQ(recovery.accept(control_snapshot(), 106, 19000), Result::Stale);
}
}  // namespace
}  // namespace ur::device
