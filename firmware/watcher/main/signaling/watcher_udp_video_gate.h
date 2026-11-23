// 文件说明：限制 Watcher 人际通话中的 UDP 视频接收节拍，优先保留音频处理时间。
#pragma once

#include <cstdint>
#include <vector>

// WatcherUdpVideoGate 只在已完成一帧后短暂跳过视频包，不修改音频或协议状态。
class WatcherUdpVideoGate {
public:
    // IsVideoPacket 识别 UDP 视频数据报；短包及其他类型仍交由原协议处理。
    static bool IsVideoPacket(const std::vector<std::uint8_t>& packet) {
        return packet.size() > 16 && packet[0] == 0x02 && packet[1] == 0x00;
    }

    // ShouldSkip 判断数据报是否为限速期内的视频；now 为单调毫秒，其他数据交由协议校验。
    bool ShouldSkip(const std::vector<std::uint8_t>& packet, std::uint64_t now) const {
        return started_ && now >= last_frame_ms_ && now - last_frame_ms_ < kIntervalMs &&
               IsVideoPacket(packet);
    }

    // OnFrame 仅在合法完整 JPEG 组帧后记录时间，缺片或伪造包不能推进节拍。
    void OnFrame(std::uint64_t now) { last_frame_ms_ = now; started_ = true; }

    // Reset 在新通话或重新安装媒体参数时清除前一会话的限速状态。
    void Reset() { last_frame_ms_ = 0; started_ = false; }

private:
    static constexpr std::uint64_t kIntervalMs = 166;  // 本地处理不超过约六个完整帧每秒。
    std::uint64_t last_frame_ms_ = 0;  // 最近一次成功组帧的单调时间。
    bool started_ = false;  // 首帧不等待，且区分零时刻与未开始。
};
