// 文件说明：视频上行所用的 RGB565 中心裁剪与缩放，不申请缓冲区。
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace stream_video {

// ScaleRgb565ToSquare 将小端 RGB565 按中心正方形最近邻缩放，尺寸或缓冲无效时返回 false。
inline bool ScaleRgb565ToSquare(const std::uint8_t* source, std::size_t source_size,
                                int width, int height, std::uint8_t* output,
                                std::size_t output_size, int target) {
    if (!source || !output || width <= 0 || height <= 0 || target <= 0) return false;
    const auto w = static_cast<std::size_t>(width);
    const auto h = static_cast<std::size_t>(height);
    const auto n = static_cast<std::size_t>(target);
    const auto limit = std::numeric_limits<std::size_t>::max();
    if (w > limit / h / 2 || n > limit / n / 2 || source_size < w * h * 2 ||
        output_size < n * n * 2) return false;
    const auto side = w < h ? w : h;
    const auto left = (w - side) / 2;
    const auto top = (h - side) / 2;
    for (std::size_t y = 0; y < n; ++y) {
        const auto src_y = top + y * side / n;
        for (std::size_t x = 0; x < n; ++x) {
            const auto src_x = left + x * side / n;
            const auto src = (src_y * w + src_x) * 2;
            const auto dst = (y * n + x) * 2;
            output[dst] = source[src];
            output[dst + 1] = source[src + 1];
        }
    }
    return true;
}

}  // namespace stream_video
