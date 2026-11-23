// RIFF音频读写实现；所有长度先验证再分配，畸形或截断输入不产生半份语料。
#include "wave_file.hpp"
#include <array>
#include <cstring>

namespace ur::device {
namespace {
// max_bytes 限定WAV正文与附加块总预算，避免文件头伪造导致无界内存分配。
constexpr std::size_t max_bytes = 48000U * 120U * 2U + 4096U;
// u16/u32 以字节解析小端整数，不依赖主机端序或非对齐指针。
std::uint16_t u16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (static_cast<unsigned>(p[1]) << 8));
}
std::uint32_t u32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(u16(p)) | (static_cast<std::uint32_t>(u16(p + 2)) << 16);
}
// rate_valid 对齐桌面Opus支持矩阵，文件模块不进行隐式转换。
bool rate_valid(std::uint32_t rate) { return rate == 16000 || rate == 24000 || rate == 48000; }
// put32 写出小端字段；16位字段由零初始化头配合put32安全覆盖。
void put32(std::uint8_t* p, std::uint32_t value) {
    for (unsigned i = 0; i != 4; ++i)
        p[i] = static_cast<std::uint8_t>(value >> (i * 8));
}
}  // namespace

bool read_wave(std::istream& input, WaveAudio& out) {
    out = {};
    std::array<std::uint8_t, 12> header{};
    if (!input.read(reinterpret_cast<char*>(header.data()), header.size()) ||
        std::memcmp(header.data(), "RIFF", 4) != 0 ||
        std::memcmp(header.data() + 8, "WAVE", 4) != 0)
        return false;
    const auto length = u32(header.data() + 4);
    if (length < 4 || length > max_bytes)
        return false;
    std::vector<std::uint8_t> body(length - 4);
    if (!input.read(reinterpret_cast<char*>(body.data()),
                    static_cast<std::streamsize>(body.size())) ||
        input.peek() != std::char_traits<char>::eof())
        return false;
    WaveAudio candidate;
    bool format_seen = false;
    bool data_seen = false;
    for (std::size_t offset = 0; offset < body.size();) {
        if (body.size() - offset < 8)
            return false;
        const auto* chunk = body.data() + offset;
        const auto size = static_cast<std::size_t>(u32(chunk + 4));
        if (size > body.size() - offset - 8)
            return false;
        const auto padded = size + (size % 2);
        if (padded > body.size() - offset - 8)
            return false;
        const auto* data = chunk + 8;
        if (std::memcmp(chunk, "fmt ", 4) == 0) {
            if (format_seen || data_seen || (size != 16 && size != 18) || u16(data) != 1 ||
                u16(data + 2) != 1 || !rate_valid(u32(data + 4)) ||
                u32(data + 8) != u32(data + 4) * 2 || u16(data + 12) != 2 || u16(data + 14) != 16 ||
                (size == 18 && u16(data + 16) != 0))
                return false;
            format_seen = true;
            candidate.sample_rate = u32(data + 4);
        } else if (std::memcmp(chunk, "data", 4) == 0) {
            if (!format_seen || data_seen || size == 0 || size % 2 != 0 ||
                size / 2 > static_cast<std::size_t>(candidate.sample_rate) * 120)
                return false;
            data_seen = true;
            candidate.samples.reserve(size / 2);
            for (std::size_t i = 0; i < size; i += 2) {
                const auto value = static_cast<std::int32_t>(u16(data + i));
                candidate.samples.push_back(
                    static_cast<std::int16_t>(value >= 32768 ? value - 65536 : value));
            }
        }
        offset += 8 + padded;
    }
    if (!format_seen || !data_seen)
        return false;
    out = std::move(candidate);
    return true;
}

bool write_wave(std::ostream& output, const WaveAudio& audio) {
    if (!rate_valid(audio.sample_rate) || audio.samples.empty() ||
        audio.samples.size() > static_cast<std::size_t>(audio.sample_rate) * 120)
        return false;
    std::array<std::uint8_t, 44> header{};
    std::memcpy(header.data(), "RIFF", 4);
    const auto bytes = static_cast<std::uint32_t>(audio.samples.size() * 2);
    put32(header.data() + 4, bytes + 36);
    std::memcpy(header.data() + 8, "WAVEfmt ", 8);
    put32(header.data() + 16, 16);
    header[20] = 1;
    header[22] = 1;
    put32(header.data() + 24, audio.sample_rate);
    put32(header.data() + 28, audio.sample_rate * 2);
    header[32] = 2;
    header[34] = 16;
    std::memcpy(header.data() + 36, "data", 4);
    put32(header.data() + 40, bytes);
    output.write(reinterpret_cast<const char*>(header.data()), header.size());
    for (const auto sample : audio.samples) {
        const auto value = static_cast<std::uint16_t>(sample);
        const std::array<char, 2> encoded{static_cast<char>(value & 255),
                                          static_cast<char>(value >> 8)};
        output.write(encoded.data(), encoded.size());
    }
    return static_cast<bool>(output);
}
}  // namespace ur::device
