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

#include "utils_hmac.h"

#include <string.h>

#include "psa/crypto.h"
#include "unitedrhino_iot_export_log.h"
#include "utils_md5.h"
#include "utils_sha1.h"

#define KEY_IOPAD_SIZE 64

#define MD5_DIGEST_SIZE  16
#define SHA1_DIGEST_SIZE 20
#define SHA256_DIGEST_SIZE 32

void utils_hmac_md5(const char *msg, int msg_len, char *digest, const char *key, int key_len)
{
    if ((NULL == msg) || (NULL == digest) || (NULL == key)) {
        Log_e("parameter is Null,failed!");
        return;
    }

    if (key_len > KEY_IOPAD_SIZE) {
        Log_e("key_len > size(%d) of array", KEY_IOPAD_SIZE);
        return;
    }

    iot_md5_context context;
    unsigned char   k_ipad[KEY_IOPAD_SIZE]; /* inner padding - key XORd with ipad */
    unsigned char   k_opad[KEY_IOPAD_SIZE]; /* outer padding - key XORd with opad */
    unsigned char   out[MD5_DIGEST_SIZE];
    int             i;

    /* start out by storing key in pads */
    memset(k_ipad, 0, sizeof(k_ipad));
    memset(k_opad, 0, sizeof(k_opad));
    memcpy(k_ipad, key, key_len);
    memcpy(k_opad, key, key_len);

    /* XOR key with ipad and opad values */
    for (i = 0; i < KEY_IOPAD_SIZE; i++) {
        k_ipad[i] ^= 0x36;
        k_opad[i] ^= 0x5c;
    }

    /* perform inner MD5 */
    utils_md5_init(&context);                                  /* init context for 1st pass */
    utils_md5_starts(&context);                                /* setup context for 1st pass */
    utils_md5_update(&context, k_ipad, KEY_IOPAD_SIZE);        /* start with inner pad */
    utils_md5_update(&context, (unsigned char *)msg, msg_len); /* then text of datagram */
    utils_md5_finish(&context, out);                           /* finish up 1st pass */

    /* perform outer MD5 */
    utils_md5_init(&context);                           /* init context for 2nd pass */
    utils_md5_starts(&context);                         /* setup context for 2nd pass */
    utils_md5_update(&context, k_opad, KEY_IOPAD_SIZE); /* start with outer pad */
    utils_md5_update(&context, out, MD5_DIGEST_SIZE);   /* then results of 1st hash */
    utils_md5_finish(&context, out);                    /* finish up 2nd pass */

    for (i = 0; i < MD5_DIGEST_SIZE; ++i) {
        digest[i * 2]     = utils_hb2hex(out[i] >> 4);
        digest[i * 2 + 1] = utils_hb2hex(out[i]);
    }
}

void utils_hmac_sha1(const char *msg, int msg_len, char *digest, const char *key, int key_len)
{
    if ((NULL == msg) || (NULL == digest) || (NULL == key)) {
        Log_e("parameter is Null,failed!");
        return;
    }

    if (key_len > KEY_IOPAD_SIZE) {
        Log_e("key_len > size(%d) of array", KEY_IOPAD_SIZE);
        return;
    }

    iot_sha1_context context;
    unsigned char    k_ipad[KEY_IOPAD_SIZE]; /* inner padding - key XORd with ipad */
    unsigned char    k_opad[KEY_IOPAD_SIZE]; /* outer padding - key XORd with opad */
    unsigned char    out[SHA1_DIGEST_SIZE];
    int              i;

    /* start out by storing key in pads */
    memset(k_ipad, 0, sizeof(k_ipad));
    memset(k_opad, 0, sizeof(k_opad));
    memcpy(k_ipad, key, key_len);
    memcpy(k_opad, key, key_len);

    /* XOR key with ipad and opad values */
    for (i = 0; i < KEY_IOPAD_SIZE; i++) {
        k_ipad[i] ^= 0x36;
        k_opad[i] ^= 0x5c;
    }

    /* perform inner SHA */
    utils_sha1_init(&context);                                  /* init context for 1st pass */
    utils_sha1_starts(&context);                                /* setup context for 1st pass */
    utils_sha1_update(&context, k_ipad, KEY_IOPAD_SIZE);        /* start with inner pad */
    utils_sha1_update(&context, (unsigned char *)msg, msg_len); /* then text of datagram */
    utils_sha1_finish(&context, out);                           /* finish up 1st pass */

    /* perform outer SHA */
    utils_sha1_init(&context);                           /* init context for 2nd pass */
    utils_sha1_starts(&context);                         /* setup context for 2nd pass */
    utils_sha1_update(&context, k_opad, KEY_IOPAD_SIZE); /* start with outer pad */
    utils_sha1_update(&context, out, SHA1_DIGEST_SIZE);  /* then results of 1st hash */
    utils_sha1_finish(&context, out);                    /* finish up 2nd pass */

    for (i = 0; i < SHA1_DIGEST_SIZE; ++i) {
        digest[i * 2]     = utils_hb2hex(out[i] >> 4);
        digest[i * 2 + 1] = utils_hb2hex(out[i]);
    }
}

/* IDF6 的 mbedtls 为 TF-PSA-Crypto 架构（mbedtls_md_hmac/底层 sha256 API 均不公开），
 * HMAC-SHA256 改用 PSA Crypto API 实现 */
int utils_hmac_sha256(const char *msg, int msg_len, char *digest, const char *key, int key_len)
{
    if ((NULL == msg) || (NULL == digest) || (NULL == key) || key_len <= 0) {
        Log_e("parameter is Null,failed!");
        return 0;
    }

    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attr, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&attr, key_len * 8);

    psa_key_id_t key_id = 0;
    if (psa_import_key(&attr, (const uint8_t *)key, key_len, &key_id) != PSA_SUCCESS) {
        Log_e("psa import hmac key failed");
        return 0;
    }

    unsigned char out[SHA256_DIGEST_SIZE];
    size_t        mac_len = 0;
    psa_status_t  st      = psa_mac_compute(key_id, PSA_ALG_HMAC(PSA_ALG_SHA_256), (const uint8_t *)msg, msg_len,
                                        out, sizeof(out), &mac_len);
    psa_destroy_key(key_id);

    if (st != PSA_SUCCESS || mac_len != SHA256_DIGEST_SIZE) {
        Log_e("psa hmac sha256 failed: %d", (int)st);
        return 0;
    }

    for (int i = 0; i < SHA256_DIGEST_SIZE; ++i) {
        digest[i * 2]     = utils_hb2hex(out[i] >> 4);
        digest[i * 2 + 1] = utils_hb2hex(out[i]);
    }

    return SHA256_DIGEST_SIZE * 2;
}

int utils_hmac_sha1_hex(const char *msg, int msg_len, char *digest, const char *key, int key_len)
{
    if ((NULL == msg) || (NULL == digest) || (NULL == key)) {
        Log_e("parameter is Null,failed!");
        return 0;
    }

    if (key_len > KEY_IOPAD_SIZE) {
        Log_e("key_len > size(%d) of array", KEY_IOPAD_SIZE);
        return 0;
    }

    iot_sha1_context context;
    unsigned char    k_ipad[KEY_IOPAD_SIZE]; /* inner padding - key XORd with ipad */
    unsigned char    k_opad[KEY_IOPAD_SIZE]; /* outer padding - key XORd with opad */
    unsigned char    out[SHA1_DIGEST_SIZE];
    int              i;

    /* start out by storing key in pads */
    memset(k_ipad, 0, sizeof(k_ipad));
    memset(k_opad, 0, sizeof(k_opad));
    memcpy(k_ipad, key, key_len);
    memcpy(k_opad, key, key_len);

    /* XOR key with ipad and opad values */
    for (i = 0; i < KEY_IOPAD_SIZE; i++) {
        k_ipad[i] ^= 0x36;
        k_opad[i] ^= 0x5c;
    }

    /* perform inner SHA */
    utils_sha1_init(&context);                                  /* init context for 1st pass */
    utils_sha1_starts(&context);                                /* setup context for 1st pass */
    utils_sha1_update(&context, k_ipad, KEY_IOPAD_SIZE);        /* start with inner pad */
    utils_sha1_update(&context, (unsigned char *)msg, msg_len); /* then text of datagram */
    utils_sha1_finish(&context, out);                           /* finish up 1st pass */

    /* perform outer SHA */
    utils_sha1_init(&context);                           /* init context for 2nd pass */
    utils_sha1_starts(&context);                         /* setup context for 2nd pass */
    utils_sha1_update(&context, k_opad, KEY_IOPAD_SIZE); /* start with outer pad */
    utils_sha1_update(&context, out, SHA1_DIGEST_SIZE);  /* then results of 1st hash */
    utils_sha1_finish(&context, out);                    /* finish up 2nd pass */

    memcpy(digest, out, SHA1_DIGEST_SIZE);

    return SHA1_DIGEST_SIZE;
}
