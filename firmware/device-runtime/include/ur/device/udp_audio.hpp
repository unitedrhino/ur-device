// UDP 音频泵连接会话策略、PCM 驱动、Opus 和加密数据报；不负责信令鉴权。
#pragma once
#include <optional>
#include "ur/device/datagram_port.hpp"
#include "ur/device/pcm_port.hpp"
#include "ur/device/session.hpp"
#include "ur/device/udp_channel.hpp"

namespace ur::device {
// UdpFailure 表示原UDP poll致命分支；普通坏包/旧包/解码丢弃不作为故障。
enum class UdpFailure {
    Unconfigured,
    ClockRollback,
    ProbeReceive,
    ProbeSend,
    ProbeDeadline,
    CaptureRead,
    CaptureEncode,
    CaptureEncrypt,
    CaptureSend,
    ReplyReceive,
    ReplyCrypto,
    ReplyBeyondEnd,
    PcmWrite,
    PlaybackStall,
    Unknown
};
// udp_failure_name 只返回固定诊断字面值；非法枚举返回空值。
std::string_view udp_failure_name(UdpFailure failure);
// AudioTiming 仅记录本次回复的网络/解码时刻，不等同扬声器起播。
struct AudioTiming {
    std::optional<std::uint64_t> first_network_ms;   // 首个有效加密音频包。
    std::optional<std::uint64_t> first_playable_ms;  // 连续两个非静音解码帧已进入队列。
    std::uint64_t network_packets = 0;               // 本回复通过认证且进入播放态的媒体包数。
    std::uint64_t decoded_frames = 0;                // 本回复成功解码并写入播放队列的帧数。
    std::uint64_t non_silent_frames = 0;             // 解码样本中非静音的帧数。
    std::uint64_t missing_packets = 0;               // 预告首包到末包之间观察到的序号缺口数。
    bool observed_complete = false;  // 停播时预告首尾、接收序号与解码均完整；不代表密码学认证。
};
// UdpAudio 由 Session 调度采集/播放；每次 poll 最多各消费八帧以免饿死信令。
class UdpAudio final : public AudioPort {
public:
    // 所有端口及通道由调用者持有，必须先完成显式格式配置与授权对端绑定。
    UdpAudio(PcmPort& pcm, AudioCodecPort& codec, DatagramPort& socket, UdpChannel& channel)
        : pcm_(pcm), codec_(codec), socket_(socket), channel_(channel) {}
    ~UdpAudio() override;
    UdpAudio(const UdpAudio&) = delete;
    UdpAudio& operator=(const UdpAudio&) = delete;
    // enable_failure_diagnostics 只能在原通道关闭前开启，不补采旧故障。
    void enable_failure_diagnostics() {
        if (!closed_)
            failure_diagnostics_enabled_ = true;
    }
    // failure_diagnostic 只读首次原poll失败；不调用驱动、不消费数据报。
    std::optional<UdpFailure> failure_diagnostic() const { return failure_; }
    bool start_capture(std::string_view input_id) override;
    void stop_capture() override;
    bool start_playback(std::string_view response_id, std::uint64_t epoch) override;
    void stop_playback(std::uint64_t epoch) override;
    // expect_first_sequence 必须在播放启动后、首包消费前调用，建立首包丢失检测起点。
    bool expect_first_sequence(std::uint32_t first_sequence);
    // poll 使用单调毫秒；失败立即停两向媒体，调用者还需关闭 Session 并报告故障。
    bool poll(std::uint64_t now);
    // close 幂等释放当前通道；不可重开，重连必须创建新密钥/nonce 的新实例。
    void close();
    const AudioTiming& timing() const { return timing_; }  // 本次回复的测量结果。
    // last_non_silent_sent_ms 仅在UDP实际发送成功后更新，尾静音不能推迟测量起点。
    std::optional<std::uint64_t> last_non_silent_sent_ms() const { return last_voice_sent_; }

private:
    // fail_observed 保持原致命分支关闭次数，只在close前保存首次类别。
    bool fail_observed(UdpFailure failure);
    bool failure_diagnostics_enabled_ = false;  // 显式开启后才锁存首因。
    std::optional<UdpFailure> failure_;         // close保留，不能被后续重复poll覆盖。
    PcmPort& pcm_;                              // 平台采集与播放。
    AudioCodecPort& codec_;                     // 独立编解码状态。
    DatagramPort& socket_;                      // 已绑定授权对端的数据报。
    UdpChannel& channel_;                       // 本会话密钥与序号。
    SpeechOnset onset_;                         // 本次回复非静音帧统计。
    AudioTiming timing_;                        // 网络与解码指标分开记录。
    std::optional<std::uint32_t>
        last_reply_sequence_;  // 本回复已接纳的包序号，重开回复时不继承前轮水位。
    std::optional<std::uint64_t> last_voice_sent_;  // 本采集段最后成功发送非静音帧的poll单调时刻。
    std::optional<std::uint64_t> playback_blocked_at_;  // 原代次连续满队列起点；两秒无消费才终止。
    std::uint64_t epoch_ = 0;                           // 当前本地播放代次。
    bool capture_ = false;                              // 已启用上行采集。
    bool playback_ = false;                             // 已启用下行队列。
    bool closed_ = false;                               // 终态后不接受新媒体。
};
}  // namespace ur::device
