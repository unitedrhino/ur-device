// 桌面测试音频文件边界：只接受有界、无压缩、单声道 PCM16 WAV，不隐藏重采样。
#pragma once
#include <istream>
#include <ostream>
#include "ur/device/audio_codec.hpp"

namespace ur::device {
// WaveAudio 保存最多两分钟的固定采样率测试语料；帧长由会话协商，不从文件推断。
struct WaveAudio {
    std::uint32_t sample_rate = 0;      // 16000、24000 或48000 Hz。
    std::vector<std::int16_t> samples;  // 小端PCM16解码后的主机整数。
};
// read_wave 校验RIFF长度、块边界、字节率、重复块及完整样本，失败清空out。
bool read_wave(std::istream& input, WaveAudio& out);
// write_wave 输出标准44字节头与PCM16；拒绝空文件、异常格式及超过两分钟的内容。
bool write_wave(std::ostream& output, const WaveAudio& audio);
}  // namespace ur::device
