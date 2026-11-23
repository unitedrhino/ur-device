/* 统一信令 AES-128-CTR 会话密钥端口：PSA Crypto 实现，IV 即完整 16 字节包头。 */
#ifndef UR_SIGNALING_CIPHER_H
#define UR_SIGNALING_CIPHER_H

#include "ur/device/udp_channel.hpp"

#include <mutex>

#include "psa/crypto.h"

namespace ur::device {

/* UrSignalingCipher 由 UdpChannel 在事件线程独占调用；与旧协议 UDP 通道共用
 * PSA 后端，密钥不落盘、不在日志出现。 */
class UrSignalingCipher final : public SessionCipherPort {
public:
    UrSignalingCipher() = default;
    ~UrSignalingCipher() override;
    UrSignalingCipher(const UrSignalingCipher&) = delete;
    UrSignalingCipher& operator=(const UrSignalingCipher&) = delete;

    /* install 安装本次会话 16 字节密钥；失败保留旧状态由上层关闭。 */
    bool install(const std::array<std::uint8_t, 16>& key) override;
    /* clear 擦除密钥上下文；重复调用幂等。 */
    void clear() override;
    /* transform 以 IV 为完整 CTR 计数块加解密 size 字节；输入输出不重叠。 */
    bool transform(const std::array<std::uint8_t, 16>& iv, const std::uint8_t* input,
                   std::uint8_t* output, std::size_t size) override;

private:
    /* key_id_ 为 PSA 易失性密钥槽；跨线程调用由事件线程串行保证。 */
    psa_key_id_t key_id_ = PSA_KEY_ID_NULL;
};

}  // namespace ur::device

#endif /* UR_SIGNALING_CIPHER_H */
