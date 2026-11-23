// 可选相机能力：单拍、RGB预览及供低负载媒体任务消费的压缩连续流。
#ifndef CAMERA_H
#define CAMERA_H

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

class Camera {
public:
    virtual ~Camera() = default;

    virtual void SetExplainUrl(const std::string& url, const std::string& token) = 0;
    virtual bool Capture() = 0;
    virtual bool SetHMirror(bool enabled) = 0;
    virtual bool SetVFlip(bool enabled) = 0;
    virtual bool SetSwapBytes(bool enabled) { return false; }  // Optional, default no-op
    /* 视频通话期间关闭本地预览刷新：每帧 614KB 拷贝和 LCD 刷新会挤占编码任务。 */
    virtual void SetPreviewEnabled(bool enabled) { (void)enabled; }
    /* 向摄像头协处理器发送原始 AT 探测命令；不支持的板保持空实现。 */
    virtual void SendRawCommand(const char* command) { (void)command; }
    /* 连续流模式（协处理器侧 AT+SAMPLE=-1）：
     * StartStream 后由协处理器持续输出 JPEG，Capture 只从队列取最新一帧，
     * 不再逐帧触发拍照，适合视频通话和镜子这类连续画面。
     * 不支持连续流的板子保持空实现，Capture 仍走原有单拍路径。 */
    virtual void StartStream() {}
    /* SupportsCompressedStream 表示相机具备 JPEG 连续流能力，不占用相机。 */
    virtual bool SupportsCompressedStream() const { return false; }
    /* StartCompressedStream 启动只发布JPEG的连续流；不支持的板返回false，由调用方回退。 */
    virtual bool StartCompressedStream() { return false; }
    /* SetStreamFrameInterval 设置压缩流进入解码前的最小间隔；0表示不限制。 */
    virtual void SetStreamFrameInterval(uint32_t interval_ms) { (void)interval_ms; }
    /* TakeStreamJPEG 复制sequence之后的最新完整JPEG并推进水位；无新帧返回false。 */
    virtual bool TakeStreamJPEG(std::vector<uint8_t>& jpeg, uint32_t& sequence) {
        (void)jpeg;
        (void)sequence;
        return false;
    }
    /* 停止连续流并恢复按需单拍；未启动流或重复调用时为空操作。 */
    virtual void StopStream() {}
    /* 当前预览帧尺寸；流未启动或尚未解码出首帧时返回 0。
     * 视频上行用它决定 RGB565 转 I420 的源尺寸。 */
    virtual int FrameWidth() const { return 0; }
    virtual int FrameHeight() const { return 0; }
    virtual std::expected<std::string, std::string> Explain(const std::string& question) = 0;
    /* UploadToPlatform 上传当前 JPEG 并返回平台 fileUri；不支持的相机保持显式失败。 */
    virtual std::expected<std::string, std::string> UploadToPlatform(
        const std::string& url, const char* username, const char* password) {
        (void)url;
        (void)username;
        (void)password;
        return std::unexpected("Camera platform upload is not supported");
    }
};

#endif  // CAMERA_H
