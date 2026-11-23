// 单调时钟替身允许精确测试截止时间，不进行真实等待。
#pragma once
#include <cstdint>
inline int64_t fake_time_us = 0;  // 用例控制的微秒时间。
inline int64_t esp_timer_get_time() { return fake_time_us; }
