// 可移植异步动作执行器：事件线程只提交/轮询，硬件耗时工作由驱动独立调度。
#pragma once
#include "ur/device/action_wire.hpp"
#include "ur/device/fixed_queue.hpp"
#include <optional>
#include <vector>

namespace ur::device {
// ActionCompletion 来自驱动真实完成，Token隔离取消后的迟到结果。
struct ActionCompletion {
    std::string token;        // 原提交Token。
    int code = 500;           // 200为已执行成功，其他值为设备错误。
    std::string data = "{}";  // 实际输出；不能回显未经执行的输入。
};
// ActionDriver 所有方法必须非阻塞，不持有传入对象引用；生命周期长于执行器。
class ActionDriver {
public:
    virtual ~ActionDriver() = default;
    // begin 只接纳工作；false表示没有开始，true不表示执行成功。
    virtual bool begin(const ActionRequest& request) = 0;
    // take_completion 每次最多交付一个完成事实；不能从驱动线程回调执行器。
    virtual std::optional<ActionCompletion> take_completion() = 0;
    // cancel
    // 为true表示已阻止后续副作用；false表示结果未知，执行器停止接纳新动作。
    virtual bool cancel(std::string_view token) = 0;
};
// ActionAdmission 区分接纳、执行中重投和已完成重放；未接纳不触发驱动副作用。
enum class ActionAdmission { Accepted, Pending, Replay, Conflict, Busy, Full, Invalid, Closed };
// ActionSubmission 仅Replay携带原始回执；Accepted/Pending绝不返回成功回执。
struct ActionSubmission {
    ActionAdmission state = ActionAdmission::Invalid;  // 默认拒绝。
    std::string reply;                                 // 重放原JSON，不重新序列化结果。
};
// ActionPersistence 是非阻塞账本的固化屏障，Stored才允许驱动执行或原终态重放。
enum class ActionPersistence { Pending, Stored, Failed };
// ActionHistoryLedger
// 可选持久化账本探针：按clienttool账本模式跨进程固化原受理与原终态，
// 供重启后拒绝同Token重复执行。实现必须非阻塞且键为原Token；探针缺失时保持纯内存语义。
class ActionHistoryLedger {
public:
    virtual ~ActionHistoryLedger() = default;
    // recorded
    // 原Token是否已登记受理；true且无终态表示重启窗口内在途，结果未知不得重执行。
    virtual bool recorded(std::string_view token) const = 0;
    // find_request
    // 返回不可变原受理请求（含规范化参数）；已登记却无法读取时必须保守停机。
    virtual std::optional<ActionRequest> find_request(std::string_view token) const = 0;
    // find_reply 返回原Token已固化回执原文；nullopt表示账本尚无终态。
    virtual std::optional<std::string> find_reply(std::string_view token) const = 0;
    // record
    // 登记或排队原受理；false表示不可写，true仍须persistence确认固化后才能执行。
    virtual bool record(const ActionRequest& request) = 0;
    // record_reply 固化原终态；false同record，内存终态保留但停机防重放缺口。
    virtual bool record_reply(std::string_view token, std::string_view reply) = 0;
    // persistence 非阻塞读取受理或终态的固化事实；原同步账本保持Stored默认行为。
    virtual ActionPersistence persistence(std::string_view, bool) const {
        return ActionPersistence::Stored;
    }
};
// ActionEngine 保留当前实例的幂等记录；断网不能重建实例清空记录。
// 容量耗尽时无账本拒绝新动作，有账本则驱逐最旧已完成记录（其终态仍可从账本重放）；
// 跨进程/断电幂等通过可选ActionHistoryLedger持久化探针实现。
class ActionEngine {
public:
    explicit ActionEngine(ActionDriver& driver, std::uint64_t timeout_ms = 5000,
                          ActionHistoryLedger* ledger = nullptr);
    ~ActionEngine();
    ActionEngine(const ActionEngine&) = delete;
    ActionEngine& operator=(const ActionEngine&) = delete;
    // submit 只解析和登记，不在MQTT收消息的路径启动硬件。
    ActionSubmission submit(std::string_view json, std::uint64_t now);
    // poll 每次至多处理四条驱动结果，避免迟到洪泛饿死语音。
    void poll(std::uint64_t now);
    // take_reply 取得完成回执；发布失败可用相同Token重投获取原文，不能重复执行。
    std::optional<std::string> take_reply();
    // close 幂等取消活动动作，保留历史仅供重放，不能重新接纳新动作。
    void close();
    // disconnected
    // 取消在途任务但保留幂等历史；确认取消后可在同实例重连接纳新动作。
    void disconnected();
    bool active() const { return active_.has_value(); }
    bool closed() const { return closed_; }

private:
    // Record 在登记到完成期间保持固定请求，结果一旦生成不能被迟到驱动输出覆盖。
    struct Record {
        ActionRequest request;  // 已规范化输入。
        std::string reply;      // 非空表示已终结，包括失败/超时。
        bool persistence_failed = false;  // 终态未能排队固化时禁止对外发布或重放。
    };
    // PendingReply 保留原token关联，固化屏障未越过时不提前消费回执。
    struct PendingReply {
        std::string token;  // 账本原token，不依赖可驱逐历史的地址。
        std::string body;   // 已固定的真实驱动结果。
    };
    // observe 拒绝单调时钟回退，防止超时无限延长。
    bool observe(std::uint64_t now);
    // finish 固定本动作结果并入回执队列，非法驱动数据转稳定失败。
    void finish(int code, const char* message, std::string data);
    // abort 取消在途驱动；取消不能确认时关闭执行器，禁止下一个动作接管同一资源。
    void abort(const char* reason);
    // evictOldestRetired 有账本兜底时驱逐最旧已完成记录腾出needed字节；
    // 未决记录（无回执）不可驱逐，腾不出空间返回false。
    bool evictOldestRetired(std::size_t needed);
    ActionDriver& driver_;                    // 平台执行端口。
    ActionHistoryLedger* ledger_ = nullptr;   // 可选持久账本探针；空表示纯内存语义。
    std::vector<Record> history_;             // 最多32项；有账本时满员驱逐最旧已完成项。
    FixedQueue<PendingReply, 4> replies_;     // 有界结果背压，等待终态固化。
    std::optional<std::size_t> active_;       // 当前唯一已接纳动作。
    std::optional<std::uint64_t> last_time_;  // 单调时钟前值。
    std::uint64_t timeout_ms_;                // 合法范围1至30000毫秒。
    std::uint64_t admitted_at_ = 0;           // 排队也计入超时，不只计算驱动执行时间。
    std::size_t retained_bytes_ = 0;          // 历史请求和回执正文最多256KiB，不计容器开销。
    bool started_ = false;                    // 只有poll能调用驱动begin一次。
    bool closed_ = false;                     // 显式关闭、时钟失效或无法确认取消。
};
}  // namespace ur::device
