/* Watcher 拍照纯逻辑单元测试：校验门禁、JPEG、URL、响应与凭据清零。 */
#include "ur_photo_runtime.h"

#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {
/* Check 输出稳定断言名称并累计失败数。 */
void Check(bool condition, std::string_view name, int* failures) {
    if (!condition) {
        std::cerr << "FAIL: " << name << '\n';
        ++*failures;
    }
}

/* TestGate 覆盖 token 去重、相机忙、断网、OTA 和完成恢复。 */
void TestGate(int* failures) {
    UrPhotoGate gate(2);
    Check(gate.Start("", true, false) == UrPhotoStartResult::kInvalidToken, "empty-token",
          failures);
    Check(gate.Start("offline", false, false) == UrPhotoStartResult::kOffline, "offline",
          failures);
    Check(gate.Start("ota", true, true) == UrPhotoStartResult::kOtaBusy, "ota", failures);
    Check(gate.Start("photo-1", true, false) == UrPhotoStartResult::kAccepted, "accepted",
          failures);
    Check(gate.Start("photo-2", true, false) == UrPhotoStartResult::kBusy, "busy", failures);
    gate.Finish();
    Check(gate.Start("photo-1", true, false) == UrPhotoStartResult::kDuplicate, "duplicate",
          failures);
    Check(gate.Start("photo-2", true, false) == UrPhotoStartResult::kAccepted, "next",
          failures);
    gate.Finish();
    Check(gate.Start("photo-3", true, false) == UrPhotoStartResult::kAccepted, "history-roll",
          failures);
    gate.Finish();
    Check(gate.Start("photo-1", true, false) == UrPhotoStartResult::kAccepted, "history-evicted",
          failures);
}

/* TestJpegAndResponse 覆盖图片边界和三重上传成功条件。 */
void TestJpegAndResponse(int* failures) {
    const uint8_t jpeg[] = {0xff, 0xd8, 0xff, 0xe0, 0xff, 0xd9};
    Check(UrPhotoValidateJpeg(jpeg, sizeof(jpeg)), "jpeg-valid", failures);
    Check(!UrPhotoValidateJpeg(jpeg, 4), "jpeg-short", failures);
    Check(!UrPhotoValidateJpeg(nullptr, sizeof(jpeg)), "jpeg-null", failures);
    // 真机复现 EOI 后 33 个零，旧边界检查误拒绝可正常预览的图像。
    for (size_t padding : {size_t(0), size_t(1), size_t(3), size_t(33), size_t(64)}) {
        std::vector<uint8_t> padded(jpeg, jpeg + sizeof(jpeg));
        padded.resize(sizeof(jpeg) + padding, 0);
        Check(UrPhotoJpegPayloadSize(padded.data(), padded.size()) == sizeof(jpeg),
              "jpeg-camera-zero-padding", failures);
        if (padding > 0) {
            Check(!UrPhotoValidateJpeg(padded.data(), padded.size()), "jpeg-upload-stays-strict", failures);
            padded.back() = 1;
            Check(UrPhotoJpegPayloadSize(padded.data(), padded.size()) == 0,
                  "jpeg-nonzero-tail-rejected", failures);
        }
    }
    std::vector<uint8_t> oversized(jpeg, jpeg + sizeof(jpeg));
    oversized.resize(kUrPhotoMaxJpegBytes + 1, 0);
    Check(UrPhotoJpegPayloadSize(oversized.data(), oversized.size()) == 0, "jpeg-padded-oversize", failures);
    const uint8_t invalid[] = {0xff, 0xd8, 0xff, 0xe0, 0, 0};
    Check(UrPhotoJpegPayloadSize(invalid, sizeof(invalid)) == 0, "jpeg-missing-eoi", failures);
    const uint8_t zeros[] = {0, 0, 0};
    Check(UrPhotoJpegPayloadSize(zeros, sizeof(zeros)) == 0, "jpeg-zero-only", failures);
    Check(UrPhotoJpegPayloadSize(nullptr, 0) == 0, "jpeg-normalize-null", failures);
    Check(UrPhotoValidateUploadResponse(200, 200, "https://objects.test/photo.jpg"),
          "response-valid", failures);
    Check(!UrPhotoValidateUploadResponse(500, 200, "x"), "response-http", failures);
    Check(!UrPhotoValidateUploadResponse(200, 500, "x"), "response-code", failures);
    Check(!UrPhotoValidateUploadResponse(200, 200, ""), "response-uri", failures);
}

/* TestUrl 验证相对路径拼接、同源绝对地址与跨域拒绝。 */
void TestUrl(int* failures) {
    std::string result;
    Check(UrPhotoResolvePlatformUrl("https://example.test:7777/base", "", &result) &&
              result == "https://example.test:7777/api/v1/things/device/edge/upload-file",
          "url-cold-boot-device-upload", failures);
    Check(!UrPhotoResolvePlatformUrl("", "", &result), "url-cold-boot-missing-platform", failures);
    Check(!UrPhotoResolvePlatformUrl("https://example.test", "", nullptr),
          "url-cold-boot-missing-output", failures);
    Check(UrPhotoResolvePlatformUrl("https://example.test:7777/base", "/api/upload", &result) &&
              result == "https://example.test:7777/api/upload",
          "url-relative", failures);
    Check(UrPhotoResolvePlatformUrl("https://example.test:7777", "https://example.test:7777/u",
                                    &result),
          "url-same-origin", failures);
    Check(!UrPhotoResolvePlatformUrl("https://example.test:7777", "https://attacker.test/u",
                                     &result),
          "url-cross-origin", failures);
    Check(!UrPhotoResolvePlatformUrl("https://example.test:7777", "//attacker.test/u", &result),
          "url-scheme-relative", failures);
    Check(!UrPhotoResolvePlatformUrl("https://example.test:7777", "relative", &result),
          "url-relative-without-slash", failures);
}

/* TestCredentials 验证上传结束时两个短期认证字段都被抹除。 */
void TestCredentials(int* failures) {
    UrPhotoCredentials credentials;
    std::strcpy(credentials.Username(), "device;appid;conn;expiry");
    std::strcpy(credentials.Password(), "proof;hmacsha256");
    Check(credentials.Valid(), "credentials-valid", failures);
    credentials.Clear();
    bool cleared = true;
    for (size_t i = 0; i < credentials.UsernameCapacity(); ++i) {
        cleared = cleared && credentials.Username()[i] == '\0';
    }
    for (size_t i = 0; i < credentials.PasswordCapacity(); ++i) {
        cleared = cleared && credentials.Password()[i] == '\0';
    }
    Check(cleared && !credentials.Valid(), "credentials-cleared", failures);
}
}  // namespace

/* main 执行全部纯逻辑用例并返回失败数量。 */
int main() {
    int failures = 0;
    TestGate(&failures);
    TestJpegAndResponse(&failures);
    TestUrl(&failures);
    TestCredentials(&failures);
    if (failures == 0) {
        std::cout << "PASS photo: gate jpeg url response credentials\n";
    }
    return failures;
}
