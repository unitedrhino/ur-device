// Watcher 相机接口：单拍与流预览共享解码器，预览双缓冲最多容纳 416 方图。
#ifndef SSCMA_CAMERA_H
#define SSCMA_CAMERA_H

#include <lvgl.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <esp_io_expander_tca95xx_16bit.h>
#include <esp_jpeg_dec.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <mbedtls/base64.h>

#include "camera.h"
#include "sscma_client.h"

class CameraTransportTrace;  // IO 公开接口的可选传输计时包装。
class CameraDmaIo;           // 连续读流的常驻内部DMA缓冲，与诊断独立。

struct SscmaData {
    uint8_t* img;
    size_t len;
};
struct JpegData {
    uint8_t* buf;
    size_t len;
};

class SscmaCamera : public Camera {
private:
    lv_img_dsc_t preview_image_;
    std::string explain_url_;
    std::string explain_token_;
    sscma_client_io_handle_t sscma_client_io_handle_;
    sscma_client_handle_t sscma_client_handle_;
    QueueHandle_t sscma_data_queue_;
    JpegData jpeg_data_;
    jpeg_dec_handle_t jpeg_dec_ = nullptr;  // RGB解码器在JPEG直送时释放，预览或单拍再建立。
    jpeg_dec_io_t* jpeg_io_;
    jpeg_dec_header_info_t* jpeg_out_;
    // 检测状态机
    enum DetectionState {
        IDLE,        // 空闲状态
        VALIDATING,  // 验证中（连续检测3秒）
        COOLDOWN     // 冷却期（等待重新检测）
    };

    DetectionState detection_state = IDLE;
    int64_t state_start_time = 0;
    bool need_start_cooldown = false;  // 是否需要开始冷却期
    int64_t last_detected_time = 0;    // 验证期间最后一次检测到物体的时间

    int detect_target = 0;
    int detect_threshold = 75;
    int detect_duration_sec = 2;         // 检测持续时间2秒，确认人员持续存在
    int detect_invoke_interval_sec = 8;  // 默认15秒冷却期，避免频繁开始会话
    int detect_debounce_sec = 1;         // 验证期间人员离开的去抖动时间1秒
    int inference_en = 0;                // 推理使能开关（0: 关闭, 1: 开启）
    bool sscma_restarted_ = false;
    bool preview_valid_ = false;   // 最近一次单拍是否已解码出有效预览帧
    bool preview_enabled_ = true;  // 通话编码期间关闭预览显示，避免每帧拷贝与刷屏抢占编码任务
    std::atomic<bool> streaming_{false};  // 已启动 416 持续推理流，Capture 只取最新帧
    /* 预览流双缓冲：回调内直接解码到非当前缓冲，原子切换后 display 立即取用；
     * 视频上行通过 preview_frame() 读取同一份 RGB565，不再经队列中转。 */
    std::atomic<uint32_t> stream_seq_{0};
    std::atomic<int> stream_write_{0};
    uint8_t* stream_frame_[2] = {nullptr, nullptr};
    uint16_t stream_w_ = 416;
    uint16_t stream_h_ = 416;
    std::mutex stream_mutex_;  // 串行化单拍与流共用的 JPEG 解码器
    std::atomic<bool> jpeg_only_{false};  // UDP视频直送相机JPEG，不承担RGB解码和本地预览。
    std::atomic<uint32_t> stream_frame_interval_ms_{0};  // 通话期间采样最小间隔，0表示不限速。
    std::atomic<int64_t> stream_last_published_us_{0};  // 最近一次完成JPEG发布的单调时钟。
    size_t stream_jpeg_size_ = 0;  // 最近完整JPEG长度，由stream_mutex_保护。
    std::unique_ptr<CameraDmaIo> dma_io_;  // 先于客户端任务创建，停止客户端后才销毁。
    std::unique_ptr<CameraTransportTrace> transport_trace_;  // 原始 IO 的非拥有计时包装。
    // StreamFrameTiming 区分图像回调内的各段耗时，均为微秒；仅成功帧进入诊断样本。
    struct StreamFrameTiming {
        uint32_t sequence;           // 已发布的相机帧序号。
        uint32_t extract_us;         // 提取并复制 Base64 图像字符串。
        uint32_t mutex_us;           // 等待共用 JPEG 解码器互斥锁。
        uint32_t prepare_us;         // 缓冲与解码器状态检查。
        uint32_t base64_us;          // Base64 解码及释放提取字符串。
        uint32_t header_us;          // JPEG 头解析、容量检查及输出参数准备。
        uint32_t jpeg_us;            // 仅 jpeg_dec_process 的解码耗时。
        uint32_t publish_us;         // 发布尺寸、双缓冲索引和帧序号。
        uint32_t display_submit_us;  // 调用显示入口，不包含其后异步绘制和 DMA。
        uint32_t callback_us;        // 图像回调进入至显示入口返回。
        uint32_t interval_us;        // 与上一成功帧回调进入时刻的间隔。
        uint32_t base64_bytes;       // 提取的 Base64 字符串长度。
        uint32_t jpeg_bytes;         // Base64 解码得到的 JPEG 字节数。
    };
    static constexpr size_t kStreamTimingCapacity = 128;  // 固定上限，避免诊断数据增长。
    StreamFrameTiming* stream_timings_ = nullptr;  // 首次诊断 reset 时在 PSRAM 分配，正常流不分配。
    size_t stream_timing_write_ = 0;               // 下一条环形样本写入位置。
    size_t stream_timing_count_ = 0;               // 已保留的成功帧样本数。
    int64_t stream_previous_received_us_ = 0;      // 最近成功帧回调入口，用于计算间隔。
    // RecordStreamTiming 在已持解码锁时记录 sample；received_us 为本帧回调入口，诊断关闭时跳过。
    void RecordStreamTiming(const StreamFrameTiming& sample, int64_t received_us);
    void HandleStreamImage(const sscma_client_reply_t* reply);
    bool EnsureStreamBuffers();
    // EnsureJpegDecoder 在持有stream_mutex_时恢复RGB解码器，返回创建是否成功。
    bool EnsureJpegDecoder();

    /* UploadJpegMultipart 复用 Explain 与联犀上传的 multipart 写入流程。 */
    std::expected<std::string, std::string> UploadJpegMultipart(
        const std::string& url, const std::string& authorization,
        const std::string& question = "");

    sscma_client_model_t* model;
    int model_class_cnt = 0;

public:
    SscmaCamera(esp_io_expander_handle_t io_exp_handle);
    ~SscmaCamera();
    void InitializeMcpTools();

    virtual void SetExplainUrl(const std::string& url, const std::string& token);
    virtual bool Capture();
    virtual void SetPreviewEnabled(bool enabled) override { preview_enabled_ = enabled; }
    virtual void SendRawCommand(const char* command) override;
    /* 启动连续流：本地镜子为 240 采样，独立视频为 416 推理，单拍仍为 640x480。 */
    virtual void StartStream() override;
    // StartJpegStream 启动240原生连续JPEG采样；与普通视频和镜子互斥使用。
    void StartJpegStream();
    // StartCompressedStream 让通用媒体管线申请240JPEG流，返回是否真正启动。
    bool SupportsCompressedStream() const override { return true; }
    bool StartCompressedStream() override;
    // SetStreamFrameInterval 限制JPEG进入解码前的最小间隔，降低通话期间相机处理负载。
    void SetStreamFrameInterval(uint32_t interval_ms) override;
    // StreamActive 供显式媒体预约核对，相机正在取流时拒绝第二生产者。
    bool StreamActive() const { return streaming_.load(); }
    // TakeStreamJPEG 复制sequence之后的最新完整JPEG；成功推进调用者水位，不返回旧帧。
    bool TakeStreamJPEG(std::vector<uint8_t>& jpeg, uint32_t& sequence) override;
    /* 发送 AT+BREAK 中止持续推理，恢复 Capture 的按需单拍路径。 */
    virtual void StopStream() override;
    // PrintStreamPerformance 在锁外导出成功帧分段耗时；reset 启用 PSRAM 环形缓冲并清空旧样本。
    void PrintStreamPerformance(bool reset);
    /* 视频上行按当前帧实际解码尺寸转换，连续流可能为 240 或 416。 */
    int FrameWidth() const override {
        if (streaming_.load())
            return stream_w_;
        return preview_valid_ ? preview_image_.header.w : 0;
    }
    int FrameHeight() const override {
        if (streaming_.load()) return stream_h_;
        return preview_valid_ ? preview_image_.header.h : 0;
    }
    /* 视频通话管线读取最近一次解码出的 RGB565 预览帧；流模式返回回调双缓冲中的最新帧。 */
    const uint8_t* preview_frame() const {
        if (streaming_.load()) {
            return stream_frame_[stream_write_.load(std::memory_order_acquire)];
        }
        return preview_valid_ ? preview_image_.data : nullptr;
    }
    // 翻转控制函数
    virtual bool SetHMirror(bool enabled) override;
    virtual bool SetVFlip(bool enabled) override;
    virtual std::expected<std::string, std::string> Explain(const std::string& question) override;
    virtual std::expected<std::string, std::string> UploadToPlatform(
        const std::string& url, const char* username, const char* password) override;
};

#endif  // ESP32_CAMERA_H
