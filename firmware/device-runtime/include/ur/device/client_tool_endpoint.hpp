// 文件说明：审核工具独立MSG端点；原内容生命周期、实际副作用与结果传输确认分层，不借音频当前回复。
#pragma once
#include "ur/device/client_tool.hpp"
#include "ur/device/content_wire.hpp"
#include "ur/device/fixed_queue.hpp"
#include "ur/device/mqtt_port.hpp"
#include "ur/device/requests.hpp"

namespace ur::device {
// ClientToolReceipt只读原合法Submit
// ACK；accepted为结果收件，continuation另行表示原AISVR接纳。
struct ClientToolReceipt {
    std::string session, response,
        call;                  // 原三身份，仅宿主内存/配对桥，不进入自由日志。
    std::string continuation;  // accepted或unknown；省略按unknown，不推断模型已续答。
    int code = 0;              // 原明确200或拒绝码；传输受理/超时不生成事实。
    std::uint64_t index = 0;   // 本Endpoint单调回执水位，重复ACK不计。
};
// ClientToolEndpoint
// 复用原Requests仅重传结果；真实主题执行只由独立Executor驱动一次。
class ClientToolEndpoint final : private PublishPort {
public:
    // identity是已配置product/device；instance_id每进程唯一，不使用平台执行令牌作本地票据。
    ClientToolEndpoint(MqttPort& mqtt, ClientToolDisplayPort& display, std::string identity,
                       std::string instance_id);
    // observe 仅接收Runtime已经准入的原内容；展示参数不授予schema或任意脚本权限。
    bool observe(const ContentEvent& content);
    // receive_execute
    // 仍须原活会话和原toolCallStart；wall只用于原授权截止，now只用于本地调度。
    ClientToolDecision receive_execute(std::string_view body, std::string_view session,
                                       bool established, std::uint64_t now, std::uint64_t wall);
    // receive_ack
    // 严格核原请求/会话/回复/调用，accepted不表示模型续答或UI再次执行。
    bool receive_ack(std::string_view body, std::uint64_t now);
    // take_receipt返回最多32条原合法ACK观察；不包含令牌、参数/schema或结果正文。
    std::optional<ClientToolReceipt> take_receipt();
    // poll 在原deadline、取消及会话结束栅栏之后推进结果重传；未知不重发副作用。
    bool poll(std::uint64_t now);
    // cancel_response 撤销原回复结果传输；不能让原Requests排队重传越过取消边界。
    void cancel_response(std::string_view session, std::string_view response);
    // response_cancelled 查询原显式取消墓碑；正常完成不代表音频已播放，不据此过滤跨通道迟到音频。
    bool response_cancelled(std::string_view session, std::string_view response) const;
    // close_session 关闭原会话的执行与传输，保留墓碑以拒绝迟到原调用复活。
    void close_session(std::string_view session, std::uint64_t now);
    // close退休本连接全部原scope；VoiceClient故障先清sessionId时也不能遗留可重传结果。
    void close(std::uint64_t now);

private:
    // Scope只来自已准入内容，每回复最多64个开始调用，会话最多512个回复，终态不淘汰。
    struct Scope {
        // Tool保留原开始名称与已展示终态，终态后不能再从同名展示事件取得执行许可。
        struct Tool {
            std::string name;   // 原开始审核名。
            bool done = false;  // 原工具结果已终结。
        };
        std::string session;                // 原会话，不能跨会话迁移。
        std::map<std::string, Tool> tools;  // 原开始调用及审核工具名，不保存展示参数。
        bool closed = false;                // 回复终态或本地取消后不可复活。
        bool cancelled = false;             // 仅显式取消；正常完成与本地播放排空不是同一事实。
    };
    // Receipt固定原结果发送身份和原期限；Requests不得自行续期或在取消后实际发布。
    struct Receipt {
        std::string session, response, call;  // 三个原关联，与msgToken共同核ACK。
        std::uint64_t deadline;               // 原Execute墙钟预算对应的固定单调截止。
    };
    // publish是Requests唯一出口，先核原活scope/原deadline，拒绝过期排队重传而不改原正文。
    bool publish(std::string_view topic, std::string_view body) override;
    // active判断原授权是否仍可传输，不能凭当前resp或本地成功状态延长期限。
    bool active(const Receipt& receipt) const;
    MqttPort& mqtt_;                           // 唯一已鉴权设备连接。
    std::string topic_;                        // 固定本设备MSG上行。
    ClientToolExecutor executor_;              // 单副作用状态机，真实驱动收据另行推进。
    Requests requests_;                        // 固定字节结果重试，不重新调用Executor。
    std::map<std::string, Scope> scopes_;      // 原内容授权关联墓碑。
    std::map<std::string, Receipt> receipts_;  // 原Submit token关联，最多512项。
    std::map<std::pair<std::string, std::string>, std::uint64_t> deadlines_;  // 原调用固定截止。
    std::uint64_t now_ = 0;                           // 当前事件线程的单调时钟。
    bool valid_ = true;                               // 时钟或容量失效后不再执行/发布。
    FixedQueue<ClientToolReceipt, 32> observations_;  // 有界原ACK事实，不安装额外协议监听器。
    std::uint64_t observation_index_ = 0;  // ≤512个原调用的单调水位，不靠当前工具状态补造。
};
}  // namespace ur::device
