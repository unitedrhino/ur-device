/* 联犀模块桥接层实现：封装 xiaozhi C++ 单例调用，供联犀 C 模块使用
 *
 * 涉及的线程约束：LVGL 显示与音频编解码操作必须经 Application::Schedule 切回主线程执行，
 * 电池/RSSI 等只读查询可在联犀任务线程直接调用。
 */
#include "ur_bridge.h"

#include <cstring>
#include <atomic>
#include <mutex>
#include <new>
#include <string>

#include "board.h"
#include "application.h"
#include "display.h"
#include "backlight.h"
#include "audio_codec.h"
#include "camera.h"
#include "settings.h"
#include "ota.h"
#include "ur_device.h"
#include "ur_network.h"
#include "ur_photo_runtime.h"
#include "unitedrhino_iot_export.h"
#include "sdkconfig.h"

#include <esp_app_desc.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>

#include <wifi_manager.h>
#include <ssid_manager.h>
#if CONFIG_UR_AI_SIGNALING && CONFIG_UR_AI_SIGNALING_WEBRTC
#include "ur_signaling_call.h"
#endif

extern "C" {

/* OTA 状态由 worker 写、MQTT 任务读，用互斥锁保证一次读取到一致快照。 */
static std::mutex s_ota_mutex;
static ur_ota_status_t s_ota_status = {0, 0, 0};
/* 启动门禁按“独占锁 → OTA/相机状态锁 → 换网锁”的顺序，避免检查后竞态。 */
static std::mutex s_operation_mutex;

#if CONFIG_UR_AI
/* 拍照门禁与 worker 输入只在本模块存在，避免相机状态泄漏到 MQTT 回调。 */
static std::mutex s_photo_mutex;
static UrPhotoGate s_photo_gate;
static std::atomic<uint32_t> s_photo_sequence{0};

/* UrPhotoRequestKind 区分云端 actionReply 与本地双击 inputSend 两条结果路径。 */
enum class UrPhotoRequestKind {
    kAction,
    kImageInput,
};

struct UrPhotoRequest {
    UrPhotoRequestKind kind;
    std::string token;
    uint32_t generation;  // 本地显示代次，不使用云端 Token 或图片地址关联界面。
};

/* ur_photo_process 完成捕获、认证、上传和回送，正常返回以析构所有局部 C++ 对象。 */
static void ur_photo_process(void *argument)
{
    auto *request = static_cast<UrPhotoRequest *>(argument);
    auto finish = [request](int code, const char *message, const char *file_uri) {
        ESP_LOGI("ur_photo", "stage=complete code=%d", code);
        Board::GetInstance().GetDisplay()->SetPhotoStage(request->generation,
            code == 200 ? PhotoUiStage::Waiting : code == 500 ? PhotoUiStage::CaptureFailed :
            code == 502 ? PhotoUiStage::UploadFailed : PhotoUiStage::Unavailable);
        {
            std::lock_guard<std::mutex> lock(s_photo_mutex);
            s_photo_gate.Finish();
        }
        if (request->kind == UrPhotoRequestKind::kAction) {
            if (!ur_device_photo_complete(request->token.c_str(), code, message, file_uri)) {
                ESP_LOGW("ur_photo", "photo result was not queued");
            }
        } else {
            Application::GetInstance().CompleteUrPhotoInput(
                code, file_uri != nullptr ? file_uri : "");
        }
        delete request;
    };

    std::string upload_url;
    if (!UrPhotoResolvePlatformUrl(CONFIG_UR_PLATFORM_BASE_URL,
                                   Application::GetInstance().GetUrAiUploadUrl(), &upload_url)) {
        finish(503, "upload endpoint unavailable", nullptr);
        return;
    }
    auto *camera = Board::GetInstance().GetCamera();
    if (camera == nullptr || !camera->Capture()) {
        finish(500, "capture failed", nullptr);
        return;
    }
    ESP_LOGI("ur_photo", "stage=captured");
    Board::GetInstance().GetDisplay()->SetPhotoStage(request->generation, PhotoUiStage::Uploading);

    UrPhotoCredentials credentials;
    ESP_LOGI("ur_photo", "stage=auth_start");
    if (!ur_device_copy_http_auth(credentials.Username(), credentials.UsernameCapacity(),
                                  credentials.Password(), credentials.PasswordCapacity()) ||
        !credentials.Valid()) {
        credentials.Clear();
        finish(503, "authentication unavailable", nullptr);
        return;
    }
    auto uploaded = camera->UploadToPlatform(upload_url, credentials.Username(),
                                             credentials.Password());
    credentials.Clear();
    if (!uploaded) {
        ESP_LOGW("ur_photo", "upload rejected: %s", uploaded.error().c_str());
        finish(502, "upload failed", nullptr);
        return;
    }
    finish(200, "success", uploaded->c_str());
}

/* ur_photo_worker 在处理函数完成资源析构后自删，避免 RTOS 删除任务跳过栈展开。 */
static void ur_photo_worker(void *argument)
{
    ur_photo_process(argument);
    vTaskDeleteWithCaps(nullptr);
}
#endif

/* OTA worker 的不可变输入，在任务结束时释放。 */
struct UrOtaRequest {
    std::string url;
    std::string version;
    std::string md5;
    size_t size;
};

void ur_bridge_save_wifi(const char *ssid, const char *password)
{
    auto &ssid_manager = SsidManager::GetInstance();
    ssid_manager.AddSsid(ssid, password);
}

void ur_bridge_clear_wifi(void) {
    auto& ssid_manager = SsidManager::GetInstance();
    ssid_manager.Clear();
}

bool ur_bridge_load_preprovision_wifi(void)
{
    /* 预置凭据存于 ur_dev namespace（产线烧录或测试脚本经 esptool write-flash 注入），
     * BLE 配网不可用期间用此方式让设备直接联网 */
    Settings settings("ur_dev", false);
    std::string ssid = settings.GetString("ssid");
    std::string pass = settings.GetString("pass");
    if (ssid.empty()) {
        return false;
    }
    auto &ssid_manager = SsidManager::GetInstance();
    if (ssid_manager.GetSsidList().empty()) {
        ssid_manager.AddSsid(ssid, pass);
    }
    return true;
}

void ur_bridge_clear_device_info(void)
{
    /* 清除联犀设备密钥（NVS namespace ur_dev，见 HAL_Device_freertos.c） */
    Settings settings("ur_dev", true);
    settings.EraseAll();

    /* 清除已保存的 WiFi 凭据，重启后重新进入蓝牙配网 */
    ur_bridge_clear_wifi();

    /* 云端解绑后必须重新进入 BLE，而不是沿用一次性的热点回退标志。 */
    Settings wifi_settings("wifi", true);
    wifi_settings.SetInt("force_ap", 0);
}

bool ur_bridge_clear_device_secret(void)
{
    DeviceInfo device_info = {};
    if (HAL_GetDevInfo(&device_info) != UNITEDRHINO_RET_SUCCESS) {
        return false;
    }
    device_info.device_secret[0] = '\0';
    return HAL_SetDevInfo(&device_info) == UNITEDRHINO_RET_SUCCESS;
}

bool ur_bridge_get_battery(int *level, bool *charging)
{
    bool discharging = false;
    return Board::GetInstance().GetBatteryLevel(*level, *charging, discharging);
}

int ur_bridge_get_volume(void)
{
    auto *codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr) {
        return 0;
    }
    return codec->output_volume();
}

void ur_bridge_set_volume(int volume)
{
    if (volume < 0) {
        volume = 0;
    }
    if (volume > 100) {
        volume = 100;
    }
    Application::GetInstance().Schedule([volume]() {
        auto *codec = Board::GetInstance().GetAudioCodec();
        if (codec != nullptr) {
            codec->SetOutputVolume(volume);
        }
    });
}

/* 从背光实际状态读取，覆盖菜单调整、持久化恢复和渐变完成后的亮度。 */
int ur_bridge_get_brightness(void)
{
    auto *backlight = Board::GetInstance().GetBacklight();
    return backlight != nullptr ? backlight->brightness() : 0;
}

/* 联犀下行亮度在应用任务生效；上报不提前宣称尚未执行的目标值。 */
void ur_bridge_set_brightness(int brightness)
{
    if (brightness < 0) {
        brightness = 0;
    }
    if (brightness > 100) {
        brightness = 100;
    }
    Application::GetInstance().Schedule([brightness]() {
        auto *backlight = Board::GetInstance().GetBacklight();
        if (backlight != nullptr) {
            backlight->SetBrightness(brightness);
        }
    });
}

int ur_bridge_get_rssi(void)
{
    auto &wifi_manager = WifiManager::GetInstance();
    if (!wifi_manager.IsConnected()) {
        return 0;
    }
    return wifi_manager.GetRssi();
}

int ur_bridge_get_properties_json(char *buf, size_t size)
{
    if (buf == NULL || size == 0) {
        return 0;
    }
    std::string json = Board::GetInstance().GetUrPropertiesJson();
    if (json.empty()) {
        return 0;
    }
    if (json.size() + 1 > size) {
        return -(int)(json.size() + 1);
    }
    memcpy(buf, json.c_str(), json.size() + 1);
    return (int)json.size();
}

int ur_bridge_handle_control(const cJSON *params, char *reply_data, size_t reply_size)
{
    if (reply_data == NULL || reply_size == 0) {
        return -1;
    }
    std::string reply;
    int handled = Board::GetInstance().HandleUrControl(params, &reply);
    if (handled < 0) {
        return handled;
    }
    if (reply.size() + 1 > reply_size) {
        return -2;
    }
    memcpy(reply_data, reply.c_str(), reply.size() + 1);
    return handled;
}

int ur_bridge_handle_action(const char *action_id, const char *params_json)
{
#if CONFIG_UR_AI_SIGNALING && CONFIG_UR_AI_SIGNALING_WEBRTC
    if (action_id && std::strcmp(action_id, "CallDevice") == 0) {
        if (ur_network_protect_credentials()) return 423;
        cJSON* params = cJSON_Parse(params_json ? params_json : "");  // 只接受平台工具已解析的设备目标。
        const cJSON* target = cJSON_GetObjectItemCaseSensitive(params, "target");
        const cJSON* kind = cJSON_GetObjectItemCaseSensitive(params, "callType");
        if (!cJSON_IsObject(params) || cJSON_GetArraySize(params) != 2 ||
            !cJSON_IsString(target) || !cJSON_IsString(kind) ||
            (std::strcmp(kind->valuestring, "voice") != 0 && std::strcmp(kind->valuestring, "video") != 0)) {
            cJSON_Delete(params);
            return 400;
        }
        const bool accepted = UrSignalingCall::Instance().Dial(target->valuestring,
                                      std::strcmp(kind->valuestring, "voice") == 0);
        cJSON_Delete(params);
        return accepted ? 200 : 409;  // 仅确认拨号排队，振铃和接通由正式通话状态确认。
    }
#endif
    return Board::GetInstance().HandleUrAction(action_id != NULL ? action_id : "",
                                               params_json != NULL ? params_json : "");
}

void ur_bridge_notify_disconnected(void)
{
    Board::GetInstance().OnUrDisconnected();
}

void ur_bridge_show_text(const char *text)
{
    std::string content = text;
    Application::GetInstance().Schedule([content]() {
        auto *display = Board::GetInstance().GetDisplay();
        if (display != nullptr) {
            display->ShowNotification(content.c_str(), 5000);
        }
    });
}

#if CONFIG_UR_AI
/* ur_start_photo_worker 对两条拍照入口使用同一门禁和同一 worker。 */
static int ur_start_photo_worker(UrPhotoRequestKind kind, const char *token)
{
    std::lock_guard<std::mutex> operation(s_operation_mutex);
    if (ur_network_protect_credentials()) return 423;
    ur_ota_status_t ota = {0};
    ur_bridge_get_ota_status(&ota);
    UrPhotoStartResult start;
    {
        std::lock_guard<std::mutex> lock(s_photo_mutex);
        start = s_photo_gate.Start(token != nullptr ? token : "", ur_device_is_connected(),
                                   ota.state == 1 || ota.state == 2);
    }
    switch (start) {
        case UrPhotoStartResult::kAccepted:
            break;
        case UrPhotoStartResult::kInvalidToken:
            return 400;
        case UrPhotoStartResult::kBusy:
        case UrPhotoStartResult::kDuplicate:
            return 409;
        case UrPhotoStartResult::kOtaBusy:
            return 423;
        case UrPhotoStartResult::kOffline:
            return 503;
    }

    // 两个入口共享显示代次；先记录开始再启动 worker，避免进度先于开始入队。
    static std::atomic<uint32_t> ui_sequence{0};
    uint32_t generation = ui_sequence.fetch_add(1) + 1;
    if (generation == 0) generation = ui_sequence.fetch_add(1) + 1;
    auto *request = new (std::nothrow) UrPhotoRequest{kind, token, generation};
    if (request) Board::GetInstance().GetDisplay()->SetPhotoStage(generation, PhotoUiStage::Capturing);
    /* 捕获和 HTTP 上传不直接写 Flash，任务栈可用 PSRAM，为相机 DMA 留出内部内存。 */
    if (request == nullptr ||
        xTaskCreateWithCaps(ur_photo_worker, "ur_photo", 8192, request, 4, nullptr,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        delete request;
        Board::GetInstance().GetDisplay()->SetPhotoStage(generation, PhotoUiStage::Unavailable);
        std::lock_guard<std::mutex> lock(s_photo_mutex);
        s_photo_gate.Finish();
        return 503;
    }
    return 0;
}

int ur_bridge_start_photo_action(const char *token)
{
    return ur_start_photo_worker(UrPhotoRequestKind::kAction, token);
}

int ur_bridge_start_photo_input(void)
{
    char token[32] = {0};
    const uint32_t sequence = s_photo_sequence.fetch_add(1) + 1;
    snprintf(token, sizeof(token), "input-%08lx", static_cast<unsigned long>(sequence));
    return ur_start_photo_worker(UrPhotoRequestKind::kImageInput, token);
}

bool ur_bridge_photo_busy(void)
{
    std::lock_guard<std::mutex> lock(s_photo_mutex);
    return s_photo_gate.Busy();
}
#endif

/* ur_ota_worker 执行下载与分区切换，避免阻塞联犀 MQTT 的 Yield 主循环。 */
static void ur_ota_worker(void *arg)
{
    auto *request = static_cast<UrOtaRequest *>(arg);
#if CONFIG_UR_AI
    /* 让 MQTT 任务先发出已排队的 sessionClose，并完成一次下行 Yield。 */
    vTaskDelay(pdMS_TO_TICKS(1000));
#endif
    OtaUpgradeResult result = Ota::UpgradeVerified(
        request->url, request->size, request->md5, request->version,
        [](int progress, size_t) {
            std::lock_guard<std::mutex> lock(s_ota_mutex);
            s_ota_status.progress = progress < 1 ? 1 : (progress > 99 ? 99 : progress);
        });

    {
        std::lock_guard<std::mutex> lock(s_ota_mutex);
        if (result == OtaUpgradeResult::kSuccess) {
            Settings settings("ur_ota", true);
            settings.SetString("pending", request->version);
            s_ota_status.state = 2;
            s_ota_status.progress = 100;
            s_ota_status.error_code = 0;
        } else {
            s_ota_status.state = 3;
            s_ota_status.error_code = static_cast<int>(result);
        }
    }

    if (result != OtaUpgradeResult::kSuccess) {
        Application::GetInstance().RecoverFromPlatformOta();
    }

    delete request;
    vTaskDelete(nullptr);
}

bool ur_bridge_start_ota(const char *url, const char *version, size_t size, const char *md5)
{
    std::lock_guard<std::mutex> operation(s_operation_mutex);
    if (ur_network_protect_credentials()) return false;
    if (url == nullptr || version == nullptr || md5 == nullptr || url[0] == '\0' ||
        version[0] == '\0' || md5[0] == '\0' || size == 0) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(s_ota_mutex);
        if (s_ota_status.state == 1 || s_ota_status.state == 2) {
            return false;
        }
    }

    std::string resolved_url(url);
    if (resolved_url.front() == '/') {
        std::string base_url(CONFIG_UR_PLATFORM_BASE_URL);
        if (base_url.empty()) {
            return false;
        }
        while (!base_url.empty() && base_url.back() == '/') {
            base_url.pop_back();
        }
        resolved_url = base_url + resolved_url;
    }
    if (resolved_url.rfind("http://", 0) != 0 && resolved_url.rfind("https://", 0) != 0) {
        return false;
    }

    auto *request = new (std::nothrow) UrOtaRequest{resolved_url, version, md5, size};
    if (request == nullptr) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(s_ota_mutex);
        s_ota_status = {1, 1, 0};
    }
#if CONFIG_UR_AI
    if (!Application::GetInstance().PrepareForPlatformOta()) {
        std::lock_guard<std::mutex> lock(s_ota_mutex);
        s_ota_status = {3, 0, -1};
        delete request;
        Application::GetInstance().RecoverFromPlatformOta();
        return false;
    }
    /* AudioService::Stop 让音频任务自行退出；给调度器一个有界窗口回收它们的
     * 内部 RAM 栈，再申请 OTA worker，避免语音版设备稳定复现 pdFAIL。 */
    vTaskDelay(pdMS_TO_TICKS(250));
#endif
    if (xTaskCreate(ur_ota_worker, "ur_ota", 10240, request, 4, nullptr) != pdPASS) {
        std::lock_guard<std::mutex> lock(s_ota_mutex);
        s_ota_status = {3, 0, -1};
        delete request;
#if CONFIG_UR_AI
        Application::GetInstance().RecoverFromPlatformOta();
#endif
        return false;
    }
    return true;
}

/* ur_bridge_begin_network_change 与相机/OTA 的启动检查共用独占锁，封住跨任务同时受理。 */
bool ur_bridge_begin_network_change(const char *request_id)
{
    std::lock_guard<std::mutex> operation(s_operation_mutex);
    ur_ota_status_t ota{};
    ur_bridge_get_ota_status(&ota);
    if (ota.state == 1 || ota.state == 2) return false;
#if CONFIG_UR_AI
    if (ur_bridge_photo_busy()) return false;
#endif
    return ur_network_begin(request_id);
}

/* ur_bridge_prepare_network_change 原子预留确认窗口，后续 OTA/拍照不得抢占。 */
int ur_bridge_prepare_network_change(const char *request_id)
{
    std::lock_guard<std::mutex> operation(s_operation_mutex);
    ur_ota_status_t ota{};
    ur_bridge_get_ota_status(&ota);
    if (ota.state == 1 || ota.state == 2) return 423;
#if CONFIG_UR_AI
    if (ur_bridge_photo_busy()) return 423;
#endif
    return ur_network_prepare_request(request_id);
}

/* ur_bridge_present_network_change 必须在 actionReply 成功发布后调用，UI 只在应用任务展示。 */
void ur_bridge_present_network_change(const char *request_id)
{
    if (!ur_network_activate_request(request_id)) return;
    std::string id(request_id);
    Application::GetInstance().Schedule([id]() {
        if (!Board::GetInstance().PresentNetworkChange(id)) {
            ur_network_cancel_request(id.c_str(), UR_NETWORK_ERROR_CANCELLED);
            ur_device_request_report();
        }
    });
}

void ur_bridge_get_ota_status(ur_ota_status_t *status)
{
    if (status == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(s_ota_mutex);
    *status = s_ota_status;
}

bool ur_bridge_get_pending_ota_version(char *version, size_t version_size)
{
    if (version == nullptr || version_size == 0) {
        return false;
    }
    Settings settings("ur_ota", false);
    std::string pending = settings.GetString("pending");
    if (pending.empty() || pending.size() >= version_size) {
        version[0] = '\0';
        return false;
    }
    std::memcpy(version, pending.c_str(), pending.size() + 1);
    return true;
}

bool ur_bridge_confirm_ota(const char *version)
{
    if (version == nullptr || version[0] == '\0') {
        return false;
    }

    Settings settings("ur_ota", true);
    std::string pending = settings.GetString("pending");
    const esp_app_desc_t *description = esp_app_get_description();
    if (pending.empty() || pending != version || pending != description->version) {
        return false;
    }

    Ota ota;
    ota.MarkCurrentVersionValid();
    settings.EraseKey("pending");
    ESP_LOGI("ur_bridge", "platform confirmed firmware version %s", version);
    return true;
}

} /* extern "C" */
