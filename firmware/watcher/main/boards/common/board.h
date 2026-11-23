#ifndef BOARD_H
#define BOARD_H

#include <http.h>
#include <web_socket.h>
#include <mqtt.h>
#include <udp.h>
#include <string>
#include <functional>
#include <network_interface.h>

#include "led/led.h"
#include "backlight.h"
#include "camera.h"
#include "assets.h"
#include <cJSON.h>

/**
 * Network events for unified callback
 */
enum class NetworkEvent {
    Scanning,              // Network is scanning (WiFi scanning, etc.)
    Connecting,            // Network is connecting (data: SSID/network name)
    Connected,             // Network connected successfully (data: SSID/network name)
    Disconnected,          // Network disconnected
    WifiConfigModeEnter,   // Entered WiFi configuration mode
    WifiConfigModeExit,    // Exited WiFi configuration mode
    // Cellular modem specific events
    ModemDetecting,        // Detecting modem (baud rate, module type)
    ModemErrorNoSim,       // No SIM card detected
    ModemErrorRegDenied,   // Network registration denied
    ModemErrorInitFailed,  // Modem initialization failed
    ModemErrorTimeout      // Operation timeout
};

// Power save level enumeration
enum class PowerSaveLevel {
    LOW_POWER,    // Maximum power saving (lowest power consumption)
    BALANCED,     // Medium power saving (balanced)
    PERFORMANCE,  // No power saving (maximum power consumption / full performance)
};

// Network event callback type (event, data)
// data contains additional info like SSID for Connecting/Connected events
using NetworkEventCallback = std::function<void(NetworkEvent event, const std::string& data)>;

void* create_board();
class AudioCodec;
class Display;
class Board {
private:
    Board(const Board&) = delete; // 禁用拷贝构造函数
    Board& operator=(const Board&) = delete; // 禁用赋值操作

protected:
    Board();
    std::string GenerateUuid();

    // 软件生成的设备唯一标识
    std::string uuid_;

public:
    static Board& GetInstance() {
        static Board* instance = static_cast<Board*>(create_board());
        return *instance;
    }

    virtual ~Board() = default;
    virtual std::string GetBoardType() = 0;
    virtual std::string GetUuid() { return uuid_; }
    virtual Backlight* GetBacklight() { return nullptr; }
    virtual Led* GetLed();
    virtual AudioCodec* GetAudioCodec() = 0;
    virtual bool GetTemperature(float& esp32temp);
    virtual Display* GetDisplay();
    virtual Camera* GetCamera();
    virtual NetworkInterface* GetNetwork() = 0;
    virtual void StartNetwork() = 0;
    // PresentNetworkChange 在应用任务展示物理确认；不支持该能力的板型安全拒绝。
    virtual bool PresentNetworkChange(const std::string& request_id) { (void)request_id; return false; }
    virtual void SetNetworkEventCallback(NetworkEventCallback callback) { (void)callback; }
    virtual const char* GetNetworkStateIcon() = 0;
    virtual bool GetBatteryLevel(int &level, bool& charging, bool& discharging);
    virtual std::string GetSystemInfoJson();
    virtual void SetPowerSaveLevel(PowerSaveLevel level) = 0;
    virtual std::string GetBoardJson() = 0;
    virtual std::string GetDeviceStatusJson() = 0;

    // 联犀物模型扩展点：属性/行为/控制随板型差异大，协议层不认识具体板，
    // 默认实现保持 Watcher 既有行为（五属性上报、Volume/Brightness 控制、行为不支持）。
    // 返回非空表示本板完全接管属性 params JSON；空串走默认五属性格式。
    virtual std::string GetUrPropertiesJson() { return ""; }
    // 处理本板支持的属性控制（LightMode/Speed 等）；返回 -1 表示不接管，走默认
    // Volume/Brightness 路径；>=0 表示已接管，返回值为处理属性数，reply_data
    // 填 controlReply 的 data JSON 对象文本。
    virtual int HandleUrControl(const cJSON* params, std::string* reply_data) {
        (void)params; (void)reply_data; return -1;
    }
    // 处理本板行为（坦克动作等）；返回 HTTP 风格 code，404 表示本板不支持。
    virtual int HandleUrAction(const std::string& action_id, const std::string& params_json) {
        (void)action_id; (void)params_json; return 404;
    }
    // 联犀 MQTT 断连通知：板级需要立即停止底盘等外设运动，避免指令残留。
    virtual void OnUrDisconnected() {}
};

#define DECLARE_BOARD(BOARD_CLASS_NAME) \
void* create_board() { \
    return new BOARD_CLASS_NAME(); \
}

#endif // BOARD_H
