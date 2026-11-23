if (CONFIG_UNITEDRHINO_LLSYNC_SUPPORT)
    set(srcs
            src/core/ble_qiot_llsync_data.c
            src/core/ble_qiot_llsync_device.c
            src/core/ble_qiot_llsync_event.c
            src/core/ble_qiot_llsync_ota.c
            src/core/ble_qiot_service.c
            src/utils/ble_qiot_utils_base64.c
            src/utils/ble_qiot_utils_hmac.c
            src/utils/ble_qiot_utils_log.c
            src/utils/ble_qiot_utils_md5.c
            src/utils/ble_qiot_utils_sha1.c
            src/utils/ble_qiot_utils_crc.c)

    list(APPEND srcs
            date_template/ble_qiot_template.c)

    # BLE HAL 使用 IDF 6 的 Bluedroid GATTS；非蓝牙构建不暴露 esp_bt.h，故按配置选源文件。
    if (CONFIG_BT_ENABLED)
        list(APPEND srcs
                hal/ble_qiot_ble_device.c
                hal/ble_qiot_ble_service.c)
    endif ()

    set(includes ${includes} inc hal)
    set(priv_includes date_template src/internal_inc)

endif ()

idf_component_register(SRCS "${srcs}"
        INCLUDE_DIRS "${includes}"
        PRIV_INCLUDE_DIRS "${priv_includes}"
        REQUIRES app_update nvs_flash spi_flash unitedrhino_iot_c_sdk
        PRIV_REQUIRES bt)
