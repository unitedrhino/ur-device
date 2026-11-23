// Watcher UDP视频在ROM解码前检查尺寸及采样格式，允许将软件工作区放入PSRAM。
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// WatcherRomJpegDimensions 从bytes的size字节解析适用ROM解码的SOF0，成功回写width/height。
// 只选择已验证的三分量YCbCr及4:4:4、4:2:2、4:2:0；其他格式交回既有解码器。
// 本函数只检查帧头，完整压缩数据仍由解码器校验，不把帧头成功当作图片有效。
inline bool WatcherRomJpegDimensions(const uint8_t* bytes, size_t size, int& width, int& height) {
    if (bytes == nullptr || size < 4 || bytes[0] != 0xff || bytes[1] != 0xd8)
        return false;
    size_t at = 2;  // 跳过SOI，所有段长都先校验剩余字节再读取内容。
    while (at + 1 < size) {
        if (bytes[at++] != 0xff)
            return false;
        while (at < size && bytes[at] == 0xff)
            ++at;
        if (at >= size)
            return false;
        const auto marker = bytes[at++];  // SOS或EOI之前必须找到可检查的尺寸段。
        if (marker == 0x00 || marker == 0xd8 || marker == 0xd9 || marker == 0xda)
            return false;
        if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7))
            continue;
        if (at + 2 > size)
            return false;
        const size_t length = (bytes[at] << 8) | bytes[at + 1];  // JPEG段长包含长度字段。
        if (length < 2 || length > size - at)
            return false;
        if (marker == 0xc0) {
            if (length != 17 || bytes[at + 2] != 8 || bytes[at + 7] != 3)
                return false;
            const int h = (bytes[at + 3] << 8) | bytes[at + 4];  // 原始高度，先限幅再允许解码。
            const int w = (bytes[at + 5] << 8) | bytes[at + 6];  // 原始宽度，避免输出尺寸乘法溢出。
            const auto sampling = bytes[at + 9];  // 仅采用已做色彩及边界验证的Y采样排列。
            if (w <= 0 || h <= 0 || w > 240 || h > 240 || bytes[at + 8] != 1 ||
                bytes[at + 11] != 2 || bytes[at + 14] != 3 || bytes[at + 12] != 0x11 ||
                bytes[at + 15] != 0x11 ||
                (sampling != 0x11 && sampling != 0x21 && sampling != 0x22))
                return false;
            width = w;
            height = h;
            return true;
        }
        if (marker >= 0xc1 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 && marker != 0xcc)
            return false;
        at += length;
    }
    return false;
}

// WatcherRomJpegDimensions 的容器入口沿用同一检查，避免解码与相机直送的尺寸合同分叉。
inline bool WatcherRomJpegDimensions(const std::vector<uint8_t>& bytes, int& width, int& height) {
    return WatcherRomJpegDimensions(bytes.data(), bytes.size(), width, height);
}

// WatcherRomJpegDmaReady 核对PSRAM工作区路径的DMA预算，两个参数分别为最大连续块和总余量。
// 音频与相机4KB DMA区已预分配；ROM工作区在PSRAM，保留6KB连续块及16KB总量供网络并发。
// 需要内部工作区的普通JPEG回退另行保持原8KB分配后检查，不能使用本预算代签。
inline bool WatcherRomJpegDmaReady(size_t largest_dma, size_t free_dma) {
    return largest_dma >= 6 * 1024 && free_dma >= 16 * 1024;
}
