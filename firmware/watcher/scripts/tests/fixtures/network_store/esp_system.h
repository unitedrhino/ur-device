// 主机复位替身：保留冷启动/软件重启区别，用异常截断不可返回的重启动作。
#pragma once
enum esp_reset_reason_t { ESP_RST_POWERON, ESP_RST_SW };
inline esp_reset_reason_t fake_reset_reason = ESP_RST_POWERON;  // 用例指定的本次启动原因。
inline esp_reset_reason_t esp_reset_reason() { return fake_reset_reason; }
[[noreturn]] inline void esp_restart() { throw 1; }
