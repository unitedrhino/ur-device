/*
 * Copyright (c) 2020 Tencent Cloud. All rights reserved.

 * Licensed under the MIT License (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://opensource.org/licenses/MIT

 * Unless required by applicable law or agreed to in writing, software distributed under the License is
 * distributed on an "AS IS" basis, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
 * either express or implied. See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

/*
 * Copyright (c) 2020 UnitedRhino Cloud. All rights reserved.

 * Licensed under the MIT License (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://opensource.org/licenses/MIT

 * Unless required by applicable law or agreed to in writing, software distributed under the License is
 * distributed on an "AS IS" basis, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
 * either express or implied. See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#ifndef __UNITEDRHINO_IOT_WIFI_CONFIG_H__
#define __UNITEDRHINO_IOT_WIFI_CONFIG_H__

#include "sdkconfig.h"
#include "unitedrhino_wifi_provision.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_ERR_LOG_POST 1
#define WIFI_LOG_UPLOAD   1

/* Watcher 启用 LLSync 时只编译 BT Combo；其他固件继续保留 SDK SoftAP 兼容入口。 */
#if CONFIG_UR_BLE_PROVISION
#define WIFI_PROV_BT_COMBO_CONFIG_ENABLE 1  ///< wifi provisioning method: bt combo config, need Wechat Applets
#else
#define WIFI_PROV_SOFT_AP_ENABLE         1  ///< wifi provisioning method: device AP, need Wechat Applets
#endif
//#define WIFI_PROV_SMART_CONFIG_ENABLE    1  ///< wifi provisioning method: smart config, need Wechat Applets
//#define WIFI_PROV_AIRKISS_CONFIG_ENABLE  1  ///< wifi provisioning method: airkiss, need Wechat Applets
//#define WIFI_PROV_SIMPLE_CONFIG_ENABLE   1  ///< wifi provisioning method: simple config, need Wechat Applets

#define MAX_SSID_LEN 32  // max ssid len
#define MAX_PSK_LEN  65  // max psk len

typedef enum {
    EVENT_WIFI_CONFIG_SUCCESS = 0, /* WiFi config success */
    EVENT_WIFI_CONFIG_FAILED  = 1, /* WiFi config failed */
    EVENT_WIFI_CONFIG_TIMEOUT = 2, /* WiFi config timeout */
} eWiFiConfigEvent;

typedef void (*WifiConfigEventCallBack)(eWiFiConfigEvent event, void *usr_data);

/**
 * @brief Send wifi config log to mini program
 */
int qiot_wifi_config_send_log(void);

/**
 * @brief set the ap ssid and psk fetched by wifi config
 */
void qiot_wifi_ap_info_set(const uint8_t *ssid, const uint8_t *psw);

/**
 * @brief get the ap ssid and psk fetched by wifi config
 */
void qiot_wifi_ap_info_get(uint8_t *ssid, int ssid_buff_len, uint8_t *psk, int psk_buff_len);

/**
 * @brief set device token and notify to bind device
 *
 * @param token from wechat applet
 *
 */
void qiot_device_bind_set_token(const char *token);

/**
 * @brief get bind device cloud reply code
 *
 * @param bind_reply_code 0 is success other is fail, send it to wechat applet
 *
 * @return recv cloud platform reply device bind msg is true, other return false happen error
 */
bool qiot_device_bind_get_cloud_reply_code(int *bind_reply_code);

/**
 * @brief device start to connect ap, include switch mode to sta, disconnect ap, connect ap
 *
 * @param ssid connect ap ssid
 * @param psw  connect ap ssid->password
 * @param channel connect ap in channel
 *
 * @return 0 is fun proc success, other fun proc failed
 */
int HAL_Wifi_StaConnect(const char *ssid, const char *psw, uint8_t channel);

/**
 * @brief get station connect ap result
 *
 * @param void
 *
 * @return true is get ip; other is false
 */
bool HAL_Wifi_IsConnected(void);

/**
 * @brief get curr device local ipv4 ip
 *
 * @param ipv4_buf, save ipv4 addr
 * @param ipv4_buf_len, ipv4_buf max length
 *
 * @return UNITEDRHINO_ERR_FAILURE when invalid ip, other return UNITEDRHINO_RET_SUCCESS
 */
int HAL_Wifi_GetLocalIP(char *ipv4_buf, int ipv4_buf_len);

/**
 * @brief get curr connected ap ssid
 *
 * @param ssid_buf, save ssid
 * @param ssid_buf_len, ssid_buf max length
 *
 * @return UNITEDRHINO_ERR_FAILURE when invalid ssid or not connected ap, other return ssid length
 */
int HAL_Wifi_GetAP_SSID(char *ssid_buf, int ssid_buf_len);

/**
 * @brief get curr connected ap password of ssid
 *
 * @param pwd_buf, save password of ssid
 * @param pwd_buf_len, pwd_buf max length
 *
 * @return UNITEDRHINO_ERR_FAILURE when invalid password or not connected ap, other return password length
 */
int HAL_Wifi_GetAP_PWD(char *pwd_buf, int pwd_buf_len);

typedef struct {
    const char *ssid;
    const char *psw;
    uint8_t     ch;
} WiFiConfigParams;

/**
 * @brief Start softAP provision
 *
 * @param params See WiFiConfigParams
 * @event_cb wifi config event callback, success to bind device
 *
 * @return 0 when success, or err code for failure
 */
int HAL_SoftApProvision_Start(void *params, WifiConfigEventCallBack event_cb);

/**
 * @brief Stop softAp WiFi provision and device binding process
 */
int HAL_SoftApProvision_Stop(void);

/**
 * @brief start device softap
 * @param ssid  softap ssid
 * @param psw   softap pws
 * @param ch    softap channel
 *
 * @return 0 when success, or err code for failuer
 */
int start_device_softAP(const char *ssid, const char *psw, uint8_t ch);

/**
 * @brief start device softap
 *
 * @return 0 when success, or err code for failuer
 */
int stop_device_softAP(void);

/**
 * @brief Start airkiss
 *
 * @param params is NULL
 * @event_cb wifi config event callback, success to bind device
 *
 * @return 0 when success, or err code for failure
 */
int HAL_AirkissConfig_Start(void *params, WifiConfigEventCallBack event_cb);
/**
 * @brief Stop airkiss and device binding process
 */
int HAL_AirkissConfig_Stop(void);

/**
 * @brief set airkiss config state
 *
 * @param eWiFiConfigStatee, ing,succ,fail
 *
 * @return null
 */
void set_airkiss_config_state(eWiFiConfigState state);

/**
 * @brief Start smart config
 *
 * @param params is NULL
 * @event_cb wifi config event callback, success to bind device
 *
 * @return 0 when success, or err code for failure
 */
int HAL_SmartConfig_Start(void *params, WifiConfigEventCallBack event_cb);
/**
 * @brief Stop SmartConfig and device binding process
 */
int HAL_SmartConfig_Stop(void);

/**
 * @brief set smart config state
 *
 * @param eWiFiConfigState ing,succ,fail
 *
 * @return null
 */
void set_smart_config_state(eWiFiConfigState state);

/**
 * @brief Start simple config
 *
 * @param params is NULL
 * @event_cb wifi config event callback, success to bind device
 *
 * @return 0 when success, or err code for failure
 */
int HAL_SimpleConfig_Start(void *params, WifiConfigEventCallBack event_cb);
/**
 * @brief Stop simple config and device binding process
 */
int HAL_SimpleConfig_Stop(void);

/**
 * @brief set simple config state
 *
 * @param eWiFiConfigState ing,succ,fail
 *
 * @return null
 */
void set_simple_config_state(eWiFiConfigState state);

/**
 * @brief Start bt combo config
 *
 * @param params is NULL
 * @event_cb wifi config event callback, success to bind device
 *
 * @return 0 when success, or err code for failure
 */
int HAL_BTComboConfig_Start(void *params, WifiConfigEventCallBack event_cb);
/**
 * @brief Stop bt combo config and device binding process
 */
int HAL_BTComboConfig_Stop(void);

/**
 * @brief set bt combo config state
 *
 * @param eWiFiConfigState ing,succ,fail
 *
 * @return null
 */
/**
 * @brief send ble dev log
 */
int app_send_ble_dev_log(void);

/**
 * @brief send ble error log
 */
int app_send_ble_error_log(void);

/**
 * @brief get wifi config err log from saved
 *
 * @param offset   log read start position
 * @param log  err log get buff
 * @param log_size err log buff size
 *
 * @return 0 when success, or err code for failure
 */
int HAL_Wifi_read_err_log(uint32_t offset, void *log, size_t log_size);

/**
 * @brief save wifi config err log
 *
 * @param offset   log write start position
 * @param log  err log buff
 * @param log_size err log buff size
 *
 * @return 0 when success, or err code for failure
 */
int HAL_Wifi_write_err_log(uint32_t offset, void *log, size_t log_size);

/**
 * @brief clear saved all log
 *
 * @param is NULL
 *
 * @return 0 when success, or err code for failure
 */
int HAL_Wifi_clear_err_log(void);

#ifdef __cplusplus
}
#endif

#endif  //__UNITEDRHINO_WIFI_CONFIG_H__
