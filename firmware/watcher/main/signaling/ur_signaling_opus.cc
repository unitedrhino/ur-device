// 文件说明：统一信令 Opus 编解码，分别配置人际低延迟和 AI 语音。
#include "ur_signaling_opus.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <sdkconfig.h>

#include <cstring>

namespace ur::device {
namespace {
constexpr const char* kTag = "ur_sig_opus";

esp_opus_enc_frame_duration_t DurationEnum(int frame_ms) {
    switch (frame_ms) {
        case 10:
            return ESP_OPUS_ENC_FRAME_DURATION_10_MS;
        case 20:
            return ESP_OPUS_ENC_FRAME_DURATION_20_MS;
        case 40:
            return ESP_OPUS_ENC_FRAME_DURATION_40_MS;
        case 60:
            return ESP_OPUS_ENC_FRAME_DURATION_60_MS;
        default:
            return static_cast<esp_opus_enc_frame_duration_t>(-1);
    }
}
}  // namespace

UrSignalingOpus::~UrSignalingOpus() {
    std::scoped_lock lock(encoder_mutex_, decoder_mutex_);
    if (human_call_) {
        ESP_LOGI(kTag, "call codec summary encoded=%u decoded=%u encode_us=%llu decode_us=%llu",
                 encode_frames_, decode_frames_, static_cast<unsigned long long>(encode_us_),
                 static_cast<unsigned long long>(decode_us_));
    }
    if (encoder_ != nullptr) {
        esp_opus_enc_close(encoder_);
        encoder_ = nullptr;
    }
    if (decoder_ != nullptr) {
        esp_opus_dec_close(decoder_);
        decoder_ = nullptr;
    }
}

bool UrSignalingOpus::configure(AudioFormat format) { return Configure(format, false); }
bool UrSignalingOpus::configure_for_call(AudioFormat format) { return Configure(format, true); }
bool UrSignalingOpus::Configure(AudioFormat format, bool human_call) {
    std::scoped_lock lock(encoder_mutex_, decoder_mutex_);
    if (encoder_ != nullptr) {
        esp_opus_enc_close(encoder_);
        encoder_ = nullptr;
    }
    if (decoder_ != nullptr) {
        esp_opus_dec_close(decoder_);
        decoder_ = nullptr;
    }
    /* UDP 形态 16k；WebRTC 形态 48k（协议校验要求）。解码输出固定 16k：
     * opus 解码器可把任意原生采样率码流直接产出 16k，两种形态的下游播放
     * 路径（16k→24k 硬件输出重采样）保持一致。 */
    if ((format.sample_rate != 16000 && format.sample_rate != 48000) || format.channels != 1) {
        ESP_LOGE(kTag, "unsupported signaling audio format rate=%d ch=%d", format.sample_rate,
                 format.channels);
        return false;
    }
    const auto duration = DurationEnum(format.frame_ms);
    const int duration_value = static_cast<int>(duration);
    if (duration_value < 0) {
        ESP_LOGE(kTag, "unsupported frame duration %d", format.frame_ms);
        return false;
    }
    esp_opus_enc_config_t enc_cfg = {};
    enc_cfg.sample_rate =
        format.sample_rate == 48000 ? ESP_AUDIO_SAMPLE_RATE_48K : ESP_AUDIO_SAMPLE_RATE_16K;
    enc_cfg.channel = ESP_AUDIO_MONO;
    enc_cfg.bits_per_sample = ESP_AUDIO_BIT16;
    enc_cfg.bitrate = human_call ? 24000 : ESP_OPUS_BITRATE_AUTO;
    enc_cfg.frame_duration = duration;
    // 人际媒体使用已验证的低延迟用途；VOIP实机未改善供帧，AI仍使用音频用途。
    enc_cfg.application_mode =
        human_call ? ESP_OPUS_ENC_APPLICATION_LOWDELAY : ESP_OPUS_ENC_APPLICATION_AUDIO;
    enc_cfg.complexity = 0;
#if CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
    if (!human_call && format.sample_rate == 16000 && format.frame_ms == 20) {
        // 固定码率与适度提高复杂度，可减少弱人声编码后的关键词退化。
        enc_cfg.bitrate = 32000;
        enc_cfg.complexity = 3;
    }
#endif
    enc_cfg.enable_fec = false;
    enc_cfg.enable_dtx = false;
    enc_cfg.enable_vbr = true;
    if (esp_opus_enc_open(&enc_cfg, sizeof(enc_cfg), &encoder_) != ESP_AUDIO_ERR_OK ||
        encoder_ == nullptr) {
        ESP_LOGE(kTag, "opus encoder open failed");
        encoder_ = nullptr;
        return false;
    }
    int frame_bytes = 0;
    esp_opus_enc_get_frame_size(encoder_, &frame_bytes, &encoder_outbuf_size_);
    encoder_frame_samples_ = frame_bytes / static_cast<int>(sizeof(int16_t));
    if (encoder_frame_samples_ != format.sample_rate / 1000 * format.frame_ms) {
        ESP_LOGE(kTag, "unexpected encoder frame samples %d", encoder_frame_samples_);
        esp_opus_enc_close(encoder_);
        encoder_ = nullptr;
        return false;
    }

    esp_opus_dec_cfg_t dec_cfg = {};
    dec_cfg.sample_rate = static_cast<int>(ESP_AUDIO_SAMPLE_RATE_16K);
    /* channel 零值非法（0 声道会被组件参数校验拒绝并返回 ESP_AUDIO_ERR_FAIL），
     * 必须显式置单声道。 */
    dec_cfg.channel = ESP_AUDIO_MONO;
    dec_cfg.frame_duration = static_cast<esp_opus_dec_frame_duration_t>(duration);
    if (esp_opus_dec_open(&dec_cfg, sizeof(dec_cfg), &decoder_) != ESP_AUDIO_ERR_OK ||
        decoder_ == nullptr) {
        ESP_LOGE(kTag, "opus decoder open failed");
        esp_opus_enc_close(encoder_);
        encoder_ = nullptr;
        decoder_ = nullptr;
        return false;
    }
    /* 解码输出固定 16k（见 configure 顶部说明），与上行编码采样率解耦。 */
    decoder_frame_samples_ = 16000 / 1000 * format.frame_ms;
    format_ = format;
    human_call_ = human_call;
    encode_us_ = decode_us_ = 0;
    encode_frames_ = decode_frames_ = 0;
    return true;
}

bool UrSignalingOpus::encode(const std::vector<std::int16_t>& pcm,
                             std::vector<std::uint8_t>& opus) {
    std::lock_guard<std::mutex> lock(encoder_mutex_);
    opus.clear();
    if (encoder_ == nullptr || pcm.size() != static_cast<size_t>(encoder_frame_samples_)) {
        return false;
    }
    opus.resize(static_cast<size_t>(encoder_outbuf_size_));
    esp_audio_enc_in_frame_t in = {};
    in.buffer = reinterpret_cast<uint8_t*>(const_cast<int16_t*>(pcm.data()));
    in.len = static_cast<uint32_t>(pcm.size() * sizeof(int16_t));
    esp_audio_enc_out_frame_t out = {};
    out.buffer = opus.data();
    out.len = static_cast<uint32_t>(opus.size());
    out.encoded_bytes = 0;
    const auto started = esp_timer_get_time();
    if (esp_opus_enc_process(encoder_, &in, &out) != ESP_AUDIO_ERR_OK || out.encoded_bytes == 0) {
        opus.clear();
        return false;
    }
    encode_us_ += esp_timer_get_time() - started;
    ++encode_frames_;
    opus.resize(out.encoded_bytes);
    return true;
}

bool UrSignalingOpus::decode(const std::vector<std::uint8_t>& opus,
                             std::vector<std::int16_t>& pcm) {
    std::lock_guard<std::mutex> lock(decoder_mutex_);
    pcm.clear();
    if (decoder_ == nullptr || opus.empty() || opus.size() > 1275) {
        return false;
    }
    pcm.resize(static_cast<size_t>(decoder_frame_samples_));
    esp_audio_dec_in_raw_t raw = {};
    raw.buffer = const_cast<uint8_t*>(opus.data());
    raw.len = static_cast<uint32_t>(opus.size());
    raw.consumed = 0;
    raw.frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE;
    esp_audio_dec_out_frame_t out = {};
    out.buffer = reinterpret_cast<uint8_t*>(pcm.data());
    out.len = static_cast<uint32_t>(pcm.size() * sizeof(int16_t));
    out.decoded_size = 0;
    esp_audio_dec_info_t info = {};
    const auto started = esp_timer_get_time();
    if (esp_opus_dec_decode(decoder_, &raw, &out, &info) != ESP_AUDIO_ERR_OK ||
        out.decoded_size == 0) {
        pcm.clear();
        return false;
    }
    decode_us_ += esp_timer_get_time() - started;
    ++decode_frames_;
    pcm.resize(out.decoded_size / sizeof(int16_t));
    return true;
}

void UrSignalingOpus::reset() {
    AudioFormat format;  // 持有者串行重置时保存本次格式，算法调用先在两锁内结束。
    bool human_call;     // 保留既有通话编码配置，不能切回AI默认用途。
    {
        std::scoped_lock lock(encoder_mutex_, decoder_mutex_);
        if (encoder_ == nullptr && decoder_ == nullptr) {
            return;
        }
        format = format_;
        human_call = human_call_;
    }
    // 组件无单独重置入口；释放状态锁后由Configure统一重建，避免递归加锁。
    Configure(format, human_call);
}

}  // namespace ur::device
