// 文件说明：统一麦克风/扬声器任务与有界信令队列，独立采集通过AFE通知取帧。
#ifndef AUDIO_SERVICE_H
#define AUDIO_SERVICE_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>

#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <model_path.h>
#include "esp_ae_rate_cvt.h"
#include "esp_audio_enc.h"
#include "esp_audio_types.h"
#include "esp_opus_dec.h"
#include "esp_opus_enc.h"

#include "audio_codec.h"
#include "audio_debugger.h"
#include "audio_engine.h"
#include "fixed_queue.h"
#include "ogg_demuxer.h"
#include "protocol.h"
#if CONFIG_UR_SPARKBOT_VOICE_MODEL_TEST
#include "call_voice/call_voice_worker.h"
#endif

/*
 * There are two types of audio data flow:
 * 1. (MIC) -> [Audio Engine] -> {Encode Queue} -> [Opus Encoder] -> {Send Queue} -> (Server)
 * 2. (Server) -> {Decode Queue} -> [Opus Decoder] -> {Playback Queue} -> (Speaker)
 *
 * We use dedicated tasks for input, output, and Opus encoding/decoding.
 *
 * Decode Queue and Send Queue are the main queues, because Opus packets are quite smaller than PCM
 * packets.
 *
 */

#define OPUS_FRAME_DURATION_MS 60
#define MAX_ENCODE_TASKS_IN_QUEUE 2
#define MAX_PLAYBACK_TASKS_IN_QUEUE 2
#define MAX_DECODE_PACKETS_IN_QUEUE (1200 / OPUS_FRAME_DURATION_MS)
#define MAX_SEND_PACKETS_IN_QUEUE (2400 / OPUS_FRAME_DURATION_MS)
#define AUDIO_TESTING_MAX_DURATION_MS 10000
#define MAX_TIMESTAMPS_IN_QUEUE 3

#define AUDIO_POWER_TIMEOUT_MS 15000
#define AUDIO_POWER_CHECK_INTERVAL_MS 1000

#define AS_EVENT_AUDIO_TESTING_RUNNING (1 << 0)
#define AS_EVENT_WAKE_WORD_RUNNING (1 << 1)
#define AS_EVENT_AUDIO_PROCESSOR_RUNNING (1 << 2)
#define AS_EVENT_AUDIO_INPUT_STOP_REQUEST (1 << 4)

#define AS_OPUS_GET_FRAME_DRU_ENUM(duration_ms)                  \
    ((duration_ms) == 5     ? ESP_OPUS_ENC_FRAME_DURATION_5_MS   \
     : (duration_ms) == 10  ? ESP_OPUS_ENC_FRAME_DURATION_10_MS  \
     : (duration_ms) == 20  ? ESP_OPUS_ENC_FRAME_DURATION_20_MS  \
     : (duration_ms) == 40  ? ESP_OPUS_ENC_FRAME_DURATION_40_MS  \
     : (duration_ms) == 60  ? ESP_OPUS_ENC_FRAME_DURATION_60_MS  \
     : (duration_ms) == 80  ? ESP_OPUS_ENC_FRAME_DURATION_80_MS  \
     : (duration_ms) == 100 ? ESP_OPUS_ENC_FRAME_DURATION_100_MS \
     : (duration_ms) == 120 ? ESP_OPUS_ENC_FRAME_DURATION_120_MS \
                            : -1)

#define AS_OPUS_ENC_CONFIG()                                                                   \
    {                                                                                          \
        .sample_rate = ESP_AUDIO_SAMPLE_RATE_16K,                                              \
        .channel = ESP_AUDIO_MONO,                                                             \
        .bits_per_sample = ESP_AUDIO_BIT16,                                                    \
        .bitrate = ESP_OPUS_BITRATE_AUTO,                                                      \
        .frame_duration =                                                                      \
            (esp_opus_enc_frame_duration_t)AS_OPUS_GET_FRAME_DRU_ENUM(OPUS_FRAME_DURATION_MS), \
        .application_mode = ESP_OPUS_ENC_APPLICATION_AUDIO,                                    \
        .complexity = 0,                                                                       \
        .enable_fec = false,                                                                   \
        .enable_dtx = true,                                                                    \
        .enable_vbr = true,                                                                    \
    }

struct AudioServiceCallbacks {
    std::function<void(void)> on_send_queue_available;
    std::function<void(const std::string&)> on_wake_word_detected;
    std::function<void(bool)> on_vad_change;
    std::function<void(void)> on_audio_testing_queue_full;
    // Fired when the decode/playback queues and their in-flight work are drained.
    std::function<void(void)> on_playback_drained;
    std::function<void(uint32_t playback_id, uint32_t media_position_ms)> on_playback_progress;
};

enum AudioTaskType {
    kAudioTaskTypeEncodeToSendQueue,
    kAudioTaskTypeEncodeToTestingQueue,
    kAudioTaskTypeDecodeToPlaybackQueue,
};

struct AudioTask {
    AudioTaskType type;
    std::vector<int16_t> pcm;
    uint32_t timestamp = 0;
    uint32_t playback_id = 0;
    uint32_t media_position_ms = 0;
    // 信令旁路播放所属的会话代次；迟到的驱动完成回执不得进入下一通电话。
    uint32_t signaling_generation = 0;
};

struct DebugStatistics {
    uint32_t input_count = 0;
    uint32_t decode_count = 0;
    uint32_t encode_count = 0;
    uint32_t playback_count = 0;
    uint32_t encode_drop_count = 0;
};

class AudioService {
public:
    AudioService();
    ~AudioService();

    void Initialize(AudioCodec* codec);
    void Start();
    void Stop();
    void EncodeWakeWord();
    std::unique_ptr<AudioStreamPacket> PopWakeWordPacket();
    const std::string& GetLastWakeWord() const;
    bool IsVoiceDetected() const { return voice_detected_; }
    bool IsIdle();
    bool IsPlaybackIdle();
    bool IsWakeWordRunning() const {
        return xEventGroupGetBits(event_group_) & AS_EVENT_WAKE_WORD_RUNNING;
    }
    bool IsAudioProcessorRunning() const {
        return xEventGroupGetBits(event_group_) & AS_EVENT_AUDIO_PROCESSOR_RUNNING;
    }
    bool IsAfeWakeWord();

    void EnableWakeWordDetection(bool enable);
#if CONFIG_UR_SPARKBOT_VOICE_MODEL_TEST
    // BeginCallVoiceCapture 仅由应用任务调用；仅空闲唤醒采集运行时启动指定来电的试验识别。
    bool BeginCallVoiceCapture(std::uint32_t generation, CallVoiceWorker::Completion completion);
    // StopCallVoiceCapture 禁止后续送帧并异步停止模型，调用方须等待 Busy 为假再启用通话媒体。
    void StopCallVoiceCapture();
    bool IsCallVoiceCaptureBusy() const;
#endif
    void ReleaseWakeWordResources();
    void EnableVoiceProcessing(bool enable);
    void EnableAudioTesting(bool enable);
    void EnableDeviceAec(bool enable);
    // IsDeviceAecEnabled 仅供应用任务保存/恢复人际接管前的AEC设置，返回当前控制值。
    bool IsDeviceAecEnabled() const { return device_aec_enabled_; }

    /* SuspendDecoder 释放旧协议 Opus 编码器与解码器给信令语音端口腾内部内存
     * （两者都常驻且各占一块内部堆，只关解码器仍会令信令端 open 失败）；
     * ResumeDecoder 幂等重开。播放提示音等用途在信令会话期间由
     * PushSignalingPcm 路径承载，两者运行时互斥。 */
    void SuspendDecoder();
    void ResumeDecoder();

    /* 以下接口仅服务统一信令语音链路（CONFIG_UR_AI_SIGNALING）；旧协议不使用。 */
    /* EnableSignalingCapture 打开后 AFE 输出改入信令队列，不再进入 60ms 编码管线。 */
    void EnableSignalingCapture(bool enable);
    // EnableLowLatencyCapture 由应用任务为人际通话选择原生块及时交付；退出恢复初始化默认值。
    void EnableLowLatencyCapture(bool enable);
#if CONFIG_UR_AI_SIGNALING_WEBRTC
    /* InjectSignalingCapture 按 AFE 10ms 块节奏注入调试语料（sig_speak）：
     * 仅在采集开启时接受，与真实麦克风路径同一队列与上限语义。 */
    bool InjectSignalingCapture(const std::vector<int16_t>& pcm);
    /* SetSignalingMicMute 仅调试注入期间屏蔽真实麦克风帧：麦克风与注入同队列同
     * 1x 节奏，同时灌入会以 2x 速率积压（真机表现为注入语料整段滞留未上行），
     * 注入测试时须独占队列。产线路径不使用。 */
    void SetSignalingMicMute(bool mute) {
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        signaling_mic_muted_ = mute;
    }
    /* InjectMicInput 将16k单声道调试块排入AFE输入；true只表示本地队列受理。
     * 队列间隙仍可回到物理麦克风读取，不能据注入块数断言独占源时钟或真人ASR等价。
     * 仅在信令采集打开时使用，验收须另外核对实际采样和识别结果。 */
    bool InjectMicInput(const std::vector<int16_t>& pcm) {
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        if (!signaling_capture_enabled_) {
            return false;
        }
        if (signaling_mic_inject_queue_.full()) {
            signaling_mic_inject_queue_.pop_front();
        }
        signaling_mic_inject_queue_.push_back(pcm);
        return true;
    }
    /* SignalingCaptureEnabled 供调试注入等待信令会话开麦（audioStart 后）。 */
    bool SignalingCaptureEnabled() {
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        return signaling_capture_enabled_;
    }
#endif
    /* PopSignalingPcm 经互斥队列领取一个16k单声道AFE块；空返回false。 */
    bool PopSignalingPcm(std::vector<int16_t>& pcm);
#if CONFIG_UR_AI_SIGNALING
    // WaitSignalingPcm 供独立采集任务限时等AFE块；不消费样本，关闭或无数据时最多等待timeout。
    bool WaitSignalingPcm(std::chrono::milliseconds timeout);
#endif
#if CONFIG_UR_AI_SIGNALING
    // TraceSignalingCapture 显式读取16k入口、AFE输出及消费/丢旧样本累计值，供同窗口差分。
    void TraceSignalingCapture();
#endif
    /* SignalingPlaybackQueueHasSpace 报告信令播放队列是否还能接收一帧。 */
    bool SignalingPlaybackQueueHasSpace();
    /* PushSignalingPcm 仅由信令事件线程提交20ms PCM；转换不占共享队列锁。
     * 返回是否接纳；ticket 在 OutputData 返回后回投，不代表声学播放完成。 */
    bool PushSignalingPcm(std::vector<int16_t>&& pcm, uint32_t ticket);
    /* PopSignalingPlaybackDone 在事件线程领取已实际写出的播放帧 ticket。 */
    bool PopSignalingPlaybackDone(uint32_t& ticket);
    /* ResetSignalingPlayback 等待在途帧收尾并清理信令播放/完成队列。 */
    void ResetSignalingPlayback();

    void SetCallbacks(AudioServiceCallbacks& callbacks);

    bool PushPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket> packet, bool wait = false);
    std::unique_ptr<AudioStreamPacket> PopPacketFromSendQueue();
    void PlaySound(const std::string_view& sound);
    bool ReadAudioData(std::vector<int16_t>& data, int sample_rate, int samples);
    void ResetDecoder();
    void SetModelsList(srmodel_list_t* models_list);

private:
    AudioCodec* codec_ = nullptr;
    AudioServiceCallbacks callbacks_;
    std::unique_ptr<AudioEngine> audio_engine_;
#if CONFIG_UR_SPARKBOT_VOICE_MODEL_TEST
    std::atomic<CallVoiceWorker*> call_voice_worker_{nullptr};
    std::atomic<std::uint32_t> call_voice_producers_{0};
    bool call_voice_capture_armed_ = false;  // 应用任务独占；重复停止不得重置后续通话音频。
#endif
    std::unique_ptr<AudioDebugger> audio_debugger_;
    void* opus_encoder_ = nullptr;
    void* opus_decoder_ = nullptr;
    std::mutex decoder_mutex_;
    std::mutex input_resampler_mutex_;
    esp_ae_rate_cvt_handle_t input_resampler_ = nullptr;
    esp_ae_rate_cvt_handle_t output_resampler_ = nullptr;

    // Encoder/Decoder state
    int encoder_sample_rate_ = 16000;
    int encoder_duration_ms_ = OPUS_FRAME_DURATION_MS;
    int encoder_frame_size_ = 0;
    int encoder_outbuf_size_ = 0;
    int decoder_sample_rate_ = 0;
    int decoder_duration_ms_ = OPUS_FRAME_DURATION_MS;
    int decoder_frame_size_ = 0;
    DebugStatistics debug_statistics_;
    int64_t last_encode_drop_log_time_ = 0;
    srmodel_list_t* models_list_ = nullptr;

    EventGroupHandle_t event_group_;

    // Audio encode / decode
    TaskHandle_t audio_input_task_handle_ = nullptr;
    TaskHandle_t audio_output_task_handle_ = nullptr;
    TaskHandle_t opus_codec_task_handle_ = nullptr;
    std::mutex audio_queue_mutex_;
    std::condition_variable audio_queue_cv_;
    // Testing records up to AUDIO_TESTING_MAX_DURATION_MS, then swaps into the
    // decode queue. Both queues must share this capacity so swap() is valid.
    static constexpr size_t kAudioTestingPacketCapacity =
        AUDIO_TESTING_MAX_DURATION_MS / OPUS_FRAME_DURATION_MS;
    FixedQueue<std::unique_ptr<AudioStreamPacket>, kAudioTestingPacketCapacity> audio_decode_queue_;
    FixedQueue<std::unique_ptr<AudioStreamPacket>, MAX_SEND_PACKETS_IN_QUEUE> audio_send_queue_;
    FixedQueue<std::unique_ptr<AudioStreamPacket>, kAudioTestingPacketCapacity>
        audio_testing_queue_;
    FixedQueue<AudioTask, MAX_ENCODE_TASKS_IN_QUEUE> audio_encode_queue_;
    FixedQueue<AudioTask, MAX_PLAYBACK_TASKS_IN_QUEUE> audio_playback_queue_;
    bool decode_in_flight_ = false;
    bool output_in_flight_ = false;
    bool playback_drained_notified_ = true;
    uint32_t playback_generation_ = 0;
    // For server AEC
    FixedQueue<uint32_t, MAX_TIMESTAMPS_IN_QUEUE> timestamp_queue_;

    bool audio_engine_initialized_ = false;
    bool voice_detected_ = false;
#if CONFIG_USE_DEVICE_AEC
    bool device_aec_enabled_ = true;
#else
    bool device_aec_enabled_ = false;
#endif
    std::atomic<bool> service_stopped_{true};
    std::atomic<bool> audio_input_need_warmup_{false};

    /* 统一信令语音的旁路队列；仅在 CONFIG_UR_AI_SIGNALING 构建使用。
     * 播放队列压到 8 帧（160ms）：通话追求低延迟，16 帧（320ms）会明显拖后。 */
    FixedQueue<std::vector<int16_t>, 48> signaling_capture_queue_;
#if CONFIG_UR_AI_SIGNALING
    std::condition_variable signaling_capture_cv_;  // AFE出帧后唤醒消费者，独立于网络事件循环。
#endif
    // 人际通话优先实时性；4 个 20ms 帧足够吸收短抖动又不会长期播放旧声音。
    FixedQueue<AudioTask, 4> signaling_playback_queue_;
    FixedQueue<uint32_t, 24> signaling_done_queue_;
    uint32_t signaling_playback_generation_ = 1;  // 信令播放会话代次，跨通话递增。
    bool signaling_output_in_flight_ = false;      // 信令帧是否已移交驱动但尚未返回。
#if CONFIG_UR_AI_SIGNALING
    // 32位样本计数按模累计，显式诊断差分使用；各生产线程无锁递增，不记录PCM内容。
    std::atomic<uint32_t> signaling_input_samples_{0}, signaling_output_samples_{0};
    std::atomic<uint32_t> signaling_taken_samples_{0}, signaling_drop_samples_{0};
#endif
    bool signaling_capture_enabled_ = false;
    bool signaling_mic_muted_ = false; /* 仅调试注入独占队列；产线恒为 false。 */
    FixedQueue<std::vector<int16_t>, 48> signaling_mic_inject_queue_; /* 调试语料喂 AFE 输入端。 */
    /* 信令下行 16k 与设备输出率之间的重采样；两个方向各自单线程使用。 */
    esp_ae_rate_cvt_handle_t signaling_output_resampler_ = nullptr;   /* 仅事件线程。 */
    esp_ae_rate_cvt_handle_t signaling_reference_resampler_ = nullptr; /* 仅播放任务。 */
    // 独立实例各自串行化，避免下行转换阻塞播放任务的AEC参考。
    std::mutex signaling_output_resampler_mutex_;  // 保护下行重采样器。
    std::mutex signaling_reference_resampler_mutex_;  // 保护参考重采样器。

    esp_timer_handle_t audio_power_timer_ = nullptr;
    std::chrono::steady_clock::time_point last_input_time_;
    std::chrono::steady_clock::time_point last_output_time_;

    void AudioInputTask();
    void AudioOutputTask();
    void OpusCodecTask();
    void PushTaskToEncodeQueue(AudioTaskType type, std::vector<int16_t>&& pcm);
    bool InitializeAudioEngine();
    void SetDecodeSampleRate(int sample_rate, int frame_duration);
    void CheckAndUpdateAudioPowerState();
    bool IsPlaybackDrainedLocked() const;
    bool MarkPlaybackDrainedLocked();
};

#endif
