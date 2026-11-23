// 文件说明：唯一MQTT连接上的焦点信令分派；不拥有网络、不默认授权或上报空闲基线。
#pragma once
#include <array>
#include <functional>
#include <memory>
#include <utility>
#include "ur/device/focus_engine.hpp"
#include "ur/device/requests.hpp"

namespace ur::device {
// FocusEndpointResult 区分非本端点报文、非法请求与有界容量；Handled不是实际取得资源。
enum class FocusEndpointResult { Ignored, Invalid, Handled, Full, Closed };
// FocusReceiptObserver 只交付原state/revision/operation/code和本地序号，不暴露身份或资源组。
// MQTT本地接纳不是云端收录；回调须同步复制string_view，不得重入端点、驱动或授权器。
using FocusReceiptObserver =
    std::function<void(std::string_view, std::uint64_t, std::string_view, int, std::uint64_t)>;
// FocusEndpoint 借用唯一全局驱动和可信授权器，固定已鉴权Topic及独立宿主截止。
// 最多四个工作租约；完整释放后可转为有界只读墓碑，旧请求不重新执行或授予输出。
class FocusEndpoint {
public:
    FocusEndpoint(PublishPort& mqtt, FocusDriver& driver, FocusAuthority& authority,
                  std::vector<std::string> resources, std::string up, std::string down,
                  std::uint64_t deadline, bool report_initial_idle = false);
    ~FocusEndpoint();
    FocusEndpoint(const FocusEndpoint&) = delete;
    FocusEndpoint& operator=(const FocusEndpoint&) = delete;
    // receive 仅消费精确SIG Topic的focusControl，驱动执行留给事件线程poll。
    FocusEndpointResult receive(std::string_view topic, std::string_view json, std::uint64_t now,
                                std::uint64_t wall);
    // poll 推进原驱动事实和有界发布；失败保留原字节，不重授许可或重执行。
    void poll(std::uint64_t now, std::uint64_t wall);
    // close 仅关闭本端点持有的原租约，不断开共享MQTT、不控制AI采集。
    void close();
    // close_scope只围栏当前原会话租约，保留端点能力和原关闭资格，不影响另一已知会话。
    bool close_scope(std::string_view session);
    // begin_connection 原资源/结果排空后重新准入，保留旧租约且不延长独立宿主期限。
    bool begin_connection(std::uint64_t now);
    // matches 同时核对唯一发布器与鉴权路由，不允许同Topic接到另一连接。
    bool matches(const PublishPort& publisher, std::string_view up, std::string_view down) const;
    // capability 默认只合并资源；显式上报模式必须取得驱动完整首次idle事实，不改变语音/token。
    WireResult capability(const Envelope& source, Envelope& out) const;
    // recovery_capability只声明原已知会话的资源，不在旧scope未知时补造新的idle基线。
    WireResult recovery_capability(const Envelope& source, std::string_view session, Envelope& out) const;
    // released 为未曾分配或驱动确认全部释放；settled另要求本地原回执已接纳发布。
    bool released() const;
    bool settled() const;
    // cleanup_settled 只核可信owns所选原子会话的实际租约闭合及原回执交付，不改变旧generic释放策略。
    bool cleanup_settled(const std::function<bool(std::string_view)>& owns) const;
    // scoped_permitted只读指定活会话完整资源组；原实际取得与原回执本地交付须同时完成。
    // 不是ReceiptObserver的历史事件；查询不执行HAL、不刷新截止或推进资源水位。
    bool scoped_permitted(std::string_view session, const std::vector<std::string>& resources,
                          std::uint64_t now, std::uint64_t wall,
                          ScopeFailure* observation = nullptr) const;
    // enable_scope_diagnostics 只装配原租约首因观察，不查询或改变当前授权。
    void enable_scope_diagnostics();
    // set_receipt_observer 装配可选纯只读旁路；不缓存队列、重置水位或改变权限与原发布结果。
    void set_receipt_observer(FocusReceiptObserver observer) {
        receipt_observer_ = std::move(observer);
    }

private:
    // Slot 保留原会话/租约及背压结果，避免用新操作覆盖尚未发布的原实际事实。
    struct Slot {
        std::string session, focus;           // 固定原租约的二元身份。
        std::unique_ptr<FocusEngine> engine;  // 驱动与授权器必须比执行器活得久。
        std::optional<std::string> pending;   // 已出队但尚未被网络接纳的不可变原回执。
    };
    // Retired 保留原租约全部事实而不保留可运行执行器；待发重放仅引用原记录下标。
    struct Retired {
        std::string session, focus;              // 固定原租约二元身份，跨连接不复用。
        std::vector<FocusReplayRecord> records;  // 有界原请求/结果，不持驱动或授权器。
        std::optional<std::size_t> pending;      // 发布背压期间固定原回执，不被另一重放覆盖。
    };
    PublishPort& mqtt_;                   // 唯一共享发布器；端点不执行connect/disconnect。
    FocusDriver& driver_;                 // 唯一全局资源水位来源，非每租约虚构计数器。
    FocusAuthority& authority_;           // 必须显式注入，拒绝默认true许可。
    std::vector<std::string> resources_;  // 显式可管理能力全集；各租约固定其申请的完整子集。
    std::string up_, down_;               // 已鉴权设备的固定路由。
    std::uint64_t deadline_;              // 独立宿主单调截止，不从请求expiresAt续长。
    bool report_initial_idle_;            // 宿主显式选择；未知驱动不能以启用选项补造idle。
    std::array<Slot, 4> slots_;           // 固定工作容量，未实际释放或发布未交付不回收。
    std::vector<Retired> retired_;        // 堆上按需保留，最多128租约，不新增大型栈数组。
    std::size_t retired_bytes_ = 0;       // 墓碑对象/记录/字符串容量64KiB，不计分配器和容器空余槽。
    std::optional<std::uint64_t> clock_, wall_;  // 固定连接时钟，回退必须关闭全部原作用域。
    bool closed_ = false;                        // 逻辑关闭与实际释放分离。
    bool valid_ = true;                          // 无效构造不能重新准入为有效配置。
    bool scope_diagnostics_ = false;             // 可选本机诊断，传播到后续原执行器。
    FocusReceiptObserver receipt_observer_;      // 默认关闭，观察回调不能参与业务准入或资源执行。
    std::uint64_t receipt_index_ = 0;  // 单实例交付水位；历史重放保留原revision而只增加序号。
    // observe 防止时钟回退后新租约绕过旧执行器的期限检查。
    void observe(std::uint64_t now, std::uint64_t wall);
    // retire_slot 仅在工作槽满时收紧原终态为只读墓碑；预算不足保持原执行器和事实。
    Slot* retire_slot();
    // report_receipt 仅投影严格校验的原事实；序号耗尽时停观察，不关闭或修改原业务状态。
    void report_receipt(std::string_view reply, std::string_view operation);
};
}  // namespace ur::device
