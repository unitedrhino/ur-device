// 文件说明：IPC桌面驱动为每个观看者保留独立Peer，共享核心不依赖浏览器或操作系统。
#pragma once
#include <map>
#include "ur/device/capture_gate.hpp"
#include "ur/device/fixed_queue.hpp"
#include "ur/device/ipc_client.hpp"

namespace ur::device {
// IPCBrowserRequest 只包含本地驱动事实；不能携带设备凭据或自由平台命令。
struct IPCBrowserRequest {
    std::string kind;      // ready、shutdown、无参doorbellPress意图或指定观看者的事实。
    std::string session;   // 独立观看会话，不能替代摄像头标识。
    std::string sdp;       // 当前Peer完整offer，仅ipcOffer有值。
    std::string token;     // 原轨/质量命令或Stopped原releaseId，不接受自由平台请求。
    std::string track;     // 本地质量意图的原授权轨，核心再次检查归属。
    std::string profile;   // 本地质量意图的固定档位，不接受浏览器自报计划修订。
    std::string state;     // 已执行active/paused/stopped，失败用unknown。
    bool success = false;  // 真实驱动完成结果，不从桥排队状态推断。
    bool enabled = false;  // privacyReport的原布尔意图，不借驱动成功字段表达物模型值。
    std::uint64_t media_revision = 1;  // IPC媒体驱动事实代次；旧桥省略只对应首次协商。
};
// decode_ipc_browser 严格校验本地桥字段，失败清空结果且不返回原始正文。
bool decode_ipc_browser(std::string_view body, IPCBrowserRequest& out);
// ipc_browser_sync_configuration 仅显式同步、名册消费与会议驱动同时开启时允许声明新能力。
bool ipc_browser_sync_configuration(bool sync, bool roster, bool talk);
// ipc_browser_sync_state 只投影原核心查询状态和已消费名册水位，不泄露token或触发媒体。
std::string ipc_browser_sync_state(const IPCClient& client, std::string_view session, bool enabled,
                                   bool rejected = false);
// ipc_browser_sync 只提交已鉴权本地桥的本腿查询，Pending重复意图由核心幂等保留。
std::string ipc_browser_sync(IPCClient& client, const IPCBrowserRequest& request, bool enabled,
                             std::uint64_t now, std::uint64_t wall_ms);
// ipc_browser_invite 仅转发本地确认标识，使用JSON编码器避免会话标识注入。
std::string ipc_browser_invite(std::string_view session, bool talk = false);
// ipc_browser_stop 终态或邀请超时也移除确认控件，不假定媒体端口已经存在。
std::string ipc_browser_stop(std::string_view session);
// ipc_browser_active
// 仅在共享状态机确认连通ACK及平台建立事件后发送，不从Peer自行推断。
std::string ipc_browser_active(std::string_view session);
// ipc_browser_participants
// 仅序列化C++原腿实际已提交名册，event_sequence不是媒体ready或完整快照证明。
std::string ipc_browser_participants(std::string_view session, std::uint64_t event_sequence,
                                     const std::vector<RosterParticipant>& participants);
// BrowserIPCMediaFactory
// 为最多四个会话创建独立Peer；双向驱动必须显式启用，预览不隐式获得对讲或AI视觉权限。
class BrowserIPCMediaFactory final : public IPCMediaFactory {
public:
    // Shared 是桥命令队列及活Peer索引；对象析构后仍由在途端口保持生命周期。
    struct Shared;
    // conference_talk 只允许宿主明确开启双向驱动，省略仍保持旧预览行为。
    explicit BrowserIPCMediaFactory(bool conference_talk = false, CaptureGate* gate = nullptr);
    bool supports_talk() const override;
    std::unique_ptr<IPCMediaPort> create() override;
    // report_connected
    // 必须来自指定Peer真实connected回调，answer之前或旧会话均拒绝。
    bool report_connected(std::string_view session, std::uint64_t revision = 1);
    // report_connectivity 只收原对讲租约同Peer自然暂断/恢复；原首次五秒固定，Stale旧回调忽略。
    Result report_connectivity(std::string_view session, std::string_view release_id,
                               bool connected, std::uint64_t now);
    // poll_connectivity 返回固定窗口已过的原会话；宿主须沿原client.end释放，不依赖网页定时器。
    std::vector<std::string> poll_connectivity(std::uint64_t now);
    // report_stopped 收录原会话/租约实际停止，排队close不能释放HumanCall共用资源。
    bool report_stopped(std::string_view session, std::string_view release_id);
    // take_command 由宿主逐条交给已鉴权的本地浏览器；空表示无待处理事实。
    std::optional<std::string> take_command();
    // failed 表示不可丢控制队列溢出，宿主须关闭全部会话和本地桥。
    bool failed() const;

private:
    std::shared_ptr<Shared> shared_;  // 多观看者共享有界交接，不共享Peer协议状态。
};
}  // namespace ur::device
