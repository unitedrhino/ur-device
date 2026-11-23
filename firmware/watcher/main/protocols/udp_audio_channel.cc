/* 加密 UDP Opus 通道实现；帧格式与 backend/things/tools/devicesim/udp.go 一致。 */
#include "udp_audio_channel.h"
#if CONFIG_UR_UDP_TX_TRACE
#include "ur_udp_netif_trace.h"
#include "ur_udp_wifi_trace.h"
#endif

#include <esp_log.h>

#include <array>
#include <cstring>

namespace {
constexpr char kTag[] = "UdpAudio";

/* CharToHex 把单个十六进制字符转换为数值。 */
int CharToHex(char value) {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    return -1;
}
}  // namespace

UdpAudioChannel::UdpAudioChannel() = default;

UdpAudioChannel::~UdpAudioChannel() { Close(); }

bool UdpAudioChannel::Open(NetworkInterface* network, const UdpAudioChannelConfig& config,
                           IncomingAudioCallback callback) {
    Close();
    if (network == nullptr || config.server.empty() || config.port <= 0 ||
        config.port > UINT16_MAX || config.sample_rate <= 0 || config.frame_duration <= 0) {
        ESP_LOGE(kTag, "invalid UDP channel endpoint or audio parameters");
        return false;
    }

    std::string key;
    std::string nonce;
    if (!DecodeHexString(config.key_hex, key) || !DecodeHexString(config.nonce_hex, nonce) ||
        key.size() != 16 || nonce.size() != kHeaderSize) {
        ESP_LOGE(kTag, "invalid AES key or nonce length");
        return false;
    }

    psa_status_t status = psa_crypto_init();
    if (status != PSA_SUCCESS) {
        ESP_LOGE(kTag, "PSA crypto init failed: %ld", static_cast<long>(status));
        return false;
    }
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attributes, PSA_ALG_CTR);
    psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attributes, 128);
    {
        std::lock_guard<std::mutex> lock(crypto_mutex_);
        status = psa_import_key(&attributes, reinterpret_cast<const uint8_t*>(key.data()),
                                key.size(), &aes_key_id_);
    }
    psa_reset_key_attributes(&attributes);
    if (status != PSA_SUCCESS) {
        ESP_LOGE(kTag, "AES key import failed: %ld", static_cast<long>(status));
        return false;
    }

    auto udp = network->CreateUdp(2);
    if (udp == nullptr) {
        ESP_LOGE(kTag, "UDP allocation failed");
        Close();
        return false;
    }
    udp->OnMessage([this](const std::string& data) { HandleIncomingDatagram(data); });
    if (auto connected = udp->Connect(config.server, config.port); !connected) {
        ESP_LOGE(kTag, "UDP connect failed: %s", connected.error().ToString().c_str());
        Close();
        return false;
    }

    /* 仅显式诊断包观察网络交接和驱动回调；观察失败不修改UDP业务的成功判据。 */
#if CONFIG_UR_UDP_TX_TRACE
    UrUdpNetifTraceStart(reinterpret_cast<const uint8_t*>(nonce.data()), nonce.size(), config.port);
    UrUdpWifiTraceStart(reinterpret_cast<const uint8_t*>(nonce.data()), nonce.size(), config.port);
#endif
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        udp_ = std::move(udp);
        nonce_template_ = std::move(nonce);
        local_sequence_ = 0;
        remote_sequence_ = 0;
        sample_rate_ = config.sample_rate;
        frame_duration_ = config.frame_duration;
        preserve_nonce_timestamp_ = config.preserve_nonce_timestamp;
        incoming_audio_callback_ = std::move(callback);
    }
    return true;
}

void UdpAudioChannel::Close() {
#if CONFIG_UR_UDP_TX_TRACE
    UrUdpNetifTraceStop();
    UrUdpWifiTraceStop();
#endif
    std::shared_ptr<Udp> udp;
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        udp = std::move(udp_);
        nonce_template_.clear();
        incoming_audio_callback_ = nullptr;
        local_sequence_ = 0;
        remote_sequence_ = 0;
    }
    udp.reset();
    std::lock_guard<std::mutex> lock(crypto_mutex_);
    if (aes_key_id_ != PSA_KEY_ID_NULL) {
        psa_destroy_key(aes_key_id_);
        aes_key_id_ = PSA_KEY_ID_NULL;
    }
}

bool UdpAudioChannel::Send(const AudioStreamPacket& packet) {
    return SendPayload(packet.payload.data(), packet.payload.size(), packet.timestamp);
}

bool UdpAudioChannel::SendPayload(const uint8_t* payload, size_t payload_size, uint32_t timestamp) {
    if (payload == nullptr || payload_size == 0 || payload_size > UINT16_MAX) {
        return false;
    }

    std::shared_ptr<Udp> udp;
    std::string nonce;
    bool preserve_timestamp = false;
    uint32_t sequence = 0;
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        if (udp_ == nullptr || nonce_template_.size() != kHeaderSize) {
            return false;
        }
        udp = udp_;
        nonce = nonce_template_;
        preserve_timestamp = preserve_nonce_timestamp_;
        sequence = ++local_sequence_;
    }

    std::array<uint8_t, kHeaderSize> header = {};
    if (!UrAiBuildUdpHeader(reinterpret_cast<const uint8_t*>(nonce.data()), nonce.size(),
                            payload_size, timestamp, sequence, preserve_timestamp, header.data(),
                            header.size())) {
        return false;
    }

    std::string datagram(kHeaderSize + payload_size, '\0');
    std::memcpy(datagram.data(), header.data(), kHeaderSize);
    if (!CryptAesCtr(payload, payload_size, header.data(),
                     reinterpret_cast<uint8_t*>(datagram.data() + kHeaderSize))) {
        return false;
    }
    /* 保持原发送判据；仅受控测试包记录套接字提交结果，用于对照服务端入站序号。
     * 提交成功不代表对端收到，不记录帧头/nonce、凭据或音频内容。正式包完全排除该日志。 */
    const int sent = udp->Send(datagram);
#if CONFIG_UR_AI_E2E_TEST
    ESP_LOGI(kTag, "[UR_UDP_E2E] SEND seq=%lu bytes=%u sent=%d",
             static_cast<unsigned long>(sequence), static_cast<unsigned>(datagram.size()), sent);
#endif
    return sent > 0;
}

bool UdpAudioChannel::IsOpen() const {
    std::lock_guard<std::mutex> lock(channel_mutex_);
    return udp_ != nullptr;
}

bool UdpAudioChannel::DecodeHexString(const std::string& value, std::string& decoded) {
    decoded.clear();
    if ((value.size() % 2) != 0) {
        return false;
    }
    decoded.reserve(value.size() / 2);
    for (size_t index = 0; index < value.size(); index += 2) {
        const int high = CharToHex(value[index]);
        const int low = CharToHex(value[index + 1]);
        if (high < 0 || low < 0) {
            decoded.clear();
            return false;
        }
        decoded.push_back(static_cast<char>((high << 4) | low));
    }
    return true;
}

bool UdpAudioChannel::CryptAesCtr(const uint8_t* input, size_t input_size, const uint8_t* nonce,
                                  uint8_t* output) {
    std::lock_guard<std::mutex> lock(crypto_mutex_);
    if (aes_key_id_ == PSA_KEY_ID_NULL || input == nullptr || nonce == nullptr ||
        output == nullptr) {
        return false;
    }

    psa_cipher_operation_t operation = PSA_CIPHER_OPERATION_INIT;
    psa_status_t status = psa_cipher_encrypt_setup(&operation, aes_key_id_, PSA_ALG_CTR);
    if (status == PSA_SUCCESS) {
        status = psa_cipher_set_iv(&operation, nonce, kHeaderSize);
    }
    size_t output_len = 0;
    if (status == PSA_SUCCESS) {
        status = psa_cipher_update(&operation, input, input_size, output, input_size, &output_len);
    }
    uint8_t finish_output[kHeaderSize];
    size_t finish_len = 0;
    if (status == PSA_SUCCESS) {
        status = psa_cipher_finish(&operation, finish_output, sizeof(finish_output), &finish_len);
    }
    psa_cipher_abort(&operation);
    if (status != PSA_SUCCESS || output_len != input_size || finish_len != 0) {
        ESP_LOGE(kTag, "AES-CTR operation failed: %ld", static_cast<long>(status));
        return false;
    }
    return true;
}

void UdpAudioChannel::HandleIncomingDatagram(const std::string& data) {
    UrAiUdpHeader header;
    if (!UrAiParseUdpHeader(reinterpret_cast<const uint8_t*>(data.data()), data.size(), &header)) {
        ESP_LOGW(kTag, "ignored invalid UDP audio header");
        return;
    }

    IncomingAudioCallback callback;
    int sample_rate = 0;
    int frame_duration = 0;
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        if (header.sequence <= remote_sequence_) {
            return;
        }
        if (remote_sequence_ != 0 && header.sequence != remote_sequence_ + 1) {
            ESP_LOGW(kTag, "UDP audio sequence gap: got=%lu expected=%lu",
                     static_cast<unsigned long>(header.sequence),
                     static_cast<unsigned long>(remote_sequence_ + 1));
        }
        sample_rate = sample_rate_;
        frame_duration = frame_duration_;
        callback = incoming_audio_callback_;
    }

    auto packet = std::make_unique<AudioStreamPacket>();
    packet->sample_rate = sample_rate;
    packet->frame_duration = frame_duration;
    packet->timestamp = header.timestamp;
    packet->payload.resize(header.payload_size);
    if (!CryptAesCtr(reinterpret_cast<const uint8_t*>(data.data() + kHeaderSize),
                     header.payload_size, reinterpret_cast<const uint8_t*>(data.data()),
                     packet->payload.data())) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        if (header.sequence <= remote_sequence_) {
            return;
        }
        remote_sequence_ = header.sequence;
    }
    if (callback) {
        callback(std::move(packet));
    }
}
