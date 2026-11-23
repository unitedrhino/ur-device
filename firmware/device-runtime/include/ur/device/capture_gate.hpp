// 文件说明：单设备采集组的有界忙线预约；不执行全局输出焦点、抢占或音量控制。
#pragma once
#include <array>
#include <cstdint>
#include <limits>
#include <optional>

namespace ur::device {
// CaptureGate 由唯一宿主事件线程驱动，未证明可共享的mic/speaker独占，相机按真实发布能力计数。
class CaptureGate {
public:
    // publishers 为同一物理相机的真实发布上限1至4，非法配置只能拒绝。
    explicit CaptureGate(unsigned publishers) : publishers_(publishers) {}
    // available 只核验整组可用性；不代表已预约，Speaker必须属于原Mic组。
    bool available(bool mic, bool speaker, bool camera) const {
        if (publishers_ < 1 || publishers_ > 4 || (!mic && speaker) || (!mic && !camera) ||
            serial_ == std::numeric_limits<std::uint64_t>::max())
            return false;
        unsigned cameras = 0;
        bool empty = false;
        for (const auto& slot : slots_) {
            if (!slot.id) {
                empty = true;
                continue;
            }
            if ((mic && slot.mic) || (speaker && slot.speaker))
                return false;
            cameras += slot.camera ? 1U : 0U;
        }
        return empty && (!camera || cameras < publishers_);
    }
    // acquire 原子预约整组资源，冲突时不留下部分租约或打开实际媒体。
    std::optional<std::uint64_t> acquire(bool mic, bool speaker, bool camera) {
        if (!available(mic, speaker, camera))
            return {};
        for (auto& slot : slots_)
            if (!slot.id) {
                slot = {++serial_, mic, speaker, camera};
                return slot.id;
            }
        return {};
    }
    // release 只收录实际Stopped回执或未开始预约取消；迟到旧租约不能释放新使用者。
    bool release(std::uint64_t lease) {
        if (!lease)
            return false;
        for (auto& slot : slots_)
            if (slot.id == lease) {
                slot = {};
                return true;
            }
        return false;
    }

private:
    // Slot 不保存驱动和业务权限，仅保存原物理资源组及独立租约身份。
    struct Slot {
        std::uint64_t id = 0;  // 零为空，非零租约在本实例内不重用。
        bool mic = false;      // 麦克风独占，不推断不同getUserMedia请求可共享。
        bool speaker = false;  // 扬声器独占，不替代会议MAC权限。
        bool camera = false;   // 消耗一份当前相机发布预算。
    };
    unsigned publishers_;          // 宿主固定实际能力，不从邀请扩大。
    std::array<Slot, 5> slots_{};  // 四路视频和一独立对讲，有界常驻，无动态分配。
    std::uint64_t serial_ = 0;     // 单调本地票据，溢出拒绝而不回绕。
};
}  // namespace ur::device
