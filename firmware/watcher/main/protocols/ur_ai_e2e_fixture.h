/* 联犀语音 E2E 内置音频资源解析器；资源由 devicesim 的真实 MP3 样本生成。 */
#ifndef UR_AI_E2E_FIXTURE_H
#define UR_AI_E2E_FIXTURE_H

#include <cstddef>
#include <cstdint>

/* UrAiE2eAudioFrame 是一个不拥有内存的单帧 Opus 视图。 */
struct UrAiE2eAudioFrame {
    const uint8_t* data = nullptr;
    size_t size = 0;
};

/* UrAiE2eFixture 顺序读取内置 URAF 音频包，避免运行时复制整段测试音频。 */
class UrAiE2eFixture {
public:
    /* 三个选择参数互斥；tank_forward为坦克前进语音，默认保留音量查询。 */
    explicit UrAiE2eFixture(bool take_photo = false, bool control_volume = false, bool tank_forward = false);

    /* IsValid 表示包头、编码参数和全部帧边界均通过校验。 */
    bool IsValid() const { return valid_; }
    /* sample_rate 返回 Opus 采样率。 */
    uint16_t sample_rate() const { return sample_rate_; }
    /* frame_duration_ms 返回每个 Opus 帧的时长。 */
    uint16_t frame_duration_ms() const { return frame_duration_ms_; }
    /* frame_count 返回资源中的总帧数。 */
    uint16_t frame_count() const { return frame_count_; }
    /* Next 返回下一帧；全部读取后返回 false。 */
    bool Next(UrAiE2eAudioFrame* frame);

private:
    const uint8_t* cursor_ = nullptr;
    const uint8_t* end_ = nullptr;
    uint16_t sample_rate_ = 0;
    uint16_t frame_duration_ms_ = 0;
    uint16_t frame_count_ = 0;
    uint16_t frame_index_ = 0;
    bool valid_ = false;
};

#endif  // UR_AI_E2E_FIXTURE_H
