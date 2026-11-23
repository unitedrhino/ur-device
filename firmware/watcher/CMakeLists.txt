# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)

if(NOT DEFINED PROJECT_VER)
    set(PROJECT_VER "2.7.0")
endif()
project(xiaozhi)

# Watcher 复用原驱动，仅在构建目录生成最近网络优先级适配，不改受管理依赖。
if(CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER)
    include(cmake/watcher_wifi_order.cmake)
    include(cmake/watcher_http_lifetime.cmake)
    include(cmake/watcher_touch_i2c.cmake)
    if(CONFIG_UR_BLE_PROVISION)
        # SDK 收到 Token 不等于绑定完成；保留 GATT 到最终 E3 回执，禁止旧 HAL 提前关闭。
        target_link_options(${CMAKE_PROJECT_NAME}.elf PRIVATE "-Wl,--wrap=ble_combo_release_radio_after_token")
    endif()
endif()
