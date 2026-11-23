// 文件说明：可信宿主媒体目录单测；仅合成原业务事实，不冒充内容服务或实际媒体E2E。
#include "browser_file_catalog.hpp"
#include <gtest/gtest.h>

namespace ur::device {
namespace {
// entry 返回固定原授权源，目录没有预先捏造平台子会话或播放器。
BrowserFileBinding entry(std::string ref = "music") {
    return {"", "", std::move(ref), "/media/file.wav", "", "audio/wav", true, 1000, 1024};
}
// invite 只描述原独立子会话/完整输出，不包含源URL或可刷新内容期限。
PlaybackInvite invite(std::string ref = "music") {
    PlaybackInvite value;
    value.envelope.session = "child";
    value.playback_id = "player";
    value.content_ref = std::move(ref);
    value.output_modalities = {"audio"};
    value.expires_at = 999999;
    return value;
}

TEST(BrowserFileCatalog, ResolvesOriginalReferenceWithoutRefreshingSource) {
    std::optional<std::string> parent = "ai-parent";
    auto configured = entry();
    BrowserFileCatalog catalog({configured}, [&] { return parent; });
    ASSERT_TRUE(catalog.valid());
    configured.url = "/untrusted";  // 外部修改不影响目录副本。
    auto original = invite();
    auto result = catalog.resolve(original, 10);
    ASSERT_TRUE(result);
    EXPECT_EQ(result->binding.session, "child");
    EXPECT_EQ(result->binding.playback_id, "player");
    EXPECT_EQ(result->binding.url, "/media/file.wav");
    EXPECT_EQ(result->binding.expires_at, 1000U);
    EXPECT_TRUE(result->permission(true, false, 11));
    EXPECT_TRUE(result->permission(true, false, 900));  // 未取得焦点也允许准备，不以mask撤销意图。
    auto repeated = catalog.resolve(original, 900);
    ASSERT_TRUE(repeated);
    EXPECT_EQ(repeated->binding.expires_at, 1000U);
    EXPECT_FALSE(result->permission(true, false, 1000));
    EXPECT_FALSE(catalog.resolve(original, 1000));
}

TEST(BrowserFileCatalog, ExactCompleteOutputAndIndependentParentRequired) {
    std::optional<std::string> parent = "ai-parent";
    auto video = entry("video");
    video.mime = "video/mp4";
    video.url = "/media/file.mp4";
    BrowserFileCatalog catalog({entry(), video}, [&] { return parent; });
    auto request = invite("video");
    EXPECT_FALSE(catalog.resolve(request, 10));
    request.output_modalities = {"video", "audio"};
    ASSERT_TRUE(catalog.resolve(request, 11));
    request.output_modalities = {"audio", "video", "video"};
    EXPECT_FALSE(catalog.resolve(request, 12));
    request = invite("unknown");
    EXPECT_FALSE(catalog.resolve(request, 13));
    request = invite();
    request.envelope.session = *parent;
    EXPECT_FALSE(catalog.resolve(request, 14));
    request = invite();
    parent.reset();
    EXPECT_FALSE(catalog.resolve(request, 15));
    parent = "ai-parent";
    request.playback_id = std::string("bad\0id", 6);
    EXPECT_FALSE(catalog.resolve(request, 16));
}

TEST(BrowserFileCatalog, ObservedPermissionRevocationAndClockFailureNeverRevive) {
    std::optional<std::string> parent = "ai-parent";
    BrowserFileCatalog catalog({entry()}, [&] { return parent; });
    auto result = catalog.resolve(invite(), 10);
    ASSERT_TRUE(result);
    parent = "another-parent";
    EXPECT_FALSE(result->permission(true, false, 11));
    parent = "ai-parent";
    EXPECT_FALSE(result->permission(true, false, 12));
    result = catalog.resolve(invite(), 13);
    ASSERT_TRUE(result);
    EXPECT_FALSE(result->permission(true, false, 12));
    EXPECT_FALSE(result->permission(true, false, 14));
    EXPECT_FALSE(catalog.resolve(invite(), 12));
    EXPECT_FALSE(catalog.resolve(invite(), 15));
}

TEST(BrowserFileCatalog, RejectsEveryInvalidConfigurationRatherThanSkipping) {
    const auto parent = [] { return std::optional<std::string>("ai-parent"); };
    EXPECT_FALSE(BrowserFileCatalog({}, parent).valid());
    EXPECT_FALSE(BrowserFileCatalog({entry()}, {}).valid());
    EXPECT_FALSE(BrowserFileCatalog(std::vector<BrowserFileBinding>(129, entry()), parent).valid());
    EXPECT_FALSE(BrowserFileCatalog({entry(), entry()}, parent).valid());
    for (unsigned index = 0; index < 9; ++index) {
        auto invalid = entry("bad");
        switch (index) {
            case 0:
                invalid.session = "already-bound";
                break;
            case 1:
                invalid.playback_id = "already-bound";
                break;
            case 2:
                invalid.content_ref = std::string("bad\0ref", 7);
                break;
            case 3:
                invalid.url = "https://untrusted.example/media";
                break;
            case 4:
                invalid.mime = "application/json";
                break;
            case 5:
                invalid.audio = false;
                break;
            case 6:
                invalid.expires_at = 0;
                break;
            case 7:
                invalid.max_bytes = 67108865;
                break;
            case 8:
                invalid.origin = "https://source.example";
                break;
        }
        EXPECT_FALSE(BrowserFileCatalog({entry(), invalid}, parent).valid()) << index;
    }
}

TEST(BrowserFileCatalog, SilentVideoCannotAcquireAudioPermission) {
    auto source = entry("silent");
    source.mime = "video/webm";
    source.url = "/media/file.webm";
    source.audio = false;
    BrowserFileCatalog catalog({source}, [] { return std::optional<std::string>("ai-parent"); });
    ASSERT_TRUE(catalog.valid());
    auto request = invite("silent");
    request.output_modalities = {"video"};
    auto result = catalog.resolve(request, 10);
    ASSERT_TRUE(result);
    EXPECT_TRUE(result->permission(false, true, 11));
    EXPECT_FALSE(result->permission(true, true, 12));
    EXPECT_FALSE(result->permission(false, true, 13));  // 越权尝试后不能复活旧许可。
    request.output_modalities = {"audio", "video"};
    EXPECT_FALSE(catalog.resolve(request, 14));
}

TEST(BrowserFileCatalog, ResolutionLeaseDoesNotBorrowDestroyedCatalog) {
    std::optional<BrowserFileResolution> result;
    {
        BrowserFileCatalog catalog({entry()},
                                   [] { return std::optional<std::string>("ai-parent"); });
        result = catalog.resolve(invite(), 10);
    }
    ASSERT_TRUE(result);
    EXPECT_TRUE(result->permission(true, false, 11));
    EXPECT_FALSE(result->permission(true, false, 1000));
}

// 固定宿主JSON配置不能从邀请续期，Unix截止只在启动时换算一次。
TEST(BrowserFileCatalog, StrictConfigurationAndAbsoluteExpiry) {
    const std::string json =
        R"({"entries":[{"contentRef":"music","url":"/media/file.wav","origin":"","mime":"audio/wav","audio":true,"expiresAt":2000,"maxBytes":1024}]})";
    std::vector<BrowserFileBinding> decoded;
    ASSERT_TRUE(decode_browser_file_catalog(json, 10, 1000, decoded));
    ASSERT_EQ(decoded.size(), 1U);
    EXPECT_EQ(decoded.front().expires_at, 1010U);
    EXPECT_TRUE(decoded.front().session.empty());
    EXPECT_FALSE(decode_browser_file_catalog(json, 10, 2000, decoded));
    EXPECT_TRUE(decoded.empty());
    EXPECT_FALSE(decode_browser_file_catalog(json, 10, 0, decoded));
    EXPECT_FALSE(decode_browser_file_catalog(json, UINT64_MAX, 1000, decoded));
    EXPECT_FALSE(decode_browser_file_catalog("{\"entries\":[],\"extra\":true}", 10, 1000, decoded));
    EXPECT_FALSE(decode_browser_file_catalog("{\"entries\":[],\"entries\":[]}", 10, 1000, decoded));
    auto invalid = json;
    invalid.replace(invalid.find("2000"), 4, "9999999999");
    EXPECT_FALSE(decode_browser_file_catalog(invalid, 10, 1000, decoded));
    invalid = json;
    invalid.replace(invalid.find("1024"), 4, "1.5");
    EXPECT_FALSE(decode_browser_file_catalog(invalid, 10, 1000, decoded));
    invalid = json;
    invalid.replace(invalid.find("true"), 4, "\"true\"");
    EXPECT_FALSE(decode_browser_file_catalog(invalid, 10, 1000, decoded));
    invalid = json;
    invalid.replace(invalid.find("music"), 5, "bad\\u0000ref");
    EXPECT_FALSE(decode_browser_file_catalog(invalid, 10, 1000, decoded));
    EXPECT_TRUE(decoded.empty());
}
}  // namespace
}  // namespace ur::device
