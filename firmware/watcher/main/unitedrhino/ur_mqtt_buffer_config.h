/* 联犀 AI 的 MQTT 接收容量适配：长字幕/工具结果不能沿用物模型的 2 KiB 默认值。
 * 由 CMake 对 SDK 全部翻译单元统一预包含，避免修改供应商目录及客户端结构 ABI 不一致。 */
#ifndef UR_MQTT_BUFFER_CONFIG_H
#define UR_MQTT_BUFFER_CONFIG_H

#include "unitedrhino_iot_export_variables.h"

/* SDK 允许的最大接收容量为 16 KiB，覆盖实测 2512 字节长回复及协议开销。 */
#undef UNITEDRHINO_IOT_MQTT_RX_BUF_LEN
#define UNITEDRHINO_IOT_MQTT_RX_BUF_LEN (16 * 1024)

#endif
