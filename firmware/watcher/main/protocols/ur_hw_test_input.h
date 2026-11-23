/* 测试串口使用 ASCII 十六进制传输 UTF-8 物品词，避开终端行编辑器的非 ASCII 分词。 */
#pragma once
#include <string>

/* UrDecodeTestWord 解码最多 64 字节的非空物品词，非法编码/NUL 返回空串。 */
inline std::string UrDecodeTestWord(const std::string& hex) {
    if (hex.empty() || hex.size() > 128 || hex.size() % 2 != 0) return {};
    // nibble 严格识别 ASCII 十六进制字符，不接受有歧义的宽松转换。
    auto nibble = [](char value) -> int {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        return -1;
    };
    std::string result;
    for (size_t i = 0; i < hex.size(); i += 2) {
        const int high = nibble(hex[i]), low = nibble(hex[i + 1]);
        if (high < 0 || low < 0 || (high == 0 && low == 0)) return {};
        result.push_back(static_cast<char>((high << 4) | low));
    }
    return result;
}
