// 文件说明：IPC软件设备使用真实MQTT和独立WebRTC发布Peer；浏览器不持有设备认证信息。
#include <openssl/rand.h>
#include <array>
#include <charconv>
#include <chrono>
#include <iostream>
#include <map>
#include <thread>
#include "browser_bridge.hpp"
#include "browser_call_media.hpp"
#include "browser_endpoint_wire.hpp"
#include "browser_ipc_media.hpp"
#include "device_credentials.hpp"
#include "environment.hpp"
#include "ipc_assets.hpp"
#include "mosquitto_port.hpp"
#include "ur/device/doorbell_event.hpp"
#include "ur/device/endpoint_handoff_client.hpp"
#include "ur/device/privacy_property.hpp"

namespace ur::device {
namespace {
// clock_ms 返回单调或墙钟毫秒，邀请有效期和请求预算不可混用。
std::uint64_t clock_ms(bool wall) {
    return wall ? std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count()
                : std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count();
}
// random_token 每次进程产生独立桥配对和消息前缀，不输出认证正文。
std::string random_token() {
    std::array<unsigned char, 32> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
        return {};
    const char* alphabet = "0123456789abcdef";
    std::string result;
    for (auto value : bytes) {
        result += alphabet[value >> 4];
        result += alphabet[value & 15];
    }
    return result;
}
// number 只解析明确范围内的整数，不接受符号、尾缀或隐式缺省。
unsigned number(std::string_view text, unsigned minimum, unsigned maximum) {
    unsigned value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size() &&
                   value >= minimum && value <= maximum
               ? value
               : 0;
}
// env 读取宿主注入值，设备密钥不进入浏览器、参数列表或日志。
std::string env(const char* key) { return environment_value(key).value_or(""); }
// run 单事件线程串行推进能力、独立观看会话与真实媒体事实；失联先停全部媒体。
int run(unsigned port) {
    const auto token = random_token(), instance = random_token().substr(0, 32);
    const auto product = env("UR_DEVICE_PRODUCT"), device = env("UR_DEVICE_NAME");
    const auto credentials = device_credentials(product, device, env("UR_DEVICE_SECRET"), instance,
                                                clock_ms(true) / 1000 + 3600);
    const auto mqtt_port = number(env("UR_MQTT_PORT"), 1024, 65535);
    const auto microphone =
        environment_flag("UR_IPC_MICROPHONE");  // 宿主明确配置可用采集驱动，默认不声明音频。
    const auto conference_talk =
        environment_flag("UR_IPC_CONFERENCE_TALK");  // 双向驱动独立开关，不因监听自动获得对讲。
    const auto human_enabled = environment_flag("UR_IPC_HUMAN_CALL");  // 新驱动默认关闭。
    const auto receive_video =
        environment_flag("UR_IPC_HUMAN_RECEIVE_VIDEO");  // 独立接收驱动默认关闭。
    const auto privacy_enabled =
        environment_flag("UR_IPC_PRIVACY_REPORT");  // 仅已注册物模型宿主显式开启。
    const auto roster_enabled =
        environment_flag("UR_IPC_PARTICIPANT_ROSTER");  // 只有已装配消费与界面的宿主显式开启。
    const auto roster_sync =
        environment_flag("UR_IPC_PARTICIPANT_ROSTER_SYNC");  // 默认关闭，仅原连接全量查询。
    const auto camera = env("UR_IPC_CAMERA_ID");             // 新模式显式绑定真实声明的本地画面源。
    if (token.empty() || instance.empty() || !credentials || !mqtt_port ||
        env("UR_MQTT_HOST").empty() || !microphone || !conference_talk || !human_enabled ||
        !roster_enabled || !roster_sync || !privacy_enabled || !receive_video ||
        (*receive_video && !*human_enabled) ||
        (*roster_sync &&
         !ipc_browser_sync_configuration(*roster_sync, *roster_enabled, *conference_talk)) ||
        (*roster_enabled && !*conference_talk && !*human_enabled) ||
        (*conference_talk && !*microphone) ||
        (*human_enabled && (!*microphone || !*conference_talk || camera.empty()))) {
        std::cerr << "invalid_device_environment\n";
        return 2;
    }
    MqttConfig config;
    config.host = env("UR_MQTT_HOST");
    config.port = static_cast<std::uint16_t>(mqtt_port);
    config.ca_file = env("UR_MQTT_CA");
    config.client_id = credentials->client_id;
    config.username = credentials->username;
    config.password = credentials->password;
    const auto up = "$thing/up/signaling/" + product + "/" + device;
    const auto down = "$thing/down/signaling/" + product + "/" + device;
    BrowserBridge bridge(port, token, ipc_assets());
    if (!bridge.start()) {
        std::cerr << "browser_bind_failed\n";
        return 2;
    }
    std::cout << "http://127.0.0.1:" << port << "/\n" << std::flush;
    MosquittoPort mqtt;
    PrivacyProperty privacy(mqtt, {*privacy_enabled, product, device});
    if (*privacy_enabled && !privacy.enabled()) {
        bridge.stop();
        std::cerr << "invalid_privacy_property_configuration\n";
        return 2;
    }
    // 门铃事件只由宿主物模型配置开启，缺省不订阅事件Topic，也不改变媒体能力。
    const auto doorbell_event_id = env("UR_IPC_DOORBELL_EVENT");
    DoorbellEvent doorbell(mqtt, {!doorbell_event_id.empty(), product, device, doorbell_event_id});
    if (!doorbell_event_id.empty() && !doorbell.enabled()) {
        bridge.stop();
        std::cerr << "invalid_doorbell_event_configuration\n";
        return 2;
    }
    CaptureGate capture_gate(3);  // 同一物理采集组：新模式不能靠独立驱动绕过忙线。
    BrowserIPCMediaFactory factory(*conference_talk, *human_enabled ? &capture_gate : nullptr);
    BrowserCallMedia call_media(*human_enabled ? &capture_gate : nullptr,
                                camera.empty() ? "front" : camera);
    HumanCallClient human(mqtt, call_media, product + "/" + device, instance + "-human",
                          {camera.empty() ? "front" : camera, true, *roster_enabled,
                           HumanCallMode::Video, *receive_video});
    EndpointHandoffClient endpoint(mqtt, up, down, instance + "-endpoint");
    std::string endpoint_projection;  // 仅固定安全状态投影，不保存或转发平台自由正文。
    IPCCapability capability;
    capability.max_publishers = 3;
    capability.local_accept = true;
    capability.microphone = *microphone;
    capability.conference_talk = *conference_talk;
    capability.human_call = *human_enabled;
    capability.participant_roster = *roster_enabled;
    capability.participant_roster_sync = *roster_sync;
    capability.receive_video = *receive_video;
    if (!camera.empty())
        capability.camera_id = camera;
    IPCClient client(mqtt, factory, up, down, instance, capability);
    Requests requests(mqtt);
    std::map<std::string, IPCPhase> phases;  // 控件终态也按会话跟踪，邀请尚未创建媒体时仍须关闭。
    std::map<std::string, std::uint64_t>
        projected_rosters;  // 仅记录实际已转交界面的原名册水位，不借用全局会话。
    std::map<std::string, std::string>
        projected_syncs;  // 有界原腿查询投影；不保存平台正文或token。
    const auto cap_token = instance + "-capability";
    bool started = false, available = false, done = false, failed = false;
    int subscription = 0;
    // 原连接的独立事件SUBACK，不能借信令订阅成功准入。
    int doorbell_subscription = 0;
    bool doorbell_ready = false;        // 仅物模型事件出口就绪，不表示呼叫或媒体建立。
    bool doorbell_unavailable = false;  // 订阅失败/超时后不借迟到SUBACK恢复出口。
    // 当前按钮的原token，Pending重复点击不生成新意图。
    std::string doorbell_token;
    // 页面只展示原事件本地事实。
    RequestState doorbell_state = RequestState::Missing;
    int privacy_subscription = 0;  // 属性下行有独立SUBACK，不借信令或事件订阅准入。
    bool privacy_ready = false,
         privacy_unavailable = false;                    // 失败出口不由迟到SUBACK复活。
    std::string privacy_token;                           // 当前原报告；Pending不产生新的相反意图。
    RequestState privacy_state = RequestState::Missing;  // ACK仅确认上报，不证明采集停止。
    HumanCallState human_previous = HumanCallState::Idle;  // 仅投影共享人际腿，不修改其他观看状态。
    std::string human_previous_session;  // 同轮Ended与新Invite不能因枚举相同而漏新原腿。
    FixedQueue<std::string, 32>
        waiting_human;  // 先到的邀请等待原能力ACK，有界保存原字节不伪造确认。
    // doorbell_notice 只发送固定状态字面量，页面不获得身份、配置事件或原报文。
    const auto doorbell_notice = [&](const char* state) {
        return bridge.send(std::string("{\"kind\":\"doorbellState\",\"state\":\"") + state + "\"}");
    };
    // privacy_notice
    // 固定状态不含原属性正文；媒体停止必须另由正式信令及真实驱动确认。
    const auto privacy_notice = [&](const char* state) {
        return bridge.send(std::string("{\"kind\":\"privacyState\",\"state\":\"") + state + "\"}");
    };
    // receive_human
    // 复用已归属的原人际状态机，错误只使本腿失败，不断开其他IPC观看者。
    const auto receive_human = [&](std::string_view body, std::uint64_t now, std::uint64_t wall) {
        // 接续专属拒绝不能误交人际建连；错token或矛盾事实也不能降级失败原通话。
        if (endpoint_handoff_ingress_ack(body)) {
            endpoint.receive(down, body, now);
            return;
        }
        // 源端结束可能先于commit ACK；接续控制器仍消费原腿终态，不重开媒体。
        endpoint.receive(down, body, now);
        if (!human.receive_shared(down, body, now, wall))
            return;
        if (human.state() == HumanCallState::Ringing &&
            !call_media.bind_session(human.session_id()))
            human.reject(now);
    };
    std::uint64_t startup = 0;
    while (!done) {
        const auto now = clock_ms(false), wall = clock_ms(true);
        for (unsigned count = 0; count < 32; ++count) {
            auto event = bridge.poll();
            if (!event)
                break;
            if (event->kind == BrowserEventKind::Closed ||
                event->kind == BrowserEventKind::Failed) {
                done = true;
                failed = event->kind == BrowserEventKind::Failed;
                break;
            }
            if (event->kind != BrowserEventKind::Message)
                continue;
            EndpointBrowserRequest endpoint_request;
            const auto endpoint_decode = decode_endpoint_browser(event->body, endpoint_request);
            if (endpoint_decode == EndpointBrowserDecode::Invalid) {
                failed = done = true;
                break;
            }
            if (endpoint_decode == EndpointBrowserDecode::Ok) {
                if (!available || !*human_enabled)
                    continue;
                if (endpoint_request.kind == "callDial") {
                    human.dial(endpoint_request.target, now);
                } else if (human.active() && endpoint_request.session == human.session_id()) {
                    if (endpoint.receipt().session != human.session_id() &&
                        !endpoint.bind(human.session_id(), now))
                        continue;
                    if (endpoint_request.kind == "endpointPrepare")
                        endpoint.prepare(endpoint_request.target, now);
                    else if (endpoint_request.kind == "endpointCommit")
                        endpoint.commit(now);
                    else
                        endpoint.abort(now);
                }
                continue;
            }
            IPCBrowserRequest request;
            if (!decode_ipc_browser(event->body, request)) {
                failed = done = true;
                break;
            }
            if (request.kind == "shutdown") {
                done = true;
                break;
            }
            if (request.kind == "ready") {
                if (started || !mqtt.connect(config)) {
                    failed = done = true;
                    break;
                }
                started = true;
                startup = now;
            } else if (request.kind == "doorbellPress") {
                // 原物模型事件不依赖统一会话capability
                // ACK；仅要求原连接事件下行SUBACK。
                if (!doorbell_ready) {
                    if (!bridge.send(R"({"kind":"doorbellUnavailable"})"))
                        failed = done = true;
                } else if (doorbell_token.empty() ||
                           doorbell.state(doorbell_token) != RequestState::Pending) {
                    const auto original_token = random_token();
                    if (original_token.empty()) {
                        failed = done = true;
                        break;
                    }
                    // 墙钟仅在新的独立按压处采样一次；后续重试由核心逐字节保留。
                    const DoorbellPress original{original_token, wall, "{}"};
                    if (doorbell.submit(original, now) == SubmitResult::Accepted) {
                        doorbell_token = original_token;
                        doorbell_state = RequestState::Pending;
                        if (!doorbell_notice("pending"))
                            failed = done = true;
                    } else if (!doorbell_notice("unknown"))
                        failed = done = true;
                }
            } else if (request.kind == "privacyReport") {
                if (!privacy_ready) {
                    if (!bridge.send(R"({"kind":"privacyUnavailable"})"))
                        failed = done = true;
                } else if (privacy_token.empty() ||
                           privacy.state(privacy_token) != RequestState::Pending) {
                    const auto original_token = random_token();
                    if (original_token.empty()) {
                        failed = done = true;
                        break;
                    }
                    // 不在本地按钮处分支停流或复开；保留正式DMS→IM→C++媒体处理链。
                    if (privacy.submit({original_token, wall, request.enabled}, now) ==
                        SubmitResult::Accepted) {
                        privacy_token = original_token;
                        privacy_state = RequestState::Pending;
                        if (!privacy_notice("pending"))
                            failed = done = true;
                    } else if (!privacy_notice("rejected"))
                        failed = done = true;
                }
            } else if (request.kind == "ipcStopped") {
                // 迟到回执只可能释放原租约，不得让新观看者失败或重建任何资源。
                factory.report_stopped(request.session, request.token);
            } else if (request.kind == "callStopped") {
                call_media.report_stopped(request.session, request.token);
            } else if (*human_enabled && request.kind == "callAccept") {
                if (available && request.session == human.session_id() &&
                    human.state() == HumanCallState::Ringing)
                    human.accept(now);
            } else if (*human_enabled && request.kind == "callReject") {
                if (available && request.session == human.session_id())
                    human.reject(now);
            } else if (*human_enabled && request.kind == "callHangup") {
                if (available && request.session == human.session_id())
                    human.end(now);
            } else if (*human_enabled && call_media.matches(request.session, request.token) &&
                       request.kind == "callOffer") {
                if (!human.offer_ready(request.sdp, now))
                    human.end(now);
            } else if (*human_enabled && call_media.matches(request.session, request.token) &&
                       request.kind == "callConnectivity") {
                // 自然恢复沿原已建立腿/租约，不重复初次mediaConnected或恢复已停止的采集。
                if (human.state() != HumanCallState::Connected)
                    continue;
                const auto result = call_media.report_connectivity(
                    request.session, request.token, request.state == "connected", now);
                if (result != Result::Applied && result != Result::Duplicate &&
                    result != Result::Stale)
                    human.end(now);
            } else if (*human_enabled && call_media.matches(request.session, request.token) &&
                       request.kind == "callConnected") {
                if (!call_media.report_connected() || !human.media_connected(now))
                    human.end(now);
            } else if (*human_enabled && call_media.matches(request.session, request.token) &&
                       request.kind == "callFailure") {
                human.end(now);
            } else if (request.kind.rfind("call", 0) == 0) {
                // 已结束腿的迟到驱动回调只能忽略，不影响当前IPC或新的原人际腿。
                continue;
            } else if (request.kind == "ipcSync") {
                // 查询拒绝不关闭主媒体；只读展示原核心状态，不把本地发布受理当已安装快照。
                const auto notice =
                    ipc_browser_sync(client, request, available && *roster_sync, now, wall);
                if (!notice.empty() && bridge.send(notice) && phases.count(request.session))
                    projected_syncs[request.session] = notice;
            } else if (!available) {
                failed = done = true;
                break;
            } else if (request.kind == "ipcOffer") {
                if (client.media_revision(request.session) != request.media_revision)
                    continue;  // 旧驱动事实不终止新代次。
                if (!client.offer_ready(request.session, request.sdp, now, request.media_revision))
                    client.end(request.session, now);
            } else if (request.kind == "ipcConnectivity") {
                const auto result = factory.report_connectivity(request.session, request.token,
                                                                request.state == "connected", now);
                if (result != Result::Applied && result != Result::Duplicate &&
                    result != Result::Stale)
                    client.end(request.session, now);
            } else if (request.kind == "ipcConnected") {
                if (client.media_revision(request.session) != request.media_revision)
                    continue;
                if (!factory.report_connected(request.session, request.media_revision) ||
                    !client.media_connected(request.session, now, request.media_revision))
                    client.end(request.session, now);
            } else if (request.kind == "ipcDisconnected") {
                if (client.media_revision(request.session) != request.media_revision)
                    continue;
                if (!client.media_disconnected(request.session, request.media_revision, now))
                    client.end(request.session, now);
            } else if (request.kind == "ipcTrackApplied") {
                if (!client.track_applied(request.session, request.token, request.state,
                                          request.success, now))
                    client.end(request.session, now);
            } else if (request.kind == "ipcSetQuality") {
                // 页面只提交本地档位意图，身份、token和修订号由共享核心生成并校验。
                std::cout << (client.media_update(request.session, request.track, request.profile,
                                                  now)
                                  ? "ipc_quality_requested\n"
                                  : "ipc_quality_rejected\n")
                          << std::flush;
            } else if (request.kind == "ipcQualityApplied") {
                // 迟到质量回调不能结束主通话；核心只接受原计划的真实驱动事实。
                client.quality_applied(request.session, request.token, request.state,
                                       request.success, now);
            } else if (request.kind == "ipcAccept") {
                // 固定诊断不含会话或认证数据，区分本地点击到达与共享状态机拒绝，便于真实联调定位。
                std::cout << (client.accept(request.session, now, wall)
                                  ? "ipc_local_accept_ok\n"
                                  : "ipc_local_accept_rejected\n")
                          << std::flush;
            } else if (request.kind == "ipcEnd" || request.kind == "ipcFailure")
                client.end(request.session, now);
        }
        if (done)
            break;
        if (started && !mqtt.poll()) {
            failed = true;
            break;
        }
        for (unsigned count = 0; count < 32; ++count) {
            auto event = mqtt.take_event();
            if (!event)
                break;
            if (event->kind == MqttEvent::Kind::Connected) {
                subscription = mqtt.subscribe(down);
                if (!subscription)
                    failed = done = true;
                if (doorbell.enabled()) {
                    doorbell_subscription = mqtt.subscribe(doorbell.reply_topic());
                    if (!doorbell_subscription) {
                        doorbell_unavailable = true;
                        if (!bridge.send(R"({"kind":"doorbellUnavailable"})"))
                            failed = done = true;
                    }
                }
                if (privacy.enabled()) {
                    privacy_subscription = mqtt.subscribe(privacy.reply_topic());
                    if (!privacy_subscription) {
                        privacy_unavailable = true;
                        if (!bridge.send(R"({"kind":"privacyUnavailable"})"))
                            failed = done = true;
                    }
                }
            } else if (event->kind == MqttEvent::Kind::Subscribed && privacy_subscription != 0 &&
                       event->operation == privacy_subscription) {
                if (!privacy_unavailable && !privacy_ready) {
                    privacy_ready = event->code == 0 && privacy.begin_connection(now);
                    privacy_unavailable = !privacy_ready;
                    if (!bridge.send(privacy_ready ? R"({"kind":"privacyAvailable"})"
                                                   : R"({"kind":"privacyUnavailable"})"))
                        failed = done = true;
                }
            } else if (event->kind == MqttEvent::Kind::Subscribed && doorbell_subscription != 0 &&
                       event->operation == doorbell_subscription) {
                if (!doorbell_unavailable && !doorbell_ready) {
                    doorbell_ready = event->code == 0 && doorbell.begin_connection(now);
                    doorbell_unavailable = !doorbell_ready;
                    if (!bridge.send(doorbell_ready ? R"({"kind":"doorbellAvailable"})"
                                                    : R"({"kind":"doorbellUnavailable"})"))
                        failed = done = true;
                }
            } else if (event->kind == MqttEvent::Kind::Subscribed &&
                       event->operation == subscription) {
                Envelope command;
                std::string body;
                if (event->code != 0 ||
                    ipc_capability_command(cap_token, capability, command) != WireResult::Ok ||
                    encode_envelope(command, body) != DecodeResult::Ok ||
                    requests.submit({cap_token, {}, "capabilityReport", up, body}, now) !=
                        SubmitResult::Accepted)
                    failed = done = true;
            } else if (event->kind == MqttEvent::Kind::Message && privacy.enabled() &&
                       event->topic == privacy.reply_topic()) {
                privacy.receive(event->topic, event->body, now);
            } else if (event->kind == MqttEvent::Kind::Message && doorbell.enabled() &&
                       event->topic == doorbell.reply_topic()) {
                // 事件ACK不交给IPCClient，不借原物模型确认创建任何Peer或播放音频。
                doorbell.receive(event->topic, event->body, now);
            } else if (event->kind == MqttEvent::Kind::Message && event->topic == down) {
                VoiceEvent ack;
                if (decode_voice_event(WireRoute::Signaling, event->body, ack) == WireResult::Ok &&
                    ack.envelope.token == cap_token && ack.request_method == "capabilityReport") {
                    // QoS1可能重复投递已确认快照，不能再次消费在途请求或终止已建立观看者。
                    if (available)
                        continue;
                    if (!ack.envelope.code || *ack.envelope.code != 200 ||
                        ack.revision != capability.revision ||
                        !requests.acknowledge({cap_token, {}, "capabilityReport", 200, up}, now)) {
                        failed = done = true;
                        break;
                    }
                    if (!available) {
                        available = true;
                        if (*human_enabled && !human.start_shared(now)) {
                            failed = done = true;
                            break;
                        }
                        if (!bridge.send(R"({"kind":"ipcAvailable"})"))
                            failed = done = true;
                        std::cout << "ipc_available\n" << std::flush;
                    }
                } else if (*human_enabled &&
                           (human_call_invitation(event->body) ||
                            [&] {
                                // 创建成功才首次得到本腿ID，只能按原创建token路由，不能先要求已知session。
                                Envelope message;
                                return decode_envelope(event->body, message) == DecodeResult::Ok &&
                                       human.owns_shared_create_reply(message);
                            }() ||
                            (!endpoint.receipt().session.empty() &&
                             [&] {
                                 Envelope message;
                                 return decode_envelope(event->body, message) == DecodeResult::Ok &&
                                        message.method == "endpointState" &&
                                        message.session == endpoint.receipt().session;
                             }()) ||
                            (!human.session_id().empty() && [&] {
                                Envelope message;
                                return decode_envelope(event->body, message) == DecodeResult::Ok &&
                                       message.session == human.session_id();
                            }()))) {
                    if (available)
                        receive_human(event->body, now, wall);
                    else if (!waiting_human.push_back(std::move(event->body)))
                        failed = done = true;
                } else if (available) {
                    const auto accepted = client.receive(event->topic, event->body, now, wall);
                    Envelope message;
                    if (accepted && decode_envelope(event->body, message) == DecodeResult::Ok) {
                        const auto phase = client.phase(message.session);
                        if (phase && !phases.count(message.session))
                            phases.emplace(message.session, *phase);
                        if (message.method == "sessionInvite") {
                            IPCInvite invite;
                            IPCTalkInvite talk;
                            const bool preview =
                                decode_ipc_invite(WireRoute::Signaling, event->body, capability,
                                                  invite) == WireResult::Ok;
                            const bool conference =
                                !preview &&
                                decode_ipc_talk_invite(WireRoute::Signaling, event->body,
                                                       capability, talk) == WireResult::Ok;
                            if (((preview && invite.needs_accept) ||
                                 (conference && talk.needs_accept)) &&
                                phase && *phase != IPCPhase::Ended) {
                                // 邀请只转安全会话标识，绝不转交原云端正文或设备认证信息。
                                const auto notice = ipc_browser_invite(message.session, conference);
                                if (notice.empty() || !bridge.send(notice))
                                    failed = done = true;
                            }
                        }
                    }
                }
            } else if (event->kind == MqttEvent::Kind::Failed ||
                       event->kind == MqttEvent::Kind::Disconnected) {
                // 原连接失效立即隔离事件，不能在本轮tick再补发。
                doorbell.disconnect(now);
                privacy.disconnect(now);
                privacy_ready = false;
                human.close();  // 连接围栏已失效，借用腿不可继续重试或消费旧邀请。
                endpoint.close(now);
                doorbell_ready = false;
                failed = done = true;
            }
        }
        requests.tick(now);
        // HAL固定截止独立于网页定时器；失联页面不能留下无限占用的原麦克风。
        for (const auto& session : factory.poll_connectivity(now))
            client.end(session, now);
        if (*human_enabled && call_media.connectivity_expired(now))
            human.end(now);
        endpoint.poll(now);
        const auto endpoint_notice = browser_endpoint_receipt(endpoint.receipt());
        if (!endpoint_notice.empty() && endpoint_notice != endpoint_projection) {
            if (!bridge.send(endpoint_notice))
                failed = done = true;
            else
                endpoint_projection = endpoint_notice;
        }
        if (*human_enabled && available) {
            while (auto* body = waiting_human.front()) {
                receive_human(*body, now, wall);
                waiting_human.pop_front();
            }
            human.poll(now,
                       wall);  // 借用模式不消费MQTT或重复发布能力，只有宿主拥有网络。
            if (human.state() != human_previous || human.session_id() != human_previous_session) {
                human_previous = human.state();
                human_previous_session = human.session_id();
                const char* state = human_previous == HumanCallState::Ringing     ? "ringing"
                                    : human_previous == HumanCallState::Connected ? "connected"
                                    : human_previous == HumanCallState::Available ? "available"
                                    : human_previous == HumanCallState::Failed    ? "unavailable"
                                                                                  : "negotiating";
                const auto notice = browser_call_state(state, human.session_id());
                if (notice.empty() || !bridge.send(notice))
                    failed = done = true;
            }
            while (auto command = call_media.take_command())
                if (!bridge.send(*command)) {
                    failed = done = true;
                    break;
                }
        }
        doorbell.tick(now);
        privacy.tick(now);
        if (!privacy_token.empty()) {
            const auto state = privacy.state(privacy_token);
            if (state != privacy_state) {
                privacy_state = state;
                if (!privacy_notice(state == RequestState::Succeeded ? "ack"
                                    : state == RequestState::Failed  ? "rejected"
                                                                     : "unknown"))
                    failed = done = true;
            }
        }
        if (started && privacy.enabled() && !privacy_ready && !privacy_unavailable &&
            now - startup >= 30000) {
            privacy_unavailable = true;
            privacy.disconnect(now);
            if (!bridge.send(R"({"kind":"privacyUnavailable"})"))
                failed = done = true;
        }
        if (!doorbell_token.empty()) {
            const auto state = doorbell.state(doorbell_token);
            if (state != doorbell_state) {
                doorbell_state = state;
                if (!doorbell_notice(state == RequestState::Succeeded ? "ack" : "unknown"))
                    failed = done = true;
            }
        }
        // 配置启用但事件SUBACK未到时明确关闭该出口，不使原IPC观看链路永久等待。
        if (started && doorbell.enabled() && !doorbell_ready && !doorbell_unavailable &&
            now - startup >= 30000) {
            doorbell_unavailable = true;
            doorbell.disconnect(now);
            if (!bridge.send(R"({"kind":"doorbellUnavailable"})"))
                failed = done = true;
        }
        client.tick(now, wall);
        for (auto iterator = phases.begin(); iterator != phases.end();) {
            const auto phase = client.phase(iterator->first);
            if (!phase) {
                projected_rosters.erase(iterator->first);
                projected_syncs.erase(iterator->first);
                iterator = phases.erase(iterator);
                continue;
            }
            if (*phase == IPCPhase::Ended && iterator->second != IPCPhase::Ended) {
                projected_rosters.erase(iterator->first);
                projected_syncs.erase(iterator->first);
                const auto notice = ipc_browser_stop(iterator->first);
                if (notice.empty() || !bridge.send(notice))
                    failed = done = true;
            }
            if (*phase == IPCPhase::Active && iterator->second != IPCPhase::Active) {
                const auto notice = ipc_browser_active(iterator->first);
                if (notice.empty() || !bridge.send(notice))
                    failed = done = true;
            }
            if (*phase == IPCPhase::Active) {
                if (*roster_sync) {
                    const auto sync_notice =
                        ipc_browser_sync_state(client, iterator->first, available);
                    if (!sync_notice.empty() && sync_notice != projected_syncs[iterator->first] &&
                        bridge.send(sync_notice))
                        projected_syncs[iterator->first] = sync_notice;
                }
                const auto sequence = client.participant_sequence(iterator->first);
                const auto* members = client.participants(iterator->first);
                if (members && sequence && *sequence != 0 &&
                    *sequence > projected_rosters[iterator->first]) {
                    const auto notice =
                        ipc_browser_participants(iterator->first, *sequence, *members);
                    // 展示旁路失败不能关闭主对讲或扩大媒体权限；下轮可投影当前已消费原事实。
                    if (!notice.empty() && bridge.send(notice))
                        projected_rosters[iterator->first] = *sequence;
                }
            }
            iterator->second = *phase;
            ++iterator;
        }
        for (unsigned count = 0; count < 32; ++count) {
            auto command = factory.take_command();
            if (!command)
                break;
            if (!bridge.send(*command)) {
                failed = done = true;
                break;
            }
        }
        if (factory.failed() || (started && !available && now - startup >= 30000))
            failed = done = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const auto closing_time = clock_ms(false);
    endpoint.close(closing_time);
    human.close();  // 借用模式只停原人际腿，不断开宿主MQTT。
    while (auto command = call_media.take_command())
        bridge.send(*command);
    doorbell.disconnect(closing_time);
    privacy.disconnect(closing_time);
    if (!privacy_token.empty() && privacy_state == RequestState::Pending)
        privacy_notice("unknown");
    // 桥可用时交付本地取消事实，不能声称远端事件未执行。
    if (!doorbell_token.empty() && doorbell_state == RequestState::Pending)
        doorbell_notice("unknown");
    client.close_all(closing_time);
    // 正常退出先交付所有停止指令；桥断线时网页自身也必须停止全部采集。
    while (auto command = factory.take_command())
        bridge.send(*command);
    mqtt.disconnect();
    bridge.stop();
    std::cerr << (failed ? "ipc_device_failed\n" : "ipc_device_closed\n");
    return failed ? 1 : 0;
}
}  // namespace
}  // namespace ur::device
// main 只接受明确回环监听端口，不通过命令行接收长期认证凭据。
int main(int argc, char** argv) {
    const auto port = argc == 2 ? ur::device::number(argv[1], 1024, 65535) : 0;
    return port ? ur::device::run(port) : 2;
}
