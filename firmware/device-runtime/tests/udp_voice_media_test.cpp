// 新 UDP 媒体组合使用可控时间与端口；真实密码、Opus 和网络另在桌面集成覆盖。
#include "ur/device/udp_voice_media.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <deque>

namespace ur::device {
namespace {
// MediaPcm 模拟驱动实际排空，网络末包到达不会自动改变此状态。
struct MediaPcm final : DrainablePcmPort {
    bool write_ok = true;                      // 定向拒绝原实际写入，不改变默认驱动行为。
    bool empty = false;                        // 播放设备队列事实。
    bool blocked = false;                      // 仅模拟当前队列暂时满，不代替消费事实。
    unsigned captures = 0, plays = 0;          // 真实启动次数。
    unsigned frames = 0;                       // 已解码交给设备的帧。
    std::deque<std::uint64_t> stopped_epochs;  // 桌面桥尚未消费的停播命令代次。
    bool begin_capture(std::string_view) override {
        ++captures;
        return true;
    }
    void end_capture() override {}
    PcmResult read_frame(std::vector<std::int16_t>&) override { return PcmResult::Empty; }
    bool begin_playback(std::string_view, std::uint64_t) override {
        ++plays;
        return true;
    }
    bool write_frame(const std::vector<std::int16_t>& pcm, std::uint64_t) override {
        EXPECT_EQ(pcm.size(), 320U);
        ++frames;
        return write_ok;
    }
    bool playback_backpressured(std::uint64_t) const override { return blocked; }
    void end_playback(std::uint64_t epoch) override { stopped_epochs.push_back(epoch); }
    bool playback_empty(std::uint64_t) const override { return empty; }
};
// MediaCodec 固定帧用于驱动策略测试，并允许配置失败注入。
struct MediaCodec final : AudioCodecPort {
    bool accept = true;  // 编码与配置是否成功。
    bool configure(AudioFormat) override { return accept; }
    bool encode(const std::vector<std::int16_t>&, std::vector<std::uint8_t>& out) override {
        out = {1};
        return accept;
    }
    bool decode(const std::vector<std::uint8_t>&, std::vector<std::int16_t>& out) override {
        out.assign(320, 1000);
        return true;
    }
    void reset() override {}
};
// MediaCipher 不模拟密码安全；只让边界测试观察包头与密钥生命周期。
struct MediaCipher final : SessionCipherPort {
    bool installed = false;  // 关闭后禁止继续使用。
    bool install(const std::array<std::uint8_t, 16>&) override {
        installed = true;
        return true;
    }
    void clear() override { installed = false; }
    bool transform(const std::array<std::uint8_t, 16>&, const std::uint8_t* in, std::uint8_t* out,
                   std::size_t size) override {
        std::copy_n(in, size, out);
        return installed;
    }
};
// MediaSocket 保存收发字节；测试显式回显，不在发送时自动宣称网络成功。
struct MediaSocket final : DatagramPort {
    bool opened = false;                            // 句柄状态。
    bool accept = true;                             // 发送结果。
    std::deque<std::vector<std::uint8_t>> packets;  // 未读入站包。
    std::vector<std::vector<std::uint8_t>> sent;    // 全部实际发送字节。
    bool open(std::string_view, std::uint16_t) override {
        opened = true;
        return true;
    }
    bool set_peer(std::string_view, std::uint16_t) override { return true; }
    std::uint16_t local_port() const override { return opened ? 1 : 0; }
    bool send(const std::vector<std::uint8_t>& packet) override {
        sent.push_back(packet);
        return accept;
    }
    ReceiveResult receive(std::vector<std::uint8_t>& packet) override {
        if (packets.empty())
            return ReceiveResult::Empty;
        packet = std::move(packets.front());
        packets.pop_front();
        return ReceiveResult::Received;
    }
    void close() override {
        opened = false;
        packets.clear();
    }
};
// UdpVoiceTest 每例重新创建端口与序号，避免用旧通道模拟重连。
class UdpVoiceTest : public testing::Test {
protected:
    MediaPcm pcm;                                     // 模拟驱动。
    MediaCodec codec;                                 // 模拟编解码。
    MediaCipher cipher;                               // 模拟密码。
    MediaSocket socket;                               // 模拟数据报。
    UdpVoiceMedia media{pcm, codec, socket, cipher};  // 被测可移植组合。
    VoiceMedia config;                                // 会话授权参数。
    std::array<std::uint8_t, 8> reply{2};             // 第一轮回复 nonce。
    void SetUp() override {
        config.host = "127.0.0.1";
        config.port = 12345;
        config.nonce[0] = 1;
    }
    void ready() {
        ASSERT_TRUE(media.configure(config));
        ASSERT_TRUE(media.poll(0));
        socket.packets.push_back(socket.sent.back());
        ASSERT_TRUE(media.poll(1));
        ASSERT_TRUE(media.ready());
    }
    void frame(UdpChannel& sender) {
        std::vector<std::uint8_t> packet;
        ASSERT_EQ(sender.encode({1}, packet), UdpResult::Ok);
        socket.packets.push_back(std::move(packet));
    }
};
TEST_F(UdpVoiceTest, ProbeRequiresExactEchoAndNeverStartsMicrophoneOrSpeaker) {
    ASSERT_TRUE(media.configure(config));
    EXPECT_EQ(media.next_input_sequence(), 0U);
    EXPECT_FALSE(media.start_capture("i"));
    ASSERT_TRUE(media.poll(0));
    auto wrong = socket.sent.back();
    wrong.back() ^= 1;
    socket.packets.push_back(wrong);
    ASSERT_TRUE(media.poll(1));
    EXPECT_FALSE(media.ready());
    ASSERT_TRUE(media.poll(500));
    EXPECT_EQ(socket.sent.size(), 2U);
    EXPECT_EQ(socket.sent[0], socket.sent[1]);
    socket.packets.push_back(socket.sent.back());
    ASSERT_TRUE(media.poll(501));
    EXPECT_TRUE(media.ready());
    EXPECT_EQ(media.next_input_sequence(), 256U);
    EXPECT_EQ(pcm.captures, 0U);
    EXPECT_EQ(pcm.plays, 0U);
    EXPECT_TRUE(media.start_capture("i"));
    media.close();
    EXPECT_FALSE(cipher.installed);
    EXPECT_FALSE(socket.opened);
    EXPECT_FALSE(media.poll(502));
    EXPECT_FALSE(media.timing().first_network_ms);
}
TEST_F(UdpVoiceTest, PresignalObservationRequiresPreConfigureEnableAndNeverStartsMedia) {
    ready();
    EXPECT_FALSE(media.enable_presignal_diagnostics());
    UdpChannel sender(cipher, reply, 128);
    frame(sender);
    ASSERT_TRUE(media.poll(2));
    ASSERT_TRUE(media.prepare_reply(reply, 128));
    EXPECT_FALSE(media.presignal_diagnostic());
    EXPECT_EQ(pcm.captures, 0U);
    EXPECT_EQ(pcm.plays, 0U);
}
TEST_F(UdpVoiceTest,
       PresignalSelectedSnapshotSurvivesCloseWithoutOldNonceAndAdvancesAcrossConfigure) {
    ASSERT_TRUE(media.enable_presignal_diagnostics());
    ready();
    UdpChannel sender(cipher, reply, 128);
    for (unsigned i = 0; i < 169; ++i) {
        frame(sender);
        ASSERT_TRUE(media.poll(i + 2));
    }
    EXPECT_FALSE(media.presignal_diagnostic());
    ASSERT_TRUE(media.prepare_reply(reply, 128));
    auto snapshot = media.presignal_diagnostic();
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot->received, 169U);
    EXPECT_EQ(snapshot->evicted, 153U);
    EXPECT_EQ(snapshot->retained, 16U);
    EXPECT_TRUE(snapshot->complete);
    EXPECT_EQ(snapshot->observation, 1U);
    EXPECT_EQ(pcm.captures, 0U);
    EXPECT_EQ(pcm.plays, 0U);
    EXPECT_FALSE(media.prepare_reply(reply, 128));
    EXPECT_EQ(media.presignal_diagnostic()->observation, 1U);
    media.close();
    EXPECT_EQ(media.presignal_diagnostic()->received, 169U);
    ready();
    ASSERT_TRUE(media.prepare_reply(reply, 128));
    snapshot = media.presignal_diagnostic();
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot->observation, 2U);
    EXPECT_EQ(snapshot->received, 0U);
    EXPECT_EQ(snapshot->evicted, 0U);
    EXPECT_EQ(snapshot->retained, 0U);
    EXPECT_TRUE(snapshot->complete);
}
// 原探测尝试预算不延长，实际第三次等待后的false要在close之前保存。
TEST_F(UdpVoiceTest, FailureDiagnosticsObserveOriginalProbeDeadline) {
    media.enable_failure_diagnostics();
    ASSERT_TRUE(media.configure(config));
    ASSERT_TRUE(media.poll(0));
    ASSERT_TRUE(media.poll(500));
    ASSERT_TRUE(media.poll(1000));
    EXPECT_FALSE(media.poll(1500));
    EXPECT_FALSE(socket.opened);
    EXPECT_EQ(socket.sent.size(), 3U);
    EXPECT_EQ(media.failure_diagnostic(), UdpFailure::ProbeDeadline);
}
// 原泵销毁前复制具体类别，纯getter和重复close不能重开媒体或覆盖首因。
TEST_F(UdpVoiceTest, FailureDiagnosticsCopyOriginalPumpBeforeClose) {
    media.enable_failure_diagnostics();
    ready();
    ASSERT_TRUE(media.prepare_reply(reply, 128));
    ASSERT_TRUE(media.start_playback("r", 1));
    UdpChannel sender(cipher, reply, 128);  // 原下行回复序号编码。
    frame(sender);
    pcm.write_ok = false;
    EXPECT_FALSE(media.poll(2));
    EXPECT_FALSE(socket.opened);
    EXPECT_FALSE(cipher.installed);
    EXPECT_EQ(pcm.frames, 1U);
    EXPECT_EQ(media.failure_diagnostic(), UdpFailure::PcmWrite);
    media.close();
    media.enable_failure_diagnostics();
    EXPECT_FALSE(media.poll(3));
    EXPECT_EQ(media.failure_diagnostic(), UdpFailure::PcmWrite);
    EXPECT_EQ(pcm.frames, 1U);
}
// 默认关闭不追溯旧失败；正常close后的false也不得伪造unconfigured故障。
TEST_F(UdpVoiceTest, FailureDiagnosticsDefaultOffAndNormalCloseDoNotForgeFailure) {
    ASSERT_TRUE(media.configure(config));
    ASSERT_TRUE(media.poll(0));
    ASSERT_TRUE(media.poll(500));
    ASSERT_TRUE(media.poll(1000));
    EXPECT_FALSE(media.poll(1500));
    media.enable_failure_diagnostics();
    EXPECT_FALSE(media.poll(1501));
    EXPECT_FALSE(media.failure_diagnostic());
}
TEST_F(UdpVoiceTest, FailureDiagnosticsNormalCloseHasNoFailureAndCannotBackfill) {
    media.enable_failure_diagnostics();
    ready();
    media.close();
    EXPECT_FALSE(media.poll(2));
    EXPECT_FALSE(media.failure_diagnostic());
}
TEST_F(UdpVoiceTest, FailureDiagnosticsObserveRollbackWithoutRefreshingProbeBudget) {
    media.enable_failure_diagnostics();
    ready();
    EXPECT_FALSE(media.poll(0));
    EXPECT_EQ(media.failure_diagnostic(), UdpFailure::ClockRollback);
    EXPECT_FALSE(socket.opened);
    EXPECT_FALSE(media.poll(100000));
    EXPECT_EQ(media.failure_diagnostic(), UdpFailure::ClockRollback);
}
// 不执行驱动的名称映射，非法枚举必须空而非unknown兜底。
TEST(UdpFailureNames, FixedNamesAndInvalidValue) {
    const std::array<std::string_view, 15> names{
        // 与声明的固定闭集完全一致。
        "unconfigured", "clockRollback",  "probeReceive",   "probeSend",     "probeDeadline",
        "captureRead",  "captureEncode",  "captureEncrypt", "captureSend",   "replyReceive",
        "replyCrypto",  "replyBeyondEnd", "pcmWrite",       "playbackStall", "unknown"};
    for (std::size_t i = 0; i < names.size(); ++i)
        EXPECT_EQ(udp_failure_name(static_cast<UdpFailure>(i)), names[i]);
    EXPECT_TRUE(udp_failure_name(static_cast<UdpFailure>(-1)).empty());
}
// MQTT末包通知早于实际排空时，满队列等待不能冒充网络尾包丢失；真实恢复后仍给UDP迟到预算。
TEST_F(UdpVoiceTest, BackpressureDoesNotMarkPendingTailMissingBeforeReceiveCanResume) {
    ready();
    ASSERT_TRUE(media.prepare_reply(reply, 128));
    ASSERT_TRUE(media.start_playback("r", 1));
    ASSERT_TRUE(media.finish_reply(256));
    pcm.blocked = true;
    ASSERT_TRUE(media.poll(2));
    ASSERT_TRUE(media.poll(301));
    pcm.blocked = false;
    pcm.empty = true;
    ASSERT_TRUE(media.poll(302));
    EXPECT_FALSE(media.drained_epoch());  // 原端到端250ms绝对计时错误会在此假排空。
    UdpChannel sender(cipher, reply,
                      128);  // 容量恢复后实际原回复两包，不重编号或补造帧。
    frame(sender);
    frame(sender);
    ASSERT_TRUE(media.poll(303));
    EXPECT_EQ(media.timing().decoded_frames, 2U);
    EXPECT_EQ(media.drained_epoch(), 1U);
    media.stop_playback(1);
    const auto stopped = media.take_stopped_timing(1);
    ASSERT_TRUE(stopped);
    EXPECT_TRUE(stopped->observed_complete);
}
TEST_F(UdpVoiceTest, FutureReplyBufferedOldReplyDroppedAndDriverDrainRequired) {
    ready();
    UdpChannel sender(cipher, reply, 128);
    frame(sender);
    ASSERT_TRUE(media.poll(2));
    EXPECT_EQ(pcm.frames, 0U);
    ASSERT_TRUE(media.prepare_reply(reply, 128));
    ASSERT_TRUE(media.start_playback("r1", 1));
    frame(sender);
    ASSERT_TRUE(media.finish_reply(256));
    ASSERT_TRUE(media.poll(3));
    EXPECT_EQ(pcm.frames, 2U);
    EXPECT_EQ(media.timing().first_playable_ms, 3U);
    EXPECT_FALSE(media.drained_epoch());
    pcm.empty = true;
    EXPECT_EQ(media.drained_epoch(), 1U);
    media.stop_playback(2);
    EXPECT_EQ(media.drained_epoch(), 1U);
    media.stop_playback(1);
    EXPECT_FALSE(media.drained_epoch());
    EXPECT_FALSE(media.prepare_reply(reply, 128));
    std::array<std::uint8_t, 8> next{3};
    ASSERT_TRUE(media.prepare_reply(next, 128));
    ASSERT_TRUE(media.start_playback("r2", 2));
    frame(sender);
    UdpChannel sender2(cipher, next, 128);
    frame(sender2);
    ASSERT_TRUE(media.poll(4));
    EXPECT_EQ(pcm.frames, 3U);
    EXPECT_FALSE(media.finish_reply(0));
    EXPECT_FALSE(media.finish_reply(1));
    ASSERT_TRUE(media.finish_reply(128));
    EXPECT_TRUE(media.finish_reply(128));
    EXPECT_FALSE(media.finish_reply(256));
    EXPECT_EQ(media.drained_epoch(), 2U);
}
TEST_F(UdpVoiceTest, StoppedReplyTimingMustSurviveFollowingReplyStart) {
    ready();
    ASSERT_TRUE(media.prepare_reply(reply, 128));
    ASSERT_TRUE(media.start_playback("r1", 1));
    UdpChannel sender(cipher, reply, 128);
    frame(sender);
    ASSERT_TRUE(media.poll(2));
    ASSERT_EQ(media.timing().network_packets, 1U);
    media.stop_playback(1);  // 桌面主循环仍未消费停播命令。
    std::array<std::uint8_t, 8> next{3};
    ASSERT_TRUE(media.prepare_reply(next, 128));
    ASSERT_TRUE(media.start_playback("r2", 2));  // 下一回复将当前计数清零。
    ASSERT_EQ(pcm.stopped_epochs.front(), 1U);
    EXPECT_EQ(media.timing().network_packets, 0U);
    const auto stopped = media.take_stopped_timing(pcm.stopped_epochs.front());
    ASSERT_TRUE(stopped);
    EXPECT_EQ(stopped->network_packets, 1U);
    EXPECT_EQ(stopped->decoded_frames, 1U);
    EXPECT_EQ(stopped->non_silent_frames, 1U);
    EXPECT_FALSE(stopped->observed_complete);    // 未收到末包信令不能宣称音频完整。
    EXPECT_FALSE(media.take_stopped_timing(1));  // 最终事实只能领取一次。
}
TEST_F(UdpVoiceTest, StoppedTimingQueueIsBoundedOrderedAndClearedOnClose) {
    ready();
    for (std::uint64_t epoch = 1; epoch <= 9; ++epoch) {
        std::array<std::uint8_t, 8> nonce{};
        nonce[0] = static_cast<std::uint8_t>(epoch + 1);
        ASSERT_TRUE(media.prepare_reply(nonce, 128));
        ASSERT_TRUE(media.start_playback("r", epoch));
        media.stop_playback(epoch);
    }
    EXPECT_FALSE(media.take_stopped_timing(1));  // 超过八条后旧快照被丢弃，不会错配到新回复。
    for (std::uint64_t epoch = 2; epoch <= 9; ++epoch)
        ASSERT_TRUE(media.take_stopped_timing(epoch));
    EXPECT_FALSE(media.take_stopped_timing(9));
    std::array<std::uint8_t, 8> next{};
    next[0] = 11;
    ASSERT_TRUE(media.prepare_reply(next, 128));
    ASSERT_TRUE(media.start_playback("r", 10));
    media.stop_playback(10);
    media.close();
    EXPECT_FALSE(media.take_stopped_timing(10));  // 断连后不伪造最终媒体计数。
}
TEST_F(UdpVoiceTest, MissingTailRecordedSeparatelyFromPlaybackDrain) {
    ready();
    ASSERT_TRUE(media.prepare_reply(reply, 128));
    ASSERT_TRUE(media.start_playback("r", 1));
    ASSERT_TRUE(media.finish_reply(256));
    pcm.empty = true;
    ASSERT_TRUE(media.poll(250));
    EXPECT_FALSE(media.drained_epoch());
    ASSERT_TRUE(media.poll(251));
    EXPECT_EQ(media.drained_epoch(), 1U);
    EXPECT_TRUE(media.missing_tail());
    media.stop_playback(1);
    const auto stopped = media.take_stopped_timing(1);
    ASSERT_TRUE(stopped);
    EXPECT_FALSE(stopped->observed_complete);
}
TEST_F(UdpVoiceTest, FinalObservationRejectsMiddleGapButAcceptsCompleteReply) {
    ready();
    ASSERT_TRUE(media.prepare_reply(reply, 128));
    ASSERT_TRUE(media.start_playback("gap", 1));
    UdpChannel sender(cipher, reply, 128);
    frame(sender);
    std::vector<std::uint8_t> skipped;
    ASSERT_EQ(sender.encode({1}, skipped), UdpResult::Ok);
    frame(sender);
    ASSERT_TRUE(media.poll(2));
    ASSERT_TRUE(media.finish_reply(384));
    pcm.empty = true;
    EXPECT_EQ(media.drained_epoch(), 1U);
    media.stop_playback(1);
    const auto incomplete = media.take_stopped_timing(1);
    ASSERT_TRUE(incomplete);
    EXPECT_EQ(incomplete->missing_packets, 1U);
    EXPECT_FALSE(incomplete->observed_complete);

    std::array<std::uint8_t, 8> next{3};
    ASSERT_TRUE(media.prepare_reply(next, 128));
    ASSERT_TRUE(media.start_playback("complete", 2));
    UdpChannel next_sender(cipher, next, 128);
    frame(next_sender);
    ASSERT_TRUE(media.poll(3));
    ASSERT_TRUE(media.finish_reply(128));
    EXPECT_EQ(media.drained_epoch(), 2U);
    media.stop_playback(2);
    const auto complete = media.take_stopped_timing(2);
    ASSERT_TRUE(complete);
    EXPECT_EQ(complete->missing_packets, 0U);
    EXPECT_TRUE(complete->observed_complete);
}
TEST_F(UdpVoiceTest, FirstPacketLossIsVisibleFromAnnouncedSequence) {
    ready();
    ASSERT_TRUE(media.prepare_reply(reply, 128));
    ASSERT_TRUE(media.start_playback("first-gap", 1));
    UdpChannel sender(cipher, reply, 128);
    std::vector<std::uint8_t> lost;
    ASSERT_EQ(sender.encode({1}, lost), UdpResult::Ok);
    frame(sender);
    ASSERT_TRUE(media.poll(2));
    ASSERT_TRUE(media.finish_reply(256));
    pcm.empty = true;
    EXPECT_EQ(media.drained_epoch(), 1U);
    media.stop_playback(1);
    const auto stopped = media.take_stopped_timing(1);
    ASSERT_TRUE(stopped);
    EXPECT_EQ(stopped->network_packets, 1U);
    EXPECT_EQ(stopped->missing_packets, 1U);
    EXPECT_FALSE(stopped->observed_complete);
}
// LastBeforeFirstDoesNotCompleteReply 验证错误末包不结束回复。
TEST_F(UdpVoiceTest, LastBeforeFirstDoesNotCompleteReply) {
    ready();
    ASSERT_TRUE(media.prepare_reply(reply, 256));
    ASSERT_TRUE(media.start_playback("bounded", 1));
    EXPECT_FALSE(media.finish_reply(128));
    EXPECT_FALSE(media.drained_epoch());

    UdpChannel sender(cipher, reply, 128);
    std::vector<std::uint8_t> earlier;
    ASSERT_EQ(sender.encode({1}, earlier), UdpResult::Ok);
    frame(sender);
    ASSERT_TRUE(media.poll(2));
    EXPECT_EQ(pcm.frames, 1U);
    EXPECT_EQ(media.timing().missing_packets, 0U);
    ASSERT_TRUE(media.finish_reply(256));
    pcm.empty = true;
    EXPECT_EQ(media.drained_epoch(), 1U);
    media.stop_playback(1);
    const auto stopped = media.take_stopped_timing(1);
    ASSERT_TRUE(stopped);
    EXPECT_TRUE(stopped->observed_complete);
}
TEST_F(UdpVoiceTest, RetryExhaustionBackwardClockAndConfigFailuresReleaseResources) {
    EXPECT_FALSE(media.configure(config = {}));
    config.host = "127.0.0.1";
    config.port = 12345;
    codec.accept = false;
    EXPECT_FALSE(media.configure(config));
    EXPECT_FALSE(cipher.installed);
    codec.accept = true;
    ASSERT_TRUE(media.configure(config));
    ASSERT_TRUE(media.poll(0));
    ASSERT_TRUE(media.poll(500));
    ASSERT_TRUE(media.poll(1000));
    EXPECT_FALSE(media.poll(1500));
    EXPECT_FALSE(socket.opened);
    EXPECT_FALSE(cipher.installed);
    ready();
    EXPECT_FALSE(media.poll(0));
    EXPECT_FALSE(media.ready());
    ASSERT_TRUE(media.configure(config));
    socket.accept = false;
    EXPECT_FALSE(media.poll(0));
    EXPECT_FALSE(cipher.installed);
}
TEST_F(UdpVoiceTest, NoOutputWithoutFenceAndPacketsBeyondDoneFailClosed) {
    ready();
    EXPECT_FALSE(media.start_playback("r", 1));
    EXPECT_FALSE(media.prepare_reply(config.nonce, 128));
    EXPECT_FALSE(media.finish_reply(0));
    ASSERT_TRUE(media.prepare_reply(reply, 128));
    ASSERT_TRUE(media.start_playback("r", 1));
    EXPECT_FALSE(media.prepare_reply({3}, 128));
    ASSERT_TRUE(media.finish_reply(0));
    UdpChannel sender(cipher, reply, 128);
    frame(sender);
    EXPECT_FALSE(media.poll(2));
    EXPECT_EQ(pcm.frames, 0U);  // 已知末包之后的数据必须在解码/送扬声器前拒绝。
    EXPECT_FALSE(media.ready());
    EXPECT_FALSE(cipher.installed);
}
}  // namespace
}  // namespace ur::device
