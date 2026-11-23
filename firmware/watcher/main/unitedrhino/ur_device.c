/* 联犀设备任务：动态注册、MQTT 物模型、鉴权自愈与全量 OTA，按构建板型上报硬件。 */
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "lite-utils.h"
#include "unitedrhino_iot_export.h"
#include "unitedrhino_iot_export_device_bind.h"
#include "unitedrhino_iot_export_dynreg.h"
#include "unitedrhino_iot_import.h"

#include "ur_bridge.h"
#include "ur_device.h"
#include "ur_network.h"

#define UR_TAG "ur_device"
#define UR_REPORT_INTERVAL_MS (60 * 1000)
/* UR_MQTT_YIELD_MS：MQTT 任务单轮 Yield 窗口，既是 socket 读窗口，也是各上行队列
 * （信令/属性/AI/动作/照片结果）的排空节拍——队列只在每轮循环末尾被排空。
 * 原 500ms 让排队上行平均晚约 250ms 才真正发出：真机归档实测信令 publish 被量化到
 * ~515ms 网格（相邻 publish 间隔 528/1549/2047ms），libpeer 连通到实际上报
 * mediaState=connected 为 387/425/436/438ms。压到 50ms 后该等待不超过一个窗口，
 * 上报进入 100ms 内。再压到 20ms：建连关键路径上 audioStart/媒体信令的上行排空
 * 等待从最多 50ms 降到 20ms；每轮循环体只做空队列检查与一次 select，开销可忽略。 */
#define UR_MQTT_YIELD_MS 20
/* UR_MQTT_TRANSIENT_BUDGET_MS：读包级瞬态失败（超时/读包失败）的累计重试预算，
 * 超预算才按连接级错误重建；重连会丢弃错位的 MQTT 流。
 * 旧实现按"连续 10 次 × 200ms 退避"计数，真机 UDP 洪流场景约 2 秒即判 mqttDisconnected、
 * 会话被终结；同一场景平台给媒体恢复的固定窗口是 25 秒，设备先于平台自判失败会白丢会话。
 * 预算与平台恢复窗口同量级后，设备不再在平台截止前终结连接。 */
#define UR_MQTT_TRANSIENT_BUDGET_MS 25000
#define UR_TOPIC_BUF_LEN 160
#define UR_AUTH_FAILURE_LIMIT 3
#define UR_AI_QUEUE_DEPTH 16
#define UR_AI_PAYLOAD_MAX 1536
#define UR_PROPERTY_QUEUE_DEPTH 16
#define UR_PROPERTY_PAYLOAD_MAX 4096
#define UR_ACTION_QUEUE_DEPTH 8
#define UR_PHOTO_RESULT_QUEUE_DEPTH 4
#define UR_TIME_SYNC_TIMEOUT_SECONDS 15

static const uint32_t s_retry_delays_ms[] = {30000, 60000, 120000, 240000, 300000};

static void *s_mqtt_client = NULL;
static volatile bool s_connected = false;
/* MQTT 任务内保存重连事件，避免 SDK 后续读包把 Yield 的重连返回值覆盖。 */
static bool s_resubscribe_pending = false;
/* s_force_mqtt_reconnect 平台重启后设备 TCP 不断但平台侧连接绑定失效，上行信令
 * 全部静默丢弃（会话建立超时根因）；信令协议层检测到无任何 ACK 的建会话超时后
 * 置位本标志，MQTT 任务下一轮强制断开重连+重订阅自愈。 */
static volatile bool s_force_mqtt_reconnect = false;
/* 仅 MQTT 任务读写；按本次报文 ID 等待确认，不能复用重连前的 topic 缓存。 */
static int s_subscribe_packet_id = 0;
/* 1 为待确认，0 为成功，负数为失败；失败锁存，防止 SDK 随后重复成功事件覆盖。 */
static int s_subscribe_result = 1;
static volatile bool s_task_started = false;
/* 属性请求由应用与 MQTT 两任务共享；原子交换避免消费期间的新请求丢失。 */
static atomic_bool s_report_requested = false;
static volatile bool s_report_info_requested = false;
static char s_product_id[MAX_SIZE_OF_PRODUCT_ID + 1] = {0};
static char s_device_name[MAX_SIZE_OF_DEVICE_NAME + 1] = {0};
static char s_report_info_token[64] = {0};
static char s_ota_msg_token[128] = {0};
static SemaphoreHandle_t s_mqtt_client_mutex = NULL;

/* ur_property_item_t 保存物模型下行原文；MQTT 回调只复制，解析和回复在设备任务执行。 */
typedef struct {
    size_t length;
    char *payload;
} ur_property_item_t;

static QueueHandle_t s_property_queue = NULL;

/* ur_publish_topic 构造带产品和设备名的主题并发布。 */
static int ur_publish_topic(void *client, const char *topic_prefix, const char *message,
                            int message_len);

#if CONFIG_UR_AI
/* ur_ai_publish_item_t 只在内部 RAM 队列保存指针；报文放入 PSRAM，
 * 避免 16 个最大报文挤占 MQTT 建连需要的连续内部内存。 */
typedef struct {
    size_t length;
    char *payload;
} ur_ai_publish_item_t;

/* ur_action_item_t 是设备任务解析出的行为最小快照，不持有 cJSON 指针。 */
typedef struct {
    char token[128];
    char action_id[32];
    char content[129];
    char request_id[17]; /* 换网请求关联 ID，独立于 MQTT msgToken。 */
    char params[256];    /* 下行 params 序列化副本，供板级行为解析输入参数。 */
} ur_action_item_t;

/* ur_photo_result_item_t 只在队列内保存 fileUri 指针，实际内容分配在 PSRAM。 */
typedef struct {
    char token[128];
    int code;
    char message[64];
    char *file_uri;
} ur_photo_result_item_t;

static QueueHandle_t s_ai_publish_queue = NULL;
static QueueHandle_t s_action_queue = NULL;
static QueueHandle_t s_photo_result_queue = NULL;
static portMUX_TYPE s_ai_callback_mux = portMUX_INITIALIZER_UNLOCKED;
static ur_ai_message_callback_t s_ai_message_callback = NULL;
static ur_ai_connection_callback_t s_ai_connection_callback = NULL;
static void *s_ai_callback_context = NULL;

void ur_device_ai_set_callbacks(ur_ai_message_callback_t message_callback,
                                ur_ai_connection_callback_t connection_callback,
                                void *context)
{
    taskENTER_CRITICAL(&s_ai_callback_mux);
    s_ai_message_callback = message_callback;
    s_ai_connection_callback = connection_callback;
    s_ai_callback_context = context;
    taskEXIT_CRITICAL(&s_ai_callback_mux);
}

bool ur_device_ai_publish(const char *payload, size_t length)
{
    if (payload == NULL || length == 0 || length >= UR_AI_PAYLOAD_MAX ||
        s_ai_publish_queue == NULL) {
        return false;
    }
    char *payload_copy = heap_caps_malloc(length + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (payload_copy == NULL) {
        return false;
    }
    memcpy(payload_copy, payload, length);
    payload_copy[length] = '\0';
    ur_ai_publish_item_t item = {.length = length, .payload = payload_copy};
    if (xQueueSend(s_ai_publish_queue, &item, 0) != pdTRUE) {
        heap_caps_free(payload_copy);
        return false;
    }
    return true;
}

bool ur_device_copy_http_auth(char *username, size_t username_size, char *password,
                              size_t password_size)
{
    if (username == NULL || password == NULL || s_mqtt_client_mutex == NULL) {
        return false;
    }
    bool success = false;
    if (xSemaphoreTake(s_mqtt_client_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        if (s_connected && s_mqtt_client != NULL) {
            ESP_LOGI(UR_TAG, "photo auth stage=generate");
            int auth_result = IOT_MQTT_GenerateHTTPAuth(s_mqtt_client, username, username_size,
                                                       password, password_size);
            ESP_LOGI(UR_TAG, "photo auth result=%d", auth_result);
            success = auth_result == UNITEDRHINO_RET_SUCCESS;
        }
        xSemaphoreGive(s_mqtt_client_mutex);
    }
    if (!success) {
        memset(username, 0, username_size);
        memset(password, 0, password_size);
    }
    return success;
}

bool ur_device_photo_complete(const char *token, int code, const char *message,
                              const char *file_uri)
{
    if (token == NULL || token[0] == '\0' || strlen(token) >= 128 || message == NULL ||
        s_photo_result_queue == NULL || !s_connected) {
        return false;
    }
    ur_photo_result_item_t item = {.code = code, .file_uri = NULL};
    strlcpy(item.token, token, sizeof(item.token));
    strlcpy(item.message, message, sizeof(item.message));
    if (file_uri != NULL && file_uri[0] != '\0') {
        size_t length = strlen(file_uri);
        if (length > 2048) {
            return false;
        }
        item.file_uri = heap_caps_malloc(length + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (item.file_uri == NULL) {
            return false;
        }
        memcpy(item.file_uri, file_uri, length + 1);
    }
    if (xQueueSend(s_photo_result_queue, &item, 0) != pdTRUE) {
        heap_caps_free(item.file_uri);
        return false;
    }
    return true;
}

/* ur_notify_ai_connection 复制回调快照后通知协议层，临界区内不运行用户代码。 */
static void ur_notify_ai_connection(bool connected)
{
    ur_ai_connection_callback_t callback = NULL;
    void *context = NULL;
    taskENTER_CRITICAL(&s_ai_callback_mux);
    callback = s_ai_connection_callback;
    context = s_ai_callback_context;
    taskEXIT_CRITICAL(&s_ai_callback_mux);
    if (callback != NULL) {
        callback(connected, context);
    }
}

/* ur_ai_message_cb 只复制并转交 AI 报文；解析、UI 和音频均在协议工作任务处理。 */
static void ur_ai_message_cb(void *client, MQTTMessage *message, void *user_data)
{
    (void)client;
    (void)user_data;
    if (message == NULL || message->payload == NULL || message->payload_len == 0) {
        return;
    }
    ur_ai_message_callback_t callback = NULL;
    void *context = NULL;
    taskENTER_CRITICAL(&s_ai_callback_mux);
    callback = s_ai_message_callback;
    context = s_ai_callback_context;
    taskEXIT_CRITICAL(&s_ai_callback_mux);
    if (callback != NULL) {
        callback((const char *)message->payload, message->payload_len, context);
    }
}

/* ur_drain_ai_publish_queue 仅由 MQTT 任务调用，串行发布所有 AI 上行。 */
static void ur_drain_ai_publish_queue(void)
{
    ur_ai_publish_item_t item = {0};
    while (s_connected && s_ai_publish_queue != NULL &&
           xQueueReceive(s_ai_publish_queue, &item, 0) == pdTRUE) {
        int result = ur_publish_topic(s_mqtt_client, "$thing/up/ai", item.payload,
                                      (int)item.length);
        heap_caps_free(item.payload);
        item.payload = NULL;
        if (result < 0) {
            ESP_LOGW(UR_TAG, "AI uplink publish failed");
            break;
        }
    }
}

/* ur_clear_ai_publish_queue 在断线时逐项释放 PSRAM 报文，不能直接 reset 泄漏指针。 */
static void ur_clear_ai_publish_queue(void)
{
    ur_ai_publish_item_t item = {0};
    while (s_ai_publish_queue != NULL &&
           xQueueReceive(s_ai_publish_queue, &item, 0) == pdTRUE) {
        heap_caps_free(item.payload);
    }
}

/* ur_clear_action_queues 清理断线期间不可再发布的行为与拍照结果。 */
static void ur_clear_action_queues(void)
{
    if (s_action_queue != NULL) {
        xQueueReset(s_action_queue);
    }
    ur_photo_result_item_t item = {0};
    while (s_photo_result_queue != NULL &&
           xQueueReceive(s_photo_result_queue, &item, 0) == pdTRUE) {
        heap_caps_free(item.file_uri);
    }
}
#endif

/* ur_clear_property_queue 在断线时释放尚未处理的物模型下行副本。 */
static void ur_clear_property_queue(void)
{
    ur_property_item_t item = {0};
    while (s_property_queue != NULL && xQueueReceive(s_property_queue, &item, 0) == pdTRUE) {
        heap_caps_free(item.payload);
    }
}

bool ur_device_is_connected(void)
{
    return s_connected;
}

/* ur_device_request_report 只置位请求，由 MQTT 任务串行发布。 */
/* ur_device_force_mqtt_reconnect 请求 MQTT 任务强制断开重连（平台重启自愈）。 */
void ur_device_force_mqtt_reconnect(void)
{
    if (s_connected) {
        s_force_mqtt_reconnect = true;
        ESP_LOGW(UR_TAG, "MQTT force reconnect requested (platform binding stale)");
    }
}

void ur_device_request_report(void) { s_report_requested = true; }

/* ur_next_retry_delay 返回 30/60/120/240/300 秒封顶退避并推进索引。 */
static uint32_t ur_next_retry_delay(size_t *index)
{
    size_t last = sizeof(s_retry_delays_ms) / sizeof(s_retry_delays_ms[0]) - 1;
    size_t current = *index > last ? last : *index;
    if (*index < last) {
        (*index)++;
    }
    return s_retry_delays_ms[current];
}

/* ur_publish_topic 构造带产品和设备名的主题并发布。 */
static int ur_publish_topic(void *client, const char *topic_prefix, const char *message,
                            int message_len)
{
    char topic[UR_TOPIC_BUF_LEN] = {0};
    PublishParams params = DEFAULT_PUB_PARAMS;
    int size = HAL_Snprintf(topic, sizeof(topic), "%s/%s/%s", topic_prefix, s_product_id,
                            s_device_name);
    if (client == NULL || size < 0 || size >= (int)sizeof(topic)) {
        return UNITEDRHINO_ERR_FAILURE;
    }
    params.qos = QOS0;
    params.payload_len = message_len;
    params.payload = (void *)message;
    return IOT_MQTT_Publish(client, topic, &params);
}

/* ur_make_token 生成设备侧请求 token，避免上报之间互相覆盖。 */
static void ur_make_token(char *buffer, size_t size, const char *prefix)
{
    HAL_Snprintf(buffer, size, "%s-%u", prefix, (unsigned)HAL_GetTimeMs());
}

/* ur_report_all_properties 上报平台物模型定义的属性；板级可整体接管属性集合。 */
static void ur_report_all_properties(void)
{
    char message[512] = {0};
    char token[64] = {0};
    char props[384] = {0};
    int props_len = ur_bridge_get_properties_json(props, sizeof(props));
    ur_make_token(token, sizeof(token), "ur-report");
    int length;
    if (props_len > 0) {
        length = HAL_Snprintf(message, sizeof(message),
                              "{\"method\":\"report\",\"msgToken\":\"%s\",\"params\":%s}",
                              token, props);
    } else {
        int battery = 0;
        bool charging = false;
        ur_bridge_get_battery(&battery, &charging);
        length = HAL_Snprintf(
            message, sizeof(message),
            "{\"method\":\"report\",\"msgToken\":\"%s\",\"params\":{"
            "\"Battery\":%d,\"Charging\":%s,\"Volume\":%d,\"Brightness\":%d,\"Rssi\":%d}}",
            token, battery, charging ? "true" : "false", ur_bridge_get_volume(),
            ur_bridge_get_brightness(), ur_bridge_get_rssi());
    }
    if (length > 0 && length < (int)sizeof(message)) {
        ur_publish_topic(s_mqtt_client, "$thing/up/property", message, length);
    }
}

/* ur_report_device_info 上报固件与硬件信息；平台以 version 判定 OTA 是否成功。 */
static void ur_report_device_info(void)
{
    char message[512] = {0};
    /* 坦克复用多板工程，基础信息必须匹配实物；其他板型沿用原有上报值。 */
#if CONFIG_BOARD_TYPE_ESP_SPARKBOT
    const char *hardware = "ESP-SparkBot Tank";
#else
    const char *hardware = "SenseCAP Watcher";
#endif
    const esp_app_desc_t *description = esp_app_get_description();
    ur_make_token(s_report_info_token, sizeof(s_report_info_token), "ur-info");
    int length = HAL_Snprintf(
        message, sizeof(message),
        "{\"method\":\"reportInfo\",\"msgToken\":\"%s\",\"params\":{"
        "\"version\":\"%s\",\"module\":\"default\",\"mac\":\"%s\","
        "\"hardInfo\":\"%s\",\"softInfo\":\"xiaozhi ESP-IDF 6\",\"rssi\":%d}}",
        s_report_info_token, description->version, s_device_name, hardware, ur_bridge_get_rssi());
    if (length > 0 && length < (int)sizeof(message)) {
        ur_publish_topic(s_mqtt_client, "$thing/up/property", message, length);
    }
}

/* ur_json_string 读取字符串字段，并允许一个旧字段作为下行兼容。 */
static const char *ur_json_string(cJSON *root, const char *field, const char *legacy_field)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, field);
    if (!cJSON_IsString(item) && legacy_field != NULL) {
        item = cJSON_GetObjectItemCaseSensitive(root, legacy_field);
    }
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

/* ur_handle_control 执行属性控制并按当前 camelCase 契约回执；板级可整体接管。 */
static void ur_handle_control(const char *token, cJSON *root)
{
    cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
    char data[128] = {0};
    char message[384] = {0};
    int handled = 0;
    int board_handled = cJSON_IsObject(params) ? ur_bridge_handle_control(params, data, sizeof(data)) : -1;
    if (board_handled >= 0) {
        handled = board_handled;
    } else if (board_handled == -1) {
        cJSON *volume = cJSON_IsObject(params) ? cJSON_GetObjectItemCaseSensitive(params, "Volume") : NULL;
        cJSON *brightness = cJSON_IsObject(params) ? cJSON_GetObjectItemCaseSensitive(params, "Brightness") : NULL;
        int offset = 0;

        offset += HAL_Snprintf(data + offset, sizeof(data) - offset, "{");
        if (cJSON_IsNumber(volume)) {
            int value = volume->valueint < 0 ? 0 : (volume->valueint > 100 ? 100 : volume->valueint);
            ur_bridge_set_volume(value);
            offset += HAL_Snprintf(data + offset, sizeof(data) - offset, "\"Volume\":%d", value);
            handled++;
        }
        if (cJSON_IsNumber(brightness)) {
            int value = brightness->valueint < 0 ? 0 : (brightness->valueint > 100 ? 100 : brightness->valueint);
            ur_bridge_set_brightness(value);
            offset += HAL_Snprintf(data + offset, sizeof(data) - offset, "%s\"Brightness\":%d",
                                   handled ? "," : "", value);
            handled++;
        }
        HAL_Snprintf(data + offset, sizeof(data) - offset, "}");
    } else {
        /* 板级接管但回执缓冲不足等异常，按不支持处理避免回执残缺。 */
        strlcpy(data, "{}", sizeof(data));
    }

    int code = handled > 0 ? 200 : 400;
    int length = HAL_Snprintf(
        message, sizeof(message),
        "{\"method\":\"controlReply\",\"msgToken\":\"%s\",\"code\":%d,"
        "\"msg\":\"%s\",\"data\":%s}",
        token != NULL ? token : "", code, handled > 0 ? "success" : "unsupported property", data);
    if (length > 0 && length < (int)sizeof(message)) {
        ur_publish_topic(s_mqtt_client, "$thing/up/property", message, length);
    }
    if (handled > 0) {
        s_report_requested = true;
    }
}

/* ur_publish_action_reply 在 MQTT 任务中构造行为回执；fileUri 只在拍照成功时出现。 */
static bool ur_publish_action_reply(const char *action_id, const char *token, int code,
                                    const char *message, const char *file_uri, bool result)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    if (root == NULL || data == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(data);
        return false;
    }
    cJSON_AddStringToObject(root, "method", "actionReply");
    cJSON_AddStringToObject(root, "actionID", action_id != NULL ? action_id : "");
    cJSON_AddStringToObject(root, "msgToken", token != NULL ? token : "");
    cJSON_AddNumberToObject(root, "code", code);
    cJSON_AddStringToObject(root, "msg", message != NULL ? message : "failed");
    if (file_uri != NULL && file_uri[0] != '\0') {
        cJSON_AddStringToObject(data, "fileUri", file_uri);
    } else {
        cJSON_AddBoolToObject(data, "Result", result);
    }
    cJSON_AddItemToObject(root, "data", data);
    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload != NULL) {
        int result = ur_publish_topic(s_mqtt_client, "$thing/up/action", payload, (int)strlen(payload));
        cJSON_free(payload);
        return result >= 0;
    }
    return false;
}

#if CONFIG_UR_AI
/* ur_enqueue_action 在设备任务中复制行为必要字段并非阻塞入队。 */
static bool ur_enqueue_action(const char *token, cJSON *root)
{
    if (token == NULL || strlen(token) >= 128 || s_action_queue == NULL) {
        return false;
    }
    const char *action_id = ur_json_string(root, "actionID", "identifier");
    if (action_id == NULL) {
        action_id = ur_json_string(root, "actionId", NULL);
    }
    if (action_id == NULL || strlen(action_id) >= 32) {
        return false;
    }
    ur_action_item_t item = {0};
    strlcpy(item.token, token, sizeof(item.token));
    strlcpy(item.action_id, action_id, sizeof(item.action_id));
    cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
    cJSON *request_id = cJSON_IsObject(params) ? cJSON_GetObjectItemCaseSensitive(params, "requestId") : NULL;
    if (cJSON_IsString(request_id) && strlen(request_id->valuestring) == 16) {
        strlcpy(item.request_id, request_id->valuestring, sizeof(item.request_id));
    }
    cJSON *content = cJSON_IsObject(params) ? cJSON_GetObjectItemCaseSensitive(params, "Content")
                                            : NULL;
    if (cJSON_IsString(content) && strlen(content->valuestring) <= 128) {
        strlcpy(item.content, content->valuestring, sizeof(item.content));
    }
    /* 坦克动作和拨号依赖完整参数快照，超长JSON必须拒绝，避免截断成默认动作。 */
    if ((strncmp(action_id, "tank", 4) == 0 || strcmp(action_id, "CallDevice") == 0) &&
        cJSON_IsObject(params)) {
        char *params_str = cJSON_PrintUnformatted(params);
        if (params_str == NULL) {
            return false;
        }
        const size_t params_len = strlen(params_str);
        if (params_len >= sizeof(item.params)) {
            cJSON_free(params_str);
            return false;
        }
        memcpy(item.params, params_str, params_len + 1);
        cJSON_free(params_str);
    }
    return xQueueSend(s_action_queue, &item, 0) == pdTRUE;
}

/* ur_drain_action_queue 在 MQTT 任务执行普通行为，并把 takePhoto 交给独立相机 worker。 */
static void ur_drain_action_queue(void)
{
    ur_action_item_t item = {0};
    while (s_action_queue != NULL && xQueueReceive(s_action_queue, &item, 0) == pdTRUE) {
        bool reboot = strcmp(item.action_id, "Reboot") == 0;
        if (strcmp(item.action_id, "configureNetwork") == 0) {
            int code = ur_bridge_prepare_network_change(item.request_id);
            bool replied = ur_publish_action_reply(item.action_id, item.token, code,
                                                   code == 200 ? "physical confirmation required" : "request rejected",
                                                   NULL, code == 200);
            if (code == 200) {
                if (replied) ur_bridge_present_network_change(item.request_id);
                else ur_network_cancel_request(item.request_id, UR_NETWORK_ERROR_NETWORK);
                s_report_requested = true;
            }
            continue;
        }
        if (strcmp(item.action_id, "takePhoto") == 0) {
            int reject_code = ur_bridge_start_photo_action(item.token);
            if (reject_code != 0) {
                ur_publish_action_reply(item.action_id, item.token, reject_code,
                                        reject_code == 409 ? "camera busy" :
                                        reject_code == 423 ? "ota in progress" :
                                                             "photo unavailable",
                                        NULL, false);
            }
            continue;
        }
        /* 协议内置行为之外的物模型行为交给板级处理（坦克动作等）；404 表示本板未注册。 */
        int board_code = ur_bridge_handle_action(item.action_id, item.params);
        if (board_code != 404) {
            bool board_ok = board_code == 0 || board_code == 200;
            ur_publish_action_reply(item.action_id, item.token, board_ok ? 200 : board_code,
                                    board_ok ? "success" : "action failed", NULL, board_ok);
            continue;
        }
        bool success = reboot || (strcmp(item.action_id, "SendMessage") == 0 && item.content[0]);
        if (success && item.content[0]) {
            ur_bridge_show_text(item.content);
        }
        ur_publish_action_reply(item.action_id, item.token, success ? 200 : 400,
                                success ? "success" : "unsupported action", NULL, success);
        if (reboot) {
            ESP_LOGI(UR_TAG, "software reboot requested by platform");
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_restart();
        }
    }
}

/* ur_drain_photo_results 在 MQTT 任务发布异步拍照结果并释放临时 fileUri。 */
static void ur_drain_photo_results(void)
{
    ur_photo_result_item_t item = {0};
    while (s_photo_result_queue != NULL &&
           xQueueReceive(s_photo_result_queue, &item, 0) == pdTRUE) {
        bool published = ur_publish_action_reply("takePhoto", item.token, item.code, item.message,
                                                 item.file_uri, item.code == 200);
#if CONFIG_UR_AI_E2E_TEST
        /* 测试证据只记录真实发布结果，不输出 Token、图片地址或认证信息。 */
        ESP_LOGI(UR_TAG, "[UR_VOICE_PHOTO_E2E] action_reply code=%d published=%d",
                 item.code, published);
#else
        (void)published;
#endif
        heap_caps_free(item.file_uri);
        item.file_uri = NULL;
    }
}

#if CONFIG_UR_AI_SIGNALING
#define UR_SIGNALING_PAYLOAD_MAX 4096
#define UR_SIGNALING_EVENT_DEPTH 24
#define UR_SIGNALING_CMD_DEPTH 12

/* ur_signaling_event_t 的队列条目；主题固定长度，正文为 PSRAM 副本。 */
typedef struct {
    ur_signaling_event_kind_t kind;
    int packet_id;
    int code;
    char topic[UR_TOPIC_BUF_LEN];
    char *payload;
    size_t length;
} ur_signaling_event_item_t;

/* ur_signaling_cmd_t 是信令线程投递给 MQTT 任务的发布/订阅请求。 */
typedef struct {
    bool subscribe;
    char topic[UR_TOPIC_BUF_LEN];
    char *payload; /* 发布正文 PSRAM 副本；订阅时为 NULL。 */
    size_t length;
} ur_signaling_cmd_item_t;

static QueueHandle_t s_signaling_event_queue = NULL;
static QueueHandle_t s_signaling_cmd_queue = NULL;

/* 已订阅主题缓存：MQTT 建连时 ur_subscribe_down_topics 已订阅 signaling/message，
 * 信令 runtime 收到 Connected 后还会通过命令队列再订阅一次。缓存命中时只在
 * MQTT 任务内回投 SUBSCRIBED 成功事件（端口仍按序分配唯一 operation id），
 * 省掉一次 Subscribe/SUBACK 往返（真机约 150ms）。只在 MQTT 任务内读写；
 * 每次重建 broker 订阅前由 ur_subscribe_down_topics 清空，避免沿用旧连接事实。 */
#define UR_SIGNALING_SUB_CACHE_MAX 4
static char s_signaling_sub_cache[UR_SIGNALING_SUB_CACHE_MAX][UR_TOPIC_BUF_LEN];
static size_t s_signaling_sub_cache_count = 0;

static void ur_signaling_sub_cache_clear(void)
{
    s_signaling_sub_cache_count = 0;
}

static bool ur_signaling_sub_cache_hit(const char *topic)
{
    if (topic == NULL) {
        return false;
    }
    for (size_t i = 0; i < s_signaling_sub_cache_count; ++i) {
        if (strncmp(s_signaling_sub_cache[i], topic, UR_TOPIC_BUF_LEN) == 0) {
            return true;
        }
    }
    return false;
}

static void ur_signaling_sub_cache_add(const char *topic)
{
    if (topic == NULL || ur_signaling_sub_cache_hit(topic)) {
        return;
    }
    if (s_signaling_sub_cache_count >= UR_SIGNALING_SUB_CACHE_MAX) {
        /* 只缓存 signaling/message 等少量主题；溢出淘汰最旧，保持最新连接事实。 */
        memmove(s_signaling_sub_cache[0], s_signaling_sub_cache[1],
                sizeof(s_signaling_sub_cache[0]) * (UR_SIGNALING_SUB_CACHE_MAX - 1));
        s_signaling_sub_cache_count = UR_SIGNALING_SUB_CACHE_MAX - 1;
    }
    strncpy(s_signaling_sub_cache[s_signaling_sub_cache_count], topic, UR_TOPIC_BUF_LEN - 1);
    s_signaling_sub_cache[s_signaling_sub_cache_count][UR_TOPIC_BUF_LEN - 1] = '\0';
    ++s_signaling_sub_cache_count;
}

/* ur_signaling_push_event 复制主题并在事件满时丢弃最旧事件，保证信令线程不被阻塞。 */
static void ur_signaling_push_event(ur_signaling_event_kind_t kind, int packet_id, int code,
                                    const char *topic, const char *payload, size_t length)
{
    if (s_signaling_event_queue == NULL) {
        return;
    }
    ur_signaling_event_item_t item = {0};
    item.kind = kind;
    item.packet_id = packet_id;
    item.code = code;
    if (topic != NULL) {
        strncpy(item.topic, topic, sizeof(item.topic) - 1);
    }
    if (payload != NULL && length > 0) {
        item.payload = heap_caps_malloc(length + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (item.payload == NULL) {
            ESP_LOGW(UR_TAG, "signaling event dropped: payload alloc failed");
            return;
        }
        memcpy(item.payload, payload, length);
        item.payload[length] = '\0';
        item.length = length;
    }
    if (xQueueSend(s_signaling_event_queue, &item, 0) != pdTRUE) {
        /* 队列只保留最新现场；迟到事件对信令状态机没有副作用。 */
        ur_signaling_event_item_t stale;
        if (xQueueReceive(s_signaling_event_queue, &stale, 0) == pdTRUE) {
            if (stale.payload != NULL) {
                heap_caps_free(stale.payload);
            }
        }
        if (xQueueSend(s_signaling_event_queue, &item, 0) != pdTRUE && item.payload != NULL) {
            heap_caps_free(item.payload);
        }
    }
}

bool ur_signaling_take_event(ur_signaling_event_t *event)
{
    if (event == NULL || s_signaling_event_queue == NULL) {
        return false;
    }
    ur_signaling_event_item_t item;
    if (xQueueReceive(s_signaling_event_queue, &item, 0) != pdTRUE) {
        return false;
    }
    event->kind = item.kind;
    event->packet_id = item.packet_id;
    event->code = item.code;
    memcpy(event->topic, item.topic, sizeof(event->topic));
    event->payload = item.payload;
    event->length = item.length;
    return true;
}

void ur_signaling_free_event(ur_signaling_event_t *event)
{
    if (event != NULL && event->payload != NULL) {
        heap_caps_free(event->payload);
        event->payload = NULL;
        event->length = 0;
    }
}

bool ur_signaling_publish(const char *topic, const char *payload, size_t length)
{
    if (topic == NULL || payload == NULL || length == 0 || length >= UR_SIGNALING_PAYLOAD_MAX ||
        s_signaling_cmd_queue == NULL) {
        return false;
    }
    ur_signaling_cmd_item_t item = {0};
    item.subscribe = false;
    strncpy(item.topic, topic, sizeof(item.topic) - 1);
    item.payload = heap_caps_malloc(length + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (item.payload == NULL) {
        return false;
    }
    memcpy(item.payload, payload, length);
    item.payload[length] = '\0';
    item.length = length;
    if (xQueueSend(s_signaling_cmd_queue, &item, 0) != pdTRUE) {
        heap_caps_free(item.payload);
        return false;
    }
    return true;
}

bool ur_signaling_subscribe(const char *topic)
{
    if (topic == NULL || s_signaling_cmd_queue == NULL) {
        return false;
    }
    ur_signaling_cmd_item_t item = {0};
    item.subscribe = true;
    strncpy(item.topic, topic, sizeof(item.topic) - 1);
    if (xQueueSend(s_signaling_cmd_queue, &item, 0) != pdTRUE) {
        return false;
    }
    return true;
}

/* ur_signaling_inject_connected 共享连接在线时补投 Connected 事件；
 * 长期在线的连接不会重连，信令 runtime 的 Connecting 状态需要显式唤醒。 */
bool ur_signaling_inject_connected(void)
{
    if (!s_connected) {
        return false;
    }
    ur_signaling_push_event(UR_SIGNALING_EVENT_CONNECTED, 0, 0, NULL, NULL, 0);
    return true;
}

/* ur_signaling_message_cb 只把信令/消息下行复制进事件队列，禁止回调内解析。 */
static void ur_signaling_message_cb(void *client, MQTTMessage *message, void *user_data)
{
    (void)client;
    (void)user_data;
    if (message == NULL || message->payload == NULL || message->payload_len == 0 ||
        message->ptopic == NULL || message->topic_len == 0) {
        return;
    }
    char topic[UR_TOPIC_BUF_LEN] = {0};
    size_t topic_len = message->topic_len < sizeof(topic) - 1 ? message->topic_len
                                                              : sizeof(topic) - 1;
    memcpy(topic, message->ptopic, topic_len);
    ur_signaling_push_event(UR_SIGNALING_EVENT_MESSAGE, 0, 0, topic,
                            (const char *)message->payload, message->payload_len);
}

/* ur_signaling_subscribe_blocking 在 MQTT 任务内订阅并等待 SUBACK；复用既有确认通道。 */
static int ur_signaling_subscribe_blocking(const char *topic)
{
    if (s_mqtt_client == NULL) {
        return UNITEDRHINO_ERR_FAILURE;
    }
    SubscribeParams params = DEFAULT_SUB_PARAMS;
    params.qos = QOS1;
    params.on_message_handler = ur_signaling_message_cb;
    s_subscribe_packet_id = 0;
    s_subscribe_result = 1;
    int result = IOT_MQTT_Subscribe(s_mqtt_client, (char *)topic, &params);
    if (result < 0) {
        return result;
    }
    s_subscribe_packet_id = result;
    /* SDK 的 IOT_MQTT_Yield 会跑满 timeout 才返回（SUBACK 已到仍需等满窗口），
     * 原 10×1000ms 轮询让两个订阅串行耗约 2.05s；改为 20ms 短切片 + 单调
     * deadline，SUBACK 一到立即返回，单主题最坏仍按 2000ms 上限判失败。
     * 20ms 超时在 cycle_for_read 中返回 SUCCESS，不会误判为断线。 */
    TickType_t start = xTaskGetTickCount();
    while ((xTaskGetTickCount() - start) < pdMS_TO_TICKS(2000)) {
        result = IOT_MQTT_Yield(s_mqtt_client, 20);
        if (result != UNITEDRHINO_RET_SUCCESS && result != UNITEDRHINO_RET_MQTT_RECONNECTED) {
            s_subscribe_packet_id = 0;
            return result;
        }
        if (s_subscribe_result != 1) {
            s_subscribe_packet_id = 0;
            /* 只有当前连接确实订阅成功才登记缓存；失败/超时不登记。 */
            if (s_subscribe_result == UNITEDRHINO_RET_SUCCESS) {
                ur_signaling_sub_cache_add(topic);
            }
            return s_subscribe_result;
        }
    }
    s_subscribe_packet_id = 0;
    return UNITEDRHINO_ERR_FAILURE;
}

/* ur_drain_signaling_cmds 在 MQTT 任务内执行信令发布/订阅；结果转事件队列。 */
static void ur_drain_signaling_cmds(void)
{
    ur_signaling_cmd_item_t item;
    while (s_signaling_cmd_queue != NULL &&
           xQueueReceive(s_signaling_cmd_queue, &item, 0) == pdTRUE) {
        if (s_mqtt_client == NULL || !s_connected) {
            ur_signaling_push_event(item.subscribe ? UR_SIGNALING_EVENT_SUBSCRIBED
                                                    : UR_SIGNALING_EVENT_PUBLISHED,
                                    0, -1, item.topic, NULL, 0);
        } else if (item.subscribe) {
            int result;
            if (ur_signaling_sub_cache_hit(item.topic)) {
                /* 建连时 ur_subscribe_down_topics 已订阅 signaling/message：跳过
                 * 重复 Subscribe/SUBACK 往返，直接回投成功事件；端口仍按命令
                 * 顺序分配唯一 operation id，runtime 的去重/关联语义不变。 */
                ESP_LOGI(UR_TAG, "signaling subscribe cache hit: %s", item.topic);
                result = UNITEDRHINO_RET_SUCCESS;
            } else {
                result = ur_signaling_subscribe_blocking(item.topic);
            }
            ur_signaling_push_event(UR_SIGNALING_EVENT_SUBSCRIBED, 0, result, item.topic, NULL, 0);
        } else {
            PublishParams params = DEFAULT_PUB_PARAMS;
            params.qos = QOS1;
            params.payload = item.payload;
            params.payload_len = item.length;
            int packet_id = IOT_MQTT_Publish(s_mqtt_client, item.topic, &params);
            /* QoS1 受理不等于送达：只有 PUBACK 事件才算网络确认；本地失败立即报 FAILED。 */
            if (packet_id <= 0) {
                ur_signaling_push_event(UR_SIGNALING_EVENT_FAILED, 0, -1, item.topic, NULL, 0);
            }
        }
        if (item.payload != NULL) {
            heap_caps_free(item.payload);
        }
    }
}

/* ur_signaling_publish_ack 由 SDK 事件通道转发 QoS1 PUBACK 结果。 */
static void ur_signaling_publish_ack(uintptr_t packet_id, bool success)
{
    ur_signaling_push_event(UR_SIGNALING_EVENT_PUBLISHED, (int)packet_id, success ? 0 : -1, NULL,
                            NULL, 0);
}

/* ur_signaling_identity 输出 productID/deviceName 身份串，供信令主题与能力使用。 */
bool ur_signaling_identity(char *out, size_t size)
{
    if (out == NULL || size == 0 || s_product_id[0] == '\0' || s_device_name[0] == '\0') {
        return false;
    }
    int written = HAL_Snprintf(out, size, "%s/%s", s_product_id, s_device_name);
    return written > 0 && written < (int)size;
}
#endif /* CONFIG_UR_AI_SIGNALING */
#endif /* CONFIG_UR_AI */


/* ur_confirm_report_info 仅在 token、code 与待确认版本全部匹配时确认 OTA 镜像。 */
static void ur_confirm_report_info(const char *token, cJSON *root)
{
    cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
    char pending[32] = {0};
    if (token == NULL || strcmp(token, s_report_info_token) != 0 || !cJSON_IsNumber(code) ||
        code->valueint != 200 || !ur_bridge_get_pending_ota_version(pending, sizeof(pending))) {
        return;
    }
    if (!ur_bridge_confirm_ota(pending)) {
        ESP_LOGE(UR_TAG, "failed to confirm pending OTA image");
    }
}

/* ur_property_message_cb 只复制物模型下行，禁止在 MQTT 回调中解析、拍照或发布回复。 */
static void ur_property_message_cb(void *client, MQTTMessage *message, void *user_data)
{
    (void)client;
    (void)user_data;
    if (message == NULL || message->payload == NULL || message->payload_len == 0) {
        return;
    }

    if (message->payload_len >= UR_PROPERTY_PAYLOAD_MAX || s_property_queue == NULL) {
        ESP_LOGW(UR_TAG, "property downlink queue rejected payload");
        return;
    }
    char *payload = heap_caps_malloc(message->payload_len + 1,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (payload == NULL) {
        return;
    }
    memcpy(payload, message->payload, message->payload_len);
    payload[message->payload_len] = '\0';
    ur_property_item_t item = {.length = message->payload_len, .payload = payload};
    if (xQueueSend(s_property_queue, &item, 0) != pdTRUE) {
        heap_caps_free(payload);
        ESP_LOGW(UR_TAG, "property downlink queue is full");
    }
}

/* ur_process_property_payload 在 MQTT 设备任务中解析并分发物模型消息。 */
static void ur_process_property_payload(const char *payload, size_t length)
{
    cJSON *root = cJSON_ParseWithLength(payload, length);
    if (root == NULL) {
        ESP_LOGE(UR_TAG, "invalid downlink JSON");
        return;
    }

    const char *method = ur_json_string(root, "method", NULL);
    const char *token = ur_json_string(root, "msgToken", "clientToken");
    if (method != NULL) {
        ESP_LOGI(UR_TAG, "received downlink method=%s", method);
        if (strcmp(method, "control") == 0) {
            ur_handle_control(token, root);
        } else if (strcmp(method, "action") == 0) {
#if CONFIG_UR_AI
            if (!ur_enqueue_action(token, root)) {
                ESP_LOGW(UR_TAG, "action queue rejected downlink");
                const char *action_id = ur_json_string(root, "actionID", "identifier");
                if (action_id == NULL) {
                    action_id = ur_json_string(root, "actionId", NULL);
                }
                /* 设备任务不直接碰相机；仅把 takePhoto 拒绝结果放回 MQTT
                 * 发布队列，确保队列拥塞也有确定的非 200 回执。 */
                if (action_id != NULL && strcmp(action_id, "takePhoto") == 0) {
                    ur_device_photo_complete(token, 503, "action queue full", NULL);
                } else if (action_id != NULL &&
                           (strcmp(action_id, "configureNetwork") == 0 ||
                            strncmp(action_id, "tank", 4) == 0)) {
                    /* 非法或过长参数、队列拥塞都不执行动作，并返回明确失败。 */
                    ur_publish_action_reply(action_id, token, 503, "action rejected", NULL, false);
                }
            }
#else
            ur_publish_action_reply("", token, 501, "action worker unavailable", NULL, false);
#endif
        } else if (strcmp(method, "reportInfoReply") == 0) {
            ur_confirm_report_info(token, root);
        }
    }
    cJSON_Delete(root);
}

/* ur_drain_property_queue 串行处理回调复制的物模型下行。 */
static void ur_drain_property_queue(void)
{
    ur_property_item_t item = {0};
    while (s_property_queue != NULL && xQueueReceive(s_property_queue, &item, 0) == pdTRUE) {
        ur_process_property_payload(item.payload, item.length);
        heap_caps_free(item.payload);
        item.payload = NULL;
    }
}

/* ur_report_ota_progress 按平台 method:progress 契约上报 1-100 或固定失败码。 */
static void ur_report_ota_progress(int step, const char *description)
{
    char message[384] = {0};
    int length = HAL_Snprintf(
        message, sizeof(message),
        "{\"method\":\"progress\",\"msgToken\":\"%s\",\"params\":{"
        "\"step\":%d,\"desc\":\"%s\"}}",
        s_ota_msg_token, step, description);
    if (length > 0 && length < (int)sizeof(message)) {
        ur_publish_topic(s_mqtt_client, "$ota/up/progress", message, length);
    }
}

/* ur_handle_ota_upgrade 严格接受单文件、全量、MD5、4MB 以内且版本不同的包。 */
static void ur_handle_ota_upgrade(MQTTMessage *message)
{
    char *payload = HAL_Malloc(message->payload_len + 1);
    if (payload == NULL) {
        return;
    }
    memcpy(payload, message->payload, message->payload_len);
    payload[message->payload_len] = '\0';
    cJSON *root = cJSON_Parse(payload);
    HAL_Free(payload);
    if (root == NULL) {
        return;
    }

    const char *method = ur_json_string(root, "method", NULL);
    const char *token = ur_json_string(root, "msgToken", "clientToken");
    cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    cJSON *file = cJSON_IsObject(data) ? cJSON_GetObjectItemCaseSensitive(data, "file") : NULL;
    cJSON *files = cJSON_IsObject(data) ? cJSON_GetObjectItemCaseSensitive(data, "files") : NULL;
    if (!cJSON_IsObject(file)) {
        file = data; /* 当前平台单文件结构把 file 字段直接展开到 data。 */
    }

    const char *version = cJSON_IsObject(data) ? ur_json_string(data, "version", NULL) : NULL;
    const char *sign_method = cJSON_IsObject(data) ? ur_json_string(data, "signMethod", NULL) : NULL;
    const char *url = cJSON_IsObject(file) ? ur_json_string(file, "fileUrl", NULL) : NULL;
    const char *name = cJSON_IsObject(file) ? ur_json_string(file, "name", NULL) : NULL;
    const char *md5 = cJSON_IsObject(file) ? ur_json_string(file, "fileMd5", NULL) : NULL;
    cJSON *is_diff = cJSON_IsObject(data) ? cJSON_GetObjectItemCaseSensitive(data, "isDiff") : NULL;
    cJSON *size = cJSON_IsObject(file) ? cJSON_GetObjectItemCaseSensitive(file, "size") : NULL;
    const esp_app_desc_t *description = esp_app_get_description();
    /* 实际可写分区决定上限；旧设备仍按其 4 MiB 分区拒绝过大的升级包。 */
    const esp_partition_t *next_partition = esp_ota_get_next_update_partition(NULL);

    bool valid = method != NULL && strcmp(method, "upgrade") == 0 && token != NULL &&
                 version != NULL && strcmp(version, description->version) != 0 &&
                 cJSON_IsNumber(is_diff) && is_diff->valueint == 1 && sign_method != NULL &&
                 strcasecmp(sign_method, "MD5") == 0 && !cJSON_IsArray(files) && url != NULL &&
                 name != NULL && md5 != NULL && strlen(md5) == 32 && cJSON_IsNumber(size) &&
                 next_partition != NULL && size->valuedouble > 0 &&
                 size->valuedouble <= next_partition->size;

    if (!valid) {
        if (token != NULL) {
            strncpy(s_ota_msg_token, token, sizeof(s_ota_msg_token) - 1);
            s_ota_msg_token[sizeof(s_ota_msg_token) - 1] = '\0';
            ur_report_ota_progress(-4, "package or version mismatch");
        }
        ESP_LOGE(UR_TAG, "rejected incompatible OTA package");
        cJSON_Delete(root);
        return;
    }

    strncpy(s_ota_msg_token, token, sizeof(s_ota_msg_token) - 1);
    s_ota_msg_token[sizeof(s_ota_msg_token) - 1] = '\0';
    if (!ur_bridge_start_ota(url, version, (size_t)size->valuedouble, md5)) {
        ur_report_ota_progress(-1, "OTA worker unavailable");
    } else {
        ur_report_ota_progress(1, "download started");
        ESP_LOGI(UR_TAG, "accepted OTA target version=%s size=%u", version,
                 (unsigned)size->valuedouble);
    }
    cJSON_Delete(root);
}

/* ur_ota_message_cb 是独立 OTA 下行主题的轻量回调。 */
static void ur_ota_message_cb(void *client, MQTTMessage *message, void *user_data)
{
    (void)client;
    (void)user_data;
    if (message != NULL && message->payload != NULL) {
        ur_handle_ota_upgrade(message);
    }
}

/* ur_subscribe_topic 订阅单个主题并等待 SUBACK。 */
static int ur_subscribe_topic(void *client, const char *prefix, OnMessageHandler callback)
{
    char topic[UR_TOPIC_BUF_LEN] = {0};
    SubscribeParams params = DEFAULT_SUB_PARAMS;
    int size = HAL_Snprintf(topic, sizeof(topic), "%s/%s/%s", prefix, s_product_id, s_device_name);
    if (size < 0 || size >= (int)sizeof(topic)) {
        return UNITEDRHINO_ERR_FAILURE;
    }
    params.qos = QOS0;
    params.on_message_handler = callback;
    s_subscribe_packet_id = 0;
    s_subscribe_result = 1;
    int result = IOT_MQTT_Subscribe(client, topic, &params);
    if (result < 0) {
        return result;
    }
    s_subscribe_packet_id = result;
    /* SDK 的 IOT_MQTT_Yield 会跑满 timeout 才返回（SUBACK 已到仍需等满窗口），
     * 原 10×1000ms 轮询让两个订阅串行耗约 2.05s；改为 20ms 短切片 + 单调
     * deadline，SUBACK 一到立即返回，单主题最坏仍按 2000ms 上限判失败。
     * 20ms 超时在 cycle_for_read 中返回 SUCCESS，不会误判为断线。 */
    TickType_t start = xTaskGetTickCount();
    while ((xTaskGetTickCount() - start) < pdMS_TO_TICKS(2000)) {
        result = IOT_MQTT_Yield(client, 20);
        if (result != UNITEDRHINO_RET_SUCCESS && result != UNITEDRHINO_RET_MQTT_RECONNECTED) {
            s_subscribe_packet_id = 0;
            return result;
        }
        if (s_subscribe_result != 1) {
            s_subscribe_packet_id = 0;
            return s_subscribe_result;
        }
    }
    s_subscribe_packet_id = 0;
    return UNITEDRHINO_ERR_FAILURE;
}

/* ur_subscribe_down_topics 订阅物模型与当前平台 OTA 主题。 */
static int ur_subscribe_down_topics(void *client)
{
#if CONFIG_UR_AI_SIGNALING
    /* 每次新建连接（含重连/自愈重建）都从空缓存重建 broker 订阅事实：
     * 旧连接上的订阅不能证明新连接仍然有效。 */
    ur_signaling_sub_cache_clear();
#endif
    int result = ur_subscribe_topic(client, "$thing/down/property", ur_property_message_cb);
    if (result == UNITEDRHINO_RET_SUCCESS) {
        result = ur_subscribe_topic(client, "$thing/down/action", ur_property_message_cb);
    }
#if CONFIG_UR_AI
    if (result == UNITEDRHINO_RET_SUCCESS) {
        result = ur_subscribe_topic(client, "$thing/down/ai", ur_ai_message_cb);
    }
#endif
#if CONFIG_UR_AI_SIGNALING
    /* 统一信令与内容下行主题；订阅失败直接判连接失败，避免半就绪状态。 */
    if (result == UNITEDRHINO_RET_SUCCESS) {
        char topic[UR_TOPIC_BUF_LEN] = {0};
        HAL_Snprintf(topic, sizeof(topic), "$thing/down/signaling/%s/%s", s_product_id,
                     s_device_name);
        result = ur_signaling_subscribe_blocking(topic);
    }
    if (result == UNITEDRHINO_RET_SUCCESS) {
        char topic[UR_TOPIC_BUF_LEN] = {0};
        HAL_Snprintf(topic, sizeof(topic), "$thing/down/message/%s/%s", s_product_id,
                     s_device_name);
        result = ur_signaling_subscribe_blocking(topic);
    }
#endif
#if CONFIG_UR_PLATFORM_OTA
    if (result == UNITEDRHINO_RET_SUCCESS) {
        result = ur_subscribe_topic(client, "$ota/down/upgrade", ur_ota_message_cb);
    }
#endif
    return result;
}

/* ur_mqtt_event_handler 只记录传输状态；重连后的订阅恢复由 MQTT 任务串行执行。 */
static void ur_mqtt_event_handler(void *client, void *context, MQTTEventMsg *message)
{
    (void)client;
    (void)context;
    /* SDK 全局订阅事件携带 packet ID；旧包、其他订阅及已结束等待不能确认当前请求。 */
    if (s_subscribe_packet_id > 0 && s_subscribe_result == 1 &&
        (uintptr_t)message->msg == (uintptr_t)s_subscribe_packet_id) {
        if (message->event_type == MQTT_EVENT_SUBCRIBE_SUCCESS) {
            s_subscribe_result = UNITEDRHINO_RET_SUCCESS;
        } else if (message->event_type == MQTT_EVENT_SUBCRIBE_NACK ||
                   message->event_type == MQTT_EVENT_SUBCRIBE_TIMEOUT) {
            s_subscribe_result = UNITEDRHINO_ERR_FAILURE;
        }
    }
    if (message->event_type == MQTT_EVENT_RECONNECT) {
        /* clean_session=1 不保留 broker 订阅；先保持不可用，等 Yield 返回后重订阅。 */
        s_connected = false;
        s_resubscribe_pending = true;
        ESP_LOGI(UR_TAG, "MQTT transport reconnected; refreshing subscriptions");
    } else if (message->event_type == MQTT_EVENT_DISCONNECT) {
        s_subscribe_result = UNITEDRHINO_ERR_FAILURE;
        s_connected = false;
        ur_network_disconnected();
        ur_bridge_notify_disconnected();
        s_resubscribe_pending = false;
        ESP_LOGI(UR_TAG, "MQTT disconnected");
#if CONFIG_UR_AI
        ur_notify_ai_connection(false);
#endif
#if CONFIG_UR_AI_SIGNALING
        ur_signaling_push_event(UR_SIGNALING_EVENT_DISCONNECTED, 0, 0, NULL, NULL, 0);
#endif
    }
#if CONFIG_UR_AI_SIGNALING
    else if (message->event_type == MQTT_EVENT_PUBLISH_SUCCESS) {
        ur_signaling_publish_ack((uintptr_t)message->msg, true);
    } else if (message->event_type == MQTT_EVENT_PUBLISH_TIMEOUT ||
               message->event_type == MQTT_EVENT_PUBLISH_NACK) {
        ur_signaling_publish_ack((uintptr_t)message->msg, false);
    }
#endif
}

/* ur_setup_connect_params 使用任务生命周期内的 DeviceInfo，避免悬空连接参数。 */
static void ur_setup_connect_params(MQTTInitParams *params, DeviceInfo *device_info)
{
    HAL_GetDevInfo(device_info);
    strncpy(s_product_id, device_info->product_id, MAX_SIZE_OF_PRODUCT_ID);
    strncpy(s_device_name, device_info->device_name, MAX_SIZE_OF_DEVICE_NAME);
    params->region = device_info->region;
    params->device_name = device_info->device_name;
    params->product_id = device_info->product_id;
    params->device_secret = device_info->device_secret;
    params->command_timeout = UNITEDRHINO_IOT_MQTT_COMMAND_TIMEOUT;
    params->keep_alive_interval_ms = UNITEDRHINO_IOT_MQTT_KEEP_ALIVE_INTERNAL;
    params->auto_connect_enable = 1;
    params->event_handle.h_fp = ur_mqtt_event_handler;
    params->event_handle.context = NULL;
}

/* ur_reply_bind_change 确认当前平台的 bindChange 通知。 */
static void ur_reply_bind_change(const char *token)
{
    char message[256] = {0};
    int length = HAL_Snprintf(
        message, sizeof(message),
        "{\"method\":\"bindChange\",\"msgToken\":\"%s\",\"code\":200,"
        "\"msg\":\"success\",\"data\":{}}",
        token != NULL ? token : "");
    if (length > 0 && length < (int)sizeof(message)) {
        ur_publish_topic(s_mqtt_client, "$thing/up/service", message, length);
    }
}

/* ur_unbind_callback 兼容旧解绑方法和当前 bindChange，未分类时清凭据并重启。 */
static void ur_unbind_callback(void *context, const char *message, uint32_t length)
{
    (void)context;
    cJSON *root = cJSON_ParseWithLength(message, length);
    if (root == NULL) {
        ESP_LOGW(UR_TAG, "ignore malformed cloud unbind message");
        return;
    }
    const char *method = ur_json_string(root, "method", NULL);

    if (method != NULL && strcmp(method, "bindChange") == 0) {
        cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
        cJSON *project = cJSON_IsObject(params)
                             ? cJSON_GetObjectItemCaseSensitive(params, "projectID")
                             : NULL;
        const char *token = ur_json_string(root, "msgToken", "clientToken");
        long long project_id = cJSON_IsString(project) ? atoll(project->valuestring)
                                                       : (cJSON_IsNumber(project) ? project->valuedouble : -1);
        ur_reply_bind_change(token);
        if (project_id != 2) {
            ESP_LOGI(UR_TAG, "binding changed to project %lld", project_id);
            cJSON_Delete(root);
            return;
        }
    }

    ESP_LOGI(UR_TAG, "cloud unbind received");
    cJSON_Delete(root);
    HAL_SleepMs(1000);
    ur_bridge_clear_device_info();
    esp_restart();
}

/* ur_is_auth_error 仅识别 broker 明确返回的两类鉴权错误。 */
static bool ur_is_auth_error(int error)
{
    return error == UNITEDRHINO_ERR_MQTT_CONNACK_BAD_USERDATA ||
           error == UNITEDRHINO_ERR_MQTT_CONNACK_NOT_AUTHORIZED;
}

/* ur_wait_time_sync 在有限预算内等待实际 SNTP 同步，避免看似有效的旧时间
 * 让短有效期的来电被错误判为过期。同步失败后仍继续连接，不放宽过期校验。 */
static void ur_wait_time_sync(void)
{
    /* 单一公共池失联时不能在本次启动窗口内恢复；同一总预算内轮换来源，
     * 每次只配置一个槽位，兼容SNTP最大服务器数为1的既有SDK配置。 */
    static const char *const servers[] = {
        "pool.ntp.org", "ntp.aliyun.com", "time.cloudflare.com"
    };
    const size_t server_count = sizeof(servers) / sizeof(servers[0]);  // 固定候选数，不从网络接受地址。
    const int slice = UR_TIME_SYNC_TIMEOUT_SECONDS / (int)server_count;  // 不延长原启动等待上限。
    const int switch_seconds = slice > 0 ? slice : 1;  // 较小测试预算仍可按秒推进。
    size_t next_server = 0;  // 只前进，不在总预算外继续轮转。
    bool sntp_started = false;  // 未初始化时不能等待；初始化失败后可尝试下一个来源。
    bool sntp_owned = false;    // 不能关闭其他模块已初始化的 SNTP。
    bool synchronized = false;
    for (int seconds = 0; seconds < UR_TIME_SYNC_TIMEOUT_SECONDS; ++seconds) {
        if (seconds % switch_seconds == 0 && next_server < server_count &&
            (!sntp_started || sntp_owned)) {
            if (sntp_owned) {
                esp_netif_sntp_deinit();
            }
            const char *server = servers[next_server++];  // 静态地址保持至SNTP驱动下一次关闭。
            esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(server);
            esp_err_t error = esp_netif_sntp_init(&config);
            sntp_started = error == ESP_OK || error == ESP_ERR_INVALID_STATE;
            sntp_owned = error == ESP_OK;
            if (!sntp_started) {
                ESP_LOGW(UR_TAG, "initialize SNTP failed: %s", esp_err_to_name(error));
            } else if (sntp_owned) {
                ESP_LOGI(UR_TAG, "SNTP source %u/%u started", (unsigned)next_server,
                         (unsigned)server_count);
            } else {
                ESP_LOGI(UR_TAG, "waiting for existing SNTP service");
            }
        }
        if (sntp_started) {
            const esp_err_t sync_result = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(1000));
            if (sync_result == ESP_OK) {
                synchronized = true;
                break;
            }
            // 共享服务可能没有等待信号量；立即报错时也要按原节拍退避。
            if (sync_result != ESP_ERR_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    if (synchronized) {
        ESP_LOGI(UR_TAG, "system time synchronized");
    } else {
        ESP_LOGW(UR_TAG, "system time not confirmed after %d seconds; continue connection",
                 UR_TIME_SYNC_TIMEOUT_SECONDS);
    }
}

/* ur_ensure_registered 在密钥为空时执行动态注册并按封顶退避重试。 */
static void ur_ensure_registered(DeviceInfo *device_info, size_t *retry_index)
{
    HAL_GetDevInfo(device_info);
    while (device_info->device_secret[0] == '\0') {
        /* 换网验证绝不动态注册；原密钥缺失只能结束事务并恢复旧网。 */
        if (ur_network_protect_credentials()) {
            if (ur_network_fail(UR_NETWORK_ERROR_AUTH) == UR_NETWORK_ROLLBACK) esp_restart();
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        ESP_LOGI(UR_TAG, "DeviceSecret is empty, start dynamic registration");
        if (IOT_DynReg_Device(device_info) == UNITEDRHINO_RET_SUCCESS &&
            HAL_SetDevInfo(device_info) == UNITEDRHINO_RET_SUCCESS) {
            ESP_LOGI(UR_TAG, "dynamic registration succeeded");
            *retry_index = 0;
            return;
        }
        uint32_t delay = ur_next_retry_delay(retry_index);
        ESP_LOGE(UR_TAG, "dynamic registration failed, retry in %u seconds", (unsigned)(delay / 1000));
        vTaskDelay(pdMS_TO_TICKS(delay));
        HAL_GetDevInfo(device_info);
    }
}

/* ur_poll_ota_status 将 worker 快照转换为平台进度，成功上报 100 后重启。 */
static void ur_poll_ota_status(int *last_state, int *last_progress)
{
#if CONFIG_UR_PLATFORM_OTA
    if (s_ota_msg_token[0] == '\0') {
        return;
    }
    ur_ota_status_t status = {0};
    ur_bridge_get_ota_status(&status);
    if (status.state == 1 && status.progress != *last_progress) {
        *last_progress = status.progress;
        ur_report_ota_progress(status.progress, "downloading");
    } else if (status.state == 3 && status.state != *last_state) {
        ur_report_ota_progress(status.error_code, "upgrade failed");
        s_ota_msg_token[0] = '\0';
    } else if (status.state == 2 && status.state != *last_state) {
        ur_report_ota_progress(100, "upgrade complete");
        *last_state = status.state;
        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_restart();
    }
    *last_state = status.state;
#else
    (void)last_state;
    (void)last_progress;
#endif
}

/* ur_destroy_mqtt_client 与相机 worker 的认证复制互斥，避免读取已释放客户端。 */
static void ur_destroy_mqtt_client(void)
{
    if (s_mqtt_client_mutex != NULL &&
        xSemaphoreTake(s_mqtt_client_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        IOT_MQTT_Destroy(&s_mqtt_client);
        xSemaphoreGive(s_mqtt_client_mutex);
    } else if (s_mqtt_client != NULL) {
        ESP_LOGE(UR_TAG, "failed to lock MQTT client for destroy");
    }
}

/* ur_mqtt_yield_is_transient 判定 IOT_MQTT_Yield 返回值是否属于"读包级瞬态失败"：
 * - -604/-605（读写超时）、-1001（generic failure：包内读取失败、ACK 解析失败）、
 * -110（读包异常）都可能在 UDP 洪流饿死 lwIP TCP 时出现，不代表连接已断；SDK 侧已改为
 *   跨调用续读（包内超时保留已消费字节），因此重试不会再把 MQTT 流读错位。
 * - -123（UNITEDRHINO_ERR_MQTT_RX_STREAM_DESYNC）表示"包边界已不可信"：刻意不列入，
 *   直接走下面的连接级重建（重连即复位读包状态），绝不带错位继续解析。
 * 连接级错误（-103/-606/-607/-608）同样不列入。 */
static bool ur_mqtt_yield_is_transient(int result)
{
    return result == UNITEDRHINO_ERR_TCP_READ_TIMEOUT ||
           result == UNITEDRHINO_ERR_TCP_WRITE_TIMEOUT ||
           result == UNITEDRHINO_ERR_FAILURE ||
           result == UNITEDRHINO_ERR_MQTT_PACKET_READ;
}

/* ur_device_task 持续维护连接；只有连续三次明确鉴权失败才清 DeviceSecret。 */
static void ur_device_task(void *argument)
{
    (void)argument;
    IOT_Log_Set_Level(eLOG_INFO);
    size_t retry_index = 0;
    int auth_failures = 0;

    s_property_queue = xQueueCreate(UR_PROPERTY_QUEUE_DEPTH, sizeof(ur_property_item_t));
    if (s_property_queue == NULL) {
        ESP_LOGE(UR_TAG, "failed to create property queue");
    }
#if CONFIG_UR_AI
    s_ai_publish_queue = xQueueCreate(UR_AI_QUEUE_DEPTH, sizeof(ur_ai_publish_item_t));
    if (s_ai_publish_queue == NULL) {
        ESP_LOGE(UR_TAG, "failed to create AI publish queue");
    }
    /* 行为和拍照回执只在任务上下文访问，放入 PSRAM，避免常驻报文挤占
     * UDP 建连及 SSCMA SPI 所需的内部 DMA 连续块。 */
    s_action_queue = xQueueCreateWithCaps(UR_ACTION_QUEUE_DEPTH, sizeof(ur_action_item_t),
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_photo_result_queue =
        xQueueCreateWithCaps(UR_PHOTO_RESULT_QUEUE_DEPTH, sizeof(ur_photo_result_item_t),
                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_action_queue == NULL || s_photo_result_queue == NULL) {
        ESP_LOGE(UR_TAG, "failed to create action queues");
    }
#endif
#if CONFIG_UR_AI_SIGNALING
    s_signaling_event_queue = xQueueCreateWithCaps(UR_SIGNALING_EVENT_DEPTH,
                                                   sizeof(ur_signaling_event_item_t),
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_signaling_cmd_queue = xQueueCreateWithCaps(UR_SIGNALING_CMD_DEPTH,
                                                 sizeof(ur_signaling_cmd_item_t),
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_signaling_event_queue == NULL || s_signaling_cmd_queue == NULL) {
        ESP_LOGE(UR_TAG, "failed to create signaling queues");
    }
#endif
    s_mqtt_client_mutex = xSemaphoreCreateMutex();
    if (s_mqtt_client_mutex == NULL) {
        ESP_LOGE(UR_TAG, "failed to create MQTT client mutex");
        vTaskDelete(NULL);
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(3000));
    ur_wait_time_sync();

    while (true) {
        DeviceInfo device_info = {0};
        MQTTInitParams params = DEFAULT_MQTTINIT_PARAMS;
        ur_ensure_registered(&device_info, &retry_index);
        ur_setup_connect_params(&params, &device_info);

        s_mqtt_client = IOT_MQTT_Construct(&params);
        if (s_mqtt_client == NULL) {
            int error = IOT_MQTT_GetErrCode();
            auth_failures = ur_is_auth_error(error) ? auth_failures + 1 : 0;
            ESP_LOGE(UR_TAG, "MQTT connect failed: %d (auth=%d/%d)", error, auth_failures,
                     UR_AUTH_FAILURE_LIMIT);
            if (ur_network_protect_credentials()) {
                if (ur_is_auth_error(error) &&
                    ur_network_fail(UR_NETWORK_ERROR_AUTH) == UR_NETWORK_ROLLBACK) esp_restart();
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            if (auth_failures >= UR_AUTH_FAILURE_LIMIT) {
                if (ur_bridge_clear_device_secret()) {
                    ESP_LOGI(UR_TAG, "invalid DeviceSecret cleared; WiFi preserved");
                }
                auth_failures = 0;
                retry_index = 0;
                continue;
            }
            uint32_t delay = ur_next_retry_delay(&retry_index);
            vTaskDelay(pdMS_TO_TICKS(delay));
            continue;
        }

        auth_failures = 0;
        retry_index = 0;
        ESP_LOGI(UR_TAG, "UnitedRhino MQTT transport connected, pid=%s dev=%s", s_product_id,
                 s_device_name);
        if (ur_subscribe_down_topics(s_mqtt_client) != UNITEDRHINO_RET_SUCCESS) {
            ESP_LOGE(UR_TAG, "downlink subscription failed");
            ur_destroy_mqtt_client();
            uint32_t delay = ur_next_retry_delay(&retry_index);
            vTaskDelay(pdMS_TO_TICKS(delay));
            continue;
        }
        IOT_Unbind_Device_ByCloud(s_mqtt_client, ur_unbind_callback, NULL);
        s_connected = true;
#if CONFIG_UR_AI
        ur_notify_ai_connection(true);
#endif
#if CONFIG_UR_AI_SIGNALING
        ur_signaling_push_event(UR_SIGNALING_EVENT_CONNECTED, 0, 0, NULL, NULL, 0);
#endif
        ESP_LOGI(UR_TAG, "UnitedRhino MQTT subscriptions ready");

        s_report_requested = true;
        s_report_info_requested = true;
        int last_report = HAL_GetTimeMs();
        int last_ota_state = 0;
        int last_ota_progress = 0;
        int final_error = UNITEDRHINO_RET_SUCCESS;
        /* 主动修复连接绑定不属于网络失败；完成清理后立即重建，不消耗故障退避。 */
        bool reconnect_requested = false;
        int transient_tcp_errors = 0;
        /* 本轮连接内首次瞬态错误时刻；0 表示尚未进入瞬态重试，成功或重建后清零重新起算。 */
        uint32_t transient_since_ms = 0;

        while (true) {
            if (s_force_mqtt_reconnect) {
                /* 自愈重连：断开当前 TCP 走外层循环重建连接与订阅，恢复平台侧绑定。 */
                s_force_mqtt_reconnect = false;
                s_connected = false;
                ur_network_disconnected();
                ESP_LOGW(UR_TAG, "MQTT force reconnect: rebuilding connection");
                reconnect_requested = true;
                final_error = UNITEDRHINO_ERR_MQTT_ATTEMPTING_RECONNECT;
                break;
            }
            int result = IOT_MQTT_Yield(s_mqtt_client, UR_MQTT_YIELD_MS);
            if (result == UNITEDRHINO_ERR_MQTT_ATTEMPTING_RECONNECT) {
                /* SDK 已在重建连接：本次瞬态预算随之结束，不让重建前的饥饿累计到新连接。 */
                transient_tcp_errors = 0;
                transient_since_ms = 0;
                s_connected = false;
                ur_network_disconnected();
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            if (ur_mqtt_yield_is_transient(result)) {
                /* WebRTC 媒体负载下 lwip TCP 事件会被 UDP 洪流瞬时饿死，读/写超时与
                 * 读包级失败（含包内读取失败 / ACK 解析失败的 -1001）多为可自愈瞬断；
                 * 旧固件只用 500ms 窗口时同样的饥饿被更长的包内读取预算吸收，从不到达
                 * 这里。预算内保持 200ms 短退避重试，超预算才走完整重建（重连即复位
                 * 读包状态与字节流）。 */
                ++transient_tcp_errors;
                uint32_t now_ms = HAL_GetTimeMs();
                if (transient_since_ms == 0) {
                    transient_since_ms = now_ms;
                }
                if (now_ms - transient_since_ms < UR_MQTT_TRANSIENT_BUDGET_MS) {
                    ESP_LOGW(UR_TAG, "MQTT yield transient error %d (%d, %ums/%dms), retrying",
                             result, transient_tcp_errors, (unsigned)(now_ms - transient_since_ms),
                             UR_MQTT_TRANSIENT_BUDGET_MS);
                    vTaskDelay(pdMS_TO_TICKS(200));
                    continue;
                }
                ESP_LOGW(UR_TAG, "MQTT yield transient budget exhausted: %d (%d, %ums)", result,
                         transient_tcp_errors, (unsigned)(now_ms - transient_since_ms));
            }
            if (result != UNITEDRHINO_RET_SUCCESS && result != UNITEDRHINO_RET_MQTT_RECONNECTED) {
                final_error = result;
                ESP_LOGE(UR_TAG, "MQTT yield failed: %d", result);
                break;
            }
            transient_tcp_errors = 0;
            transient_since_ms = 0; /* 读包成功：本次瞬态累计（计数与起算时刻）结束。 */
            if (s_resubscribe_pending || result == UNITEDRHINO_RET_MQTT_RECONNECTED) {
                /* SDK 默认 clean_session=1，重连后必须恢复全部下行与解绑订阅，成功后
                 * 才允许 AI 协议开启新 session。 */
                s_resubscribe_pending = false;
                if (ur_subscribe_down_topics(s_mqtt_client) != UNITEDRHINO_RET_SUCCESS) {
                    final_error = UNITEDRHINO_ERR_FAILURE;
                    ESP_LOGE(UR_TAG, "downlink resubscription failed after reconnect");
                    break;
                }
                IOT_Unbind_Device_ByCloud(s_mqtt_client, ur_unbind_callback, NULL);
                s_connected = true;
                s_report_requested = true;
                s_report_info_requested = true;
#if CONFIG_UR_AI
                ur_notify_ai_connection(true);
#endif
#if CONFIG_UR_AI_SIGNALING
                ur_signaling_push_event(UR_SIGNALING_EVENT_CONNECTED, 0, 0, NULL, NULL, 0);
#endif
                ESP_LOGI(UR_TAG, "MQTT subscriptions restored");
            }

            if (s_report_info_requested) {
                s_report_info_requested = false;
                ur_report_device_info();
            }
            ur_drain_property_queue();
#if CONFIG_UR_AI
            ur_drain_ai_publish_queue();
            ur_drain_action_queue();
            ur_drain_photo_results();
#endif
#if CONFIG_UR_AI_SIGNALING
            ur_drain_signaling_cmds();
#endif
            int now = HAL_GetTimeMs();
            if (atomic_exchange(&s_report_requested, false) || now - last_report >= UR_REPORT_INTERVAL_MS) {
                ur_report_all_properties();
                last_report = now;
            }
            ur_poll_ota_status(&last_ota_state, &last_ota_progress);
        }

        s_connected = false;
        ur_network_disconnected();
        ur_clear_property_queue();
#if CONFIG_UR_AI
        ur_notify_ai_connection(false);
        if (s_ai_publish_queue != NULL) {
            ur_clear_ai_publish_queue();
        }
        ur_clear_action_queues();
#endif
#if CONFIG_UR_AI_SIGNALING
        {
            ur_signaling_event_item_t stale;
            if (s_signaling_event_queue != NULL) {
                while (xQueueReceive(s_signaling_event_queue, &stale, 0) == pdTRUE) {
                    if (stale.payload != NULL) {
                        heap_caps_free(stale.payload);
                    }
                }
            }
            if (s_signaling_cmd_queue != NULL) {
                ur_signaling_cmd_item_t stale_cmd;
                while (xQueueReceive(s_signaling_cmd_queue, &stale_cmd, 0) == pdTRUE) {
                    if (stale_cmd.payload != NULL) {
                        heap_caps_free(stale_cmd.payload);
                    }
                }
            }
            ur_signaling_push_event(UR_SIGNALING_EVENT_DISCONNECTED, 0, 0, NULL, NULL, 0);
        }
#endif
        ur_destroy_mqtt_client();
        auth_failures = ur_is_auth_error(final_error) ? auth_failures + 1 : 0;
        if (ur_network_protect_credentials()) {
            if (ur_is_auth_error(final_error) &&
                ur_network_fail(UR_NETWORK_ERROR_AUTH) == UR_NETWORK_ROLLBACK) esp_restart();
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (auth_failures >= UR_AUTH_FAILURE_LIMIT) {
            ur_bridge_clear_device_secret();
            auth_failures = 0;
            retry_index = 0;
            continue;
        }
        if (reconnect_requested) {
            continue;
        }
        uint32_t delay = ur_next_retry_delay(&retry_index);
        vTaskDelay(pdMS_TO_TICKS(delay));
    }
}

void ur_device_task_start(void)
{
    if (s_task_started) {
        return;
    }
    s_task_started = true;
    if (xTaskCreate(ur_device_task, "ur_device", 12288, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(UR_TAG, "failed to create device task");
        s_task_started = false;
    }
}
