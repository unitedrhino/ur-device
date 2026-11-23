// 文件说明：实时节目独立接收核心；原会话/请求由C++持有，端口负责真实解码、可见输出和全部释放。
#pragma once
#include <array>
#include <memory>
#include <vector>
#include "ur/device/fixed_queue.hpp"
#include "ur/device/live_wire.hpp"
#include "ur/device/requests.hpp"

namespace ur::device {
// LivePreparation 仅表示原完整轨组预算与实际格式预检，不等同焦点、Peer或媒体已播放。
enum class LivePreparation { Ready, Unsupported, Denied, Busy };
// LiveMediaFailure 仅表达驱动已确认的有限失败分类，不允许自由错误文本进入协议。
enum class LiveMediaFailure { MediaFailed, OutputBusy };
// LiveMediaFact 由原驱动交付实际单轨操作结果，不能由云端trackState或Connected构造。
struct LiveMediaFact {
    std::uint64_t generation = 0;      // 本地原操作代次，不能成为平台授信或协议字段。
    std::uint64_t media_revision = 0;  // 原接收Peer媒体代次，首轮固定1。
    std::string track_id;              // 原操作轨，不能扩展源或借另一轨结果确认。
    std::string state;                 // 实际active/paused/stopped，非意图或仅decoder-ready。
    int code = 200;                    // 原操作成功或400～599；自由错误不进入协议。
    LiveMediaFailure failure = LiveMediaFailure::MediaFailed;  // 失败事实的固定分类。
};
// LiveMediaPort 独立拥有节目接收器；不采集、不访问自由URL、不复用IPC/file/record权限。
// 唯一事件线程调用；actual事实异步、有界交回。audio/video独立操作，焦点许可沿原输出组核验。
class LiveMediaPort {
public:
    virtual ~LiveMediaPort() = default;
    // prepare 预约完整原组并检验接收/输出能力，不开始播放或宣称focus已持有。
    virtual LivePreparation prepare(const LiveInvite& invite, std::uint64_t deadline) = 0;
    // begin_peer 首代次1只协商；初始抑制输出，无麦克风/摄像头或自由源读取。
    virtual bool begin_peer() = 0;
    // apply_answer 应用原offer完整答复，不把成功设置为Connected。
    virtual bool apply_answer(std::string_view sdp) = 0;
    // connected 只读取本Peer当前真实ICE/DTLS事实，与平台Established及actual输出分开。
    virtual bool connected() const = 0;
    // authorization_valid 核原节目/父业务/有限期限；临时focus mask不是节目源撤权。
    virtual bool authorization_valid() const = 0;
    // begin_track false保证无副作用；true仅受理。resume须清旧队列，从当前源新帧独立恢复。
    // audio需真实非静音PCM且扬声器门允许输出；video需当前新IDR解码且可见呈现后回active。
    virtual bool begin_track(const LiveTrackControl& command, std::uint64_t generation) = 0;
    // take_fact 提交原操作真实完成；不以入队、接收RTP、解码隐藏画面或目标state冒充完成。
    virtual std::optional<LiveMediaFact> take_fact() = 0;
    // presentation_permitted 只核原完整输出组当前focus及本轨末端mask，不替代节目授权。
    // mask关闭须驱动实际关闭末端并取消原pending resume，交原generation的OutputBusy失败；
    // 恢复mask不自动新resume或交active。既有actual仅是历史事实，不能据此重开输出。
    virtual bool presentation_permitted(std::string_view track_id) const = 0;
    // stop_all 幂等切断整组；所有轨/Peer/PCM队列/输出和异步回调实际退出后才为true。
    virtual bool stop_all() = 0;
};
// LiveMediaFactory 为宿主创建独立接收资源所有者；不在共享核心依赖浏览器或具体芯片。
class LiveMediaFactory {
public:
    virtual ~LiveMediaFactory() = default;
    // create nullptr表示无法提供真实独立端口，不允许回退为相机/采集或假媒体。
    virtual std::unique_ptr<LiveMediaPort> create(const LiveInvite& invite) = 0;
};
// LivePhase 区分原准入、网络协商、平台会话建立和结束；Established仍不是实际播放。
enum class LivePhase { Prepared, Accepting, Rejecting, Negotiating, Established, Ended };
// LiveClient 每实例固定原完整接收轨组；32原控制历史/4原回执有界，两个独立轨可并行等待事实。
// 不编辑现行AI单双工策略；节目input恒关闭，output仅按原单轨控制与真实输出许可开放。
class LiveClient {
public:
    // deadline是宿主给定单调原授权截止；wall仅判邀请准入，重复请求不能延长两种期限。
    LiveClient(PublishPort& mqtt, LiveMediaPort& port, LiveInvite binding, std::string up,
               std::string down, std::uint64_t deadline);
    ~LiveClient();
    LiveClient(const LiveClient&) = delete;
    LiveClient& operator=(const LiveClient&) = delete;
    // receive 只接原鉴权SIG下行；云端trackState绝不转成驱动actual输出事实。
    bool receive(std::string_view topic, std::string_view body, std::uint64_t now,
                 std::uint64_t wall);
    // offer_ready 只发布当前原Peer实际完整候选offer；原重复同字节可重放，不另建Peer。
    bool offer_ready(std::string_view sdp, std::uint64_t now);
    // media_connected 须原answer已应用且真实ICE/DTLS成立；尚不授输出或造active。
    bool media_connected(std::uint64_t now);
    // tick 有界消费实际事实、超时/撤权及发布背压；终态后只补原结果，不复活原轨。
    void tick(std::uint64_t now);
    // end 是明确本地结束意图，发送原sessionEnd；发布接纳与平台终态、实际释放分开。
    bool end(std::string_view reason, std::uint64_t now);
    // close 断线/本地撤权立即切全组；不伪造云终态或新增released方法。
    void close(std::uint64_t now);
    LivePhase phase() const { return phase_; }   // 逻辑阶段，不代表实际输出。
    bool input_open() const { return false; }    // 接收节目没有任何采集入口。
    bool released() const { return released_; }  // 所有原资源实际退出，不是结束命令入队。
    // actual_state 返回最近原实际状态；未有事实时为空，不表示当前授权/末端mask或自动恢复意图。
    std::string_view actual_state(std::string_view track_id) const;

private:
    // Operation 保存紧凑原命令/原actual或unknown回执，不保存可运行端口或重建授权。
    struct Operation {
        LiveTrackControl command;      // 原token/session/seq/规范化字段。
        std::string reply;             // 一次确定的原字节，重投不重做实际控制。
        std::uint64_t generation = 0;  // 原本地操作唯一代次。
        std::uint64_t started = 0;     // 原单调预算起点，不被重复请求刷新。
    };
    bool observe(std::uint64_t now);                 // 时钟倒退/原授权截止结束，不能借重试恢复。
    bool send(Envelope command, std::uint64_t now);  // 原请求注册及规范化字节重发。
    bool control(std::string_view body, std::uint64_t now);  // 先原历史去重再核当前权限。
    bool receipt(std::size_t operation, int code, std::string_view state,
                 std::string_view reason);  // 只从原驱动事实或明确失败生成ACK。
    void activate();                        // 当前answer/ICE/mediaACK/Established完整屏障。
    void flush();                           // 固定原字节按序交付，背压不改变关联。
    std::size_t pending_count() const;      // 与队列预算共同预留最多四个原回执槽。
    std::optional<std::size_t> track_index(std::string_view track_id) const;  // 原组两轨定位。
    PublishPort& mqtt_;                   // 借原同一鉴权连接，不持第二个设备身份。
    LiveMediaPort& port_;                 // 原端口所有者须比本对象活得久，宿主等待actual释放。
    LiveInvite binding_;                  // 原不可变五键邀请及完整接收组。
    Requests requests_;                   // 接听/拒绝、offer、网络状态和明确结束原关联。
    std::string up_, down_;               // 从宿主鉴权Topic获得，绝不信消息体自报身份。
    std::uint64_t deadline_;              // 独立授权截止，重放不延期。
    std::vector<Operation> history_;      // 最多32条，堆上按需分配，不放大嵌入式栈。
    FixedQueue<std::string, 4> replies_;  // 四条已确定原回执，等待发布接纳。
    std::array<std::optional<std::size_t>, 2> pending_{};  // 原audio/video独立实际操作。
    std::array<std::string, 2> actual_{};                  // 最近实际状态，结束后不代表当前权限。
    std::optional<std::uint64_t> clock_;                   // 单调高水位，倒退只结束不清历史。
    std::string admission_method_, rejection_reason_;      // 原准入决策及标准原因。
    std::string offer_token_, offer_payload_, media_token_, end_token_,
        end_reason_;                                            // 原请求关联。
    std::uint64_t sequence_ = 0, generation_ = 0, serial_ = 0;  // 三个独立水位。
    std::uint64_t started_at_ = 0, admission_deadline_ = 0;     // 原协商与准入预算。
    LivePhase phase_ = LivePhase::Prepared;                     // 构造不开放媒体或输出。
    bool peer_started_ = false, answered_ = false;              // 端口受理与实际网络事实分开。
    bool media_acked_ = false, established_ = false;            // 原网络报告ACK与平台事件分开。
    bool released_ = false;  // 本端actual完整退出，不能据此推断后端已收录。
};
}  // namespace ur::device
