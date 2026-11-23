/* 联犀拍照可移植核心：URL、JPEG、上传响应、token 去重与临时凭据生命周期。 */
#ifndef UR_PHOTO_RUNTIME_H
#define UR_PHOTO_RUNTIME_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>

/* Watcher 的 SSCMA JPEG 缓冲固定为 48 KiB，上传前必须再次执行相同上限检查。 */
constexpr size_t kUrPhotoMaxJpegBytes = 48 * 1024;

/* UrPhotoStartResult 描述拍照请求在进入 worker 前的门禁结果。 */
enum class UrPhotoStartResult {
    kAccepted,
    kDuplicate,
    kBusy,
    kOffline,
    kOtaBusy,
    kInvalidToken,
};

/* UrPhotoCredentials 保存一次 HTTP 上传使用的短期 Basic Auth，析构时主动清零。 */
class UrPhotoCredentials {
public:
    static constexpr size_t kUsernameCapacity = 192;
    static constexpr size_t kPasswordCapacity = 80;

    UrPhotoCredentials() = default;
    ~UrPhotoCredentials();
    UrPhotoCredentials(const UrPhotoCredentials&) = delete;
    UrPhotoCredentials& operator=(const UrPhotoCredentials&) = delete;

    /* Username/Password 返回 SDK 可写入的定长缓冲。 */
    char* Username();
    char* Password();
    size_t UsernameCapacity() const;
    size_t PasswordCapacity() const;
    /* Valid 要求两个字段均为非空且已经在边界内终止。 */
    bool Valid() const;
    /* Clear 立即抹除两个字段；上传结束或断线时必须调用。 */
    void Clear();

private:
    std::array<char, kUsernameCapacity> username_{};
    std::array<char, kPasswordCapacity> password_{};
};

/* UrPhotoGate 对 token 去重并保证全局只运行一个相机任务。 */
class UrPhotoGate {
public:
    explicit UrPhotoGate(size_t history_limit = 8);
    /* Start 在离线、OTA、忙、重复和空 token 场景返回稳定拒绝原因。 */
    UrPhotoStartResult Start(std::string_view token, bool connected, bool ota_busy);
    /* Finish 结束当前 worker；token 仍保留在历史中以拒绝重复下行。 */
    void Finish();
    /* Busy 返回是否已有拍照任务运行。 */
    bool Busy() const;

private:
    size_t history_limit_;
    bool busy_ = false;
    std::deque<std::string> recent_tokens_;
};

/* UrPhotoValidateJpeg 校验长度、48 KiB 上限、JPEG SOI 与 EOI。 */
bool UrPhotoValidateJpeg(const uint8_t* data, size_t size);

/* UrPhotoJpegPayloadSize 去除相机传输尾部零填充；返回严格校验后的 JPEG 长度，非法返回 0。 */
size_t UrPhotoJpegPayloadSize(const uint8_t* data, size_t size);

/* UrPhotoResolvePlatformUrl 解析同源上传地址，非法地址返回 false；
 * 无会话地址时使用标准设备上传接口。 */
bool UrPhotoResolvePlatformUrl(std::string_view platform_base, std::string_view upload_url,
                               std::string* resolved);

/* UrPhotoValidateUploadResponse 校验 HTTP、平台 code 与 fileUri 三重成功条件。 */
bool UrPhotoValidateUploadResponse(int http_status, int platform_code, std::string_view file_uri);

#endif  // UR_PHOTO_RUNTIME_H
