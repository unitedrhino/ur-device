/* 联犀 AI 可移植协议核心实现；不依赖 ESP-IDF，供固件与主机测试共同使用。 */
#include "ur_ai_runtime.h"

void UrAiVadEndpoint::Begin(int64_t now_us) {
    ready_us_ = now_us + kWarmupUs;
    speech_start_us_ = 0;
    speech_us_ = 0;
    silence_start_us_ = 0;
    speaking_ = false;
}

void UrAiVadEndpoint::Update(bool speaking, int64_t now_us) {
    if (speaking == speaking_) return;
    speaking_ = speaking;
    if (speaking) {
        speech_start_us_ = now_us > ready_us_ ? now_us : ready_us_;
        silence_start_us_ = 0;
    } else {
        if (now_us > speech_start_us_) speech_us_ += now_us - speech_start_us_;
        silence_start_us_ = now_us;
    }
}

bool UrAiVadEndpoint::ShouldStop(int64_t now_us) const {
    return !speaking_ && speech_us_ >= kMinSpeechUs && silence_start_us_ > 0 &&
           now_us - silence_start_us_ >= silence_us_;
}

#include <array>
#include <cstring>

namespace {
/* EmotionPair 保存 devicesim emotion_test.go 定义的 emoji 与设备表情名称。 */
struct EmotionPair {
    std::string_view emoji;
    std::string_view emotion;
};

/* kEmotionMap 是固件唯一的联犀表情白名单。 */
constexpr std::array<EmotionPair, 21> kEmotionMap = {{
    {"😶", "neutral"},     {"🙂", "happy"},     {"😆", "laughing"}, {"😂", "funny"},
    {"😔", "sad"},         {"😠", "angry"},     {"😭", "crying"},   {"😍", "loving"},
    {"😳", "embarrassed"}, {"😲", "surprised"}, {"😱", "shocked"},  {"🤔", "thinking"},
    {"😉", "winking"},     {"😎", "cool"},      {"😌", "relaxed"},  {"🤤", "delicious"},
    {"😘", "kissy"},       {"😏", "confident"}, {"😴", "sleepy"},   {"😜", "silly"},
    {"🙄", "confused"},
}};

/* WriteUint16 写入网络字节序 16 位整数，避免主机端测试依赖 socket 头文件。 */
void WriteUint16(uint8_t* output, uint16_t value) {
    output[0] = static_cast<uint8_t>(value >> 8);
    output[1] = static_cast<uint8_t>(value);
}

/* WriteUint32 写入网络字节序 32 位整数。 */
void WriteUint32(uint8_t* output, uint32_t value) {
    output[0] = static_cast<uint8_t>(value >> 24);
    output[1] = static_cast<uint8_t>(value >> 16);
    output[2] = static_cast<uint8_t>(value >> 8);
    output[3] = static_cast<uint8_t>(value);
}

/* ReadUint16 读取网络字节序 16 位整数。 */
uint16_t ReadUint16(const uint8_t* input) {
    return static_cast<uint16_t>((static_cast<uint16_t>(input[0]) << 8) | input[1]);
}

/* ReadUint32 读取网络字节序 32 位整数。 */
uint32_t ReadUint32(const uint8_t* input) {
    return (static_cast<uint32_t>(input[0]) << 24) | (static_cast<uint32_t>(input[1]) << 16) |
           (static_cast<uint32_t>(input[2]) << 8) | input[3];
}
}  // namespace

void UrAiTerminalTracker::Reset() { seen_mask_ = 0; }

bool UrAiTerminalTracker::Mark(UrAiTerminalEvent event) {
    const uint8_t mask = static_cast<uint8_t>(1U << static_cast<uint8_t>(event));
    if ((seen_mask_ & mask) != 0) {
        return false;
    }
    seen_mask_ |= mask;
    return true;
}

bool UrAiTerminalTracker::Seen(UrAiTerminalEvent event) const {
    const uint8_t mask = static_cast<uint8_t>(1U << static_cast<uint8_t>(event));
    return (seen_mask_ & mask) != 0;
}

std::string_view UrAiNormalizeEmotion(std::string_view emoji, std::string_view emotion) {
    for (const auto& item : kEmotionMap) {
        if ((!emoji.empty() && emoji == item.emoji) ||
            (!emotion.empty() && emotion == item.emotion)) {
            return item.emotion;
        }
    }
    return "neutral";
}

bool UrAiMatchesSession(std::string_view current_session, std::string_view incoming_session) {
    return incoming_session.empty() ||
           (!current_session.empty() && current_session == incoming_session);
}

UrAiResponseDisposition UrAiClassifyResponse(std::string_view current_response,
                                             std::string_view incoming_response,
                                             std::string_view method) {
    if (incoming_response.empty()) {
        return UrAiResponseDisposition::kReject;
    }
    if (current_response.empty()) {
        return method == "respTextDelta" ? UrAiResponseDisposition::kReject
                                         : UrAiResponseDisposition::kDefer;
    }
    return current_response == incoming_response ? UrAiResponseDisposition::kAccept
                                                 : UrAiResponseDisposition::kReject;
}

bool UrAiBuildUdpHeader(const uint8_t* nonce_template, size_t nonce_size, size_t payload_size,
                        uint32_t timestamp, uint32_t sequence, bool preserve_nonce_timestamp,
                        uint8_t* output, size_t output_size) {
    if (nonce_template == nullptr || nonce_size != kUrAiUdpHeaderSize || output == nullptr ||
        output_size != kUrAiUdpHeaderSize || payload_size == 0 || payload_size > UINT16_MAX) {
        return false;
    }
    std::memcpy(output, nonce_template, kUrAiUdpHeaderSize);
    WriteUint16(output + 2, static_cast<uint16_t>(payload_size));
    if (!preserve_nonce_timestamp) {
        WriteUint32(output + 8, timestamp);
    }
    WriteUint32(output + 12, sequence);
    return true;
}

bool UrAiParseUdpHeader(const uint8_t* datagram, size_t datagram_size, UrAiUdpHeader* output) {
    if (datagram == nullptr || output == nullptr || datagram_size < kUrAiUdpHeaderSize ||
        datagram[0] != 0x01) {
        return false;
    }
    UrAiUdpHeader parsed;
    parsed.payload_size = ReadUint16(datagram + 2);
    parsed.timestamp = ReadUint32(datagram + 8);
    parsed.sequence = ReadUint32(datagram + 12);
    if (parsed.payload_size == 0 || datagram_size != kUrAiUdpHeaderSize + parsed.payload_size) {
        return false;
    }
    *output = parsed;
    return true;
}
