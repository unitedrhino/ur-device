// 文件说明：真实共享仲裁驱动与桌面会话准入回归；物理票据明确由测试端口确认，不冒充媒体E2E。
#include "browser_voice_focus.hpp"
#include <gtest/gtest.h>

namespace ur::device {
namespace {
// Gate 仅模拟独立物理确认；请求本身不产生实际完成事实。
struct Gate final : OutputGroupGatePort {
    std::uint64_t ticket = 0;                // 原始实际端口代次。
    bool confirmed = false, enabled = true;  // 用例明确控制原票据确认。
    std::optional<std::uint64_t> request_output_gate(bool value, std::uint64_t) override {
        enabled = value;
        confirmed = false;
        return ++ticket;
    }
    bool output_gate_confirmed(std::uint64_t value, bool state) const override {
        return confirmed && value == ticket && state == enabled;
    }
};
// request 构造原AI speaker请求；实际网络/正文解析由既有端点测试覆盖。
FocusCommand request(std::string operation = "acquire") {
    FocusCommand command;
    command.envelope.session = "ai";
    command.focus_id = "voice-focus";
    command.resources = {"speaker"};
    command.priority = "conversation";
    command.resume_policy = "explicit";
    command.operation = std::move(operation);
    if (command.operation == "acquire" || command.operation == "restore")
        command.expires_at = 1400;
    return command;
}
// Host 用例会话来自独立可撤销状态，不由被测请求选择当前会话。
struct Host {
    Gate gate;
    OutputFocusDriver driver{{{"", "", {"speaker"}, &gate}}, 0, FocusBindingMode::DeferredOnce};
    std::optional<std::string> session = "ai";
    BrowserVoiceFocusAuthority authority{driver, 1000, [this] { return session; }};
    // idle 必须先初始化并由独立端口明确确认原false。
    void idle() {
        ASSERT_TRUE(driver.initialize(10));
        gate.confirmed = true;
        ASSERT_TRUE(driver.poll(11));
    }
};
}  // namespace

TEST(BrowserVoiceFocus, RequiresPhysicalIdleAndFirstAcquireWithoutEnablingOutput) {
    Host host;
    EXPECT_FALSE(host.authority.authorize(request(), 12, 1000));
    host.idle();
    EXPECT_FALSE(host.authority.authorize(request("restore"), 12, 1000));
    EXPECT_FALSE(host.authority.authorize(request("release"), 12, 1000));
    const auto permit = host.authority.authorize(request(), 12, 1000);
    ASSERT_TRUE(permit);
    EXPECT_EQ(permit->deadline, 412U);
    EXPECT_TRUE(host.authority.valid(*permit, 13, 1001));
    EXPECT_FALSE(host.gate.enabled);
    EXPECT_EQ(host.gate.ticket, 1U);
    EXPECT_EQ(host.driver.revision(), 0U);
    EXPECT_TRUE(host.driver.initial_idle());
}

TEST(BrowserVoiceFocus, MissingProviderOrEstablishedSessionCannotAuthorize) {
    Host host;
    host.idle();
    BrowserVoiceFocusAuthority missing(host.driver, 1000, {});
    EXPECT_FALSE(missing.authorize(request(), 12, 1000));
    host.session.reset();
    EXPECT_FALSE(host.authority.authorize(request(), 12, 1000));
    host.session = "different-ai";
    EXPECT_FALSE(host.authority.authorize(request(), 12, 1000));
    EXPECT_TRUE(host.driver.initial_idle());
}

TEST(BrowserVoiceFocus, RejectsOtherOutputBusinessesAndUnsafeDeadlines) {
    Host host;
    host.idle();
    for (unsigned scenario = 0; scenario < 12; ++scenario) {
        auto command = request();
        switch (scenario) {
            case 0:
                command.resources = {"speaker", "display"};
                break;
            case 1:
                command.resources = {"display"};
                break;
            case 2:
                command.resources = {"speaker", "speaker"};
                break;
            case 3:
                command.priority = "emergency";
                break;
            case 4:
                command.resume_policy = "ifUnchanged";
                break;
            case 5:
                command.expires_at.reset();
                break;
            case 6:
                command.expires_at = 1000;
                break;
            case 7:
                command.expires_at = 2000;
                break;
            case 8:
                command.operation = "invalid";
                break;
            case 9:
                command.expected_revision = 1;
                break;
            case 10:
                command.focus_id.clear();
                break;
            case 11:
                command.envelope.session.clear();
                break;
        }
        EXPECT_FALSE(host.authority.authorize(command, 12, 1000)) << scenario;
    }
    EXPECT_FALSE(host.authority.authorize(request(), 1000, 1000));
    EXPECT_FALSE(host.authority.authorize(request(), 12, 0));
    EXPECT_TRUE(host.authority.authorize(request(), 12, 1000));
}

TEST(BrowserVoiceFocus, RevocationAndIdentityReplacementNeverRefreshOriginalPermit) {
    Host host;
    host.idle();
    const auto permit = host.authority.authorize(request(), 12, 1000);
    ASSERT_TRUE(permit);
    host.session.reset();
    EXPECT_FALSE(host.authority.valid(*permit, 13, 1001));
    host.session = "new-ai";
    auto replacement = request();
    replacement.envelope.session = "new-ai";
    EXPECT_FALSE(host.authority.authorize(replacement, 13, 1001));
    host.session = "ai";
    replacement = request();
    replacement.focus_id = "new-focus";
    EXPECT_FALSE(host.authority.authorize(replacement, 13, 1001));
    EXPECT_FALSE(host.authority.valid(*permit, 412, 1399));
    EXPECT_FALSE(host.authority.valid(*permit, 13, 1400));
    auto malformed = *permit;
    malformed.intent_version = 2;
    EXPECT_FALSE(host.authority.valid(malformed, 13, 1001));
    malformed = *permit;
    malformed.deadline = 1001;
    EXPECT_FALSE(host.authority.valid(malformed, 13, 1001));
    EXPECT_TRUE(host.authority.authorize(request("suspend"), 13, 1001));
    EXPECT_TRUE(host.authority.authorize(request("release"), 13, 1001));
    EXPECT_TRUE(host.authority.authorize(request("restore"), 13, 1001));
    EXPECT_EQ(host.gate.ticket, 1U);
}
}  // namespace ur::device
