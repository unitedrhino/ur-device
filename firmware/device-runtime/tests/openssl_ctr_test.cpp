// 真实 AES 适配测试：公开 NIST SP800-38A 向量及现行 UDP 包往返。
#include "openssl_ctr.hpp"
#include <gtest/gtest.h>
#include <limits>
#include "device_credentials.hpp"

namespace ur::device {
// Python标准库独立计算的公开测试向量，与后端 hex HMAC-SHA256 规则对齐。
TEST(DeviceCredentials, MatchesExistingBackendSignatureFormat) {
    const auto credentials = device_credentials("p", "d", "dGVzdC1zZWNyZXQ=", "nonce", 2000000000);
    ASSERT_TRUE(credentials);
    EXPECT_EQ(credentials->client_id, "p&d");
    EXPECT_EQ(credentials->username, "p&d;12010126;nonce;2000000000");
    // 公开测试签名，非真实设备凭据。
    EXPECT_EQ(credentials->password,
              "be322b58cb2f130afdad6e0ad57e197fac8b0c6ef0f513862e15e7bf6bc3bd3d;hmacsha256");
}
TEST(DeviceCredentials, RejectsCorruptSecretsAndIdentityInjection) {
    for (const auto secret : {"", "=", "====", "ab=c", "dGVzdC1zZWNyZXQ!", "Zh=="})
        EXPECT_FALSE(device_credentials("p", "d", secret, "nonce", 2000000000));
    EXPECT_FALSE(device_credentials("p/x", "d", "dGVzdA==", "nonce", 2000000000));
    EXPECT_FALSE(device_credentials("p", "d&x", "dGVzdA==", "nonce", 2000000000));
    EXPECT_FALSE(device_credentials("p", "d", "dGVzdA==", "nonce;other", 2000000000));
    EXPECT_FALSE(device_credentials("p", "d", "dGVzdA==", "nonce", 0));
    EXPECT_FALSE(device_credentials("p", "d", "dGVzdA==", std::string(33, 'x'), 2000000000));
}
TEST(OpenSslCtr, PublicNistAes128CtrVector) {
    const std::array<std::uint8_t, 16> key{0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
                                           0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c};
    const std::array<std::uint8_t, 16> iv{0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7,
                                          0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff};
    const std::array<std::uint8_t, 16> plain{0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96,
                                             0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a};
    const std::array<std::uint8_t, 16> expected{0x87, 0x4d, 0x61, 0x91, 0xb6, 0x20, 0xe3, 0x26,
                                                0x1b, 0xef, 0x68, 0x64, 0x99, 0x0d, 0xb6, 0xce};
    OpenSslCtr cipher(key);
    std::array<std::uint8_t, 16> encrypted{};
    ASSERT_TRUE(cipher.transform(iv, plain.data(), encrypted.data(), plain.size()));
    EXPECT_EQ(encrypted, expected);
    std::array<std::uint8_t, 16> recovered{};
    ASSERT_TRUE(cipher.transform(iv, encrypted.data(), recovered.data(), encrypted.size()));
    EXPECT_EQ(recovered, plain);
    EXPECT_FALSE(cipher.transform(iv, nullptr, recovered.data(), 1));
    EXPECT_FALSE(cipher.transform(iv, plain.data(), nullptr, 1));
    EXPECT_FALSE(cipher.transform(iv, plain.data(), recovered.data(), 0));
    EXPECT_FALSE(cipher.transform(iv, plain.data(), recovered.data(),
                                  static_cast<std::size_t>(std::numeric_limits<int>::max()) + 1));
}
TEST(OpenSslCtr, UdpCipherRoundTripUsesIndependentCounters) {
    const std::array<std::uint8_t, 16> key{};  // 仅限离线测试的公开零密钥。
    OpenSslCtr encryptor(key);
    OpenSslCtr decryptor(key);
    UdpChannel sender(encryptor, {1, 2, 3, 4, 5, 6, 7, 8});
    UdpChannel receiver(decryptor, {1, 2, 3, 4, 5, 6, 7, 8});
    for (std::size_t size : {1U, 3U, 16U, 17U, 960U}) {
        std::vector<std::uint8_t> plain(size, 0x42);
        std::vector<std::uint8_t> packet;
        std::vector<std::uint8_t> recovered;
        ASSERT_EQ(sender.encode(plain, packet), UdpResult::Ok);
        ASSERT_EQ(receiver.decode(packet.data(), packet.size(), recovered), UdpResult::Ok);
        EXPECT_EQ(recovered, plain);
    }
}
}  // namespace ur::device
