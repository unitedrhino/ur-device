// 文件说明：Linux/Windows共用的人际视频模拟设备；C++独占MQTT身份，网页只提供音视频驱动。
#include <openssl/rand.h>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>
#include "browser_bridge.hpp"
#include "browser_call_media.hpp"
#include "browser_call_wire.hpp"
#include "device_credentials.hpp"
#include "environment.hpp"
#include "mosquitto_port.hpp"
#include "ur/device/human_call.hpp"
#include "video_assets.hpp"

namespace ur::device {
namespace {
// env只读取显式环境变量，不在日志中输出设备长期密钥。
std::string env(const char* key) { return environment_value(key).value_or(""); }
// mono提供可控业务预算所需的单调毫秒。
std::uint64_t mono() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
// wall提供邀请Unix截止检查，不混用单调时钟。
std::uint64_t wall() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
// random_token生成本地桥配对值与每次进程请求唯一前缀。
std::string random_token() {
    std::array<unsigned char, 32> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
        return {};
    constexpr char alphabet[] = "0123456789abcdef";
    std::string result;
    for (auto byte : bytes) {
        result += alphabet[byte >> 4];
        result += alphabet[byte & 15];
    }
    return result;
}
// port_number只接受明确的高位本机端口，不使用端口0自动绑定。
unsigned port_number(const std::string& text) {
    unsigned value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && value >= 1024 &&
                   value <= 65535
               ? value
               : 0;
}
// configuration沿用正式设备签名认证，浏览器从不接收三元组。
bool configuration(MqttConfig& config, std::string& identity, const std::string& instance) {
    const auto product = env("UR_DEVICE_PRODUCT"), device = env("UR_DEVICE_NAME");
    const auto expiry = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count() +
                        3600;
    const auto credentials = device_credentials(product, device, env("UR_DEVICE_SECRET"), instance,
                                                static_cast<std::uint64_t>(expiry));
    const auto port = port_number(env("UR_MQTT_PORT"));
    if (!credentials || port == 0 || env("UR_MQTT_HOST").empty())
        return false;
    config.host = env("UR_MQTT_HOST");
    config.port = static_cast<std::uint16_t>(port);
    config.ca_file = env("UR_MQTT_CA");
    config.client_id = credentials->client_id;
    config.username = credentials->username;
    config.password = credentials->password;
    identity = product + "/" + device;
    return true;
}
// run将所有回环、网络与媒体事实串行交给可移植状态机；桥关闭后释放全部任务资源。
int run(unsigned port) {
    const auto token = random_token();
    const auto instance = random_token().substr(0, 32);
    MqttConfig config;
    std::string identity;
    if (token.empty() || instance.empty() || !configuration(config, identity, instance)) {
        std::cerr << "invalid_device_environment\n";
        return 2;
    }
    BrowserBridge bridge(port, token, video_assets());
    if (!bridge.start()) {
        std::cerr << "browser_bind_failed\n";
        return 2;
    }
    std::cout << "http://127.0.0.1:" << bridge.port() << "/\n" << std::flush;
    MosquittoPort mqtt;
    BrowserCallMedia media;
    HumanCallClient client(mqtt, media, identity, instance);
    const auto deadline =
        mono() + 3600000;  // 本独立宿主固定一小时许可，控制不能延长或跨进程抢焦点。
    OutputFocusDriver focus({{"", "", {"speaker", "display"}, &media}}, 0,
                            FocusBindingMode::DeferredReleased);
    BrowserCallFocusAuthority authority(focus, deadline, [&]() {
        auto scope = media.focus_scope();
        if (scope)
            scope->established = client.active() && client.session_id() == scope->session;
        return scope;
    });
    if (!media.enable_hold(focus, authority, deadline))
        return 2;
    HumanCallState previous = HumanCallState::Idle;
    bool started = false, done = false, failed = false;
    bool ready_seen = false,
         output_started = false;  // 原物理静音首读齐全之前不启动平台能力与通话。
    const char* failure_reason = "none";
    // message_diagnostics 仅精确显式开关打印固定失败枚举，不输出网络正文或改变启动预算。
    const char* diagnostic_switch = std::getenv("UR_DEVICE_MESSAGE_DIAGNOSTICS");
    const bool message_diagnostics =
        diagnostic_switch && std::string_view(diagnostic_switch) == "1";
    std::string pending_track, pending_track_state;
    client.observe_control([&](const HumanCallControlReceipt& receipt) {
        const auto body = browser_call_control_receipt(receipt);
        if (body.empty() || !bridge.send(body)) {
            failure_reason = "control_receipt";
            failed = done = true;
        }
    });
    // 原转接/撤候选分别投影，平台受理与结果均不替代原sessionEnded或本端媒体实态。
    client.observe_transfer([&](const HumanCallTransferReceipt& receipt) {
        const auto body = browser_call_transfer_receipt(receipt);
        if (body.empty() || !bridge.send(body)) {
            failure_reason = "control_receipt";
            failed = done = true;
        }
    });
    while (!done) {
        const auto now = mono();
        for (unsigned count = 0; count < 32; ++count) {
            auto event = bridge.poll();
            if (!event)
                break;
            if (event->kind == BrowserEventKind::Closed ||
                event->kind == BrowserEventKind::Failed) {
                failed = event->kind == BrowserEventKind::Failed;
                if (failed)
                    failure_reason = "browser_bridge";
                done = true;
                break;
            }
            if (event->kind != BrowserEventKind::Message)
                continue;
            CallBrowserRequest request;
            if (!decode_call_browser(event->body, request)) {
                failure_reason = "browser_message";
                failed = done = true;
                break;
            }
            if (request.kind == "ready") {
                if (ready_seen) {
                    failure_reason = "duplicate_ready";
                    failed = done = true;
                    break;
                }
                if (!focus.initialize(now)) {
                    failure_reason = "device_start";
                    failed = done = true;
                    break;
                }
                ready_seen = true;
            } else if (request.kind == "callOutputApplied" || request.kind == "callHoldApplied") {
                // 实际旧/迟到票据只拒绝收录，不借它推进状态或重新打开下一腿。
                if (ready_seen)
                    media.receive_control(request.control_body, now);
            } else if (!started) {
                failure_reason = "before_start";
                failed = done = true;
                break;
            } else if (request.kind == "callDial") {
                if (!client.dial(request.target, now)) {
                    failure_reason = "call_dial";
                    failed = done = true;
                }
            } else if (request.kind == "callAccept") {
                if (!client.accept(now)) {
                    failure_reason = "call_accept";
                    failed = done = true;
                }
            } else if (request.kind == "callReject") {
                if (!client.reject(now)) {
                    failure_reason = "call_reject";
                    failed = done = true;
                }
            } else if (request.kind == "callOffer") {
                if (!client.offer_ready(request.sdp, now)) {
                    failure_reason = "call_offer";
                    failed = done = true;
                }
            } else if (request.kind == "callConnected") {
                if (!media.report_connected() || !client.media_connected(now)) {
                    failure_reason = "call_connected";
                    failed = done = true;
                }
            } else if (request.kind == "callTrack") {
                if (!client.control_track(request.track_id, request.state == "paused", now)) {
                    failure_reason = "call_track";
                    failed = done = true;
                } else {
                    pending_track = request.track_id;
                    pending_track_state = request.state;
                }
            } else if (request.kind == "callControl") {
                // 会话、版本和候选只由核心原账本取得，页面撤候选不是新的线协议操作。
                const bool transfer =
                    request.operation == "transfer" || request.operation == "cancelTransfer";
                const bool accepted = request.operation == "transfer"
                                          ? client.transfer_call(request.target, now)
                                      : request.operation == "cancelTransfer"
                                          ? client.cancel_transfer(now)
                                          : client.control_call(request.operation, now);
                const std::string rejection =
                    transfer ? "{\"kind\":\"callTransferRejected\",\"operation\":\"" +
                                   request.operation + "\"}"
                             : R"({"kind":"callControlRejected"})";
                if (!accepted && !bridge.send(rejection)) {
                    failure_reason = "control_rejected_delivery";
                    failed = done = true;
                }
            } else if (request.kind == "callHangup") {
                if (!client.end(now)) {
                    failure_reason = "call_hangup";
                    failed = done = true;
                }
            }
        }
        if (done)
            break;
        if (ready_seen && !media.poll_control(now, wall())) {
            failure_reason = "call_actual_control";
            failed = true;
            break;
        }
        if (ready_seen && !started && focus.initial_idle()) {
            if (!client.start(config, now)) {
                failure_reason = "device_start";
                failed = true;
                break;
            }
            started = true;
        }
        if (started) {
            client.poll(now, wall());
            if (client.state() == HumanCallState::Failed) {
                std::cerr << "call_runtime_failed state=" << static_cast<int>(previous) << '\n';
                if (message_diagnostics) {
                    const auto facts =
                        client.failure_facts();  // 原失败当下快照，不在收尾重新采样。
                    std::cerr << "call_runtime_diagnostic failure="
                              << static_cast<int>(facts.failure)
                              << " capabilityCode=" << facts.capability_ack_code
                              << " capabilityReason=" << static_cast<int>(facts.capability_reason)
                              << '\n';
                }
                failed = true;
                break;
            }
            if (client.active() && !output_started) {
                const auto readiness =
                    media.output_readiness();  // Connected不替代原新Peer静音实态。
                if (readiness == CallOutputReadiness::Failed ||
                    (readiness == CallOutputReadiness::Ready &&
                     (!media.bind_control_scope(client.session_id()) ||
                      !media.acquire_output(now)))) {
                    failure_reason = "call_output_acquire";
                    failed = true;
                    break;
                }
                // Pending继续交付原命令/收录原回执，poll_control仍执行原两秒截止，不能延长。
                if (readiness == CallOutputReadiness::Ready)
                    output_started = true;
            }
            if (client.state() == HumanCallState::Available)
                output_started = false;
            if (!client.active() && !client.track_control_pending()) {
                pending_track.clear();
                pending_track_state.clear();
            }
            if (!pending_track.empty() && !client.track_control_pending() && client.active()) {
                if (!bridge.send(std::string("{\"kind\":\"callTrackAck\",\"trackId\":\"") +
                                 pending_track + "\",\"state\":\"" + pending_track_state + "\"}")) {
                    failure_reason = "track_ack_delivery";
                    failed = done = true;
                }
                pending_track.clear();
                pending_track_state.clear();
            }
            if (client.state() != previous) {
                previous = client.state();
                const char* state = previous == HumanCallState::Ringing     ? "ringing"
                                    : previous == HumanCallState::Connected ? "connected"
                                    : previous == HumanCallState::Available ? "available"
                                    : previous == HumanCallState::Inviting  ? "inviting"
                                    : (previous == HumanCallState::Connecting ||
                                       previous == HumanCallState::Subscribing ||
                                       previous == HumanCallState::Reporting)
                                        ? "connecting"
                                        : "negotiating";
                const auto body = browser_call_state(state, client.session_id());
                if (body.empty() || !bridge.send(body)) {
                    failure_reason = "state_delivery";
                    failed = done = true;
                }
            }
        }
        while (auto command = media.take_command()) {
            if (!bridge.send(*command)) {
                failure_reason = "media_command";
                failed = done = true;
                break;
            }
        }
        if (!done)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    client.close();
    if (failed)
        std::cerr << "call_failed reason=" << failure_reason << '\n';
    bridge.send(failed ? R"({"kind":"error"})" : R"({"kind":"ended"})");
    bridge.stop();
    return failed ? 1 : 0;
}
}  // namespace
}  // namespace ur::device
int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: device_video_call <loopback-port>\n";
        return 2;
    }
    const auto port = ur::device::port_number(argv[1]);
    return port == 0 ? 2 : ur::device::run(port);
}
