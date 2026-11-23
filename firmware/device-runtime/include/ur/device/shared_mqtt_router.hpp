// 文件说明：单事件线程的唯一MQTT宿主借用路由；请求归属与会话绑定分离，不管理网络、媒体或授权。
#pragma once
#include <array>
#include <optional>
#include <vector>
#include "ur/device/envelope.hpp"
#include "ur/device/mqtt_port.hpp"

namespace ur::device {
// SharedMqttOwner 为实际业务状态机划分收发归属，不能从token前缀推断。
enum class SharedMqttOwner { AI, Human, Global, Invalid };
// SharedRouteResult 的Full表示原消息未被接纳，唯一宿主应保留原帧而非广播或换token。
enum class SharedRouteResult { Routed, Waiting, Rejected, Full, Closed };
// SharedUnroutableReason 保留不能安全分派的原消息；不把等待过期解释为业务失败。
enum class SharedUnroutableReason { Expired, Retired, Closed };
// SharedUnroutable 保存实际原帧供宿主明确处理，不包含生成的业务成功或替代事件。
struct SharedUnroutable {
    MqttEvent message;  // 原Topic与正文，只允许交回已鉴权唯一宿主，不用于日志。
    SharedUnroutableReason reason = SharedUnroutableReason::Expired;  // 本地分派不能确认的原因。
};
// SharedSessionBinding 把原来源token与新腿明确具名，避免同类型相邻实参误交换。
struct SharedSessionBinding {
    std::string_view origin_token;  // 原创建请求或原已交付邀请token，不生成替代关联。
    std::string_view session;       // 原状态机实际接受的新腿，不是会议组。
};
// SharedMqttRouter
// 的identity必须来自原认证配置，格式product/device；所有公开操作在同一事件线程调用。
// 宿主先tick更新原单调时钟，再调用业务；只有业务状态机实际接受后才confirm_session。
// 本类不调用connect/subscribe/poll/take_event/disconnect，不声明能力、媒体连通或焦点取得。
class SharedMqttRouter {
public:
    // 构造仅绑定借用端口/身份；receipt仅由本机实际能力快照选择，默认关闭，不产生安装许可。
    SharedMqttRouter(PublishPort& sole_publisher, std::string identity, bool receipt = false);
    SharedMqttRouter(const SharedMqttRouter&) = delete;
    SharedMqttRouter& operator=(const SharedMqttRouter&) = delete;
    // port 返回固定业务发布端口；所有网络所有权方法明确拒绝或不操作原连接。
    MqttPort& port(SharedMqttOwner owner);
    // tick 使用原宿主单调毫秒，时钟倒退永久关闭；不重试业务、不推进实际网络。
    bool tick(std::uint64_t now);
    // expire_private_context 接原可信墙钟；到期或校时失效只清私有副本，不结束主通话。
    void expire_private_context(std::uint64_t wall_ms);
    // private_context_bytes 只报告本路由仍保留的私有字符串逻辑字节，不输出正文或身份。
    std::size_t private_context_bytes() const;
    // ingest 只接纳原已鉴权精确下行Topic的原帧；64KiB上限独立于JSON语义校验。
    SharedRouteResult ingest(std::string_view topic, std::string_view body, std::uint64_t now);
    // take_message 取本业务已归属的原帧，不向其他业务重复投递。
    std::optional<MqttEvent> take_message(SharedMqttOwner owner);
    // confirm_session 必须在业务实际接受原创建应答或邀请后调用；仅交付过的原来源可绑定。
    SharedRouteResult confirm_session(SharedMqttOwner owner, SharedSessionBinding binding,
                                      std::uint64_t now);
    // retire_session 由原业务终态调用；十分钟墓碑阻断迟到事件和跨业务复用旧腿。
    bool retire_session(SharedMqttOwner owner, std::string_view session, std::uint64_t now);
    // take_unroutable 明确取出过期/退休/断连不能消费的原消息，不静默丢消息。
    std::optional<SharedUnroutable> take_unroutable();
    // close 为实际宿主断连终态；仅退休路由并保留未消费原消息，不操作原网络。
    void close();
    // closed 仅表示本路由不可继续分派，不证明任何实际连接或媒体已经释放。
    bool closed() const { return closed_; }

private:
    friend class ReceiptRouterTest;  // 专用合同fixture仅核私有来源关联；不公开生产调用端口。
    friend class SharedHandoffHost;  // 唯一宿主登记已取出的AI原Start；网络/页面无此关联入口。
    // relate_reply_start只登记原已交付字节，不授予安装；满预算拒本Start且不占接收retained槽。
    bool relate_reply_start(std::string_view topic, std::string_view body, std::uint64_t now);
    // BorrowedPort 只允许原业务的精确上行发布；不提供伪网络成功。
    class BorrowedPort final : public MqttPort {
    public:
        BorrowedPort(SharedMqttRouter& router, SharedMqttOwner owner)
            : router_(router), owner_(owner) {}
        bool publish(std::string_view topic, std::string_view body) override;
        bool connect(const MqttConfig&) override { return false; }
        int subscribe(std::string_view) override { return 0; }
        bool poll() override { return false; }
        std::optional<MqttEvent> take_event() override { return std::nullopt; }
        bool publications_drained() override { return false; }
        void disconnect() override {}  // 借用者不得释放唯一宿主网络。
    private:
        SharedMqttRouter& router_;  // 原宿主路由，生命周期长于业务借用。
        SharedMqttOwner owner_;     // 本端口固定归属，不能在publish时改变。
    };
    // Source区分请求/邀请/焦点/动作/私有Context与选定UDP Start，不合并同token的不同方法。
    enum class Source { Request, Invite, Focus, Action, Context, ReplyStart };
    // Record 保存原字节关联；本地publish背压仍保留归属，首次时间不随重试刷新。
    struct Record {
        SharedMqttOwner owner = SharedMqttOwner::Global;  // 已登记原归属。
        Source source = Source::Request;                  // 请求方向/类别，不由前缀猜测。
        std::string topic;                                // 原来源完整精确Topic。
        std::string body;                      // 原不可变字节，同归属重投仍须逐字节相同。
        Envelope envelope;                     // 已通过公共严格解码的关联。
        std::string reply;                     // 首个应答的规范化字节，拒绝冲突重投。
        std::string candidate;                 // 成功sessionCreated原新腿或原邀请腿，尚未绑定。
        std::uint64_t started = 0;             // 首次原单调时钟，不续期。
        std::optional<std::uint64_t> retired;  // 本地终态起始，非远端撤销证明。
        bool delivered = false;                // 原来源已交给业务，仍非业务接受或执行证明。
        std::uint64_t private_expires_at = 0;  // Context原授权截止；不能由重投刷新。
    };
    // Scope 的退休腿仍占容量，避免提前淘汰迟到保护；最大16条不表示IM准入上限。
    struct Scope {
        SharedMqttOwner owner = SharedMqttOwner::Global;  // 真实已确认业务归属。
        std::string session;                              // 原腿，不是会议group或媒体track。
        std::optional<std::uint64_t> retired;             // 原终态墓碑时间。
    };
    // Queued 保存原帧；owner空表示等待显式绑定，reason有值表示明确不可消费。
    struct Queued {
        MqttEvent message;                             // 原Topic/正文，不修改内容投影。
        std::string session;                           // 仅用于精确腿归属与退休。
        std::optional<SharedMqttOwner> owner;          // 归属未确认时不得广播。
        std::optional<SharedUnroutableReason> reason;  // 终态后原消息仍可明确取回。
        std::uint64_t started = 0;                     // 早到等待固定5秒，重投不续期。
        std::uint64_t private_expires_at =
            0;  // 非零只表示私有正文，到期直接释放，不交回unroutable。
    };
    // publish 登记不可变关联之后才借用原适配器，不将本地接纳当远端ACK。
    bool publish(SharedMqttOwner owner, std::string_view topic, std::string_view body);
    // publish_context_ack 仅允许原已交付Human私有消息的固定确认，不放宽通用请求发布规则。
    bool publish_context_ack(SharedMqttOwner owner, std::string_view body);
    // enqueue 对队列及总保留字节同时背压；返回Full不改变来源关联或原帧。
    SharedRouteResult enqueue(std::string_view topic, std::string_view body,
                              std::string_view session, std::optional<SharedMqttOwner> owner,
                              std::uint64_t private_expires_at = 0);
    // clear_private_record 删除原私有正文及回执，保留精确关联墓碑以拒绝迟到重授权。
    void clear_private_record(Record& value);
    // retained_bytes 计算实际保留字符串，避免只限制条数而被大正文耗尽内存。
    std::size_t retained_bytes() const;
    // scope 查找原腿，包括退休墓碑；不依赖当前正在播放的会话。
    Scope* scope(std::string_view session);
    // record 查找完整来源Topic/method/token，不合并异通道同token。
    Record* record(Source source, std::string_view topic, std::string_view method,
                   std::string_view token);
    // register_record 限512条与256KiB保留；重复原来源保持首次时间。
    Record* register_record(Record value);
    PublishPort& sole_publisher_;        // 唯一原宿主发布适配器，不借用网络所有权。
    std::string identity_;               // 仅内存原鉴权身份，不输出。
    std::array<std::string, 3> up_;      // SIG/MSG/action精确上行。
    std::array<std::string, 3> down_;    // SIG/MSG/action精确下行。
    std::array<BorrowedPort, 4> ports_;  // AI/Human/Global及永久拒绝的非法枚举端口，不借到Global。
    std::vector<Record> records_;        // 有界原请求与平台来源墓碑。
    std::vector<Scope> scopes_;          // 有界实际业务明确确认的腿。
    std::vector<Queued> queue_;          // 各owner32、等待16、总112帧；未消费原帧保留。
    std::uint64_t now_ = 0;              // 仅原宿主单调时钟。
    bool has_time_ = false;              // 未tick前拒绝登记，禁止用设备正文时间推断。
    bool closed_ = false;                // 倒退或断连终态，不隐式重新打开。
    bool publishing_ = false;            // 原发布适配器不得同步重入路由。
    std::uint64_t private_wall_highwater_ = 0;  // 仅可信宿主墙钟，倒退不能复活原授权。
    bool private_clock_valid_ = false;          // 未校时或回退拒绝私有接收/确认，不影响普通通话。
    bool reply_receipt_enabled_ = false;  // 本机固定能力选择；原selected与业务安装仍分别核验。
    // reply_receipt_selected只读取已交付创建应答的明确选择，不给旧selected或Human新增记录。
    bool reply_receipt_selected(std::string_view session) const;
    // publish_reply_receipt仅转交原已交付AI Start精确200，返回原发布受理，不授予焦点或播放许可。
    bool publish_reply_receipt(SharedMqttOwner owner, std::string_view topic,
                               std::string_view body);
};
}  // namespace ur::device
