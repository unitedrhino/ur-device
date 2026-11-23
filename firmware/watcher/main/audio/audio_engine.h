// 文件说明：跨芯片音频引擎接口，输出聚帧与网络编解码格式独立。
#ifndef AUDIO_ENGINE_H
#define AUDIO_ENGINE_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <model_path.h>

#include "audio_codec.h"

class AudioEngine {
public:
    virtual ~AudioEngine() = default;

    virtual bool Initialize(AudioCodec* codec, int frame_duration_ms, srmodel_list_t* models_list) = 0;
    virtual void Feed(std::vector<int16_t>&& data) = 0;
    /* FeedReference 注入播放侧数字参考（16k 单声道）；仅软件回采 AEC 使用，
     * 默认实现丢弃数据，不改变原始单麦克风引擎行为。 */
    virtual void FeedReference(std::vector<int16_t>&& data) { (void)data; }

    virtual void EnableWakeWordDetection(bool enable) = 0;
    virtual void EnableVoiceProcessing(bool enable) = 0;
    virtual void EnableDeviceAec(bool enable) = 0;
    // EnableLowLatencyOutput 选择原生PCM块及时交付；不支持的引擎沿用初始化粒度。
    virtual void EnableLowLatencyOutput(bool enable) { (void)enable; }

    virtual bool HasWakeWord() const = 0;
    virtual bool IsWakeWordDetectionEnabled() const = 0;
    virtual bool IsVoiceProcessingEnabled() const = 0;
    virtual bool IsAfeWakeWord() const = 0;
    virtual size_t GetFeedSize() const = 0;

    virtual void OnWakeWordDetected(std::function<void(const std::string& wake_word)> callback) = 0;
    virtual void OnOutput(std::function<void(std::vector<int16_t>&& data)> callback) = 0;
    virtual void OnVadStateChange(std::function<void(bool speaking)> callback) = 0;

    virtual void EncodeWakeWordData() = 0;
    virtual bool GetWakeWordOpus(std::vector<uint8_t>& opus) = 0;
    virtual const std::string& GetLastDetectedWakeWord() const = 0;
};

#endif
