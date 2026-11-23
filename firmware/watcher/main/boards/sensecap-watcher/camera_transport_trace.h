// Watcher 相机传输诊断：通过公开 IO 接口记录查询、读取和图像回调时刻，不改供应商驱动。
#pragma once

#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <sscma_client_io_interface.h>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <vector>

// CameraTransportTrace 借用 inner，并保留其总线 handle；调用方须先销毁客户端再销毁本包装。
// 只在 reset 后且 active 为真时记录，固定容量覆盖最旧样本，不记录图像或凭据正文。
class CameraTransportTrace final : public sscma_client_io_t {
public:
    // 构造函数包装公开回调；inner 和 active 由相机持有，生命周期覆盖客户端任务。
    CameraTransportTrace(sscma_client_io_t* inner, const std::atomic<bool>* active)
        : inner_(inner), active_(active) {
        handle = inner->handle;
        del = [](sscma_client_io_t* io) {
            auto* self = static_cast<CameraTransportTrace*>(io);  // 当前借用包装。
            return self->inner_->del(self->inner_);
        };
        write = [](sscma_client_io_t* io, const void* data, size_t size) {
            auto* self = static_cast<CameraTransportTrace*>(io);  // 写命令保持原驱动行为。
            return self->inner_->write(self->inner_, data, size);
        };
        flush = [](sscma_client_io_t* io) {
            auto* self = static_cast<CameraTransportTrace*>(io);  // 清空仍由原驱动实现。
            return self->inner_->flush(self->inner_);
        };
        read = [](sscma_client_io_t* io, void* data, size_t size) {
            auto* self = static_cast<CameraTransportTrace*>(io);  // 一次公开读取可含多个 SPI 分块。
            const int64_t begin = esp_timer_get_time();  // 原驱动读取开始，包含等待及总线传输。
            const esp_err_t result = self->inner_->read(self->inner_, data, size);  // 原始错误码。
            self->Record('R', begin, esp_timer_get_time(), size, result);
            return result;
        };
        available = [](sscma_client_io_t* io, size_t* size) {
            auto* self = static_cast<CameraTransportTrace*>(io);  // 查询可读长度的原驱动入口。
            const int64_t begin = esp_timer_get_time();           // 包含握手等待和可用长度查询。
            const esp_err_t result = self->inner_->available(self->inner_, size);  // 保留原结果。
            self->Record('A', begin, esp_timer_get_time(), result == ESP_OK ? *size : 0, result);
            return result;
        };
    }
    // 析构时客户端任务已停止，只释放诊断内存；原 IO 的所有权不变。
    ~CameraTransportTrace() { heap_caps_free(samples_); }
    // MarkFrame 标记已解析图像进入回调的时刻；不将其误当作传感器曝光时刻。
    void MarkFrame(int64_t received_us) { Record('F', received_us, received_us, 0, ESP_OK); }
    // Print 的 reset 清空并按需分配诊断环；普通导出在锁内复制、锁外打印，不阻塞 IO。
    void Print(bool reset) {
        std::vector<Sample> copy;  // 锁外输出的有限快照。
        bool enabled = false;      // 分配失败不会影响相机读写。
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (reset) {
                if (!samples_)
                    samples_ = static_cast<Sample*>(heap_caps_calloc(
                        kCapacity, sizeof(Sample), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                count_ = next_ = 0;
            } else if (samples_) {
                copy.reserve(count_);
                const size_t first = (next_ + kCapacity - count_) % kCapacity;  // 最旧事件。
                for (size_t index = 0; index < count_; ++index)
                    copy.push_back(samples_[(first + index) % kCapacity]);
            }
            enabled = samples_ != nullptr;
            enabled_.store(enabled);
        }
        printf("[UR_TRANSPORT_PERF] BEGIN count=%u enabled=%d reset=%d\n",
               static_cast<unsigned>(copy.size()), enabled, reset);
        for (const auto& sample : copy)
            printf("[UR_TRANSPORT_PERF] %c,%lld,%lld,%u,%d\n", sample.kind,
                   static_cast<long long>(sample.begin), static_cast<long long>(sample.end),
                   static_cast<unsigned>(sample.bytes), static_cast<int>(sample.result));
        printf("[UR_TRANSPORT_PERF] END\n");
    }

private:
    // Sample 使用主控微秒时钟，A/R 是查询/读取区间，F 是图像回调入口的时间点。
    struct Sample {
        int64_t begin;   // 操作开始或回调进入时刻。
        int64_t end;     // 操作返回时刻，包含驱动内部等待。
        uint32_t bytes;  // 查询可用长度或本次读取长度，不保存图像内容。
        int32_t result;  // 原驱动错误码。
        char kind;       // A 查询、R 读取、F 图像回调。
    };
    static constexpr size_t kCapacity = 128;  // 固定环上限，防止长期预览积累诊断数据。
    // Record 不改变驱动结果或调度；诊断关闭、未采集时立即返回。
    void Record(char kind, int64_t begin, int64_t end, size_t bytes, esp_err_t result) {
        if (!enabled_.load() || !active_->load())
            return;
        std::lock_guard<std::mutex> lock(mutex_);
        samples_[next_] = {begin, end, static_cast<uint32_t>(bytes), result, kind};
        next_ = (next_ + 1) % kCapacity;
        if (count_ < kCapacity)
            ++count_;
    }
    sscma_client_io_t* inner_;          // 原始 IO 的借用指针，总线与锁仍由原驱动维护。
    const std::atomic<bool>* active_;   // 相机连续流状态，停流后保留最后一段有效样本。
    std::mutex mutex_;                  // 只保护记录和快照，不持锁执行 IO 或打印。
    std::atomic<bool> enabled_{false};  // 尚未 reset 时不申请诊断缓冲。
    Sample* samples_ = nullptr;         // 按需 PSRAM 环，仅本包装析构时释放。
    size_t count_ = 0, next_ = 0;       // 有效记录数和下一写入位置。
};
