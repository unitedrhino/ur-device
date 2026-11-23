// 文件说明：完整IPC同步契约的纯C++回归；合成原绑定不表示网络恢复、媒体或MAC许可。
#include <cJSON.h>
#include <gtest/gtest.h>
#include <functional>
#include <memory>
#include "ur/device/ipc_wire.hpp"

namespace ur::device {
namespace {
// sync_capability 声明实际解析能力，默认生产宿主仍须显式启用。
IPCCapability sync_capability() {
    IPCCapability result;
    result.revision = 3;
    result.microphone = result.conference_talk = true;
    result.participant_roster = result.participant_roster_sync = true;
    return result;
}
// bound_invite 保存平台已经明确给出的原端腿绑定，不从连接时间戳推算。
IPCInvite bound_invite() {
    IPCInvite invite;
    invite.envelope.session = "original-leg";
    invite.audio_track = "mic-1";
    invite.speaker_track = "speaker-1";
    invite.endpoint_epoch = 7;
    invite.capability_revision = 3;
    return invite;
}
// snapshot_body
// 生成与Go严格子集相同的完整原轨及控制快照，便于逐项非法输入测试。
std::string snapshot_body() {
    const std::string tracks =
        R"([{"trackId":"mic-1","kind":"audio","sourceId":"mic","direction":"sendonly","codec":"opus","state":"active"},{"trackId":"speaker-1","kind":"audio","sourceId":"speaker","direction":"recvonly","codec":"opus","state":"paused"}])";
    Envelope envelope;
    envelope.token = "original-sync";
    envelope.session = "original-leg";
    envelope.method = "sessionSnapshot";
    envelope.request = false;
    envelope.code = 200;
    envelope.event_sequence = 8;
    envelope.payload =
        R"({"state":"established","revision":7,"eventWatermark":8,"messageWatermark":0,"endpointEpoch":7,"mediaRevision":1,"tracks":)" +
        tracks +
        R"(,"controls":{"call":null,"focus":null,"floor":null,"recordings":[],"playbacks":[],"processors":[],"broadcasts":[],"handoffs":[],"mediaPlan":{"revision":1,"tracks":)" +
        tracks +
        R"(},"ai":null},"capabilityRevision":3,"resumeAllowed":false,"reason":"snapshotOnly","participants":[{"participantId":"101","tracks":[]}],"expiresAt":1001000})";
    std::string body;
    EXPECT_EQ(encode_envelope(envelope, body), DecodeResult::Ok);
    return body;
}
// mutate 在完整夹具中只改一个字段，不使用真实身份或保存自由正文。
std::string mutate(const std::function<void(cJSON*)>& change) {
    const auto body = snapshot_body();
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_Parse(body.c_str()), cJSON_Delete);
    change(root.get());
    char* raw = cJSON_PrintUnformatted(root.get());
    const std::string result = raw ? raw : "";
    cJSON_free(raw);
    return result;
}

TEST(IPCRosterSnapshot, CompleteOriginalBindingAndExplicitEmptyAreValid) {
    IPCRosterSnapshot result;
    ASSERT_EQ(decode_ipc_roster_snapshot(WireRoute::Signaling, snapshot_body(), bound_invite(),
                                         sync_capability(), result),
              WireResult::Ok);
    EXPECT_EQ(result.event_watermark, 8U);
    EXPECT_EQ(result.track_states.at("speaker-1"), "paused");
    EXPECT_EQ(result.participants, R"([{"participantId":"101","tracks":[]}])");
    auto empty = mutate([](cJSON* root) {
        cJSON_ReplaceItemInObjectCaseSensitive(cJSON_GetObjectItemCaseSensitive(root, "data"),
                                               "participants", cJSON_CreateArray());
    });
    ASSERT_EQ(decode_ipc_roster_snapshot(WireRoute::Signaling, empty, bound_invite(),
                                         sync_capability(), result),
              WireResult::Ok);
    EXPECT_EQ(result.participants, "[]");
}

TEST(IPCRosterSnapshot, MissingAnyCompleteFieldOrControlIsRejectedAndOutputCleared) {
    for (const auto* key :
         {"state", "revision", "eventWatermark", "messageWatermark", "endpointEpoch",
          "mediaRevision", "tracks", "controls", "capabilityRevision", "resumeAllowed", "reason",
          "participants", "expiresAt"}) {
        IPCRosterSnapshot result;
        result.participants = "old";
        const auto body = mutate([&](cJSON* root) {
            cJSON_DeleteItemFromObjectCaseSensitive(cJSON_GetObjectItemCaseSensitive(root, "data"),
                                                    key);
        });
        EXPECT_EQ(decode_ipc_roster_snapshot(WireRoute::Signaling, body, bound_invite(),
                                             sync_capability(), result),
                  WireResult::Invalid)
            << key;
        EXPECT_TRUE(result.participants.empty());
    }
    for (const auto* key : {"call", "focus", "floor", "recordings", "playbacks", "processors",
                            "broadcasts", "handoffs", "mediaPlan", "ai"}) {
        const auto body = mutate([&](cJSON* root) {
            auto* data = cJSON_GetObjectItemCaseSensitive(root, "data");
            cJSON_DeleteItemFromObjectCaseSensitive(
                cJSON_GetObjectItemCaseSensitive(data, "controls"), key);
        });
        IPCRosterSnapshot result;
        EXPECT_EQ(decode_ipc_roster_snapshot(WireRoute::Signaling, body, bound_invite(),
                                             sync_capability(), result),
                  WireResult::Invalid)
            << key;
    }
}

TEST(IPCRosterSnapshot, WrongBindingWatermarkAndExtraAuthorityAreRejected) {
    for (const auto* key : {"endpointEpoch", "capabilityRevision", "mediaRevision",
                            "eventWatermark", "messageWatermark"}) {
        const auto body = mutate([&](cJSON* root) {
            auto* data = cJSON_GetObjectItemCaseSensitive(root, "data");
            cJSON_ReplaceItemInObjectCaseSensitive(data, key, cJSON_CreateNumber(99));
        });
        IPCRosterSnapshot result;
        EXPECT_EQ(decode_ipc_roster_snapshot(WireRoute::Signaling, body, bound_invite(),
                                             sync_capability(), result),
                  WireResult::Invalid)
            << key;
    }
    for (const auto* key : {"call", "focus", "floor", "ai"}) {
        const auto body = mutate([&](cJSON* root) {
            auto* controls = cJSON_GetObjectItemCaseSensitive(
                cJSON_GetObjectItemCaseSensitive(root, "data"), "controls");
            cJSON_ReplaceItemInObjectCaseSensitive(controls, key, cJSON_CreateObject());
        });
        IPCRosterSnapshot result;
        EXPECT_EQ(decode_ipc_roster_snapshot(WireRoute::Signaling, body, bound_invite(),
                                             sync_capability(), result),
                  WireResult::Invalid)
            << key;
    }
}

TEST(IPCRosterSnapshot, ExplicitCapabilityRequiredAndWrongRouteNeverProjects) {
    auto capability = sync_capability();
    capability.participant_roster_sync = false;
    IPCRosterSnapshot result;
    EXPECT_EQ(decode_ipc_roster_snapshot(WireRoute::Signaling, snapshot_body(), bound_invite(),
                                         capability, result),
              WireResult::Invalid);
    EXPECT_EQ(decode_ipc_roster_snapshot(WireRoute::Message, snapshot_body(), bound_invite(),
                                         sync_capability(), result),
              WireResult::WrongRoute);
    capability.participant_roster = false;
    capability.participant_roster_sync = true;
    Envelope command;
    EXPECT_EQ(ipc_capability_command("cap", capability, command), WireResult::Invalid);
    EXPECT_EQ(ipc_capability_command("cap", sync_capability(), command), WireResult::Ok);
    EXPECT_NE(command.payload.find("participantRosterSync"), std::string::npos);
}

// SnapshotOnly不授予节目、成员轨或新媒体；任何附带许可均须整体拒绝。
TEST(IPCRosterSnapshot, NonemptyControlsAndParticipantTracksCannotGrantAuthority) {
    for (const auto* key : {"recordings", "playbacks", "processors", "broadcasts", "handoffs"}) {
        for (const bool nonempty : {false, true}) {
            const auto body = mutate([&](cJSON* root) {
                auto* controls = cJSON_GetObjectItemCaseSensitive(
                    cJSON_GetObjectItemCaseSensitive(root, "data"), "controls");
                auto* value = nonempty ? cJSON_CreateArray() : cJSON_CreateNull();
                if (nonempty)
                    cJSON_AddItemToArray(value, cJSON_CreateObject());
                cJSON_ReplaceItemInObjectCaseSensitive(controls, key, value);
            });
            IPCRosterSnapshot result;
            result.participants = "previous";
            EXPECT_EQ(decode_ipc_roster_snapshot(WireRoute::Signaling, body, bound_invite(),
                                                 sync_capability(), result),
                      WireResult::Invalid)
                << key << "/" << nonempty;
            EXPECT_TRUE(result.participants.empty());
            EXPECT_TRUE(result.track_states.empty());
        }
    }
    for (const bool duplicate : {false, true}) {
        const auto body = mutate([&](cJSON* root) {
            auto* members = cJSON_GetObjectItemCaseSensitive(
                cJSON_GetObjectItemCaseSensitive(root, "data"), "participants");
            if (duplicate)
                cJSON_AddItemToArray(members,
                                     cJSON_Duplicate(cJSON_GetArrayItem(members, 0), true));
            else
                cJSON_AddItemToArray(
                    cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(members, 0), "tracks"),
                    cJSON_CreateObject());
        });
        IPCRosterSnapshot result;
        EXPECT_EQ(decode_ipc_roster_snapshot(WireRoute::Signaling, body, bound_invite(),
                                             sync_capability(), result),
                  WireResult::Invalid);
        EXPECT_TRUE(result.participants.empty());
    }
}

// 原邀请和完整媒体计划必须逐字段一致，不能借展示快照替换源、方向或实际状态。
TEST(IPCRosterSnapshot, OriginalTracksAndMediaPlanMustRemainConsistent) {
    for (const bool plan_only : {false, true}) {
        for (const auto* key : {"trackId", "kind", "sourceId", "direction", "codec", "state"}) {
            const auto body = mutate([&](cJSON* root) {
                auto* data = cJSON_GetObjectItemCaseSensitive(root, "data");
                if (plan_only)
                    data = cJSON_GetObjectItemCaseSensitive(
                        cJSON_GetObjectItemCaseSensitive(data, "controls"), "mediaPlan");
                auto* track =
                    cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(data, "tracks"), 0);
                cJSON_ReplaceItemInObjectCaseSensitive(
                    track, key,
                    cJSON_CreateString(std::string_view(key) == "state" ? "paused" : "different"));
            });
            IPCRosterSnapshot result;
            EXPECT_EQ(decode_ipc_roster_snapshot(WireRoute::Signaling, body, bound_invite(),
                                                 sync_capability(), result),
                      WireResult::Invalid)
                << key << "/" << plan_only;
            EXPECT_TRUE(result.track_states.empty());
        }
    }
}
}  // namespace
}  // namespace ur::device
