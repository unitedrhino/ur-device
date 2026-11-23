// Linux组件验收探针：正式C++核心/媒体桥接实际浏览器；仅BUILD_TESTING，不持真实设备身份。
#include <openssl/rand.h>
#include <poll.h>
#include <unistd.h>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <thread>
#include "browser_bridge.hpp"
#include "browser_file_catalog.hpp"
#include "browser_file_lane.hpp"
#include "browser_file_media.hpp"
#include "browser_playback_admission.hpp"
#include "environment.hpp"
#include "file_assets.hpp"
#include "ur/device/output_focus_driver.hpp"
#include "ur/device/playback_session.hpp"

namespace {
// ProbeFocusAuthority仅授权本次固定资产/原会话的组件夹具，不替代平台权限或设备身份。
class ProbeFocusAuthority final : public ur::device::FocusAuthority {
public:
    std::string session;                 // 从受信资产准入取得原会话，不能随控制请求换绑。
    std::vector<std::string> resources;  // 固定真实输出路径的完整资源组。
    // authorize仅给固定原组及测试期限发许可；不从请求截止推导新业务授权。
    std::optional<ur::device::FocusPermit> authorize(const ur::device::FocusCommand& command,
                                                     std::uint64_t now, std::uint64_t) override {
        if (now >= 120000 || command.envelope.session != session ||
            command.focus_id != "probe-focus" || command.resources != resources)
            return std::nullopt;
        return ur::device::FocusPermit{command, 120000, 1};
    }
    // valid复核原许可而不续期；此夹具不模拟平台撤权服务。
    bool valid(const ur::device::FocusPermit& permit, std::uint64_t now,
               std::uint64_t) const override {
        return now < permit.deadline && permit.scope.envelope.session == session &&
               permit.scope.focus_id == "probe-focus" && permit.scope.resources == resources;
    }
};
// ProbePublisher 仅记录测试协议夹具的原上行；不是MQTT Broker，不持设备身份。
class ProbePublisher final : public ur::device::PublishPort {
public:
    bool release_published = false;  // 只在核心实际发布原释放事实后记录，不按平台终态猜测。
    bool publish(std::string_view, std::string_view body) override {
        std::cout << body << std::endl;
        ur::device::Envelope message;
        if (std::cout &&
            ur::device::decode_envelope(body, message) == ur::device::DecodeResult::Ok &&
            message.method == "playbackState" && !message.code &&
            message.payload.find("\"released\":true") != std::string::npos)
            release_published = true;
        return static_cast<bool>(std::cout);
    }
};
// asset 有界读取明确测试文件，HTTP端不使用请求路径访问文件系统。
std::optional<std::string> asset(const std::string& path, std::size_t limit) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input || input.tellg() <= 0 || static_cast<std::uint64_t>(input.tellg()) > limit)
        return std::nullopt;
    input.seekg(0);
    std::string value{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    return input.bad() ? std::nullopt : std::optional<std::string>(std::move(value));
}
// token 使用系统密码学随机源，仅交给已验证同源页面，不输出到日志。
std::string token() {
    std::array<unsigned char, 32> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
        return {};
    constexpr char digits[] = "0123456789abcdef";  // 固定编码，不是配对凭据。
    std::string result;
    for (const auto byte : bytes) {
        result += digits[byte >> 4];
        result += digits[byte & 15];
    }
    return result;
}
}  // namespace

// main 输入为原信令JSON行，stdout仅核心实际回执及固定探针状态；不注册生产功能。
int main(int argc, char** argv) {
    const bool dynamic_mode = argc == 5 && std::string(argv[4]) == "--dynamic-session";
    const bool session_mode = dynamic_mode || (argc == 5 && std::string(argv[4]) == "--session");
    const bool focus_mode = ur::device::environment_flag("UR_FILE_FOCUS")
                                .value_or(false);  // 仅原显式1启用，其他值不授权。
    const bool lane_mode = ur::device::environment_flag("UR_FILE_LANE")
                               .value_or(false);  // 读取一次独立副本，不借后续getenv。
    if (lane_mode && !focus_mode)
        return 2;
    if (argc != 4 && !session_mode)
        return 2;
    unsigned port = 0;  // 显式独占回环端口，冲突拒绝，不自动切环境。
    const std::string raw_port = argv[1];
    const auto parsed = std::from_chars(raw_port.data(), raw_port.data() + raw_port.size(), port);
    const std::string mime = argv[2];  // 仅受信CLI配置音乐或带音轨视频测试文件。
    if (parsed.ec != std::errc{} || parsed.ptr != raw_port.data() + raw_port.size() ||
        port < 1024 || port > 65535 ||
        (mime != "audio/wav" && mime != "video/mp4" && mime != "video/webm"))
        return 2;
    auto content = asset(argv[3], 8388608);
    auto html = asset(std::string(UR_FILE_PROBE_SOURCE) + "/file_probe.html", 262144);
    auto script = asset(std::string(UR_FILE_PROBE_SOURCE) + "/file_probe.js", 262144);
    if (!content || !html || !script)
        return 2;
    auto assets = ur::device::file_assets();  // 正式组件字节，不复制媒体逻辑到探针。
    assets.emplace("/index.html", std::move(*html));
    assets.emplace("/app.js", std::move(*script));
    const auto path = mime == "audio/wav"    ? "/media/file.wav"
                      : mime == "video/webm" ? "/media/file.webm"
                                             : "/media/file.mp4";
    assets.emplace(path, std::move(*content));
    ur::device::BrowserBridge bridge(port, token(), std::move(assets));
    if (!bridge.start())
        return 3;
    const auto epoch = std::chrono::steady_clock::now();  // 同一进程受控单调期限，不接受墙钟延长。
    const auto now = [&epoch] {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                              std::chrono::steady_clock::now() - epoch)
                                              .count());
    };
    ur::device::BrowserFileBinding binding{
        "media-session", "player-1", "content-1", path, "", mime, true, 120000, 8388608};
    std::string resolved_session = binding.session;  // 仅受信固定资产解析成功时记录实际原会话。
    std::unique_ptr<ur::device::BrowserPlaybackAdmission> fixed;  // 旧组件入口仍使用固定原实例。
    std::unique_ptr<ur::device::BrowserResolvedPlaybackAdmission>
        dynamic;  // 新入口在邀请前不准备媒体。
    if (dynamic_mode) {
        auto source = binding;  // 固定本地资产配置仍为明确夹具，可信解析使用正式宿主目录。
        source.session.clear();
        source.playback_id.clear();
        auto catalog = std::make_shared<ur::device::BrowserFileCatalog>(
            std::vector<ur::device::BrowserFileBinding>{source}, [] {
                return std::optional<std::string>(
                    "probe-ai-parent");  // 无真实AI身份，不冒称端云授权。
            });
        if (!catalog->valid())
            return 2;
        dynamic = std::make_unique<ur::device::BrowserResolvedPlaybackAdmission>(
            [catalog, &resolved_session](
                const ur::device::PlaybackInvite& invite,
                std::uint64_t now) -> std::optional<ur::device::BrowserFileResolution> {
                auto result = catalog->resolve(invite, now);
                if (result)
                    resolved_session = result->binding.session;
                return result;
            });
    } else {
        fixed = std::make_unique<ur::device::BrowserPlaybackAdmission>(
            binding, [](bool, bool, std::uint64_t) { return true; });  // 保留原组件独占输出夹具。
    }
    // media 每轮重新取当前驱动；动态入口直到可信邀请解析完成才提供唯一实例。
    const auto media = [&]() -> ur::device::BrowserFileMedia* {
        return dynamic ? dynamic->active_media() : &fixed->media();
    };
    ur::device::PlaybackAdmissionPort& admission =
        dynamic ? static_cast<ur::device::PlaybackAdmissionPort&>(*dynamic)
                : static_cast<ur::device::PlaybackAdmissionPort&>(*fixed);
    // 两种入口只创建一个执行层；会话模式由实际准备适配及共享端点接邀请，不预建伪会话。
    ProbePublisher publisher;
    std::unique_ptr<ur::device::PlaybackEngine> engine;
    std::unique_ptr<ur::device::PlaybackEndpoint> session;
    ProbeFocusAuthority focus_authority;  // 夹具许可独立于物理门控，暂停不撤销资产许可。
    std::unique_ptr<ur::device::BrowserFileLane> lane;            // 固定实际路径寿命长于焦点驱动。
    std::unique_ptr<ur::device::OutputFocusDriver> focus_driver;  // 先于借用的媒体对象析构。
    std::unique_ptr<ur::device::FocusEngine> focus;               // 必须先于共享驱动析构。
    if (lane_mode) {
        lane = std::make_unique<ur::device::BrowserFileLane>(mime == "audio/wav" ? "audio"
                                                                                 : "audioVideo");
        focus_authority.resources = mime == "audio/wav"
                                        ? std::vector<std::string>{"speaker"}
                                        : std::vector<std::string>{"speaker", "display"};
        focus_driver = std::make_unique<ur::device::OutputFocusDriver>(
            std::vector<ur::device::FocusOutputBinding>{
                {"", "", focus_authority.resources, lane.get()}},
            0, ur::device::FocusBindingMode::DeferredOnce);
    }
    if (session_mode) {
        session = std::make_unique<ur::device::PlaybackEndpoint>(
            publisher, admission, ur::device::PlaybackCapability{1, true, mime != "audio/wav"},
            "fixture-up", "fixture-down");
    } else {
        engine = std::make_unique<ur::device::PlaybackEngine>(*media(), "media-session", "player-1",
                                                              120000);
    }
    // close_current 只操作本探针构造的唯一会话/引擎，不使用第二实例作为清理兜底。
    const auto close_current = [&] {
        if (focus)
            focus->close();
        if (session)
            session->close(now());
        else
            engine->close();
    };
    std::cout << "{\"kind\":\"probeReady\",\"port\":" << port << "}" << std::endl;
    bool connected = false;       // 未鉴权连接前不消费固定绑定。
    bool lane_started = false;    // 固定路径必须先实际关闭，随后才按可信原准备结果一次绑定身份。
    bool ready = false;           // 一次发布真实初始化事实。
    bool platform_ended = false;  // 仅成功接收平台终态置位，不能由本地关闭冒充。
    int result = 0;               // 未确认退出留明确失败，不把断连当资源释放。
    std::string input_buffer;     // poll配合原始read，不混用stdio缓存导致连续报文丢失可读通知。
    std::optional<std::pair<std::uint64_t, bool>> gate_evidence;  // 独立组件票据，不伪装MQTT信令。
    while (now() < 120000) {
        auto* driver = media();  // 不跨准入/替换保留借用指针，原资源实际释放前不会替换。
        if (driver)
            driver->tick(now());
        while (auto event = bridge.poll()) {
            if (event->kind == ur::device::BrowserEventKind::Connected)
                connected = true;
            else if (event->kind == ur::device::BrowserEventKind::Message) {
                if (lane && lane->receive(event->body, now()))
                    continue;
                if (!driver || !driver->receive(event->body)) {
                    result = 4;
                    close_current();
                }
            } else {
                result = driver && driver->quiescent() ? 0 : 4;
                connected = false;
                close_current();
            }
        }
        if (connected && lane && !lane_started) {
            lane_started = true;
            if (!focus_driver->initialize(now())) {
                result = 4;
                close_current();
            }
        }
        if (lane) {
            if (!lane->poll(now())) {
                result = 4;
                close_current();
            }
            while (auto command = lane->take_command())
                if (!bridge.send(*command)) {
                    result = 4;
                    close_current();
                    break;
                }
        }
        if (connected && driver) {
            if (driver->ready() && focus_mode && !focus_driver) {
                focus_authority.session = resolved_session;
                focus_authority.resources = mime == "audio/wav"
                                                ? std::vector<std::string>{"speaker"}
                                                : std::vector<std::string>{"speaker", "display"};
                focus_driver = std::make_unique<ur::device::OutputFocusDriver>(
                    std::vector<ur::device::FocusOutputBinding>{
                        {focus_authority.session, "probe-focus", focus_authority.resources,
                         driver}},
                    0);
                if (!focus_driver->initialize(now())) {
                    result = 4;
                    close_current();
                }
            }
            if (driver->ready() && !ready && (!focus_mode || focus_driver->initial_idle())) {
                if (focus_mode) {
                    if (lane_mode) {
                        focus_authority.session = resolved_session;
                        if (!focus_driver->bind_scope(0, resolved_session, "probe-focus", now())) {
                            result = 4;
                            close_current();
                            break;
                        }
                    }
                    focus = std::make_unique<ur::device::FocusEngine>(
                        *focus_driver, focus_authority, focus_authority.session, "probe-focus",
                        focus_authority.resources, 120000);
                    std::cout << "{\"kind\":\"focusInitialIdle\",\"revision\":0}" << std::endl;
                }
                ready = true;
                std::cout << "{\"kind\":\"driverReady\"}" << std::endl;
            }
            while (auto command = driver->take_command()) {
                if (!bridge.send(*command)) {
                    result = 4;
                    close_current();
                    break;
                }
            }
        }
        pollfd input{0, POLLIN, 0};  // 可信测试管线，避免stdin阻塞媒体事件轮询。
        const auto available = poll(&input, 1, 0);
        if (available > 0 && !(input.revents & POLLIN) &&
            (input.revents & (POLLHUP | POLLERR | POLLNVAL))) {
            // 控制管线断开必须关闭实例，不能空转至两分钟期限仍播放节目。
            close_current();
            result = 4;
        } else if (available > 0 && (input.revents & POLLIN)) {
            std::array<char, 4096> chunk{};  // 控制管线单次读取有界，不阻塞媒体轮询。
            const auto count = ::read(0, chunk.data(), chunk.size());
            if (count <= 0) {
                close_current();
                result = 4;
            } else {
                input_buffer.append(chunk.data(), static_cast<std::size_t>(count));
            }
        }
        if (input_buffer.size() > 65536) {
            input_buffer.clear();
            close_current();
            result = 4;
        }
        // 已缓存的完整行也要消费；每轮最多四条，避免多控制报文饿死浏览器事实处理。
        for (unsigned index = 0; index < 4; ++index) {
            const auto newline = input_buffer.find('\n');
            if (newline == std::string::npos)
                break;
            const auto line = input_buffer.substr(0, newline);
            input_buffer.erase(0, newline + 1);
            ur::device::Envelope incoming;  // 仅识别路由，完整字段仍由正式焦点解析器校验。
            if (focus_mode &&
                ur::device::decode_envelope(line, incoming) == ur::device::DecodeResult::Ok &&
                incoming.method == "focusControl") {
                const auto submission =
                    focus ? focus->submit(line, now(), now()) : ur::device::FocusSubmission{};
                if (submission.admission == ur::device::FocusAdmission::Replay)
                    std::cout << submission.reply << std::endl;
                else if (submission.admission != ur::device::FocusAdmission::Accepted)
                    std::cout << "{\"kind\":\"admissionRejected\"}" << std::endl;
                continue;
            }
            if (line == "{\"probeOutputGate\":false}" || line == "{\"probeOutputGate\":true}") {
                const bool enabled = line == "{\"probeOutputGate\":true}";
                const auto ticket = driver && !focus_mode && !gate_evidence
                                        ? driver->request_output_gate(enabled, now())
                                        : std::nullopt;
                if (ticket)
                    gate_evidence = std::make_pair(*ticket, enabled);
                else
                    std::cout << "{\"kind\":\"admissionRejected\"}" << std::endl;
                continue;  // 私有验收入口，不改变平台状态水位、控制修订号或业务许可。
            }
            if (session) {
                // 测试墙钟从0开始与单调时钟同速；不伪称真实设备已经校时或鉴权。
                if (!session->receive("fixture-down", line, now(), now()))
                    std::cout << "{\"kind\":\"admissionRejected\"}" << std::endl;
                else {
                    ur::device::Envelope message;
                    if (ur::device::decode_envelope(line, message) ==
                            ur::device::DecodeResult::Ok &&
                        message.method == "sessionEnded")
                        platform_ended = true;
                }
            } else {
                const auto submission = engine->submit(line, now());
                if (submission.admission == ur::device::PlaybackAdmission::Replay)
                    std::cout << submission.reply << std::endl;
                else if (submission.admission != ur::device::PlaybackAdmission::Accepted)
                    std::cout << "{\"kind\":\"admissionRejected\"}" << std::endl;
            }
        }
        // 原焦点完整释放后保留其固定历史，不再用后续播放器销毁改写成功事实。
        if (focus_driver && (!lane_mode || lane_started) &&
            (!focus || !focus->closed() || !focus->resources_released())) {
            if (!focus_driver->poll(now())) {
                result = 4;
                close_current();
            }
            if (focus) {
                focus->poll(now(), now());
                while (auto reply = focus->take_reply())
                    std::cout << *reply << std::endl;
            }
        }
        if (session) {
            session->tick(now(), now());
        } else {
            engine->poll(now());
            while (auto reply = engine->take_reply())
                std::cout << *reply << std::endl;
        }
        driver = media();  // 本轮新邀请可能刚创建实例，不能继续用准入前的空值或旧借用。
        if (driver && gate_evidence &&
            driver->output_gate_confirmed(gate_evidence->first, gate_evidence->second)) {
            std::cout << "{\"kind\":\"outputGateConfirmed\",\"ticket\":\"" << gate_evidence->first
                      << "\",\"enabled\":" << (gate_evidence->second ? "true" : "false") << "}"
                      << std::endl;
            gate_evidence.reset();
        }
        if (driver && driver->quiescent() &&
            (!focus_mode || (focus && focus->resources_released() && focus->replies_drained())) &&
            (!session || (publisher.release_published && platform_ended))) {
            if (session)
                std::cout << "{\"kind\":\"released\",\"sessionEnded\":true,"
                             "\"driverReleased\":true,"
                             "\"sessionReleased\":true,\"platformEnded\":true}"
                          << std::endl;
            else
                std::cout << "{\"kind\":\"released\"}" << std::endl;
            break;
        }
        if (!connected && ready)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!media() || !media()->quiescent() ||
        (focus_mode && (!focus || !focus->resources_released() || !focus->replies_drained())))
        result = 4;
    bridge.stop();
    return result;
}
