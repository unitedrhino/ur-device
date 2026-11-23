/* esp_video相机接口：采集、图像解释及联犀模式的认证图片上传。 */
#pragma once
#include "sdkconfig.h"

#include <lvgl.h>
#include <memory>
#include <mutex>
#include <sys/time.h>
#include <thread>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "camera.h"
#include "esp_video_init.h"
#include "jpg/image_to_jpeg.h"

struct JpegChunk {
    uint8_t* data;
    size_t len;
};

class EspVideo : public Camera {
private:
    struct FrameBuffer {
        uint8_t* data = nullptr;
        size_t len = 0;
        uint16_t width = 0;
        uint16_t height = 0;
        v4l2_pix_fmt_t format = 0;
    } frame_;
    v4l2_pix_fmt_t sensor_format_ = 0;
#ifdef CONFIG_XIAOZHI_ENABLE_ROTATE_CAMERA_IMAGE
    uint16_t sensor_width_ = 0;
    uint16_t sensor_height_ = 0;
#endif  // CONFIG_XIAOZHI_ENABLE_ROTATE_CAMERA_IMAGE
    int video_fd_ = -1;
    bool streaming_on_ = false;
    bool stream_timeout_ready_ = false;  // 仅可靠设置并读回单拍超时后允许借用压缩流。
    struct MmapBuffer {
        void* start = nullptr;
        size_t length = 0;
    };
    std::vector<MmapBuffer> mmap_buffers_;
    std::string explain_url_;
    std::string explain_token_;
    std::thread encoder_thread_;
    std::mutex frame_mutex_;              // 串行化单拍、平台上传和通话流对帧缓冲的访问。
    bool compressed_stream_owned_ = false; // 通话独占相机期间拒绝单拍和图片上传。
    bool still_frame_valid_ = false;      // 流取帧前作废单拍，防止上传通话中的画面。
    timeval previous_dq_timeout_{};        // 流退出时恢复原先取帧超时。
    uint8_t* stream_rgb_ = nullptr;       // 240x240 RGB565 暂存，仅通话期间持有。
    uint32_t stream_sequence_ = 0;        // 已发布 JPEG 的本地序号。
    // CaptureFrameLocked 调用者须持帧锁；流采集不更新单拍预览。
    bool CaptureFrameLocked(bool show_preview);

public:
    EspVideo(const esp_video_init_config_t& config);
    ~EspVideo() override;

    virtual void SetExplainUrl(const std::string& url, const std::string& token);
    virtual bool Capture();
    // 连续压缩流按需领取图像，失败不接管相机；停止后恢复单拍。
    bool SupportsCompressedStream() const override;
    bool StartCompressedStream() override;
    void StopStream() override;
    bool TakeStreamJPEG(std::vector<uint8_t>& jpeg, uint32_t& sequence) override;
    // 翻转控制函数
    virtual bool SetHMirror(bool enabled) override;
    virtual bool SetVFlip(bool enabled) override;
    virtual std::expected<std::string, std::string> Explain(const std::string& question) override;
#if CONFIG_UR_AI
    // UploadToPlatform 将已捕获图像编码为JPEG并认证上传，成功返回fileUri，失败返回脱敏原因。
    std::expected<std::string, std::string> UploadToPlatform(
        const std::string& url, const char* username, const char* password) override;
#endif
};
