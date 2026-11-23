// 文件说明：AI转人工的三屏障协调核心；只借共享发布与真实焦点端口，不创建第二路采集或MQTT连接。
#pragma once
#include "ur/device/focus_engine.hpp"
#include "ur/device/requests.hpp"

namespace ur::device {
// AiHandoffState 区分候选、实际资源与平台提交；Unknown禁止自动恢复或重试副作用。
enum class AiHandoffState {
    AiActive,
    Binding,
    Candidate,
    Suspending,
    Suspended,
    Acquiring,
    Acquired,
    Restoring,
    HumanActive,
    Unknown,
    Closed
};
// AiHandoffResult 仅描述本地契约接纳，不把Applied解释为资源移交成功。
enum class AiHandoffResult { Applied, Duplicate, Ignored, Invalid, Rejected };
// AiHandoffConfig 固定原AI及两组租约，时限由可信宿主提供，不能用报文自造授权。
struct AiHandoffConfig {
    std::string topic, ai_session, ai_focus,
        human_focus;  // 新human_focus可空，仅原授权acquire接纳后首次绑定。
    std::uint64_t call_revision = 0, resource_revision = 0,
                  event_watermark = 0;  // 呼叫CAS/真实资源CAS/原共享水位。
    std::uint64_t deadline = 0,
                  expires_at =
                      0;  // 原AI可信固定业务单调/Unix毫秒期限，非候选45秒操作窗，不因重放延长。
};
// AiHandoffSessionPort 必须装配真实Voice/Human协调宿主；候选只准备Peer，绝不默认取得麦克风。
class AiHandoffSessionPort {
public:
    virtual ~AiHandoffSessionPort() = default;
    // ai_authorized_and_owned 核验原完整输入输出组及原用户意图；没有事实须false。
    virtual bool ai_authorized_and_owned(std::string_view session, std::string_view focus,
                                         std::uint64_t now, std::uint64_t wall) const = 0;
    // prepare_voice_candidate 必须通过原HumanCall Voice严格解析器，只准备不采集/播放。
    virtual bool prepare_voice_candidate(const Envelope& invite) = 0;
    // candidate_media_revision 只返回该原本端腿实际Peer就绪水位，不凭ACK/connected请求推断。
    virtual std::optional<std::uint64_t> candidate_media_revision(std::string_view local) const = 0;
    // restore_authorized 重新核原AI租约/用户意图及原候选路由明确未提交，不从旧成功回执推断。
    virtual bool restore_authorized(std::string_view session, std::string_view focus,
                                    std::uint64_t now, std::uint64_t wall) const = 0;
    virtual void close_candidate(
        std::string_view local) = 0;  // 仅关闭候选Peer，不能关闭原AI物理资源。
    virtual void fence_ai(
        std::string_view session) = 0;  // 最终持久提交后围栏原迟到文本/工具/音频。
    virtual void release_ai(
        std::string_view session) = 0;  // 只释放原AI上下文，不能停新腿采集/播放。
};
// AiHandoffFocusResult 不从denied/超时推断未执行，补偿资格必须由真实适配明确提供。
struct AiHandoffFocusResult {
    FocusState actual;                    // 原完整实际回执，仍须校验原token/会话/租约/资源/CAS。
    bool definitely_not_applied = false;  // 原驱动保证整组未取得；默认未知，不增加公共字段。
};
// AiHandoffFocusPort 借真实FocusEngine/Endpoint的已授权整组执行；受理不等于实际完成。
class AiHandoffFocusPort {
public:
    virtual ~AiHandoffFocusPort() = default;
    virtual bool supports_microphone_speaker() const = 0;  // 实际唯一驱动支持完整组才可声明。
    virtual FocusSubmission submit(const FocusCommand& command, std::uint64_t now,
                                   std::uint64_t wall) = 0;
    // take_result 只能给本协调器已提交操作的实际回执；不可把网络下行focusState当驱动事实。
    virtual std::optional<AiHandoffFocusResult> take_result() = 0;
};
// AiHumanHandoff 单事件线程协调一次明确意图；MQTT/其他会话的分派及共享水位仍由唯一宿主管理。
class AiHumanHandoff {
public:
    AiHumanHandoff(PublishPort& publish, AiHandoffSessionPort& sessions, AiHandoffFocusPort& focus,
                   AiHandoffConfig config);
    AiHumanHandoff(const AiHumanHandoff&) = delete;  // 不能复制正在借用同一驱动的原操作代次。
    AiHumanHandoff& operator=(const AiHumanHandoff&) =
        delete;  // 一份协调状态只能由唯一事件线程推进。
    // begin 显式device或service目标；consent_id存在时仅向service传一个本人原授权引用。
    // 引用不授本人权限，不接受正文/版本/摘要；后端仍核原授权，缺省保持不分享且不自动重发。
    bool begin(std::string token, std::string_view target_kind, std::string_view target_id,
               std::uint64_t now, std::uint64_t wall,
               std::optional<std::string_view> consent_id = std::nullopt);
    // receive 仅供已鉴权SIG宿主调用，完整信封再次严格校验；无关事件不会关闭主AI。
    AiHandoffResult receive(const Envelope& event, std::uint64_t now, std::uint64_t wall);
    // observe_event 推进其他已验证SIG事件的共享水位，不把名册/媒体序号缺口误判为丢消息。
    bool observe_event(std::string_view session, std::uint64_t sequence);
    // cancel 仅提交原已绑定候选取消，不把发送/ACK当成恢复物理资源。
    bool cancel(std::string token, std::uint64_t now, std::uint64_t wall);
    // poll 消费至多4条真实焦点事实并复验固定期限；没有自动恢复、第二路采集或无限重试。
    void poll(std::uint64_t now, std::uint64_t wall);
    AiHandoffState state() const { return state_; }  // 只读阶段，不代表媒体实际呈现。
    bool ai_forwarding_allowed() const;  // 整组暂停后即阻断旧输出，最终提交前不释放原上下文。
    std::string_view local_session() const { return local_; }  // 只供宿主路由，禁止默认写日志。
private:
    bool clock_valid(std::uint64_t now, std::uint64_t wall);  // 两时钟回退/原截止严格未知。
    bool send(const Envelope& event);  // 共享发布，不消费MQTT事件、不创建连接。
    AiHandoffResult binding(const Envelope& event, std::uint64_t now, std::uint64_t wall);
    AiHandoffResult invite(const Envelope& event, std::uint64_t now, std::uint64_t wall);
    AiHandoffResult control(const Envelope& event, std::uint64_t now, std::uint64_t wall);
    void finish_commit();                 // 三屏障同时满足才永久围栏，原Ended才释放上下文。
    void fail_before_focus();             // 未开始整组副作用时失败保留AI，不猜已暂停后的恢复。
    bool compensation_available() const;  // 完整实际暂停且明确从未取得新组，才可申请独立补偿授权。
    PublishPort& publish_;                // 借用唯一MQTT上行。
    AiHandoffSessionPort& sessions_;      // 实际候选与旧AI宿主。
    AiHandoffFocusPort& focus_;           // 实际完整组驱动/授权入口。
    AiHandoffConfig config_;              // 固定原会话及期限。
    AiHandoffState state_ = AiHandoffState::AiActive;  // 初始仅原AI，无任何候选或整组副作用。
    std::string token_, cancel_token_, local_, remote_, group_,
        binding_bytes_;                     // 原意图/取消与两腿绑定，不取全局当前会话。
    std::optional<Envelope> early_invite_;  // 最多一个未绑定邀请，不开Peer、不抢占AI。
    std::optional<FocusCommand> pending_;   // 唯一本轮实际操作，不能消费其他租约事实。
    std::vector<FocusCommand> controls_;    // 最多三条suspend/acquire/restore原token墓碑。
    std::vector<Envelope> replies_;         // 至多三条不可变实际结果，背压不重复驱动。
    std::uint64_t deadline_ = 0, now_ = 0, wall_ = 0, ai_seq_ = 0,
                  local_seq_ = 0;       // 固定候选截止/两时钟/独立腿共享水位。
    std::string ai_last_, local_last_;  // 当前已消费水位原字节，重复冲突不能冒幂等。
    bool clock_seen_ = false, suspended_ = false, acquired_ = false,
         acquire_denied_ = false;  // 实际事实不从请求或ACK推断。
    bool established_ = false, committed_ = false, ended_ = false, fenced_ = false,
         released_ = false;            // 平台双确认和旧AI专属收尾均只执行一次。
    bool candidate_prepared_ = false;  // 只有原邀请通过实际候选端口后才可使用该腿就绪事实。
};
}  // namespace ur::device
