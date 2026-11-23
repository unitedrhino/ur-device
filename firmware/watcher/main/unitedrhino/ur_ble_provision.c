/* 联犀 LLSync BLE 配网实现：FFF0 服务，FFE1 写入，FFE3 通知。 */
#include <string.h>
#include <stdatomic.h>

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_coexist.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "ble_qiot_export.h"
#include "ble_qiot_import.h"
#include "unitedrhino_iot_export.h"
#include "unitedrhino_wifi_provision.h"

#include "ur_ble_provision.h"
#include "ur_bridge.h"
#include "ur_network.h"

#define UR_TAG "ur_ble_prov"
#define UR_PROV_SUCCESS_BIT BIT0
#define UR_PROV_FAIL_BIT BIT1

/* __wrap_ble_combo_release_radio_after_token 拦截旧 HAL 的提前断连请求。
 * Token 写入只代表受理；小程序必须继续收到 SDK 完成云端上报后的 E3。
 * 最终资源释放仍由 ur_ble_provision_start 的结果通知宽限期和 stop 负责。 */
void __wrap_ble_combo_release_radio_after_token(void)
{
    ESP_LOGI(UR_TAG, "Token accepted; keep GATT until provisioning result");
}

/* HAL_Wifi_api.c 通过此标志判断配网过程是否已获取 IP。 */
bool sg_gotip_flag = false;

static bool s_provisioning = false;
static bool s_sntp_started = false;
static EventGroupHandle_t s_result_event = NULL;

/* 换网独立启动状态；不启动 SDK 的绑定任务，因此不会索取 Token 或执行动态注册。 */
static atomic_bool s_change_mode = false;
static atomic_bool s_connect_requested = false;
static bool s_trial_mode = false;
/* 单槽凭据信箱：0 空闲、1 回调复制中、2 worker 可消费；发布/消费边界保护凭据副本。 */
static atomic_int s_candidate_mailbox = 0;
static atomic_bool s_candidate_ready = false; /* E3 必须等候选持久化与临时驱动配置均成功。 */
static char s_candidate_ssid[33] = {0}; /* 仅 worker 持久化，不在 BTC_TASK 上展开事务快照。 */
static char s_candidate_password[65] = {0}; /* 消费后立即清零，不留历史密码副本。 */
static EventGroupHandle_t s_change_events = NULL;
static ur_network_ip_callback_t s_ip_callback = NULL;
static void *s_ip_context = NULL;
static atomic_bool s_rebind_mode = false; /* worker 与 GATT 共享模式；不提前覆盖正式网络。 */
#define UR_CHANGE_IP BIT0
#define UR_CHANGE_DISCONNECTED BIT1
/* 固定版本 LLSync 提供的只读连接查询，声明复用符号，不修改供应商实现。 */
extern bool ble_is_connected(void);
/* 与 BT combo HAL 相同的 Bluedroid 服务入口，独立换网不启动绑定任务。 */
extern void ble_qiot_service_init(void);

/* ur_change_report_result 复用联网通知；只有本地提交完成且网络仍连接才报告成功。 */
static int ur_change_report_result(void)
{
    ur_network_status_t status = {0};
    wifi_config_t config = {0};
    ur_network_status(&status);
    bool success = !status.blocked && status.stage == 7 && sg_gotip_flag &&
                   esp_wifi_get_config(WIFI_IF_STA, &config) == ESP_OK;
    uint8_t length = success ? (uint8_t)strnlen((const char *)config.sta.ssid, 32) : 0;
    int result = ble_event_report_wifi_connect(BLE_WIFI_MODE_STA,
        success ? BLE_WIFI_STATE_CONNECT : BLE_WIFI_STATE_OTHER, length,
        success ? (const char *)config.sta.ssid : "");
    memset(&config, 0, sizeof(config));
    return result;
}

/* ur_wifi_event_handler 只处理 BLE 配网临时 WiFi 实例的断开与拿 IP 事件。 */
static void ur_wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (atomic_load(&s_change_mode)) {
            /* 只记录驱动数值原因，区分扫描/认证等失败；不输出事件内 SSID 或凭据。
             * 缺少事件数据时用 -1 表示未知，不猜测热点频段或更改恢复策略。 */
            const wifi_event_sta_disconnected_t *disconnected = event_data;
            ESP_LOGW(UR_TAG, "network change station disconnected reason=%d",
                     disconnected != NULL ? (int)disconnected->reason : -1);
            sg_gotip_flag = false;
            xEventGroupClearBits(s_change_events, UR_CHANGE_IP);
            xEventGroupSetBits(s_change_events, UR_CHANGE_DISCONNECTED);
            if (atomic_load(&s_connect_requested)) esp_wifi_connect();
            return;
        }
        wifi_config_t config = {0};
        sg_gotip_flag = false;
        esp_wifi_get_config(WIFI_IF_STA, &config);
        if (s_provisioning) {
            ble_event_report_wifi_connect(BLE_WIFI_MODE_STA, BLE_WIFI_STATE_OTHER,
                                          (uint8_t)strnlen((const char *)config.sta.ssid,
                                                           sizeof(config.sta.ssid)),
                                          (const char *)config.sta.ssid);
            /* Bluedroid 与 WiFi 首次并发建链时可能触发短暂共存退避。配网窗口仍有效时
             * 继续使用已校验的临时凭据重连，错误密码最终仍由总超时统一回退热点。 */
            esp_err_t reconnect_err = esp_wifi_connect();
            if (reconnect_err == ESP_OK) {
                ESP_LOGW(UR_TAG, "station disconnected during provisioning, retrying");
            } else {
                ESP_LOGW(UR_TAG, "station reconnect request failed: %s",
                         esp_err_to_name(reconnect_err));
            }
        } else {
            ESP_LOGI(UR_TAG, "station disconnected after provisioning stopped");
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(UR_TAG, "station got ip: " IPSTR, IP2STR(&event->ip_info.ip));
        sg_gotip_flag = true;
        if (atomic_load(&s_change_mode)) xEventGroupSetBits(s_change_events, UR_CHANGE_IP);
        esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);

        /* 域名型 SNTP 服务必须在网络可用后启动；提前启动时首次 DNS 失败会进入长周期重试，
         * 使动态注册仍停留在 1970 年。 */
        if (!s_sntp_started) {
            esp_sntp_config_t sntp_config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            esp_err_t sntp_err = esp_netif_sntp_init(&sntp_config);
            if (sntp_err == ESP_OK || sntp_err == ESP_ERR_INVALID_STATE) {
                s_sntp_started = true;
            } else {
                ESP_LOGW(UR_TAG, "initialize SNTP failed: %s", esp_err_to_name(sntp_err));
            }
        }
    }
}

/* wifi_init_sta 是 LLSync BT combo HAL 约定的 WiFi 初始化入口。 */
void wifi_init_sta(void)
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }
    if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") == NULL) {
        esp_netif_create_default_wifi_sta();
    }

    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&config);
    if (err != ESP_OK && err != ESP_ERR_WIFI_INIT_STATE) {
        /* 内存不足时 abort 会造成「重启→再配网→再失败」死循环；记录后直接
         * 返回，App 侧表现为本次配网失败可重试。 */
        ESP_LOGE(UR_TAG, "esp_wifi_init failed %d, provisioning unavailable", err);
        return;
    }
    /* 换网的临时 STA 永不写 IDF 默认 Wi-Fi NVS；正式列表只由提交协调器修改。 */
    if (atomic_load(&s_change_mode) || s_rebind_mode) ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    /* IDF 会从空 NVS 采用默认 AP 模式；LLSync 只支持 STA，启动前固定模式以免配网期额外广播热点。 */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, ur_wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, ur_wifi_event_handler, NULL));
    if (esp_wifi_start() != ESP_OK) {
        ESP_LOGE(UR_TAG, "esp_wifi_start failed, provisioning unavailable");
        return;
    }
    ESP_LOGI(UR_TAG, "temporary provisioning WiFi initialized");
}

/* ble_combo_wifi_mode_set 应答 App 的 STA 模式设置。 */
ble_qiot_ret_status_t ble_combo_wifi_mode_set(BLE_WIFI_MODE mode)
{
    (void)mode;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    return ble_event_report_wifi_mode(0);
}

/* ur_change_queue_candidate 只复制已校验凭据；成功通知由 worker 持久化后发送。 */
static int ur_change_queue_candidate(const char *ssid, uint8_t ssid_len,
                                     const char *password, uint8_t password_len)
{
    int empty = 0;
    if (s_trial_mode || atomic_load(&s_connect_requested) ||
        !atomic_compare_exchange_strong(&s_candidate_mailbox, &empty, 1)) {
        return ble_event_report_wifi_info(1);
    }
    atomic_store(&s_candidate_ready, false);
    memcpy(s_candidate_ssid, ssid, ssid_len);
    s_candidate_ssid[ssid_len] = 0;
    if (password_len) memcpy(s_candidate_password, password, password_len);
    s_candidate_password[password_len] = 0;
    atomic_store(&s_candidate_mailbox, 2);
    return 0;
}

/* ur_change_apply_candidate 在 8KB worker 中保存候选并配置临时 STA，随后回报受理结果。 */
static void ur_change_apply_candidate(void)
{
    if (atomic_load(&s_candidate_mailbox) != 2) return;
    wifi_config_t config = {0};
    bool accepted = !s_trial_mode && !atomic_load(&s_connect_requested) &&
                    ur_network_accept(s_candidate_ssid, s_candidate_password);
    if (accepted) {
        memcpy(config.sta.ssid, s_candidate_ssid, strnlen(s_candidate_ssid, 32));
        memcpy(config.sta.password, s_candidate_password, strnlen(s_candidate_password, 64));
        config.sta.threshold.authmode = s_candidate_password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        config.sta.pmf_cfg.capable = true;
        accepted = esp_wifi_set_config(WIFI_IF_STA, &config) == ESP_OK;
    }
    memset(&config, 0, sizeof(config));
    memset(s_candidate_ssid, 0, sizeof(s_candidate_ssid));
    memset(s_candidate_password, 0, sizeof(s_candidate_password));
    atomic_store(&s_candidate_ready, accepted);
    atomic_store(&s_candidate_mailbox, 0);
    if (s_rebind_mode && accepted) set_bt_combo_config_state(WIFI_CONFIG_SUCCESS);
    ble_event_report_wifi_info(accepted ? 0 : 1);
}

/* ble_combo_wifi_info_set 首次绑定保留原路径；换网回调仅校验并投递，不执行 NVS 事务。 */
ble_qiot_ret_status_t ble_combo_wifi_info_set(const char *ssid, uint8_t ssid_len,
                                              const char *password, uint8_t password_len)
{
    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {.capable = true, .required = false},
        },
    };
    if (ssid == NULL || ssid_len == 0 || ssid_len > sizeof(wifi_config.sta.ssid) ||
        (password == NULL && password_len != 0) ||
        password_len > sizeof(wifi_config.sta.password) || memchr(ssid, 0, ssid_len) ||
        (password_len && memchr(password, 0, password_len))) {
        return ble_event_report_wifi_info(1);
    }
    if (atomic_load(&s_change_mode) || s_rebind_mode) {
        return ur_change_queue_candidate(ssid, ssid_len, password, password_len);
    }

    /* ESP-IDF 允许 32 字节 SSID 和 64 字节十六进制 PSK，此时驱动结构没有结尾 NUL；
     * 持久化前用扩展一字节的副本保证 C 字符串边界安全。 */
    char saved_ssid[sizeof(wifi_config.sta.ssid) + 1] = {0};
    char saved_password[sizeof(wifi_config.sta.password) + 1] = {0};
    memcpy(wifi_config.sta.ssid, ssid, ssid_len);
    memcpy(saved_ssid, ssid, ssid_len);
    if (password_len != 0) {
        memcpy(wifi_config.sta.password, password, password_len);
        memcpy(saved_password, password, password_len);
    }
    ESP_LOGI(UR_TAG, "WiFi credentials received, ssid length=%u", ssid_len);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ur_bridge_save_wifi(saved_ssid, saved_password);
    set_bt_combo_config_state(WIFI_CONFIG_SUCCESS);
    return ble_event_report_wifi_info(0);
}

/* ble_combo_wifi_connect 执行 App 明确下发的联网步骤。 */
ble_qiot_ret_status_t ble_combo_wifi_connect(void)
{
    if (s_rebind_mode) {
        if (!atomic_load(&s_candidate_ready)) return BLE_QIOT_RS_ERR;
        atomic_store(&s_connect_requested, true);
    }
    if (atomic_load(&s_change_mode)) {
        ur_network_status_t status = {0};
        ur_network_status(&status);
        /* 成功通知保留窗口内重复 E3 只回报当前结果，不重复连接或写入配置。 */
        if (!s_trial_mode && (status.stage == 7 || status.stage == 8)) return ur_change_report_result();
        if (s_trial_mode || status.blocked || status.stage != 3 ||
            atomic_load(&s_candidate_mailbox) != 0 || !atomic_load(&s_candidate_ready)) return BLE_QIOT_RS_ERR;
        atomic_store(&s_connect_requested, true);
    }
    ESP_LOGI(UR_TAG, "connect WiFi requested");
    /* GATT 会一直保持到 token 回执。关联与 DHCP 阶段临时提高 WiFi 的射频优先级，
     * 避免 BLE 连接占满共存时隙而让 DHCP 延迟到配网总超时之后。 */
    esp_err_t coex_err = esp_coex_preference_set(ESP_COEX_PREFER_WIFI);
    if (coex_err != ESP_OK) {
        ESP_LOGW(UR_TAG, "set WiFi coexist preference failed: %s", esp_err_to_name(coex_err));
    }
    return esp_wifi_connect() == ESP_OK ? 0 : BLE_QIOT_RS_ERR;
}

/* unbind_dev_handle 由 LLSync 解绑事件触发，清除密钥和 WiFi 后重启。 */
void unbind_dev_handle(void)
{
    /* 旧客户端误把换网当绑定/解绑时拒绝清除原设备身份。 */
    if (atomic_load(&s_change_mode) || ur_network_protect_credentials()) return;
    ur_bridge_clear_device_info();
    esp_restart();
}

/* ur_provision_result_cb 把 SDK 异步结果转换为调用方可等待的事件。 */
static void ur_provision_result_cb(eWiFiConfigResult event, void *user_data)
{
    (void)user_data;
    if (s_result_event == NULL) {
        return;
    }
    if (event == RESULT_WIFI_CONFIG_SUCCESS) {
        ESP_LOGI(UR_TAG, "provisioning succeeded");
        xEventGroupSetBits(s_result_event, UR_PROV_SUCCESS_BIT);
    } else {
        ESP_LOGE(UR_TAG, "provisioning ended with result=%d", event);
        xEventGroupSetBits(s_result_event, UR_PROV_FAIL_BIT);
    }
}

bool ur_ble_provision_start(uint32_t timeout_ms)
{
    /* 配网和绑定阶段也强制使用发布日志级别，避免 SDK 调试日志输出请求内容。 */
    IOT_Log_Set_Level(eLOG_INFO);
    s_result_event = xEventGroupCreate();
    if (s_result_event == NULL) {
        return false;
    }
    s_provisioning = true;
    sg_gotip_flag = false;
    s_sntp_started = false;

    ESP_LOGI(UR_TAG, "start LLSync BLE provisioning");
    int result = qiot_wifi_config_start(WIFI_CONFIG_TYPE_BT_COMBO, NULL, ur_provision_result_cb);
    if (result != UNITEDRHINO_RET_SUCCESS) {
        ESP_LOGE(UR_TAG, "start BLE provisioning failed: %d", result);
        s_provisioning = false;
        vEventGroupDelete(s_result_event);
        s_result_event = NULL;
        return false;
    }

    /* HAL 任务使用同一 Kconfig 超时；调用方多等待 1 秒，让 HAL 先完成超时回调和
     * BLE 结果通知，避免外层抢先释放仍被后台任务使用的事件组与蓝牙栈。 */
    EventBits_t bits = 0;
    const TickType_t started = xTaskGetTickCount();
    do {
        if (s_rebind_mode) ur_change_apply_candidate();
        bits = xEventGroupWaitBits(s_result_event, UR_PROV_SUCCESS_BIT | UR_PROV_FAIL_BIT,
                                  pdTRUE, pdFALSE, pdMS_TO_TICKS(100));
    } while (!bits && (xTaskGetTickCount() - started) < pdMS_TO_TICKS(timeout_ms + 1000));
    /* 成功回调先于 BLE 绑定结果通知返回；预留通知发送时间，再停止后台任务。
     * 超时路径也等待一个轮询周期，避免任务仍访问 Bluedroid 时释放控制器。 */
    if ((bits & UR_PROV_SUCCESS_BIT) != 0) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    qiot_wifi_config_stop();
    vTaskDelay(pdMS_TO_TICKS(250));
    esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
    s_provisioning = false;
    vEventGroupDelete(s_result_event);
    s_result_event = NULL;
    return (bits & UR_PROV_SUCCESS_BIT) != 0;
}

/* ur_ble_rebind_start 仅提交已完成 SDK 绑定的当前候选；异常重启由原事务恢复旧网络。 */
bool ur_ble_rebind_start(uint32_t timeout_ms)
{
    s_rebind_mode = true;
    bool success = ur_ble_provision_start(timeout_ms);
    if (success) {
        wifi_config_t config = {0};
        char ssid[33] = {0};
        success = sg_gotip_flag && esp_wifi_get_config(WIFI_IF_STA, &config) == ESP_OK;
        memcpy(ssid, config.sta.ssid, 32);
        if (success) success = ur_network_commit_connected(ssid) == UR_NETWORK_COMMITTED;
        memset(&config, 0, sizeof(config));
    }
    if (!success) ur_network_fail(UR_NETWORK_ERROR_NETWORK);
    s_rebind_mode = false;
    return success;
}

void ur_ble_provision_deinit_bt(void)
{
    if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_ENABLED) {
        esp_bluedroid_disable();
    }
    if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_INITIALIZED) {
        esp_bluedroid_deinit();
    }
    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED) {
        esp_bt_controller_disable();
    }
    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_INITED) {
        esp_bt_controller_deinit();
    }
    esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
}

/* ur_change_notify 扩展 E5：长度 12，版本/模式/阶段/错误各 1 字节，加 8 字节请求 ID。 */
static void ur_change_notify(void)
{
    if (s_trial_mode || !ble_is_connected()) return;
    ur_network_status_t status = {0};
    ur_network_status(&status);
    uint8_t packet[15] = {0xe5, 0, 12, 2, 1, status.stage, status.error};
    for (unsigned i = 0; i < 8; ++i) {
        unsigned char high = status.request_id[i * 2];
        unsigned char low = status.request_id[i * 2 + 1];
        high = high <= '9' ? high - '0' : high - 'a' + 10;
        low = low <= '9' ? low - '0' : low - 'a' + 10;
        packet[7 + i] = (uint8_t)((high << 4) | low);
    }
    ble_send_notify(packet, sizeof(packet));
}

/* ur_change_worker 独占 Wi-Fi/BLE，250ms 检查事务时限，不依赖 MQTT 重连循环。 */
static void ur_change_worker(void *argument)
{
    (void)argument;
    IOT_Log_Set_Level(eLOG_INFO);
    wifi_init_sta();
    if (s_trial_mode) {
        char ssid[33] = {0}, password[65] = {0};
        wifi_config_t config = {0};
        if (!ur_network_candidate(ssid, password)) {
            if (ur_network_fail(UR_NETWORK_ERROR_STORAGE) == UR_NETWORK_ROLLBACK) esp_restart();
            vTaskDelete(NULL);
            return;
        }
        memcpy(config.sta.ssid, ssid, strnlen(ssid, 32));
        memcpy(config.sta.password, password, strnlen(password, 64));
        config.sta.threshold.authmode = password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        config.sta.pmf_cfg.capable = true;
        esp_err_t error = esp_wifi_set_config(WIFI_IF_STA, &config);
        memset(password, 0, sizeof(password));
        memset(&config, 0, sizeof(config));
        atomic_store(&s_connect_requested, error == ESP_OK);
        if (error != ESP_OK || esp_wifi_connect() != ESP_OK) {
            if (ur_network_fail(UR_NETWORK_ERROR_NETWORK) == UR_NETWORK_ROLLBACK) esp_restart();
        }
    } else {
        ble_qiot_service_init();
        if (esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_ENABLED) {
            if (ur_network_fail(UR_NETWORK_ERROR_NETWORK) == UR_NETWORK_ROLLBACK) esp_restart();
        }
    }
    /* 事件会提前唤醒 worker，通知频率必须按时间而不是循环次数控制。 */
    TickType_t last_notify_at = xTaskGetTickCount();
    bool completed = false; /* 保存完成后保留 30 秒 BLE 结果窗口，允许手机漏收后重连。 */
    TickType_t completed_at = 0; /* 按单调时间计时，不能被频繁 Wi-Fi 事件提前耗尽窗口。 */
    for (;;) {
        EventBits_t events = xEventGroupWaitBits(s_change_events, UR_CHANGE_IP | UR_CHANGE_DISCONNECTED,
                                               pdTRUE, pdFALSE, pdMS_TO_TICKS(250));
        ur_change_apply_candidate();
        if ((events & UR_CHANGE_DISCONNECTED) && s_ip_callback) s_ip_callback(false, s_ip_context);
        if (events & UR_CHANGE_IP) {
            if (s_trial_mode) {
                if (s_ip_callback) s_ip_callback(true, s_ip_context);
            } else if (!completed && atomic_load(&s_connect_requested) && sg_gotip_flag) {
                /* 读取真实关联 AP，而非仅信任旧 IP 事件或已请求的驱动配置。 */
                wifi_ap_record_t ap = {0};
                char connected_ssid[33] = {0};
                bool associated = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
                if (associated) memcpy(connected_ssid, ap.ssid, 32);
                int committed = associated ? ur_network_commit_connected(connected_ssid)
                                           : ur_network_fail(UR_NETWORK_ERROR_NETWORK);
                if (committed == UR_NETWORK_COMMITTED) {
                    completed = true;
                    completed_at = xTaskGetTickCount();
                    ur_change_report_result();
                } else if (committed == UR_NETWORK_ROLLBACK || committed == UR_NETWORK_FAULT) {
                    ur_change_report_result();
                    if (committed == UR_NETWORK_ROLLBACK) {
                        vTaskDelay(pdMS_TO_TICKS(500));
                        esp_restart();
                    }
                }
            }
        }
        if (completed && (TickType_t)(xTaskGetTickCount() - completed_at) >= pdMS_TO_TICKS(30000)) {
            atomic_store(&s_connect_requested, false);
            ur_ble_provision_deinit_bt();
            esp_restart();
        }
        int result = ur_network_tick();
        if (result == UR_NETWORK_ROLLBACK) esp_restart();
        if (result == UR_NETWORK_FAULT) {
            atomic_store(&s_connect_requested, false);
            esp_wifi_disconnect();
            if (!s_trial_mode) ur_ble_provision_deinit_bt();
            vTaskDelete(NULL);
            return;
        }
        if ((TickType_t)(xTaskGetTickCount() - last_notify_at) >= pdMS_TO_TICKS(1000)) {
            ur_change_notify();
            if (completed && ble_is_connected()) ur_change_report_result();
            last_notify_at = xTaskGetTickCount();
        }
    }
}

bool ur_ble_network_start(bool trial, ur_network_ip_callback_t callback, void *context)
{
    if (atomic_exchange(&s_change_mode, true)) return false;
    s_trial_mode = trial;
    s_ip_callback = callback;
    s_ip_context = context;
    s_change_events = xEventGroupCreate();
    if (!s_change_events) { atomic_store(&s_change_mode, false); return false; }
    if (xTaskCreate(ur_change_worker, "ur_network", 8192, NULL, 3, NULL) != pdPASS) {
        vEventGroupDelete(s_change_events);
        s_change_events = NULL;
        atomic_store(&s_change_mode, false);
        return false;
    }
    return true;
}
