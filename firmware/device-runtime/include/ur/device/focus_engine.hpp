// 文件说明：单原租约的异步焦点执行器；全局仲裁由共享驱动管理，核心不默认授权或启动采集。
#pragma once
#include "ur/device/fixed_queue.hpp"
#include "ur/device/focus_scope_diagnostic.hpp"
#include "ur/device/focus_wire.hpp"

namespace ur::device {
// FocusPermit 由可信会话/权限/用户意图适配给出，必须精确匹配原操作且不能用请求截止自造许可。
struct FocusPermit {
    FocusCommand scope;                // 固定原token/session/focusId/完整资源与操作字段。
    std::uint64_t deadline = 0;        // 独立授权的单调毫秒截止，重复提交不能刷新。
    std::uint64_t intent_version = 0;  // 原用户意图版本，restore不能借新版本复活旧租约。
};
// FocusAuthority 检查可信业务作用域；没有适配就不能构造执行器，不提供默认true实现。
class FocusAuthority {
public:
    virtual ~FocusAuthority() = default;
    // authorize 只在新原操作准入复核时调用，空值拒绝；不得在设备事件线程阻塞网络。
    virtual std::optional<FocusPermit> authorize(const FocusCommand& command, std::uint64_t now,
                                                 std::uint64_t wall) = 0;
    // valid 复核原固定许可/会话/权限/意图，不重新授予许可或延长截止。
    virtual bool valid(const FocusPermit& permit, std::uint64_t now, std::uint64_t wall) const = 0;
    // valid_observed
    // 保留一次原valid求值；未适配层级时仅记录固定authority拒绝，不重新授权。
    virtual bool valid_observed(const FocusPermit& permit, std::uint64_t now, std::uint64_t wall,
                                ScopeFailure* observation) const {
        return valid(permit, now, wall) ||
               scope_reject(observation, ScopeFailureCode::AuthorityDenied);
    }
    // closed_release_valid 默认拒绝Closed租约执行任何新操作；仅可信有限原终态关闭适配显式启用。
    // 即使启用也只准原已实际持有并已停止输出的完整组release，不能使旧acquire/restore重新有效。
    virtual bool closed_release_valid(const FocusPermit&, std::uint64_t, std::uint64_t) const {
        return false;
    }
};
// FocusFact 是整组驱动实际完成事实；部分资源或命令受理不能报告成功。
struct FocusFact {
    std::uint64_t generation = 0;        // 驱动原操作代次，旧回调不能修改新的等待。
    bool success = false;                // 只在全部实际资源完成后为true。
    std::string state;                   // 成功对应acquired/suspended/restored/released。
    std::vector<std::string> resources;  // 原完整实际资源集合，不得仅回speaker。
    std::uint64_t revision = 0;          // 实际全局资源修订号，CAS成功后推进一次。
};
// FocusIdleObservation 是首次完整物理空闲观察，不能从能力、构造或逻辑释放推断。
struct FocusIdleObservation {
    std::vector<std::string> resources;  // 全部管理路径去重后的完整资源，不允许部分组。
    std::uint64_t revision = 0;          // 唯一实际驱动水位，零为合法初始值。
};
// FocusDriver 由宿主唯一全局资源管理器实现，多个租约执行器不得各自伪造全局revision。
class FocusDriver {
public:
    virtual ~FocusDriver() = default;
    virtual std::uint64_t revision() const = 0;  // 当前实际全局修订号，不是已排队请求数。
    // initial_idle_observation 默认未知；只有独立物理事实驱动能提供首次完整空闲，不自动重新基线化。
    virtual std::optional<FocusIdleObservation> initial_idle_observation() const { return {}; }
    // begin 非阻塞整组CAS；false保证未执行，true仅接纳，不隐含停止其他输入/输出。
    virtual bool begin(const FocusCommand& command, std::uint64_t generation) = 0;
    virtual std::optional<FocusFact> take_fact(
        std::string_view session,
        std::string_view focus) = 0;  // 精确原租约流，不能消费别的执行器事实。
    // owns 核验原租约完整实际所有权，不凭一个已静音扬声器确认文件整组许可。
    virtual bool owns(const FocusCommand& applied) const = 0;
    // owns_observed 兼容原驱动；一次原owns失败只补闭集层级，不执行新驱动查询。
    virtual bool owns_observed(const FocusCommand& applied, ScopeFailure* observation) const {
        return owns(applied) || scope_reject(observation, ScopeFailureCode::DriverState);
    }
    // stop_scope 幂等关闭此原作用域，不清其他会话；true只能代表整组实际停止/释放。
    virtual bool stop_scope(std::string_view session, std::string_view focus,
                            const std::vector<std::string>& resources) = 0;
};
// FocusAdmission 仅描述准入；Accepted/Pending不是资源实际取得。
enum class FocusAdmission { Accepted, Pending, Replay, Conflict, Invalid, Full, Busy, Closed };
// FocusSubmission 返回准入结果和可选原重放回执，不能以Accepted判断实际资源已取得。
struct FocusSubmission {
    FocusAdmission admission = FocusAdmission::Invalid;  // 默认拒绝未知输入。
    std::string reply;                                   // 只有Replay携带不可变原实际结果。
};
// FocusReplayRecord 是已闭合租约的紧凑墓碑，不持驱动、许可或可执行状态。
struct FocusReplayRecord {
    std::string token;    // 原请求关联，不允许用新token复活已释放租约。
    std::string request;  // 规范化原信封，保证同token字段冲突仍被拒绝。
    std::string reply;    // 固定原实际结果，迟到请求只重放此字节。
};
// FocusEngine 在设备事件线程驱动一个固定原租约，最多32条近期原操作、4条待交付回执。
// 宿主须保留到实际释放/明确未知及幂等保留窗结束，驱动/授权器寿命必须长于执行器。
class FocusEngine {
public:
    FocusEngine(FocusDriver& driver, FocusAuthority& authority, std::string session,
                std::string focus, std::vector<std::string> resources, std::uint64_t deadline,
                std::uint64_t timeout = 10000);
    ~FocusEngine();
    FocusEngine(const FocusEngine&) = delete;
    FocusEngine& operator=(const FocusEngine&) = delete;
    // submit 严格解析、原token去重后绑定许可；不在MQTT回调执行驱动动作。
    FocusSubmission submit(std::string_view json, std::uint64_t now, std::uint64_t wall);
    // poll 至多消费4个事实；持续复核许可，超时/回退/畸形实际事实只关闭原作用域。
    void poll(std::uint64_t now, std::uint64_t wall);
    std::optional<std::string> take_reply();  // 发布背压不得改变原结果或重新执行。
    // reply_operation 只查询原字节回执关联的操作；历史结果不授予当前资源或更新执行状态。
    std::optional<std::string_view> reply_operation(std::string_view reply) const;
    // delivered_reply只记录本地MQTT已接纳的当前实际取得原回执；旧重放不能覆盖新restore许可。
    void delivered_reply(std::string_view reply);
    void close();  // 取消当前等待；实际释放未知时仍需poll，不可立即复用资源。
    bool closed() const { return closed_; }      // 逻辑关闭，不能代替实际资源释放。
    bool released() const { return released_; }  // 仅驱动真实整组释放为true。
    // resources_released 区分从未执行驱动的空作用域与已分配后待实际释放的作用域。
    bool resources_released() const { return !allocated_ || released_; }
    // lease_settled
    // 仅从未开始驱动或已收到原release实际事实才闭合；被动stop/未知取得不代表租约释放。
    bool lease_settled() const { return !active_ && (!allocated_ || state_ == "released"); }
    // replies_drained 只有原结果全部编码并交给宿主后为true，不代表MQTT确认或远端完成。
    bool replies_drained() const;
    // retirement 仅整组实际释放且原结果全部交付后导出墓碑；空结果禁止删除执行器。
    std::optional<std::vector<FocusReplayRecord>> retirement() const;
    bool output_permitted(std::uint64_t now, std::uint64_t wall);  // 核对当前原整组实际拥有。
    // scoped_permitted只读完整原资源组的当前真实许可；不执行observe、重新授权、驱动或回执。
    bool scoped_permitted(const std::vector<std::string>& resources, std::uint64_t now,
                          std::uint64_t wall, ScopeFailure* observation = nullptr) const;
    // enable_scope_diagnostics
    // 只观察后续原租约失效；不重读当前许可或补造过去首因。
    void enable_scope_diagnostics() { scope_diagnostics_ = true; }
    // scope_failure 只读原租约失效首因；未开启或未观察时为空。
    std::optional<ScopeFailureCode> scope_failure() const {
        return first_scope_failure_ == ScopeFailureCode::None
                   ? std::nullopt
                   : std::optional<ScopeFailureCode>(first_scope_failure_);
    }

private:
    struct Result {
        std::string state, reason;   // 首次确认的固定公开状态与原因，不接受自由驱动错误文字。
        std::uint64_t revision = 0;  // 固定实际完成/拒绝时的全局修订号，不从后续操作推断。
        int code = 0;                // 固定原操作结果码。
    };
    struct Record {
        FocusCommand command;          // 固定原规范化报文，不能用同token换操作/资源/会话。
        std::string reply;             // 固定原结果，迟到事实不能覆盖。
        std::optional<Result> result;  // 编码失败保留原事实，恢复后仍按原值编码而非重新执行。
        bool queued = false;           // 原结果只进入本地交付队列一次，重放通过submit独立返回。
    };
    bool observe(std::uint64_t now, std::uint64_t wall);  // 两类时钟、独立授权截止及撤权检查。
    // begin_cleanup 只恢复原对象的有限关闭执行路径；原历史/意图/实际水位不重置，禁止所有开启操作。
    bool begin_cleanup(const FocusCommand& command, std::uint64_t now, std::uint64_t wall);
    bool emit(std::size_t index, std::string_view state, std::uint64_t revision, int code,
              std::string_view reason);            // 编码失败必须关闭，不生成半组证明。
    void fail(std::string_view reason, int code);  // 固定原失败回执并切断原作用域。
    void flush();                  // 编码/发布背压恢复后交付原固定事实，不刷新驱动、授权或修订号。
    FocusDriver& driver_;          // 与全局资源管理器关联的原租约驱动端口。
    FocusAuthority& authority_;    // 必须显式装配的可信许可入口。
    std::string session_, focus_;  // 固定原会话及租约身份，不从控制参数换绑。
    std::vector<std::string> resources_;                  // 固定完整资源集合。
    std::uint64_t deadline_, timeout_;                    // 独立宿主截止及操作固定等待预算。
    std::vector<Record> history_;                         // 不提前驱逐旧token重新执行。
    FixedQueue<std::string, 4> replies_;                  // 有界待发布原结果。
    std::optional<std::size_t> active_;                   // 唯一等待原操作索引。
    std::optional<FocusPermit> pending_, lease_;          // 操作许可与实际租约许可独立。
    std::optional<std::uint64_t> clock_, wall_, intent_;  // 两类时钟与原意图版本。
    std::uint64_t generation_ = 0, sequence_ = 0, started_at_ = 0;  // 不复用旧驱动代次及平台水位。
    std::uint64_t lease_deadline_ = 0;    // 首次原授权截止，restore不能续长。
    std::uint64_t pending_deadline_ = 0;  // 原操作有效截止独立保存，不篡改可信许可结构。
    std::uint64_t lease_wall_deadline_ = 0,
                  pending_wall_deadline_ = 0;  // 原绝对截止也必须遵守，restore不能改长。
    std::string state_;                        // 最近确认实际状态，关闭后仅为历史观察。
    std::string
        delivered_lease_token_;  // 当前原取得回执已实际发布的token；未发布新restore仍拒许可。
    bool started_ = false, allocated_ = false;  // 驱动已受理与原作用域可能有资源。
    bool closed_ = false, released_ = false;    // 逻辑终态与实际释放不可互相推断。
    bool cleanup_only_ = false;  // 原Closed租约只可继续有限原release，不恢复开启权或清历史。
    bool scope_diagnostics_ = false;  // 可信本机开启，不受协议报文控制。
    ScopeFailureCode first_scope_failure_ =
        ScopeFailureCode::None;  // 原租约首次失效，不被close覆盖。
};
}  // namespace ur::device
