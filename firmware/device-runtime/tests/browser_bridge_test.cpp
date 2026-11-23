// 浏览器桥真实回环HTTP/WebSocket集成：鉴权、单客户端、有界消息与线程关闭。
#include "browser_bridge.hpp"
#include <gtest/gtest.h>
#include <ixwebsocket/IXHttpClient.h>
#include <ixwebsocket/IXHttpServer.h>
#include <ixwebsocket/IXWebSocket.h>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <random>
#include <thread>
#ifdef __linux__
#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#endif
#include "thread_progress.hpp"

namespace ur::device {
namespace {
// 真实固定SDK停服后复用本测试自有监听FD；析构旧server不能再次关闭已属于新socket的FD。
// 仅只读枚举本进程fd以定位原监听socket，绝不遍历关闭其它描述符或依赖概率并发。
TEST(SocketGuard, ServerDestructorKeepsPreciselyReusedOwnedSocket) {
#ifdef __linux__
    std::random_device random;
    std::unique_ptr<ix::HttpServer> server;
    unsigned port = 0;
    for (unsigned attempt = 0; attempt < 20; ++attempt) {
        port = 40000U + random() % 20000U;
        server = std::make_unique<ix::HttpServer>(static_cast<int>(port), "127.0.0.1", 4, 4);
        if (server->listen().first)
            break;
        server.reset();
    }
    ASSERT_TRUE(server);
    server->start();
    // 原SDK不公开监听FD，只读核对本进程AF_INET/loopback/port/SO_ACCEPTCONN绑定。
    int listener = -1;
    // 独立deleter保留closedir语义，不将libc附带属性作为模板函数指针类型。
    const auto close_directory = [](DIR* directory) { closedir(directory); };
    std::unique_ptr<DIR, decltype(close_directory)> descriptors(opendir("/proc/self/fd"),
                                                                close_directory);
    ASSERT_TRUE(descriptors);
    while (const auto* entry = readdir(descriptors.get())) {
        char* end = nullptr;
        const auto parsed = std::strtol(entry->d_name, &end, 10);
        if (!end || *end != '\0' || parsed < 0 || parsed > std::numeric_limits<int>::max())
            continue;
        const auto fd = static_cast<int>(parsed);
        sockaddr_in address{};
        socklen_t length = sizeof(address);
        int accepting = 0;
        socklen_t accepting_length = sizeof(accepting);
        if (getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) == 0 &&
            address.sin_family == AF_INET && address.sin_addr.s_addr == htonl(INADDR_LOOPBACK) &&
            address.sin_port == htons(static_cast<std::uint16_t>(port)) &&
            getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &accepting, &accepting_length) == 0 &&
            accepting)
            listener = fd;
    }
    descriptors.reset();
    ASSERT_GE(listener, 0);
    server->stop();
    ASSERT_EQ(fcntl(listener, F_GETFD), -1);
    // 停服并join后新建socket自然精准复用原空闲FD；不dup2覆盖任何他人资源。
    const auto owned = socket(AF_INET, SOCK_STREAM, 0);
    if (owned != listener) {
        if (owned >= 0)
            close(owned);
        FAIL() << "自有socket未复用已验证的原监听FD，未执行析构回归";
    }
    server.reset();
    const auto still_owned = fcntl(owned, F_GETFD);
    EXPECT_NE(still_owned, -1);
    if (still_owned != -1)
        close(owned);
#else
    GTEST_SKIP() << "精准自有FD复用回归当前仅Linux已实现";
#endif
}
// BrowserBridgeTest只使用本机临时端口与测试令牌，不读取真实设备身份。
class BrowserBridgeTest : public ::testing::Test {
protected:
    std::unique_ptr<BrowserBridge> bridge;     // 必须在客户端停机后销毁。
    ix::WebSocket client;                      // 每个测试独立实际网络连接。
    std::string token = std::string(64, 'a');  // 固定测试令牌，不用于可运行应用。
    std::mutex mutex;                          // 网络回调与断言线程的交接锁。
    std::condition_variable changed;           // 不用长sleep等待连接事实。
    bool closed = false;                       // 回调确认的关闭事实。
    bool opened = false;   // 客户端握手也必须完成，服务端接纳不代表客户端可发送。
    std::string received;  // 回调持有的正文副本。

    void SetUp() override {
        std::random_device random;
        for (unsigned attempt = 0; attempt < 20; ++attempt) {
            const auto candidate = 40000U + random() % 20000U;
            bridge = std::make_unique<BrowserBridge>(
                candidate, token, std::map<std::string, std::string>{{"/index.html", "test-page"}});
            if (bridge->start())
                break;
            bridge.reset();
        }
        ASSERT_TRUE(bridge);
        client.disableAutomaticReconnection();
        client.setOnMessageCallback([this](const ix::WebSocketMessagePtr& event) {
            std::lock_guard<std::mutex> lock(mutex);
            if (event->type == ix::WebSocketMessageType::Open)
                opened = true;
            if (event->type == ix::WebSocketMessageType::Close ||
                event->type == ix::WebSocketMessageType::Error)
                closed = true;
            if (event->type == ix::WebSocketMessageType::Message)
                received = event->str;
            changed.notify_all();
        });
    }
    void TearDown() override {
        client.stop();
        if (bridge) {
            bridge->stop();
            bridge->stop();
        }
    }
    // origin固定使用真实绑定地址，不允许被待测请求的Host影响。
    std::string origin() const { return "http://127.0.0.1:" + std::to_string(bridge->port()); }
    void connect(const std::string& requested_token, const std::string& requested_origin) {
        client.setUrl("ws://127.0.0.1:" + std::to_string(bridge->port()) +
                      "/bridge?token=" + requested_token);
        client.setExtraHeaders({{"Origin", requested_origin}});
        client.start();
        std::unique_lock<std::mutex> lock(mutex);
        ASSERT_TRUE(
            changed.wait_for(lock, std::chrono::seconds(3), [this] { return opened || closed; }));
    }
    // next只为真实网络集成短轮询；核心单测仍使用可控时钟。
    std::optional<BrowserEvent> next() {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline) {
            if (auto event = bridge->poll())
                return event;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return std::nullopt;
    }
};

TEST_F(BrowserBridgeTest, AssetsAndBootstrapRejectCrossSite) {
    ix::HttpClient http;
    auto args = http.createRequest();
    args->connectTimeout = 2;
    args->transferTimeout = 2;
    auto page = http.get(origin() + "/", args);
    ASSERT_EQ(page->statusCode, 200);
    EXPECT_EQ(page->body, "test-page");
    EXPECT_EQ(http.get(origin() + "/?media=webrtc", args)->body, "test-page");
    EXPECT_EQ(http.get(origin() + "/?media=unknown", args)->statusCode, 404);
    EXPECT_EQ(page->headers["Cache-Control"], "no-store");
    EXPECT_NE(page->headers["Content-Security-Policy"].find("ws://127.0.0.1:" +
                                                            std::to_string(bridge->port())),
              std::string::npos);
    EXPECT_EQ(http.get(origin() + "/bootstrap", args)->statusCode, 403);
    args->extraHeaders["Sec-Fetch-Site"] = "cross-site";
    EXPECT_EQ(http.get(origin() + "/bootstrap", args)->statusCode, 403);
    args->extraHeaders["Sec-Fetch-Site"] = "same-origin";
    EXPECT_EQ(http.get(origin() + "/bootstrap", args)->body, "{\"token\":\"" + token + "\"}");
    EXPECT_EQ(http.get(origin() + "/../credentials.json", args)->statusCode, 404);
}
// 独立HTTP线程在宿主完全不poll时仍可读固定原子；鉴权失败不反射请求内容。
TEST_F(BrowserBridgeTest, ThreadProgressUsesIndependentAuthenticatedHttp) {
    auto progress = std::make_shared<ThreadProgress>();
    const auto bound = bridge->port();
    bridge->stop();
    bridge = std::make_unique<BrowserBridge>(
        bound, token, std::map<std::string, std::string>{{"/index.html", "test-page"}}, "",
        progress);
    ASSERT_TRUE(bridge->start());
    ix::HttpClient http;
    auto args = http.createRequest();
    args->connectTimeout = args->transferTimeout = 2;
    const auto url = origin() + "/diagnostics/progress";
    EXPECT_EQ(http.get(url, args)->statusCode, 403);
    args->extraHeaders = {{"Sec-Fetch-Site", "same-origin"}, {"Authorization", "Bearer " + token}};
    const auto snapshot = http.get(url, args);
    ASSERT_EQ(snapshot->statusCode, 200);
    EXPECT_EQ(snapshot->headers["Cache-Control"], "no-store");
    EXPECT_NE(snapshot->body.find("\"nonTransactional\":true"), std::string::npos);
    EXPECT_NE(snapshot->body.find("\"callbacksEntered\":0"), std::string::npos);
    EXPECT_LT(snapshot->body.size(), 2048U);
    EXPECT_EQ(snapshot->body.find(token), std::string::npos);
    EXPECT_EQ(http.get(url + "?extra=1", args)->statusCode, 404);
    args->extraHeaders["Authorization"] = "Bearer wrong";
    EXPECT_EQ(http.get(url, args)->statusCode, 403);
    args->extraHeaders["Authorization"] = "Bearer " + token;
    args->extraHeaders["Sec-Fetch-Site"] = "cross-site";
    EXPECT_EQ(http.get(url, args)->statusCode, 403);
    args->extraHeaders["Sec-Fetch-Site"] = "same-origin";
    args->extraHeaders["Origin"] = "http://rejected.invalid";
    EXPECT_EQ(http.get(url, args)->statusCode, 403);
    args->extraHeaders["Origin"] = origin();
    args->extraHeaders["Host"] = "rejected.invalid";
    EXPECT_EQ(http.get(url, args)->statusCode, 403);
    args->extraHeaders.erase("Host");
    EXPECT_EQ(http.post(url, "{}", args)->statusCode, 403);
    // 新出口不改变原WS的唯一入队/发送合同。
    connect(token, origin());
    ASSERT_TRUE(next());
    client.send("{\"kind\":\"test\"}");
    const auto message = next();
    ASSERT_TRUE(message);
    EXPECT_EQ(message->body, "{\"kind\":\"test\"}");
    EXPECT_FALSE(bridge->poll());
}
// 默认不装tracker时404；原message诊断开关不隐式开放HTTP出口。
TEST_F(BrowserBridgeTest, ThreadProgressDefaultOff) {
    EXPECT_FALSE(make_thread_progress(""));
    EXPECT_FALSE(make_thread_progress("true"));
    EXPECT_FALSE(make_thread_progress("01"));
    EXPECT_TRUE(make_thread_progress("1"));
    ix::HttpClient http;
    auto args = http.createRequest();
    args->connectTimeout = args->transferTimeout = 2;
    args->extraHeaders = {{"Sec-Fetch-Site", "same-origin"}, {"Authorization", "Bearer " + token}};
    EXPECT_EQ(http.get(origin() + "/diagnostics/progress", args)->statusCode, 404);
}
// 多HTTP读者与唯一写者并行，stop join后释放tracker；快照非事务不推相邻槽的顺序。
TEST_F(BrowserBridgeTest, ThreadProgressConcurrentSnapshotsAndLifetime) {
    auto progress = std::make_shared<ThreadProgress>();
    std::weak_ptr<ThreadProgress> lifetime = progress;
    const auto bound = bridge->port();
    bridge->stop();
    bridge = std::make_unique<BrowserBridge>(
        bound, token, std::map<std::string, std::string>{{"/index.html", "test-page"}}, "",
        progress);
    ASSERT_TRUE(bridge->start());
    const auto url = origin() + "/diagnostics/progress";
    std::atomic<bool> readers_ok{true};
    std::atomic<bool> writer_started{false}, stop_writer{false};
    // 唯一写者持续到两个真实HTTP读者完成，避免有限循环先结束而没有实际并发观察。
    // 此写者只验证原子并发合同，不冒认Mosquitto已接收。
    std::thread writer([progress, &writer_started, &stop_writer] {
        writer_started.store(true);
        while (!stop_writer.load()) {
            progress->loops_entered.increment();
            std::this_thread::yield();
        }
    });
    const auto read = [&] {
        ix::HttpClient http;
        auto args = http.createRequest();
        args->connectTimeout = args->transferTimeout = 2;
        args->extraHeaders = {{"Sec-Fetch-Site", "same-origin"},
                              {"Authorization", "Bearer " + token}};
        for (unsigned index = 0; index < 12; ++index) {
            const auto snapshot = http.get(url, args);
            if (snapshot->statusCode != 200 || snapshot->body.size() >= 2048)
                readers_ok.store(false);
        }
    };
    while (!writer_started.load())
        std::this_thread::yield();
    std::thread reader_one(read), reader_two(read);
    reader_one.join();
    reader_two.join();
    stop_writer.store(true);
    writer.join();
    progress.reset();
    EXPECT_TRUE(readers_ok.load());
    EXPECT_FALSE(lifetime.expired());
    bridge->stop();
    EXPECT_TRUE(lifetime.expired());
}
// 饱和计数不会环绕；非法内部stage只投影unknown，不能回显数值或自由值。
TEST_F(BrowserBridgeTest, ThreadProgressSaturationAndUnknownStage) {
    ProgressCounter counter(std::numeric_limits<std::uint32_t>::max() - 1);
    counter.increment();
    EXPECT_EQ(counter.value(), std::numeric_limits<std::uint32_t>::max());
    EXPECT_FALSE(counter.overflow());
    counter.increment();
    EXPECT_TRUE(counter.overflow());
    auto progress = std::make_shared<ThreadProgress>();
    progress->callback_stage.store(999);
    progress->host_stage.store(999);
    const auto bound = bridge->port();
    bridge->stop();
    bridge = std::make_unique<BrowserBridge>(
        bound, token, std::map<std::string, std::string>{{"/index.html", "test-page"}}, "",
        progress);
    ASSERT_TRUE(bridge->start());
    ix::HttpClient http;
    auto args = http.createRequest();
    args->extraHeaders = {{"Sec-Fetch-Site", "same-origin"}, {"Authorization", "Bearer " + token}};
    const auto snapshot = http.get(origin() + "/diagnostics/progress", args);
    ASSERT_EQ(snapshot->statusCode, 200);
    EXPECT_NE(snapshot->body.find("\"callbackStage\":\"unknown\""), std::string::npos);
    EXPECT_NE(snapshot->body.find("\"hostStage\":\"unknown\""), std::string::npos);
    EXPECT_EQ(snapshot->body.find("999"), std::string::npos);
}
// stop与真实HTTP请求重叠时join原网络线程；最后的shared原子对象不悬空捕获主线程对象。
TEST_F(BrowserBridgeTest, ThreadProgressStopJoinsConcurrentGetter) {
    auto progress = std::make_shared<ThreadProgress>();
    std::weak_ptr<ThreadProgress> lifetime = progress;
    const auto bound = bridge->port();
    bridge->stop();
    bridge = std::make_unique<BrowserBridge>(
        bound, token, std::map<std::string, std::string>{{"/index.html", "test-page"}}, "",
        progress);
    ASSERT_TRUE(bridge->start());
    const auto url = origin() + "/diagnostics/progress";
    progress.reset();
    std::atomic<bool> first_completed{false}, valid{true};
    std::thread reader([&] {
        ix::HttpClient http;
        auto args = http.createRequest();
        args->connectTimeout = args->transferTimeout = 1;
        args->extraHeaders = {{"Sec-Fetch-Site", "same-origin"},
                              {"Authorization", "Bearer " + token}};
        for (unsigned index = 0; index < 32; ++index) {
            const auto snapshot = http.get(url, args);
            if ((index == 0 && snapshot->statusCode != 200) ||
                (snapshot->statusCode != 0 && snapshot->statusCode != 200))
                valid.store(false);
            first_completed.store(true);
        }
    });
    while (!first_completed.load())
        std::this_thread::yield();
    bridge->stop();
    reader.join();
    EXPECT_TRUE(valid.load());
    EXPECT_TRUE(lifetime.expired());
}
// 固定PCM本地选项不能扩大HTTP入口；原失败query必须404，浏览器fragment仍请求原根资产。
TEST_F(BrowserBridgeTest, FixedPcmQueryRejectedAndRootAssetUnchanged) {
    ix::HttpClient http;
    auto args = http.createRequest();
    args->connectTimeout = 2;
    args->transferTimeout = 2;
    const auto rejected = http.get(origin() + "/?softwareFixedPcm=1", args);
    ASSERT_EQ(rejected->statusCode, 404);
    EXPECT_EQ(rejected->body, "Not found");
    const auto root = http.get(origin() + "/", args);
    ASSERT_EQ(root->statusCode, 200);
    EXPECT_EQ(root->body, "test-page");
    EXPECT_EQ(http.get(origin() + "/?media=webrtc", args)->statusCode, 200);
}
// 新主题脚本是正式桌面资产，必须可启动并按原同源CSP服务，不能开放任意脚本或自由query。
TEST_F(BrowserBridgeTest, ClientThemeAssetStartsAndKeepsFixedSameOriginPolicy) {
    const auto bound = bridge->port();
    bridge->stop();
    bridge = std::make_unique<BrowserBridge>(
        bound, token,
        std::map<std::string, std::string>{{"/index.html", "theme-page"},
                                           {"/client-theme.js", "fixed-theme-component"}});
    ASSERT_TRUE(bridge->start());
    ix::HttpClient http;
    auto args = http.createRequest();
    args->connectTimeout = 2;
    args->transferTimeout = 2;
    const auto script = http.get(origin() + "/client-theme.js", args);
    ASSERT_EQ(script->statusCode, 200);
    EXPECT_EQ(script->body, "fixed-theme-component");
    EXPECT_EQ(script->headers["Content-Type"], "text/javascript");
    EXPECT_EQ(script->headers["Cache-Control"], "no-store");
    EXPECT_NE(script->headers["Content-Security-Policy"].find("script-src 'self'"),
              std::string::npos);
    EXPECT_EQ(http.get(origin() + "/client-theme.js?other=1", args)->statusCode, 404);
    EXPECT_EQ(http.get(origin() + "/arbitrary-theme.js", args)->statusCode, 404);
}
// 补充固定主题资产不放宽已有单脚本预算或未知路径白名单，失败发生在监听之前。
TEST(BrowserBridgeValidation, ClientThemeDoesNotAllowOversizedOrUnknownScript) {
    const auto token = std::string(64, 'a');  // 合成离线令牌，不用于真实设备。
    BrowserBridge large(
        42000, token,
        {{"/index.html", "theme-page"}, {"/client-theme.js", std::string(262145, 'x')}});
    EXPECT_FALSE(large.start());
    BrowserBridge unknown(42000, token,
                          {{"/index.html", "theme-page"}, {"/other-theme.js", "unknown"}});
    EXPECT_FALSE(unknown.start());
}
// 正式宿主唯一固定native路径可按原同源策略服务；HTTP query或相似路径不能加载其它脚本。
TEST_F(BrowserBridgeTest, SharedHandoffNativeKeepsFixedSameOriginPolicy) {
    const auto bound = bridge->port();
    bridge->stop();
    const std::string native =
        "'use strict';globalThis.UrInputOwnershipBoot='1';globalThis."
        "UrInputOwnershipSampleRate=48000;globalThis.UrHumanVoiceEnabled=true;";
    bridge = std::make_unique<BrowserBridge>(
        bound, token,
        std::map<std::string, std::string>{{"/index.html", "native-page"},
                                           {"/shared-handoff-native.js", native},
                                           {"/human-voice-media.js", "fixed-human-driver"}});
    ASSERT_TRUE(bridge->start());
    ix::HttpClient http;
    auto args = http.createRequest();
    args->connectTimeout = args->transferTimeout = 2;
    const auto script = http.get(origin() + "/shared-handoff-native.js", args);
    ASSERT_EQ(script->statusCode, 200);
    EXPECT_EQ(script->body, native);
    EXPECT_EQ(script->headers["Content-Type"], "text/javascript");
    EXPECT_EQ(script->headers["Cache-Control"], "no-store");
    EXPECT_NE(script->headers["Content-Security-Policy"].find("script-src 'self'"),
              std::string::npos);
    EXPECT_EQ(http.get(origin() + "/shared-handoff-native.js?other=1", args)->statusCode, 404);
    EXPECT_EQ(http.get(origin() + "/shared-handoff-native.js.extra", args)->statusCode, 404);
    EXPECT_EQ(http.get(origin() + "/custom-native.js", args)->statusCode, 404);
}
// 新增固定native路径保留单脚本预算与闭集；未知路径不能因为同名前缀而进入监听。
TEST(BrowserBridgeValidation, SharedHandoffNativeRejectsOversizedOrUnknownScript) {
    const auto token = std::string(64, 'a');
    BrowserBridge large(
        42000, token,
        {{"/index.html", "native-page"}, {"/shared-handoff-native.js", std::string(262145, 'x')}});
    EXPECT_FALSE(large.start());
    for (const auto* path : {"/shared-handoff-native.js.extra", "/shared-handoff-native.js?other=1",
                             "/custom-native.js"}) {
        BrowserBridge unknown(42000, token, {{"/index.html", "native-page"}, {path, "unknown"}});
        EXPECT_FALSE(unknown.start());
    }
}
// 呼叫音频实态处理器仅提供固定同源脚本，不放宽任意脚本路径、查询或字节预算。
TEST_F(BrowserBridgeTest, CallWorkletKeepsFixedSameOriginPolicy) {
    const auto bound = bridge->port();
    bridge->stop();
    bridge = std::make_unique<BrowserBridge>(
        bound, token,
        std::map<std::string, std::string>{{"/index.html", "call-page"},
                                           {"/call-worklet.js", "fixed-call-audio"}});
    ASSERT_TRUE(bridge->start());
    ix::HttpClient http;
    auto args = http.createRequest();
    args->connectTimeout = 2;
    args->transferTimeout = 2;
    const auto script = http.get(origin() + "/call-worklet.js", args);
    ASSERT_EQ(script->statusCode, 200);
    EXPECT_EQ(script->body, "fixed-call-audio");
    EXPECT_EQ(script->headers["Content-Type"], "text/javascript");
    EXPECT_NE(script->headers["Content-Security-Policy"].find("script-src 'self'"),
              std::string::npos);
    EXPECT_EQ(http.get(origin() + "/call-worklet.js?other=1", args)->statusCode, 404);
    EXPECT_EQ(http.get(origin() + "/other-call-worklet.js", args)->statusCode, 404);
}
// 单个呼叫脚本仍遵守原大小预算，失败不产生监听或执行路径。
TEST(BrowserBridgeValidation, CallWorkletSizeBudgetUnchanged) {
    BrowserBridge large(
        42000, std::string(64, 'a'),
        {{"/index.html", "call-page"}, {"/call-worklet.js", std::string(262145, 'x')}});
    EXPECT_FALSE(large.start());
}
// 只有固定完整实时资产可装配，媒体CSP仅扩本机blob，不开放任意HTTP、上传或脚本路径。
TEST_F(BrowserBridgeTest, LiveAssetsAreCompleteFixedAndBounded) {
    const auto bound = bridge->port();
    bridge->stop();
    std::map<std::string, std::string> assets = {
        {"/index.html", "live-page"},    {"/live-probe.js", "probe"},
        {"/live-player.js", "player"},   {"/live-host.js", "host"},
        {"/live-worklet.js", "worklet"}, {"/live-transform-worker.js", "transform"}};
    bridge = std::make_unique<BrowserBridge>(bound, token, assets);
    ASSERT_TRUE(bridge->start());
    ix::HttpClient http;
    auto args = http.createRequest();
    args->connectTimeout = 2;
    args->transferTimeout = 2;
    for (const auto& asset : assets) {
        const auto actual = http.get(origin() + asset.first, args);
        ASSERT_EQ(actual->statusCode, 200);
        EXPECT_EQ(actual->body, asset.second);
    }
    const auto page = http.get(origin() + "/", args);
    EXPECT_NE(page->headers["Content-Security-Policy"].find("media-src 'self' blob:"),
              std::string::npos);
    EXPECT_EQ(http.get(origin() + "/live-host.js?source=arbitrary", args)->statusCode, 404);
    EXPECT_EQ(http.get(origin() + "/live-upload", args)->statusCode, 404);
    bridge->stop();
    for (const auto& removed : assets) {
        if (removed.first == "/index.html")
            continue;
        auto partial = assets;
        partial.erase(removed.first);
        BrowserBridge invalid(bound, token, partial);
        EXPECT_FALSE(invalid.start());
    }
    assets["/live-host.js"] = std::string(262145, 'x');
    BrowserBridge overlong(bound, token, assets);
    EXPECT_FALSE(overlong.start());
}
TEST_F(BrowserBridgeTest, FileAssetsRequireSameOriginBearerAndBoundedCsp) {
    const auto bound = bridge->port();
    bridge->stop();
    const std::string media("RIFF\0WAVE", 9);  // 保留二进制字节，不宣称此片段是可解码音频。
    bridge = std::make_unique<BrowserBridge>(
        bound, token,
        std::map<std::string, std::string>{{"/index.html", "file-page"},
                                           {"/file-player.js", "file-component"},
                                           {"/file-lane.js", "fixed-lane-component"},
                                           {"/media/file.wav", media}},
        "https://media.example.com");
    ASSERT_TRUE(bridge->start());
    ix::HttpClient http;
    auto args = http.createRequest();
    args->connectTimeout = 2;
    args->transferTimeout = 2;
    const auto lane = http.get(origin() + "/file-lane.js", args);
    EXPECT_EQ(lane->statusCode, 200);
    EXPECT_EQ(lane->body, "fixed-lane-component");
    EXPECT_EQ(lane->headers["Content-Type"], "text/javascript");
    EXPECT_EQ(http.get(origin() + "/?lane=1", args)->statusCode, 404);
    const auto page = http.get(origin() + "/", args);
    EXPECT_NE(page->headers["Content-Security-Policy"].find("media-src 'self' blob:"),
              std::string::npos);
    EXPECT_NE(page->headers["Content-Security-Policy"].find("https://media.example.com"),
              std::string::npos);
    EXPECT_EQ(http.get(origin() + "/media/file.wav", args)->statusCode, 403);
    args->extraHeaders["Sec-Fetch-Site"] = "same-origin";
    args->extraHeaders["Authorization"] = "Bearer wrong";
    EXPECT_EQ(http.get(origin() + "/media/file.wav", args)->statusCode, 403);
    args->extraHeaders["Authorization"] = "Bearer " + token;
    args->extraHeaders["Sec-Fetch-Site"] = "cross-site";
    EXPECT_EQ(http.get(origin() + "/media/file.wav", args)->statusCode, 403);
    args->extraHeaders["Sec-Fetch-Site"] = "same-origin";
    const auto response = http.get(origin() + "/media/file.wav", args);
    EXPECT_EQ(response->statusCode, 200);
    EXPECT_EQ(response->headers["Content-Type"], "audio/wav");
    EXPECT_EQ(response->body, media);
    EXPECT_EQ(http.get(origin() + "/media/file.wav?other=1", args)->statusCode, 404);
    bridge->stop();
    ASSERT_TRUE(bridge->start());
    EXPECT_EQ(http.get(origin() + "/media/file.wav", args)->body, media);
}
TEST(BrowserBridgeValidation, MediaCannotWidenExistingPageOrInjectPolicy) {
    const auto token = std::string(64, 'a');  // 固定离线测试凭据。
    BrowserBridge old_page(42000, token, {{"/index.html", "page"}}, "https://media.example.com");
    EXPECT_FALSE(old_page.start());
    BrowserBridge missing_player(42000, token, {{"/media/file.wav", "media"}});
    EXPECT_FALSE(missing_player.start());
    BrowserBridge missing_lane_player(42000, token, {{"/file-lane.js", "lane"}});
    EXPECT_FALSE(missing_lane_player.start());
    BrowserBridge injected(42000, token, {{"/file-player.js", "player"}},
                           "https://media.example.com; script-src *");
    EXPECT_FALSE(injected.start());
    BrowserBridge large(
        42000, token,
        {{"/file-player.js", "player"}, {"/media/file.mp4", std::string(8388609, 'x')}});
    EXPECT_FALSE(large.start());
}

// 历史页仅暴露一个固定有界MP4，不借文件下载或摄像头权限；真实HTTP同时核验凭据和CSP。
TEST_F(BrowserBridgeTest, RecordAssetsRequireSameOriginBearerAndFixedMp4Route) {
    const auto bound = bridge->port();
    bridge->stop();
    const std::string archive("\0\0\0\10ftyp", 8);  // 仅检查原二进制保留，不声称是可解码录像。
    bridge = std::make_unique<BrowserBridge>(
        bound, token,
        std::map<std::string, std::string>{{"/index.html", "record-page"},
                                           {"/record-source.js", "record-component"},
                                           {"/record-host.js", "record-host"},
                                           {"/record-assets/archive.mp4", archive}});
    ASSERT_TRUE(bridge->start());
    ix::HttpClient http;
    auto args = http.createRequest();
    args->connectTimeout = 2;
    args->transferTimeout = 2;
    const auto page = http.get(origin() + "/", args);
    ASSERT_EQ(page->statusCode, 200);
    EXPECT_NE(page->headers["Content-Security-Policy"].find("media-src 'self' blob:"),
              std::string::npos);
    EXPECT_EQ(http.get(origin() + "/record-source.js", args)->headers["Content-Type"],
              "text/javascript");
    EXPECT_EQ(http.get(origin() + "/record-assets/archive.mp4", args)->statusCode, 403);
    args->extraHeaders["Sec-Fetch-Site"] = "same-origin";
    args->extraHeaders["Authorization"] = "Bearer wrong";
    EXPECT_EQ(http.get(origin() + "/record-assets/archive.mp4", args)->statusCode, 403);
    args->extraHeaders["Authorization"] = "Bearer " + token;
    args->extraHeaders["Sec-Fetch-Site"] = "cross-site";
    EXPECT_EQ(http.get(origin() + "/record-assets/archive.mp4", args)->statusCode, 403);
    args->extraHeaders["Sec-Fetch-Site"] = "same-origin";
    const auto actual = http.get(origin() + "/record-assets/archive.mp4", args);
    EXPECT_EQ(actual->statusCode, 200);
    EXPECT_EQ(actual->headers["Content-Type"], "video/mp4");
    EXPECT_EQ(actual->body, archive);
    EXPECT_EQ(http.get(origin() + "/record-assets/archive.mp4?other=1", args)->statusCode, 404);
    EXPECT_EQ(http.get(origin() + "/record-assets/other.mp4", args)->statusCode, 404);
}

// 档案白名单不是目录服务，缺专用源、任意路径及超预算资产都在监听前拒绝。
TEST(BrowserBridgeValidation, RecordAssetsCannotOpenGenericDirectoryOrUnboundedSource) {
    const auto token = std::string(64, 'a');  // 固定离线凭据，不进入实际设备身份。
    BrowserBridge missing_source(42000, token, {{"/record-assets/archive.mp4", "archive"}});
    EXPECT_FALSE(missing_source.start());
    BrowserBridge alias(42000, token,
                        {{"/record-source.js", "source"}, {"/record-assets/other.mp4", "archive"}});
    EXPECT_FALSE(alias.start());
    BrowserBridge large(42000, token,
                        {{"/record-source.js", "source"},
                         {"/record-assets/archive.mp4", std::string(8388609, 'x')}});
    EXPECT_FALSE(large.start());
}

TEST_F(BrowserBridgeTest, RoundTripAndClose) {
    connect(token, origin());
    auto event = next();
    ASSERT_TRUE(event);
    ASSERT_EQ(event->kind, BrowserEventKind::Connected);
    ASSERT_TRUE(client.sendText("{\"command\":\"start\"}").success);
    event = next();
    ASSERT_TRUE(event);
    EXPECT_EQ(event->kind, BrowserEventKind::Message);
    EXPECT_EQ(event->body, "{\"command\":\"start\"}");
    EXPECT_FALSE(bridge->send(std::string(65537, 'x')));
    ASSERT_TRUE(bridge->send("{\"state\":\"ready\"}"));
    std::unique_lock<std::mutex> lock(mutex);
    ASSERT_TRUE(
        changed.wait_for(lock, std::chrono::seconds(3), [this] { return !received.empty(); }));
    EXPECT_EQ(received, "{\"state\":\"ready\"}");
    lock.unlock();
    client.stop();
    event = next();
    ASSERT_TRUE(event);
    EXPECT_EQ(event->kind, BrowserEventKind::Closed);
}

TEST_F(BrowserBridgeTest, CrossOriginCannotOpenDevice) {
    connect(token, "https://untrusted.invalid");
    std::unique_lock<std::mutex> lock(mutex);
    ASSERT_TRUE(changed.wait_for(lock, std::chrono::seconds(3), [this] { return closed; }));
    EXPECT_FALSE(bridge->poll());
    EXPECT_FALSE(bridge->send("not-authorized"));
}

TEST_F(BrowserBridgeTest, WrongTokenCannotOpenDevice) {
    connect(std::string(64, 'b'), origin());
    std::unique_lock<std::mutex> lock(mutex);
    ASSERT_TRUE(changed.wait_for(lock, std::chrono::seconds(3), [this] { return closed; }));
    EXPECT_FALSE(bridge->poll());
}

TEST_F(BrowserBridgeTest, BinaryFailsExplicitly) {
    connect(token, origin());
    ASSERT_TRUE(next());
    ASSERT_TRUE(client.sendBinary("unexpected").success);
    const auto event = next();
    ASSERT_TRUE(event);
    EXPECT_EQ(event->kind, BrowserEventKind::Failed);
    EXPECT_FALSE(bridge->send("closed"));
}

TEST_F(BrowserBridgeTest, OversizedMessageFailsExplicitly) {
    connect(token, origin());
    ASSERT_TRUE(next());
    ASSERT_TRUE(client.sendText(std::string(65537, 'x')).success);
    const auto event = next();
    ASSERT_TRUE(event);
    EXPECT_EQ(event->kind, BrowserEventKind::Failed);
    EXPECT_FALSE(bridge->send("after-oversize"));
}

TEST_F(BrowserBridgeTest, StopReleasesConnectionAndCanRestart) {
    connect(token, origin());
    ASSERT_TRUE(next());
    bridge->stop();
    EXPECT_EQ(bridge->port(), 0U);
    EXPECT_FALSE(bridge->send("after-stop"));
    EXPECT_FALSE(bridge->poll());
    client.stop();
    {
        std::lock_guard<std::mutex> lock(mutex);
        opened = closed = false;
    }
    ASSERT_TRUE(bridge->start());
    connect(token, origin());
    const auto event = next();
    ASSERT_TRUE(event);
    EXPECT_EQ(event->kind, BrowserEventKind::Connected);
    EXPECT_TRUE(bridge->send("restarted"));
}

TEST_F(BrowserBridgeTest, SecondClientCannotReplaceOwner) {
    connect(token, origin());
    ASSERT_TRUE(next());
    ix::WebSocket second;
    bool second_closed = false;  // 始终在fixture互斥锁内访问。
    second.disableAutomaticReconnection();
    second.setUrl("ws://127.0.0.1:" + std::to_string(bridge->port()) + "/bridge?token=" + token);
    second.setExtraHeaders({{"Origin", origin()}});
    second.setOnMessageCallback([&](const ix::WebSocketMessagePtr& event) {
        std::lock_guard<std::mutex> lock(mutex);
        if (event->type == ix::WebSocketMessageType::Close ||
            event->type == ix::WebSocketMessageType::Error)
            second_closed = true;
        changed.notify_all();
    });
    second.start();
    {
        std::unique_lock<std::mutex> lock(mutex);
        EXPECT_TRUE(changed.wait_for(lock, std::chrono::seconds(3), [&] { return second_closed; }));
    }
    second.stop();
    EXPECT_FALSE(bridge->poll());
    EXPECT_TRUE(bridge->send("original-owner"));
}

TEST_F(BrowserBridgeTest, InputBackpressureIsTerminal) {
    connect(token, origin());
    ASSERT_TRUE(next());
    for (unsigned index = 0; index < 256; ++index) {
        if (!client.sendText("queued").success)
            break;
    }
    {
        std::unique_lock<std::mutex> lock(mutex);
        ASSERT_TRUE(changed.wait_for(lock, std::chrono::seconds(3), [this] { return closed; }));
    }
    const auto event = bridge->poll();
    ASSERT_TRUE(event);
    EXPECT_EQ(event->kind, BrowserEventKind::Failed);
    EXPECT_FALSE(bridge->poll());
    EXPECT_FALSE(bridge->send("after-overflow"));
}

TEST(BrowserBridgeValidation, InvalidConfigDoesNotListen) {
    BrowserBridge bad_token(40123, "bad", {});
    EXPECT_FALSE(bad_token.start());
    BrowserBridge zero_port(0, std::string(64, 'a'), {});
    EXPECT_FALSE(zero_port.start());
    BrowserBridge traversal(40123, std::string(64, 'a'), {{"/../secret", "bad"}});
    EXPECT_FALSE(traversal.start());
}
}  // namespace
}  // namespace ur::device
