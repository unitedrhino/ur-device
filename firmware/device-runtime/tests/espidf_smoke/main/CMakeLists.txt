# 入口仅引用共享协议组件，硬件适配不在本次编译证明范围内。
idf_component_register(SRCS "main.cpp" REQUIRES ur_device)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
set_target_properties(${COMPONENT_LIB} PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
target_compile_options(${COMPONENT_LIB} PRIVATE -std=c++17 -Wall -Wextra -Wpedantic -Werror -fno-exceptions -fno-rtti)
