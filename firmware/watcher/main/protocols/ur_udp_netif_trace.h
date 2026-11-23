/* 默认关闭的网络栈到STA驱动交接取证；不改发送结果、不保存帧或认证信息。 */
#ifndef UR_UDP_NETIF_TRACE_H
#define UR_UDP_NETIF_TRACE_H

#include <cstddef>
#include <cstdint>

/* UrUdpNetifTraceStart 在普通任务绑定现有STA接口与当前内存路由；失败仅停用观察。 */
bool UrUdpNetifTraceStart(const uint8_t* nonce, size_t nonce_size, int port);
/* UrUdpNetifTraceStop 清除观察目标并排空数值队列；不修改或注销网络驱动。 */
void UrUdpNetifTraceStop();
/* UrUdpNetifTraceFlush 仅供普通worker有界输出，不在TCP/IP交接上下文调用日志。 */
void UrUdpNetifTraceFlush();

#endif  // UR_UDP_NETIF_TRACE_H
