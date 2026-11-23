// 浏览器PCM驱动契约：可控事件、实际播放确认、迟到隔离、背压与断连终态。
#include "browser_pcm.hpp"
#include <gtest/gtest.h>
#include <array>
#include <limits>
#include "ur/device/output_focus_driver.hpp"

namespace ur::device {
// focus_command 复用正式解析器，不以测试直接填充结构绕过契约校验。
static FocusCommand focus_command(const char* operation, unsigned revision) {
    const std::string body =
        std::string("{\"msgToken\":\"") + operation +
        "\",\"method\":\"focusControl\",\"sessionId\":\"ai\",\"eventSeq\":" +
        std::to_string(revision + 1) + " ,\"params\":{\"operation\":\"" + operation +
        "\",\"expectedRevision\":" + std::to_string(revision) +
        ",\"focusId\":\"output\",\"resources\":[\"speaker\"],\"priority\":"
        "\"conversation\","
        "\"resumePolicy\":\"explicit\"" +
        ((std::string(operation) == "acquire" || std::string(operation) == "restore")
             ? ",\"expiresAt\":10000"
             : "") +
        "}}";
    FocusCommand result;  // 用固定合法报文建立原范围，不代表业务许可。
    EXPECT_EQ(decode_focus_control(WireRoute::Signaling, body, result), WireResult::Ok);
    return result;
}

TEST(BrowserPcm, SharedFocusWaitsForActualSpeakerAndPreservesCapture) {
    BrowserPcm pcm({16000, 1, 20});
    ASSERT_TRUE(pcm.ready({16000, 1, 20}, true));
    ASSERT_TRUE(pcm.begin_capture("input"));
    ASSERT_TRUE(pcm.begin_playback("reply", 1));
    while (pcm.take_command()) {
    }
    OutputFocusDriver focus({{"ai", "output", {"speaker"}, &pcm}}, 0);
    ASSERT_TRUE(focus.initialize(1));
    EXPECT_FALSE(focus.initial_idle());
    auto gate = pcm.take_command();
    ASSERT_TRUE(gate);
    EXPECT_EQ(gate->kind, BrowserAudioCommandKind::OutputGate);
    ASSERT_TRUE(pcm.output_gated(gate->gate_ticket, false, 2));
    ASSERT_TRUE(focus.poll(2));
    ASSERT_TRUE(focus.initial_idle());
    const std::array<const char*, 4> operations{"acquire", "suspend", "restore", "release"};
    const std::array<const char*, 4> states{"acquired", "suspended", "restored", "released"};
    for (unsigned index = 0; index < operations.size(); ++index) {
        const auto command = focus_command(operations[index], index);
        ASSERT_TRUE(focus.begin(command, index + 1));
        EXPECT_FALSE(focus.take_fact("ai", "output"));
        EXPECT_EQ(focus.revision(), index);
        gate = pcm.take_command();
        ASSERT_TRUE(gate);
        const bool enabled = index == 0 || index == 2;
        ASSERT_TRUE(pcm.output_gated(gate->gate_ticket, enabled, 3 + index));
        ASSERT_TRUE(focus.poll(3 + index));
        auto fact = focus.take_fact("ai", "output");
        ASSERT_TRUE(fact);
        EXPECT_TRUE(fact->success);
        EXPECT_EQ(fact->state, states[index]);
        EXPECT_EQ(fact->resources, std::vector<std::string>{"speaker"});
        EXPECT_EQ(fact->revision, index + 1);
        EXPECT_EQ(fact->generation, index + 1);
        EXPECT_EQ(focus.owns(command), enabled);
        const std::vector<std::int16_t> frame(320, 1200);
        ASSERT_TRUE(pcm.captured("input", frame));
        std::vector<std::int16_t> captured;
        EXPECT_EQ(pcm.read_frame(captured), PcmResult::Frame);
        EXPECT_EQ(captured, frame);
    }
    EXPECT_TRUE(pcm.write_frame(std::vector<std::int16_t>(320, 1200), 1));
}

TEST(BrowserPcm, SharedFocusCannotClaimIdleAfterDisconnectOrUnconfirmedGate) {
    BrowserPcm pcm({16000, 1, 20});
    ASSERT_TRUE(pcm.ready({16000, 1, 20}, true));
    OutputFocusDriver focus({{"ai", "output", {"speaker"}, &pcm}}, 0);
    ASSERT_TRUE(focus.initialize(1));
    auto gate = pcm.take_command();
    ASSERT_TRUE(gate);
    EXPECT_FALSE(pcm.output_gated(gate->gate_ticket + 1, false, 2));
    EXPECT_FALSE(focus.initial_idle());
    pcm.disconnect();
    EXPECT_FALSE(pcm.output_gated(gate->gate_ticket, false, 3));
    EXPECT_FALSE(focus.initial_idle());
    EXPECT_FALSE(focus.poll(2001));
    EXPECT_EQ(focus.revision(), 0U);
    EXPECT_FALSE(focus.take_fact("ai", "output"));
}

TEST(BrowserPcm, ExplicitReadinessAndActualAec) {
    BrowserPcm pcm({16000, 1, 20});
    EXPECT_FALSE(pcm.begin_capture("i"));
    EXPECT_FALSE(pcm.begin_playback("r", 1));
    EXPECT_FALSE(pcm.aec_available());
    EXPECT_FALSE(pcm.ready({48000, 1, 20}, true));
    ASSERT_TRUE(pcm.ready({16000, 1, 20}, false));
    EXPECT_FALSE(pcm.aec_available());
    EXPECT_FALSE(pcm.ready({16000, 1, 20}, true));
    pcm.disconnect();
    EXPECT_FALSE(pcm.ready({16000, 1, 20}, true));
}
TEST(BrowserPcm, DuplexCommandsPrecedeMediaAndPlaybackRequiresConsumption) {
    BrowserPcm pcm({16000, 1, 20});
    const std::vector<std::int16_t> frame(320, 1200);
    ASSERT_TRUE(pcm.ready({16000, 1, 20}, true));
    EXPECT_TRUE(pcm.aec_available());
    ASSERT_TRUE(pcm.begin_capture("i"));
    ASSERT_TRUE(pcm.begin_playback("r", 1));
    ASSERT_TRUE(pcm.write_frame(frame, 1));
    EXPECT_FALSE(pcm.take_playback());
    auto control = pcm.take_command();
    ASSERT_TRUE(control);
    EXPECT_EQ(control->kind, BrowserAudioCommandKind::CaptureStart);
    EXPECT_EQ(control->id, "i");
    control = pcm.take_command();
    ASSERT_TRUE(control);
    EXPECT_EQ(control->kind, BrowserAudioCommandKind::PlaybackStart);
    EXPECT_EQ(control->epoch, 1U);
    EXPECT_FALSE(pcm.take_command());
    ASSERT_TRUE(pcm.captured("i", frame));
    auto playback = pcm.take_playback();
    ASSERT_TRUE(playback);
    EXPECT_EQ(playback->samples, frame);
    EXPECT_FALSE(pcm.playback_empty(1));
    std::vector<std::int16_t> captured;
    EXPECT_EQ(pcm.read_frame(captured), PcmResult::Frame);
    EXPECT_EQ(captured, frame);
    EXPECT_TRUE(pcm.played(1, playback->ticket));
    EXPECT_TRUE(pcm.played(1, playback->ticket));
    EXPECT_TRUE(pcm.playback_empty(1));
}
TEST(BrowserPcm, CancelAndPauseAreIndependentAndLateCallbacksCannotRevive) {
    BrowserPcm pcm({24000, 1, 60});
    const std::vector<std::int16_t> frame(1440, -1200);
    ASSERT_TRUE(pcm.ready({24000, 1, 60}, true));
    ASSERT_TRUE(pcm.begin_capture("i"));
    ASSERT_TRUE(pcm.begin_playback("r", 1));
    while (pcm.take_command()) {
    }
    pcm.end_playback(2);
    EXPECT_FALSE(pcm.take_command());
    pcm.end_playback(1);
    pcm.end_playback(1);
    auto control = pcm.take_command();
    ASSERT_TRUE(control);
    EXPECT_EQ(control->kind, BrowserAudioCommandKind::PlaybackStop);
    EXPECT_FALSE(pcm.take_command());
    EXPECT_TRUE(pcm.captured("i", frame));
    ASSERT_TRUE(pcm.begin_playback("r2", 2));
    EXPECT_FALSE(pcm.played(1, 1));
    pcm.end_capture();
    pcm.end_capture();
    EXPECT_FALSE(pcm.captured("i", frame));
    EXPECT_TRUE(pcm.write_frame(frame, 2));
    ASSERT_TRUE(pcm.begin_capture("i2"));
    EXPECT_FALSE(pcm.captured("i", frame));
    EXPECT_TRUE(pcm.captured("i2", frame));
}
TEST(BrowserPcm, CaptureOverflowOrMalformedCurrentFrameFailsClosed) {
    for (const bool malformed : {false, true}) {
        BrowserPcm pcm({16000, 1, 20});
        ASSERT_TRUE(pcm.ready({16000, 1, 20}, true));
        ASSERT_TRUE(pcm.begin_capture("i"));
        const std::vector<std::int16_t> frame(320, 1);
        if (!malformed) {
            for (unsigned i = 0; i < 16; ++i)
                ASSERT_TRUE(pcm.captured("i", frame));
        }
        EXPECT_FALSE(pcm.captured("i", malformed ? std::vector<std::int16_t>{1} : frame));
        std::vector<std::int16_t> read{42};
        EXPECT_EQ(pcm.read_frame(read), PcmResult::Failed);
        EXPECT_TRUE(read.empty());
        EXPECT_FALSE(pcm.take_command());
        EXPECT_FALSE(pcm.aec_available());
        EXPECT_FALSE(pcm.begin_capture("new"));
    }
}
TEST(BrowserPcm, UnsentPlaybackConfirmationIsTerminal) {
    BrowserPcm pcm({16000, 1, 20});
    ASSERT_TRUE(pcm.ready({16000, 1, 20}, false));
    ASSERT_TRUE(pcm.begin_playback("r", 1));
    ASSERT_TRUE(pcm.write_frame(std::vector<std::int16_t>(320), 1));
    EXPECT_FALSE(pcm.played(1, 1));
    EXPECT_FALSE(pcm.take_playback());
    EXPECT_FALSE(pcm.playback_empty(1));
    std::vector<std::int16_t> read;
    EXPECT_EQ(pcm.read_frame(read), PcmResult::Failed);
}
TEST(BrowserPcm, ControlBackpressureAndDisconnectReleaseEverything) {
    BrowserPcm pcm({16000, 1, 20});
    ASSERT_TRUE(pcm.ready({16000, 1, 20}, false));
    for (unsigned i = 0; i < 8; ++i) {
        ASSERT_TRUE(pcm.begin_capture("i"));
        pcm.end_capture();
    }
    EXPECT_FALSE(pcm.begin_capture("overflow"));
    EXPECT_FALSE(pcm.take_command());
    pcm.disconnect();
    pcm.disconnect();
    EXPECT_FALSE(pcm.begin_playback("r", 1));
    EXPECT_FALSE(pcm.write_frame(std::vector<std::int16_t>(320), 1));
    EXPECT_FALSE(pcm.take_playback());
}
TEST(BrowserPcm, VolumeChangesActualDeliveredSamplesButNotCapture) {
    BrowserPcm pcm({16000, 1, 20});
    ASSERT_TRUE(pcm.ready({16000, 1, 20}, false));
    ASSERT_TRUE(pcm.begin_capture("i"));
    ASSERT_TRUE(pcm.begin_playback("r", 1));
    while (pcm.take_command()) {
    }
    std::vector<std::int16_t> frame(320, 32767);
    frame[0] = -32768;
    ASSERT_TRUE(pcm.captured("i", frame));
    ASSERT_TRUE(pcm.write_frame(frame, 1));
    EXPECT_FALSE(pcm.set_volume(101));
    EXPECT_EQ(pcm.volume(), 100U);
    ASSERT_TRUE(pcm.set_volume(50));
    auto output = pcm.take_playback();
    ASSERT_TRUE(output);
    EXPECT_EQ(output->samples[0], -16384);
    EXPECT_EQ(output->samples[1], 16383);
    std::vector<std::int16_t> input;
    EXPECT_EQ(pcm.read_frame(input), PcmResult::Frame);
    EXPECT_EQ(input, frame);
    ASSERT_TRUE(pcm.set_volume(0));
    ASSERT_TRUE(pcm.write_frame(frame, 1));
    output = pcm.take_playback();
    ASSERT_TRUE(output);
    EXPECT_EQ(output->samples, std::vector<std::int16_t>(320));
    pcm.disconnect();
    EXPECT_FALSE(pcm.set_volume(100));
    EXPECT_EQ(pcm.volume(), 0U);
}
// 新扬声器门控确认来自独立音频线程，不借用帧消费或停止采集来假报资源静音。
TEST(BrowserPcm, OutputGateNeedsDeliveredActualFactAndKeepsCapture) {
    BrowserPcm pcm({16000, 1, 20});
    EXPECT_FALSE(pcm.request_output_gate(false, 0));
    ASSERT_TRUE(pcm.ready({16000, 1, 20}, true));
    ASSERT_TRUE(pcm.begin_capture("input"));
    ASSERT_TRUE(pcm.begin_playback("reply", 1));
    const auto gate = pcm.request_output_gate(false, 100);
    ASSERT_TRUE(gate);
    EXPECT_FALSE(pcm.output_gate_confirmed(*gate, false));
    EXPECT_FALSE(pcm.request_output_gate(true, 101));
    ASSERT_TRUE(pcm.take_command());
    ASSERT_TRUE(pcm.take_command());
    const auto command = pcm.take_command();
    ASSERT_TRUE(command);
    EXPECT_EQ(command->kind, BrowserAudioCommandKind::OutputGate);
    EXPECT_EQ(command->gate_ticket, *gate);
    EXPECT_FALSE(command->output_enabled);
    EXPECT_FALSE(pcm.output_gate_confirmed(*gate, false));
    ASSERT_TRUE(pcm.output_gated(*gate, false, 102));
    EXPECT_TRUE(pcm.output_gate_confirmed(*gate, false));
    EXPECT_FALSE(pcm.output_gate_confirmed(*gate, true));
    ASSERT_TRUE(pcm.captured("input", std::vector<std::int16_t>(320, 2000)));
    std::vector<std::int16_t> input;
    EXPECT_EQ(pcm.read_frame(input), PcmResult::Frame);
    EXPECT_EQ(input, std::vector<std::int16_t>(320, 2000));
    EXPECT_EQ(pcm.volume(), 100U);
    pcm.end_playback(1);
    ASSERT_TRUE(pcm.begin_playback("next-reply", 2));
    EXPECT_TRUE(pcm.output_gate_confirmed(*gate, false)) << "新回复不能绕过旧输出门控";
    while (pcm.take_command()) {
    }
    const auto restore = pcm.request_output_gate(true, 200);
    ASSERT_TRUE(restore);
    EXPECT_GT(*restore, *gate);
    EXPECT_FALSE(pcm.output_gate_confirmed(*gate, false));
    ASSERT_TRUE(pcm.take_command());
    EXPECT_TRUE(pcm.output_gated(*gate, false, 201));
    EXPECT_FALSE(pcm.output_gate_confirmed(*restore, true));
    ASSERT_TRUE(pcm.output_gated(*restore, true, 202));
    EXPECT_TRUE(pcm.output_gate_confirmed(*restore, true));
    EXPECT_TRUE(pcm.output_gated(*restore, true, 203));
    pcm.disconnect();
    EXPECT_FALSE(pcm.output_gate_confirmed(*restore, true));
}
// 超时、回退、错状态、未来票据与未交付确认不能授权文件开始输出。
TEST(BrowserPcm, OutputGateFailureIsNotMutedOrReleasedProof) {
    for (const unsigned failure : {0U, 1U, 2U, 3U, 4U}) {
        BrowserPcm pcm({16000, 1, 20});
        ASSERT_TRUE(pcm.ready({16000, 1, 20}, false));
        const auto gate = pcm.request_output_gate(false, 100);
        ASSERT_TRUE(gate);
        if (failure != 4) {
            ASSERT_TRUE(pcm.take_command());
        }
        if (failure == 0) {
            EXPECT_TRUE(pcm.poll_output_gate(2099));
            EXPECT_FALSE(pcm.output_gated(*gate, false, 2100));
        } else if (failure == 1) {
            EXPECT_FALSE(pcm.poll_output_gate(99));
        } else if (failure == 2) {
            EXPECT_FALSE(pcm.output_gated(*gate, true, 101));
        } else if (failure == 3) {
            EXPECT_FALSE(pcm.output_gated(*gate + 1, false, 101));
        } else {
            EXPECT_FALSE(pcm.output_gated(*gate, false, 101));
        }
        EXPECT_FALSE(pcm.output_gate_confirmed(*gate, false));
        EXPECT_FALSE(pcm.ready({16000, 1, 20}, false));
        EXPECT_FALSE(pcm.request_output_gate(false, 2200));
    }
}
// 控制队列和绝对单调截止都有限，失败不会留下可复活的旧静音证明。
TEST(BrowserPcm, OutputGateCapacityAndDeadlineOverflowFailClosed) {
    BrowserPcm pcm({16000, 1, 20});
    ASSERT_TRUE(pcm.ready({16000, 1, 20}, false));
    for (unsigned count = 0; count < 8; ++count) {
        ASSERT_TRUE(pcm.begin_capture("i"));
        pcm.end_capture();
    }
    EXPECT_FALSE(pcm.request_output_gate(false, 100));
    EXPECT_FALSE(pcm.take_command());
    BrowserPcm overflow({16000, 1, 20});
    ASSERT_TRUE(overflow.ready({16000, 1, 20}, false));
    EXPECT_FALSE(
        overflow.request_output_gate(false, std::numeric_limits<std::uint64_t>::max() - 1999));
    EXPECT_FALSE(overflow.poll_output_gate(0));
}
// 开启旁证不改变原32帧容量；交付不等于played，成功写不挪用旧bufferFull原因。
TEST(BrowserPcm, WriteFailureDiagnosticPreservesCapacityAndActualConsumption) {
    BrowserPcm pcm({16000, 1, 20});
    ASSERT_TRUE(pcm.ready({16000, 1, 20}, true));
    ASSERT_TRUE(pcm.begin_playback("reply", 1));
    ASSERT_TRUE(pcm.take_command());
    pcm.enable_failure_diagnostics();
    const std::vector<std::int16_t> frame(320, 1200);  // 原协商20ms单帧，不以静音或假ACK凑容量。
    for (unsigned index = 0; index < 32; ++index) {
        EXPECT_FALSE(pcm.playback_backpressured(1));
        ASSERT_TRUE(pcm.write_frame(frame, 1));
        ASSERT_TRUE(pcm.take_playback());
    }
    EXPECT_TRUE(pcm.playback_backpressured(1));
    EXPECT_FALSE(pcm.playback_backpressured(2));
    EXPECT_FALSE(pcm.write_frame(frame, 1));
    EXPECT_EQ(pcm.last_write_failure(), PcmWriteFailure::BufferFull);
    pcm.enable_failure_diagnostics();
    EXPECT_EQ(pcm.last_write_failure(), PcmWriteFailure::BufferFull);
    ASSERT_TRUE(pcm.played(1, 1));
    EXPECT_FALSE(pcm.playback_backpressured(1));
    ASSERT_TRUE(pcm.write_frame(frame, 1));
    EXPECT_EQ(pcm.last_write_failure(), PcmWriteFailure::None);
    EXPECT_FALSE(pcm.write_frame(frame, 2));
    EXPECT_EQ(pcm.last_write_failure(), PcmWriteFailure::EpochMismatch);
    pcm.disconnect();
    EXPECT_EQ(pcm.last_write_failure(), PcmWriteFailure::EpochMismatch);
    EXPECT_FALSE(pcm.write_frame(frame, 1));
    EXPECT_EQ(pcm.last_write_failure(), PcmWriteFailure::DriverClosed);
}
// 默认或终态后首次启用均不补造历史故障，帧长与代次拒绝仍沿原驱动返回false。
TEST(BrowserPcm, WriteFailureDiagnosticsDefaultOffAndCannotBackfillAfterClose) {
    const std::vector<std::int16_t> frame(320, 1200);  // 普通原格式，用于比较开启前后的失败边界。
    BrowserPcm pcm({16000, 1, 20});
    ASSERT_TRUE(pcm.ready({16000, 1, 20}, true));
    ASSERT_TRUE(pcm.begin_playback("reply", 1));
    EXPECT_FALSE(pcm.write_frame(frame, 2));
    EXPECT_EQ(pcm.last_write_failure(), PcmWriteFailure::None);
    pcm.enable_failure_diagnostics();
    EXPECT_EQ(pcm.last_write_failure(), PcmWriteFailure::None);
    EXPECT_FALSE(pcm.write_frame(std::vector<std::int16_t>(319), 1));
    EXPECT_EQ(pcm.last_write_failure(), PcmWriteFailure::FrameSize);
    BrowserPcm closed({16000, 1, 20});
    closed.disconnect();
    closed.enable_failure_diagnostics();
    EXPECT_FALSE(closed.write_frame(frame, 1));
    EXPECT_EQ(closed.last_write_failure(), PcmWriteFailure::None);
}
}  // namespace ur::device
