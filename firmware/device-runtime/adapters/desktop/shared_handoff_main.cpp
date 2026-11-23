// 文件说明：正式Linux共享AI→人工设备入口；浏览器仅持本地配对令牌，C++独占平台身份和原协议状态。
#include <openssl/rand.h>

#include <array>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <thread>

#include "action_file_ledger.hpp"
#include "browser_bridge.hpp"
#include "device_credentials.hpp"
#include "environment.hpp"
#include "file_actions.hpp"
#include "mosquitto_port.hpp"
#include "openssl_ctr.hpp"
#include "opus_codec.hpp"
#include "shared_handoff_host.hpp"
#include "udp_socket.hpp"
#include "ur/device/udp_voice_media.hpp"
#include "web_assets.hpp"

namespace ur::device {
namespace {
// monotonic/wall 使用独立时钟域，不用协议或网页时间刷新原租约。
std::uint64_t monotonic() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}
std::uint64_t wall() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
}
// random_token 生成原启动配对nonce，只送本地桥，不输出凭据。
std::string random_token() {
    std::array<unsigned char, 32> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
        return {};
    constexpr char alphabet[] = "0123456789abcdef";
    std::string out;
    for (const auto byte : bytes) {
        out += alphabet[byte >> 4];
        out += alphabet[byte & 15];
    }
    return out;
}
std::string env(const char* key) {
    return environment_value(key).value_or("");
}
// port 拒绝隐式目标、越界或非十进制输入。
unsigned port(std::string_view value) {
    unsigned result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && result <= 65535
               ? result
               : 0;
}
// run 只配置正式设备签名；一个回环桥/采集源/网络连接持续服务AI和Human两腿。
int run(unsigned local_port, bool human_receiver) {
    const auto human_endpoint_source = environment_flag("UR_DEVICE_HUMAN_ENDPOINT_SOURCE");
    if (!human_endpoint_source) {
        std::cerr << "invalid_human_endpoint_source_environment\n";
        return 2;
    }
    // 模式限制只收窄native可协商子集；严格环境读取在签名/监听前拒绝非法值，绝不改AEC事实。
    const auto half_duplex_only = environment_flag("UR_DEVICE_HALF_DUPLEX_ONLY");
    if (!half_duplex_only) {
        std::cerr << "invalid_half_duplex_only_environment\n";
        return 2;
    }
    // 同独立UDP入口的严格native开关，先拒非法/纯Human，不签名、不监听、不经网页授权。
    const auto reply_receipt = env("UR_DEVICE_UDP_REPLY_START_RECEIPT");
    if ((!reply_receipt.empty() && reply_receipt != "0" && reply_receipt != "1") ||
        (human_receiver && reply_receipt == "1")) {
        std::cerr << "invalid_udp_reply_start_receipt_environment\n";
        return 2;
    }
    const auto paired_token = random_token();
    const auto instance = random_token().substr(0, 32);
    const auto clock = monotonic(), unix_ms = wall();
    const auto product = env("UR_DEVICE_PRODUCT"), device = env("UR_DEVICE_NAME");
    const auto credentials = device_credentials(product, device, env("UR_DEVICE_SECRET"), instance,
                                                unix_ms / 1000 + 3600);
    const auto mqtt_port = port(env("UR_MQTT_PORT"));
    if (paired_token.empty() || instance.empty() || !credentials || !mqtt_port ||
        env("UR_MQTT_HOST").empty()) {
        std::cerr << "invalid_device_environment\n";
        return 2;
    }
    SharedHandoffHostConfig config;
    config.human_receiver = human_receiver;
    config.human_endpoint_source = *human_endpoint_source;
    config.half_duplex_only = *half_duplex_only;
    config.udp_reply_start_receipt = reply_receipt == "1";
    // 临时借用只接受native显式开关；纯受话角色不具备原AI上下文，不能复用恢复入口。
    const auto temporary = env("UR_DEVICE_TEMPORARY_CALL");
    if ((!temporary.empty() && temporary != "1") || (human_receiver && temporary == "1")) {
        std::cerr << "invalid_temporary_call_environment\n";
        return 2;
    }
    config.temporary_calls = temporary == "1";
    config.identity = product + "/" + device;
    config.instance = instance;
    config.boot = clock ? clock : 1;
    config.deadline = clock + 3600000;
    config.expires_at = unix_ms + 3600000;
    config.mqtt = {env("UR_MQTT_HOST"),
                   static_cast<std::uint16_t>(mqtt_port),
                   credentials->client_id,
                   credentials->username,
                   credentials->password,
                   env("UR_MQTT_CA"),
                   10000};
    auto assets = browser_assets();
    const bool context_diagnostic =
        env("UR_DEVICE_CONTEXT_DIAGNOSTIC") == "1"; // 仅精确1启用原私有事实脱敏观察。
    config.context_diagnostic = context_diagnostic;
    const auto marker = assets.at("/index.html").find("  <script src=\"/app.js\"");
    if (marker == std::string::npos)
        return 2;
    // native固定资产仅此入口装配；输入仲裁驱动须先于人工驱动和app的defer执行，不放宽原装配校验。
    // 默认独立页面仍不加载共享输入；不通过URL、邀请、环境自由JS文本提升能力。
    assets["/shared-handoff-native.js"] =
        "'use strict';globalThis.UrInputOwnershipBoot='" + std::to_string(config.boot) +
        "';globalThis.UrInputOwnershipSampleRate=48000;globalThis."
        "UrHumanVoiceEnabled=true;" +
        std::string(human_receiver ? "globalThis.UrHumanReceiveOnly=true;" : "") +
        (human_receiver && config.human_endpoint_source
             ? "globalThis.UrHumanDialEnabled=true;"
             : "") +
        (config.temporary_calls ? "globalThis.UrTemporaryCalls=true;" : "") +
        (config.human_endpoint_source ? "globalThis.UrHumanEndpointSource=true;" : "");
    assets.at("/index.html")
        .insert(marker, "  <script src=\"/shared-handoff-native.js\"></script>\n  "
                        "<script src=\"/input-ownership.js\" defer></script>\n  "
                        "<script src=\"/human-voice-media.js\" defer></script>\n");
    BrowserBridge bridge(local_port, paired_token, std::move(assets));
    if (!bridge.start()) {
        std::cerr << "browser_bind_failed\n";
        return 2;
    }
    std::cout << "http://127.0.0.1:" << bridge.port() << "/\n" << std::flush;
    BrowserPcm pcm({48000, 1, 20});
    OpusCodec codec;
    UdpSocket socket;
    OpenSslCtr cipher;
    UdpVoiceMedia media(pcm, codec, socket, cipher);
    if (context_diagnostic) {
        (void)socket.enable_receive_failure_diagnostics(); // 实物socket首次receive前启用；不改变失败或重试。
        media.enable_failure_diagnostics(); // 原泵失败前启用，仅保留固定首因，不能由关闭反推原因。
    }
    MosquittoPort mqtt;
    config.timing = [&media] { return media.timing(); };
    config.stopped_timing = [&media](std::uint64_t epoch) {
        return media.take_stopped_timing(epoch);
    };
    config.last_voice_sent_ms = [&media] { return media.last_non_silent_sent_ms(); };
    FileActions action_driver(pcm);
    std::unique_ptr<ActionFileLedger> action_ledger; // 原设备身份绑定文件，跨AI/人工保持同一账本。
    if (!configured_action_ledger(config.identity, action_ledger)) {
        std::cerr << "action_ledger_unavailable\n";
        return 2;
    }
    ActionEngine actions(action_driver, 5000, action_ledger.get());
    bool paired = false, done = false, failed = false;
    SharedHandoffHost host(
        mqtt, media, pcm, std::move(config), [&paired] { return paired; },
        [&bridge](const std::string& body) { return bridge.send(body); }, &actions);
    std::string previous_context_diagnostic; // 只去重固定布尔/计数JSON，不持有私有正文或关联标识。
    const auto observe_context = [&] {
        if (!context_diagnostic)
            return;
        const auto line =
            host.context_diagnostic_line(); // 从实际Human留存及原ACK提交回读，不从UI推断。
        if (!line.empty() && line != previous_context_diagnostic) {
            std::cerr << line << '\n' << std::flush;
            previous_context_diagnostic = line;
        }
    };
    std::string previous_startup_diagnostic; // 只保留固定启动标签和布尔，不携带关联身份。
    const auto observe_startup = [&] {
        if (!context_diagnostic)
            return;
        const auto line = host.startup_diagnostic_line();
        if (!line.empty() && line != previous_startup_diagnostic) {
            std::cerr << line << '\n' << std::flush;
            previous_startup_diagnostic = line;
        }
    };
    bool exit_diagnostic_printed = false; // 原main退出前至多输出一次；日志失败也不重试业务。
    // observe_exit
    // 只输出固定枚举与布尔；默认关闭，sink失败不影响原return或任何资源操作。
    const auto observe_exit = [&](SharedHostExitBranch branch) {
        if (!context_diagnostic || exit_diagnostic_printed)
            return;
        exit_diagnostic_printed = true;
        const auto line = host.exit_diagnostic_line(branch, failed);
        if (!line.empty()) {
            (void)std::fwrite(line.data(), 1, line.size(), stderr);
            (void)std::fputc('\n', stderr);
            (void)std::fflush(stderr);
        }
        // 独立固定首因旁证不改变原四字段退出行、返回值、队列或清理时序。
        const auto media_line = host.human_media_failure_diagnostic_line();
        if (!media_line.empty()) {
            (void)std::fwrite(media_line.data(), 1, media_line.size(), stderr);
            (void)std::fputc('\n', stderr);
            (void)std::fflush(stderr);
        }
        const auto runtime_line = host.ai_runtime_failure_diagnostic_line(); // 只输出原首因，unknown不作归因。
        if (!runtime_line.empty()) {
            (void)std::fwrite(runtime_line.data(), 1, runtime_line.size(), stderr);
            (void)std::fputc('\n', stderr);
            (void)std::fflush(stderr);
        }
        const auto driver_line = host.duplex_driver_failure_diagnostic_line(); // 在原close前单次投影，不重读票据。
        if (!driver_line.empty()) {
            (void)std::fwrite(driver_line.data(), 1, driver_line.size(), stderr);
            (void)std::fputc('\n', stderr);
            (void)std::fflush(stderr);
        }
        const auto human_line = host.human_signal_diagnostic_line(); // 原close前读当前信令阶段，不能用cleanup的Idle覆盖。
        if (!human_line.empty()) {
            (void)std::fwrite(human_line.data(), 1, human_line.size(), stderr);
            (void)std::fputc('\n', stderr);
            (void)std::fflush(stderr);
        }
        const auto rejection_line =
            host.human_rejection_diagnostic_line(); // 首拒帧独立闭集，不含原文。
        if (!rejection_line.empty()) {
            (void)std::fwrite(rejection_line.data(), 1, rejection_line.size(), stderr);
            (void)std::fputc('\n', stderr);
            (void)std::fflush(stderr);
        }
        const auto udp_failure = media.failure_diagnostic(); // 已销毁泵的固定首因仍可只读。
        const auto udp_reason = udp_failure ? udp_failure_name(*udp_failure) : std::string_view("unknown");
        const auto udp_line = std::string("{\"kind\":\"voiceMediaFailureDiagnostic\",\"reason\":\"") +
                              std::string(udp_reason) + "\"}";
        (void)std::fwrite(udp_line.data(), 1, udp_line.size(), stderr);
        (void)std::fputc('\n', stderr);
        (void)std::fflush(stderr);
        const auto socket_failure = socket.receive_failure_diagnostic(); // 原socket实际首因，不从泵失败反推。
        const auto socket_reason = socket_failure ? *socket_failure : std::string_view("unknown");
        const auto socket_line = std::string("{\"kind\":\"udpSocketReceiveFailureDiagnostic\",\"reason\":\"") +
                                 std::string(socket_reason) + "\"}";
        (void)std::fwrite(socket_line.data(), 1, socket_line.size(), stderr);
        (void)std::fputc('\n', stderr);
        (void)std::fflush(stderr);
    };
    while (!done) {
        for (unsigned i = 0; i < 32; ++i) {
            auto event = bridge.poll();
            if (!event)
                break;
            if (event->kind == BrowserEventKind::Connected)
                paired = true;
            else if (event->kind == BrowserEventKind::Closed ||
                     event->kind == BrowserEventKind::Failed) {
                paired = false;
                failed = event->kind == BrowserEventKind::Failed;
                observe_exit(failed ? SharedHostExitBranch::BridgeInvalid
                                    : SharedHostExitBranch::BridgeClosed);
                done = true;
                break;
            } else if (event->kind == BrowserEventKind::Message) {
                // 拒绝的重复/迟到票据不成为新授权，不因未知本地UI事件切断已经接通的另一腿。
                host.receive_local(event->body, monotonic(), wall());
            }
        }
        if (!done && !host.poll(monotonic(), wall())) {
            failed = host.connection_state() != SharedConnectionState::Closed;
            observe_exit(SharedHostExitBranch::HostPollFalse);
            done = true;
        }
        observe_context();
        observe_startup(); // 最后原poll结果在close前采样，清理不能覆盖原启动故障阶段。
        if (!done)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    host.close(monotonic());
    observe_context();
    bridge.send(failed ? "{\"kind\":\"error\"}" : "{\"kind\":\"ended\"}");
    bridge.stop();
    return failed ? 1 : 0;
}
} // namespace
} // namespace ur::device
// main
// 只接受原本地监听端口和显式受话角色，密钥通过正式环境安全注入。
int main(int argc, char** argv) {
    if (argc != 2 && (argc != 3 || std::string_view(argv[2]) != "--human")) {
        std::cerr << "usage: device_ai_handoff <loopback-port> [--human]\n";
        return 2;
    }
    const auto port = ur::device::port(argv[1]);
    return port >= 1024 ? ur::device::run(port, argc == 3) : 2;
}
