// 文件说明：正式复合焦点授权器回归；物理确认及已建立业务由用例明确注入，不冒称E2E。
#include "browser_media_focus.hpp"
#include <gtest/gtest.h>

namespace ur::device {
namespace {
// Port 明确模拟原端口确认，用于隔离准入规则；不是硬件或浏览器事实。
struct Port final : OutputGroupGatePort {
    std::uint64_t ticket = 0;  // 原模拟端口代次。
    bool enabled = false;      // 原模拟实际目标。
    std::optional<std::uint64_t> request_output_gate(bool value, std::uint64_t) override {
        enabled = value;
        return ++ticket;
    }
    bool output_gate_confirmed(std::uint64_t value, bool state) const override {
        return value == ticket && state == enabled;
    }
};

// 每个输出组独立构造原目录、实际媒体实例和唯一全局驱动，禁止跨用例残留许可。
TEST(BrowserMediaFocus, FileRequiresEstablishedOriginalCompleteGroupAndFixedExpiry) {
    for (const auto& group :
         std::vector<std::vector<std::string>>{{"speaker"}, {"display"}, {"speaker", "display"}}) {
        const bool audio = group.front() == "speaker";
        const bool video = group.back() == "display";
        BrowserFileBinding binding{"child",  "player",
                                   "source", video ? "/media/file.mp4" : "/media/file.wav",
                                   "",       video ? "video/mp4" : "audio/wav",
                                   audio,    5000,
                                   1024};
        BrowserResolvedPlaybackAdmission files(
            [binding, video](const PlaybackInvite&, std::uint64_t) {
                return std::optional<BrowserFileResolution>(
                    {binding, [audio = binding.audio, video](bool a, bool v, std::uint64_t now) {
                         return a == audio && v == video && now < 5000;
                     }});
            });
        PlaybackInvite invite;
        invite.envelope.session = "child";
        invite.playback_id = "player";
        invite.content_ref = "source";
        if (audio)
            invite.output_modalities.push_back("audio");
        if (video)
            invite.output_modalities.push_back("video");
        ASSERT_TRUE(files.authorize(invite, 10).pending);
        ASSERT_TRUE(files.active_media()->take_command());
        ASSERT_TRUE(files.active_media()->receive(
            R"({"kind":"fileReady","sessionId":"child","playbackId":"player","success":true})"));
        auto grant = files.authorize(invite, 11);
        ASSERT_TRUE(grant.driver);
        std::array<Port, 4> ports;
        OutputFocusDriver driver({{"", "", {"speaker"}, &ports[0]},
                                  {"", "", {"speaker"}, &ports[1]},
                                  {"", "", {"display"}, &ports[2]},
                                  {"", "", {"speaker", "display"}, &ports[3]}},
                                 0, FocusBindingMode::DeferredOnce);
        ASSERT_TRUE(driver.initialize(10));
        ASSERT_TRUE(driver.poll(11));
        bool established = false;
        BrowserMediaFocusAuthority authority(
            driver, files, 10000, [] { return std::optional<std::string>("ai-parent"); },
            [&](std::string_view id) { return established && id == "child"; });
        FocusCommand command;
        command.envelope.session = "child";
        command.focus_id = "file-focus";
        command.resources = group;
        command.priority = "background";
        command.resume_policy = "explicit";
        command.operation = "acquire";
        command.expires_at = 2000;
        EXPECT_FALSE(authority.authorize(command, 12, 1000));
        established = true;
        auto wrong = command;
        wrong.resources = {"microphone"};
        EXPECT_FALSE(authority.authorize(wrong, 12, 1000));
        wrong = command;
        wrong.resume_policy = "automatic";
        EXPECT_FALSE(authority.authorize(wrong, 12, 1000));
        wrong = command;
        wrong.expires_at = 99999;
        EXPECT_FALSE(authority.authorize(wrong, 12, 1000));
        auto permit = authority.authorize(command, 12, 1000);
        ASSERT_TRUE(permit);
        EXPECT_EQ(permit->deadline, 1012U);
        EXPECT_TRUE(authority.valid(*permit, 13, 1001));
        wrong = command;
        wrong.focus_id = "replacement";
        EXPECT_FALSE(authority.authorize(wrong, 14, 1002));
        established = false;
        EXPECT_FALSE(authority.valid(*permit, 15, 1003));
        established = true;
        EXPECT_FALSE(authority.valid(*permit, 1012, 2000));
        wrong = command;
        wrong.envelope.session = "ai-parent";
        wrong.priority = "conversation";
        wrong.resources = {"speaker"};
        EXPECT_TRUE(authority.authorize(wrong, 16, 1004));  // AI仍只绑定路径0，文件许可不提升范围。
    }
}
// 两个独立节目复用同一真实端口配置；媒体释放、焦点release和建立事实缺一不可。
TEST(BrowserMediaFocus, NextProgramRequiresConsumedReleaseAndActualPlayerClose) {
    BrowserResolvedPlaybackAdmission files([](const PlaybackInvite& invite, std::uint64_t) {
        BrowserFileBinding binding{invite.envelope.session,
                                   "player",
                                   "source",
                                   "/media/file.wav",
                                   "",
                                   "audio/wav",
                                   true,
                                   5000,
                                   1024};
        return std::optional<BrowserFileResolution>(
            {binding, [](bool audio, bool video, std::uint64_t now) {
                 return audio && !video && now < 5000;
             }});
    });
    std::array<Port, 4> ports;
    OutputFocusDriver driver({{"", "", {"speaker"}, &ports[0]},
                              {"", "", {"speaker"}, &ports[1]},
                              {"", "", {"display"}, &ports[2]},
                              {"", "", {"speaker", "display"}, &ports[3]}},
                             0, FocusBindingMode::DeferredReleased);
    ASSERT_TRUE(driver.initialize(10));
    ASSERT_TRUE(driver.poll(11));
    BrowserMediaFocusAuthority authority(
        driver, files, 10000, [] { return std::optional<std::string>("ai"); },
        [](std::string_view id) { return id == "first" || id == "second"; });
    // 准备和建立使用明确夹具事实，不将此单测称为浏览器或后端E2E。
    auto prepare = [&](const std::string& id, std::uint64_t now) {
        PlaybackInvite invite;
        invite.envelope.session = id;
        invite.playback_id = "player";
        invite.content_ref = "source";
        invite.output_modalities = {"audio"};
        EXPECT_TRUE(files.authorize(invite, now).pending);
        EXPECT_TRUE(files.active_media()->take_command());
        EXPECT_TRUE(
            files.active_media()->receive("{\"kind\":\"fileReady\",\"sessionId\":\"" + id +
                                          "\",\"playbackId\":\"player\",\"success\":true}"));
        EXPECT_TRUE(files.authorize(invite, now + 1).driver);
    };
    auto request = [](const std::string& id, const std::string& operation, unsigned revision) {
        FocusCommand command;
        const auto body = "{\"msgToken\":\"" + id + operation +
                          "\",\"method\":\"focusControl\",\"sessionId\":\"" + id +
                          "\",\"eventSeq\":1,\"params\":{\"focusId\":\"f\",\"operation\":\"" +
                          operation + "\",\"expectedRevision\":" + std::to_string(revision) +
                          ",\"resources\":[\"speaker\"],\"priority\":\"background\","
                          "\"resumePolicy\":\"explicit\"" +
                          (operation == "acquire" ? ",\"expiresAt\":2000" : "") + "}}";
        EXPECT_EQ(decode_focus_control(WireRoute::Signaling, body, command), WireResult::Ok);
        return command;
    };
    prepare("first", 12);
    auto first = request("first", "acquire", 0);
    auto permit = authority.authorize(first, 14, 1000);
    ASSERT_TRUE(permit);
    ASSERT_TRUE(driver.begin(first, 1));
    ASSERT_TRUE(driver.poll(15));
    ASSERT_TRUE(driver.take_fact("first", "f"));
    auto release = request("first", "release", 1);
    ASSERT_TRUE(authority.authorize(release, 16, 1002));
    ASSERT_TRUE(driver.begin(release, 2));
    ASSERT_TRUE(driver.poll(17));
    ASSERT_TRUE(driver.take_fact("first", "f"));
    EXPECT_FALSE(files.cancel("first", 18));
    ASSERT_TRUE(files.active_media()->take_command());
    ASSERT_TRUE(files.active_media()->receive(
        R"({"kind":"fileClosed","sessionId":"first","playbackId":"player","generation":"0"})"));
    ASSERT_TRUE(files.cancel("first", 19));
    prepare("second", 20);
    auto second = request("second", "acquire", 2);
    ASSERT_TRUE(authority.authorize(second, 22, 1008));
    EXPECT_FALSE(authority.valid(*permit, 22, 1008));
    EXPECT_FALSE(authority.authorize(first, 22, 1008));
    ASSERT_TRUE(driver.begin(second, 3));
    ASSERT_TRUE(driver.poll(23));
    EXPECT_EQ(driver.revision(), 3U);
    EXPECT_TRUE(driver.owns(second));
}
}  // namespace
}  // namespace ur::device
