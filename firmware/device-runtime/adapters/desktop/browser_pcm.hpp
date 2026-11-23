// 浏览器PCM驱动：主线程交接采集/播放事实，协议核心不依赖WebAudio或WebSocket。
#pragma once
#include "ur/device/audio_output_gate.hpp"
#include "ur/device/buffered_pcm.hpp"
#include "ur/device/output_group_gate.hpp"
#include "ur/device/volume_port.hpp"

namespace ur::device {
// BrowserAudioCommandKind 的停止命令只作用于对应输入ID或播放代次。
enum class BrowserAudioCommandKind {
    CaptureStart,
    CaptureStop,
    PlaybackStart,
    PlaybackStop,
    OutputGate
};
// BrowserAudioCommand 由本地界面桥序列化；不包含平台设备凭据。
struct BrowserAudioCommand {
    BrowserAudioCommandKind kind;          // 需要浏览器实际执行的驱动操作。
    std::string id;                        // 采集段或回复标识。
    std::uint64_t epoch = 0;               // 仅播放方向使用。
    std::uint64_t gate_ticket = 0;         // 仅独立输出门控使用，不能与回复代次混用。
    bool output_enabled = true;            // 仅门控有效，不修改采集、用户音量或回复身份。
    std::string capture_session{};         // 仅显式仲裁的采集命令携带原scope，不在发送时填当前腿。
    std::uint64_t capture_generation = 0;  // 原可信路由代次；0保留旧本地JSON。
};
// BrowserPcm 只允许设备事件线程访问；网络/Worklet回调必须先经有界桥排队。
// 同一实际扬声器门控同时满足独立音频与完整speaker路径接口；不声称管理display。
class BrowserPcm final : public DrainablePcmPort,
                         public VolumePort,
                         public AudioOutputGatePort,
                         public OutputGroupGatePort {
public:
    explicit BrowserPcm(AudioFormat format);
    // ready须在浏览器成功创建音频设备后调用；格式不匹配不隐式重采样。
    bool ready(AudioFormat actual, bool echo_cancellation);
    // disconnect永久终止本驱动实例，旧网页不能恢复旧会话。
    void disconnect();
    bool aec_available() const { return ready_ && !failed_ && aec_; }
    // reply_install_ready仅实际音频Ready且原有界缓冲有效时声明同步安装；断连不复活旧实例。
    bool reply_install_ready() const override { return ready_ && !failed_ && buffer_.valid(); }
    bool begin_capture(std::string_view input_id) override;
    void end_capture() override;
    PcmResult read_frame(std::vector<std::int16_t>& pcm) override;
    bool begin_playback(std::string_view response_id, std::uint64_t epoch) override;
    // playback_backpressured
    // 保留浏览器尚未实际消费的容量；断连仍交由写帧原错误路径处理。
    bool playback_backpressured(std::uint64_t epoch) const override;
    bool write_frame(const std::vector<std::int16_t>& pcm, std::uint64_t epoch) override;
    // enable_failure_diagnostics
    // 仅在实例有效时开启当前写帧原因旁证；重复调用不清除已有原因。
    void enable_failure_diagnostics();
    // last_write_failure
    // 只返回最近一次实际写帧的闭集类别；默认关闭时返回None，不驱动媒体。
    PcmWriteFailure last_write_failure() const { return last_write_failure_; }
    void end_playback(std::uint64_t epoch) override;
    bool playback_empty(std::uint64_t epoch) const override;
    // captured拒绝旧输入；当前输入畸形或满队列使驱动失败，不能默默丢音频。
    bool captured(std::string_view input_id, const std::vector<std::int16_t>& pcm);
    // bind_capture_route 仅在闲置时绑定一次可信宿主的原AI腿；绑定后默认阻断输入，不提升旧驱动能力。
    bool bind_capture_route(std::string_view session, std::uint64_t generation);
    // fence_capture_route 关闭原PCM转发、清除积压和未交付采集命令；不是浏览器实际停麦回执。
    bool fence_capture_route(std::string_view session, std::uint64_t generation);
    // enable_capture_route 仅由宿主收齐原实际输入票据后调用；恢复原采集段，不自行重获设备焦点。
    bool enable_capture_route(std::string_view session, std::uint64_t generation);
    // captured_route 要求原会话、路由代次及采集段全部一致；迟到他腿帧不得进入UDP媒体泵。
    bool captured_route(std::string_view session, std::uint64_t generation,
                        std::string_view input_id, const std::vector<std::int16_t>& pcm);
    // played由Worklet实际消费完一帧后确认；收到网络消息或排入浏览器队列均不能确认。
    bool played(std::uint64_t epoch, std::uint32_t ticket);
    std::optional<BrowserAudioCommand> take_command();  // 控制命令必须先于媒体帧交付。
    std::optional<PlaybackFrame> take_playback();       // 有待发命令时不允许先发媒体。
    bool set_volume(unsigned percent) override;         // 修改后续交付帧的实际PCM增益。
    unsigned volume() const override { return volume_; }
    std::optional<std::uint64_t> request_output_gate(bool enabled, std::uint64_t now) override;
    bool output_gate_confirmed(std::uint64_t ticket, bool enabled) const override;
    bool poll_output_gate(std::uint64_t now) override;
    // output_gated 只收录原已交付门控的实际音频块确认；旧票据幂等丢弃，不改变当前许可。
    bool output_gated(std::uint64_t ticket, bool enabled, std::uint64_t now);

private:
    bool command(BrowserAudioCommand value);  // 队列满转终态，不无限缓存控制。
    // capture_route_matches 只核原固定路由，断连后所有旧票据永久失效。
    bool capture_route_matches(std::string_view session, std::uint64_t generation) const;
    // accept_capture_frame 保存原有有界队列规则；只有可信路由或未绑定的旧入口可调用。
    bool accept_capture_frame(std::string_view input_id, const std::vector<std::int16_t>& pcm);
    AudioFormat format_;                            // 显式协商格式。
    BufferedPcm buffer_;                            // 共用代次、顺序确认和有界音频队列。
    FixedQueue<BrowserAudioCommand, 16> commands_;  // 主线程与网页间待发控制。
    std::string response_;                          // 当前播放回复。
    std::uint64_t epoch_ = 0;                       // 当前播放代次，停止后归零。
    bool ready_ = false;                            // 浏览器设备已初始化的事实。
    bool aec_ = false;                              // getSettings确认的AEC，不代表硬件验收。
    bool failed_ = false;                           // 故障不可在原实例上复活。
    bool failure_diagnostics_ = false;              // 宿主显式开启，普通驱动不记录额外写帧事实。
    PcmWriteFailure last_write_failure_ =
        PcmWriteFailure::None;                 // 仅当前写调用的原因，成功写清空。
    unsigned volume_ = 100;                    // 已交给浏览器的有限在途帧不追溯修改。
    std::uint64_t gate_ticket_ = 0;            // 独立门控代次，不在回复取消或切换时重置。
    std::uint64_t gate_confirmed_ = 0;         // 最后收录的实际音频块票据，初始为未知。
    std::uint64_t gate_deadline_ = 0;          // 原请求两秒固定截止，重复回调不延长。
    std::optional<std::uint64_t> gate_clock_;  // 门控使用单调时钟，与媒体墙钟无关。
    bool gate_enabled_ = true;                 // 当前原门控请求目标，不是实际证明。
    bool gate_pending_ = false;                // 同一音频输出仅一条在途门控。
    bool gate_sent_ = false;                   // 必须先交付本地桥，不能确认未发出票据。
    std::string capture_session_;              // 一次绑定的原AI腿，终态后不能换成人工或另一个AI。
    std::uint64_t capture_generation_ = 0;     // 原宿主路由代次；0保持既有无仲裁驱动行为。
    std::string capture_held_input_;    // 围栏保留的原逻辑采集段；取消后必须清空，不能自动复活。
    bool capture_forwarding_ = false;   // 仅实际门控确认后开启PCM转发，不由ready/AEC授予。
    bool legacy_capture_used_ = false;  // 已用旧采集源不再转换新路由，避免重新归属旧排队命令。
};
}  // namespace ur::device
