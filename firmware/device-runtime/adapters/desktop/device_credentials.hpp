// 联犀设备 MQTT 签名适配：长期密钥仅在本机参与 HMAC，不发到 Broker。
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ur::device {
// DeviceCredentials 是短期认证三元组；不得输出到日志或浏览器页面。
struct DeviceCredentials {
    std::string client_id;  // productID&deviceName。
    std::string username;   // clientID;sdkAppID;connID;expiry。
    std::string password;   // 小写hex HMAC-SHA256;hmacsha256。
};
// device_credentials 与后端 deviceAuth.GenSecretDeviceInfo 对齐；nonce/expiry 由时钟随机源传入。
std::optional<DeviceCredentials> device_credentials(std::string_view product,
                                                    std::string_view device,
                                                    std::string_view secret_base64,
                                                    std::string_view nonce, std::uint64_t expiry);
}  // namespace ur::device
