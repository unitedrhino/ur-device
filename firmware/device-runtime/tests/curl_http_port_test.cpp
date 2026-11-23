// 正式libcurl驱动对真实隔离HTTPS端点回归：证书/主机名、字节、取消、超时、重入及错误脱敏。
#include "curl_http_port.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <future>

#include "environment.hpp"
#include "ur/device/content_upload.hpp"

namespace ur::device {
namespace {
// Bytes
// 顺序提供合成公开原件，可选择实际读取失败；不把整包构建要求传给平台接口。
class Bytes final : public HttpBodySource {
   public:
    explicit Bytes(std::size_t count, bool broken = false) : total_(count), broken_(broken) {}
    bool read(std::uint8_t* buffer, std::size_t capacity, std::size_t& count) override {
        if (broken_)
            return false;
        count = std::min(capacity, total_ - read_);
        for (std::size_t i = 0; i < count; ++i)
            buffer[i] = static_cast<std::uint8_t>((read_ + i) % 251);
        read_ += count;
        return true;
    }
    std::size_t read_count() const { return read_; }

   private:
    std::size_t total_;     // 实际源长，不依赖request声明。
    bool broken_;           // 合成源的明确失败端口。
    std::size_t read_ = 0;  // 此worker已读取字节。
};
// GateBytes 有限等待测试线程放行，证明同实例并发perform被拒绝且不混用请求。
class GateBytes final : public HttpBodySource {
   public:
    std::promise<void> entered;        // 首个真实上传已到source回调，不用sleep猜测重入窗口。
    std::shared_future<void> release;  // 测试线程有界释放，不占用MQTT或浏览器线程。
    bool read(std::uint8_t* buffer, std::size_t capacity, std::size_t& count) override {
        if (!started_) {
            started_ = true;
            entered.set_value();
            if (release.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
                return false;
        }
        count = std::min<std::size_t>(capacity, 1 - produced_);
        if (count)
            buffer[0] = 0;
        produced_ += count;
        return true;
    }

   private:
    bool started_ = false;      // 同一源只有原worker访问。
    std::size_t produced_ = 0;  // 单字节真实body进度。
};
// UploadRuntime 只提供本次TLS集成的实际双时钟；平台/模型授权仍是明确fixture。
class UploadRuntime final : public ContentUploadRuntime {
   public:
    std::uint64_t monotonic_ms() const override {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                              std::chrono::steady_clock::now().time_since_epoch())
                                              .count());
    }
    std::uint64_t unix_ms() const override {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                              std::chrono::system_clock::now().time_since_epoch())
                                              .count());
    }
    bool current() const override { return true; }
};
// HttpsTest 必须由维护入口提供本轮HTTPS端点/临时CA，缺失为失败而不是隐式SKIP。
class HttpsTest : public ::testing::Test {
   protected:
    void SetUp() override {
        const auto endpoint = environment_value("UR_TEST_HTTPS_URL");
        const auto ca = environment_value("UR_TEST_HTTPS_CA");
        const auto mismatch = environment_value("UR_TEST_HTTPS_MISMATCH_URL");
        ASSERT_TRUE(endpoint && ca && mismatch);
        base_ = *endpoint;
        ca_ = *ca;
        mismatch_ = *mismatch;
    }
    // make_request 每次生成独立目标/头，不继承其他平台的身份或签名。
    HttpRequest make_request(std::string path, std::size_t size = 0) const {
        HttpRequest request;
        request.url = base_ + path;
        request.body_size = size;
        request.headers = {{"Content-Type", "application/octet-stream"}};
        request.connect_timeout_ms = 1000;
        request.timeout_ms = 3000;
        return request;
    }
    std::string base_;                // 本轮回环HTTPS端点。
    std::string ca_;                  // 本轮自签名CA路径。
    std::string mismatch_;            // 同端点IP但证书只包含localhost，用于真实主机名拒绝。
    std::atomic_bool cancel_{false};  // 原worker生命周期内有效的取消标志。
};
TEST_F(HttpsTest, RealPostAndPutStreamExactBytes) {
    CurlHttpPort port(ca_);
    for (const auto method : {HttpMethod::Post, HttpMethod::Put}) {
        Bytes bytes(262144);
        auto request = make_request("/bytes", 262144);
        request.method = method;
        const auto response = port.perform(request, &bytes, cancel_);
        EXPECT_EQ(response.result, HttpResult::Ok);
        EXPECT_EQ(response.status, 200U);
        EXPECT_EQ(response.body, "{\"verified\":true}");
        EXPECT_EQ(bytes.read_count(), 262144U);
    }
}
TEST_F(HttpsTest, ZeroBodyAndExactResponseBoundary) {
    CurlHttpPort port(ca_);
    auto request = make_request("/limit");
    request.response_limit = 1024;
    const auto response = port.perform(request, nullptr, cancel_);
    EXPECT_EQ(response.result, HttpResult::Ok);
    EXPECT_EQ(response.body.size(), 1024U);
    request.response_limit = 1023;
    const auto large = port.perform(request, nullptr, cancel_);
    EXPECT_EQ(large.result, HttpResult::ResponseTooLarge);
    EXPECT_TRUE(large.body.empty());
    EXPECT_EQ(port.perform(make_request("/bytes"), nullptr, cancel_).result, HttpResult::Ok);
}
TEST_F(HttpsTest, EnforcesTrustChainAndHostnameBeforeReadingSource) {
    Bytes bytes(1);
    CurlHttpPort untrusted;
    auto request = make_request("/bytes", 1);
    EXPECT_EQ(untrusted.perform(request, &bytes, cancel_).result, HttpResult::TlsFailed);
    EXPECT_EQ(bytes.read_count(), 0U);
    CurlHttpPort trusted(ca_);
    request.url = mismatch_ + "/bytes";
    EXPECT_EQ(trusted.perform(request, &bytes, cancel_).result, HttpResult::TlsFailed);
    EXPECT_EQ(bytes.read_count(), 0U);
}
TEST_F(HttpsTest, NeverFollowsRedirectOrReturnsSensitiveErrorBody) {
    CurlHttpPort port(ca_);
    auto request = make_request("/redirect");
    request.headers.push_back({"Authorization", "Basic synthetic-credential"});
    const auto redirect = port.perform(request, nullptr, cancel_);
    EXPECT_EQ(redirect.result, HttpResult::HttpFailed);
    EXPECT_EQ(redirect.status, 307U);
    EXPECT_TRUE(redirect.body.empty());
    const auto checked = port.perform(make_request("/check-redirect"), nullptr, cancel_);
    EXPECT_EQ(checked.result, HttpResult::Ok);
    EXPECT_EQ(checked.body, "{\"redirected\":false}");
    const auto error = port.perform(make_request("/error"), nullptr, cancel_);
    EXPECT_EQ(error.result, HttpResult::HttpFailed);
    EXPECT_EQ(error.status, 403U);
    EXPECT_TRUE(error.body.empty());
}
TEST_F(HttpsTest, RejectsShortAndFailedSourcesThenReleasesPort) {
    CurlHttpPort port(ca_);
    for (const auto broken : {false, true}) {
        Bytes source(2, broken);
        const auto response = port.perform(make_request("/bytes", 3), &source, cancel_);
        EXPECT_EQ(response.result, HttpResult::SourceFailed);
        EXPECT_TRUE(response.body.empty());
    }
    EXPECT_EQ(port.perform(make_request("/bytes", 1), nullptr, cancel_).result,
              HttpResult::Invalid);
    EXPECT_EQ(port.perform(make_request("/bytes"), nullptr, cancel_).result, HttpResult::Ok);
}
TEST_F(HttpsTest, TimeoutAndCancellationHaveBoundedLifetimes) {
    CurlHttpPort port(ca_);
    auto request = make_request("/delay");
    request.connect_timeout_ms = 100;
    request.timeout_ms = 150;
    auto response = port.perform(request, nullptr, cancel_);
    EXPECT_EQ(response.result, HttpResult::Timeout);
    EXPECT_TRUE(response.body.empty());
    cancel_.store(true);
    Bytes bytes(1);
    EXPECT_EQ(port.perform(make_request("/bytes", 1), &bytes, cancel_).result,
              HttpResult::Cancelled);
    EXPECT_EQ(bytes.read_count(), 0U);
    cancel_.store(false);
    GateBytes in_progress;
    std::promise<void> release;
    in_progress.release = release.get_future().share();
    auto entered = in_progress.entered.get_future();
    auto pending = std::async(std::launch::async, [&] {
        return port.perform(make_request("/bytes", 1), &in_progress, cancel_);
    });
    const auto started = entered.wait_for(std::chrono::seconds(3));
    cancel_.store(true);
    release.set_value();
    ASSERT_EQ(started,
              std::future_status::ready);  // 真实TLS后source已进入，不用预取消冒称传输中取消。
    ASSERT_EQ(pending.wait_for(std::chrono::seconds(4)), std::future_status::ready);
    response = pending.get();
    EXPECT_EQ(response.result, HttpResult::Cancelled);
    EXPECT_TRUE(response.body.empty());
    cancel_.store(false);
    EXPECT_EQ(port.perform(make_request("/bytes"), nullptr, cancel_).result, HttpResult::Ok);
}
TEST_F(HttpsTest, RejectsConcurrentUseOfSamePortWithoutBorrowingAnotherBody) {
    CurlHttpPort port(ca_);
    GateBytes first;
    std::promise<void> release;
    first.release = release.get_future().share();
    auto entered = first.entered.get_future();
    auto pending = std::async(std::launch::async, [&] {
        return port.perform(make_request("/bytes", 1), &first, cancel_);
    });
    const auto ready = entered.wait_for(std::chrono::seconds(3));
    if (ready == std::future_status::ready) {
        Bytes second(1);
        EXPECT_EQ(port.perform(make_request("/bytes", 1), &second, cancel_).result,
                  HttpResult::Busy);
        EXPECT_EQ(second.read_count(), 0U);
    }
    release.set_value();  // 断言失败也释放原worker，不能依靠future析构阻塞至测试超时。
    ASSERT_EQ(ready, std::future_status::ready);
    ASSERT_EQ(pending.wait_for(std::chrono::seconds(4)), std::future_status::ready);
    EXPECT_EQ(pending.get().result, HttpResult::Ok);
}
TEST_F(HttpsTest, RejectsMalformedAndEncodedUserinfoBeforeNetworkRead) {
    CurlHttpPort port(ca_);
    Bytes bytes(1);
    for (const auto* target : {"https://%75ser%3Asecret@localhost/bytes", "https://:bad/bytes",
                               "https://localhost:999999/bytes", "http://127.0.0.1/bytes"}) {
        auto request = make_request("/bytes", 1);
        request.url = target;
        EXPECT_EQ(port.perform(request, &bytes, cancel_).result, HttpResult::Invalid);
        EXPECT_EQ(bytes.read_count(), 0U);
    }
}
TEST_F(HttpsTest, ContentUploadUsesActualTlsMetadataPutAndCompletion) {
    CurlHttpPort port(ca_);
    UploadRuntime runtime;
    ContentUploader uploader(port, runtime);
    ContentUploadRequest request;
    request.api_origin = base_;
    request.authorization = "Basic c3ludGhldGljOnByaXZhdGU=";
    request.session_id = "session-tls";
    request.request_id = "upload-tls";
    request.name = "note.txt";
    request.mime_type = "text/plain";
    request.sha256 = "d67c656e01756650d77717b0839985a056ec28ffe174601d690fc407a2ceffca";
    request.size = 4096;
    request.timeout_ms = 10000;
    Bytes original(4096);
    const auto outcome = uploader.upload(request, original, cancel_);
    EXPECT_EQ(outcome.result, ContentUploadResult::Ready);
    EXPECT_EQ(outcome.stage, ContentUploadStage::Complete);
    EXPECT_EQ(outcome.content_ref, "artifact_upload-tls");
    EXPECT_EQ(outcome.type, "file");
    EXPECT_EQ(outcome.mime_type, "text/plain");
    EXPECT_EQ(original.read_count(), 4096U);
}
}  // namespace
}  // namespace ur::device
