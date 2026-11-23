// 文件说明：人际UDP对端画面解码；只保留最新JPEG，低优先级处理，慢帧或音频过载后本通话停显。
#pragma once
#include "sdkconfig.h"
#if CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/idf_additions.h>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>
#include "esp_jpeg_dec.h"
#include "ur/device/udp_av.hpp"
#if CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER && CONFIG_UR_HUMAN_CALL_UDP_AV && \
    CONFIG_JD_USE_ROM
#include "jpeg_decoder.h"
#include "watcher_call_jpeg.h"
#endif
#if CONFIG_BOARD_TYPE_ESP_SPARKBOT
#include "boards/espressif/esp-sparkbot/sparkbot_jpeg_cadence.h"
#endif

// UrCallJpegOutput 独占解码器和双RGB缓冲；停止必须等工作线程实际退出，缓冲不跨回调借用。
class UrCallJpegOutput {
public:
    // FrameCallback 空指针通知UI清除旧画面，不改变通话控制和声音。
    using FrameCallback = std::function<void(const uint8_t*, int, int)>;
    ~UrCallJpegOutput() { Stop(); }
    // Start 仅成功创建一次线程；分配失败保持停显。
    bool Start(FrameCallback callback) {
        if (task_)
            return false;
        callback_ = std::move(callback);
        // JPEG解码固定在核1、与AFE同级且低于信令任务，视频过载时仍让出信令时间片。
        return xTaskCreatePinnedToCoreWithCaps(
                   [](void* arg) {
                       auto* self = static_cast<UrCallJpegOutput*>(arg);
                       self->Run();
                       self->done_.store(true);
                       vTaskDeleteWithCaps(nullptr);
                   },
                   "call_jpeg", 12288, this, 4, &task_, 1,
                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS;
    }
    // Submit 只替换一张有界最新图；网络任务不解码或等待屏幕锁。
    void Submit(std::vector<uint8_t> image) {
        if (image.empty() || image.size() > ur::device::UdpAV::kMaxJPEG || disabled_.load())
            return;
        std::lock_guard<std::mutex> lock(mutex_);
        latest_ = std::move(image);
        received_ = esp_timer_get_time() / 1000;
        submitted_.fetch_add(1, std::memory_order_relaxed);
    }
    // ProtectAudio 永久停显到当前通话结束，不在调用线程等待解码退出。
    void ProtectAudio() {
        if (!disabled_.exchange(true))
            ESP_LOGW("call_jpeg", "remote jpeg protected for audio");
    }
    // Disabled 返回本通话不可逆停显事实，供信令请求平台停止视频转发。
    bool Disabled() const { return disabled_.load(); }
#if CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
    // WaitingForDma 表示暂不解码视频，收包线程可跳过视频包而继续处理音频。
    bool WaitingForDma() const { return dma_waiting_.load(); }
#endif
    // Stop 等待退出后才允许销毁实例；不伪造任务完成回执。
    void Stop() {
        stop_.store(true);
        while (task_ && !done_.load())
            vTaskDelay(pdMS_TO_TICKS(10));
        task_ = nullptr;
    }

private:
    // Run 独占JPEG上下文；先校验尺寸再解码，画面失鲜或连续慢帧清屏。
    void Run() {
        jpeg_dec_handle_t decoder = nullptr;
        jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
        cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
        cfg.rotate = JPEG_ROTATE_0D;
        uint8_t* buffers[2] = {};
#if CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER && CONFIG_UR_HUMAN_CALL_UDP_AV && \
    CONFIG_JD_USE_ROM
        // ROM TJpgD推荐3.1KB工作区；固定4KB PSRAM避免SIMD解码器占用紧缺内部DMA堆。
        constexpr size_t kRomWorkBytes = 4096;
        auto* work = static_cast<uint8_t*>(
            heap_caps_aligned_alloc(16, kRomWorkBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        bool ready = work != nullptr;  // 回退上下文逐帧创建并释放，不持有内部工作区。
#else
        bool ready = jpeg_dec_open(&cfg, &decoder) == JPEG_ERR_OK;
#endif
        for (auto& p : buffers) {
            p = static_cast<uint8_t*>(
                heap_caps_aligned_alloc(16, 240 * 240 * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            ready = ready && p;
        }
        ur::device::RemoteVideoGate gate;
        unsigned index = 0;
        bool shown = false;
        int64_t shown_at = 0;
#if CONFIG_BOARD_TYPE_ESP_SPARKBOT
        constexpr int64_t kShownTimeoutMs =
            1200;  // 覆盖坦克解码节拍及调度抖动，避免持续播放时闪屏。
#else
        constexpr int64_t kShownTimeoutMs =
            500;  // Watcher保持原有失鲜时限，DMA等待不延长画面寿命。
#endif
#if CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
        constexpr int64_t kDmaWaitLimitMs = 1000;  // 等待超时仍执行既有不可逆保护。
        constexpr int64_t kDmaStableMs =
            50;  // 覆盖数个音频帧，避免250ms窗口被周期视频突发反复打断。
        int64_t dma_wait_at = 0, dma_good_at = 0;  // 仅解码线程访问，限定等待和稳定观察期。
#endif
#if CONFIG_BOARD_TYPE_ESP_SPARKBOT
        sparkbot::JpegCadence cadence;  // 坦克双核音频实测繁忙，限制下行解码占用。
#endif
        while (ready && !stop_.load()) {
            const auto now = esp_timer_get_time() / 1000;
            const auto largest_dma = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
            // free_dma 补足连续块之外的总量约束，避免碎片虽小却误判PSRAM解码不可用。
            const auto free_dma = heap_caps_get_free_size(MALLOC_CAP_DMA);
#if CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER && CONFIG_UR_HUMAN_CALL_UDP_AV && \
    CONFIG_JD_USE_ROM
            const bool dma_ready = WatcherRomJpegDmaReady(largest_dma, free_dma);
#else
            const bool dma_ready = largest_dma >= 8192;  // 其他解码器维持原连续块要求。
#endif
            // 暂停解码时仍检查画面年龄；短时内存波动保留有效帧，过期则按原时限清除。
            if (shown && now - shown_at >= kShownTimeoutMs) {
                if (callback_)
                    callback_(nullptr, 0, 0);
                shown = false;
            }
#if CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
            // 短暂不足先停解码及收包，保留尚未过期的画面；持续不足仍永久停轨。
            if (!disabled_.load()) {
                if (!dma_ready && !dma_waiting_.load()) {
                    dma_wait_at = now;
                    dma_good_at = 0;
                    dma_waiting_.store(true);
                    ESP_LOGW("call_jpeg", "remote jpeg waiting dma=%u free=%u",
                             static_cast<unsigned>(largest_dma), static_cast<unsigned>(free_dma));
                }
                if (dma_waiting_.load()) {
                    if (dma_ready) {
                        if (!dma_good_at)
                            dma_good_at = now;
                        if (now - dma_good_at >= kDmaStableMs) {
                            dma_waiting_.store(false);
                            ESP_LOGI("call_jpeg", "remote jpeg resumed dma=%u free=%u",
                                     static_cast<unsigned>(largest_dma),
                                     static_cast<unsigned>(free_dma));
                        }
                    } else {
                        dma_good_at = 0;
                        // 期限到达时仍缺内存才永久停显；已恢复的窗口最多再等完50ms稳定期。
                        if (now - dma_wait_at >= kDmaWaitLimitMs) {
                            dma_waiting_.store(false);
                            disabled_.store(true);
                        }
                    }
                    if (dma_waiting_.load()) {
                        vTaskDelay(pdMS_TO_TICKS(10));
                        continue;
                    }
                }
            }
            if (disabled_.load())
                dma_waiting_.store(false);
#endif
            if (disabled_.load() || !dma_ready) {
                if (!disable_logged_) {
                    ESP_LOGW("call_jpeg",
                             "remote jpeg disabled dma=%u free=%u submitted=%u decoded=%u",
                             static_cast<unsigned>(largest_dma), static_cast<unsigned>(free_dma),
                             submitted_.load(std::memory_order_relaxed), decoded_);
                    disable_logged_ = true;
                }
                disabled_.store(true);
                if (shown && callback_)
                    callback_(nullptr, 0, 0);
                shown = false;
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
#if CONFIG_BOARD_TYPE_ESP_SPARKBOT
            // 节拍未到仍保留最新帧，网络更新会覆盖旧帧，停流和过期检查不受阻塞。
            if (!cadence.Ready(now)) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
#endif
            std::vector<uint8_t> image;
            int64_t received = 0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                image.swap(latest_);
                received = received_;
            }
            if (image.empty() || now - received >= 500) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
#if CONFIG_BOARD_TYPE_ESP_SPARKBOT
            cadence.Mark(now);
#endif
            const auto started = esp_timer_get_time();
            int width = 0, height = 0;  // 回调尺寸必须对应本次实际解码成功的图片。
#if CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER && CONFIG_UR_HUMAN_CALL_UDP_AV && \
    CONFIG_JD_USE_ROM
            if (WatcherRomJpegDimensions(image, width, height)) {
                esp_jpeg_image_cfg_t rom_cfg{};  // 使用现有组件的公开工作区接口与RGB565小端输出。
                rom_cfg.indata = image.data();
                rom_cfg.indata_size = image.size();
                rom_cfg.outbuf = buffers[index];
                rom_cfg.outbuf_size = 240 * 240 * 2;
                rom_cfg.out_format = JPEG_IMAGE_FORMAT_RGB565;
                rom_cfg.out_scale = JPEG_IMAGE_SCALE_0;
                rom_cfg.advanced.working_buffer = work;
                rom_cfg.advanced.working_buffer_size = kRomWorkBytes;
                esp_jpeg_image_output_t output{};  // 解码器返回尺寸须与预检一致，避免误提交缓冲。
                if (esp_jpeg_decode(&rom_cfg, &output) != ESP_OK || output.width != width ||
                    output.height != height ||
                    output.output_len != static_cast<size_t>(width * height * 2))
                    continue;
            } else
#endif
            {
                // 回退分配后仍保留8KB DMA余量；逐帧所有权使错误分支也能归还工作区。
#if CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER && CONFIG_UR_HUMAN_CALL_UDP_AV && \
    CONFIG_JD_USE_ROM
                jpeg_dec_handle_t active_decoder = nullptr;  // 回退句柄只在本帧作用域内有效。
                std::unique_ptr<void, decltype(&jpeg_dec_close)> fallback_decoder(nullptr,
                                                                                  jpeg_dec_close);
                const auto opened =
                    jpeg_dec_open(&cfg, &active_decoder);  // 句柄和创建结果分别核对。
                fallback_decoder.reset(active_decoder);
                if (opened != JPEG_ERR_OK || !active_decoder ||
                    heap_caps_get_largest_free_block(MALLOC_CAP_DMA) < 8192)
                    continue;
#else
                auto* active_decoder = decoder;  // 其他构建保留原来的整通话上下文。
#endif
                jpeg_dec_io_t io = {};
                jpeg_dec_header_info_t header = {};
                io.inbuf = image.data();
                io.inbuf_len = image.size();
                if (jpeg_dec_parse_header(active_decoder, &io, &header) != JPEG_ERR_OK ||
                    header.width <= 0 || header.height <= 0 || header.width > 240 ||
                    header.height > 240)
                    continue;
                int output_size = 0;
                if (jpeg_dec_get_outbuf_len(active_decoder, &output_size) != JPEG_ERR_OK ||
                    output_size != header.width * header.height * 2)
                    continue;
                io.outbuf = buffers[index];
                if (jpeg_dec_process(active_decoder, &io) != JPEG_ERR_OK)
                    continue;
                width = header.width;
                height = header.height;
            }
            const auto decoded_at = esp_timer_get_time();
            if (callback_)
                callback_(buffers[index], width, height);
            shown = true;
            shown_at = received;
            index ^= 1;
            ++decoded_;
            if (decoded_ % 10 == 1) {
                ESP_LOGI("call_jpeg", "remote jpeg decoded #%u submitted=%u size=%u", decoded_,
                         submitted_.load(std::memory_order_relaxed),
                         static_cast<unsigned>(image.size()));
            }
            const auto elapsed_ms = (esp_timer_get_time() - started) / 1000;
            if (elapsed_ms > 200) {
                ESP_LOGW("call_jpeg", "remote jpeg slow total_ms=%u decode_ms=%u",
                         static_cast<unsigned>(elapsed_ms),
                         static_cast<unsigned>((decoded_at - started) / 1000));
            }
            // 解码偶发跨核等待可达数百毫秒；保留连续慢帧门控，避免单次调度抖动永久停显。
            if (!gate.completed(now, elapsed_ms, 400)) {
                ESP_LOGW("call_jpeg", "remote jpeg disabled slow_decode_ms=%u decoded=%u",
                         static_cast<unsigned>(elapsed_ms), decoded_);
                disabled_.store(true);
                disable_logged_ = true;
            }
            vTaskDelay(1);
        }
        if (shown && callback_)
            callback_(nullptr, 0, 0);
        if (decoder)
            jpeg_dec_close(decoder);
        for (auto* p : buffers)
            heap_caps_free(p);
#if CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER && CONFIG_UR_HUMAN_CALL_UDP_AV && \
    CONFIG_JD_USE_ROM
        heap_caps_free(work);
#endif
    }
    FrameCallback callback_;       // 工作线程独占调用，Stop后不再触发。
    TaskHandle_t task_ = nullptr;  // 控制线程创建和回收，不作为停止完成事实。
    std::atomic<bool> stop_{false}, done_{false}, disabled_{false};  // 退出与不可逆停显门。
#if CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
    std::atomic<bool> dma_waiting_{false};  // 本地临时暂停，避免等待期间继续组装视频。
#endif
    std::mutex mutex_;                    // 仅保护单帧交换。
    std::vector<uint8_t> latest_;         // 最多48KiB。
    int64_t received_ = 0;                // 最新图单调到达时间。
    std::atomic<unsigned> submitted_{0};  // 已交给解码队列的完整JPEG数。
    unsigned decoded_ = 0;                // 已完成JPEG解码并提交回调的帧数。
    bool disable_logged_ = false;         // 仅记录一次停显原因。
};
#endif
