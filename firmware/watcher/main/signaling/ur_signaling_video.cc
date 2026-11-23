/* 视频上行：AI保留416推理流；通话优先240原生JPEG，按5fps在线程内解码并编码160x120。
 * 网络线程只取有界NALU队列，低优先级编码线程退出后才释放相机和编解码资源。 */
#include "ur_signaling_video.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cstdio>

namespace ur::device {
namespace {
constexpr const char* kTag = "ur_sig_video";
constexpr size_t kQueueLimit = 8;                /* 待发送 NALU 上限，满丢旧保低延迟 */
constexpr size_t kJpegRgbBytes = 240 * 240 * 2;  // 压缩相机输出严格限定240方图。
/* 强制 IDR（编码器重建）最小间隔：PLI 风暴下不能逐条重建；GOP=10@10fps
 * 自然 IDR 约 1s 一个，重建仅用于"立刻"响应关键帧请求。 */
constexpr int64_t kIdrRebuildMinIntervalMs = 3000;
/* NALU 诊断逐帧打印的帧数上限（仅前 N 帧刷日志，之后静默）。 */
constexpr int kDiagFrameLimit = 10;
}  // namespace

UrSignalingVideo::~UrSignalingVideo() { Stop(); }

/* CreateEncoder 按低码率低帧率配置创建软编码器；失败返回 false。 */
bool UrSignalingVideo::CreateEncoder(esp_h264_enc_handle_t* out) {
    esp_h264_enc_cfg_t cfg = {};
    cfg.pic_type = ESP_H264_RAW_FMT_I420;
    cfg.gop = 1000 / frame_interval_ms_;
    cfg.fps = cfg.gop;
    cfg.res.width = output_width_;
    cfg.res.height = output_height_;
    cfg.rc.bitrate = output_width_ == 160 ? 120000 : 500000;
    cfg.rc.qp_min = 10;
    cfg.rc.qp_max = 51;
    if (esp_h264_enc_sw_new(&cfg, out) != ESP_H264_ERR_OK || *out == nullptr) {
        ESP_LOGE(kTag, "h264 sw encoder create failed");
        *out = nullptr;
        return false;
    }
    if ((*out)->open(*out) != ESP_H264_ERR_OK) {
        ESP_LOGE(kTag, "h264 sw encoder open failed");
        (*out)->del(*out);
        *out = nullptr;
        return false;
    }
    return true;
}

bool UrSignalingVideo::Start(Camera* camera, FrameSource frame_source, Profile profile) {
    if (running_) {
        return true;
    }
    if (camera == nullptr || !frame_source) {
        ESP_LOGE(kTag, "camera or frame source unavailable");
        return false;
    }
    output_width_ = profile == Profile::HumanCall ? 160 : 320;
    output_height_ = profile == Profile::HumanCall ? 120 : 240;
    frame_interval_ms_ = profile == Profile::HumanCall ? 200 : 100;
    human_profile_ = profile == Profile::HumanCall;
    audio_priority_.store(false);
    if (encoder_ == nullptr && !CreateEncoder(&encoder_)) {
        return false;
    }
    i420_.assign(static_cast<size_t>(output_width_) * output_height_ * 3 / 2, 0);
    /* 输出按输入同量级预分配，覆盖 IDR 帧峰值。 */
    encoded_.assign(i420_.size(), 0);
    camera_ = camera;
    frame_source_ = std::move(frame_source);
    pts_ = 0;
    {
        std::lock_guard<std::mutex> lock(frames_mutex_);
        frames_.clear();
    }
    camera_->SetPreviewEnabled(false); /* 编码期间停本地预览，释放 PSRAM 拷贝与 LCD 刷新。 */
    compressed_stream_ = profile == Profile::HumanCall && camera_->StartCompressedStream();
    jpeg_sequence_ = 0;
    if (compressed_stream_) {
        jpeg_dec_config_t config = {};
        config.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
        config.rotate = JPEG_ROTATE_0D;
        jpeg_rgb_ = static_cast<uint8_t*>(
            heap_caps_aligned_alloc(16, kJpegRgbBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (jpeg_rgb_ == nullptr || jpeg_dec_open(&config, &jpeg_decoder_) != JPEG_ERR_OK) {
            Stop();
            return false;
        }
    } else {
        camera_->StartStream();  // 不支持压缩流的可选相机保留RGB取帧路径。
    }
    running_ = true;
    /* 编码优先级2低于AFE的3，过载时先保证麦克风取帧；栈仍放PSRAM。 */
    /* 栈 24KB(PSRAM)：openh264 WelsCreateSVCEncoder/编码 process 与 JPEG 转换的
     * 调用栈深（真机曾 4KB 栈溢出 abort：vApplicationStackOverflowHook 实证）；
     * 帧转换局部缓冲亦在该栈上，用 WithCaps 放 PSRAM 不占内部堆。 */
    TaskHandle_t created_task = nullptr;  // 创建接口只写原生句柄，成功后再发布给停止方。
    if (xTaskCreateWithCaps(TaskEntry, "ur_sig_video", 24 * 1024, this, 2, &created_task,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(kTag, "video task create failed");
        running_ = false;
        task_.store(nullptr);
        Stop();
        return false;
    }
    task_.store(created_task, std::memory_order_release);
    ESP_LOGI(kTag, "video pipeline started %dx%d@%dfps compressed=%d", output_width_,
             output_height_, 1000 / frame_interval_ms_, compressed_stream_);
    return true;
}

void UrSignalingVideo::Stop() {
    if (!running_ && task_ == nullptr && encoder_ == nullptr) {
        return;
    }
    running_.store(false);
    if (camera_ != nullptr) {
        camera_->StopStream();  // 先中断下一次采集，防止停止期间继续累积新相机帧。
    }
    // 软编码在低优先级下可能超过1.5秒；必须等真实线程退出，不以超时充当释放许可。
    while (task_.load(std::memory_order_acquire) != nullptr) {
        vTaskDelay(std::max<TickType_t>(1, pdMS_TO_TICKS(10)));
    }
    if (camera_ != nullptr) {
        camera_->SetPreviewEnabled(true);
    }
    if (encoder_ != nullptr) {
        encoder_->close(encoder_);
        encoder_->del(encoder_);
        encoder_ = nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(frames_mutex_);
        frames_.clear();
    }
    i420_.clear();
    i420_.shrink_to_fit();
    encoded_.clear();
    encoded_.shrink_to_fit();
    sps_pps_.clear();
    sps_pps_.shrink_to_fit();
    jpeg_.clear();
    jpeg_.shrink_to_fit();
    if (jpeg_decoder_ != nullptr) {
        jpeg_dec_close(jpeg_decoder_);
        jpeg_decoder_ = nullptr;
    }
    heap_caps_free(jpeg_rgb_);
    jpeg_rgb_ = nullptr;
    compressed_stream_ = false;
    camera_ = nullptr;
    frame_source_ = nullptr;
    ESP_LOGI(kTag, "video pipeline stopped");
}

bool UrSignalingVideo::Running() const { return running_; }

bool UrSignalingVideo::TakeFrame(std::vector<std::uint8_t>& nalu) {
    std::lock_guard<std::mutex> lock(frames_mutex_);
    if (frames_.empty()) {
        return false;
    }
    nalu = std::move(frames_.front());
    frames_.pop_front();
    return true;
}

void UrSignalingVideo::TaskEntry(void* context) {
    auto* self = static_cast<UrSignalingVideo*>(context);
    self->Run();
    self->task_.store(nullptr, std::memory_order_release);
    vTaskDeleteWithCaps(nullptr); /* 栈经 WithCaps 分配，须配套删除释放 */
}

void UrSignalingVideo::Run() {
    int timing_frames = 0; /* 仅前若干帧输出分段耗时，避免长期刷日志。 */
    while (running_) {
        const int64_t started = esp_timer_get_time();
        /* PLI/FIR 置位的强制 IDR：按节流重建编码器（顺带重发 SPS/PPS）。 */
        if (request_idr_.exchange(false) && RebuildEncoder()) {
            ESP_LOGI(kTag, "forced IDR via encoder rebuild (PLI/FIR)");
        }
        if (encoder_ == nullptr) {
            running_ = false;  // 重建失败不能继续解引用已释放编码器，停止后由宿主结束媒体。
            break;
        }
        const uint8_t* rgb565 = nullptr;  // 当前编码拥有的稳定帧；压缩路径不借用相机双缓冲。
        int src_w = 0, src_h = 0;         // 成功取帧后由相机或JPEG头给出实际尺寸。
        if (!AcquireFrame(rgb565, src_w, src_h)) {
            vTaskDelay(pdMS_TO_TICKS(compressed_stream_ ? 10 : 1000));
            continue;
        }
        const int64_t captured = esp_timer_get_time();
        ConvertFrame(rgb565, src_w, src_h);
        const int64_t converted = esp_timer_get_time();
        esp_h264_enc_in_frame_t in = {};
        in.raw_data.buffer = i420_.data();
        in.raw_data.len = i420_.size();
        in.pts = pts_++;
        esp_h264_enc_out_frame_t out = {};
        out.raw_data.buffer = encoded_.data();
        out.raw_data.len = encoded_.size();
        if (encoder_->process(encoder_, &in, &out) != ESP_H264_ERR_OK || out.length == 0) {
            ESP_LOGW(kTag, "h264 encode failed");
        } else {
            /* IDR 与非刷新 I 帧都需自带参数集（Chrome 可从任意 I 帧起解码）。 */
            const bool key = out.frame_type == ESP_H264_FRAME_TYPE_IDR ||
                             out.frame_type == ESP_H264_FRAME_TYPE_I;
            LogNaluTypes(out.raw_data.buffer, out.length, key);
            StashSpsPps(out.raw_data.buffer, out.length);
            std::vector<std::uint8_t> frame(out.raw_data.buffer, out.raw_data.buffer + out.length);
            if (key) {
                PrependSpsPps(frame);
            }
            std::lock_guard<std::mutex> lock(frames_mutex_);
            if (frames_.size() >= kQueueLimit) {
                frames_.pop_front();
            }
            frames_.push_back(std::move(frame));
        }
        const int64_t encoded = esp_timer_get_time();
        if (timing_frames++ < 20) {
            ESP_LOGI(kTag, "frame timing capture=%lldms convert=%lldms encode=%lldms",
                     (captured - started) / 1000, (converted - captured) / 1000,
                     (encoded - converted) / 1000);
        }
        /* 即使编码过载也让出至少一个调度tick；否则连续流始终有帧时会饿死Idle。
         * 音频AFE具有更高优先级，低于预算时仍维持原有采集/编码总节拍。 */
        const int64_t elapsed_ms = (esp_timer_get_time() - started) / 1000;
        const int interval_ms =
            audio_priority_.load() ? std::max(1000, frame_interval_ms_) : frame_interval_ms_;
        const auto remaining_ms = std::max<int64_t>(1, interval_ms - elapsed_ms);
        vTaskDelay(std::max<TickType_t>(1, pdMS_TO_TICKS(remaining_ms)));
    }
}

// AcquireFrame 只消费最新压缩帧；JPEG解码按编码节拍执行，避免高优先级相机回调逐帧抢占音频。
bool UrSignalingVideo::AcquireFrame(const std::uint8_t*& rgb565, int& width, int& height) {
    if (!compressed_stream_) {
        if (!camera_->Capture())
            return false;
        rgb565 = frame_source_();
        width = camera_->FrameWidth();
        height = camera_->FrameHeight();
        return rgb565 != nullptr && width > 0 && height > 0;
    }
    if (!camera_->TakeStreamJPEG(jpeg_, jpeg_sequence_) || jpeg_.empty() ||
        jpeg_.size() > 48 * 1024)
        return false;
    jpeg_dec_io_t io = {};
    io.inbuf = jpeg_.data();
    io.inbuf_len = static_cast<int>(jpeg_.size());
    jpeg_dec_header_info_t header = {};
    int output_bytes = 0;  // 解码器要求的实际容量，异常尺寸不能写入固定大小缓冲。
    if (jpeg_dec_parse_header(jpeg_decoder_, &io, &header) != JPEG_ERR_OK || header.width != 240 ||
        header.height != 240 ||
        jpeg_dec_get_outbuf_len(jpeg_decoder_, &output_bytes) != JPEG_ERR_OK || output_bytes <= 0 ||
        static_cast<size_t>(output_bytes) > kJpegRgbBytes)
        return false;
    io.outbuf = jpeg_rgb_;
    io.inbuf += io.inbuf_len - io.inbuf_remain;
    io.inbuf_len = io.inbuf_remain;
    if (jpeg_dec_process(jpeg_decoder_, &io) != JPEG_ERR_OK)
        return false;
    rgb565 = jpeg_rgb_;
    width = header.width;
    height = header.height;
    return true;
}

void UrSignalingVideo::RequestIDR() { request_idr_.store(true); }

bool UrSignalingVideo::RebuildEncoder() {
    const int64_t now_ms = esp_timer_get_time() / 1000;
    if (last_rebuild_ms_ != 0 && now_ms - last_rebuild_ms_ < kIdrRebuildMinIntervalMs) {
        return false; /* 节流窗口内等待每秒一次的自然IDR。 */
    }
    last_rebuild_ms_ = now_ms;
    if (encoder_ != nullptr) {
        encoder_->close(encoder_);
        encoder_->del(encoder_);
        encoder_ = nullptr;
    }
    return CreateEncoder(&encoder_);
}

/* FindNalus 按 3/4 字节起始码切分 Annex-B 码流，回调每个 NALU 的
 * （起始码长度、NALU 头指针、NALU 长度）。 */
static void FindNalus(
    const std::uint8_t* data, std::size_t size,
    const std::function<void(std::size_t, const std::uint8_t*, std::size_t)>& fn) {
    std::size_t i = 0;
    while (i + 4 <= size) {
        /* 定位下一个起始码（00 00 01，兼容前置 00）。 */
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            const std::size_t code = (i > 0 && data[i - 1] == 0) ? 4 : 3;
            const std::uint8_t* head = data + i + 3;
            /* 找下一个起始码或缓冲尾作为本 NALU 结束。 */
            std::size_t j = i + 3;
            std::size_t end = size;
            while (j + 3 <= size) {
                if (data[j] == 0 && data[j + 1] == 0 && data[j + 2] == 1) {
                    end = j;
                    if (j > 0 && data[j - 1] == 0) {
                        end--; /* 起始码前导 00 归下一 NALU。 */
                    }
                    break;
                }
                ++j;
            }
            std::size_t len = end - (i + 3);
            while (len > 0 && head[len - 1] == 0) {
                --len; /* 去尾随零。 */
            }
            fn(code, head, len);
            i = end;
        } else {
            ++i;
        }
    }
}

void UrSignalingVideo::LogNaluTypes(const std::uint8_t* data, std::size_t size, bool key) {
    if (diag_frames_ >= kDiagFrameLimit) {
        return;
    }
    ++diag_frames_;
    char text[192] = {0};
    int used = snprintf(text, sizeof(text), "nalus[%s %zub]:", key ? "KEY" : "P", size);
    FindNalus(data, size, [&](std::size_t, const std::uint8_t* head, std::size_t len) {
        if (used > 0 && static_cast<std::size_t>(used) < sizeof(text) - 16) {
            used += snprintf(text + used, sizeof(text) - used, " %u(%zub)",
                             static_cast<unsigned>(head[0] & 0x1F), len);
        }
    });
    ESP_LOGI(kTag, "%s", text);
}

void UrSignalingVideo::StashSpsPps(const std::uint8_t* data, std::size_t size) {
    const std::uint8_t* sps = nullptr;
    std::size_t sps_len = 0;
    const std::uint8_t* pps = nullptr;
    std::size_t pps_len = 0;
    /* 只记录 NALU 净荷（不含起始码），缓存时统一以 4 字节起始码拼装，
     * 保证下游 libpeer h264_find_nalu（仅识别 00 00 00 01）能正确切分。 */
    FindNalus(data, size, [&](std::size_t, const std::uint8_t* head, std::size_t len) {
        const std::uint8_t type = head[0] & 0x1F;
        if (type == 7 && sps == nullptr) {
            sps = head;
            sps_len = len;
        } else if (type == 8 && pps == nullptr) {
            pps = head;
            pps_len = len;
        }
    });
    if (sps == nullptr || pps == nullptr) {
        return;
    }
    sps_pps_.clear();
    const std::uint8_t start[4] = {0, 0, 0, 1};
    sps_pps_.insert(sps_pps_.end(), start, start + 4);
    sps_pps_.insert(sps_pps_.end(), sps, sps + sps_len);
    sps_pps_.insert(sps_pps_.end(), start, start + 4);
    sps_pps_.insert(sps_pps_.end(), pps, pps + pps_len);
}

bool UrSignalingVideo::PrependSpsPps(std::vector<std::uint8_t>& frame) {
    if (sps_pps_.empty()) {
        return false;
    }
    /* 帧内已携带参数集（首个 IDR）则不重复。 */
    bool has_params = false;
    FindNalus(frame.data(), frame.size(), [&](std::size_t, const std::uint8_t* head, std::size_t) {
        const std::uint8_t type = head[0] & 0x1F;
        if (type == 7 || type == 8) {
            has_params = true;
        }
    });
    if (has_params) {
        return false;
    }
    frame.insert(frame.begin(), sps_pps_.begin(), sps_pps_.end());
    return true;
}

void UrSignalingVideo::ConvertFrame(const std::uint8_t* rgb565, int src_w, int src_h) {
    /* RGB565 小端：低字节 GGGBBBBB，高字节 RRRRRGGG。整数系数近似 BT.601：
     * Y=(77R+150G+29B)>>8，U=(-43R-85G+128B)>>8+128，V=(128R-107G-21B)>>8+128。
     * 流模式源为 416x416 方形：按 4:3 中心裁剪后再缩放，避免画面拉伸变形。 */
    if (rgb565 == nullptr || src_w <= 0 || src_h <= 0 || i420_.empty()) {
        return;
    }
    int crop_w = src_w;
    int crop_h = src_w * 3 / 4;
    if (crop_h > src_h) {
        crop_h = src_h;
        crop_w = src_h * 4 / 3;
    }
    const int crop_x = (src_w - crop_w) / 2;
    const int crop_y = (src_h - crop_h) / 2;

    const int y_plane = output_width_ * output_height_;
    uint8_t* y_plane_ptr = i420_.data();
    uint8_t* u_plane_ptr = i420_.data() + y_plane;
    uint8_t* v_plane_ptr = u_plane_ptr + y_plane / 4;
    for (int dy = 0; dy < output_height_; ++dy) {
        const int src_y = crop_y + dy * crop_h / output_height_;
        for (int dx = 0; dx < output_width_; ++dx) {
            const int src_x = crop_x + dx * crop_w / output_width_;
            const size_t src_index = (static_cast<size_t>(src_y) * src_w + src_x) * 2;
            const uint16_t pixel = static_cast<uint16_t>(rgb565[src_index]) |
                                   (static_cast<uint16_t>(rgb565[src_index + 1]) << 8);
            const int r = (pixel >> 11) & 0x1F;
            const int g = (pixel >> 5) & 0x3F;
            const int b = pixel & 0x1F;
            y_plane_ptr[dy * output_width_ + dx] =
                static_cast<uint8_t>((r * 8 * 77 + g * 4 * 150 + b * 8 * 29) >> 8);

            /* 色度：取 2x2 块的右下角像素，坐标夹在裁剪区内。 */
            if ((dx & 1) == 0 && (dy & 1) == 0) {
                const int cs_x = std::min(src_x + 1, crop_x + crop_w - 1);
                const int cs_y = std::min(src_y + 1, crop_y + crop_h - 1);
                const size_t c_index = (static_cast<size_t>(cs_y) * src_w + cs_x) * 2;
                const uint16_t cpixel = static_cast<uint16_t>(rgb565[c_index]) |
                                        (static_cast<uint16_t>(rgb565[c_index + 1]) << 8);
                const int cr = ((cpixel >> 11) & 0x1F) * 8;
                const int cg = ((cpixel >> 5) & 0x3F) * 4;
                const int cb = (cpixel & 0x1F) * 8;
                const int cu_index = (dy / 2) * (output_width_ / 2) + dx / 2;
                u_plane_ptr[cu_index] =
                    static_cast<uint8_t>(((-43 * cr - 85 * cg + 128 * cb) >> 8) + 128);
                v_plane_ptr[cu_index] =
                    static_cast<uint8_t>(((128 * cr - 107 * cg - 21 * cb) >> 8) + 128);
            }
        }
    }
}

}  // namespace ur::device
