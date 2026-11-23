// WAV文件契约测试：有效语料及畸形头在内存流执行，不依赖文件系统或音频设备。
#include "wave_file.hpp"
#include <gtest/gtest.h>
#include <sstream>

namespace ur::device {
namespace {
// encoded 生成含正负边界样本的标准测试语料。
std::string encoded(std::uint32_t rate = 16000) {
    std::ostringstream stream(std::ios::binary);
    EXPECT_TRUE(write_wave(stream, WaveAudio{rate, {-32768, -1, 0, 1, 32767}}));
    return stream.str();
}
// rejects 验证错误出口不会遗留上次成功内容，不能被应用误发为本轮语料。
void rejects(const std::string& bytes) {
    std::istringstream stream(bytes, std::ios::binary);
    WaveAudio audio{16000, {123}};
    EXPECT_FALSE(read_wave(stream, audio));
    EXPECT_EQ(audio.sample_rate, 0U);
    EXPECT_TRUE(audio.samples.empty());
}
TEST(WaveFile, RoundTripEverySupportedRateAndSignedBoundaries) {
    for (const auto rate : {16000U, 24000U, 48000U}) {
        std::istringstream stream(encoded(rate), std::ios::binary);
        WaveAudio audio;
        ASSERT_TRUE(read_wave(stream, audio));
        EXPECT_EQ(audio.sample_rate, rate);
        EXPECT_EQ(audio.samples, (std::vector<std::int16_t>{-32768, -1, 0, 1, 32767}));
    }
}
TEST(WaveFile, RejectsAllTruncationPointsAndTrailingData) {
    const auto bytes = encoded();
    for (std::size_t length = 0; length < bytes.size(); ++length)
        rejects(bytes.substr(0, length));
    rejects(bytes + "x");
}
TEST(WaveFile, RejectsInvalidFormatAndOversizedChunks) {
    for (const auto offset : {0U, 8U, 20U, 22U, 24U, 28U, 32U, 34U}) {
        auto bytes = encoded();
        bytes[offset] = static_cast<char>(bytes[offset] ^ 4);
        rejects(bytes);
    }
    for (const auto offset : {4U, 16U, 40U}) {
        auto bytes = encoded();
        bytes.replace(offset, 4, 4, static_cast<char>(255));
        rejects(bytes);
    }
    auto empty = encoded();
    empty[40] = 0;
    rejects(empty);
}
TEST(WaveFile, AncillaryPaddedChunkAllowedButDuplicateDataRejected) {
    auto bytes = encoded();
    const std::string ancillary("JUNK\x01\0\0\0x\0", 10);
    bytes.insert(36, ancillary);
    bytes[4] = static_cast<char>(static_cast<unsigned char>(bytes[4]) + 10);
    std::istringstream stream(bytes, std::ios::binary);
    WaveAudio audio;
    ASSERT_TRUE(read_wave(stream, audio));
    bytes += bytes.substr(46);
    bytes[4] = static_cast<char>(static_cast<unsigned char>(bytes[4]) + 18);
    rejects(bytes);
}
TEST(WaveFile, WriterRejectsInvalidEmptyAndLongContentAndReportsIoFailure) {
    std::ostringstream output;
    EXPECT_FALSE(write_wave(output, WaveAudio{44100, {1}}));
    EXPECT_FALSE(write_wave(output, WaveAudio{16000, {}}));
    EXPECT_FALSE(
        write_wave(output, WaveAudio{16000, std::vector<std::int16_t>(16000U * 120U + 1)}));
    EXPECT_TRUE(output.str().empty());
    output.setstate(std::ios::badbit);
    EXPECT_FALSE(write_wave(output, WaveAudio{16000, {1}}));
}
}  // namespace
}  // namespace ur::device
