// libopus 桌面编解码器；核心接口不暴露 OpusEncoder/Decoder 句柄。
#pragma once
#include <memory>
#include "ur/device/audio_codec.hpp"

namespace ur::device {
class OpusCodec final : public AudioCodecPort {
public:
    OpusCodec();
    ~OpusCodec() override;
    OpusCodec(const OpusCodec&) = delete;
    OpusCodec& operator=(const OpusCodec&) = delete;
    bool configure(AudioFormat format) override;
    bool encode(const std::vector<std::int16_t>& pcm, std::vector<std::uint8_t>& opus) override;
    bool decode(const std::vector<std::uint8_t>& opus, std::vector<std::int16_t>& pcm) override;
    void reset() override;

private:
    struct Impl;                  // 平台编解码器状态及显式音频参数。
    std::unique_ptr<Impl> impl_;  // 独占有状态编解码器，不跨会话共享。
};
}  // namespace ur::device
