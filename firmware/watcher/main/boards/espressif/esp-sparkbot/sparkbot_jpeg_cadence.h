// 文件说明：坦克通话下行 JPEG 解码节拍，给实时双向音频保留处理时间。
#pragma once

#include <cstdint>

namespace sparkbot {

// JpegCadence 限制单通话的画面解码频率；时间回退时允许立即重新开始。
class JpegCadence {
public:
    // Ready 判断当前单调毫秒是否到达下一次解码时间；首帧不等待。
    bool Ready(std::int64_t now) const {
        return !started_ || now < last_ms_ || now - last_ms_ >= kIntervalMs;
    }
    // Mark 记录即将解码的时间，不因传入帧被覆盖而提前推进节拍。
    void Mark(std::int64_t now) { last_ms_ = now; started_ = true; }

private:
    static constexpr std::int64_t kIntervalMs = 500;  // 坦克对照测试每秒两次，优先保障音频。
    std::int64_t last_ms_ = 0;                       // 最近一次解码启动的单调毫秒。
    bool started_ = false;                           // 区分零时刻与尚未解码。
};

}  // namespace sparkbot
