// 现行 MQTT+UDP 音频包编解码；16 字节包头不因新信令而改变。
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace ur::device {
// AesCtrPort 由平台提供 AES-128-CTR；IV 是完整包头，输入输出不重叠。
// 每次调用必须重新初始化 CTR，不得沿用上一数据报的计数器。
class AesCtrPort {
public:
    virtual ~AesCtrPort() = default;
    // transform 成功写满 size 字节，失败返回 false，不允许把失败输出作为媒体。
    virtual bool transform(const std::array<std::uint8_t, 16>& iv, const std::uint8_t* input,
                           std::uint8_t* output, std::size_t size) = 0;
};
// SessionCipherPort 安装/擦除短期会话密钥；平台可使用 OpenSSL、mbedTLS 或硬件密钥槽。
class SessionCipherPort : public AesCtrPort {
public:
    virtual bool install(const std::array<std::uint8_t, 16>& key) = 0;
    virtual void clear() = 0;
};
// UdpResult 区分格式错误、会话错误、迟到包、密钥驱动失败和序号耗尽。
enum class UdpResult { Ok, Invalid, WrongSession, Stale, CryptoFailed, Exhausted, BeyondEnd };
// UdpChannel 一个实例对应一个密钥/nonce 会话；由事件线程独占，重连创建新实例。
class UdpChannel {
public:
    // nonce 为服务端下发头部的 [4,12) 字节，cipher 寿命必须超过本实例。
    UdpChannel(AesCtrPort& cipher, std::array<std::uint8_t, 8> nonce, std::uint32_t stride = 1)
        : cipher_(cipher), nonce_(nonce), receive_nonce_(nonce), stride_(stride) {}
    UdpChannel(const UdpChannel&) = delete;
    UdpChannel& operator=(const UdpChannel&) = delete;
    // encode 序号从 1 递增；失败清空输出，序号耗尽必须重建密钥而非回绕。
    UdpResult encode(const std::vector<std::uint8_t>& opus, std::vector<std::uint8_t>& packet);
    // decode 严格校验完整数据报，拒绝重复/迟到包；成功后才更新接收水位。
    UdpResult decode(const std::uint8_t* packet, std::size_t size, std::vector<std::uint8_t>& opus);
    // select_receive_nonce 仅由新信令回复过滤层调用；不同回复重置接收水位，上行不受影响。
    // 调用方必须阻止退休 nonce 再次激活，旧协议不调用此接口。
    void select_receive_nonce(std::array<std::uint8_t, 8> nonce);
    // set_receive_floor 拒绝该回复声明首包之前的旧包；首包缺失由音频泵统计。
    bool set_receive_floor(std::uint32_t first_sequence);
    // limit_receive_sequence 安装当前回复末包边界；重复相同边界幂等，变更/倒退拒绝。
    // 必须在解密和播放前执行上限检查，旧 Topic 不安装此约束。
    bool limit_receive_sequence(std::uint32_t last_sequence);
    // next_sequence 返回下一上行序号，0表示已耗尽；输入段可据此声明迟到包屏障。
    std::uint32_t next_sequence() const;
    std::uint32_t received_sequence() const { return remote_seen_ ? remote_seq_ : 0; }
    // sequence_stride 是当前通道合法相邻数据报的序号间隔；供媒体完整性统计使用。
    std::uint32_t sequence_stride() const { return stride_; }

private:
    AesCtrPort& cipher_;                         // 平台密钥上下文，核心不打印或持久化密钥。
    std::array<std::uint8_t, 8> nonce_;          // 授权通道标识。
    std::array<std::uint8_t, 8> receive_nonce_;  // 当前授权下行回复，缺省与旧通道相同。
    std::uint32_t stride_;          // 旧通道为1；新单 Opus 包通道为128，避免相邻包CTR块区间重叠。
    std::uint32_t local_seq_ = 0;   // 上行序号。
    std::uint32_t remote_seq_ = 0;  // 已接受下行的最大序号。
    bool remote_seen_ = false;      // 首包允许序号 0，与当前服务端保持一致。
    std::optional<std::uint32_t> receive_limit_;  // 新信令已确认的当前回复末包。
    std::uint32_t receive_floor_ = 0;             // 当前回复首包序号，旧通道保持零。
};
}  // namespace ur::device
