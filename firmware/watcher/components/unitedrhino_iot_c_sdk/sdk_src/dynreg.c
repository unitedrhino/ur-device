/*
 * Tencent is pleased to support the open source community by making IoT Hub
 available.
 * Copyright (C) 2016 Tencent. All rights reserved.

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
 * Copyright (C) 2016 UnitedRhino. All rights reserved.

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

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "lite-utils.h"
#include "mqtt_client.h"
#include "unitedrhino_iot_ca.h"
#include "unitedrhino_iot_common.h"
#include "unitedrhino_iot_device.h"
#include "unitedrhino_iot_export.h"
#include "unitedrhino_iot_import.h"
#include "utils_aes.h"
#include "utils_base64.h"
#include "utils_hmac.h"
#include "utils_httpc.h"

#define REG_URL_MAX_LEN             (128)
#define DYN_REG_SIGN_LEN            (64)
#define DYN_BUFF_DATA_MORE          (10)
#define BASE64_ENCODE_OUT_LEN(x)    (((x + 3) * 4) / 3)
#define DYN_REG_RES_HTTP_TIMEOUT_MS (10000)
#define DYN_REG_RES_MQTT_TIMEOUT_MS (10000)

#ifdef AUTH_MODE_CERT
#define DYN_RESPONSE_BUFF_LEN (5 * 1024)
#define DECODE_BUFF_LEN       (5 * 1024)
#else
#define DYN_RESPONSE_BUFF_LEN (256)
#define DECODE_BUFF_LEN       (256)
#endif

/* Knuth's TAOCP section 3.6 */
#define M ((1U << 31) - 1)
#define A 48271
#define Q 44488  // M/A
#define R 3399   // M%A; R < Q !!!

#define CODE_RESAULT "code"
#define ENCRYPT_TYPE "encryptionType"
#define PSK_DATA     "psk"
#define CERT_DATA    "clientCert"
#define KEY_DATA     "clientKey"

typedef enum {
    eCERT_TYPE = 1,
    ePSK_TYPE  = 2,
} eAuthType;

/*Global value*/
static unsigned int _seed = 1;

typedef struct {
    bool reply_ready;
    char response[DYN_RESPONSE_BUFF_LEN];
} DynRegMqttContext;

int rand_r(unsigned int *seed)
{
    int32_t X;

    X = *seed;
    X = A * (X % Q) - R * (int32_t)(X / Q);
    if (X < 0)
        X += M;

    *seed = X;
    return X;
}

int rand_d(void)
{
    return rand_r(&_seed);
}

void srand_d(unsigned int i)
{
    _seed = i;
}

static int _get_json_result_code(char *json)
{
    int32_t resault = -1;
    char   *v       = LITE_json_value_of(CODE_RESAULT, json);

    if (v == NULL) {
        Log_e("Invalid dynamic registration response JSON");
        return -1;
    }

    if (LITE_get_int32(&resault, v) != UNITEDRHINO_RET_SUCCESS) {
        Log_e("Invalid dynamic registration response JSON");
        HAL_Free(v);
        return -1;
    }

    HAL_Free(v);

    if ((resault == 0) || (resault == 200)) {
        return UNITEDRHINO_RET_SUCCESS;
    }

    return resault;
}

static int _get_json_encry_type(char *json)
{
    int32_t type = -1;
    char   *v    = LITE_json_value_of(ENCRYPT_TYPE, json);

    if (v == NULL) {
        Log_e("Get dynamic registration encryption type failed");
        return -1;
    }

    if (LITE_get_int32(&type, v) != UNITEDRHINO_RET_SUCCESS) {
        Log_e("Invalid dynamic registration response JSON");
        HAL_Free(v);
        return -1;
    }

    HAL_Free(v);

    return type;
}

#ifndef AUTH_MODE_CERT

static char *_get_json_psk(char *json)
{
    char *psk = LITE_json_value_of(PSK_DATA, json);

    if (psk == NULL) {
        Log_e("Get dynamic registration PSK failed");
    }

    return psk;
}

#else
static char *_get_json_cert_data(char *json)
{
    char *cert = LITE_json_value_of(CERT_DATA, json);

    if (cert == NULL) {
        Log_e("Get dynamic registration client certificate failed");
    }

    return cert;
}

static char *_get_json_key_data(char *json)
{
    char *key = LITE_json_value_of(KEY_DATA, json);

    if (key == NULL) {
        Log_e("Get dynamic registration client certificate failed");
    }

    return key;
}

/*\\n in data change to '\n'*/
static void _deal_transfer(char *data, uint32_t dataLen)
{
    int i;

    for (i = 0; i < dataLen; i++) {
        if ((data[i] == '\\') && (data[i + 1] == 'n')) {
            data[i]     = ' ';
            data[i + 1] = '\n';
        }
    }
}

static int _cert_file_save(const char *fileName, char *data, uint32_t dataLen)
{
    void *   fp;
    char     filePath[FILE_PATH_MAX_LEN];
    uint32_t len;
    int      Ret = UNITEDRHINO_ERR_FAILURE;

    memset(filePath, 0, FILE_PATH_MAX_LEN);
    HAL_Snprintf(filePath, FILE_PATH_MAX_LEN, "./certs/%s", STRING_PTR_PRINT_SANITY_CHECK(fileName));

    if ((fp = HAL_FileOpen(filePath, "w+")) == NULL) {
        Log_e("fail to open file %s", STRING_PTR_PRINT_SANITY_CHECK(fileName));
        goto exit;
    }

    _deal_transfer(data, dataLen);
    len = fprintf(fp, "%s", STRING_PTR_PRINT_SANITY_CHECK(data));
    HAL_FileClose(fp);

    if (len == dataLen) {
        Log_d("save %s file succes", STRING_PTR_PRINT_SANITY_CHECK(fileName));
        Ret = UNITEDRHINO_RET_SUCCESS;
    }

exit:
    return Ret;
}

#endif

static int _parse_devinfo(char *jdoc, DeviceInfo *pDevInfo)
{
    int           ret = 0;
    size_t        len;
    int           datalen;
    int           enType;
    unsigned int  keybits;
    char          key[UTILS_AES_BLOCK_LEN + 1];
    char          decodeBuff[DECODE_BUFF_LEN] = {0};
    unsigned char iv[16];
    char *        payload = NULL;

#ifdef AUTH_MODE_CERT
    char *clientCert;
    char *clientKey;
#else
    char *psk;
#endif

    Log_d("dynamic registration response received");

    ret = _get_json_result_code(jdoc);
    if (UNITEDRHINO_RET_SUCCESS != ret) {
        Log_e("response err, ret:%d", ret);
        goto exit;
    }

    payload = LITE_json_value_of("data.payload", jdoc);
    if (payload == NULL) {
        payload = LITE_json_value_of("payload", jdoc);
    }
    if (payload == NULL) {
        Log_e("Invalid dynamic registration response JSON");
        ret = UNITEDRHINO_ERR_FAILURE;
        goto exit;
    } else {
        Log_d("dynamic registration payload decoded");
    }

    ret = unitedrhino_iot_utils_base64decode((uint8_t *)decodeBuff, sizeof(decodeBuff), &len, (uint8_t *)payload,
                                        strlen(payload));
    if (ret != UNITEDRHINO_RET_SUCCESS) {
        Log_e("Dynamic registration response decode failed");
        ret = UNITEDRHINO_ERR_FAILURE;
        goto exit;
    }

    datalen = len + (UTILS_AES_BLOCK_LEN - len % UTILS_AES_BLOCK_LEN);
    keybits = AES_KEY_BITS_128;
    memset(key, 0, UTILS_AES_BLOCK_LEN);
    strncpy(key, pDevInfo->product_secret, UTILS_AES_BLOCK_LEN);
    memset(iv, '0', UTILS_AES_BLOCK_LEN);
    ret = utils_aes_cbc((uint8_t *)decodeBuff, datalen, (uint8_t *)decodeBuff, DECODE_BUFF_LEN, UTILS_AES_DECRYPT,
                        (uint8_t *)key, keybits, iv);
    if (UNITEDRHINO_RET_SUCCESS == ret) {
        // Log_d("The decrypted data is:%s", decodeBuff);

    } else {
        Log_e("data decry err,ret:%d", ret);
        goto exit;
    }

    enType = _get_json_encry_type(decodeBuff);
    if (enType < 0) {
        Log_e("invlid encryt type, decrypt maybe faild");
        ret = UNITEDRHINO_ERR_FAILURE;
        goto exit;
    }

#ifdef AUTH_MODE_CERT
    if (eCERT_TYPE != enType) {
        Log_e("encryt type should be cert type");
        ret = UNITEDRHINO_ERR_FAILURE;
        goto exit;
    }

    clientCert = _get_json_cert_data(decodeBuff);
    if (NULL != clientCert) {
        memset(pDevInfo->dev_cert_file_name, 0, MAX_SIZE_OF_DEVICE_CERT_FILE_NAME);
        HAL_Snprintf(pDevInfo->dev_cert_file_name, MAX_SIZE_OF_DEVICE_CERT_FILE_NAME, "%s_cert.crt",
                     pDevInfo->device_name);
        if (UNITEDRHINO_RET_SUCCESS != _cert_file_save(pDevInfo->dev_cert_file_name, clientCert, strlen(clientCert))) {
            Log_e("save %s file fail", pDevInfo->dev_cert_file_name);
            ret = UNITEDRHINO_ERR_FAILURE;
        }

        HAL_Free(clientCert);

    } else {
        Log_e("Get clientCert data fail");
        ret = UNITEDRHINO_ERR_FAILURE;
    }

    clientKey = _get_json_key_data(decodeBuff);
    if (NULL != clientKey) {
        memset(pDevInfo->dev_key_file_name, 0, MAX_SIZE_OF_DEVICE_SECRET_FILE_NAME);
        HAL_Snprintf(pDevInfo->dev_key_file_name, MAX_SIZE_OF_DEVICE_SECRET_FILE_NAME, "%s_private.key",
                     pDevInfo->device_name);
        if (UNITEDRHINO_RET_SUCCESS != _cert_file_save(pDevInfo->dev_key_file_name, clientKey, strlen(clientKey))) {
            Log_e("save %s file fail", pDevInfo->dev_key_file_name);
            ret = UNITEDRHINO_ERR_FAILURE;
        }

        HAL_Free(clientKey);

    } else {
        Log_e("Get clientCert data fail");
        ret = UNITEDRHINO_ERR_FAILURE;
    }

#else
    psk = _get_json_psk(decodeBuff);
    if ((NULL != psk) && (strlen(psk) > 0)) {
        if (strlen(psk) > MAX_SIZE_OF_DEVICE_SECRET) {
            Log_e("Dynamic registration PSK exceeds maximum length");
            strcpy(pDevInfo->device_secret, psk);
        } else {
            strncpy(pDevInfo->device_secret, psk, MAX_SIZE_OF_DEVICE_SECRET);
            pDevInfo->device_secret[MAX_SIZE_OF_DEVICE_SECRET] = '\0';
        }
        HAL_Free(psk);
    } else {
        Log_e("Get psk data fail, encryptionType:%d", enType);
        if (psk) {
            HAL_Free(psk);
        }
        ret = UNITEDRHINO_ERR_FAILURE;
    }
#endif
exit:

    if (payload) {
        HAL_Free(payload);
    }

    return ret;
}

static void _dynreg_mqtt_on_message(void *pClient, MQTTMessage *message, void *userData)
{
    POINTER_SANITY_CHECK_RTN(message);
    POINTER_SANITY_CHECK_RTN(userData);

    DynRegMqttContext *ctx = (DynRegMqttContext *)userData;
    size_t             len = message->payload_len < sizeof(ctx->response) - 1 ? message->payload_len : sizeof(ctx->response) - 1;
    memset(ctx->response, 0, sizeof(ctx->response));
    memcpy(ctx->response, message->payload, len);
    ctx->response[len] = '\0';
    ctx->reply_ready   = true;
    Log_d("dynamic registration MQTT response received");
}

static void *_dynreg_mqtt_construct(DeviceInfo *pDevInfo)
{
    MQTTInitParams init_params = DEFAULT_MQTTINIT_PARAMS;
    init_params.product_id     = pDevInfo->product_id;
    init_params.device_name    = pDevInfo->device_name;
    init_params.region         = pDevInfo->region;

    UnitedRhino_IoT_Client *mqtt_client = (UnitedRhino_IoT_Client *)HAL_Malloc(sizeof(UnitedRhino_IoT_Client));
    if (mqtt_client == NULL) {
        Log_e("malloc register MQTT client failed");
        return NULL;
    }

    int rc = unitedrhino_iot_mqtt_init(mqtt_client, &init_params);
    if (rc != UNITEDRHINO_RET_SUCCESS) {
        Log_e("register mqtt init failed: %d", rc);
        HAL_Free(mqtt_client);
        return NULL;
    }

    char *client_id = HAL_Malloc(MAX_SIZE_OF_CLIENT_ID + 1);
    if (client_id == NULL) {
        Log_e("malloc register client_id failed");
        unitedrhino_iot_mqtt_fini(mqtt_client);
        HAL_Free(mqtt_client);
        return NULL;
    }
    memset(client_id, 0, MAX_SIZE_OF_CLIENT_ID + 1);
    HAL_Snprintf(client_id, MAX_SIZE_OF_CLIENT_ID, "register&%s&%s", pDevInfo->product_id, pDevInfo->device_name);

    MQTTConnectParams connect_params = DEFAULT_MQTTCONNECT_PARAMS;
    connect_params.client_id         = client_id;
    connect_params.keep_alive_interval = 60;
    connect_params.clean_session       = 1;
    connect_params.auto_connect_enable = 0;

    rc = unitedrhino_iot_mqtt_connect(mqtt_client, &connect_params);
    if (rc != UNITEDRHINO_RET_SUCCESS) {
        Log_e("register mqtt connect failed: %d", rc);
        unitedrhino_iot_mqtt_fini(mqtt_client);
        HAL_Free(client_id);
        HAL_Free(mqtt_client);
        return NULL;
    }

    Log_i("register mqtt connect success");
    return mqtt_client;
}

static int _post_reg_request_by_mqtt(char *request_buf, DeviceInfo *pDevInfo)
{
    int               ret = UNITEDRHINO_ERR_FAILURE;
    void *            client = NULL;
    DynRegMqttContext ctx;
    char              up_topic[MAX_SIZE_OF_CLOUD_TOPIC]   = {0};
    char              down_topic[MAX_SIZE_OF_CLOUD_TOPIC] = {0};

    memset(&ctx, 0, sizeof(ctx));
    HAL_Snprintf(up_topic, sizeof(up_topic), "$ext/up/register/%s/%s", pDevInfo->product_id, pDevInfo->device_name);
    HAL_Snprintf(down_topic, sizeof(down_topic), "$ext/down/register/%s/%s", pDevInfo->product_id, pDevInfo->device_name);

    client = _dynreg_mqtt_construct(pDevInfo);
    if (client == NULL) {
        return UNITEDRHINO_ERR_FAILURE;
    }

    SubscribeParams sub_params = DEFAULT_SUB_PARAMS;
    sub_params.qos                = QOS0;
    sub_params.on_message_handler = _dynreg_mqtt_on_message;
    sub_params.user_data          = &ctx;
    ret = IOT_MQTT_Subscribe(client, down_topic, &sub_params);
    if (ret < 0) {
        Log_e("register mqtt subscribe failed: %d", ret);
        goto exit;
    }

    for (int i = 0; i < 10 && !IOT_MQTT_IsSubReady(client, down_topic); ++i) {
        IOT_MQTT_Yield(client, 200);
    }
    if (!IOT_MQTT_IsSubReady(client, down_topic)) {
        Log_e("register mqtt subscribe ack timeout");
        ret = UNITEDRHINO_ERR_MQTT_REQUEST_TIMEOUT;
        goto exit;
    }

    PublishParams pub_params = DEFAULT_PUB_PARAMS;
    pub_params.qos         = QOS0;
    pub_params.payload     = request_buf;
    pub_params.payload_len = strlen(request_buf);
    ret = IOT_MQTT_Publish(client, up_topic, &pub_params);
    if (ret != UNITEDRHINO_RET_SUCCESS) {
        Log_e("register mqtt publish failed: %d", ret);
        goto exit;
    }

    Timer timer;
    InitTimer(&timer);
    countdown_ms(&timer, DYN_REG_RES_MQTT_TIMEOUT_MS);
    while (!ctx.reply_ready && left_ms(&timer) > 0) {
        IOT_MQTT_Yield(client, 500);
    }

    if (!ctx.reply_ready) {
        Log_e("register mqtt wait reply timeout");
        ret = UNITEDRHINO_ERR_FAILURE;
        goto exit;
    }

    ret = _parse_devinfo(ctx.response, pDevInfo);
    if (ret != UNITEDRHINO_RET_SUCCESS) {
        Log_e("parse mqtt register device info err");
    }

exit:
    if (client) {
        IOT_MQTT_Destroy(&client);
    }
    return ret;
}

static int _cal_dynreg_sign(DeviceInfo *pDevInfo, char *signout, int max_signlen, int nonce, uint32_t timestamp)
{
    int         sign_len;
    size_t      olen                   = 0;
    char *      pSignSource            = NULL;
    const char *sign_fmt               = "deviceName=%s&nonce=%d&productID=%s&timestamp=%d";
    char        sign[DYN_REG_SIGN_LEN] = {0};

    /*format sign data*/
    sign_len = strlen(sign_fmt) + strlen(pDevInfo->device_name) + strlen(pDevInfo->product_id) + sizeof(int) +
               sizeof(uint32_t) + DYN_BUFF_DATA_MORE;
    pSignSource = HAL_Malloc(sign_len);
    if (pSignSource == NULL) {
        Log_e("malloc sign source buff fail");
        return UNITEDRHINO_ERR_FAILURE;
    }
    memset(pSignSource, 0, sign_len);
    HAL_Snprintf((char *)pSignSource, sign_len, sign_fmt, pDevInfo->device_name, nonce, pDevInfo->product_id,
                 timestamp);

    /*cal hmac sha1*/
    utils_hmac_sha1(pSignSource, strlen(pSignSource), sign, pDevInfo->product_secret, strlen(pDevInfo->product_secret));

    /*base64 encode*/
    unitedrhino_iot_utils_base64encode((uint8_t *)signout, max_signlen, &olen, (const uint8_t *)sign, strlen(sign));

    HAL_Free(pSignSource);

    return (olen > max_signlen) ? UNITEDRHINO_ERR_FAILURE : UNITEDRHINO_RET_SUCCESS;
}

int IOT_DynReg_Device(DeviceInfo *pDevInfo)
{
    const char *para_format =
        "{\"msgToken\":\"%s-%u\",\"payload\":{\"nonce\":%d,\"timestamp\":%d,\"signature\":\"%s\","
        "\"retEnc\":\"aes128cbc\"}}";
    int      nonce;
    int      Ret;
    uint32_t timestamp;
    int      len;
    char     sign[DYN_REG_SIGN_LEN] = {0};
    char *   pRequest               = NULL;

    if (strlen(pDevInfo->product_secret) < UTILS_AES_BLOCK_LEN) {
        Log_e("product key inllegal");
        return UNITEDRHINO_ERR_FAILURE;
    }

    srand_d(HAL_GetTimeMs());
    nonce     = rand_d();
    timestamp = time(0);

    /*cal sign*/
    if (UNITEDRHINO_RET_SUCCESS == _cal_dynreg_sign(pDevInfo, sign, DYN_REG_SIGN_LEN, nonce, timestamp)) {
        Log_d("dynamic registration signature calculated");
    } else {
        Log_e("cal sign fail");
        return UNITEDRHINO_ERR_FAILURE;
    }

    /*format http request*/
    len = strlen(para_format) + strlen(pDevInfo->device_name) + sizeof(uint32_t) + sizeof(int) + sizeof(uint32_t) +
          strlen(sign) + DYN_BUFF_DATA_MORE;
    pRequest = HAL_Malloc(len);
    if (!pRequest) {
        Log_e("malloc request memory fail");
        return UNITEDRHINO_ERR_FAILURE;
    }
    memset(pRequest, 0, len);
    HAL_Snprintf(pRequest, len, para_format, pDevInfo->device_name, HAL_GetTimeMs(), nonce, timestamp, sign);
    Log_d("dynamic registration request created");
    /*post request*/
    Ret = _post_reg_request_by_mqtt(pRequest, pDevInfo);
    if (UNITEDRHINO_RET_SUCCESS == Ret) {
        Log_d("request dev info success");
    } else {
        Log_e("request dev info fail");
    }

    HAL_Free(pRequest);

    return Ret;
}

#ifdef __cplusplus
}
#endif
