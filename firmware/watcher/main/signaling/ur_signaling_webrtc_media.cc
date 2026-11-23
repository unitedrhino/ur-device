// 文件说明：AI WebRTC 双向媒体泵，含有界收包、起播缓冲及采集播放时序诊断。
#include "ur_signaling_webrtc_media.h"

#include <esp_log.h>
#include <esp_timer.h>

#include <algorithm>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if CONFIG_UR_SOFTWARE_AEC
#include <esp_heap_caps.h>
#include "freertos/idf_additions.h"
#endif

#if CONFIG_UR_AI_SIGNALING_H264_UPLINK
/* 视频形态需要完整类型驱动管线；纯音频构建仅前向声明。 */
#include "ur_signaling_video.h"
#endif

namespace ur::device {
namespace {
constexpr const char* kTag = "ur_sig_rtc";
/* 下行 opus 帧缓存上限：20ms 帧 50fps，100 帧=2 秒缓冲，超限丢最旧保实时。 */
constexpr size_t kDownlinkLimit = 100;
// AFE原生16k/20ms语音输入；Opus的RTP时钟由libpeer保持48k。
constexpr int kIn16kSamples = 320;
// 起播暂存5帧以吸收无线突发；短回复最多等100ms，避免为凑帧数一直静音。
constexpr size_t kPlaybackPrefillFrames = 5;
constexpr std::int64_t kPlaybackPrefillTimeoutUs = 100000;
// 每轮最多收8个连续音频包；空轮立即结束，保留外层任务的调度预算。
constexpr int kReceiveDrainBudget = 8;
#if CONFIG_UR_SOFTWARE_AEC
// 解码等待与PCM结果各限制四帧；在途算法调用最多一帧，不扩张已有网络缓存上限。
constexpr size_t kDecodeQueueLimit = 4;
#endif
}  // namespace

WebRtcVoiceMedia::WebRtcVoiceMedia(UrSignalingPcm& pcm, AudioCodecPort& codec)
    : pcm_(pcm), codec_(codec) {}

WebRtcVoiceMedia::~WebRtcVoiceMedia() { close(); }

bool WebRtcVoiceMedia::OpenPeer() {
    /* 公共 STUN 实测仅 stun.miwifi.com 可达（qq/bilibili 超时），成功约 60-85ms、
     * 失败最长约 1.3s；平台应答端是 Pion ICE lite + host candidate，并从设备
     * 入站 binding request 学习 peer-reflexive 地址，设备 offer 只带 host candidate
     * 即可建连。关闭 STUN 去掉该段抖动；回滚：kEnableStun 改回 true。 */
    static const char* kStunUrls[] = {"stun:stun.miwifi.com:3478"};
    constexpr bool kEnableStun = false;
    ESP_LOGI(kTag, "[connect-ms] stage=pc_create_begin t=%llu",
             static_cast<unsigned long long>(esp_timer_get_time() / 1000));
    PeerConfiguration config = {};
    if (kEnableStun) {
        for (size_t i = 0; i < sizeof(kStunUrls) / sizeof(kStunUrls[0]); ++i) {
            config.ice_servers[i].urls = kStunUrls[i];
        }
    }
    config.audio_codec = CODEC_OPUS;
    /* 视频形态：offer 追加 sendonly H264 m-line（平台 OpenVision 只收不发）。 */
#if CONFIG_UR_AI_SIGNALING_H264_UPLINK
    if (vision_profile_.has_value()) {
        config.video_codec = CODEC_H264;
        /* 三轨分离模式：平台 vision 校验要求 video/mic/speaker 三段方向唯一。 */
        config.separate_audio_tracks = 1;
    }
#endif
    config.datachannel = DATA_CHANNEL_NONE;
    config.onaudiotrack = &WebRtcVoiceMedia::OnAudioTrack;
    config.user_data = this;
    pc_ = peer_connection_create(&config);
    if (pc_ == nullptr) {
        ESP_LOGE(kTag, "peer_connection_create failed");
        return false;
    }
    ESP_LOGI(kTag, "[connect-ms] stage=pc_created t=%llu",
             static_cast<unsigned long long>(esp_timer_get_time() / 1000));
    peer_connection_oniceconnectionstatechange(pc_, &WebRtcVoiceMedia::OnIceState);
    /* 上游 agent 候选收集为同步完成，create_offer 返回含全部候选的完整
     * SDP，与协议 iceMode=complete 语义天然对齐。 */
    ESP_LOGI(kTag, "[connect-ms] stage=ice_gather_begin t=%llu",
             static_cast<unsigned long long>(esp_timer_get_time() / 1000));
    const char* offer = peer_connection_create_offer(pc_);
    if (offer == nullptr) {
        ESP_LOGE(kTag, "create_offer failed");
        peer_connection_destroy(pc_);
        pc_ = nullptr;
        return false;
    }
    ESP_LOGI(kTag, "[connect-ms] stage=ice_gather_end t=%llu",
             static_cast<unsigned long long>(esp_timer_get_time() / 1000));
    offer_sdp_ = offer;
    offer_taken_ = false;
    /* offer 内候选条数：srflx 收集结果的最直接证据（收集失败时仅剩 host 候选）。 */
    int candidate_count = 0;
    for (size_t pos = offer_sdp_.find("a=candidate:"); pos != std::string::npos;
         pos = offer_sdp_.find("a=candidate:", pos + 1)) {
        ++candidate_count;
    }
    ESP_LOGI(kTag, "local offer ready (%d bytes, candidates=%d)",
             static_cast<int>(offer_sdp_.size()), candidate_count);
    ESP_LOGI(kTag, "[connect-ms] stage=offer_ready bytes=%d candidates=%d t=%llu",
             static_cast<int>(offer_sdp_.size()), candidate_count,
             static_cast<unsigned long long>(esp_timer_get_time() / 1000));
    return true;
}

bool WebRtcVoiceMedia::configure(const VoiceMedia& media) {
    ESP_LOGI(kTag, "[connect-ms] stage=media_configure_begin transport=webrtc rate=%d t=%llu",
             media.audio.sample_rate,
             static_cast<unsigned long long>(esp_timer_get_time() / 1000));
    close();
    if (media.transport != VoiceTransport::WebRTC || media.audio.sample_rate != 48000 ||
        media.audio.channels != 1 || media.audio.frame_ms != 20) {
        ESP_LOGE(kTag, "unsupported webrtc media rate=%d ch=%d frame=%d", media.audio.sample_rate,
                 media.audio.channels, media.audio.frame_ms);
        return false;
    }
    if (!OpenPeer()) {
        close();
        return false;
    }
    state_.store(PeerState::Offering);
    ESP_LOGI(kTag, "libpeer pc configured, offer ready");
    ESP_LOGI(kTag, "[connect-ms] stage=media_configure_done transport=webrtc t=%llu",
             static_cast<unsigned long long>(esp_timer_get_time() / 1000));
    return true;
}

std::uint32_t WebRtcVoiceMedia::next_input_sequence() const {
    /* WebRTC 上行序号由 RTP 内部维护；返回非零占位满足 input 关联校验。 */
    return rtp_sequence_base_;
}

bool WebRtcVoiceMedia::prepare_reply(const std::array<std::uint8_t, 8>&, std::uint32_t) {
    /* WebRTC 无 UDP nonce 窗口语义；回复边界由 RTP/播放票据承载。 */
    return true;
}

bool WebRtcVoiceMedia::finish_reply(std::uint32_t) { return true; }

bool WebRtcVoiceMedia::arm_capture(std::string_view input_id) {
    if (capture_armed_) {
        /* 同一输入段的重复 arm 幂等；不同输入段说明旧命令未清理，拒绝重复打开。 */
        return input_id == input_id_;
    }
    if (state_.load() != PeerState::Connected) {
        return false;
    }
    if (!pcm_.begin_capture(input_id)) {
        return false;
    }
    input_id_.assign(input_id);
    capture_armed_ = true;
    capture_paused_ = true;
    /* 体感采集在此刻已开启：队列打开、用户语音开始进入 PCM 缓冲。 */
    ESP_LOGI(kTag, "[connect-ms] stage=capture_driver_start transport=webrtc t=%llu",
             static_cast<unsigned long long>(esp_timer_get_time() / 1000));
    return true;
}

bool WebRtcVoiceMedia::start_capture(std::string_view input_id) {
    /* 已提前 arm：ACK 成功路径只解除暂停，绝不能重复 begin_capture。 */
    if (capture_armed_) {
        if (input_id != input_id_) {
            return false;
        }
        return start_capture_after_ack(input_id);
    }
    if (state_.load() != PeerState::Connected) {
        return false;
    }
    if (!pcm_.begin_capture(input_id)) {
        return false;
    }
    input_id_.assign(input_id);
    capture_running_ = true;
    ESP_LOGI(kTag, "[connect-ms] stage=capture_driver_start transport=webrtc t=%llu",
             static_cast<unsigned long long>(esp_timer_get_time() / 1000));
    return true;
}

bool WebRtcVoiceMedia::start_capture_after_ack(std::string_view input_id) {
    if (!capture_armed_) {
        /* 未提前武装（不支持 arm 的旧路径）：按原逻辑直接开始采集。 */
        return start_capture(input_id);
    }
    if (input_id != input_id_ || state_.load() != PeerState::Connected) {
        /* ACK 属于旧输入段或 Peer 已失效：撤销武装，迟到 ACK 不能重新开麦。 */
        disarm_capture();
        return false;
    }
    capture_armed_ = false;
    capture_paused_ = false;
    capture_running_ = true;
    ESP_LOGI(kTag, "[connect-ms] stage=capture_pump_start transport=webrtc t=%llu",
             static_cast<unsigned long long>(esp_timer_get_time() / 1000));
    return true;
}

void WebRtcVoiceMedia::disarm_capture() {
    if (!capture_armed_ && !capture_paused_ && !capture_running_) {
        return;
    }
    const bool was_armed = capture_armed_ || capture_paused_;
    capture_armed_ = false;
    capture_paused_ = false;
    capture_running_ = false;
    pcm_.end_capture();
    if (was_armed) {
        ESP_LOGI(kTag, "[connect-ms] stage=capture_disarmed transport=webrtc t=%llu",
                 static_cast<unsigned long long>(esp_timer_get_time() / 1000));
    }
}

void WebRtcVoiceMedia::stop_capture() { disarm_capture(); }

bool WebRtcVoiceMedia::start_playback(std::string_view response_id, std::uint64_t epoch) {
    if (state_.load() != PeerState::Connected) {
        return false;
    }
#if CONFIG_UR_SOFTWARE_AEC
    if (!EnsureDecoderTask()) {
        return false;
    }
#endif
    if (!pcm_.begin_playback(response_id, epoch)) {
        return false;
    }
#if CONFIG_UR_SOFTWARE_AEC
    ResetDecodeQueue();
#endif
    playback_epoch_ = epoch;
    playback_running_ = true;
    drained_epoch_value_ = 0;
    playback_frames_ = 0;
    playback_prefilled_ = false;
    playback_first_frame_us_ = -1;
    return true;
}

void WebRtcVoiceMedia::stop_playback(std::uint64_t epoch) {
    if (playback_running_ && epoch == playback_epoch_) {
        playback_running_ = false;
        playback_prefilled_ = false;
        playback_first_frame_us_ = -1;
        downlink_.clear();
#if CONFIG_UR_SOFTWARE_AEC
        ResetDecodeQueue();
#endif
    }
    pcm_.end_playback(epoch);
}

#if CONFIG_UR_SOFTWARE_AEC
// EnsureDecoderTask 返回任务是否可用；解码栈放PSRAM，算法不挤占信令循环。
bool WebRtcVoiceMedia::EnsureDecoderTask() {
    if (decoder_task_ != nullptr) {
        return true;
    }
    decoder_stopped_ = xSemaphoreCreateBinary();
    if (decoder_stopped_ == nullptr) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(decoder_mutex_);
        decoder_shutdown_ = false;
    }
    if (xTaskCreateWithCaps(
            [](void* context) {
                auto* self = static_cast<WebRtcVoiceMedia*>(context);
                const auto stopped = self->decoder_stopped_;  // 退出屏障后不得再访问self。
                self->DecoderTask();
                xSemaphoreGive(stopped);
                vTaskDeleteWithCaps(nullptr);
            },
            "ur_rtc_decode", 24 * 1024, this, 4, &decoder_task_,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        vSemaphoreDelete(decoder_stopped_);
        decoder_stopped_ = nullptr;
        decoder_task_ = nullptr;
        ESP_LOGE(kTag, "decoder task creation failed");
        return false;
    }
    return true;
}

// StopDecoderTask 作废旧帧并等待当前算法退出，回收前不销毁codec或本对象。
void WebRtcVoiceMedia::StopDecoderTask() {
    if (decoder_task_ == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(decoder_mutex_);
        decoder_shutdown_ = true;
        ++decode_generation_;
        decode_pending_.clear();
        decoded_ready_.clear();
    }
    decoder_cv_.notify_all();
    // 不等待网络或事件线程回调；只等当前有界Opus调用及队列访问退出。
    xSemaphoreTake(decoder_stopped_, portMAX_DELAY);
    vSemaphoreDelete(decoder_stopped_);
    decoder_stopped_ = nullptr;
    decoder_task_ = nullptr;
}

// ResetDecodeQueue 非阻塞取消旧回复；在途算法结束后按代次丢弃其结果。
void WebRtcVoiceMedia::ResetDecodeQueue() {
    {
        std::lock_guard<std::mutex> lock(decoder_mutex_);
        ++decode_generation_;
        decode_pending_.clear();
        decoded_ready_.clear();
    }
    decoder_cv_.notify_all();
}

// DecoderIdle 只在等待、在途和结果三处都为空时返回true，供真实排空判定。
bool WebRtcVoiceMedia::DecoderIdle() {
    std::lock_guard<std::mutex> lock(decoder_mutex_);
    return decode_pending_.empty() && decoded_ready_.empty() && !decode_in_flight_;
}

// DecoderTask 串行推进下行算法，输出有界；不访问网络、PCM票据或应用状态。
void WebRtcVoiceMedia::DecoderTask() {
    while (true) {
        DecodeRequest request;  // 搬出短锁后执行算法，不阻塞媒体线程的收包及上行。
        {
            std::unique_lock<std::mutex> lock(decoder_mutex_);
            decoder_cv_.wait(lock, [this] {
                return decoder_shutdown_ ||
                       (!decode_pending_.empty() && decoded_ready_.size() < kDecodeQueueLimit);
            });
            if (decoder_shutdown_) {
                return;
            }
            request = std::move(decode_pending_.front());
            decode_pending_.pop_front();
            decode_in_flight_ = true;
        }
        std::vector<std::int16_t> pcm;  // 解码结果仅在代次仍有效时交给事件线程。
        const bool decoded = codec_.decode(request.opus, pcm);
        {
            std::lock_guard<std::mutex> lock(decoder_mutex_);
            decode_in_flight_ = false;
            if (!decoder_shutdown_ && request.generation == decode_generation_ && decoded &&
                !pcm.empty()) {
                decoded_ready_.push_back(std::move(pcm));
            }
        }
    }
}

// PumpDecodedPlayback 在原事件线程提交最多四帧，驱动背压时保留未提交结果。
void WebRtcVoiceMedia::PumpDecodedPlayback() {
    for (int i = 0; i < 4; ++i) {
        if (pcm_.playback_backpressured(playback_epoch_)) {
            break;
        }
        std::vector<std::int16_t> pcm;  // 驱动提交保持在原事件线程，工作任务不碰播放票据。
        {
            std::lock_guard<std::mutex> lock(decoder_mutex_);
            if (decoded_ready_.empty()) {
                break;
            }
            pcm = std::move(decoded_ready_.front());
            decoded_ready_.pop_front();
        }
        decoder_cv_.notify_all();
        if (pcm_.write_frame(pcm, playback_epoch_)) {
            ++playback_frames_;
            drained_epoch_value_ = 0;
        } else {
            ESP_LOGW(kTag, "playback write failed");
        }
    }
}
#endif

// PumpCapture 有界发送原生16k语音帧；RTP时间戳不随本地输入采样率变化。
void WebRtcVoiceMedia::PumpCapture() {
    if (capture_paused_) {
        /* audioStart ACK 等待期：只把 AFE 分流块搬入 PCM 缓冲，不编码/不上行。
         * 继续排空可避免 AudioService 的 48×10ms 有界队列在 ACK 迟到时丢首字。 */
        pcm_.DrainCaptureWhilePaused();
        return;
    }
    std::vector<std::int16_t> pcm16k;
    /* 每轮最多泵 3 帧（60ms），避免长回复下行饥饿事件线程。 */
    for (int i = 0; i < 3 && capture_running_; ++i) {
        if (pcm_.read_frame(pcm16k) != PcmResult::Frame || pcm16k.size() != kIn16kSamples) {
            return;
        }
#if CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
        // Watcher 的近场弱音在 AFE 后仍偏小。仅 AI 上行增益，并限制峰值避免整数溢出。
        constexpr int kInputGain = 16;
        constexpr int kInputPeak = 30000;
        for (auto& sample : pcm16k) {
            sample = static_cast<std::int16_t>(
                std::clamp(static_cast<int>(sample) * kInputGain, -kInputPeak, kInputPeak));
        }
#endif
        std::vector<std::uint8_t> opus;
        if (!codec_.encode(pcm16k, opus) || opus.empty()) {
            return;
        }
        if (peer_connection_send_audio(pc_, opus.data(), opus.size()) != 0) {
            ESP_LOGW(kTag, "send_audio failed");
            return;
        }
        rtp_sequence_base_ += 1;
    }
}

void WebRtcVoiceMedia::PumpPlayback() {
    if (playback_running_ && !playback_prefilled_ && !downlink_.empty()) {
        const auto now_us = esp_timer_get_time();  // 从首帧到达起计算有界起播等待。
        if (playback_first_frame_us_ < 0)
            playback_first_frame_us_ = now_us;
        if (downlink_.size() < kPlaybackPrefillFrames &&
            now_us - playback_first_frame_us_ < kPlaybackPrefillTimeoutUs)
            return;
        playback_prefilled_ = true;
        ESP_LOGI(kTag, "playback prefill frames=%u wait_ms=%lld",
                 static_cast<unsigned>(downlink_.size()),
                 static_cast<long long>((now_us - playback_first_frame_us_) / 1000));
    }
#if CONFIG_UR_SOFTWARE_AEC
    if (playback_running_) {
        PumpDecodedPlayback();
    }
    for (int i = 0; i < 4 && !downlink_.empty(); ++i) {
        if (!playback_running_) {
            downlink_.pop_front();
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(decoder_mutex_);
            if (decode_pending_.size() >= kDecodeQueueLimit) {
                break;
            }
            decode_pending_.push_back({decode_generation_, std::move(downlink_.front())});
            downlink_.pop_front();
        }
        decoder_cv_.notify_all();
    }
#else
    for (int i = 0; i < 4 && !downlink_.empty(); ++i) {
        // 缓冲满时保留编码帧，避免提前弹出/解码后丢失本轮回复的一部分。
        if (playback_running_ && pcm_.playback_backpressured(playback_epoch_))
            break;
        auto opus = std::move(downlink_.front());
        downlink_.pop_front();
        if (!playback_running_) {
            continue;
        }
        std::vector<std::int16_t> pcm;
        if (!codec_.decode(opus, pcm) || pcm.empty()) {
            continue;
        }
        if (!pcm_.write_frame(pcm, playback_epoch_)) {
            ESP_LOGW(kTag, "playback write failed");
        } else {
            ++playback_frames_;
            /* 上报后又有新帧写入（本代续播）时重新武装，保证真实结尾的空窗仍会上报。 */
            drained_epoch_value_ = 0;
        }
    }
#endif
    /* 排空检测与 UDP 形态对齐：下行队列空 + 硬件播放完成计数满（playback_empty 是
     * completed==submitted 的驱动级完成事实）即可上报排空，不再要求 playback_running_
     * 先复位——该标志的复位恰由排空回调后的 stop_playback 触发，原条件形成互锁，
     * 真机表现为 AI 回复播完 UI 永远卡在"说话中"（tts stop 事件永不发出）。
     * 另需本代已真实写入过帧（playback_frames_>0）：播放起始的空窗曾把单次上报
     * 提前吃掉，真实结尾不再上报，同样卡"说话中"（真机复测实证，勿删该门槛）。 */
    if (downlink_.empty() && playback_frames_ > 0 && drained_epoch_value_ == 0 &&
        playback_epoch_ != 0 && pcm_.playback_empty(playback_epoch_)
#if CONFIG_UR_SOFTWARE_AEC
        && DecoderIdle()
#endif
    ) {
        ESP_LOGI(kTag, "playback drained epoch=%llu",
                 static_cast<unsigned long long>(playback_epoch_));
        drained_epoch_value_ = playback_epoch_;
    }
}

bool WebRtcVoiceMedia::poll(std::uint64_t) {
    if (pc_ == nullptr) {
        return false;
    }
#if CONFIG_UR_SOFTWARE_AEC
    const auto poll_started = esp_timer_get_time();  // 诊断事件泵被采集或播放占用的真实时长。
#endif
    // 仅在已连接且本轮确实收到音频时继续收包；空轮和ICE协商保持单次推进。
    for (int received = 0; received < kReceiveDrainBudget; ++received) {
        const int before = downlink_frames_.load();  // 判断是否有真实音频进展，不空转等待8次。
        peer_connection_loop(pc_);
        if (state_.load() != PeerState::Connected || downlink_frames_.load() == before)
            break;
    }
#if CONFIG_UR_SOFTWARE_AEC
    const auto received_at = esp_timer_get_time();  // 收包结束点，与编码耗时分开计量。
#endif
    if (state_.load() == PeerState::Connected) {
        PumpCapture();
    }
#if CONFIG_UR_SOFTWARE_AEC
    const auto captured_at = esp_timer_get_time();  // 上行重采样与编码结束点。
#endif
    PumpPlayback();
#if CONFIG_UR_SOFTWARE_AEC
    const auto played_at = esp_timer_get_time();  // 下行解码与播放提交结束点。
    /* 单一AI媒体实例的低频诊断，只记录时间和计数，不读取或输出音频正文。 */
    static std::int64_t window_at = 0, previous_at = 0, receive_sum = 0,
                        capture_sum = 0, playback_sum = 0, gap_max = 0;
    static unsigned polls = 0;
    if (capture_running_ && playback_running_) {
        if (window_at == 0) window_at = poll_started;
        if (previous_at && poll_started - previous_at > gap_max)
            gap_max = poll_started - previous_at;
        previous_at = poll_started;
        receive_sum += received_at - poll_started;
        capture_sum += captured_at - received_at;
        playback_sum += played_at - captured_at;
        ++polls;
        if (played_at - window_at >= 1000000) {
            unsigned decode_wait, decode_ready;  // 每秒快照区分解码算法积压与事件线程提交积压。
            bool decode_busy;                    // 在途算法也计入状态，空队列不等于解码已结束。
            {
                std::lock_guard<std::mutex> lock(decoder_mutex_);
                decode_wait = static_cast<unsigned>(decode_pending_.size());
                decode_ready = static_cast<unsigned>(decoded_ready_.size());
                decode_busy = decode_in_flight_;
            }
            ESP_LOGI(kTag,
                     "duplex timing polls=%u rx_avg_us=%lld cap_avg_us=%lld play_avg_us=%lld "
                     "gap_max_us=%lld queued=%u decode_wait=%u decode_ready=%u decode_busy=%d",
                     polls, static_cast<long long>(receive_sum / polls),
                     static_cast<long long>(capture_sum / polls),
                     static_cast<long long>(playback_sum / polls), static_cast<long long>(gap_max),
                     static_cast<unsigned>(downlink_.size()), decode_wait, decode_ready,
                     decode_busy);
            window_at = played_at;
            receive_sum = capture_sum = playback_sum = gap_max = polls = 0;
        }
    } else {
        window_at = previous_at = receive_sum = capture_sum = playback_sum = gap_max = polls = 0;
    }
#endif
    PumpVideo();
    return true;
}

bool WebRtcVoiceMedia::recovery_needed() const { return recovery_needed_.load(); }

std::optional<std::uint64_t> WebRtcVoiceMedia::drained_epoch() const {
    if (drained_epoch_value_ != 0) {
        return drained_epoch_value_;
    }
    return std::nullopt;
}

std::optional<std::string> WebRtcVoiceMedia::take_offer() {
    if (offer_sdp_.empty() || offer_taken_) {
        return std::nullopt;
    }
    offer_taken_ = true;
    return offer_sdp_;
}

bool WebRtcVoiceMedia::apply_answer(std::string_view sdp) {
    if (pc_ == nullptr || sdp.empty()) {
        return false;
    }
    std::string owned(sdp);
    peer_connection_set_remote_description(pc_, owned.c_str(), SDP_TYPE_ANSWER);
    state_.store(PeerState::Connecting);
    ESP_LOGI(kTag, "[connect-ms] stage=media_answer_applied bytes=%d t=%llu",
             static_cast<int>(sdp.size()),
             static_cast<unsigned long long>(esp_timer_get_time() / 1000));
    return true;
}

bool WebRtcVoiceMedia::begin_recovery(std::uint64_t) {
    recovery_needed_.store(false);
    connected_once_ = false;
    /* 新 Peer 代次不继承旧代次的提前采集：audioStart ACK 属于旧媒体，
     * 迟到后也不能重新打开麦克风。 */
    disarm_capture();
    state_.store(PeerState::Idle);
    if (pc_ != nullptr) {
        peer_connection_destroy(pc_);
        pc_ = nullptr;
    }
    if (!OpenPeer()) {
        return false;
    }
    state_.store(PeerState::Offering);
    return true;
}

void WebRtcVoiceMedia::close() {
#if CONFIG_UR_SOFTWARE_AEC
    StopDecoderTask();
#endif
    /* 会话关闭也可能发生在"已 arm、ACK 未到"的窗口：Session 尚未记录采集，
     * 只能由媒体层负责关队列并清空缓冲。 */
    disarm_capture();
    capture_running_ = false;
    playback_running_ = false;
#if CONFIG_UR_AI_SIGNALING_H264_UPLINK
    /* 视频取流随会话关闭立即停止；核心侧 cancel 不区分 publish/stop 意图。
     * 纯音频形态（VIDEO=0）仅前向声明 UrSignalingVideo，不能调用其方法。 */
    if (video_ != nullptr && video_->Running()) {
        video_->Stop();
    }
#endif
    video_publishing_ = false;
    pending_vision_generation_ = 0;
    if (pc_ != nullptr) {
        peer_connection_destroy(pc_);
        pc_ = nullptr;
    }
    /* 会话结束必须复位共享 PCM 播放代次；Session随runtime重建后epoch从1计数，
     * 残留 epoch_/last_epoch_ 会让新会话首个 begin_playback 被拒
     * （真机 respAudioStart 即失败的 signalClientDriver 根因）。 */
    pcm_.reset_playback_epochs();
    downlink_.clear();
    downlink_frames_ = 0;
    offer_sdp_.clear();
    offer_taken_ = false;
    recovery_needed_.store(false);
    connected_once_ = false;
    drained_epoch_value_ = 0;
    playback_epoch_ = 0;
    state_.store(PeerState::Idle);
}

void WebRtcVoiceMedia::SetVideoPipeline(UrSignalingVideo* video, std::function<bool()> starter) {
    video_ = video;
    video_starter_ = std::move(starter);
}

const VoiceVisionProfile* WebRtcVoiceMedia::vision_profile() const {
#if CONFIG_UR_AI_SIGNALING_H264_UPLINK
    /* 摄像头未注入或管线缺失时对外无视觉能力，能力上报不带 aiVision。 */
    if (video_ == nullptr) {
        return nullptr;
    }
    return DefaultVisionProfile();
#else
    return nullptr;
#endif
}

const VoiceVisionProfile* WebRtcVoiceMedia::call_video_profile() const {
    if (const auto* profile = vision_profile()) return profile;
    return call_video_available_ ? DefaultVisionProfile() : nullptr;
}

const VoiceVisionProfile* WebRtcVoiceMedia::DefaultVisionProfile() {
#if CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO
    static const VoiceVisionProfile kProfile = [] {
        VoiceVisionProfile profile;
        profile.camera_id = "front";
        profile.video_track = "vision-1";
        profile.max_publishers = 1;
        profile.ready = true;
        return profile;
    }();
    return &kProfile;
#else
    return nullptr;
#endif
}

bool WebRtcVoiceMedia::configure_vision(const VoiceMedia& media,
                                        const VoiceVisionProfile& profile) {
#if CONFIG_UR_AI_SIGNALING_H264_UPLINK
    if (video_ == nullptr) {
        return false;
    }
    /* 先冻结三轨快照再建 Peer：OpenPeer 依此在 offer 追加 H264 m-line。 */
    vision_profile_ = profile;
    if (!configure(media)) {
        vision_profile_.reset();
        return false;
    }
    ESP_LOGI(kTag, "vision configured camera=%s track=%s", profile.camera_id.c_str(),
             profile.video_track.c_str());
    return true;
#else
    (void)media;
    (void)profile;
    return false;
#endif
}

VoiceVisionRequestResult WebRtcVoiceMedia::begin_vision_control(const VoiceVisionControl& control) {
#if CONFIG_UR_AI_SIGNALING_H264_UPLINK
    ESP_LOGI(kTag, "vision control op=%d state=%d", static_cast<int>(control.operation),
             static_cast<int>(state_.load()));
    if (video_ == nullptr || state_.load() != PeerState::Connected) {
        ESP_LOGW(kTag, "vision control rejected: video=%d state", video_ != nullptr);
        return VoiceVisionRequestResult::Unsupported;
    }
    if (control.operation == VoiceVisionOperation::Publish) {
        if (video_publishing_) {
            return VoiceVisionRequestResult::Busy;
        }
        /* 事实在管线稳定出帧后由 take_vision_fact 交付；此处仅受理。 */
        pending_vision_generation_ = control.generation;
        video_publishing_ = true;
        ESP_LOGI(kTag, "vision publish accepted gen=%llu",
                 static_cast<unsigned long long>(control.generation));
        return VoiceVisionRequestResult::Accepted;
    }
    /* Stop 抢占未完成 Publish：管线立即停流并交 Stopped 事实。 */
    ESP_LOGI(kTag, "vision stop gen=%llu", static_cast<unsigned long long>(control.generation));
    video_->Stop();
    video_publishing_ = false;
    pending_vision_generation_ = control.generation;
    return VoiceVisionRequestResult::Accepted;
#else
    (void)control;
    return VoiceVisionRequestResult::Unsupported;
#endif
}

std::optional<VoiceVisionFact> WebRtcVoiceMedia::take_vision_fact() {
#if CONFIG_UR_AI_SIGNALING_H264_UPLINK
    if (pending_vision_generation_ == 0 || !video_publishing_) {
        return std::nullopt;
    }
    /* 首个受理意图以管线真实启动为事实：启动失败交 Failed(MediaFailed)。 */
    VoiceVisionFact fact;
    fact.generation = pending_vision_generation_;
    ESP_LOGI(kTag, "vision fact gen=%llu running=%d",
             static_cast<unsigned long long>(pending_vision_generation_), video_->Running());
    if (video_->Running() || (video_starter_ && video_starter_())) {
        fact.state = VoiceVisionState::Active;
        fact.failure = VoiceVisionFailure::None;
    } else {
        fact.state = VoiceVisionState::Failed;
        fact.failure = VoiceVisionFailure::MediaFailed;
        video_publishing_ = false;
    }
    pending_vision_generation_ = 0;
    return fact;
#else
    return std::nullopt;
#endif
}

void WebRtcVoiceMedia::cancel_vision_control(std::uint64_t generation) {
#if CONFIG_UR_AI_SIGNALING_H264_UPLINK
    if (pending_vision_generation_ == generation) {
        pending_vision_generation_ = 0;
    }
    if (video_ != nullptr && video_->Running()) {
        video_->Stop();
    }
    video_publishing_ = false;
#else
    (void)generation;
#endif
}

void WebRtcVoiceMedia::PumpVideo() {
#if CONFIG_UR_AI_SIGNALING_H264_UPLINK
    if (!video_publishing_ || video_ == nullptr || pc_ == nullptr ||
        state_.load() != PeerState::Connected) {
        return;
    }
    /* 帧级限速与大小上限：IDR 大帧（数百 FU-A 分片）单次同步发送会阻塞
     * libpeer socket 数百毫秒，STUN keepalive 停发导致平台 Pion 判
     * disconnected（真机 attach 后 4 秒断链根因）。发送间隔 ≥400ms，
     * 超过 8KB 的帧直接丢弃等下一个较小的帧。 */
    /* 仅"距上次发送不足400ms"才跳过；首次必须放行（同 ur_signaling_call PumpVideo 修复）。 */
    if (video_send_at_.load() && esp_timer_get_time() - video_send_at_.load() < 400 * 1000LL) {
        return;
    }
    std::vector<std::uint8_t> nalu;
    if (video_->TakeFrame(nalu)) {
        if (nalu.size() > 8192) {
            return;
        }
        video_send_at_.store(esp_timer_get_time());
        if (peer_connection_send_video(pc_, nalu.data(), nalu.size()) != 0) {
            ESP_LOGW(kTag, "send_video failed");
            return;
        }
        ++video_uplink_frames_;
        if (video_uplink_frames_ % 10 == 1) {
            ESP_LOGI(kTag, "uplink video frame #%d size=%d", video_uplink_frames_,
                     static_cast<int>(nalu.size()));
        }
    }
#endif
}

void WebRtcVoiceMedia::OnAudioTrack(uint8_t* data, size_t size, void* userdata) {
    auto* self = static_cast<WebRtcVoiceMedia*>(userdata);
    if (data == nullptr || size == 0) {
        return;
    }
    /* 下行音频帧计数（每 50 帧一条）：语音轮次验收的 TTS 通道证据。 */
    ++self->downlink_frames_;
    if (self->downlink_frames_ % 50 == 1) {
        ESP_LOGI(kTag, "downlink audio frame #%d size=%d", self->downlink_frames_.load(),
                 static_cast<int>(size));
    }
    if (self->downlink_.size() >= kDownlinkLimit) {
        self->downlink_.pop_front();
    }
    self->downlink_.emplace_back(data, data + size);
}

void WebRtcVoiceMedia::OnIceState(PeerConnectionState state, void* userdata) {
    auto* self = static_cast<WebRtcVoiceMedia*>(userdata);
    ESP_LOGI(kTag, "libpeer state -> %d", static_cast<int>(state));
    ESP_LOGI(kTag, "[connect-ms] stage=peer_state state=%d t=%llu", static_cast<int>(state),
             static_cast<unsigned long long>(esp_timer_get_time() / 1000));
    switch (state) {
        case PEER_CONNECTION_CONNECTED:
            self->state_.store(PeerState::Connected);
            self->connected_once_ = true;
            self->recovery_needed_.store(false);
            break;
        /* 已连接后的媒体断开必须激活恢复链：libpeer 在 STUN 保活超时（15 秒无响应）
         * 时回调 CLOSED(5)（peer_connection.c keepalive 分支），ICE 暂断回调
         * DISCONNECTED(3)，连接建立期失败才是 FAILED(4)。此前仅处理 FAILED，
         * CLOSED 落入 default 被忽略——recovery_needed 永不置位，会话静默到
         * 平台恢复窗口超时终结，设备侧无法发起重协商（真机媒体断开根因）。 */
        case PEER_CONNECTION_DISCONNECTED:
        case PEER_CONNECTION_FAILED:
        case PEER_CONNECTION_CLOSED:
            if (self->connected_once_) {
                self->recovery_needed_.store(true);
                self->state_.store(PeerState::Connecting);
            } else {
                self->state_.store(PeerState::Failed);
            }
            break;
        default:
            break;
    }
}

}  // namespace ur::device
