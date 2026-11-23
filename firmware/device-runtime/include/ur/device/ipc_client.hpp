// IPC设备会话控制器；由唯一设备事件线程驱动，网络、摄像头与WebRTC依赖平台端口。
#pragma once
#include <deque>
#include <map>
#include <memory>

#include "ur/device/ipc_wire.hpp"
#include "ur/device/participant_roster.hpp"
#include "ur/device/requests.hpp"

namespace ur::device {
// IPCMediaPort
// 表示独立预览或对讲Peer，只能分配邀请明确授权的轨，不包含AI取帧权限。
class IPCMediaPort {
public:
    virtual ~IPCMediaPort() = default;
    // begin 必须检查本地隐私及源预算，失败不能开始采集；多个Peer可复用采集源。
    virtual bool begin(const IPCInvite& invite) = 0;
    // apply_answer 仅应用当前Peer的完整SDP，不冒充实际媒体连通。
    virtual bool apply_answer(std::string_view sdp) = 0;
    // connected 返回真实ICE／DTLS状态。
    virtual bool connected() const = 0;
    // begin_recovery 仅重协商原Peer及原轨，不重新申请采集；未适配默认拒绝。
    virtual bool begin_recovery(std::uint64_t) { return false; }
    // control_track
    // 异步控制原邀请轨；speaker仅暂停恢复，实际完成由track_applied回报，未适配驱动拒绝。
    virtual bool control_track(const IPCTrackCommand&) { return false; }
    // apply_quality
    // 只接纳异步质量命令；必须以quality_applied回调真实结果，未适配默认拒绝。
    virtual bool apply_quality(const IPCQualityCommand&) { return false; }
    // close 只释放当前观看者的Peer和源引用，不能停止其它观看者。
    virtual void close() = 0;
};
// IPCMediaFactory 为每个观看者创建独立端口；实际驱动不得进入共享核心。
class IPCMediaFactory {
public:
    virtual ~IPCMediaFactory() = default;
    // supports_talk 只有真实双向音频适配完成才返回true；旧预览工厂默认拒绝对讲。
    virtual bool supports_talk() const { return false; }
    virtual std::unique_ptr<IPCMediaPort> create() = 0;
};
// IPCPhase 区分接听受理、媒体协商、实际连通及平台建立，不把ACK等同可观看。
enum class IPCPhase {
    Invited,
    Accepting,
    Negotiating,
    Connecting,
    WaitingEstablished,
    Active,
    Recovering,
    Ending,
    Ended
};
// IPCClient 管理有界独立观看会话；宿主完成MQTT鉴权和能力ACK后才允许分派下行。
class IPCClient {
public:
    IPCClient(PublishPort& mqtt, IPCMediaFactory& factory, std::string up_topic,
              std::string down_topic, std::string instance, IPCCapability capability);
    ~IPCClient();
    IPCClient(const IPCClient&) = delete;
    IPCClient& operator=(const IPCClient&) = delete;
    // receive 只处理精确已鉴权Topic，wall_ms仅用于邀请期限，now为单调毫秒。
    bool receive(std::string_view topic, std::string_view body, std::uint64_t now,
                 std::uint64_t wall_ms);
    // accept 仅用于accept策略的本地同意；direct仍由真实媒体端口检查隐私。
    bool accept(std::string_view session, std::uint64_t now, std::uint64_t wall_ms);
    // offer_ready 接收当前独立Peer产生的完整SDP，不能转交其它观看者的offer。
    bool offer_ready(std::string_view session, std::string_view sdp, std::uint64_t now,
                     std::uint64_t revision = 1);
    // media_connected 须由驱动事实触发，等待ACK与平台事件后才变Active。
    bool media_connected(std::string_view session, std::uint64_t now, std::uint64_t revision = 1);
    // media_disconnected
    // 只恢复已建立预览的媒体；保持原控制鉴权代次与采集暂停／停止意图。 revision
    // 必须来自当前驱动，首次失联起15秒硬截止，不因重复事件或重试刷新。
    bool media_disconnected(std::string_view session, std::uint64_t revision, std::uint64_t now);
    // track_applied
    // 仅接当前命令驱动事实；state必须匹配请求目标，失败释放本观看或对讲腿。
    bool track_applied(std::string_view session, std::string_view token, std::string_view state,
                       bool success, std::uint64_t now);
    // end 先本地停流再请求平台结束；不影响其它观看者。
    bool end(std::string_view session, std::uint64_t now);
    // tick 执行超时、同正文重试与终态保留，不接受旧回调复活会话。
    void tick(std::uint64_t now, std::uint64_t wall_ms);
    // close_all 用于隐私、断线和宿主退出；立即停止所有本地发布。
    void close_all(std::uint64_t now);
    std::optional<IPCPhase> phase(std::string_view session) const;
    // media_revision 返回本腿当前受理代次；未知腿不投影成首次协商。
    std::optional<std::uint64_t> media_revision(std::string_view session) const;
    std::size_t live_sessions() const;
    // participants 返回已声明且平台已建立的原对讲腿展示；空指针不是完整空名册。
    const std::vector<RosterParticipant>* participants(std::string_view session) const;
    // participant_sequence
    // 返回本腿最后实际提交名册的原水位；零表示还未消费成员事件。
    std::optional<std::uint64_t> participant_sequence(std::string_view session) const;
    // sync 查询当前原腿完整展示，成功发布不代表快照已安装，不恢复媒体或MAC。
    bool sync(std::string_view session, std::uint64_t now, std::uint64_t wall_ms);
    // sync_state 返回原查询的实际关联状态，超时/过期为未知，不终止主对讲。
    RequestState sync_state(std::string_view session) const;
    // media_update
    // 弱网降质入口；setQuality不改变授权轨或传输供应商，冲突由平台409原token应答。
    bool media_update(std::string_view session, std::string_view track_id, std::string_view profile,
                      std::uint64_t now);
    // quality_applied
    // 仅接原驱动完成事实；旧token或终态不复活计划，失败不结束主对讲。
    bool quality_applied(std::string_view session, std::string_view token, std::string_view state,
                         bool success, std::uint64_t now);

private:
    // View 包含当前观看者的全部关联，不借用全局“当前会话”。
    struct View {
        IPCInvite invite;                     // 初始邀请正文及授权轨。
        std::unique_ptr<IPCMediaPort> media;  // 只属于此观看者的Peer。
        IPCPhase phase = IPCPhase::Invited;   // 明确的生命周期阶段。
        std::string pending_method;           // 当前在途业务方法。
        std::string pending_token;            // 必须严格关联的应答token。
        std::uint64_t changed_at = 0;         // 阶段截止依据，不因重投刷新。
        std::uint64_t event_sequence = 0;     // 本会话信令水位。
        bool media_acked = false;             // 连通报告已获受理。
        bool established = false;             // 平台实际建立事件已收到。
        std::uint64_t media_revision = 1;     // 本端已受理的媒体代次，旧驱动事实不可越代次。
        bool recovering = false;              // 与业务建立状态独立，不重建会话或输入段。
        std::uint64_t recovery_started = 0;   // 首次单调失联时刻，全部恢复阶段共用15秒截止。
        ParticipantRoster roster;             // 只读成员事实，不分配轨、不决定IM话权。
        std::uint64_t roster_sequence = 0;    // 仅有效名册推进，不借用其他信令水位。
        std::string sync_token, sync_body,
            sync_reply;                                   // 原请求/完整回执，最多一份待确认查询。
        RequestState sync_state = RequestState::Missing;  // 同步不借用媒体pending_method。
        std::uint64_t sync_started = 0,
                      sync_wall = 0;                   // 原单调起点和墙钟高水位，不随重投刷新。
        unsigned sync_retries = 0;                     // 原字节10/20秒最多两次，不续快照许可。
        std::uint64_t snapshot_revision = 0;           // 只推进完整快照修订，不猜控制子对象修订。
        std::optional<IPCTrackCommand> track_pending;  // 当前异步驱动请求，不能排队无限执行。
        std::uint64_t track_started = 0;               // 十秒驱动截止，重复请求不续期。
        std::map<std::string, std::string> track_states;  // 发布状态与平台观看订阅状态分开。
        std::map<std::string, std::pair<std::string, std::string>>
            track_replies;  // 有界原正文/回执缓存。
        std::deque<std::string>
            track_reply_order;  // 最近32次回执FIFO；更旧控制由事件水位拒绝执行。
        std::uint64_t media_plan_revision = 0;  // 本端已确认媒体计划修订；仅平台受理推进。
        std::string update_token;               // 最近受理的mediaUpdate原token；applied关联后清除。
        std::string update_track;               // 原修订目标轨，实态上报保持同轨。
        std::string update_profile;             // 原质量档位，不能由下行投影替换请求内容。
        std::string update_report_token;        // 实态上报独立关联，用于同正文重试及requestAck。
        bool update_accepted = false;           // 已收到pending且驱动命令至多执行一次。
        bool update_driver_pending = false;     // 实际异步完成前禁止后续质量命令越过旧驱动。
        bool update_reported = false;           // 实际结果或超时事实已经报告，禁止重复推进。
        std::optional<Envelope> update_deferred_report;  // 背压时保留原事实和token，不能改报超时。
        bool update_terminal = false;      // 云端已结束计划；迟到驱动仅释放本地质量占用。
        std::uint64_t update_started = 0;  // 十秒受理截止，超时放弃不猜测平台修订。
    };
    bool send(View& view, Envelope command, std::uint64_t now);
    void finish(View& view, std::uint64_t now);
    // publish_track_state
    // 在驱动实际完成或质量截止后报告原计划，不冒充媒体已经应用。
    bool publish_track_state(View& view, std::string_view state, std::string_view reason,
                             std::uint64_t now);
    IPCMediaFactory& factory_;           // 平台独立Peer工厂。
    PublishPort& mqtt_;                  // 直接发送已执行回执，不登记成新的上行请求。
    Requests requests_;                  // 借用MQTT端口发布并管理有界重试，不重复保存无用端口引用。
    std::string up_, down_, instance_;   // 精确鉴权Topic及进程唯一前缀。
    IPCCapability capability_;           // 当前连接真实能力。
    std::map<std::string, View> views_;  // 最多64条，终态十分钟内保留防重投。
    std::uint64_t serial_ = 0;           // 当前进程单调请求号。
    std::uint64_t wall_high_water_ = 0;  // 宿主墙钟高水位，倒退不能让已过期快照恢复合法。
};
}  // namespace ur::device
