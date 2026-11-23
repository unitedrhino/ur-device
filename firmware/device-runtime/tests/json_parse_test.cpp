// 共享JSON解析并发回归：真实上传响应与信封入口并行，不锁网络、不调用实际平台或模型。
#include "ur/device/content_upload.hpp"
#include "ur/device/envelope.hpp"
#include "ur/device/json_parse.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cJSON.h>
#include <cstdlib>
#include <gtest/gtest.h>
#include <memory>
#include <thread>

namespace ur::device {
namespace {
// Clock使用固定合成时钟，避免线程调度改变上传租约或测试业务截止。
class Clock final : public ContentUploadRuntime {
public:
  std::uint64_t monotonic_ms() const override { return 100; }
  std::uint64_t unix_ms() const override { return 1700000000000ULL; }
  bool current() const override { return true; }
};
// Bytes只顺序提供三字节合成原件，保留真实metadata/PUT读取路径。
class Bytes final : public HttpBodySource {
public:
  bool read(std::uint8_t *buffer, std::size_t capacity,
            std::size_t &count) override {
    constexpr char value[] = "abc";
    count = std::min(capacity, 3 - offset_);
    std::copy_n(value + offset_, count, buffer);
    offset_ += count;
    return true;
  }

private:
  std::size_t offset_ = 0; // 仅由当前上传线程读取。
};
// Http仅提供合成端口响应；响应JSON必须经真实ContentUploader两轮解析，夹具不另解析。
class Http final : public HttpPort {
public:
  HttpResponse perform(const HttpRequest &request, HttpBodySource *source,
                       const std::atomic_bool &) override {
    std::array<std::uint8_t, 32> buffer{};
    std::size_t total = 0;
    while (total < request.body_size) {
      std::size_t count = 0;
      if (!source || !source->read(buffer.data(), buffer.size(), count) ||
          count == 0)
        return {HttpResult::SourceFailed, 0, {}};
      total += count;
    }
    switch (calls_++) {
    case 0:
      return {HttpResult::Ok, 200,
              R"({"code":200,"data":{"contentRef":"artifact_parallel","type":"file","mimeType":"text/plain","status":"uploading","uploadUrl":"https://oss.invalid/put","uploadReceipt":"synthetic","expiresAt":1700000030000}})"};
    case 1:
      return {HttpResult::Ok, 204, {}};
    case 2:
      return {HttpResult::Ok, 200,
              R"({"code":200,"data":{"contentRef":"artifact_parallel","type":"file","mimeType":"text/plain","status":"ready"}})"};
    default:
      return {HttpResult::Invalid, 0, {}};
    }
  }

private:
  unsigned calls_ = 0; // 单次原上传实例，不与信封线程共享。
};
// upload_request保持当前metadata合同，不以错误响应或自动重试制造并发。
ContentUploadRequest upload_request() {
  ContentUploadRequest request;
  request.api_origin = "https://platform.invalid";
  request.authorization = "Basic c3ludGhldGljOnByaXZhdGU=";
  request.session_id = "parallel-session";
  request.request_id = "parallel-upload";
  request.name = "one.txt";
  request.mime_type = "text/plain";
  request.sha256.assign(64, 'a');
  request.size = 3;
  return request;
}
// concurrent_paths用受控启动屏障并行执行原上传/信封入口；所有树及字符串只归单一线程。
bool concurrent_paths() {
  std::atomic_uint ready{0};
  std::atomic_bool start{false}, valid{true};
  const auto barrier = [&] {
    ready.fetch_add(1);
    while (!start.load())
      std::this_thread::yield();
  };
  std::thread upload([&] {
    barrier();
    for (unsigned index = 0; index < 64; ++index) {
      Clock clock;
      Http http;
      Bytes bytes;
      std::atomic_bool cancel{false};
      ContentUploader uploader(http, clock);
      const auto result = uploader.upload(upload_request(), bytes, cancel);
      if (result.result != ContentUploadResult::Ready ||
          result.content_ref != "artifact_parallel")
        valid.store(false);
    }
  });
  std::thread envelope([&] {
    barrier();
    for (unsigned index = 0; index < 64; ++index) {
      Envelope message;
      message.token = "parallel-token";
      message.method = "sessionEnd";
      message.session = "parallel-session";
      message.payload = R"({"reason":"ended"})";
      std::string encoded, canonical;
      Envelope decoded;
      if (canonical_json_object(message.payload, canonical) != DecodeResult::Ok ||
          encode_envelope(message, encoded) != DecodeResult::Ok ||
          decode_envelope(encoded, decoded) != DecodeResult::Ok ||
          decoded.payload != canonical)
        valid.store(false);
    }
  });
  while (ready.load() != 2)
    std::this_thread::yield();
  start.store(true);
  upload.join();
  envelope.join();
  return valid.load();
}
TEST(JsonParseTest, UploadRepliesAndEnvelopeParsingAreConcurrentAndOwned) {
  EXPECT_TRUE(concurrent_paths());
}
// hook_alloc/free仅核启动前自定义分配器仍可使用；原Hooks不允许在解析线程存活时更换。
std::atomic_uint allocations{0};
void *hook_alloc(std::size_t size) {
  allocations.fetch_add(1);
  return std::malloc(size);
}
void hook_free(void *value) { std::free(value); }
TEST(JsonParseTest, AllocatorHooksAreSetBeforeThreadsAndRestoredAfterJoin) {
  allocations.store(0);
  cJSON_Hooks hooks{hook_alloc, hook_free};
  cJSON_InitHooks(&hooks);
  const bool valid = concurrent_paths();
  cJSON_InitHooks(nullptr); // 所有树已析构，两个线程已join，不能在线热切分配器。
  EXPECT_TRUE(valid);
  EXPECT_GT(allocations.load(), 0U);
}
// 包装层必须保留原Parse对null输入、JSON null和非严格尾部的不同语义。
TEST(JsonParseTest, NullAndPermissiveParseKeepOriginalOwnership) {
  EXPECT_EQ(parse_json(nullptr), nullptr);
  EXPECT_EQ(parse_json(""), nullptr);
  std::unique_ptr<cJSON, decltype(&cJSON_Delete)> value(parse_json("null"), cJSON_Delete);
  ASSERT_NE(value, nullptr);
  EXPECT_TRUE(cJSON_IsNull(value.get()));
  value.reset(parse_json("{}tail"));
  ASSERT_NE(value, nullptr);
  EXPECT_TRUE(cJSON_IsObject(value.get()));
}
// length严格限制当前缓冲区；end是本次输入的原地址，不引用全局错误位置。
TEST(JsonParseTest, LengthEndPointerAndStrictTerminationAreUnchanged) {
  const char bytes[] = {'{', '}', 'x', '\0'};
  const char *end = nullptr;
  std::unique_ptr<cJSON, decltype(&cJSON_Delete)> value(
      parse_json_with_length_opts(bytes, 2, &end, 0), cJSON_Delete);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(end, bytes + 2);
  value.reset(parse_json_with_length_opts(bytes, sizeof(bytes), &end, 1));
  EXPECT_EQ(value, nullptr);
  EXPECT_EQ(end, bytes + 2);
  value.reset(parse_json_with_length_opts(bytes, 1, &end, 0));
  EXPECT_EQ(value, nullptr);
  EXPECT_EQ(end, bytes);
  const char terminated[] = "{} \t";
  value.reset(parse_json_with_length_opts(terminated, sizeof(terminated), &end, 1));
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(end, terminated + sizeof(terminated) - 1);
  value.reset(parse_json_with_length_opts(terminated, sizeof(terminated) - 1, &end, 1));
  EXPECT_EQ(value, nullptr);
  value.reset(parse_json_with_length_opts(terminated, 0, &end, 0));
  EXPECT_EQ(value, nullptr);
  EXPECT_EQ(end, terminated);
  // 原null输入不会写调用方结束指针；锁不能额外替换这个输出。
  end = terminated;
  EXPECT_EQ(parse_json_with_length_opts(nullptr, 2, &end, 1), nullptr);
  EXPECT_EQ(end, terminated);
}
} // namespace
} // namespace ur::device
