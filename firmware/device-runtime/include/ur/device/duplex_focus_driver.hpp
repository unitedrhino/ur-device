// 文件说明：唯一microphone+speaker完整组仲裁；两端实际票据收齐后才推进共享CAS。
#pragma once
#include "ur/device/focus_engine.hpp"
#include "ur/device/input_ownership_gate.hpp"
#include "ur/device/output_group_gate.hpp"

namespace ur::device {
// DuplexDriverFailureReason 仅记录实际失败分支；不含会话、票据、时刻或业务正文。
enum class DuplexDriverFailureReason {
    NoClock,
    ClockRollback,
    ClockOverflow,
    InvalidLatched,
    BootDeadline,
    OutputUnconfirmed,
    InputUnconfirmed,
    PendingDeadline,
    InputTicketOrder,
    OutputTicketOrder
};
// DuplexDriverFailure 是原首因的有界副本；path为0/1，-1表示无法关联固定路径。
struct DuplexDriverFailure {
    DuplexDriverFailureReason reason;
    int path = -1;
};
// duplex_driver_failure_name 将内部枚举映射为固定公开诊断字面量，未知枚举不输出自由内容。
std::string_view duplex_driver_failure_name(DuplexDriverFailureReason reason);
// DuplexFocusDriver
// 最多管理AI/人工两路径；本实例独占同一物理源及扬声器，不能再装旧输出仲裁器。
// 授权仍由FocusAuthority决定；本类不自动抢占、恢复、采集或把Peer就绪当移交成功。
class DuplexFocusDriver final : public FocusDriver {
public:
    // outputs必须为不同原路由的真实端口；boot_generation来自宿主启动配对，revision为实际水位。
    DuplexFocusDriver(InputOwnershipGatePort& input, std::vector<OutputGroupGatePort*> outputs,
                      std::uint64_t boot_generation, std::uint64_t revision);
    ~DuplexFocusDriver() override;  // 只尽力围栏原组，析构不证明实际释放。
    // initialize只请求输出false；独立输入idle探针与全部输出原确认收齐前没有baseline。
    bool initialize(std::uint64_t now);
    // bind_scope只接原已创建会话；重新请求两端false，scope_ready之前不能执行租约。
    // 固定路径只绑定一次，接纳不授予资源；部分接纳失败仍保留原身份以便精确关闭。
    bool bind_scope(std::size_t path, std::string session, std::string focus,
                    std::uint64_t route_generation, std::uint64_t now);
    // replace_released_focus仅供同端控制恢复；原正式release事实已消费及双false后换独立新focus。
    // 媒体路由/票据/全局修订不重置；调用方还须核新socket快照代次及原released回执交付。
    bool replace_released_focus(std::size_t path, std::string_view original_session,
                                std::string_view original_focus, std::string new_focus,
                                std::uint64_t now);
    // released_idle_observation仅原正式release事实已消费且当前全组实际false时给出当前CAS基线。
    std::optional<FocusIdleObservation> released_idle_observation(std::size_t path) const;
    bool scope_ready(std::size_t path) const;  // 原绑定两端false已实际确认，不等于取得租约。
    // stopped_scope_ready只读原租约退役后的两端false；可交正式release，但不造实际release或推进CAS。
    // 未取得租约、未知事实、在途操作及未消费回执均不能作为正式关闭准备。
    bool stopped_scope_ready(std::size_t path) const;
    // quiescent_scope_ready 核候选原绑定的两端实际关闭；不要求曾取得租约，不推定正式release。
    // 在途操作、未消费事实与未知票据均拒绝，供接续abort等待真实释放使用。
    bool quiescent_scope_ready(std::size_t path) const;
    // refresh_idle_output仅刷新从未分配路径的实际输出false；不创建租约、绑定会话或申请输入。
    // 候选媒体换scope使旧boot票据失效时使用，接纳后仍须等待原两秒窗口的实际回执。
    // 首个人工会话还须原完整boot确曾完成且当前输入idle仍有效，首次绑定再次核全组原实态。
    bool refresh_idle_output(std::size_t path, std::uint64_t now);
    bool idle_output_ready(
        std::size_t path) const;   // 仅原未分配路径当前false已确认，不是完整输入idle或开启授权。
    bool poll(std::uint64_t now);  // 同一事件线程/单调毫秒；超时或事实失效禁止重新初始化。
    // enable_failure_diagnostics 只允许初始化前显式开启；默认关闭且不追溯既有失败。
    void enable_failure_diagnostics();
    // failure_diagnostic 纯读首因副本；未启用或未发生实际失败返回空，不读取端口或重跑poll。
    std::optional<DuplexDriverFailure> failure_diagnostic() const { return failure_; }
    std::optional<FocusIdleObservation> initial_idle_observation() const override;
    std::uint64_t revision() const override { return revision_; }
    // begin的false保证没有本次副作用；第一端已接纳后第二端失败返回true及失败事实，而非假未执行。
    bool begin(const FocusCommand& command, std::uint64_t generation) override;
    std::optional<FocusFact> take_fact(std::string_view session, std::string_view focus) override;
    bool owns(const FocusCommand& applied) const override;  // 同原完整组、操作和两端当前实际票据。
    // owns_observed 在原票据判断处补首因，不重复confirmed或改变原短路顺序。
    bool owns_observed(const FocusCommand& applied, ScopeFailure* observation) const override;
    bool stop_scope(
        std::string_view session, std::string_view focus,
        const std::vector<std::string>& resources) override;  // 被动停止不造release/CAS。
    // stop_input 仅可信本人停止原held腿输入；输出仍沿原票据，不改完整租约或Focus修订。
    bool stop_input(std::string_view session, std::string_view focus, std::uint64_t now);
    // input_stopped 只读原本人停止的输入false及保留输出票据，不表示完整组已released。
    bool input_stopped(std::string_view session, std::string_view focus) const;

   private:
    // Path始终保留原绑定；关闭、超时或取走事实均不复用旧代次。
    struct Path {
        OutputGroupGatePort* output = nullptr;
        std::string session, focus, state;  // 原会话/租约与最近完整实际状态。
        InputGateRequest request;           // 原输入目标；本人停止时与独立输出目标不同。
        std::optional<std::uint64_t> input_ticket, output_ticket;
        std::uint64_t last_output_ticket = 0, deadline = 0;
        bool binding = false, stopping = false,
             unavailable = false;     // 仅未取得租约的绑定失败局部隔离。
        bool output_enabled = false,
             input_stopping = false;  // 仅本人stop_input使两目标分离；在途等真实false。
        bool input_stop_latched = false; // 本人停止意图不因原资源suspend/restore或重投重开采集。
        bool refreshed_idle = false;  // 前会话输出刷新已受理，不把初始boot观察混成刷新回执。
        std::optional<FocusCommand> applied;
        std::optional<FocusFact> fact;  // 至多一条原执行事实，未取走不接新操作。
    };
    // Pending只有一个完整组操作，不以分别开启输入/输出推进两次水位。
    struct Pending {
        std::size_t path;
        FocusCommand command;
        std::uint64_t generation;
    };
    // RetiredScope只保留已释放的原焦点身份；不复用原名称或重置媒体路由及全局票据。
    struct RetiredScope {
        std::string session, focus; // 同进程最多128次恢复；满时拒绝，不驱逐后重新准入。
    };
    std::optional<std::size_t> find(std::string_view session, std::string_view focus) const;
    bool confirmed(const Path& path, ScopeFailure* observation =
                                         nullptr) const;  // 原输出票据及已绑定输入票据同时有效。
    // unallocated
    // 仅从未绑定、从未请求开启且从未分配的原路径；不是新的物理idle证明。
    bool unallocated(const Path& path) const;
    bool request_input(Path& path, bool enabled,
                       std::uint64_t now);  // 核单调票据，非法返回不得当未执行。
    bool request_output(Path& path, bool enabled, std::uint64_t now);
    void close_path(Path& path,
                    std::uint64_t now);  // 保留已在途false，禁止无界反复重发。
    void fail(std::uint64_t now);        // 失败保留原水位及未知事实，不补偿恢复其他原意图。
    // record_failure 仅首次实际失败锁存；后继围栏、析构或重复poll不得覆盖原因。
    void record_failure(DuplexDriverFailureReason reason, int path = -1);
    InputOwnershipGatePort& input_;      // 全部路径共享同一实际采集所有权端口。
    std::vector<Path> paths_;            // 固定最多两条输出路径。
    std::optional<Pending> pending_;
    std::vector<RetiredScope> retired_; // 与FocusEndpoint旧租约回执分离的驱动绑定墓碑。
    std::optional<std::uint64_t> clock_;
    std::uint64_t boot_, revision_, initial_revision_, last_input_ticket_ = 0;
    bool valid_ = true, initializing_ = false, ready_ = false, used_ = false;
    bool observe_failure_ = false; // 默认关闭，不改变调用次数、状态机或期限。
    std::optional<DuplexDriverFailure> failure_; // 固定枚举+路径，禁止自由字符串或身份。
};
}  // namespace ur::device
