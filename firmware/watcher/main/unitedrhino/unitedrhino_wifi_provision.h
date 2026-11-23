/* 联犀 C SDK Wi-Fi 配网公开类型；供 SDK 配网实现与 Watcher LLSync 适配层共享。 */
#ifndef UNITEDRHINO_WIFI_PROVISION_H
#define UNITEDRHINO_WIFI_PROVISION_H

#ifdef __cplusplus
extern "C" {
#endif

/* eWiFiConfigType 的顺序与 SDK 内部方法表严格一致。 */
typedef enum {
    WIFI_CONFIG_TYPE_SOFT_AP = 0,
    WIFI_CONFIG_TYPE_SMART_CONFIG = 1,
    WIFI_CONFIG_TYPE_AIRKISS = 2,
    WIFI_CONFIG_TYPE_SIMPLE_CONFIG = 3,
    WIFI_CONFIG_TYPE_BT_COMBO = 4,
} eWiFiConfigType;

/* eWiFiConfigState 表示单个配网传输的连接进度。 */
typedef enum {
    WIFI_CONFIG_ING = 0,
    WIFI_CONFIG_SUCCESS = 1,
    WIFI_CONFIG_FAIL = 2,
} eWiFiConfigState;

/* eWiFiConfigResult 是完成 Wi-Fi 与设备绑定后的最终结果。 */
typedef enum {
    RESULT_WIFI_CONFIG_SUCCESS = 0,
    RESULT_WIFI_CONFIG_FAILED = 1,
    RESULT_WIFI_CONFIG_TIMEOUT = 2,
} eWiFiConfigResult;

/* WifiConfigResultCallBack 返回最终结果；user_data 由具体传输实现提供。 */
typedef void (*WifiConfigResultCallBack)(eWiFiConfigResult result, void *user_data);

/* 配网入口的返回值；成功必须保持为零以符合 SDK 通用返回约定。 */
enum {
    RET_WIFI_CONFIG_START_SUCCESS = 0,
    ERR_UNKNOWN_WIFI_CONFIG_TYPE = -1,
    ERR_UNSUPPORTED_WIFI_CONFIG_TYPE = -2,
    ERR_WIFI_CONFIG_START_FAILED = -3,
    ERR_COMM_SERVICE_START_FAILED = -4,
};

/* qiot_wifi_config_start 启动指定配网传输并在绑定完成后回调结果。 */
int qiot_wifi_config_start(eWiFiConfigType type, void *params,
                           WifiConfigResultCallBack result_callback);
/* qiot_wifi_config_stop 停止当前配网传输与辅助通信服务。 */
void qiot_wifi_config_stop(void);
/* set_bt_combo_config_state 由 Watcher Wi-Fi 适配层更新 BT Combo 联网状态。 */
void set_bt_combo_config_state(eWiFiConfigState state);

#ifdef __cplusplus
}
#endif

#endif  // UNITEDRHINO_WIFI_PROVISION_H
