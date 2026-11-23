// 文件说明：在共享鉴权MQTT连接中分派文件子会话；内容和输出准入仍由平台适配提供。
#pragma once
#include <array>
#include "ur/device/playback_session.hpp"

namespace ur::device {
// PlaybackGrant 绑定独立内容/焦点准入结果；截止是单调时间，不能取邀请期限替代。
struct PlaybackGrant {
    std::unique_ptr<PlaybackDriver> driver;  // 固定实例驱动，端点拥有直到实际释放。
    std::uint64_t deadline = 0;              // 内容与整组输出许可的共同最早截止。
    bool pending = false;                    // 准备尚未就绪；须为空driver并给独立许可截止。
};
// PlaybackAdmissionPort 由内容解析和焦点适配实现；核心不解释URL或默认授权。
class PlaybackAdmissionPort {
public:
    virtual ~PlaybackAdmissionPort() = default;
    // authorize 严格邀请通过后非阻塞准备/轮询同实例；空结果拒绝，不在回调内下载或采集。
    virtual PlaybackGrant authorize(const PlaybackInvite& invite, std::uint64_t now) = 0;
    // cancel 撤销尚未交付的固定准备实例；true须为实际释放，不得以请求受理替代。
    virtual bool cancel(std::string_view session, std::uint64_t now) = 0;
};
// PlaybackEndpoint 可挂到AI宿主的同一事件线程和MQTT连接，不另发能力快照覆盖AI能力。
// 一次仅一组实际输出；四个工作槽，完整结束可转为有界十分钟墓碑，旧sessionId不复用。
class PlaybackEndpoint {
public:
    PlaybackEndpoint(PublishPort& mqtt, PlaybackAdmissionPort& admission,
                     PlaybackCapability capability, std::string up, std::string down);
    ~PlaybackEndpoint();
    PlaybackEndpoint(const PlaybackEndpoint&) = delete;
    PlaybackEndpoint& operator=(const PlaybackEndpoint&) = delete;
    // receive 只消费精确文件子会话；false须由宿主按未处理/非法报文处置，不路由到AI状态机。
    bool receive(std::string_view topic, std::string_view body, std::uint64_t now,
                 std::uint64_t wall);
    // tick 推进已有实例和释放背压；平台驱动的媒体事实须先由宿主泵入。
    void tick(std::uint64_t now, std::uint64_t wall);
    // close 只切断文件实例，随后仍需tick确认实际释放；不关闭共享MQTT或AI输入。
    void close(std::uint64_t now);
    // owns 仅识别保留的固定子会话，不将未知会话路由进AI或授权新实例。
    bool owns(std::string_view session) const;
    // established 仅原接听ACK与平台建立事件均已收录且仍Active；不是实际播放证明。
    bool established(std::string_view session) const;
    // released 独立确认整组实际资源已释放，不以逻辑Ended或拒绝报文作为成功。
    bool released() const;
    // settled 当前保留实例均收到原平台终态、实际释放且提交事实；不是远端持久收录证明。
    bool settled() const;
    // begin_connection 只在关闭后且旧输出实际释放时开始新代次；保留原实例和终态关联。
    bool begin_connection(std::uint64_t now);

private:
    // Slot 将驱动声明在会话之前，保证会话先析构；拒绝也保留固定原文防止重准入。
    struct Slot {
        std::string session_id;                    // 原固定子会话，不能与AI主会话混用。
        std::string invitation;                    // 规范化原邀请，重复不得替换资源。
        std::string rejection;                     // 原拒绝字节，重复只重放。
        std::unique_ptr<PlaybackDriver> driver;    // 被session借用，始终最后释放。
        std::unique_ptr<PlaybackSession> session;  // 复用原协议状态机，不复制播放执行。
        std::optional<std::uint64_t> closed_at;    // 实际释放/拒绝后的本地保留起点。
        std::uint64_t source_deadline = 0;         // 首次独立许可上限，轮询不能延长。
        std::uint64_t prepare_until = 0;           // 原邀请墙钟剩余量换算单调截止，不刷新。
        bool preparing = false;                    // 尚未发送sessionAccept，重复只保留原邀请。
        bool canceling = false;                    // 准备/无效交付正在等待实际资源释放。
        bool rejection_pending = false;            // 本地发布背压保留同一拒绝字节。
        std::string terminal_token;                // 首个严格平台终态关联，不随重复终态替换。
        std::uint64_t terminal_sequence = 0;       // 原终态事件水位，仅用于独立释放事实。
        std::string release_fact;                  // 实际关闭后的固定字节，发布背压不重构关联。
        bool release_published = false;            // 本地发布端口已接纳，不代表平台已持久化。
    };
    // Retired 是无驱动/无许可的只读终态墓碑，不允许旧会话重接听或控制播放器。
    struct Retired {
        std::string session, invitation;  // 固定原邀请和身份，迟到报文不得覆盖或重新准入。
        std::string release;              // 原平台终态关联的已交付整组释放事实。
        std::uint64_t closed_at = 0;      // 原实际关闭起点，重放不能延长十分钟保留。
        std::size_t bytes = 0;            // 记录对象/字符串预算，清理时精确扣除。
    };
    // retire_slot 仅完整结束/实际释放/原事实交付后回收工作槽，预算满仍保留原资源关联。
    Slot* retire_slot();
    bool reject(const PlaybackInvite& invite, std::string_view reason, Slot* slot);
    // resolve 只推进原实例的非阻塞准入；许可不能增长，接听严格等待实际就绪。
    bool resolve(Slot& slot, PlaybackInvite invite, PlaybackGrant grant, std::uint64_t now,
                 std::uint64_t wall);
    // cancel_preparation 保留准备资源直到实际释放，不把关闭请求或时间耗尽当释放事实。
    void cancel_preparation(Slot& slot, std::uint64_t now);
    // publish_preparation_release 仅在准备资源实际关闭后上报原终态的独立释放事实。
    void publish_preparation_release(Slot& slot);
    PublishPort& mqtt_;                   // 共享已鉴权发布端口，不负责连接和能力上报。
    PlaybackAdmissionPort& admission_;    // 显式可信准入适配，生命周期长于端点。
    PlaybackCapability capability_;       // 已合并进设备全量能力的真实文件输出。
    std::string up_, down_;               // 宿主固定的精确设备Topic。
    std::array<Slot, 4> slots_;           // 有界近期实例，满时背压而非提前淘汰旧事实。
    std::vector<Retired> retired_;        // 堆上按需保存，最多128条/64KiB，不保留可执行播放器。
    std::size_t retired_bytes_ = 0;       // 墓碑对象/字符串容量预算，不计分配器和容器空余槽。
    std::optional<std::uint64_t> clock_;  // 单调时间回退后只能收敛释放。
    bool closing_ = false;                // 关闭/断连后不得新准入或恢复资源。
};
}  // namespace ur::device
