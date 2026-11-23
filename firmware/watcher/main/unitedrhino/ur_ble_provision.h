/* 联犀 LLSync 蓝牙配网接口：无 WiFi 凭据时由 Watcher 启动，结束后回到正常启动链路。 */
#ifndef UR_BLE_PROVISION_H
#define UR_BLE_PROVISION_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 阻塞等待 BLE 配网完成；成功时 WiFi 与绑定信息均已持久化。 */
bool ur_ble_provision_start(uint32_t timeout_ms);
/* 已有配置的配网绑定：候选隔离，绑定成功后才提交 Wi-Fi；调用前须恢复绑定事务。 */
bool ur_ble_rebind_start(uint32_t timeout_ms);

/* 停止并释放 Bluedroid/BLE 控制器资源；重复或部分初始化时安全返回。 */
void ur_ble_provision_deinit_bt(void);

/* 独占换网 worker 的 IP 状态回调；调用方只调度应用事件，不直接操作 LVGL。 */
typedef void (*ur_network_ip_callback_t)(bool connected, void *context);
/* 异步启动 BLE 候选接收或候选 STA 验证；不得同时初始化 WifiManager。 */
bool ur_ble_network_start(bool trial, ur_network_ip_callback_t callback, void *context);

#ifdef __cplusplus
}
#endif

#endif /* UR_BLE_PROVISION_H */
