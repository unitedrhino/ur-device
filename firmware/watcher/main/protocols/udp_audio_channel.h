/* 加密 UDP Opus 通道：统一维护帧头、AES-CTR、序号和下行解密。 */
#ifndef UDP_AUDIO_CHANNEL_H
#define UDP_AUDIO_CHANNEL_H

#include "protocol.h"
#include "ur_ai_runtime.h"

#include <network_interface.h>
#include <psa/crypto.h>
#include <udp.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

/* UdpAudioChannelConfig 描述服务端下发的加密 UDP 通道参数。 */
struct UdpAudioChannelConfig {
    std::string server;
    int port = 0;
    std::string key_hex;
    std::string nonce_hex;
    int sample_rate = 24000;
    int frame_duration = 60;
    /* 联犀 devicesim 保留 nonce 内的时间戳；公共小智协议使用设备包时间戳。 */
    bool preserve_nonce_timestamp = false;
};

/* UdpAudioChannel 封装 xiaozhi 和联犀共用的 16 字节 AES-CTR UDP 音频帧。 */
class UdpAudioChannel {
public:
    using IncomingAudioCallback = std::function<void(std::unique_ptr<AudioStreamPacket> packet)>;

    UdpAudioChannel();
    ~UdpAudioChannel();

    /* Open 校验参数、导入 AES 密钥并连接 UDP；失败时保持关闭。 */
    bool Open(NetworkInterface* network, const UdpAudioChannelConfig& config,
              IncomingAudioCallback callback);
    /* Close 释放 UDP 和密钥，可重复调用。 */
    void Close();
    /* Send 按配置构造并加密一帧 Opus 数据。 */
    bool Send(const AudioStreamPacket& packet);
    /* SendPayload 用于联犀会话首轮发送已编码的静音预热帧。 */
    bool SendPayload(const uint8_t* payload, size_t payload_size, uint32_t timestamp = 0);
    /* IsOpen 返回通道是否已建立。 */
    bool IsOpen() const;

private:
    /* kHeaderSize 是 devicesim 契约固定的 AES-CTR nonce/帧头长度。 */
    static constexpr size_t kHeaderSize = kUrAiUdpHeaderSize;

    /* channel_mutex_ 保护 UDP、nonce、序号和回调快照。 */
    mutable std::mutex channel_mutex_;
    /* crypto_mutex_ 串行使用当前 PSA 密钥，避免收发并发复用 operation。 */
    std::mutex crypto_mutex_;
    /* 以下字段描述当前打开的唯一 UDP 会话及双向帧序号。 */
    std::shared_ptr<Udp> udp_;
    psa_key_id_t aes_key_id_ = PSA_KEY_ID_NULL;
    std::string nonce_template_;
    uint32_t local_sequence_ = 0;
    uint32_t remote_sequence_ = 0;
    int sample_rate_ = 24000;
    int frame_duration_ = 60;
    bool preserve_nonce_timestamp_ = false;
    IncomingAudioCallback incoming_audio_callback_;

    /* DecodeHexString 把平台十六进制参数转换为二进制。 */
    static bool DecodeHexString(const std::string& value, std::string& decoded);
    /* CryptAesCtr 使用当前导入的密钥完成等长加解密。 */
    bool CryptAesCtr(const uint8_t* input, size_t input_size, const uint8_t* nonce,
                     uint8_t* output);
    /* HandleIncomingDatagram 校验、解密并分发一帧服务端音频。 */
    void HandleIncomingDatagram(const std::string& data);
};

#endif  // UDP_AUDIO_CHANNEL_H
