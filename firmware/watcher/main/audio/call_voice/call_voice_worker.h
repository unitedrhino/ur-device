// 文件说明：测试固件的有界语音接听工作任务；麦克风生产者从不等待模型推理。
#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <model_path.h>

// CallVoiceWorker 异步管理一次来电的模型和识别结果；不直接操作通话或音频开关。
class CallVoiceWorker {
public:
    // Outcome 表示模型已释放后的结果；只有 Recognized 可尝试接听。
    enum class Outcome { Recognized, Cancelled, Unavailable };
    // Completion 在识别任务中调用，不得销毁工作对象或直接操作应用状态。
    using Completion = std::function<void(std::uint32_t, Outcome)>;

    CallVoiceWorker() = default;
    // 销毁前调用方须先撤销音频生产者，避免生产者持有悬空引用。
    ~CallVoiceWorker();
    CallVoiceWorker(const CallVoiceWorker&) = delete;
    CallVoiceWorker& operator=(const CallVoiceWorker&) = delete;

    // Initialize 在控制任务创建工作线程；失败时语音识别不可用。
    bool Initialize();
    // Begin 借用模型列表并启动指定来电代次，列表须保持有效直至完成回调。
    bool Begin(srmodel_list_t* models, std::uint32_t generation, Completion completion);
    // Push 接收最多 1024 个单声道采样；不等待锁、队列或模型，丢帧使本次识别失效。
    bool Push(std::uint32_t generation, const std::int16_t* samples, std::size_t count);
    // RequestStop 异步停止；完成回调仅在模型销毁、生产者退出后执行。
    void RequestStop();
    // Busy 为 true 时模型可能仍持有内存，不得启动与其竞争内存的通话媒体。
    bool Busy() const { return busy_.load(); }
    // Ready 表示当前代次已装载模型并可以接收音频。
    bool Ready() const { return ready_.load(); }

private:
    static constexpr std::size_t kFrameSamples = 1024; // 覆盖常见 AFE 帧；不在音频线程分配。
    static constexpr std::size_t kFrameCapacity = 8;   // 识别跟不上时整体失效，不拼接缺失语音。
    struct Frame { std::array<std::int16_t, kFrameSamples> data{}; std::size_t count = 0; };
    void Run();

    std::mutex mutex_;
    std::condition_variable cv_;
    std::array<Frame, kFrameCapacity> frames_{};
    std::size_t head_ = 0, count_ = 0;
    srmodel_list_t* models_ = nullptr;    // 借用至本轮完成，不跨完成回调持有。
    Completion completion_;
    bool pending_ = false, cancel_ = false, shutdown_ = false;
    std::atomic<bool> busy_{false}, ready_{false}, lost_{false};
    std::atomic<std::uint32_t> generation_{0}, inflight_{0};
    TaskHandle_t task_ = nullptr;
    SemaphoreHandle_t stopped_ = nullptr;
};
