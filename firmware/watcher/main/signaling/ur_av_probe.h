// 文件说明：显式串口启动的有时限音视频硬件验收，不改变正式设备身份或MQTT连接。
#pragma once
#include <cstdint>
#include "sdkconfig.h"

#if CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER && CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO

// UrStartAVProbe 连接用户指定的私网验收Broker，使用独立测试Topic协商一次性UDP媒体。
// host为私网IPv4，port为Broker端口，id为公开测试标识；最长90秒，到期释放全部媒体资源。
bool UrStartAVProbe(const char* host, std::uint16_t port, const char* id);
// UrAVProbeActive 表示验收正在独占媒体，正式AI与人际来电必须等待本次结束。
bool UrAVProbeActive();

#else
// 其他板型无Watcher硬件验收入口，正式媒体保持原互斥行为。
inline bool UrAVProbeActive() { return false; }
#endif
