// 使用 OpenSSL EVP 提供桌面密码适配，不将 OpenSSL 暴露给可移植核心。
#include "openssl_ctr.hpp"
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <limits>
#include <memory>

namespace ur::device {
OpenSslCtr::~OpenSslCtr() { clear(); }
bool OpenSslCtr::install(const std::array<std::uint8_t, 16>& key) {
    clear();
    key_ = key;
    installed_ = true;
    return true;
}
void OpenSslCtr::clear() {
    OPENSSL_cleanse(key_.data(), key_.size());
    installed_ = false;
}
bool OpenSslCtr::transform(const std::array<std::uint8_t, 16>& iv, const std::uint8_t* input,
                           std::uint8_t* output, std::size_t size) {
    if (!installed_ || !input || !output || size == 0 ||
        size > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return false;
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> ctx(EVP_CIPHER_CTX_new(),
                                                                        EVP_CIPHER_CTX_free);
    if (!ctx ||
        EVP_EncryptInit_ex(ctx.get(), EVP_aes_128_ctr(), nullptr, key_.data(), iv.data()) != 1)
        return false;
    int written = 0;
    int final_bytes = 0;
    return EVP_EncryptUpdate(ctx.get(), output, &written, input, static_cast<int>(size)) == 1 &&
           EVP_EncryptFinal_ex(ctx.get(), output + written, &final_bytes) == 1 && written >= 0 &&
           final_bytes >= 0 &&
           static_cast<std::size_t>(written) + static_cast<std::size_t>(final_bytes) == size;
}
}  // namespace ur::device
