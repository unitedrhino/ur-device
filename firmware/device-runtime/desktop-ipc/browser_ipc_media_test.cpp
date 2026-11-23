// 文件说明：本地IPC驱动契约测试覆盖独立Peer、权限意图、乱序事实和背压，不冒充媒体解码E2E。
#include "browser_ipc_media.hpp"
#include <gtest/gtest.h>
#include <vector>

namespace ur::device {
namespace {
// 自然连通只属于原本地租约，字段严格固定；不能携带媒体代次、身份或控制授权。
TEST(BrowserIPCMedia, NaturalConnectivityStrictLocalReceipt) {
    IPCBrowserRequest request;
    for (const auto* kind : {"ipcConnectivity", "callConnectivity"}) {
        const std::string prefix = std::string("{\"kind\":\"") + kind + "\",\"sessionId\":\"leg\",\"releaseId\":\"1\"";
        for (const auto* state : {"connected", "disconnected"}) {
            ASSERT_TRUE(decode_ipc_browser(prefix + ",\"state\":\"" + state + "\"}", request));
            EXPECT_EQ(request.token, "1");
            EXPECT_EQ(request.state, state);
        }
        for (const auto* suffix : {"}", ",\"state\":\"failed\"}", ",\"state\":true}",
                                   ",\"state\":\"connected\",\"mediaRevision\":2}"}) {
            EXPECT_FALSE(decode_ipc_browser(prefix + suffix, request));
            EXPECT_TRUE(request.kind.empty());
        }
    }
    EXPECT_FALSE(decode_ipc_browser(R"({"kind":"ipcConnectivity","sessionId":"leg","releaseId":"0","state":"connected"})", request));
}
// 质量桥只接受原轨/档位意图或真实完成事实，不能注入平台token、修订或设备身份。
TEST(BrowserIPCMedia, QualityStrictLocalIntentAndCompletion) {
    IPCBrowserRequest request;
    ASSERT_TRUE(decode_ipc_browser(R"({"kind":"ipcSetQuality","sessionId":"leg","trackId":"video-1","profile":"low"})", request));
    EXPECT_EQ(request.track, "video-1");
    EXPECT_EQ(request.profile, "low");
    ASSERT_TRUE(decode_ipc_browser(R"({"kind":"ipcQualityApplied","sessionId":"leg","token":"original","state":"paused","success":true})", request));
    EXPECT_TRUE(request.success);
    for (const auto* body : {
             R"({"kind":"ipcSetQuality","sessionId":"leg","trackId":"video-1","profile":"low","token":"injected"})",
             R"({"kind":"ipcSetQuality","sessionId":"leg","trackId":"video-1","profile":"invalid"})",
             R"({"kind":"ipcQualityApplied","sessionId":"leg","token":"original","state":"active","success":false})"})
        EXPECT_FALSE(decode_ipc_browser(body, request));
}
// 质量端口只发送固定原轨指令；桥入队不能伪装驱动完成或允许控制接收扬声器。
TEST(BrowserIPCMedia, QualityRequiresConnectedOriginalSender) {
    BrowserIPCMediaFactory factory;
    auto peer = factory.create();
    IPCInvite invite;
    invite.envelope.session = "leg";
    invite.video_track = "video-1";
    invite.audio_track = "mic-1";
    ASSERT_TRUE(peer->begin(invite));
    ASSERT_TRUE(factory.take_command());
    const IPCQualityCommand quality{"leg", "original", "video-1", "low", 1};
    EXPECT_FALSE(peer->apply_quality(quality));
    ASSERT_TRUE(peer->apply_answer("v=0\r\nanswer"));
    ASSERT_TRUE(factory.take_command());
    ASSERT_TRUE(factory.report_connected("leg"));
    ASSERT_TRUE(peer->apply_quality(quality));
    ASSERT_EQ(factory.take_command(), R"({"kind":"ipcQuality","sessionId":"leg","token":"original","trackId":"video-1","profile":"low"})");
    EXPECT_FALSE(factory.take_command());
    auto invalid = quality;
    invalid.track = "other-track";
    EXPECT_FALSE(peer->apply_quality(invalid));
    invalid = quality;
    invalid.session = "other-leg";
    EXPECT_FALSE(peer->apply_quality(invalid));
    peer->close();
    EXPECT_FALSE(peer->apply_quality(quality));
}
// 查询入口只接受本腿意图，不能让页面提交平台token、水位或自由协议报文。
TEST(BrowserIPCMedia, SyncStrictLocalIntentAndCapabilityConfiguration) {
    IPCBrowserRequest request;
    ASSERT_TRUE(decode_ipc_browser(R"({"kind":"ipcSync","sessionId":"leg"})", request));
    EXPECT_EQ(request.kind, "ipcSync");
    EXPECT_EQ(request.session, "leg");
    for (const auto* body : {R"({"kind":"ipcSync"})", R"({"kind":"ipcSync","sessionId":""})",
                             R"({"kind":"ipcSync","sessionId":"leg","token":"injected"})",
                             R"({"kind":"ipcSync","sessionId":"leg","eventSeq":1})",
                             R"({"kind":"ipcSync","sessionId":"leg","sessionId":"other"})",
                             R"({"kind":"ipcSync","sessionId":"leg\u0000hidden"})"}) {
        EXPECT_FALSE(decode_ipc_browser(body, request));
        EXPECT_TRUE(request.session.empty());
    }
    for (const bool sync : {false, true}) {
        for (const bool roster : {false, true}) {
            for (const bool talk : {false, true}) {
                EXPECT_EQ(ipc_browser_sync_configuration(sync, roster, talk),
                          sync && roster && talk);
            }
        }
    }
}
// SyncPublish 记录共享核心实际生成的协议，不生成虚构网络确认或真实用户身份。
struct SyncPublish final : PublishPort {
    std::vector<Envelope> sent;  // 原token可仅在测试内验证，不进入宿主日志。
    bool accepted = true;        // 发送失败仍可留下核心在途意图，不冒充确定未执行。
    bool publish(std::string_view, std::string_view body) override {
        Envelope envelope;
        if (decode_envelope(body, envelope) != DecodeResult::Ok)
            return false;
        sent.push_back(std::move(envelope));
        return accepted;
    }
};
// sync_reply 将合成平台回执经正式编码器返回原请求，不能把测试受理当真实端云连通。
std::string sync_reply(const Envelope& request, const char* method, const char* payload) {
    Envelope reply;
    reply.token = request.token;
    reply.session = request.session;
    reply.method = method;
    reply.request = false;
    reply.code = 200;
    reply.payload = payload;
    std::string body;
    EXPECT_EQ(encode_envelope(reply, body), DecodeResult::Ok);
    return body;
}
// 原会议腿建立后查询复用核心状态；没有新工厂指令、Peer或本地媒体执行。
TEST(BrowserIPCMedia, SyncPendingDuplicateAndTerminalProjectionDoNotDriveMedia) {
    SyncPublish mqtt;
    BrowserIPCMediaFactory factory(true);
    IPCCapability capability;
    capability.microphone = capability.conference_talk = true;
    capability.participant_roster = capability.participant_roster_sync = true;
    IPCClient client(mqtt, factory, "up", "down", "local", capability);
    IPCBrowserRequest request;
    ASSERT_TRUE(decode_ipc_browser(R"({"kind":"ipcSync","sessionId":"leg"})", request));
    EXPECT_NE(ipc_browser_sync(client, request, true, 0, 1000000).find("unavailable"),
              std::string::npos);
    EXPECT_TRUE(mqtt.sent.empty());
    // 邀请是下行请求而非事件答复；正式编码器保留params与原共享水位，授权不超过30秒。
    Envelope invite;
    invite.token = "invite";
    invite.method = "sessionInvite";
    invite.session = "leg";
    invite.event_sequence = 1;
    invite.payload =
        R"({"purpose":"ipcTalk","admission":"direct","transport":"webrtc","expiresAt":1020000,"cameraId":"front","tracks":[{"trackId":"mic-1","kind":"audio","sourceId":"mic","direction":"sendonly","codec":"opus","state":"requested"},{"trackId":"speaker-1","kind":"audio","sourceId":"speaker","direction":"recvonly","codec":"opus","state":"requested"}],"endpointEpoch":1,"capabilityRevision":1})";
    std::string invite_body;
    ASSERT_EQ(encode_envelope(invite, invite_body), DecodeResult::Ok);
    ASSERT_TRUE(client.receive("down", invite_body, 1, 1000000));
    ASSERT_TRUE(client.receive(
        "down",
        sync_reply(mqtt.sent.back(), "requestAck",
                   R"({"requestMethod":"sessionAccept","result":{"state":"negotiating"}})"),
        2, 1000000));
    ASSERT_TRUE(factory.take_command());
    EXPECT_NE(ipc_browser_sync(client, request, true, 3, 1000000).find("unavailable"),
              std::string::npos);
    ASSERT_TRUE(client.offer_ready("leg", "v=0\r\npublic-offer", 4));
    ASSERT_TRUE(client.receive(
        "down",
        sync_reply(
            mqtt.sent.back(), "mediaAnswer",
            R"({"mediaRevision":1,"webrtc":{"iceMode":"complete","sdp":"v=0\r\npublic-answer"}})"),
        5, 1000000));
    ASSERT_TRUE(factory.take_command());
    ASSERT_TRUE(factory.report_connected("leg"));
    ASSERT_TRUE(client.media_connected("leg", 6));
    ASSERT_TRUE(client.receive(
        "down",
        sync_reply(
            mqtt.sent.back(), "requestAck",
            R"({"requestMethod":"mediaState","result":{"state":"connected","mediaRevision":1}})"),
        7, 1000000));
    ASSERT_TRUE(client.receive(
        "down",
        R"({"msgToken":"established","method":"sessionEstablished","sessionId":"leg","eventSeq":2,"data":{"state":"established","mediaRevision":1}})",
        8, 1000000));
    ASSERT_EQ(client.phase("leg"), IPCPhase::Active);
    const auto before = mqtt.sent.size();
    EXPECT_NE(ipc_browser_sync(client, request, false, 9, 1000000).find("unavailable"),
              std::string::npos);
    EXPECT_EQ(mqtt.sent.size(), before);
    mqtt.accepted = false;
    EXPECT_NE(ipc_browser_sync(client, request, true, 10, 1000000).find("pending"),
              std::string::npos);
    mqtt.accepted = true;
    ASSERT_EQ(mqtt.sent.size(), before + 1);
    const auto original = mqtt.sent.back();
    EXPECT_EQ(original.method, "sessionSync");
    EXPECT_NE(ipc_browser_sync(client, request, true, 11, 1000000).find("pending"),
              std::string::npos);
    EXPECT_EQ(mqtt.sent.size(), before + 1);
    EXPECT_FALSE(factory.take_command());
    auto failure = sync_reply(original, "sessionSnapshot",
                              R"({"requestMethod":"sessionSync","reason":"requestDenied"})");
    const auto position = failure.find("\"code\":200");
    ASSERT_NE(position, std::string::npos);
    failure.replace(position, 10, "\"code\":403");
    ASSERT_TRUE(client.receive("down", failure, 12, 1000000));
    EXPECT_NE(ipc_browser_sync_state(client, "leg", true).find("failed"), std::string::npos);
    EXPECT_EQ(client.phase("leg"), IPCPhase::Active);
    EXPECT_FALSE(factory.take_command());
    EXPECT_NE(ipc_browser_sync(client, request, true, 13, 1000000).find("pending"),
              std::string::npos);
    const auto tracks =
        R"([{"trackId":"mic-1","kind":"audio","sourceId":"mic","direction":"sendonly","codec":"opus","state":"active"},{"trackId":"speaker-1","kind":"audio","sourceId":"speaker","direction":"recvonly","codec":"opus","state":"active"}])";
    Envelope snapshot;
    snapshot.token = mqtt.sent.back().token;
    snapshot.session = "leg";
    snapshot.method = "sessionSnapshot";
    snapshot.request = false;
    snapshot.code = 200;
    snapshot.event_sequence = 3;
    snapshot.payload =
        std::string(
            R"({"state":"established","revision":5,"eventWatermark":3,"messageWatermark":0,"endpointEpoch":1,"mediaRevision":1,"tracks":)") +
        tracks +
        R"(,"controls":{"call":null,"focus":null,"floor":null,"recordings":[],"playbacks":[],"processors":[],"broadcasts":[],"handoffs":[],"mediaPlan":{"revision":1,"tracks":)" +
        tracks +
        R"(},"ai":null},"capabilityRevision":1,"resumeAllowed":false,"reason":"snapshotOnly","participants":[],"expiresAt":1001000})";
    std::string body;
    ASSERT_EQ(encode_envelope(snapshot, body), DecodeResult::Ok);
    ASSERT_TRUE(client.receive("down", body, 14, 1000000));
    EXPECT_NE(ipc_browser_sync_state(client, "leg", true).find("succeeded"), std::string::npos);
    EXPECT_EQ(client.participant_sequence("leg"), 3U);
    ASSERT_NE(client.participants("leg"), nullptr);
    EXPECT_TRUE(client.participants("leg")->empty());
    EXPECT_FALSE(factory.take_command());
    // 上次成功不代表本次受理：原扬声器控制尚待真实驱动回执时，查询不得借旧成功返回。
    ASSERT_TRUE(client.receive(
        "down",
        R"({"msgToken":"speaker-pause","method":"trackControl","sessionId":"leg","eventSeq":4,"params":{"trackId":"speaker-1","operation":"pause","mediaRevision":1}})",
        15, 1000000));
    const auto speaker_control = factory.take_command();
    ASSERT_TRUE(speaker_control);
    EXPECT_NE(speaker_control->find("ipcTrack"), std::string::npos);
    const auto before_rejected = mqtt.sent.size();
    EXPECT_NE(ipc_browser_sync(client, request, true, 16, 1000000).find("rejected"),
              std::string::npos);
    EXPECT_EQ(mqtt.sent.size(), before_rejected);
    EXPECT_EQ(client.phase("leg"), IPCPhase::Active);
    EXPECT_FALSE(factory.take_command());
    // 只读投影仍保留前次成功；拒绝本次意图不篡改核心终态或已安装的名册。
    EXPECT_NE(ipc_browser_sync_state(client, "leg", true).find("succeeded"), std::string::npos);
    ASSERT_TRUE(client.track_applied("leg", "speaker-pause", "paused", true, 17));
    EXPECT_NE(ipc_browser_sync(client, request, true, 18, 1000000).find("pending"),
              std::string::npos);
    const auto before_timeout = mqtt.sent.size();
    // 原30秒待确认截止拒绝继续等待时必须是unknown，不伪装成此次确定未受理。
    EXPECT_NE(ipc_browser_sync(client, request, true, 30018, 1000000).find("unknown"),
              std::string::npos);
    EXPECT_EQ(mqtt.sent.size(), before_timeout);
    EXPECT_EQ(client.phase("leg"), IPCPhase::Active);
    EXPECT_FALSE(factory.take_command());
    client.close_all(30019);
    EXPECT_NE(ipc_browser_sync_state(client, "leg", true).find("unavailable"), std::string::npos);
}
// 已接受名册只编码本地展示消息；不附云端方法、凭据或媒体执行指令。
TEST(BrowserIPCMedia, AcceptedRosterProjectionAndLimits) {
    std::vector<RosterParticipant> members{{"101", {}}};
    EXPECT_EQ(
        ipc_browser_participants("talk", 3, members),
        R"({"kind":"ipcParticipants","sessionId":"talk","eventSeq":3,"participants":[{"participantId":"101","tracks":[]}]})");
    EXPECT_EQ(ipc_browser_participants("talk", 4, {}),
              R"({"kind":"ipcParticipants","sessionId":"talk","eventSeq":4,"participants":[]})");
    EXPECT_TRUE(ipc_browser_participants("", 3, members).empty());
    EXPECT_TRUE(ipc_browser_participants("talk", 0, members).empty());
    EXPECT_TRUE(ipc_browser_participants("talk", 9007199254740992ULL, members).empty());
    members.push_back(members.front());
    EXPECT_TRUE(ipc_browser_participants("talk", 3, members).empty());
    members.pop_back();
    members.front().participant_id = std::string("101\0hidden", 10);
    EXPECT_TRUE(ipc_browser_participants("talk", 3, members).empty());
    members.front().participant_id = "<img>\"";
    EXPECT_NE(ipc_browser_participants("talk", 3, members).find("<img>\\\""), std::string::npos);
    members.front().tracks.push_back({"t", "audio", "mic", "recvonly", "opus", "active"});
    EXPECT_NE(ipc_browser_participants("talk", 3, members).find("\"trackId\":\"t\""),
              std::string::npos);
    members.front().tracks.front().state = "invalid";
    EXPECT_TRUE(ipc_browser_participants("talk", 3, members).empty());
}
// invitation 构造已验证邀请的驱动输入；视频发布不隐式包含音频。
IPCInvite invitation(const std::string& session) {
    IPCInvite invite;
    invite.envelope.session = session;
    invite.video_track = "video-1";
    return invite;
}
// 驱动事实只能影响指定会话，关闭一个观看者不停止其它观看者。
TEST(BrowserIPCMedia, IndependentLifecycle) {
    BrowserIPCMediaFactory factory;
    auto first = factory.create(), second = factory.create();
    ASSERT_TRUE(first->begin(invitation("view-a")));
    ASSERT_TRUE(second->begin(invitation("view-b")));
    EXPECT_NE(factory.take_command()->find("\"audio\":false"), std::string::npos);
    EXPECT_NE(factory.take_command()->find("view-b"), std::string::npos);
    EXPECT_FALSE(factory.report_connected("view-a"));
    EXPECT_FALSE(first->begin(invitation("changed")));
    ASSERT_TRUE(first->apply_answer("v=0\r\nanswer"));
    ASSERT_TRUE(second->apply_answer("v=0\r\nanswer"));
    EXPECT_TRUE(factory.report_connected("view-a"));
    EXPECT_TRUE(factory.report_connected("view-b"));
    EXPECT_FALSE(factory.report_connected("view-a"));
    EXPECT_TRUE(first->connected());
    first.reset();
    EXPECT_FALSE(factory.report_connected("view-a"));
    EXPECT_TRUE(second->connected());
    EXPECT_FALSE(second->apply_answer("v=0\r\nchanged"));
    factory.take_command();
    factory.take_command();
    EXPECT_NE(factory.take_command()->find("ipcStop"), std::string::npos);
    EXPECT_FALSE(factory.take_command());
    second->close();
    second->close();
    EXPECT_FALSE(second->connected());
    EXPECT_TRUE(factory.take_command());
    EXPECT_FALSE(factory.take_command());
}
// 源控制仅排队当前授权轨，未连通、订阅权限及关闭后副作用全部拒绝。
TEST(BrowserIPCMedia, SourceTrackControlAndStrictCallbacks) {
    BrowserIPCMediaFactory factory;
    auto peer = factory.create();
    ASSERT_TRUE(peer->begin(invitation("view")));
    factory.take_command();
    IPCTrackCommand command;
    command.envelope.session = "view";
    command.envelope.token = "source-control";
    command.track_id = "video-1";
    command.operation = "pause";
    command.revision = 1;
    EXPECT_FALSE(peer->control_track(command));
    ASSERT_TRUE(peer->apply_answer("v=0\r\nanswer"));
    factory.take_command();
    ASSERT_TRUE(factory.report_connected("view"));
    ASSERT_TRUE(peer->control_track(command));
    const auto queued = factory.take_command();
    ASSERT_TRUE(queued);
    EXPECT_NE(queued->find("ipcTrack"), std::string::npos);
    EXPECT_NE(queued->find("source-control"), std::string::npos);
    command.track_id = "audio-1";
    EXPECT_FALSE(peer->control_track(command));
    command.track_id = "video-1";
    command.operation = "subscribe";
    EXPECT_FALSE(peer->control_track(command));
    command.operation = "resume";
    peer->close();
    EXPECT_FALSE(peer->control_track(command));
    IPCBrowserRequest request;
    ASSERT_TRUE(decode_ipc_browser(
        R"({"kind":"ipcTrackApplied","sessionId":"view","token":"source-control","state":"paused","success":true})",
        request));
    EXPECT_EQ(request.token, "source-control");
    EXPECT_TRUE(request.success);
    for (
        const auto* body :
        {R"({"kind":"ipcTrackApplied","sessionId":"view","token":"source-control","state":"unknown","success":true})",
         R"({"kind":"ipcTrackApplied","sessionId":"view","token":"source-control","state":"paused","success":null})",
         R"({"kind":"ipcTrackApplied","sessionId":"view","token":"","state":"paused","success":true})",
         R"({"kind":"ipcTrackApplied","sessionId":"view","token":"source-control","state":"paused","success":true,"method":"trackState"})"}) {
        EXPECT_FALSE(decode_ipc_browser(body, request));
        EXPECT_TRUE(request.token.empty());
    }
}
// 未授权标识、同会话重复绑定及超过四路均不得创建浏览器采集副作用。
TEST(BrowserIPCMedia, LimitsAndPermissionIntent) {
    BrowserIPCMediaFactory factory;
    auto invalid = factory.create();
    EXPECT_FALSE(invalid->begin(invitation("")));
    std::vector<std::unique_ptr<IPCMediaPort>> peers;
    for (int index = 0; index < 4; ++index) {
        auto peer = factory.create();
        auto invite = invitation("view-" + std::to_string(index));
        if (index == 1)
            invite.audio_track = "mic";
        ASSERT_TRUE(peer->begin(invite));
        const auto command = factory.take_command();
        ASSERT_TRUE(command);
        EXPECT_NE(command->find(index == 1 ? "\"audio\":true" : "\"audio\":false"),
                  std::string::npos);
        peers.push_back(std::move(peer));
    }
    EXPECT_FALSE(invalid->begin(invitation("view-0")));
    EXPECT_FALSE(invalid->begin(invitation("extra")));
    EXPECT_FALSE(factory.take_command());
    peers[0]->close();
    EXPECT_TRUE(invalid->begin(invitation("replacement")));
}
// 背压必须变成明确故障；不得无限缓存或在没有停止命令时继续运行新Peer。
TEST(BrowserIPCMedia, QueueFailureStopsNewWork) {
    BrowserIPCMediaFactory factory;
    for (int index = 0; index < 17; ++index) {
        auto peer = factory.create();
        if (!peer)
            break;
        peer->begin(invitation("view-" + std::to_string(index)));
        peer->close();
    }
    EXPECT_TRUE(factory.failed());
    EXPECT_FALSE(factory.create());
    EXPECT_FALSE(factory.report_connected("view-16"));
}
// 本地桥不能透传设备密钥、平台method、重复键或过时/错误类型事实。
TEST(BrowserIPCMedia, StrictBrowserMessages) {
    IPCBrowserRequest request;
    EXPECT_TRUE(decode_ipc_browser(R"({"kind":"ready"})", request));
    EXPECT_TRUE(decode_ipc_browser(
        R"({"kind":"ipcOffer","sessionId":"view","sdp":"v=0\r\nanswer"})", request));
    EXPECT_EQ(request.session, "view");
    for (const auto* body :
         {R"({"kind":"ready","secret":"bad"})", R"({"kind":"ready","kind":"ready"})",
          R"({"kind":"ipcConnected","sessionId":2})", R"({"kind":"ipcConnected","sessionId":""})",
          R"({"kind":"ipcOffer","sessionId":"view","sdp":"not-sdp"})",
          R"({"kind":"ipcEnd","sessionId":"view","method":"sessionEnd"})",
          R"({"kind":"sessionEnd","sessionId":"view"})"}) {
        EXPECT_FALSE(decode_ipc_browser(body, request));
        EXPECT_TRUE(request.kind.empty());
        EXPECT_TRUE(request.session.empty());
    }
}
// 新媒体代次事实必须严格可解析；旧省略字段只对应1，不能省略失联事实代次。
TEST(BrowserIPCMedia, RecoveryFactsStrictRevisionAndOriginalPeer) {
    IPCBrowserRequest request;
    ASSERT_TRUE(decode_ipc_browser(
        R"({"kind":"ipcDisconnected","sessionId":"view","mediaRevision":1})", request));
    EXPECT_EQ(request.media_revision, 1U);
    ASSERT_TRUE(decode_ipc_browser(
        R"({"kind":"ipcOffer","sessionId":"view","mediaRevision":2,"sdp":"v=0\r\nanswer"})",
        request));
    EXPECT_EQ(request.media_revision, 2U);
    for (const auto* body :
         {R"({"kind":"ipcDisconnected","sessionId":"view"})",
          R"({"kind":"ipcConnected","sessionId":"view","mediaRevision":0})",
          R"({"kind":"ipcConnected","sessionId":"view","mediaRevision":1.5})",
          R"({"kind":"ipcConnected","sessionId":"view","mediaRevision":null})",
          R"({"kind":"ipcConnected","sessionId":"view","mediaRevision":9007199254740992})",
          R"({"kind":"ipcDisconnected","sessionId":"view","mediaRevision":1,"secret":"x"})"}) {
        EXPECT_FALSE(decode_ipc_browser(body, request));
        EXPECT_TRUE(request.kind.empty());
    }
}
// 恢复只沿用原资源预约，驱动两阶段连通与旧回调隔离，不再配置/采集。
TEST(BrowserIPCMedia, RecoveryDoesNotConfigureOrAcceptOldConnectedCallback) {
    BrowserIPCMediaFactory factory;
    auto peer = factory.create();
    IPCInvite invite;
    invite.envelope.session = "view";
    invite.video_track = "video-1";
    ASSERT_TRUE(peer->begin(invite));
    ASSERT_TRUE(factory.take_command());
    ASSERT_TRUE(peer->apply_answer("v=0\r\nanswer"));
    ASSERT_TRUE(factory.take_command());
    ASSERT_TRUE(factory.report_connected("view"));
    ASSERT_FALSE(peer->begin_recovery(3));
    ASSERT_TRUE(peer->begin_recovery(2));
    ASSERT_EQ(factory.take_command(),
              R"({"kind":"ipcRecover","sessionId":"view","mediaRevision":2})");
    EXPECT_FALSE(factory.report_connected("view", 1));
    EXPECT_FALSE(factory.report_connected("view", 2));
    ASSERT_TRUE(peer->apply_answer("v=0\r\nnew"));
    ASSERT_EQ(factory.take_command(),
              R"({"kind":"ipcAnswer","sessionId":"view","sdp":"v=0\r\nnew","mediaRevision":2})");
    EXPECT_FALSE(factory.report_connected("view", 1));
    EXPECT_TRUE(factory.report_connected("view", 2));
    peer->close();
    EXPECT_FALSE(peer->begin_recovery(3));
}
// 会话标识中引号必须编码而不是变成新的桥命令；非法标识拒绝。
TEST(BrowserIPCMedia, InviteNoticeEscaping) {
    EXPECT_EQ(ipc_browser_invite("view"), R"({"kind":"ipcInvite","sessionId":"view"})");
    EXPECT_EQ(ipc_browser_invite("view\"quote"),
              R"({"kind":"ipcInvite","sessionId":"view\"quote"})");
    EXPECT_TRUE(ipc_browser_invite("").empty());
    EXPECT_TRUE(ipc_browser_invite("view\n").empty());
    EXPECT_TRUE(ipc_browser_invite(std::string(65, 'a')).empty());
    EXPECT_EQ(ipc_browser_stop("view"), R"({"kind":"ipcStop","sessionId":"view"})");
    EXPECT_TRUE(ipc_browser_stop("view\n").empty());
    EXPECT_EQ(ipc_browser_active("view"), R"({"kind":"ipcActive","sessionId":"view"})");
    EXPECT_TRUE(ipc_browser_active("").empty());
}

// 对讲使用明确两条音轨和独立驱动许可；观看Peer不被隐式改变媒体方向。
TEST(BrowserIPCMedia, ExplicitTalkDriverAndAudioOnlyInvitation) {
    auto talk = invitation("talk");
    talk.video_track.clear();
    talk.audio_track = "mic-1";
    talk.speaker_track = "speaker-1";
    BrowserIPCMediaFactory preview;
    EXPECT_FALSE(preview.supports_talk());
    auto denied = preview.create();
    EXPECT_FALSE(denied->begin(talk));
    EXPECT_FALSE(preview.take_command());
    BrowserIPCMediaFactory factory(true);
    EXPECT_TRUE(factory.supports_talk());
    auto peer = factory.create();
    ASSERT_TRUE(peer->begin(talk));
    const auto command = factory.take_command();
    ASSERT_TRUE(command);
    EXPECT_NE(command->find(R"("videoTrack":"")"), std::string::npos);
    EXPECT_NE(command->find(R"("audioTrack":"mic-1")"), std::string::npos);
    EXPECT_NE(command->find(R"("speakerTrack":"speaker-1")"), std::string::npos);
    EXPECT_FALSE(factory.report_connected("talk"));
    ASSERT_TRUE(peer->apply_answer("v=0\r\nanswer"));
    ASSERT_TRUE(factory.report_connected("talk"));
    EXPECT_TRUE(peer->connected());
    peer->close();
    EXPECT_FALSE(peer->connected());
    EXPECT_FALSE(factory.report_connected("talk"));
    EXPECT_EQ(ipc_browser_invite("talk", true),
              R"({"kind":"ipcInvite","sessionId":"talk","talk":true})");
    EXPECT_TRUE(ipc_browser_invite("", true).empty());
}

// 自然暂断沿原本机租约和同Peer，只允许首次五秒内恢复，不改变协议媒体代次或重新采集。
TEST(BrowserIPCMedia, TalkNaturalConnectivityRetainsOriginalPeerAndFixedDeadline) {
    BrowserIPCMediaFactory factory(true);
    auto peer = factory.create();
    auto invite = invitation("natural-talk");
    invite.video_track.clear();
    invite.audio_track = "mic-1";
    invite.speaker_track = "speaker-1";
    ASSERT_TRUE(peer->begin(invite));
    const auto configure = factory.take_command();
    ASSERT_TRUE(configure);
    EXPECT_NE(configure->find(R"("releaseId":"1")"), std::string::npos);
    ASSERT_TRUE(peer->apply_answer("v=0\r\nanswer"));
    factory.take_command();
    ASSERT_TRUE(factory.report_connected("natural-talk"));
    ASSERT_EQ(factory.report_connectivity("natural-talk", "1", false, 100), Result::Applied);
    EXPECT_FALSE(peer->connected());
    EXPECT_FALSE(factory.report_connected("natural-talk"));
    EXPECT_EQ(factory.report_connectivity("natural-talk", "1", false, 1000), Result::Duplicate);
    EXPECT_EQ(factory.report_connectivity("natural-talk", "1", true, 5099), Result::Applied);
    EXPECT_TRUE(peer->connected());
    EXPECT_EQ(factory.report_connectivity("natural-talk", "1", true, 5099), Result::Duplicate);
    EXPECT_TRUE(factory.poll_connectivity(10000).empty());
    ASSERT_EQ(factory.report_connectivity("natural-talk", "1", false, 11000), Result::Applied);
    EXPECT_EQ(factory.report_connectivity("natural-talk", "1", false, 15000), Result::Duplicate);
    EXPECT_EQ(factory.poll_connectivity(16000), std::vector<std::string>{"natural-talk"});
    EXPECT_FALSE(peer->connected());
    EXPECT_EQ(factory.report_connectivity("natural-talk", "1", true, 16000), Result::DriverFailed);
    EXPECT_TRUE(factory.poll_connectivity(16001).empty());
    peer->close();
    EXPECT_EQ(factory.report_connectivity("natural-talk", "1", true, 16001), Result::Stale);
}

// 原自然连通回调必须绑定唯一本机租约；相同session重用也不能接纳旧Peer的迟到事实。
TEST(BrowserIPCMedia, TalkNaturalConnectivityFencesOldLeaseAndUnestablishedMedia) {
    BrowserIPCMediaFactory factory(true);
    auto invite = invitation("natural-reused");
    invite.video_track.clear();
    invite.audio_track = "mic-1";
    invite.speaker_track = "speaker-1";
    auto original = factory.create();
    ASSERT_TRUE(original->begin(invite));
    factory.take_command();
    EXPECT_EQ(factory.report_connectivity("natural-reused", "1", false, 0), Result::Stale);
    ASSERT_TRUE(original->apply_answer("v=0\r\nanswer"));
    factory.take_command();
    EXPECT_EQ(factory.report_connectivity("natural-reused", "1", false, 0), Result::Invalid);
    EXPECT_EQ(factory.report_connectivity("natural-reused", "1", true, 0), Result::Invalid);
    ASSERT_TRUE(factory.report_connected("natural-reused"));
    for (const auto* stale : {"", "01", "2", "1.0"}) {
        EXPECT_EQ(factory.report_connectivity("natural-reused", stale, false, 0), Result::Stale);
        EXPECT_TRUE(original->connected());
    }
    original->close();
    factory.take_command();
    auto replacement = factory.create();
    ASSERT_TRUE(replacement->begin(invite));
    const auto configure = factory.take_command();
    ASSERT_TRUE(configure);
    EXPECT_NE(configure->find(R"("releaseId":"2")"), std::string::npos);
    ASSERT_TRUE(replacement->apply_answer("v=0\r\nanswer"));
    factory.take_command();
    ASSERT_TRUE(factory.report_connected("natural-reused"));
    EXPECT_EQ(factory.report_connectivity("natural-reused", "1", false, 1), Result::Stale);
    EXPECT_TRUE(replacement->connected());
    EXPECT_EQ(factory.report_connectivity("natural-reused", "2", false, 1), Result::Applied);
    EXPECT_FALSE(replacement->connected());
}

// 自然暂断的宿主强制截止不依赖网页回调；单调回退和精确到期均不得复活原媒体。
TEST(BrowserIPCMedia, TalkNaturalDeadlineAtZeroRollbackAndLateConnectedAreFailClosed) {
    for (const auto start : {0ULL, 100ULL}) {
        BrowserIPCMediaFactory factory(true);
        auto peer = factory.create();
        auto invite = invitation("natural-clock");
        invite.video_track.clear();
        invite.audio_track = "mic-1";
        invite.speaker_track = "speaker-1";
        ASSERT_TRUE(peer->begin(invite));
        factory.take_command();
        ASSERT_TRUE(peer->apply_answer("v=0\r\nanswer"));
        factory.take_command();
        ASSERT_TRUE(factory.report_connected("natural-clock"));
        ASSERT_EQ(factory.report_connectivity("natural-clock", "1", false, start), Result::Applied);
        EXPECT_TRUE(factory.poll_connectivity(start + 4999).empty());
        EXPECT_EQ(factory.report_connectivity("natural-clock", "1", true, start == 0 ? 5000 : 99),
                  Result::DriverFailed);
        EXPECT_FALSE(peer->connected());
        EXPECT_FALSE(factory.report_connected("natural-clock"));
        EXPECT_EQ(factory.poll_connectivity(start + 5001),
                  std::vector<std::string>{"natural-clock"});
        EXPECT_TRUE(factory.poll_connectivity(start + 5002).empty());
        EXPECT_EQ(factory.report_connectivity("natural-clock", "1", true, start + 5002),
                  Result::DriverFailed);
    }
}

// 同Peer自然暂断不触碰其他预览；原gate租约与ungated对讲本机序号不能互换。
TEST(BrowserIPCMedia, TalkNaturalConnectivityUsesActualGateLeaseAndDoesNotAffectPreview) {
    CaptureGate gate(3);
    BrowserIPCMediaFactory factory(true, &gate);
    auto preview = factory.create();
    ASSERT_TRUE(preview->begin(invitation("natural-preview")));
    factory.take_command();
    ASSERT_TRUE(preview->apply_answer("v=0\r\nanswer"));
    factory.take_command();
    ASSERT_TRUE(factory.report_connected("natural-preview"));
    auto invite = invitation("natural-gated-talk");
    invite.video_track.clear();
    invite.audio_track = "mic-1";
    invite.speaker_track = "speaker-1";
    auto talk = factory.create();
    ASSERT_TRUE(talk->begin(invite));
    factory.take_command();
    ASSERT_TRUE(talk->apply_answer("v=0\r\nanswer"));
    factory.take_command();
    ASSERT_TRUE(factory.report_connected("natural-gated-talk"));
    EXPECT_EQ(factory.report_connectivity("natural-preview", "1", false, 1), Result::Unsupported);
    EXPECT_EQ(factory.report_connectivity("natural-gated-talk", "1", false, 1), Result::Stale);
    EXPECT_EQ(factory.report_connectivity("natural-gated-talk", "2", false, 1), Result::Applied);
    EXPECT_TRUE(preview->connected());
    EXPECT_FALSE(talk->connected());
    EXPECT_EQ(factory.poll_connectivity(5001), std::vector<std::string>{"natural-gated-talk"});
    EXPECT_TRUE(preview->connected());
}

// 会议接收轨只能暂停恢复；不能通过源控制命令增加采集、重新发布或改变另一条音轨。
TEST(BrowserIPCMedia, TalkSpeakerControlIsBoundedAndIndependent) {
    BrowserIPCMediaFactory factory(true);
    auto peer = factory.create();
    auto invite = invitation("talk");
    invite.video_track.clear();
    invite.audio_track = "mic-1";
    invite.speaker_track = "speaker-1";
    ASSERT_TRUE(peer->begin(invite));
    factory.take_command();
    ASSERT_TRUE(peer->apply_answer("v=0\r\nanswer"));
    factory.take_command();
    ASSERT_TRUE(factory.report_connected("talk"));
    IPCTrackCommand command;
    command.envelope.session = "talk";
    command.envelope.token = "speaker";
    command.track_id = "speaker-1";
    command.revision = 1;
    for (const auto* operation : {"pause", "resume"}) {
        command.operation = operation;
        ASSERT_TRUE(peer->control_track(command));
        const auto queued = factory.take_command();
        ASSERT_TRUE(queued);
        EXPECT_NE(queued->find(R"("trackId":"speaker-1")"), std::string::npos);
    }
    for (const auto* operation : {"stop", "publish", "subscribe", "unsubscribe"}) {
        command.operation = operation;
        EXPECT_FALSE(peer->control_track(command));
        EXPECT_FALSE(factory.take_command());
    }
    command.operation = "pause";
    command.track_id = "uninvited-speaker";
    EXPECT_FALSE(peer->control_track(command));
    command.track_id.clear();
    EXPECT_FALSE(peer->control_track(command));
    command.track_id = "speaker-1";
    command.envelope.token = "invalid\n";
    EXPECT_FALSE(peer->control_track(command));
    command.envelope.token = "speaker";
    peer->close();
    EXPECT_FALSE(peer->control_track(command));
}

// 单设备输出预算不代替IM成员权限，一个对讲结束后只归还自己的扬声器引用。
TEST(BrowserIPCMedia, TalkOutputBudgetIndependentFromExistingView) {
    BrowserIPCMediaFactory factory(true);
    auto viewer = factory.create(), talker = factory.create(), second = factory.create();
    ASSERT_TRUE(viewer->begin(invitation("view")));
    auto invite = invitation("talk");
    invite.video_track.clear();
    invite.audio_track = "mic";
    invite.speaker_track = "speaker";
    ASSERT_TRUE(talker->begin(invite));
    factory.take_command();
    factory.take_command();
    invite.envelope.session = "second";
    EXPECT_FALSE(second->begin(invite));
    EXPECT_FALSE(factory.take_command());
    talker->close();
    EXPECT_NE(factory.take_command()->find(R"("sessionId":"talk")"), std::string::npos);
    EXPECT_TRUE(second->begin(invite));
    ASSERT_TRUE(viewer->apply_answer("v=0\r\nanswer"));
    EXPECT_TRUE(factory.report_connected("view"));
    EXPECT_TRUE(viewer->connected());
}

TEST(BrowserIPCMedia, TalkRejectsIncompleteTracksWithoutBridgeSideEffects) {
    for (int mode = 0; mode < 4; ++mode) {
        BrowserIPCMediaFactory factory(true);
        auto peer = factory.create();
        auto invite = invitation("talk");
        invite.video_track.clear();
        invite.audio_track = "mic";
        invite.speaker_track = "speaker";
        if (mode == 0)
            invite.video_track = "video";
        if (mode == 1)
            invite.audio_track.clear();
        if (mode == 2)
            invite.audio_track = invite.speaker_track;
        if (mode == 3)
            invite.speaker_track = "speaker\n";
        EXPECT_FALSE(peer->begin(invite));
        EXPECT_FALSE(factory.take_command());
    }
}
}  // namespace
}  // namespace ur::device
