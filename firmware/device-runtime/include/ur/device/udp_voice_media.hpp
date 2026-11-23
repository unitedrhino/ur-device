// 新信令 UDP 媒体驱动组合：平台仅实现端口，探测/回复边界和释放策略由设备共用。
#pragma once
#include <memory>
#include "ur/device/fixed_queue.hpp"
#include "ur/device/reply_datagram.hpp"
#include "ur/device/udp_audio.hpp"
#include "ur/device/voice_client.hpp"

namespace ur::device {
// UdpVoiceMedia 由单事件线程调用；configure
// 后先用静音编码包验证双向网络，不启动真实采集。
class UdpVoiceMedia final : public PumpedVoiceMediaPort {
public:
    UdpVoiceMedia(DrainablePcmPort& pcm, AudioCodecPort& codec, DatagramPort& socket,
                  SessionCipherPort& cipher)
        : pcm_(pcm), codec_(codec), socket_(socket), cipher_(cipher) {}
    ~UdpVoiceMedia() override { close(); }
    UdpVoiceMedia(const UdpVoiceMedia&) = delete;
    UdpVoiceMedia& operator=(const UdpVoiceMedia&) = delete;
    // enable_failure_diagnostics 在实际终态前开启；新泵沿原配置时装配相同开关。
    void enable_failure_diagnostics();
    // enable_presignal_diagnostics仅原媒体配置前显式启用，不补采已存在队列。
    bool enable_presignal_diagnostics();
    // presignal_diagnostic只读最近实际prepare_reply成功的安全计数，关闭保留但不重造。
    std::optional<ReplyPreSignalDiagnostic> presignal_diagnostic() const {
        return presignal_snapshot_;
    }
    // failure_diagnostic 只读关闭前锁存的原UDP首因，不恢复旧媒体或驱动。
    std::optional<UdpFailure> failure_diagnostic() const { return failure_; }
    // udp_reply_start_receipt_ready声明同步安装路径，不能单独生成200；prepare_reply及start_playback
    // 都实际成功才可回执。PCM成功须已建立本机有界回复对象，网页执行/物理出声另验。
    bool udp_reply_start_receipt_ready() const override { return pcm_.reply_install_ready(); }
    bool configure(const VoiceMedia& media) override;
    std::uint32_t next_input_sequence() const override;
    bool prepare_reply(const std::array<std::uint8_t, 8>& nonce,
                       std::uint32_t first_sequence) override;
    bool finish_reply(std::uint32_t last_sequence) override;
    bool start_capture(std::string_view input_id) override;
    void stop_capture() override;
    bool start_playback(std::string_view response_id, std::uint64_t epoch) override;
    void stop_playback(std::uint64_t epoch) override;
    // poll 的 now 为单调毫秒；时钟回退或驱动错误终止通道。
    bool poll(std::uint64_t now) override;
    // ready 仅表示已收回本次完整探测包，不表示麦克风、扬声器或硬件 AEC 验收。
    bool ready() const override { return ready_; }
    // drained_epoch 需交回 VoiceClient；带代次可拒绝旧驱动回调。
    std::optional<std::uint64_t> drained_epoch() const override;
    // missing_tail
    // 表示末包等待超过250ms，须作为效果失败样本记录，不能冒充完整播放。
    bool missing_tail() const { return missing_tail_; }
    const AudioTiming& timing() const;
    // take_stopped_timing按停播代次取一次不可变统计；下一回复启动不会覆盖上一回复的最终事实。
    std::optional<AudioTiming> take_stopped_timing(std::uint64_t epoch);
    // last_non_silent_sent_ms
    // 暴露实际UDP发送测量点，无有效输入时返回空值而不是零延迟。
    std::optional<std::uint64_t> last_non_silent_sent_ms() const;
    void close() override;

private:
    // fail_observed 在原close前锁存实际类别，绝不延长探测或媒体预算。
    bool fail_observed(UdpFailure failure);
    bool failure_diagnostics_enabled_ = false, failure_terminal_ = false;  // 不补采终态。
    bool presignal_enabled_ = false;           // 默认关闭，只有宿主专用开关启用。
    std::uint32_t presignal_observation_ = 0;  // 跨媒体配置保持本机观察次序，不是协议标识。
    std::optional<ReplyPreSignalDiagnostic> presignal_snapshot_;  // 不含nonce的已冻结计数。
    std::optional<UdpFailure> failure_;  // 原泵销毁后仍保存固定首因。
    // StoppedTiming属于已停播回复，按驱动停播命令的顺序领取，不与当前回复统计混用。
    struct StoppedTiming {
        std::uint64_t epoch;
        AudioTiming timing;
    };
    DrainablePcmPort& pcm_;                         // 平台播放完成事实与采集队列。
    AudioCodecPort& codec_;                         // 显式格式编解码，不隐藏重采样。
    DatagramPort& socket_;                          // 已授权服务器套接字。
    SessionCipherPort& cipher_;                     // 密钥只存平台密码对象。
    std::unique_ptr<UdpChannel> channel_;           // 每次配置重新创建序号空间。
    std::unique_ptr<ReplyDatagram> replies_;        // 先于信令到达的有限缓存。
    std::unique_ptr<UdpAudio> pump_;                // 独立采集/播放策略。
    FixedQueue<StoppedTiming, 8> stopped_timings_;  // 有界终态快照；积压溢出时旧命令显式取不到。
    std::vector<std::uint8_t> probe_;               // 已加密静音探测，重发完全相同字节。
    std::optional<std::array<std::uint8_t, 8>> reply_nonce_;  // 当前选择的回复。
    std::array<std::uint8_t, 8> uplink_nonce_{};              // 禁止回复复用上行空间。
    std::optional<std::uint64_t> now_;                        // 最近一次事件循环单调时刻。
    std::optional<std::uint64_t> probe_at_;                   // 最近一次探测发送时刻。
    std::optional<std::uint64_t> end_at_;                     // 首个有效末包通知时刻。
    std::optional<std::uint64_t> tail_idle_at_;  // 可接收时最后进展；本地背压不能算作网络尾包丢失。
    std::uint64_t epoch_ = 0;                    // 当前播放代次。
    std::uint32_t last_sequence_ = 0;            // 云端实际发送的最后一包。
    std::uint32_t first_sequence_ = 0;           // 云端预告的首包序号。
    unsigned probe_attempts_ = 0;                // 最多三次，禁止无限保持失效会话。
    bool ready_ = false;                         // 双向网络验证完成。
    bool playing_ = false;                       // 本驱动当前正在播放。
    bool reply_prepared_ = false;                // 先安装 nonce 才允许启动输出。
    bool missing_tail_ = false;                  // 回复尾部超时丢失事实。
};
}  // namespace ur::device
