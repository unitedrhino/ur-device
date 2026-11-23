// 文件说明：受限历史录像软件设备；同一MQTT持身份，浏览器只读固定档案并执行原m/g媒体意图。
// 模块划分：固定配置、能力接纳、原历史实例路由、实际浏览器事实交接和有界完整关闭。
#include <openssl/rand.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <iostream>
#include <limits>
#include <thread>
#include <vector>
#include "browser_bridge.hpp"
#include "device_credentials.hpp"
#include "environment.hpp"
#include "mosquitto_port.hpp"
#include "record_assets.hpp"
#include "record_host_support.hpp"

namespace ur::device {
namespace {
// clock_ms明确区分邀请墙钟与单调授权预算，原重试不得重新采样续期。
std::uint64_t clock_ms(bool wall) {
    return wall ? std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count()
                : std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count();
}
// token为本地桥及能力请求生成独立原关联，不输出设备三元组或私有签名。
std::string token() {
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
// env仅读宿主注入，凭据不经命令行／网页或日志传递。
std::string env(const char* key) { return environment_value(key).value_or(""); }
// number只接受明确的非零监听端口，不自动寻找替代端口或外部网卡。
unsigned number(std::string_view text) {
    unsigned value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && value >= 1024 &&
                   value <= 65535
               ? value
               : 0;
}
// configuration只组装已批准配置名，不把整份环境或长期密钥交给档案模块。
std::optional<RecordHostConfiguration> configuration() {
    std::map<std::string, std::string> values;
    for (const auto* key :
         {"UR_RECORD_ENABLED", "UR_RECORD_CAMERA_ID", "UR_RECORD_ID", "UR_RECORD_VERSION",
          "UR_RECORD_CONTENT_REF", "UR_RECORD_AUDIO", "UR_RECORD_DURATION_MS",
          "UR_RECORD_MAX_BYTES", "UR_RECORD_FILE", "UR_RECORD_LEASE_MS"})
        values.emplace(key, env(key));
    return record_host_configuration(values);
}
// run只推进一个原档案实例；Ended实例仍保留原账本，不靠重建进程／驱动擦除迟到隔离。
int run(unsigned port) {
    const auto archive = configuration();
    const auto bridge_token = token(), instance = token().substr(0, 32);
    const auto product = env("UR_DEVICE_PRODUCT"), device = env("UR_DEVICE_NAME");
    const auto credentials = device_credentials(product, device, env("UR_DEVICE_SECRET"), instance,
                                                clock_ms(true) / 1000 + 3600);
    const auto mqtt_port = number(env("UR_MQTT_PORT"));
    std::string bytes;
    if (!archive || bridge_token.empty() || instance.empty() || !credentials || !mqtt_port ||
        env("UR_MQTT_HOST").empty() || !read_record_archive(*archive, bytes)) {
        std::cerr << "invalid_record_device_environment\n";
        return 2;
    }
    auto assets = record_assets();
    assets.emplace(archive->asset.route, std::move(bytes));
    BrowserBridge bridge(port, bridge_token, std::move(assets));
    if (!bridge.start()) {
        std::cerr << "record_browser_bind_failed\n";
        return 2;
    }
    std::cout << "http://127.0.0.1:" << port << "/\n" << std::flush;
    MqttConfig config;
    config.host = env("UR_MQTT_HOST");
    config.port = static_cast<std::uint16_t>(mqtt_port);
    config.ca_file = env("UR_MQTT_CA");
    config.client_id = credentials->client_id;
    config.username = credentials->username;
    config.password = credentials->password;
    const auto up = "$thing/up/signaling/" + product + "/" + device;
    const auto down = "$thing/down/signaling/" + product + "/" + device;
    MosquittoPort mqtt;
    Requests requests(mqtt);
    std::uint64_t lease_deadline = 0;  // 仅原邀请开始时固定，重复邀请不能延长。
    BrowserRecordPlaybackPort media(archive->asset, [&](std::uint64_t now) {
        return lease_deadline != 0 && now < lease_deadline;
    });
    std::unique_ptr<RecordPlaybackClient> client;  // 唯一实例，与原媒体对象共同保留到宿主退出。
    std::optional<Envelope> binding;               // 固定原实例路由，不从网页选择平台会话。
    std::vector<Envelope> busy_invites;            // 至多32份被拒原邀请事实，不驱逐旧原意图。
    const auto cap_token = instance + "-record-cap";
    std::string projection;  // 最近投影仅用于减少重复UI消息，不作为实际媒体证据。
    bool started = false, subscribed = false, cap_sent = false, available = false;
    bool done = false, failed = false, browser_alive = false;
    int subscription = 0;
    std::uint64_t startup = 0;
    // drain_media交原驱动事实给共享核心，空sender连通不由宿主变成playing。
    const auto drain_media = [&](std::uint64_t now) {
        while (auto event = media.take_event()) {
            if (!client)
                continue;
            if (event->kind == BrowserRecordEvent::Kind::Offer) {
                if (!client->offer_ready(event->media_revision, event->sdp, now))
                    client->close(now);
            } else if (event->kind == BrowserRecordEvent::Kind::Connected) {
                if (!client->media_connected(event->media_revision, now))
                    client->close(now);
            } else
                client->close(now);
        }
    };
    // deliver_media保持原本地字节；背压立即关闭，不输出SDP／档案路径或偷换token。
    const auto deliver_media = [&] {
        while (auto command = media.take_command())
            if (!bridge.send(*command))
                return false;
        return true;
    };
    // reject_busy只拒绝新的原邀请，不停当前实例；原参数变化不能变成新控制意图。
    const auto reject_busy = [&](const Envelope& invite, std::uint64_t now) {
        if (binding && invite.token == binding->token)
            return;
        const auto existing =
            std::find_if(busy_invites.begin(), busy_invites.end(),
                         [&](const Envelope& original) { return original.token == invite.token; });
        if (existing != busy_invites.end()) {
            if (existing->session != invite.session || existing->payload != invite.payload)
                return;
        } else {
            if (busy_invites.size() >= 32)
                return;  // 满时保持拒绝，不丢原token保护后重跑。
            busy_invites.push_back(invite);
        }
        Envelope reply;
        reply.token = invite.token;
        reply.session = invite.session;
        reply.method = "sessionReject";
        reply.payload = R"({"reason":"resourceBusy"})";
        std::string body;
        if (encode_envelope(reply, body) == DecodeResult::Ok)
            requests.submit({reply.token, reply.session, reply.method, up, std::move(body)}, now);
    };
    while (!done) {
        const auto now = clock_ms(false), wall = clock_ms(true);
        media.tick(now);
        for (unsigned count = 0; count < 32; ++count) {
            auto event = bridge.poll();
            if (!event)
                break;
            if (event->kind == BrowserEventKind::Connected) {
                browser_alive = true;
                continue;
            }
            if (event->kind == BrowserEventKind::Closed ||
                event->kind == BrowserEventKind::Failed) {
                browser_alive = false;
                failed = done = true;
                break;
            }
            if (event->kind != BrowserEventKind::Message)
                continue;
            std::string kind;
            if (record_host_local_request(event->body, kind)) {
                if (kind == "shutdown") {
                    done = true;
                    break;
                }
                if (started || !mqtt.connect(config)) {
                    failed = done = true;
                    break;
                }
                started = true;
                startup = now;
            } else if (!media.receive(event->body)) {
                failed = done = true;
                break;
            }
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
            } else if (event->kind == MqttEvent::Kind::Subscribed &&
                       event->operation == subscription) {
                subscribed = event->code == 0;
                if (!subscribed)
                    failed = done = true;
            } else if (event->kind == MqttEvent::Kind::Message && event->topic == down) {
                VoiceEvent ack;
                if (decode_voice_event(WireRoute::Signaling, event->body, ack) == WireResult::Ok &&
                    ack.envelope.token == cap_token && ack.request_method == "capabilityReport") {
                    if (available)
                        continue;
                    if (ack.envelope.code != 200 || ack.revision != 1 ||
                        !requests.acknowledge({cap_token, {}, "capabilityReport", 200, up}, now)) {
                        failed = done = true;
                        break;
                    }
                    available = true;
                    if (!bridge.send(R"({"kind":"recordAvailable"})"))
                        failed = done = true;
                    std::cout << "record_available\n" << std::flush;
                } else if (available) {
                    Envelope envelope;
                    if (decode_envelope(event->body, envelope) != DecodeResult::Ok)
                        continue;
                    if (binding && envelope.session == binding->session) {
                        client->receive(down, event->body, now, wall);
                    } else if (envelope.method == "sessionInvite") {
                        RecordPlaybackInvite invite;
                        if (decode_record_playback_invite(WireRoute::Signaling, event->body,
                                                          invite) != WireResult::Ok)
                            continue;
                        if (client) {
                            reject_busy(envelope, now);
                            continue;
                        }
                        if (now > std::numeric_limits<std::uint64_t>::max() - archive->lease_ms) {
                            failed = done = true;
                            break;
                        }
                        lease_deadline = now + archive->lease_ms;
                        binding = envelope;
                        client = std::make_unique<RecordPlaybackClient>(
                            mqtt, media, std::move(invite), up, down, lease_deadline);
                        client->receive(down, event->body, now, wall);
                    } else if (
                        envelope.method == "requestAck" && !envelope.request &&
                        envelope.code == 200 && !envelope.event_sequence &&
                        !envelope.message_sequence &&
                        envelope.payload ==
                            R"({"requestMethod":"sessionReject","result":{"reason":"resourceBusy","state":"ended"}})") {
                        requests.acknowledge(
                            {envelope.token, envelope.session, "sessionReject", *envelope.code, up},
                            now);
                    }
                }
            } else if (event->kind == MqttEvent::Kind::Failed ||
                       event->kind == MqttEvent::Kind::Disconnected) {
                failed = done = true;
            }
        }
        if (!cap_sent && subscribed && media.ready()) {
            Envelope command;
            std::string body;
            if (record_host_capability(cap_token, archive->asset, command) != WireResult::Ok ||
                encode_envelope(command, body) != DecodeResult::Ok ||
                requests.submit({cap_token, {}, "capabilityReport", up, std::move(body)}, now) !=
                    SubmitResult::Accepted)
                failed = done = true;
            else
                cap_sent = true;
        }
        requests.tick(now);
        drain_media(now);
        if (client) {
            client->tick(now);
            const auto current = record_host_projection(*client);
            if (current.empty() || (current != projection && !bridge.send(current)))
                failed = done = true;
            projection = current;
        }
        if (browser_alive && !deliver_media())
            failed = done = true;
        if (media.failed() || (started && !available && now - startup >= 30000))
            failed = done = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const auto closing = clock_ms(false);
    if (client)
        client->close(closing);
    else
        media.stop_all();
    // 原网页仍在线时给实际关闭留有限交接预算；超时保持未知，不虚构所有资源已释放。
    bool stopped = media.stop_all();
    while (browser_alive && !stopped && clock_ms(false) - closing < 10000) {
        if (!deliver_media()) {
            failed = true;
            break;
        }
        for (unsigned count = 0; count < 32; ++count) {
            auto event = bridge.poll();
            if (!event)
                break;
            if (event->kind == BrowserEventKind::Closed ||
                event->kind == BrowserEventKind::Failed) {
                browser_alive = false;
                break;
            }
            if (event->kind == BrowserEventKind::Message)
                media.receive(event->body);
        }
        const auto now = clock_ms(false);
        media.tick(now);
        if (client)
            client->tick(now);
        if (started)
            mqtt.poll();
        stopped = media.stop_all();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!stopped)
        failed = true;
    mqtt.disconnect();
    bridge.stop();
    std::cerr << (failed ? "record_device_failed\n" : "record_device_closed\n");
    return failed ? 1 : 0;
}
}  // namespace
}  // namespace ur::device
// main仅读显式回环端口，原设备身份和档案配置全部由宿主环境提供。
int main(int argc, char** argv) {
    const auto port = argc == 2 ? ur::device::number(argv[1]) : 0;
    return port ? ur::device::run(port) : 2;
}
