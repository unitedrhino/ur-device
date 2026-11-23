// UDP 核心测试使用可失败的复制驱动，只验证封包与水位，不声称验证密码算法。
#include "ur/device/udp_channel.hpp"
#include <gtest/gtest.h>
#include <algorithm>

namespace ur::device {
namespace {
// CopyCipher 保留负载使包头断言直观；真实 AES 另由 OpenSSL 测试验证。
struct CopyCipher final : AesCtrPort {
    bool succeeds = true;  // 单次加解密失败注入。
    bool transform(const std::array<std::uint8_t, 16>&, const std::uint8_t* in, std::uint8_t* out,
                   std::size_t size) override {
        if (!succeeds)
            return false;
        std::copy_n(in, size, out);
        return true;
    }
};
// UdpTest 每个用例独享两个序号空间和相同协商 nonce。
class UdpTest : public testing::Test {
protected:
    CopyCipher cipher;                                                // 无密码运算的平台替身。
    const std::array<std::uint8_t, 8> nonce{1, 2, 3, 4, 5, 6, 7, 8};  // 非凭据测试会话标识。
    UdpChannel sender{cipher, nonce};                                 // 上行生成器。
    UdpChannel receiver{cipher, nonce};                               // 独立下行水位。
    std::vector<std::uint8_t> packet;                                 // 本轮数据报。
    std::vector<std::uint8_t> decoded;                                // 解码结果。
};
TEST_F(UdpTest, HeaderMatchesExistingSixteenByteWireFormat) {
    ASSERT_EQ(sender.encode({0xf8, 0xff, 0xfe}, packet), UdpResult::Ok);
    const std::vector<std::uint8_t> expected{1, 0, 0, 3, 1, 2, 3,    4,    5,   6,
                                             7, 8, 0, 0, 0, 1, 0xf8, 0xff, 0xfe};
    EXPECT_EQ(packet, expected);
    EXPECT_EQ(receiver.decode(packet.data(), packet.size(), decoded), UdpResult::Ok);
    EXPECT_EQ(decoded, (std::vector<std::uint8_t>{0xf8, 0xff, 0xfe}));
}
TEST_F(UdpTest, RejectsDuplicatesLatePacketsButAcceptsGaps) {
    ASSERT_EQ(sender.encode({1}, packet), UdpResult::Ok);
    const auto first = packet;
    ASSERT_EQ(receiver.decode(packet.data(), packet.size(), decoded), UdpResult::Ok);
    EXPECT_EQ(receiver.decode(packet.data(), packet.size(), decoded), UdpResult::Stale);
    EXPECT_TRUE(decoded.empty());
    ASSERT_EQ(sender.encode({2}, packet), UdpResult::Ok);
    ASSERT_EQ(sender.encode({3}, packet), UdpResult::Ok);
    EXPECT_EQ(receiver.decode(packet.data(), packet.size(), decoded), UdpResult::Ok);
    EXPECT_EQ(receiver.decode(first.data(), first.size(), decoded), UdpResult::Stale);
}
TEST_F(UdpTest, ReplyCeilingRejectsPacketsBeforeCipherAndOnlyNewNonceResetsIt) {
    UdpChannel bounded(cipher, nonce, 128);
    UdpChannel producer(cipher, nonce, 128);
    ASSERT_EQ(producer.encode({1}, packet), UdpResult::Ok);
    EXPECT_FALSE(bounded.limit_receive_sequence(1));
    ASSERT_TRUE(bounded.limit_receive_sequence(128));
    EXPECT_TRUE(bounded.limit_receive_sequence(128));
    EXPECT_FALSE(bounded.limit_receive_sequence(256));
    ASSERT_EQ(bounded.decode(packet.data(), packet.size(), decoded), UdpResult::Ok);
    EXPECT_FALSE(bounded.limit_receive_sequence(0));
    ASSERT_EQ(producer.encode({2}, packet), UdpResult::Ok);
    cipher.succeeds = false;
    EXPECT_EQ(bounded.decode(packet.data(), packet.size(), decoded), UdpResult::BeyondEnd);
    EXPECT_TRUE(decoded.empty());
    EXPECT_EQ(bounded.received_sequence(), 128U);
    bounded.select_receive_nonce(nonce);
    EXPECT_EQ(bounded.decode(packet.data(), packet.size(), decoded), UdpResult::BeyondEnd);
    const std::array<std::uint8_t, 8> next_nonce{9};
    bounded.select_receive_nonce(next_nonce);
    producer.select_receive_nonce(next_nonce);  // 上行空间不受接收 nonce 切换影响。
    UdpChannel next_producer(cipher, next_nonce, 128);
    cipher.succeeds = true;
    ASSERT_EQ(next_producer.encode({3}, packet), UdpResult::Ok);
    EXPECT_EQ(bounded.decode(packet.data(), packet.size(), decoded), UdpResult::Ok);
    EXPECT_TRUE(bounded.limit_receive_sequence(256));
    UdpChannel invalid(cipher, nonce, 0);
    EXPECT_FALSE(invalid.limit_receive_sequence(0));
}
TEST_F(UdpTest, AnnouncedReplyFloorRejectsEarlierPacketsBeforeDecode) {
    UdpChannel bounded(cipher, nonce, 128);
    UdpChannel producer(cipher, nonce, 128);
    EXPECT_FALSE(bounded.set_receive_floor(0));
    EXPECT_FALSE(bounded.set_receive_floor(129));
    ASSERT_TRUE(bounded.set_receive_floor(256));
    EXPECT_FALSE(bounded.set_receive_floor(256));
    EXPECT_FALSE(bounded.limit_receive_sequence(128));
    ASSERT_EQ(producer.encode({1}, packet), UdpResult::Ok);
    cipher.succeeds = false;
    EXPECT_EQ(bounded.decode(packet.data(), packet.size(), decoded), UdpResult::Stale);
    EXPECT_EQ(bounded.received_sequence(), 0U);
    cipher.succeeds = true;
    ASSERT_EQ(producer.encode({2}, packet), UdpResult::Ok);
    EXPECT_EQ(bounded.decode(packet.data(), packet.size(), decoded), UdpResult::Ok);
    EXPECT_FALSE(bounded.set_receive_floor(384));
}
TEST_F(UdpTest, MalformedPacketsDoNotPoisonWatermark) {
    ASSERT_EQ(sender.encode({1}, packet), UdpResult::Ok);
    const auto valid = packet;
    EXPECT_EQ(receiver.decode(nullptr, 17, decoded), UdpResult::Invalid);
    EXPECT_EQ(receiver.decode(packet.data(), 16, decoded), UdpResult::Invalid);
    EXPECT_EQ(receiver.decode(packet.data(), 65508, decoded), UdpResult::Invalid);
    for (auto offset : {0U, 1U, 2U, 3U}) {
        packet = valid;
        packet[offset] = 42;
        EXPECT_EQ(receiver.decode(packet.data(), packet.size(), decoded), UdpResult::Invalid);
    }
    packet = valid;
    packet[4] = 42;
    EXPECT_EQ(receiver.decode(packet.data(), packet.size(), decoded), UdpResult::WrongSession);
    packet = valid;
    packet.push_back(0);
    EXPECT_EQ(receiver.decode(packet.data(), packet.size(), decoded), UdpResult::Invalid);
    EXPECT_EQ(receiver.decode(valid.data(), valid.size(), decoded), UdpResult::Ok);
}
TEST_F(UdpTest, FailedCipherConsumesSendNonceButNotReceiveWatermark) {
    cipher.succeeds = false;
    EXPECT_EQ(sender.encode({1}, packet), UdpResult::CryptoFailed);
    EXPECT_TRUE(packet.empty());
    cipher.succeeds = true;
    ASSERT_EQ(sender.encode({1}, packet), UdpResult::Ok);
    EXPECT_EQ(packet[15], 2);
    cipher.succeeds = false;
    EXPECT_EQ(receiver.decode(packet.data(), packet.size(), decoded), UdpResult::CryptoFailed);
    EXPECT_TRUE(decoded.empty());
    cipher.succeeds = true;
    EXPECT_EQ(receiver.decode(packet.data(), packet.size(), decoded), UdpResult::Ok);
}
TEST_F(UdpTest, PayloadBoundsAndAliasingAreExplicit) {
    EXPECT_EQ(sender.encode({}, packet), UdpResult::Invalid);
    EXPECT_EQ(sender.encode(std::vector<std::uint8_t>(65507U - 15U), packet), UdpResult::Invalid);
    packet = {1};
    EXPECT_EQ(sender.encode(packet, packet), UdpResult::Invalid);
    EXPECT_EQ(packet, (std::vector<std::uint8_t>{1}));
    ASSERT_EQ(sender.encode(std::vector<std::uint8_t>(65507U - 16U, 42), packet), UdpResult::Ok);
    EXPECT_EQ(receiver.decode(packet.data(), packet.size(), packet), UdpResult::Ok);
    EXPECT_EQ(packet.size(), 65507U - 16U);
    EXPECT_EQ(packet.front(), 42);
}
TEST_F(UdpTest, SequenceZeroAndMaximumDoNotWrapReceiver) {
    ASSERT_EQ(sender.encode({1}, packet), UdpResult::Ok);
    packet[15] = 0;
    EXPECT_EQ(receiver.decode(packet.data(), packet.size(), decoded), UdpResult::Ok);
    std::fill(packet.begin() + 12, packet.begin() + 16, 255);
    EXPECT_EQ(receiver.decode(packet.data(), packet.size(), decoded), UdpResult::Ok);
    std::fill(packet.begin() + 12, packet.begin() + 16, 0);
    EXPECT_EQ(receiver.decode(packet.data(), packet.size(), decoded), UdpResult::Stale);
}
TEST_F(UdpTest, NewProfileStrideAndReplyNonceKeepInputIndependent) {
    UdpChannel profile(cipher, nonce, 128);
    EXPECT_EQ(profile.next_sequence(), 128U);
    ASSERT_EQ(profile.encode({1}, packet), UdpResult::Ok);
    EXPECT_EQ(packet[15], 128);
    const auto upstream = packet;
    auto reply_nonce = nonce;
    reply_nonce[0] = 99;
    UdpChannel reply(cipher, reply_nonce, 128);
    ASSERT_EQ(reply.encode({2}, packet), UdpResult::Ok);
    EXPECT_EQ(profile.decode(packet.data(), packet.size(), decoded), UdpResult::WrongSession);
    profile.select_receive_nonce(reply_nonce);
    EXPECT_EQ(profile.decode(packet.data(), packet.size(), decoded), UdpResult::Ok);
    profile.select_receive_nonce(reply_nonce);
    EXPECT_EQ(profile.decode(packet.data(), packet.size(), decoded), UdpResult::Stale);
    EXPECT_EQ(profile.decode(upstream.data(), upstream.size(), decoded), UdpResult::WrongSession);
    EXPECT_EQ(profile.next_sequence(), 256U);
    EXPECT_EQ(profile.encode(std::vector<std::uint8_t>(2049), packet), UdpResult::Invalid);
    UdpChannel exhausted(cipher, nonce, 0xffffffffU);
    EXPECT_EQ(exhausted.encode({1}, packet), UdpResult::Ok);
    EXPECT_EQ(exhausted.next_sequence(), 0U);
    EXPECT_EQ(exhausted.encode({1}, packet), UdpResult::Exhausted);
    UdpChannel invalid(cipher, nonce, 0);
    EXPECT_EQ(invalid.encode({1}, packet), UdpResult::Exhausted);
}
}  // namespace
}  // namespace ur::device
