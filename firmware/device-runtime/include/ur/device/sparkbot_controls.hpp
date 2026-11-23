// 文件说明：SparkBot 坦克的可中断运动时序、灯效映射与电容按键去抖；不依赖
// ESP-IDF。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace ur::device {

// SparkBotLightCommand 将灯效1–6映射到底盘3–8；0兼容映射为休眠8，并非彻底熄灭，非法输入返回零。
inline char SparkBotLightCommand(int mode) {
    if (mode == 0)
        return '8';
    return mode >= 1 && mode <= 6 ? static_cast<char>('0' + mode + 2) : '\0';
}

// SparkBotMotion 在主控上按时间推进舞步，使停止和断连可以立即取消后续动作。
// 调用方负责串行化访问，并每 100ms 以内发送一次 Current
// 返回的速度，维持底盘看门狗。
class SparkBotMotion {
public:
    // Frame 表示底盘归一化速度向量；x 为转向，y 为前后方向。
    struct Frame {
        float x = 0;
        float y = 0;
    };

    // Start 开始限时运动；now_ms 为单调时钟，duration_ms 为持续时长。
    void Start(float x, float y, int64_t now_ms, uint32_t duration_ms) {
        frame_ = {x, y};
        deadline_ms_ = now_ms + duration_ms;
        dancing_ = false;
    }

    // Dance 开始与官方底盘相同的舞步，不发送无法中断的 d1 指令。
    void Dance(int64_t now_ms) {
        dancing_ = true;
        step_ = 0;
        frame_ = steps_[0].frame;
        deadline_ms_ = now_ms + steps_[0].duration_ms;
    }

    // Stop 清除当前运动和未执行舞步；后续 Current 始终返回零速，直到下一次启动。
    void Stop() {
        frame_ = {};
        dancing_ = false;
        deadline_ms_ = 0;
    }

    // Current 根据单调时间推进动作，并按业务 Speed 百分比缩放输出。
    Frame Current(int64_t now_ms, int speed) {
        while (deadline_ms_ > 0 && now_ms >= deadline_ms_) {
            if (!dancing_ || ++step_ >= steps_.size()) {
                Stop();
                break;
            }
            frame_ = steps_[step_].frame;
            deadline_ms_ += steps_[step_].duration_ms;
        }
        const float scale = (speed < 0 ? 0 : (speed > 100 ? 100 : speed)) / 100.0f;
        return {frame_.x * scale, frame_.y * scale};
    }

private:
    // Step 对应官方左右履带舞步换算后的速度与持续时间，停止段同样占用时间。
    struct Step {
        Frame frame;
        uint32_t duration_ms;
    };
    static constexpr std::array<Step, 18> steps_ = {{
        {{0, 1}, 1000},
        {{0.8f, 0}, 800},
        {{0, -0.6f}, 1200},
        {{-0.8f, 0}, 800},
        {{0, 0.8f}, 1000},
        {{-0.8f, 0}, 800},
        {{0, -1}, 1000},
        {{0.8f, 0}, 800},
        {{0, 0}, 500},
        {{-0.8f, 0}, 800},
        {{0, 1}, 1000},
        {{0.8f, 0}, 800},
        {{0, -0.8f}, 1000},
        {{0.8f, 0}, 800},
        {{0, 0.6f}, 1200},
        {{-0.8f, 0}, 800},
        {{0, -1}, 1000},
        {{0, 0}, 500},
    }};
    Frame frame_{};            // 当前未缩放速度。
    int64_t deadline_ms_ = 0;  // 当前动作结束时间；零表示停止。
    std::size_t step_ = 0;     // 当前舞步索引。
    bool dancing_ = false;     // 是否继续自动推进舞步。
};

// SparkBotTouchDebounce
// 将稳定按下折叠为一次事件；持续按住或短暂抖动不会重复切换会话。
class SparkBotTouchDebounce {
public:
    // Update 接收三个触点的合并状态与单调微秒时钟；返回 true
    // 时由调用方投递主任务。
    bool Update(bool active, int64_t now_us) {
        if (active != candidate_) {
            candidate_ = active;
            changed_us_ = now_us;
        }
        if (now_us - changed_us_ < 60000 || stable_ == candidate_)
            return false;
        stable_ = candidate_;
        if (!stable_ || now_us - last_press_us_ < 750000)
            return false;
        last_press_us_ = now_us;
        return true;
    }

private:
    bool candidate_ = false;           // 最近观测值。
    bool stable_ = false;              // 已确认的稳定值。
    int64_t changed_us_ = 0;           // 最近电平变化时刻。
    int64_t last_press_us_ = -750000;  // 最近有效按下，负初值允许启动后的首次操作。
};
}  // namespace ur::device
