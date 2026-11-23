/* 统一信令 Opus 编解码端口：独立 16k/20ms 实例，不影响旧协议 60ms 管线。 */
#ifndef UR_SIGNALING_OPUS_H
#define UR_SIGNALING_OPUS_H

#include "ur/device/audio_codec.hpp"

#include <mutex>

#include "esp_opus_dec.h"
#include "esp_opus_enc.h"

namespace ur::device {

/* UrSignalingOpus 允许上行编码与下行解码并发；配置、重置和关闭仍由持有者串行调用。 */
class UrSignalingOpus final : public AudioCodecPort {
public:
    UrSignalingOpus() = default;
    ~UrSignalingOpus() override;
    UrSignalingOpus(const UrSignalingOpus&) = delete;
    UrSignalingOpus& operator=(const UrSignalingOpus&) = delete;

    /* configure 按显式格式重建编解码实例；失败不留半初始化状态。 */
    bool configure(AudioFormat format) override;
    // configure_for_call 使用24kbps低延迟模式，输入PCM与RTP时钟合同不变，AI默认编码不变。
    bool configure_for_call(AudioFormat format);
    /* encode 输入必须为配置帧长的 PCM，输出为单帧 Opus。 */
    bool encode(const std::vector<std::int16_t>& pcm, std::vector<std::uint8_t>& opus) override;
    /* decode 接受合法 Opus 帧，输出配置帧长 PCM。 */
    bool decode(const std::vector<std::uint8_t>& opus, std::vector<std::int16_t>& pcm) override;
    /* reset 清空编解码历史，避免跨会话残留。 */
    void reset() override;

private:
    // Configure 串行创建本次编解码器；human_call只调整低延迟用途和目标码率。
    bool Configure(AudioFormat format, bool human_call);
    // 编码和解码使用独立实例及锁，持续收音不阻塞下行；生命周期操作同时持有两锁。
    std::mutex encoder_mutex_;
    std::mutex decoder_mutex_;
    void* encoder_ = nullptr;
    void* decoder_ = nullptr;
    AudioFormat format_{};
    bool human_call_ = false;  // 重建后仍保持本次人际配置，AI默认路径不启用。
    std::uint64_t encode_us_ = 0, decode_us_ = 0;  // 本实例编解码累计耗时，非端到端延迟。
    unsigned encode_frames_ = 0, decode_frames_ = 0;  // 成功处理帧，不代表扬声器已播放。

    int encoder_frame_samples_ = 0;
    int encoder_outbuf_size_ = 0;
    int decoder_frame_samples_ = 0;
};

}  // namespace ur::device

#endif /* UR_SIGNALING_OPUS_H */
