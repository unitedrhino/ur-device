// 截图合成纯逻辑：把 LVGL 的 BGRA 顶层像素叠加到原生 RGB565 页面，保留行步长。
#pragma once
#include <cstddef>
#include <cstdint>

namespace display_snapshot {
// Composite 校验缓冲边界后按直通 alpha 合成；false 表示格式/大小错误且不修改目标。
inline bool Composite(uint8_t* rgb, size_t rgb_size, size_t rgb_stride,
                      const uint8_t* bgra, size_t bgra_size, size_t bgra_stride,
                      size_t width, size_t height) {
    if (!rgb || !bgra || !width || !height || width > SIZE_MAX / 4 ||
        rgb_stride < width * 2 || bgra_stride < width * 4 ||
        height > rgb_size / rgb_stride || height > bgra_size / bgra_stride) return false;
    for (size_t y = 0; y < height; ++y) {
        for (size_t x = 0; x < width; ++x) {
            auto* dst = rgb + y * rgb_stride + x * 2;
            const auto* src = bgra + y * bgra_stride + x * 4;
            const unsigned alpha = src[3];
            if (!alpha) continue;
            const unsigned pixel = dst[0] | (unsigned(dst[1]) << 8);
            // 先扩展到八位通道再混合，避免透明边缘产生明显色带。
            const unsigned red5 = (pixel >> 11) & 31, green6 = (pixel >> 5) & 63, blue5 = pixel & 31;
            const unsigned red = (src[2] * alpha + ((red5 << 3) | (red5 >> 2)) * (255 - alpha) + 127) / 255;
            const unsigned green = (src[1] * alpha + ((green6 << 2) | (green6 >> 4)) * (255 - alpha) + 127) / 255;
            const unsigned blue = (src[0] * alpha + ((blue5 << 3) | (blue5 >> 2)) * (255 - alpha) + 127) / 255;
            const unsigned result = ((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3);
            dst[0] = result & 255;
            dst[1] = result >> 8;
        }
    }
    return true;
}
}  // namespace display_snapshot
