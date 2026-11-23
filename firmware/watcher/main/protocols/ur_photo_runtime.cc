/* 联犀拍照可移植核心实现，不依赖 ESP-IDF，供主机单元测试直接编译。 */
#include "ur_photo_runtime.h"

#include <algorithm>

namespace {
/* SecureZero 使用 volatile 写入，避免编译器删除凭据清零。 */
void SecureZero(char* data, size_t size) {
    volatile char* cursor = data;
    while (size-- > 0) {
        *cursor++ = '\0';
    }
}

/* UrlOrigin 只解析本任务允许的 http/https origin，不接受用户信息或缺失主机。 */
bool UrlOrigin(std::string_view value, std::string_view* origin) {
    size_t scheme_end = value.find("://");
    if (scheme_end == std::string_view::npos) {
        return false;
    }
    const std::string_view scheme = value.substr(0, scheme_end);
    if (scheme != "http" && scheme != "https") {
        return false;
    }
    const size_t authority_start = scheme_end + 3;
    const size_t authority_end = value.find_first_of("/?#", authority_start);
    const size_t end = authority_end == std::string_view::npos ? value.size() : authority_end;
    if (end == authority_start || value.substr(authority_start, end - authority_start).find('@') !=
                                      std::string_view::npos) {
        return false;
    }
    *origin = value.substr(0, end);
    return true;
}
}  // namespace

UrPhotoCredentials::~UrPhotoCredentials() { Clear(); }

char* UrPhotoCredentials::Username() { return username_.data(); }

char* UrPhotoCredentials::Password() { return password_.data(); }

size_t UrPhotoCredentials::UsernameCapacity() const { return username_.size(); }

size_t UrPhotoCredentials::PasswordCapacity() const { return password_.size(); }

bool UrPhotoCredentials::Valid() const {
    return username_.front() != '\0' && password_.front() != '\0' && username_.back() == '\0' &&
           password_.back() == '\0';
}

void UrPhotoCredentials::Clear() {
    SecureZero(username_.data(), username_.size());
    SecureZero(password_.data(), password_.size());
}

UrPhotoGate::UrPhotoGate(size_t history_limit)
    : history_limit_(history_limit == 0 ? 1 : history_limit) {}

UrPhotoStartResult UrPhotoGate::Start(std::string_view token, bool connected, bool ota_busy) {
    if (token.empty() || token.size() > 127) {
        return UrPhotoStartResult::kInvalidToken;
    }
    if (std::find(recent_tokens_.begin(), recent_tokens_.end(), token) != recent_tokens_.end()) {
        return UrPhotoStartResult::kDuplicate;
    }
    if (!connected) {
        return UrPhotoStartResult::kOffline;
    }
    if (ota_busy) {
        return UrPhotoStartResult::kOtaBusy;
    }
    if (busy_) {
        return UrPhotoStartResult::kBusy;
    }
    busy_ = true;
    recent_tokens_.emplace_back(token);
    while (recent_tokens_.size() > history_limit_) {
        recent_tokens_.pop_front();
    }
    return UrPhotoStartResult::kAccepted;
}

void UrPhotoGate::Finish() { busy_ = false; }

bool UrPhotoGate::Busy() const { return busy_; }

bool UrPhotoValidateJpeg(const uint8_t* data, size_t size) {
    return data != nullptr && size >= 5 && size <= kUrPhotoMaxJpegBytes && data[0] == 0xff &&
           data[1] == 0xd8 && data[2] == 0xff && data[size - 2] == 0xff && data[size - 1] == 0xd9;
}

size_t UrPhotoJpegPayloadSize(const uint8_t* data, size_t size) {
    // SSCMA 输出可在 EOI 后附加零填充。仅剥离全零尾部，不放宽上传校验或接受额外内容。
    if (data == nullptr || size > kUrPhotoMaxJpegBytes) return 0;
    while (size > 0 && data[size - 1] == 0) --size;
    return UrPhotoValidateJpeg(data, size) ? size : 0;
}

bool UrPhotoResolvePlatformUrl(std::string_view platform_base, std::string_view upload_url,
                               std::string* resolved) {
    if (resolved == nullptr) {
        return false;
    }
    // 独立拍照可能早于首次语音会话；设备鉴权上传不依赖 sessionCreated。
    // 仅缺省地址使用标准设备接口，显式地址仍必须通过下方同源检查。
    if (upload_url.empty()) {
        upload_url = "/api/v1/things/device/edge/upload-file";
    }
    std::string_view base_origin;
    if (!UrlOrigin(platform_base, &base_origin)) {
        return false;
    }
    std::string_view upload_origin;
    if (UrlOrigin(upload_url, &upload_origin)) {
        if (upload_origin != base_origin) {
            return false;
        }
        *resolved = std::string(upload_url);
        return true;
    }
    if (upload_url.front() != '/' || upload_url.rfind("//", 0) == 0) {
        return false;
    }
    *resolved = std::string(base_origin) + std::string(upload_url);
    return true;
}

bool UrPhotoValidateUploadResponse(int http_status, int platform_code, std::string_view file_uri) {
    return http_status == 200 && platform_code == 200 && !file_uri.empty() &&
           file_uri.size() <= 2048;
}
