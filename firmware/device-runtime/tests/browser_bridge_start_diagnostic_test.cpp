// 文件说明：真实固定资产启动及原监听端口冲突；固定诊断不推断现场失败原因。
#include "browser_bridge.hpp"
#include "file_assets.hpp"
#include "web_assets.hpp"
#include <cstdlib>
#include <functional>
#include <gtest/gtest.h>
#include <memory>
#include <random>

namespace ur::device {
namespace {
// switch_value 在两桌面平台恢复环境开关；不写入启动配置或日志。
void switch_value(const char *value) {
#ifdef _WIN32
  _putenv_s("UR_BROWSER_BRIDGE_START_DIAGNOSTICS", value ? value : "");
#else
  if (value)
    setenv("UR_BROWSER_BRIDGE_START_DIAGNOSTICS", value, 1);
  else
    unsetenv("UR_BROWSER_BRIDGE_START_DIAGNOSTICS");
#endif
}
// BridgeStartFixture 保留原环境开关，合成令牌与真实构建资产不含平台身份。
class BridgeStartFixture : public ::testing::Test {
protected:
  // SetUp 只保存测试进程原开关，不创建平台资源。
  void SetUp() override {
    const char *value = std::getenv("UR_BROWSER_BRIDGE_START_DIAGNOSTICS");
    if (value)
      original = value;
    existed = value != nullptr;
  }
  // TearDown 恢复开关，不影响其它桌面测试默认行为。
  void TearDown() override {
    switch_value(existed ? original.c_str() : nullptr);
  }
  // assets
  // 装配与正常media模式相同的生成资产和两个原同源前缀，不包含任意用户路径。
  std::map<std::string, std::string> assets() {
    auto value = browser_assets();
    value.at("/app.js").insert(
        0, "'use strict';globalThis.UrAudioFailureDiagnostics=true;\n");
    const auto marker =
        value.at("/index.html").find("  <script src=\"/app.js\"");
    EXPECT_NE(marker, std::string::npos);
    if (marker != std::string::npos)
      value.at("/index.html")
          .insert(marker, "  <script src=\"/file-player.js\" defer></script>\n"
                          "  <script src=\"/file-lane.js\" defer></script>\n"
                          "  <script src=\"/file-host.js\" defer></script>\n");
    auto files = file_assets();
    value.insert(files.begin(), files.end());
    return value;
  }
  // listening 每次独立校验：成功必须无诊断；失败仅允许显式开关下的固定listen原因。
  // port_for_attempt 只为确定性重试回归注入本测试持有端口；默认保持原随机序列与32次预算。
  std::unique_ptr<BrowserBridge> listening(
      const std::function<unsigned(unsigned)> &port_for_attempt = {}) {
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
      auto bridge = std::make_unique<BrowserBridge>(
          port_for_attempt ? port_for_attempt(attempt)
                           : 40000 + std::random_device{}() % 20000,
          std::string(64, 'a'),
          assets(), "https://media.example.test");
      ::testing::internal::CaptureStderr();
      const auto started = bridge->start();
      const auto output = ::testing::internal::GetCapturedStderr();
      const auto *flag = std::getenv("UR_BROWSER_BRIDGE_START_DIAGNOSTICS");
      const bool diagnostic_enabled = flag && std::string_view(flag) == "1";
      if (started) {
        EXPECT_TRUE(output.empty())
            << "成功监听尝试=" << attempt << "，捕获诊断=" << output;
        return bridge;
      }
      EXPECT_EQ(output, diagnostic_enabled
                            ? "{\"marker\":\"UR_BROWSER_BRIDGE_START_DIAGNOSTIC\","
                              "\"reason\":\"listen\"}\n"
                            : "")
          << "失败监听尝试=" << attempt;
    }
    return nullptr;
  }
  std::string original; // 测试结束原样恢复，永不打印开关之外环境。
  bool existed = false; // 区分原未配置与空值配置。
};

// 固定真实media资产应可启动；成功不能生成失败记录。
TEST_F(BridgeStartFixture, OriginalMediaAssetsStartWithoutFailureRecord) {
  switch_value("1");
  auto owner = listening();
  ASSERT_NE(owner, nullptr);
  EXPECT_NE(owner->port(), 0U);
}

// 真实首端口冲突后成功重试；先前固定listen诊断不得污染后次成功零诊断判据。
TEST_F(BridgeStartFixture, OccupiedAttemptThenSuccessHasSeparateDiagnostics) {
  switch_value(nullptr);
  auto occupied = listening();
  auto available = listening();
  ASSERT_NE(occupied, nullptr);
  ASSERT_NE(available, nullptr);
  const auto available_port = available->port();
  unsigned attempts = 0; // 必须恰好首次拒绝、第二次成功，准备竞态不能被后续重试掩盖。
  switch_value("1");
  auto owner = listening([&](unsigned attempt) {
    ++attempts;
    if (attempt == 0)
      return occupied->port();
    if (attempt == 1) {
      available->stop();
      return available_port;
    }
    return 40000 + std::random_device{}() % 20000;
  });
  ASSERT_NE(owner, nullptr);
  EXPECT_EQ(attempts, 2U);
  EXPECT_EQ(owner->port(), available_port);
  EXPECT_NE(occupied->port(), 0U);
}

// 保留原真实监听；第二桥不得偷偷换端口，也不得关闭第一个桥。
TEST_F(BridgeStartFixture, OccupiedOriginalPortHasFixedListenReason) {
  switch_value(nullptr);
  auto owner = listening();
  ASSERT_NE(owner, nullptr);
  switch_value("1");
  BrowserBridge blocked(owner->port(), std::string(64, 'b'), assets(),
                        "https://media.example.test");
  ::testing::internal::CaptureStderr();
  const auto started = blocked.start();
  const auto output = ::testing::internal::GetCapturedStderr();
  EXPECT_FALSE(started);
  EXPECT_EQ(blocked.port(), 0U);
  EXPECT_EQ(output, "{\"marker\":\"UR_BROWSER_BRIDGE_START_DIAGNOSTIC\","
                    "\"reason\":\"listen\"}\n");
  EXPECT_NE(owner->port(), 0U);
}

// 默认关闭及非法开关不能记录路径、令牌或系统错误；显式启用仅记录原校验分支。
TEST_F(BridgeStartFixture, ConfigurationFailureIsOptInAndRedacted) {
  for (const auto *flag : {"", "true", "01", "1"}) {
    switch_value(flag);
    BrowserBridge invalid(1, "PRIVATE_TOKEN",
                          {{"/PRIVATE_PATH", "PRIVATE_BODY"}});
    ::testing::internal::CaptureStderr();
    const auto started = invalid.start();
    const auto output = ::testing::internal::GetCapturedStderr();
    EXPECT_FALSE(started);
    EXPECT_EQ(output, std::string(flag) == "1"
                          ? "{\"marker\":\"UR_BROWSER_BRIDGE_START_"
                            "DIAGNOSTIC\",\"reason\":\"configuration\"}\n"
                          : "");
  }
}

// 固定分支均来自真实start校验；编译资产不满足前提时不进入网络初始化。
TEST_F(BridgeStartFixture, AssetRejectionReasonsStayClosed) {
  switch_value("1");
  for (const auto *reason : {"liveAssets", "mediaOrigin", "assetPath",
                             "assetPrerequisite", "assetSize", "assetsTotal"}) {
    auto value = assets();
    std::string origin = "https://media.example.test";
    const auto branch = std::string(reason);
    if (branch == "liveAssets")
      value["/live-host.js"] = "";
    if (branch == "mediaOrigin")
      origin = "PRIVATE_INVALID_ORIGIN";
    if (branch == "assetPath")
      value["/PRIVATE_PATH"] = "PRIVATE_BODY";
    if (branch == "assetPrerequisite")
      value["/record-host.js"] = "";
    if (branch == "assetSize")
      value["/app.js"] = std::string(262145, 'x');
    if (branch == "assetsTotal") {
      value["/record-source.js"] = "";
      for (const auto *path :
           {"/media/file.wav", "/media/file.mp3", "/media/file.mp4",
            "/media/file.webm", "/record-assets/archive.mp4"})
        value[path] = std::string(8388608, 'x');
    }
    BrowserBridge invalid(40000, std::string(64, 'a'), std::move(value),
                          origin);
    ::testing::internal::CaptureStderr();
    const auto started = invalid.start();
    const auto output = ::testing::internal::GetCapturedStderr();
    EXPECT_FALSE(started);
    EXPECT_EQ(
        output,
        "{\"marker\":\"UR_BROWSER_BRIDGE_START_DIAGNOSTIC\",\"reason\":\"" +
            branch + "\"}\n");
  }
}
} // namespace
} // namespace ur::device
