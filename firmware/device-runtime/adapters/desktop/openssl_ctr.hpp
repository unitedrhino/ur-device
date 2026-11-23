// 桌面 AES-128-CTR 适配；ESP-IDF 使用相同 AesCtrPort 接入 mbedTLS。
#pragma once
#include "ur/device/udp_channel.hpp"

namespace ur::device {
// OpenSslCtr 独占 16 字节会话密钥；不提供打印/读取接口，销毁时清零。
class OpenSslCtr final : public SessionCipherPort {
public:
    OpenSslCtr() = default;
    // key 必须为测试生成或鉴权服务下发的 AES-128 密钥。
    explicit OpenSslCtr(const std::array<std::uint8_t, 16>& key) : key_(key), installed_(true) {}
    ~OpenSslCtr() override;
    OpenSslCtr(const OpenSslCtr&) = delete;
    OpenSslCtr& operator=(const OpenSslCtr&) = delete;
    bool install(const std::array<std::uint8_t, 16>& key) override;
    void clear() override;
    // transform 用独立 EVP 上下文完成一次数据报加解密。
    bool transform(const std::array<std::uint8_t, 16>& iv, const std::uint8_t* input,
                   std::uint8_t* output, std::size_t size) override;

private:
    std::array<std::uint8_t, 16> key_{};  // 仅保存当前媒体通道密钥。
    bool installed_ = false;              // 清除密钥后不能继续加解密。
};
}  // namespace ur::device
