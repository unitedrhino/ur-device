// 有界请求关联与超时重试：单事件线程驱动，重试保持原始报文及 token。
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ur::device {
// PublishPort 接受发布不代表远端已执行；不允许同步重入 Requests。
class PublishPort {
public:
    virtual ~PublishPort() = default;
    // publish 必须使用 QoS1、retained=false；false 仅表示本地未能接受发送。
    virtual bool publish(std::string_view topic, std::string_view body) = 0;
};
// RequestState 查询本地关联结果；TimedOut 表示执行结果未知，不等同失败未执行。
enum class RequestState { Missing, Pending, Succeeded, Failed, TimedOut, Cancelled };
// SubmitResult 区分幂等重提、token 冲突及资源上限，禁止自动换 token 重试。
enum class SubmitResult { Accepted, Duplicate, Conflict, Full, Invalid };
// Request 一个已完成语义校验的请求；正文必须由同一规范化编码器生成。
struct Request {
    std::string token;    // 原请求唯一标识。
    std::string session;  // 可为空，仅适用于未建会话请求。
    std::string method;   // 原请求方法，requestAck.data.requestMethod 必须匹配。
    std::string topic;    // 已鉴权设备上行 Topic。
    std::string body;     // 重试逐字节使用原正文，不重新生成随机字段。
};
// Acknowledgement 将线上应答与本地计时分开，防止状态码和时间参数错位。
struct Acknowledgement {
    std::string_view token;    // 原请求标识。
    std::string_view session;  // 原会话，未分配时为空。
    std::string_view method;   // 原请求方法，不是 requestAck 字面值。
    int code;                  // 云端实际应答码。
    std::string_view topic;    // 对应的原上行 Topic；接收层须先校验下行身份再映射。
};
// RequestKey 按完整请求作用域定位，不把不同 Topic 或 method 的同 token 合并。
struct RequestKey {
    std::string_view topic;   // 原上行 Topic，含已鉴权设备身份。
    std::string_view method;  // 原请求方法。
    std::string_view token;   // 原关联键。
};
// Requests 分别限制在途和近期终态；资源满时背压，不提前淘汰幂等保护。
class Requests {
public:
    // now 参数统一为同一单调时钟的毫秒数，不接收墙钟时间。
    explicit Requests(PublishPort& port) : port_(port) {}
    Requests(const Requests&) = delete;
    Requests& operator=(const Requests&) = delete;
    // submit 首次尝试发送；相同 token/正文不延长等待期限。
    SubmitResult submit(Request request, std::uint64_t now);
    // tick 在 10/20 秒重试，常规 30 秒转结果未知（延迟预算见 extend_pending）；
    // 会话内记录保留到关闭后十分钟。
    void tick(std::uint64_t now);
    // extend_pending 为媒体恢复期放宽请求应答预算：恢复窗口内的链路抖动会让远端应答晚到
    // （真机定向洪泛实测下行积压约 30 秒），普通 30 秒预算会把仍可能成功的恢复请求误判为
    // TimedOut，并让迟到 ACK 被拒绝。until 为统一单调毫秒截止，只延长不缩短；
    // 在途请求与之后提交的请求都按延长后的截止判定。
    void extend_pending(std::uint64_t until);
    // clear_pending_extension 恢复常规 30 秒预算；由恢复完成或会话释放调用。
    void clear_pending_extension();
    // republish_pending
    // 仅按完整key与原session重发仍在途原字节；不修改首次时间、通用次数或终态，调用者负责专属有界调度。
    bool republish_pending(RequestKey key, std::string_view session, std::uint64_t now);
    // acknowledge 只接受 Topic/token/session/method 全匹配的在途结果；返回是否接受。
    bool acknowledge(Acknowledgement ack, std::uint64_t now);
    // cancel_session 标记本地会话已关闭并取消等待；不声称撤销远端执行。
    void cancel_session(std::string_view session, std::uint64_t now);
    // suspend_session 取消旧连接等待但保留业务会话与幂等事实，恢复后只能提交新输入。
    void suspend_session(std::string_view session, std::uint64_t now);
    // cancel_request 仅退休精确原等待并保留幂等事实，不关闭会话或声称撤销远端执行。
    bool cancel_request(RequestKey key, std::uint64_t now);
    // state 查询保留记录；不打印含凭据正文，不推断已超时操作的远端结果。
    RequestState state(RequestKey key) const;

private:
    // Entry 记录创建和终态时间，不随重试刷新初始期限；应答截止可被恢复期延长。
    struct Entry {
        Request request;                             // 关联字段及原始正文。
        RequestState state = RequestState::Missing;  // 默认空槽。
        std::uint64_t started = 0;                   // 首次发送的单调时间。
        // Pending 期间保存应答截止（可被恢复期延长），进入终态后保存终态起始时间用于保留窗口；
        // 两者在时间上互斥，合并存储不增加嵌入式对象的固定尺寸。
        union {
            std::uint64_t finished = 0;  // 终态起始时间。
            std::uint64_t deadline;      // 本条请求的应答截止；常规为 started+30 秒。
        };
        unsigned retries = 0;                        // 已尝试重发次数，最多两次。
        bool session_closed = false;                 // 关闭后才开始计算会话记录保留期限。
    };
    PublishPort& port_;              // 借用发布适配器。
    std::vector<Entry> entries_;     // 按需分配，最多 512 条；避免在嵌入式栈上放大数组。
    std::uint64_t extended_until_ = 0;  // 恢复期统一延长截止；0 表示常规预算。
};
}  // namespace ur::device
