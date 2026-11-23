/* 联犀 MQTT 设备任务对外接口
 *
 * 设备联网后由板级启动该任务：动态注册（首次）→ MQTT 连接 → 物模型上报/控制。
 * 三元组来源：ProductID/ProductSecret 为编译配置（Kconfig），DeviceName 自动取蓝牙 MAC，
 * DeviceSecret 首次动态注册后持久化 NVS（见 HAL_Device_freertos.c）。
 */
#ifndef UR_DEVICE_H
#define UR_DEVICE_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 查询联犀 MQTT 是否已连接（供板级/UI 展示） */
bool ur_device_is_connected(void);
/* 请求 MQTT 任务尽快发布属性；不在调用线程访问 MQTT 客户端。 */
void ur_device_request_report(void);
/* ur_device_force_mqtt_reconnect 请求 MQTT 任务强制断开重连并重订阅
 * （平台重启后连接绑定失效、上行信令静默丢弃时的自愈入口）。 */
void ur_device_force_mqtt_reconnect(void);

#if CONFIG_UR_AI
/* ur_ai_message_callback_t 在联犀 MQTT 回调中转交一份只读 AI 下行报文。 */
typedef void (*ur_ai_message_callback_t)(const char *payload, size_t length, void *context);
/* ur_ai_connection_callback_t 通知 AI 协议联犀 MQTT 的连接状态变化。 */
typedef void (*ur_ai_connection_callback_t)(bool connected, void *context);

/* ur_device_ai_set_callbacks 注册 AI 下行和连接状态回调；传 NULL 可注销。 */
void ur_device_ai_set_callbacks(ur_ai_message_callback_t message_callback,
                                ur_ai_connection_callback_t connection_callback,
                                void *context);
/* ur_device_ai_publish 把 AI 上行报文放入 MQTT 任务队列，调用方不会直接操作客户端。 */
bool ur_device_ai_publish(const char *payload, size_t length);
/* ur_device_copy_http_auth 复制一次短期 HTTP Basic Auth；调用方结束后必须清零。 */
bool ur_device_copy_http_auth(char *username, size_t username_size, char *password,
                              size_t password_size);
/* ur_device_photo_complete 把相机 worker 结果送回 MQTT 任务发布 actionReply。 */
bool ur_device_photo_complete(const char *token, int code, const char *message,
                              const char *file_uri);

#if CONFIG_UR_AI_SIGNALING
/* 统一信令事件类别；payload 仅 Message 事件非空，消费方必须调用释放接口。 */
typedef enum {
    UR_SIGNALING_EVENT_CONNECTED = 0,
    UR_SIGNALING_EVENT_DISCONNECTED = 1,
    UR_SIGNALING_EVENT_SUBSCRIBED = 2,
    UR_SIGNALING_EVENT_PUBLISHED = 3,
    UR_SIGNALING_EVENT_MESSAGE = 4,
    UR_SIGNALING_EVENT_FAILED = 5
} ur_signaling_event_kind_t;

/* ur_signaling_event_t 是信令适配层与 MQTT 任务之间的一份报文副本。 */
typedef struct {
    ur_signaling_event_kind_t kind;
    int packet_id; /* SUBACK/PUBACK 报文标识，事件与连接类别为 0。 */
    int code;      /* 0 成功，负数为传输失败。 */
    char topic[160];
    char *payload; /* PSRAM 副本，仅 MESSAGE 事件非空。 */
    size_t length;
} ur_signaling_event_t;

/* ur_signaling_publish 以 QoS1 发布信令上行；PUBACK 以 PUBLISHED 事件回投。 */
bool ur_signaling_publish(const char *topic, const char *payload, size_t length);
/* ur_signaling_subscribe 订阅精确下行主题；SUBACK 以 SUBSCRIBED 事件回投。 */
bool ur_signaling_subscribe(const char *topic);
/* ur_signaling_take_event 按到达顺序取事件；无事件返回 false。 */
bool ur_signaling_take_event(ur_signaling_event_t *event);
/* ur_signaling_free_event 释放事件持有的报文副本；事件本身由调用方栈上持有。 */
void ur_signaling_free_event(ur_signaling_event_t *event);
/* ur_signaling_inject_connected 在共享 MQTT 连接在线时补投一个 Connected 事件；
 * 连接事件只在重连时推送，长期在线的连接需要按需补投，否则信令状态机永远等不到。 */
bool ur_signaling_inject_connected(void);
/* ur_signaling_identity 输出 productID/deviceName；未联网完成注册前返回 false。 */
bool ur_signaling_identity(char *out, size_t size);
#endif /* CONFIG_UR_AI_SIGNALING */
#endif

/* 创建联犀 MQTT 设备任务（内部等待网络就绪后连接，重复调用忽略） */
void ur_device_task_start(void);

#ifdef __cplusplus
}
#endif

#endif /* UR_DEVICE_H */
