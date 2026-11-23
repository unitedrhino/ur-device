// 音频泵单测使用可控驱动，验证真实状态和媒体去向，不用等待网络的时间测试。
#include "ur/device/udp_audio.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <deque>
#include "ur/device/buffered_pcm.hpp"

namespace ur::device {
namespace {
// Pcm 收集播放代次并模拟有界采集、驱动失败；不宣称硬件 AEC。
struct Pcm final : PcmPort {
    bool accept = true;             // 启停/写入结果。
    bool read_failure = false;      // 单独模拟采集故障。
    bool playback_blocked = false;  // 当前代次短暂耗尽容量，不等同写入错误。
    unsigned available = 0;         // 等待采集的帧数。
    unsigned writes = 0;            // 播放队列实际接受的帧数。
    unsigned capture_stops = 0;     // 停采次数。
    unsigned playback_stops = 0;    // 清播放次数。
    std::int16_t sample = 1000;     // 可控采集能量，尾静音必须仍发送但不能移动首音测量起点。
    bool begin_capture(std::string_view) override { return accept; }
    void end_capture() override {
        ++capture_stops;
        available = 0;
    }
    PcmResult read_frame(std::vector<std::int16_t>& pcm) override {
        if (read_failure)
            return PcmResult::Failed;
        if (!available)
            return PcmResult::Empty;
        --available;
        pcm = {sample, sample};
        return PcmResult::Frame;
    }
    bool begin_playback(std::string_view, std::uint64_t) override { return accept; }
    bool playback_backpressured(std::uint64_t) const override { return playback_blocked; }
    bool write_frame(const std::vector<std::int16_t>&, std::uint64_t) override {
        if (accept)
            ++writes;
        return accept;
    }
    void end_playback(std::uint64_t) override { ++playback_stops; }
};
// Codec 将测试帧映射成可辨认的单字节包，真实 Opus 另由桌面集成测试覆盖。
struct Codec final : AudioCodecPort {
    bool encode_ok = true;  // 编码是否成功。
    bool decode_ok = true;  // 解码是否成功。
    unsigned resets = 0;    // 终态清理次数。
    bool configure(AudioFormat) override { return true; }
    bool encode(const std::vector<std::int16_t>&, std::vector<std::uint8_t>& opus) override {
        opus = {1};
        return encode_ok;
    }
    bool decode(const std::vector<std::uint8_t>&, std::vector<std::int16_t>& pcm) override {
        pcm = {1000, 1000};
        return decode_ok;
    }
    void reset() override { ++resets; }
};
// Cipher 仅让本单测观察媒体泵，真实 AES 已知向量另测。
struct Cipher final : AesCtrPort {
    bool ok = true;  // 独立模拟加密驱动故障。
    bool transform(const std::array<std::uint8_t, 16>&, const std::uint8_t* input,
                   std::uint8_t* output, std::size_t size) override {
        std::copy_n(input, size, output);
        return ok;
    }
};
// Socket 队列完全可控，验证 poll 公平性和终态释放。
struct Socket final : DatagramPort {
    std::deque<std::vector<std::uint8_t>> inbound;  // 待收数据报。
    unsigned sent = 0;                              // 已实际发送的包数。
    unsigned closes = 0;                            // 资源关闭次数。
    bool send_ok = true;                            // 本地发送结果。
    ReceiveResult empty = ReceiveResult::Empty;     // 无包时可模拟断网或畸形包。
    bool open(std::string_view, std::uint16_t) override { return true; }
    bool set_peer(std::string_view, std::uint16_t) override { return true; }
    std::uint16_t local_port() const override { return 1; }
    bool send(const std::vector<std::uint8_t>&) override {
        ++sent;
        return send_ok;
    }
    ReceiveResult receive(std::vector<std::uint8_t>& packet) override {
        if (inbound.empty())
            return empty;
        packet = std::move(inbound.front());
        inbound.pop_front();
        return ReceiveResult::Received;
    }
    void close() override {
        ++closes;
        inbound.clear();
    }
};
// PumpTest 生命周期按依赖逆序释放，测试无真实时钟或网络。
class PumpTest : public testing::Test {
protected:
    Pcm pcm;                                     // 模拟采集和播放。
    Codec codec;                                 // 模拟编解码。
    Cipher cipher;                               // 模拟密码适配。
    Socket socket;                               // 模拟数据报。
    UdpChannel incoming{cipher, {}};             // 服务端序号发生器。
    UdpChannel channel{cipher, {}};              // 设备收发通道。
    UdpAudio pump{pcm, codec, socket, channel};  // 被测连接层。
    Session session{pump};                       // 实际半/全双工策略。
    void queue_audio(unsigned count = 1) {
        for (unsigned index = 0; index < count; ++index) {
            std::vector<std::uint8_t> packet;
            ASSERT_EQ(incoming.encode({1}, packet), UdpResult::Ok);
            socket.inbound.push_back(std::move(packet));
        }
    }
};
TEST_F(PumpTest, InputTimingUsesLastSuccessfullySentNonSilentFrameAndResetsPerInput) {
    EXPECT_FALSE(pump.last_non_silent_sent_ms());
    ASSERT_TRUE(pump.start_capture("first"));
    pcm.available = 1;
    ASSERT_TRUE(pump.poll(0));
    EXPECT_EQ(pump.last_non_silent_sent_ms(), 0U);
    pcm.sample = 0;
    pcm.available = 1;
    ASSERT_TRUE(pump.poll(20));
    EXPECT_EQ(pump.last_non_silent_sent_ms(), 0U);
    EXPECT_EQ(socket.sent, 2U);
    pcm.sample = 128;
    pcm.available = 1;
    ASSERT_TRUE(pump.poll(40));
    EXPECT_EQ(pump.last_non_silent_sent_ms(), 40U);
    pump.stop_capture();
    ASSERT_TRUE(pump.start_capture("second"));
    EXPECT_FALSE(pump.last_non_silent_sent_ms());
    socket.send_ok = false;
    pcm.available = 1;
    EXPECT_FALSE(pump.poll(60));
    EXPECT_FALSE(pump.last_non_silent_sent_ms());
}
TEST_F(PumpTest, FullDuplexBargeInPreservesUplinkAndBoundsEachPoll) {
    ASSERT_EQ(session.open("s", Mode::FullDuplex, Pipeline::Realtime, {true, true, true}),
              Result::Applied);
    ASSERT_EQ(session.start_input("s", "i"), Result::Applied);
    ASSERT_EQ(session.begin_response("s", "r", 1), Result::Applied);
    pcm.available = 20;
    queue_audio(20);
    ASSERT_TRUE(pump.poll(100));
    EXPECT_EQ(socket.sent, 8U);
    EXPECT_EQ(pcm.writes, 8U);
    EXPECT_EQ(pump.timing().first_network_ms, 100U);
    EXPECT_EQ(pump.timing().first_playable_ms, 100U);
    EXPECT_EQ(pump.timing().network_packets, 8U);
    EXPECT_EQ(pump.timing().decoded_frames, 8U);
    EXPECT_EQ(pump.timing().non_silent_frames, 8U);
    ASSERT_EQ(session.cancel_response("s", "r"), Result::Applied);
    ASSERT_TRUE(pump.poll(101));
    EXPECT_EQ(socket.sent, 16U);
    EXPECT_EQ(pcm.writes, 8U);
    EXPECT_EQ(pcm.capture_stops, 0U);
    ASSERT_TRUE(pump.poll(102));  // 取消期间消费旧包，不重新送播放队列。
    ASSERT_EQ(session.begin_response("s", "r2", 2), Result::Applied);
    EXPECT_FALSE(pump.timing().first_network_ms);
    queue_audio();
    ASSERT_TRUE(pump.poll(110));
    EXPECT_EQ(pump.timing().first_network_ms, 110U);
    EXPECT_EQ(pump.timing().network_packets, 1U);
    EXPECT_EQ(pump.timing().decoded_frames, 1U);
    EXPECT_EQ(pump.timing().non_silent_frames, 1U);
    EXPECT_FALSE(pump.timing().first_playable_ms);
    queue_audio();
    ASSERT_TRUE(pump.poll(120));
    EXPECT_EQ(pump.timing().first_playable_ms, 120U);
    EXPECT_EQ(pump.timing().network_packets, 2U);
    EXPECT_EQ(pump.timing().decoded_frames, 2U);
    EXPECT_EQ(pump.timing().non_silent_frames, 2U);
}
// 背压只暂停下行领取；实际新语音必须仍发出，释放容量后原包按原顺序解码。
TEST_F(PumpTest, PlaybackBackpressureKeepsCaptureAndResumesWithoutSyntheticConsumption) {
    ASSERT_TRUE(pump.start_capture("i"));
    ASSERT_TRUE(pump.start_playback("r", 1));
    pcm.playback_blocked = true;
    pcm.available = 16;
    queue_audio(2);
    ASSERT_TRUE(pump.poll(0));
    ASSERT_TRUE(pump.poll(1999));
    EXPECT_EQ(socket.sent, 16U);
    EXPECT_EQ(pcm.writes, 0U);
    EXPECT_EQ(socket.inbound.size(), 2U);
    EXPECT_FALSE(pump.timing().first_network_ms);
    pcm.playback_blocked = false;
    ASSERT_TRUE(pump.poll(2000));
    EXPECT_EQ(pcm.writes, 2U);
    EXPECT_EQ(pump.timing().first_playable_ms, 2000U);
    EXPECT_EQ(socket.closes, 0U);
    pcm.playback_blocked = true;
    ASSERT_TRUE(pump.poll(2100));  // 一次实际可写后，后继阻塞使用自己的固定预算。
    ASSERT_TRUE(pump.poll(4099));
}
// 长期停止消费不能变成无限缓存或永久活会话，截止时两向媒体均真实释放。
TEST_F(PumpTest, PlaybackBackpressureTimesOutAndPreservesFirstCause) {
    pump.enable_failure_diagnostics();
    ASSERT_TRUE(pump.start_capture("i"));
    ASSERT_TRUE(pump.start_playback("r", 1));
    pcm.playback_blocked = true;
    ASSERT_TRUE(pump.poll(10));
    ASSERT_TRUE(pump.poll(2009));
    EXPECT_FALSE(pump.poll(2010));
    EXPECT_EQ(pump.failure_diagnostic(), UdpFailure::PlaybackStall);
    EXPECT_EQ(socket.closes, 1U);
    EXPECT_EQ(pcm.capture_stops, 1U);
    EXPECT_EQ(pcm.playback_stops, 1U);
    EXPECT_FALSE(pump.poll(2011));
    EXPECT_EQ(socket.closes, 1U);
}
// 满队列不拦取消；旧包仍消费丢弃，新回复不继承原阻塞计时。
TEST_F(PumpTest, CancelWhileBackpressuredDropsOldAudioAndDoesNotPoisonNextReply) {
    ASSERT_TRUE(pump.start_capture("i"));
    ASSERT_TRUE(pump.start_playback("r", 1));
    pcm.playback_blocked = true;
    queue_audio(2);
    ASSERT_TRUE(pump.poll(0));
    pump.stop_playback(1);
    ASSERT_TRUE(pump.poll(3000));
    EXPECT_TRUE(socket.inbound.empty());
    EXPECT_EQ(pcm.writes, 0U);
    ASSERT_TRUE(pump.start_playback("next", 2));
    ASSERT_TRUE(pump.poll(3001));
    pcm.playback_blocked = false;
    queue_audio(2);
    ASSERT_TRUE(pump.poll(3002));
    EXPECT_EQ(pcm.writes, 2U);
    EXPECT_EQ(pcm.capture_stops, 0U);
}
// 原UDP组合也拒绝倒退的背压时钟，不能用负时间延长失效驱动预算。
TEST_F(PumpTest, PlaybackBackpressureRejectsClockRollback) {
    pump.enable_failure_diagnostics();
    ASSERT_TRUE(pump.start_playback("r", 1));
    pcm.playback_blocked = true;
    ASSERT_TRUE(pump.poll(10));
    EXPECT_FALSE(pump.poll(9));
    EXPECT_EQ(pump.failure_diagnostic(), UdpFailure::ClockRollback);
}
TEST_F(PumpTest, HalfDuplexForBothPipelinesCannotCaptureWhilePlaying) {
    for (auto pipeline : {Pipeline::Cascade, Pipeline::Realtime}) {
        ASSERT_EQ(session.open("s", Mode::HalfDuplex, pipeline, {}), Result::Applied);
        ASSERT_EQ(session.start_input("s", "i"), Result::Applied);
        pcm.available = 1;
        ASSERT_TRUE(pump.poll(0));
        ASSERT_EQ(session.stop_input("s", "i"), Result::Applied);
        ASSERT_EQ(session.begin_response("s", "r", 1), Result::Applied);
        EXPECT_EQ(session.start_input("s", "i2"), Result::Busy);
        queue_audio();
        ASSERT_TRUE(pump.poll(1));
        ASSERT_EQ(session.network_done("s", "r"), Result::Applied);
        EXPECT_TRUE(session.playing());
        ASSERT_EQ(session.playback_drained(session.playback_epoch()), Result::Applied);
        ASSERT_EQ(session.start_input("s", "i2"), Result::Applied);
        session.close();
    }
    EXPECT_EQ(socket.sent, 2U);
    EXPECT_EQ(pcm.writes, 2U);
}
TEST_F(PumpTest, RejectsInvalidStartsAndOldStopCannotStopCurrentPlayback) {
    EXPECT_FALSE(pump.start_capture(""));
    EXPECT_FALSE(pump.start_capture(std::string(129, 'x')));
    EXPECT_FALSE(pump.start_playback("", 1));
    EXPECT_FALSE(pump.start_playback(std::string(129, 'x'), 1));
    EXPECT_FALSE(pump.start_playback("r", 0));
    pcm.accept = false;
    EXPECT_FALSE(pump.start_capture("i"));
    EXPECT_FALSE(pump.start_playback("r", 1));
    pcm.accept = true;
    ASSERT_TRUE(pump.start_capture("i"));
    EXPECT_FALSE(pump.start_capture("other"));
    ASSERT_TRUE(pump.start_playback("r", 1));
    EXPECT_FALSE(pump.start_playback("r2", 2));
    pump.stop_playback(0);
    EXPECT_EQ(pcm.playback_stops, 0U);
    pump.stop_playback(1);
    EXPECT_FALSE(pump.start_playback("old", 1));
    pump.close();
    pump.close();
    EXPECT_EQ(socket.closes, 1U);
    EXPECT_EQ(codec.resets, 1U);
    EXPECT_FALSE(pump.start_capture("closed"));
    EXPECT_FALSE(pump.start_playback("closed", 2));
    EXPECT_FALSE(pump.poll(2));
}
TEST_F(PumpTest, MalformedAndDuplicatePacketsNeverReachPlayback) {
    ASSERT_TRUE(pump.start_playback("r", 1));
    socket.inbound.push_back({1});
    queue_audio();
    socket.inbound.push_back(socket.inbound.back());
    ASSERT_TRUE(pump.poll(0));
    EXPECT_EQ(pcm.writes, 1U);
    codec.decode_ok = false;
    queue_audio();
    ASSERT_TRUE(pump.poll(1));
    codec.decode_ok = true;
    queue_audio();
    ASSERT_TRUE(pump.poll(2));
    EXPECT_FALSE(pump.timing().first_playable_ms);
    queue_audio();
    ASSERT_TRUE(pump.poll(3));
    EXPECT_EQ(pump.timing().first_playable_ms, 3U);
    socket.empty = ReceiveResult::Invalid;
    EXPECT_TRUE(pump.poll(4));  // 畸形包预算耗尽即返回，不无限循环。
}
TEST_F(PumpTest, MissingMiddlePacketIsReportedAndDoesNotCarryIntoNextReply) {
    ASSERT_TRUE(pump.start_playback("first", 1));
    queue_audio();  // 当前回复首包正常到达。
    std::vector<std::uint8_t> skipped, following;
    ASSERT_EQ(incoming.encode({1}, skipped), UdpResult::Ok);
    ASSERT_EQ(incoming.encode({1}, following), UdpResult::Ok);
    socket.inbound.push_back(std::move(following));
    ASSERT_TRUE(pump.poll(10));
    EXPECT_EQ(pump.timing().network_packets, 2U);
    EXPECT_EQ(pump.timing().missing_packets, 1U);
    pump.stop_playback(1);
    ASSERT_TRUE(pump.start_playback("second", 2));
    queue_audio();
    ASSERT_TRUE(pump.poll(20));
    EXPECT_EQ(pump.timing().missing_packets, 0U);
}
TEST_F(PumpTest, CaptureFailureReleasesBothDirections) {
    ASSERT_TRUE(pump.start_capture("i"));
    ASSERT_TRUE(pump.start_playback("r", 1));
    pcm.read_failure = true;
    EXPECT_FALSE(pump.poll(0));
    EXPECT_EQ(pcm.capture_stops, 1U);
    EXPECT_EQ(pcm.playback_stops, 1U);
}
TEST_F(PumpTest, EncodingFailureIsTerminal) {
    ASSERT_TRUE(pump.start_capture("i"));
    pcm.available = 1;
    codec.encode_ok = false;
    EXPECT_FALSE(pump.poll(0));
    EXPECT_EQ(socket.sent, 0U);
}
TEST_F(PumpTest, EncryptionFailureIsTerminal) {
    ASSERT_TRUE(pump.start_capture("i"));
    pcm.available = 1;
    cipher.ok = false;
    EXPECT_FALSE(pump.poll(0));
    EXPECT_EQ(socket.sent, 0U);
}
TEST_F(PumpTest, SendingFailureIsTerminal) {
    ASSERT_TRUE(pump.start_capture("i"));
    pcm.available = 1;
    socket.send_ok = false;
    EXPECT_FALSE(pump.poll(0));
    EXPECT_EQ(socket.closes, 1U);
}
TEST_F(PumpTest, ReceivingFailureIsTerminal) {
    socket.empty = ReceiveResult::Failed;
    EXPECT_FALSE(pump.poll(0));
    EXPECT_EQ(socket.closes, 1U);
}
TEST_F(PumpTest, DecryptionFailureIsTerminal) {
    queue_audio();
    cipher.ok = false;
    EXPECT_FALSE(pump.poll(0));
    EXPECT_EQ(socket.closes, 1U);
}
TEST_F(PumpTest, PlaybackBackpressureIsExplicitFailureNotSilentAudioLoss) {
    ASSERT_TRUE(pump.start_playback("r", 1));
    queue_audio();
    pcm.accept = false;
    EXPECT_FALSE(pump.poll(0));
    EXPECT_EQ(pcm.writes, 0U);
    EXPECT_EQ(pcm.playback_stops, 1U);
}
// 原PCM拒绝仍只关闭一次；开启旁观后应保留原poll首因。
TEST_F(PumpTest, FailureDiagnosticsObserveOriginalPcmFailure) {
    pump.enable_failure_diagnostics();
    ASSERT_TRUE(pump.start_playback("r", 1));
    queue_audio();
    pcm.accept = false;
    EXPECT_FALSE(pump.poll(0));
    EXPECT_EQ(socket.closes, 1U);
    EXPECT_EQ(pcm.playback_stops, 1U);
    EXPECT_EQ(pump.failure_diagnostic(), UdpFailure::PcmWrite);
}
// 每例重新创建原泵，注入同一个原驱动false分支；不读取旁证来驱动关闭。
class PumpFailureTest : public PumpTest, public testing::WithParamInterface<UdpFailure> {};
TEST_P(PumpFailureTest, FirstFailureIsOriginalBranchAndCloseIsNotRepeated) {
    pump.enable_failure_diagnostics();
    ASSERT_TRUE(pump.start_capture("i"));
    ASSERT_TRUE(pump.start_playback("r", 1));
    const auto expected = GetParam();  // 本例原驱动故障类别。
    if (expected == UdpFailure::CaptureRead)
        pcm.read_failure = true;
    else if (expected == UdpFailure::CaptureEncode) {
        pcm.available = 1;
        codec.encode_ok = false;
    } else if (expected == UdpFailure::CaptureEncrypt) {
        pcm.available = 1;
        cipher.ok = false;
    } else if (expected == UdpFailure::CaptureSend) {
        pcm.available = 1;
        socket.send_ok = false;
    } else if (expected == UdpFailure::ReplyReceive)
        socket.empty = ReceiveResult::Failed;
    else {
        queue_audio(expected == UdpFailure::ReplyBeyondEnd ? 2 : 1);
        if (expected == UdpFailure::ReplyCrypto)
            cipher.ok = false;
        else if (expected == UdpFailure::ReplyBeyondEnd)
            ASSERT_TRUE(channel.limit_receive_sequence(1));
        else
            pcm.accept = false;
    }
    EXPECT_FALSE(pump.poll(1));
    EXPECT_EQ(pump.failure_diagnostic(), expected);
    EXPECT_EQ(socket.closes, 1U);
    EXPECT_EQ(pcm.capture_stops, 1U);
    EXPECT_EQ(pcm.playback_stops, 1U);
    pump.enable_failure_diagnostics();
    pump.close();
    EXPECT_FALSE(pump.poll(2));
    EXPECT_EQ(pump.failure_diagnostic(), expected);
    EXPECT_EQ(socket.closes, 1U);
}
INSTANTIATE_TEST_SUITE_P(OriginalFatalBranches, PumpFailureTest,
                         testing::Values(UdpFailure::CaptureRead, UdpFailure::CaptureEncode,
                                         UdpFailure::CaptureEncrypt, UdpFailure::CaptureSend,
                                         UdpFailure::ReplyReceive, UdpFailure::ReplyCrypto,
                                         UdpFailure::ReplyBeyondEnd, UdpFailure::PcmWrite));
// 普通坏包、旧包和解码跳过仍沿原策略继续，不因开启旁证变致命。
TEST_F(PumpTest, FailureDiagnosticsIgnoreNonfatalPacketsAndLateEnable) {
    pump.enable_failure_diagnostics();
    ASSERT_TRUE(pump.start_playback("r", 1));
    socket.inbound.push_back({1});
    queue_audio();
    codec.decode_ok = false;
    EXPECT_TRUE(pump.poll(1));
    EXPECT_FALSE(pump.failure_diagnostic());
    codec.decode_ok = true;
    socket.empty = ReceiveResult::Invalid;
    EXPECT_TRUE(pump.poll(2));
    EXPECT_FALSE(pump.failure_diagnostic());
    pump.close();
    EXPECT_FALSE(pump.poll(3));
    EXPECT_FALSE(pump.failure_diagnostic());
}
TEST_F(PumpTest, FailureDiagnosticsDefaultOffDoesNotBackfillClosedPump) {
    socket.empty = ReceiveResult::Failed;
    EXPECT_FALSE(pump.poll(1));
    EXPECT_FALSE(pump.failure_diagnostic());
    pump.enable_failure_diagnostics();
    EXPECT_FALSE(pump.poll(2));
    EXPECT_FALSE(pump.failure_diagnostic());
    EXPECT_EQ(socket.closes, 1U);
}
// CapacityCodec只替换硬件编解码边界；生产BufferedPcm与UDP泵真实串联33包。
struct CapacityCodec final : AudioCodecPort {
    bool configure(AudioFormat) override { return true; }
    bool encode(const std::vector<std::int16_t>&, std::vector<std::uint8_t>& out) override {
        out = {1};
        return true;
    }
    bool decode(const std::vector<std::uint8_t>&, std::vector<std::int16_t>& out) override {
        out.assign(320, 1000);
        return true;
    }
    void reset() override {}
};
TEST(BufferedPumpFailure, ThirtyThirdFrameWaitsForActualConsumptionWithoutClosingOriginalPump) {
    BufferedPcm pcm({16000, 1, 20});                     // 原32帧容量，不扩缓存。
    CapacityCodec codec;                                 // 显式受控编解码，不称真实Opus。
    Cipher cipher;                                       // 原可控密码端口，不称密码安全验收。
    Socket socket;                                       // 原队列端口，不启动实际网络。
    UdpChannel sender(cipher, {}), channel(cipher, {});  // 真实序号验证。
    UdpAudio pump(pcm, codec, socket, channel);          // 真实原泵。
    pcm.enable_failure_diagnostics();
    pump.enable_failure_diagnostics();
    ASSERT_TRUE(pump.start_playback("r", 1));
    for (unsigned i = 0; i < 33; ++i) {
        std::vector<std::uint8_t> packet;  // 每次由原channel实际编码。
        ASSERT_EQ(sender.encode({1}, packet), UdpResult::Ok);
        socket.inbound.push_back(std::move(packet));
    }
    for (unsigned i = 0; i < 4; ++i)
        ASSERT_TRUE(pump.poll(i));
    ASSERT_TRUE(pcm.take_playback());  // 不冒充实际played，容量仍满。
    ASSERT_TRUE(pump.poll(4));
    EXPECT_EQ(pump.timing().decoded_frames, 32U);
    EXPECT_FALSE(pump.failure_diagnostic());
    EXPECT_EQ(socket.closes, 0U);
    EXPECT_EQ(socket.inbound.size(),
              1U);  // 尚无容量时不能先取走、解码或丢掉第33包。
    ASSERT_TRUE(pcm.played(1, 1));
    ASSERT_TRUE(pump.poll(5));
    EXPECT_EQ(pump.timing().decoded_frames, 33U);
    EXPECT_TRUE(socket.inbound.empty());
    EXPECT_FALSE(pcm.playback_empty(1));  // 排入驱动仍不等于实际播放完成。
}
TEST(SpeechOnset, SilenceEmptyAndResetBreakContinuity) {
    SpeechOnset onset;
    EXPECT_FALSE(onset.observe({1000}));
    EXPECT_FALSE(onset.observe({}));
    EXPECT_FALSE(onset.observe({1000}));
    EXPECT_FALSE(onset.observe({0}));
    EXPECT_FALSE(onset.observe({128}));
    EXPECT_TRUE(onset.observe({-128}));
    EXPECT_TRUE(onset.observe({32767}));
    onset.reset();
    EXPECT_FALSE(onset.observe({1000}));
}
}  // namespace
}  // namespace ur::device
