// 每个ESP共享核心翻译单元都强制检查实际编译标准，不能被SDK后置选项悄悄覆盖。
#pragma once
#if __cplusplus != 201703L
#error "设备共享核心必须实际使用C++17编译"
#endif
#if defined(__EXCEPTIONS) || defined(__GXX_RTTI)
#error "ESP设备共享核心不能依赖异常或RTTI"
#endif
