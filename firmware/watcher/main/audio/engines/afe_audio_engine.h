// 文件说明：AFE语音引擎及软件播放参考缓冲；参考锁不跨越DSP喂音调用。
#ifndef AFE_AUDIO_ENGINE_H
#define AFE_AUDIO_ENGINE_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <esp_afe_sr_models.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>

#include "audio_engine.h"
#include "wake_words/wake_word_audio_cache.h"

class CustomWakeWord;

class AfeAudioEngine : public AudioEngine {
public:
    AfeAudioEngine();
    ~AfeAudioEngine() override;

    bool Initialize(AudioCodec* codec, int frame_duration_ms, srmodel_list_t* models_list) override;
    void Feed(std::vector<int16_t>&& data) override;
    void FeedReference(std::vector<int16_t>&& data) override;

    void EnableWakeWordDetection(bool enable) override;
    void EnableVoiceProcessing(bool enable) override;
    void EnableDeviceAec(bool enable) override;
    // EnableLowLatencyOutput 在原生块交付与初始化默认粒度间切换，缓冲由输出任务复位。
    void EnableLowLatencyOutput(bool enable) override;

    bool HasWakeWord() const override;
    bool IsWakeWordDetectionEnabled() const override;
    bool IsVoiceProcessingEnabled() const override;
    bool IsAfeWakeWord() const override { return HasWakeWord(); }
    size_t GetFeedSize() const override;

    void OnWakeWordDetected(std::function<void(const std::string& wake_word)> callback) override;
    void OnOutput(std::function<void(std::vector<int16_t>&& data)> callback) override;
    void OnVadStateChange(std::function<void(bool speaking)> callback) override;
#if CONFIG_UR_SPARKBOT_VOICE_MODEL_TEST
    // SetCallVoiceFrameHook 在处理任务启动前注册借用帧回调；回调不得阻塞或保存样本指针。
    void SetCallVoiceFrameHook(std::function<void(const int16_t*, std::size_t, std::uint32_t)> hook);
    // SetCallVoiceCapture 开关试验采样，重置 AFE 代次以隔离来电之前的音频。
    void SetCallVoiceCapture(bool enabled, std::uint32_t generation);
#endif

    void EncodeWakeWordData() override;
    bool GetWakeWordOpus(std::vector<uint8_t>& opus) override;
    const std::string& GetLastDetectedWakeWord() const override { return last_detected_wake_word_; }

private:
    enum class WakeDetector {
        kNone,
        kWakeNet,
        kMultiNet,
    };

    static constexpr EventBits_t kWakeWordEnabled = 1 << 0;
    static constexpr EventBits_t kVoiceProcessingEnabled = 1 << 1;
    static constexpr EventBits_t kAfeActive = 1 << 2;
    static constexpr size_t kProcessingTaskStackSize = 4096;

    AudioCodec* codec_ = nullptr;
    srmodel_list_t* models_ = nullptr;
    bool owns_models_ = false;
    const esp_afe_sr_iface_t* afe_iface_ = nullptr;
    esp_afe_sr_data_t* afe_data_ = nullptr;
    EventGroupHandle_t event_group_ = nullptr;
    TaskHandle_t processing_task_ = nullptr;
    StaticTask_t* processing_task_buffer_ = nullptr;
    StackType_t* processing_task_stack_ = nullptr;
    int frame_samples_ = 0;
    std::atomic<bool> low_latency_output_{false};  // 人际本地聚帧选项，不改变AFE算法或网络帧。
    bool is_speaking_ = false;
    std::atomic<bool> device_aec_enabled_{false};
    // Deferred AFE buffer reset, performed by ProcessingTask (see UpdateActiveState)
    std::atomic<bool> reset_pending_{false};
    // Deferred WakeNet/AEC toggles, applied by ProcessingTask (see ApplyAfeControls)
    std::atomic<bool> afe_control_dirty_{false};
    // Deferred output_buffer_ clear, performed by the output-producing task
    std::atomic<bool> output_reset_pending_{false};
    // Incremented whenever an active AFE session is invalidated. ProcessingTask
    // uses it to reject a fetch result produced before a disable/re-enable cycle.
    std::atomic<uint32_t> control_generation_{0};
#if CONFIG_UR_SPARKBOT_VOICE_MODEL_TEST
    std::atomic<bool> call_voice_enabled_{false};
    std::atomic<std::uint32_t> call_voice_epoch_{0}, call_voice_reset_epoch_{0}, call_voice_generation_{0};
    std::function<void(const int16_t*, std::size_t, std::uint32_t)> call_voice_frame_hook_;
    // ForwardCallVoiceFrame 只交付当前来电启用后的 AFE 帧，旧的阻塞 fetch 结果被丢弃。
    void ForwardCallVoiceFrame(const int16_t* samples, std::size_t count, std::uint32_t epoch);
#endif
    WakeDetector wake_detector_ = WakeDetector::kNone;

    std::unique_ptr<CustomWakeWord> custom_wake_word_;
    std::vector<std::string> wake_words_;
    std::string last_detected_wake_word_;
    std::vector<int16_t> input_buffer_;
    std::vector<int16_t> output_buffer_;
    std::mutex input_buffer_mutex_;

    std::function<void(const std::string&)> wake_word_detected_callback_;
    std::function<void(std::vector<int16_t>&&)> output_callback_;
    std::function<void(bool)> vad_state_change_callback_;

#if CONFIG_UR_SOFTWARE_AEC
    /* 软件回采 AEC：AFE 以「麦克风+数字参考」双通道初始化；参考由播放任务
     * 经 FeedReference 注入，Feed 时按帧交错；参考环独立加锁，播放任务不等待DSP处理。 */
    bool UsesReferenceChannel() const { return software_reference_channels_ > 0; }
    // reference_buffer_mutex_ 只保护参考环搬运，不与可能阻塞的AFE调用共用。
    std::mutex reference_buffer_mutex_;
    std::deque<int16_t> reference_ring_;
    std::vector<int16_t> mic_staging_;
    // reference_interleaved_ 由输入锁保护，完整块到齐后复用；每次喂音前覆盖全部采样。
    std::vector<int16_t> reference_interleaved_;
    int software_reference_channels_ = 0;
    /* 参考起始水位：环内不足该量时补静音，麦克风采集不能等待播放。 */
#if CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER && CONFIG_UR_HUMAN_CALL_UDP_AV
    // Watcher原生16k UDP时100ms余量晚于实测回声；保留一帧20ms供因果AEC对齐。
    static constexpr size_t kReferenceRingLowWater = 320;
#else
    static constexpr size_t kReferenceRingLowWater = 1600; /* 其他板型及RTC保持100ms @16k。 */
#endif
    static constexpr size_t kReferenceRingCapacity = 8000; /* 500ms @16k。 */
#endif

    TaskHandle_t wake_word_encode_task_ = nullptr;
    StaticTask_t* wake_word_encode_task_buffer_ = nullptr;
    StackType_t* wake_word_encode_task_stack_ = nullptr;
    WakeWordAudioCache wake_word_audio_cache_;
    std::deque<std::vector<uint8_t>> wake_word_opus_;
    std::mutex wake_word_mutex_;
    std::condition_variable wake_word_cv_;

    void ProcessingTask();
    void UpdateActiveState();
    void UpdateAecState();
    void ApplyAfeControls();
    void ApplyPendingReset();
    void OutputRawAudio(const std::vector<int16_t>& data);
    void HandleWakeWordResult(const afe_fetch_result_t* result);
    void HandleVoiceResult(const afe_fetch_result_t* result);
};

#endif
