// 文件说明：唯一事件线程的输出资源仲裁；实际原路径门控完成后才推进全局修订号。
#pragma once
#include "ur/device/focus_engine.hpp"
#include "ur/device/output_group_gate.hpp"

namespace ur::device {
// FocusBindingMode 延迟模式由可信宿主绑定；可复用模式还要求旧租约显式释放及原回执被收录。
enum class FocusBindingMode { Fixed, DeferredOnce, DeferredReleased };
// FocusOutputBinding 由可信宿主绑定原租约及真实独占路径，不接受模型或设备报文自建驱动。
struct FocusOutputBinding {
    std::string session, focus;           // 原身份；延迟模式初始为空，可复用模式须实际释放后改绑。
    std::vector<std::string> resources;   // 实际路径完整输出组，目前speaker/display，不包含采集。
    OutputGroupGatePort* port = nullptr;  // 借用真实路径，寿命必须长于驱动及所有执行器。
};
// OutputFocusDriver 在一个连接内管理最多四个固定输出路径，所有租约共享一个修订号。
// FocusAuthority仍独立复核业务许可；本类不自行提权、抢占或自动恢复用户意图。
class OutputFocusDriver final : public FocusDriver {
public:
    // initial_revision由宿主明确提供；构造不授予输出，不证明空闲或执行驱动。
    OutputFocusDriver(std::vector<FocusOutputBinding> bindings, std::uint64_t initial_revision,
                      FocusBindingMode mode = FocusBindingMode::Fixed);
    // bind_scope 在实际静音后绑定；DeferredReleased可更换已释放身份，旧身份永不重新准入。
    bool bind_scope(std::size_t path, std::string session, std::string focus, std::uint64_t now);
    // 析构只尽力请求关闭；宿主须保留驱动到原实际停止收录，不能以析构当释放证明。
    ~OutputFocusDriver() override;
    // initialize请求全部实际路径关闭，必须由poll收齐原票据才能提供首次idle证明。
    bool initialize(std::uint64_t now);
    // refresh_idle 仅原已实际false且无租约/在途/未取事实的路径重建末端基线；不涨CAS修订号。
    // 原lease历史不变，刷新期间同资源取得Busy；超时只封该路径，不停止无关现有AI输出。
    bool refresh_idle(std::size_t path, std::uint64_t now);
    // idle_refresh_ready 新实际false已确认，不能用构造/命令受理/旧node的票据推定。
    bool idle_refresh_ready(std::size_t path) const;
    // idle_refresh_failed 固定该路径不可用；迟到false不能重新授予新节目或恢复路径。
    bool idle_refresh_failed(std::size_t path) const;
    // poll只在设备事件线程运行；now为同一单调毫秒时钟，回退或超时关闭所有本驱动路径。
    bool poll(std::uint64_t now);
    // initial_idle仅在首次完整静音、没有接纳过任何控制时有效，不能重新基线化旧连接。
    bool initial_idle() const;
    // initial_idle_observation 仅收齐全部原false票据且未使用时提供整组实际水位，不授予输出。
    std::optional<FocusIdleObservation> initial_idle_observation() const override;
    std::uint64_t revision() const override { return revision_; }
    bool begin(const FocusCommand& command, std::uint64_t generation) override;
    std::optional<FocusFact> take_fact(std::string_view session, std::string_view focus) override;
    bool owns(const FocusCommand& applied) const override;
    // stop_scope只关闭匹配的原完整组；未知、在途或错组不补造实际释放，也不操作输入。
    bool stop_scope(std::string_view session, std::string_view focus,
                    const std::vector<std::string>& resources) override;

private:
    // Path保留固定整组与端口；只在显式复用模式收录实际release后更换身份，不改换端口。
    struct Path {
        FocusOutputBinding binding;              // 固定原路径及身份。
        std::string state;                       // 空/ acquired/suspended/restored/released。
        std::optional<std::uint64_t> ticket;     // 当前请求的实际原票据，不用排队代替完成。
        bool enabled = false, stopping = false;  // 原目标及独立关闭流程。
        bool refreshing = false,
             unavailable = false;             // 原无lease节点重建及永久局部故障，非新全局仲裁。
        std::uint64_t deadline = 0;           // 原门控两秒截止，重试不续期。
        std::optional<FocusFact> fact;        // 原租约最多一条未取完成，不覆盖旧事实。
        std::optional<FocusCommand> applied;  // 当前取得操作，历史成功不能授予新token。
    };
    // Pending固定唯一整组操作；全局CAS串行化，接纳不推进revision。
    struct Pending {
        std::size_t path;          // 原路径索引，永不指向后来替换对象。
        FocusCommand command;      // 原完整请求，取消不能改写为另一个成功。
        std::uint64_t generation;  // 执行器原代次。
    };
    std::optional<std::size_t> find(std::string_view session, std::string_view focus) const;
    bool confirmed(const Path& path) const;  // 只读真实端口的当前原票据事实。
    void fail();                      // 全局观察不可用时请求本驱动所有路径关闭，不输出虚假成功。
    std::vector<Path> paths_;         // 有界原路径，不复制平台驱动。
    std::optional<Pending> pending_;  // 全局唯一正在执行的原操作。
    std::optional<std::uint64_t> clock_;         // 所有门控与截止共用的单调时钟。
    std::uint64_t revision_, initial_revision_;  // 当前与首次实际水位，不能溢出回零。
    FocusBindingMode binding_mode_;              // 构造时固定，既有调用默认禁止后置身份绑定。
    std::vector<std::pair<std::string, std::string>>
        retired_;  // 最多128个旧身份，满额拒绝而非淘汰栅栏。
    bool valid_ = true, initializing_ = false, ready_ = false, used_ = false;
    // 上述标志分别表示配置有效、首次初始化在途、可接纳控制及禁止重复idle证明。
};
}  // namespace ur::device
