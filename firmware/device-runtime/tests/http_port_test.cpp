// 可移植HTTPS请求边界回归；不启动网络，不以签名URL或头的存在授予附件读取权限。
#include "ur/device/http_port.hpp"

#include <gtest/gtest.h>

namespace ur::device {
namespace {
// request 构造公开合成HTTPS请求，测试不使用真实平台认证或签名。
HttpRequest request() {
    HttpRequest result;
    result.url = "https://upload.example.invalid/private?signature=synthetic";
    result.headers = {{"Content-Type", "image/png"}, {"Authorization", "Basic synthetic"}};
    result.body_size = 1024;
    return result;
}
TEST(HttpPortTest, AcceptsExplicitBoundedPostAndPut) {
    auto value = request();
    EXPECT_TRUE(valid_http_request(value));
    value.method = HttpMethod::Put;
    value.body_size = 50ULL * 1024 * 1024;
    value.response_limit = 65536;
    value.connect_timeout_ms = 30000;
    value.timeout_ms = 120000;
    EXPECT_TRUE(valid_http_request(value));
    value.url = "https://[::1]:443/object";
    EXPECT_TRUE(valid_http_request(value));
}
TEST(HttpPortTest, RejectsPlaintextUserinfoFragmentAndInjectedUrl) {
    for (const auto* url :
         {"", "https://", "https:///object", "http://localhost/object", "file:///private",
          "https://user:secret@upload.invalid/object", "https://upload.invalid/object#fragment",
          "https://upload.invalid/a b", "https://upload.invalid/\r\nAuthorization: exposed",
          "https://upload.invalid\\evil/object"}) {
        auto value = request();
        value.url = url;
        EXPECT_FALSE(valid_http_request(value));
    }
    auto value = request();
    value.url.push_back('\0');
    EXPECT_FALSE(valid_http_request(value));
    value.url.assign(8193, 'x');
    EXPECT_FALSE(valid_http_request(value));
}
TEST(HttpPortTest, RejectsInvalidBudgetsAndUnknownMethod) {
    auto value = request();
    value.method = static_cast<HttpMethod>(9);
    EXPECT_FALSE(valid_http_request(value));
    value = request();
    value.body_size = 50ULL * 1024 * 1024 + 1;
    EXPECT_FALSE(valid_http_request(value));
    for (const auto limit : {0U, 65537U}) {
        value = request();
        value.response_limit = limit;
        EXPECT_FALSE(valid_http_request(value));
    }
    for (const auto connect : {0U, 30001U}) {
        value = request();
        value.connect_timeout_ms = connect;
        EXPECT_FALSE(valid_http_request(value));
    }
    for (const auto timeout : {9999U, 120001U}) {
        value = request();
        value.timeout_ms = timeout;
        EXPECT_FALSE(valid_http_request(value));
    }
}
TEST(HttpPortTest, RejectsHeaderSmugglingDuplicateAndExcessiveSecrets) {
    for (const auto* name : {"", "Bad Header", "Host", "content-length", "Transfer-Encoding",
                             "Connection", "Expect", "Proxy-Authorization", "Trailer", "TE"}) {
        auto value = request();
        value.headers = {{name, "synthetic"}};
        EXPECT_FALSE(valid_http_request(value));
    }
    for (const auto& secret : {std::string("value\r\nX-Injected: yes"), std::string("a\0b", 3),
                               std::string(4097, 'x')}) {
        auto value = request();
        value.headers = {{"Authorization", secret}};
        EXPECT_FALSE(valid_http_request(value));
    }
    auto value = request();
    value.headers.push_back({"AUTHORIZATION", "other"});
    EXPECT_FALSE(valid_http_request(value));
    value = request();
    value.headers = {{"One", std::string(4096, 'x')}, {"Two", std::string(4096, 'x')}};
    EXPECT_FALSE(valid_http_request(value));
    value.headers.assign(17, {"X-Synthetic", "value"});
    EXPECT_FALSE(valid_http_request(value));
}
}  // namespace
}  // namespace ur::device
