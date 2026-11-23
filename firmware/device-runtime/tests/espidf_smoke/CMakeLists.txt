# 仅编译/链接共享C++核心，不烧录、不初始化硬件或连接网络。
cmake_minimum_required(VERSION 3.22)
if(NOT UR_DEVICE_ROOT)
  message(FATAL_ERROR "需要显式UR_DEVICE_ROOT，使用设备核心的绝对源码目录")
endif()
set(EXTRA_COMPONENT_DIRS "${UR_DEVICE_ROOT}/adapters/espidf/ur_device")
# 只构建入口、共享核心及必需的SDK依赖，不为了编译探针构建无关网络/硬件组件。
set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ur_device_core_smoke)
