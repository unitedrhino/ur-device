// Watcher相机SPI读流缓冲：预留内部DMA内存，避免通话时每次从PSRAM申请大块临时搬运区。
#pragma once

#include <esp_heap_caps.h>
#include <sscma_client_io_interface.h>
#include <algorithm>
#include <cstring>
#include <mutex>

// CameraDmaIo 借用原IO并保持原句柄；客户端停止后才销毁包装，不修改供应商驱动。
class CameraDmaIo final : public sscma_client_io_t {
public:
    // 构造时预留一块16字节对齐的DMA区；失败时读流显式返回内存不足，不进入危险驱动路径。
    explicit CameraDmaIo(sscma_client_io_t* inner) : inner_(inner) {
        handle = inner->handle;
        buffer_ = static_cast<uint8_t*>(heap_caps_aligned_alloc(
            16, kBufferBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
        del = [](sscma_client_io_t* io) {
            auto* self = static_cast<CameraDmaIo*>(io);
            return self->inner_->del(self->inner_);
        };
        write = [](sscma_client_io_t* io, const void* data, size_t size) {
            auto* self = static_cast<CameraDmaIo*>(io);
            return self->inner_->write(self->inner_, data, size);
        };
        flush = [](sscma_client_io_t* io) {
            auto* self = static_cast<CameraDmaIo*>(io);
            return self->inner_->flush(self->inner_);
        };
        available = [](sscma_client_io_t* io, size_t* size) {
            auto* self = static_cast<CameraDmaIo*>(io);
            return self->inner_->available(self->inner_, size);
        };
        read = [](sscma_client_io_t* io, void* data, size_t size) {
            return static_cast<CameraDmaIo*>(io)->Read(data, size);
        };
    }
    // 析构只回收本包装的DMA区；原IO由既有客户端的del入口管理。
    ~CameraDmaIo() { heap_caps_free(buffer_); }

private:
    // Read 保持原始字节顺序及错误码；大块按4092分拆，末尾不足4字节独立读，绝不多读流数据。
    esp_err_t Read(void* data, size_t size) {
        if (size == 0)
            return ESP_OK;
        if (data == nullptr)
            return ESP_ERR_INVALID_ARG;
        std::lock_guard<std::mutex> lock(read_mutex_);
        if (buffer_ == nullptr)
            return ESP_ERR_NO_MEM;
        auto* output = static_cast<uint8_t*>(data);
        size_t consumed = 0;
        while (consumed < size) {
            const auto remaining = size - consumed;
            // 原驱动单包上限4095；四字节对齐让IDF直接使用预留区，避免按大帧临时申请RX区。
            const auto chunk =
                remaining >= 4 ? std::min(kAlignedChunk, remaining & ~size_t(3)) : remaining;
            const auto result = inner_->read(inner_, buffer_, chunk);
            if (result != ESP_OK)
                return result;
            std::memcpy(output + consumed, buffer_, chunk);
            consumed += chunk;
        }
        return ESP_OK;
    }
    static constexpr size_t kBufferBytes = 4096;   // 唯一常驻内部DMA区，避免每帧重新分配。
    static constexpr size_t kAlignedChunk = 4092;  // 小于供应商4095字节包上限的最大四字节倍数。
    sscma_client_io_t* inner_;                     // 原SPI实现，仅借用。
    uint8_t* buffer_ = nullptr;                    // Read独占的DMA区；成功后才向调用方复制数据。
    std::mutex read_mutex_;                        // 防止并发读复用同一块DMA缓冲。
};
