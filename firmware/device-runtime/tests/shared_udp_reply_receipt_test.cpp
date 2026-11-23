// 文件说明：从原共享宿主夹具机械提取的安装合同回归；真实BrowserPcm/Opus/kernelUDP/AES，不冒称MQTT/硬件E2E。
#include "shared_handoff_host.hpp"
#include "openssl_ctr.hpp"
#include "opus_codec.hpp"
#include "udp_socket.hpp"
#include "ur/device/udp_voice_media.hpp"

#include <cJSON.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <deque>

namespace ur::device {
namespace {
// Network 保存原帧和唯一连接调用计数；ACK只由用例显式发送，不由publish伪造。
struct Network final : MqttPort {
    unsigned connects = 0, disconnects = 0;
    int mid = 10;
    std::deque<MqttEvent> events;
    std::vector<Envelope> sent;
    std::vector<std::string> topics;
    bool reject_context_ack = false;  // 仅拒绝本地私有回执受理，不伪造网络已发送。
    bool connect_ok = true;           // 仅控制原网络connect返回值，不合成连接事件或重连。
    bool reject_install = false;      // 只制造原安装回执publish受理背压，不阻断focus原协议。
    unsigned rejected_installs = 0;   // 必须由真正尝试原publish产生。
    std::vector<Envelope> refused_context_acks;  // 保留实际被拒回执，验证原消息可安全重投。
    bool connect(const MqttConfig&) override {
        ++connects;
        return connect_ok;
    }
    int subscribe(std::string_view topic) override {
        topics.emplace_back(topic);
        return mid++;
    }
    bool poll() override { return true; }
    std::optional<MqttEvent> take_event() override {
        if (events.empty())
            return {};
        auto result = std::move(events.front());
        events.pop_front();
        return result;
    }
    bool publish(std::string_view, std::string_view body) override {
        Envelope value;
        EXPECT_EQ(decode_envelope(body, value), DecodeResult::Ok);
        if (reject_install && value.method == "respAudioStart" && value.code == 200) {
            ++rejected_installs;
            return false;
        }
        if (reject_context_ack && value.method == "contextSnapshotAck") {
            refused_context_acks.push_back(std::move(value));
            return false;
        }
        sent.push_back(std::move(value));
        return true;
    }
    void disconnect() override { ++disconnects; }
};
// Media 仅注入AI UDP异步连通事实，绝不提供替代FocusDriver或默认资源所有权。
struct Media final : PumpedVoiceMediaPort {
    bool configured = false, connected = false, capture = false;
    bool poll_ok = true;  // 原AI媒体poll失败注入，不替代实际焦点或连通事实。
    unsigned starts = 0;
    bool configure(const VoiceMedia&) override {
        configured = true;
        return true;
    }
    std::uint32_t next_input_sequence() const override { return 128; }
    bool prepare_reply(const std::array<std::uint8_t, 8>&, std::uint32_t) override { return true; }
    bool finish_reply(std::uint32_t) override { return true; }
    bool start_capture(std::string_view) override {
        ++starts;
        capture = true;
        return true;
    }
    void stop_capture() override { capture = false; }
    bool start_playback(std::string_view, std::uint64_t) override { return true; }
    void stop_playback(std::uint64_t) override {}
    bool poll(std::uint64_t) override { return poll_ok; }
    bool ready() const override { return connected; }
    std::optional<std::uint64_t> drained_epoch() const override { return {}; }
    void close() override { capture = connected = configured = false; }
};
// Fixture
// 使用真正Host/InputOwnership/BrowserPcm/HumanMedia/DuplexDriver/SharedFocus/VoiceRuntime组合。
class SharedReceiptHost : public testing::Test {
protected:
    Network network;
    Media media;
    BrowserPcm pcm{{48000, 1, 20}};
    OpusCodec codec;         // 原真实软件codec，不以编解码stub证明安装。
    UdpSocket socket, peer;  // 原非阻塞本机UDP；peer只回送真实握手探包，非真实平台媒体。
    OpenSslCtr cipher;       // 原真实AES软件驱动，固定合成密钥不访问身份。
    UdpVoiceMedia udp{pcm, codec, socket, cipher};  // 与native相同四组件装配。
    bool paired = true;
    unsigned source_reads = 0;  // 精确记录原配对源查询，诊断getter不得重复读取业务端口。
    std::uint64_t now = 10, wall = 100000;
    std::vector<std::string> outputs;
    std::size_t consumed = 0;
    bool reject_input_binding = false;  // 只模拟原本地桥拒绝发送绑定，不制造驱动或云端成功。
    const bool receiving;  // 用例只能通过native构造意图选择角色，不能以平台邀请换模式。
    const bool receipt_requested, real_udp;  // 固定native意图与实际媒体选择，旧fake默认无新增能力。
    SharedHandoffHost host;
    SharedReceiptHost(bool receiver = false, bool diagnostic = false, bool temporary_calls = false,
                      bool receipt = true, bool actual_udp = true)
        : receiving(receiver),
          receipt_requested(receipt),
          real_udp(actual_udp),
          host(
              network,
              actual_udp ? static_cast<PumpedVoiceMediaPort&>(udp)
                         : static_cast<PumpedVoiceMediaPort&>(media),
              pcm,
              {"p/d",
               "instance",
               {},
               7,
               500000,
               600000,
               {},
               {},
               receiver,
               diagnostic,
               temporary_calls,
               receipt},
              [this] {
                  ++source_reads;
                  return paired;
              },
              [this](const std::string& body) {
                  outputs.push_back(body);
                  if (reject_input_binding && body.find("inputOwnershipBind") != std::string::npos)
                      return false;
                  return true;
              }) {}
    // step只推进可控时钟，不进行真实sleep。
    bool step() { return host.poll(++now, ++wall); }
    bool local(std::string_view body) { return host.receive_local(body, ++now, ++wall); }
    // down通过真实公共编码器生成完整MQTT帧，不调用Host内部跳过路由。
    void down(Envelope envelope) {
        std::string bytes;
        ASSERT_EQ(encode_envelope(envelope, bytes), DecodeResult::Ok);
        if (envelope.method == "sessionCreated") {
            VoiceEvent checked;
            ASSERT_EQ(decode_voice_event(WireRoute::Signaling, bytes, checked), WireResult::Ok);
        }
        network.events.push_back(
            {MqttEvent::Kind::Message, 0, 0, "$thing/down/signaling/p/d", bytes});
        ASSERT_TRUE(step());
    }
    // actual只确认实际已由Host交付的原线程命令；可选择一类以验证半组不能成立。
    void actual(std::string_view only = {}) {
        const auto end = outputs.size();
        for (; consumed < end; ++consumed) {
            std::unique_ptr<cJSON, decltype(&cJSON_Delete)> json(
                cJSON_Parse(outputs[consumed].c_str()), cJSON_Delete);
            const auto* value = cJSON_GetObjectItemCaseSensitive(json.get(), "kind");
            if (!cJSON_IsString(value))
                continue;
            const std::string kind = value->valuestring;
            if (!only.empty() && only != kind)
                continue;
            std::string changed;
            if (kind == "playbackGate")
                changed = "outputGated";
            else if (kind == "inputOwnershipGate") {
                const auto* route = cJSON_GetObjectItemCaseSensitive(json.get(), "routeGeneration");
                changed = cJSON_IsString(route) && std::string_view(route->valuestring) == "2"
                              ? "inputOwnershipSenderGated"
                              : "inputOwnershipGated";
            } else if (kind == "humanVoiceOutputInit") {
                changed = "humanVoiceOutputIdle";
                cJSON_AddNumberToObject(json.get(), "frames", 1);
            } else if (kind == "humanVoiceOutputGate") {
                changed = "humanVoiceOutputGated";
                cJSON_AddNumberToObject(json.get(), "frames", 1);
                cJSON_AddNumberToObject(json.get(), "nonSilentFrames", 0);
            } else
                continue;
            cJSON_ReplaceItemInObjectCaseSensitive(json.get(), "kind",
                                                   cJSON_CreateString(changed.c_str()));
            char* raw = cJSON_PrintUnformatted(json.get());
            ASSERT_NE(raw, nullptr);
            const std::string body = raw;
            cJSON_free(raw);
            EXPECT_TRUE(local(body));
        }
    }
    void boot(bool early = false) {
        if (early) {
            ASSERT_TRUE(local(R"({"kind":"inputOwnershipIdle","bootGeneration":"7"})"));
        }
        ASSERT_TRUE(local(R"({"kind":"ready","sampleRate":48000,"frameMs":20,"aec":true})"));
        if (!early) {
            ASSERT_TRUE(local(R"({"kind":"inputOwnershipIdle","bootGeneration":"7"})"));
        }
        ASSERT_TRUE(step());
        actual();
        ASSERT_TRUE(step());
        ASSERT_EQ(network.connects, 1U);
    }
    void connected() {
        boot();
        network.events.push_back({MqttEvent::Kind::Connected, 0, 0, {}, {}});
        ASSERT_TRUE(step());
        for (int mid = 10; mid < 13; ++mid)
            network.events.push_back({MqttEvent::Kind::Subscribed, mid, 0, {}, {}});
        ASSERT_TRUE(step());
        ASSERT_EQ(network.sent.back().method, "capabilityReport");
        Envelope ack;
        ack.request = false;
        ack.token = network.sent.back().token;
        ack.method = "requestAck";
        ack.code = 200;
        ack.payload = R"({"requestMethod":"capabilityReport","result":{"acceptedRevision":1}})";
        down(ack);
        ASSERT_EQ(host.connection_state(), SharedConnectionState::Ready);
        if (!receiving) {
            ASSERT_EQ(network.sent.back().method, "sessionCreate");
        }
    }
    void established(std::string session = "ai", bool selected_receipt = true) {
        connected();
        if (real_udp) {
            ASSERT_TRUE(peer.open("127.0.0.1", 0));
        }
        Envelope created;
        created.request = false;
        created.token = network.sent.back().token;
        created.method = "sessionCreated";
        created.session = session;
        created.code = 200;
        created.payload =
            R"({"state":"negotiating","selected":{"transport":"udp","interactionMode":"fullDuplex","audioParams":{"format":"opus","sampleRate":48000,"channels":1,"frameDuration":20},"inputModalities":["text"],"outputModalities":["text","audio"]},"tracks":[{"trackId":"mic-1","kind":"audio","sourceId":"mic","direction":"sendonly","codec":"opus","state":"requested"},{"trackId":"speaker-1","kind":"audio","sourceId":"speaker","direction":"recvonly","codec":"opus","state":"requested"}],"media":{"udp":{"server":"127.0.0.1","port":12345,"key":"000102030405060708090a0b0c0d0e0f","nonce":"01000000112233445566778800000000"}}})";
        if (receipt_requested && selected_receipt) {
            const auto selected = created.payload.find("\"interactionMode\"");
            created.payload.insert(selected, "\"udpReplyStartReceipt\":true,");
        }
        if (real_udp) {
            const auto port = created.payload.find("12345");
            created.payload.replace(port, 5, std::to_string(peer.local_port()));
        }
        down(created);
        if (real_udp) {
            ASSERT_NE(socket.local_port(), 0);
            ASSERT_TRUE(peer.set_peer("127.0.0.1", socket.local_port()));
            std::vector<std::uint8_t> probe;
            ASSERT_EQ(peer.receive(probe), ReceiveResult::Received);
            ASSERT_TRUE(peer.send(probe));
        } else {
            ASSERT_TRUE(media.configured);
            media.connected = true;
        }
        ASSERT_TRUE(step());
        ASSERT_EQ(network.sent.back().method, "mediaState");
        Envelope ack;
        ack.request = false;
        ack.token = network.sent.back().token;
        ack.method = "requestAck";
        ack.session = session;
        ack.code = 200;
        ack.payload =
            R"({"requestMethod":"mediaState","result":{"mediaRevision":1,"state":"connected"}})";
        down(ack);
        Envelope event;
        event.request = false;
        event.token = "established";
        event.method = "sessionEstablished";
        event.session = std::move(session);
        event.event_sequence = 1;
        event.payload = R"({"state":"established","mediaRevision":1})";
        down(event);
        ASSERT_EQ(host.voice_state(), VoiceState::Established);
    }
    void acquire(std::string session = "ai", std::uint64_t sequence = 2) {
        Envelope control;
        control.token = "initial-focus";
        control.method = "focusControl";
        control.session = std::move(session);
        control.request = true;
        control.event_sequence = sequence;
        control.payload =
            R"({"operation":"acquire","expectedRevision":0,"focusId":"ai-focus","resources":["microphone","speaker"],"priority":"conversation","resumePolicy":"explicit","expiresAt":200000})";
        down(control);
        actual();
        ASSERT_TRUE(step());
        actual();
        ASSERT_TRUE(step());
    }
    // input_binding 验证实际Host输出：原四字段绑定必须唯一且早于同scope的任何门控票据。
    void input_binding(std::string_view session, std::string_view generation,
                       std::string_view kind) {
        std::size_t bindings = 0;
        bool seen = false;
        for (const auto& body : outputs) {
            std::unique_ptr<cJSON, decltype(&cJSON_Delete)> json(cJSON_Parse(body.c_str()),
                                                                 cJSON_Delete);
            const auto* method = cJSON_GetObjectItemCaseSensitive(json.get(), "kind");
            const auto* id = cJSON_GetObjectItemCaseSensitive(json.get(), "sessionId");
            if (!cJSON_IsString(method) || !cJSON_IsString(id) || session != id->valuestring)
                continue;
            const std::string_view name = method->valuestring;
            if (name == "inputOwnershipBind") {
                const auto* route = cJSON_GetObjectItemCaseSensitive(json.get(), "routeGeneration");
                const auto* type = cJSON_GetObjectItemCaseSensitive(json.get(), "routeKind");
                ASSERT_EQ(cJSON_GetArraySize(json.get()), 4);
                ASSERT_TRUE(cJSON_IsString(route) && cJSON_IsString(type));
                EXPECT_EQ(generation, route->valuestring);
                EXPECT_EQ(kind, type->valuestring);
                seen = true;
                ++bindings;
            } else if (name == "inputOwnershipGate") {
                EXPECT_TRUE(seen) << "原页面必须先取得可信关联，门控票据本身不登记路由";
            }
        }
        EXPECT_EQ(bindings, 1U);
    }
    // candidate
    // 沿原callState关联和真实HumanCall邀请/接听/答案状态推进，不直接创建空Peer事实。
    void candidate(bool output_idle = true) {
        established();
        acquire();
        ASSERT_TRUE(host.ai_owned());
        ASSERT_TRUE(local(
            R"({"kind":"handoffControl","operation":"escalate","targetKind":"device","targetId":"other"})"));
        Envelope ack;
        ack.request = false;
        ack.token = network.sent.back().token;
        ack.method = "callState";
        ack.session = "ai";
        ack.code = 200;
        ack.payload =
            R"({"state":"transferring","revision":1,"candidateSessionId":"remote-human","localSessionId":"local-human","callGroupId":"group"})";
        down(ack);
        ASSERT_EQ(host.handoff_state(), AiHandoffState::Candidate);
        Envelope invite;
        invite.token = "invite-local";
        invite.method = "sessionInvite";
        invite.session = "local-human";
        invite.event_sequence = 1;
        invite.payload =
            R"({"purpose":"humanCall","transport":"webrtc","admission":"accept","callType":"voice","callGroupId":"group","tracks":[{"trackId":"voice","kind":"audio","sourceId":"microphone","sourceType":"microphone","direction":"sendrecv","codec":"opus","state":"requested"}],"expiresAt":125000})";
        down(invite);
        ASSERT_EQ(network.sent.back().method, "sessionAccept");
        Envelope accepted;
        accepted.request = false;
        accepted.token = network.sent.back().token;
        accepted.method = "requestAck";
        accepted.session = "local-human";
        accepted.code = 200;
        accepted.payload = R"({"requestMethod":"sessionAccept","result":{}})";
        down(accepted);
        if (output_idle) {
            actual();
            ASSERT_TRUE(step());
        }
        ASSERT_TRUE(std::any_of(outputs.begin(), outputs.end(), [](const auto& body) {
            return body.find("humanVoiceStart") != std::string::npos;
        }));
        ASSERT_TRUE(local(
            R"({"kind":"humanVoiceOffer","sessionId":"local-human","routeGeneration":"2","peerEpoch":"1","sdp":"v=0\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\na=rtpmap:111 opus/48000/2\r\n"})"));
        ASSERT_TRUE(step());
        ASSERT_EQ(network.sent.back().method, "mediaOffer");
        Envelope answer;
        answer.request = false;
        answer.token = network.sent.back().token;
        answer.method = "mediaAnswer";
        answer.session = "local-human";
        answer.code = 200;
        answer.payload =
            R"({"mediaRevision":1,"webrtc":{"iceMode":"complete","sdp":"v=0\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\na=rtpmap:111 opus/48000/2\r\n"}})";
        down(answer);
        ASSERT_TRUE(local(
            R"({"kind":"humanVoiceAnswered","sessionId":"local-human","routeGeneration":"2","peerEpoch":"1"})"));
        ASSERT_TRUE(local(
            R"({"kind":"humanVoiceConnected","sessionId":"local-human","routeGeneration":"2","peerEpoch":"1"})"));
        ASSERT_TRUE(step());
        if (!output_idle) {
            ASSERT_NE(network.sent.back().method, "mediaState");
            return;
        }
        ASSERT_EQ(network.sent.back().method, "mediaState");
        Envelope media_ack;
        media_ack.request = false;
        media_ack.token = network.sent.back().token;
        media_ack.method = "requestAck";
        media_ack.session = "local-human";
        media_ack.code = 200;
        media_ack.payload = R"({"requestMethod":"mediaState","result":{}})";
        down(media_ack);
    }
    // reply 使用原合法五字段Start；不调用core私有状态、不给PCM虚假Ready。
    Envelope reply(std::uint64_t sequence = 3, std::string token = "reply-start") const {
        Envelope event;
        event.request = false;
        event.token = std::move(token);
        event.method = "respAudioStart";
        event.session = "ai";
        event.event_sequence = sequence;
        event.payload =
            R"({"respId":"reply","trackId":"speaker-1","mediaRevision":1,"udpNonce":"0000000000000063","firstPacketSeq":128})";
        return event;
    }
    // installs 只数真正受理的200，不从媒体Ready或publish尝试反推业务成功。
    std::size_t installs() const {
        return static_cast<std::size_t>(std::count_if(
            network.sent.begin(), network.sent.end(),
            [](const auto& e) { return e.method == "respAudioStart" && e.code == 200; }));
    }
    // suspend_ai 沿原真实完整组许可/两票据，不直接调用待新增生命周期端口。
    void suspend_ai() {
        Envelope control;
        control.token = "suspend-ai";
        control.method = "focusControl";
        control.session = "ai";
        control.event_sequence = 4;
        control.payload =
            R"({"operation":"suspend","expectedRevision":1,"focusId":"ai-focus","resources":["microphone","speaker"],"priority":"conversation","resumePolicy":"explicit"})";
        down(control);
        actual();
        ASSERT_TRUE(step());
        ASSERT_TRUE(step());
        ASSERT_FALSE(host.ai_owned());
    }
    // commit_without_old_end 保留原AI会话未Ended窗口，以核永久围栏后待发200不能靠后继close兜底。
    void commit_without_old_end(std::uint64_t sequence = 5) {
        Envelope control;
        control.token = "acquire-human";
        control.method = "focusControl";
        control.session = "local-human";
        control.event_sequence = 2;
        control.payload =
            R"({"operation":"acquire","expectedRevision":2,"focusId":"human-focus","resources":["microphone","speaker"],"priority":"call","resumePolicy":"never","expiresAt":200000})";
        down(control);
        actual();
        ASSERT_TRUE(step());
        actual();
        ASSERT_TRUE(step());
        ASSERT_TRUE(step());
        ASSERT_EQ(host.handoff_state(), AiHandoffState::Acquired);
        Envelope event;
        event.request = false;
        event.token = "human-established";
        event.method = "sessionEstablished";
        event.session = "local-human";
        event.event_sequence = 3;
        event.payload = R"({"state":"established","mediaRevision":1})";
        down(event);
        const auto call = std::find_if(network.sent.begin(), network.sent.end(),
                                       [](const auto& e) { return e.method == "callControl"; });
        ASSERT_NE(call, network.sent.end());
        event.token = "commit";
        event.method = "callState";
        event.session = "ai";
        event.event_sequence = sequence;
        event.payload =
            "{\"state\":\"transferred\",\"revision\":2,\"candidateSessionId\":\"remote-human\",\"localSessionId\":\"local-human\",\"callGroupId\":\"group\",\"requestToken\":\"" +
            call->token + "\"}";
        down(event);
        ASSERT_EQ(host.handoff_state(), AiHandoffState::HumanActive);
    }
};

TEST_F(SharedReceiptHost, RealPcmReadyAndUdpAdvertiseGlobalAndBorrowedAiConsistently) {
    EXPECT_FALSE(udp.udp_reply_start_receipt_ready());
    EXPECT_TRUE(network.sent.empty());
    established();
    ASSERT_TRUE(udp.udp_reply_start_receipt_ready());
    unsigned constructions = 0;  // 真实发出的全局广告和原借用AI创建都必须出现，不允许空循环通过。
    for (const auto& e : network.sent)
        if (e.method == "capabilityReport" || e.method == "sessionCreate") {
            ++constructions;
            // 全局快照features为字符串数组；sessionCreate只请求UDP，true仅属于平台selected。
            std::unique_ptr<cJSON, decltype(&cJSON_Delete)> data(cJSON_Parse(e.payload.c_str()),
                                                                 cJSON_Delete);
            ASSERT_NE(data, nullptr);
            if (e.method == "capabilityReport") {
                const auto* features = cJSON_GetObjectItemCaseSensitive(data.get(), "features");
                ASSERT_TRUE(cJSON_IsArray(features));
                unsigned receipt_features = 0;
                const cJSON* item = nullptr;
                cJSON_ArrayForEach (item, features) {
                    if (cJSON_IsString(item) &&
                        std::string_view(item->valuestring) == "udpReplyStartReceipt")
                        ++receipt_features;
                }
                EXPECT_EQ(receipt_features, 1U);
                EXPECT_NE(e.payload.find("webrtc"), std::string::npos);
            } else {
                const auto* purpose = cJSON_GetObjectItemCaseSensitive(data.get(), "purpose");
                const auto* transport = cJSON_GetObjectItemCaseSensitive(data.get(), "transport");
                ASSERT_TRUE(cJSON_IsString(purpose));
                ASSERT_TRUE(cJSON_IsString(transport));
                EXPECT_STREQ(purpose->valuestring, "aiConversation");
                EXPECT_STREQ(transport->valuestring, "udp");
                EXPECT_EQ(e.payload.find("udpReplyStartReceipt"), std::string::npos);
            }
        }
    EXPECT_EQ(constructions, 2U);
    acquire();
    down(reply());
    EXPECT_EQ(installs(), std::size_t{1});
}
TEST_F(SharedReceiptHost, BeforeActualOwnedOldStartCannotInstallOrReviveUnderChangedSequence) {
    established();
    down(reply(2));
    EXPECT_EQ(installs(), std::size_t{0});
    acquire("ai", 3);
    ASSERT_TRUE(host.ai_owned());
    down(reply(4));
    EXPECT_EQ(installs(), std::size_t{0});
}
TEST_F(SharedReceiptHost, OldSequenceUnownedStartDoesNotTombstoneFreshHigherSequenceWork) {
    established();
    // sessionEstablished已消费seq1；未知token的同seq晚包无新事实，不能提前封死将来的合法新票据。
    down(reply(1, "fresh-after-owned"));
    EXPECT_EQ(installs(), std::size_t{0});
    acquire();
    ASSERT_TRUE(host.ai_owned());
    // 后继完整组真实取得，seq3为新工作；该断言专门保存现gate在eventSeq之前记摘要的静态风险。
    down(reply(3, "fresh-after-owned"));
    EXPECT_EQ(installs(), std::size_t{1});
}
// WrongScopeLateStart不能抬路由自造水位；随后较小但核心新鲜的正确原Start仍须真实安装。
TEST_F(SharedReceiptHost, WrongTrackOrRevisionHighSequenceCannotSealLaterLegalStart) {
    established();
    auto wrong = reply(100, "fresh-correct-scope");
    wrong.payload.replace(wrong.payload.find("speaker-1"), 9, "speaker-2");
    down(wrong);
    wrong = reply(101, "fresh-correct-scope");
    wrong.payload.replace(wrong.payload.find("\"mediaRevision\":1"), 17, "\"mediaRevision\":2");
    down(wrong);
    EXPECT_EQ(installs(), std::size_t{0});
    acquire();
    ASSERT_TRUE(host.ai_owned());
    down(reply(3, "fresh-correct-scope"));
    EXPECT_EQ(installs(), std::size_t{1});
}
TEST_F(SharedReceiptHost, PermanentFenceRetiresPublisherBusyInstallationBeforeOldAiEnded) {
    candidate();
    network.reject_install = true;
    down(reply());
    ASSERT_GT(network.rejected_installs, 0U);
    ASSERT_EQ(installs(), std::size_t{0});
    suspend_ai();
    commit_without_old_end();
    network.reject_install = false;
    ASSERT_TRUE(step());
    down(reply());
    EXPECT_EQ(installs(), std::size_t{0});
    EXPECT_EQ(network.disconnects, 0U);
    EXPECT_EQ(host.voice_state(), VoiceState::Established);
}
TEST_F(SharedReceiptHost, PermanentFenceInvalidatesPreviouslyDeliveredOriginalReceipt) {
    candidate();
    down(reply());
    ASSERT_EQ(installs(), std::size_t{1});
    suspend_ai();
    commit_without_old_end();
    down(reply());
    ASSERT_TRUE(step());
    EXPECT_EQ(installs(), std::size_t{1});
    EXPECT_EQ(network.disconnects, 0U);
}
TEST_F(SharedReceiptHost, HeldUnownedStartCannotReturn200OrReviveAfterPermanentHandoff) {
    candidate();
    suspend_ai();
    down(reply(5, "held-start"));
    EXPECT_EQ(installs(), std::size_t{0});
    commit_without_old_end(6);
    down(reply(7, "held-start"));
    EXPECT_EQ(installs(), std::size_t{0});
    EXPECT_EQ(network.disconnects, 0U);
}
// DefaultSharedReceiptHost 不选新合同，真实UDP仍不得自行声明或回200。
class DefaultSharedReceiptHost : public SharedReceiptHost {
protected:
    DefaultSharedReceiptHost() : SharedReceiptHost(false, false, false, false, true) {}
};
TEST_F(DefaultSharedReceiptHost, DefaultFeatureAndOldSelectedRemainUnchanged) {
    established();
    acquire();
    down(reply());
    EXPECT_EQ(installs(), std::size_t{0});
    for (const auto& e : network.sent) {
        if (e.method == "capabilityReport" || e.method == "sessionCreate") {
            EXPECT_EQ(e.payload.find("udpReplyStartReceipt"), std::string::npos);
        }
    }
}
TEST_F(SharedReceiptHost, AdvertisedSupportButOldDeviceSelectionNeverReturnsInstallReceipt) {
    established("ai", false);
    acquire();
    down(reply());
    EXPECT_EQ(installs(), std::size_t{0});
}
// UnsupportedSharedReceiptHost 原通用fake无真实UDP安装能力，不能借actual PCM Ready声明。
class UnsupportedSharedReceiptHost : public SharedReceiptHost {
protected:
    UnsupportedSharedReceiptHost() : SharedReceiptHost(false, false, false, true, false) {}
};
TEST_F(UnsupportedSharedReceiptHost, GenericMediaCannotAdvertiseEvenWithRealPcmReady) {
    EXPECT_TRUE(local(R"({"kind":"ready","sampleRate":48000,"frameMs":20,"aec":true})"));
    EXPECT_TRUE(local(R"({"kind":"inputOwnershipIdle","bootGeneration":"7"})"));
    ASSERT_TRUE(step());
    actual();
    EXPECT_FALSE(step());
    EXPECT_TRUE(network.sent.empty());
}
// HumanOnlySharedReceiptHost 纯Human入口没有AI UDP用途，不得借真实PCM/UDP声明新能力。
class HumanOnlySharedReceiptHost : public SharedReceiptHost {
protected:
    HumanOnlySharedReceiptHost() : SharedReceiptHost(true, false, false, true, true) {}
};
TEST_F(HumanOnlySharedReceiptHost, HumanOnlyConfigCannotDeclareAiInstallReceipt) {
    EXPECT_TRUE(local(R"({"kind":"ready","sampleRate":48000,"frameMs":20,"aec":true})"));
    EXPECT_TRUE(local(R"({"kind":"inputOwnershipIdle","bootGeneration":"7"})"));
    ASSERT_TRUE(step());
    actual();
    EXPECT_FALSE(step());
    EXPECT_TRUE(network.sent.empty());
}

}  // namespace
}  // namespace ur::device
