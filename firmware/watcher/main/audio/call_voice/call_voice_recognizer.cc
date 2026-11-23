// 文件说明：按有界帧驱动 MultiNet 的接听词识别，命令表随模型生命周期独占。
#include "call_voice_recognizer.h"

#include <algorithm>
#include <cstring>

#include <esp_err.h>
#include <esp_mn_models.h>
#include <esp_mn_speech_commands.h>

std::atomic<bool> CallVoiceRecognizer::owned_{false};

CallVoiceRecognizer::~CallVoiceRecognizer() { Stop(); }

bool CallVoiceRecognizer::Start(srmodel_list_t* models) {
    if (owns_ || models == nullptr) return false;
    bool expected = false;
    if (!owned_.compare_exchange_strong(expected, true)) return false;
    owns_ = true;
    auto* name = esp_srmodel_filter(models, ESP_MN_PREFIX, "cn");
    if (name == nullptr || (model_ = esp_mn_handle_from_name(name)) == nullptr ||
        (data_ = model_->create(name, 3000)) == nullptr) {
        Stop();
        return false;
    }
    const int samples = model_->get_samp_chunksize(data_);
    if (samples <= 0 || static_cast<std::size_t>(samples) > kMaxChunkSamples ||
        model_->get_samp_rate(data_) != 16000 ||
        esp_mn_commands_clear() != ESP_OK ||
        esp_mn_commands_add(1, "jie ting dian hua") != ESP_OK ||
        esp_mn_commands_update() != nullptr) {
        Stop();
        return false;
    }
    // SDK 返回值并非通用错误码；阈值固定在其合法范围内，效果须实机校准。
    model_->set_det_threshold(data_, 0.5f);
    chunk_ = static_cast<std::size_t>(samples);
    buffered_ = 0;
    detected_ = false;
    return true;
}

bool CallVoiceRecognizer::Feed(const std::int16_t* samples, std::size_t count) {
    if (data_ == nullptr || samples == nullptr || detected_) return false;
    while (count > 0) {
        const auto n = std::min(count, chunk_ - buffered_);
        std::memcpy(frame_ + buffered_, samples, n * sizeof(*samples));
        samples += n;
        count -= n;
        buffered_ += n;
        if (buffered_ != chunk_) continue;
        buffered_ = 0;
        const auto state = model_->detect(data_, frame_);
        if (state == ESP_MN_STATE_DETECTED) {
            const auto* result = model_->get_results(data_);
            if (result != nullptr) {
                const int nresult = std::min(result->num, ESP_MN_RESULT_MAX_NUM);
                for (int i = 0; i < nresult; ++i) {
                    if (result->command_id[i] == 1) detected_ = true;
                }
            }
            model_->clean(data_);
            if (detected_) return true;
        } else if (state == ESP_MN_STATE_TIMEOUT) {
            model_->clean(data_);
        }
    }
    return false;
}

void CallVoiceRecognizer::Stop() {
    if (data_ != nullptr) model_->destroy(data_);
    data_ = nullptr;
    model_ = nullptr;
    chunk_ = buffered_ = 0;
    detected_ = false;
    if (owns_) {
        owns_ = false;
        owned_.store(false);
    }
}
