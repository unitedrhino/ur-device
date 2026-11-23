// 文件说明：统一AI／人工宿主的真实单连接入口；精确SUBACK及原统一能力ACK齐全后才允许业务收包。
#pragma once
#include <array>
#include "ur/device/focus_engine.hpp"
#include "ur/device/shared_mqtt_router.hpp"
#include "ur/device/voice_wire.hpp"

namespace ur::device {
// handoff_capability_command
// 从实际完整组首次idle证明构造快照；human_only仅声明人工语音与被动文本输出，默认AI/Human共存。
// voice为真实驱动格式，driver提供首次idle证明，out返回规范信封；能力不等于会话使用授权。
// human_endpoint_handoff仅在宿主实际实现源接口及严格候选承接时声明，不由网页或旧接听能力推断。
WireResult handoff_capability_command(std::string token, const VoiceCapability& voice,
                                      const FocusDriver& driver, Envelope& out,
                                      bool human_only = false, bool human_endpoint_handoff = false);
// SharedConnectionState
// Ready仅证明原连接及统一快照就绪，不能证明任一会话、媒体或焦点已建立。
enum class SharedConnectionState {
    Idle,
    Connecting,
    Subscribing,
    Reporting,
    Ready,
    Failed,
    Closed
};
// SharedDeviceConnection
// 单事件线程持有唯一网络调用权；AI／人工／动作／焦点只能通过固定借用端口发布。
// 关闭不宣称媒体释放：宿主必须先停业务及实际输入输出，再关闭本连接。
class SharedDeviceConnection {
public:
    // 构造冻结原身份、已由实际驱动产生的快照及独立进程截止；不连接或申请媒体。
    SharedDeviceConnection(MqttPort& network, const std::string& identity, Envelope capability,
                           std::uint64_t deadline);
    ~SharedDeviceConnection();
    SharedDeviceConnection(const SharedDeviceConnection&) = delete;
    SharedDeviceConnection& operator=(const SharedDeviceConnection&) = delete;
    // start只接纳一次原连接；不重连、不刷新原能力token或进程期限。
    bool start(const MqttConfig& config, std::uint64_t now);
    // poll先以单调时钟推进网络、可信wall_ms清理私有正文，再消费至多32帧；未校时拒私有投递。
    void poll(std::uint64_t now, std::uint64_t wall_ms = 0);
    // take_message只有Ready允许交给业务；confirm_session仍须业务解析器确实接受原来源。
    std::optional<MqttEvent> take_message(SharedMqttOwner owner);
    MqttPort& port(SharedMqttOwner owner) {
        return router_.port(owner);
    }  // 固定归属，不借网络控制权。
    SharedMqttRouter& router() { return router_; }          // 同一事件线程显式确认原腿或退休原腿。
    SharedConnectionState state() const { return state_; }  // 不从Ready推断物理资源取得。
    void close();  // 幂等精确关闭原网络，不清数据库、不重开连接。
private:
    bool observe(std::uint64_t now);  // 回退／独立截止终态，原能力重试不延长许可。
    bool consume(const MqttEvent& event,
                 std::uint64_t now);  // 全部精确订阅之后才发布统一能力。
    bool report(std::uint64_t now);   // 复用Requests原token、正文及固定重试预算。
    void fail();                      // 路由封闭与网络退出，不产生媒体停止或业务失败应答。
    MqttPort& network_;               // 唯一原连接适配，生命周期长于本对象。
    SharedMqttRouter router_;         // AI／Human／Global共用原请求关联及精确Topic。
    Requests requests_;               // 只保存原统一能力请求，不消费其他腿ACK。
    Envelope capability_;             // 原实际快照，不允许在收到邀请时升级能力。
    std::string up_, down_, capability_bytes_;  // 精确原信令路由及不可变编码。
    std::array<std::string, 3> topics_;         // SIG、MSG、action精确下行，不订阅通配符。
    std::array<int, 3> subscriptions_{};        // 各自原MID，不借另一条SUBACK准入。
    std::array<bool, 3> subscribed_{};          // 只有实际成功SUBACK才确认。
    std::optional<MqttEvent> retained_;         // 背压至多保留一条原帧，不读走后续帧。
    std::optional<std::uint64_t> clock_;        // 仅同一宿主单调时钟。
    std::uint64_t deadline_, phase_started_ = 0, retained_at_ = 0, revision_ = 0;
    std::uint64_t router_wall_highwater_ = 0;  // 独立可信墙钟防回退，不改变网络单调时钟期限。
    SharedConnectionState state_ = SharedConnectionState::Idle;
    bool valid_ = false, network_started_ = false, disconnected_ = false;
};
}  // namespace ur::device
