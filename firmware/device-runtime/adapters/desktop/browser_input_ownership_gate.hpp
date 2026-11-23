// 文件说明：唯一浏览器输入所有权适配；CPP旧队列围栏与原Worklet实际处理回执缺一不可。
#pragma once
#include <functional>
#include <vector>
#include "browser_input_ownership_wire.hpp"

namespace ur::device {
// BrowserInputRouteKind区分UDP线程与原Sender事实；AI/人工均不能借Worklet中间回执取得权限。
enum class BrowserInputRouteKind { Udp, HumanSender, AISender };
// BrowserInputOwnershipGate只在宿主事件线程访问；借原本地桥，不创建网络、媒体源或Peer。
class BrowserInputOwnershipGate final : public InputOwnershipGatePort {
public:
    // Source检查原boot配对的唯一活源及原Sender registry；Scope检查原已绑定会话而非页面当前值。
    using Source = std::function<bool()>;
    using Scope = std::function<bool(const InputGateRequest&)>;
    // Fence仅确认CPP原PCM队列/UDP转发已围栏；false必须保证未执行，不代替physical ACK。
    using Fence = std::function<bool(std::string_view, std::uint64_t)>;
    // 构造仅借真实端口；boot为原页面配对代次，缺任一回调不得取得初始事实。
    BrowserInputOwnershipGate(std::uint64_t boot, Source source, Scope scope, Fence fence);
    // bind_route只登记可信宿主原关联，不授资源；最多两条且原session/代次不可复用。
    bool bind_route(std::string session, std::uint64_t generation, BrowserInputRouteKind kind);
    // initial_idle_observation仅返回原actual无session处理块探针，租约必须另取实际票据。
    std::optional<std::uint64_t> initial_idle_observation() const override;
    // request_input_gate接纳唯一原scope票据，null保证未发命令；false先完成CPP旧帧围栏。
    std::optional<std::uint64_t> request_input_gate(const InputGateRequest& request,
                                                    std::uint64_t now) override;
    // input_gate_confirmed仅认可最新原票据/完整scope及当前活源，不认可旧相同enabled。
    bool input_gate_confirmed(std::uint64_t ticket, const InputGateRequest& request) const override;
    std::optional<std::string> take_command();  // 标记原票据已交付，未送命令不得收成功。
    bool receive(std::string_view body, std::uint64_t now);  // 严格实际回调，错原关联不冒成功。
    bool poll(std::uint64_t now);                            // 固定两秒，不因重复确认或退役延长。
    void disconnect();                                       // 原页面/源断连后所有旧事实永久失效。
private:
    struct Route {
        std::string session;
        std::uint64_t generation;
        BrowserInputRouteKind kind;
    };  // 原固定关联。
    struct Record {
        InputGateRequest request;
        std::uint64_t ticket;
        bool actual = false;
    };  // 最多128原事实，不淘汰复用票据。
    bool live() const;  // 可信宿主source回读，回调异常固定未知。
    bool scope_live(const InputGateRequest& request) const;
    std::optional<std::size_t> route(const InputGateRequest& request) const;
    Source source_;
    Scope scope_;
    Fence fence_;  // 必须真实装配，无默认true。
    std::vector<Route> routes_;
    std::vector<Record> records_;
    std::optional<std::size_t> pending_;
    std::optional<std::uint64_t> clock_;
    std::string command_;  // 仅唯一原在途，发送后清空而不重执行。
    std::uint64_t boot_, ticket_ = 0, deadline_ = 0;
    bool idle_ = false, sent_ = false, closed_ = false;
};
}  // namespace ur::device
