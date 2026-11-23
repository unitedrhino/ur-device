// 文件说明：审核客户端工具的独立执行边界；展示事件、模型参数与端能力声明均不能自行授予执行权限。
#pragma once
#include <map>
#include <optional>
#include "ur/device/voice_wire.hpp"

namespace ur::device {
// ClientToolCommand 保存平台授权的原调用；令牌仅用于原结果提交，不进入日志。
struct ClientToolCommand {
    Envelope envelope;             // 原messageSeq及已验证会话。
    std::string response, call;    // 原回复与调用，禁止拿当前回复补齐。
    std::string name, theme;       // 唯一审核工具及light/dark闭合参数。
    std::string execution_token;   // 原端定向执行令牌。
    std::uint64_t expires_at = 0;  // 原Unix毫秒截止，重投不续期。
};
// decode_client_tool_execute
// 只解析独立Execute，不将toolCallStart/Result展示对象当动作。
WireResult decode_client_tool_execute(WireRoute route, std::string_view body,
                                      ClientToolCommand& out);
// ClientToolScope
// 来自Runtime已接纳的当前原回复，不接受Execute报文自行创建scope。
struct ClientToolScope {
    std::string session, response;  // 当前已建立会话与尚未终结的原回复。
    bool established = false;       // 真实建立事实，不是sessionCreated或目录声明。
};
// ClientToolFact
// 是驱动对原generation的实际观察；缺少事实时保持等待，不宣称成功。
struct ClientToolFact {
    std::uint64_t generation = 0;  // 原本地执行票据，不能确认后来的调用。
    std::string status, theme;     // succeeded/failed/unknown，成功必须为原目标主题。
};
// ClientToolDisplayPort
// 仅固定本地主题副作用，不提供脚本、网络或物模型action执行器。
class ClientToolDisplayPort {
public:
    virtual ~ClientToolDisplayPort() = default;
    // available仅真实宿主预检后可声明能力；未知驱动默认关闭，不能凭非空对象自授权限。
    virtual bool available() const { return false; }
    // request_theme
    // 只表示真实驱动接纳；generation由唯一事件线程提供，接纳不等于已应用。
    virtual bool request_theme(std::string_view theme, std::uint64_t generation) = 0;
    // take_theme_fact
    // 只返回原generation实际末端观察；乱序、重复或迟到事实不能补授权。
    virtual std::optional<ClientToolFact> take_theme_fact(std::uint64_t generation) = 0;
    // retire_theme 退休原generation的待发副作用及票据，不回滚已经实际应用的主题。
    // 默认空实现仅兼容同步应用且没有延迟副作用的端口；异步/排队端口必须实现代次隔离。
    virtual void retire_theme(std::uint64_t /*generation*/) {}
};
// ClientToolDecision 区分格式/原scope拒绝与执行接纳；Accepted不代表副作用成功。
enum class ClientToolDecision { Accepted, Replay, Denied, Invalid, Busy };
// ClientToolSubmission 是原调用唯一结果；请求token稳定重放，未知不重发副作用。
struct ClientToolSubmission {
    Envelope envelope;         // toolCallSubmit的原session/resp/call/token及固定结果。
    std::string status;        // 原驱动明确结果，不从文字或入队推断。
    std::uint64_t generation;  // 原本地执行代次，仅用于测试及当前宿主关联，不进入线协议。
};
// ClientToolExecutor
// 在单设备事件线程运行，最多512墓碑；无需音频端口或后台线程。
class ClientToolExecutor {
public:
    // instance_id由可信宿主提供唯一进程标识，用于结果msgToken；不能使用凭据充当标识。
    ClientToolExecutor(ClientToolDisplayPort& display, std::string instance_id);
    // accept
    // 先核Runtime原scope、闭合schema和原wall期限，再请求一次实际驱动；两种时钟明确分层。
    ClientToolDecision accept(const ClientToolCommand& command, const ClientToolScope& scope,
                              std::uint64_t now, std::uint64_t wall_now);
    // poll
    // 非阻塞取得原实际事实；局部两秒或原期限更早时只提交unknown，迟到不能变success。
    void poll(std::uint64_t now);
    // take_submission
    // 消费一份规范结果，重复Execute可在原期限内重放同msgToken结果而不重执行。
    std::optional<ClientToolSubmission> take_submission();
    // cancel_response
    // 仅取消匹配原回复的等待，不回滚已经完成的UI副作用、不影响下一轮。
    void cancel_response(std::string_view session, std::string_view response);
    // close_session 撤销原会话的等待者；墓碑保留以拒绝旧调用复活。
    void close_session(std::string_view session);

private:
    // Call 固定原命令/代次/截止与终态，结果不能因重投或当前回复变化而重写。
    struct Call {
        ClientToolCommand command;                     // 原授权副本，禁止日志输出。
        std::uint64_t generation, deadline;            // 本地单调原预算，重复不延长。
        std::optional<ClientToolSubmission> terminal;  // 原唯一结果。
        bool emit = false;                             // 有一份待取结果，不扩无界事件队列。
    };
    // finish 将实际状态固定为唯一终态；成功仅允许原驱动实际主题与原参数一致。
    void finish(Call& call, std::string_view status);
    ClientToolDisplayPort& display_;                             // 固定宿主真实显示端口。
    std::string instance_;                                       // 稳定结果请求标识前缀。
    std::map<std::pair<std::string, std::string>, Call> calls_;  // 原会话+调用业务键。
    std::optional<std::uint64_t> clock_;  // 单调回退导致所有在途unknown，不能重建预算。
    std::uint64_t generation_ = 0;        // 本实例递增，不允许复用原驱动票据。
    bool valid_ = true;                   // 实例标识或时钟失效后禁止重新执行。
};
}  // namespace ur::device
