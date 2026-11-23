// 三阶段上传核心回归：使用可控时间/流式端口，不启动OSS或把离线fixture称真实模型。
#include "ur/device/content_upload.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <functional>
#include <string>
#include <vector>

namespace ur::device {
namespace {
// Runtime 独立控制单调时钟、校时及原会话失效，不靠真实sleep推进状态。
class Runtime final : public ContentUploadRuntime {
public:
  std::uint64_t mono = 500;              // 本次单调毫秒起点。
  std::uint64_t wall = 1700000000000ULL; // 合成UTC毫秒，非环境配置。
  bool active = true;                    // 原会话/连接是否仍有效。
  std::uint64_t monotonic_ms() const override { return mono; }
  std::uint64_t unix_ms() const override { return wall; }
  bool current() const override { return active; }
};
// Bytes 模拟不可变原件/metadata的顺序读取，允许检查拒绝前原件读取为零。
class Bytes final : public HttpBodySource {
public:
  std::string value = "original-file-content"; // 合成测试原件。
  unsigned reads = 0;                          // 实际读取回调次数。
  std::size_t offset = 0;                      // 顺序读取位置。
  bool read(std::uint8_t *out, std::size_t capacity,
            std::size_t &count) override {
    ++reads;
    count = std::min(capacity, value.size() - offset);
    std::copy_n(value.data() + offset, count, out);
    offset += count;
    return true;
  }
};
// Http 只保留本次合成请求以核对方向及凭据隔离；网络TLS另由Curl真实集成验证。
class Http final : public HttpPort {
public:
  std::vector<HttpResponse> replies; // 顺序且不自动重试的明确端口响应。
  std::vector<HttpRequest> requests; // 合成请求快照，不写日志。
  std::vector<std::string>
      bodies; // 实际顺序读取字节，不作完整原件RAM设计示例。
  std::function<void(std::size_t)> after; // 控制原会话/取消/时间的阶段切换。
  HttpResponse perform(const HttpRequest &in, HttpBodySource *source,
                       const std::atomic_bool &) override {
    const auto index = requests.size();
    requests.push_back(in);
    EXPECT_TRUE(valid_http_request(in));
    std::string body;
    std::array<std::uint8_t, 7> buffer{};
    while (body.size() < in.body_size) {
      std::size_t count = 0;
      if (!source || !source->read(buffer.data(), buffer.size(), count) ||
          count == 0)
        return {HttpResult::SourceFailed, 0, {}};
      body.append(reinterpret_cast<const char *>(buffer.data()), count);
    }
    EXPECT_EQ(body.size(), in.body_size);
    bodies.push_back(std::move(body));
    if (after)
      after(index);
    return index < replies.size() ? replies[index] : HttpResponse{};
  }
};
// request 与真实AISVR metadata字段一致，合成Basic只用于方向隔离断言。
ContentUploadRequest request() {
  ContentUploadRequest out;
  out.api_origin = "https://platform.invalid";
  out.authorization = "Basic c3ludGhldGljOnByaXZhdGU=";
  out.session_id = "session-01";
  out.request_id = "upload-01";
  out.name = "note.txt";
  out.mime_type = "text/plain";
  out.sha256.assign(64, 'a');
  out.size = std::string("original-file-content").size();
  return out;
}
// prepared/complete 严格保留Things标准code/data外壳；地址与收据均为合成值。
HttpResponse prepared(std::string status = "uploading",
                      std::uint64_t expires = 1700000030000ULL) {
  const bool ready = status == "ready";
  return {HttpResult::Ok, 200,
          "{\"code\":200,\"msg\":\"success\",\"data\":{\"contentRef\":"
          "\"artifact_one\","
          "\"type\":\"file\",\"mimeType\":\"text/plain\",\"status\":\"" +
              status + "\",\"uploadUrl\":\"" +
              (ready
                   ? ""
                   : "https://oss.invalid/private?signature=synthetic-secret") +
              "\",\"uploadReceipt\":\"" +
              (ready ? "" : "synthetic-private-receipt") +
              "\",\"expiresAt\":" + std::to_string(ready ? 0 : expires) + "}}"};
}
// complete 只表示原件ready，不生成模型回复或MQTT应答。
HttpResponse completed(std::string ref = "artifact_one") {
  return {HttpResult::Ok, 200,
          "{\"code\":200,\"data\":{\"contentRef\":\"" + ref +
              "\",\"type\":\"file\",\"mimeType\":\"text/"
              "plain\",\"status\":\"ready\"}}"};
}
// authorized 只检查metadata两步携带Basic，禁止第三方PUT继承认证。
bool authorized(const HttpRequest &in) {
  return std::any_of(
      in.headers.begin(), in.headers.end(),
      [](const HttpHeader &header) { return header.name == "Authorization"; });
}
TEST(ContentUploadTest, StreamsThreePhasesAndNeverSendsBasicToOss) {
  Runtime runtime;
  Http http;
  http.replies = {prepared(), {HttpResult::Ok, 204, {}}, completed()};
  ContentUploader uploader(http, runtime);
  Bytes source;
  std::atomic_bool cancel{false};
  const auto outcome = uploader.upload(request(), source, cancel);
  ASSERT_EQ(outcome.result, ContentUploadResult::Ready);
  EXPECT_EQ(outcome.content_ref, "artifact_one");
  EXPECT_EQ(outcome.type, "file");
  EXPECT_EQ(outcome.mime_type, "text/plain");
  ASSERT_EQ(http.requests.size(), 3U);
  EXPECT_EQ(
      http.requests[0].url,
      "https://platform.invalid/api/v1/things/device/edge/content/prepare");
  EXPECT_EQ(http.requests[1].method, HttpMethod::Put);
  EXPECT_EQ(
      http.requests[2].url,
      "https://platform.invalid/api/v1/things/device/edge/content/complete");
  EXPECT_TRUE(authorized(http.requests[0]));
  EXPECT_FALSE(authorized(http.requests[1]));
  EXPECT_TRUE(authorized(http.requests[2]));
  EXPECT_EQ(http.bodies[1], source.value);
  EXPECT_NE(http.bodies[2].find("synthetic-private-receipt"),
            std::string::npos);
  EXPECT_EQ(http.bodies[0].find("owner"), std::string::npos);
  EXPECT_EQ(http.bodies[0].find("authorization"), std::string::npos);
}
TEST(ContentUploadTest, ReadyIdempotentPrepareNeverReadsOrPutsOriginal) {
  Runtime runtime;
  Http http;
  http.replies = {prepared("ready")};
  ContentUploader uploader(http, runtime);
  Bytes source;
  std::atomic_bool cancel{false};
  EXPECT_EQ(uploader.upload(request(), source, cancel).result,
            ContentUploadResult::Ready);
  EXPECT_EQ(source.reads, 0U);
  EXPECT_EQ(http.requests.size(), 1U);
}
TEST(ContentUploadTest,
     RejectsInvalidMetadataAndUntrustedOriginBeforeNetworkOrOriginal) {
  Runtime runtime;
  Http http;
  ContentUploader uploader(http, runtime);
  std::atomic_bool cancel{false};
  std::vector<ContentUploadRequest> invalid;
  for (const auto *origin :
       {"http://platform.invalid", "https://user@platform.invalid",
        "https://platform.invalid/path", "https://platform.invalid?token=x",
        "https://platform.invalid#fragment",
        "https://platform%40evil.invalid"}) {
    auto value = request();
    value.api_origin = origin;
    invalid.push_back(value);
  }
  for (const auto *name : {"../note.txt", "note.mp4", "folder\\note.txt",
                           "x\r\n.txt", ".", ".."}) {
    auto value = request();
    value.name = name;
    invalid.push_back(value);
  }
  auto value = request();
  value.request_id = "not.allowed";
  invalid.push_back(value);
  value = request();
  value.request_id.assign(101, 'a');
  invalid.push_back(value);
  value = request();
  value.size = 50ULL * 1024 * 1024 + 1;
  invalid.push_back(value);
  value = request();
  value.sha256.assign(64, 'A');
  invalid.push_back(value);
  value = request();
  value.authorization += "\r\nX-Injected: yes";
  invalid.push_back(value);
  value = request();
  value.name = "photo.png";
  value.mime_type = "image/png";
  value.size = 20ULL * 1024 * 1024 + 1;
  invalid.push_back(value);
  for (const auto &item : invalid) {
    Bytes source;
    EXPECT_EQ(uploader.upload(item, source, cancel).result,
              ContentUploadResult::Invalid);
    EXPECT_EQ(source.reads, 0U);
  }
  EXPECT_TRUE(http.requests.empty());
}
TEST(ContentUploadTest, RejectsMalformedReplyAndNeverTreatsHttp200AsReady) {
  for (const auto *body :
       {"", "[]", "{\"code\":200}", "{\"code\":\"200\",\"data\":{}}",
        "{\"code\":200,\"code\":200,\"data\":{}}", "{} trailing",
        "{\"code\":200,\"data\":{\"contentRef\":\"artifact_one\"}}"}) {
    Runtime runtime;
    Http http;
    http.replies = {{HttpResult::Ok, 200, body}};
    ContentUploader uploader(http, runtime);
    Bytes source;
    std::atomic_bool cancel{false};
    const auto outcome = uploader.upload(request(), source, cancel);
    EXPECT_EQ(outcome.result, ContentUploadResult::InvalidReply);
    EXPECT_TRUE(outcome.content_ref.empty());
    EXPECT_EQ(source.reads, 0U);
    EXPECT_EQ(http.requests.size(), 1U);
  }
}
TEST(ContentUploadTest, PreservesStableBusinessFailureWithoutLeakingBody) {
  for (const auto code : {403U, 409U, 422U, 503U}) {
    Runtime runtime;
    Http http;
    http.replies = {{HttpResult::Ok, 200,
                     "{\"code\":" + std::to_string(code) +
                         ",\"msg\":\"synthetic-private-receipt\"}"}};
    ContentUploader uploader(http, runtime);
    Bytes source;
    std::atomic_bool cancel{false};
    const auto outcome = uploader.upload(request(), source, cancel);
    const auto expected = code == 403   ? ContentUploadResult::Denied
                          : code == 409 ? ContentUploadResult::Conflict
                          : code == 422 ? ContentUploadResult::Unsupported
                                        : ContentUploadResult::ServiceFailed;
    EXPECT_EQ(outcome.result, expected);
    EXPECT_TRUE(outcome.content_ref.empty());
    EXPECT_TRUE(outcome.mime_type.empty());
    EXPECT_EQ(source.reads, 0U);
  }
}
// REST使用share/errors及result.Error的业务码，不能套用MQTT的403/422分类。
TEST(ContentUploadTest, UsesActualThingsRestCodesAtPrepareAndComplete) {
  struct Failure {
    unsigned code;       // 正式share/errors/sys.go中的平台业务code。
    const char *message; // devicecontent.rpcError固定分类；不向调用方回传。
    ContentUploadResult result; // 宿主可判定的稳定失败结果。
  };
  const std::array<Failure, 8> failures{{
      {100012, "contentForbidden", ContentUploadResult::Denied},
      {100013, "contentUnsupported", ContentUploadResult::Unsupported},
      {100006, "invalidContent", ContentUploadResult::Invalid},
      {100006, "contentConflict", ContentUploadResult::Conflict},
      {100007, "contentUnavailable", ContentUploadResult::ServiceFailed},
      {100006, "contentConflict:synthetic-private-receipt",
       ContentUploadResult::Invalid},
      {100012, "synthetic-private-receipt", ContentUploadResult::Denied},
      {999999, "contentConflict", ContentUploadResult::ServiceFailed},
  }};
  for (const auto &failure : failures) {
    for (const bool complete : {false, true}) {
      SCOPED_TRACE(std::to_string(failure.code) +
                   (complete ? "/complete" : "/prepare"));
      Runtime runtime;
      Http http;
      const HttpResponse rejected{
          HttpResult::Ok, 200,
          "{\"code\":" + std::to_string(failure.code) + " ,\"msg\":\"" +
              failure.message + "\",\"detail\":\"synthetic-private-receipt\"}"};
      http.replies = complete
                         ? std::vector<HttpResponse>{prepared(),
                                                     {HttpResult::Ok, 204, {}},
                                                     rejected}
                         : std::vector<HttpResponse>{rejected};
      ContentUploader uploader(http, runtime);
      Bytes source;
      std::atomic_bool cancel{false};
      const auto outcome = uploader.upload(request(), source, cancel);
      EXPECT_EQ(outcome.result, failure.result);
      EXPECT_EQ(outcome.stage, complete ? ContentUploadStage::Complete
                                        : ContentUploadStage::Prepare);
      EXPECT_TRUE(outcome.content_ref.empty());
      EXPECT_TRUE(outcome.type.empty());
      EXPECT_TRUE(outcome.mime_type.empty());
      EXPECT_EQ(http.requests.size(), complete ? 3U : 1U);
      if (!complete) {
        EXPECT_EQ(source.reads, 0U);
      }
    }
  }
}
TEST(ContentUploadTest, CancellationAndClosedSessionStopBeforePut) {
  for (const bool cancelled : {false, true}) {
    Runtime runtime;
    Http http;
    http.replies = {prepared()};
    std::atomic_bool cancel{false};
    http.after = [&](std::size_t) {
      if (cancelled)
        cancel.store(true);
      else
        runtime.active = false;
    };
    ContentUploader uploader(http, runtime);
    Bytes source;
    const auto outcome = uploader.upload(request(), source, cancel);
    EXPECT_EQ(outcome.result, cancelled ? ContentUploadResult::Cancelled
                                        : ContentUploadResult::Inactive);
    EXPECT_TRUE(outcome.content_ref.empty());
    EXPECT_EQ(source.reads, 0U);
    EXPECT_EQ(http.requests.size(), 1U);
  }
}
TEST(ContentUploadTest, FixedWholeBudgetAndSignatureDeadlineAreNeverRenewed) {
  Runtime runtime;
  Http http;
  http.replies = {prepared(), {HttpResult::Ok, 204, {}}, completed()};
  http.after = [&](std::size_t index) {
    const auto elapsed = index == 0 ? 15000U : 14000U;
    runtime.mono += elapsed;
    runtime.wall += elapsed;
  };
  ContentUploader uploader(http, runtime);
  Bytes source;
  std::atomic_bool cancel{false};
  const auto outcome = uploader.upload(request(), source, cancel);
  ASSERT_EQ(http.requests.size(), 3U);
  EXPECT_EQ(http.requests[0].timeout_ms, 90000U);
  EXPECT_EQ(http.requests[1].timeout_ms, 15000U);
  EXPECT_EQ(http.requests[2].timeout_ms, 1000U);
  EXPECT_EQ(outcome.result, ContentUploadResult::Timeout);
  EXPECT_TRUE(outcome.content_ref.empty());
}
TEST(ContentUploadTest,
     DelayedPrepareGrantUsesReceiptTimeWithoutRenewingWholeBudget) {
  // 平台在授权/SQL后签发120秒授权；Prepare耗时不应使合法授权超过客户端错误的旧起点上限。
  for (const auto delay : {1100U, 12345U}) {
    Runtime runtime;
    Http http;
    http.replies = {prepared("uploading", runtime.wall + delay + 120000),
                    {HttpResult::Ok, 204, {}},
                    completed()};
    http.after = [&](std::size_t index) {
      if (index == 0) {
        runtime.mono += delay;
        runtime.wall += delay;
      }
    };
    ContentUploader uploader(http, runtime);
    Bytes source;
    std::atomic_bool cancel{false};
    const auto outcome = uploader.upload(request(), source, cancel);
    ASSERT_EQ(outcome.result, ContentUploadResult::Ready);
    ASSERT_EQ(http.requests.size(), 3U);
    EXPECT_EQ(http.requests[0].timeout_ms, 90000U);
    EXPECT_EQ(http.requests[1].timeout_ms, 90000U - delay);
    EXPECT_EQ(http.requests[2].timeout_ms, 90000U - delay);
    EXPECT_EQ(http.bodies[1], source.value);
  }
}
TEST(ContentUploadTest,
     DelayedPrepareStillRejectsOverlongExpiredOrExhaustedGrant) {
  // 合法起算修正不能续本地90秒总截止，也不能接纳到达时已过期或超过服务120秒限额的租约。
  struct Boundary {
    std::uint32_t delay;        // 原Prepare已消耗的双时钟毫秒。
    std::uint32_t grant;        // 到达时的服务剩余租约毫秒。
    ContentUploadResult result; // 拒绝分类，所有边界均不得读原件或发PUT。
  };
  for (const auto boundary :
       {Boundary{1100, 120001, ContentUploadResult::InvalidReply},
        Boundary{1100, 0, ContentUploadResult::Timeout},
        Boundary{90000, 120000, ContentUploadResult::Timeout}}) {
    Runtime runtime;
    Http http;
    http.replies = {
        prepared("uploading", runtime.wall + boundary.delay + boundary.grant)};
    http.after = [&](std::size_t) {
      runtime.mono += boundary.delay;
      runtime.wall += boundary.delay;
    };
    ContentUploader uploader(http, runtime);
    Bytes source;
    std::atomic_bool cancel{false};
    EXPECT_EQ(uploader.upload(request(), source, cancel).result,
              boundary.result);
    EXPECT_EQ(http.requests.size(), 1U);
    EXPECT_EQ(source.reads, 0U);
  }
}
TEST(ContentUploadTest, DelayedPrepareShortGrantStillClampsPutAndComplete) {
  Runtime runtime;
  Http http;
  http.replies = {prepared("uploading", runtime.wall + 1100 + 500),
                  {HttpResult::Ok, 204, {}},
                  completed()};
  http.after = [&](std::size_t index) {
    const auto elapsed = index == 0 ? 1100U : index == 1 ? 499U : 1U;
    runtime.mono += elapsed;
    runtime.wall += elapsed;
  };
  ContentUploader uploader(http, runtime);
  Bytes source;
  std::atomic_bool cancel{false};
  const auto outcome = uploader.upload(request(), source, cancel);
  ASSERT_EQ(http.requests.size(), 3U);
  EXPECT_EQ(http.requests[1].timeout_ms, 500U);
  EXPECT_EQ(http.requests[2].timeout_ms, 1U);
  EXPECT_EQ(outcome.result, ContentUploadResult::Timeout);
  EXPECT_EQ(outcome.stage, ContentUploadStage::Complete);
  EXPECT_TRUE(outcome.content_ref.empty());
}
TEST(ContentUploadTest, WallRollbackAfterDelayedPrepareNeverCompletes) {
  // 回拨即使仍晚于最初wall_start，也不得用旧校时续新收到的授权。
  Runtime runtime;
  Http http;
  http.replies = {prepared(), {HttpResult::Ok, 204, {}}, completed()};
  http.after = [&](std::size_t index) {
    if (index == 0) {
      runtime.mono += 1100;
      runtime.wall += 1100;
    } else if (index == 1) {
      ++runtime.mono;
      --runtime.wall;
    }
  };
  ContentUploader uploader(http, runtime);
  Bytes source;
  std::atomic_bool cancel{false};
  const auto outcome = uploader.upload(request(), source, cancel);
  EXPECT_EQ(outcome.result, ContentUploadResult::Timeout);
  EXPECT_EQ(outcome.stage, ContentUploadStage::Put);
  EXPECT_EQ(http.requests.size(), 2U);
  EXPECT_TRUE(outcome.content_ref.empty());
}
TEST(ContentUploadTest, ExpiredOrOverlongGrantDoesNotReadOriginal) {
  for (const auto expires : {1699999999999ULL, 1700000120001ULL}) {
    Runtime runtime;
    Http http;
    http.replies = {prepared("uploading", expires)};
    ContentUploader uploader(http, runtime);
    Bytes source;
    std::atomic_bool cancel{false};
    const auto outcome = uploader.upload(request(), source, cancel);
    EXPECT_EQ(outcome.result, expires < runtime.wall
                                  ? ContentUploadResult::Timeout
                                  : ContentUploadResult::InvalidReply);
    EXPECT_EQ(source.reads, 0U);
    EXPECT_TRUE(outcome.content_ref.empty());
  }
}
TEST(ContentUploadTest, PutFailureNeverCompletesAndWorkerCanBeReused) {
  Runtime runtime;
  Http http;
  http.replies = {prepared(), {HttpResult::SourceFailed, 0, {}}};
  ContentUploader uploader(http, runtime);
  Bytes source;
  std::atomic_bool cancel{false};
  const auto failed = uploader.upload(request(), source, cancel);
  EXPECT_EQ(failed.result, ContentUploadResult::TransportFailed);
  EXPECT_EQ(failed.stage, ContentUploadStage::Put);
  EXPECT_EQ(failed.transport_result, HttpResult::SourceFailed);
  EXPECT_EQ(http.requests.size(), 2U);
  EXPECT_TRUE(failed.content_ref.empty());
  http.replies.push_back(prepared("ready"));
  EXPECT_EQ(uploader.upload(request(), source, cancel).result,
            ContentUploadResult::Ready);
  EXPECT_EQ(http.requests.size(), 3U);
}
TEST(ContentUploadTest, CompleteCannotSubstituteReferenceOrMimetype) {
  Runtime runtime;
  Http http;
  http.replies = {
      prepared(), {HttpResult::Ok, 204, {}}, completed("artifact_different")};
  ContentUploader uploader(http, runtime);
  Bytes source;
  std::atomic_bool cancel{false};
  const auto outcome = uploader.upload(request(), source, cancel);
  EXPECT_EQ(outcome.result, ContentUploadResult::InvalidReply);
  EXPECT_EQ(outcome.stage, ContentUploadStage::Complete);
  EXPECT_TRUE(outcome.content_ref.empty());
}
TEST(ContentUploadTest, ReentrantCallIsBusyAndDoesNotCorruptOriginalWorker) {
  Runtime runtime;
  Http http;
  http.replies = {prepared("ready")};
  ContentUploader uploader(http, runtime);
  Bytes source;
  std::atomic_bool cancel{false};
  http.after = [&](std::size_t) {
    EXPECT_EQ(uploader.upload(request(), source, cancel).result,
              ContentUploadResult::Busy);
  };
  EXPECT_EQ(uploader.upload(request(), source, cancel).result,
            ContentUploadResult::Ready);
  EXPECT_EQ(http.requests.size(), 1U);
  EXPECT_EQ(source.reads, 0U);
}
TEST(ContentUploadTest, UncalibratedClockAndPrecancelHaveNoNetworkSideEffects) {
  for (const bool uncalibrated : {false, true}) {
    Runtime runtime;
    if (uncalibrated)
      runtime.wall = 0;
    Http http;
    ContentUploader uploader(http, runtime);
    Bytes source;
    std::atomic_bool cancel{!uncalibrated};
    const auto outcome = uploader.upload(request(), source, cancel);
    EXPECT_EQ(outcome.result, uncalibrated ? ContentUploadResult::Invalid
                                           : ContentUploadResult::Cancelled);
    EXPECT_TRUE(http.requests.empty());
    EXPECT_EQ(source.reads, 0U);
  }
}
TEST(ContentUploadTest, ClockRollbackOrRevocationAfterPutNeverCompletes) {
  for (const auto which : {0U, 1U, 2U}) {
    Runtime runtime;
    Http http;
    http.replies = {prepared(), {HttpResult::Ok, 204, {}}};
    http.after = [&](std::size_t index) {
      if (index != 1)
        return;
      if (which == 0)
        runtime.mono -= 1;
      else if (which == 1)
        runtime.wall -= 1;
      else
        runtime.active = false;
    };
    ContentUploader uploader(http, runtime);
    Bytes source;
    std::atomic_bool cancel{false};
    const auto outcome = uploader.upload(request(), source, cancel);
    EXPECT_EQ(outcome.result, which == 2 ? ContentUploadResult::Inactive
                                         : ContentUploadResult::Timeout);
    EXPECT_EQ(http.requests.size(), 2U);
    EXPECT_EQ(outcome.stage, ContentUploadStage::Put);
    EXPECT_TRUE(outcome.content_ref.empty());
  }
}
TEST(ContentUploadTest,
     PrivateExtraMetadataAndUnsafeOssUrlAreRejectedBeforeOriginalRead) {
  for (const auto which : {0U, 1U, 2U}) {
    Runtime runtime;
    Http http;
    auto response = prepared();
    if (which == 0)
      response.body.insert(response.body.size() - 2,
                           ",\"leaseToken\":\"synthetic-private\"");
    else if (which == 1) {
      const auto offset = response.body.find("https://oss.invalid");
      response.body.replace(offset, std::string("https://oss.invalid").size(),
                            "http://oss.invalid");
    } else {
      const auto offset = response.body.find("artifact_one");
      response.body.replace(offset, std::string("artifact_one").size(),
                            "https://oss.invalid/private");
    }
    http.replies = {response};
    ContentUploader uploader(http, runtime);
    Bytes source;
    std::atomic_bool cancel{false};
    EXPECT_EQ(uploader.upload(request(), source, cancel).result,
              ContentUploadResult::InvalidReply);
    EXPECT_EQ(source.reads, 0U);
    EXPECT_EQ(http.requests.size(), 1U);
  }
}
TEST(ContentUploadTest, DeviceHttpLimitsAreCheckedBeforeOssPut) {
  Runtime runtime;
  Http http;
  ContentUploader uploader(http, runtime);
  Bytes source;
  std::atomic_bool cancel{false};
  auto in = request();
  in.request_id.assign(65, 'a');
  EXPECT_EQ(uploader.upload(in, source, cancel).result,
            ContentUploadResult::Invalid);
  EXPECT_TRUE(http.requests.empty());
  http.requests.clear();
  http.bodies.clear();
  auto prepared_reply = prepared();
  const auto offset = prepared_reply.body.find("synthetic-private-receipt");
  prepared_reply.body.replace(offset,
                              std::string("synthetic-private-receipt").size(),
                              std::string(4097, 's'));
  http.replies = {prepared_reply};
  EXPECT_EQ(uploader.upload(request(), source, cancel).result,
            ContentUploadResult::InvalidReply);
  EXPECT_EQ(source.reads, 0U);
  EXPECT_EQ(http.requests.size(), 1U);
}
// ready引用仍须与typed输入的artifact后缀字符集一致，不能把非法引用交回事件线程。
TEST(ContentUploadTest, ReadyReferenceUsesSameSuffixContractAsTypedInput) {
  for (const auto *reference : {"artifact_.one", "artifact_:one"}) {
    Runtime runtime;
    Http http;
    auto reply = prepared("ready");
    const auto offset = reply.body.find("artifact_one");
    ASSERT_NE(offset, std::string::npos);
    reply.body.replace(offset, std::string("artifact_one").size(), reference);
    http.replies = {reply};
    ContentUploader uploader(http, runtime);
    Bytes source;
    std::atomic_bool cancel{false};
    const auto outcome = uploader.upload(request(), source, cancel);
    EXPECT_EQ(outcome.result, ContentUploadResult::InvalidReply);
    EXPECT_TRUE(outcome.content_ref.empty());
    EXPECT_EQ(source.reads, 0U);
    EXPECT_EQ(http.requests.size(), 1U);
  }
}
// 各网络阶段固定失败分类，不把端口取消/超时或HTTP拒绝当作原件ready。
TEST(ContentUploadTest, TransportFailuresAtEveryStageNeverAdvanceOrLeak) {
  const std::array<HttpResponse, 7> failures{{
      {HttpResult::Cancelled, 0, {}},
      {HttpResult::Timeout, 0, {}},
      {HttpResult::HttpFailed, 401, {}},
      {HttpResult::HttpFailed, 403, {}},
      {HttpResult::TlsFailed, 0, {}},
      {HttpResult::Ok, 503, {}},
      {HttpResult::ResponseTooLarge, 0, {}},
  }};
  for (const auto stage : {0U, 1U, 2U}) {
    for (const auto &failure : failures) {
      SCOPED_TRACE(std::to_string(stage) + "/" +
                   std::to_string(static_cast<unsigned>(failure.result)));
      Runtime runtime;
      Http http;
      if (stage > 0)
        http.replies.push_back(prepared());
      if (stage > 1)
        http.replies.push_back({HttpResult::Ok, 204, {}});
      http.replies.push_back(failure);
      ContentUploader uploader(http, runtime);
      Bytes source;
      std::atomic_bool cancel{false};
      const auto outcome = uploader.upload(request(), source, cancel);
      const auto expected =
          failure.result == HttpResult::Cancelled
              ? ContentUploadResult::Cancelled
          : failure.result == HttpResult::Timeout ? ContentUploadResult::Timeout
          : stage != 1 && failure.result == HttpResult::HttpFailed
              ? ContentUploadResult::Denied
              : ContentUploadResult::TransportFailed;
      EXPECT_EQ(outcome.result, expected);
      EXPECT_EQ(static_cast<unsigned>(outcome.stage), stage);
      EXPECT_EQ(outcome.transport_result, failure.result);
      EXPECT_EQ(http.requests.size(), stage + 1U);
      EXPECT_TRUE(outcome.content_ref.empty());
      EXPECT_TRUE(outcome.type.empty());
      EXPECT_TRUE(outcome.mime_type.empty());
      if (stage == 0) {
        EXPECT_EQ(source.reads, 0U);
      }
    }
  }
}
// ready与uploading租约分别校验，空值/错误类型/非整数截止均不得触碰原件。
TEST(ContentUploadTest, InvalidGrantStatesAndDeadlinesNeverReadOriginal) {
  std::vector<HttpResponse> invalid{prepared("failed"),
                                    prepared("uploading", 0)};
  const auto changed = [](HttpResponse response, const std::string &from,
                          const std::string &to) {
    const auto at = response.body.find(from);
    EXPECT_NE(at, std::string::npos);
    if (at != std::string::npos)
      response.body.replace(at, from.size(), to);
    return response;
  };
  invalid.push_back(changed(prepared("ready"), "\"uploadUrl\":\"\"",
                            "\"uploadUrl\":\"https://oss.invalid/private\""));
  invalid.push_back(changed(prepared("ready"), "\"uploadReceipt\":\"\"",
                            "\"uploadReceipt\":\"synthetic\""));
  invalid.push_back(
      changed(prepared("ready"), "\"expiresAt\":0", "\"expiresAt\":1"));
  invalid.push_back(changed(prepared(), "1700000030000", "1700000030000.5"));
  invalid.push_back(changed(prepared(), "1700000030000", "-1"));
  invalid.push_back(changed(prepared(), "1700000030000", "9007199254740992"));
  invalid.push_back(changed(prepared(), "1700000030000", "null"));
  invalid.push_back(changed(prepared(), "synthetic-private-receipt", ""));
  invalid.push_back(changed(prepared(), "\"type\":\"file\"", "\"type\":null"));
  invalid.push_back(changed(prepared(), "\"mimeType\":\"text/plain\"",
                            "\"mimeType\":\"image/png\""));
  for (const auto &reply : invalid) {
    Runtime runtime;
    Http http;
    http.replies = {reply};
    ContentUploader uploader(http, runtime);
    Bytes source;
    std::atomic_bool cancel{false};
    EXPECT_EQ(uploader.upload(request(), source, cancel).result,
              ContentUploadResult::InvalidReply);
    EXPECT_EQ(source.reads, 0U);
    EXPECT_EQ(http.requests.size(), 1U);
  }
}
// 短总预算必须同时裁剪三阶段连接截止；扩展名大小写不改变平台MIME合同。
TEST(ContentUploadTest,
     UppercaseExtensionAndShortConnectionBudgetArePreserved) {
  Runtime runtime;
  Http http;
  http.replies = {prepared(), {HttpResult::Ok, 204, {}}, completed()};
  http.after = [&](std::size_t) {
    runtime.mono += 100;
    runtime.wall += 100;
  };
  ContentUploader uploader(http, runtime);
  Bytes source;
  std::atomic_bool cancel{false};
  auto in = request();
  in.name = "NOTE.TXT";
  in.timeout_ms = 1500;
  EXPECT_EQ(uploader.upload(in, source, cancel).result,
            ContentUploadResult::Ready);
  ASSERT_EQ(http.requests.size(), 3U);
  for (std::size_t index = 0; index < http.requests.size(); ++index) {
    EXPECT_EQ(http.requests[index].timeout_ms, 1500U - index * 100U);
    EXPECT_EQ(http.requests[index].connect_timeout_ms, 1500U - index * 100U);
  }
  in.name = "filenameWithoutExtension";
  EXPECT_EQ(uploader.upload(in, source, cancel).result,
            ContentUploadResult::Invalid);
  EXPECT_EQ(http.requests.size(), 3U);
}
} // namespace
} // namespace ur::device
