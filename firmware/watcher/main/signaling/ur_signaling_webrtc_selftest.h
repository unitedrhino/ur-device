/* libpeer 本地自检：同进程建 offerer/answerer 双 PeerConnection 互连，
 * 验证源码化栈（agent/ICE/DTLS-SRTP/RTP）在当前构建环境的基本可用性；
 * 作为 webrtc-audio 形态的串口级组件自证命令。 */
#ifndef UR_SIGNALING_WEBRTC_SELFTEST_H
#define UR_SIGNALING_WEBRTC_SELFTEST_H

#include <esp_err.h>

/* ur_signaling_webrtc_selftest 在独立大栈任务中运行双 peer 互连自检
 * （约 20 秒），返回 0 表示两 peer 达到 CONNECTED 且完成一次音频帧往返。 */
esp_err_t ur_signaling_webrtc_selftest(void);

#endif /* UR_SIGNALING_WEBRTC_SELFTEST_H */
