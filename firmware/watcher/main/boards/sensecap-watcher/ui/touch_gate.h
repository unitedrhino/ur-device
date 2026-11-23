// 触摸手势门禁：首次唤醒和通讯故障后，必须完整抬手才能产生下一次有效输入。
#pragma once

namespace watcher {
// TouchGate 不依赖 LVGL 或硬件，供真实采样回调与主机测试共用。
class TouchGate {
public:
    // Down 返回是否仍在一次接触内；通讯故障按未抬手处理以禁止误确认。
    bool Down() const { return down_; }
    // CancelUntilRelease 取消已经开始的接触，页切换后需完整抬手才能再次输入。
    void CancelUntilRelease() {
        if (down_)
            suppressed_ = true;
    }
    // Sample 返回本次是否可上报按下；valid 为读取成功，wake_only 只在新接触开始时生效。
    bool Sample(bool valid, bool pressed, bool wake_only) {
        if (!valid) {
            down_ = true;
            suppressed_ = true;
            return false;
        }
        if (!pressed) {
            down_ = false;
            suppressed_ = false;
            return false;
        }
        if (!down_)
            suppressed_ = wake_only;
        down_ = true;
        return !suppressed_;
    }

private:
    bool down_ = false;        // 上次有效按下或未解除的通信故障。
    bool suppressed_ = false;  // 当前整段接触只唤醒或已因通信错误作废。
};
}  // namespace watcher
