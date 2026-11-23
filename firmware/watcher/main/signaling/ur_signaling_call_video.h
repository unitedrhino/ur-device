/* 通话下行视频解码渲染契约：libpeer 输出的 Annex-B H264 NALU 进，RGB565 帧出。
 * 解码在独立任务执行，不阻塞信令/音频路径。仅视频形态编译。 */
#ifndef UR_SIGNALING_CALL_VIDEO_H
#define UR_SIGNALING_CALL_VIDEO_H

#include <cstddef>
#include <cstdint>
#include <functional>

#include "sdkconfig.h"

#if CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO

namespace ur::device {

/* CallVideoRenderer 通话下行视频解码器接口；实现须保证：
 * - OnNalu 可在网络/信令任务上下文调用，内部只做入队（有界，满则丢旧）；
 * - 帧回调在独立解码任务上下文触发，rgb565 缓冲仅回调期间有效，接收方须拷贝；
 * - Start/Stop 幂等且可重复配对。 */
class CallVideoRenderer {
public:
    /* FrameCallback：rgb565 为帧首地址，width/height 为解码原生分辨率。 */
    using FrameCallback = std::function<void(const std::uint8_t* rgb565, int width, int height)>;

    virtual ~CallVideoRenderer() = default;

    /* Start 启动解码任务并注册帧回调；返回 false 表示资源不足不可用。 */
    virtual bool Start(FrameCallback on_frame) = 0;
    /* Stop 停止解码任务并清空队列；回调此后不再触发。
     * 禁止在帧回调上下文调用 Stop：等待任务退出会与回调互相死锁。 */
    virtual void Stop() = 0;
    /* OnNalu 喂入一个完整 Annex-B NALU（带 4 字节起始码）。 */
    virtual void OnNalu(const std::uint8_t* nalu, std::size_t len) = 0;
    /* TakeKeyframeRequest 取走待发的下行关键帧请求；由通话媒体桥在 ICE
     * 连通后轮询，取走 true 时应立即向下行发送方发 PLI。 */
    virtual bool TakeKeyframeRequest() = 0;
    // LastFrameProcessingUs 返回最近成功帧的解码与RGB转换耗时，供音频优先呈现策略判断。
    virtual uint32_t LastFrameProcessingUs() const { return 0; }
};

/* GetCallVideoRenderer 返回进程级单例（PSRAM 懒创建，进程生命周期内不释放）。 */
CallVideoRenderer* GetCallVideoRenderer();

}  // namespace ur::device

#endif /* CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO */

#endif /* UR_SIGNALING_CALL_VIDEO_H */
