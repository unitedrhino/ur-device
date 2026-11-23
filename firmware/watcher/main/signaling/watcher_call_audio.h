// 文件说明：Watcher 人际通话的16k单声道上行带外噪声抑制与下行人声整形，不增加音频队列。
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// WatcherCallCaptureAudio 在AFE后抑制实录的低频及近8kHz残留，不放大弱音或截断语音。
class WatcherCallCaptureAudio {
public:
    // Reset 在新通话建立时清除上行历史，避免上一通话的尾音进入首帧。
    void Reset() {
        delay_1_ = 0.0f;
        delay_2_ = 0.0f;
        lowpass_delay_1_ = 0.0f;
        lowpass_delay_2_ = 0.0f;
    }

    // Process 就地滤波16k单声道PCM，跨20ms帧保留状态；限幅避免瞬态转换溢出。
    void Process(std::vector<std::int16_t>& pcm) {
        for (auto& sample : pcm) {
            const float input = static_cast<float>(sample);  // AFE输出仍保持原PCM量级。
            const float highpass = kB0 * input + delay_1_;   // 二阶高通约束低频底噪。
            delay_1_ = kB1 * input - kA1 * highpass + delay_2_;
            delay_2_ = kB2 * input - kA2 * highpass;
            const float filtered = kLowB0 * highpass + lowpass_delay_1_;  // 约束采样上限附近残留。
            lowpass_delay_1_ = kLowB1 * highpass - kLowA1 * filtered + lowpass_delay_2_;
            lowpass_delay_2_ = kLowB2 * highpass - kLowA2 * filtered;
            sample =
                static_cast<std::int16_t>(std::lrintf(std::clamp(filtered, -32768.0f, 32767.0f)));
        }
        // 静音长尾低于PCM精度后清零，避免次正规浮点数的额外计算开销。
        if (std::fabs(delay_1_) < 1.0e-12f && std::fabs(delay_2_) < 1.0e-12f) {
            delay_1_ = 0.0f;
            delay_2_ = 0.0f;
        }
        if (std::fabs(lowpass_delay_1_) < 1.0e-12f && std::fabs(lowpass_delay_2_) < 1.0e-12f) {
            lowpass_delay_1_ = 0.0f;
            lowpass_delay_2_ = 0.0f;
        }
    }

private:
    // 16kHz、250Hz二阶Butterworth高通：实录对照降低低频底噪并保留完整测试语句。
    static constexpr float kB0 = 0.9329321560713878f;
    static constexpr float kB1 = -1.8658643121427756f;
    static constexpr float kB2 = 0.9329321560713878f;
    static constexpr float kA1 = -1.8613611468290827f;
    static constexpr float kA2 = 0.8703674774564693f;
    // 6kHz二阶Butterworth低通；4kHz谐波仅衰减约0.13dB，7.9kHz衰减约53dB。
    static constexpr float kLowB0 = 0.5690355937288493f;
    static constexpr float kLowB1 = 1.1380711874576985f;
    static constexpr float kLowB2 = 0.5690355937288493f;
    static constexpr float kLowA1 = 0.9428090415820634f;
    static constexpr float kLowA2 = 0.33333333333333337f;
    float delay_1_ = 0.0f;          // 仅当前人际上行使用的第一个滤波延迟状态。
    float delay_2_ = 0.0f;          // 仅当前人际上行使用的第二个滤波延迟状态。
    float lowpass_delay_1_ = 0.0f;  // 低通第一个延迟状态，与高通状态分开。
    float lowpass_delay_2_ = 0.0f;  // 低通第二个延迟状态，与高通状态分开。
};

// WatcherCallAudio 按实机听感确认的参数约束低频和峰值，状态只属于当前通话。
class WatcherCallAudio {
public:
    // Reset 在新通话开始时清除滤波历史和增益包络，避免上一通话影响首音。
    void Reset() {
        delay_1_ = 0.0f;
        delay_2_ = 0.0f;
        envelope_ = 0.0f;
    }

    // Process 就地处理16k解码PCM；跨帧保留状态，空帧不推进历史。
    void Process(std::vector<std::int16_t>& pcm) {
        for (auto& sample : pcm) {
            const float input = static_cast<float>(sample);  // 保留有符号PCM量级计算模拟输出余量。
            const float filtered = kB0 * input + delay_1_;   // 二阶Butterworth高通的当前输出。
            delay_1_ = kB1 * input - kA1 * filtered + delay_2_;
            delay_2_ = kB2 * input - kA2 * filtered;
            envelope_ = std::max(std::fabs(filtered), envelope_ * kRelease);
            const float gain = std::min(kMaxGain, kPeak / std::max(envelope_, 1.0f));
            sample = static_cast<std::int16_t>(std::lrintf(filtered * gain));
        }
        // 静音尾部低于PCM可表达精度后归零，避免长期处理浮点次正规数。
        if (std::fabs(delay_1_) < 1.0e-12f && std::fabs(delay_2_) < 1.0e-12f) {
            delay_1_ = 0.0f;
            delay_2_ = 0.0f;
        }
        if (envelope_ < 1.0e-6f)
            envelope_ = 0.0f;
    }

private:
    // 16kHz、600Hz二阶Butterworth高通系数，对齐已验证的完整语句实验。
    static constexpr float kB0 = 0.8464592541088377f;
    static constexpr float kB1 = -1.6929185082176754f;
    static constexpr float kB2 = 0.8464592541088377f;
    static constexpr float kA1 = -1.6692031429311929f;
    static constexpr float kA2 = 0.7166338735041575f;
    static constexpr float kRelease = 0.9979188352992993f;  // 30ms包络释放，逐样本恢复。
    static constexpr float kMaxGain = 4.0f;                 // 弱人声的最大倍率，避免无限抬升底噪。
    static constexpr float kPeak = 10000.0f;                // 为小扬声器保留模拟输出峰值余量。
    float delay_1_ = 0.0f;                                  // 当前通话高通的第一个延迟状态。
    float delay_2_ = 0.0f;                                  // 当前通话高通的第二个延迟状态。
    float envelope_ = 0.0f;  // 峰值包络控制瞬态与弱语音之间的增益变化。
};
