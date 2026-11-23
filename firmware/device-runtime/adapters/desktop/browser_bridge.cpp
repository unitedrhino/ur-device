// 回环HTTP/WebSocket桌面驱动：同源令牌配对、单客户端、有界线程交接及幂等释放。
#include "browser_bridge.hpp"
#include <ixwebsocket/IXHttpServer.h>
#include <ixwebsocket/IXNetSystem.h>
#include <algorithm>
#include <charconv>
#include <deque>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>
#include "thread_progress.hpp"

namespace ur::device {
namespace {
constexpr std::size_t kMessageBytes = 65536;  // 本地媒体消息上限，不接受任意文件传输。
constexpr std::size_t kQueueCount = 64;       // 网页回调不得无限积压到设备主线程。
// reject_start 只在精确开关启用时记录编译期原因；不读取errno、资产路径或令牌，原false返回不变。
bool reject_start(const char* reason) {
    const auto* flag = std::getenv("UR_BROWSER_BRIDGE_START_DIAGNOSTICS");
    if (flag && std::string_view(flag) == "1")
        std::fprintf(stderr, "{\"marker\":\"UR_BROWSER_BRIDGE_START_DIAGNOSTIC\",\"reason\":\"%s\"}\n", reason);
    return false;
}
// progress_json读取固定25个计数槽及两个枚举；没有业务锁、自由文本或Runtime捕获。
// 字段独立原子读取而非事务快照；只能判断各阶段是否有进展，不能推严格跨线程顺序。
std::string progress_json(const ThreadProgress& progress) {
    constexpr std::array<const char*, 6> callbacks = {"idle",     "entered",  "locked",
                                                      "rejected", "enqueued", "exited"};
    constexpr std::array<const char*, 15> hosts = {
        "idle",           "bridge_input",    "output_gate", "runtime_start", "runtime_poll",
        "receipts",       "tool_command",    "text_result", "content",       "message_diagnostic",
        "media_commands", "playback_frames", "sleep",       "shutdown",      "stopped"};
    const auto callback = progress.callback_stage.load(std::memory_order_relaxed);
    const auto host = progress.host_stage.load(std::memory_order_relaxed);
    std::string body =
        "{\"kind\":\"threadProgress\",\"version\":1,"
        "\"nonTransactional\":true,\"callbackStage\":\"";
    body += callback < callbacks.size() ? callbacks[callback] : "unknown";
    body += "\",\"hostStage\":\"";
    body += host < hosts.size() ? hosts[host] : "unknown";
    body += "\",\"counters\":{";
    bool overflow = false;
    bool first = true;
    // 名称仅编译期固定值，每槽一次value读取；overflow独立保留饱和历史。
    const auto append = [&](const char* name, const ProgressCounter& counter) {
        if (!first)
            body += ',';
        first = false;
        body += '"';
        body += name;
        body += "\":";
        body += std::to_string(counter.value());
        overflow = overflow || counter.overflow();
    };
    append("callbacksEntered", progress.callbacks_entered);
    append("callbacksLocked", progress.callbacks_locked);
    append("callbacksExited", progress.callbacks_exited);
    append("rejected", progress.rejected);
    append("enqueueOverflow", progress.enqueue_overflow);
    append("mqttPollEntered", progress.mqtt_poll_entered);
    append("mqttPollExited", progress.mqtt_poll_exited);
    append("takeEntered", progress.take_entered);
    append("takeExited", progress.take_exited);
    append("loopsEntered", progress.loops_entered);
    append("loopsExited", progress.loops_exited);
    append("runtimeEntered", progress.runtime_entered);
    append("runtimeExited", progress.runtime_exited);
    append("diagnosticAttempts", progress.diagnostic_attempts);
    append("diagnosticSent", progress.diagnostic_sent);
    append("diagnosticFailed", progress.diagnostic_failed);
    constexpr std::array<std::array<const char*, 3>, 3> routes = {
        {{"messageValid", "messageEnqueued", "messageTaken"},
         {"signalingValid", "signalingEnqueued", "signalingTaken"},
         {"otherValid", "otherEnqueued", "otherTaken"}}};
    for (std::size_t index = 0; index < routes.size(); ++index) {
        append(routes[index][0], progress.routes[index].valid);
        append(routes[index][1], progress.routes[index].enqueued);
        append(routes[index][2], progress.routes[index].taken);
    }
    body += "},\"overflow\":";
    body += overflow ? "true}" : "false}";
    return body;
}
// response固定安全头；任何请求错误都不反射输入或返回配置。
// 保留既有调用签名兼容原宿主；响应类型、正文及可信来源各自编码，固定安全头不反射输入。
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
ix::HttpResponsePtr response(int code, const std::string& type, const std::string& body,
                             const std::string& authority = {}, bool file_player = false,
                             const std::string& media_origin = {}) {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    ix::WebSocketHttpHeaders headers{
        {"Content-Type", type},
        {"Cache-Control", "no-store"},
        {"X-Content-Type-Options", "nosniff"},
        {"Referrer-Policy", "no-referrer"},
        {"Content-Security-Policy",
         "default-src 'self'; script-src 'self'; style-src 'self'; connect-src "
         "'self'" +
             (authority.empty() ? std::string{} : " ws://" + authority) +
             (media_origin.empty() ? std::string{} : " " + media_origin) +
             (file_player ? "; media-src 'self' blob:" : "") +
             "; frame-ancestors 'none'; base-uri 'none'"}};
    return std::make_shared<ix::HttpResponse>(code, code == 200 ? "OK" : "Rejected",
                                              ix::HttpErrorCode::Ok, headers, body);
}
}  // namespace

bool valid_browser_media_origin(std::string_view origin) {
    if (origin.substr(0, 8) != "https://")
        return false;
    auto host = origin.substr(8);
    const auto colon = host.find(':');
    if (colon != std::string_view::npos) {
        const auto raw = host.substr(colon + 1);
        unsigned port = 0;
        const auto parsed = std::from_chars(raw.data(), raw.data() + raw.size(), port);
        if (raw.empty() || raw.front() == '0' || parsed.ec != std::errc{} ||
            parsed.ptr != raw.data() + raw.size() || port == 0 || port > 65535 || port == 443)
            return false;
        host = host.substr(0, colon);
    }
    if (host.empty() || host.size() > 253 || host.find('.') == std::string_view::npos ||
        host == "localhost" || host.size() < 3 ||
        (host.size() >= 6 && host.substr(host.size() - 6) == ".local") ||
        (host.size() >= 10 && host.substr(host.size() - 10) == ".localhost"))
        return false;
    // 标签只允许规范小写DNS字符；顶级标签含字母，排除数字IP和尾点别名。
    auto remaining = host;
    std::string_view label;
    do {
        const auto dot = remaining.find('.');
        label = remaining.substr(0, dot);
        if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-' ||
            !std::all_of(label.begin(), label.end(), [](char ch) {
                return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-';
            }))
            return false;
        if (dot == std::string_view::npos)
            break;
        remaining.remove_prefix(dot + 1);
    } while (true);
    return std::any_of(label.begin(), label.end(), [](char ch) { return ch >= 'a' && ch <= 'z'; });
}

// Impl只在网络回调间共享队列和连接引用，协议状态仍由主线程独占。
struct BrowserBridge::Impl {
    explicit Impl(unsigned port)
        : server(static_cast<int>(port), "127.0.0.1", 4, 4), requested_port(port) {}
    std::string token;                          // 只经同源bootstrap交给界面，不进入URL输出或日志。
    std::map<std::string, std::string> assets;  // 启动后只读白名单，不按请求路径读取磁盘。
    std::string media_origin;  // 可信宿主配置的单个HTTPS下载源，不接受来自模型或浏览器的域名。
    std::shared_ptr<const ThreadProgress> progress;  // 启动后只读，stop等待HTTP线程join后才释放。
    ix::HttpServer server;                           // 不监听外部网卡。
    unsigned requested_port;                         // 明确申请的端口；不使用库无法回读的0端口。
    mutable std::mutex mutex;                        // 只保护线程交接，不持锁调用网络stop。
    std::deque<BrowserEvent> queue;                  // 有界正文副本。
    std::weak_ptr<ix::WebSocket> peer;               // 单个已验证的浏览器连接。
    unsigned bound_port = 0;                         // start完成后发布，stop后归零。
    bool initialized = false;                        // 本实例是否已初始化Windows网络系统。
    bool failed = false;                             // 背压后不再接受消息，直到显式stop。

    // authority是固定回环绑定，不信任客户端Host决定信任域。
    std::string authority() const { return "127.0.0.1:" + std::to_string(bound_port); }
    // pushLocked调用方持锁；溢出保留明确故障，不默默丢掉采集或播放确认。
    void pushLocked(BrowserEvent event) {
        if (failed)
            return;
        if (queue.size() >= kQueueCount) {
            failed = true;
            queue.clear();
            queue.push_back({BrowserEventKind::Failed, {}});
        } else
            queue.push_back(std::move(event));
    }
    // http只服务固定资产；bootstrap要求浏览器同源fetch，跨站与DNS重绑定不能取得控制令牌。
    ix::HttpResponsePtr http(const ix::HttpRequestPtr& request) {
        if (request->method != "GET" || request->headers["Host"] != authority())
            return response(403, "text/plain", "Rejected");
        if (request->uri == "/diagnostics/progress") {
            // 关闭时没有观察入口；同源+原Bearer鉴权不开放裸debug口或query能力。
            const auto observed = progress;
            if (!observed)
                return response(404, "text/plain", "Not found");
            const auto origin = request->headers.find("Origin");
            if (request->headers["Sec-Fetch-Site"] != "same-origin" ||
                request->headers["Authorization"] != "Bearer " + token ||
                (origin != request->headers.end() && origin->second != "http://" + authority()))
                return response(403, "text/plain", "Rejected");
            return response(200, "application/json", progress_json(*observed));
        }
        if (request->uri == "/bootstrap") {
            if (request->headers["Sec-Fetch-Site"] != "same-origin")
                return response(403, "text/plain", "Rejected");
            return response(200, "application/json", "{\"token\":\"" + token + "\"}");
        }
        // WebRTC模式只允许固定入口查询参数；其他查询仍按资产白名单拒绝。
        const auto path =
            request->uri == "/" || request->uri == "/?media=webrtc" ? "/index.html" : request->uri;
        const auto asset = assets.find(path);
        if (asset == assets.end())
            return response(404, "text/plain", "Not found");
        const bool record_media = path == "/record-assets/archive.mp4";
        const bool media = path.rfind("/media/", 0) == 0 || record_media;
        if (media && (request->headers["Sec-Fetch-Site"] != "same-origin" ||
                      request->headers["Authorization"] != "Bearer " + token))
            return response(403, "text/plain", "Rejected");
        const auto type = path == "/media/file.wav"                   ? "audio/wav"
                          : path == "/media/file.mp3"                 ? "audio/mpeg"
                          : path == "/media/file.mp4" || record_media ? "video/mp4"
                          : path == "/media/file.webm"                ? "video/webm"
                          : path == "/index.html"                     ? "text/html; charset=utf-8"
                          : path == "/app.css"                        ? "text/css"
                                                                      : "text/javascript";
        return response(200, type, asset->second, authority(),
                        assets.count("/file-player.js") != 0 ||
                            assets.count("/record-source.js") != 0 ||
                            assets.count("/live-host.js") != 0,
                        media_origin);
    }
    // incoming只校验本地连接并入队；不调用MQTT、设备动作或PCM状态机。
    void incoming(const std::weak_ptr<ix::WebSocket>& weak,
                  const ix::WebSocketMessagePtr& message) {
        const auto socket = weak.lock();
        if (!socket)
            return;
        std::unique_lock<std::mutex> lock(mutex);
        if (message->type == ix::WebSocketMessageType::Open) {
            const auto& info = message->openInfo;
            const auto origin = info.headers.find("Origin");
            const auto host = info.headers.find("Host");
            if (failed || !peer.expired() || info.uri != "/bridge?token=" + token ||
                origin == info.headers.end() || origin->second != "http://" + authority() ||
                host == info.headers.end() || host->second != authority()) {
                // close可能同步触发库回调；释放队列锁，避免与库状态锁形成反向锁序。
                lock.unlock();
                socket->close(1008, "Rejected");
                return;
            }
            peer = weak;
            pushLocked({BrowserEventKind::Connected, {}});
        } else if (peer.lock() == socket) {
            if (message->type == ix::WebSocketMessageType::Message) {
                if (message->binary || message->str.empty() ||
                    message->str.size() > kMessageBytes) {
                    pushLocked({BrowserEventKind::Failed, {}});
                    failed = true;
                } else
                    pushLocked({BrowserEventKind::Message, message->str});
                if (failed) {
                    // 先发布终态再关闭连接，回调重入不得持有队列锁。
                    lock.unlock();
                    socket->close(1009, "Message limit");
                }
            } else if (message->type == ix::WebSocketMessageType::Close ||
                       message->type == ix::WebSocketMessageType::Error) {
                peer.reset();
                pushLocked({BrowserEventKind::Closed, {}});
            }
        }
    }
};

BrowserBridge::BrowserBridge(unsigned port, std::string token,
                             std::map<std::string, std::string> assets,
                             std::string trusted_media_origin,
                             std::shared_ptr<const ThreadProgress> progress)
    : impl_(std::make_unique<Impl>(port)) {
    impl_->token = std::move(token);
    impl_->assets = std::move(assets);
    impl_->media_origin = std::move(trusted_media_origin);
    impl_->progress = std::move(progress);
}
BrowserBridge::~BrowserBridge() { stop(); }
bool BrowserBridge::start() {
    if (impl_->initialized || impl_->requested_port < 1024 || impl_->requested_port > 65535 ||
        impl_->token.size() != 64 ||
        !std::all_of(impl_->token.begin(), impl_->token.end(),
                     [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
        return reject_start("configuration");
    const bool file_player = impl_->assets.count("/file-player.js") != 0;
    const bool record_source = impl_->assets.count("/record-source.js") != 0;
    const bool live_host =
        impl_->assets.count("/live-host.js") != 0;  // 实时接收必须完整固定五资产，不隐式启动。
    const std::vector<std::string> live_paths = {"/live-probe.js", "/live-player.js",
                                                 "/live-host.js", "/live-worklet.js",
                                                 "/live-transform-worker.js"};
    const auto live_count =
        std::count_if(live_paths.begin(), live_paths.end(),
                      [this](const std::string& path) { return impl_->assets.count(path) != 0; });
    if (live_count != 0 && live_count != static_cast<std::ptrdiff_t>(live_paths.size()))
        return reject_start("liveAssets");
    if (!impl_->media_origin.empty() &&
        (!file_player || !valid_browser_media_origin(impl_->media_origin)))
        return reject_start("mediaOrigin");
    std::size_t asset_bytes = 0;  // 固定媒体资产仅供桌面验证，不提供任意路径或无限内存文件服务。
    for (const auto& asset : impl_->assets) {
        const bool record_media = asset.first == "/record-assets/archive.mp4";
        const bool media = record_media || asset.first == "/media/file.wav" ||
                           asset.first == "/media/file.mp3" || asset.first == "/media/file.mp4" ||
                           asset.first == "/media/file.webm";
        const bool live_asset = live_host && std::find(live_paths.begin(), live_paths.end(),
                                                       asset.first) != live_paths.end();
        // 生成的输入仲裁／人工驱动及宿主固定native脚本也须可服务；仅允许精确路径，
        // 不从URL、环境或邀请加载自由脚本，允许服务不等于默认加载或授予媒体资源。
        if (asset.first != "/index.html" && asset.first != "/app.js" && asset.first != "/app.css" &&
            asset.first != "/client-theme.js" && asset.first != "/call-worklet.js" &&
            asset.first != "/input-ownership.js" && asset.first != "/human-voice-media.js" &&
            asset.first != "/shared-handoff-native.js" && asset.first != "/audio-worklet.js" &&
            asset.first != "/file-player.js" && asset.first != "/file-worklet.js" &&
            asset.first != "/file-lane.js" && asset.first != "/file-host.js" &&
            asset.first != "/record-source.js" && asset.first != "/record-host.js" && !media &&
            !live_asset)
            return reject_start("assetPath");
        if (((record_media || asset.first == "/record-host.js") && !record_source) ||
            (((media && !record_media) || asset.first == "/file-lane.js" ||
              asset.first == "/file-host.js") &&
             !file_player))
            return reject_start("assetPrerequisite");
        if (asset.second.size() > (media ? 8388608U : 262144U))
            return reject_start("assetSize");
        asset_bytes += asset.second.size();
        if (asset_bytes > 34603008)
            return reject_start("assetsTotal");
    }
    if (!ix::initNetSystem())
        return reject_start("networkInitialization");
    impl_->initialized = true;
    impl_->server.disablePerMessageDeflate();
    impl_->server.setOnConnectionCallback(
        [this](const ix::HttpRequestPtr& request, const std::shared_ptr<ix::ConnectionState>&) {
            return impl_->http(request);
        });
    static_cast<ix::WebSocketServer&>(impl_->server)
        .setOnConnectionCallback([this](const std::weak_ptr<ix::WebSocket>& socket,
                                        const std::shared_ptr<ix::ConnectionState>&) {
            if (auto peer = socket.lock())
                peer->setOnMessageCallback([this, socket](const ix::WebSocketMessagePtr& message) {
                    impl_->incoming(socket, message);
                });
        });
    if (!impl_->server.listen().first) {
        stop();
        return reject_start("listen");
    }
    impl_->bound_port = static_cast<unsigned>(impl_->server.getPort());
    impl_->server.start();
    return true;
}
void BrowserBridge::stop() {
    if (!impl_ || !impl_->initialized)
        return;
    impl_->server.stop();
    // 库的listen会重新初始化唤醒管道，已停止对象不能直接再次listen。
    // 所有回调线程join后重建网络对象；只保留启动配置，绝不保留旧连接事件。
    auto next = std::make_unique<Impl>(impl_->requested_port);
    next->token = std::move(impl_->token);
    next->assets = std::move(impl_->assets);
    next->media_origin = std::move(impl_->media_origin);
    impl_ = std::move(next);
    ix::uninitNetSystem();
}
unsigned BrowserBridge::port() const { return impl_->bound_port; }
std::optional<BrowserEvent> BrowserBridge::poll() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->queue.empty())
        return std::nullopt;
    auto event = std::move(impl_->queue.front());
    impl_->queue.pop_front();
    return event;
}
bool BrowserBridge::send(const std::string& body) {
    std::shared_ptr<ix::WebSocket> peer;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->failed || body.empty() || body.size() > kMessageBytes)
            return false;
        peer = impl_->peer.lock();
    }
    return peer && peer->bufferedAmount() <= kMessageBytes && peer->sendText(body).success;
}
}  // namespace ur::device
