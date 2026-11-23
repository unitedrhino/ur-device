/* 联犀语音 E2E 内置音频资源解析实现。 */
#include "ur_ai_e2e_fixture.h"

#include <cstdint>
#include <cstring>

#include <esp_log.h>

namespace {
constexpr uint8_t kFixtureMagic[] = {'U', 'R', 'A', 'F'};
constexpr uint16_t kFixtureVersion = 1;
/* kFixtureSampleRate 必须与联犀会话和设备 AFE 的 16 kHz 编码合同一致。 */
constexpr uint16_t kFixtureSampleRate = 16000;
constexpr size_t kFixtureHeaderSize = 12;
constexpr size_t kMaxOpusFrameBytes = 4096;
constexpr const char* kTag = "UrAiE2eFixture";

/* ESP-IDF 的 EMBED_FILES 按文件名生成链接符号，不包含传入的相对目录。 */
extern const uint8_t kFixtureStart[] asm("_binary_ur_ai_query_volume_opuspack_start");
extern const uint8_t kFixtureEnd[] asm("_binary_ur_ai_query_volume_opuspack_end");
/* 拍照语音复用同一编码/解析合同，不把图片或识别文字伪造为上行音频。 */
extern const uint8_t kPhotoFixtureStart[] asm("_binary_ur_ai_take_photo_opuspack_start");
extern const uint8_t kPhotoFixtureEnd[] asm("_binary_ur_ai_take_photo_opuspack_end");
/* 设置语音用于实际控制回归，仍由devicesim编码器生成，不注入识别文字。 */
extern const uint8_t kControlFixtureStart[] asm("_binary_ur_ai_control_volume_opuspack_start");
extern const uint8_t kControlFixtureEnd[] asm("_binary_ur_ai_control_volume_opuspack_end");

/* 前进样本用于真实ASR、模型行为调用及底盘接收的联合验收。 */
extern const uint8_t kForwardFixtureStart[] asm("_binary_ur_ai_tank_forward_opuspack_start");
extern const uint8_t kForwardFixtureEnd[] asm("_binary_ur_ai_tank_forward_opuspack_end");

/* ReadU16 读取 URAF 格式中的大端无符号整数。 */
uint16_t ReadU16(const uint8_t* data) {
    return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) | data[1]);
}
}  // namespace

UrAiE2eFixture::UrAiE2eFixture(bool take_photo, bool control_volume, bool tank_forward) {
    if ((take_photo && control_volume) || (tank_forward && (take_photo || control_volume))) return;
    const uint8_t* fixture_start = tank_forward ? kForwardFixtureStart :
        (control_volume ? kControlFixtureStart : (take_photo ? kPhotoFixtureStart : kFixtureStart));
    const uint8_t* fixture_end = tank_forward ? kForwardFixtureEnd :
        (control_volume ? kControlFixtureEnd : (take_photo ? kPhotoFixtureEnd : kFixtureEnd));
    /*
     * start/end 是链接器分别导出的符号，不是同一个 C++ 数组对象。先转为整数地址
     * 计算长度，再从 start 派生 end，避免跨对象指针相减/比较触发未定义行为。
     */
    const uintptr_t start_address = reinterpret_cast<uintptr_t>(fixture_start);
    const uintptr_t end_address = reinterpret_cast<uintptr_t>(fixture_end);
    if (end_address <= start_address) {
        ESP_LOGE(kTag, "fixture address range invalid");
        return;
    }
    const size_t size = end_address - start_address;
    if (size < kFixtureHeaderSize) {
        ESP_LOGE(kTag, "fixture too short: size=%u", static_cast<unsigned>(size));
        return;
    }
    if (std::memcmp(fixture_start, kFixtureMagic, sizeof(kFixtureMagic)) != 0) {
        ESP_LOGE(kTag, "fixture magic mismatch: size=%u magic=%02x%02x%02x%02x",
                 static_cast<unsigned>(size), fixture_start[0], fixture_start[1],
                 fixture_start[2], fixture_start[3]);
        return;
    }
    const uint16_t version = ReadU16(fixture_start + 4);
    sample_rate_ = ReadU16(fixture_start + 6);
    frame_duration_ms_ = ReadU16(fixture_start + 8);
    frame_count_ = ReadU16(fixture_start + 10);
    if (version != kFixtureVersion || sample_rate_ != kFixtureSampleRate ||
        frame_duration_ms_ != 60 || frame_count_ == 0) {
        ESP_LOGE(kTag, "fixture metadata mismatch: version=%u sample_rate=%u frame_ms=%u frames=%u",
                 version, sample_rate_, frame_duration_ms_, frame_count_);
        return;
    }

    cursor_ = fixture_start + kFixtureHeaderSize;
    end_ = fixture_start + size;
    const uint8_t* validation_cursor = cursor_;
    for (uint16_t index = 0; index < frame_count_; ++index) {
        if (end_ - validation_cursor < 2) {
            ESP_LOGE(kTag, "fixture frame header truncated: index=%u remaining=%u", index,
                     static_cast<unsigned>(end_ - validation_cursor));
            return;
        }
        const size_t frame_size = ReadU16(validation_cursor);
        validation_cursor += 2;
        if (frame_size == 0 || frame_size > kMaxOpusFrameBytes ||
            static_cast<size_t>(end_ - validation_cursor) < frame_size) {
            ESP_LOGE(kTag, "fixture frame invalid: index=%u size=%u remaining=%u", index,
                     static_cast<unsigned>(frame_size),
                     static_cast<unsigned>(end_ - validation_cursor));
            return;
        }
        validation_cursor += frame_size;
    }
    /* 拒绝尾随数据，保证生成器和固件对帧边界的理解完全一致。 */
    valid_ = static_cast<size_t>(validation_cursor - fixture_start) == size;
    if (!valid_) {
        ESP_LOGE(kTag, "fixture trailing bytes: parsed=%u total=%u",
                 static_cast<unsigned>(validation_cursor - fixture_start),
                 static_cast<unsigned>(size));
    }
}

bool UrAiE2eFixture::Next(UrAiE2eAudioFrame* frame) {
    if (!valid_ || frame == nullptr || frame_index_ >= frame_count_ || end_ - cursor_ < 2) {
        return false;
    }
    const size_t frame_size = ReadU16(cursor_);
    cursor_ += 2;
    if (frame_size == 0 || static_cast<size_t>(end_ - cursor_) < frame_size) {
        valid_ = false;
        return false;
    }
    frame->data = cursor_;
    frame->size = frame_size;
    cursor_ += frame_size;
    ++frame_index_;
    return true;
}
