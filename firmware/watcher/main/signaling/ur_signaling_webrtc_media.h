/* 统一新版协议 WebRTC 音频媒体适配：libpeer 上游源码栈（components/
 * libpeer_src，替换 esp_peer 预编译库的 nominate 缺陷实现）。
 * 音频上行 AFE 16k → Opus编码 → RTP（固定48k时钟）；下行
 * onaudiotrack(Opus 48k) → 解码 16k → 既有信令播放路径。offer 为
 * 候选收集完成的完整 SDP（上游 agent 同步收集，天然 iceMode complete）。 */
#ifndef UR_SIGNALING_WEBRTC_MEDIA_H
#define UR_SIGNALING_WEBRTC_MEDIA_H

#include <stdint.h>

#include <atomic>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "peer_connection.h"
#include "ur/device/audio_codec.hpp"
#include "ur/device/pcm_port.hpp"
#include "ur/device/voice_client.hpp"

#include "ur_signaling_pcm.h"

#if CONFIG_UR_SOFTWARE_AEC
#include <condition_variable>
#include <mutex>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#endif

namespace ur::device {
class UrSignalingVideo;  /* 前向声明：视频形态才链接 esp_h264，头文件经 protocol 层引入 */

/* WebRtcVoiceMedia 由信令事件线程驱动；libpeer 单线程模型经
 * peer_connection_loop 推进，采集与播放经既有 AudioService 信令旁路。 */
class WebRtcVoiceMedia final : public PumpedVoiceMediaPort {
public:
    /* pcm 为 16k/20ms 帧端口；codec 上行按16k/20ms配置、下行解码输出16k；RTP时钟保持48k。
     * 持有具体 UrSignalingPcm：close 需跨会话复位播放代次（接口无该能力）。 */
    explicit WebRtcVoiceMedia(UrSignalingPcm& pcm, AudioCodecPort& codec);
    ~WebRtcVoiceMedia() override;
    WebRtcVoiceMedia(const WebRtcVoiceMedia&) = delete;
    WebRtcVoiceMedia& operator=(const WebRtcVoiceMedia&) = delete;

    bool configure(const VoiceMedia& media) override;
    std::uint32_t next_input_sequence() const override;
    bool prepare_reply(const std::array<std::uint8_t, 8>& nonce,
                       std::uint32_t first_sequence) override;
    bool finish_reply(std::uint32_t last_sequence) override;
    bool start_capture(std::string_view input_id) override;
    void stop_capture() override;
    /* arm_capture 提前打开采集队列但暂停上行：audioStart ACK 往返期间只缓冲，
     * ACK 后由 start_capture_after_ack 按序发送，避免丢首字。 */
    bool arm_capture(std::string_view input_id) override;
    /* start_capture_after_ack 解除暂停并开始上行；未 arm 时保持旧行为。 */
    bool start_capture_after_ack(std::string_view input_id) override;
    /* disarm_capture 撤销武装并清空缓冲（ACK 失败/暂停/关闭）。 */
    void disarm_capture() override;
    bool capture_armed() const override { return capture_armed_; }
    bool start_playback(std::string_view response_id, std::uint64_t epoch) override;
    void stop_playback(std::uint64_t epoch) override;
    bool poll(std::uint64_t now) override;
    bool ready() const override { return state_.load() == PeerState::Connected; }
    bool recovery_needed() const override;
    std::optional<std::uint64_t> drained_epoch() const override;
    /* take_offer 返回本地候选收集完成的完整 offer SDP；仅产生一次。 */
    std::optional<std::string> take_offer() override;
    /* apply_answer 把平台 mediaAnswer 的完整 answer SDP 交给本端。 */
    bool apply_answer(std::string_view sdp) override;
    /* begin_recovery 重建 Peer；旧资源同步回收。 */
    bool begin_recovery(std::uint64_t /*media_revision*/) override;
    void close() override;

    /* ---- aiVision 三轨接口（仅视频形态生效）---- */
    /* SetVideoPipeline 注入视频管线与启动闭包（闭包内绑定板级相机与帧源）；
     * 无摄像头时不注入，能力上报不带 aiVision。 */
    void SetVideoPipeline(UrSignalingVideo* video, std::function<bool()> starter);
    /* vision_profile 返回摄像头能力快照；无视频形态或摄像头未就绪返回 nullptr。 */
    const VoiceVisionProfile* vision_profile() const override;
    // SetCallVideoAvailable 在装配时登记人际 JPEG 相机，不授予 AI 视觉权限。
    void SetCallVideoAvailable(bool available) { call_video_available_ = available; }
    // call_video_profile 返回人际通话的相机快照；不可用时保持空。
    const VoiceVisionProfile* call_video_profile() const override;
    /* DefaultVisionProfile 返回视频形态的静态能力快照；空闲能力上报与运行时
     * 必须共用该快照，否则同修订号能力指纹不一致会被平台拒收。 */
    static const VoiceVisionProfile* DefaultVisionProfile();
    /* configure_vision 安装三轨 Peer（视频初始 paused，不取帧）。 */
    bool configure_vision(const VoiceMedia& media, const VoiceVisionProfile& profile) override;
    /* begin_vision_control 受理平台 publish/stop：Publish 启动视频管线，Stop 抢占停流。 */
    VoiceVisionRequestResult begin_vision_control(const VoiceVisionControl& control) override;
    /* take_vision_fact 交接视频管线实际发布/停止事实。 */
    std::optional<VoiceVisionFact> take_vision_fact() override;
    /* cancel_vision_control 撤销未完成意图并回收取流。 */
    void cancel_vision_control(std::uint64_t generation) override;

private:
    enum class PeerState { Idle, Offering, Connecting, Connected, Failed };

    /* OpenPeer 创建 libpeer PeerConnection 并生成完整 offer。 */
    bool OpenPeer();
    /* OnAudioTrack / OnIceState 是 libpeer 回调（loop 线程）。 */
    static void OnAudioTrack(uint8_t* data, size_t size, void* userdata);
    static void OnIceState(PeerConnectionState state, void* userdata);
    /* PumpCapture 直接编码16k原始语音帧；仅采集期调用，RTP保持48k时钟。 */
    void PumpCapture();
    /* PumpPlayback 解码下行队列并写入播放端口。 */
    void PumpPlayback();
#if CONFIG_UR_SOFTWARE_AEC
    // DecodeRequest 用代次隔离取消前已提交但尚未完成的压缩帧。
    struct DecodeRequest {
        std::uint64_t generation;
        std::vector<std::uint8_t> opus;
    };
    // EnsureDecoderTask 创建有界解码任务；失败由播放启动路径明确返回。
    bool EnsureDecoderTask();
    // StopDecoderTask 等当前算法调用退出后回收任务，关闭后不再访问codec或本对象。
    void StopDecoderTask();
    // DecoderTask 只解码，不访问libpeer、PCM驱动或会话状态机。
    void DecoderTask();
    // ResetDecodeQueue 作废在途结果并清空旧回复，两次回复不能串音。
    void ResetDecodeQueue();
    // DecoderIdle 同时核对等待、解码中和已解码队列，防止提前上报播放排空。
    bool DecoderIdle();
    // PumpDecodedPlayback 在事件线程提交PCM并回收票据，保留原驱动线程归属。
    void PumpDecodedPlayback();

    std::mutex decoder_mutex_;                  // 只保护有界队列与代次，不跨越算法调用。
    std::condition_variable decoder_cv_;        // 无输入或输出队列满时阻塞任务，避免空转。
    std::deque<DecodeRequest> decode_pending_;  // 最多四帧等待解码，满时保留媒体原始帧。
    std::deque<std::vector<std::int16_t>> decoded_ready_;  // 最多四帧等待事件线程提交驱动。
    std::uint64_t decode_generation_ = 0;          // 每次回复或取消递增，在途旧代次结果直接丢弃。
    bool decoder_shutdown_ = false;                // 关闭时唤醒任务，不再解码或交付帧。
    bool decode_in_flight_ = false;                // 队列搬空但算法尚未返回时仍不算排空。
    TaskHandle_t decoder_task_ = nullptr;          // 只由事件线程创建和关闭。
    SemaphoreHandle_t decoder_stopped_ = nullptr;  // 任务退出算法及队列访问后的完成屏障。
#endif
    /* PumpVideo 把视频管线已编码 NALU 经本 Peer 上行；仅发布期由 poll 调用。 */
    void PumpVideo();

    UrSignalingPcm& pcm_;
    AudioCodecPort& codec_;
    PeerConnection* pc_ = nullptr;
    std::atomic<PeerState> state_{PeerState::Idle};
    bool capture_running_ = false;
    /* capture_armed_ 表示采集队列已提前打开（audioStart 待 ACK）；capture_paused_
     * 表示泵暂停、帧只进入 PCM 缓冲。ACK 成功后两者复位。 */
    bool capture_armed_ = false;
    bool capture_paused_ = false;
    bool connected_once_ = false;
    std::string offer_sdp_;
    bool offer_taken_ = false;
    /* downlink_ 缓存下行 opus 帧（回调写入，poll 消费）。 */
    std::deque<std::vector<std::uint8_t>> downlink_;
    /* downlink_frames_ 累计下行音频帧（验收证据日志用）。 */
    std::atomic<int> downlink_frames_{0};
    std::uint32_t rtp_sequence_base_ = 1;
    std::string input_id_;
    std::uint64_t playback_epoch_ = 0;
    bool playback_running_ = false;
    bool playback_prefilled_ = false;  // 仅AI回复起播暂存，后续帧保持流式播放。
    std::int64_t playback_first_frame_us_ = -1;  // 首帧时间；负值代表尚未收到本轮首帧。
    std::uint64_t drained_epoch_value_ = 0;
    /* playback_frames_ 记录本代实际写入硬件队列的帧数：排空上报必须建立在"本代
     * 真正播放过数据"之上，防止播放起始的瞬时空窗被误判为排空并吃掉单次上报
     * （真机复测实证：回复播完 UI 卡"说话中"）。 */
    std::uint32_t playback_frames_ = 0;
    std::atomic<bool> recovery_needed_{false};
    /* ---- 视频形态状态（CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO）---- */
    /* vision_profile_ 冻结的三轨能力快照；ConfigureVision 前为空。 */
    std::optional<VoiceVisionProfile> vision_profile_;
    /* video_ 为视频上行管线；由外部注入（启动闭包绑定板级相机），无视频形态为空。 */
    UrSignalingVideo* video_ = nullptr;
    /* video_starter_ 封装 Start(camera, frame_source)；由板级装配时注入。 */
    std::function<bool()> video_starter_;
    bool call_video_available_ = false;  // 人际通话的独立 JPEG 相机装配结果。
    /* video_publishing_ 表示平台已 publish 且管线在跑。 */
    bool video_publishing_ = false;
    /* pending_vision_generation_ 关联受理中控制与事实。 */
    std::uint64_t pending_vision_generation_ = 0;
    /* video_connected_frames_ 累计上行视频帧（验收日志）。 */
    int video_uplink_frames_ = 0;
    /* video_send_at_ 上次视频帧发送时刻（微秒）：帧级限速保护 keepalive。 */
    std::atomic<int64_t> video_send_at_{0};
};

}  // namespace ur::device

#endif /* UR_SIGNALING_WEBRTC_MEDIA_H */
