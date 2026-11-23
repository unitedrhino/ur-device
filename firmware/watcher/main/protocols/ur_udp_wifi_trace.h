/* 固定SDK的测试专用Wi-Fi发送回调适配；正式固件不编译此接口及实现。 */
#ifndef UR_UDP_WIFI_TRACE_H
#define UR_UDP_WIFI_TRACE_H

#include <cstddef>
#include <cstdint>

/* UrUdpWifiTraceStart 注册唯一诊断回调并校验观察目标；失败仅停用观察，不改业务发送。 */
bool UrUdpWifiTraceStart(const uint8_t* nonce, size_t nonce_size, int port);
/* UrUdpWifiTraceStop 停用、清路由并仅注销本模块已注册的回调，输出剩余数值证据。 */
void UrUdpWifiTraceStop();
/* UrUdpWifiTraceFlush 在普通worker上下文最多读取八条，禁止在Wi-Fi回调中打印。 */
void UrUdpWifiTraceFlush();

#endif  // UR_UDP_WIFI_TRACE_H
