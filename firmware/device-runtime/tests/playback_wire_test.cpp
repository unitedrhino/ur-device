// 文件播放器解析回归：验证条件字段、零值、非法输入和清空输出；不冒充驱动播放或源授权。
#include "ur/device/playback_wire.hpp"
#include <gtest/gtest.h>
#include <array>

namespace ur::device {
namespace {
// control 生成当前SIG下行信封；测试不伪造主AI会话或真实节目凭据。
std::string control(std::string_view params) {
    return "{\"msgToken\":\"play-1\",\"method\":\"playbackControl\","
           "\"sessionId\":\"media-child\",\"eventSeq\":1,\"params\":" +
           std::string(params) + "}";
}

// PlaybackWire.AcceptedOperations
// 所有操作保留实例和原信封，0起点不退化成缺字段。
TEST(PlaybackWire, AcceptedOperations) {
    const std::array<std::pair<std::string_view, std::string_view>, 6> cases = {
        {{"load",
          R"({"operation":"load","expectedRevision":0,"playbackId":"p","contentRef":"asset-1"})"},
         {"play", R"({"operation":"play","expectedRevision":1,"playbackId":"p"})"},
         {"pause", R"({"operation":"pause","expectedRevision":2,"playbackId":"p"})"},
         {"seek", R"({"operation":"seek","expectedRevision":3,"playbackId":"p","positionMs":0})"},
         {"setRate", R"({"operation":"setRate","expectedRevision":4,"playbackId":"p","rate":1.5})"},
         {"stop",
          R"({"operation":"stop","expectedRevision":5,"playbackId":"p","mediaRevision":2})"}}};
    for (const auto& item : cases) {
        PlaybackCommand result;
        ASSERT_EQ(decode_playback_control(WireRoute::Signaling, control(item.second), result),
                  WireResult::Ok);
        EXPECT_EQ(result.operation, item.first);
        EXPECT_EQ(result.playback_id, "p");
        EXPECT_EQ(result.envelope.session, "media-child");
        EXPECT_EQ(result.envelope.token, "play-1");
        ASSERT_TRUE(result.envelope.event_sequence);
        EXPECT_EQ(*result.envelope.event_sequence, 1U);
        if (item.first == "load") {
            EXPECT_EQ(result.content_ref, "asset-1");
        }
        if (item.first == "seek") {
            ASSERT_TRUE(result.position_ms);
            EXPECT_EQ(*result.position_ms, 0U);
        }
        if (item.first == "setRate") {
            ASSERT_TRUE(result.rate);
            EXPECT_DOUBLE_EQ(*result.rate, 1.5);
        }
        if (item.first == "stop") {
            ASSERT_TRUE(result.media_revision);
            EXPECT_EQ(*result.media_revision, 2U);
        }
    }
}

// PlaybackWire.RejectsInvalidParams
// 未知、重复、null及多操作字段都不得交给驱动。
TEST(PlaybackWire, RejectsInvalidParams) {
    for (const auto* params :
         {R"({"operation":"load","expectedRevision":0,"playbackId":"p"})",
          R"({"operation":"live","expectedRevision":0,"playbackId":"p"})",
          R"({"operation":"play","playbackId":"p"})",
          R"({"operation":"play","expectedRevision":null,"playbackId":"p"})",
          R"({"operation":"play","expectedRevision":-1,"playbackId":"p"})",
          R"({"operation":"play","expectedRevision":0.5,"playbackId":"p"})",
          R"({"operation":"play","expectedRevision":"0","playbackId":"p"})",
          R"({"operation":"play","expectedRevision":0,"playbackId":""})",
          R"({"operation":"play","expectedRevision":0,"playbackId":"p","ownerUserId":77})",
          R"({"operation":"play","operation":"stop","expectedRevision":0,"playbackId":"p"})",
          R"({"operation":"pause","expectedRevision":0,"playbackId":"p","contentRef":"asset"})",
          R"({"operation":"load","expectedRevision":0,"playbackId":"p","contentRef":null})",
          R"({"operation":"seek","expectedRevision":0,"playbackId":"p","positionMs":null})",
          R"({"operation":"seek","expectedRevision":0,"playbackId":"p","positionMs":-1})",
          R"({"operation":"setRate","expectedRevision":0,"playbackId":"p","rate":1.25})",
          R"({"operation":"setRate","expectedRevision":0,"playbackId":"p","rate":"1"})",
          R"({"operation":"play","expectedRevision":0,"playbackId":"p","mediaRevision":0})",
          R"({"operation":"play","expectedRevision":0,"playbackId":"p","mediaRevision":null})"}) {
        PlaybackCommand result;
        result.playback_id = "previous";  // 失败必须清空，不让旧实例继续播放。
        EXPECT_EQ(decode_playback_control(WireRoute::Signaling, control(params), result),
                  WireResult::Invalid);
        EXPECT_TRUE(result.playback_id.empty());
        EXPECT_TRUE(result.envelope.session.empty());
        EXPECT_FALSE(result.position_ms);
    }
}

// PlaybackWire.RejectsSourceAndRoute
// 引用不直接解释为URL或路径，message内容不是播放控制。
TEST(PlaybackWire, RejectsSourceAndRoute) {
    for (const auto* reference : {"", "https://source.example/video", "file:///tmp/video",
                                  "data:audio/x", "/tmp/video", "../video", " asset", "asset "}) {
        const auto params =
            std::string(
                R"({"operation":"load","expectedRevision":0,"playbackId":"p","contentRef":")") +
            reference + "\"}";
        PlaybackCommand result;
        EXPECT_EQ(decode_playback_control(WireRoute::Signaling, control(params), result),
                  WireResult::Invalid);
    }
    const auto body = control(R"({"operation":"play","expectedRevision":0,"playbackId":"p"})");
    PlaybackCommand result;
    EXPECT_EQ(decode_playback_control(WireRoute::Message, body, result), WireResult::WrongRoute);
    for (const auto* bad :
         {"", "{}", R"({"msgToken":"x","method":"playbackControl","sessionId":"s","params":{}})"})
        EXPECT_EQ(decode_playback_control(WireRoute::Signaling, bad, result), WireResult::Invalid);
}

// PlaybackWire.SafeIntegerBoundary 严格遵守Go/browser/cJSON共享的精度上界。
TEST(PlaybackWire, SafeIntegerBoundary) {
    PlaybackCommand result;
    ASSERT_EQ(
        decode_playback_control(
            WireRoute::Signaling,
            control(
                R"({"operation":"seek","expectedRevision":9007199254740991,"playbackId":"p","positionMs":9007199254740991})"),
            result),
        WireResult::Ok);
    EXPECT_EQ(result.expected_revision, 9007199254740991ULL);
    EXPECT_EQ(result.position_ms, 9007199254740991ULL);
    EXPECT_EQ(
        decode_playback_control(
            WireRoute::Signaling,
            control(
                R"({"operation":"seek","expectedRevision":9007199254740992,"playbackId":"p","positionMs":0})"),
            result),
        WireResult::Invalid);
}
// PlaybackWire.FileCapability 必须显式装配输出驱动，失败清空旧能力，不从摄像头声明推断视频输出。
TEST(PlaybackWire, FileCapability) {
    PlaybackCapability capability;
    Envelope result;
    EXPECT_EQ(playback_capability_command("cap", capability, result), WireResult::Invalid);
    capability.audio_output = true;
    ASSERT_EQ(playback_capability_command("cap", capability, result), WireResult::Ok);
    EXPECT_EQ(result.method, "capabilityReport");
    EXPECT_EQ(
        result.payload,
        R"({"features":[],"outputModalities":["audio"],"purposes":["mediaPlayback"],"revision":1,"transports":["none"]})");
    capability.video_output = true;
    ASSERT_EQ(playback_capability_command("cap", capability, result), WireResult::Ok);
    EXPECT_NE(result.payload.find(R"("outputModalities":["audio","video"])"), std::string::npos);
    capability.audio_output = false;
    ASSERT_EQ(playback_capability_command("cap", capability, result), WireResult::Ok);
    EXPECT_NE(result.payload.find(R"("outputModalities":["video"])"), std::string::npos);
    capability.revision = 0;
    EXPECT_EQ(playback_capability_command("cap", capability, result), WireResult::Invalid);
    EXPECT_TRUE(result.payload == "{}");
    capability.revision = 9007199254740992ULL;
    EXPECT_EQ(playback_capability_command("cap", capability, result), WireResult::Invalid);
    EXPECT_TRUE(result.token.empty());
    capability.revision = 1;
    EXPECT_EQ(playback_capability_command("", capability, result), WireResult::Invalid);
    EXPECT_TRUE(result.method.empty());
}

// PlaybackWire.FileOutputIntersection 双输出请求不能因一个输出可用而接纳，重复输出也非法。
TEST(PlaybackWire, FileOutputIntersection) {
    PlaybackCapability capability;
    PlaybackInvite invite;
    invite.output_modalities = {"audio"};
    EXPECT_EQ(validate_playback_outputs(capability, invite), WireResult::Invalid);
    capability.audio_output = true;
    EXPECT_EQ(validate_playback_outputs(capability, invite), WireResult::Ok);
    invite.output_modalities = {"audio", "video"};
    EXPECT_EQ(validate_playback_outputs(capability, invite), WireResult::Invalid);
    capability.video_output = true;
    EXPECT_EQ(validate_playback_outputs(capability, invite), WireResult::Ok);
    for (const auto& outputs : std::vector<std::vector<std::string>>{
             {}, {"audio", "audio"}, {"camera"}, {"text"}, {"audio", "video", "text"}}) {
        invite.output_modalities = outputs;
        EXPECT_EQ(validate_playback_outputs(capability, invite), WireResult::Invalid);
    }
    invite.output_modalities = {"video"};
    capability.revision = 0;
    EXPECT_EQ(validate_playback_outputs(capability, invite), WireResult::Invalid);
    capability.revision = 9007199254740992ULL;
    EXPECT_EQ(validate_playback_outputs(capability, invite), WireResult::Invalid);
}

// PlaybackWire.FileInvite 无实时轨和采集，失败清空先前绑定；源授权不由解析器代替。
TEST(PlaybackWire, FileInvite) {
    const std::string valid =
        R"({"msgToken":"invite-1","method":"sessionInvite","sessionId":"media-child","eventSeq":1,"params":{"purpose":"mediaPlayback","admission":"direct","transport":"none","tracks":[],"expiresAt":9007199254740991,"playbackId":"p","mediaSource":{"contentRef":"asset-1"},"inputModalities":[],"outputModalities":["audio","video"]}})";
    PlaybackInvite result;
    ASSERT_EQ(decode_playback_invite(WireRoute::Signaling, valid, result), WireResult::Ok);
    EXPECT_EQ(result.playback_id, "p");
    EXPECT_EQ(result.content_ref, "asset-1");
    EXPECT_EQ(result.output_modalities, (std::vector<std::string>{"audio", "video"}));
    EXPECT_EQ(result.expires_at, 9007199254740991ULL);
    for (const auto& item : std::vector<std::pair<std::string, std::string>>{
             {"\"none\"", "\"udp\""},
             {"\"tracks\":[]", "\"tracks\":null"},
             {"\"inputModalities\":[]", "\"inputModalities\":[\"audio\"]"},
             {"[\"audio\",\"video\"]", "[\"audio\",\"audio\"]"},
             {"[\"audio\",\"video\"]", "[]"},
             {"9007199254740991", "9007199254740992"},
             {"9007199254740991", "0"},
             {"asset-1", "../private"},
             {"asset-1", "https://example.invalid"},
             {"\"eventSeq\":1", "\"eventSeq\":0"},
             {"\"params\":", "\"data\":"},
             {"\"purpose\":\"mediaPlayback\"", "\"purpose\":\"mediaPlayback\",\"version\":2"}}) {
        auto invalid = valid;
        const auto position = invalid.find(item.first);
        ASSERT_NE(position, std::string::npos);
        invalid.replace(position, item.first.size(), item.second);
        result.playback_id = "previous";
        EXPECT_EQ(decode_playback_invite(WireRoute::Signaling, invalid, result),
                  WireResult::Invalid);
        EXPECT_TRUE(result.playback_id.empty());
        EXPECT_TRUE(result.output_modalities.empty());
    }
    EXPECT_EQ(decode_playback_invite(WireRoute::Message, valid, result), WireResult::WrongRoute);
    EXPECT_TRUE(result.envelope.session.empty());
}
}  // namespace
}  // namespace ur::device
