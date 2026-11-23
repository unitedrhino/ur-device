// 视频编码工作线程：AI保留原有规格，人与设备通话使用低负载压缩采集档。
#ifndef UR_SIGNALING_VIDEO_H
#define UR_SIGNALING_VIDEO_H

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <vector>

#include "esp_h264_enc_single_sw.h"
#include "esp_jpeg_dec.h"

#include "camera.h"

namespace ur::device {

/* 视频上行管线：AI默认416源转320x240；人际通话240JPEG在线程解码后转160x120@5fps，
 * RGB565 中心裁剪/下采样转 I420 后经 esp_h264 软编码出 Annex-B NALU，
 * 经有界队列交信令事件线程 peer_connection_send_video（libpeer 单线程模型，
 * 不允许本任务直接调用发送）。 */
class UrSignalingVideo {
public:
    // Profile 区分AI视觉原规格与音频优先的人际通话档，不隐式降低既有AI能力。
    enum class Profile { AiVision, HumanCall };
    /* frame_source 返回相机最新 RGB565 帧，尺寸由 camera->FrameWidth/Height 给出；
     * 无效返回 nullptr。 */
    using FrameSource = std::function<const std::uint8_t*()>;

    UrSignalingVideo() = default;
    ~UrSignalingVideo();

    UrSignalingVideo(const UrSignalingVideo&) = delete;
    UrSignalingVideo& operator=(const UrSignalingVideo&) = delete;

    /* Start 创建编码器并启动取帧任务；回调与相机生命周期由调用方保证。 */
    bool Start(Camera* camera, FrameSource frame_source, Profile profile = Profile::AiVision);
    // PrioritizeAudio 仅人际通话过载后把编码节拍降为1fps；AI视觉档不受影响。
    void PrioritizeAudio() {
        if (human_profile_ && running_.load())
            audio_priority_.store(true);
    }
    /* Stop 请求停止并等待取帧线程退出后释放资源；调用方串行调用，重复停止无副作用。 */
    void Stop();
    /* Running 表示取帧任务存活（发布中）。 */
    bool Running() const;

    /* TakeFrame 非阻塞取走一帧已编码 Annex-B 码流；无帧返回 false。 */
    bool TakeFrame(std::vector<std::uint8_t>& nalu);

    /* RequestIDR 请求下一帧强制输出 IDR（对端 PLI/FIR 触发）。esp_h264 公共
     * API 无 force-idr 入口，实现为按节流重建编码器（重建同时重发 SPS/PPS）。
     * 任意任务可调，仅置原子标志。 */
    void RequestIDR();

private:
    /* 取帧任务主体：最新帧 → 下采样转I420 → 编码 → 有界队列。 */
    static void TaskEntry(void* context);
    void Run();
    /* AcquireFrame 获取待编码RGB帧；压缩流只在编码线程内解码最新JPEG，无新帧返回false。 */
    bool AcquireFrame(const std::uint8_t*& rgb565, int& width, int& height);
    /* CreateEncoder 按当前通话/AI档创建编码器，成功返回true。 */
    bool CreateEncoder(esp_h264_enc_handle_t* out);
    /* RGB565 帧 → 当前档位I420：按4:3中心裁剪后最近邻缩放。 */
    void ConvertFrame(const std::uint8_t* rgb565, int src_width, int src_height);
    /* RebuildEncoder 销毁并重建编码器，强制下一帧为携带 SPS/PPS 的 IDR；
     * 带 ≥kIdrRebuildMinIntervalMs 节流，返回是否实际重建。 */
    bool RebuildEncoder();
    /* StashSpsPps 从一帧 Annex-B 码流截取 SPS/PPS NALU 缓存（含起始码）。 */
    void StashSpsPps(const std::uint8_t* data, std::size_t size);
    /* PrependSpsPps 若 IDR 帧未携带参数集，把缓存的 SPS/PPS 拼到帧首；
     * 返回拼接后的帧缓冲（无缓存或已含参数集时原样返回空）。 */
    bool PrependSpsPps(std::vector<std::uint8_t>& frame);
    /* LogNaluTypes 一次性诊断：按起始码切分打印帧内 NALU 类型/长度序列。 */
    void LogNaluTypes(const std::uint8_t* data, std::size_t size, bool key);

    Camera* camera_ = nullptr;
    FrameSource frame_source_;
    int output_width_ = 320;                   // 当前编码宽度，AI320，通话160。
    int output_height_ = 240;                  // 当前编码高度，AI240，通话120。
    bool human_profile_ = false;               // Start前冻结的用途，停止后无并发重建。
    std::atomic<bool> audio_priority_{false};  // 仅本次人际会话允许降帧，下一次Start恢复。
    int frame_interval_ms_ = 100;              // 实际取帧/解码节拍，AI10fps，通话5fps。
    bool compressed_stream_ = false;           // 相机是否只在回调发布压缩数据，耗时解码留给本线程。
    std::uint32_t jpeg_sequence_ = 0;          // 压缩帧消费水位，避免重复编码同一帧。
    std::vector<std::uint8_t> jpeg_;           // 只保留一个已取走的JPEG，最大48KiB。
    jpeg_dec_handle_t jpeg_decoder_ = nullptr;  // 编码线程专用解码器，与相机回调无锁竞争。
    std::uint8_t* jpeg_rgb_ = nullptr;          // 16字节对齐的240RGB565输出，仅压缩流分配。
    std::atomic<void*> task_{nullptr};  // 工作线程退出后发布空句柄，Stop以此确认资源已不再使用。
    std::atomic<bool> running_{false};  // 跨信令/编码线程的停止请求，不用volatile充当同步。
    esp_h264_enc_handle_t encoder_ = nullptr;
    std::vector<std::uint8_t> i420_;    /* 当前档位I420工作缓冲。 */
    std::vector<std::uint8_t> encoded_; /* 编码输出缓冲（与输入同量级预分配） */
    std::mutex frames_mutex_;
    std::deque<std::vector<std::uint8_t>> frames_; /* 已编码待发送，容量8丢旧。 */
    std::uint32_t pts_ = 0;
    /* request_idr_ 为跨任务强制 IDR 请求标志（PLI 回调在信令任务置位）。 */
    std::atomic<bool> request_idr_{false};
    /* sps_pps_ 缓存最近一次编码输出的参数集（Annex-B 含起始码），供每个
     * IDR 帧前重拼（openh264 CONSTANT_ID 只在首个 IDR 携带 SPS/PPS，真机
     * Chrome 首包丢失后永不解锁解码的根因）。 */
    std::vector<std::uint8_t> sps_pps_;
    /* last_rebuild_ms_ 为上次编码器重建时刻（单调毫秒），节流强制 IDR。 */
    int64_t last_rebuild_ms_ = 0;
    /* diag_frames_ 为已打印 NALU 诊断的帧数（前 10 帧逐帧打印）。 */
    int diag_frames_ = 0;
};

}  // namespace ur::device

#endif  // UR_SIGNALING_VIDEO_H
