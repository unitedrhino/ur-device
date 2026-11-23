// 文件说明：音频任务生命周期、AFE采集分流、Opus调度及播放完成票据交接。
/* 音频采集、Opus 编解码与播放队列；联犀模式将纯编解码任务栈放入 PSRAM，保留硬件 DMA 内存。 */
#include "audio_service.h"
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <freertos/idf_additions.h>
#include <cstring>
#include <new>

#define RATE_CVT_CFG(_src_rate, _dest_rate, _channel)                                        \
    (esp_ae_rate_cvt_cfg_t) {                                                                \
        .src_rate = (uint32_t)(_src_rate), .dest_rate = (uint32_t)(_dest_rate),              \
        .channel = (uint8_t)(_channel), .bits_per_sample = ESP_AUDIO_BIT16, .complexity = 2, \
        .perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,                                        \
    }

#define OPUS_DEC_CFG(_sample_rate, _frame_duration_ms)                                     \
    (esp_opus_dec_cfg_t) {                                                                 \
        .sample_rate = (uint32_t)(_sample_rate), .channel = ESP_AUDIO_MONO,                \
        .frame_duration =                                                                  \
            (esp_opus_dec_frame_duration_t)AS_OPUS_GET_FRAME_DRU_ENUM(_frame_duration_ms), \
        .self_delimited = false,                                                           \
    }

#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4 || CONFIG_IDF_TARGET_ESP32S31
#include "engines/afe_audio_engine.h"
#else
#include "engines/lite_audio_engine.h"
#endif

#define TAG "AudioService"

namespace {
/* IsNativeOpusSampleRate 判断硬件输出采样率能否由 Opus 解码器直接产生。 */
bool IsNativeOpusSampleRate(int sample_rate) {
    return sample_rate == 8000 || sample_rate == 12000 || sample_rate == 16000 ||
           sample_rate == 24000 || sample_rate == 48000;
}
}  // namespace

AudioService::AudioService() { event_group_ = xEventGroupCreate(); }

AudioService::~AudioService() {
#if CONFIG_UR_SPARKBOT_VOICE_MODEL_TEST
    if (audio_engine_initialized_ && audio_engine_) {
        static_cast<AfeAudioEngine*>(audio_engine_.get())->SetCallVoiceCapture(false, 0);
    }
    auto* worker = call_voice_worker_.exchange(nullptr);
    while (call_voice_producers_.load() != 0) vTaskDelay(1);
    if (worker != nullptr) {
        worker->~CallVoiceWorker();
        heap_caps_free(worker);
    }
#endif
    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
    }
    if (opus_encoder_ != nullptr) {
        esp_opus_enc_close(opus_encoder_);
    }
    if (opus_decoder_ != nullptr) {
        esp_opus_dec_close(opus_decoder_);
    }
    if (input_resampler_ != nullptr) {
        esp_ae_rate_cvt_close(input_resampler_);
    }
    if (output_resampler_ != nullptr) {
        esp_ae_rate_cvt_close(output_resampler_);
    }
}

void AudioService::Initialize(AudioCodec* codec) {
    codec_ = codec;
    codec_->Start();

#if CONFIG_UR_AI_SIGNALING
    /* 信令构建下旧协议 Opus 编解码器不再常驻：内部堆需同时容纳信令会话的
     * 编码器+解码器，常驻实例会造成碎片化不足。解码器经 SetDecodeSampleRate、
     * 编码器经编码循环按需打开（ResumeDecoder 幂等重开两者）。 */
    ESP_LOGI(TAG, "legacy opus codec deferred (signaling build)");
#else
    esp_opus_dec_cfg_t opus_dec_cfg =
        OPUS_DEC_CFG(codec->output_sample_rate(), OPUS_FRAME_DURATION_MS);
    auto ret = esp_opus_dec_open(&opus_dec_cfg, sizeof(esp_opus_dec_cfg_t), &opus_decoder_);
    if (opus_decoder_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create audio decoder, error code: %d", ret);
    } else {
        decoder_sample_rate_ = codec->output_sample_rate();
        decoder_duration_ms_ = OPUS_FRAME_DURATION_MS;
        decoder_frame_size_ = decoder_sample_rate_ / 1000 * OPUS_FRAME_DURATION_MS;
    }
    esp_opus_enc_config_t opus_enc_cfg = AS_OPUS_ENC_CONFIG();
    ret = esp_opus_enc_open(&opus_enc_cfg, sizeof(esp_opus_enc_config_t), &opus_encoder_);
    if (opus_encoder_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create audio encoder, error code: %d", ret);
    } else {
        encoder_sample_rate_ = 16000;
        encoder_duration_ms_ = OPUS_FRAME_DURATION_MS;
        esp_opus_enc_get_frame_size(opus_encoder_, &encoder_frame_size_, &encoder_outbuf_size_);
        encoder_frame_size_ = encoder_frame_size_ / sizeof(int16_t);
    }
#endif

    if (codec->input_sample_rate() != 16000) {
        esp_ae_rate_cvt_cfg_t input_resampler_cfg = RATE_CVT_CFG(
            codec->input_sample_rate(), ESP_AUDIO_SAMPLE_RATE_16K, codec->input_channels());
        auto resampler_ret = esp_ae_rate_cvt_open(&input_resampler_cfg, &input_resampler_);
        if (input_resampler_ == nullptr) {
            ESP_LOGE(TAG, "Failed to create input resampler, error code: %d", resampler_ret);
        }
    }

#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4 || CONFIG_IDF_TARGET_ESP32S31
    audio_engine_ = std::make_unique<AfeAudioEngine>();
#else
    audio_engine_ = std::make_unique<LiteAudioEngine>();
#endif
#if CONFIG_UR_SPARKBOT_VOICE_MODEL_TEST
    // 在 AFE 处理任务启动前固定回调；音频线程只做有界、非阻塞的帧投递。
    static_cast<AfeAudioEngine*>(audio_engine_.get())->SetCallVoiceFrameHook(
        [this](const int16_t* samples, std::size_t count, std::uint32_t generation) {
            call_voice_producers_.fetch_add(1);
            auto* worker = call_voice_worker_.load();
            if (worker != nullptr) worker->Push(generation, samples, count);
            call_voice_producers_.fetch_sub(1);
        });
#endif
    audio_engine_->OnOutput([this](std::vector<int16_t>&& data) {
#if CONFIG_UR_AI_SIGNALING
        bool signaling_captured =
            false;  // 解锁后再唤醒高优先级消费者，避免立即抢占后争同一队列锁。
        {
            std::lock_guard<std::mutex> lock(audio_queue_mutex_);
            if (signaling_capture_enabled_) {
                if (signaling_mic_muted_) {
                    /* 调试注入独占队列：丢弃真实麦克风帧，避免 2x 积压。 */
                    return;
                }
                /* 信令链路直接取 PCM；队列满丢最旧，实时音频没有重放价值。 */
                signaling_output_samples_.fetch_add(data.size(), std::memory_order_relaxed);
                if (signaling_capture_queue_.full()) {
                    signaling_drop_samples_.fetch_add(signaling_capture_queue_.front().size(),
                                                       std::memory_order_relaxed);
                    signaling_capture_queue_.pop_front();
                }
                signaling_capture_queue_.push_back(std::move(data));
                signaling_captured = true;
            }
        }
        if (signaling_captured) {
            signaling_capture_cv_.notify_one();
            return;
        }
        // 关闭人际分流时可能仍有一个原生块回调在途，不能送入旧60ms编码器。
        if (data.size() != OPUS_FRAME_DURATION_MS * 16000 / 1000)
            return;
#endif
        PushTaskToEncodeQueue(kAudioTaskTypeEncodeToSendQueue, std::move(data));
    });
    audio_engine_->OnVadStateChange([this](bool speaking) {
        voice_detected_ = speaking;
        if (callbacks_.on_vad_change) {
            callbacks_.on_vad_change(speaking);
        }
    });
    audio_engine_->OnWakeWordDetected([this](const std::string& wake_word) {
        xEventGroupClearBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
        if (callbacks_.on_wake_word_detected) {
            callbacks_.on_wake_word_detected(wake_word);
        }
    });

    esp_timer_create_args_t audio_power_timer_args = {
        .callback =
            [](void* arg) {
                AudioService* audio_service = (AudioService*)arg;
                audio_service->CheckAndUpdateAudioPowerState();
            },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "audio_power_timer",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&audio_power_timer_args, &audio_power_timer_);
}

void AudioService::Start() {
    /* 软件AEC持续双向编解码时，扬声器必须能抢占优先级5的信令媒体任务，
     * 及时补充DMA；仍低于优先级8的麦克风输入，其他构建保留原优先级。 */
#if CONFIG_UR_SOFTWARE_AEC
    constexpr UBaseType_t output_priority = 7;
#else
    constexpr UBaseType_t output_priority = 4;
#endif
    service_stopped_.store(false);
    xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING | AS_EVENT_WAKE_WORD_RUNNING |
                                           AS_EVENT_AUDIO_PROCESSOR_RUNNING |
                                           AS_EVENT_AUDIO_INPUT_STOP_REQUEST);

    esp_timer_start_periodic(audio_power_timer_, 1000000);

#if CONFIG_USE_AUDIO_PROCESSOR
    /* Start the audio input task */
    xTaskCreatePinnedToCore(
        [](void* arg) {
            AudioService* audio_service = (AudioService*)arg;
            audio_service->AudioInputTask();
            vTaskDelete(NULL);
        },
        "audio_input", 2048 * 3, this, 8, &audio_input_task_handle_, 0);

    /* Start the audio output task */
    xTaskCreate(
        [](void* arg) {
            AudioService* audio_service = (AudioService*)arg;
            audio_service->AudioOutputTask();
            vTaskDelete(NULL);
        },
        "audio_output", 2048 * 2, this, output_priority, &audio_output_task_handle_);
#else
    /* Start the audio input task */
    xTaskCreate(
        [](void* arg) {
            AudioService* audio_service = (AudioService*)arg;
            audio_service->AudioInputTask();
            vTaskDelete(NULL);
        },
        "audio_input", 2048 * 2, this, 8, &audio_input_task_handle_);

    /* Start the audio output task */
    xTaskCreate(
        [](void* arg) {
            AudioService* audio_service = (AudioService*)arg;
            audio_service->AudioOutputTask();
            vTaskDelete(NULL);
        },
        "audio_output", 2048, this, output_priority, &audio_output_task_handle_);
#endif

    /* Start the opus codec task */
#if CONFIG_UR_AI
    /* Opus 只处理内存帧，不写 Flash；24 KiB 栈迁入 PSRAM，避免语音播放期间
     * AES DMA 临时缓冲分配失败导致加解密丢帧。TCB 仍由 IDF 分配在内部 RAM。 */
    xTaskCreateWithCaps(
        [](void* arg) {
            auto* audio_service = static_cast<AudioService*>(arg);
            audio_service->OpusCodecTask();
            vTaskDeleteWithCaps(nullptr);
        },
        "opus_codec", 2048 * 12, this, 2, &opus_codec_task_handle_,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    xTaskCreate(
        [](void* arg) {
            AudioService* audio_service = (AudioService*)arg;
            audio_service->OpusCodecTask();
            vTaskDelete(NULL);
        },
        "opus_codec", 2048 * 12, this, 2, &opus_codec_task_handle_);
#endif
}

void AudioService::Stop() {
#if CONFIG_UR_SPARKBOT_VOICE_MODEL_TEST
    StopCallVoiceCapture();
#endif
    esp_timer_stop(audio_power_timer_);
    service_stopped_.store(true);
    xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING | AS_EVENT_WAKE_WORD_RUNNING |
                                         AS_EVENT_AUDIO_PROCESSOR_RUNNING);

    bool notify_drained = false;
    {
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        ++playback_generation_;
        audio_encode_queue_.clear();
        audio_decode_queue_.clear();
        audio_playback_queue_.clear();
        audio_testing_queue_.clear();
#if CONFIG_UR_AI_SIGNALING
        signaling_capture_queue_.clear();
        signaling_playback_queue_.clear();
        signaling_done_queue_.clear();
        signaling_capture_enabled_ = false;
#endif
        notify_drained = MarkPlaybackDrainedLocked();
        audio_queue_cv_.notify_all();
    }
    if (notify_drained && callbacks_.on_playback_drained) {
        callbacks_.on_playback_drained();
    }
}

bool AudioService::ReadAudioData(std::vector<int16_t>& data, int sample_rate, int samples) {
    if (!codec_->input_enabled()) {
        esp_timer_stop(audio_power_timer_);
        esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
        codec_->EnableInput(true);
    }

    if (codec_->input_sample_rate() != sample_rate) {
        data.resize(samples * codec_->input_sample_rate() / sample_rate * codec_->input_channels());
        if (!codec_->InputData(data)) {
            return false;
        }
        if (input_resampler_ != nullptr) {
            std::lock_guard<std::mutex> lock(input_resampler_mutex_);
            uint32_t in_sample_num = data.size() / codec_->input_channels();
            uint32_t output_samples = 0;
            esp_ae_rate_cvt_get_max_out_sample_num(input_resampler_, in_sample_num,
                                                   &output_samples);
            auto resampled = std::vector<int16_t>(output_samples * codec_->input_channels());
            uint32_t actual_output = output_samples;
            esp_ae_rate_cvt_process(input_resampler_, (esp_ae_sample_t)data.data(), in_sample_num,
                                    (esp_ae_sample_t)resampled.data(), &actual_output);
            resampled.resize(actual_output * codec_->input_channels());
            data = std::move(resampled);
        }
    } else {
        data.resize(samples * codec_->input_channels());
        if (!codec_->InputData(data)) {
            return false;
        }
    }

#if CONFIG_UR_AI_SIGNALING
    // 此处数据已到请求采样域；计的是ReadAudioData返回样本，不冒充外部声学测量。
    signaling_input_samples_.fetch_add(data.size() / codec_->input_channels(),
                                       std::memory_order_relaxed);
#endif
    /* Update the last input time */
    last_input_time_ = std::chrono::steady_clock::now();
    debug_statistics_.input_count++;

#if CONFIG_USE_AUDIO_DEBUGGER
    // 音频调试：发送原始音频数据
    if (audio_debugger_ == nullptr) {
        audio_debugger_ = std::make_unique<AudioDebugger>();
    }
    audio_debugger_->Feed(data);
#endif

    return true;
}

// AudioInputTask 按活动状态领取物理或调试PCM，遵循板级声道合同喂入AFE；服务停止时退出。
void AudioService::AudioInputTask() {
    constexpr EventBits_t kAudioInputActiveBits = AS_EVENT_AUDIO_TESTING_RUNNING |
                                                  AS_EVENT_WAKE_WORD_RUNNING |
                                                  AS_EVENT_AUDIO_PROCESSOR_RUNNING;
#if CONFIG_UR_AI_E2E_TEST && CONFIG_BOARD_TYPE_ESP_SPARKBOT
    // 测试包按窗口记录采集与 AFE 喂音耗时，区分设备等待与输入线程负载。
    uint64_t input_read_us = 0, input_feed_us = 0;
    unsigned input_sample_count = 0;
#endif

    while (true) {
        EventBits_t bits = xEventGroupWaitBits(
            event_group_, kAudioInputActiveBits | AS_EVENT_AUDIO_INPUT_STOP_REQUEST, pdFALSE,
            pdFALSE, portMAX_DELAY);

        if (service_stopped_.load()) {
            // ADC continuous mode keeps its hardware mutex from start until stop,
            // so the input task that started it must also stop it before exiting.
            if (codec_->input_enabled()) {
                codec_->EnableInput(false);
            }
            break;
        }

        if (bits & AS_EVENT_AUDIO_INPUT_STOP_REQUEST) {
            xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_INPUT_STOP_REQUEST);

            // Recheck the active state in this task. Audio capture may have been
            // enabled after the timer posted the stop request.
            bits = xEventGroupGetBits(event_group_);
            if ((bits & kAudioInputActiveBits) == 0) {
                if (codec_->input_enabled()) {
                    codec_->EnableInput(false);
                }
                // Do not process the stale active bits returned by waitBits().
                continue;
            }
        }

        if (audio_input_need_warmup_.exchange(false)) {
            vTaskDelay(pdMS_TO_TICKS(120));
            continue;
        }

        /* Used for audio testing in NetworkConfiguring mode by clicking the BOOT button */
        if (bits & AS_EVENT_AUDIO_TESTING_RUNNING) {
            bool testing_queue_full = false;
            {
                std::lock_guard<std::mutex> lock(audio_queue_mutex_);
                testing_queue_full = audio_testing_queue_.full();
            }
            if (testing_queue_full) {
                ESP_LOGW(TAG, "Audio testing queue is full, stopping audio testing");
                EnableAudioTesting(false);
                continue;
            }
            std::vector<int16_t> data;
            int samples = OPUS_FRAME_DURATION_MS * 16000 / 1000;
            if (ReadAudioData(data, 16000, samples)) {
                // If input channels is 2, we need to fetch the left channel data
                if (codec_->input_channels() == 2) {
                    auto mono_data = std::vector<int16_t>(data.size() / 2);
                    for (size_t i = 0, j = 0; i < mono_data.size(); ++i, j += 2) {
                        mono_data[i] = data[j];
                    }
                    data = std::move(mono_data);
                }
                PushTaskToEncodeQueue(kAudioTaskTypeEncodeToTestingQueue, std::move(data));
                continue;
            }
        }

        /* Feed the selected audio engine */
        if (bits & (AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_AUDIO_PROCESSOR_RUNNING)) {
            int samples = 160;  // 10ms
            std::vector<int16_t> data;
#if CONFIG_UR_AI_SIGNALING_WEBRTC
            /* 调试注入优先替代麦克风读取，单声道语料按板级格式进入同一AFE。
             * 队列间隙仍可能读取物理麦克风，不能代替真实声学验收。 */
            {
                // injected 保持调试接口的16k单声道合同；队列锁不覆盖可能阻塞的AFE处理。
                std::vector<int16_t> injected;
                {
                    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
                    if (signaling_capture_enabled_ && !signaling_mic_inject_queue_.empty()) {
                        injected = std::move(signaling_mic_inject_queue_.front());
                        signaling_mic_inject_queue_.pop_front();
                    }
                }
                if (!injected.empty()) {
                    if (codec_->input_reference() && codec_->input_channels() == 2) {
                        // 硬件回采的Feed接收M／R交错采样，调试单声道必须补静音参考。
                        data.resize(injected.size() * 2, 0);
                        // sample_index 对应原单声道位置，不把相邻麦克风采样误当参考。
                        for (size_t sample_index = 0; sample_index < injected.size();
                             ++sample_index) {
                            data[sample_index * 2] = injected[sample_index];
                        }
                        audio_engine_->Feed(std::move(data));
                    } else {
                        audio_engine_->Feed(std::move(injected));
                    }
                    continue;
                }
            }
#endif
#if CONFIG_UR_AI_E2E_TEST && CONFIG_BOARD_TYPE_ESP_SPARKBOT
            const int64_t input_read_at = esp_timer_get_time();
#endif
            if (ReadAudioData(data, 16000, samples)) {
#if CONFIG_UR_AI_E2E_TEST && CONFIG_BOARD_TYPE_ESP_SPARKBOT
                const int64_t input_feed_at = esp_timer_get_time();
#endif
                audio_engine_->Feed(std::move(data));
#if CONFIG_UR_AI_E2E_TEST && CONFIG_BOARD_TYPE_ESP_SPARKBOT
                input_read_us += input_feed_at - input_read_at;
                input_feed_us += esp_timer_get_time() - input_feed_at;
                if (++input_sample_count == 500) {
                    ESP_LOGI(TAG, "input timing count=%u read_us=%llu feed_us=%llu",
                             input_sample_count, static_cast<unsigned long long>(input_read_us),
                             static_cast<unsigned long long>(input_feed_us));
                    input_read_us = input_feed_us = 0;
                    input_sample_count = 0;
                }
#endif
                continue;
            }
        }

        // Read timeout/error should not terminate the input task.
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    ESP_LOGW(TAG, "Audio input task stopped");
}

void AudioService::AudioOutputTask() {
    while (true) {
        std::unique_lock<std::mutex> lock(audio_queue_mutex_);
#if CONFIG_UR_AI_SIGNALING
        audio_queue_cv_.wait(lock, [this]() {
            return !audio_playback_queue_.empty() || !signaling_playback_queue_.empty() ||
                   service_stopped_.load();
        });
#else
        audio_queue_cv_.wait(
            lock, [this]() { return !audio_playback_queue_.empty() || service_stopped_.load(); });
#endif
        if (service_stopped_.load()) {
            break;
        }

#if CONFIG_UR_AI_SIGNALING
        /* 信令播放帧旁路解码队列；真实写出完成后回投 ticket 供事件线程确认。 */
        if (!signaling_playback_queue_.empty()) {
            AudioTask signaling_task = std::move(signaling_playback_queue_.front());
            signaling_playback_queue_.pop_front();
            output_in_flight_ = true;
            signaling_output_in_flight_ = true;
            audio_queue_cv_.notify_all();
            lock.unlock();

            if (!codec_->output_enabled()) {
                esp_timer_stop(audio_power_timer_);
                esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
                codec_->EnableOutput(true);
            }
#if CONFIG_UR_SOFTWARE_AEC
            const auto duplex_write_at = esp_timer_get_time();  // 驱动实际写出起点。
#endif
            codec_->OutputData(signaling_task.pcm);
#if CONFIG_UR_SOFTWARE_AEC
            const auto duplex_reference_at = esp_timer_get_time();  // AEC回采独立计量，避免混作I2S耗时。
#endif
            last_output_time_ = std::chrono::steady_clock::now();
            debug_statistics_.playback_count++;

#if CONFIG_UR_SOFTWARE_AEC
            if (audio_engine_ != nullptr && !signaling_task.pcm.empty()) {
                /* 实际送出的播放 PCM 重采样回 16k 作 AEC 数字参考。 */
                std::lock_guard<std::mutex> resampler_lock(signaling_reference_resampler_mutex_);
                if (signaling_reference_resampler_ == nullptr && codec_ != nullptr &&
                    codec_->output_sample_rate() != 16000) {
                    esp_ae_rate_cvt_cfg_t cfg = RATE_CVT_CFG(
                        codec_->output_sample_rate(), ESP_AUDIO_SAMPLE_RATE_16K, 1);
                    if (esp_ae_rate_cvt_open(&cfg, &signaling_reference_resampler_) !=
                        ESP_AE_ERR_OK) {
                        signaling_reference_resampler_ = nullptr;
                    }
                }
                if (signaling_reference_resampler_ != nullptr) {
                    uint32_t target = 0;
                    esp_ae_rate_cvt_get_max_out_sample_num(signaling_reference_resampler_,
                                                           signaling_task.pcm.size(), &target);
                    std::vector<int16_t> reference(target);
                    uint32_t actual = target;
                    if (esp_ae_rate_cvt_process(signaling_reference_resampler_,
                                                (esp_ae_sample_t)signaling_task.pcm.data(),
                                                signaling_task.pcm.size(),
                                                (esp_ae_sample_t)reference.data(),
                                                &actual) == ESP_AE_ERR_OK &&
                        actual > 0) {
                        reference.resize(actual);
                        audio_engine_->FeedReference(std::move(reference));
                    }
                } else if (codec_ != nullptr && codec_->output_sample_rate() == 16000) {
                    audio_engine_->FeedReference(std::move(signaling_task.pcm));
                }
            }
#endif

#if CONFIG_UR_SOFTWARE_AEC
            const auto duplex_reference_done = esp_timer_get_time();  // 回采重采样与参考提交已完成。
#endif
            lock.lock();
#if CONFIG_UR_SOFTWARE_AEC
            /* 在短锁内快照队列；串口日志不能阻塞采集和已写出帧的完成通知。 */
            const auto queued_after_write = static_cast<unsigned>(signaling_playback_queue_.size());
#endif
            output_in_flight_ = false;
            /* 重置代次可能发生在驱动写出期间；迟到帧只能释放在途状态，不能回投旧票据。 */
            if (signaling_task.signaling_generation == signaling_playback_generation_) {
                if (signaling_done_queue_.full()) {
                    signaling_done_queue_.pop_front();
                }
                signaling_done_queue_.push_back(signaling_task.timestamp);
            }
            signaling_output_in_flight_ = false;
            audio_queue_cv_.notify_all();
            lock.unlock();
#if CONFIG_UR_SOFTWARE_AEC
            /* 输出任务独占的低频统计，区分驱动等待和软件参考锁竞争。 */
            static std::int64_t trace_at = 0, write_sum = 0, reference_sum = 0,
                                write_max = 0, reference_max = 0;
            static unsigned trace_frames = 0;
            const auto write_us = duplex_reference_at - duplex_write_at;
            const auto reference_us = duplex_reference_done - duplex_reference_at;
            if (!trace_at) trace_at = duplex_write_at;
            write_sum += write_us;
            reference_sum += reference_us;
            if (write_us > write_max) write_max = write_us;
            if (reference_us > reference_max) reference_max = reference_us;
            ++trace_frames;
            if (duplex_reference_done - trace_at >= 1000000) {
                ESP_LOGI(TAG, "duplex output frames=%u io_avg_us=%lld io_max_us=%lld ref_avg_us=%lld ref_max_us=%lld queued=%u core=%d affinity=%d",
                         trace_frames, static_cast<long long>(write_sum / trace_frames),
                         static_cast<long long>(write_max),
                         static_cast<long long>(reference_sum / trace_frames),
                         static_cast<long long>(reference_max), queued_after_write,
                         xPortGetCoreID(), xTaskGetCoreID(nullptr));
                trace_at = duplex_reference_done;
                write_sum = reference_sum = write_max = reference_max = trace_frames = 0;
            }
#endif
            continue;
        }
#endif

        auto task = std::move(audio_playback_queue_.front());
        audio_playback_queue_.pop_front();
        output_in_flight_ = true;
        audio_queue_cv_.notify_all();
        lock.unlock();

        if (!codec_->output_enabled()) {
            esp_timer_stop(audio_power_timer_);
            esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
            codec_->EnableOutput(true);
        }

        if (task.playback_id != 0 && callbacks_.on_playback_progress) {
            callbacks_.on_playback_progress(task.playback_id, task.media_position_ms);
        }

        codec_->OutputData(task.pcm);

        /* Update the last output time */
        last_output_time_ = std::chrono::steady_clock::now();
        debug_statistics_.playback_count++;

        bool notify_drained = false;
        lock.lock();
#if CONFIG_USE_SERVER_AEC
        /* Record the timestamp for server AEC */
        if (task.timestamp > 0) {
            if (timestamp_queue_.full()) {
                timestamp_queue_.pop_front();
            }
            timestamp_queue_.push_back(task.timestamp);
        }
#endif
        output_in_flight_ = false;
        notify_drained = MarkPlaybackDrainedLocked();
        audio_queue_cv_.notify_all();
        lock.unlock();

        if (notify_drained && callbacks_.on_playback_drained) {
            callbacks_.on_playback_drained();
        }
    }

    ESP_LOGW(TAG, "Audio output task stopped");
}

void AudioService::OpusCodecTask() {
    while (true) {
        std::unique_lock<std::mutex> lock(audio_queue_mutex_);
        audio_queue_cv_.wait(lock, [this]() {
            return service_stopped_.load() || !audio_encode_queue_.empty() ||
                   (!audio_decode_queue_.empty() &&
                    audio_playback_queue_.size() < MAX_PLAYBACK_TASKS_IN_QUEUE);
        });
        if (service_stopped_.load()) {
            break;
        }

        /* Decode the audio from decode queue */
        if (!audio_decode_queue_.empty() &&
            audio_playback_queue_.size() < MAX_PLAYBACK_TASKS_IN_QUEUE) {
            auto packet = std::move(audio_decode_queue_.front());
            audio_decode_queue_.pop_front();
            decode_in_flight_ = true;
            const uint32_t generation = playback_generation_;
            audio_queue_cv_.notify_all();
            lock.unlock();

            AudioTask task;
            task.type = kAudioTaskTypeDecodeToPlaybackQueue;
            task.timestamp = packet->timestamp;
            task.playback_id = packet->playback_id;
            task.media_position_ms = packet->media_position_ms;

            SetDecodeSampleRate(packet->sample_rate, packet->frame_duration);
            bool decoded = false;
            if (opus_decoder_ != nullptr) {
                task.pcm.resize(decoder_frame_size_);
                esp_audio_dec_in_raw_t raw = {
                    .buffer = (uint8_t*)(packet->payload.data()),
                    .len = (uint32_t)(packet->payload.size()),
                    .consumed = 0,
                    .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE,
                };
                esp_audio_dec_out_frame_t out_frame = {
                    .buffer = (uint8_t*)(task.pcm.data()),
                    .len = (uint32_t)(task.pcm.size() * sizeof(int16_t)),
                    .decoded_size = 0,
                };
                esp_audio_dec_info_t dec_info = {};
                std::unique_lock<std::mutex> decoder_lock(decoder_mutex_);
                auto ret = esp_opus_dec_decode(opus_decoder_, &raw, &out_frame, &dec_info);
                decoder_lock.unlock();
                if (ret == ESP_AUDIO_ERR_OK) {
                    task.pcm.resize(out_frame.decoded_size / sizeof(int16_t));
                    if (decoder_sample_rate_ != codec_->output_sample_rate() &&
                        output_resampler_ != nullptr) {
                        uint32_t target_size = 0;
                        const auto size_ret = esp_ae_rate_cvt_get_max_out_sample_num(
                            output_resampler_, task.pcm.size(), &target_size);
                        if (size_ret != ESP_AE_ERR_OK || target_size == 0) {
                            ESP_LOGE(TAG,
                                     "Failed to size output resampler buffer, error=%d input=%u "
                                     "target=%u",
                                     size_ret, static_cast<unsigned>(task.pcm.size()),
                                     static_cast<unsigned>(target_size));
                        } else {
                            const uint32_t output_capacity = target_size;
                            std::vector<int16_t> resampled(output_capacity);
                            uint32_t actual_output = output_capacity;
                            const auto resample_ret = esp_ae_rate_cvt_process(
                                output_resampler_, (esp_ae_sample_t)task.pcm.data(),
                                task.pcm.size(), (esp_ae_sample_t)resampled.data(),
                                &actual_output);
                            if (resample_ret != ESP_AE_ERR_OK ||
                                actual_output > output_capacity) {
                                ESP_LOGE(TAG,
                                         "Failed to resample output audio, error=%d actual=%u "
                                         "capacity=%u",
                                         resample_ret, static_cast<unsigned>(actual_output),
                                         static_cast<unsigned>(output_capacity));
                            } else {
                                resampled.resize(actual_output);
                                task.pcm = std::move(resampled);
                                decoded = true;
                            }
                        }
                    } else {
                        decoded = true;
                    }
                } else {
                    ESP_LOGE(TAG, "Failed to decode audio after resize, error code: %d", ret);
                }
            } else {
                ESP_LOGE(TAG, "Audio decoder is not configured");
            }

            lock.lock();
            if (decoded && generation == playback_generation_ && !service_stopped_.load()) {
                audio_playback_queue_.push_back(std::move(task));
            }
            decode_in_flight_ = false;
            debug_statistics_.decode_count++;
            const bool notify_drained = MarkPlaybackDrainedLocked();
            audio_queue_cv_.notify_all();
            lock.unlock();
            if (notify_drained && callbacks_.on_playback_drained) {
                callbacks_.on_playback_drained();
            }
            lock.lock();
        }
        /* Encode the audio to send queue */
        if (!audio_encode_queue_.empty()) {
            auto task = std::move(audio_encode_queue_.front());
            audio_encode_queue_.pop_front();
            audio_queue_cv_.notify_all();
            lock.unlock();

            auto packet = std::make_unique<AudioStreamPacket>();
            packet->frame_duration = OPUS_FRAME_DURATION_MS;
            packet->sample_rate = 16000;
            packet->timestamp = task.timestamp;

#if CONFIG_UR_AI_SIGNALING
            /* 旧协议编码器惰性打开：首次真正编码时补开（见 Initialize 注释）。 */
            if (opus_encoder_ == nullptr) {
                ResumeDecoder();
            }
#endif
            if (opus_encoder_ != nullptr && task.pcm.size() == encoder_frame_size_) {
                packet->payload.resize(encoder_outbuf_size_);
                esp_audio_enc_in_frame_t in = {
                    .buffer = (uint8_t*)(task.pcm.data()),
                    .len = (uint32_t)(encoder_frame_size_ * sizeof(int16_t)),
                };
                esp_audio_enc_out_frame_t out = {
                    .buffer = packet->payload.data(),
                    .len = (uint32_t)encoder_outbuf_size_,
                    .encoded_bytes = 0,
                };
                auto ret = esp_opus_enc_process(opus_encoder_, &in, &out);
                if (ret == ESP_AUDIO_ERR_OK) {
                    packet->payload.resize(out.encoded_bytes);

                    if (task.type == kAudioTaskTypeEncodeToSendQueue) {
                        {
                            std::lock_guard<std::mutex> lock2(audio_queue_mutex_);
                            /* Never let a full send queue stall encoding: stale realtime
                             * audio is useless to the server, so drop the oldest packet. */
                            if (audio_send_queue_.size() >= MAX_SEND_PACKETS_IN_QUEUE) {
                                audio_send_queue_.pop_front();
                            }
                            audio_send_queue_.push_back(std::move(packet));
                        }
                        if (callbacks_.on_send_queue_available) {
                            callbacks_.on_send_queue_available();
                        }
                    } else if (task.type == kAudioTaskTypeEncodeToTestingQueue) {
                        bool testing_queue_full = false;
                        {
                            std::lock_guard<std::mutex> lock2(audio_queue_mutex_);
                            testing_queue_full = !audio_testing_queue_.push_back(std::move(packet));
                        }
                        if (testing_queue_full && callbacks_.on_audio_testing_queue_full) {
                            callbacks_.on_audio_testing_queue_full();
                        }
                    }
                    debug_statistics_.encode_count++;
                } else {
                    ESP_LOGE(TAG, "Failed to encode audio, error code: %d", ret);
                }
            } else {
                ESP_LOGE(TAG,
                         "Failed to encode audio: encoder not configured or invalid frame size "
                         "(got %u, expected %u)",
                         task.pcm.size(), encoder_frame_size_);
            }
            lock.lock();
        }
    }

    ESP_LOGW(TAG, "Opus codec task stopped");
}

void AudioService::SetDecodeSampleRate(int sample_rate, int frame_duration) {
    auto codec = Board::GetInstance().GetAudioCodec();
    const int output_sample_rate = codec->output_sample_rate();
    /* Opus 码流不携带固定 PCM 采样率，解码器可以直接选择任一原生 Opus 采样率。
     * Watcher 因此把联犀 16 kHz 码流直接解码为硬件所需的 24 kHz，避免再经过
     * esp_audio_effects 的 16→24 kHz 转换器；该转换器在连续 TTS 帧下曾越界写堆。 */
    const int decode_sample_rate =
        IsNativeOpusSampleRate(output_sample_rate) ? output_sample_rate : sample_rate;
    if (decoder_sample_rate_ == decode_sample_rate &&
        decoder_duration_ms_ == frame_duration) {
        return;
    }
    std::unique_lock<std::mutex> decoder_lock(decoder_mutex_);
    if (opus_decoder_ != nullptr) {
        esp_opus_dec_close(opus_decoder_);
        opus_decoder_ = nullptr;
    }
    decoder_lock.unlock();
    esp_opus_dec_cfg_t opus_dec_cfg = OPUS_DEC_CFG(decode_sample_rate, frame_duration);
    auto ret = esp_opus_dec_open(&opus_dec_cfg, sizeof(esp_opus_dec_cfg_t), &opus_decoder_);
    if (opus_decoder_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create audio decoder, error code: %d", ret);
        return;
    }
    decoder_sample_rate_ = decode_sample_rate;
    decoder_duration_ms_ = frame_duration;
    decoder_frame_size_ = decoder_sample_rate_ / 1000 * frame_duration;

    if (decoder_sample_rate_ != output_sample_rate) {
        ESP_LOGI(TAG, "Resampling audio from %d to %d", decoder_sample_rate_,
                 output_sample_rate);
        if (output_resampler_ != nullptr) {
            esp_ae_rate_cvt_close(output_resampler_);
            output_resampler_ = nullptr;
        }
        esp_ae_rate_cvt_cfg_t output_resampler_cfg =
            RATE_CVT_CFG(decoder_sample_rate_, output_sample_rate, ESP_AUDIO_MONO);
        auto resampler_ret = esp_ae_rate_cvt_open(&output_resampler_cfg, &output_resampler_);
        if (output_resampler_ == nullptr) {
            ESP_LOGE(TAG, "Failed to create output resampler, error code: %d", resampler_ret);
        }
    } else if (output_resampler_ != nullptr) {
        esp_ae_rate_cvt_close(output_resampler_);
        output_resampler_ = nullptr;
    }
}

void AudioService::PushTaskToEncodeQueue(AudioTaskType type, std::vector<int16_t>&& pcm) {
    AudioTask task;
    task.type = type;
    task.pcm = std::move(pcm);

    uint32_t dropped_total = 0;
    {
        /* Push the task to the encode queue */
        std::unique_lock<std::mutex> lock(audio_queue_mutex_);

        /* If the task is to send queue, we need to set the timestamp */
        if (type == kAudioTaskTypeEncodeToSendQueue && !timestamp_queue_.empty()) {
            task.timestamp = timestamp_queue_.front();
            timestamp_queue_.pop_front();
        }

        /* Microphone audio is realtime, so drop the oldest frame instead of blocking.
         * Blocking here would stall the audio engine task (AFE fetch) and deadlock the
         * whole input pipeline when the send queue stops being drained (e.g. network
         * congestion or a failed UDP send). */
        if (audio_encode_queue_.size() >= MAX_ENCODE_TASKS_IN_QUEUE) {
            audio_encode_queue_.pop_front();
            dropped_total = ++debug_statistics_.encode_drop_count;
        }
        audio_encode_queue_.push_back(std::move(task));
        audio_queue_cv_.notify_all();
    }

    /* Log outside the lock (UART writes are slow and would starve the codec task),
     * at most once per second. */
    if (dropped_total > 0) {
        int64_t now = esp_timer_get_time();
        if (now - last_encode_drop_log_time_ >= 1000000) {
            last_encode_drop_log_time_ = now;
            ESP_LOGW(TAG, "Encode queue is full, dropping oldest frame (dropped %lu so far)",
                     (unsigned long)dropped_total);
        }
    }
}

bool AudioService::PushPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket> packet, bool wait) {
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);
    const uint32_t generation = playback_generation_;
    if (audio_decode_queue_.size() >= MAX_DECODE_PACKETS_IN_QUEUE) {
        if (wait) {
            audio_queue_cv_.wait(lock, [this, generation]() {
                return service_stopped_.load() || generation != playback_generation_ ||
                       audio_decode_queue_.size() < MAX_DECODE_PACKETS_IN_QUEUE;
            });
        } else {
            return false;
        }
    }
    if (service_stopped_.load() || generation != playback_generation_) {
        return false;
    }
    playback_drained_notified_ = false;
    audio_decode_queue_.push_back(std::move(packet));
    audio_queue_cv_.notify_all();
    return true;
}

std::unique_ptr<AudioStreamPacket> AudioService::PopPacketFromSendQueue() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    if (audio_send_queue_.empty()) {
        return nullptr;
    }
    auto packet = std::move(audio_send_queue_.front());
    audio_send_queue_.pop_front();
    audio_queue_cv_.notify_all();
    return packet;
}

void AudioService::EncodeWakeWord() {
    if (audio_engine_) {
        audio_engine_->EncodeWakeWordData();
    }
}

const std::string& AudioService::GetLastWakeWord() const {
    static const std::string empty;
    return audio_engine_ ? audio_engine_->GetLastDetectedWakeWord() : empty;
}

std::unique_ptr<AudioStreamPacket> AudioService::PopWakeWordPacket() {
    auto packet = std::make_unique<AudioStreamPacket>();
    if (audio_engine_ && audio_engine_->GetWakeWordOpus(packet->payload)) {
        return packet;
    }
    return nullptr;
}

void AudioService::EnableWakeWordDetection(bool enable) {
    ESP_LOGD(TAG, "%s wake word detection", enable ? "Enabling" : "Disabling");
    if (enable) {
        if (!InitializeAudioEngine()) {
            xEventGroupClearBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
            return;
        }
#if !(CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4 || CONFIG_IDF_TARGET_ESP32S31)
        auto* lite_engine = static_cast<LiteAudioEngine*>(audio_engine_.get());
        if (!lite_engine->RestoreWakeWordResources()) {
            xEventGroupClearBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
            return;
        }
#endif
        if (!audio_engine_->HasWakeWord()) {
            xEventGroupClearBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(input_resampler_mutex_);
            if (input_resampler_ != nullptr) {
                esp_ae_rate_cvt_reset(input_resampler_);
            }
        }
        audio_engine_->EnableWakeWordDetection(true);
        xEventGroupSetBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
    } else {
        if (audio_engine_initialized_) {
            audio_engine_->EnableWakeWordDetection(false);
        }
        xEventGroupClearBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
    }
}

#if CONFIG_UR_SPARKBOT_VOICE_MODEL_TEST
bool AudioService::BeginCallVoiceCapture(std::uint32_t generation,
                                         CallVoiceWorker::Completion completion) {
    // 不接管 AI 或人际采集；没有空闲唤醒采集时，保留电容接听。
    if (service_stopped_.load() || !audio_engine_initialized_ || models_list_ == nullptr ||
        !IsWakeWordRunning() || IsAudioProcessorRunning() || !completion) return false;
    auto* worker = call_voice_worker_.load();
    if (worker == nullptr) {
        void* memory = heap_caps_malloc(sizeof(CallVoiceWorker), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (memory == nullptr) return false;
        worker = new (memory) CallVoiceWorker();
        if (!worker->Initialize()) {
            worker->~CallVoiceWorker();
            heap_caps_free(worker);
            return false;
        }
        call_voice_worker_.store(worker);
    }
    if (!worker->Begin(models_list_, generation, std::move(completion))) return false;
    static_cast<AfeAudioEngine*>(audio_engine_.get())->SetCallVoiceCapture(true, generation);
    call_voice_capture_armed_ = true;
    return true;
}

void AudioService::StopCallVoiceCapture() {
    if (!call_voice_capture_armed_) return;
    call_voice_capture_armed_ = false;
    if (audio_engine_initialized_ && audio_engine_)
        static_cast<AfeAudioEngine*>(audio_engine_.get())->SetCallVoiceCapture(false, 0);
    auto* worker = call_voice_worker_.load();
    if (worker != nullptr) worker->RequestStop();
}

bool AudioService::IsCallVoiceCaptureBusy() const {
    auto* worker = call_voice_worker_.load();
    return worker != nullptr && worker->Busy();
}
#endif

void AudioService::ReleaseWakeWordResources() {
#if !(CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4 || CONFIG_IDF_TARGET_ESP32S31)
    if (!audio_engine_initialized_) {
        return;
    }
    if (xEventGroupGetBits(event_group_) &
        (AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_AUDIO_PROCESSOR_RUNNING)) {
        ESP_LOGW(TAG, "Cannot release WakeNet while the audio engine is active");
        return;
    }
    static_cast<LiteAudioEngine*>(audio_engine_.get())->ReleaseWakeWordResources();
#endif
}

void AudioService::EnableVoiceProcessing(bool enable) {
    ESP_LOGD(TAG, "%s voice processing", enable ? "Enabling" : "Disabling");

    if (enable) {
        if (!InitializeAudioEngine()) {
            return;
        }
        ResetDecoder();
        audio_input_need_warmup_ = true;
        {
            std::lock_guard<std::mutex> lock(input_resampler_mutex_);
            if (input_resampler_ != nullptr) {
                esp_ae_rate_cvt_reset(input_resampler_);
            }
        }
        audio_engine_->EnableVoiceProcessing(true);
        xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_PROCESSOR_RUNNING);
    } else {
        if (audio_engine_initialized_) {
            audio_engine_->EnableVoiceProcessing(false);
        }
        xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_PROCESSOR_RUNNING);
    }
}

void AudioService::EnableAudioTesting(bool enable) {
    ESP_LOGI(TAG, "%s audio testing", enable ? "Enabling" : "Disabling");
    if (enable) {
        xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING);
    } else {
        xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING);
        /* Copy audio_testing_queue_ to audio_decode_queue_ */
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        audio_decode_queue_.clear();
        audio_decode_queue_.swap(audio_testing_queue_);
        if (!audio_decode_queue_.empty()) {
            playback_drained_notified_ = false;
        }
        audio_queue_cv_.notify_all();
    }
}

void AudioService::EnableDeviceAec(bool enable) {
    ESP_LOGI(TAG, "%s device AEC", enable ? "Enabling" : "Disabling");
    device_aec_enabled_ = enable;

    if (audio_engine_initialized_) {
        audio_engine_->EnableDeviceAec(enable);
    } else {
        ESP_LOGI(TAG, "Deferring AEC change until the audio engine is initialized");
    }
}

#if CONFIG_UR_AI_SIGNALING
// EnableLowLatencyCapture 不重建AFE或修改编解码器；原子选择由引擎输出任务应用。
void AudioService::EnableLowLatencyCapture(bool enable) {
    if (audio_engine_)
        audio_engine_->EnableLowLatencyOutput(enable);
    ESP_LOGI(TAG, "capture output quantum=%s", enable ? "native" : "default");
}

void AudioService::EnableSignalingCapture(bool enable) {
    {
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        signaling_capture_enabled_ = enable;
        if (!enable) {
            signaling_capture_queue_.clear();
        }
    }
    signaling_capture_cv_.notify_all();
    ESP_LOGI(TAG, "%s signaling capture", enable ? "Enabling" : "Disabling");
}

#if CONFIG_UR_AI_SIGNALING_WEBRTC
bool AudioService::InjectSignalingCapture(const std::vector<int16_t>& pcm) {
    {
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        if (!signaling_capture_enabled_ || pcm.empty()) {
            return false;
        }
        if (signaling_capture_queue_.full()) {
            signaling_capture_queue_.pop_front();
        }
        signaling_capture_queue_.push_back(pcm);
    }
    signaling_capture_cv_.notify_one();
    return true;
}
#endif

bool AudioService::PopSignalingPcm(std::vector<int16_t>& pcm) {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    if (signaling_capture_queue_.empty()) {
        return false;
    }
    pcm = std::move(signaling_capture_queue_.front());
    signaling_capture_queue_.pop_front();
    signaling_taken_samples_.fetch_add(pcm.size(), std::memory_order_relaxed);
    return true;
}

// WaitSignalingPcm 与生产者共用队列锁，避免检查空队列到睡眠之间漏掉出帧通知。
bool AudioService::WaitSignalingPcm(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);
    signaling_capture_cv_.wait_for(lock, timeout,
                                   [this] { return !signaling_capture_queue_.empty(); });
    return signaling_capture_enabled_ && !signaling_capture_queue_.empty();
}

// TraceSignalingCapture 只在本地诊断调用；读取同一通话窗口两次才能判断各层样本推进率。
void AudioService::TraceSignalingCapture() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    printf("[UR_CAPTURE] at_us=%lld input_samples=%u afe_samples=%u taken_samples=%u drop_samples=%u queued_blocks=%u enabled=%d\n",
           static_cast<long long>(esp_timer_get_time()), static_cast<unsigned>(signaling_input_samples_.load()),
           static_cast<unsigned>(signaling_output_samples_.load()),
           static_cast<unsigned>(signaling_taken_samples_.load()),
           static_cast<unsigned>(signaling_drop_samples_.load()),
           static_cast<unsigned>(signaling_capture_queue_.size()),
           int(signaling_capture_enabled_));
}

bool AudioService::SignalingPlaybackQueueHasSpace() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    return !signaling_playback_queue_.full();
}

// PushSignalingPcm 将16k单声道PCM与驱动票据交给播放队列，返回是否接纳。
// 仅信令事件线程生产；算法不持有共享队列锁，转换后重新检查停止与容量。
bool AudioService::PushSignalingPcm(std::vector<int16_t>&& pcm, uint32_t ticket) {
    {
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        if (service_stopped_.load() || signaling_playback_queue_.full()) {
            return false;
        }
    }
    if (codec_ != nullptr && codec_->output_sample_rate() != 16000) {
        /* 设备输出率非 16k 时在事件线程重采样；参考与下行共用同源 16k 数据。 */
        std::lock_guard<std::mutex> resampler_lock(signaling_output_resampler_mutex_);
        if (signaling_output_resampler_ == nullptr) {
            esp_ae_rate_cvt_cfg_t cfg = RATE_CVT_CFG(
                ESP_AUDIO_SAMPLE_RATE_16K, codec_->output_sample_rate(), 1);
            if (esp_ae_rate_cvt_open(&cfg, &signaling_output_resampler_) != ESP_AE_ERR_OK) {
                signaling_output_resampler_ = nullptr;
            }
        }
        if (signaling_output_resampler_ != nullptr) {
            uint32_t target = 0;
            esp_ae_rate_cvt_get_max_out_sample_num(signaling_output_resampler_, pcm.size(),
                                                   &target);
            std::vector<int16_t> resampled(target);
            uint32_t actual = target;
            if (esp_ae_rate_cvt_process(signaling_output_resampler_,
                                        (esp_ae_sample_t)pcm.data(), pcm.size(),
                                        (esp_ae_sample_t)resampled.data(),
                                        &actual) == ESP_AE_ERR_OK &&
                actual > 0) {
                resampled.resize(actual);
                pcm = std::move(resampled);
            }
        }
    }
    AudioTask task;  // 先完成帧准备，队列锁只覆盖最后的状态检查和移交。
    task.type = kAudioTaskTypeDecodeToPlaybackQueue;
    /* timestamp 字段在信令播放路径复用为完成确认 ticket。 */
    task.timestamp = ticket;
    task.playback_id = 0;
    task.media_position_ms = 0;
    task.pcm = std::move(pcm);
    {
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        if (service_stopped_.load() || signaling_playback_queue_.full()) {
            return false;
        }
        task.signaling_generation = signaling_playback_generation_;
        signaling_playback_queue_.push_back(std::move(task));
    }
    audio_queue_cv_.notify_all();
    return true;
}

bool AudioService::PopSignalingPlaybackDone(uint32_t& ticket) {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    if (signaling_done_queue_.empty()) {
        return false;
    }
    ticket = signaling_done_queue_.front();
    signaling_done_queue_.pop_front();
    return true;
}

void AudioService::ResetSignalingPlayback() {
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);
    ++signaling_playback_generation_;
    signaling_playback_queue_.clear();
    signaling_done_queue_.clear();
    audio_queue_cv_.notify_all();
    /* 不能让新会话在旧PCM仍由驱动写出时开始；完成回执由代次门禁丢弃。 */
    audio_queue_cv_.wait(lock, [this] { return !signaling_output_in_flight_; });
    signaling_done_queue_.clear();
}
#endif

#if CONFIG_UR_AI_SIGNALING
void AudioService::SuspendDecoder() {
    std::lock_guard<std::mutex> lock(decoder_mutex_);
    if (opus_decoder_ != nullptr) {
        esp_opus_dec_close(opus_decoder_);
        opus_decoder_ = nullptr;
    }
    if (opus_encoder_ != nullptr) {
        esp_opus_enc_close(opus_encoder_);
        opus_encoder_ = nullptr;
    }
    ESP_LOGI(TAG, "legacy codec suspended for signaling session, free internal=%u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
}

void AudioService::ResumeDecoder() {
    std::lock_guard<std::mutex> lock(decoder_mutex_);
    if (codec_ == nullptr) {
        return;
    }
    if (opus_decoder_ == nullptr) {
        esp_opus_dec_cfg_t opus_dec_cfg =
            OPUS_DEC_CFG(codec_->output_sample_rate(), OPUS_FRAME_DURATION_MS);
        auto ret = esp_opus_dec_open(&opus_dec_cfg, sizeof(esp_opus_dec_cfg_t), &opus_decoder_);
        if (opus_decoder_ == nullptr) {
            ESP_LOGE(TAG, "Failed to resume decoder, error code: %d", ret);
        } else {
            decoder_sample_rate_ = codec_->output_sample_rate();
            decoder_duration_ms_ = OPUS_FRAME_DURATION_MS;
            decoder_frame_size_ = decoder_sample_rate_ / 1000 * OPUS_FRAME_DURATION_MS;
        }
    }
    if (opus_encoder_ == nullptr) {
        esp_opus_enc_config_t opus_enc_cfg = AS_OPUS_ENC_CONFIG();
        auto ret = esp_opus_enc_open(&opus_enc_cfg, sizeof(esp_opus_enc_config_t), &opus_encoder_);
        if (opus_encoder_ == nullptr) {
            ESP_LOGE(TAG, "Failed to resume encoder, error code: %d", ret);
        } else {
            encoder_sample_rate_ = 16000;
            encoder_duration_ms_ = OPUS_FRAME_DURATION_MS;
            esp_opus_enc_get_frame_size(opus_encoder_, &encoder_frame_size_,
                                        &encoder_outbuf_size_);
            encoder_frame_size_ = encoder_frame_size_ / sizeof(int16_t);
        }
    }
    ESP_LOGI(TAG, "legacy codec resumed, free internal=%u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
}
#endif

void AudioService::SetCallbacks(AudioServiceCallbacks& callbacks) { callbacks_ = callbacks; }

void AudioService::PlaySound(const std::string_view& ogg) {
    if (!codec_->output_enabled()) {
        esp_timer_stop(audio_power_timer_);
        esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
        codec_->EnableOutput(true);
    }

    const auto* buf = reinterpret_cast<const uint8_t*>(ogg.data());
    size_t size = ogg.size();

    auto demuxer = std::make_unique<OggDemuxer>();
    demuxer->OnPacket(
        [this](const uint8_t* data, int sample_rate, int frame_duration_ms, size_t size) {
            auto packet = std::make_unique<AudioStreamPacket>();
            packet->sample_rate = sample_rate;
            packet->frame_duration = frame_duration_ms;
            packet->payload.resize(size);
            std::memcpy(packet->payload.data(), data, size);
            PushPacketToDecodeQueue(std::move(packet), true);
        });
    demuxer->Reset();
    demuxer->Process(buf, size);
}

bool AudioService::IsIdle() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    return audio_encode_queue_.empty() && IsPlaybackDrainedLocked() && audio_testing_queue_.empty();
}

bool AudioService::IsPlaybackIdle() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    return IsPlaybackDrainedLocked();
}

void AudioService::ResetDecoder() {
    bool notify_drained = false;
    {
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        ++playback_generation_;
        std::unique_lock<std::mutex> decoder_lock(decoder_mutex_);
        if (opus_decoder_ != nullptr) {
            esp_opus_dec_reset(opus_decoder_);
        }
        decoder_lock.unlock();
        timestamp_queue_.clear();
        audio_decode_queue_.clear();
        audio_playback_queue_.clear();
        audio_testing_queue_.clear();
        notify_drained = MarkPlaybackDrainedLocked();
        audio_queue_cv_.notify_all();
    }
    if (notify_drained && callbacks_.on_playback_drained) {
        callbacks_.on_playback_drained();
    }
}

bool AudioService::IsPlaybackDrainedLocked() const {
    return audio_decode_queue_.empty() && audio_playback_queue_.empty() && !decode_in_flight_ &&
           !output_in_flight_;
}

bool AudioService::MarkPlaybackDrainedLocked() {
    if (!IsPlaybackDrainedLocked() || playback_drained_notified_) {
        return false;
    }
    playback_drained_notified_ = true;
    return true;
}

void AudioService::CheckAndUpdateAudioPowerState() {
    auto now = std::chrono::steady_clock::now();
    auto input_elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - last_input_time_).count();
    auto output_elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - last_output_time_).count();
    if (input_elapsed > AUDIO_POWER_TIMEOUT_MS && codec_->input_enabled()) {
        // ADC continuous start/stop must run in the same task. Wake the audio
        // input task instead of closing the codec from the esp_timer task.
        xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_INPUT_STOP_REQUEST);
    }
    if (output_elapsed > AUDIO_POWER_TIMEOUT_MS && codec_->output_enabled()) {
        // Keep TX clock when duplex RX is active; otherwise RX may stall on some boards.
        if (!(codec_->duplex() && codec_->input_enabled())) {
            codec_->EnableOutput(false);
        }
    }
    if (!codec_->input_enabled() && !codec_->output_enabled()) {
        esp_timer_stop(audio_power_timer_);
    }
}

void AudioService::SetModelsList(srmodel_list_t* models_list) {
    if (audio_engine_initialized_ && models_list_ != models_list) {
        ESP_LOGW(TAG, "Ignoring speech model replacement after audio engine initialization");
        return;
    }
    models_list_ = models_list;
}

bool AudioService::IsAfeWakeWord() {
    return audio_engine_initialized_ && audio_engine_->IsAfeWakeWord();
}

bool AudioService::InitializeAudioEngine() {
    if (!audio_engine_) {
        return false;
    }
    if (audio_engine_initialized_) {
        return true;
    }
    if (!audio_engine_->Initialize(codec_, OPUS_FRAME_DURATION_MS, models_list_)) {
        ESP_LOGE(TAG, "Failed to initialize audio engine");
        return false;
    }
    audio_engine_initialized_ = true;
    audio_engine_->EnableDeviceAec(device_aec_enabled_);
    return true;
}
