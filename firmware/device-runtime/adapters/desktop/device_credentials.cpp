// 设备签名只使用 OpenSSL 已验证算法；解码密钥生命周期结束时清零。
#include "device_credentials.hpp"
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <algorithm>
#include <array>
#include <vector>

namespace ur::device {
namespace {
// identity_piece 拒绝协议分隔符及控制字符，避免认证身份与 Topic 身份分歧。
bool identity_piece(std::string_view value) {
    return !value.empty() && value.size() <= 128 &&
           std::all_of(value.begin(), value.end(), [](unsigned char ch) {
               return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                      (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
           });
}
// Secret 临时保存解码后的密钥，任何错误出口都清零分配的字节。
struct Secret {
    std::vector<unsigned char> bytes;  // 仅本地 HMAC 使用。
    ~Secret() {
        if (!bytes.empty())
            OPENSSL_cleanse(bytes.data(), bytes.size());
    }
};
}  // namespace
std::optional<DeviceCredentials> device_credentials(std::string_view product,
                                                    std::string_view device,
                                                    std::string_view secret_base64,
                                                    std::string_view nonce, std::uint64_t expiry) {
    if (!identity_piece(product) || !identity_piece(device) || !identity_piece(nonce) ||
        nonce.size() > 32 || expiry == 0 || expiry > 9007199254740991ULL || secret_base64.empty() ||
        secret_base64.size() > 1024 || secret_base64.size() % 4 != 0)
        return std::nullopt;
    // 先检查 base64 正文和末尾 padding，避免宽松解码器接受损坏密钥。
    auto padding = std::size_t{0};
    if (secret_base64.back() == '=')
        ++padding;
    if (secret_base64[secret_base64.size() - 2] == '=')
        ++padding;
    const auto content = secret_base64.substr(0, secret_base64.size() - padding);
    if (content.empty() || !std::all_of(content.begin(), content.end(), [](unsigned char ch) {
            return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                   (ch >= '0' && ch <= '9') || ch == '+' || ch == '/';
        }))
        return std::nullopt;
    Secret secret{std::vector<unsigned char>(secret_base64.size())};
    const auto decoded = EVP_DecodeBlock(
        secret.bytes.data(), reinterpret_cast<const unsigned char*>(secret_base64.data()),
        static_cast<int>(secret_base64.size()));
    if (decoded <= static_cast<int>(padding))
        return std::nullopt;
    const auto key_size = static_cast<std::size_t>(decoded) - padding;
    // 重新编码核验未使用位，拒绝不同 base64 文本解码成同一非规范密钥。
    Secret canonical{std::vector<unsigned char>(secret_base64.size() + 1)};
    const auto encoded =
        EVP_EncodeBlock(canonical.bytes.data(), secret.bytes.data(), static_cast<int>(key_size));
    if (encoded != static_cast<int>(secret_base64.size()) ||
        !std::equal(secret_base64.begin(), secret_base64.end(), canonical.bytes.begin()))
        return std::nullopt;
    DeviceCredentials credentials;
    credentials.client_id = std::string(product) + "&" + std::string(device);
    credentials.username =
        credentials.client_id + ";12010126;" + std::string(nonce) + ";" + std::to_string(expiry);
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_size = 0;
    if (!HMAC(EVP_sha256(), secret.bytes.data(), static_cast<int>(key_size),
              reinterpret_cast<const unsigned char*>(credentials.username.data()),
              credentials.username.size(), digest.data(), &digest_size) ||
        digest_size != 32)
        return std::nullopt;
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned int i = 0; i < digest_size; ++i) {
        credentials.password.push_back(hex[digest[i] >> 4]);
        credentials.password.push_back(hex[digest[i] & 15]);
    }
    OPENSSL_cleanse(digest.data(), digest.size());
    credentials.password += ";hmacsha256";
    return credentials;
}
}  // namespace ur::device
