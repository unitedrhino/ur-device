// 文件说明：节目完整组焦点许可单测；明确注入原业务/物理票据，不称真实媒体或硬件验证。
#include "browser_live_focus.hpp"
#include <gtest/gtest.h>

namespace ur::device {
namespace {
// Gate 模拟实际票据，仅验证原授权与路径选择，不替代浏览器真实关闭/开启事实。
struct Gate final : OutputGroupGatePort {
    std::uint64_t ticket = 0;  // 原模拟实际票据。
    bool enabled = false;      // 原明确目标，不是业务授权。
    std::optional<std::uint64_t> request_output_gate(bool value, std::uint64_t) override {
        enabled = value;
        return ++ticket;
    }
    bool output_gate_confirmed(std::uint64_t value, bool state) const override {
        return value == ticket && state == enabled;
    }
};
// command 构造原当前完整组；实际报文严格校验另由FocusWire与Endpoint保证。
FocusCommand command(const std::vector<std::string>& resources) {
    FocusCommand value;
    value.envelope.session = "programme";
    value.focus_id = "original-focus";
    value.resources = resources;
    value.priority = "background";
    value.resume_policy = "explicit";
    value.operation = "acquire";
    value.expires_at = 2000;
    return value;
}
}  // namespace
TEST(BrowserLiveFocusTest, CompleteOriginalGroupsRequireSourceEstablishedAndBoundedLease) {
    for (const auto& resources :
         std::vector<std::vector<std::string>>{{"speaker"}, {"display"}, {"speaker", "display"}}) {
        std::array<Gate, 4> gates;
        OutputFocusDriver driver({{"", "", {"speaker"}, &gates[0]},
                                  {"", "", {"speaker"}, &gates[1]},
                                  {"", "", {"display"}, &gates[2]},
                                  {"", "", {"speaker", "display"}, &gates[3]}},
                                 0, FocusBindingMode::DeferredReleased);
        ASSERT_TRUE(driver.initialize(10));
        ASSERT_TRUE(driver.poll(11));
        BrowserLiveFocusScope scope{
            "programme", resources.front() == "speaker", resources.back() == "display", 5000, true,
            false};
        BrowserLiveFocusAuthority authority(
            driver, 10000, [] { return std::optional<std::string>("ai"); },
            [&] { return std::optional<BrowserLiveFocusScope>(scope); });
        auto request = command(resources);
        EXPECT_FALSE(authority.authorize(request, 12, 1000));
        scope.established = true;
        auto permit = authority.authorize(request, 12, 1000);
        ASSERT_TRUE(permit);
        EXPECT_TRUE(authority.valid(*permit, 13, 1001));
        EXPECT_EQ(permit->deadline, 1012U);
        auto wrong = request;
        wrong.resources = {"camera"};
        EXPECT_FALSE(authority.authorize(wrong, 14, 1002));
        wrong = request;
        wrong.resume_policy = "automatic";
        EXPECT_FALSE(authority.authorize(wrong, 14, 1002));
        wrong = request;
        wrong.expires_at = 99999;
        EXPECT_FALSE(authority.authorize(wrong, 14, 1002));
        scope.authorized = false;
        EXPECT_FALSE(authority.valid(*permit, 15, 1003));
        scope.authorized = true;
        scope.session = "new-programme";
        EXPECT_FALSE(authority.valid(*permit, 16, 1004));
        scope.session = "programme";
        EXPECT_FALSE(authority.valid(*permit, 1012, 2000));
    }
}
TEST(BrowserLiveFocusTest, ForegroundAiUsesOnlyExistingVoiceRuleAndNoCapturePermission) {
    std::array<Gate, 4> gates;
    OutputFocusDriver driver({{"", "", {"speaker"}, &gates[0]},
                              {"", "", {"speaker"}, &gates[1]},
                              {"", "", {"display"}, &gates[2]},
                              {"", "", {"speaker", "display"}, &gates[3]}},
                             0, FocusBindingMode::DeferredReleased);
    ASSERT_TRUE(driver.initialize(10));
    ASSERT_TRUE(driver.poll(11));
    BrowserLiveFocusAuthority authority(
        driver, 10000, [] { return std::optional<std::string>("ai"); },
        [] { return std::optional<BrowserLiveFocusScope>{}; });
    auto request = command({"speaker"});
    request.priority = "conversation";
    request.envelope.session = "ai";
    ASSERT_TRUE(authority.authorize(request, 12, 1000));
    request.resources = {"speaker", "display"};
    EXPECT_FALSE(authority.authorize(request, 13, 1001));
    request.resources = {"microphone"};
    EXPECT_FALSE(authority.authorize(request, 14, 1002));
}
TEST(BrowserLiveFocusTest, ReplacementFocusCannotBorrowAnUnreleasedOriginalPath) {
    std::array<Gate, 4> gates;
    OutputFocusDriver driver({{"", "", {"speaker"}, &gates[0]},
                              {"", "", {"speaker"}, &gates[1]},
                              {"", "", {"display"}, &gates[2]},
                              {"", "", {"speaker", "display"}, &gates[3]}},
                             0, FocusBindingMode::DeferredReleased);
    ASSERT_TRUE(driver.initialize(10));
    ASSERT_TRUE(driver.poll(11));
    BrowserLiveFocusScope scope{"programme", true, false, 5000, true, true};
    BrowserLiveFocusAuthority authority(
        driver, 10000, [] { return std::optional<std::string>("ai"); },
        [&] { return std::optional<BrowserLiveFocusScope>(scope); });
    auto request = command({"speaker"});
    ASSERT_TRUE(authority.authorize(request, 12, 1000));
    request.focus_id = "different-focus";
    EXPECT_FALSE(authority.authorize(request, 13, 1001));
    scope.session = "new-programme";
    request.envelope.session = scope.session;
    EXPECT_FALSE(authority.authorize(request, 14, 1002));
}
}  // namespace ur::device
