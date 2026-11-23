// 文件说明：正式共享宿主组合回归；真实协议/门控状态机结合可控网络和线程票据，不冒称真实媒体E2E。
#include "shared_handoff_host.hpp"

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
    bool reject_context_ack = false;            // 仅拒绝本地私有回执受理，不伪造网络已发送。
    bool connect_ok = true;                     // 仅控制原网络connect返回值，不合成连接事件或重连。
    bool reject_human_create_publish = false; // 仅模拟人际创建首次发布背压，不造平台拒绝应答。
    std::vector<Envelope> refused_human_creates; // 原发布尝试留在单测内核同token重传，不输真实身份。
    std::vector<Envelope> refused_context_acks; // 保留实际被拒回执，验证原消息可安全重投。
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
        if (reject_human_create_publish && value.method == "sessionCreate" &&
            value.payload.find("humanCall") != std::string::npos) {
            refused_human_creates.push_back(std::move(value));
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
    bool poll_ok = true; // 原AI媒体poll失败注入，不替代实际焦点或连通事实。
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
class SharedHost : public testing::Test {
    protected:
    Network network;
    Media media;
    BrowserPcm pcm{{48000, 1, 20}};
    bool paired = true;
    unsigned source_reads = 0; // 精确记录原配对源查询，诊断getter不得重复读取业务端口。
    std::uint64_t now = 10, wall = 100000;
    std::vector<std::string> outputs;
    std::size_t consumed = 0;
    bool reject_input_binding = false; // 只模拟原本地桥拒绝发送绑定，不制造驱动或云端成功。
    const bool receiving;              // 用例只能通过native构造意图选择角色，不能以平台邀请换模式。
    SharedHandoffHost host;
    SharedHost(bool receiver = false, bool diagnostic = false, bool temporary_calls = false,
               bool half_duplex_only = false, bool endpoint_source = false,
               std::function<std::optional<AudioTiming>(std::uint64_t)> stopped_timing = {},
               std::function<std::optional<std::uint64_t>()> last_voice_sent_ms = {})
        : receiving(receiver),
          host(
              network, media, pcm,
              {"p/d",
               "instance",
               {},
               7,
               500000,
               600000,
               {},
               std::move(stopped_timing),
               receiver,
               diagnostic,
               temporary_calls,
               false,
               half_duplex_only,
               endpoint_source,
               std::move(last_voice_sent_ms)},
              [this] {
                  ++source_reads;
                  return paired;
              },
              [this](const std::string &body) {
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
    // established 注入正式协商选择；text_input=false仅选择音频能力，不伪造文字支持。
    void established(std::string session = "ai", bool text_input = true) {
        connected();
        Envelope created;
        created.request = false;
        created.token = network.sent.back().token;
        created.method = "sessionCreated";
        created.session = session;
        created.code = 200;
        created.payload =
            R"({"state":"negotiating","selected":{"transport":"udp","interactionMode":"fullDuplex","audioParams":{"format":"opus","sampleRate":48000,"channels":1,"frameDuration":20},"inputModalities":["text"],"outputModalities":["text","audio"]},"tracks":[{"trackId":"mic-1","kind":"audio","sourceId":"mic","direction":"sendonly","codec":"opus","state":"requested"},{"trackId":"speaker-1","kind":"audio","sourceId":"speaker","direction":"recvonly","codec":"opus","state":"requested"}],"media":{"udp":{"server":"127.0.0.1","port":12345,"key":"000102030405060708090a0b0c0d0e0f","nonce":"01000000112233445566778800000000"}}})";
        if (!text_input) {
            const std::string original = R"(,"inputModalities":["text"],"outputModalities":["text","audio"])";
            const auto at = created.payload.find(original);
            ASSERT_NE(at, std::string::npos);
            created.payload.erase(at, original.size()); // 音频轨仍在，离散文字能力没有被选择。
        }
        down(created);
        ASSERT_TRUE(media.configured);
        media.connected = true;
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
    void acquire(std::string session = "ai") {
        Envelope control;
        control.token = "initial-focus";
        control.method = "focusControl";
        control.session = std::move(session);
        control.request = true;
        control.event_sequence = 2;
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
    // commit_human
    // 等待真实组合原三屏障，供正式终态回归复用，不直接修改租约或会话状态。
    // 同poll选项只改变合法Established的调度，不改变原实际双票据和转接屏障。
    // 最后选项只将父腿的合法提交/结束放在本地腿Established前，验证跨会话不存在总序。
    void commit_human(bool holdable = false, bool established_in_receipt_poll = false,
                      bool final_before_local_established = false);
    // acquire_human_with_clocks 沿原AI/候选/实际暂停链分别推进两时钟，不替代任何资源事实。
    void acquire_human_with_clocks(std::uint64_t monotonic_progress, std::uint64_t wall_progress);
};
// HumanReceiver 复用同原宿主/采集源/门控，区别仅为native不主动创建AI。
class HumanReceiver : public SharedHost {
    protected:
    HumanReceiver(bool diagnostic = false, bool endpoint_source = false) : SharedHost(true, diagnostic, false, false, endpoint_source) {}
    // invite 仅向真实共享路由交原目标邀请，不授接听意图或实际资源。
    void invite(bool endpoint_candidate = false) {
        connected();
        Envelope value;
        value.token = "invite-target";
        value.method = "sessionInvite";
        value.session = "target-human";
        value.event_sequence = 1;
        value.payload =
            R"({"purpose":"humanCall","transport":"webrtc","admission":"accept","callType":"voice","callGroupId":"group","tracks":[{"trackId":"voice","kind":"audio","sourceId":"microphone","sourceType":"microphone","direction":"sendrecv","codec":"opus","state":"requested"}],"expiresAt":125000})";
        if (endpoint_candidate) {
            value.payload.pop_back();
            value.payload += R"(,"handoff":{"sourceSessionId":"previous-human","handoffId":"incoming-original-handoff"}})";
        }
        down(value);
    }
    // prepare 等待真实接听ACK、原offer/answer和输出freshfalse；ICE不代替Focus。
    void prepare(bool output_idle = true, bool media_ack = true, bool endpoint_candidate = false) {
        invite(endpoint_candidate);
        ASSERT_TRUE(local(R"({"kind":"handoffControl","operation":"accept"})"));
        ASSERT_EQ(network.sent.back().method, "sessionAccept");
        Envelope ack;
        ack.request = false;
        ack.token = network.sent.back().token;
        ack.method = "requestAck";
        ack.session = "target-human";
        ack.code = 200;
        ack.payload = R"({"requestMethod":"sessionAccept","result":{}})";
        down(ack);
        if (output_idle) {
            actual();
            ASSERT_TRUE(step());
        }
        ASSERT_TRUE(local(
            R"({"kind":"humanVoiceOffer","sessionId":"target-human","routeGeneration":"2","peerEpoch":"1","sdp":"v=0\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\na=rtpmap:111 opus/48000/2\r\n"})"));
        ASSERT_TRUE(step());
        ASSERT_EQ(network.sent.back().method, "mediaOffer");
        Envelope answer;
        answer.request = false;
        answer.token = network.sent.back().token;
        answer.method = "mediaAnswer";
        answer.session = "target-human";
        answer.code = 200;
        answer.payload =
            R"({"mediaRevision":1,"webrtc":{"iceMode":"complete","sdp":"v=0\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\na=rtpmap:111 opus/48000/2\r\n"}})";
        down(answer);
        ASSERT_TRUE(local(
            R"({"kind":"humanVoiceAnswered","sessionId":"target-human","routeGeneration":"2","peerEpoch":"1"})"));
        ASSERT_TRUE(local(
            R"({"kind":"humanVoiceConnected","sessionId":"target-human","routeGeneration":"2","peerEpoch":"1"})"));
        ASSERT_TRUE(step());
        if (!output_idle)
            return;
        ASSERT_EQ(network.sent.back().method, "mediaState");
        Envelope state_ack;
        state_ack.request = false;
        state_ack.token = network.sent.back().token;
        state_ack.method = "requestAck";
        state_ack.session = "target-human";
        state_ack.code = 200;
        state_ack.payload = R"({"requestMethod":"mediaState","result":{}})";
        if (media_ack)
            down(state_ack);
    }
    // control 目标全局CAS从0开始，与主叫AI租约无关。
    Envelope control(std::string operation = "acquire", unsigned revision = 0,
                     unsigned sequence = 2) {
        Envelope value;
        value.request = true;
        value.token = operation == "acquire" ? "target-acquire" : "target-release";
        value.method = "focusControl";
        value.session = "target-human";
        value.event_sequence = sequence;
        value.payload =
            "{\"operation\":\"" + operation +
            "\",\"expectedRevision\":" + std::to_string(revision) +
            R"(,"focusId":"target-focus","resources":["microphone","speaker"],"priority":"call","resumePolicy":"never","expiresAt":200000})";
        return value;
    }
    // holdable只改变原初始策略；同poll选项在真实第二组回执到达后入队合法最终事件。
    void acquire_target(bool holdable = false, bool established_in_receipt_poll = false,
                        bool endpoint_candidate = false) {
        prepare(true, true, endpoint_candidate);
        auto initial = control();
        if (holdable) {
            const auto policy = initial.payload.find("never");
            ASSERT_NE(policy, std::string::npos);
            initial.payload.replace(policy, 5, "explicit");
        }
        down(initial);
        actual();
        ASSERT_TRUE(step());
        actual();
        if (established_in_receipt_poll) {
            Envelope value;
            value.request = false;
            value.token = "target-established";
            value.method = "sessionEstablished";
            value.session = "target-human";
            value.event_sequence = 3;
            value.payload = R"({"state":"established","mediaRevision":1})";
            std::string bytes;
            ASSERT_EQ(encode_envelope(value, bytes), DecodeResult::Ok);
            network.events.push_back(
                {MqttEvent::Kind::Message, 0, 0, "$thing/down/signaling/p/d", bytes});
        }
        ASSERT_TRUE(step());
    }
    // acquire_with_clocks 在原绑定false与true回执之间分别推进两时钟，不改期限或许可。
    void acquire_with_clocks(std::uint64_t monotonic_progress, std::uint64_t wall_progress) {
        prepare();
        const auto original = control();
        down(original);
        now += monotonic_progress;
        wall += wall_progress;
        actual();
        ASSERT_TRUE(step());
        actual();
        ASSERT_TRUE(step());
        const auto receipt =
            std::find_if(network.sent.rbegin(), network.sent.rend(), [&](const auto& value) {
                return value.method == "focusState" && value.token == original.token;
            });
        ASSERT_NE(receipt, network.sent.rend());
        EXPECT_EQ(receipt->code, 200);
        EXPECT_EQ(host.handoff_state(), AiHandoffState::Acquired);
        EXPECT_EQ(network.disconnects, 0U);
    }
    void establish_target() {
        Envelope value;
        value.request = false;
        value.token = "target-established";
        value.method = "sessionEstablished";
        value.session = "target-human";
        value.event_sequence = 3;
        value.payload = R"({"state":"established","mediaRevision":1})";
        down(value);
    }
    // hold_focus 固定原完整焦点及资源CAS，token关联同一次callControl而非call
    // revision。
    Envelope hold_focus(bool held, std::string_view token, unsigned revision, unsigned sequence) {
        auto value = control(held ? "suspend" : "restore", revision, sequence);
        value.token = token;
        value.payload =
            "{\"operation\":\"" + std::string(held ? "suspend" : "restore") +
            "\",\"expectedRevision\":" + std::to_string(revision) +
            R"(,"focusId":"target-focus","resources":["microphone","speaker"],"priority":"call","resumePolicy":"explicit")" +
            (held ? "}" : ",\"expiresAt\":200000}");
        return value;
    }
    // hold_state 只生成原平台完成事件，不把它自身当作实际停止/恢复事实。
    Envelope hold_state(bool held, std::string_view token, unsigned revision, unsigned sequence) {
        Envelope value;
        value.request = false;
        value.token = "state-" + std::string(token);
        value.method = "callState";
        value.session = "target-human";
        value.event_sequence = sequence;
        value.payload = "{\"state\":\"" + std::string(held ? "held" : "active") +
                        "\",\"revision\":" + std::to_string(revision) + ",\"requestToken\":\"" +
                        std::string(token) + "\"}";
        return value;
    }
    // last_projection 读取宿主固定投影，不以页面按钮反向授权。
    std::string last_projection() const {
        for (auto it = outputs.rbegin(); it != outputs.rend(); ++it)
            if (it->find("\"kind\":\"handoffState\"") != std::string::npos)
                return *it;
        return {};
    }
};
// private_context
// 仅生成完整已完成私有wire，接收仍受原实际完整焦点/通话状态控制。
Envelope private_context(std::string_view text = "已授权的完整本人文本") {
    Envelope value;
    value.request = false;
    value.token = std::string(64, 'a');
    value.method = "contextSnapshot";
    value.session = "target-human";
    value.message_sequence = 1;
    value.payload =
        R"({"deliveryId":")" + value.token + R"(","consentId":")" + std::string(64, 'b') +
        R"(","scope":"completedText","sourceSessionId":"ai-source","serviceId":"support","serviceRevision":"r1","snapshotVersion":"v1","snapshotDigest":")" +
        std::string(64, 'c') +
        R"(","firstTurnIndex":1,"lastTurnIndex":1,"expiresAt":110000,"turns":[{"messageId":"u1","sequence":1,"turnIndex":1,"role":"user","text":")" +
        std::string(text) + R"("}]})";
    return value;
}
// context_down
// 只通过原宿主真实MQTT队列接MSG，不能直接调用私有接收器绕过路由/整组守卫。
void context_down(Network& network, SharedHandoffHost& host, const Envelope& value,
                  std::uint64_t& now, std::uint64_t& wall) {
    std::string bytes;
    ASSERT_EQ(encode_envelope(value, bytes), DecodeResult::Ok);
    network.events.push_back({MqttEvent::Kind::Message, 0, 0, "$thing/down/message/p/d", bytes});
    ASSERT_TRUE(host.poll(++now, ++wall));
}
// 候选真实整组可先取得，但没有平台Established不得投影业务Active或交付私有内容。
// 资源门控可能已启用，此用例不把WaitingEstablished等同于设备没有采集。
TEST_F(HumanReceiver, ActualWholeFocusBeforeEstablishedDoesNotDeliverBusinessContext) {
    acquire_target();
    EXPECT_EQ(host.handoff_state(), AiHandoffState::Acquired);
    EXPECT_TRUE(std::any_of(network.sent.begin(), network.sent.end(), [](const auto& value) {
        return value.method == "focusState" && value.token == "target-acquire" && value.code == 200;
    }));
    ASSERT_NO_FATAL_FAILURE(context_down(network, host, private_context(), now, wall));
    EXPECT_EQ(host.context_snapshot(), nullptr);
    EXPECT_FALSE(std::any_of(network.sent.begin(), network.sent.end(), [](const auto& value) {
        return value.method == "contextSnapshotAck";
    }));
}

// HumanSourceReceiver 显式开启源意图，平台完整Focus换端围栏仍由真实后端决定。
class HumanSourceReceiver : public HumanReceiver {
   protected:
    HumanSourceReceiver() : HumanReceiver(false, true) {}
    // human_response
    // 沿原Router投递人际创建应答，不能套AI专用VoiceEvent解析预检。
    void human_response(const Envelope &value) {
      std::string bytes;
      ASSERT_EQ(encode_envelope(value, bytes), DecodeResult::Ok);
      network.events.push_back(
          {MqttEvent::Kind::Message, 0, 0, "$thing/down/signaling/p/d", bytes});
      ASSERT_TRUE(step());
    }
    // pure_candidate
    // 只有原创建应答绑定leg，原Peer连通和输出false不代表正式采播授权。
    void pure_candidate() {
      connected();
      ASSERT_TRUE(local(
          R"({"kind":"handoffControl","operation":"dial","targetKind":"device","targetId":"p:other"})"));
      Envelope created;
      created.request = false;
      created.token = network.sent.back().token;
      created.method = "sessionCreated";
      created.session = "target-human";
      created.code = 200;
      created.payload =
          R"({"state":"inviting","callGroupId":"group","selected":{"transport":"webrtc"},"tracks":[{"trackId":"voice","kind":"audio","sourceId":"microphone","sourceType":"microphone","direction":"sendrecv","codec":"opus","state":"requested"}]})";
      human_response(created);
      actual();
      ASSERT_TRUE(step());
      ASSERT_TRUE(local(
          R"({"kind":"humanVoiceOffer","sessionId":"target-human","routeGeneration":"2","peerEpoch":"1","sdp":"v=0\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\na=rtpmap:111 opus/48000/2\r\n"})"));
      ASSERT_TRUE(step());
      ASSERT_EQ(network.sent.back().method, "mediaOffer");
      Envelope answer;
      answer.request = false;
      answer.token = network.sent.back().token;
      answer.method = "mediaAnswer";
      answer.session = "target-human";
      answer.code = 200;
      answer.payload =
          R"({"mediaRevision":1,"webrtc":{"iceMode":"complete","sdp":"v=0\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\na=rtpmap:111 opus/48000/2\r\n"}})";
      down(answer);
      ASSERT_TRUE(local(
          R"({"kind":"humanVoiceAnswered","sessionId":"target-human","routeGeneration":"2","peerEpoch":"1"})"));
      ASSERT_TRUE(local(
          R"({"kind":"humanVoiceConnected","sessionId":"target-human","routeGeneration":"2","peerEpoch":"1"})"));
      ASSERT_TRUE(step());
      ASSERT_EQ(network.sent.back().method, "mediaState");
      answer.token = network.sent.back().token;
      answer.method = "requestAck";
      answer.payload = R"({"requestMethod":"mediaState","result":{}})";
      down(answer);
    }
    // ready_endpoint 通过真实Router及Core把原准备请求推进ready，不授第二组资源。
    void ready_endpoint(bool holdable = false) {
        acquire_target(holdable);
        establish_target();
        ASSERT_TRUE(local(
            R"({"kind":"endpointSourceControl","operation":"prepare","target":"device:p:next"})"));
        const auto prepare = network.sent.back();
        EndpointCommand control;
        std::string bytes;
        ASSERT_EQ(encode_envelope(prepare, bytes), DecodeResult::Ok);
        ASSERT_EQ(decode_endpoint_control(WireRoute::Signaling, bytes, control), WireResult::Ok);
        Envelope ack;
        ack.request = false;
        ack.method = "endpointState";
        ack.session = prepare.session;
        ack.token = prepare.token;
        ack.code = 200;
        ack.payload =
            "{\"state\":\"preparing\",\"revision\":1,\"handoffId\":\"" + control.handoff_id + "\"}";
        down(ack);
        ack.code.reset();
        ack.token = "endpoint-ready";
        ack.event_sequence = 4;
        ack.payload = "{\"state\":\"ready\",\"revision\":2,\"handoffId\":\"" + control.handoff_id +
                      "\",\"candidateSessionId\":\"next-candidate\"}";
        down(ack);
    }
};

// 本机实际源与候选接线才诚实声明新能力，资源operations仍只有原focusControl。
TEST_F(HumanSourceReceiver, ExplicitSDKDeclaresHumanEndpointWithoutBorrowingAIOrOperations) {
    connected();
    const auto cap = std::find_if(network.sent.begin(), network.sent.end(), [](const auto& value) {
        return value.method == "capabilityReport";
    });
    ASSERT_NE(cap, network.sent.end());
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_Parse(cap->payload.c_str()), cJSON_Delete);
    const auto* features = cJSON_GetObjectItemCaseSensitive(root.get(), "features");
    ASSERT_TRUE(cJSON_IsArray(features));
    bool human = false, ai = false;
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, features) {
        ASSERT_TRUE(cJSON_IsString(item));
        human |= std::string_view(item->valuestring) == "humanEndpointHandoff";
        ai |= std::string_view(item->valuestring) == "aiEndpointHandoff";
    }
    EXPECT_TRUE(human);
    EXPECT_FALSE(ai);
    const auto* controls = cJSON_GetObjectItemCaseSensitive(root.get(), "controlCapabilities");
    const auto* operations = cJSON_GetObjectItemCaseSensitive(controls, "operations");
    ASSERT_EQ(cJSON_GetArraySize(operations), 1);
    EXPECT_STREQ(cJSON_GetArrayItem(operations, 0)->valuestring, "focusControl");
}

// 默认旧Human宿主不声明新能力，平台继续发送原七字段邀请。
TEST_F(HumanReceiver, DefaultSDKDoesNotClaimHumanEndpointHandoff) {
    connected();
    const auto cap = std::find_if(network.sent.begin(), network.sent.end(), [](const auto& value) {
        return value.method == "capabilityReport";
    });
    ASSERT_NE(cap, network.sent.end());
    EXPECT_EQ(cap->payload.find("humanEndpointHandoff"), std::string::npos);
}

// 候选实际完整Focus、原媒体与本人接听都成立后，原提交CAS才成为新源的请求水位。
TEST_F(HumanSourceReceiver, FormalCandidateWholeFocusAndOriginalCommitBecomeNewSource) {
    acquire_target(false, false, true);
    EXPECT_FALSE(local(R"({"kind":"endpointSourceControl","operation":"prepare","target":"device:p:next"})"));
    establish_target();
    EXPECT_FALSE(local(R"({"kind":"endpointSourceControl","operation":"prepare","target":"device:p:next"})"));
    Envelope commit;
    commit.request = false;
    commit.method = "endpointState";
    commit.token = "incoming-commit";
    commit.session = "target-human";
    commit.event_sequence = 4;
    commit.payload = R"({"state":"committed","revision":3,"handoffId":"incoming-original-handoff","candidateSessionId":"target-human"})";
    auto wrong = commit;
    wrong.payload = R"({"state":"committed","revision":3,"handoffId":"incoming-original-handoff","candidateSessionId":"other-human"})";
    down(wrong);
    EXPECT_FALSE(local(R"({"kind":"endpointSourceControl","operation":"prepare","target":"device:p:next"})"));
    down(commit);
    const auto gates = outputs.size();
    ASSERT_TRUE(local(R"({"kind":"endpointSourceControl","operation":"prepare","target":"device:p:next"})"));
    EndpointCommand control;
    std::string bytes;
    ASSERT_EQ(encode_envelope(network.sent.back(), bytes), DecodeResult::Ok);
    ASSERT_EQ(decode_endpoint_control(WireRoute::Signaling, bytes, control), WireResult::Ok);
    EXPECT_EQ(control.expected_revision, 3U);
    EXPECT_EQ(control.envelope.session, "target-human");
    EXPECT_NE(control.handoff_id, "incoming-original-handoff");
    EXPECT_EQ(outputs.size(), gates); // 源意图不排新HAL门控，也不提前投影平台完成。
    down(commit); // 当前prepare在途，原已核commit重放不得清新事务或改水位。
    EXPECT_FALSE(local(R"({"kind":"endpointSourceControl","operation":"commit"})"));
}

// 显式纯Human源使用原sessionCreate拨号；本地本人意图只预约候选，不能启动AI或直接取得采播。
TEST_F(HumanSourceReceiver,
       PureHumanDialPublishesOriginalCreateWithoutAiOrAcquisition) {
  connected();
  const auto before = network.sent.size();
  ASSERT_TRUE(local(
      R"({"kind":"handoffControl","operation":"dial","targetKind":"device","targetId":"p:other"})"));
  ASSERT_EQ(network.sent.size(), before + 1);
  EXPECT_EQ(network.sent.back().method, "sessionCreate");
  EXPECT_NE(network.sent.back().payload.find("humanCall"), std::string::npos);
  EXPECT_EQ(network.sent.back().payload.find("aiConversation"),
            std::string::npos);
  EXPECT_FALSE(media.configured);
  EXPECT_FALSE(media.capture);
  EXPECT_EQ(host.voice_state(), VoiceState::Idle);
  EXPECT_FALSE(
      std::any_of(outputs.begin(), outputs.end(), [](const auto &value) {
        return (value.find("inputOwnershipGate") != std::string::npos ||
                value.find("humanVoiceOutputGate") != std::string::npos) &&
               value.find("\"enabled\":true") != std::string::npos;
      }));
  EXPECT_FALSE(local(
      R"({"kind":"handoffControl","operation":"dial","targetKind":"device","targetId":"p:another"})"));
}

// 纯主叫严格经历原创建/媒体/完整焦点actual，原建立事件即使早到也不能提前授采播或接续。
TEST_F(HumanSourceReceiver,
       PureHumanCreatedMediaAndActualWholeFocusEnableEndpointSource) {
  pure_candidate();
  EXPECT_FALSE(local(
      R"({"kind":"endpointSourceControl","operation":"prepare","target":"device:p:next"})"));
  EXPECT_FALSE(
      std::any_of(outputs.begin(), outputs.end(), [](const auto &value) {
        return value.find("\"enabled\":true") != std::string::npos;
      }));
  Envelope established;
  established.request = false;
  established.token = "pure-caller-established";
  established.method = "sessionEstablished";
  established.session = "target-human";
  established.event_sequence = 1;
  established.payload = R"({"state":"established","mediaRevision":1})";
  down(established);
  EXPECT_FALSE(local(
      R"({"kind":"endpointSourceControl","operation":"prepare","target":"device:p:next"})"));
  down(control());
  EXPECT_FALSE(std::any_of(
      network.sent.begin(), network.sent.end(), [](const auto &value) {
        return value.method == "focusState" && value.code == 200;
      }));
  actual();
  ASSERT_TRUE(step());
  actual();
  ASSERT_TRUE(step());
  EXPECT_EQ(host.handoff_state(), AiHandoffState::HumanActive);
  ASSERT_TRUE(local(
      R"({"kind":"endpointSourceControl","operation":"prepare","target":"device:p:next"})"));
  EXPECT_EQ(network.sent.back().method, "endpointControl");
  EXPECT_FALSE(media.configured);
  EXPECT_FALSE(media.capture);
}

// 原session已绑定而远端尚未接听时，本人取消仍须排原sessionEnd并永久撤销本腿采播。
TEST_F(HumanSourceReceiver, PureHumanBoundOutboundCanCancelBeforeAnswer) {
    pure_candidate();
    ASSERT_TRUE(local(R"({"kind":"handoffControl","operation":"cancel"})"));
    ASSERT_EQ(network.sent.back().method, "sessionEnd");
    EXPECT_EQ(network.sent.back().session, "target-human");
    EXPECT_FALSE(local(R"({"kind":"handoffControl","operation":"cancel"})"));
    std::string late;
    ASSERT_EQ(encode_envelope(control(), late), DecodeResult::Ok);
    network.events.push_back({MqttEvent::Kind::Message, 0, 0,
                              "$thing/down/signaling/p/d", late});
    EXPECT_FALSE(step()); // 原已关闭本人意图的迟到acquire沿既有围栏失败，不能重新取得采播。
    actual();
    EXPECT_FALSE(step());
    EXPECT_FALSE(std::any_of(outputs.begin(), outputs.end(), [](const auto& value) {
        return value.find("\"enabled\":true") != std::string::npos;
    }));
}

// 未收到原创建应答不能凭空构造sessionEnd；取消拒绝仍保留原请求，不能丢弃迟到创建。
TEST_F(HumanSourceReceiver, PureHumanUnboundOutboundCannotInventCancellationSession) {
    connected();
    ASSERT_TRUE(local(R"({"kind":"handoffControl","operation":"dial","targetKind":"device","targetId":"p:other"})"));
    const auto token = network.sent.back().token;
    const auto before = network.sent.size();
    EXPECT_FALSE(local(R"({"kind":"handoffControl","operation":"cancel"})"));
    EXPECT_EQ(network.sent.size(), before);
    Envelope created;
    created.request = false;
    created.token = token;
    created.method = "sessionCreated";
    created.session = "target-human";
    created.code = 200;
    created.payload = R"({"state":"inviting","callGroupId":"group","selected":{"transport":"webrtc"},"tracks":[{"trackId":"voice","kind":"audio","sourceId":"microphone","sourceType":"microphone","direction":"sendrecv","codec":"opus","state":"requested"}]})";
    human_response(created);
    ASSERT_TRUE(local(R"({"kind":"handoffControl","operation":"cancel"})"));
    EXPECT_EQ(network.sent.back().method, "sessionEnd");
    EXPECT_EQ(network.sent.back().session, "target-human");
}

// 旧受话入口不因新增纯源能力被升级为可主动拨号。
TEST_F(HumanReceiver, OldReceiveOnlyStillCannotDial) {
  connected();
  const auto before = network.sent.size();
  EXPECT_FALSE(local(
      R"({"kind":"handoffControl","operation":"dial","targetKind":"device","targetId":"p:other"})"));
  EXPECT_EQ(network.sent.size(), before);
  EXPECT_FALSE(media.configured);
}

// 本机控制不接纳外部会话/服务目标或已撤配对；它们不能产生第二原请求。
TEST_F(HumanSourceReceiver,
       PureHumanDialRejectsUntrustedScopeTargetAndLostPairing) {
  connected();
  const auto before = network.sent.size();
  EXPECT_FALSE(local(
      R"({"kind":"handoffControl","operation":"dial","targetKind":"service","targetId":"p:other"})"));
  EXPECT_FALSE(local(
      R"({"kind":"handoffControl","operation":"dial","targetKind":"device","targetId":"p:other","sessionId":"forged"})"));
  paired = false;
  EXPECT_FALSE(local(
      R"({"kind":"handoffControl","operation":"dial","targetKind":"device","targetId":"p:other"})"));
  EXPECT_EQ(network.sent.size(), before);
}
// 可信双时钟回退与宿主原截止不允许发新sessionCreate；拒绝不得消耗可用原请求或引入AI。
TEST_F(HumanSourceReceiver, PureHumanDialClockRollbackDoesNotPublish) {
  connected();
  const auto before = network.sent.size();
  const std::string request =
      R"({"kind":"handoffControl","operation":"dial","targetKind":"device","targetId":"p:other"})";
  EXPECT_FALSE(host.receive_local(request, now - 1, wall));
  EXPECT_FALSE(host.receive_local(request, now, wall - 1));
  EXPECT_EQ(network.sent.size(), before);
  EXPECT_TRUE(local(request));
  EXPECT_EQ(network.sent.size(), before + 1);
}
// 宿主自己已到期时，即使原MQTT曾经Ready也不能创建新通话。
TEST_F(HumanSourceReceiver, PureHumanDialExpiredHostDoesNotPublish) {
  connected();
  const auto before = network.sent.size();
  EXPECT_FALSE(host.receive_local(
      R"({"kind":"handoffControl","operation":"dial","targetKind":"device","targetId":"p:other"})",
      500000, wall));
  EXPECT_EQ(network.sent.size(), before);
    EXPECT_FALSE(media.configured);
}
// 原明确创建拒绝只退本次本人意图；相同旧拒绝重放不能消耗或取消后继原创建。
TEST_F(HumanSourceReceiver, PureHumanCreateRejectionPreservesNextRequestAndIdleResources) {
    connected();
    const std::string request = R"({"kind":"handoffControl","operation":"dial","targetKind":"device","targetId":"p:other"})";
    ASSERT_TRUE(local(request));
    const auto first = network.sent.back();
    Envelope rejected;
    rejected.request = false;
    rejected.method = "sessionCreated";
    rejected.token = first.token;
    rejected.code = 409;
    rejected.payload = R"({"reason":"busy"})";
    human_response(rejected);
    ASSERT_TRUE(local(request));
    const auto second = network.sent.back();
    EXPECT_NE(first.token, second.token);
    human_response(rejected);
    EXPECT_FALSE(local(request));
    EXPECT_FALSE(media.configured);
    EXPECT_FALSE(media.capture);
    EXPECT_FALSE(std::any_of(outputs.begin(), outputs.end(), [](const auto& value) {
        return value.find("\"enabled\":true") != std::string::npos;
    }));
}
// 发布背压保留原token重传，不伪装为创建拒绝、生成新请求或启动未授权媒体。
TEST_F(HumanSourceReceiver, PureHumanCreateBackpressureKeepsOriginalToken) {
    connected();
    network.reject_human_create_publish = true;
    ASSERT_TRUE(local(R"({"kind":"handoffControl","operation":"dial","targetKind":"device","targetId":"p:other"})"));
    ASSERT_EQ(network.refused_human_creates.size(), 1U);
    const auto original = network.refused_human_creates.back();
    EXPECT_FALSE(local(R"({"kind":"handoffControl","operation":"dial","targetKind":"device","targetId":"p:other"})"));
    network.reject_human_create_publish = false;
    now += 10000;
    ASSERT_TRUE(step());
    const auto retried = std::find_if(network.sent.begin(), network.sent.end(), [&](const auto& value) {
        return value.method == "sessionCreate" && value.token == original.token;
    });
    ASSERT_NE(retried, network.sent.end());
    EXPECT_EQ(retried->payload, original.payload);
    EXPECT_FALSE(media.configured);
}

// 早到平台事件不能越过实际整组确认；原同字节稍后重投才可沿当前水位成为新源。
TEST_F(HumanSourceReceiver, EarlyEstablishedAndCommitDoNotSkipActualCandidateWholeFocus) {
    prepare(true, true, true);
    establish_target(); // 原Host只保留早到Established，不凭平台事件取得实际资源。
    Envelope commit;
    commit.request = false;
    commit.method = "endpointState";
    commit.token = "incoming-commit";
    commit.session = "target-human";
    commit.event_sequence = 4;
    commit.payload = R"({"state":"committed","revision":3,"handoffId":"incoming-original-handoff","candidateSessionId":"target-human"})";
    down(commit);
    EXPECT_FALSE(local(R"({"kind":"endpointSourceControl","operation":"prepare","target":"device:p:next"})"));
    down(control());
    EXPECT_FALSE(std::any_of(network.sent.begin(), network.sent.end(), [](const auto& value) {
        return value.method == "focusState" && value.token == "target-acquire" && value.code == 200;
    }));
    actual();
    ASSERT_TRUE(step());
    EXPECT_FALSE(local(R"({"kind":"endpointSourceControl","operation":"prepare","target":"device:p:next"})"));
    actual();
    ASSERT_TRUE(step());
    EXPECT_FALSE(local(R"({"kind":"endpointSourceControl","operation":"prepare","target":"device:p:next"})"));
    down(commit);
    ASSERT_TRUE(local(R"({"kind":"endpointSourceControl","operation":"prepare","target":"device:p:next"})"));
    EndpointCommand control;
    std::string bytes;
    ASSERT_EQ(encode_envelope(network.sent.back(), bytes), DecodeResult::Ok);
    ASSERT_EQ(decode_endpoint_control(WireRoute::Signaling, bytes, control), WireResult::Ok);
    EXPECT_EQ(control.expected_revision, 3U);
}

// 本人只停原输入后仍可撤候选；真实false票据不释放完整组、不停或恢复原输出。
TEST_F(HumanSourceReceiver, ActualUserInputStopStillAbortsWithoutNewFocusOrOutputChange) {
    ready_endpoint();
    const auto old_input = std::find_if(outputs.rbegin(), outputs.rend(), [](const auto& body) {
        return body.find("inputOwnershipGate") != std::string::npos;
    });
    ASSERT_NE(old_input, outputs.rend());
    auto late_true = *old_input;
    late_true.replace(late_true.find("inputOwnershipGate"), 18, "inputOwnershipSenderGated");
    const auto output_count = std::count_if(outputs.begin(), outputs.end(), [](const auto& body) {
        return body.find("humanVoiceOutputGate") != std::string::npos;
    });
    const auto acquire_count = std::count_if(
        network.sent.begin(), network.sent.end(),
        [](const auto& value) { return value.method == "focusState" && value.code == 200; });
    ASSERT_TRUE(local(R"({"kind":"humanInputControl","operation":"stop"})"));
    EXPECT_FALSE(local(
        R"({"kind":"endpointSourceControl","operation":"abort"})"));  // 未actual false不能先撤。
    actual("inputOwnershipGate");
    ASSERT_TRUE(step());
    EXPECT_TRUE(
        local(late_true));  // 既有历史actual回执可幂等重投，但不改变当前最新false票据或恢复采集。
    EXPECT_TRUE(std::any_of(network.sent.begin(), network.sent.end(), [](const auto& value) {
        return value.method == "trackControl" && value.payload.find("pause") != std::string::npos;
    }));
    EXPECT_FALSE(local(R"({"kind":"endpointSourceControl","operation":"commit"})"));
    ASSERT_TRUE(local(R"({"kind":"endpointSourceControl","operation":"abort"})"));
    EXPECT_EQ(network.sent.back().method, "endpointControl");
    EXPECT_EQ(host.handoff_state(), AiHandoffState::HumanActive);
    EXPECT_EQ(std::count_if(outputs.begin(), outputs.end(),
                            [](const auto& body) {
                                return body.find("humanVoiceOutputGate") != std::string::npos;
                            }),
              output_count);
    EXPECT_EQ(std::count_if(network.sent.begin(), network.sent.end(),
                            [](const auto& value) {
                                return value.method == "focusState" && value.code == 200;
                            }),
              acquire_count);
}

// 本人实际停输入后，原完整组保持/恢复只恢复资源及输出，不得重开本人采集。
TEST_F(HumanSourceReceiver, ActualUserInputStopSurvivesFormalSuspendAndRestore) {
    ready_endpoint(true);
    ASSERT_TRUE(local(R"({"kind":"humanInputControl","operation":"stop"})"));
    actual("inputOwnershipGate");
    ASSERT_TRUE(step());
    ASSERT_TRUE(local(R"({"kind":"endpointSourceControl","operation":"abort"})"));
    const auto abort = network.sent.back();
    EndpointCommand control;
    std::string bytes;
    ASSERT_EQ(encode_envelope(abort, bytes), DecodeResult::Ok);
    ASSERT_EQ(decode_endpoint_control(WireRoute::Signaling, bytes, control), WireResult::Ok);
    Envelope ack;
    ack.request = false;
    ack.method = "endpointState";
    ack.token = abort.token;
    ack.session = abort.session;
    ack.code = 200;
    ack.payload = "{\"state\":\"aborted\",\"revision\":3,\"handoffId\":\"" +
                  control.handoff_id + "\",\"candidateSessionId\":\"next-candidate\"}";
    down(ack);
    down(hold_focus(true, "original-hold", 1, 5));
    actual();
    ASSERT_TRUE(step());
    down(hold_state(true, "original-hold", 1, 6));
    const auto start = outputs.size();
    down(hold_focus(false, "original-resume", 2, 7));
    actual();
    ASSERT_TRUE(step());
    ASSERT_TRUE(std::any_of(network.sent.begin(), network.sent.end(), [](const auto& event) {
        return event.method == "focusState" && event.token == "original-resume" && event.code == 200;
    }));
    down(hold_state(false, "original-resume", 2, 8));
    for (auto it = outputs.begin() + static_cast<std::ptrdiff_t>(start); it != outputs.end(); ++it) {
        std::unique_ptr<cJSON, decltype(&cJSON_Delete)> json(cJSON_Parse(it->c_str()), cJSON_Delete);
        const auto* kind = cJSON_GetObjectItemCaseSensitive(json.get(), "kind");
        const auto* enabled = cJSON_GetObjectItemCaseSensitive(json.get(), "enabled");
        if (cJSON_IsString(kind) && std::string_view(kind->valuestring) == "inputOwnershipGate") {
            EXPECT_FALSE(cJSON_IsTrue(enabled)) << *it;
        }
    }
    EXPECT_FALSE(local(R"({"kind":"endpointSourceControl","operation":"prepare","target":"device:p:next"})"));
    EXPECT_EQ(host.handoff_state(), AiHandoffState::HumanActive);
}

// HumanSourceCaller 仅显式安装本人源接口，不修改平台焦点或原主叫三屏障。
class HumanSourceCaller : public SharedHost {
protected:
    HumanSourceCaller() : SharedHost(false, false, false, false, true) {}
};
// AI宿主只有源接口但关闭普通候选入口时，不能向平台虚报完整Human接续能力。
TEST_F(HumanSourceCaller, SourceOnlyAIHostDoesNotClaimCompleteCandidateFeature) {
    connected();
    const auto cap = std::find_if(network.sent.begin(), network.sent.end(), [](const auto& value) {
        return value.method == "capabilityReport";
    });
    ASSERT_NE(cap, network.sent.end());
    EXPECT_EQ(cap->payload.find("humanEndpointHandoff"), std::string::npos);
}

// AI宿主明确开放原普通候选入口后才具有完整源/候选SDK能力，不新增抢占旧AI的入口。
class HumanSourceTemporaryCaller : public SharedHost {
protected:
    HumanSourceTemporaryCaller() : SharedHost(false, false, true, false, true) {}
};
TEST_F(HumanSourceTemporaryCaller, OriginalCandidateAdmissionAndSourceDeclareCompleteFeature) {
    connected();
    const auto cap = std::find_if(network.sent.begin(), network.sent.end(), [](const auto& value) {
        return value.method == "capabilityReport";
    });
    ASSERT_NE(cap, network.sent.end());
    EXPECT_NE(cap->payload.find("humanEndpointHandoff"), std::string::npos);
}
// 原主叫AI已正式退役后的人际腿也保留本人停输入意图，不借平台restore重新采集。
TEST_F(HumanSourceCaller, ActualCallerHumanStopSurvivesOriginalFocusRestore) {
    ASSERT_NO_FATAL_FAILURE(commit_human(true));
    Envelope focus;
    focus.method = "focusControl";
    focus.token = "release-original-ai";
    focus.session = "ai";
    focus.event_sequence = 6;
    focus.payload = R"({"operation":"release","expectedRevision":3,"focusId":"ai-focus","resources":["microphone","speaker"],"priority":"conversation","resumePolicy":"explicit"})";
    down(focus);
    actual();
    ASSERT_TRUE(step());
    ASSERT_TRUE(local(R"({"kind":"humanInputControl","operation":"stop"})"));
    actual("inputOwnershipGate");
    ASSERT_TRUE(step());
    focus.session = "local-human";
    focus.token = "caller-hold";
    focus.event_sequence = 4;
    focus.payload = R"({"operation":"suspend","expectedRevision":4,"focusId":"human-focus","resources":["microphone","speaker"],"priority":"call","resumePolicy":"explicit"})";
    down(focus);
    actual();
    ASSERT_TRUE(step());
    Envelope held;
    held.request = false;
    held.method = "callState";
    held.token = "caller-held";
    held.session = "local-human";
    held.event_sequence = 5;
    held.payload = R"({"state":"held","revision":1,"requestToken":"caller-hold"})";
    down(held);
    const auto start = outputs.size();
    focus.token = "caller-resume";
    focus.event_sequence = 6;
    focus.payload = R"({"operation":"restore","expectedRevision":5,"focusId":"human-focus","resources":["microphone","speaker"],"priority":"call","resumePolicy":"explicit","expiresAt":200000})";
    down(focus);
    actual();
    ASSERT_TRUE(step());
    ASSERT_TRUE(std::any_of(network.sent.begin(), network.sent.end(), [](const auto& event) {
        return event.method == "focusState" && event.token == "caller-resume" && event.code == 200;
    }));
    for (auto it = outputs.begin() + static_cast<std::ptrdiff_t>(start); it != outputs.end(); ++it) {
        std::unique_ptr<cJSON, decltype(&cJSON_Delete)> json(cJSON_Parse(it->c_str()), cJSON_Delete);
        const auto* kind = cJSON_GetObjectItemCaseSensitive(json.get(), "kind");
        const auto* enabled = cJSON_GetObjectItemCaseSensitive(json.get(), "enabled");
        if (cJSON_IsString(kind) && std::string_view(kind->valuestring) == "inputOwnershipGate") {
            EXPECT_FALSE(cJSON_IsTrue(enabled)) << *it;
        }
    }
}

// 页面只提交目标意图；实际完整组/Established缺一不可，不接网页手填许可水位。
TEST_F(HumanSourceReceiver, OriginalWholeFocusAndEstablishedGateSourceRequests) {
    acquire_target();
    EXPECT_FALSE(local(
        R"({"kind":"endpointSourceControl","operation":"prepare","target":"device:p:next"})"));
    establish_target();
    const auto original_gates = std::count_if(outputs.begin(), outputs.end(), [](const auto& body) {
        return body.find("inputOwnershipGate") != std::string::npos ||
               body.find("humanVoiceOutputGate") != std::string::npos;
    });
    EXPECT_FALSE(local(
        R"({"kind":"endpointSourceControl","operation":"prepare","target":"device:p:next","revision":1})"));
    ASSERT_TRUE(local(
        R"({"kind":"endpointSourceControl","operation":"prepare","target":"device:p:next"})"));
    ASSERT_EQ(network.sent.back().method, "endpointControl");
    const auto prepare = network.sent.back();
    Envelope ack;
    ack.request = false;
    ack.method = "endpointState";
    ack.session = prepare.session;
    ack.token = prepare.token;
    ack.code = 200;
    EndpointCommand control;
    std::string bytes;
    ASSERT_EQ(encode_envelope(prepare, bytes), DecodeResult::Ok);
    ASSERT_EQ(decode_endpoint_control(WireRoute::Signaling, bytes, control), WireResult::Ok);
    ack.payload =
        "{\"state\":\"preparing\",\"revision\":1,\"handoffId\":\"" + control.handoff_id + "\"}";
    down(ack);  // 正式Shared Router必须按endpointControl原token找到Human，不可直接注入Core。
    auto ready = ack;
    ready.code.reset();
    ready.token = "endpoint-ready";
    ready.event_sequence = 4;
    ready.payload = "{\"state\":\"ready\",\"revision\":2,\"handoffId\":\"" + control.handoff_id +
                    "\",\"candidateSessionId\":\"next-candidate\"}";
    down(ready);
    ASSERT_TRUE(local(R"({"kind":"endpointSourceControl","operation":"commit"})"));
    EXPECT_EQ(network.sent.back().method, "endpointControl");
    EXPECT_EQ(host.handoff_state(), AiHandoffState::HumanActive);
    EXPECT_EQ(std::count_if(outputs.begin(), outputs.end(),
                            [](const auto& body) {
                                return body.find("inputOwnershipGate") != std::string::npos ||
                                       body.find("humanVoiceOutputGate") != std::string::npos;
                            }),
              original_gates);
}

// 默认配置不开放源；客户端按钮或伪scope不能为旧宿主添加源权限。
TEST_F(HumanReceiver, UnconfiguredEndpointSourceNeverCreatesRequest) {
    acquire_target();
    establish_target();
    const auto sent = network.sent.size();
    EXPECT_FALSE(local(
        R"({"kind":"endpointSourceControl","operation":"prepare","target":"device:p:next"})"));
    EXPECT_EQ(network.sent.size(), sent);
}
// 平台先正式释放再发sessionEnded；被叫不得依赖先收到终态才能接受原停止命令。
TEST_F(HumanReceiver, PlatformReleaseBeforeTerminalRequiresActualWholeGroup) {
    acquire_target();
    establish_target();
    Envelope release = control("release", 1, 4);
    release.payload =
        R"({"operation":"release","expectedRevision":1,"focusId":"target-focus","resources":["microphone","speaker"],"priority":"call","resumePolicy":"never"})";
    down(release);
    // Broker下发及本地停止请求不能代替原输入/输出false票据。
    EXPECT_FALSE(std::any_of(network.sent.begin(), network.sent.end(), [](const auto& value) {
        return value.method == "focusState" && value.token == "target-release" && value.code == 200;
    }));
    for (unsigned round = 0; round < 4; ++round) {
        actual();
        ASSERT_TRUE(step());
    }
    const auto receipt =
        std::find_if(network.sent.begin(), network.sent.end(), [](const auto& value) {
            return value.method == "focusState" && value.token == "target-release";
        });
    ASSERT_NE(receipt, network.sent.end());
    EXPECT_EQ(receipt->code, 200);
    FocusState fact;
    std::string bytes;
    ASSERT_EQ(encode_envelope(*receipt, bytes), DecodeResult::Ok);
    ASSERT_EQ(decode_focus_state(WireRoute::Signaling, bytes, fact), WireResult::Ok);
    EXPECT_EQ(fact.state, "released");
    EXPECT_EQ(fact.revision, 2U);
    EXPECT_EQ(network.disconnects, 0U);
}

// 已接听候选真实取得完整组后仍未Established；原阶段超时只能撤业务资格，不能丢失原资源清理资格。
// 所有ACK均来自生产Host/FocusEngine收录原线程实际停止票据，不将超时或媒体close冒作released。
TEST_F(HumanSourceReceiver, CandidatePhaseTimeoutStillAcknowledgesOnlyOriginalActualRelease) {
    acquire_target(false, false, true);
    ASSERT_TRUE(std::any_of(network.sent.begin(), network.sent.end(), [](const auto &value) {
        return value.method == "focusState" && value.token == "target-acquire" && value.code == 200;
    }));
    EXPECT_FALSE(local(
        R"({"kind":"endpointSourceControl","operation":"prepare","target":"device:p:next"})"));
    now += 30001;
    wall += 30001;
    ASSERT_TRUE(step());
    actual();
    ASSERT_TRUE(step());
    EXPECT_FALSE(local(
        R"({"kind":"endpointSourceControl","operation":"prepare","target":"device:p:next"})"));
    Envelope release = control("release", 1, 3);
    release.payload =
        R"({"operation":"release","expectedRevision":1,"focusId":"target-focus","resources":["microphone","speaker"],"priority":"call","resumePolicy":"never"})";
    down(release);
    for (unsigned round = 0; round < 4; ++round) {
        actual();
        ASSERT_TRUE(step());
    }
    const auto receipt =
        std::find_if(network.sent.begin(), network.sent.end(), [](const auto &value) {
            return value.method == "focusState" && value.token == "target-release" &&
                   value.code == 200;
        });
    ASSERT_NE(receipt, network.sent.end());
    FocusState fact;
    std::string bytes;
    ASSERT_EQ(encode_envelope(*receipt, bytes), DecodeResult::Ok);
    ASSERT_EQ(decode_focus_state(WireRoute::Signaling, bytes, fact), WireResult::Ok);
    EXPECT_EQ(fact.state, "released");
    EXPECT_EQ(fact.revision, 2U);
    EXPECT_EQ(network.disconnects, 0U);
    const auto original_receipt = bytes;
    down(release);
    actual();
    ASSERT_TRUE(step());
    std::size_t replay_count = 0;
    for (const auto &value : network.sent) {
        if (value.method != "focusState" || value.token != "target-release")
            continue;
        std::string replay;
        ASSERT_EQ(encode_envelope(value, replay), DecodeResult::Ok);
        EXPECT_EQ(replay, original_receipt);
        ++replay_count;
    }
    EXPECT_EQ(replay_count, 2U);
    Envelope ended;
    ended.request = false;
    ended.token = "candidate-original-ended";
    ended.method = "sessionEnded";
    ended.session = "target-human";
    ended.event_sequence = 4;
    ended.payload = R"({"reason":"hangup"})";
    down(ended);
    actual();
    EXPECT_EQ(network.disconnects, 0U);
}

// 原平台给已取得候选发ready水位，然后撤销排下一release；ready不授权新源，也不等待Established。
TEST_F(HumanSourceReceiver, CandidateReadyWatermarkAllowsOriginalAbortReleaseAfterPhaseTimeout) {
    acquire_target(false, false, true);
    Envelope ready;
    ready.request = false;
    ready.token = "incoming-ready";
    ready.method = "endpointState";
    ready.session = "target-human";
    ready.event_sequence = 3;
    ready.payload =
        R"({"state":"ready","revision":2,"handoffId":"incoming-original-handoff","candidateSessionId":"target-human"})";
    down(ready);
    EXPECT_FALSE(local(
        R"({"kind":"endpointSourceControl","operation":"prepare","target":"device:p:next"})"));
    now += 30001;
    wall += 30001;
    ASSERT_TRUE(step());
    actual();
    ASSERT_TRUE(step());
    auto release = control("release", 1, 4);
    release.payload =
        R"({"operation":"release","expectedRevision":1,"focusId":"target-focus","resources":["microphone","speaker"],"priority":"call","resumePolicy":"never"})";
    down(release);
    for (unsigned round = 0; round < 4; ++round) {
        actual();
        ASSERT_TRUE(step());
    }
    EXPECT_TRUE(std::any_of(network.sent.begin(), network.sent.end(), [](const auto& value) {
        return value.method == "focusState" && value.token == "target-release" && value.code == 200;
    }));
    EXPECT_EQ(network.disconnects, 0U);
    EXPECT_FALSE(local(
        R"({"kind":"endpointSourceControl","operation":"prepare","target":"device:p:next"})"));
}
// CandidateAbortReceiver 只开原诊断出口以核终态，不新增驱动或许可入口。
class CandidateAbortReceiver : public HumanReceiver {
protected:
    CandidateAbortReceiver() : HumanReceiver(true, true) {}
    // abort_candidate
    // 复用正式ready/release/aborted/Ended顺序；超时分支仍要求原两张实际停止票据。
    void abort_candidate(bool timeout) {
        acquire_target(false, false, true);
        Envelope ready;
        ready.request = false;
        ready.token = "incoming-ready";
        ready.method = "endpointState";
        ready.session = "target-human";
        ready.event_sequence = 3;
        ready.payload =
            R"({"state":"ready","revision":2,"handoffId":"incoming-original-handoff","candidateSessionId":"target-human"})";
        down(ready);
        down(ready); // 原同字节重投不能把水位再加一或授予新源。
        EXPECT_FALSE(local(
            R"({"kind":"endpointSourceControl","operation":"prepare","target":"device:p:next"})"));
        if (timeout) {
            now += 30001;
            wall += 30001;
            ASSERT_TRUE(step());
            actual();
            ASSERT_TRUE(step());
        }
        auto release = control("release", 1, 4);
        release.payload =
            R"({"operation":"release","expectedRevision":1,"focusId":"target-focus","resources":["microphone","speaker"],"priority":"call","resumePolicy":"never"})";
        down(release);
        for (unsigned round = 0; round < 4; ++round) {
            actual();
            ASSERT_TRUE(step());
        }
        ASSERT_TRUE(std::any_of(network.sent.begin(), network.sent.end(), [](const auto& value) {
            return value.method == "focusState" && value.token == "target-release" &&
                   value.code == 200;
        }));
        Envelope aborted = ready;
        aborted.token = "incoming-aborted";
        aborted.event_sequence = 5;
        aborted.payload =
            R"({"state":"aborted","revision":3,"handoffId":"incoming-original-handoff","reason":"userCancelled"})";
        down(aborted);
        Envelope ended;
        ended.request = false;
        ended.token = "incoming-ended";
        ended.method = "sessionEnded";
        ended.session = "target-human";
        ended.event_sequence = 6;
        ended.payload = R"({"state":"ended","reason":"userCancelled"})";
        down(ended);
        EXPECT_EQ(host.human_signal_diagnostic_line(),
                  R"({"kind":"humanSignalDiagnostic","state":"available","failure":")" +
                      std::string(timeout ? "phaseDeadline" : "none") + "\"}");
        EXPECT_EQ(network.disconnects, 0U);
        EXPECT_FALSE(local(
            R"({"kind":"endpointSourceControl","operation":"prepare","target":"device:p:next"})"));
    }
};
TEST_F(CandidateAbortReceiver, CandidateReadyReleaseAbortedEndedBeforePhaseTimeout) {
    abort_candidate(false);
}
TEST_F(CandidateAbortReceiver, CandidateReadyReleaseAbortedEndedAfterPhaseTimeout) {
    abort_candidate(true);
}
// 原业务已失效仍只清理同一实际租约；不借平台新release续播放或重建已停止scope。
TEST_F(HumanReceiver, OriginalBusinessExpiryAllowsOnlyActualReleaseCleanup) {
    acquire_target();
    establish_target();
    wall = 200000;  // 达到原acquire绝对墙钟截止，原宿主600000截止仍不变。
    ASSERT_TRUE(step());
    actual();
    ASSERT_TRUE(step());
    Envelope release = control("release", 1, 4);
    release.payload =
        R"({"operation":"release","expectedRevision":1,"focusId":"target-focus","resources":["microphone","speaker"],"priority":"call","resumePolicy":"never"})";
    down(release);
    for (unsigned round = 0; round < 4; ++round) {
        actual();
        ASSERT_TRUE(step());
    }
    const auto receipt =
        std::find_if(network.sent.begin(), network.sent.end(), [](const auto& value) {
            return value.method == "focusState" && value.token == "target-release";
        });
    ASSERT_NE(receipt, network.sent.end());
    EXPECT_EQ(receipt->code, 200);
    EXPECT_EQ(network.disconnects, 0U);
}

// 清理超时不能把随后真实停止洗成成功，也不能用同原字节重投刷新两秒期限。
TEST_F(HumanReceiver, CleanupTimeoutAndReplayNeverForgeActualRelease) {
    acquire_target();
    establish_target();
    auto release = control("release", 1, 4);
    release.payload =
        R"({"operation":"release","expectedRevision":1,"focusId":"target-focus","resources":["microphone","speaker"],"priority":"call","resumePolicy":"never"})";
    down(release);
    const auto pending_input = std::find_if(outputs.rbegin(), outputs.rend(), [](const auto& body) {
        return body.find("inputOwnershipGate") != std::string::npos;
    });
    ASSERT_NE(pending_input, outputs.rend());
    auto late_input = *pending_input;
    late_input.replace(late_input.find("inputOwnershipGate"), 18, "inputOwnershipSenderGated");
    now += 2000;
    wall += 2000;
    EXPECT_FALSE(step());  // 原HAL未实际停止，短清理超时沿既有失败终态关闭，不续许可。
    EXPECT_FALSE(local(late_input)); // 超时后的原实际回调亦不能复活已关闭宿主。
    EXPECT_FALSE(step());
    std::string replay_bytes;
    ASSERT_EQ(encode_envelope(release, replay_bytes), DecodeResult::Ok);
    network.events.push_back(
        {MqttEvent::Kind::Message, 0, 0, "$thing/down/signaling/p/d", replay_bytes});
    EXPECT_FALSE(step());
    EXPECT_FALSE(std::any_of(network.sent.begin(), network.sent.end(), [](const auto& value) {
        return value.method == "focusState" && value.token == "target-release" && value.code == 200;
    }));
}

// 错误scope或旧序号不得提前排停止；随后原完整release仍须独立真实回执才能推进水位。
TEST_F(HumanReceiver, StaleOrForeignReleaseCannotStopOriginalScope) {
    acquire_target();
    establish_target();
    auto release = control("release", 1, 4);
    release.payload =
        R"({"operation":"release","expectedRevision":1,"focusId":"target-focus","resources":["microphone","speaker"],"priority":"call","resumePolicy":"never"})";
    const auto original_outputs = outputs.size();
    auto foreign = release;
    foreign.token = "foreign-release";
    foreign.session = "foreign-human";
    down(foreign);
    auto stale = release;
    stale.token = "stale-release";
    stale.event_sequence = 3;
    down(stale);
    EXPECT_EQ(outputs.size(), original_outputs);
    down(release);
    for (unsigned round = 0; round < 4; ++round) {
        actual();
        ASSERT_TRUE(step());
    }
    EXPECT_TRUE(std::any_of(network.sent.begin(), network.sent.end(), [](const auto& value) {
        return value.method == "focusState" && value.token == "target-release" && value.code == 200;
    }));
}

// 正式释放成功只重放原实际回执；冲突字节不能再驱动或新增资源修订。
TEST_F(HumanReceiver, CompletedCleanupOnlyReplaysOriginalActualFact) {
    acquire_target();
    establish_target();
    auto release = control("release", 1, 4);
    release.payload =
        R"({"operation":"release","expectedRevision":1,"focusId":"target-focus","resources":["microphone","speaker"],"priority":"call","resumePolicy":"never"})";
    down(release);
    for (unsigned round = 0; round < 4; ++round) {
        actual();
        ASSERT_TRUE(step());
    }
    // 只比较真实门控命令；原UI状态回报不属于新资源操作。
    const auto gates = [&] {
        return std::count_if(outputs.begin(), outputs.end(), [](const auto& body) {
            return body.find("inputOwnershipGate") != std::string::npos ||
                   body.find("humanVoiceOutputGate") != std::string::npos;
        });
    };
    const auto original_outputs = gates();
    down(release);
    EXPECT_EQ(gates(), original_outputs);
    auto conflicting = release;
    conflicting.payload.replace(conflicting.payload.find("target-focus"), 12, "other-focus");
    down(conflicting);
    EXPECT_EQ(gates(), original_outputs);
    EXPECT_EQ(std::count_if(network.sent.begin(), network.sent.end(),
                            [](const auto& value) {
                                return value.method == "focusState" &&
                                       value.token == "target-release" && value.code == 200;
                            }),
              2);
}
// 只到扬声器停止票据仍为unknown；随后原宿主来源失效必须关闭，不能拼实际释放ACK。
TEST_F(HumanReceiver, PartialCleanupAndLostOriginalSourceCannotForgeRelease) {
    acquire_target();
    establish_target();
    auto release = control("release", 1, 4);
    release.payload =
        R"({"operation":"release","expectedRevision":1,"focusId":"target-focus","resources":["microphone","speaker"],"priority":"call","resumePolicy":"never"})";
    down(release);
    actual("humanVoiceOutputGate");
    ASSERT_TRUE(step());
    EXPECT_FALSE(std::any_of(network.sent.begin(), network.sent.end(), [](const auto& value) {
        return value.method == "focusState" && value.token == "target-release" && value.code == 200;
    }));
    paired = false;
    EXPECT_FALSE(step());
    actual();
    EXPECT_FALSE(step());
    EXPECT_FALSE(std::any_of(network.sent.begin(), network.sent.end(), [](const auto& value) {
        return value.method == "focusState" && value.token == "target-release" && value.code == 200;
    }));
}

TEST_F(HumanReceiver, ActualFullGroupAndEstablishedRetainPrivateTextBeforeMessageAck) {
    acquire_target();
    establish_target();
    ASSERT_EQ(host.handoff_state(), AiHandoffState::HumanActive);
    const auto original = private_context();
    ASSERT_NO_FATAL_FAILURE(context_down(network, host, original, now, wall));
    ASSERT_NE(host.context_snapshot(), nullptr);
    ASSERT_EQ(host.context_snapshot()->turns.size(), 1U);
    EXPECT_EQ(host.context_snapshot()->turns[0].text, "已授权的完整本人文本");
    ASSERT_EQ(network.sent.back().method, "contextSnapshotAck");
    EXPECT_EQ(network.sent.back().session, "target-human");
    EXPECT_EQ(network.sent.back().token, original.token);
    EXPECT_EQ(network.sent.back().code, 200);
    const auto size = network.sent.size();
    ASSERT_NO_FATAL_FAILURE(context_down(network, host, original, now, wall));
    ASSERT_EQ(network.sent.size(), size + 1);
    EXPECT_EQ(network.sent.back().method, "contextSnapshotAck");
    ASSERT_NO_FATAL_FAILURE(context_down(network, host, private_context("替换正文"), now, wall));
    EXPECT_EQ(network.sent.size(), size + 1);
    ASSERT_NE(host.context_snapshot(), nullptr);
    EXPECT_EQ(host.context_snapshot()->turns[0].text, "已授权的完整本人文本");
    EXPECT_EQ(host.handoff_state(), AiHandoffState::HumanActive);
    host.close(++now);
    EXPECT_EQ(host.context_snapshot(), nullptr);
    EXPECT_EQ(network.disconnects, 1U);
}
TEST_F(HumanReceiver, CandidateWithoutActualFullGroupNeverRetainsOrAcknowledgesPrivateContext) {
    prepare();
    establish_target();
    const auto before = network.sent.size();
    ASSERT_NO_FATAL_FAILURE(context_down(network, host, private_context(), now, wall));
    EXPECT_EQ(host.context_snapshot(), nullptr);
    EXPECT_EQ(network.sent.size(), before);
    EXPECT_EQ(host.handoff_state(), AiHandoffState::Candidate);
}
TEST_F(HumanReceiver, PrivateContextExpiresWithoutEndingOriginalActualCall) {
    acquire_target();
    establish_target();
    ASSERT_NO_FATAL_FAILURE(context_down(network, host, private_context(), now, wall));
    ASSERT_NE(host.context_snapshot(), nullptr);
    wall = 110000;
    ASSERT_TRUE(step());
    EXPECT_EQ(host.context_snapshot(), nullptr);
    EXPECT_EQ(host.handoff_state(), AiHandoffState::HumanActive);
    const auto before = network.sent.size();
    ASSERT_NO_FATAL_FAILURE(context_down(network, host, private_context(), now, wall));
    EXPECT_EQ(network.sent.size(), before);
    EXPECT_EQ(host.context_snapshot(), nullptr);
}
TEST_F(HumanReceiver, PassiveTextCapabilityDoesNotInventActiveAIOrTextInput) {
    ASSERT_NO_FATAL_FAILURE(connected());
    const auto report =
        std::find_if(network.sent.begin(), network.sent.end(),
                     [](const auto& value) { return value.method == "capabilityReport"; });
    ASSERT_NE(report, network.sent.end());
    const std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_Parse(report->payload.c_str()),
                                                               cJSON_Delete);
    ASSERT_TRUE(root);
    // 原audio结构声明语音输入；inputModalities只声明可选主动text，纯受话没有该入口。
    const auto* inputs = cJSON_GetObjectItemCaseSensitive(root.get(), "inputModalities");
    EXPECT_TRUE(!inputs || (cJSON_IsArray(inputs) && cJSON_GetArraySize(inputs) == 0));
    const auto* purposes = cJSON_GetObjectItemCaseSensitive(root.get(), "purposes");
    ASSERT_TRUE(cJSON_IsArray(purposes));
    ASSERT_EQ(cJSON_GetArraySize(purposes), 1);
    ASSERT_TRUE(cJSON_IsString(cJSON_GetArrayItem(purposes, 0)));
    EXPECT_STREQ(cJSON_GetArrayItem(purposes, 0)->valuestring, "humanCall");
    const auto* outputs = cJSON_GetObjectItemCaseSensitive(root.get(), "outputModalities");
    ASSERT_TRUE(cJSON_IsArray(outputs));
    unsigned audio = 0,
             text = 0; // 音频真实驱动和原私有文本留存分别声明，不增加新能力字段。
    for (const auto* item = outputs->child; item; item = item->next) {
        ASSERT_TRUE(cJSON_IsString(item));
        audio += std::string_view(item->valuestring) == "audio";
        text += std::string_view(item->valuestring) == "text";
    }
    EXPECT_EQ(audio, 1U);
    EXPECT_EQ(text, 1U);
}
TEST_F(HumanReceiver, RefusedPrivateAckRetainsOriginalBodyAndReplayPublishesSameReceipt) {
    acquire_target();
    establish_target();
    const auto original = private_context();
    const auto before = network.sent.size();
    network.reject_context_ack = true;
    ASSERT_NO_FATAL_FAILURE(context_down(network, host, original, now, wall));
    ASSERT_EQ(network.refused_context_acks.size(), 1U);
    EXPECT_EQ(network.sent.size(), before);
    ASSERT_NE(host.context_snapshot(), nullptr);
    ASSERT_EQ(host.context_snapshot()->turns.size(), 1U);
    EXPECT_EQ(host.context_snapshot()->turns[0].text, "已授权的完整本人文本");
    EXPECT_EQ(host.handoff_state(), AiHandoffState::HumanActive);
    std::string refused;
    ASSERT_EQ(encode_envelope(network.refused_context_acks[0], refused), DecodeResult::Ok);
    network.reject_context_ack = false;
    ASSERT_NO_FATAL_FAILURE(context_down(network, host, original, now, wall));
    ASSERT_EQ(network.sent.size(), before + 1);
    std::string replay;
    ASSERT_EQ(encode_envelope(network.sent.back(), replay), DecodeResult::Ok);
    EXPECT_EQ(replay, refused);
    ASSERT_NE(host.context_snapshot(), nullptr);
    EXPECT_EQ(host.context_snapshot()->turns[0].text, "已授权的完整本人文本");
    EXPECT_EQ(host.handoff_state(), AiHandoffState::HumanActive);
}
TEST_F(HumanReceiver, ActualMqttDisconnectedImmediatelyClearsPrivateBodyWithoutManualClose) {
    acquire_target();
    establish_target();
    ASSERT_NO_FATAL_FAILURE(context_down(network, host, private_context(), now, wall));
    ASSERT_NE(host.context_snapshot(), nullptr);
    const auto before = network.sent.size();
    network.events.push_back({MqttEvent::Kind::Disconnected, 0, 0, {}, {}});
    EXPECT_FALSE(step());
    EXPECT_EQ(host.context_snapshot(), nullptr);
    EXPECT_EQ(network.sent.size(), before);
    EXPECT_EQ(network.connects, 1U);
}
TEST_F(HumanReceiver, UnconfirmedRemoteHoldDoesNotEndCallOrAcknowledgePrivateBody) {
    acquire_target(true);
    establish_target();
    const auto original = private_context();
    ASSERT_NO_FATAL_FAILURE(context_down(network, host, original, now, wall));
    ASSERT_NE(host.context_snapshot(), nullptr);
    Envelope held;
    held.request = false;
    held.token = "remote-hold-event";
    held.method = "callState";
    held.session = "target-human";
    held.event_sequence = 4;
    held.payload = R"({"state":"held","revision":1,"requestToken":"remote-hold"})";
    // 原平台完成事件只能请求实际双向停止，不能以事件自身假造保持成功或结束通话。
    down(held);
    EXPECT_NE(host.handoff_state(), AiHandoffState::Unknown);
    actual();
    ASSERT_TRUE(step());
    actual();
    ASSERT_TRUE(step());
    const auto before = network.sent.size();
    ASSERT_NO_FATAL_FAILURE(context_down(network, host, original, now, wall));
    auto another = private_context("保持期间的新正文");
    another.token = std::string(64, 'd');
    another.message_sequence = 2;
    const auto id = another.payload.find(std::string(64, 'a'));
    ASSERT_NE(id, std::string::npos);
    another.payload.replace(id, 64, std::string(64, 'd'));
    ASSERT_NO_FATAL_FAILURE(context_down(network, host, another, now, wall));
    EXPECT_EQ(network.sent.size(), before);
    EXPECT_NE(host.handoff_state(), AiHandoffState::Unknown);
    EXPECT_FALSE(std::any_of(network.sent.begin(), network.sent.end(),
                             [](const auto& message) { return message.method == "sessionEnd"; }));
}
TEST_F(HumanReceiver, OriginalActualFocusSuspendAndRestoreGatePrivateDeliveryWithoutEndingCall) {
    acquire_target(true);
    establish_target();
    const auto original = private_context();
    ASSERT_NO_FATAL_FAILURE(context_down(network, host, original, now, wall));
    down(hold_focus(true, "original-hold", 1, 4));
    const auto before_actual = network.sent.size();
    ASSERT_NO_FATAL_FAILURE(context_down(network, host, original, now, wall));
    EXPECT_EQ(network.sent.size(),
              before_actual); // 双票据未收齐时不得以旧acquire交付私有文本。
    actual();
    ASSERT_TRUE(step());
    const auto held_receipt =
        std::find_if(network.sent.rbegin(), network.sent.rend(), [](const auto& event) {
            return event.method == "focusState" && event.token == "original-hold";
        });
    ASSERT_NE(held_receipt, network.sent.rend());
    ASSERT_EQ(held_receipt->code, 200);
    down(hold_state(true, "original-hold", 1, 5));
    EXPECT_NE(host.handoff_state(), AiHandoffState::Unknown);
    const auto held_size = network.sent.size();
    ASSERT_NO_FATAL_FAILURE(context_down(network, host, original, now, wall));
    EXPECT_EQ(network.sent.size(), held_size);
    ASSERT_NE(host.context_snapshot(), nullptr);
    EXPECT_EQ(host.context_snapshot()->turns[0].text, "已授权的完整本人文本");
    down(hold_focus(false, "original-resume", 2, 6));
    actual();
    ASSERT_TRUE(step());
    const auto resumed_receipt =
        std::find_if(network.sent.rbegin(), network.sent.rend(), [](const auto& event) {
            return event.method == "focusState" && event.token == "original-resume";
        });
    ASSERT_NE(resumed_receipt, network.sent.rend());
    ASSERT_EQ(resumed_receipt->code, 200);
    down(hold_state(false, "original-resume", 2, 7));
    ASSERT_EQ(host.handoff_state(), AiHandoffState::HumanActive);
    const auto resumed_size = network.sent.size();
    ASSERT_NO_FATAL_FAILURE(context_down(network, host, original, now, wall));
    ASSERT_EQ(network.sent.size(), resumed_size + 1);
    EXPECT_EQ(network.sent.back().method, "contextSnapshotAck");
    EXPECT_FALSE(std::any_of(network.sent.begin(), network.sent.end(),
                             [](const auto& event) { return event.method == "sessionEnd"; }));
}
// 本地保持/恢复必须发布原HumanClient控制，且双实际焦点回执与原平台完成分开。
TEST_F(HumanReceiver, LocalHoldResumeUsesOriginalClientAndActualFocusReceipts) {
    acquire_target(true);
    establish_target();
    ASSERT_TRUE(local(R"({"kind":"handoffControl","operation":"hold"})"));
    ASSERT_TRUE(step());
    ASSERT_EQ(network.sent.back().method, "callControl");
    const auto hold_token = network.sent.back().token;
    EXPECT_EQ(network.sent.back().payload, R"({"expectedRevision":0,"operation":"hold"})");
    EXPECT_FALSE(local(R"({"kind":"handoffControl","operation":"hold"})"));
    down(hold_focus(true, hold_token, 1, 4));
    ASSERT_TRUE(step());
    EXPECT_FALSE(local(R"({"kind":"handoffControl","operation":"resume"})"));
    actual();
    ASSERT_TRUE(step());
    auto ack = hold_state(true, hold_token, 1, 5);
    ack.token = hold_token;
    ack.event_sequence.reset();
    ack.code = 200;
    ack.payload = R"({"state":"held","revision":1})";
    down(ack);
    down(hold_state(true, hold_token, 1, 5));
    ASSERT_TRUE(step());
    ASSERT_TRUE(local(R"({"kind":"handoffControl","operation":"resume"})"));
    ASSERT_TRUE(step());
    ASSERT_EQ(network.sent.back().method, "callControl");
    const auto resume_token = network.sent.back().token;
    EXPECT_NE(resume_token, hold_token);
    EXPECT_EQ(network.sent.back().payload, R"({"expectedRevision":1,"operation":"resume"})");
    down(hold_focus(false, resume_token, 2, 6));
    actual();
    ASSERT_TRUE(step());
    ack.token = resume_token;
    ack.payload = R"({"state":"active","revision":2})";
    down(ack);
    down(hold_state(false, resume_token, 2, 7));
    ASSERT_TRUE(step());
    EXPECT_EQ(host.handoff_state(), AiHandoffState::HumanActive);
    EXPECT_EQ(network.disconnects, 0U);
    EXPECT_TRUE(std::any_of(outputs.begin(), outputs.end(), [](const auto& value) {
        return value.find("humanControlReceipt") != std::string::npos &&
               value.find("\"actualApplied\":true") != std::string::npos;
    }));
}
TEST_F(HumanReceiver, HoldOutputReceiptAloneCannotPublishCompletedFocusOrPrivateAck) {
    acquire_target(true);
    establish_target();
    const auto original = private_context();
    ASSERT_NO_FATAL_FAILURE(context_down(network, host, original, now, wall));
    down(hold_focus(true, "partial-hold", 1, 4));
    actual("humanVoiceOutputGate");
    ASSERT_TRUE(step());
    EXPECT_FALSE(std::any_of(network.sent.begin(), network.sent.end(), [](const auto& event) {
        return event.method == "focusState" && event.token == "partial-hold";
    }));
    const auto before = network.sent.size();
    ASSERT_NO_FATAL_FAILURE(context_down(network, host, original, now, wall));
    EXPECT_EQ(network.sent.size(), before);
    EXPECT_NE(host.handoff_state(), AiHandoffState::Unknown);
    EXPECT_FALSE(std::any_of(network.sent.begin(), network.sent.end(),
                             [](const auto& event) { return event.method == "sessionEnd"; }));
}
// 确认保持仍是同原业务腿，不把资源owns=false误作主通话结束或禁挂断。
TEST_F(HumanReceiver, ConfirmedHeldCanHangupButNeverAndCandidateCannotControl) {
    acquire_target(true);
    EXPECT_FALSE(local(R"({"kind":"handoffControl","operation":"hold"})"));
    EXPECT_FALSE(local(R"({"kind":"handoffControl","operation":"hangup"})"));
    establish_target();
    down(hold_focus(true, "remote-hold", 1, 4));
    actual();
    ASSERT_TRUE(step());
    down(hold_state(true, "remote-hold", 1, 5));
    ASSERT_TRUE(step());
    EXPECT_EQ(host.handoff_state(), AiHandoffState::HumanActive);
    EXPECT_NE(last_projection().find("\"canHangup\":true"), std::string::npos);
    EXPECT_FALSE(local(R"({"kind":"handoffControl","operation":"hold"})"));
    ASSERT_TRUE(local(R"({"kind":"handoffControl","operation":"hangup"})"));
    EXPECT_FALSE(local(R"({"kind":"handoffControl","operation":"resume"})"));
    ASSERT_TRUE(step());
    EXPECT_TRUE(std::any_of(network.sent.begin(), network.sent.end(), [](const auto& value) {
        return value.method == "sessionEnd" && value.session == "target-human";
    }));
}
// 旧never租约不因按钮存在获保持权限；控制超时不续预算或自动发第二请求。
TEST_F(HumanReceiver, OriginalNeverLeaseRejectsLocalHoldResume) {
    acquire_target();
    establish_target();
    const auto before = network.sent.size();
    EXPECT_FALSE(local(R"({"kind":"handoffControl","operation":"hold"})"));
    EXPECT_FALSE(local(R"({"kind":"handoffControl","operation":"resume"})"));
    EXPECT_EQ(network.sent.size(), before);
    EXPECT_EQ(host.handoff_state(), AiHandoffState::HumanActive);
}
// 默认关闭诊断不留计数；显式native开启也只观察原完整留存及本地ACK受理。
TEST_F(HumanReceiver, PrivateDiagnosticsRemainDisabledByDefault) {
    acquire_target();
    establish_target();
    ASSERT_NO_FATAL_FAILURE(context_down(network, host, private_context(), now, wall));
    ASSERT_NE(host.context_snapshot(), nullptr);
    const auto facts = host.context_diagnostic();
    EXPECT_FALSE(facts.retained);
    EXPECT_EQ(facts.ack_attempts, 0U);
    EXPECT_EQ(facts.ack_accepted, 0U);
    EXPECT_TRUE(host.context_diagnostic_line().empty());
}
// DiagnosticReceiver 仅native构造开启脱敏观察，设备协议和原媒体准入均与普通受话端一致。
class DiagnosticReceiver : public HumanReceiver {
    protected:
    DiagnosticReceiver() : HumanReceiver(true) {}
};
// DiagnosticCaller 仅native构造开启启动诊断，不创建网络、媒体或租约的替代事实。
class DiagnosticCaller : public SharedHost {
    protected:
    DiagnosticCaller() : SharedHost(false, true) {}
};
TEST_F(SharedHost, ExitDiagnosticsDisabledDoesNotChangeOriginalSourceFailure) {
    connected();
    paired = false;
    EXPECT_FALSE(step());
    const auto reads = source_reads;
    const auto sends = network.sent.size(), emits = outputs.size();
    EXPECT_TRUE(host.exit_diagnostic_line(SharedHostExitBranch::HostPollFalse, true).empty());
    EXPECT_TRUE(host.human_media_failure_diagnostic_line().empty());
    EXPECT_EQ(source_reads, reads);
    EXPECT_EQ(network.sent.size(), sends);
    EXPECT_EQ(outputs.size(), emits);
}
TEST_F(DiagnosticCaller, ExitDiagnosticBranchesAreFixedAndUnknownBeforeFailure) {
    EXPECT_EQ(host.human_media_failure_diagnostic_line(),
              R"({"kind":"humanMediaFailureDiagnostic","reason":"unknown","stopCause":"unknown"})");
    for (const auto branch :
         {SharedHostExitBranch::BridgeInvalid, SharedHostExitBranch::BridgeClosed,
          SharedHostExitBranch::HostPollFalse, SharedHostExitBranch::Unknown}) {
        const auto line =
            host.exit_diagnostic_line(branch, branch != SharedHostExitBranch::BridgeClosed);
        std::unique_ptr<cJSON, decltype(&cJSON_Delete)> json(cJSON_Parse(line.c_str()),
                                                             cJSON_Delete);
        ASSERT_TRUE(json);
        EXPECT_EQ(cJSON_GetArraySize(json.get()), 4);
        const auto* reason = cJSON_GetObjectItemCaseSensitive(json.get(), "reason");
        ASSERT_TRUE(cJSON_IsString(reason));
        EXPECT_STREQ(reason->valuestring, "unknown");
    }
    EXPECT_EQ(source_reads, 0U);
    EXPECT_EQ(network.connects, 0U);
    EXPECT_TRUE(network.sent.empty());
    EXPECT_TRUE(outputs.empty());
}
TEST_F(DiagnosticCaller, ExitSourceFailureIsObservedBeforeCleanupAndGetterHasNoSideEffects) {
    connected();
    paired = false;
    const auto reads = source_reads;
    EXPECT_FALSE(step());
    EXPECT_EQ(source_reads, reads + 2); // 原Human媒体poll及其后的source_live短路各读一次。
    const auto after = source_reads;
    const auto sends = network.sent.size(), emits = outputs.size();
    const auto line = host.exit_diagnostic_line(SharedHostExitBranch::HostPollFalse, true);
    EXPECT_EQ(
        line,
        R"({"kind":"hostExitDiagnostic","branch":"hostPollFalse","reason":"sourceUnavailable","failed":true})");
    EXPECT_EQ(host.exit_diagnostic_line(SharedHostExitBranch::HostPollFalse, true), line);
    EXPECT_EQ(source_reads, after);
    EXPECT_EQ(network.sent.size(), sends);
    EXPECT_EQ(outputs.size(), emits);
    host.close(now);
    EXPECT_EQ(host.exit_diagnostic_line(SharedHostExitBranch::HostPollFalse, true), line);
}
TEST_F(DiagnosticCaller, ExitMonotonicDeadlinePrecedesSourceAndKeepsOriginalDeadline) {
    boot();
    paired = false;
    const auto reads = source_reads;
    EXPECT_FALSE(host.poll(500000, wall));
    EXPECT_EQ(source_reads, reads);
    EXPECT_EQ(
        host.exit_diagnostic_line(SharedHostExitBranch::HostPollFalse, true),
        R"({"kind":"hostExitDiagnostic","branch":"hostPollFalse","reason":"monotonicDeadline","failed":true})");
}
TEST_F(DiagnosticCaller, ExitWallDeadlineDoesNotPollOriginalMediaAgain) {
    boot();
    const auto reads = source_reads;
    EXPECT_FALSE(host.poll(now, 600000));
    EXPECT_EQ(source_reads, reads);
    EXPECT_EQ(
        host.exit_diagnostic_line(SharedHostExitBranch::HostPollFalse, true),
        R"({"kind":"hostExitDiagnostic","branch":"hostPollFalse","reason":"wallDeadline","failed":true})");
}
TEST_F(DiagnosticCaller, ExitExplicitCloseStaysNormalAndDoesNotReconnect) {
    connected();
    host.close(now);
    EXPECT_FALSE(step());
    EXPECT_EQ(
        host.exit_diagnostic_line(SharedHostExitBranch::HostPollFalse, false),
        R"({"kind":"hostExitDiagnostic","branch":"hostPollFalse","reason":"closed","failed":false})");
    EXPECT_EQ(network.disconnects, 1U);
    EXPECT_EQ(network.connects, 1U);
}
TEST_F(DiagnosticCaller, ExitRejectedBindingSendKeepsOriginalFailureAndNoFocusSideEffects) {
    established();
    reject_input_binding = true;
    Envelope control;
    control.token = "original-diagnostic-control";
    control.method = "focusControl";
    control.session = "ai";
    control.event_sequence = 2;
    control.payload =
        R"({"operation":"acquire","expectedRevision":0,"focusId":"ai-focus","resources":["microphone","speaker"],"priority":"conversation","resumePolicy":"explicit","expiresAt":200000})";
    std::string bytes;
    ASSERT_EQ(encode_envelope(control, bytes), DecodeResult::Ok);
    network.events.push_back({MqttEvent::Kind::Message, 0, 0, "$thing/down/signaling/p/d", bytes});
    EXPECT_FALSE(step());
    const auto line = host.exit_diagnostic_line(SharedHostExitBranch::HostPollFalse, true);
    EXPECT_EQ(
        line,
        R"({"kind":"hostExitDiagnostic","branch":"hostPollFalse","reason":"emitFailed","failed":true})");
    EXPECT_FALSE(host.ai_owned());
    EXPECT_EQ(media.starts, 0U);
    EXPECT_EQ(line.find("original-diagnostic-control"), std::string::npos);
    EXPECT_EQ(line.find("ai-focus"), std::string::npos);
    EXPECT_FALSE(std::any_of(network.sent.begin(), network.sent.end(),
                             [](const auto& e) { return e.method == "focusState"; }));
    host.close(now);
    EXPECT_EQ(host.exit_diagnostic_line(SharedHostExitBranch::HostPollFalse, true), line);
}
TEST_F(DiagnosticCaller, ExitFailedNetworkStartDoesNotConnectAgainForDiagnostics) {
    ASSERT_TRUE(local(R"({"kind":"ready","sampleRate":48000,"frameMs":20,"aec":true})"));
    ASSERT_TRUE(local(R"({"kind":"inputOwnershipIdle","bootGeneration":"7"})"));
    ASSERT_TRUE(step());
    actual();
    network.connect_ok = false;
    EXPECT_FALSE(step());
    EXPECT_EQ(
        host.exit_diagnostic_line(SharedHostExitBranch::HostPollFalse, true),
        R"({"kind":"hostExitDiagnostic","branch":"hostPollFalse","reason":"connectionStartFailed","failed":true})");
    EXPECT_EQ(network.connects, 1U);
    EXPECT_TRUE(network.sent.empty());
}
TEST_F(DiagnosticCaller, ExitNetworkEventUsesOriginalConnectionFailure) {
    connected();
    network.events.push_back({MqttEvent::Kind::Failed, 0, 0, {}, {}});
    EXPECT_FALSE(step());
    EXPECT_EQ(
        host.exit_diagnostic_line(SharedHostExitBranch::HostPollFalse, true),
        R"({"kind":"hostExitDiagnostic","branch":"hostPollFalse","reason":"connectionFailed","failed":true})");
    EXPECT_EQ(network.connects, 1U);
}
TEST_F(DiagnosticCaller, ExitAiRuntimeMediaFailureDoesNotInventReceipt) {
    established();
    media.poll_ok = false;
    EXPECT_FALSE(step());
    EXPECT_EQ(
        host.exit_diagnostic_line(SharedHostExitBranch::HostPollFalse, true),
        R"({"kind":"hostExitDiagnostic","branch":"hostPollFalse","reason":"voiceRuntimeFailed","failed":true})");
    EXPECT_EQ(media.starts, 0U);
    const auto runtime_line = host.ai_runtime_failure_diagnostic_line();
    EXPECT_EQ(runtime_line, R"({"kind":"voiceRuntimeFailureDiagnostic","reason":"mediaPoll"})");
    const auto reads = source_reads;
    const auto packets = network.sent.size();
    host.close(now);
    EXPECT_EQ(host.ai_runtime_failure_diagnostic_line(), runtime_line);
    EXPECT_EQ(source_reads, reads);
    EXPECT_EQ(network.sent.size(), packets);
}
// 默认关闭不暴露原运行时；尚未失败保持unknown，不能以读回动作触发失败。
TEST_F(SharedHost, RuntimeFailureDiagnosticsRemainDisabled) {
    established();
    media.poll_ok = false;
    EXPECT_FALSE(step());
    EXPECT_TRUE(host.ai_runtime_failure_diagnostic_line().empty());
    EXPECT_TRUE(host.duplex_driver_failure_diagnostic_line().empty());
    EXPECT_TRUE(host.human_signal_diagnostic_line().empty());
}
TEST_F(DiagnosticReceiver, DriverAndHumanSignalDiagnosticsArePureOriginalFacts) {
    invite();
    const auto reads = source_reads;
    const auto packets = network.sent.size();
    const auto events = outputs.size();
    EXPECT_EQ(host.duplex_driver_failure_diagnostic_line(), R"({"kind":"duplexDriverFailureDiagnostic","reason":"unknown","path":"unknown"})");
    EXPECT_EQ(host.human_signal_diagnostic_line(), R"({"kind":"humanSignalDiagnostic","state":"ringing","failure":"none"})");
    EXPECT_EQ(source_reads, reads);
    EXPECT_EQ(network.sent.size(), packets);
    EXPECT_EQ(outputs.size(), events);
    ASSERT_TRUE(local(R"({"kind":"handoffControl","operation":"accept"})"));
    EXPECT_EQ(host.human_signal_diagnostic_line(), R"({"kind":"humanSignalDiagnostic","state":"accepting","failure":"none"})");
    Envelope ack;
    ack.request = false;
    ack.method = "requestAck";
    ack.session = "target-human";
    ack.token = network.sent.back().token;
    ack.code = 200;
    ack.payload = R"({"requestMethod":"sessionAccept","result":{}})";
    down(ack);
    EXPECT_EQ(host.human_signal_diagnostic_line(), R"({"kind":"humanSignalDiagnostic","state":"negotiating","failure":"none"})");
}
// 显式观察不能把正常运行或尚未建立的AI腿猜成失败。
TEST_F(DiagnosticCaller, RuntimeFailureDiagnosticsUnknownBeforeFailure) {
    EXPECT_EQ(host.ai_runtime_failure_diagnostic_line(),
              R"({"kind":"voiceRuntimeFailureDiagnostic","reason":"unknown"})");
    established();
    EXPECT_EQ(host.ai_runtime_failure_diagnostic_line(),
              R"({"kind":"voiceRuntimeFailureDiagnostic","reason":"unknown"})");
}
TEST_F(DiagnosticCaller, ExitHumanBootOutputDeadlineIsNotPcmOrNetworkFailure) {
    ASSERT_TRUE(local(R"({"kind":"ready","sampleRate":48000,"frameMs":20,"aec":true})"));
    ASSERT_TRUE(local(R"({"kind":"inputOwnershipIdle","bootGeneration":"7"})"));
    ASSERT_TRUE(step());
    actual("playbackGate"); // 只确认原PCM false，故意保留Human boot输出实态未完成。
    now += 2000;
    wall += 2000;
    EXPECT_FALSE(step());
    EXPECT_EQ(
        host.exit_diagnostic_line(SharedHostExitBranch::HostPollFalse, true),
        R"({"kind":"hostExitDiagnostic","branch":"hostPollFalse","reason":"humanMediaFailed","failed":true})");
    EXPECT_EQ(network.connects, 0U);
    const auto reads = source_reads;
    const auto sends = network.sent.size(), emits = outputs.size();
    const auto evidence = host.human_media_failure_diagnostic_line();
    EXPECT_EQ(
        evidence,
        R"({"kind":"humanMediaFailureDiagnostic","reason":"gateDeadline","stopCause":"unknown"})");
    EXPECT_EQ(host.human_media_failure_diagnostic_line(), evidence);
    EXPECT_EQ(source_reads, reads);
    EXPECT_EQ(network.sent.size(), sends);
    EXPECT_EQ(outputs.size(), emits);
    host.close(now);
    EXPECT_EQ(host.human_media_failure_diagnostic_line(), evidence);
    EXPECT_EQ(
        host.exit_diagnostic_line(SharedHostExitBranch::HostPollFalse, true),
        R"({"kind":"hostExitDiagnostic","branch":"hostPollFalse","reason":"humanMediaFailed","failed":true})");
}
TEST_F(DiagnosticCaller, ExitPcmBootOutputDeadlineDoesNotPollConnection) {
    ASSERT_TRUE(local(R"({"kind":"ready","sampleRate":48000,"frameMs":20,"aec":true})"));
    ASSERT_TRUE(local(R"({"kind":"inputOwnershipIdle","bootGeneration":"7"})"));
    ASSERT_TRUE(step());
    actual("humanVoiceOutputInit");  // 只确认原Human false，PCM
                                     // gate仍等待自身原票据。
    now += 2000;
    wall += 2000;
    EXPECT_FALSE(step());
    EXPECT_EQ(
        host.exit_diagnostic_line(SharedHostExitBranch::HostPollFalse, true),
        R"({"kind":"hostExitDiagnostic","branch":"hostPollFalse","reason":"pcmOutputGateFailed","failed":true})");
    EXPECT_EQ(network.connects, 0U);
}
TEST_F(DiagnosticCaller, ExitInputClockFailureKeepsOriginalShortCircuitOrder) {
    boot();
    EXPECT_FALSE(host.poll(now - 1, wall));
    EXPECT_EQ(
        host.exit_diagnostic_line(SharedHostExitBranch::HostPollFalse, true),
        R"({"kind":"hostExitDiagnostic","branch":"hostPollFalse","reason":"inputGateFailed","failed":true})");
}
TEST_F(DiagnosticReceiver, ExitActualHumanReceiptRejectKeepsOriginalWatermarkGuard) {
    prepare();
    down(control("acquire", 0, 3)); // 故意跳过原邀请之后的2，实际端点不能替Human补信令水位。
    actual();
    ASSERT_TRUE(step());
    actual();
    EXPECT_FALSE(step());
    EXPECT_EQ(
        host.exit_diagnostic_line(SharedHostExitBranch::HostPollFalse, true),
        R"({"kind":"hostExitDiagnostic","branch":"hostPollFalse","reason":"humanFocusReceiptRejected","failed":true})");
    EXPECT_EQ(network.connects, 1U);
    EXPECT_EQ(media.starts, 0U);
}
TEST_F(SharedHost, StartupDiagnosticsRemainDisabledByDefault) {
    EXPECT_TRUE(host.startup_diagnostic_line().empty());
    connected();
    EXPECT_TRUE(host.startup_diagnostic_line().empty());
}
TEST_F(DiagnosticCaller, StartupReportsOriginalReadyAndIdleBeforeCapabilityHandshake) {
    EXPECT_EQ(
        host.startup_diagnostic_line(),
        R"({"kind":"startupDiagnostic","browserReady":false,"initialIdleConfirmed":false,"connection":"idle","voice":"idle","mediaReady":false,"aiOwned":false})");
    boot();
    EXPECT_EQ(
        host.startup_diagnostic_line(),
        R"({"kind":"startupDiagnostic","browserReady":true,"initialIdleConfirmed":true,"connection":"connecting","voice":"idle","mediaReady":false,"aiOwned":false})");
}
TEST_F(DiagnosticCaller, StartupCapabilityAckEnablesActualCreatingNotOwned) {
    connected();
    EXPECT_EQ(network.sent.back().method, "sessionCreate");
    EXPECT_EQ(
        host.startup_diagnostic_line(),
        R"({"kind":"startupDiagnostic","browserReady":true,"initialIdleConfirmed":true,"connection":"ready","voice":"creating","mediaReady":false,"aiOwned":false})");
}
TEST_F(DiagnosticCaller, StartupEstablishedStillRequiresOriginalFullFocusLease) {
    established();
    EXPECT_EQ(
        host.startup_diagnostic_line(),
        R"({"kind":"startupDiagnostic","browserReady":true,"initialIdleConfirmed":true,"connection":"ready","voice":"established","mediaReady":true,"aiOwned":false})");
    acquire();
    const auto line = host.startup_diagnostic_line();
    EXPECT_EQ(
        line,
        R"({"kind":"startupDiagnostic","browserReady":true,"initialIdleConfirmed":false,"connection":"ready","voice":"established","mediaReady":true,"aiOwned":true})");
    EXPECT_EQ(line.find("instance"), std::string::npos);
    EXPECT_EQ(line.find("ai-focus"), std::string::npos);
    EXPECT_EQ(line.find("\"sessionId\""), std::string::npos);
}
TEST_F(DiagnosticReceiver, RejectedAckRetainsBodyAndReplayCountsActualLocalSubmission) {
    acquire_target();
    establish_target();
    const auto original = private_context();
    network.reject_context_ack = true;
    ASSERT_NO_FATAL_FAILURE(context_down(network, host, original, now, wall));
    const auto refused = host.context_diagnostic();
    EXPECT_TRUE(refused.retained);
    EXPECT_EQ(refused.ack_attempts, 1U);
    EXPECT_EQ(refused.ack_accepted, 0U);
    network.reject_context_ack = false;
    ASSERT_NO_FATAL_FAILURE(context_down(network, host, original, now, wall));
    const auto accepted = host.context_diagnostic();
    EXPECT_TRUE(accepted.retained);
    EXPECT_EQ(accepted.ack_attempts, 2U);
    EXPECT_EQ(accepted.ack_accepted, 1U);
    const auto line = host.context_diagnostic_line();
    EXPECT_EQ(
        line,
        R"({"kind":"contextDiagnostic","retained":true,"cleared":false,"expired":false,"ackAttempts":2,"ackAccepted":1})");
    EXPECT_EQ(line.find("已授权"), std::string::npos);
    EXPECT_EQ(line.find("target-human"), std::string::npos);
    EXPECT_EQ(line.find(std::string(64, 'a')), std::string::npos);
    auto tampered = private_context("不得输出的篡改正文");
    ASSERT_NO_FATAL_FAILURE(context_down(network, host, tampered, now, wall));
    EXPECT_EQ(host.context_diagnostic_line(), line);
}
TEST_F(DiagnosticReceiver, ActualExpirationClearsWithoutEndingMainCall) {
    acquire_target();
    establish_target();
    ASSERT_NO_FATAL_FAILURE(context_down(network, host, private_context(), now, wall));
    EXPECT_TRUE(host.context_diagnostic().retained);
    wall = 110000;
    ASSERT_TRUE(step());
    const auto facts = host.context_diagnostic();
    EXPECT_FALSE(facts.retained);
    EXPECT_TRUE(facts.cleared);
    EXPECT_TRUE(facts.expired);
    EXPECT_EQ(host.handoff_state(), AiHandoffState::HumanActive);
    EXPECT_EQ(facts.ack_attempts, 1U);
}
TEST_F(DiagnosticReceiver, RealDisconnectedClearsWithoutInventingExpirationOrAck) {
    acquire_target();
    establish_target();
    ASSERT_NO_FATAL_FAILURE(context_down(network, host, private_context(), now, wall));
    EXPECT_TRUE(host.context_diagnostic().retained);
    network.events.push_back({MqttEvent::Kind::Disconnected, 0, 0, "", ""});
    EXPECT_FALSE(step());
    const auto facts = host.context_diagnostic();
    EXPECT_FALSE(facts.retained);
    EXPECT_TRUE(facts.cleared);
    EXPECT_FALSE(facts.expired);
    EXPECT_EQ(facts.ack_attempts, 1U);
    EXPECT_EQ(facts.ack_accepted, 1U);
}
TEST_F(DiagnosticReceiver, HeldDoesNotAcknowledgePrivateReplayAndObservationStaysBounded) {
    acquire_target(true);
    establish_target();
    const auto original = private_context();
    ASSERT_NO_FATAL_FAILURE(context_down(network, host, original, now, wall));
    for (unsigned replay = 0; replay < 4096; ++replay) {
        ASSERT_NO_FATAL_FAILURE(context_down(network, host, original, now, wall));
    }
    const auto before = host.context_diagnostic();
    ASSERT_EQ(before.ack_attempts, 4096U);
    ASSERT_EQ(before.ack_accepted, 4096U);
    down(hold_focus(true, "diagnostic-hold", 1, 4));
    actual();
    ASSERT_TRUE(step());
    down(hold_state(true, "diagnostic-hold", 1, 5));
    ASSERT_TRUE(step());
    ASSERT_NO_FATAL_FAILURE(context_down(network, host, original, now, wall));
    const auto facts = host.context_diagnostic();
    EXPECT_TRUE(facts.retained);
    EXPECT_EQ(facts.ack_attempts, before.ack_attempts);
    EXPECT_EQ(facts.ack_accepted, before.ack_accepted);
    EXPECT_EQ(host.handoff_state(), AiHandoffState::HumanActive);
}
TEST_F(HumanReceiver, UnknownLocalHoldAndDisconnectedProofCannotRetryControl) {
    acquire_target(true);
    establish_target();
    ASSERT_TRUE(local(R"({"kind":"handoffControl","operation":"hold"})"));
    ASSERT_TRUE(step());
    now += 10000;
    wall += 10000;
    ASSERT_TRUE(step());
    const auto before = network.sent.size();
    EXPECT_FALSE(local(R"({"kind":"handoffControl","operation":"hold"})"));
    EXPECT_FALSE(local(R"({"kind":"handoffControl","operation":"resume"})"));
    EXPECT_EQ(network.sent.size(), before);
    EXPECT_EQ(host.handoff_state(), AiHandoffState::HumanActive);
    network.events.push_back({MqttEvent::Kind::Disconnected, 0, 0, {}, {}});
    EXPECT_FALSE(step());
    EXPECT_FALSE(local(R"({"kind":"handoffControl","operation":"hold"})"));
    EXPECT_FALSE(local(R"({"kind":"handoffControl","operation":"resume"})"));
}
TEST_F(HumanReceiver, OtherControlTokenCannotBorrowOriginalSuccessfulSuspendProof) {
    acquire_target(true);
    establish_target();
    const auto original = private_context();
    ASSERT_NO_FATAL_FAILURE(context_down(network, host, original, now, wall));
    down(hold_focus(true, "actual-hold", 1, 4));
    actual();
    ASSERT_TRUE(step());
    ASSERT_TRUE(std::any_of(network.sent.begin(), network.sent.end(), [](const auto& event) {
        return event.method == "focusState" && event.token == "actual-hold" && event.code == 200;
    }));
    down(hold_state(true, "other-hold", 1, 5));
    const auto before = network.sent.size();
    ASSERT_NO_FATAL_FAILURE(context_down(network, host, original, now, wall));
    EXPECT_EQ(network.sent.size(), before);
    EXPECT_NE(host.handoff_state(), AiHandoffState::Unknown);
    now += 2000;
    ASSERT_TRUE(step()); // 原等待到期不冒成功，不重做副作用，也不自行结束主通话。
    EXPECT_EQ(network.sent.size(), before);
    EXPECT_NE(host.handoff_state(), AiHandoffState::Unknown);
    EXPECT_FALSE(std::any_of(network.sent.begin(), network.sent.end(),
                             [](const auto& event) { return event.method == "sessionEnd"; }));
}
TEST_F(HumanReceiver, CapabilityAckDoesNotCreateShadowAIOrSecondConnection) {
    connected();
    EXPECT_EQ(host.voice_state(), VoiceState::Idle);
    EXPECT_EQ(network.connects, 1U);
    EXPECT_FALSE(std::any_of(network.sent.begin(), network.sent.end(),
                             [](const auto& event) { return event.method == "sessionCreate"; }));
    EXPECT_FALSE(local(R"({"kind":"start"})"));
    EXPECT_FALSE(local(
        R"({"kind":"handoffControl","operation":"escalate","targetKind":"device","targetId":"other"})"));
}
TEST_F(HumanReceiver, OriginalInviteRingsWithoutAutoAcceptOrSecondSource) {
    invite();
    EXPECT_NE(last_projection().find("\"canAccept\":true"), std::string::npos);
    EXPECT_FALSE(std::any_of(network.sent.begin(), network.sent.end(),
                             [](const auto& v) { return v.method == "sessionAccept"; }));
    EXPECT_FALSE(std::any_of(outputs.begin(), outputs.end(), [](const auto& v) {
        return v.find("humanVoiceStart") != std::string::npos;
    }));
    EXPECT_FALSE(local(R"({"kind":"handoffControl","operation":"hangup"})"));
    EXPECT_EQ(media.starts, 0U);
}
TEST_F(HumanReceiver, ExplicitAcceptRequiresOriginalRinging) {
    connected();
    EXPECT_FALSE(local(R"({"kind":"handoffControl","operation":"accept"})"));
    EXPECT_FALSE(local(R"({"kind":"handoffControl","operation":"reject"})"));
}
TEST_F(HumanReceiver, RejectDoesNotRequireAutoAcceptedIntent) {
    invite();
    ASSERT_TRUE(local(R"({"kind":"handoffControl","operation":"reject"})"));
    EXPECT_EQ(network.sent.back().method, "sessionReject");
}
TEST_F(HumanReceiver, LocalSessionCannotForgeAccept) {
    invite();
    EXPECT_FALSE(
        local(R"({"kind":"handoffControl","operation":"accept","sessionId":"target-human"})"));
}
TEST_F(HumanReceiver, ActualConnectedWaitsFreshOutputBeforeMediaState) {
    prepare(false);
    EXPECT_NE(network.sent.back().method, "mediaState");
    EXPECT_FALSE(local(R"({"kind":"handoffControl","operation":"hangup"})"));
    EXPECT_EQ(media.starts, 0U);
}
// 首次纯Human绑定只退休boot输出票据；延后的原boot回执不能完成新scope，也不能使正常等待全局失败。
TEST_F(HumanReceiver, FirstScopeRefreshWaitsFreshFalseAndIgnoresLateBootReceipt) {
    now = 200000; // 原总期限以内的非零运行时基线，不依赖构造时零时刻。
    invite();
    const auto boot = std::find_if(outputs.begin(), outputs.end(), [](const auto& value) { return value.find("\"kind\":\"humanVoiceOutputInit\"") != std::string::npos; });
    const auto bind = std::find_if(outputs.begin(), outputs.end(), [](const auto& value) { return value.find("\"kind\":\"humanVoiceOutputBind\"") != std::string::npos; });
    const auto fresh = std::find_if(outputs.begin(), outputs.end(), [](const auto& value) { return value.find("\"kind\":\"humanVoiceOutputGate\"") != std::string::npos; });
    ASSERT_NE(boot, outputs.end());
    ASSERT_NE(bind, outputs.end());
    ASSERT_NE(fresh, outputs.end());
    EXPECT_LT(std::distance(outputs.begin(), boot), std::distance(outputs.begin(), bind));
    EXPECT_LT(std::distance(outputs.begin(), bind), std::distance(outputs.begin(), fresh));
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> old(cJSON_Parse(boot->c_str()), cJSON_Delete);
    ASSERT_NE(old, nullptr);
    cJSON_ReplaceItemInObjectCaseSensitive(old.get(), "kind", cJSON_CreateString("humanVoiceOutputIdle"));
    cJSON_AddNumberToObject(old.get(), "frames", 1);
    char* raw = cJSON_PrintUnformatted(old.get());
    ASSERT_NE(raw, nullptr);
    const std::string replay(raw);
    cJSON_free(raw);
    EXPECT_FALSE(local(replay)); // C++旧scope回执严格拒绝，main忽略该票据，不授新静音。
    ASSERT_TRUE(step());
    ASSERT_TRUE(local(R"({"kind":"handoffControl","operation":"accept"})"));
    const auto request = network.sent.back();
    ASSERT_EQ(request.method, "sessionAccept");
    Envelope accepted;
    accepted.request = false;
    accepted.method = "requestAck";
    accepted.token = request.token;
    accepted.session = "target-human";
    accepted.code = 200;
    accepted.payload = R"({"requestMethod":"sessionAccept","result":{}})";
    down(accepted);
    for (int round = 0; round < 5; ++round) ASSERT_TRUE(step());
    actual("humanVoiceOutputGate"); // 只交付已发送的原新scope票据，不能用boot重放补它。
    ASSERT_TRUE(step());
    EXPECT_FALSE(host.ai_owned());
    EXPECT_EQ(host.connection_state(), SharedConnectionState::Ready);
    EXPECT_EQ(network.disconnects, 0U);
    EXPECT_EQ(media.starts, 0U);
}
TEST_F(HumanReceiver, ActualTargetAcquireStillWaitsIndependentEstablished) {
    acquire_target();
    EXPECT_NE(last_projection().find("\"state\":6"), std::string::npos);
    EXPECT_NE(last_projection().find("\"canHangup\":false"), std::string::npos);
    EXPECT_FALSE(local(R"({"kind":"handoffControl","operation":"hangup"})"));
    establish_target();
    EXPECT_NE(last_projection().find("\"state\":8"), std::string::npos);
    EXPECT_NE(last_projection().find("\"canHangup\":true"), std::string::npos);
    EXPECT_TRUE(local(R"({"kind":"handoffControl","operation":"hangup"})"));
    EXPECT_EQ(network.sent.back().method, "sessionEnd");
    EXPECT_EQ(network.connects, 1U);
    EXPECT_EQ(network.disconnects, 0U);
}
TEST_F(HumanReceiver, EarlyEstablishedCannotReplaceTargetActualAcquire) {
    prepare();
    establish_target();
    EXPECT_EQ(host.handoff_state(), AiHandoffState::Candidate);
    EXPECT_FALSE(local(R"({"kind":"handoffControl","operation":"hangup"})"));
    down(control());
    actual();
    ASSERT_TRUE(step());
    actual();
    ASSERT_TRUE(step());
    EXPECT_EQ(host.handoff_state(), AiHandoffState::HumanActive);
    establish_target();
    EXPECT_EQ(host.handoff_state(), AiHandoffState::HumanActive);
}
// 媒体ACK已确认且平台建立事件早于实际焦点回执时，不授早权限，但不能依赖服务端重投同一QoS0事件。
TEST_F(HumanReceiver, EarlyEstablishedWaitsActualAcquireWithoutServerReplay) {
    prepare();
    establish_target();
    EXPECT_EQ(host.handoff_state(), AiHandoffState::Candidate);
    EXPECT_FALSE(local(R"({"kind":"handoffControl","operation":"hangup"})"));
    down(control());
    actual();
    ASSERT_TRUE(step());
    actual();
    ASSERT_TRUE(step());
    EXPECT_EQ(host.handoff_state(), AiHandoffState::HumanActive);
    EXPECT_TRUE(local(R"({"kind":"handoffControl","operation":"hangup"})"));
}
// 受话腿原邀请1、建立2、焦点3独立连续，信令接纳本身不能提前开启人工控制。
TEST_F(HumanReceiver, OriginalEstablishedTwoBeforeAcquireThreePreservesWatermark) {
    prepare();
    Envelope value;
    value.request = false;
    value.token = "early-target-established";
    value.method = "sessionEstablished";
    value.session = "target-human";
    value.event_sequence = 2;
    value.payload = R"({"state":"established","mediaRevision":1})";
    down(value);
    EXPECT_EQ(host.handoff_state(), AiHandoffState::Candidate);
    EXPECT_FALSE(local(R"({"kind":"handoffControl","operation":"hangup"})"));
    EXPECT_FALSE(local(R"({"kind":"handoffControl","operation":"hold"})"));
    down(control("acquire", 0, 3));
    EXPECT_NE(host.handoff_state(), AiHandoffState::HumanActive);
    actual();
    ASSERT_TRUE(step());
    actual();
    ASSERT_TRUE(step());
    EXPECT_EQ(host.handoff_state(), AiHandoffState::HumanActive);
    EXPECT_TRUE(local(R"({"kind":"handoffControl","operation":"hangup"})"));
}
// 非当前媒体代次不进入保留槽，不因后续取得完整焦点升级为有效建立事实。
TEST_F(HumanReceiver, EarlyWrongMediaRevisionDoesNotReplayAfterAcquire) {
    prepare();
    Envelope value;
    value.request = false;
    value.token = "wrong-revision-established";
    value.method = "sessionEstablished";
    value.session = "target-human";
    value.event_sequence = 3;
    value.payload = R"({"state":"established","mediaRevision":2})";
    down(value);
    down(control());
    actual();
    ASSERT_TRUE(step());
    actual();
    ASSERT_TRUE(step());
    EXPECT_EQ(host.handoff_state(), AiHandoffState::Acquired);
    EXPECT_FALSE(local(R"({"kind":"handoffControl","operation":"hangup"})"));
}
// 媒体状态ACK晚于完整取得时仍保留同一事实，不能把一次发送当作已受理。
TEST_F(HumanReceiver, EarlyEstablishedStillWaitsOriginalMediaStateAck) {
    prepare(true, false);
    const auto state_request = network.sent.back();
    ASSERT_EQ(state_request.method, "mediaState");
    establish_target();
    establish_target();
    down(control());
    actual();
    ASSERT_TRUE(step());
    actual();
    ASSERT_TRUE(step());
    EXPECT_FALSE(local(R"({"kind":"handoffControl","operation":"hangup"})"));
    Envelope ack;
    ack.request = false;
    ack.token = state_request.token;
    ack.method = "requestAck";
    ack.session = "target-human";
    ack.code = 200;
    ack.payload = R"({"requestMethod":"mediaState","result":{}})";
    down(ack);
    EXPECT_EQ(host.handoff_state(), AiHandoffState::HumanActive);
    EXPECT_TRUE(local(R"({"kind":"handoffControl","operation":"hangup"})"));
}
// 合法形状但未知未来序号不补水位缺口，也不因原取得事实获得人工控制资格。
TEST_F(HumanReceiver, EarlyUnknownFutureSequenceCannotActivate) {
    prepare();
    Envelope value;
    value.request = false;
    value.token = "unknown-future-established";
    value.method = "sessionEstablished";
    value.session = "target-human";
    value.event_sequence = 99;
    value.payload = R"({"state":"established","mediaRevision":1})";
    down(value);
    down(control());
    actual();
    ASSERT_TRUE(step());
    actual();
    ASSERT_TRUE(step());
    EXPECT_EQ(host.handoff_state(), AiHandoffState::Acquired);
    EXPECT_FALSE(local(R"({"kind":"handoffControl","operation":"hangup"})"));
}
// 原结束事件清除保留槽，旧建立事件不能跨终态复活原人工腿。
TEST_F(HumanReceiver, EndedCandidateDropsEarlyEstablished) {
    prepare();
    establish_target();
    Envelope ended;
    ended.request = false;
    ended.token = "target-ended";
    ended.method = "sessionEnded";
    ended.session = "target-human";
    ended.event_sequence = 2;
    ended.payload = R"({"reason":"hangup"})";
    down(ended);
    actual();
    ASSERT_TRUE(step());
    EXPECT_NE(host.handoff_state(), AiHandoffState::HumanActive);
    EXPECT_FALSE(local(R"({"kind":"handoffControl","operation":"hangup"})"));
}
// 同poll的正式Established只在原完整组已实际取得后接纳，不要求服务端重投QoS0事件。
TEST_F(HumanReceiver, EstablishedInActualReceiptPollAllowsOriginalHold) {
    ASSERT_NO_FATAL_FAILURE(acquire_target(true, true));
    ASSERT_EQ(host.handoff_state(), AiHandoffState::HumanActive);
    ASSERT_TRUE(local(R"({"kind":"handoffControl","operation":"hold"})"));
    ASSERT_TRUE(step());
    ASSERT_EQ(network.sent.back().method, "callControl");
    EXPECT_EQ(network.sent.back().session, "target-human");
    EXPECT_EQ(network.sent.back().payload, R"({"expectedRevision":0,"operation":"hold"})");
    EXPECT_FALSE(local(R"({"kind":"handoffControl","operation":"hold"})"));
    EXPECT_EQ(network.disconnects, 0U);
}
TEST_F(HumanReceiver, FocusBeforeExplicitAcceptCannotCreateLease) {
    invite();
    down(control());
    actual();
    ASSERT_TRUE(step());
    EXPECT_EQ(host.handoff_state(), AiHandoffState::Candidate);
    EXPECT_FALSE(std::any_of(network.sent.begin(), network.sent.end(),
                             [](const auto& v) { return v.method == "focusState"; }));
}
TEST_F(HumanReceiver, HalfGroupCannotEnableEitherOwnedLeg) {
    prepare();
    auto value = control();
    const auto position = value.payload.find("\"microphone\",\"speaker\"");
    ASSERT_NE(position, std::string::npos);
    value.payload.replace(position, std::string("\"microphone\",\"speaker\"").size(),
                          "\"speaker\"");
    down(value);
    actual();
    ASSERT_TRUE(step());
    EXPECT_EQ(host.handoff_state(), AiHandoffState::Candidate);
}
TEST_F(HumanReceiver, WrongGlobalCasCannotAcquireTarget) {
    prepare();
    down(control("acquire", 1));
    actual();
    ASSERT_TRUE(step());
    EXPECT_EQ(host.handoff_state(), AiHandoffState::Candidate);
}
TEST_F(HumanReceiver, NativeAuthorizationWindowCannotBeExtendedByFocus) {
    prepare();
    auto value = control();
    const auto position = value.payload.find("200000");
    ASSERT_NE(position, std::string::npos);
    value.payload.replace(position, 6, "600001");
    down(value);
    actual();
    ASSERT_TRUE(step());
    EXPECT_EQ(host.handoff_state(), AiHandoffState::Candidate);
}
TEST_F(HumanReceiver, DisconnectedOriginalSourceCannotKeepTargetLease) {
    acquire_target();
    establish_target();
    paired = false;
    EXPECT_FALSE(step());
    EXPECT_EQ(media.starts, 0U);
}
TEST_F(HumanReceiver, TargetActualFullGroupIsNotNetworkReplyForgery) {
    prepare();
    down(control());
    EXPECT_EQ(host.handoff_state(), AiHandoffState::Candidate);
    actual("humanVoiceOutputGate");
    ASSERT_TRUE(step());
    EXPECT_NE(host.handoff_state(), AiHandoffState::Acquired);
    EXPECT_EQ(network.disconnects, 0U);
}
// 显式本人授权引用必须沿原service意图发送，不能被默认不分享路径静默吞掉。
TEST_F(SharedHost, ExplicitConsentPublishesOriginalServiceShareIntent) {
    established();
    acquire();
    ASSERT_TRUE(host.ai_owned());
    ASSERT_TRUE(local(
        R"({"kind":"handoffControl","operation":"escalate","targetKind":"service","targetId":"human-support","shareContext":true,"consentId":"owner-consent"})"));
    ASSERT_EQ(network.sent.back().method, "callControl");
    EXPECT_NE(network.sent.back().payload.find("\"shareContext\":true"), std::string::npos);
    EXPECT_NE(network.sent.back().payload.find("\"consentIds\":[\"owner-consent\"]"),
              std::string::npos);
    EXPECT_TRUE(host.ai_owned());
    EXPECT_EQ(network.connects, 1U);
    const auto original = network.sent.back();
    const auto count = network.sent.size();
    EXPECT_FALSE(local(
        R"({"kind":"handoffControl","operation":"escalate","targetKind":"service","targetId":"human-support","shareContext":true,"consentId":"owner-consent"})"));
    EXPECT_FALSE(local(
        R"({"kind":"handoffControl","operation":"escalate","targetKind":"service","targetId":"human-support","shareContext":true,"consentId":"replacement"})"));
    EXPECT_EQ(network.sent.size(), count);
    EXPECT_EQ(network.sent.back().token, original.token);
    EXPECT_EQ(network.sent.back().payload, original.payload);
}
// 畸形分享、代签正文和其他操作不能消耗原AI意图；合法默认操作随后仍可执行。
TEST_F(SharedHost, InvalidConsentHalKeepsAiAndAllowsOriginalDefaultIntent) {
    established();
    acquire();
    const auto count = network.sent.size();
    const std::vector<std::string> invalid = {
        R"({"kind":"handoffControl","operation":"escalate","targetKind":"service","targetId":"human-support","shareContext":true})",
        R"({"kind":"handoffControl","operation":"escalate","targetKind":"service","targetId":"human-support","consentId":"owner-consent"})",
        R"({"kind":"handoffControl","operation":"escalate","targetKind":"service","targetId":"human-support","shareContext":false,"consentId":"owner-consent"})",
        R"({"kind":"handoffControl","operation":"escalate","targetKind":"device","targetId":"p:other","shareContext":true,"consentId":"owner-consent"})",
        R"({"kind":"handoffControl","operation":"escalate","targetKind":"service","targetId":"human-support","shareContext":true,"consentId":""})",
        R"({"kind":"handoffControl","operation":"escalate","targetKind":"service","targetId":"human-support","shareContext":true,"consentId":["a","b"]})",
        R"({"kind":"handoffControl","operation":"escalate","targetKind":"service","targetId":"human-support","shareContext":true,"consentIds":["a"]})",
        R"({"kind":"handoffControl","operation":"escalate","targetKind":"service","targetId":"human-support","shareContext":true,"consentId":"bad\u0000ref"})",
        R"({"kind":"handoffControl","operation":"escalate","targetKind":"service","targetId":"human-support","shareContext":true,"consentId":"a","text":"private"})",
        R"({"kind":"handoffControl","operation":"cancel","shareContext":true,"consentId":"a"})"};
    for (const auto& body : invalid) {
        EXPECT_FALSE(local(body));
        EXPECT_EQ(network.sent.size(), count);
        EXPECT_TRUE(host.ai_owned());
        EXPECT_EQ(host.handoff_state(), AiHandoffState::AiActive);
    }
    ASSERT_TRUE(local(
        R"({"kind":"handoffControl","operation":"escalate","targetKind":"service","targetId":"human-support"})"));
    EXPECT_NE(network.sent.back().payload.find("\"shareContext\":false"), std::string::npos);
    EXPECT_EQ(network.sent.back().payload.find("consentIds"), std::string::npos);
}
// 丢失原HAL配对不能传分享引用，配对恢复不替代原物理源资格重新证明。
TEST_F(SharedHost, UnpairedHalCannotSubmitExplicitConsent) {
    established();
    acquire();
    const auto count = network.sent.size();
    paired = false;
    EXPECT_FALSE(local(
        R"({"kind":"handoffControl","operation":"escalate","targetKind":"service","targetId":"human-support","shareContext":true,"consentId":"owner-consent"})"));
    EXPECT_EQ(network.sent.size(), count);
    EXPECT_EQ(host.handoff_state(), AiHandoffState::AiActive);
}
// 独立时钟各自单调但取整相差一毫秒，不应让仍在原双截止内的完整取得被误拒。
TEST_F(HumanReceiver, OriginalAcquireSurvivesIndependentClockProgressBeforeActualFalse) {
    acquire_with_clocks(1, 0);
}
TEST_F(HumanReceiver, OriginalAcquireSurvivesWallClockProgressBeforeActualFalse) {
    acquire_with_clocks(0, 1);
}
TEST_F(HumanReceiver, OriginalAcquireSurvivesDifferentClockRoundingBeforeActualFalse) {
    acquire_with_clocks(2, 1);
}
// 主叫沿原AI取得、候选Peer、实际暂停再取得，不能由短协调器端口绕过共享驱动。
TEST_F(SharedHost, HumanAcquireKeepsOriginalBusinessDeadlineWithIndependentClocks) {
    acquire_human_with_clocks(1, 0);
}
TEST_F(SharedHost, HumanAcquireKeepsOriginalBusinessDeadlineWithWallClockProgress) {
    acquire_human_with_clocks(0, 1);
}
TEST_F(SharedHost, HumanAcquireKeepsOriginalBusinessDeadlineWithDifferentClockRounding) {
    acquire_human_with_clocks(2, 1);
}
void SharedHost::acquire_human_with_clocks(std::uint64_t monotonic_progress,
                                           std::uint64_t wall_progress) {
    candidate();
    Envelope suspend;
    suspend.token = "suspend-ai";
    suspend.method = "focusControl";
    suspend.session = "ai";
    suspend.event_sequence = 3;
    suspend.payload =
        R"({"operation":"suspend","expectedRevision":1,"focusId":"ai-focus","resources":["microphone","speaker"],"priority":"conversation","resumePolicy":"explicit"})";
    down(suspend);
    actual();
    ASSERT_TRUE(step());
    ASSERT_TRUE(step());
    ASSERT_EQ(host.handoff_state(), AiHandoffState::Suspended);
    Envelope acquire_human;
    acquire_human.token = "acquire-human";
    acquire_human.method = "focusControl";
    acquire_human.session = "local-human";
    acquire_human.event_sequence = 2;
    acquire_human.payload =
        R"({"operation":"acquire","expectedRevision":2,"focusId":"human-focus","resources":["microphone","speaker"],"priority":"call","resumePolicy":"explicit","expiresAt":200000})";
    down(acquire_human);
    now += monotonic_progress;
    wall += wall_progress;
    actual();
    ASSERT_TRUE(step());
    actual();
    ASSERT_TRUE(step());
    ASSERT_EQ(host.handoff_state(), AiHandoffState::Acquired);
    const auto receipt =
        std::find_if(network.sent.rbegin(), network.sent.rend(), [](const auto& value) {
            return value.method == "focusState" && value.token == "acquire-human";
        });
    ASSERT_NE(receipt, network.sent.rend());
    EXPECT_EQ(receipt->code, 200);
    EXPECT_FALSE(host.ai_owned());
    EXPECT_EQ(network.disconnects, 0U);
}
TEST_F(HumanReceiver, EndedWithoutFormalReleaseDoesNotDisposeTargetPeer) {
    acquire_target();
    establish_target();
    ASSERT_TRUE(local(R"({"kind":"handoffControl","operation":"hangup"})"));
    Envelope ended;
    ended.request = false;
    ended.token = "target-ended";
    ended.method = "sessionEnded";
    ended.session = "target-human";
    ended.event_sequence = 4;
    ended.payload = R"({"state":"ended","reason":"hangup"})";
    down(ended);
    actual();
    ASSERT_TRUE(step());
    EXPECT_EQ(network.disconnects, 0U);
    EXPECT_FALSE(std::any_of(outputs.begin(), outputs.end(), [](const auto& v) {
        return v.find("humanVoiceStop") != std::string::npos;
    }));
}
TEST_F(HumanReceiver, OwnFormalReleaseAndActualStoppedCloseSingleSessionHost) {
    acquire_target();
    establish_target();
    ASSERT_TRUE(local(R"({"kind":"handoffControl","operation":"hangup"})"));
    auto release = control("release", 1, 4);
    release.payload =
        R"({"operation":"release","expectedRevision":1,"focusId":"target-focus","resources":["microphone","speaker"],"priority":"call","resumePolicy":"never"})";
    down(release);
    actual();
    ASSERT_TRUE(step());
    actual();
    ASSERT_TRUE(step());
    const auto receipt =
        std::find_if(network.sent.rbegin(), network.sent.rend(), [](const auto& v) {
            return v.method == "focusState" && v.token == "target-release";
        });
    ASSERT_NE(receipt, network.sent.rend());
    EXPECT_EQ(receipt->code, 200) << receipt->payload;
    Envelope ended;
    ended.request = false;
    ended.token = "target-ended";
    ended.method = "sessionEnded";
    ended.session = "target-human";
    ended.event_sequence = 5;
    ended.payload = R"({"state":"ended","reason":"hangup"})";
    down(ended);
    ASSERT_TRUE(std::any_of(outputs.begin(), outputs.end(), [](const auto& v) {
        return v.find("humanVoiceStop") != std::string::npos;
    }));
    ASSERT_TRUE(local(
        R"({"kind":"humanVoiceStopped","sessionId":"target-human","routeGeneration":"2","peerEpoch":"1"})"));
    EXPECT_FALSE(step());
    EXPECT_EQ(host.connection_state(), SharedConnectionState::Closed);
    EXPECT_EQ(network.disconnects, 1U);
    EXPECT_FALSE(local(R"({"kind":"handoffControl","operation":"accept"})"));
    host.close(now);
    EXPECT_EQ(network.disconnects, 1U);
}
// 后端同腿先终态再正式release；同poll路由不能因清空Human会话丢失原完整租约。
TEST_F(HumanReceiver, EndedThenFormalReleaseInSamePollWaitsForActualStopped) {
    acquire_target();
    establish_target();
    // 原对端结束事件和后继release按权威水位同时排队，不调用Host内部释放捷径。
    Envelope ended;
    ended.request = false;
    ended.token = "target-ended-first";
    ended.method = "sessionEnded";
    ended.session = "target-human";
    ended.event_sequence = 4;
    ended.payload = R"({"state":"ended","reason":"hangup"})";
    auto release = control("release", 1, 5);
    release.payload =
        R"({"operation":"release","expectedRevision":1,"focusId":"target-focus","resources":["microphone","speaker"],"priority":"call","resumePolicy":"never"})";
    for (const auto& event : {ended, release}) {
        std::string bytes; // 仍由正式公共编码器与唯一连接路由读取原权威报文。
        ASSERT_EQ(encode_envelope(event, bytes), DecodeResult::Ok);
        network.events.push_back(
            {MqttEvent::Kind::Message, 0, 0, "$thing/down/signaling/p/d", bytes});
    }
    ASSERT_TRUE(step());
    EXPECT_EQ(network.disconnects, 0U);
    EXPECT_FALSE(std::any_of(network.sent.begin(), network.sent.end(), [](const auto& value) {
        return value.method == "focusState" && value.token == "target-release";
    }));
    EXPECT_FALSE(std::any_of(outputs.begin(), outputs.end(), [](const auto& value) {
        return value.find("humanVoiceStop") != std::string::npos;
    }));
    // 原关闭双票据与正式release双票据分别实际确认，不把本地静音当release200。
    actual();
    ASSERT_TRUE(step());
    actual();
    ASSERT_TRUE(step());
    const auto receipt =
        std::find_if(network.sent.begin(), network.sent.end(), [](const auto& value) {
            return value.method == "focusState" && value.token == "target-release";
        });
    ASSERT_NE(receipt, network.sent.end());
    std::string receipt_bytes, release_bytes; // 按真实线编码再核完整请求/应答关系。
    ASSERT_EQ(encode_envelope(*receipt, receipt_bytes), DecodeResult::Ok);
    ASSERT_EQ(encode_envelope(release, release_bytes), DecodeResult::Ok);
    FocusState checked; // ACK必须逐字关联原release及完整mic+speaker，不仅看code200。
    ASSERT_EQ(decode_focus_state(WireRoute::Signaling, receipt_bytes, checked), WireResult::Ok);
    FocusCommand original;
    ASSERT_EQ(decode_focus_control(WireRoute::Signaling, release_bytes, original), WireResult::Ok);
    EXPECT_TRUE(focus_reply_matches(original, checked));
    EXPECT_EQ(checked.state, "released");
    EXPECT_EQ(receipt->code, 200);
    EXPECT_EQ(network.disconnects, 0U);
    ASSERT_TRUE(std::any_of(outputs.begin(), outputs.end(), [](const auto& value) {
        return value.find("humanVoiceStop") != std::string::npos;
    }));
    ASSERT_TRUE(local(
        R"({"kind":"humanVoiceStopped","sessionId":"target-human","routeGeneration":"2","peerEpoch":"1"})"));
    EXPECT_FALSE(step());
    EXPECT_EQ(host.connection_state(), SharedConnectionState::Closed);
    EXPECT_EQ(network.disconnects, 1U);
    host.close(now);
    EXPECT_EQ(network.disconnects, 1U);
}
TEST_F(SharedHost, NoSourceOrReadyDoesNotConnect) {
    EXPECT_TRUE(step());
    EXPECT_EQ(network.connects, 0U);
    EXPECT_FALSE(local(R"({"kind":"start"})"));
}
TEST_F(SharedHost, WrongFormatCannotDeclareCapabilities) {
    EXPECT_FALSE(local(R"({"kind":"ready","sampleRate":16000,"frameMs":20,"aec":true})"));
    EXPECT_TRUE(step());
    EXPECT_EQ(network.connects, 0U);
}
TEST_F(SharedHost, EarlyActualInputProbeSurvivesReadyOrdering) {
    boot(true);
    EXPECT_EQ(network.connects, 1U);
    EXPECT_FALSE(host.ai_owned());
}
TEST_F(SharedHost, BootProbeAndOnlySpeakerCannotCreateConnection) {
    ASSERT_TRUE(local(R"({"kind":"ready","sampleRate":48000,"frameMs":20,"aec":true})"));
    ASSERT_TRUE(local(R"({"kind":"inputOwnershipIdle","bootGeneration":"7"})"));
    ASSERT_TRUE(step());
    actual("playbackGate");
    ASSERT_TRUE(step());
    EXPECT_EQ(network.connects, 0U);
}
TEST_F(SharedHost, ThreeSubscriptionsAndOneCapabilityAckOnly) {
    connected();
    EXPECT_EQ(network.connects, 1U);
    EXPECT_EQ(network.topics.size(), 3U);
    EXPECT_EQ(std::count_if(network.sent.begin(), network.sent.end(),
                            [](const auto& e) { return e.method == "capabilityReport"; }),
              1);
}
TEST_F(SharedHost, EstablishedDoesNotOwnOrStartMicrophone) {
    established();
    EXPECT_FALSE(host.ai_owned());
    EXPECT_FALSE(local(R"({"kind":"start"})"));
    EXPECT_EQ(media.starts, 0U);
}
TEST_F(SharedHost, OriginalInputBindingPrecedesAIFalseAndAcquireGates) {
    established();
    acquire();
    ASSERT_TRUE(host.ai_owned());
    input_binding("ai", "1", "udp");
    acquire(); // 同原focus重放不登记第二条页面路由。
    input_binding("ai", "1", "udp");
}
TEST_F(HumanReceiver, OriginalInputBindingPrecedesActualHumanSenderGates) {
    acquire_target();
    input_binding("target-human", "2", "humanSender");
    down(control());
    input_binding("target-human", "2", "humanSender");
}
TEST_F(SharedHost, EscapedOriginalSessionBindsWithoutChangingInputScope) {
    const std::string session = "ai\"\\scope";
    established(session);
    acquire(session);
    ASSERT_TRUE(host.ai_owned());
    input_binding(session, "1", "udp");
}
TEST_F(SharedHost, RejectedBindingSendCannotQueueGateOrClaimAcquired) {
    established();
    reject_input_binding = true;
    Envelope control;
    control.token = "initial-focus";
    control.method = "focusControl";
    control.session = "ai";
    control.event_sequence = 2;
    control.payload =
        R"({"operation":"acquire","expectedRevision":0,"focusId":"ai-focus","resources":["microphone","speaker"],"priority":"conversation","resumePolicy":"explicit","expiresAt":200000})";
    std::string bytes;
    ASSERT_EQ(encode_envelope(control, bytes), DecodeResult::Ok);
    network.events.push_back({MqttEvent::Kind::Message, 0, 0, "$thing/down/signaling/p/d", bytes});
    EXPECT_FALSE(step());
    EXPECT_FALSE(host.ai_owned());
    EXPECT_EQ(media.starts, 0U);
    EXPECT_FALSE(std::any_of(outputs.begin(), outputs.end(), [](const auto& body) {
        return body.find("inputOwnershipGate") != std::string::npos;
    }));
    EXPECT_FALSE(std::any_of(network.sent.begin(), network.sent.end(),
                             [](const auto& event) { return event.method == "focusState"; }));
}
TEST_F(SharedHost, InvalidUTF8ControlCannotEmitBindingOrGate) {
    established();
    const std::string invalid =
        std::string(R"({"msgToken":"bad","method":"focusControl","sessionId":")") + char(0xc0) +
        char(0x80) + R"(","params":{}})";
    network.events.push_back(
        {MqttEvent::Kind::Message, 0, 0, "$thing/down/signaling/p/d", invalid});
    ASSERT_TRUE(step());
    EXPECT_FALSE(host.ai_owned());
    EXPECT_FALSE(std::any_of(outputs.begin(), outputs.end(), [](const auto& body) {
        return body.find("inputOwnershipBind") != std::string::npos ||
               body.find("inputOwnershipGate") != std::string::npos;
    }));
}
TEST_F(SharedHost, CompleteActualFocusOpensOriginalAIOnly) {
    established();
    acquire();
    ASSERT_TRUE(host.ai_owned());
    ASSERT_TRUE(local(R"({"kind":"start"})"));
    EXPECT_EQ(media.starts, 0U);
    ASSERT_EQ(network.sent.back().method, "audioStart");
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> request(
        cJSON_Parse(network.sent.back().payload.c_str()), cJSON_Delete);
    const auto* id = cJSON_GetObjectItemCaseSensitive(request.get(), "inputId");
    ASSERT_TRUE(cJSON_IsString(id));
    Envelope ack;
    ack.request = false;
    ack.token = network.sent.back().token;
    ack.session = "ai";
    ack.method = "requestAck";
    ack.code = 200;
    ack.payload = std::string("{\"requestMethod\":\"audioStart\",\"result\":{\"inputId\":\"") +
                  id->valuestring + "\"}}";
    down(ack);
    EXPECT_EQ(media.starts, 1U);
    EXPECT_EQ(network.connects, 1U);
}
TEST_F(SharedHost, EscalateCannotStartBeforeActualLease) {
    established();
    EXPECT_FALSE(local(
        R"({"kind":"handoffControl","operation":"escalate","targetKind":"device","targetId":"other"})"));
    EXPECT_EQ(host.handoff_state(), AiHandoffState::AiActive);
}
TEST_F(SharedHost, LocalIntentForbidsSessionAndFocusForgery) {
    established();
    acquire();
    EXPECT_FALSE(local(
        R"({"kind":"handoffControl","operation":"escalate","targetKind":"device","targetId":"other","sessionId":"ai"})"));
    EXPECT_FALSE(local(R"({"kind":"handoffControl","operation":"hangup","focusId":"ai-focus"})"));
    EXPECT_TRUE(host.ai_owned());
}
TEST_F(SharedHost, EscalateUsesSharedConnectionAndOriginalLease) {
    established();
    acquire();
    ASSERT_TRUE(host.ai_owned());
    ASSERT_TRUE(local(
        R"({"kind":"handoffControl","operation":"escalate","targetKind":"device","targetId":"other"})"));
    EXPECT_EQ(host.handoff_state(), AiHandoffState::Binding);
    EXPECT_EQ(network.sent.back().method, "callControl");
    EXPECT_EQ(network.sent.back().session, "ai");
    EXPECT_EQ(network.connects, 1U);
    EXPECT_TRUE(host.ai_owned());
}
TEST_F(SharedHost, NativeDeadlineNeverRenewedByLocalTraffic) {
    boot();
    EXPECT_FALSE(host.poll(500000, wall));
    EXPECT_EQ(network.connects, 1U);
}
TEST_F(SharedHost, CloseOwnsUniqueNetworkOnce) {
    connected();
    host.close(now);
    host.close(now);
    EXPECT_EQ(network.disconnects, 1U);
    EXPECT_FALSE(host.poll(now, wall));
}
TEST_F(SharedHost, DisconnectedSourceCannotKeepLease) {
    established();
    acquire();
    paired = false;
    EXPECT_FALSE(step());
    EXPECT_FALSE(host.ai_owned());
}
TEST_F(SharedHost, CandidatePeerDoesNotAcquireMicrophoneOrStopAI) {
    candidate();
    EXPECT_TRUE(host.ai_owned());
    EXPECT_EQ(host.handoff_state(), AiHandoffState::Candidate);
    EXPECT_EQ(media.starts, 0U);
    EXPECT_EQ(network.connects, 1U);
    EXPECT_EQ(network.disconnects, 0U);
    EXPECT_FALSE(local(R"({"kind":"handoffControl","operation":"hangup"})"));
    EXPECT_TRUE(host.ai_owned());
}
TEST_F(SharedHost, ConnectedPeerWithoutFreshActualOutputCannotDeclareMediaReady) {
    candidate(false);
    EXPECT_TRUE(host.ai_owned());
    EXPECT_EQ(host.handoff_state(), AiHandoffState::Candidate);
    EXPECT_EQ(network.disconnects, 0U);
    EXPECT_FALSE(std::any_of(network.sent.begin(), network.sent.end(), [](const auto& e) {
        return e.method == "mediaState" && e.session == "local-human";
    }));
    actual();
    ASSERT_TRUE(step());
    EXPECT_EQ(network.sent.back().method, "mediaState");
    EXPECT_TRUE(host.ai_owned());
}
TEST_F(SharedHost, FreshCandidateOutputTimeoutKeepsOriginalActualAILease) {
    candidate(false);
    now += 2000;
    wall += 2000;
    ASSERT_TRUE(step());
    EXPECT_TRUE(host.ai_owned());
    EXPECT_EQ(network.disconnects, 0U);
    EXPECT_EQ(host.connection_state(), SharedConnectionState::Ready);
    EXPECT_FALSE(std::any_of(network.sent.begin(), network.sent.end(), [](const auto& e) {
        return e.method == "mediaState" && e.session == "local-human";
    }));
}
TEST_F(SharedHost, ActualCandidateStoppedKeepsOriginalActualAILease) {
    candidate();
    ASSERT_TRUE(local(
        R"({"kind":"humanVoiceStopped","sessionId":"local-human","routeGeneration":"2","peerEpoch":"1"})"));
    ASSERT_TRUE(step());
    EXPECT_TRUE(host.ai_owned());
    EXPECT_EQ(network.disconnects, 0U);
    EXPECT_FALSE(local(R"({"kind":"handoffControl","operation":"hangup"})"));
}
void SharedHost::commit_human(bool holdable, bool established_in_receipt_poll,
                              bool final_before_local_established) {
    candidate();
    Envelope suspend;
    suspend.token = "suspend-ai";
    suspend.method = "focusControl";
    suspend.session = "ai";
    suspend.event_sequence = 3;
    suspend.payload =
        R"({"operation":"suspend","expectedRevision":1,"focusId":"ai-focus","resources":["microphone","speaker"],"priority":"conversation","resumePolicy":"explicit"})";
    down(suspend);
    actual();
    ASSERT_TRUE(step());
    ASSERT_TRUE(step());
    const auto actual_suspend =
        std::find_if(network.sent.rbegin(), network.sent.rend(), [](const auto& e) {
            return e.method == "focusState" && e.token == "suspend-ai";
        });
    ASSERT_NE(actual_suspend, network.sent.rend());
    EXPECT_EQ(actual_suspend->code, 200) << actual_suspend->payload;
    ASSERT_EQ(host.handoff_state(), AiHandoffState::Suspended);
    EXPECT_FALSE(host.ai_owned());
    Envelope acquire_human;
    acquire_human.token = "acquire-human";
    acquire_human.method = "focusControl";
    acquire_human.session = "local-human";
    acquire_human.event_sequence = 2;
    acquire_human.payload =
        R"({"operation":"acquire","expectedRevision":2,"focusId":"human-focus","resources":["microphone","speaker"],"priority":"call","resumePolicy":"never","expiresAt":200000})";
    if (holdable) {
        const auto policy = acquire_human.payload.find("never");
        ASSERT_NE(policy, std::string::npos);
        acquire_human.payload.replace(policy, 5, "explicit");
    }
    down(acquire_human);
    actual();
    ASSERT_TRUE(step());
    Envelope established;
    established.request = false;
    established.token = "human-established";
    established.method = "sessionEstablished";
    established.session = "local-human";
    established.event_sequence = 3;
    established.payload = R"({"state":"established","mediaRevision":1})";
    actual();
    if (established_in_receipt_poll) {
        std::string bytes;
        ASSERT_EQ(encode_envelope(established, bytes), DecodeResult::Ok);
        network.events.push_back(
            {MqttEvent::Kind::Message, 0, 0, "$thing/down/signaling/p/d", bytes});
    }
    ASSERT_TRUE(step());
    ASSERT_TRUE(step());
    ASSERT_EQ(host.handoff_state(), AiHandoffState::Acquired);
    if (!established_in_receipt_poll && !final_before_local_established)
        down(established);
    EXPECT_EQ(host.handoff_state(), AiHandoffState::Acquired);
    const auto call = std::find_if(network.sent.begin(), network.sent.end(),
                                   [](const auto& e) { return e.method == "callControl"; });
    ASSERT_NE(call, network.sent.end());
    Envelope commit;
    commit.request = false;
    commit.token = "commit";
    commit.method = "callState";
    commit.session = "ai";
    commit.event_sequence = 4;
    commit.payload = "{\"state\":\"transferred\",\"revision\":2,\"candidateSessionId\":"
                     "\"remote-human\",\"localSessionId\":\"local-human\",\"callGroupId\":"
                     "\"group\",\"requestToken\":\"" +
                     call->token + "\"}";
    down(commit);
    ASSERT_EQ(host.handoff_state(), final_before_local_established ? AiHandoffState::Acquired
                                                                   : AiHandoffState::HumanActive);
    // 持久提交触发原AI围栏；仍由原线程票据实际确认，不能跳过迟到旧腿关闭事实。
    actual();
    ASSERT_TRUE(step());
    actual();
    ASSERT_TRUE(step());
    Envelope ended;
    ended.request = false;
    ended.token = "ai-ended";
    ended.method = "sessionEnded";
    ended.session = "ai";
    ended.event_sequence = 5;
    ended.payload = R"({"state":"ended","reason":"transferred"})";
    if (final_before_local_established) {
        // 外腿终态不能补足本地Established；即使父腿已提交，也不得提前授予保持资格。
        auto unrelated = ended;
        unrelated.session = "unrelated-ai";
        down(unrelated);
        ASSERT_EQ(host.handoff_state(), AiHandoffState::Acquired);
        EXPECT_FALSE(local(R"({"kind":"handoffControl","operation":"hold"})"));
    }
    down(ended);
    if (final_before_local_established) {
        // 两个父腿事件保持原序号；本地腿的合法Established迟到仍必须完成原三屏障。
        ASSERT_EQ(host.handoff_state(), AiHandoffState::Acquired);
        EXPECT_FALSE(local(R"({"kind":"handoffControl","operation":"hold"})"));
        down(ended);
        ASSERT_EQ(host.handoff_state(), AiHandoffState::Acquired);
        EXPECT_FALSE(local(R"({"kind":"handoffControl","operation":"hold"})"));
        down(established);
        actual();
        ASSERT_TRUE(step());
        actual();
        ASSERT_TRUE(step());
    }
    EXPECT_EQ(host.handoff_state(), AiHandoffState::HumanActive);
    EXPECT_EQ(network.disconnects, 0U);
}
TEST_F(SharedHost, ThreeBarriersCommitHumanWithoutDisconnectingSharedNetwork) {
    commit_human();
    ASSERT_EQ(host.handoff_state(), AiHandoffState::HumanActive);
    EXPECT_FALSE(local(R"({"kind":"start"})"));
    EXPECT_TRUE(step());
    EXPECT_TRUE(local(R"({"kind":"handoffControl","operation":"hangup"})"));
    EXPECT_EQ(network.disconnects, 0U);
}
// 合法最终事件与原actual回执同poll到达时不得永久丢失保持资格，且不靠MQTT重投补救。
TEST_F(SharedHost, EstablishedInActualReceiptPollRetainsOriginalHold) {
    ASSERT_NO_FATAL_FAILURE(commit_human(true, true));
    ASSERT_EQ(host.handoff_state(), AiHandoffState::HumanActive);
    Envelope release;
    release.token = "release-original-ai";
    release.method = "focusControl";
    release.session = "ai";
    release.event_sequence = 6;
    release.payload =
        R"({"operation":"release","expectedRevision":3,"focusId":"ai-focus","resources":["microphone","speaker"],"priority":"conversation","resumePolicy":"explicit"})";
    down(release);
    actual();
    ASSERT_TRUE(step());
    const auto released =
        std::find_if(network.sent.rbegin(), network.sent.rend(), [](const auto& value) {
            return value.method == "focusState" && value.token == "release-original-ai";
        });
    ASSERT_NE(released, network.sent.rend());
    ASSERT_EQ(released->code, 200);
    ASSERT_TRUE(local(R"({"kind":"handoffControl","operation":"hold"})"));
    ASSERT_TRUE(step());
    ASSERT_EQ(network.sent.back().method, "callControl");
    EXPECT_EQ(network.sent.back().session, "local-human");
    EXPECT_EQ(network.sent.back().payload, R"({"expectedRevision":0,"operation":"hold"})");
    EXPECT_FALSE(local(R"({"kind":"handoffControl","operation":"hold"})"));
    EXPECT_EQ(network.disconnects, 0U);
}
// 父腿已提交并结束后，本地腿Established仍须保留主叫的真实围栏与保持资格。
TEST_F(SharedHost, ParentFinalBeforeLocalEstablishedRetainsOriginalHold) {
    ASSERT_NO_FATAL_FAILURE(commit_human(true, false, true));
    ASSERT_EQ(host.handoff_state(), AiHandoffState::HumanActive);
    // 正式释放仍经原完整组CAS和实际双票据确认，不能以父腿Ended代替焦点释放。
    Envelope release;
    release.token = "release-original-ai";
    release.method = "focusControl";
    release.session = "ai";
    release.event_sequence = 6;
    release.payload =
        R"({"operation":"release","expectedRevision":3,"focusId":"ai-focus","resources":["microphone","speaker"],"priority":"conversation","resumePolicy":"explicit"})";
    down(release);
    actual();
    ASSERT_TRUE(step());
    const auto released =
        std::find_if(network.sent.rbegin(), network.sent.rend(), [](const auto& value) {
            return value.method == "focusState" && value.token == "release-original-ai";
        });
    ASSERT_NE(released, network.sent.rend());
    ASSERT_EQ(released->code, 200) << released->payload;
    ASSERT_TRUE(local(R"({"kind":"handoffControl","operation":"hold"})"));
    ASSERT_TRUE(step());
    ASSERT_EQ(network.sent.back().method, "callControl");
    EXPECT_EQ(network.sent.back().session, "local-human");
    EXPECT_EQ(network.sent.back().payload, R"({"expectedRevision":0,"operation":"hold"})");
    EXPECT_EQ(network.disconnects, 0U);
}
// 发起端新显式Human策略须通过原移交三屏障，再允许本地控制原人工腿。
TEST_F(SharedHost, ExplicitHumanAcquireCanHoldAfterOriginalThreeBarriers) {
    ASSERT_NO_FATAL_FAILURE(commit_human(true));
    ASSERT_EQ(host.handoff_state(), AiHandoffState::HumanActive);
    // 旧AI已围栏不等于已正式释放；先按原Global CAS释放它才能接纳新人工保持操作。
    Envelope release;
    release.token = "release-original-ai";
    release.method = "focusControl";
    release.session = "ai";
    release.event_sequence = 6;
    release.payload =
        R"({"operation":"release","expectedRevision":3,"focusId":"ai-focus","resources":["microphone","speaker"],"priority":"conversation","resumePolicy":"explicit"})";
    down(release);
    actual();
    ASSERT_TRUE(step());
    const auto released =
        std::find_if(network.sent.rbegin(), network.sent.rend(), [](const auto& value) {
            return value.method == "focusState" && value.token == "release-original-ai";
        });
    ASSERT_NE(released, network.sent.rend());
    ASSERT_EQ(released->code, 200) << released->payload;
    ASSERT_TRUE(local(R"({"kind":"handoffControl","operation":"hold"})"));
    ASSERT_TRUE(step());
    ASSERT_EQ(network.sent.back().method, "callControl");
    EXPECT_EQ(network.sent.back().session, "local-human");
    EXPECT_EQ(network.sent.back().payload, R"({"expectedRevision":0,"operation":"hold"})");
    const auto hold_token = network.sent.back().token;
    // 焦点资源CAS独立于callRevision；原AI正式释放后完整组资源版本为4。
    Envelope focus;
    focus.token = hold_token;
    focus.method = "focusControl";
    focus.session = "local-human";
    focus.event_sequence = 4;
    focus.payload =
        R"({"operation":"suspend","expectedRevision":4,"focusId":"human-focus","resources":["microphone","speaker"],"priority":"call","resumePolicy":"explicit"})";
    down(focus);
    EXPECT_FALSE(local(R"({"kind":"handoffControl","operation":"resume"})"));
    actual();
    ASSERT_TRUE(step());
    Envelope ack;
    ack.request = false;
    ack.token = hold_token;
    ack.method = "callState";
    ack.session = "local-human";
    ack.code = 200;
    ack.payload = R"({"state":"held","revision":1})";
    const auto suspend_receipt =
        std::find_if(network.sent.rbegin(), network.sent.rend(), [&hold_token](const auto& value) {
            return value.method == "focusState" && value.token == hold_token;
        });
    ASSERT_NE(suspend_receipt, network.sent.rend());
    ASSERT_EQ(suspend_receipt->code, 200) << suspend_receipt->payload;
    down(ack);
    Envelope event = ack;
    event.code.reset();
    event.token = "caller-held-event";
    event.event_sequence = 5;
    event.payload = "{\"state\":\"held\",\"revision\":1,\"requestToken\":\"" + hold_token + "\"}";
    down(event);
    ASSERT_TRUE(step());
    ASSERT_EQ(host.handoff_state(), AiHandoffState::HumanActive);
    ASSERT_TRUE(local(R"({"kind":"handoffControl","operation":"resume"})"));
    ASSERT_TRUE(step());
    ASSERT_EQ(network.sent.back().method, "callControl");
    const auto resume_token = network.sent.back().token;
    ASSERT_NE(resume_token, hold_token);
    EXPECT_EQ(network.sent.back().payload, R"({"expectedRevision":1,"operation":"resume"})");
    focus.token = resume_token;
    focus.event_sequence = 6;
    focus.payload =
        R"({"operation":"restore","expectedRevision":5,"focusId":"human-focus","resources":["microphone","speaker"],"priority":"call","resumePolicy":"explicit","expiresAt":200000})";
    down(focus);
    actual();
    ASSERT_TRUE(step());
    ack.token = resume_token;
    ack.payload = R"({"state":"active","revision":2})";
    down(ack);
    event.token = "caller-active-event";
    event.event_sequence = 7;
    event.payload =
        "{\"state\":\"active\",\"revision\":2,\"requestToken\":\"" + resume_token + "\"}";
    down(event);
    ASSERT_TRUE(step());
    EXPECT_EQ(host.handoff_state(), AiHandoffState::HumanActive);
    EXPECT_TRUE(local(R"({"kind":"handoffControl","operation":"hangup"})"));
    EXPECT_EQ(network.disconnects, 0U);
}
TEST_F(SharedHost, HumanEndedWithoutFormalReleaseDoesNotDisposeSharedPeer) {
    commit_human();
    ASSERT_EQ(host.handoff_state(), AiHandoffState::HumanActive);
    ASSERT_TRUE(local(R"({"kind":"handoffControl","operation":"hangup"})"));
    Envelope ended;
    ended.request = false;
    ended.token = "human-ended";
    ended.method = "sessionEnded";
    ended.session = "local-human";
    ended.event_sequence = 4;
    ended.payload = R"({"state":"ended","reason":"hangup"})";
    down(ended);
    actual();
    ASSERT_TRUE(step());
    EXPECT_EQ(network.disconnects, 0U);
    EXPECT_FALSE(std::any_of(outputs.begin(), outputs.end(), [](const auto& body) {
        return body.find("humanVoiceStop") != std::string::npos;
    }));
}
TEST_F(SharedHost, FormalReleaseBeforeEndedAndActualStoppedCleansOneConnection) {
    commit_human();
    ASSERT_EQ(host.handoff_state(), AiHandoffState::HumanActive);
    ASSERT_TRUE(local(R"({"kind":"handoffControl","operation":"hangup"})"));
    Envelope ai_release;
    ai_release.token = "release-ai";
    ai_release.method = "focusControl";
    ai_release.session = "ai";
    ai_release.event_sequence = 6;
    ai_release.payload =
        R"({"operation":"release","expectedRevision":3,"focusId":"ai-focus","resources":["microphone","speaker"],"priority":"conversation","resumePolicy":"explicit"})";
    down(ai_release);
    actual();
    ASSERT_TRUE(step());
    actual();
    ASSERT_TRUE(step());
    Envelope human_release;
    human_release.token = "release-human";
    human_release.method = "focusControl";
    human_release.session = "local-human";
    human_release.event_sequence = 4;
    human_release.payload =
        R"({"operation":"release","expectedRevision":4,"focusId":"human-focus","resources":["microphone","speaker"],"priority":"call","resumePolicy":"never"})";
    down(human_release);
    actual();
    ASSERT_TRUE(step());
    actual();
    ASSERT_TRUE(step());
    const auto released =
        std::find_if(network.sent.rbegin(), network.sent.rend(), [](const auto& e) {
            return e.method == "focusState" && e.token == "release-human";
        });
    if (released == network.sent.rend()) {
        for (const auto& event : network.sent)
            if (event.method == "focusState")
                ADD_FAILURE() << event.token << " code=" << event.code.value_or(0) << " "
                              << event.payload;
    }
    ASSERT_NE(released, network.sent.rend());
    EXPECT_EQ(released->code, 200) << released->payload;
    EXPECT_EQ(network.disconnects, 0U);
    Envelope ended;
    ended.request = false;
    ended.token = "human-ended";
    ended.method = "sessionEnded";
    ended.session = "local-human";
    ended.event_sequence = 5;
    ended.payload = R"({"state":"ended","reason":"hangup"})";
    down(ended);
    ASSERT_TRUE(std::any_of(outputs.begin(), outputs.end(), [](const auto& body) {
        return body.find("humanVoiceStop") != std::string::npos;
    }));
    EXPECT_EQ(network.disconnects, 0U);
    ASSERT_TRUE(local(
        R"({"kind":"humanVoiceStopped","sessionId":"local-human","routeGeneration":"2","peerEpoch":"1"})"));
    EXPECT_FALSE(step());
    EXPECT_EQ(host.connection_state(), SharedConnectionState::Closed);
    EXPECT_EQ(network.disconnects, 1U);
    host.close(now);
    EXPECT_EQ(network.disconnects, 1U);
}
// 主叫原sessionEnd的精确ACK先到，仍须后继人工正式release及实际Stopped才能关闭共享连接。
TEST_F(SharedHost, SessionEndAckThenFormalReleaseWaitsForBothLeasesAndActualStopped) {
    commit_human();
    ASSERT_EQ(host.handoff_state(), AiHandoffState::HumanActive);
    // 原AI已经终态，先完成它的正式释放，不能用finally替代两租约settled判据。
    Envelope ai_release;
    ai_release.token = "release-ai-before-human-end";
    ai_release.method = "focusControl";
    ai_release.session = "ai";
    ai_release.event_sequence = 6;
    ai_release.payload =
        R"({"operation":"release","expectedRevision":3,"focusId":"ai-focus","resources":["microphone","speaker"],"priority":"conversation","resumePolicy":"explicit"})";
    down(ai_release);
    actual();
    ASSERT_TRUE(step());
    actual();
    ASSERT_TRUE(step());
    ASSERT_TRUE(std::any_of(network.sent.begin(), network.sent.end(), [](const auto& value) {
        return value.method == "focusState" && value.token == "release-ai-before-human-end" &&
               value.code == 200;
    }));
    ASSERT_TRUE(local(R"({"kind":"handoffControl","operation":"hangup"})"));
    const auto request =
        std::find_if(network.sent.rbegin(), network.sent.rend(), [](const auto& value) {
            return value.method == "sessionEnd" && value.session == "local-human";
        });
    ASSERT_NE(request, network.sent.rend());
    Envelope ended; // 只承认原本地主动挂断的token与会话，不以无关联终态代替ACK。
    ended.request = false;
    ended.token = request->token;
    ended.method = "sessionEnded";
    ended.session = request->session;
    ended.code = 200;
    ended.event_sequence = 4;
    ended.payload = R"({"state":"ended","reason":"hangup"})";
    down(ended);
    EXPECT_EQ(network.disconnects, 0U);
    EXPECT_FALSE(std::any_of(outputs.begin(), outputs.end(), [](const auto& value) {
        return value.find("humanVoiceStop") != std::string::npos;
    }));
    Envelope release; // 人工正式释放只能使用其后继权威水位及AI释放后的实际全局修订。
    release.token = "release-human-after-end";
    release.method = "focusControl";
    release.session = "local-human";
    release.event_sequence = 5;
    release.payload =
        R"({"operation":"release","expectedRevision":4,"focusId":"human-focus","resources":["microphone","speaker"],"priority":"call","resumePolicy":"never"})";
    down(release);
    EXPECT_FALSE(std::any_of(network.sent.begin(), network.sent.end(), [](const auto& value) {
        return value.method == "focusState" && value.token == "release-human-after-end";
    }));
    EXPECT_EQ(network.disconnects, 0U);
    actual();
    ASSERT_TRUE(step());
    actual();
    ASSERT_TRUE(step());
    const auto receipt =
        std::find_if(network.sent.begin(), network.sent.end(), [](const auto& value) {
            return value.method == "focusState" && value.token == "release-human-after-end";
        });
    ASSERT_NE(receipt, network.sent.end());
    std::string receipt_bytes, release_bytes; // 不以fixture手填ACK替代正式协议校验。
    ASSERT_EQ(encode_envelope(*receipt, receipt_bytes), DecodeResult::Ok);
    ASSERT_EQ(encode_envelope(release, release_bytes), DecodeResult::Ok);
    FocusState checked;
    ASSERT_EQ(decode_focus_state(WireRoute::Signaling, receipt_bytes, checked), WireResult::Ok);
    FocusCommand original;
    ASSERT_EQ(decode_focus_control(WireRoute::Signaling, release_bytes, original), WireResult::Ok);
    EXPECT_TRUE(focus_reply_matches(original, checked));
    EXPECT_EQ(checked.state, "released");
    EXPECT_EQ(receipt->code, 200);
    EXPECT_EQ(network.disconnects, 0U);
    ASSERT_TRUE(std::any_of(outputs.begin(), outputs.end(), [](const auto& value) {
        return value.find("humanVoiceStop") != std::string::npos;
    }));
    ASSERT_TRUE(local(
        R"({"kind":"humanVoiceStopped","sessionId":"local-human","routeGeneration":"2","peerEpoch":"1"})"));
    EXPECT_FALSE(step());
    EXPECT_EQ(host.connection_state(), SharedConnectionState::Closed);
    EXPECT_EQ(network.disconnects, 1U);
    host.close(now);
    EXPECT_EQ(network.disconnects, 1U);
}
// 原shared Host直接消费旁证不读配对源、不推进时钟/焦点；真正Stopped仍走原退出分支。
TEST_F(DiagnosticReceiver, ScopedStopDiagnosticPrecedesStoppedWithoutDrivingMedia) {
    prepare();
    const auto reads = source_reads, starts = media.starts;
    const auto sends = network.sent.size(), emits = outputs.size();
    const std::string diagnostic =
        R"({"kind":"humanVoiceStopDiagnostic","sessionId":"target-human","routeGeneration":"2","peerEpoch":"1","known":true,"cause":"peerFailed"})";
    ASSERT_TRUE(host.receive_local(diagnostic, 900000, 900000));
    EXPECT_EQ(source_reads, reads);
    EXPECT_EQ(media.starts, starts);
    EXPECT_EQ(network.sent.size(), sends);
    EXPECT_EQ(outputs.size(), emits);
    EXPECT_EQ(
        host.human_media_failure_diagnostic_line(),
        R"({"kind":"humanMediaFailureDiagnostic","reason":"unknown","stopCause":"peerFailed"})");
    EXPECT_TRUE(step());
    ASSERT_TRUE(local(diagnostic));
    ASSERT_TRUE(local(
        R"({"kind":"humanVoiceStopped","sessionId":"target-human","routeGeneration":"2","peerEpoch":"1"})"));
    EXPECT_FALSE(step());
    EXPECT_EQ(
        host.human_media_failure_diagnostic_line(),
        R"({"kind":"humanMediaFailureDiagnostic","reason":"remoteStopped","stopCause":"peerFailed"})");
    host.close(now);
    EXPECT_EQ(
        host.human_media_failure_diagnostic_line(),
        R"({"kind":"humanMediaFailureDiagnostic","reason":"remoteStopped","stopCause":"peerFailed"})");
}
TEST_F(DiagnosticCaller, ScopedStopDiagnosticCallerRemainsIndependentOfAiAuthority) {
    candidate();
    const auto reads = source_reads;
    ASSERT_TRUE(local(
        R"({"kind":"humanVoiceStopDiagnostic","sessionId":"local-human","routeGeneration":"2","peerEpoch":"1","known":true,"cause":"remoteSinkRejected"})"));
    EXPECT_EQ(source_reads, reads);
    EXPECT_TRUE(host.ai_owned());
    EXPECT_EQ(
        host.human_media_failure_diagnostic_line(),
        R"({"kind":"humanMediaFailureDiagnostic","reason":"unknown","stopCause":"remoteSinkRejected"})");
    EXPECT_FALSE(local(
        R"({"kind":"humanVoiceStopDiagnostic","sessionId":"local-human","routeGeneration":"2","peerEpoch":"1","known":true,"cause":"peerFailed"})"));
    ASSERT_TRUE(local(
        R"({"kind":"humanVoiceStopped","sessionId":"local-human","routeGeneration":"2","peerEpoch":"1"})"));
    // 候选尚未申请Human完整组；沿原隔离分支保留AI，不能把诊断消息变成共享宿主退出授权。
    ASSERT_TRUE(step());
    EXPECT_TRUE(host.ai_owned());
    EXPECT_EQ(network.disconnects, 0U);
    EXPECT_EQ(
        host.human_media_failure_diagnostic_line(),
        R"({"kind":"humanMediaFailureDiagnostic","reason":"remoteStopped","stopCause":"remoteSinkRejected"})");
}
// 原三屏障已提交的Human才须在实际Stopped后退出；不是机械放宽候选的退出断言。
TEST_F(DiagnosticCaller, ScopedStopDiagnosticActivatedCallerKeepsOriginalFailure) {
    commit_human();
    ASSERT_EQ(host.handoff_state(), AiHandoffState::HumanActive);
    ASSERT_TRUE(local(
        R"({"kind":"humanVoiceStopDiagnostic","sessionId":"local-human","routeGeneration":"2","peerEpoch":"1","known":true,"cause":"peerFailed"})"));
    ASSERT_TRUE(local(
        R"({"kind":"humanVoiceStopped","sessionId":"local-human","routeGeneration":"2","peerEpoch":"1"})"));
    EXPECT_FALSE(step());
    EXPECT_EQ(
        host.human_media_failure_diagnostic_line(),
        R"({"kind":"humanMediaFailureDiagnostic","reason":"remoteStopped","stopCause":"peerFailed"})");
}
// 文字提交通过真实Host/VoiceRuntime/router；网络ACK由夹具显式注入，不冒称真实供应商或实际播放验收。
TEST_F(SharedHost,
              TextSubmissionReportsAcceptedAndBusyWithoutChangingAdmission) {
    established();
    acquire();
    ASSERT_TRUE(local(R"({"kind":"sendText","text":"合成文字","audio":false})"));
    EXPECT_NE(std::find(outputs.begin(), outputs.end(),
                                            R"({"kind":"textSubmitted","result":"submitted"})"),
                        outputs.end());
    const auto sent = network.sent.size();
    EXPECT_FALSE(
            local(R"({"kind":"sendText","text":"第二次合成文字","audio":false})"));
    EXPECT_EQ(network.sent.size(), sent);
    EXPECT_NE(std::find(outputs.begin(), outputs.end(),
                                            R"({"kind":"textSubmitted","result":"busy"})"),
                        outputs.end());
}
TEST_F(SharedHost, TextSubmissionReportsUnsupportedActualSelection) {
    established("ai", false);
    acquire();
    const auto sent = network.sent.size();
    EXPECT_FALSE(
            local(R"({"kind":"sendText","text":"未协商文字","audio":false})"));
    EXPECT_EQ(network.sent.size(), sent);
    EXPECT_NE(std::find(outputs.begin(), outputs.end(),
                                            R"({"kind":"textSubmitted","result":"unsupported"})"),
                        outputs.end());
}
TEST_F(SharedHost, TextSubmissionReportsRejectedBeforeEstablished) {
    connected();
    EXPECT_FALSE(local(R"({"kind":"sendText","text":"尚未建立","audio":false})"));
    EXPECT_NE(std::find(outputs.begin(), outputs.end(),
                                            R"({"kind":"textSubmitted","result":"rejected"})"),
                        outputs.end());
}
TEST_F(SharedHost,
              TextCloudAcknowledgementIsVisibleAndDoesNotExposeInputIdentity) {
    established();
    acquire();
    for (const auto code : {200, 409, 422, 403}) {
        ASSERT_TRUE(
                local(R"({"kind":"sendText","text":"合成请求","audio":false})"));
        const auto request = network.sent.back();
        ASSERT_EQ(request.method, "inputSend");
        std::unique_ptr<cJSON, decltype(&cJSON_Delete)> params(
                cJSON_Parse(request.payload.c_str()), cJSON_Delete);
        const auto *input =
                cJSON_GetObjectItemCaseSensitive(params.get(), "inputId");
        ASSERT_TRUE(cJSON_IsString(input));
        Envelope ack;
        ack.request = false;
        ack.token = request.token;
        ack.session = "ai";
        ack.method = "requestAck";
        ack.code = code;
        ack.payload =
                code == 200
                        ? std::string(
                                    R"({"requestMethod":"inputSend","result":{"inputId":")") +
                                input->valuestring + R"(","state":"accepted"}})"
                        : R"({"requestMethod":"inputSend","reason":"inputRejected"})";
        std::string bytes;
        ASSERT_EQ(encode_envelope(ack, bytes), DecodeResult::Ok);
        network.events.push_back(
                {MqttEvent::Kind::Message, 0, 0, "$thing/down/message/p/d", bytes});
        ASSERT_TRUE(step());
        const char *outcome = code == 200   ? "accepted"
                                                    : code == 409 ? "busy"
                                                    : code == 422 ? "unsupported"
                                                                                : "rejected";
        const auto expected =
                std::string(R"({"kind":"textStatus","result":")") + outcome + R"("})";
        EXPECT_NE(std::find(outputs.begin(), outputs.end(), expected),
                            outputs.end());
        ASSERT_EQ(host.voice_state(), VoiceState::Established);
    }
}

// 普通文本恰含诊断种类字样仍是原sendText；开关关闭不能吞正文或改变发布次数。
TEST_F(SharedHost, StopDiagnosticMarkerInOrdinaryTextDoesNotInterceptOriginalInput) {
    established();
    acquire();
    ASSERT_TRUE(host.ai_owned());
    const auto before = network.sent.size();
    ASSERT_TRUE(local(R"({"kind":"sendText","text":"humanVoiceStopDiagnostic","audio":false})"));
    ASSERT_GT(network.sent.size(), before);
    EXPECT_EQ(network.sent.back().method, "inputSend");
    EXPECT_NE(network.sent.back().payload.find("humanVoiceStopDiagnostic"), std::string::npos);
}
TEST_F(DiagnosticCaller, StopDiagnosticMarkerInOrdinaryTextDoesNotInterceptOriginalInput) {
    established();
    acquire();
    ASSERT_TRUE(host.ai_owned());
    const auto before = network.sent.size();
    ASSERT_TRUE(local(R"({"kind":"sendText","text":"humanVoiceStopDiagnostic","audio":false})"));
    ASSERT_GT(network.sent.size(), before);
    EXPECT_EQ(network.sent.back().method, "inputSend");
    EXPECT_EQ(host.human_media_failure_diagnostic_line(),
              R"({"kind":"humanMediaFailureDiagnostic","reason":"unknown","stopCause":"unknown"})");
}
TEST_F(HumanReceiver, StopDiagnosticDefaultOffRejectsWithoutSourceReads) {
    prepare();
    const auto reads = source_reads;
    EXPECT_FALSE(local(
        R"({"kind":"humanVoiceStopDiagnostic","sessionId":"target-human","routeGeneration":"2","peerEpoch":"1","known":true,"cause":"peerFailed"})"));
    EXPECT_EQ(source_reads, reads);
    EXPECT_TRUE(host.human_media_failure_diagnostic_line().empty());
    EXPECT_TRUE(step());
}
// 临时普通通话只由native显式装配，不用永久escalate伪装资源可恢复。
class TemporaryCaller : public SharedHost {
    protected:
    TemporaryCaller() : SharedHost(false, false, true) {}
    // human_down只跳过AI专属sessionCreated预检，仍经正式MQTT信封与唯一Router/严格Human解析。
    void human_down(Envelope envelope) {
        std::string bytes;
        ASSERT_EQ(encode_envelope(envelope, bytes), DecodeResult::Ok);
        network.events.push_back(
            {MqttEvent::Kind::Message, 0, 0, "$thing/down/signaling/p/d", bytes});
        ASSERT_TRUE(step());
    }
    // ordinary_candidate使用真正主叫创建、原Peer答案与连通回执；尚不暂停AI或取得Human资源。
    void ordinary_candidate() {
        established();
        acquire();
        ASSERT_TRUE(local(R"({"kind":"start"})"));
        const auto input = network.sent.back();
        ASSERT_EQ(input.method, "audioStart");
        Envelope ack;
        ack.request = false;
        ack.token = input.token;
        ack.method = "requestAck";
        ack.session = "ai";
        ack.code = 200;
        std::unique_ptr<cJSON, decltype(&cJSON_Delete)> original_input(
            cJSON_Parse(input.payload.c_str()), cJSON_Delete);
        const auto* input_id = cJSON_GetObjectItemCaseSensitive(original_input.get(), "inputId");
        ASSERT_TRUE(cJSON_IsString(input_id));
        ack.payload = std::string("{\"requestMethod\":\"audioStart\",\"result\":{\"inputId\":\"") +
                      input_id->valuestring + "\"}}";
        down(ack);
        ASSERT_TRUE(media.capture);
        ASSERT_TRUE(local(
            R"({"kind":"handoffControl","operation":"dial","targetKind":"device","targetId":"p:other"})"));
        Envelope created;
        created.request = false;
        created.token = network.sent.back().token;
        created.method = "sessionCreated";
        created.session = "ordinary-human";
        created.code = 200;
        created.payload =
            R"({"state":"inviting","callGroupId":"group","selected":{"transport":"webrtc"},"tracks":[{"trackId":"voice","kind":"audio","sourceId":"microphone","sourceType":"microphone","direction":"sendrecv","codec":"opus","state":"requested"}]})";
        human_down(created);
        actual();
        ASSERT_TRUE(step());
        std::string candidate_diagnostic; // 仅公开合成夹具的本机命令，用于定位严格创建后的阻断阶段。
        for (const auto& value : outputs)
            candidate_diagnostic += value + "\n";
        ASSERT_TRUE(std::any_of(outputs.begin(), outputs.end(), [](const auto& value) {
            return value.find("humanVoiceStart") != std::string::npos;
        })) << "正式创建应答后尚未开始原候选Peer\n" << candidate_diagnostic;
        ASSERT_TRUE(local(
            R"({"kind":"humanVoiceOffer","sessionId":"ordinary-human","routeGeneration":"2","peerEpoch":"1","sdp":"v=0\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\na=rtpmap:111 opus/48000/2\r\n"})"));
        ASSERT_TRUE(step());
        ASSERT_EQ(network.sent.back().method, "mediaOffer");
        Envelope answer;
        answer.request = false;
        answer.token = network.sent.back().token;
        answer.method = "mediaAnswer";
        answer.session = "ordinary-human";
        answer.code = 200;
        answer.payload =
            R"({"mediaRevision":1,"webrtc":{"iceMode":"complete","sdp":"v=0\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\na=rtpmap:111 opus/48000/2\r\n"}})";
        human_down(answer);
        ASSERT_TRUE(local(
            R"({"kind":"humanVoiceAnswered","sessionId":"ordinary-human","routeGeneration":"2","peerEpoch":"1"})"));
        ASSERT_TRUE(local(
            R"({"kind":"humanVoiceConnected","sessionId":"ordinary-human","routeGeneration":"2","peerEpoch":"1"})"));
        ASSERT_TRUE(step());
        ASSERT_EQ(network.sent.back().method, "mediaState");
        ack.token = network.sent.back().token;
        ack.session = "ordinary-human";
        ack.payload = R"({"requestMethod":"mediaState","result":{}})";
        human_down(ack);
        ASSERT_TRUE(host.ai_owned());
        ASSERT_TRUE(media.capture);
    }
    // original_focus沿原AI/Human独立事件水位与共享资源CAS发正式完整组控制。
    Envelope original_focus(bool ai, std::string operation, unsigned revision, unsigned sequence) {
        Envelope control;
        control.method = "focusControl";
        control.token = (ai ? "ai-" : "human-") + operation;
        control.session = ai ? "ai" : "ordinary-human";
        control.event_sequence = sequence;
        control.payload =
            "{\"operation\":\"" + operation +
            "\",\"expectedRevision\":" + std::to_string(revision) + ",\"focusId\":\"" +
            (ai ? "ai-focus" : "human-focus") +
            "\",\"resources\":[\"microphone\",\"speaker\"],\"priority\":\"" +
            (ai ? "conversation" : "call") + "\",\"resumePolicy\":\"explicit\"" +
            ((operation == "acquire" || operation == "restore") ? ",\"expiresAt\":200000}" : "}");
        return control;
    }
    // focus_receipt只匹配真实已经发布的原完整结果，不以下行事件猜资源所有权。
    const Envelope* focus_receipt(std::string_view token) const {
        const auto found =
            std::find_if(network.sent.rbegin(), network.sent.rend(), [&](const auto& value) {
                return value.method == "focusState" && value.token == token;
            });
        return found == network.sent.rend() ? nullptr : &*found;
    }
    // ordinary_finished只结束本次人际腿，确认正式整组释放后再回报原Peer实际停止。
    void ordinary_finished() {
        ordinary_candidate();
        down(original_focus(true, "suspend", 1, 3));
        actual();
        ASSERT_TRUE(step());
        actual();
        ASSERT_TRUE(step());
        ASSERT_NE(focus_receipt("ai-suspend"), nullptr);
        ASSERT_EQ(focus_receipt("ai-suspend")->code, 200);
        down(original_focus(false, "acquire", 2, 1));
        actual();
        ASSERT_TRUE(step());
        actual();
        ASSERT_TRUE(step());
        ASSERT_NE(focus_receipt("human-acquire"), nullptr);
        ASSERT_EQ(focus_receipt("human-acquire")->code, 200);
        Envelope established;
        established.request = false;
        established.method = "sessionEstablished";
        established.token = "human-established";
        established.session = "ordinary-human";
        established.event_sequence = 2;
        established.payload = R"({"state":"established","mediaRevision":1})";
        human_down(established);
        ASSERT_EQ(host.handoff_state(), AiHandoffState::HumanActive);
        ASSERT_TRUE(local(R"({"kind":"handoffControl","operation":"hangup"})"));
        ASSERT_EQ(network.sent.back().method, "sessionEnd");
        Envelope ended;
        ended.request = false;
        ended.token = "human-ended";
        ended.method = "sessionEnded";
        ended.session = "ordinary-human";
        ended.event_sequence = 3;
        ended.payload = R"({"state":"ended","reason":"hangup"})";
        human_down(ended);
        down(original_focus(false, "release", 3, 4));
        actual();
        ASSERT_TRUE(step());
        actual();
        ASSERT_TRUE(step());
        ASSERT_NE(focus_receipt("human-release"), nullptr);
        ASSERT_EQ(focus_receipt("human-release")->code, 200);
        ASSERT_FALSE(local(R"({"kind":"start"})"));
        ASSERT_TRUE(std::any_of(outputs.begin(), outputs.end(), [](const auto& value) {
            return value.find("humanVoiceStop\"") != std::string::npos;
        }));
        // 原Peer异步停止可跨poll；此窗口既不能恢复AI，也不能误关闭原共享连接。
        ASSERT_TRUE(step());
        ASSERT_FALSE(local(R"({"kind":"start"})"));
        ASSERT_EQ(network.disconnects, 0U);
        ASSERT_TRUE(local(
            R"({"kind":"humanVoiceStopped","sessionId":"ordinary-human","routeGeneration":"2","peerEpoch":"1"})"));
        ASSERT_TRUE(step());
    }
};
TEST_F(SharedHost, OrdinaryDialRemainsDisabledWithoutNativeTemporaryPolicy) {
    established();
    acquire();
    const auto before = network.sent.size();
    EXPECT_FALSE(local(
        R"({"kind":"handoffControl","operation":"dial","targetKind":"device","targetId":"p:other"})"));
    EXPECT_EQ(network.sent.size(), before);
    EXPECT_TRUE(host.ai_owned());
}
// 原平台先发布Established，再下发后继焦点；不能改写旧帧序号或要求服务器重投。
// TimedSharedHost
// 只向真实Host注入原停播代次冻结计时，不创建网络或用网页时钟补首音。
class TimedSharedHost : public SharedHost {
    protected:
    TimedSharedHost(bool anchored = false)
        : SharedHost(
              false, false, false, false, false,
              [](std::uint64_t epoch) -> std::optional<AudioTiming> {
                  if (epoch != 7)
                      return {};
                  AudioTiming timing;
                  timing.first_network_ms = 1100;
                  timing.first_playable_ms = 1120;
                  return timing;
              },
              [anchored]() -> std::optional<std::uint64_t> {
                  return anchored ? std::optional<std::uint64_t>{1000} : std::nullopt;
              }) {}
    // content经正式Message编码及Runtime解析，不直接调用宿主内部文本队列。
    void content(std::string method, std::string payload, std::uint64_t sequence) {
        Envelope event;
        event.request = false;
        event.token = "content-test";
        event.session = "ai";
        event.method = std::move(method);
        event.payload = std::move(payload);
        event.message_sequence = sequence;
        std::string bytes;
        ASSERT_EQ(encode_envelope(event, bytes), DecodeResult::Ok);
        network.events.push_back(
            {MqttEvent::Kind::Message, 0, 0, "$thing/down/message/p/d", bytes});
        ASSERT_TRUE(step());
    }
};
// 原共享入口只投影媒体计数，导致已冻结的网络／解码时刻丢失；缺输入锚仍须明确null。
TEST_F(TimedSharedHost, FrozenReplyTimingPrecedesPlaybackStopWithoutInventedInputAnchor) {
    ASSERT_NO_FATAL_FAILURE(boot());
    now = 1200; // 停止时晚于原冻结网络／解码事实，不用Host本次时刻代替它们。
    ASSERT_TRUE(pcm.begin_playback("measured-reply", 7));
    ASSERT_TRUE(step());
    pcm.end_playback(7);
    ASSERT_TRUE(step());
    const auto timing = std::find_if(outputs.begin(), outputs.end(), [](const auto &body) {
        return body.find(R"("kind":"replyTiming")") != std::string::npos;
    });
    ASSERT_NE(timing, outputs.end());
    EXPECT_EQ(
        *timing,
        R"({"kind":"replyTiming","responseId":"measured-reply","lastVoiceSentMs":null,"firstTextMs":null,"firstNetworkMs":1100,"firstPlayableMs":1120})");
    const auto stopped = std::find_if(timing, outputs.end(), [](const auto &body) {
        return body.find(R"("kind":"playbackStop")") != std::string::npos;
    });
    EXPECT_NE(stopped, outputs.end());
}

// AnchoredTimedSharedHost 只增加原UDP发送锚点，默认无文本事件仍明确null。
class AnchoredTimedSharedHost : public TimedSharedHost {
    protected:
    AnchoredTimedSharedHost() : TimedSharedHost(true) {}
};
TEST_F(AnchoredTimedSharedHost, FrozenStopIncludesActualInputAnchorWithoutInventedText) {
    ASSERT_NO_FATAL_FAILURE(boot());
    now = 1200;
    ASSERT_TRUE(pcm.begin_playback("measured-reply", 7));
    ASSERT_TRUE(step());
    pcm.end_playback(7);
    ASSERT_TRUE(step());
    EXPECT_NE(
        std::find(
            outputs.begin(), outputs.end(),
            R"({"kind":"replyTiming","responseId":"measured-reply","lastVoiceSentMs":1000,"firstTextMs":null,"firstNetworkMs":1100,"firstPlayableMs":1120})"),
        outputs.end());
}

TEST_F(AnchoredTimedSharedHost, UnknownStoppedEpochCannotReusePreviousFrozenTiming) {
    ASSERT_NO_FATAL_FAILURE(boot());
    ASSERT_TRUE(pcm.begin_playback("other-reply", 8));
    ASSERT_TRUE(step());
    pcm.end_playback(8);
    ASSERT_TRUE(step());
    EXPECT_FALSE(std::any_of(outputs.begin(), outputs.end(), [](const auto &body) {
        return body.find(R"("kind":"replyTiming")") != std::string::npos;
    }));
}

// 交错回复只允许保守缺失，不能把已切走的旧回复晚到文本重写成首文本。
TEST_F(AnchoredTimedSharedHost, InterleavedTextDoesNotRetimestampOldReply) {
    ASSERT_NO_FATAL_FAILURE(established());
    ASSERT_NO_FATAL_FAILURE(acquire());
    now = 1000;
    content("respCreated", R"({"respId":"measured-reply","inputId":"input-a"})", 1);
    content("respTextDelta", R"({"respId":"measured-reply","contentIndex":0,"delta":"a"})", 2);
    content("respCreated", R"({"respId":"other-reply","inputId":"input-b"})", 3);
    content("respTextDelta", R"({"respId":"other-reply","contentIndex":0,"delta":"b"})", 4);
    content("respTextDelta", R"({"respId":"measured-reply","contentIndex":0,"delta":"late"})", 5);
    now = 1200;
    ASSERT_TRUE(pcm.begin_playback("measured-reply", 7));
    ASSERT_TRUE(step());
    pcm.end_playback(7);
    ASSERT_TRUE(step());
    EXPECT_NE(
        std::find(
            outputs.begin(), outputs.end(),
            R"({"kind":"replyTiming","responseId":"measured-reply","lastVoiceSentMs":1000,"firstTextMs":null,"firstNetworkMs":1100,"firstPlayableMs":1120})"),
        outputs.end());
}

TEST_F(AnchoredTimedSharedHost, RepeatedTextForOriginalCreatedReplyKeepsFirstClock) {
    ASSERT_NO_FATAL_FAILURE(established());
    ASSERT_NO_FATAL_FAILURE(acquire());
    now = 1000;
    content("respCreated", R"({"respId":"measured-reply","inputId":"input-a"})", 1);
    content("respTextDelta", R"({"respId":"measured-reply","contentIndex":0,"delta":"a"})", 2);
    content("respTextDelta", R"({"respId":"measured-reply","contentIndex":0,"delta":"b"})", 3);
    now = 1200;
    ASSERT_TRUE(pcm.begin_playback("measured-reply", 7));
    ASSERT_TRUE(step());
    pcm.end_playback(7);
    ASSERT_TRUE(step());
    EXPECT_NE(
        std::find(
            outputs.begin(), outputs.end(),
            R"({"kind":"replyTiming","responseId":"measured-reply","lastVoiceSentMs":1000,"firstTextMs":1002,"firstNetworkMs":1100,"firstPlayableMs":1120})"),
        outputs.end());
}

TEST_F(TemporaryCaller, EarlyEstablishedBeforeFocusKeepsAiAndActivatesAfterActualAcquire) {
    ASSERT_NO_FATAL_FAILURE(ordinary_candidate());
    Envelope established;
    established.request = false;
    established.method = "sessionEstablished";
    established.token = "human-established";
    established.session = "ordinary-human";
    established.event_sequence = 1;
    established.payload = R"({"state":"established","mediaRevision":1})";
    human_down(established);
    ASSERT_TRUE(host.ai_owned());
    ASSERT_TRUE(media.capture);
    EXPECT_NE(host.handoff_state(), AiHandoffState::HumanActive);
    down(original_focus(true, "suspend", 1, 3));
    actual();
    ASSERT_TRUE(step());
    actual();
    ASSERT_TRUE(step());
    down(original_focus(false, "acquire", 2, 2));
    actual();
    ASSERT_TRUE(step());
    actual();
    ASSERT_TRUE(step());
    ASSERT_NE(focus_receipt("human-acquire"), nullptr);
    EXPECT_EQ(focus_receipt("human-acquire")->code, 200);
    EXPECT_EQ(host.handoff_state(), AiHandoffState::HumanActive);
    EXPECT_FALSE(media.capture);
    EXPECT_TRUE(local(R"({"kind":"handoffControl","operation":"hangup"})"));
}
TEST_F(TemporaryCaller, DialKeepsOriginalAIUntilActualPeerAndPlatformSuspend) {
    established();
    acquire();
    const auto starts = media.starts;
    ASSERT_TRUE(local(
        R"({"kind":"handoffControl","operation":"dial","targetKind":"device","targetId":"p:other"})"));
    ASSERT_FALSE(network.sent.empty());
    EXPECT_EQ(network.sent.back().method, "sessionCreate");
    EXPECT_NE(network.sent.back().payload.find("humanCall"), std::string::npos);
    EXPECT_TRUE(host.ai_owned());
    EXPECT_TRUE(media.configured);
    EXPECT_TRUE(media.connected);
    EXPECT_EQ(media.starts, starts);
    EXPECT_EQ(network.connects, 1U);
    EXPECT_EQ(network.disconnects, 0U);
}
// 创建明确拒绝只退役未绑定候选，保持原AI实际输入和所有权；原关联终态不能阻塞新拨号。
TEST_F(TemporaryCaller, CreationRejectionKeepsAiAndAllowsFreshDial) {
    established();
    acquire();
    ASSERT_TRUE(local(R"({"kind":"start"})"));
    const auto input = network.sent.back();
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> params(cJSON_Parse(input.payload.c_str()), cJSON_Delete);
    const auto* input_id = cJSON_GetObjectItemCaseSensitive(params.get(), "inputId");
    ASSERT_TRUE(cJSON_IsString(input_id));
    Envelope started;
    started.request = false;
    started.method = "requestAck";
    started.token = input.token;
    started.session = "ai";
    started.code = 200;
    started.payload = std::string("{\"requestMethod\":\"audioStart\",\"result\":{\"inputId\":\"") + input_id->valuestring + "\"}}";
    down(started);
    ASSERT_TRUE(media.capture);
    const auto starts = media.starts;
    for (const int code : {201, 403, 500}) {
        ASSERT_TRUE(local(R"({"kind":"handoffControl","operation":"dial","targetKind":"device","targetId":"p:other"})"));
        const auto original = network.sent.back();
        ASSERT_EQ(original.method, "sessionCreate");
        Envelope rejected;
        rejected.request = false;
        rejected.method = "sessionCreated";
        rejected.token = original.token;
        rejected.code = code;
        rejected.payload = R"({"reason":"denied"})";
        auto unrelated = rejected;
        unrelated.token = "unrelated-create";
        human_down(unrelated);
        EXPECT_EQ(host.handoff_state(), AiHandoffState::Candidate);
        human_down(rejected);
        EXPECT_EQ(host.handoff_state(), AiHandoffState::AiActive);
        const auto projection = std::find_if(outputs.rbegin(), outputs.rend(), [](const auto& value) { return value.find("\"kind\":\"handoffState\"") != std::string::npos; });
        ASSERT_NE(projection, outputs.rend());
        EXPECT_NE(projection->find("\"canEscalate\":true"), std::string::npos);
        EXPECT_TRUE(host.ai_owned());
        EXPECT_TRUE(media.capture);
        EXPECT_TRUE(media.connected);
        EXPECT_EQ(media.starts, starts);
        ASSERT_TRUE(local(R"({"kind":"handoffControl","operation":"dial","targetKind":"device","targetId":"p:again"})"));
        const auto next = network.sent.back();
        EXPECT_NE(next.token, original.token);
        auto changed = rejected;
        changed.payload = R"({"reason":"changed"})";
        human_down(changed); // 同旧token但非原精确字节由Router拒绝，不借墓碑放行新内容。
        EXPECT_EQ(host.handoff_state(), AiHandoffState::Candidate);
        human_down(rejected); // 原拒绝重放不能撤销后继新请求或开放第二次并发拨号。
        EXPECT_EQ(host.handoff_state(), AiHandoffState::Candidate);
        EXPECT_FALSE(local(R"({"kind":"handoffControl","operation":"dial","targetKind":"device","targetId":"p:third"})"));
        rejected.token = next.token;
        human_down(rejected);
        EXPECT_EQ(host.handoff_state(), AiHandoffState::AiActive);
    }
    EXPECT_EQ(network.connects, 1U);
    EXPECT_EQ(network.disconnects, 0U);
}
// 发布背压仍遵循原请求账本等待及同token重传，不能冒充已收到创建非200。
TEST_F(TemporaryCaller, PublishBackpressureDoesNotTurnDialIntoRejection) {
    established();
    acquire();
    network.reject_human_create_publish = true;
    ASSERT_TRUE(local(R"({"kind":"handoffControl","operation":"dial","targetKind":"device","targetId":"p:other"})"));
    ASSERT_EQ(network.refused_human_creates.size(), 1U);
    const auto request = network.refused_human_creates.back();
    EXPECT_EQ(host.handoff_state(), AiHandoffState::Candidate);
    ASSERT_TRUE(step());
    EXPECT_EQ(host.handoff_state(), AiHandoffState::Candidate);
    EXPECT_TRUE(host.ai_owned());
    EXPECT_FALSE(local(R"({"kind":"handoffControl","operation":"dial","targetKind":"device","targetId":"p:again"})"));
    network.reject_human_create_publish = false;
    now += 10000;
    ASSERT_TRUE(step());
    const auto found = std::find_if(network.sent.begin(), network.sent.end(), [&request](const auto& value) { return value.method == "sessionCreate" && value.token == request.token; });
    ASSERT_NE(found, network.sent.end());
    EXPECT_EQ(found->payload, request.payload);
    EXPECT_EQ(host.handoff_state(), AiHandoffState::Candidate);
    EXPECT_TRUE(host.ai_owned());
}
TEST_F(TemporaryCaller, OrdinaryDialCannotBypassOriginalAIAllocationOrChooseScope) {
    established();
    const auto before = network.sent.size();
    EXPECT_FALSE(local(
        R"({"kind":"handoffControl","operation":"dial","targetKind":"device","targetId":"p:other"})"));
    EXPECT_EQ(network.sent.size(), before);
    acquire();
    EXPECT_FALSE(local(
        R"({"kind":"handoffControl","operation":"dial","targetKind":"device","targetId":"p:other","sessionId":"borrowed"})"));
    EXPECT_FALSE(local(
        R"({"kind":"handoffControl","operation":"dial","targetKind":"service","targetId":"p:other"})"));
    EXPECT_TRUE(host.ai_owned());
}
TEST_F(TemporaryCaller, ActualSuspendRetiresOriginalInputWithoutSecondAudioStopOrClosingAI) {
    ASSERT_NO_FATAL_FAILURE(ordinary_candidate());
    const auto stops = std::count_if(network.sent.begin(), network.sent.end(),
                                     [](const auto& value) { return value.method == "audioStop"; });
    down(original_focus(true, "suspend", 1, 3));
    EXPECT_FALSE(media.capture);
    EXPECT_EQ(focus_receipt("ai-suspend"), nullptr);
    actual();
    ASSERT_TRUE(step());
    actual();
    ASSERT_TRUE(step());
    const auto* receipt = focus_receipt("ai-suspend");
    ASSERT_NE(receipt, nullptr);
    EXPECT_EQ(receipt->code, 200);
    EXPECT_FALSE(host.ai_owned());
    EXPECT_TRUE(media.configured);
    EXPECT_TRUE(media.connected);
    EXPECT_EQ(network.disconnects, 0U);
    EXPECT_EQ(std::count_if(network.sent.begin(), network.sent.end(),
                            [](const auto& value) { return value.method == "audioStop"; }),
              stops);
    EXPECT_FALSE(local(R"({"kind":"start"})"));
}
TEST_F(TemporaryCaller, ActualHumanReleaseAndPeerStopRequireExplicitOriginalAIStartAndRestore) {
    ASSERT_NO_FATAL_FAILURE(ordinary_finished());
    const auto starts = media.starts;
    EXPECT_FALSE(media.capture);
    EXPECT_FALSE(host.ai_owned());
    ASSERT_TRUE(local(R"({"kind":"start"})"));
    ASSERT_EQ(network.sent.back().method, "audioStart");
    const auto requested = network.sent.back();
    EXPECT_FALSE(media.capture);
    down(original_focus(true, "restore", 4, 4));
    EXPECT_EQ(focus_receipt("ai-restore"), nullptr);
    actual();
    ASSERT_TRUE(step());
    actual();
    ASSERT_TRUE(step());
    ASSERT_NE(focus_receipt("ai-restore"), nullptr);
    ASSERT_EQ(focus_receipt("ai-restore")->code, 200) << focus_receipt("ai-restore")->payload;
    EXPECT_TRUE(host.ai_owned());
    EXPECT_FALSE(media.capture);
    Envelope ack;
    ack.request = false;
    ack.token = requested.token;
    ack.method = "requestAck";
    ack.session = "ai";
    ack.code = 200;
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> original_input(
        cJSON_Parse(requested.payload.c_str()), cJSON_Delete);
    const auto* input_id = cJSON_GetObjectItemCaseSensitive(original_input.get(), "inputId");
    ASSERT_TRUE(cJSON_IsString(input_id));
    ack.payload = std::string("{\"requestMethod\":\"audioStart\",\"result\":{\"inputId\":\"") +
                  input_id->valuestring + "\"}}";
    down(ack);
    EXPECT_TRUE(media.capture);
    EXPECT_EQ(media.starts, starts + 1);
    EXPECT_EQ(network.connects, 1U);
    EXPECT_EQ(network.disconnects, 0U);
    EXPECT_TRUE(media.configured);
    EXPECT_FALSE(local(
        R"({"kind":"handoffControl","operation":"dial","targetKind":"device","targetId":"p:second"})"));
}
TEST_F(TemporaryCaller, RealUserStopNeverWashesFrozenResumeIntent) {
    ASSERT_NO_FATAL_FAILURE(ordinary_finished());
    ASSERT_TRUE(local(R"({"kind":"stop"})"));
    const auto before = network.sent.size();
    EXPECT_FALSE(local(R"({"kind":"start"})"));
    EXPECT_EQ(network.sent.size(), before);
    down(original_focus(true, "restore", 4, 4));
    actual();
    ASSERT_TRUE(step());
    EXPECT_FALSE(host.ai_owned());
    EXPECT_FALSE(media.capture);
    EXPECT_EQ(network.disconnects, 0U);
}
// 恢复完成后的普通逐轮停止只停止输入，不能把已经实际取得的原完整组租约误撤掉。
TEST_F(TemporaryCaller, UserStopAfterCompletedRestorePreservesLeaseForNextOriginalTurn) {
    ASSERT_NO_FATAL_FAILURE(ordinary_finished());
    ASSERT_TRUE(local(R"({"kind":"start"})"));
    const auto requested = network.sent.back();
    ASSERT_EQ(requested.method, "audioStart");
    down(original_focus(true, "restore", 4, 4));
    actual();
    ASSERT_TRUE(step());
    actual();
    ASSERT_TRUE(step());
    ASSERT_NE(focus_receipt("ai-restore"), nullptr);
    ASSERT_EQ(focus_receipt("ai-restore")->code, 200);
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> input(cJSON_Parse(requested.payload.c_str()),
                                                       cJSON_Delete);
    const auto* id = cJSON_GetObjectItemCaseSensitive(input.get(), "inputId");
    ASSERT_TRUE(cJSON_IsString(id));
    Envelope ack; // 仍沿本次原inputId与token，不以恢复事件代替模型输入ACK。
    ack.request = false;
    ack.token = requested.token;
    ack.method = "requestAck";
    ack.session = "ai";
    ack.code = 200;
    ack.payload = std::string("{\"requestMethod\":\"audioStart\",\"result\":{\"inputId\":\"") +
                  id->valuestring + "\"}}";
    down(ack);
    ASSERT_TRUE(host.ai_owned());
    ASSERT_TRUE(media.capture);
    ASSERT_TRUE(local(R"({"kind":"stop"})"));
    ASSERT_TRUE(step());
    EXPECT_FALSE(media.capture);
    EXPECT_TRUE(host.ai_owned());
    EXPECT_EQ(network.disconnects, 0U);
    ASSERT_TRUE(local(R"({"kind":"start"})"));
    EXPECT_EQ(network.sent.back().method, "audioStart");
    EXPECT_NE(network.sent.back().token, requested.token);
    EXPECT_FALSE(media.capture);
}
TEST_F(TemporaryCaller, UserStopAfterNewStartRejectsPendingOldRestoreAndCannotReopenCapture) {
    ASSERT_NO_FATAL_FAILURE(ordinary_finished());
    ASSERT_TRUE(local(R"({"kind":"start"})"));
    ASSERT_TRUE(local(R"({"kind":"stop"})"));
    down(original_focus(true, "restore", 4, 4));
    actual();
    ASSERT_TRUE(step());
    const auto* denied = focus_receipt("ai-restore");
    ASSERT_NE(denied, nullptr);
    EXPECT_NE(denied->code, 200);
    EXPECT_NE(denied->payload.find("userIntentChanged"), std::string::npos);
    EXPECT_FALSE(host.ai_owned());
    EXPECT_FALSE(media.capture);
}
TEST_F(TemporaryCaller, OriginalLeaseExpiryRefusesNewResumeRequestWithoutExtendingHostLifetime) {
    ASSERT_NO_FATAL_FAILURE(ordinary_finished());
    wall = 200000;
    const auto before = network.sent.size();
    EXPECT_FALSE(local(R"({"kind":"start"})"));
    EXPECT_EQ(network.sent.size(), before);
    EXPECT_FALSE(media.capture);
}
// HalfOnlyCaller 仅限制native发布的模式子集；仍使用真实ready中的设备AEC事实。
class HalfOnlyCaller : public SharedHost {
public:
  HalfOnlyCaller() : SharedHost(false, false, false, true) {}
};
TEST_F(HalfOnlyCaller, AdvertisesHalfOnlyWithoutFalsifyingAec) {
  connected();
  const auto report = std::find_if(
      network.sent.begin(), network.sent.end(),
      [](const auto &value) { return value.method == "capabilityReport"; });
  ASSERT_NE(report, network.sent.end());
  EXPECT_NE(report->payload.find("halfDuplex"), std::string::npos);
  EXPECT_EQ(report->payload.find("fullDuplex"), std::string::npos);
  EXPECT_NE(report->payload.find("\"aecMode\":\"device\""), std::string::npos);
  ASSERT_EQ(network.sent.back().method, "sessionCreate");
  EXPECT_NE(network.sent.back().payload.find("halfDuplex"), std::string::npos);
  EXPECT_EQ(network.sent.back().payload.find("fullDuplex"), std::string::npos);
}
TEST_F(SharedHost, DefaultStillAdvertisesActualAecFullDuplex) {
  connected();
  const auto report = std::find_if(
      network.sent.begin(), network.sent.end(),
      [](const auto &value) { return value.method == "capabilityReport"; });
  ASSERT_NE(report, network.sent.end());
  EXPECT_NE(report->payload.find("halfDuplex"), std::string::npos);
  EXPECT_NE(report->payload.find("fullDuplex"), std::string::npos);
  EXPECT_NE(report->payload.find("\"aecMode\":\"device\""), std::string::npos);
}
// RejectedHumanOfferReceiver 只开启原诊断，拒绝报文为公开合成；原scope实际false仍须由HAL确认。
class RejectedHumanOfferReceiver : public HumanReceiver {
protected:
    RejectedHumanOfferReceiver() : HumanReceiver(true) {}
};
// 平台拒绝可使已确认输出随后失效；outputUnconfirmed不能独自证明此前Worklet未确认。
TEST_F(RejectedHumanOfferReceiver, RejectedAnswerRetiresConfirmedOutputAndPreservesSignalFailure) {
    invite();
    ASSERT_TRUE(local(R"({"kind":"handoffControl","operation":"accept"})"));
    Envelope ack;
    ack.request = false;
    ack.method = "requestAck";
    ack.session = "target-human";
    ack.token = network.sent.back().token;
    ack.code = 200;
    ack.payload = R"({"requestMethod":"sessionAccept","result":{}})";
    down(ack);
    actual();
    ASSERT_TRUE(step());
    ASSERT_TRUE(local(R"({"kind":"humanVoiceOffer","sessionId":"target-human","routeGeneration":"2","peerEpoch":"1","sdp":"v=0\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\na=rtpmap:111 opus/48000/2\r\n"})"));
    ASSERT_TRUE(step());
    ASSERT_EQ(network.sent.back().method, "mediaOffer");
    EXPECT_EQ(host.duplex_driver_failure_diagnostic_line(), R"({"kind":"duplexDriverFailureDiagnostic","reason":"unknown","path":"unknown"})");
    ack.method = "mediaAnswer";
    ack.token = network.sent.back().token;
    ack.code = 409;
    ack.payload = R"({"reason":"temporaryCallResourcesUnavailable"})";
    down(ack);
    EXPECT_EQ(host.human_signal_diagnostic_line(), R"({"kind":"humanSignalDiagnostic","state":"failed","failure":"receiveRejected"})");
    EXPECT_EQ(
        host.human_rejection_diagnostic_line(),
        R"({"kind":"humanSignalRejectionDiagnostic","method":"mediaAnswer","requestMethod":"unknown","code":409,"state":"negotiating"})");
    EXPECT_FALSE(step());
    EXPECT_EQ(host.duplex_driver_failure_diagnostic_line(), R"({"kind":"duplexDriverFailureDiagnostic","reason":"outputUnconfirmed","path":"1"})");
    EXPECT_FALSE(std::any_of(network.sent.begin(), network.sent.end(), [](const auto& value) {
        return value.method == "focusState" && value.code == 200;
    }));
}
// 原媒体状态ACK拒绝先于输出失确认；诊断不反射平台reason或原token。
TEST_F(RejectedHumanOfferReceiver, RejectedMediaStateAckKeepsOriginalWaitingState) {
    prepare(true, false);
    EXPECT_TRUE(host.human_rejection_diagnostic_line().empty());
    Envelope ack;
    ack.request = false;
    ack.method = "requestAck";
    ack.session = "target-human";
    ack.token = network.sent.back().token;
    ack.code = 409;
    ack.payload = R"({"requestMethod":"mediaState","reason":"private-body","result":{}})";
    down(ack);
    EXPECT_EQ(
        host.human_rejection_diagnostic_line(),
        R"({"kind":"humanSignalRejectionDiagnostic","method":"requestAck","requestMethod":"mediaState","code":409,"state":"waitingEstablished"})");
    const auto original = host.human_rejection_diagnostic_line();
    EXPECT_FALSE(step());
    host.close(++now);
    EXPECT_EQ(host.human_rejection_diagnostic_line(), original);
}
// 正常真实应答不产生拒帧事实，默认不开启的宿主亦无新输出。
TEST_F(RejectedHumanOfferReceiver, SuccessfulAnswersDoNotCreateRejection) {
    prepare();
    EXPECT_TRUE(host.human_rejection_diagnostic_line().empty());
}
TEST_F(HumanReceiver, DefaultDiagnosticHasNoRejectionOutput) {
    prepare();
    EXPECT_TRUE(host.human_rejection_diagnostic_line().empty());
}
} // namespace
} // namespace ur::device
