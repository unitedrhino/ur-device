// 文件播放器异步状态机；驱动确认实际媒体状态，核心不下载文件或授予输出焦点。
#pragma once
#include <optional>
#include <vector>
#include "ur/device/fixed_queue.hpp"
#include "ur/device/playback_wire.hpp"

namespace ur::device {
// PlaybackFact 是驱动在设备事件线程交付的事实，不是开始工作的受理确认。
struct PlaybackFact {
    std::uint64_t generation = 0;   // 当前驱动命令代次；旧代次永远不能修改新状态。
    bool spontaneous = false;       // 自然状态或长seek完成；不冒用原命令应答。
    int code = 200;                 // 命令实际成功200，失败400～599；主动事件不序列化code。
    std::string state;              // 实际loaded/playing/paused/seeking/stopped/ended/failed。
    std::uint64_t position_ms = 0;  // 实际解码位置，不能回显seek目标代替实际位置。
    std::optional<std::uint64_t> duration_ms;  // 已知真实时长，不允许小于position_ms。
    double rate = 1;                           // 实际倍率，只接受协议枚举。
};
// PlaybackDriver 方法非阻塞、不持有命令引用；媒体线程通过有界队列交付事实。
// 驱动必须自行检查内容引用授权及焦点，禁止将contentRef直接作为任意URL或文件路径。
class PlaybackDriver {
public:
    virtual ~PlaybackDriver() = default;
    // begin
    // 接纳一次操作，false保证未产生副作用；true仅受理，不能用它报告playing。
    virtual bool begin(const PlaybackCommand& command, std::uint64_t generation) = 0;
    // take_fact 返回实际状态；播放、暂停、停止均在输出变化后才能报告完成。
    virtual std::optional<PlaybackFact> take_fact() = 0;
    // output_permitted
    // 必须来自实际有效speaker/display整组租约，不由模型参数决定。
    virtual bool output_permitted() const = 0;
    // stop_all 幂等切断本实例所有音视频；true为已确认停止，false为结果未知。
    // 不能停止其他播放器/AI/公共直播源；析构前驱动生命周期必须仍然有效。
    virtual bool stop_all() = 0;
};
// PlaybackAdmission 只表示设备命令接纳情况；Accepted/Pending不是播放成功。
enum class PlaybackAdmission {
    Accepted,
    Pending,
    Replay,
    Conflict,
    Busy,
    Full,
    Invalid,
    Closed,
    Expired,
    Stale,
    StateConflict,
    ResourceBusy
};
// PlaybackSubmission 仅Replay携带原JSON；拒绝分类由上层映射稳定失败应答。
struct PlaybackSubmission {
    PlaybackAdmission admission = PlaybackAdmission::Invalid;  // 默认拒绝非法命令。
    std::string reply;  // 已完成同token请求的原文，不重新执行驱动。
};
// PlaybackEngine 固定一个文件子会话/实例，在同一事件线程提交、轮询和关闭。
// load不自动播放；停止/自然结束/失败不可复活，需另建播放器实例。历史回放另用媒体代次适配。
// 最多32条幂等记录，不驱逐旧token重新执行；宿主保留实例到结束后至少10分钟。
class PlaybackEngine {
public:
    // deadline 是会话单调时钟期限；timeout_ms为单次操作1～30000ms，默认10秒。
    PlaybackEngine(PlaybackDriver& driver, std::string session, std::string playback_id,
                   std::uint64_t deadline, std::uint64_t timeout_ms = 10000);
    ~PlaybackEngine();
    PlaybackEngine(const PlaybackEngine&) = delete;
    PlaybackEngine& operator=(const PlaybackEngine&) = delete;
    // submit 先去重，再校验修订号/水位/生命周期；不在MQTT收包路径调用媒体驱动。
    PlaybackSubmission submit(std::string_view json, std::uint64_t now);
    // poll 至多取4条驱动事实，避免迟到/错误事件洪泛阻塞语音事件循环。
    void poll(std::uint64_t now);
    // take_reply
    // 取实际应答或自然状态事件；发送失败时原token重投能重放已完成应答。
    std::optional<std::string> take_reply();
    // close/断连/撤权由上层调用，幂等停止本播放器；关闭后只允许原token结果重放。
    void close();
    std::uint64_t revision() const { return revision_; }  // 当前实际状态修订号。
    const std::string& state() const { return state_; }   // 最近确认事实；closed后不是当前输出。
    bool closed() const { return closed_; }              // 包含期限、时钟失效或结果未知的安全关闭。
    bool active() const { return active_.has_value(); }  // 是否在等待驱动实际完成。
    // replies_drained 结束后原操作事实和唯一关闭事实均已交付才为true，不能只看实际释放。
    bool replies_drained() const { return !active_ && replies_.empty() && !terminal_reason_; }

private:
    friend class PlaybackStaticFaultTest;  // 仅测试注入缺失关联，不增加生产 API 或对象状态。
    // Record 从接纳到终结保存固定请求；结果一旦生成不能被迟到驱动事实替换。
    struct Record {
        PlaybackCommand command;  // 已严格解析的原请求。
        std::string reply;        // 完成后固定原回执。
    };
    bool observe(std::uint64_t now);                  // 单调时钟倒退/期限结束时切断输出。
    bool valid_fact(const PlaybackFact& fact) const;  // 校验实际位置、倍率及合法转移。
    bool emit(const PlaybackFact& fact, bool spontaneous,
              const char* reason = "decodeFailed");  // 从实际事实编码，不透传原始驱动错误。
    void fail(int code, const char* reason);         // 固定失败，只含稳定脱敏原因。
    void terminate(const char* reason);              // 中止在途命令，终态不能重新接纳。
    void flush_terminal();                    // 背压解除后交付唯一关闭事件，不能因取回执重复终结。
    bool terminal() const;                    // 自然完成/主动停止/失败为实例终态。
    PlaybackDriver& driver_;                  // 生命周期长于本状态机的具体平台驱动。
    std::string session_;                     // 不接受主AI或其他子会话冒用。
    std::string playback_id_;                 // 不跨实例复用请求/驱动事实。
    std::uint64_t deadline_;                  // 单调时钟会话截止。
    std::uint64_t timeout_ms_;                // 包括排队时间的操作超时。
    std::vector<Record> history_;             // 32条有界幂等记录。
    FixedQueue<std::string, 4> replies_;      // 有界输出，背压时不能先执行后丢结果。
    std::optional<std::size_t> active_;       // 当前唯一操作。
    std::optional<std::uint64_t> last_time_;  // 已观察单调时钟。
    std::uint64_t admitted_at_ = 0;           // 在途操作接纳时间。
    std::uint64_t generation_ = 0;            // 已提交驱动的内部代次，不进入设备协议。
    std::uint64_t revision_ = 0;              // 只有合法实际成功/主动状态才推进。
    std::uint64_t event_sequence_ = 0;        // 已接纳平台信令水位。
    std::string state_;                       // 当前实际媒体状态。
    std::string seeking_resume_state_;        // 长seek实际完成后恢复此前状态，不猜测自动播放。
    std::uint64_t seeking_at_ = 0;            // seek进展期限，不因已应答而无限等待。
    std::optional<std::string> terminal_reason_;  // 背压时保留一个固定原因，延后交付失败事件。
    bool started_ = false;                        // 同一操作仅启动驱动一次。
    bool closed_ = false;                         // 安全关闭标志。
};
}  // namespace ur::device
