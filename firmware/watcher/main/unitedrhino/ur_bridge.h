/* 联犀模块与 xiaozhi C++ 框架的桥接接口
 *
 * ur_device.c / ur_ble_provision.c 为 C 实现，无法直接调用 xiaozhi 的 C++ 单例
 * （Board/Application/SsidManager 等），本文件以 extern "C" 函数提供桥接。
 */
#ifndef UR_BRIDGE_H
#define UR_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <cJSON.h>

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 保存 WiFi 凭据到 xiaozhi 的 SsidManager（NVS），重启后 WifiStation 自动联网 */
void ur_bridge_save_wifi(const char *ssid, const char *password);

/* 清除已保存的 WiFi 凭据，供 BLE 绑定失败或超时后回退热点配网。 */
void ur_bridge_clear_wifi(void);

/* 读取预置 WiFi 凭据（NVS ur_dev/ssid、ur_dev/pass，产线或测试注入）并装入
 * SsidManager；无预置凭据时返回 false */
bool ur_bridge_load_preprovision_wifi(void);

/* 清除设备绑定信息：设备密钥（NVS）与已保存的 WiFi 凭据，回到待配网状态 */
void ur_bridge_clear_device_info(void);

/* 仅清除联犀 DeviceSecret，保留 WiFi，用于明确鉴权失效后的动态注册自愈。 */
bool ur_bridge_clear_device_secret(void);

/* 读取电池状态：level 为电量百分比 0-100，charging 为是否充电中；读取失败返回 false */
bool ur_bridge_get_battery(int *level, bool *charging);

/* 读取当前输出音量 0-100 */
int ur_bridge_get_volume(void);

/* 设置输出音量 0-100（经 Application Schedule 在主线程执行） */
void ur_bridge_set_volume(int volume);

/* 读取屏幕背光亮度 0-100 */
int ur_bridge_get_brightness(void);

/* 设置屏幕背光亮度 0-100 */
void ur_bridge_set_brightness(int brightness);

/* 读取 WiFi RSSI（dBm，负值）；未联网返回 0 */
int ur_bridge_get_rssi(void);

/* 板级属性 params JSON（联犀物模型扩展）；返回写入长度，0 表示走默认五属性格式，负值为缓冲不足 */
int ur_bridge_get_properties_json(char *buf, size_t size);

/* 板级属性控制；params 为下行 JSON 对象。返回 -1 不接管（走默认 Volume/Brightness），
 * >=0 为处理属性数且 reply_data 填 controlReply 的 data JSON 对象文本，其他负值为异常 */
int ur_bridge_handle_control(const cJSON *params, char *reply_data, size_t reply_size);

/* 板级行为处理（坦克动作等）；返回 HTTP 风格 code，200 成功，404 本板不支持 */
int ur_bridge_handle_action(const char *action_id, const char *params_json);

/* 联犀 MQTT 断连通知；板级立即停止底盘等外设运动，避免指令残留 */
void ur_bridge_notify_disconnected(void);

/* 屏幕显示通知文本（经 Application Schedule 在主线程执行） */
void ur_bridge_show_text(const char *text);

#if CONFIG_UR_AI
/* ur_bridge_start_photo_action 启动独立相机 worker；0 表示已接纳，其余为 HTTP 风格拒绝码。 */
int ur_bridge_start_photo_action(const char *token);
/* ur_bridge_start_photo_input 启动双击拍照 worker；0 表示已接纳，其余为 HTTP 风格拒绝码。 */
int ur_bridge_start_photo_input(void);
/* ur_bridge_photo_busy 返回全局相机 worker 是否占用，供本地双击在打断播报前检查。 */
bool ur_bridge_photo_busy(void);
#endif

/* 联犀 OTA worker 的只读状态快照。 */
typedef struct {
    int state;      /* 0=空闲，1=下载写入中，2=成功待重启，3=失败 */
    int progress;   /* 1-100；失败时保留最后进度 */
    int error_code; /* 0 或平台固定失败码 -1~-4 */
} ur_ota_status_t;

/* 启动单文件全量 OTA；worker 独立运行，不阻塞 MQTT Yield。 */
bool ur_bridge_start_ota(const char *url, const char *version, size_t size, const char *md5);
/* 原子排除 OTA/相机后持久化换网意图；调用方仍需物理确认及受控重启。 */
bool ur_bridge_begin_network_change(const char *request_id);
/* 预留换网确认，返回 HTTP 风格状态；回执发出后另行展示，不直接开启 BLE。 */
int ur_bridge_prepare_network_change(const char *request_id);
void ur_bridge_present_network_change(const char *request_id);

/* 获取 OTA worker 的线程安全状态快照。 */
void ur_bridge_get_ota_status(ur_ota_status_t *status);

/* 读取待云端确认的目标版本；无待确认版本时返回 false。 */
bool ur_bridge_get_pending_ota_version(char *version, size_t version_size);

/* reportInfoReply 确认版本后标记当前镜像有效并清除待确认状态。 */
bool ur_bridge_confirm_ota(const char *version);

#ifdef __cplusplus
}
#endif

#endif /* UR_BRIDGE_H */
