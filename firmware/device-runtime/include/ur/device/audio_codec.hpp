// 音频编解码接口与实际发声检测，设备驱动和桌面适配共用数据单位。
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ur::device {
// AudioFormat 的 sample_rate 为 PCM 采样率，不是 RTP 时钟；frame_ms 为单帧时长。
struct AudioFormat {
    int sample_rate = 16000;  // Hz，主测16k，兼容24k。
    int channels = 1;         // 声道数。
    int frame_ms = 20;        // 毫秒，主要20ms，兼容60ms。
};
// AudioCodecPort 不隐藏重采样；输入 PCM 必须符合显式配置。
class AudioCodecPort {
public:
    virtual ~AudioCodecPort() = default;
    virtual bool configure(AudioFormat format) = 0;  // 失败不留下部分初始化状态。
    virtual bool encode(const std::vector<std::int16_t>& pcm, std::vector<std::uint8_t>& opus) = 0;
    virtual bool decode(const std::vector<std::uint8_t>& opus, std::vector<std::int16_t>& pcm) = 0;
    virtual void reset() = 0;  // 清空编解码历史，避免跨会话残留。
};
// non_silent_pcm 使用固定RMS阈值定义测量用非静音帧，不承担语义VAD/AEC或过滤上行的职责。
bool non_silent_pcm(const std::vector<std::int16_t>& pcm);
// SpeechOnset 统计连续两个有效非静音帧；只衡量解码可播放音频，不宣称扬声器起播。
class SpeechOnset {
public:
    // observe 接受本帧 PCM；返回 true 表示已达到连续两帧，空帧视为非有效音频。
    bool observe(const std::vector<std::int16_t>& pcm);
    void reset() { count_ = 0; }  // 新回复必须重置，不能跨回复拼接两帧。
private:
    unsigned count_ = 0;  // 连续非静音帧数，上限2。
};
}  // namespace ur::device
