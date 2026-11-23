// 文件说明：桌面线程进度的定长只读原子快照；不持业务锁、不创建线程、不保存身份/报文。
// 每个计数槽只有注释指定的唯一写线程；HTTP线程只读，各字段不是事务快照，不能反推严格时序。
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <string_view>

namespace ur::device {
// ProgressCounter使用唯一写线程饱和计数，读取不等待写线程或业务锁。
class ProgressCounter {
public:
    // initial仅供可信构造及饱和边界测试，网络请求不能设置计数。
    explicit ProgressCounter(std::uint32_t initial = 0) : value_(initial) {}
    // increment到达上限后保留计数并标溢出，不环绕、不使用可能等待的CAS循环。
    void increment() noexcept {
        const auto value = value_.load(std::memory_order_relaxed);
        if (value == std::numeric_limits<std::uint32_t>::max())
            overflow_.store(1, std::memory_order_relaxed);
        else
            value_.store(value + 1, std::memory_order_relaxed);
    }
    // value返回当前固定数值；不与其它槽形成事务快照。
    std::uint32_t value() const noexcept { return value_.load(std::memory_order_relaxed); }
    // overflow表示此槽曾达到饱和后继续计数，不能当作业务队列溢出。
    bool overflow() const noexcept { return overflow_.load(std::memory_order_relaxed) != 0; }

private:
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
                  "progress diagnostics must never take hidden locks");
    std::atomic<std::uint32_t> value_;        // 唯一所属线程写，HTTP可并发只读。
    std::atomic<std::uint32_t> overflow_{0};  // 唯一所属线程设置，不复位历史。
};
// ProgressRoute仅比较既有下行Topic前缀；没有产品/设备标识的存储槽。
enum class ProgressRoute { Message, Signaling, Other };
// CallbackStage属于Mosquitto网络线程，HTTP读取不等待其获取主互斥锁。
enum class CallbackStage { Idle, Entered, Locked, Rejected, Enqueued, Exited };
// HostStage只描述真实宿主边界，RuntimePoll不能冒称已经定位核心内具体阻塞点。
enum class HostStage {
    Idle,
    BridgeInput,
    OutputGate,
    RuntimeStart,
    RuntimePoll,
    Receipts,
    ToolCommand,
    TextResult,
    Content,
    MessageDiagnostic,
    MediaCommands,
    PlaybackFrames,
    Sleep,
    Shutdown,
    Stopped
};
// RouteProgress的valid/enqueued只由Mosquitto网络线程写，taken只由宿主线程写。
struct RouteProgress {
    ProgressCounter valid;     // 原回调通过原长度/Topic/retained检查。
    ProgressCounter enqueued;  // 原Message实际进入生产有界队列，不等同于Runtime消费。
    ProgressCounter taken;     // 原Message实际从队列取出，不等同于正文解析或UI展示。
};
// ThreadProgress只存固定原子；由设备、桥共同持有，HTTP绝不借用Runtime或宿主栈对象。
struct ThreadProgress {
    std::array<RouteProgress, 3> routes;  // 固定Message/Signaling/Other，不记录自由Topic。
    ProgressCounter callbacks_entered, callbacks_locked, callbacks_exited, rejected,
        enqueue_overflow;  // Mosquitto网络线程唯一写，enqueue_overflow仅Message入队失败。
    ProgressCounter mqtt_poll_entered, mqtt_poll_exited, take_entered, take_exited;
    // 上述poll/take和下述宿主计数都由同一宿主线程唯一写。
    ProgressCounter loops_entered, loops_exited, runtime_entered, runtime_exited,
        diagnostic_attempts, diagnostic_sent, diagnostic_failed;
    std::atomic<std::uint32_t> callback_stage{0};  // Mosquitto网络线程唯一写。
    std::atomic<std::uint32_t> host_stage{0};      // 宿主线程唯一写。
    // route只分类已有前缀，不复制或保存传入Topic。
    static ProgressRoute route(std::string_view topic) noexcept {
        constexpr std::string_view message = "$thing/down/message/";
        constexpr std::string_view signaling = "$thing/down/signaling/";
        return topic.substr(0, message.size()) == message       ? ProgressRoute::Message
               : topic.substr(0, signaling.size()) == signaling ? ProgressRoute::Signaling
                                                                : ProgressRoute::Other;
    }
    // for_route仅接收本类route产生的固定枚举，不接受网络数值索引。
    RouteProgress& for_route(ProgressRoute route) noexcept {
        return routes[static_cast<std::size_t>(route)];
    }
    // host记录宿主实际到达边界，不代表该边界内部工作已经完成。
    void host(HostStage stage) noexcept {
        host_stage.store(static_cast<std::uint32_t>(stage), std::memory_order_relaxed);
    }
    // callback记录网络线程实际阶段，HTTP读者不影响回调推进。
    void callback(CallbackStage stage) noexcept {
        callback_stage.store(static_cast<std::uint32_t>(stage), std::memory_order_relaxed);
    }
};
// make_thread_progress仅精确开关1装配；默认/非法值不分配诊断对象或改变业务能力。
inline std::shared_ptr<ThreadProgress> make_thread_progress(std::string_view enabled) {
    return enabled == "1" ? std::make_shared<ThreadProgress>() : nullptr;
}
// ProgressExit只在原作用域结束记一次退出；不改变返回值、业务状态或异常处理。
class ProgressExit {
public:
    // stage可选地随原作用域结束记录完成阶段；构造在锁之前，析构在原锁释放之后。
    explicit ProgressExit(ProgressCounter* counter, std::atomic<std::uint32_t>* stage = nullptr,
                          std::uint32_t finished = 0) noexcept
        : counter_(counter), stage_(stage), finished_(finished) {}
    ~ProgressExit() {
        if (stage_)
            stage_->store(finished_, std::memory_order_relaxed);
        if (counter_)
            counter_->increment();
    }
    ProgressExit(const ProgressExit&) = delete;
    ProgressExit& operator=(const ProgressExit&) = delete;

private:
    ProgressCounter* counter_;           // 所属对象在当前原调用期间持续持有shared_tracker。
    std::atomic<std::uint32_t>* stage_;  // 可选唯一写者阶段，无业务状态或外部对象捕获。
    std::uint32_t finished_;             // 编译期固定枚举值，不接受报文输入。
};
}  // namespace ur::device
