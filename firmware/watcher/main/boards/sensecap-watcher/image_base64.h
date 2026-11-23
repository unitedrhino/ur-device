// SSCMA 图像的标准 Base64 解码：单次查表转换，严格校验字符、填充和目标容量。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace watcher {

// kImageBase64Values 只用于相机图像通道，不替换密码学库对密钥等输入的恒定时间实现。
inline constexpr std::array<uint8_t, 256> kImageBase64Values = [] {
    std::array<uint8_t, 256> values{};  // 255 表示不属于标准 Base64 字母表。
    for (auto& value : values)
        value = 255;
    for (size_t index = 0; index < 26; ++index) {
        values['A' + index] = index;
        values['a' + index] = index + 26;
    }
    for (size_t index = 0; index < 10; ++index)
        values['0' + index] = index + 52;
    values['+'] = 62;
    values['/'] = 63;
    return values;
}();

// DecodeImageBase64 将 source 的 size 个标准编码字符写入 target，容量为 capacity。
// 成功返回 true 并设置 written；失败时 written 为 0，调用方必须丢弃可能已写入的部分字节。
// SSCMA JSON 图像为不含空白的标准 Base64；不接受缺失填充、非法字符或非零填充位。
inline bool DecodeImageBase64(uint8_t* target, size_t capacity, size_t* written,
                              const unsigned char* source, size_t size) {
    if (!written)
        return false;
    *written = 0;
    if (size == 0)
        return true;
    if (!target || !source || size % 4 != 0)
        return false;
    const size_t groups = size / 4;  // 每组编码四字符，末组可能含一至两个填充字符。
    const size_t padding = (source[size - 1] == '=') + (source[size - 2] == '=');  // 末尾填充数量。
    const size_t tail = 3 - padding;  // 末组实际输出字节数。
    if (capacity < tail || groups - 1 > (capacity - tail) / 3)
        return false;
    const auto* last = source + size - 4;           // 先校验末组，禁止中间填充和非零尾部填充位。
    const uint8_t a = kImageBase64Values[last[0]];  // 末组第一个六位码值。
    const uint8_t b = kImageBase64Values[last[1]];  // 末组第二个码值，也包含双填充时的零位。
    const uint8_t c = padding == 2 ? 0 : kImageBase64Values[last[2]];  // 填充位置不参与码值映射。
    const uint8_t d = padding != 0 ? 0 : kImageBase64Values[last[3]];  // 无填充时保留完整末字节。
    if ((a | b | c | d) > 63 || (padding == 2 && (last[3] != '=' || (b & 15))) ||
        (padding == 1 && (c & 3)))
        return false;
    size_t offset = 0;  // 已完成转换的字节数，容量检查保证所有写入均在目标范围内。
    for (size_t index = 0; index < groups - 1; ++index) {
        const auto* group = source + index * 4;           // 不允许包含填充的完整编码组。
        const uint8_t v0 = kImageBase64Values[group[0]];  // 完整组第 1 个六位码值。
        const uint8_t v1 = kImageBase64Values[group[1]];  // 完整组第 2 个六位码值。
        const uint8_t v2 = kImageBase64Values[group[2]];  // 完整组第 3 个六位码值。
        const uint8_t v3 = kImageBase64Values[group[3]];  // 完整组第 4 个六位码值。
        if ((v0 | v1 | v2 | v3) > 63)
            return false;
        target[offset++] = (v0 << 2) | (v1 >> 4);
        target[offset++] = (v1 << 4) | (v2 >> 2);
        target[offset++] = (v2 << 6) | v3;
    }
    target[offset++] = (a << 2) | (b >> 4);
    if (tail > 1)
        target[offset++] = (b << 4) | (c >> 2);
    if (tail > 2)
        target[offset++] = (c << 6) | d;
    *written = offset;
    return true;
}

}  // namespace watcher
