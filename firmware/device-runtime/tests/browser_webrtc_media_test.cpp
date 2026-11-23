// 文件说明：浏览器WebRTC媒体适配的协商、输入输出栅栏及过期回调单测。
#include "browser_webrtc_media.hpp"
#include <gtest/gtest.h>
#include <type_traits>
#include "ur/device/output_group_gate.hpp"

namespace ur::device {
// 原启动预检消息只证明可协商能力，不含会话、取帧或模型同意。
static std::string vision_ready_message(std::string_view state = "ready") {
    return "{\"kind\":\"webrtcVisionReady\",\"cameraId\":\"front\",\"videoTrack\":\"vision-1\","
           "\"deviceId\":\"explicit-device\",\"state\":\"" +
           std::string(state) + "\"}";
}
TEST(BrowserWebRTCMedia, VisionNativeMappingIsNotCapabilityUntilActualPreflightMatches) {
    BrowserWebRTCMedia media;
    VoiceVisionProfile profile{"front", "vision-1", 1, false};
    EXPECT_FALSE(media.receive_vision_ready(vision_ready_message()));
    ASSERT_TRUE(media.prepare_vision_preflight(profile, "explicit-device"));
    EXPECT_FALSE(media.prepare_vision_preflight(profile, "explicit-device"));
    EXPECT_EQ(media.vision_profile(), nullptr);
    profile.ready = true;
    EXPECT_FALSE(media.enable_vision(profile));
    EXPECT_FALSE(media.take_command());
    ASSERT_TRUE(media.receive_vision_ready(vision_ready_message()));
    ASSERT_NE(media.vision_profile(), nullptr);
    EXPECT_TRUE(media.vision_profile()->ready);
    EXPECT_TRUE(media.receive_vision_ready(vision_ready_message()));
    EXPECT_FALSE(media.receive_vision_ready(vision_ready_message("unsupported")));
    EXPECT_FALSE(media.take_command());  // 预检不能创建Peer或摄像头流。
}
TEST(BrowserWebRTCMedia, VisionUnsupportedPreflightCannotBePromotedButAudioRemainsUsable) {
    BrowserWebRTCMedia media;
    VoiceVisionProfile profile{"front", "vision-1", 1, false};
    ASSERT_TRUE(media.prepare_vision_preflight(profile, "explicit-device"));
    ASSERT_TRUE(media.receive_vision_ready(vision_ready_message("unsupported")));
    EXPECT_EQ(media.vision_profile(), nullptr);
    EXPECT_TRUE(media.receive_vision_ready(vision_ready_message("unsupported")));
    EXPECT_FALSE(media.receive_vision_ready(vision_ready_message()));
    profile.ready = true;
    EXPECT_FALSE(media.enable_vision(profile));
    VoiceMedia audio;
    audio.transport = VoiceTransport::WebRTC;
    audio.audio = {48000, 1, 20};
    ASSERT_TRUE(media.configure(audio));
    EXPECT_FALSE(media.receive_vision_ready(vision_ready_message()));
}
TEST(BrowserWebRTCMedia, VisionPreflightRejectsWrongNullDuplicateExtraAndUnknownFields) {
    for (const auto& pair :
         {std::pair<std::string, std::string>{"\"deviceId\":\"explicit-device\"",
                                              "\"deviceId\":\"other\""},
          {"\"cameraId\":\"front\"", "\"cameraId\":null"},
          {"\"videoTrack\":\"vision-1\"", "\"videoTrack\":\"speaker-1\""},
          {"\"state\":\"ready\"", "\"state\":true"},
          {"\"state\":\"ready\"", "\"state\":\"connected\""},
          {"\"state\":\"ready\"", "\"state\":\"ready\",\"secret\":\"test\""},
          {"\"state\":\"ready\"", "\"state\":\"ready\",\"st\\u0061te\":\"ready\""}}) {
        BrowserWebRTCMedia media;
        ASSERT_TRUE(
            media.prepare_vision_preflight({"front", "vision-1", 1, false}, "explicit-device"));
        auto malformed = vision_ready_message();
        malformed.replace(malformed.find(pair.first), pair.first.size(), pair.second);
        EXPECT_FALSE(media.receive_vision_ready(malformed));
        EXPECT_EQ(media.vision_profile(), nullptr);
        EXPECT_TRUE(media.receive_vision_ready(vision_ready_message()));
    }
}
TEST(BrowserWebRTCMedia, VisionPreflightInvalidMappingCannotInitializeOrBypassReady) {
    for (unsigned variant = 0; variant < 6; ++variant) {
        BrowserWebRTCMedia media;
        VoiceVisionProfile profile{"front", "vision-1", 1, false};
        std::string device = "explicit-device";
        if (variant == 0)
            profile.ready = true;
        if (variant == 1)
            profile.camera_id.clear();
        if (variant == 2)
            profile.max_publishers = 2;
        if (variant == 3)
            device.clear();
        if (variant == 4)
            device = "invalid\ndevice";
        if (variant == 5)
            device = std::string(257, 'x');
        EXPECT_FALSE(media.prepare_vision_preflight(profile, device));
        EXPECT_FALSE(media.receive_vision_ready(vision_ready_message()));
        EXPECT_EQ(media.vision_profile(), nullptr);
    }
}
// 视觉通过原虚端口调用，旧默认configure_vision明确拒绝；能力不存在不能借普通音频配置放行。
TEST(BrowserWebRTCMedia, VisionRequiresExplicitOriginalProfileAndPausedConfiguration) {
    BrowserWebRTCMedia media;
    VoiceMedia selected;
    selected.transport = VoiceTransport::WebRTC;
    selected.audio = {48000, 1, 20};
    selected.ai_vision = true;
    selected.input_track = "mic-1";
    selected.output_track = "speaker-1";
    VoiceVisionProfile profile{"front", "vision-1", 1, true};
    VoiceMediaPort& port = media;
    EXPECT_EQ(port.vision_profile(), nullptr);
    EXPECT_FALSE(port.configure_vision(selected, profile));
    EXPECT_FALSE(media.configure(selected));
}
// vision_selection保留实际平台已校验的三轨选择；不包含任何摄像头许可。
static VoiceMedia vision_selection() {
    VoiceMedia selected;
    selected.transport = VoiceTransport::WebRTC;
    selected.audio = {48000, 1, 20};
    selected.ai_vision = true;
    selected.input_track = "mic-1";
    selected.output_track = "speaker-1";
    return selected;
}
// 原端口受理仅排队；浏览器实际替轨回执之前不得宣称摄像头已发布。
TEST(BrowserWebRTCMedia, VisionPublishRequiresActualControlPortRatherThanConnected) {
    BrowserWebRTCMedia media;
    const VoiceVisionProfile profile{"front", "vision-1", 1, true};
    ASSERT_TRUE(media.enable_vision(profile));
    ASSERT_TRUE(media.configure_vision(vision_selection(), profile));
    ASSERT_TRUE(media.take_command());
    ASSERT_TRUE(media.offer_received("v=0\r\no=offer\r\n", 1));
    ASSERT_TRUE(media.take_offer());
    ASSERT_TRUE(media.apply_answer("v=0\r\no=answer\r\n"));
    ASSERT_TRUE(media.take_command());
    ASSERT_TRUE(media.connected_received(1));
    VoiceMediaPort& port = media;
    VoiceVisionControl control{
        1, "original-ai", "front", "vision-1", 1, VoiceVisionOperation::Publish};
    EXPECT_EQ(port.begin_vision_control(control), VoiceVisionRequestResult::Accepted);
    EXPECT_FALSE(port.take_vision_fact());
}
// ready_vision只完成原Peer协商；摄像头仍未打开，后续每项测试必须另交实际回执。
static void ready_vision(BrowserWebRTCMedia& media) {
    const VoiceVisionProfile profile{"front", "vision-1", 1, true};
    ASSERT_TRUE(media.enable_vision(profile));
    ASSERT_TRUE(media.configure_vision(vision_selection(), profile));
    ASSERT_TRUE(media.take_command());
    ASSERT_TRUE(media.offer_received("v=0\r\no=offer\r\n", 1));
    ASSERT_TRUE(media.take_offer());
    ASSERT_TRUE(media.apply_answer("v=0\r\no=answer\r\n"));
    ASSERT_TRUE(media.take_command());
    ASSERT_TRUE(media.connected_received(1));
}
// vision_fact是原可信桥实际回执的固定格式；不含云token或网页自报授权。
static std::string vision_fact(std::uint64_t generation, std::string_view state,
                               std::string_view error = "") {
    return "{\"kind\":\"webrtcVisionActual\",\"generation\":\"" + std::to_string(generation) +
           "\",\"sessionId\":\"original-ai\",\"cameraId\":\"front\",\"trackId\":\"vision-1\","
           "\"mediaRevision\":\"1\",\"peerRevision\":\"1\",\"state\":\"" +
           std::string(state) + "\",\"error\":\"" + std::string(error) + "\"}";
}
// control_vision固定原Core合法关联；不根据Peer推导session或权限。
static VoiceVisionControl control_vision(
    std::uint64_t generation = 1, VoiceVisionOperation operation = VoiceVisionOperation::Publish) {
    return {generation, "original-ai", "front", "vision-1", 1, operation};
}
TEST(BrowserWebRTCMedia, VisionActualRequiresDeliveryAndExactOriginalScope) {
    BrowserWebRTCMedia media;
    ready_vision(media);
    ASSERT_EQ(media.begin_vision_control(control_vision()), VoiceVisionRequestResult::Accepted);
    EXPECT_FALSE(media.receive_vision_actual(vision_fact(1, "active")));
    EXPECT_EQ(media.begin_vision_control(control_vision()), VoiceVisionRequestResult::Accepted);
    const auto command = media.take_command();
    ASSERT_TRUE(command);
    EXPECT_NE(command->find("\"operation\":\"publish\""), std::string::npos);
    EXPECT_FALSE(media.take_command());
    ASSERT_TRUE(media.receive_vision_actual(vision_fact(1, "active")));
    ASSERT_TRUE(media.receive_vision_actual(vision_fact(1, "active")));
    const auto fact = media.take_vision_fact();
    ASSERT_TRUE(fact);
    EXPECT_EQ(fact->generation, 1U);
    EXPECT_EQ(fact->state, VoiceVisionState::Active);
    EXPECT_EQ(fact->failure, VoiceVisionFailure::None);
    EXPECT_FALSE(media.take_vision_fact());
    EXPECT_FALSE(media.receive_vision_actual(vision_fact(1, "failed", "mediaFailed")));
    EXPECT_EQ(media.begin_vision_control(control_vision(2)), VoiceVisionRequestResult::Busy);
}
TEST(BrowserWebRTCMedia, VisionStopPreemptsPendingPublishAndIgnoresRetiredActual) {
    BrowserWebRTCMedia media;
    ready_vision(media);
    ASSERT_EQ(media.begin_vision_control(control_vision()), VoiceVisionRequestResult::Accepted);
    ASSERT_TRUE(media.take_command());
    ASSERT_EQ(media.begin_vision_control(control_vision(2, VoiceVisionOperation::Stop)),
              VoiceVisionRequestResult::Accepted);
    EXPECT_FALSE(media.receive_vision_actual(vision_fact(2, "stopped")));
    EXPECT_TRUE(media.receive_vision_actual(vision_fact(1, "active")));
    EXPECT_FALSE(media.take_vision_fact());
    ASSERT_TRUE(media.take_command());
    EXPECT_FALSE(media.receive_vision_actual(vision_fact(2, "active")));
    ASSERT_TRUE(media.receive_vision_actual(vision_fact(2, "stopped")));
    ASSERT_TRUE(media.take_vision_fact());
    EXPECT_EQ(media.begin_vision_control(control_vision(3)), VoiceVisionRequestResult::Accepted);
}
TEST(BrowserWebRTCMedia, VisionCancelIsNotStoppedAndRequiresSeparateStopProof) {
    BrowserWebRTCMedia media;
    ready_vision(media);
    ASSERT_EQ(media.begin_vision_control(control_vision()), VoiceVisionRequestResult::Accepted);
    ASSERT_TRUE(media.take_command());
    media.cancel_vision_control(99);
    EXPECT_FALSE(media.take_command());
    media.cancel_vision_control(1);
    const auto cancel = media.take_command();
    ASSERT_TRUE(cancel);
    EXPECT_NE(cancel->find("\"kind\":\"webrtcVisionCancel\""), std::string::npos);
    EXPECT_EQ(cancel->find("operation"), std::string::npos);
    EXPECT_TRUE(media.receive_vision_actual(vision_fact(1, "active")));
    EXPECT_FALSE(media.take_vision_fact());
    EXPECT_EQ(media.begin_vision_control(control_vision(2)), VoiceVisionRequestResult::Busy);
    ASSERT_EQ(media.begin_vision_control(control_vision(2, VoiceVisionOperation::Stop)),
              VoiceVisionRequestResult::Accepted);
    ASSERT_TRUE(media.take_command());
    ASSERT_TRUE(media.receive_vision_actual(vision_fact(2, "stopped")));
    const auto fact = media.take_vision_fact();
    ASSERT_TRUE(fact);
    EXPECT_EQ(fact->state, VoiceVisionState::Stopped);
}
TEST(BrowserWebRTCMedia, VisionStrictActualRejectsDuplicateNullUnknownAndMismatchedFields) {
    const auto original = vision_fact(1, "active");
    for (const auto& pair : {std::pair<std::string, std::string>{"\"sessionId\":\"original-ai\"",
                                                                 "\"sessionId\":\"other-ai\""},
                             {"\"trackId\":\"vision-1\"", "\"trackId\":null"},
                             {"\"mediaRevision\":\"1\"", "\"mediaRevision\":\"9007199254740992\""},
                             {"\"generation\":\"1\"", "\"generation\":\"18446744073709551616\""},
                             {"\"generation\":\"1\"", "\"generation\":1"},
                             {"\"peerRevision\":\"1\"", "\"peerRevision\":\"2\""},
                             {"\"error\":\"\"", "\"error\":\"private error\""},
                             {"\"error\":\"\"", "\"error\":\"mediaFailed\""},
                             {"\"state\":\"active\"", "\"state\":\"connected\""},
                             {"\"kind\":\"webrtcVisionActual\"",
                              "\"kind\":\"webrtcVisionActual\",\"kind\":\"webrtcVisionActual\""}}) {
        BrowserWebRTCMedia media;
        ready_vision(media);
        ASSERT_EQ(media.begin_vision_control(control_vision()), VoiceVisionRequestResult::Accepted);
        ASSERT_TRUE(media.take_command());
        auto malformed = original;
        malformed.replace(malformed.find(pair.first), pair.first.size(), pair.second);
        EXPECT_FALSE(media.receive_vision_actual(malformed));
        EXPECT_FALSE(media.take_vision_fact());
        EXPECT_TRUE(media.ready());
    }
}
TEST(BrowserWebRTCMedia, VisionFailureEnumsRemainFailureNotResourceRelease) {
    for (const auto* error : {"unsupported", "busy", "mediaFailed", "timeout", "cancelled"}) {
        BrowserWebRTCMedia media;
        ready_vision(media);
        ASSERT_EQ(media.begin_vision_control(control_vision()), VoiceVisionRequestResult::Accepted);
        ASSERT_TRUE(media.take_command());
        EXPECT_FALSE(media.receive_vision_actual(vision_fact(1, "failed")));
        ASSERT_TRUE(media.receive_vision_actual(vision_fact(1, "failed", error)));
        const auto fact = media.take_vision_fact();
        ASSERT_TRUE(fact);
        EXPECT_EQ(fact->state, VoiceVisionState::Failed);
        EXPECT_NE(fact->failure, VoiceVisionFailure::None);
        EXPECT_TRUE(media.ready());
        EXPECT_EQ(media.begin_vision_control(control_vision(2)), VoiceVisionRequestResult::Busy);
    }
}
TEST(BrowserWebRTCMedia, VisionInvalidControlAndRetirementCannotAcquireCamera) {
    for (unsigned variant = 0; variant < 7; ++variant) {
        BrowserWebRTCMedia media;
        ready_vision(media);
        auto control = control_vision();
        if (variant == 0)
            control.generation = 0;
        if (variant == 1)
            control.camera_id = "other";
        if (variant == 2)
            control.track_id = "speaker-1";
        if (variant == 3)
            control.session_id = "invalid\nsession";
        if (variant == 4)
            control.media_revision = 9007199254740992ULL;
        if (variant == 5)
            control.operation = static_cast<VoiceVisionOperation>(99);
        if (variant == 6)
            media.close();
        EXPECT_EQ(media.begin_vision_control(control), VoiceVisionRequestResult::Unsupported);
        EXPECT_FALSE(media.take_vision_fact());
    }
}
TEST(BrowserWebRTCMedia, VisionFactsAreBoundedAndCompletedHistoryIsNotRewritten) {
    BrowserWebRTCMedia media;
    ready_vision(media);
    for (std::uint64_t generation = 1; generation <= 16; ++generation) {
        const auto operation =
            generation % 2 ? VoiceVisionOperation::Publish : VoiceVisionOperation::Stop;
        ASSERT_EQ(media.begin_vision_control(control_vision(generation, operation)),
                  VoiceVisionRequestResult::Accepted);
        ASSERT_TRUE(media.take_command());
        ASSERT_TRUE(media.receive_vision_actual(
            vision_fact(generation, generation % 2 ? "active" : "stopped")));
    }
    EXPECT_EQ(media.begin_vision_control(control_vision(17)), VoiceVisionRequestResult::Busy);
    for (std::uint64_t generation = 1; generation <= 16; ++generation) {
        const auto fact = media.take_vision_fact();
        ASSERT_TRUE(fact);
        EXPECT_EQ(fact->generation, generation);
    }
    EXPECT_FALSE(media.take_vision_fact());
    EXPECT_EQ(media.begin_vision_control(control_vision(17)), VoiceVisionRequestResult::Accepted);
}
// 原控制排队满额时取消必须退休原Peer；不能丢取消还让旧摄像头继续受理。
TEST(BrowserWebRTCMedia, VisionQueueBackpressureCancellationRetiresPeerWithoutStoppedFact) {
    BrowserWebRTCMedia media;
    ready_vision(media);
    for (std::uint64_t generation = 1; generation <= 16; ++generation) {
        ASSERT_EQ(
            media.begin_vision_control(control_vision(generation, VoiceVisionOperation::Stop)),
            VoiceVisionRequestResult::Accepted);
    }
    EXPECT_EQ(media.begin_vision_control(control_vision(17, VoiceVisionOperation::Stop)),
              VoiceVisionRequestResult::Busy);
    media.cancel_vision_control(16);
    EXPECT_FALSE(media.ready());
    EXPECT_EQ(media.take_command(),
              std::optional<std::string>(R"({"kind":"webrtcStop","peerRevision":"1"})"));
    EXPECT_FALSE(media.take_command());
    EXPECT_FALSE(media.take_vision_fact());
    EXPECT_TRUE(media.receive_vision_actual(vision_fact(16, "stopped")));
    EXPECT_FALSE(media.take_vision_fact());
}
TEST(BrowserWebRTCMedia,
     VisionOriginalScopeCannotChangeAndOldCallbacksDoNotReviveDisconnectedPeer) {
    BrowserWebRTCMedia media;
    ready_vision(media);
    ASSERT_EQ(media.begin_vision_control(control_vision()), VoiceVisionRequestResult::Accepted);
    ASSERT_TRUE(media.take_command());
    auto other = control_vision(2, VoiceVisionOperation::Stop);
    other.session_id = "another-ai";
    EXPECT_EQ(media.begin_vision_control(other), VoiceVisionRequestResult::Unsupported);
    other = control_vision(2, VoiceVisionOperation::Stop);
    other.media_revision = 2;
    EXPECT_EQ(media.begin_vision_control(other), VoiceVisionRequestResult::Unsupported);
    ASSERT_TRUE(media.disconnected_received(1));
    EXPECT_TRUE(media.receive_vision_actual(vision_fact(1, "active")));
    EXPECT_FALSE(media.take_vision_fact());
    EXPECT_EQ(media.begin_vision_control(control_vision(2, VoiceVisionOperation::Stop)),
              VoiceVisionRequestResult::Unsupported);
    EXPECT_FALSE(media.take_command());
}
// 原Profile按值固定，外部参数后续变化不能修改已上报能力或创建配置。
TEST(BrowserWebRTCMedia, VisionProfileIsExplicitImmutableAndConfigurationIsPaused) {
    BrowserWebRTCMedia media;
    VoiceVisionProfile original{"front", "vision-1", 1, true};
    ASSERT_TRUE(media.enable_vision(original));
    original.camera_id = "other";
    ASSERT_NE(media.vision_profile(), nullptr);
    EXPECT_EQ(media.vision_profile()->camera_id, "front");
    const auto profile = *media.vision_profile();
    EXPECT_FALSE(media.enable_vision(profile));
    EXPECT_FALSE(media.configure_vision(vision_selection(), original));
    ASSERT_TRUE(media.configure_vision(vision_selection(), profile));
    EXPECT_FALSE(media.offer_received("v=0\r\no=offer\r\n", 1));
    EXPECT_EQ(
        media.take_command(),
        std::optional<std::string>(
            R"({"kind":"webrtcConfigureVision","peerRevision":"1","cameraId":"front","videoTrack":"vision-1","codec":"H264","state":"paused"})"));
    ASSERT_TRUE(media.offer_received("v=0\r\no=offer\r\n", 1));
    ASSERT_TRUE(media.take_offer());
    ASSERT_TRUE(media.apply_answer("v=0\r\no=answer\r\n"));
    ASSERT_TRUE(media.take_command());
    ASSERT_TRUE(media.connected_received(1));
    EXPECT_TRUE(media.ready());
    EXPECT_FALSE(media.configure_vision(vision_selection(), profile));
    EXPECT_FALSE(media.take_command());
}
// 未预检、错轨、超并发、畸形标识不能声明视觉；默认普通音频仍可用。
TEST(BrowserWebRTCMedia, VisionInvalidOrUnsupportedProfileDoesNotCreatePeer) {
    for (unsigned variant = 0; variant != 6; ++variant) {
        BrowserWebRTCMedia media;
        VoiceVisionProfile profile{"front", "vision-1", 1, true};
        if (variant == 0) {
            profile.ready = false;
        }
        if (variant == 1) {
            profile.max_publishers = 2;
        }
        if (variant == 2) {
            profile.camera_id.clear();
        }
        if (variant == 3) {
            profile.video_track = "mic-1";
        }
        if (variant == 4) {
            profile.camera_id = "bad\nsource";
        }
        if (variant == 5) {
            profile.camera_id = std::string(65, 'x');
        }
        EXPECT_FALSE(media.enable_vision(profile));
        EXPECT_EQ(media.vision_profile(), nullptr);
        EXPECT_FALSE(media.configure_vision(vision_selection(), profile));
        EXPECT_FALSE(media.take_command());
        auto audio = vision_selection();
        audio.ai_vision = false;
        EXPECT_TRUE(media.configure(audio));
    }
}
// 原平台选择不可被驱动悄悄改为音频或另一camera；所有拒绝保持可接受合法原选择。
TEST(BrowserWebRTCMedia, VisionRejectsWrongMediaSelectionWithoutInventingTrackState) {
    for (unsigned variant = 0; variant != 7; ++variant) {
        BrowserWebRTCMedia media;
        const VoiceVisionProfile profile{"front", "vision-1", 1, true};
        ASSERT_TRUE(media.enable_vision(profile));
        auto selected = vision_selection();
        if (variant == 0) {
            selected.ai_vision = false;
        }
        if (variant == 1) {
            selected.transport = VoiceTransport::Udp;
        }
        if (variant == 2) {
            selected.audio.sample_rate = 16000;
        }
        if (variant == 3) {
            selected.input_track = "other";
        }
        if (variant == 4) {
            selected.output_track = "mic-1";
        }
        if (variant == 5) {
            selected.host = "unexpected";
        }
        if (variant == 6) {
            selected.port = 1;
        }
        EXPECT_FALSE(media.configure_vision(selected, profile));
        EXPECT_FALSE(media.take_command());
        EXPECT_FALSE(media.ready());
        ASSERT_TRUE(media.configure_vision(vision_selection(), profile));
    }
}
// 视觉本阶段无恢复授权；停止与迟到回调不得发布摄像头或复活已退休配置。
TEST(BrowserWebRTCMedia, VisionClosedAndDisconnectedCannotRecoverOrReuseOriginalPeer) {
    BrowserWebRTCMedia media;
    const VoiceVisionProfile profile{"front", "vision-1", 1, true};
    ASSERT_TRUE(media.enable_vision(profile));
    ASSERT_TRUE(media.configure_vision(vision_selection(), profile));
    ASSERT_TRUE(media.take_command());
    ASSERT_TRUE(media.offer_received("v=0\r\no=offer\r\n", 1));
    ASSERT_TRUE(media.take_offer());
    ASSERT_TRUE(media.apply_answer("v=0\r\no=answer\r\n"));
    ASSERT_TRUE(media.take_command());
    ASSERT_TRUE(media.connected_received(1));
    ASSERT_TRUE(media.disconnected_received(1));
    EXPECT_FALSE(media.begin_recovery(2));
    media.close();
    EXPECT_EQ(media.take_command(),
              std::optional<std::string>(R"({"kind":"webrtcStop","peerRevision":"1"})"));
    EXPECT_FALSE(media.offer_received("v=0\r\no=late\r\n", 1));
    EXPECT_FALSE(media.connected_received(1));
    EXPECT_FALSE(media.configure_vision(vision_selection(), profile));
    EXPECT_FALSE(media.take_command());
}
// WebRTC真正提供实际输出端口以前不得删除宿主能力拒绝门；本断言在旧源可编译但业务失败。
TEST(BrowserWebRTCMedia, ActualOutputGateRequiredForSharedMediaFocus) {
    EXPECT_TRUE((std::is_base_of<OutputGroupGatePort, BrowserWebRTCMedia>::value));
}
TEST(BrowserWebRTCMedia, RequiresOfferAnswerAndConnectedBeforeAudio) {
    BrowserWebRTCMedia media;
    VoiceMedia selected;
    selected.transport = VoiceTransport::WebRTC;
    selected.audio = {48000, 1, 20};
    EXPECT_FALSE(media.ready());
    EXPECT_FALSE(media.offer_received("v=0\r\no=offer\r\n", 1));
    ASSERT_TRUE(media.configure(selected));
    EXPECT_FALSE(media.configure(selected));
    ASSERT_TRUE(media.take_command().has_value());
    EXPECT_FALSE(media.connected_received(1));
    EXPECT_FALSE(media.start_capture("input"));
    EXPECT_FALSE(media.offer_received("v=0\n", 1));
    ASSERT_TRUE(media.offer_received("v=0\r\no=offer\r\n", 1));
    ASSERT_EQ(media.take_offer(), std::optional<std::string>("v=0\r\no=offer\r\n"));
    EXPECT_FALSE(media.take_offer().has_value());
    EXPECT_FALSE(media.offer_received("v=0\r\no=other\r\n", 1));
    EXPECT_FALSE(media.apply_answer("v=0\n"));
    ASSERT_TRUE(media.apply_answer("v=0\r\no=answer\r\n"));
    ASSERT_TRUE(media.take_command().has_value());
    EXPECT_FALSE(media.ready());
    ASSERT_TRUE(media.connected_received(1));
    EXPECT_TRUE(media.ready());
    EXPECT_FALSE(media.connected_received(1));
    ASSERT_TRUE(media.start_capture("input"));
    EXPECT_FALSE(media.start_capture("second"));
    media.stop_capture();
    ASSERT_TRUE(media.start_playback("reply", 9));
    EXPECT_EQ(media.current_response(), "reply");
    EXPECT_TRUE(media.finish_reply(0));
    EXPECT_FALSE(media.drained_received(8, 1));
    ASSERT_TRUE(media.drained_received(9, 1));
    EXPECT_EQ(media.drained_epoch(), std::optional<std::uint64_t>(9));
    media.stop_playback(8);
    EXPECT_EQ(media.current_response(), "reply");
    media.stop_playback(9);
    EXPECT_TRUE(media.current_response().empty());
    EXPECT_FALSE(media.drained_epoch().has_value());
    media.close();
    EXPECT_FALSE(media.ready());
}

TEST(BrowserWebRTCMedia, CommandsAreBoundedAndSDPIsEscaped) {
    BrowserWebRTCMedia media;
    VoiceMedia selected;
    selected.transport = VoiceTransport::WebRTC;
    selected.audio = {48000, 1, 20};
    ASSERT_TRUE(media.configure(selected));
    EXPECT_EQ(media.take_command(), std::optional<std::string>(R"({"kind":"webrtcConfigure","peerRevision":"1"})"));
    ASSERT_TRUE(media.offer_received("v=0\r\no=\"offer\"\r\n", 1));
    media.take_offer();
    ASSERT_TRUE(media.apply_answer("v=0\r\no=\"answer\"\r\n"));
    const auto answer = media.take_command();
    ASSERT_TRUE(answer.has_value());
    EXPECT_NE(answer->find(R"(\"answer\")"), std::string::npos);
    ASSERT_TRUE(media.connected_received(1));
    EXPECT_FALSE(media.set_volume(0));
    EXPECT_TRUE(media.request_volume("tool-1", 0));
    EXPECT_EQ(media.take_command(),
              std::optional<std::string>(R"({"kind":"webrtcVolume","id":"tool-1","volume":0})"));
    EXPECT_EQ(media.volume(), 100U);
    EXPECT_FALSE(media.request_volume("tool-2", 20));
    EXPECT_FALSE(media.volume_applied("tool-other", 0));
    EXPECT_FALSE(media.volume_applied("tool-1", 20));
    EXPECT_TRUE(media.volume_applied("tool-1", 0));
    EXPECT_EQ(media.take_volume_applied()->percent, 0U);
    EXPECT_FALSE(media.take_volume_applied());
    EXPECT_FALSE(media.set_volume(101));
    EXPECT_EQ(media.volume(), 0U);
}
}  // namespace ur::device

namespace ur::device {
// connected_media建立真实端口回调顺序；模拟浏览器事实不代替网络ICE验收。
static void connected_media(BrowserWebRTCMedia& media) {
    VoiceMedia selected;
    selected.transport = VoiceTransport::WebRTC;
    selected.audio = {48000, 1, 20};
    ASSERT_TRUE(media.configure(selected));
    ASSERT_TRUE(media.take_command());
    ASSERT_TRUE(media.offer_received("v=0\r\no=first\r\n", 1));
    ASSERT_TRUE(media.take_offer());
    ASSERT_TRUE(media.apply_answer("v=0\r\no=first-answer\r\n"));
    ASSERT_TRUE(media.take_command());
    ASSERT_TRUE(media.connected_received(1));
}
// 原断连必须由核心授予相邻代次后恢复；命令入队不等于已经交给浏览器。
TEST(BrowserWebRTCMedia, RecoveryRequiresDisconnectedAndDeliveredAdjacentRevision) {
    BrowserWebRTCMedia media;
    connected_media(media);
    EXPECT_FALSE(media.begin_recovery(2));
    ASSERT_TRUE(media.disconnected_received(1));
    ASSERT_TRUE(media.recovery_needed());
    EXPECT_FALSE(media.ready());
    EXPECT_FALSE(media.begin_recovery(1));
    EXPECT_FALSE(media.begin_recovery(3));
    ASSERT_TRUE(media.begin_recovery(2));
    EXPECT_FALSE(media.recovery_needed());
    EXPECT_FALSE(media.offer_received("v=0\r\no=new\r\n", 2));
    EXPECT_EQ(media.take_command(), std::optional<std::string>(
        R"({"kind":"webrtcRecover","peerRevision":"2"})"));
    ASSERT_TRUE(media.offer_received("v=0\r\no=new\r\n", 2));
    ASSERT_TRUE(media.take_offer());
    ASSERT_TRUE(media.apply_answer("v=0\r\no=new-answer\r\n"));
    ASSERT_TRUE(media.take_command());
    ASSERT_TRUE(media.connected_received(2));
    EXPECT_TRUE(media.ready());
}
// 旧事实返回已消费而非关闭整个宿主；未知未来代次不能取得当前网络事实。
TEST(BrowserWebRTCMedia, RecoveryIgnoresOldFactsWithoutChangingNewPeer) {
    BrowserWebRTCMedia media;
    connected_media(media);
    ASSERT_TRUE(media.start_playback("old", 9));
    ASSERT_TRUE(media.take_command());
    ASSERT_TRUE(media.disconnected_received(1));
    ASSERT_TRUE(media.begin_recovery(2));
    ASSERT_TRUE(media.take_command());
    EXPECT_TRUE(media.offer_received("v=0\r\no=old\r\n", 1));
    EXPECT_FALSE(media.take_offer());
    EXPECT_TRUE(media.connected_received(1));
    EXPECT_TRUE(media.disconnected_received(1));
    EXPECT_TRUE(media.drained_received(9, 1));
    EXPECT_FALSE(media.drained_epoch());
    EXPECT_FALSE(media.ready());
    EXPECT_FALSE(media.offer_received("v=0\r\no=future\r\n", 3));
    EXPECT_FALSE(media.connected_received(3));
    EXPECT_FALSE(media.disconnected_received(0));
    EXPECT_FALSE(media.drained_received(9, 3));
    EXPECT_TRUE(media.poll(1));
}
// 输出票据和未完成的物理音量请求在Peer恢复中保持原身份，不伪造已释放或已应用。
TEST(BrowserWebRTCMedia, RecoveryPreservesPhysicalGateAndVolumeWithoutPlaybackIntent) {
    BrowserWebRTCMedia media;
    ASSERT_TRUE(media.enable_output_gate());
    connected_media(media);
    ASSERT_TRUE(media.request_output_gate(false, 10));
    ASSERT_TRUE(media.take_command());
    ASSERT_TRUE(media.output_gated(1, false, 11));
    ASSERT_TRUE(media.request_volume("original-volume", 40));
    ASSERT_TRUE(media.take_command());
    ASSERT_TRUE(media.start_capture("original-input"));
    ASSERT_TRUE(media.take_command());
    ASSERT_TRUE(media.start_playback("original-response", 1));
    ASSERT_TRUE(media.take_command());
    ASSERT_TRUE(media.disconnected_received(1));
    EXPECT_TRUE(media.current_response().empty());
    ASSERT_TRUE(media.begin_recovery(2));
    ASSERT_TRUE(media.take_command());
    EXPECT_TRUE(media.output_gate_confirmed(1, false));
    EXPECT_FALSE(media.request_output_gate(true, 12));
    EXPECT_EQ(media.volume(), 100U);
    ASSERT_TRUE(media.volume_applied("original-volume", 40));
    ASSERT_EQ(media.take_volume_applied()->percent, 40U);
    ASSERT_TRUE(media.offer_received("v=0\r\no=new\r\n", 2));
    ASSERT_TRUE(media.take_offer());
    ASSERT_TRUE(media.apply_answer("v=0\r\no=answer\r\n"));
    ASSERT_TRUE(media.take_command());
    ASSERT_TRUE(media.connected_received(2));
    EXPECT_TRUE(media.current_response().empty());
    ASSERT_TRUE(media.start_capture("original-input"));
    EXPECT_NE(media.take_command()->find(R"("peerRevision":"2")"), std::string::npos);
}
// close取消恢复资格但不改变原末端关闭票据；未来回调不可复活本会话。
TEST(BrowserWebRTCMedia, RecoveryClosedAndPrematureFactsCannotReviveMedia) {
    BrowserWebRTCMedia media;
    EXPECT_FALSE(media.disconnected_received(1));
    connected_media(media);
    ASSERT_TRUE(media.disconnected_received(1));
    ASSERT_TRUE(media.begin_recovery(2));
    media.close();
    EXPECT_FALSE(media.begin_recovery(3));
    EXPECT_FALSE(media.connected_received(2));
    EXPECT_FALSE(media.offer_received("v=0\r\no=late\r\n", 2));
    EXPECT_TRUE(media.connected_received(1));
    EXPECT_FALSE(media.ready());
}
}  // namespace ur::device

namespace ur::device {
// 原端口能力默认关闭；真实票据确认不能被命令领取或网络Connected代替。
TEST(BrowserWebRTCMedia, OutputGateBootAndActualReceiptAreIndependentOfPeer) {
    BrowserWebRTCMedia media;
    EXPECT_FALSE(media.request_output_gate(false, 10));
    ASSERT_TRUE(media.enable_output_gate());
    EXPECT_FALSE(media.enable_output_gate());
    EXPECT_FALSE(media.request_output_gate(true, 10));
    const auto boot = media.request_output_gate(false, 10);
    ASSERT_EQ(boot, std::optional<std::uint64_t>(1));
    EXPECT_FALSE(media.output_gate_confirmed(1, false));
    ASSERT_EQ(media.take_command(), std::optional<std::string>(
                                        R"({"kind":"playbackGate","ticket":"1","enabled":false})"));
    ASSERT_TRUE(media.output_gated(1, false, 11));
    EXPECT_TRUE(media.output_gate_confirmed(1, false));
    EXPECT_TRUE(media.output_gated(1, false, 12));
    EXPECT_FALSE(media.request_output_gate(true, 12));
}
// 未交付、错误当前事实、边界到期及回退均安全失败；旧事实不能盖住新操作。
TEST(BrowserWebRTCMedia, OutputGateUndeliveredDeadlineAndOldFacts) {
    for (int scenario = 0; scenario < 4; ++scenario) {
        BrowserWebRTCMedia media;
        ASSERT_TRUE(media.enable_output_gate());
        ASSERT_TRUE(media.request_output_gate(false, 10));
        if (scenario != 0) {
            ASSERT_TRUE(media.take_command());
        }
        if (scenario == 0) {
            EXPECT_FALSE(media.output_gated(1, false, 11));
        }
        if (scenario == 1) {
            EXPECT_FALSE(media.output_gated(1, true, 11));
        }
        if (scenario == 2) {
            EXPECT_FALSE(media.output_gated(1, false, 2010));
        }
        if (scenario == 3) {
            EXPECT_FALSE(media.output_gated(1, false, 9));
        }
        EXPECT_FALSE(media.output_gate_confirmed(1, false));
        EXPECT_FALSE(media.poll(2011));
    }
    BrowserWebRTCMedia media;
    ASSERT_TRUE(media.enable_output_gate());
    ASSERT_TRUE(media.request_output_gate(false, 10));
    ASSERT_TRUE(media.take_command());
    ASSERT_TRUE(media.output_gated(1, false, 11));
    ASSERT_TRUE(media.request_output_gate(false, 12));
    ASSERT_TRUE(media.take_command());
    EXPECT_TRUE(media.output_gated(1, false, 13));
    EXPECT_FALSE(media.output_gate_confirmed(2, false));
    EXPECT_TRUE(media.output_gated(2, false, 14));
}
// 门控不停止原采集/回复意图，父Peer退休后仍可沿原端口收录独立false释放。
TEST(BrowserWebRTCMedia, OutputGateDoesNotStopCaptureOrRetireBeforeRelease) {
    BrowserWebRTCMedia media;
    ASSERT_TRUE(media.enable_output_gate());
    VoiceMedia selected;
    selected.transport = VoiceTransport::WebRTC;
    selected.audio = {48000, 1, 20};
    ASSERT_TRUE(media.configure(selected));
    ASSERT_TRUE(media.take_command());
    ASSERT_TRUE(media.offer_received("v=0\r\no=offer\r\n", 1));
    ASSERT_TRUE(media.take_offer());
    ASSERT_TRUE(media.apply_answer("v=0\r\no=answer\r\n"));
    ASSERT_TRUE(media.take_command());
    ASSERT_TRUE(media.connected_received(1));
    ASSERT_TRUE(media.start_capture("input"));
    ASSERT_TRUE(media.take_command());
    ASSERT_TRUE(media.start_playback("reply", 1));
    ASSERT_TRUE(media.take_command());
    ASSERT_TRUE(media.request_output_gate(false, 10));
    ASSERT_TRUE(media.take_command());
    ASSERT_TRUE(media.output_gated(1, false, 11));
    EXPECT_EQ(media.current_response(), "reply");
    EXPECT_FALSE(media.start_capture("other"));
    ASSERT_TRUE(media.request_output_gate(true, 12));
    ASSERT_TRUE(media.take_command());
    ASSERT_TRUE(media.output_gated(2, true, 13));
    media.close();
    ASSERT_TRUE(media.take_command());
    ASSERT_TRUE(media.request_output_gate(false, 14));
    ASSERT_TRUE(media.take_command());
    EXPECT_TRUE(media.output_gated(3, false, 15));
    EXPECT_TRUE(media.output_gate_confirmed(3, false));
    EXPECT_FALSE(media.request_output_gate(true, 16));
}
// Peer已关不提供原末端false；原未交付/已到期票据仍不能被close或重复请求变成成功。
TEST(BrowserWebRTCMedia, RetiredPeerDoesNotInventPendingOutputRelease) {
    BrowserWebRTCMedia media;
    ASSERT_TRUE(media.enable_output_gate());
    ASSERT_TRUE(media.request_output_gate(false, 10));
    media.close();
    EXPECT_FALSE(media.output_gate_confirmed(1, false));
    EXPECT_FALSE(media.request_output_gate(false, 11));
    ASSERT_TRUE(media.take_command());
    EXPECT_FALSE(media.output_gate_confirmed(1, false));
    EXPECT_FALSE(media.output_gated(1, false, 2010));
    EXPECT_FALSE(media.output_gate_confirmed(1, false));
}
// ready_endpoint只交付公开协商与独立ICE事实，不安装焦点许可或用户输入意图。
static void ready_endpoint(BrowserWebRTCMedia& media) {
    VoiceMedia selected;
    selected.transport = VoiceTransport::WebRTC;
    selected.audio = {48000, 1, 20};
    ASSERT_TRUE(media.configure(selected));
    ASSERT_TRUE(media.take_command());
    ASSERT_TRUE(media.offer_received("v=0\r\no=offer\r\n", 1));
    ASSERT_TRUE(media.take_offer());
    ASSERT_TRUE(media.apply_answer("v=0\r\no=answer\r\n"));
    ASSERT_TRUE(media.take_command());
    ASSERT_TRUE(media.connected_received(1));
}
// 路由绑定、焦点许可和用户输入是独立屏障，错候选scope不能放开原Sender。
TEST(BrowserWebRTCMedia, EndpointRouteRequiresFreshPermissionAndCarriesOriginalScope) {
    BrowserWebRTCMedia media;
    EXPECT_FALSE(media.bind_capture_route("candidate", 1));
    ready_endpoint(media);
    ASSERT_TRUE(media.bind_capture_route("candidate", 1));
    EXPECT_FALSE(media.bind_capture_route("other", 2));
    EXPECT_FALSE(media.start_capture("before-focus"));
    EXPECT_FALSE(media.enable_capture_route("other", 1));
    EXPECT_FALSE(media.enable_capture_route("candidate", 2));
    ASSERT_TRUE(media.enable_capture_route("candidate", 1));
    EXPECT_FALSE(media.take_command());
    ASSERT_TRUE(media.start_capture("fresh-input"));
    const auto start = media.take_command();
    ASSERT_TRUE(start);
    EXPECT_NE(start->find(R"("sessionId":"candidate")"), std::string::npos);
    EXPECT_NE(start->find(R"("routeGeneration":"1")"), std::string::npos);
    EXPECT_FALSE(media.fence_capture_route("other", 1));
    EXPECT_FALSE(media.take_command());
    ASSERT_TRUE(media.fence_capture_route("candidate", 1));
    ASSERT_TRUE(media.take_command());
    EXPECT_FALSE(media.start_capture("old-permission"));
}
// 逻辑终态保留原Peer供原Sender实际null回执，HAL settled后可信宿主才发网络停流。
TEST(BrowserWebRTCMedia, EndpointCloseRetiresQueuedInputBeforeDeferredPeerStop) {
    BrowserWebRTCMedia media;
    ready_endpoint(media);
    ASSERT_TRUE(media.bind_capture_route("candidate", 1));
    ASSERT_TRUE(media.enable_capture_route("candidate", 1));
    ASSERT_TRUE(media.start_capture("queued-before-end"));
    media.close();
    EXPECT_FALSE(media.ready());
    const auto stopped = media.take_command();
    ASSERT_TRUE(stopped);
    EXPECT_NE(stopped->find("captureStop"), std::string::npos);
    EXPECT_FALSE(media.take_command());
    EXPECT_FALSE(media.enable_capture_route("candidate", 1));
    EXPECT_FALSE(media.finish_capture_route_close("other", 1));
    ASSERT_TRUE(media.finish_capture_route_close("candidate", 1));
    const auto peer_stop = media.take_command();
    ASSERT_TRUE(peer_stop);
    EXPECT_NE(peer_stop->find("webrtcStop"), std::string::npos);
    EXPECT_FALSE(media.take_command());
    EXPECT_TRUE(media.finish_capture_route_close("candidate", 1));
    EXPECT_FALSE(media.take_command());
    EXPECT_FALSE(media.start_capture("late-input"));
}
// 撤权也需退休尚未交付的旧开麦命令；新的许可只允许后续新的用户意图。
TEST(BrowserWebRTCMedia, EndpointFenceDropsUndeliveredOldStartWithoutRetiringPeer) {
    BrowserWebRTCMedia media;
    ready_endpoint(media);
    ASSERT_TRUE(media.bind_capture_route("candidate", 1));
    ASSERT_TRUE(media.enable_capture_route("candidate", 1));
    ASSERT_TRUE(media.start_capture("old-input"));
    ASSERT_TRUE(media.fence_capture_route("candidate", 1));
    const auto stop = media.take_command();
    ASSERT_TRUE(stop);
    EXPECT_NE(stop->find("captureStop"), std::string::npos);
    EXPECT_FALSE(media.take_command());
    EXPECT_TRUE(media.ready());
    EXPECT_FALSE(media.start_capture("without-permission"));
    ASSERT_TRUE(media.enable_capture_route("candidate", 1));
    EXPECT_FALSE(media.take_command());
    ASSERT_TRUE(media.start_capture("new-input"));
    EXPECT_TRUE(media.take_command());
}
}  // namespace ur::device
