// 有效发声检测：PCM16 均方能量阈值，不以网络首包或压缩字节数判断起声。
#include "ur/device/audio_codec.hpp"

namespace ur::device {
bool non_silent_pcm(const std::vector<std::int16_t>& pcm) {
    if (pcm.empty())
        return false;
    double sum = 0;
    for (auto sample : pcm)
        sum += static_cast<double>(sample) * sample;
    // RMS >= 128（满量程约 -48dB）；阈值是测量口径，不承担 VAD/AEC 能力声明。
    return sum / static_cast<double>(pcm.size()) >= 128.0 * 128.0;
}
bool SpeechOnset::observe(const std::vector<std::int16_t>& pcm) {
    if (!non_silent_pcm(pcm)) {
        count_ = 0;
        return false;
    }
    if (count_ < 2)
        ++count_;
    return count_ == 2;
}
}  // namespace ur::device
