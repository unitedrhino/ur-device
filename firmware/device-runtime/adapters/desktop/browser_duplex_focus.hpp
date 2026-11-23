// 文件说明：唯一AI/人工宿主的完整麦克风与扬声器许可；不初始化驱动或自行绑定原腿。
#pragma once
#include <functional>

#include "ur/device/duplex_focus_driver.hpp"

namespace ur::device {
// BrowserDuplexRole 固定原腿用途，不允许请求临时把AI身份改成人工候选。
enum class BrowserDuplexRole { AI, Human };
// BrowserDuplexBinding
// 由唯一宿主原bind_scope入参与原权限期限固定，页面不能提供此来源。
struct BrowserDuplexBinding {
    std::size_t path = 0;                           // 真实DuplexFocusDriver原路径。
    std::string session, focus;                     // 原会话及焦点，不用组ID替代。
    std::uint64_t route_generation = 0;             // 真实输入路由代次。
    std::uint64_t intent_version = 0;               // 原用户意图，恢复不得换代。
    std::uint64_t peer_epoch = 0;                   // Human原Peer代次；无Peer原AI允许零，不造Peer。
    std::uint64_t deadline = 0;                     // 原宿主单调绝对截止。
    std::uint64_t expires_at = 0;                   // 原宿主Unix毫秒绝对截止。
    BrowserDuplexRole role = BrowserDuplexRole::AI; // 固定AI或人工权限域。
};
// BrowserDuplexScope
// 是可信宿主独立快照；门控false或网络Established不能替代授权。
struct BrowserDuplexScope {
    BrowserDuplexBinding binding;   // 必须完整匹配宿主原绑定和原期限。
    bool authorized = false;        // 原身份、意图及媒体路由权限仍有效。
    bool peer_ready = false;        // Human原Peer/AI原媒体路由已准备，不代表采集已启动。
    bool handoff_committed = false; // AI移交不可逆围栏，旧AI只能明确关闭。
    bool closing = false;           // 原腿已明确关闭，只允许有限release。
    bool acquire = false, suspend = false, restore = false,
         release = false;        // 各操作独立持续权限，base授权不能替代原恢复意图。
    std::string operation_token; // 当前原控制token；仅签发时核对，不取页面票据。
    // 当前版本只由native真实用户停止／源切换推进；默认沿原绑定，不更新暂停冻结的原版本。
    std::optional<std::uint64_t> current_intent_version = std::nullopt;
    // 原停止操作的两份固定短截止，只有宿主核过原release才填写，重投不续期。
    std::optional<std::uint64_t> cleanup_deadline = std::nullopt;
    std::optional<std::uint64_t> cleanup_expires_at = std::nullopt;
};
// BrowserDuplexCleanupBounds
// 保存原宿主自身生命周期，不能替代原业务绑定的播放/采集期限。
struct BrowserDuplexCleanupBounds {
    std::uint64_t deadline = 0;    // 原宿主单调截止，停止也不能越过。
    std::uint64_t expires_at = 0;  // 原宿主墙钟截止，不能由重投或恢复延长。
};
// BrowserDuplexScopeReader
// 在唯一宿主事件线程同步读取且不抛异常；缺回调/未知快照默认拒绝。
using BrowserDuplexScopeReader = std::function<std::optional<BrowserDuplexScope>()>;
// BrowserDuplexFocusAuthority
// 每实例仅授一个固定原腿，两个实例共用同一真实driver。
class BrowserDuplexFocusAuthority final : public FocusAuthority {
    public:
    // 构造不产生闲置事实/绑定/许可；binding须来自宿主执行过的原bind_scope调用。
    BrowserDuplexFocusAuthority(DuplexFocusDriver& driver, BrowserDuplexBinding binding,
                                BrowserDuplexScopeReader scope,
                                std::optional<BrowserDuplexCleanupBounds> cleanup = std::nullopt);
    // admit_binding
    // 在真实scope_ready后显式核对原入参及可信快照；不能从请求自绑定。
    bool admit_binding(const BrowserDuplexBinding& original, std::uint64_t now, std::uint64_t wall);
    // authorize 仅在真实CAS、完整组与独立操作权限齐全时签发原有界许可。
    std::optional<FocusPermit> authorize(const FocusCommand& command, std::uint64_t now,
                                         std::uint64_t wall) override;
    // valid 核完整原签发副本及双绝对截止，不反复比较两个时钟剩余长度。
    bool valid(const FocusPermit& permit, std::uint64_t now, std::uint64_t wall) const override;
    // valid_observed
    // 只在原current/许可/独立操作权限求值现场记录，不二次读宿主或原票据。
    bool valid_observed(const FocusPermit& permit, std::uint64_t now, std::uint64_t wall,
                        ScopeFailure* observation) const override;
    // closed_release_valid
    // 仅原已实际分配整组且宿主明确关闭的原release可有限清理。
    bool closed_release_valid(const FocusPermit& permit, std::uint64_t now,
                              std::uint64_t wall) const override;

    private:
        friend class BrowserDuplexStaticFaultTest;  // 仅测试租约期限不成对，不提供生产授予端口。
        // observe 时钟回退永久拒绝；不靠后继正确时间复活旧许可。
        bool observe(std::uint64_t now, std::uint64_t wall) const;
        // current 原绑定/期限/Peer必须齐全，AI提交后只留明确关闭资格。
        std::optional<BrowserDuplexScope> current(std::uint64_t now, std::uint64_t wall) const;
        // current_release
        // 仅可信原停止指令可在业务到期后核原宿主短清理期限；其他操作仍走current。
        std::optional<BrowserDuplexScope> current_release(std::uint64_t now,
                                                        std::uint64_t wall) const;
        // observe_allocation 仅从driver当前原整组拥有保存分配证明，签发不等于分配。
        void observe_allocation() const;
        DuplexFocusDriver& driver_;           // 唯一宿主实际输入输出组仲裁器。
        const BrowserDuplexBinding binding_;  // 原宿主不可变身份及期限。
        BrowserDuplexScopeReader scope_;      // 不读页面自报或网络未知字段。
        const std::optional<BrowserDuplexCleanupBounds> cleanup_;  // 原宿主固定的清理上界。
        bool admitted_ = false;               // 仅原显式绑定确认后为真。
        std::vector<FocusPermit> issued_;     // 最多32条，不驱逐仍被FocusEngine持有的原lease。
        std::optional<std::uint64_t> lease_deadline_,
            lease_wall_;  // 首次原租约不得由restore续期。
        mutable std::optional<std::uint64_t> now_,
            wall_;  // 双时钟各自单调，不比较剩余长度。
        mutable bool clock_failed_ = false,
                     ai_committed_ = false;  // 回退/最终移交不可逆。
        mutable bool allocated_ = false;     // 原实际完整组曾已分配，不能从仅签发推断关闭权。
};
} // namespace ur::device
