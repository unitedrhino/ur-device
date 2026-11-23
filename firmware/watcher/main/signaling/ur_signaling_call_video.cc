/* 通话下行视频解码渲染实现：Annex-B NALU → esp_h264 软解（tinyh264）→ I420→RGB565
 * 整数转换 → 帧回调上抛。NALU 经有界队列从信令任务搬运到独立低优先级解码任务，
 * 积压时丢旧保新；解码错误后丢弃至下一个 IDR 再恢复送解，避免参考帧断裂花屏。 */
#include "ur_signaling_call_video.h"

#if CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <new>

#include "esp_h264_dec.h"
#include "esp_h264_dec_sw.h"

namespace ur::device {
namespace {

constexpr const char* kTag = "ur_sig_cv";
/* 通话内存取证日志前缀：与通话腿日志同前缀，主代理真机 grep 统一口径。 */
constexpr const char* kDiagTag = "ur_sig_call";
/* 堆水位取证节拍：通话期间周期打印 DMA/内部堆最大连续块与解码计数。 */
constexpr int64_t kDiagPeriodUs = 5000000;

/* NALU 队列深度：解码慢积压时 OnNalu 端丢最旧、送最新，逼近最新帧并限制内存。 */
constexpr UBaseType_t kQueueDepth = 12;
/* 解码任务栈：tinyh264 调用链浅，12KB 覆盖 SPS 解析与 I420 取帧路径。 */
constexpr uint32_t kTaskStackBytes = 12288;
/* 解码任务优先级低于音频AFE(3)，画面过载时先让音频采集运行。 */
constexpr UBaseType_t kTaskPriority = 2;
/* 队列空时等待节拍，决定 Stop 响应延迟上限。 */
constexpr int kQueueWaitMs = 100;
/* 关键帧重请求节流：PLI 风暴会挤占 RTCP/媒体带宽，1s 足够对端完成编码切换。 */
constexpr int64_t kKeyframeReqIntervalMs = 1000;
/* 解码分辨率硬上限：业务约定输入 ≤640x480，超限码流拒解防超大内存分配。 */
constexpr int kMaxWidth = 1280;
constexpr int kMaxHeight = 1024;

/* Annex-B 起始码之后 NAL 头第一字节的类型字段掩码与关键类型值。 */
constexpr uint8_t kNalTypeMask = 0x1F;
constexpr uint8_t kNalIdr = 5;
constexpr uint8_t kNalSps = 7;
constexpr uint8_t kNalPps = 8;

/* Clamp8 将整数限幅到 [0,255]，供 RGB 分量计算使用。 */
inline uint8_t Clamp8(int value) {
    return value < 0 ? 0 : (value > 255 ? 255 : static_cast<uint8_t>(value));
}

/* NaluType 解析 NALU 类型；nalu 指向带 4 字节起始码的完整 NALU，
 * 返回 1/5/7/8 等 nal_unit_type，非法输入返回 -1。兼容 3 字节起始码。 */
int NaluType(const std::uint8_t* nalu, std::size_t len) {
    if (nalu == nullptr || len < 4) {
        return -1;
    }
    if (nalu[0] == 0 && nalu[1] == 0 && nalu[2] == 1) {
        return nalu[3] & kNalTypeMask;
    }
    if (len >= 5 && nalu[0] == 0 && nalu[1] == 0 && nalu[2] == 0 && nalu[3] == 1) {
        return nalu[4] & kNalTypeMask;
    }
    return -1;
}

/* LogHeapDiag 打印通话期 DMA/内部堆水位快照（前缀 ur_sig_call）。
 * 用途：真机取证 DMA 最大连续块在哪个事件后崩塌；stage 为事件名。
 * dma_largest 即 SPI/lwIP 临时 DMA 分配的可用连续块，是相机守卫同一口径。 */
void LogHeapDiag(const char* stage) {
    ESP_LOGI(kDiagTag, "heap diag[%s]: dma_largest=%u int_free=%u int_largest=%u total_free=%u",
             stage, static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA)),
             static_cast<unsigned>(esp_get_free_internal_heap_size()),
             static_cast<unsigned>(
                 heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(esp_get_free_heap_size()));
}

/* CallVideoRenderer 的解码渲染实现：仅本文件可见，经 GetCallVideoRenderer 暴露。
 * 生命周期约定：构造/析构只发生在单例懒创建路径；Start/Stop 可重复配对。 */
class CallVideoRendererImpl : public CallVideoRenderer {
public:
    CallVideoRendererImpl() = default;
    ~CallVideoRendererImpl() override { Stop(); }

    /* Start 创建软解码器与解码任务并注册帧回调；已运行时仅替换回调。
     * 返回 false 表示解码器或任务资源不足不可用。 */
    bool Start(FrameCallback on_frame) override {
        if (on_frame == nullptr) {
            ESP_LOGE(kTag, "frame callback required");
            return false;
        }
        std::lock_guard<std::mutex> lock(control_mutex_);
        if (running_.load()) {
            std::lock_guard<std::mutex> cb_lock(callback_mutex_);
            on_frame_ = std::move(on_frame);
            return true;
        }
        if (decoder_ == nullptr) {
            LogHeapDiag("pre-dec");
            esp_h264_dec_cfg_t cfg = {};
            /* 软解仅支持 I420 输出，分辨率由码流 SPS 决定，无须配置。 */
            cfg.pic_type = ESP_H264_RAW_FMT_I420;
            if (esp_h264_dec_sw_new(&cfg, &decoder_) != ESP_H264_ERR_OK || decoder_ == nullptr) {
                ESP_LOGE(kTag, "h264 sw decoder create failed");
                decoder_ = nullptr;
                return false;
            }
            if (decoder_->open(decoder_) != ESP_H264_ERR_OK) {
                ESP_LOGE(kTag, "h264 sw decoder open failed");
                decoder_->del(decoder_);
                decoder_ = nullptr;
                return false;
            }
            LogHeapDiag("post-dec");
        }
        if (queue_ == nullptr) {
            /* 队列只存指针项；创建后进程内复用，避免与在途 OnNalu 的释放竞态。 */
            queue_ = xQueueCreate(kQueueDepth, sizeof(NaluItem));
            if (queue_ == nullptr) {
                ESP_LOGE(kTag, "nalu queue create failed");
                decoder_->close(decoder_);
                decoder_->del(decoder_);
                decoder_ = nullptr;
                return false;
            }
        }
        DrainQueue();
        /* 启动即进入等 IDR 态：下行流加入时浏览器已在发帧，直接送解流中段的
         * delta 帧必然失败（真机 dec_err=1 实证），并使后续帧全部被丢弃。
         * 等 IDR 期间由 TakeKeyframeRequest 驱动 Poll 发 PLI 索取关键帧。 */
        wait_idr_.store(true);
        RequestKeyframeNow();
        stat_nalu_in_.store(0);
        stat_nalu_ovf_.store(0);
        stat_dec_calls_.store(0);
        stat_dec_err_.store(0);
        stat_idr_drop_.store(0);
        stat_frames_.store(0);
        {
            std::lock_guard<std::mutex> cb_lock(callback_mutex_);
            on_frame_ = std::move(on_frame);
        }
        /* 输出缓冲在 Start 预分配 480x368 单帧（353KB）：解码器启动后再分配会因
         * PSRAM 碎片/占用失败，设备只出 1-2 帧就黑屏（真机实证）。预留 614KB
         * 会饿死相机编码器，353KB 只覆盖下行实际分辨率。 */
        constexpr std::size_t kInitialBytes = 480 * 368 * 2;
        if (buffers_[0] == nullptr) {
            buffers_[0] = static_cast<std::uint8_t*>(
                heap_caps_malloc(kInitialBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (buffers_[0] != nullptr) {
                buffer_capacity_ = kInitialBytes;
            } else {
                ESP_LOGW(kTag, "rgb565 prealloc %u bytes failed",
                         static_cast<unsigned>(kInitialBytes));
            }
        }
        running_.store(true);
        /* xTaskCreateWithCaps 需要原生句柄指针，创建成功后再存入原子成员。 */
        TaskHandle_t created_task = nullptr;
        if (xTaskCreateWithCaps(TaskEntry, "ur_sig_cv", kTaskStackBytes, this, kTaskPriority,
                                &created_task, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
            ESP_LOGE(kTag, "decode task create failed");
            running_.store(false);
            task_ = nullptr;
            decoder_->close(decoder_);
            decoder_->del(decoder_);
            decoder_ = nullptr;
            return false;
        }
        task_ = created_task;
        LogHeapDiag("renderer-start");
        ESP_LOGI(kTag, "call video renderer started");
        return true;
    }

    /* Stop 停止解码任务、销毁解码器并清空队列；回调此后不再触发。
     * 注意：禁止在帧回调上下文调用——等待任务退出会与回调互相死锁。 */
    void Stop() override {
        std::lock_guard<std::mutex> lock(control_mutex_);
        if (!running_.load() && task_.load() == nullptr && decoder_ == nullptr) {
            return;
        }
        running_.store(false);
        // 慢IDR可能超过三秒，只有真实线程退出才能释放解码器及RGB输出，禁止超时后伪造空句柄。
        while (task_.load(std::memory_order_acquire) != nullptr) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (decoder_ != nullptr) {
            decoder_->close(decoder_);
            decoder_->del(decoder_);
            decoder_ = nullptr;
        }
        DrainQueue();
        {
            std::lock_guard<std::mutex> cb_lock(callback_mutex_);
            on_frame_ = nullptr;
        }
        FreeOutputBuffers();
        LogHeapDiag("renderer-stop");
    }

    /* OnNalu 把完整 NALU 拷贝进 PSRAM 后入队；满则丢最旧再送，拷贝失败直接丢弃。
     * 未 Start 时丢弃，可在网络/信令任务上下文安全调用。 */
    void OnNalu(const std::uint8_t* nalu, std::size_t len) override {
        if (!running_.load() || nalu == nullptr) {
            return;
        }
        if (NaluType(nalu, len) < 0) {
            return;
        }
        stat_nalu_in_.fetch_add(1);
        std::uint8_t* copy =
            static_cast<std::uint8_t*>(heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (copy == nullptr) {
            return;
        }
        memcpy(copy, nalu, len);
        NaluItem item{copy, static_cast<std::uint32_t>(len)};
        QueueHandle_t queue = queue_;
        if (queue == nullptr || xQueueSend(queue, &item, 0) != pdTRUE) {
            if (queue != nullptr) {
                /* 满队：丢最旧一个腾位后重送一次，仍失败则丢新数据。 */
                NaluItem dropped{};
                if (xQueueReceive(queue, &dropped, 0) == pdTRUE && dropped.data != nullptr) {
                    heap_caps_free(dropped.data);
                }
                if (xQueueSend(queue, &item, 0) == pdTRUE) {
                    stat_nalu_ovf_.fetch_add(1);
                    return;
                }
            }
            heap_caps_free(copy);
        }
    }

    /* TakeKeyframeRequest 由通话媒体桥在 ICE 连通后轮询：返回是否有一个待发
     * 关键帧请求（有则取走并应立即对下行流发送 PLI）。单线程模型内闭环，
     * 置位在渲染/网络任务、取走在信令任务，原子标志保证不重不漏。 */
    bool TakeKeyframeRequest() override {
        return keyframe_pending_.exchange(false, std::memory_order_acq_rel);
    }
    // LastFrameProcessingUs 只读最近成功帧，不将排队或网络耗时伪装成解码时间。
    uint32_t LastFrameProcessingUs() const override { return last_frame_processing_us_.load(); }

private:
    /* NaluItem 队列元素：指向 PSRAM 拷贝的所有权归队列消费方。 */
    struct NaluItem {
        std::uint8_t* data;
        std::uint32_t len;
    };

    /* RequestKeyframeNow 置位待发关键帧请求；按 1s 节流防止解码连续失败时
     * PLI 风暴（浏览器每响应一次 PLI 才产一个 IDR，连续请求只会挤占带宽）。 */
    void RequestKeyframeNow() {
        const int64_t now_ms = esp_timer_get_time() / 1000;
        int64_t last = last_keyframe_req_ms_.load(std::memory_order_relaxed);
        if (now_ms - last < kKeyframeReqIntervalMs) {
            return;
        }
        if (!last_keyframe_req_ms_.compare_exchange_strong(last, now_ms, std::memory_order_relaxed)) {
            return;
        }
        keyframe_pending_.store(true, std::memory_order_release);
    }

    /* TaskEntry 解码任务入口，退出前置空任务句柄供 Stop 回收。 */
    static void TaskEntry(void* context) {
        auto* self = static_cast<CallVideoRendererImpl*>(context);
        self->Run();
        self->task_ = nullptr;
        vTaskDelete(nullptr);
    }

    /* Run 解码主循环：阻塞取 NALU 逐个送解并上抛帧，Stop 置位后自然退出。
     * 每 5 秒打一次堆水位+计数取证日志，定位通话期 DMA 池崩塌事件。 */
    void Run() {
        int64_t last_diag_us = esp_timer_get_time();
        while (running_.load()) {
            NaluItem item{};
            if (xQueueReceive(queue_, &item, pdMS_TO_TICKS(kQueueWaitMs)) != pdTRUE) {
                item.data = nullptr;
            }
            if (item.data != nullptr) {
                ProcessNalu(item);
                heap_caps_free(item.data);
            }
            const int64_t now_us = esp_timer_get_time();
            if (now_us - last_diag_us >= kDiagPeriodUs) {
                last_diag_us = now_us;
                LogHeapDiag("period");
                ESP_LOGI(kDiagTag,
                         "renderer: nalu_in=%u nalu_ovf=%u dec_calls=%u dec_err=%u idr_drop=%u "
                         "frames=%u",
                         stat_nalu_in_.load(), stat_nalu_ovf_.load(), stat_dec_calls_.load(),
                         stat_dec_err_.load(), stat_idr_drop_.load(), stat_frames_.load());
            }
        }
    }

    /* ProcessNalu 处理单个 NALU：等待 IDR 期间只放行参数集与 IDR；
     * 送解成功且出帧时执行 YUV→RGB565 转换并触发回调。 */
    void ProcessNalu(const NaluItem& item) {
        const auto processing_started = esp_timer_get_time();  // 当前出帧NALU的解码与转换起点。
        const int type = NaluType(item.data, item.len);
        if (type < 0) {
            return;
        }
        if (wait_idr_.load()) {
            if (type != kNalSps && type != kNalPps && type != kNalIdr) {
                stat_idr_drop_.fetch_add(1);
                /* 等 IDR 期间持续（1s 节流）重请求关键帧：启动时的首个 PLI 可能早于
                 * 平台绑定远端视频 SSRC（首个上行 RTP 到达才 onTrack），会被静默丢弃；
                 * 只发一次会让设备永久停在等 IDR（真机 frames=0 根因）。 */
                RequestKeyframeNow();
                return;
            }
            if (type == kNalIdr) {
                wait_idr_.store(false);
            }
        }
        esp_h264_dec_in_frame_t in = {};
        in.raw_data.buffer = item.data;
        in.raw_data.len = item.len;
        esp_h264_dec_out_frame_t out = {};
        stat_dec_calls_.fetch_add(1);
        /* 输入恰为一个完整 NALU，tinyh264 单次调用即可全量消费。 */
        if (decoder_->process(decoder_, &in, &out) != ESP_H264_ERR_OK) {
            /* 解码失败视为参考链断裂：丢弃后续 P 帧直至下一个 IDR，并请求
             * 发送端立即重发关键帧（PLI 由 Poll 在连通后经 TakeKeyframeRequest 取走）。 */
            ESP_LOGW(kTag, "h264 decode failed, drop until next IDR");
            stat_dec_err_.fetch_add(1);
            wait_idr_.store(true);
            RequestKeyframeNow();
            return;
        }
        /* SPS/PPS 等非图像 NALU 不出帧，out_size 为 0。 */
        if (out.out_size == 0 || out.outbuf == nullptr) {
            return;
        }
        esp_h264_resolution_t res = {};
        esp_h264_dec_param_sw_handle_t param = nullptr;
        if (esp_h264_dec_sw_get_param_hd(decoder_, &param) != ESP_H264_ERR_OK || param == nullptr ||
            param->get_res(param, &res) != ESP_H264_ERR_OK || res.width == 0 || res.height == 0) {
            return;
        }
        if (res.width > kMaxWidth || res.height > kMaxHeight) {
            ESP_LOGE(kTag, "resolution %ux%u exceeds limit, frame dropped", res.width, res.height);
            return;
        }
        DeliverFrame(out.outbuf, res.width, res.height, processing_started);
    }

    /* DeliverFrame 转换 I420 到 RGB565 双缓冲并调用帧回调（解码任务上下文）。 */
    void DeliverFrame(const std::uint8_t* i420, int width, int height, int64_t started_us) {
        stat_frames_.fetch_add(1);
        std::uint8_t* rgb565 = TakeOutputBuffer(width, height);
        if (rgb565 == nullptr) {
            return;
        }
        ConvertI420ToRgb565(i420, rgb565, width, height);
        last_frame_processing_us_.store(static_cast<uint32_t>(esp_timer_get_time() - started_us));
        FrameCallback callback;
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            callback = on_frame_;
        }
        if (callback) {
            callback(rgb565, width, height);
        }
    }

    /* TakeOutputBuffer 按当前分辨率惰性分配 PSRAM 单缓冲；帧回调同步拷贝后即可
     * 复用，无需双缓冲。通话期相机/编码器占用后 PSRAM 碎片化，双缓冲会整组
     * 分配失败导致解码帧全部丢弃（真机 frames>0 但屏幕仍等待视频的根因）。 */
    std::uint8_t* TakeOutputBuffer(int width, int height) {
        const std::size_t bytes = static_cast<std::size_t>(width) * height * 2;
        if (buffers_[0] != nullptr && bytes <= buffer_capacity_) {
            out_width_ = width;
            out_height_ = height;
            return buffers_[0];
        }
        FreeOutputBuffers();
        buffers_[0] = static_cast<std::uint8_t*>(
            heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (buffers_[0] == nullptr) {
            ESP_LOGE(kTag, "rgb565 buffer %u bytes alloc failed", static_cast<unsigned>(bytes));
            FreeOutputBuffers();
            return nullptr;
        }
        buffer_capacity_ = bytes;
        out_width_ = width;
        out_height_ = height;
        ESP_LOGI(kTag, "rgb565 output %dx%d", width, height);
        return buffers_[0];
    }

    /* FreeOutputBuffers 释放 RGB565 输出缓冲并复位分辨率记录。 */
    void FreeOutputBuffers() {
        for (auto& buffer : buffers_) {
            if (buffer != nullptr) {
                heap_caps_free(buffer);
                buffer = nullptr;
            }
        }
        out_width_ = 0;
        out_height_ = 0;
        buffer_capacity_ = 0;
    }

    /* ConvertI420ToRgb565 整数 BT.601 转换：Y/U/V 平面 → 小端 RGB565。
     * 系数放大 256 后右移：R=Y+1.402V、G=Y-0.344U-0.714V、B=Y+1.772U，
     * 色度按 2x2 共享，全程无浮点无查表。 */
    static void ConvertI420ToRgb565(const std::uint8_t* i420, std::uint8_t* rgb565, int width,
                                    int height) {
        const std::uint8_t* y_plane = i420;
        const int c_stride = width / 2;
        const std::uint8_t* u_plane = i420 + static_cast<std::size_t>(width) * height;
        const std::uint8_t* v_plane = u_plane + static_cast<std::size_t>(c_stride) * (height / 2);
        auto* out = reinterpret_cast<std::uint16_t*>(rgb565);
        for (int row = 0; row < height; ++row) {
            const std::uint8_t* y_row = y_plane + static_cast<std::size_t>(row) * width;
            const std::uint8_t* u_row = u_plane + static_cast<std::size_t>(row >> 1) * c_stride;
            const std::uint8_t* v_row = v_plane + static_cast<std::size_t>(row >> 1) * c_stride;
            std::uint16_t* out_row = out + static_cast<std::size_t>(row) * width;
            for (int col = 0; col < width; ++col) {
                const int y = y_row[col];
                const int u = u_row[col >> 1] - 128;
                const int v = v_row[col >> 1] - 128;
                const int r = Clamp8(y + ((351 * v) >> 8));
                const int g = Clamp8(y - ((86 * u + 179 * v) >> 8));
                const int b = Clamp8(y + ((453 * u) >> 8));
                out_row[col] =
                    static_cast<std::uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
            }
        }
    }

    /* DrainQueue 释放队列中残留 NALU 拷贝并清空队列。 */
    void DrainQueue() {
        if (queue_ == nullptr) {
            return;
        }
        NaluItem item{};
        while (xQueueReceive(queue_, &item, 0) == pdTRUE) {
            if (item.data != nullptr) {
                heap_caps_free(item.data);
            }
        }
    }

    /* control_mutex_ 串行化 Start/Stop；callback_mutex_ 保护帧回调替换。 */
    std::mutex control_mutex_;
    std::mutex callback_mutex_;
    FrameCallback on_frame_;
    /* running_ 为解码循环与 OnNalu 入口开关；wait_idr_ 标记丢帧后等待 IDR 恢复。 */
    std::atomic<bool> running_{false};
    std::atomic<bool> wait_idr_{false};
    /* keyframe_pending_ 由 Poll 在 ICE 连通后取走并发 PLI（单线程模型内闭环）；
     * last_keyframe_req_ms_ 实现 ≥1s 节流，避免解码连续失败时刷爆 RTCP。 */
    std::atomic<bool> keyframe_pending_{false};
    std::atomic<std::int64_t> last_keyframe_req_ms_{0};
    /* 取证计数（ur_sig_call 前缀周期日志）：入队/溢出丢旧/送解/解码错误/等 IDR 丢弃/出帧。 */
    std::atomic<std::uint32_t> stat_nalu_in_{0};
    std::atomic<std::uint32_t> stat_nalu_ovf_{0};
    std::atomic<std::uint32_t> stat_dec_calls_{0};
    std::atomic<std::uint32_t> stat_dec_err_{0};
    std::atomic<std::uint32_t> stat_idr_drop_{0};
    std::atomic<std::uint32_t> stat_frames_{0};
    std::atomic<std::uint32_t> last_frame_processing_us_{0};  // 最近成功帧处理段的墙钟耗时。
    QueueHandle_t queue_ = nullptr;
    std::atomic<TaskHandle_t> task_{nullptr};
    esp_h264_dec_handle_t decoder_ = nullptr;
    /* RGB565 单缓冲（PSRAM）与当前分辨率，解码任务独占访问；回调同步拷贝后可复用。 */
    std::uint8_t* buffers_[1] = {nullptr};
    std::size_t buffer_capacity_ = 0;
    int out_width_ = 0;
    int out_height_ = 0;
};

}  // namespace

/* GetCallVideoRenderer 返回进程级单例：PSRAM 懒创建、双重检查，
 * 进程生命周期内不释放（含队列等常驻小资源）。内存不足时返回 nullptr。 */
CallVideoRenderer* GetCallVideoRenderer() {
    static std::atomic<CallVideoRendererImpl*> instance{nullptr};
    CallVideoRendererImpl* current = instance.load(std::memory_order_acquire);
    if (current != nullptr) {
        return current;
    }
    void* memory =
        heap_caps_malloc(sizeof(CallVideoRendererImpl), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (memory == nullptr) {
        ESP_LOGE(kTag, "call video renderer instance alloc failed");
        return nullptr;
    }
    auto* created = new (memory) CallVideoRendererImpl();
    CallVideoRendererImpl* expected = nullptr;
    if (!instance.compare_exchange_strong(expected, created)) {
        /* 并发创建只保留先到者，后到者构造即析构后释放。 */
        created->~CallVideoRendererImpl();
        heap_caps_free(memory);
        created = expected;
    }
    return created;
}

}  // namespace ur::device

#endif /* CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO */
