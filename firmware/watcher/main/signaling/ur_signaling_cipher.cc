#include "ur_signaling_cipher.h"

#include <esp_log.h>

#include <cstring>

namespace ur::device {
namespace {
constexpr const char* kTag = "ur_sig_cipher";
}  // namespace

UrSignalingCipher::~UrSignalingCipher() { clear(); }

bool UrSignalingCipher::install(const std::array<std::uint8_t, 16>& key) {
    clear();
    psa_status_t status = psa_crypto_init();
    if (status != PSA_SUCCESS) {
        ESP_LOGE(kTag, "psa init failed: %ld", static_cast<long>(status));
        return false;
    }
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attributes, PSA_ALG_CTR);
    psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attributes, 128);
    status = psa_import_key(&attributes, key.data(), key.size(), &key_id_);
    psa_reset_key_attributes(&attributes);
    if (status != PSA_SUCCESS) {
        ESP_LOGE(kTag, "key import failed: %ld", static_cast<long>(status));
        key_id_ = PSA_KEY_ID_NULL;
        return false;
    }
    return true;
}

void UrSignalingCipher::clear() {
    if (key_id_ != PSA_KEY_ID_NULL) {
        psa_destroy_key(key_id_);
        key_id_ = PSA_KEY_ID_NULL;
    }
}

bool UrSignalingCipher::transform(const std::array<std::uint8_t, 16>& iv, const std::uint8_t* input,
                                  std::uint8_t* output, std::size_t size) {
    if (key_id_ == PSA_KEY_ID_NULL || input == nullptr || output == nullptr || size == 0) {
        return false;
    }
    psa_cipher_operation_t operation = PSA_CIPHER_OPERATION_INIT;
    psa_status_t status = psa_cipher_encrypt_setup(&operation, key_id_, PSA_ALG_CTR);
    if (status != PSA_SUCCESS) {
        ESP_LOGE(kTag, "setup failed: %ld", static_cast<long>(status));
        psa_cipher_abort(&operation);
        return false;
    }
    status = psa_cipher_set_iv(&operation, iv.data(), iv.size());
    if (status != PSA_SUCCESS) {
        ESP_LOGE(kTag, "set iv failed: %ld", static_cast<long>(status));
        psa_cipher_abort(&operation);
        return false;
    }
    size_t output_len = 0;
    status = psa_cipher_update(&operation, input, size, output, size, &output_len);
    if (status != PSA_SUCCESS) {
        ESP_LOGE(kTag, "update failed: %ld", static_cast<long>(status));
        psa_cipher_abort(&operation);
        return false;
    }
    /* CTR 流式加解密无尾部填充；finish 仅收尾，不期望额外输出。 */
    uint8_t finish_output[16];
    size_t finish_len = 0;
    status = psa_cipher_finish(&operation, finish_output, sizeof(finish_output), &finish_len);
    psa_cipher_abort(&operation);
    if (status != PSA_SUCCESS || output_len != size) {
        ESP_LOGE(kTag, "finish failed: %ld len=%u", static_cast<long>(status),
                 (unsigned)output_len);
        return false;
    }
    return true;
}

}  // namespace ur::device
