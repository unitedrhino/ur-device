// 文件说明：逐字节验证嵌入资源、正式宿主固定资产装配及超过MSVC字符串限制的回归语料。
#include <gtest/gtest.h>
#include <ixwebsocket/IXHttpClient.h>
#include <fstream>
#include <iterator>
#include <random>
#include "browser_bridge.hpp"
#include "embedded_assets_test.hpp"
#include "file_assets.hpp"
#include "video_assets.hpp"
#include "web_assets.hpp"

namespace ur::device {
namespace {
// 读取源码资产作为独立预期值，不进行换行或编码转换。
std::string original(const char* filename) {
    std::ifstream input(std::string(UR_ASSET_SOURCE_DIR) + "/" + filename, std::ios::binary);
    EXPECT_TRUE(input.good());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
// 构造独立重复语料，校验转义与分段边界未丢失字节。
std::string repeat(const char* pattern, int count) {
    std::string result;
    for (int index = 0; index < count; ++index)
        result += pattern;
    return result;
}
// 实际网页逐字节一致，Windows与Linux均不得归一化换行或丢失UTF-8。
TEST(DesktopAssets, ActualBrowserAndVideoBytes) {
    const auto browser = browser_assets();
    const auto video = video_assets();
    EXPECT_EQ(browser.at("/index.html"), original("index.html"));
    EXPECT_EQ(browser.at("/app.css"), original("app.css"));
    EXPECT_EQ(browser.at("/app.js"), original("app.js"));
    EXPECT_EQ(browser.at("/audio-worklet.js"), original("audio-worklet.js"));
    EXPECT_EQ(browser.at("/call-worklet.js"), original("call-worklet.js"));
    EXPECT_EQ(browser.at("/input-ownership.js"), original("input-ownership.js"));
    EXPECT_EQ(browser.at("/human-voice-media.js"), original("human-voice-media.js"));
    EXPECT_EQ(browser.at("/index.html").find("input-ownership.js"), std::string::npos);
    EXPECT_EQ(browser.at("/index.html").find("human-voice-media.js"), std::string::npos);
    // 只提供固定字节，不默认加载或给已有语音页开启输入仲裁。
    EXPECT_EQ(video.at("/index.html"), original("video.html"));
    EXPECT_EQ(video.at("/app.css"), original("app.css"));
    EXPECT_EQ(video.at("/app.js"), original("video.js"));
    const auto files = file_assets();
    EXPECT_EQ(files.at("/file-player.js"), original("file-player.js"));
    EXPECT_EQ(files.at("/file-worklet.js"), original("file-worklet.js"));
    EXPECT_EQ(files.at("/file-lane.js"), original("file-lane.js"));
    EXPECT_EQ(files.at("/file-host.js"), original("file-host.js"));
    EXPECT_EQ(browser.count("/file-player.js"),
              0U);  // 默认AI界面不隐含启用文件节目。
}
// start_assets
// 用真正构建资产启动原回环桥；临时端口冲突有界重选，不替换资产白名单或伪造监听。
std::unique_ptr<BrowserBridge> start_assets(const std::map<std::string, std::string>& assets,
                                            const std::string& origin = {}) {
    std::random_device random;  // 只分配本测试自有回环监听，不扫描或停止别人的进程。
    for (unsigned attempt = 0; attempt < 20; ++attempt) {
        const auto port = 40000U + random() % 20000U;
        auto bridge = std::make_unique<BrowserBridge>(port, std::string(64, 'b'), assets, origin);
        if (bridge->start())
            return bridge;
    }
    return {};
}
// 默认页可服务固定输入仲裁／人工驱动脚本，但未加载脚本不代表用户已授权共享麦克风。
TEST(DesktopAssets, GeneratedVoiceAssetsStartOriginalBridge) {
    const auto assets = browser_assets();
    ASSERT_EQ(assets.count("/input-ownership.js"), 1U);
    ASSERT_EQ(assets.count("/human-voice-media.js"), 1U);
    EXPECT_EQ(assets.at("/index.html").find("input-ownership.js"), std::string::npos);
    EXPECT_EQ(assets.at("/index.html").find("human-voice-media.js"), std::string::npos);
    const auto bridge = start_assets(assets);
    ASSERT_TRUE(bridge);
    EXPECT_GE(bridge->port(), 1024U);
}
// 原HTTP安全头禁止inline；诊断/输出焦点固定前缀只改同源app字节，不放宽CSP或新增资产。
TEST(DesktopAssets, AudioFailureDiagnosticSameOriginHttpBytes) {
    const auto entry = original("../browser_main.cpp");
    const std::string prefix = "'use strict';globalThis.UrAudioFailureDiagnostics=true;\n";
    const std::string focus_prefix = "'use strict';globalThis.UrWebRTCOutputFocus=true;\n";
    ASSERT_NE(entry.find("if (env(\"UR_DEVICE_AUDIO_FAILURE_DIAGNOSTICS\") == \"1\")"),
              std::string::npos);
    ASSERT_NE(entry.find("assets.at(\"/app.js\").insert(0, "
                         "\"'use strict';globalThis.UrAudioFailureDiagnostics=true;\\n\");"),
              std::string::npos);
    EXPECT_EQ(entry.find("<script>globalThis.UrAudioFailureDiagnostics=true;"), std::string::npos);
    ASSERT_NE(entry.find("if (use_webrtc && (use_focus || endpoint_listen))"), std::string::npos);
    ASSERT_NE(entry.find("assets.at(\"/app.js\").insert(0, "
                         "\"'use strict';globalThis.UrWebRTCOutputFocus=true;\\n\");"),
              std::string::npos);
    EXPECT_EQ(entry.find("<script>globalThis.UrWebRTCOutputFocus=true;"), std::string::npos);
    // 0关闭、1诊断、2旧诊断inline、3焦点、4共开、5旧焦点inline；均由未改桥服务。
    for (const unsigned mode : {0U, 1U, 2U, 3U, 4U, 5U}) {
        const bool enabled = mode == 1 || mode == 4;
        const bool focus_enabled = mode == 3 || mode == 4;
        auto assets = browser_assets();
        if (enabled)
            assets.at("/app.js").insert(0, prefix);
        if (focus_enabled)
            assets.at("/app.js").insert(0, focus_prefix);
        if (mode == 2 || mode == 5) {
            const auto marker = assets.at("/index.html").find("  <script src=\"/app.js\"");
            ASSERT_NE(marker, std::string::npos);
            assets.at("/index.html")
                .insert(marker,
                        mode == 2
                            ? "  <script>globalThis.UrAudioFailureDiagnostics=true;</script>\n"
                            : "  <script>globalThis.UrWebRTCOutputFocus=true;</script>\n");
        }
        const auto bridge = start_assets(assets);
        ASSERT_TRUE(bridge);
        ix::HttpClient http;
        const auto args = http.createRequest();
        args->connectTimeout = 2;
        args->transferTimeout = 2;
        const auto origin = "http://127.0.0.1:" + std::to_string(bridge->port());
        const auto page = http.get(origin + "/", args);
        const auto app = http.get(origin + "/app.js", args);
        ASSERT_EQ(page->statusCode, 200);
        ASSERT_EQ(app->statusCode, 200);
        EXPECT_EQ(page->body, assets.at("/index.html"));
        if (mode != 2 && mode != 5) {
            EXPECT_EQ(page->body, original("index.html"));
        }
        EXPECT_EQ(app->body, (focus_enabled ? focus_prefix : std::string{}) +
                                 (enabled ? prefix : std::string{}) + original("app.js"));
        EXPECT_EQ(page->headers.at("Content-Security-Policy"),
                  "default-src 'self'; script-src 'self'; style-src 'self'; connect-src 'self' "
                  "ws://127.0.0.1:" +
                      std::to_string(bridge->port()) + "; frame-ancestors 'none'; base-uri 'none'");
        EXPECT_EQ(app->headers.at("Content-Security-Policy"),
                  page->headers.at("Content-Security-Policy"));
        EXPECT_EQ(app->headers.at("Content-Type"), "text/javascript");
        EXPECT_EQ(app->headers.at("Cache-Control"), "no-store");
        EXPECT_EQ(app->headers.at("X-Content-Type-Options"), "nosniff");
        EXPECT_EQ(app->headers.at("Referrer-Policy"), "no-referrer");
    }
}
// 正式共享入口的固定native脚本须和完整生成资产一起可启动；可服务不等于授予共享麦克风许可。
TEST(DesktopAssets, SharedHandoffNativeAssetsStartOriginalBridge) {
    // 核正式main装配源码，避免手写补齐测试夹具却遗漏真实专用入口的必要defer脚本。
    const auto entry = original("../shared_handoff_main.cpp");
    const auto input_script = entry.find("/input-ownership.js\\\" defer");
    const auto human_script = entry.find("/human-voice-media.js\\\" defer");
    ASSERT_NE(input_script, std::string::npos);
    ASSERT_NE(human_script, std::string::npos);
    EXPECT_LT(input_script, human_script);
    for (const bool human_receiver : {false, true}) {
        auto assets = browser_assets();
        const auto marker = assets.at("/index.html").find("  <script src=\"/app.js\"");
        ASSERT_NE(marker, std::string::npos);
        // 复现main的固定装配；只有离线合成boot，没有环境、邀请或URL传入自由JavaScript。
        assets["/shared-handoff-native.js"] =
            "'use strict';globalThis.UrInputOwnershipBoot='1';globalThis."
            "UrInputOwnershipSampleRate=48000;globalThis.UrHumanVoiceEnabled=true;" +
            std::string(human_receiver ? "globalThis.UrHumanReceiveOnly=true;" : "");
        assets.at("/index.html")
            .insert(marker,
                    "  <script src=\"/shared-handoff-native.js\"></script>\n  "
                    "<script src=\"/input-ownership.js\" defer></script>\n  "
                    "<script src=\"/human-voice-media.js\" defer></script>\n");
        EXPECT_LT(assets.at("/index.html").find("/input-ownership.js"),
                  assets.at("/index.html").find("/human-voice-media.js"));
        EXPECT_LT(assets.at("/index.html").find("/human-voice-media.js"),
                  assets.at("/index.html").find("/app.js"));
        ASSERT_EQ(assets.count("/human-voice-media.js"), 1U);
        const auto bridge = start_assets(assets);
        ASSERT_TRUE(bridge);
        EXPECT_GE(bridge->port(), 1024U);
    }
}
// 文件组必须和当前默认生成资产同时可启动，不能只用手写的单页夹具证明正式程序有效。
TEST(DesktopAssets, GeneratedFileAssetsStartOriginalBridge) {
    auto assets = browser_assets();
    const auto files = file_assets();
    assets.insert(files.begin(), files.end());
    const auto bridge = start_assets(assets, "https://media.example.test");
    ASSERT_TRUE(bridge);
    EXPECT_GE(bridge->port(), 1024U);
}
// 白名单新增固定脚本不扩展为目录匹配；未知同名前缀／查询／私有路径仍禁止启动。
TEST(DesktopAssets, UnknownLookalikeAssetsStillRejectBeforeListening) {
    for (const auto* path :
         {"/input-ownership.js.extra", "/human-voice-media.js?token=x", "/private.js"}) {
        auto assets = browser_assets();
        assets.emplace(path, "not-a-permitted-resource");
        EXPECT_FALSE(start_assets(assets));
    }
}
// 超过64KB的语料保留引号、反斜杠与控制字符，不依赖超长字面量。
TEST(DesktopAssets, LargeAsciiAndEscapes) {
    EXPECT_EQ(large_ascii(), repeat("abc\"\r\n\t\\", 18000));
}
// 中文与补充平面字符在分段边界仍保留完整UTF-8字节。
TEST(DesktopAssets, LargeUtf8AndEscapes) {
    EXPECT_EQ(large_utf8(), repeat("中文🎤\r\n\"\\", 5000));
}
// 空资源返回空字符串，不生成非法C++表达式。
TEST(DesktopAssets, EmptyText) { EXPECT_TRUE(empty_asset().empty()); }
}  // namespace
}  // namespace ur::device
