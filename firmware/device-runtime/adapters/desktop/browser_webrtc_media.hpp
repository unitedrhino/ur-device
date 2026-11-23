// 文件说明：桌面WebRTC媒体驱动由浏览器提供Peer，C++继续独占会话、身份和MQTT。
#pragma once
#include "ur/device/fixed_queue.hpp"
#include "ur/device/output_group_gate.hpp"
#include "ur/device/voice_client.hpp"
#include "ur/device/volume_port.hpp"

namespace ur::device {
// BrowserWebRTCMedia 的全部回调由设备主事件线程交接，网页不直接改变核心状态。
class BrowserWebRTCMedia final : public PumpedVoiceMediaPort,
                                 public VolumePort,
                                 public DeferredVolumePort,
                                 public OutputGroupGatePort {
public:
    // enable_output_gate仅由实际页面ready后可信宿主启用；构造和Peer连通不证明首次静音。
    bool enable_output_gate();
    // request_output_gate排原票据；开启须原Peer真实ready，关闭不停止采集或用户播放意图。
    std::optional<std::uint64_t> request_output_gate(bool enabled, std::uint64_t now) override;
    // output_gate_confirmed仅当前实际线程完成，命令领取或Gain属性不提供此事实。
    bool output_gate_confirmed(std::uint64_t ticket, bool enabled) const override;
    // output_gated只接原已交付票据；旧回执隔离、未知及两秒截止不得生成成功。
    bool output_gated(std::uint64_t ticket, bool enabled, std::uint64_t now);
    // bind_capture_route只固定候选的原本机路由，实际完整组取得前不能开始输入。
    bool bind_capture_route(std::string_view session, std::uint64_t generation);
    // fence_capture_route在原作用域关闭输入意图；物理Sender停止仍须原实际票据确认。
    bool fence_capture_route(std::string_view session, std::uint64_t generation);
    // enable_capture_route只由可信宿主确认原完整焦点许可后调用，不自行audioStart。
    bool enable_capture_route(std::string_view session, std::uint64_t generation);
    // finish_capture_route_close仅原HAL实际settled后调用，避免先关Peer导致无法核原Sender释放。
    bool finish_capture_route_close(std::string_view session, std::uint64_t generation);
    // configure只发起设备Peer创建，必须等完整offer/answer及ICE连通回报。
    bool configure(const VoiceMedia& media) override;
    // enable_vision仅宿主实际预检后锁定原摄像头Profile；默认无视觉端口，不授取帧许可。
    bool enable_vision(const VoiceVisionProfile& profile);
    // prepare_vision_preflight仅锁原宿主物理映射，不把配置存在当作实际视觉能力。
    bool prepare_vision_preflight(const VoiceVisionProfile& profile, std::string_view device_id);
    // receive_vision_ready接原认证桥五字段预检；匹配且实际支持后才提供ready Profile。
    bool receive_vision_ready(std::string_view json);
    // vision_profile返回本实例稳定快照，不能由网页ready或运行中修改扩大能力。
    const VoiceVisionProfile* vision_profile() const override;
    // configure_vision只配置三轨及初始paused视频；后续发布尚需原会话与媒体代次控制。
    bool configure_vision(const VoiceMedia& media, const VoiceVisionProfile& profile) override;
    // begin_vision_control只排原Core控制；实际替轨回读前不提供Active或Stopped。
    VoiceVisionRequestResult begin_vision_control(const VoiceVisionControl& control) override;
    // take_vision_fact领取同原已交付控制的实际回执；不读取网页当前状态补造事实。
    std::optional<VoiceVisionFact> take_vision_fact() override;
    // cancel_vision_control撤原代次意图，不能当作摄像头资源已停止。
    void cancel_vision_control(std::uint64_t generation) override;
    // receive_vision_actual严格解析可信本地桥九字段；原scope、代次和事实枚举必须一致。
    bool receive_vision_actual(std::string_view json);
    // begin_recovery只接核心确认原断连后的相邻代次；不重建物理输出门或音量状态。
    bool begin_recovery(std::uint64_t next_revision) override;
    // recovery_needed只报告原已连通Peer实际暂断，不把初次协商当恢复。
    bool recovery_needed() const override { return disconnected_ && !failed_; }
    std::uint32_t next_input_sequence() const override { return 0; }
    bool prepare_reply(const std::array<std::uint8_t, 8>& nonce,
                       std::uint32_t first_sequence) override;
    bool finish_reply(std::uint32_t last_sequence) override;
    std::optional<std::string> take_offer() override;
    bool apply_answer(std::string_view sdp) override;
    void close() override;
    bool poll(std::uint64_t now) override;
    bool ready() const override { return connected_ && !failed_; }
    std::optional<std::uint64_t> drained_epoch() const override { return drained_; }
    bool start_capture(std::string_view input_id) override;
    void stop_capture() override;
    bool start_playback(std::string_view response_id, std::uint64_t epoch) override;
    void stop_playback(std::uint64_t epoch) override;
    bool set_volume(unsigned percent) override;
    unsigned volume() const override { return volume_; }
    bool request_volume(std::string_view token, unsigned percent) override;
    std::optional<VolumeApplied> take_volume_applied() override;
    // volume_applied只接受当前请求的确认，过时或伪造确认不得改变音量。
    bool volume_applied(std::string_view token, unsigned percent);
    // offer_received仅接已交付当前Peer代次完整SDP；旧代次返回已消费但不改变事实。
    bool offer_received(std::string_view sdp, std::uint64_t peer_revision);
    // connected_received仅在answer交付后接纳浏览器实际ICE/DTLS连通事实。
    bool connected_received(std::uint64_t peer_revision);
    // disconnected_received隔离旧回调，只在原当前Peer曾连通后关闭输入和回复意图。
    bool disconnected_received(std::uint64_t peer_revision);
    // drained_received来自浏览器音频监测，过时代次不能完成新回复。
    bool drained_received(std::uint64_t epoch, std::uint64_t peer_revision);
    // take_command将队列消息交给已配对的回环桥，返回空不代表媒体结束。
    std::optional<std::string> take_command();
    const std::string& current_response() const { return response_; }

private:
    // command只允许固定类型和已验证值生成JSON，队列背压即媒体失败。
    bool command(std::string_view kind, std::string_view id = {}, std::uint64_t epoch = 0,
                 std::string_view sdp = {});
    // vision_command构造固定本地字段，不携带原云token或授权凭据。
    std::string vision_command(const VoiceVisionControl& control, bool cancel) const;
    FixedQueue<std::string, 16> commands_;           // 有界本地控制；不能无限积累SDP或动作。
    std::optional<std::string> offer_;               // 等待设备核心取走的完整SDP。
    std::optional<std::uint64_t> drained_;           // 已经监测到的软件播放排空代次。
    std::string input_;                              // 当前实际获准采集段。
    std::optional<VoiceVisionProfile> vision_;       // 实际宿主显式启用的不可变视觉选择。
    std::optional<VoiceVisionProfile>
        vision_preflight_;          // 原native未授取帧的选择，预检完成前不能广告。
    std::string vision_device_id_;  // 只在原回环配对内匹配物理映射，不写日志或公共MQTT。
    std::optional<bool> vision_preflight_result_;  // 首次真实预检结果；重复不能提升不支持能力。
    std::optional<VoiceVisionControl> vision_control_;  // 原唯一当前控制及固定会话关联。
    FixedQueue<VoiceVisionFact, 16> vision_facts_;  // 已合法形成的历史事实，有界领取不复活旧意图。
    std::string vision_command_;                    // 只在原字节实际交付后接纳回执。
    bool vision_sent_ = false, vision_settled_ = false, vision_cancelled_ = false,
         vision_active_ = false;     // 排队、取消与实际资源状态不能互相冒充。
    bool vision_uncertain_ = false;  // 原失败未证明释放时，新的publish必须等待独立实际Stop。
    std::optional<VoiceVisionFact> vision_last_fact_;  // 重复实际回执必须逐字段相同。
    std::string peer_command_;        // 原Peer创建完整字节；领取之后才允许offer事实。
    bool vision_configured_ = false;  // 已使用视觉端口；终态后不复用同代次或旧回调。
    std::string response_;                           // 当前实际播放回复。
    std::uint64_t epoch_ = 0;                        // 本地回复播放代次。
    std::uint64_t peer_revision_ = 0;                // 核心授予的Peer代次，仅用于本地桥栅栏。
    bool disconnected_ = false, peer_command_sent_ = false; // 暂断事实与原建Peer命令实际交付。
    unsigned volume_ = 100;                          // 浏览器应用的目标增益，0至100。
    std::optional<VolumeApplied> volume_pending_;    // 已发出而未获浏览器确认的设置。
    std::optional<VolumeApplied> volume_confirmed_;  // 待工具执行器提取的确认。
    bool configured_ = false;                        // 当前会话已分配Peer意图。
    bool offer_taken_ = false;                       // 防止复用旧SDP。
    bool answered_ = false;                          // answer已交给浏览器，不等于ICE连通。
    bool connected_ = false;                         // 浏览器报告实际ICE/DTLS connected。
    bool failed_ = false;                            // 驱动错误使本会话不可继续。
    bool output_gate_ready_ = false, gate_pending_ = false, gate_sent_ = false,
         gate_enabled_ = false;  // 显式实态端口与原在途目标；旧默认驱动不依赖新门控。
    std::uint64_t gate_ticket_ = 0, gate_confirmed_ = 0, gate_deadline_ = 0;
    std::optional<std::uint64_t> gate_clock_;  // 固定单调截止，不从重投回执续长。
    std::string gate_command_;                 // 原唯一排队门控完整字节，只在真正取走时标记已交付。
    std::string capture_session_, capture_start_command_; // 固定候选scope及尚未交付的原输入命令。
    std::uint64_t capture_generation_ = 0; // 原本机路由代次，不借Peer重连隐式更新。
    bool capture_permitted_ = false, capture_closing_ = false; // 许可不等于输入意图，实际清理前保留Peer。
};
}  // namespace ur::device
