// 文件说明：独立历史档案会话；共享核心负责原请求、实际状态及媒体代次，不读取文件或实时采集。
#pragma once
#include <memory>
#include <vector>
#include "ur/device/fixed_queue.hpp"
#include "ur/device/record_playback_wire.hpp"
#include "ur/device/requests.hpp"

namespace ur::device {
// RecordPreparation 是可信清单／源预算预检，不表示实际档案已加载或Peer已建立。
enum class RecordPreparation { Ready, Unsupported, Denied, Unavailable };
// RecordPlaybackFact 只能由原平台驱动交付；两个代次分别关联操作和实际媒体，不能替代授权。
struct RecordPlaybackFact {
    std::uint64_t generation = 0;      // 原内部操作代次，不进入设备协议，不由平台指定。
    std::uint64_t media_revision = 0;  // 当前或原seek唯一预留的媒体代次。
    std::string state;                 // 实际loaded/playing/paused/seeking/stopped/ended/failed。
    std::uint64_t position_ms = 0;     // 相对原clip的真实解码位置，不回显seek目标。
    std::optional<std::uint64_t> duration_ms;  // 真实clip时长；首次load必须确认完整档案。
    int code = 200;                            // 原操作实际成功或400～599；失败细节不直接透传。
    bool spontaneous = false;                  // 自主EOF或新Peer建立后恢复原意图，不冒原命令应答。
};
// RecordPlaybackPort 统一所有档案读取、编码和Peer所有权；所有调用由唯一设备事件线程发起。
// 平台端通过有界事实队列交回实际结果；不能调用getUserMedia或借实时IPC／本地输出焦点权限。
class RecordPlaybackPort {
public:
    virtual ~RecordPlaybackPort() = default;
    // prepare 检查同一camera/id/version/ref可信清单、整份clip和1x并预约源；不开始读取或播放。
    virtual RecordPreparation prepare(const RecordPlaybackInvite& binding,
                                      std::uint64_t deadline) = 0;
    // begin true仅受理；false保证无副作用。seek原命令带旧m，next_revision仅原命令预留的m+1。
    virtual bool begin(const PlaybackCommand& command, std::uint64_t generation,
                       std::uint64_t next_revision) = 0;
    // take_fact 返回实际状态；不得以控制入队／目标位置冒充完成。
    virtual std::optional<RecordPlaybackFact> take_fact() = 0;
    // begin_peer 仅创建指定m的新Peer，不开始档案播放；同实例旧Peer实际停止后才调用。
    virtual bool begin_peer(std::uint64_t media_revision) = 0;
    // apply_answer 只应用当前Peer原完整SDP，不表示实际连通。
    virtual bool apply_answer(std::uint64_t media_revision, std::string_view sdp) = 0;
    // connected 读取当前m真实ICE／DTLS事实；不能由answer成功置真。
    virtual bool connected(std::uint64_t media_revision) const = 0;
    // close_peer 立即切断本代次发布；命令入队不表示实际停止。
    virtual void close_peer(std::uint64_t media_revision) = 0;
    // peer_stopped 必须覆盖本代次Peer及编码／发布回调，不能只检查C++对象析构。
    virtual bool peer_stopped(std::uint64_t media_revision) const = 0;
    // restore_intent 原seek新Peer实际建立后恢复loaded/paused/playing；必须另回实际自主事实。
    virtual bool restore_intent(std::uint64_t media_revision, std::string_view state,
                                std::uint64_t generation) = 0;
    // publishing_permitted 结合原档案授权和当前m实际媒体许可；不借speaker/display焦点授权。
    virtual bool publishing_permitted(std::uint64_t media_revision) const = 0;
    // stop_all 幂等切断本实例所有代次；reader/encoder/Peer/队列/回调实际退出后才返回true。
    virtual bool stop_all() = 0;
};
// RecordPlaybackPhase 将接收ACK、加载、媒体协商、实际建立和seek分别表示。
enum class RecordPlaybackPhase {
    Prepared,
    Accepting,
    Rejecting,
    Ready,
    Negotiating,
    Active,
    Seeking,
    Ended
};
// RecordPlaybackClient 固定一个有限历史会话；同一MQTT宿主可为新的独立邀请创建另一个受限实例。
// 每实例32条原控制和4条待发布回执，满时拒绝，不驱逐原token重新执行未知副作用。
class RecordPlaybackClient {
public:
    // deadline是宿主提供的独立单调授权截止；wall仅判断原邀请接收期限，不延长播放授权。
    RecordPlaybackClient(PublishPort& mqtt, RecordPlaybackPort& port, RecordPlaybackInvite binding,
                         std::string up, std::string down, std::uint64_t deadline);
    ~RecordPlaybackClient();
    RecordPlaybackClient(const RecordPlaybackClient&) = delete;
    RecordPlaybackClient& operator=(const RecordPlaybackClient&) = delete;
    // receive 精确已鉴权Topic；解析后仍校验原session/token/水位/版本/ref及当前代次。
    bool receive(std::string_view topic, std::string_view body, std::uint64_t now,
                 std::uint64_t wall);
    // offer_ready 仅转发当前原Peer真实完整offer；旧m和第二不同offer拒绝。
    bool offer_ready(std::uint64_t media_revision, std::string_view sdp, std::uint64_t now);
    // media_connected 须同时有当前answer与真实ICE/DTLS；等待ACK和平台建立后才开放play。
    bool media_connected(std::uint64_t media_revision, std::uint64_t now);
    // tick 有界轮询原驱动事实、超时、停止与背压；关闭后仍交原事实和整组释放。
    void tick(std::uint64_t now);
    // close 用于断线／撤权／退出，逻辑终态后不能恢复；不假造原平台终态token或released。
    void close(std::uint64_t now);
    RecordPlaybackPhase phase() const { return phase_; }  // 逻辑阶段，不是媒体完成。
    const std::string& state() const { return state_; }   // 最近实际状态，Ended后不是当前许可。
    std::uint64_t revision() const { return revision_; }  // 实际播放器水位。
    std::uint64_t media_revision() const { return media_revision_; }  // 当前独立Peer代次。
    bool released() const { return released_; }                       // 整个实例所有代次实际退出。
    bool release_submitted() const { return released_ && release_published_; }  // 仅本地发布接纳。

private:
    friend class RecordPlaybackStaticFaultTest;  // 仅测试缺失在途关联，不开放宿主调用端口。
    // Operation 只保留原解析命令和原结果，不含驱动或恢复授权。
    struct Operation {
        PlaybackCommand command;  // 规范化原字段，token复用改参严格冲突。
        std::string reply;        // 一次实际结果的固定原字节。
    };
    bool observe(std::uint64_t now);                 // 单调倒退或授权截止关闭，原重放不刷新截止。
    bool send(Envelope command, std::uint64_t now);  // 复用Requests的原字节重试与关联。
    bool submit_control(std::string_view body, std::uint64_t now);  // 原token去重先于状态前置。
    bool begin_current_peer(std::uint64_t now);      // 旧Peer真正停止后才创建唯一下一Peer。
    bool emit_fact(const RecordPlaybackFact& fact);  // 成功／自主状态从实际事实编码并推进revision。
    void fail_active(int code, const char* reason);  // 固定失败ACK仅reason，不虚构Actual。
    void finish(std::uint64_t now);                  // 取消协商、切断全组，原控制历史仍保留。
    void flush();                                    // 按原事实顺序发布；背压不改字节、不换token。
    void publish_release();               // 原sessionEnded且全组停止与原事实交完才发布严格释放。
    void activate(std::uint64_t now);     // ACK、当前m平台事件与真实连通三者缺一不恢复意图。
    PublishPort& mqtt_;                   // 只借用宿主同一鉴权连接，不连接或订阅第二身份。
    RecordPlaybackPort& port_;            // 平台统一档案与媒体所有者，生命周期长于本对象。
    RecordPlaybackInvite binding_;        // 不可变原档案／版本／clip／引用。
    Requests requests_;                   // 只登记sessionAccept／Reject和当前offer／mediaState。
    std::string up_, down_;               // 宿主固定Topic，不从报文取设备身份。
    std::uint64_t deadline_;              // 独立单调授权截止。
    std::vector<Operation> history_;      // 最多32条，不在嵌入式栈放大数组。
    FixedQueue<std::string, 4> replies_;  // 有界原事实发布队列。
    std::optional<std::size_t> active_;   // 唯一在途真实播放器操作。
    std::optional<RecordPlaybackFact> seek_fact_;  // 旧Peer未实际停止时保留原seeking事实。
    std::optional<std::uint64_t> clock_;           // 单调时间高水位。
    std::string accept_method_;                    // 原接收或明确拒绝，不能将拒绝ACK当接受。
    std::string reject_reason_;                    // 原标准拒绝原因，ACK须原样关联不能变成接收。
    std::string offer_token_, state_token_, offer_body_;  // 当前协商原关联，换m不改旧历史。
    std::string state_, previous_intent_;      // 最近实际状态与seek前固定暂停／播放意图。
    std::string terminal_token_;               // 仅合法平台sessionEnded授予释放关联。
    std::optional<std::string> release_fact_;  // 原终态固定字节，重试保持不变。
    std::uint64_t revision_ = 0;               // 初始0，仅实际事实推进。
    std::uint64_t media_revision_ = 1;         // 首轮固定1，只有原seek可保留m+1。
    std::uint64_t old_revision_ = 0;           // seek等待实际Stopped的唯一旧代次。
    std::uint64_t sequence_ = 0, generation_ = 0, serial_ = 0;  // 三个独立水位不相互替代。
    std::uint64_t changed_at_ = 0;                              // 原协商预算起点，不因重投刷新。
    std::uint64_t operation_at_ = 0;                            // 唯一操作／长seek预算起点。
    std::uint64_t invite_deadline_ = 0;  // 原邀请剩余墙钟预算转单调期限，重投不能延长。
    RecordPlaybackPhase phase_ = RecordPlaybackPhase::Prepared;  // 构造不授许可。
    bool peer_started_ = false, answered_ = false;    // 当前真实Peer是否创建及应用原answer。
    bool media_acked_ = false, established_ = false;  // 平台受理和建立分开。
    bool restoring_ = false;                          // 新Peer建立后等待驱动实际恢复原意图。
    bool started_ = false;                            // 当前唯一操作已交原驱动，重投不能重复开始。
    bool released_ = false, release_published_ = false;  // 逻辑终态、实际释放和发布接纳分开。
};
}  // namespace ur::device
