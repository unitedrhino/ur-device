// 桌面文件驱动契约测试：验证真实事实门槛、源约束和独立关闭；不冒充浏览器实际解码验收。
#include "browser_file_media.hpp"
#include <gtest/gtest.h>
#include <limits>
#include "browser_bridge.hpp"
#include "browser_playback_admission.hpp"

namespace ur::device {
// BrowserFileStaticFaultTest 精确删除已观察时钟并调用原私有编码出口，不通过 decoder 模拟故障。
class BrowserFileStaticFaultTest {
public:
    // load_without_clock 只清除时钟前置，不补造媒体准备或授权事实。
    static std::string load_without_clock(BrowserFileMedia& driver, const PlaybackCommand& load) {
        driver.clock_.reset();
        return driver.command("fileControl", 1, &load);
    }
};
namespace {
// binding 为离线用例提供固定音乐源，不包含实际凭据或自由下载地址。
BrowserFileBinding binding() {
    return {"media-session", "player-1", "content-1", "/media/file.wav", "", "audio/wav", true,
            10000,           1024};
}
// control 使用公共严格解码器产生驱动输入，避免测试绕过设备协议约束。
PlaybackCommand control(const std::string& operation, const std::string& extra = "") {
    PlaybackCommand result;
    const auto json =
        "{\"msgToken\":\"cmd-1\",\"method\":\"playbackControl\",\"sessionId\":\"media-session\","
        "\"eventSeq\":1,\"params\":{\"expectedRevision\":0,\"playbackId\":\"player-1\","
        "\"operation\":\"" +
        operation + "\"" + extra + "}}";
    EXPECT_EQ(decode_playback_control(WireRoute::Signaling, json, result), WireResult::Ok);
    return result;
}
// fact 模拟已鉴权桥报告的实际测量值；音频和视频证据字段仍必须经过严格校验。
std::string fact(std::uint64_t generation, const std::string& state, bool spontaneous = false,
                 std::uint64_t samples = 256, std::uint64_t frames = 0) {
    return "{\"kind\":\"fileFact\",\"sessionId\":\"media-session\",\"playbackId\":\"player-1\","
           "\"generation\":\"" +
           std::to_string(generation) + "\",\"spontaneous\":" + (spontaneous ? "true" : "false") +
           ",\"code\":200,\"state\":\"" + state +
           "\",\"positionMs\":100,\"durationMs\":1000,"
           "\"rate\":1,\"audioSamples\":" +
           std::to_string(samples) + ",\"videoFrames\":" + std::to_string(frames) + "}";
}
// ready 回执代表组件初始化，不代表内容已加载或开始播放。
constexpr auto ready =
    R"({"kind":"fileReady","sessionId":"media-session","playbackId":"player-1","success":true})";
// initialize 消费唯一绑定命令后设置可控时钟，不启动网络或媒体。
void initialize(BrowserFileMedia& driver) {
    ASSERT_TRUE(driver.take_command());
    ASSERT_TRUE(driver.tick(1));
    ASSERT_TRUE(driver.receive(ready));
}
// load 完成一次实际加载事实；后续播放仍必须重新检查输出租约。
void load(BrowserFileMedia& driver) {
    ASSERT_TRUE(driver.begin(control("load", ",\"contentRef\":\"content-1\""), 1));
    ASSERT_TRUE(driver.take_command());
    ASSERT_TRUE(driver.receive(fact(1, "loaded")));
    ASSERT_TRUE(driver.take_fact());
}

TEST(BrowserFileOrigin, CanonicalHttpsOnly) {
    for (const auto* origin :
         {"https://media.example.com", "https://a.io", "https://media.example.com:8443"})
        EXPECT_TRUE(valid_browser_media_origin(origin)) << origin;
    for (const auto* origin :
         {"http://media.example.com", "https://127.0.0.1", "https://localhost",
          "https://camera.local", "https://camera.localhost", "https://user@media.example.com",
          "https://media.example.com/", "https://media.example.com:443",
          "https://media.example.com:01", "https://media.example.com:65536",
          "https://Media.example.com", "https://media..com", "https://-media.example.com",
          "https://media.example.com; script-src *", "https://[::1]", "https://media.example.com.",
          "https://a.io:", "https://a.io:8x", "https://a.io:0"})
        EXPECT_FALSE(valid_browser_media_origin(origin)) << origin;
}
// 缺失时钟的私有编码出口不生成 load；不把公开 begin 的既有校验冒作故障证据。
TEST(BrowserFileStaticFault, MissingClockCannotEncodeLoad) {
    BrowserFileMedia driver(binding(),
                            [](bool audio, bool video, std::uint64_t) { return audio && !video; });
    initialize(driver);
    const auto load_command = control("load", ",\"contentRef\":\"content-1\"");
    EXPECT_TRUE(BrowserFileStaticFaultTest::load_without_clock(driver, load_command).empty());
    EXPECT_FALSE(driver.take_command());
    EXPECT_FALSE(driver.take_fact());
}

TEST(BrowserFileMedia, BindAndLoadDoNotAutomaticallyPlay) {
    BrowserFileMedia driver(binding(),
                            [](bool audio, bool video, std::uint64_t) { return audio && !video; });
    EXPECT_FALSE(driver.ready());
    EXPECT_FALSE(driver.output_permitted());
    initialize(driver);
    EXPECT_TRUE(driver.ready());
    load(driver);
    EXPECT_FALSE(driver.take_command());
}

// gated 模拟独立完整路径实际确认，仅供驱动边界单测，不代替真实音频/绘制。
std::string gated(std::string ticket, bool enabled) {
    return R"({"kind":"fileOutputGated","sessionId":"media-session","playbackId":"player-1","gateTicket":")" +
           ticket + R"(","enabled":)" + (enabled ? "true}" : "false}");
}
TEST(BrowserFileMedia, GroupGateRequiresDeliveredOriginalFactAndIndependentPermission) {
    bool permit = true;
    BrowserFileMedia driver(binding(), [&](bool, bool, std::uint64_t) { return permit; });
    initialize(driver);
    load(driver);
    const auto ticket = driver.request_output_gate(false, 2);
    ASSERT_TRUE(ticket);
    EXPECT_FALSE(driver.output_gate_confirmed(*ticket, false));
    EXPECT_FALSE(driver.receive(gated("1", false)));  // 尚未交付，不能伪造确认。
    EXPECT_FALSE(driver.request_output_gate(true, 2));
    ASSERT_TRUE(driver.take_command());
    EXPECT_FALSE(driver.receive(gated("2", false)));
    EXPECT_FALSE(driver.receive(gated("1", true)));
    ASSERT_TRUE(driver.receive(gated("1", false)));
    EXPECT_TRUE(driver.output_gate_confirmed(*ticket, false));
    EXPECT_TRUE(driver.output_permitted());  // 物理限制不自造/修改业务授权。
    EXPECT_FALSE(driver.begin(control("play"), 2));
    permit = false;
    EXPECT_FALSE(driver.request_output_gate(true, 3));
    EXPECT_FALSE(driver.output_permitted());
    permit = true;
    const auto restored = driver.request_output_gate(true, 3);
    ASSERT_TRUE(restored);
    ASSERT_TRUE(driver.take_command());
    ASSERT_TRUE(driver.receive(gated("1", false)));  // 迟到原票据不能回退新门控。
    EXPECT_FALSE(driver.output_gate_confirmed(*restored, true));
    ASSERT_TRUE(driver.receive(gated("2", true)));
    EXPECT_TRUE(driver.output_gate_confirmed(*restored, true));
    EXPECT_FALSE(driver.output_gate_confirmed(*ticket, false));
    driver.stop_all();
    EXPECT_FALSE(driver.output_gate_confirmed(*restored, true));
}

TEST(BrowserFileMedia, GroupGateTimeoutAndMalformedFactsFailClosed) {
    for (bool backwards : {false, true}) {
        BrowserFileMedia driver(binding(), [](bool, bool, std::uint64_t) { return true; });
        initialize(driver);
        ASSERT_TRUE(driver.request_output_gate(false, 2));
        ASSERT_TRUE(driver.take_command());
        for (const auto ticket : {"0", "01", "1\\u0000", "1x", "18446744073709551616"})
            EXPECT_FALSE(driver.receive(gated(ticket, false)));
        EXPECT_FALSE(driver.tick(backwards ? 1 : 2002));
        EXPECT_FALSE(driver.output_gate_confirmed(1, false));
        EXPECT_FALSE(driver.request_output_gate(true, 2003));
        EXPECT_FALSE(driver.quiescent());  // 逻辑失败不是实际释放。
    }
}

// 固定内容与实际初始化缺一不可；错误邀请不消费准入，原授权只交付一次。
TEST(BrowserPlaybackAdmission, FixedReadyBindingAndSingleGrant) {
    BrowserPlaybackAdmission admission(
        binding(), [](bool audio, bool video, std::uint64_t) { return audio && !video; });
    PlaybackInvite invite;
    invite.envelope.session = "media-session";
    invite.playback_id = "player-1";
    invite.content_ref = "content-1";
    invite.output_modalities = {"audio"};
    EXPECT_FALSE(admission.authorize(invite, 1).driver);
    initialize(admission.media());
    auto wrong = invite;
    wrong.content_ref = "model-url";
    EXPECT_FALSE(admission.authorize(wrong, 2).driver);
    wrong = invite;
    wrong.envelope.session = "ai-parent";
    EXPECT_FALSE(admission.authorize(wrong, 2).driver);
    auto grant = admission.authorize(invite, 3);
    ASSERT_TRUE(grant.driver);
    EXPECT_EQ(grant.deadline, binding().expires_at);
    EXPECT_TRUE(grant.driver->output_permitted());
    EXPECT_FALSE(admission.authorize(invite, 4).driver);
    EXPECT_FALSE(admission.media().take_command());  // 准入不隐含load/play。
    EXPECT_FALSE(grant.driver->stop_all());          // 请求受理不是实际释放。
    ASSERT_TRUE(admission.media().take_command());
    EXPECT_TRUE(admission.media().receive(
        R"({"kind":"fileClosed","sessionId":"media-session","playbackId":"player-1","generation":"0"})"));
    EXPECT_TRUE(grant.driver->stop_all());
}

// 完整输出及当前许可独立核验，不以视频驱动授予未声明音轨或过期内容。
TEST(BrowserPlaybackAdmission, OutputsPermissionAndExpiry) {
    for (unsigned variant = 0; variant < 4; ++variant) {
        bool permit = true;
        BrowserPlaybackAdmission admission(binding(),
                                           [&permit](bool, bool, std::uint64_t) { return permit; });
        initialize(admission.media());
        PlaybackInvite invite;
        invite.envelope.session = "media-session";
        invite.playback_id = "player-1";
        invite.content_ref = "content-1";
        invite.output_modalities = variant == 0 ? std::vector<std::string>{"audio", "video"}
                                                : std::vector<std::string>{"audio"};
        if (variant == 1)
            invite.output_modalities = {"audio", "audio"};
        if (variant == 2)
            permit = false;
        EXPECT_FALSE(admission.authorize(invite, variant == 3 ? 10000 : 2).driver);
    }
}

// PreparationPublisher 只捕获核心原上行，不把本地publish当平台接纳或实际媒体开始。
struct PreparationPublisher final : PublishPort {
    std::vector<std::string> sent;  // 按实际核心发布顺序记录。
    bool publish(std::string_view, std::string_view body) override {
        sent.emplace_back(body);
        return true;
    }
};

// 原邀请先于fileReady到达也不拒接；原准备就绪后接听，无预先构造成功会话或自动播放。
TEST(BrowserPlaybackAdmission, EndpointWaitsForActualPreparation) {
    PreparationPublisher mqtt;
    BrowserPlaybackAdmission admission(
        binding(), [](bool audio, bool video, std::uint64_t) { return audio && !video; });
    PlaybackEndpoint endpoint(mqtt, admission, {1, true, false}, "up", "down");
    const auto invitation =
        R"({"msgToken":"invite","method":"sessionInvite","sessionId":"media-session","eventSeq":1,"params":{"purpose":"mediaPlayback","admission":"direct","transport":"none","tracks":[],"expiresAt":5000,"playbackId":"player-1","mediaSource":{"contentRef":"content-1"},"inputModalities":[],"outputModalities":["audio"]}})";
    ASSERT_TRUE(endpoint.receive("down", invitation, 1, 1));
    EXPECT_TRUE(mqtt.sent.empty());
    ASSERT_TRUE(endpoint.receive("down", invitation, 2, 2));
    EXPECT_TRUE(mqtt.sent.empty());
    initialize(admission.media());
    endpoint.tick(3, 3);
    ASSERT_EQ(mqtt.sent.size(), 1U);
    EXPECT_NE(mqtt.sent.back().find("sessionAccept"), std::string::npos);
    EXPECT_FALSE(admission.media().take_command());
    endpoint.close(4);
    EXPECT_FALSE(admission.media().quiescent());
    ASSERT_TRUE(admission.media().take_command());
    ASSERT_TRUE(admission.media().receive(
        R"({"kind":"fileClosed","sessionId":"media-session","playbackId":"player-1","generation":"0"})"));
    endpoint.tick(5, 5);
    EXPECT_TRUE(admission.media().quiescent());
}

// 超时准备关闭须等待实际fileClosed；迟到fileReady不能令原拒绝变成新的接听。
TEST(BrowserPlaybackAdmission, ExpiredPreparationDoesNotAcceptLateReady) {
    PreparationPublisher mqtt;
    BrowserPlaybackAdmission admission(
        binding(), [](bool audio, bool video, std::uint64_t) { return audio && !video; });
    PlaybackEndpoint endpoint(mqtt, admission, {1, true, false}, "up", "down");
    const auto invitation =
        R"({"msgToken":"invite","method":"sessionInvite","sessionId":"media-session","eventSeq":1,"params":{"purpose":"mediaPlayback","admission":"direct","transport":"none","tracks":[],"expiresAt":5000,"playbackId":"player-1","mediaSource":{"contentRef":"content-1"},"inputModalities":[],"outputModalities":["audio"]}})";
    ASSERT_TRUE(endpoint.receive("down", invitation, 1, 1));
    ASSERT_TRUE(admission.media().take_command());
    endpoint.tick(5000, 5000);
    ASSERT_FALSE(mqtt.sent.empty());
    EXPECT_NE(mqtt.sent.back().find("sessionReject"), std::string::npos);
    EXPECT_FALSE(admission.media().quiescent());
    ASSERT_TRUE(admission.media().take_command());
    EXPECT_TRUE(admission.media().receive(ready));
    endpoint.tick(5001, 5001);
    for (const auto& body : mqtt.sent)
        EXPECT_EQ(body.find("sessionAccept"), std::string::npos);
}

// file_invite 为动态适配提供已严格解码的原平台邀请，不绕过字段或输出校验。
PlaybackInvite file_invite(const std::string& session = "dynamic-session") {
    PlaybackInvite invite;
    const auto body =
        "{\"msgToken\":\"dynamic-invite\",\"method\":\"sessionInvite\","
        "\"sessionId\":\"" +
        session +
        "\",\"eventSeq\":1,\"params\":{\"purpose\":\"mediaPlayback\","
        "\"admission\":\"direct\",\"transport\":\"none\",\"tracks\":[],\"expiresAt\":5000,"
        "\"playbackId\":\"dynamic-player\",\"mediaSource\":{\"contentRef\":\"content-1\"},"
        "\"inputModalities\":[],\"outputModalities\":[\"audio\"]}}";
    EXPECT_EQ(decode_playback_invite(WireRoute::Signaling, body, invite), WireResult::Ok);
    return invite;
}

// resolved 为测试明确授权固定本地节目；只采用平台关联ID，不从邀请取得URL或内容截止。
BrowserFileResolution resolved(const PlaybackInvite& invite) {
    auto source = binding();
    source.session = invite.envelope.session;
    source.playback_id = invite.playback_id;
    return {source, [](bool audio, bool video, std::uint64_t) { return audio && !video; }};
}

// ready_dynamic 消费唯一绑定并提供匹配的实际初始化事实，不表示load/play已执行。
void ready_dynamic(BrowserFileMedia& media, const PlaybackInvite& invite, std::uint64_t now) {
    ASSERT_TRUE(media.take_command());
    ASSERT_TRUE(media.tick(now));
    ASSERT_TRUE(media.receive("{\"kind\":\"fileReady\",\"sessionId\":\"" + invite.envelope.session +
                              "\",\"playbackId\":\"" + invite.playback_id +
                              "\",\"success\":true}"));
}

// close_dynamic 只回报已实际关闭的固定准备实例；适配本身不生成此事实。
void close_dynamic(BrowserFileMedia& media, const PlaybackInvite& invite) {
    ASSERT_TRUE(media.take_command());
    ASSERT_TRUE(media.receive("{\"kind\":\"fileClosed\",\"sessionId\":\"" +
                              invite.envelope.session + "\",\"playbackId\":\"" +
                              invite.playback_id + "\",\"generation\":\"0\"}"));
}

// 动态邀请首次解析一次；原准备轮询不续独立内容许可，也不会预先load/play或分配第二驱动。
TEST(BrowserResolvedPlaybackAdmission, LazyOriginalPreparationDoesNotResolveAgain) {
    unsigned calls = 0;
    BrowserResolvedPlaybackAdmission admission(
        [&calls](const PlaybackInvite& invite, std::uint64_t) {
            ++calls;
            return std::optional<BrowserFileResolution>(resolved(invite));
        });
    EXPECT_EQ(admission.active_media(), nullptr);
    const auto invite = file_invite();
    auto pending = admission.authorize(invite, 1);
    EXPECT_TRUE(pending.pending);
    EXPECT_FALSE(pending.driver);
    EXPECT_EQ(pending.deadline, binding().expires_at);
    ASSERT_NE(admission.active_media(), nullptr);
    auto* original = admission.active_media();
    pending = admission.authorize(invite, 2);
    EXPECT_TRUE(pending.pending);
    EXPECT_EQ(pending.deadline, binding().expires_at);
    EXPECT_EQ(calls, 1U);
    EXPECT_EQ(admission.active_media(), original);
    ready_dynamic(*original, invite, 3);
    auto grant = admission.authorize(invite, 4);
    ASSERT_TRUE(grant.driver);
    EXPECT_FALSE(grant.pending);
    EXPECT_TRUE(grant.driver->output_permitted());
    EXPECT_FALSE(original->take_command());
    EXPECT_FALSE(admission.authorize(invite, 5).driver);
    EXPECT_EQ(calls, 1U);
}

// 源解析必须返回原关联及完整输出；无许可/非法源/过期结果均不创建可泵入的实例。
TEST(BrowserResolvedPlaybackAdmission, RefusedResolutionCannotCreateBrowserWork) {
    for (unsigned variant = 0; variant < 10; ++variant) {
        BrowserResolvedPlaybackAdmission admission(
            [variant](const PlaybackInvite& invite, std::uint64_t now) {
                if (variant == 9)
                    return std::optional<
                        BrowserFileResolution>{};  // 明确内容目录拒绝，不创建默认节目。
                auto value = resolved(invite);
                if (variant == 0)
                    value.binding.session = "ai-parent";
                if (variant == 1)
                    value.binding.playback_id = "another-player";
                if (variant == 2)
                    value.binding.content_ref = "another-content";
                if (variant == 3)
                    value.binding.expires_at = now;
                if (variant == 4)
                    value.permission = {};
                if (variant == 5)
                    value.permission = [](bool, bool, std::uint64_t) { return false; };
                if (variant == 6)
                    value.binding.url = "/credentials.json";
                if (variant == 7)
                    value.binding.mime = "video/mp4";
                if (variant == 8)
                    value.binding.max_bytes = 67108865;
                return std::optional<BrowserFileResolution>(std::move(value));
            });
        const auto result = admission.authorize(file_invite(), 1);
        EXPECT_FALSE(result.pending) << variant;
        EXPECT_FALSE(result.driver) << variant;
        EXPECT_EQ(admission.active_media(), nullptr) << variant;
    }
    BrowserResolvedPlaybackAdmission missing({});
    EXPECT_FALSE(missing.authorize(file_invite(), 1).pending);
    EXPECT_EQ(missing.active_media(), nullptr);
    EXPECT_TRUE(missing.cancel("unknown", 2));
}

// 第二个邀请必须等待实际关闭；交出的原租约在替换后仍指向已关闭驱动，不串到新节目。
TEST(BrowserResolvedPlaybackAdmission, NextInstanceRequiresActualRelease) {
    unsigned calls = 0;
    BrowserResolvedPlaybackAdmission admission(
        [&calls](const PlaybackInvite& invite, std::uint64_t) {
            ++calls;
            return std::optional<BrowserFileResolution>(resolved(invite));
        });
    const auto first = file_invite();
    const auto second = file_invite("another-session");
    ASSERT_TRUE(admission.authorize(first, 1).pending);
    ready_dynamic(*admission.active_media(), first, 2);
    auto old = admission.authorize(first, 3);
    ASSERT_TRUE(old.driver);
    EXPECT_FALSE(admission.authorize(second, 4).pending);
    EXPECT_EQ(calls, 1U);
    EXPECT_TRUE(admission.cancel("unknown", 5));
    EXPECT_FALSE(admission.active_media()->take_command());
    EXPECT_FALSE(old.driver->stop_all());
    EXPECT_FALSE(admission.authorize(second, 6).pending);
    EXPECT_EQ(calls, 1U);
    close_dynamic(*admission.active_media(), first);
    ASSERT_TRUE(old.driver->stop_all());
    ASSERT_TRUE(admission.authorize(second, 7).pending);
    EXPECT_EQ(calls, 2U);
    EXPECT_TRUE(old.driver->stop_all());
    EXPECT_FALSE(old.driver->output_permitted());
    ready_dynamic(*admission.active_media(), second, 8);
    auto current = admission.authorize(second, 9);
    ASSERT_TRUE(current.driver);
    EXPECT_TRUE(current.driver->output_permitted());
    EXPECT_FALSE(admission.active_media()->receive(ready));  // 旧实例事实不进入新实例。
}

// 时钟回退关闭原准备并永久栅栏；真实关闭后也不能由新邀请重置时钟并恢复输出。
TEST(BrowserResolvedPlaybackAdmission, ClockRollbackCannotRebindAfterRelease) {
    unsigned calls = 0;
    BrowserResolvedPlaybackAdmission admission(
        [&calls](const PlaybackInvite& invite, std::uint64_t) {
            ++calls;
            return std::optional<BrowserFileResolution>(resolved(invite));
        });
    const auto first = file_invite();
    ASSERT_TRUE(admission.authorize(first, 10).pending);
    ASSERT_TRUE(admission.active_media()->take_command());
    EXPECT_FALSE(admission.authorize(file_invite("another-session"), 9).pending);
    EXPECT_EQ(calls, 1U);
    EXPECT_FALSE(admission.cancel(first.envelope.session, 11));
    close_dynamic(*admission.active_media(), first);
    EXPECT_TRUE(admission.cancel(first.envelope.session, 12));
    EXPECT_FALSE(admission.authorize(file_invite("another-session"), 13).pending);
    EXPECT_EQ(calls, 1U);
}

// 原准备期间权限被撤销不能重新解析或换取新期限，仍保留原实例等实际释放。
TEST(BrowserResolvedPlaybackAdmission, RevocationDoesNotRefreshResolution) {
    bool permitted = true;
    unsigned calls = 0;
    BrowserResolvedPlaybackAdmission admission([&](const PlaybackInvite& invite, std::uint64_t) {
        ++calls;
        auto value = resolved(invite);
        value.permission = [&permitted](bool, bool, std::uint64_t) { return permitted; };
        return std::optional<BrowserFileResolution>(std::move(value));
    });
    const auto invite = file_invite();
    ASSERT_TRUE(admission.authorize(invite, 1).pending);
    ASSERT_TRUE(admission.active_media()->take_command());
    permitted = false;
    EXPECT_FALSE(admission.authorize(invite, 2).pending);
    EXPECT_EQ(calls, 1U);
    EXPECT_FALSE(admission.cancel(invite.envelope.session, 3));
    close_dynamic(*admission.active_media(), invite);
    EXPECT_TRUE(admission.cancel(invite.envelope.session, 4));
}

// 共享端点仍负责邀请幂等/接听，动态解析只准备一次；能力和平台原子终态协议不变。
TEST(BrowserResolvedPlaybackAdmission, EndpointUsesDynamicIdentityAndFixedDeadline) {
    unsigned calls = 0;
    PreparationPublisher mqtt;
    BrowserResolvedPlaybackAdmission admission(
        [&calls](const PlaybackInvite& invite, std::uint64_t) {
            ++calls;
            return std::optional<BrowserFileResolution>(resolved(invite));
        });
    PlaybackEndpoint endpoint(mqtt, admission, {1, true, false}, "up", "down");
    const auto invite = file_invite();
    std::string invitation;
    ASSERT_EQ(encode_envelope(invite.envelope, invitation), DecodeResult::Ok);
    ASSERT_TRUE(endpoint.receive("down", invitation, 1, 1));
    ASSERT_NE(admission.active_media(), nullptr);
    EXPECT_TRUE(mqtt.sent.empty());
    ASSERT_TRUE(endpoint.receive("down", invitation, 2, 2));
    EXPECT_EQ(calls, 1U);
    ready_dynamic(*admission.active_media(), invite, 3);
    endpoint.tick(4, 4);
    ASSERT_EQ(mqtt.sent.size(), 1U);
    Envelope accept;
    ASSERT_EQ(decode_envelope(mqtt.sent.back(), accept), DecodeResult::Ok);
    EXPECT_EQ(accept.method, "sessionAccept");
    EXPECT_EQ(accept.session, invite.envelope.session);
    EXPECT_EQ(calls, 1U);
    EXPECT_FALSE(admission.active_media()->take_command());
    endpoint.close(5);
    EXPECT_FALSE(endpoint.released());
    close_dynamic(*admission.active_media(), invite);
    endpoint.tick(6, 6);
    EXPECT_TRUE(endpoint.released());
}
TEST(BrowserFileMedia, InvalidBindingCannotProduceDownload) {
    for (unsigned variant = 0; variant < 8; ++variant) {
        auto source = binding();
        if (variant == 0)
            source.url = "/credentials.json";
        if (variant == 1)
            source.url = "/media/file.wav?token=secret";
        if (variant == 2)
            source.session = std::string("a\0b", 3);
        if (variant == 3)
            source.content_ref = "bad\nref";
        if (variant == 4)
            source.max_bytes = 67108865;
        if (variant == 5)
            source.expires_at = 0;
        if (variant == 6)
            source.mime = "text/html";
        if (variant == 7)
            source.audio = false;
        BrowserFileMedia driver(source, [](bool, bool, std::uint64_t) { return true; });
        EXPECT_FALSE(driver.take_command());
        EXPECT_FALSE(driver.tick(1));
        EXPECT_FALSE(driver.ready());
    }
    BrowserFileMedia missing(binding(), {});
    EXPECT_FALSE(missing.take_command());
}
TEST(BrowserFileMedia, AuthorizedOriginMustMatchExactBoundary) {
    auto source = binding();
    source.origin = "https://media.example.com";
    source.url = "https://media.example.com/file.wav?signature=placeholder";
    BrowserFileMedia valid(source, [](bool, bool, std::uint64_t) { return true; });
    EXPECT_TRUE(valid.take_command());
    source.url = "https://media.example.com.evil.test/file.wav";
    BrowserFileMedia invalid(source, [](bool, bool, std::uint64_t) { return true; });
    EXPECT_FALSE(invalid.take_command());
}
TEST(BrowserFileMedia, RefusedIdentityAndGenerationHaveNoEffects) {
    BrowserFileMedia driver(binding(), [](bool, bool, std::uint64_t) { return true; });
    initialize(driver);
    auto request = control("load", ",\"contentRef\":\"content-1\"");
    EXPECT_FALSE(driver.begin(request, 0));
    request.envelope.session = "another";
    EXPECT_FALSE(driver.begin(request, 1));
    request.envelope.session = "media-session";
    request.media_revision = 1;
    EXPECT_FALSE(driver.begin(request, 1));
    EXPECT_FALSE(driver.take_command());
    request.media_revision.reset();
    EXPECT_TRUE(driver.begin(request, std::numeric_limits<std::uint64_t>::max()));
    EXPECT_NE(driver.take_command()->find("18446744073709551615"), std::string::npos);
}
TEST(BrowserFileMedia, ContentDeniedProduces403WithoutFetching) {
    BrowserFileMedia driver(binding(), [](bool, bool, std::uint64_t) { return true; });
    initialize(driver);
    EXPECT_TRUE(driver.begin(control("load", ",\"contentRef\":\"another-content\""), 1));
    EXPECT_FALSE(driver.take_command());
    ASSERT_TRUE(driver.take_fact());
    EXPECT_TRUE(driver.begin(control("load", ",\"contentRef\":\"another-content\""), 2));
    EXPECT_EQ(driver.take_fact()->code, 403);
}
TEST(BrowserFileMedia, PlayRequiresActualLoadAndCurrentLease) {
    bool lease = false;
    BrowserFileMedia driver(binding(), [&](bool, bool, std::uint64_t) { return lease; });
    initialize(driver);
    EXPECT_TRUE(driver.begin(control("play"), 1));
    EXPECT_EQ(driver.take_fact()->code, 409);
    EXPECT_FALSE(driver.take_command());
    // 使用新实例，保持核心约定的一次load生命周期。
    BrowserFileMedia loaded(binding(), [&](bool, bool, std::uint64_t) { return lease; });
    initialize(loaded);
    load(loaded);
    EXPECT_TRUE(loaded.begin(control("play"), 2));
    EXPECT_EQ(loaded.take_fact()->code, 409);
    lease = true;
    EXPECT_TRUE(loaded.output_permitted());
    EXPECT_TRUE(loaded.begin(control("play"), 3));
    EXPECT_TRUE(loaded.take_command());
}
TEST(BrowserFileMedia, PlayingNeedsFreshAudioEvidence) {
    BrowserFileMedia driver(binding(), [](bool, bool, std::uint64_t) { return true; });
    initialize(driver);
    load(driver);
    ASSERT_TRUE(driver.begin(control("play"), 2));
    EXPECT_FALSE(driver.receive(fact(2, "playing", false, 255)));
    EXPECT_FALSE(driver.take_fact());
    EXPECT_TRUE(driver.receive(fact(2, "playing")));
    EXPECT_EQ(driver.take_fact()->state, "playing");
}
TEST(BrowserFileMedia, VideoRequiresPresentedFrameAndOptionalAudioLease) {
    auto source = binding();
    source.url = "/media/file.mp4";
    source.mime = "video/mp4";
    source.audio = false;
    BrowserFileMedia driver(source,
                            [](bool audio, bool video, std::uint64_t) { return !audio && video; });
    initialize(driver);
    load(driver);
    ASSERT_TRUE(driver.begin(control("play"), 2));
    EXPECT_FALSE(driver.receive(fact(2, "playing", false, 0, 0)));
    EXPECT_TRUE(driver.receive(fact(2, "playing", false, 0, 1)));
    EXPECT_EQ(driver.take_fact()->state, "playing");
}
TEST(BrowserFileMedia, DuplicateCompletionBeforeConsumptionIsIgnored) {
    BrowserFileMedia driver(binding(), [](bool, bool, std::uint64_t) { return true; });
    initialize(driver);
    ASSERT_TRUE(driver.begin(control("load", ",\"contentRef\":\"content-1\""), 1));
    EXPECT_TRUE(driver.receive(fact(1, "loaded")));
    EXPECT_TRUE(driver.receive(fact(1, "loaded")));
    EXPECT_TRUE(driver.take_fact());
    EXPECT_FALSE(driver.take_fact());
    EXPECT_TRUE(driver.receive(fact(1, "loaded")));
    EXPECT_FALSE(driver.take_fact());
}
TEST(BrowserFileMedia, OldSpontaneousFactCannotClearPendingNewOperation) {
    BrowserFileMedia driver(binding(), [](bool, bool, std::uint64_t) { return true; });
    initialize(driver);
    load(driver);
    EXPECT_TRUE(driver.receive(fact(1, "failed", true)) == false);  // failed须使用独立终态格式。
    EXPECT_TRUE(driver.receive(fact(1, "ended", true)));
    ASSERT_TRUE(driver.begin(control("play"), 2));
    EXPECT_TRUE(driver.take_fact()->spontaneous);
    EXPECT_FALSE(driver.begin(control("stop"), 3));
    EXPECT_TRUE(driver.receive(fact(2, "playing")));
    EXPECT_TRUE(driver.take_fact());
    EXPECT_TRUE(driver.begin(control("stop"), 3));
}
TEST(BrowserFileMedia, CloseNeedsActualMatchingReleaseAndFencesLateReady) {
    BrowserFileMedia driver(binding(), [](bool, bool, std::uint64_t) { return true; });
    initialize(driver);
    load(driver);
    EXPECT_FALSE(driver.stop_all());
    EXPECT_NE(driver.take_command()->find("fileClose"), std::string::npos);
    EXPECT_FALSE(driver.take_command());
    EXPECT_TRUE(driver.receive(ready));
    EXPECT_FALSE(driver.ready());
    EXPECT_TRUE(driver.receive(
        R"({"kind":"fileClosed","sessionId":"media-session","playbackId":"player-1","generation":"0"})"));
    EXPECT_FALSE(driver.quiescent());
    EXPECT_TRUE(driver.receive(
        R"({"kind":"fileClosed","sessionId":"media-session","playbackId":"player-1","generation":"1"})"));
    EXPECT_TRUE(driver.stop_all());
    EXPECT_FALSE(driver.take_command());
}
TEST(BrowserFileMedia, ExpiryAndRegressingClockCloseOnlyOwnInstance) {
    for (const auto now : {0ULL, 10000ULL}) {
        BrowserFileMedia driver(binding(), [](bool, bool, std::uint64_t) { return true; });
        initialize(driver);
        EXPECT_FALSE(driver.tick(now));
        EXPECT_FALSE(driver.output_permitted());
        const auto close = driver.take_command();
        ASSERT_TRUE(close);
        EXPECT_NE(close->find("media-session"), std::string::npos);
        EXPECT_NE(close->find("fileClose"), std::string::npos);
    }
}
TEST(BrowserFileMedia, MalformedCallbacksNeverCreateFacts) {
    BrowserFileMedia driver(binding(), [](bool, bool, std::uint64_t) { return true; });
    initialize(driver);
    load(driver);
    ASSERT_TRUE(driver.begin(control("play"), 2));
    const auto good = fact(2, "playing");
    EXPECT_FALSE(driver.receive(good.substr(0, good.size() - 1) + ",\"extra\":true}"));
    EXPECT_FALSE(driver.receive(good.substr(0, good.size() - 1) + ",\"code\":200}"));
    auto negative = good;
    negative.replace(negative.find("\"positionMs\":100"), 16, "\"positionMs\":-1");
    EXPECT_FALSE(driver.receive(negative));
    auto wrong_id = good;
    wrong_id.replace(wrong_id.find("media-session"), 13, "other-session");
    EXPECT_FALSE(driver.receive(wrong_id));
    auto padded = good;
    padded.replace(padded.find("\"generation\":\"2\""), 16, "\"generation\":\"02\"");
    EXPECT_FALSE(driver.receive(padded));
    EXPECT_FALSE(driver.take_fact());
    EXPECT_TRUE(driver.receive(fact(1, "playing")));
    EXPECT_FALSE(driver.take_fact());
    EXPECT_TRUE(driver.receive(good));
    EXPECT_TRUE(driver.take_fact());
}
TEST(BrowserFileMedia, AsyncSeekFactsPreserveGenerationAndActualPosition) {
    BrowserFileMedia driver(binding(), [](bool, bool, std::uint64_t) { return true; });
    initialize(driver);
    load(driver);
    ASSERT_TRUE(driver.begin(control("seek", ",\"positionMs\":800"), 2));
    ASSERT_TRUE(driver.take_command());
    ASSERT_TRUE(driver.receive(fact(2, "seeking")));
    EXPECT_EQ(driver.take_fact()->position_ms, 100U);
    ASSERT_TRUE(driver.receive(fact(2, "loaded", true)));
    auto completion = driver.take_fact();
    ASSERT_TRUE(completion);
    EXPECT_TRUE(completion->spontaneous);
    EXPECT_EQ(completion->position_ms, 100U);  // 驱动事实不是请求的800ms。
    EXPECT_TRUE(driver.begin(control("setRate", ",\"rate\":1.5"), 3));
}
TEST(BrowserFileMedia, StrictFailuresHaveNoRawErrorOrFreeFields) {
    BrowserFileMedia driver(binding(), [](bool, bool, std::uint64_t) { return true; });
    initialize(driver);
    ASSERT_TRUE(driver.begin(control("load", ",\"contentRef\":\"content-1\""), 1));
    constexpr auto failure =
        R"({"kind":"fileFact","sessionId":"media-session","playbackId":"player-1","generation":"1","spontaneous":false,"code":502})";
    EXPECT_TRUE(driver.receive(failure));
    EXPECT_EQ(driver.take_fact()->code, 502);
    EXPECT_FALSE(driver.receive(
        R"({"kind":"fileFact","sessionId":"media-session","playbackId":"player-1","generation":"1","spontaneous":true,"code":502})"));
    EXPECT_TRUE(driver.receive(
        R"({"kind":"fileFact","sessionId":"media-session","playbackId":"player-1","generation":"1","spontaneous":true,"code":200,"state":"failed"})"));
    EXPECT_EQ(driver.take_fact()->state, "failed");
}
TEST(BrowserFileMedia, FactOverflowClosesInsteadOfDroppingCompletion) {
    BrowserFileMedia driver(binding(), [](bool, bool, std::uint64_t) { return true; });
    initialize(driver);
    load(driver);
    for (unsigned index = 0; index < 8; ++index)
        ASSERT_TRUE(driver.receive(fact(1, "ended", true)));
    EXPECT_FALSE(driver.receive(fact(1, "ended", true)));
    EXPECT_FALSE(driver.ready());
    EXPECT_FALSE(driver.take_fact());
    EXPECT_NE(driver.take_command()->find("fileClose"), std::string::npos);
}
TEST(BrowserFileMedia, CoreReplyWaitsForStrictDesktopFact) {
    BrowserFileMedia driver(binding(), [](bool, bool, std::uint64_t) { return true; });
    initialize(driver);
    PlaybackEngine engine(driver, "media-session", "player-1", 10000);
    constexpr auto request =
        R"({"msgToken":"real-control","method":"playbackControl","sessionId":"media-session","eventSeq":1,"params":{"playbackId":"player-1","operation":"load","expectedRevision":0,"contentRef":"content-1"}})";
    ASSERT_EQ(engine.submit(request, 1).admission, PlaybackAdmission::Accepted);
    EXPECT_FALSE(engine.take_reply());
    engine.poll(1);
    ASSERT_TRUE(driver.take_command());
    EXPECT_FALSE(engine.take_reply());
    ASSERT_TRUE(driver.receive(fact(1, "loaded")));
    engine.poll(1);
    const auto reply = engine.take_reply();
    ASSERT_TRUE(reply);
    EXPECT_NE(reply->find("real-control"), std::string::npos);
    EXPECT_NE(reply->find("loaded"), std::string::npos);
    EXPECT_EQ(engine.revision(), 1U);
    EXPECT_TRUE(driver.receive(fact(1, "loaded")));
    engine.poll(1);
    EXPECT_FALSE(engine.take_reply());
    EXPECT_EQ(engine.revision(), 1U);
}
}  // namespace
}  // namespace ur::device
