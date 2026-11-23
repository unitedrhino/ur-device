/*
 * Tencent is pleased to support the open source community by making IoT Hub
 available.
 * Copyright (C) 2018-2020 Tencent. All rights
 reserved.

 * Licensed under the MIT License (the "License"); you may not use this file
 except in
 * compliance with the License. You may obtain a copy of the License at
 * http://opensource.org/licenses/MIT

 * Unless required by applicable law or agreed to in writing, software
 distributed under the License is
 * distributed on an "AS IS" basis, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 KIND,
 * either express or implied. See the License for the specific language
 governing permissions and
 * limitations under the License.
 *
 */

/*
 * UnitedRhino is pleased to support the open source community by making IoT Hub
 available.
 * Copyright (C) 2018-2020 UnitedRhino. All rights
 reserved.

 * Licensed under the MIT License (the "License"); you may not use this file
 except in
 * compliance with the License. You may obtain a copy of the License at
 * http://opensource.org/licenses/MIT

 * Unless required by applicable law or agreed to in writing, software
 distributed under the License is
 * distributed on an "AS IS" basis, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 KIND,
 * either express or implied. See the License for the specific language
 governing permissions and
 * limitations under the License.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "unitedrhino_iot_export.h"
#include "unitedrhino_iot_import.h"
#include "utils_param_check.h"

/* Enable this macro (also control by cmake) to use static string buffer to
 * store device info */
/* To use specific storing methods like files/flash, disable this macro and
 * implement dedicated methods */
/*
 * 设备信息存储说明：
 * - product_id / product_secret 来自 Kconfig 编译配置（CONFIG_UR_PRODUCT_ID / CONFIG_UR_PRODUCT_SECRET）
 * - device_name 自动取蓝牙 MAC（与 BLE 广播身份一致）
 * - device_secret 首次动态注册获得后持久化 NVS（namespace ur_dev），重启直连无需重复注册
 * - 云端解绑时 ur_bridge_clear_device_info（main/unitedrhino/ur_bridge.cc）通过 xiaozhi Settings
 *   类擦除同一 namespace，两边写入口径一致
 */
#define DEBUG_DEV_INFO_USED

/* 设备密钥的 NVS 存储参数，与 ur_bridge.cc 的 Settings("ur_dev") 保持一致 */
#define UR_DEVINFO_NVS_NAMESPACE "ur_dev"
#define UR_DEVINFO_NVS_KEY_SECRET "secret"

/* 从 NVS 读取设备密钥到 buf，未存储时返回失败（由调用方保持空串以触发动态注册） */
static int ur_nvs_load_secret(char *buf, uint8_t max_len)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(UR_DEVINFO_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return UNITEDRHINO_ERR_FAILURE;
    }
    size_t required = max_len;
    err = nvs_get_str(handle, UR_DEVINFO_NVS_KEY_SECRET, buf, &required);
    nvs_close(handle);
    return (err == ESP_OK) ? UNITEDRHINO_RET_SUCCESS : UNITEDRHINO_ERR_FAILURE;
}

/* 将设备密钥写入 NVS 持久化 */
static int ur_nvs_save_secret(const char *secret)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(UR_DEVINFO_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return UNITEDRHINO_ERR_FAILURE;
    }
    err = nvs_set_str(handle, UR_DEVINFO_NVS_KEY_SECRET, secret);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return (err == ESP_OK) ? UNITEDRHINO_RET_SUCCESS : UNITEDRHINO_ERR_FAILURE;
}

/* 删除已持久化设备密钥；NVS namespace 不存在或键不存在也视为已清理。 */
static int ur_nvs_clear_secret(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(UR_DEVINFO_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return UNITEDRHINO_RET_SUCCESS;
    }
    if (err != ESP_OK) {
        return UNITEDRHINO_ERR_FAILURE;
    }
    err = nvs_erase_key(handle, UR_DEVINFO_NVS_KEY_SECRET);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return (err == ESP_OK) ? UNITEDRHINO_RET_SUCCESS : UNITEDRHINO_ERR_FAILURE;
}

#ifdef DEBUG_DEV_INFO_USED
/* product Id  */
static char sg_product_id[MAX_SIZE_OF_PRODUCT_ID + 1] = CONFIG_UR_PRODUCT_ID;

/* device name */
static char sg_device_name[MAX_SIZE_OF_DEVICE_NAME + 1] = "";
static bool sg_device_name_inited = false;

/* region */
static char sg_region[MAX_SIZE_OF_REGION + 1] = "china";

#ifdef DEV_DYN_REG_ENABLED
/* product secret for device dynamic Registration  */
static char sg_product_secret[MAX_SIZE_OF_PRODUCT_SECRET + 1] = CONFIG_UR_PRODUCT_SECRET;
#endif

#ifdef AUTH_MODE_CERT
/* public cert file name of certificate device */
static char sg_device_cert_file_name[MAX_SIZE_OF_DEVICE_CERT_FILE_NAME + 1] = "YOUR_DEVICE_NAME_cert.crt";
/* private key file name of certificate device */
static char sg_device_privatekey_file_name[MAX_SIZE_OF_DEVICE_SECRET_FILE_NAME + 1] = "YOUR_DEVICE_NAME_private.key";
#else
/* device secret of PSK device：初始为空，动态注册成功后经 HAL_SetDevInfo 持久化 */
static char sg_device_secret[MAX_SIZE_OF_DEVICE_SECRET + 1] = "";
static bool sg_device_secret_loaded = false;
#endif

#ifdef GATEWAY_ENABLED
/* sub-device product id  */
static char sg_sub_device_product_id[MAX_SIZE_OF_PRODUCT_ID + 1] = "PRODUCT_ID";
/* sub-device device name */
static char sg_sub_device_name[MAX_SIZE_OF_DEVICE_NAME + 1] = "YOUR_SUB_DEV_NAME";
#endif

static int device_info_copy(void *pdst, void *psrc, uint8_t max_len)
{
    if (strlen(psrc) > max_len) {
        return UNITEDRHINO_ERR_FAILURE;
    }
    memset(pdst, '\0', max_len);
    strncpy(pdst, psrc, max_len);
    return UNITEDRHINO_RET_SUCCESS;
}

/* 确保设备名来自蓝牙 MAC，平台侧绑定与 BLE 广播看到的是同一个硬件身份。 */
static void ensure_device_name_from_mac(void)
{
    if (sg_device_name_inited) {
        return;
    }

    uint8_t mac[6] = {0};
    esp_err_t ret = esp_read_mac(mac, ESP_MAC_BT);
    if (ret != ESP_OK) {
        ret = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    }

    if (ret == ESP_OK) {
        HAL_Snprintf(sg_device_name, sizeof(sg_device_name), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2],
                     mac[3], mac[4], mac[5]);
    } else {
        strncpy(sg_device_name, "MACUNKNOWN", MAX_SIZE_OF_DEVICE_NAME);
    }

    sg_device_name_inited = true;
}

#endif

int HAL_SetDevInfo(void *pdevInfo)
{
    POINTER_SANITY_CHECK(pdevInfo, UNITEDRHINO_ERR_DEV_INFO);
    int         ret;
    DeviceInfo *devInfo = (DeviceInfo *)pdevInfo;

#ifdef DEBUG_DEV_INFO_USED
    ret = device_info_copy(sg_product_id, devInfo->product_id,
                           MAX_SIZE_OF_PRODUCT_ID);  // set product ID
    ret |= device_info_copy(sg_device_name, devInfo->device_name,
                            MAX_SIZE_OF_DEVICE_NAME);  // set dev name

#ifdef AUTH_MODE_CERT
    ret |= device_info_copy(sg_device_cert_file_name, devInfo->dev_cert_file_name,
                            MAX_SIZE_OF_DEVICE_CERT_FILE_NAME);  // set dev cert file name
    ret |= device_info_copy(sg_device_privatekey_file_name, devInfo->dev_key_file_name,
                            MAX_SIZE_OF_DEVICE_SECRET_FILE_NAME);  // set dev key file name
#else
    ret |= device_info_copy(sg_device_secret, devInfo->device_secret,
                            MAX_SIZE_OF_DEVICE_SECRET);  // set dev secret
    /* 非空密钥持久化；空密钥必须同时清除 NVS 与内存，供鉴权失效自愈。 */
    if (ret == UNITEDRHINO_RET_SUCCESS) {
        ret |= strlen(sg_device_secret) > 0 ? ur_nvs_save_secret(sg_device_secret)
                                            : ur_nvs_clear_secret();
        sg_device_secret_loaded = true;
    }
#endif

#else
    Log_e("HAL_SetDevInfo not implement yet");
    ret = UNITEDRHINO_ERR_DEV_INFO;
#endif

    if (UNITEDRHINO_RET_SUCCESS != ret) {
        Log_e("Set device info err");
        ret = UNITEDRHINO_ERR_DEV_INFO;
    }
    return ret;
}

int HAL_GetDevInfo(void *pdevInfo)
{
    POINTER_SANITY_CHECK(pdevInfo, UNITEDRHINO_ERR_DEV_INFO);
    int         ret;
    DeviceInfo *devInfo = (DeviceInfo *)pdevInfo;
    memset((char *)devInfo, '\0', sizeof(DeviceInfo));

#ifdef DEBUG_DEV_INFO_USED
    ensure_device_name_from_mac();

    ret = device_info_copy(devInfo->product_id, sg_product_id,
                           MAX_SIZE_OF_PRODUCT_ID);  // get product ID
    ret |= device_info_copy(devInfo->device_name, sg_device_name,
                            MAX_SIZE_OF_DEVICE_NAME);  // get dev name
    ret |= device_info_copy(devInfo->region, sg_region,
                            MAX_SIZE_OF_REGION);  // get region

#ifdef DEV_DYN_REG_ENABLED
    ret |= device_info_copy(devInfo->product_secret, sg_product_secret,
                            MAX_SIZE_OF_PRODUCT_SECRET);  // get product ID
#endif

#ifdef AUTH_MODE_CERT
    ret |= device_info_copy(devInfo->dev_cert_file_name, sg_device_cert_file_name,
                            MAX_SIZE_OF_DEVICE_CERT_FILE_NAME);  // get dev cert file name
    ret |= device_info_copy(devInfo->dev_key_file_name, sg_device_privatekey_file_name,
                            MAX_SIZE_OF_DEVICE_SECRET_FILE_NAME);  // get dev key file name
#else
    /* 首次访问时从 NVS 加载已注册的设备密钥（未注册保持空串，MQTT 连接时触发动态注册） */
    if (!sg_device_secret_loaded) {
        sg_device_secret_loaded = true;
        ur_nvs_load_secret(sg_device_secret, MAX_SIZE_OF_DEVICE_SECRET);
    }
    ret |= device_info_copy(devInfo->device_secret, sg_device_secret,
                            MAX_SIZE_OF_DEVICE_SECRET);  // get dev secret
#endif

#else
    Log_e("HAL_GetDevInfo not implement yet");
    ret = UNITEDRHINO_ERR_DEV_INFO;
#endif

    if (UNITEDRHINO_RET_SUCCESS != ret) {
        Log_e("Get device info err");
        ret = UNITEDRHINO_ERR_DEV_INFO;
    }
    return ret;
}

#ifdef GATEWAY_ENABLED
int HAL_GetGwDevInfo(void *pgwDeviceInfo)
{
    POINTER_SANITY_CHECK(pgwDeviceInfo, UNITEDRHINO_ERR_DEV_INFO);
    int                ret;
    GatewayDeviceInfo *gwDevInfo = (GatewayDeviceInfo *)pgwDeviceInfo;
    memset((char *)gwDevInfo, 0, sizeof(GatewayDeviceInfo));

#ifdef DEBUG_DEV_INFO_USED
    ret = HAL_GetDevInfo(&(gwDevInfo->gw_info));  // get gw dev info
    // only one sub-device is supported now
    gwDevInfo->sub_dev_num  = 1;
    gwDevInfo->sub_dev_info = (DeviceInfo *)HAL_Malloc(sizeof(DeviceInfo) * (gwDevInfo->sub_dev_num));
    memset((char *)gwDevInfo->sub_dev_info, '\0', sizeof(DeviceInfo));
    // copy sub dev info
    ret = device_info_copy(gwDevInfo->sub_dev_info->product_id, sg_sub_device_product_id, MAX_SIZE_OF_PRODUCT_ID);
    ret |= device_info_copy(gwDevInfo->sub_dev_info->device_name, sg_sub_device_name, MAX_SIZE_OF_DEVICE_NAME);

#else
    Log_e("HAL_GetDevInfo not implement yet");
    ret = UNITEDRHINO_ERR_DEV_INFO;
#endif

    if (UNITEDRHINO_RET_SUCCESS != ret) {
        Log_e("Get gateway device info err");
        ret = UNITEDRHINO_ERR_DEV_INFO;
    }
    return ret;
}
#endif
