// PCM16/Opus 编解码与帧长校验；输出内存有上限，坏包不能留下旧音频。
#include "opus_codec.hpp"
#include <opus.h>

namespace ur::device {
struct OpusCodec::Impl {
    OpusEncoder* encoder = nullptr;  // 发送端状态。
    OpusDecoder* decoder = nullptr;  // 接收端状态，与发送独立。
    AudioFormat format{};            // 本次显式协商格式。
    ~Impl() {
        if (encoder)
            opus_encoder_destroy(encoder);
        if (decoder)
            opus_decoder_destroy(decoder);
    }
};
OpusCodec::OpusCodec() : impl_(std::make_unique<Impl>()) {}
OpusCodec::~OpusCodec() = default;
bool OpusCodec::configure(AudioFormat format) {
    impl_ = std::make_unique<Impl>();
    if ((format.sample_rate != 16000 && format.sample_rate != 24000 &&
         format.sample_rate != 48000) ||
        format.channels != 1 || (format.frame_ms != 20 && format.frame_ms != 60))
        return false;
    int error = 0;
    impl_->encoder =
        opus_encoder_create(format.sample_rate, format.channels, OPUS_APPLICATION_VOIP, &error);
    if (error != OPUS_OK || !impl_->encoder)
        return false;
    impl_->decoder = opus_decoder_create(format.sample_rate, format.channels, &error);
    if (error != OPUS_OK || !impl_->decoder) {
        impl_ = std::make_unique<Impl>();
        return false;
    }
    impl_->format = format;
    return true;
}
bool OpusCodec::encode(const std::vector<std::int16_t>& pcm, std::vector<std::uint8_t>& opus) {
    opus.clear();
    const auto samples = impl_->format.sample_rate * impl_->format.frame_ms / 1000;
    if (!impl_->encoder || pcm.size() != static_cast<std::size_t>(samples))
        return false;
    opus.resize(1275);
    const auto size = opus_encode(impl_->encoder, pcm.data(), samples, opus.data(),
                                  static_cast<opus_int32>(opus.size()));
    if (size <= 0) {
        opus.clear();
        return false;
    }
    opus.resize(static_cast<std::size_t>(size));
    return true;
}
bool OpusCodec::decode(const std::vector<std::uint8_t>& opus, std::vector<std::int16_t>& pcm) {
    pcm.clear();
    if (!impl_->decoder || opus.empty() || opus.size() > 1275)
        return false;
    const auto samples = impl_->format.sample_rate * impl_->format.frame_ms / 1000;
    const auto actual = opus_packet_get_nb_samples(
        opus.data(), static_cast<opus_int32>(opus.size()), impl_->format.sample_rate);
    if (actual != samples || opus_packet_get_nb_channels(opus.data()) != impl_->format.channels)
        return false;
    pcm.resize(static_cast<std::size_t>(samples));
    const auto decoded = opus_decode(impl_->decoder, opus.data(),
                                     static_cast<opus_int32>(opus.size()), pcm.data(), samples, 0);
    if (decoded != samples) {
        pcm.clear();
        return false;
    }
    return true;
}
void OpusCodec::reset() {
    if (impl_->encoder)
        opus_encoder_ctl(impl_->encoder, OPUS_RESET_STATE);
    if (impl_->decoder)
        opus_decoder_ctl(impl_->decoder, OPUS_RESET_STATE);
}
}  // namespace ur::device
