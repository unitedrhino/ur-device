// 连接提速测量打点：仅在 ESP-IDF 目标输出 [connect-ms] 毫秒日志，桌面/主机测试为空操作。
// 该头只提供日志宏，不读写协议状态，不改变任何控制流或时序语义。
#pragma once

#if defined(ESP_PLATFORM)
// ESP-IDF 的系统头使用 GCC 的 include_next，局部屏蔽其扩展告警，核心代码仍执行严格检查。
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include <esp_log.h>
#include <esp_timer.h>
#pragma GCC diagnostic pop

// UR_CONNECT_TRACE 输出一行轻量结构化打点；t 必须由调用方显式传入。
#define UR_CONNECT_TRACE(tag, fmt, ...) ESP_LOGI(tag, "[connect-ms] " fmt, __VA_ARGS__)
// UR_CONNECT_MS 返回设备单调毫秒，仅用于没有宿主 now 参数的日志点。
#define UR_CONNECT_MS() (static_cast<unsigned long long>(esp_timer_get_time() / 1000))
#else
// 桌面核心与单元测试不链接 ESP-IDF，打点编译为空语句保持构建/行为不变。
#define UR_CONNECT_TRACE(tag, fmt, ...) do { (void)(tag); } while (0)
#define UR_CONNECT_MS() 0ULL
#endif
