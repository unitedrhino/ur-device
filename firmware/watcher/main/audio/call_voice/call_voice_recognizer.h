// 文件说明：坦克测试固件的离线接听词识别核心；由单一工作任务持有和调用。
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include <esp_mn_iface.h>
#include <model_path.h>

// CallVoiceRecognizer 独占 MultiNet 命令表，识别明确的“接听电话”。
// 调用方负责来电代次校验、在非音频任务创建和销毁，并在停止后才允许启动通话媒体。
class CallVoiceRecognizer {
public:
    CallVoiceRecognizer() = default;
    ~CallVoiceRecognizer();
    CallVoiceRecognizer(const CallVoiceRecognizer&) = delete;
    CallVoiceRecognizer& operator=(const CallVoiceRecognizer&) = delete;

    // Start 借用已初始化模型列表，成功返回 true；模型列表须活到 Stop 完成。
    bool Start(srmodel_list_t* models);
    // Feed 接收 16 kHz 单声道样本，仅首次命中接听命令时返回 true。
    bool Feed(const std::int16_t* samples, std::size_t count);
    // Stop 释放命令模型及其进程级所有权，可重复调用。
    void Stop();

private:
    static constexpr std::size_t kMaxChunkSamples = 1024;  // 限定识别帧缓存，拒绝未知大帧。
    static std::atomic<bool> owned_;  // 仅本试验配置使用 MultiNet；防止多个来电识别器互相抢表。
    esp_mn_iface_t* model_ = nullptr;
    model_iface_data_t* data_ = nullptr;
    std::int16_t frame_[kMaxChunkSamples]{};
    std::size_t chunk_ = 0;
    std::size_t buffered_ = 0;
    bool owns_ = false;
    bool detected_ = false;
};
