// 回复 nonce 过滤测试：控制与UDP任意到达顺序、旧回复迟到、资源边界和重复关闭。
#include "ur/device/reply_datagram.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <deque>

namespace ur::device {
namespace {
// QueueDatagram 使用确定性收包队列，不假装提供真实加密或网络验证。
class QueueDatagram final : public DatagramPort {
public:
    std::deque<std::vector<std::uint8_t>> packets;  // 待收到的加密格式数据报。
    unsigned closes = 0;                            // 实际关闭次数。
    unsigned receives = 0;                          // 原底层receive次数，诊断不能增加调用。
    bool failed = false;                            // 网络错误注入。
    bool open(std::string_view, std::uint16_t) override { return true; }
    bool set_peer(std::string_view, std::uint16_t) override { return true; }
    std::uint16_t local_port() const override { return 42; }
    bool send(const std::vector<std::uint8_t>&) override { return !failed; }
    ReceiveResult receive(std::vector<std::uint8_t>& out) override {
        ++receives;
        if (failed)
            return ReceiveResult::Failed;
        if (packets.empty())
            return ReceiveResult::Empty;
        out = std::move(packets.front());
        packets.pop_front();
        return ReceiveResult::Received;
    }
    void close() override { ++closes; }
};
// packet 构建1字节负载的公开格式包，nonce与序号由用例决定。
std::vector<std::uint8_t> packet(std::uint8_t nonce, std::uint8_t sequence = 1) {
    std::vector<std::uint8_t> result(17);
    result[0] = 1;
    result[3] = 1;
    result[4] = nonce;
    result[15] = sequence;
    result[16] = 42;
    return result;
}
// nonce 创建非凭据测试标识。
std::array<std::uint8_t, 8> nonce(std::uint8_t value) { return {value, 0, 0, 0, 0, 0, 0, 0}; }
// 新观察必须准确记录原有限缓存造成的丢包，不把缺失诊断当媒体成功。
TEST(ReplyDatagram, PresignalObservationFreezesActualSelectedNonceEvictions) {
    QueueDatagram socket;
    ReplyDatagram filter(socket);
    ASSERT_TRUE(filter.enable_presignal_diagnostics());
    std::vector<std::uint8_t> out;
    for (unsigned index = 1; index <= 169; ++index) {
        socket.packets.push_back(packet(7, static_cast<std::uint8_t>(index)));
        EXPECT_EQ(filter.receive(out), ReceiveResult::Empty);
    }
    ASSERT_TRUE(filter.select_nonce(nonce(7)));
    const auto snapshot = filter.presignal_diagnostic();
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot->observation, 1U);
    EXPECT_EQ(snapshot->received, 169U);
    EXPECT_EQ(snapshot->evicted, 153U);
    EXPECT_EQ(snapshot->retained, 16U);
    EXPECT_TRUE(snapshot->complete);
}
TEST(ReplyDatagram, PresignalCountersSaturateWithoutWrapOrChangingExistingValue) {
    std::uint32_t value = UINT32_MAX - 1;
    EXPECT_TRUE(detail::presignal_increment(value));
    EXPECT_EQ(value, UINT32_MAX);
    EXPECT_FALSE(detail::presignal_increment(value));
    EXPECT_EQ(value, UINT32_MAX);
    EXPECT_FALSE(detail::presignal_increment(value));
    EXPECT_EQ(value, UINT32_MAX);
}
TEST(ReplyDatagram, PresignalDefaultOffDoesNotChangeCallsOrBuffering) {
    for (bool enabled : {false, true}) {
        QueueDatagram socket;
        ReplyDatagram filter(socket);
        if (enabled) {
            ASSERT_TRUE(filter.enable_presignal_diagnostics());
        }
        std::vector<std::uint8_t> out;
        for (unsigned index = 1; index <= 20; ++index)
            socket.packets.push_back(packet(7, static_cast<std::uint8_t>(index)));
        EXPECT_EQ(filter.receive(out), ReceiveResult::Empty);
        EXPECT_EQ(filter.receive(out), ReceiveResult::Empty);
        EXPECT_EQ(socket.receives, 21U);
        EXPECT_EQ(filter.buffered(), 16U);
        EXPECT_FALSE(filter.presignal_diagnostic());
        ASSERT_TRUE(filter.select_nonce(nonce(7)));
        EXPECT_EQ(socket.receives, 21U);
        EXPECT_EQ(filter.receive(out), ReceiveResult::Received);
        EXPECT_EQ(out, packet(7, 5));
        EXPECT_EQ(socket.receives, 21U);
        EXPECT_EQ(filter.presignal_diagnostic().has_value(), enabled);
    }
}
TEST(ReplyDatagram, PresignalMixedNonceCountsOnlySelectedReplyAndSnapshotIsFrozen) {
    QueueDatagram socket;
    ReplyDatagram filter(socket);
    ASSERT_TRUE(filter.enable_presignal_diagnostics());
    ASSERT_TRUE(filter.enable_presignal_diagnostics());
    std::vector<std::uint8_t> out;
    for (unsigned index = 1; index <= 32; ++index) {
        socket.packets.push_back(packet(index % 2 == 0 ? 2 : 3));
        ASSERT_EQ(filter.receive(out), ReceiveResult::Empty);
    }
    ASSERT_TRUE(filter.select_nonce(nonce(2)));
    auto snapshot = filter.presignal_diagnostic();
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot->received, 16U);
    EXPECT_EQ(snapshot->evicted, 8U);
    EXPECT_EQ(snapshot->retained, 8U);
    EXPECT_TRUE(snapshot->complete);
    ASSERT_TRUE(filter.select_nonce(nonce(2)));
    EXPECT_EQ(filter.presignal_diagnostic()->observation, 1U);
    ASSERT_EQ(filter.receive(out), ReceiveResult::Received);
    EXPECT_EQ(filter.presignal_diagnostic()->retained, 8U);
    ASSERT_TRUE(filter.select_nonce(nonce(3)));
    snapshot = filter.presignal_diagnostic();
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot->observation, 2U);
    EXPECT_EQ(snapshot->received, 16U);
    EXPECT_EQ(snapshot->evicted, 8U);
    EXPECT_EQ(snapshot->retained, 8U);
    EXPECT_TRUE(snapshot->complete);
    EXPECT_FALSE(filter.select_nonce(nonce(2)));
    EXPECT_EQ(filter.presignal_diagnostic()->observation, 2U);
}
TEST(ReplyDatagram, PresignalLateEnableCannotReconstructPastPacketsOrSelectedReply) {
    QueueDatagram socket;
    ReplyDatagram filter(socket);
    std::vector<std::uint8_t> out;
    socket.packets.push_back(packet(1));
    ASSERT_EQ(filter.receive(out), ReceiveResult::Empty);
    EXPECT_FALSE(filter.enable_presignal_diagnostics());
    ASSERT_TRUE(filter.select_nonce(nonce(1)));
    EXPECT_FALSE(filter.enable_presignal_diagnostics());
    EXPECT_FALSE(filter.presignal_diagnostic());
    filter.close();
    EXPECT_FALSE(filter.enable_presignal_diagnostics());
}
TEST(ReplyDatagram, PresignalSlotsOverflowIsExplicitButNeverRejectsBusinessSelection) {
    QueueDatagram socket;
    ReplyDatagram filter(socket);
    ASSERT_TRUE(filter.enable_presignal_diagnostics());
    std::vector<std::uint8_t> out;
    for (std::uint8_t index = 1; index <= 17; ++index) {
        socket.packets.push_back(packet(index));
        ASSERT_EQ(filter.receive(out), ReceiveResult::Empty);
    }
    ASSERT_TRUE(filter.select_nonce(nonce(17)));
    const auto snapshot = filter.presignal_diagnostic();
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot->received, 0U);  // 不知道的统计不能假称零包完整。
    EXPECT_EQ(snapshot->retained, 1U);
    EXPECT_FALSE(snapshot->complete);
    ASSERT_EQ(filter.receive(out), ReceiveResult::Received);
    EXPECT_EQ(out, packet(17));
}
TEST(ReplyDatagram, PresignalSuccessfulSelectionReclaimsSlotsBeyondSixteenReplies) {
    QueueDatagram socket;
    ReplyDatagram filter(socket);
    ASSERT_TRUE(filter.enable_presignal_diagnostics());
    std::vector<std::uint8_t> out;
    for (std::uint8_t index = 1; index <= 40; ++index) {
        socket.packets.push_back(packet(index));
        ASSERT_EQ(filter.receive(out), ReceiveResult::Empty);
        ASSERT_TRUE(filter.select_nonce(nonce(index)));
        ASSERT_TRUE(filter.presignal_diagnostic());
        EXPECT_EQ(filter.presignal_diagnostic()->observation, index);
        EXPECT_EQ(filter.presignal_diagnostic()->received, 1U);
        EXPECT_TRUE(filter.presignal_diagnostic()->complete);
        ASSERT_EQ(filter.receive(out), ReceiveResult::Received);
    }
    filter.close();
    filter.close();
    EXPECT_EQ(socket.closes, 1U);
    EXPECT_EQ(filter.presignal_diagnostic()->observation, 40U);
    EXPECT_FALSE(filter.select_nonce(nonce(41)));
    EXPECT_FALSE(filter.enable_presignal_diagnostics());
}
TEST(ReplyDatagram, FuturePacketsWaitForSignalAndOldReplyNeverCrossesBoundary) {
    QueueDatagram socket;
    ReplyDatagram filter(socket);
    std::vector<std::uint8_t> out;
    socket.packets = {packet(2, 2), packet(1, 1)};
    ASSERT_TRUE(filter.select_nonce(nonce(1)));
    ASSERT_EQ(filter.receive(out), ReceiveResult::Received);
    EXPECT_EQ(out, packet(1, 1));
    EXPECT_EQ(filter.buffered(), 1U);
    ASSERT_TRUE(filter.select_nonce(nonce(2)));
    ASSERT_EQ(filter.receive(out), ReceiveResult::Received);
    EXPECT_EQ(out, packet(2, 2));
    socket.packets = {packet(1, 250), packet(2, 3)};
    ASSERT_EQ(filter.receive(out), ReceiveResult::Received);
    EXPECT_EQ(out, packet(2, 3));
    EXPECT_FALSE(filter.select_nonce(nonce(1)));
    EXPECT_TRUE(filter.select_nonce(nonce(2)));
}
TEST(ReplyDatagram, PreSignalCacheAndWorkBudgetAreBounded) {
    QueueDatagram socket;
    ReplyDatagram filter(socket);
    std::vector<std::uint8_t> out;
    for (unsigned index = 1; index <= 40; ++index)
        socket.packets.push_back(packet(7, static_cast<std::uint8_t>(index)));
    EXPECT_EQ(filter.receive(out), ReceiveResult::Empty);
    EXPECT_EQ(filter.buffered(), 16U);
    EXPECT_EQ(socket.packets.size(), 24U);
    EXPECT_EQ(filter.receive(out), ReceiveResult::Empty);
    EXPECT_EQ(filter.buffered(), 16U);
    ASSERT_TRUE(filter.select_nonce(nonce(7)));
    ASSERT_EQ(filter.receive(out), ReceiveResult::Received);
    EXPECT_EQ(out, packet(7, 17));
}
TEST(ReplyDatagram, InvalidFramesAndClosedPortCannotReviveMedia) {
    QueueDatagram socket;
    ReplyDatagram filter(socket);
    std::vector<std::uint8_t> out;
    ASSERT_TRUE(filter.open("127.0.0.1", 0));
    ASSERT_TRUE(filter.set_peer("127.0.0.1", 42));
    EXPECT_EQ(filter.local_port(), 42);
    ASSERT_TRUE(filter.select_nonce(nonce(1)));
    auto wrong = packet(1);
    wrong[3] = 2;
    socket.packets = {{}, std::vector<std::uint8_t>(1292), wrong};
    wrong = packet(1);
    wrong[0] = 2;
    socket.packets.push_back(wrong);
    wrong = packet(1);
    wrong[1] = 2;
    socket.packets.push_back(wrong);
    EXPECT_EQ(filter.receive(out), ReceiveResult::Empty);
    EXPECT_TRUE(out.empty());
    EXPECT_EQ(filter.buffered(), 0U);
    EXPECT_TRUE(filter.send(packet(1)));
    socket.failed = true;
    EXPECT_EQ(filter.receive(out), ReceiveResult::Failed);
    filter.close();
    filter.close();
    EXPECT_EQ(socket.closes, 1U);
    EXPECT_FALSE(filter.select_nonce(nonce(2)));
    EXPECT_FALSE(filter.send(packet(1)));
    EXPECT_EQ(filter.receive(out), ReceiveResult::Failed);
}
TEST(ReplyDatagram, RetiredNonceHistoryCannotBeEvictedToReplayOldAudio) {
    QueueDatagram socket;
    ReplyDatagram filter(socket);
    for (unsigned index = 0; index <= 512; ++index) {
        auto id = nonce(static_cast<std::uint8_t>(index));
        id[1] = static_cast<std::uint8_t>(index >> 8U);
        ASSERT_TRUE(filter.select_nonce(id));
    }
    auto next = nonce(1);
    next[1] = 3;
    EXPECT_FALSE(filter.select_nonce(next));
    EXPECT_FALSE(filter.select_nonce(nonce(0)));
}
}  // namespace
}  // namespace ur::device
