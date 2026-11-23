// 文件说明：坦克非触屏通话操作映射；信令状态仍由通话状态机控制。
#pragma once

#include <cstdint>
#include <mutex>

namespace sparkbot {

// CallState 是供板级输入使用的通话状态快照。
enum class CallState { Hidden, Ringing, Connecting, Active, Ended };
// TouchSide 表示左右两侧电容感应位置。
enum class TouchSide { Left, Right };
// CallAction 是板级输入向通话状态机请求的动作。
enum class CallAction { None, Accept, Reject, HangUp };

// ResolveCallAction 仅在允许的通话阶段映射单次触碰，其他状态不消费原有功能。
constexpr CallAction ResolveCallAction(CallState state, TouchSide side) {
    if (state == CallState::Ringing) {
        return side == TouchSide::Left ? CallAction::Accept : CallAction::Reject;
    }
    if ((state == CallState::Connecting || state == CallState::Active) &&
        side == TouchSide::Right) {
        return CallAction::HangUp;
    }
    return CallAction::None;
}

// CallStateEpoch 将通话状态、信令代次与本地状态代次作为同一个快照发布。
template <typename State>
class CallStateEpoch {
public:
    // Value 是状态通知对应的完整快照，generation 用于绑定通话操作。
    struct Value { State state; std::uint32_t epoch; std::uint32_t generation; };
    // 初始状态必须是调用方定义的有效状态。
    explicit CallStateEpoch(State initial) : value_{initial, 0, 0} {}
    // Store 发布新状态，任何发布都会使先前操作快照失效。
    Value Store(State state, std::uint32_t generation = 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        value_ = {state, value_.epoch + 1, generation};
        return value_;
    }
    // Snapshot 返回包含状态及代次的不可分割快照。
    Value Snapshot() const { std::lock_guard<std::mutex> lock(mutex_); return value_; }
    // StateOf 从快照读取状态，不依赖其当前有效性。
    static State StateOf(Value snapshot) { return snapshot.state; }
    // IsCurrent 校验排队通知仍属于最近一次状态发布。
    bool IsCurrent(Value snapshot) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return value_.epoch == snapshot.epoch;
    }
    // AllowsChat 仅当快照仍有效且状态为空闲时允许执行排队的对话操作。
    bool AllowsChat(Value snapshot, State hidden) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return snapshot.state == hidden && value_.epoch == snapshot.epoch;
    }

private:
    mutable std::mutex mutex_;  // 仅保护短小快照，不跨回调或硬件调用持有。
    Value value_;
};

// CallTouchGate 阻止跨状态遗留的按压和同时触碰引发误接听或误挂断。
class CallTouchGate {
public:
    // Update 输入当前状态、电极位图及已去抖的上升沿，返回本次允许的动作。
    CallAction Update(CallState state, unsigned active, bool left, bool right,
                      std::uint32_t generation = 0) {
        if (state != state_ || generation != generation_) {
            blocked_ = active;
            state_ = state;
            generation_ = generation;
        }
        blocked_ &= active;
        left = left && !(blocked_ & (1U << 2));
        right = right && !(blocked_ & (1U << 3));
        if ((active & ((1U << 2) | (1U << 3))) == ((1U << 2) | (1U << 3)) || left == right)
            return CallAction::None;
        return ResolveCallAction(state, left ? TouchSide::Left : TouchSide::Right);
    }
    // AllowChat 仅在跨状态遗留触点全部释放后允许恢复原有对话切换。
    bool AllowChat() const { return blocked_ == 0; }

private:
    CallState state_ = CallState::Hidden;  // 前一轮状态，用于识别状态切换。
    std::uint32_t generation_ = 0;        // 即使状态名称相同，新来电也须先释放旧触点。
    unsigned blocked_ = 0;                // 状态切换前已按住、尚未释放的电极。
};

}  // namespace sparkbot
