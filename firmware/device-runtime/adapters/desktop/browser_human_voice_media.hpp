// 文件说明：借用唯一宿主音频源的纯语音候选桥；网络连通不授予采集或输出焦点。
#pragma once
#include <deque>
#include <functional>
#include "ur/device/human_call.hpp"
#include "ur/device/output_group_gate.hpp"

namespace ur::device {
// BrowserHumanVoiceMedia 只建立单音轨候选与确认原输出线程票据；输入移交由独立仲裁驱动负责。
class BrowserHumanVoiceMedia final : public CallMediaPort, public OutputGroupGatePort {
public:
    // Source 检查原宿主源和本地桥仍有效；不能将此事实当作麦克风所有权。
    using Source = std::function<bool()>;
    // 构造器借用原存活回调；非零native boot代次开启无会话静音探针，默认不猜代次。
    explicit BrowserHumanVoiceMedia(Source source, std::uint64_t boot_generation = 0);
    // bind_scope 固定原合法邀请的会话、路由和Peer代次，一实例不可重新指向其他腿。
    bool bind_scope(std::string_view session, std::uint64_t generation, std::uint64_t epoch);
    bool available() const override;           // 原源有效且实例尚未持有候选时才可准备。
    bool voice_supported() const override;     // 只声明实际源可用，不声明焦点或第二采集。
    bool prepare() override { return false; }  // 此适配不接旧视频入口。
    bool begin() override { return false; }    // 不借视频实现伪造voice成功。
    bool prepare_voice(
        const CallVoiceProfile& profile) override;  // 仅固定48k/mono/20ms，无媒体副作用。
    bool begin_voice_candidate(
        const CallVoiceProfile& profile) override;     // 排原单audio候选命令，不取得麦克风。
    bool apply_answer(std::string_view sdp) override;  // 提交原单audio答案，实际应用仍须原桥回执。
    bool connected() const override;  // 仅已应用答案的真实Peer/ICE/DTLS事实，不是播放完成。
    void close() override;            // 排原腿停止并围栏旧事实，不提前声明实际资源已释放。
    // take_command 只取一条原命令；取出之后才接受该命令对应的实际浏览器回执。
    std::optional<std::string> take_command();
    // receive 校验原认证本地桥的严格作用域和实际状态；非法回执不推进媒体事实。
    bool receive(std::string_view body, std::uint64_t now);
    // receive_stop_diagnostic独立只读业务旁证：非本种类为nullopt，畸形/未启用为false；不poll、查询源或改变媒体门控。
    std::optional<bool> receive_stop_diagnostic(std::string_view body, bool enabled = false);
    // stop_cause只读原scope第一条合法旁证；缺席/无法分类仍unknown，不是媒体停止或释放证明。
    std::string_view stop_cause() const;
    std::optional<std::string> take_offer();  // 单次取出原实际Peer的offer，不生成替代SDP。
    bool poll(std::uint64_t now);             // 固定两秒输出预算、源断开和时钟倒退均安全停候选。
    // failure_reason 只读固定首因；不读源、不推进时钟或消费命令。
    std::string_view failure_reason() const;
    bool stopped() const;  // 仅原Peer/output实际停止回执确认，不等同输入已释放。
    // request_output_gate排新原票据并开始两秒单调预算；未绑定只准boot false。
    std::optional<std::uint64_t> request_output_gate(bool enabled, std::uint64_t now) override;
    bool output_gate_confirmed(std::uint64_t ticket,
                               bool enabled) const override;  // 只读原实际线程完成事实。

private:
    // StopReason 只留本实例首个停止事实；标签不参与门控、网络或资源停止策略。
    enum class StopReason {
        Unknown,
        ClockRollback,
        SourceUnavailable,
        GateDeadline,
        RemoteStopped,
        ExplicitClose
    };
    void latch_stop_reason(StopReason reason);  // 首个已知原因固定，后续清理不得覆盖。
    // Command 保存原种类与正文，不从JSON正文推断已投递事实。
    struct Command {
        std::string kind;
        std::string body;
    };
    bool live() const;  // 隔离宿主回调异常，未知保持不支持。
    std::string command(std::string_view kind,
                        std::string_view sdp = {}) const;  // 编码固定种类及原scope。
    bool enqueue(std::string kind, std::string body);  // 八条/每条64KiB上限，拒绝不能保留的命令。
    // stop_reason_ 只存闭集首因，不包含会话、票据或异常正文。
    StopReason stop_reason_ = StopReason::Unknown;
    std::string stop_cause_ = "unknown";  // 只留本实例固定枚举，不存原报文、身份或异常正文。
    bool stop_diagnostic_seen_ = false;   // 即使首条是unknown，后继重复也不能改写首因。
    Source source_;                       // 原源存活检查，不包含任何默认成功驱动。
    std::string session_, offer_;         // 原会话与不可变offer。
    std::uint64_t generation_ = 0, epoch_ = 0, clock_ = 0, ticket_ = 0,
                  gate_deadline_ = 0;    // 原scope/单调时间/出票水位。
    std::uint64_t boot_generation_ = 0;  // 原native启动代次，初始输出探针不绑定会话或Peer。
    std::deque<Command> commands_;       // 最多8条本地命令，非通用消息总线。
    // 只保存关闭前已实际确认的false票据；须原Peer实际停止后才作为退休旁证，不授予重新输出。
    std::optional<std::uint64_t> retired_false_ticket_;
    bool clock_seen_ = false, prepared_ = false, started_ = false,
         start_sent_ = false;  // 候选只按已提交命令推进。
    bool offer_taken_ = false, answer_queued_ = false, answer_sent_ = false,
         answered_ = false;  // 原协商链独立事实。
    bool connected_ = false, closing_ = false, stop_sent_ = false,
         stopped_ = false;  // 停止申请与实际停止不得混用。
    bool gate_pending_ = false, gate_sent_ = false, gate_enabled_ = false,
         gate_confirmed_ = false;  // 输出线程票据独立于网络。
};
}  // namespace ur::device
