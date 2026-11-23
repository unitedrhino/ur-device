// 文件说明：受限历史宿主的配置/能力/只读桥回归，不连接网络或读取真实用户档案。
#include "record_host_support.hpp"
#include <gtest/gtest.h>

namespace ur::device {
namespace {
// configuration提供明确部署清单，文件路径是占位绝对路径，不创建或读取样本。
std::map<std::string, std::string> configuration() {
    return {{"UR_RECORD_ENABLED", "1"},
            {"UR_RECORD_CAMERA_ID", "front"},
            {"UR_RECORD_ID", "archive"},
            {"UR_RECORD_VERSION", "v1"},
            {"UR_RECORD_CONTENT_REF", "record:archive-v1"},
            {"UR_RECORD_AUDIO", "1"},
            {"UR_RECORD_DURATION_MS", "30000"},
            {"UR_RECORD_MAX_BYTES", "8388608"},
            {"UR_RECORD_FILE", "/missing-record-host-asset/archive.mp4"},
            {"UR_RECORD_LEASE_MS", "60000"}};
}
// Publish仅保存受理字节，不伪造平台ACK或原核心许可。
class Publish final : public PublishPort {
public:
    bool publish(std::string_view, std::string_view) override { return true; }
};
TEST(RecordHostConfigurationTest, MissingOrImplicitConfigurationNeverEnablesArchive) {
    EXPECT_FALSE(record_host_configuration({}));
    auto values = configuration();
    values.erase("UR_RECORD_ENABLED");
    EXPECT_FALSE(record_host_configuration(values));
    values = configuration();
    values["UR_RECORD_AUDIO"] = "true";
    EXPECT_FALSE(record_host_configuration(values));
    values = configuration();
    values["UR_RECORD_FILE"] = "relative.mp4";
    EXPECT_FALSE(record_host_configuration(values));
    values = configuration();
    values["UR_RECORD_VERSION"].clear();
    EXPECT_FALSE(record_host_configuration(values));
}
TEST(RecordHostConfigurationTest, ExplicitSafeBoundsPreserveOriginalCameraVersionAndRef) {
    auto values = configuration();
    auto parsed = record_host_configuration(values);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed->asset.route, "/record-assets/archive.mp4");
    EXPECT_EQ(parsed->asset.recording_version, "v1");
    EXPECT_EQ(parsed->asset.content_ref, "record:archive-v1");
    EXPECT_EQ(parsed->asset.max_bytes, 8388608U);
    EXPECT_EQ(parsed->lease_ms, 60000U);
    for (const auto* invalid : {"8388609", "0", "01", "-1", "9007199254740992"}) {
        values = configuration();
        values["UR_RECORD_MAX_BYTES"] = invalid;
        EXPECT_FALSE(record_host_configuration(values));
    }
    values = configuration();
    values["UR_RECORD_LEASE_MS"] = "999";
    EXPECT_FALSE(record_host_configuration(values));
    values["UR_RECORD_LEASE_MS"] = "600001";
    EXPECT_FALSE(record_host_configuration(values));
}
TEST(RecordHostCapabilityTest, OnlyFixedRecordTracksAreDeclaredWithoutHardwareOrAec) {
    const auto config = record_host_configuration(configuration());
    ASSERT_TRUE(config);
    Envelope out;
    ASSERT_EQ(record_host_capability("cap", config->asset, out), WireResult::Ok);
    EXPECT_EQ(out.method, "capabilityReport");
    EXPECT_TRUE(out.session.empty());
    EXPECT_NE(out.payload.find("\"purposes\":[\"recordPlayback\"]"), std::string::npos);
    EXPECT_NE(out.payload.find("\"aecMode\":\"\""), std::string::npos);
    EXPECT_NE(out.payload.find("\"maxPublishers\":1"), std::string::npos);
    EXPECT_EQ(out.payload.find("localAccept"), std::string::npos);
    EXPECT_EQ(out.payload.find("microphone"), std::string::npos);
    auto video = config->asset;
    video.audio = false;
    ASSERT_EQ(record_host_capability("cap", video, out), WireResult::Ok);
    EXPECT_NE(out.payload.find("\"features\":[\"video\"]"), std::string::npos);
    EXPECT_NE(out.payload.find("\"sampleRate\":0"), std::string::npos);
    video.camera_id = std::string(65, 'x');
    EXPECT_EQ(record_host_capability("cap", video, out), WireResult::Invalid);
}
TEST(RecordHostBrowserTest, UiCannotPublishFreeMethodsOrSelfAuthorizeMedia) {
    std::string kind;
    ASSERT_TRUE(record_host_local_request(R"({"kind":"ready"})", kind));
    EXPECT_EQ(kind, "ready");
    ASSERT_TRUE(record_host_local_request(R"({"kind":"shutdown"})", kind));
    EXPECT_EQ(kind, "shutdown");
    for (const auto* body :
         {"{", R"({"kind":"ready","kind":"shutdown"})", R"({"kind":"ready","permission":true})",
          R"({"kind":"play"})", R"({"kind":1})"}) {
        EXPECT_FALSE(record_host_local_request(body, kind));
        EXPECT_TRUE(kind.empty());
    }
}
TEST(RecordHostAssetTest, ReadFailureClearsBytesAndCannotUseRemoteUrl) {
    const auto config = record_host_configuration(configuration());
    ASSERT_TRUE(config);
    std::string data = "previous bytes";
    EXPECT_FALSE(read_record_archive(*config, data));
    EXPECT_TRUE(data.empty());
    auto oversized = *config;
    oversized.asset.max_bytes = 8388609;
    data = "previous bytes";
    EXPECT_FALSE(read_record_archive(oversized, data));
    EXPECT_TRUE(data.empty());
}
TEST(RecordHostProjectionTest, CoreProjectionDoesNotExportTokensPathsOrClaimFrames) {
    Publish mqtt;
    BrowserRecordPlaybackPort port({}, {});
    RecordPlaybackClient client(mqtt, port, {}, "up", "down", 1);
    const auto body = record_host_projection(client);
    EXPECT_NE(body.find("\"phase\":\"ended\""), std::string::npos);
    EXPECT_NE(body.find("\"released\":false"), std::string::npos);
    EXPECT_EQ(body.find("token"), std::string::npos);
    EXPECT_EQ(body.find("frames"), std::string::npos);
    EXPECT_EQ(body.find("file"), std::string::npos);
    EXPECT_EQ(body.find("permission"), std::string::npos);
}
}  // namespace
}  // namespace ur::device
