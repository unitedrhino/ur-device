// 文件说明：桌面视频通话媒体适配；浏览器负责驱动，C++持有会话和MQTT身份。
#pragma once
#include <utility>
#include "browser_call_focus.hpp"
#include "ur/device/capture_gate.hpp"
#include "ur/device/fixed_queue.hpp"
#include "ur/device/human_call.hpp"

namespace ur::device {
// browser_call_state 只编码固定本地状态及已校验原会话，不转交平台正文或身份凭据。
std::string browser_call_state(std::string_view state, std::string_view session);
// browser_call_control_receipt 只编码已核验的固定ACK/本端实际投影，不携带自由平台正文。
std::string browser_call_control_receipt(const HumanCallControlReceipt& receipt);
// browser_call_transfer_receipt 投影原转接与撤候选的独立事实，不传目标或候选身份。
std::string browser_call_transfer_receipt(const HumanCallTransferReceipt& receipt);
// CallOutputReadiness 区分新Peer原静音刷新在途、可开始取得与确定失败；Ready不代表已放音。
enum class CallOutputReadiness { Pending, Ready, Failed };
// BrowserCallMedia 把可移植通话端口映射为受控回环桥命令。
class BrowserCallMedia final : public CallMediaPort, public OutputGroupGatePort {
public:
    // gate仅新统一宿主借用同一物理预约器；旧Standalone默认不改变原媒体驱动行为。
    explicit BrowserCallMedia(CaptureGate* gate = nullptr, std::string camera = "front")
        : gate_(gate), camera_(std::move(camera)) {}
    bool available() const override;
    // video_receive_supported 仅表明浏览器具备独立接收驱动，实际授权由会话所选轨决定。
    bool video_receive_supported() const override { return true; }
    // configure_video_receive 只在本次Peer启动前绑定所选合同，不允许运行中扩权。
    bool configure_video_receive(bool enabled) override;
    bool prepare() override;
    // bind_session 绑定宿主已验证的本腿；实际旧资源未释放时不可改绑。
    bool bind_session(std::string_view session);
    // bind_session_scope 由核心在启动前绑定所选原腿，不允许网页指定新的采集租约。
    bool bind_session_scope(std::string_view session) override {
        return gate_ ? bind_session(session) : true;
    }
    // report_stopped 必须是原session/releaseId实际停止回执，旧回执不能释放新租约。
    bool report_stopped(std::string_view session, std::string_view release_id);
    // matches 只允许当前已启动且未停止的原驱动回调，拒绝旧session或旧资源票据。
    bool matches(std::string_view session, std::string_view release_id) const;
    bool begin() override;
    bool apply_answer(std::string_view sdp) override;
    bool connected() const override { return connected_; }
    void close() override;
    // report_connected 只在浏览器实际ICE/DTLS connected回调后调用。
    bool report_connected();
    // report_connectivity 只接已建立原租约的同Peer暂断/恢复；首断起五秒固定窗口不续期。
    Result report_connectivity(std::string_view session, std::string_view release_id,
                               bool connected, std::uint64_t now);
    // connectivity_expired 由宿主独立时钟驱动一次终止；网页失联不能无限保留采集。
    bool connectivity_expired(std::uint64_t now);
    // enable_hold 由独立宿主显式装配唯一Call焦点；旧共享宿主默认不广告此异步能力。
    bool enable_hold(OutputFocusDriver& driver, FocusAuthority& authority, std::uint64_t deadline);
    bool hold_supported() const override { return focus_driver_ != nullptr; }
    // hold_async_supported 供后继核心识别真实异步端口，保留旧同步驱动合同。
    bool hold_async_supported() const override { return hold_supported(); }
    // bind_control_scope 在核心收录原腿后绑定当前实际Peer，不接受网页选择会话。
    bool bind_control_scope(std::string_view session);
    // output_readiness 只读原新Peer刷新状态，调用者仍须泵原回执与两秒截止，不刷新预算。
    CallOutputReadiness output_readiness() const;
    // focus_scope 仅投影宿主绑定及核心已验证原控制，调用者另须核原腿established。
    std::optional<BrowserCallFocusScope> focus_scope() const;
    // acquire_output 原已建立腿首次实际取得完整speaker/display，不能沿排队打开播放。
    bool acquire_output(std::uint64_t now);
    // request_hold 返回原在途票据；排队不是完成，固定两秒不因ACK/重复查询续期。
    std::optional<std::uint64_t> request_hold(bool held, std::string_view control_token,
                                              std::uint64_t revision, std::uint64_t now) override;
    // hold_result null为在途，true为原实际完成，false仅表示未知/拒绝，不证明资源释放。
    std::optional<bool> hold_result(std::uint64_t ticket, bool held) const override;
    // receive_control 只接本地原票据实际回执；不生成或接受任意MQTT命令。
    bool receive_control(std::string_view body, std::uint64_t now);
    // poll_control 推进独立焦点/实际门控，不阻塞网络与其他设备业务。
    bool poll_control(std::uint64_t now, std::uint64_t wall);
    // release_output 只沿原已绑定完整组显式释放，不能把Peer关闭直接当焦点release。
    bool release_output(std::uint64_t now);
    bool output_ready() const { return output_ready_; }
    std::uint64_t peer_epoch() const { return peer_epoch_; }
    std::uint64_t output_revision() const { return focus_driver_ ? focus_driver_->revision() : 0; }
    std::optional<std::uint64_t> request_output_gate(bool enabled, std::uint64_t now) override;
    bool output_gate_confirmed(std::uint64_t ticket, bool enabled) const override;
    // take_command 每轮最多交付一条固定类型命令，桥背压由主程序处理。
    std::optional<std::string> take_command();

private:
    // emit 只序列化固定命令类型和已经验证的SDP。
    bool emit(std::string_view kind, std::string_view sdp = {});
    // begin_focus 构造可信宿主局部操作，原独立授权与实际CAS均通过才进入在途。
    bool begin_focus(std::string_view operation, std::string_view token, std::uint64_t now);
    FixedQueue<std::string, 8> commands_;  // 信令层不无限堆积浏览器控制。
    bool started_ = false;                 // 本次Peer创建意图。
    bool receive_video_ = false;           // 当前所选独立接收轨，不从本地摄像头推导。
    bool answered_ = false;                // 完整answer已发送给网页。
    bool connected_ = false;               // 实际ICE/DTLS连接状态。
    bool natural_suspended_ = false, natural_expired_ = false,
         natural_notified_ = false;       // 原Peer固定窗口与一次过期通知，旧初次回调不得复活。
    std::uint64_t natural_started_ = 0;   // 原Peer首次暂断时间，零值也是合法单调时间。
    CaptureGate* gate_ = nullptr;         // 新宿主借用，寿命须长于驱动及回执。
    std::optional<std::uint64_t> lease_;  // 整组原采集资源，Stop排队并非释放。
    std::string session_;                 // 原本地驱动腿，旧Standalone为空。
    bool stopping_ = false;               // 已请求实际停止但未收录回执，不允许重建。
    std::string camera_;                  // 新宿主与核心/能力快照一致的唯一画面源绑定。
    OutputFocusDriver* focus_driver_ = nullptr;  // 仅本独立宿主，不从桥报文自建仲裁器。
    FocusAuthority* focus_authority_ = nullptr;  // 独立原Call许可，不能借AI会话。
    std::uint64_t control_deadline_ = 0, control_now_ = 0, control_wall_ = 0;  // 原独立时钟/期限。
    std::string control_session_, focus_id_;               // 当前实际Peer的原端腿/完整组租约。
    std::uint64_t peer_epoch_ = 0, focus_generation_ = 0;  // 本进程原Peer/实际焦点操作代次。
    std::optional<FocusCommand> focus_command_;  // 当前唯一原焦点请求，完成后只留实际状态。
    std::optional<FocusPermit> focus_permit_;    // 原固定许可，实际完成前持续复核且不续期。
    std::string focus_state_;    // 实际acquired/suspended/restored/released，不取平台held推导。
    bool output_ready_ = false;  // 原完整组已实际取得，关闭或在途不能作为播放许可。
    std::optional<std::string> gate_command_, hold_command_;  // 各一条原局部门控，背压不刷新。
    std::uint64_t gate_ticket_ = 0, gate_confirmed_ = 0, gate_deadline_ = 0, gate_epoch_ = 0;
    std::string gate_session_;  // 原票据形成时的腿，不能用未来当前session解释回执。
    bool gate_enabled_ = false, gate_pending_ = false, gate_sent_ = false, control_failed_ = false;
    std::uint64_t hold_ticket_ = 0, hold_deadline_ = 0,
                  hold_revision_ = 0;  // 原控制票据/固定截止。
    std::string hold_token_;           // 原核心验证的控制身份，不是本地gateTicket。
    bool hold_target_ = false, hold_pending_ = false, hold_sent_ = false;
    bool hold_up_applied_ = false, hold_focus_applied_ = false;  // 两条实态独立，缺任一不能成功。
    std::optional<bool> hold_result_;  // 原在途/实际/未知结果，不以false声称已停止。
};
}  // namespace ur::device
