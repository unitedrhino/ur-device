// 桌面附件worker永久回归：真实原件/SHA256/同句柄PUT，HTTP只用受控端口，不宣称平台或模型E2E。
#include "content_input_worker.hpp"
#include <array>
#include <cJSON.h>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <gtest/gtest.h>
#include <mutex>
#include <stdexcept>

namespace ur::device {
namespace {
// Files在系统临时目录创建独占合成原件，析构只清理自己确认创建的目录。
class Files {
public:
  Files() {
    static std::atomic_uint counter{0};
    const auto base = std::filesystem::temp_directory_path();
    for (unsigned i = 0; i < 100; ++i) {
      root = base /
             ("ur-content-worker-" +
              std::to_string(
                  std::chrono::steady_clock::now().time_since_epoch().count()) +
              "-" + std::to_string(++counter));
      if (std::filesystem::create_directory(root))
        return;
    }
    throw std::runtime_error("fixture");
  }
  ~Files() {
    std::error_code error;
    std::filesystem::remove_all(root, error);
  }
  std::filesystem::path write(const std::string &name,
                              const std::string &body) {
    const auto path = root / name;
    std::ofstream file(path, std::ios::binary);
    file.write(body.data(), static_cast<std::streamsize>(body.size()));
    return path;
  }
  std::filesystem::path root; // 仅本用例独占目录，真实路径不写测试输出。
};
// Http在Prepare解析实际worker
// metadata，并以小块读取PUT；屏障只阻塞worker，不阻塞事件线程。
class Http final : public HttpPort {
public:
  HttpResponse perform(const HttpRequest &request, HttpBodySource *source,
                       const std::atomic_bool &cancel) override {
    const auto index = calls++;
    if (throw_at == static_cast<int>(index))
      throw std::runtime_error("private-body-must-not-escape");
    if (hook)
      hook(index);
    std::string body;
    std::array<std::uint8_t, 31> buffer{};
    std::size_t read = 0;
    while (read < request.body_size) {
      std::size_t count = 0;
      if (!source || !source->read(buffer.data(), buffer.size(), count) ||
          !count)
        return {HttpResult::SourceFailed, 0, {}};
      read += count;
      body.append(reinterpret_cast<const char *>(buffer.data()), count);
    }
    if (index == 0) {
      std::unique_ptr<cJSON, decltype(&cJSON_Delete)> json(
          cJSON_Parse(body.c_str()), cJSON_Delete);
      if (!json)
        return {};
      sha = cJSON_GetObjectItemCaseSensitive(json.get(), "sha256")->valuestring;
      id = cJSON_GetObjectItemCaseSensitive(json.get(), "requestId")
               ->valuestring;
      name = cJSON_GetObjectItemCaseSensitive(json.get(), "name")->valuestring;
      mime_type =
          cJSON_GetObjectItemCaseSensitive(json.get(), "mimeType")->valuestring;
      const auto kind =
          mime_type.compare(0, 6, "image/") == 0 ? "image" : "file";
      type = kind;
      const auto wall = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
      return {
          HttpResult::Ok, 200,
          "{\"code\":200,\"data\":{\"status\":\"uploading\",\"contentRef\":"
          "\"artifact_worker\",\"type\":\"" +
              type + "\",\"mimeType\":\"" + mime_type +
              "\",\"uploadUrl\":\"https://oss.invalid/put?signature=private\","
              "\"uploadReceipt\":\"private-receipt\",\"expiresAt\":" +
              std::to_string(wall + 90000) + "}}"};
    }
    if (index == 1) {
      put = std::move(body);
      for (const auto &header : request.headers)
        EXPECT_NE(header.name, "Authorization");
      {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true;
        condition.notify_all();
        if (block)
          condition.wait_for(lock, std::chrono::seconds(2),
                             [&] { return released; });
      }
      if (cancel.load())
        return {HttpResult::Cancelled, 0, {}};
      return {HttpResult::Ok, 204, {}};
    }
    return {HttpResult::Ok, 200,
            "{\"code\":200,\"data\":{\"status\":\"ready\",\"contentRef\":"
            "\"artifact_worker\",\"type\":\"" +
                type + "\",\"mimeType\":\"" + mime_type + "\"}}"};
  }
  bool wait_put() {
    std::unique_lock<std::mutex> lock(mutex);
    return condition.wait_for(lock, std::chrono::seconds(2),
                              [&] { return entered; });
  }
  void release() {
    std::lock_guard<std::mutex> lock(mutex);
    released = true;
    condition.notify_all();
  }
  std::atomic_uint calls{0}; // 实际同步HTTP次数，不指模型执行次数。
  std::string sha, id, name, put, type,
      mime_type;                      // 仅合成原件/metadata供断言。
  std::function<void(unsigned)> hook; // 精确文件替换屏障，不注入VoiceRuntime。
  bool block = false; // 启动前固定原PUT屏障。
  int throw_at = -1; // Prepare/PUT/Complete固定故障点；-1表示不抛异常。
  std::mutex mutex;
  std::condition_variable condition;      // 原PUT退出的受控屏障。
  bool entered = false, released = false; // 只在mutex下读写。
};
// config是合成可信启动参数，不读取实际环境/身份。
ContentInputConfig config() {
  return {"https://platform.invalid",
          content_input_basic("mqtt-user", "mqtt-password")};
}
// command保留显式kind和原件路径，不把文件名当授权或自动猜image。
ContentInputCommand command(const std::filesystem::path &path,
                            InputContentKind kind = InputContentKind::File) {
  return {true, kind, path.u8string()};
}
// take在有限测试截止内轮询非阻塞结果；不修改业务90秒上传预算。
std::optional<ContentInputCompletion> take(ContentInputWorker &worker) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    if (auto result = worker.take())
      return result;
    std::this_thread::yield();
  }
  return {};
}
TEST(ContentInputWorkerTest, ParsesOnlyExplicitCommandsAndPreservesText) {
  EXPECT_FALSE(parse_content_input("hello").attachment);
  EXPECT_FALSE(parse_content_input("/image-other name.png").attachment);
  EXPECT_TRUE(parse_content_input("/file").attachment);
  EXPECT_TRUE(parse_content_input("/file").path.empty());
  const auto parsed = parse_content_input("/image folder/my image.png");
  EXPECT_EQ(parsed.kind, InputContentKind::Image);
  EXPECT_EQ(parsed.path, "folder/my image.png");
}
TEST(ContentInputWorkerTest, MissingOrUntrustedOriginNeverStartsOrCallsHttp) {
  Http http;
  Files files;
  const auto file = files.write("one.txt", "abc");
  for (const auto &origin :
       {"", "http://platform.invalid", "https://platform.invalid/path",
        "https://user@platform.invalid", "https://platform.invalid?x=1"}) {
    ContentInputWorker worker(http, {origin, config().authorization});
    EXPECT_FALSE(
        worker.start(command(file), {"session-one", "connection-one"}));
    EXPECT_FALSE(worker.busy());
  }
  EXPECT_EQ(http.calls.load(), 0U);
  EXPECT_EQ(content_input_basic("mqtt-user", "mqtt-password"),
            "Basic bXF0dC11c2VyOm1xdHQtcGFzc3dvcmQ=");
  EXPECT_TRUE(content_input_basic("user:other", "password").empty());
}
TEST(ContentInputWorkerTest, RealShaAndOriginalHandleSurvivePathReplacement) {
  Files files;
  const auto file = files.write("one.txt", "abc");
  Http http;
  http.hook = [&](unsigned stage) {
    if (stage == 0) {
      std::filesystem::rename(file, files.root / "original.txt");
      files.write("one.txt", "changed");
    }
  };
  ContentInputWorker worker(http, config());
  ASSERT_TRUE(worker.start(command(file), {"session-one", "connection-one"}));
  auto result = take(worker);
  ASSERT_TRUE(result);
  ASSERT_EQ(result->result, ContentUploadResult::Ready);
  EXPECT_EQ(result->original.session, "session-one");
  EXPECT_EQ(result->original.connection, "connection-one");
  EXPECT_EQ(result->part.kind, InputContentKind::File);
  EXPECT_EQ(result->part.content_ref, "artifact_worker");
  EXPECT_EQ(http.put, "abc");
  EXPECT_EQ(http.sha,
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  EXPECT_EQ(http.name, "one.txt");
  EXPECT_EQ(http.id.size(), 32U);
  EXPECT_EQ(http.id.find_first_not_of("0123456789abcdef"), std::string::npos);
  EXPECT_EQ(http.calls.load(), 3U);
  EXPECT_FALSE(worker.take());
  EXPECT_FALSE(worker.busy());
}
TEST(ContentInputWorkerTest, CancelAndBusyNeverSubmitCompleteOrReviveReady) {
  Files files;
  const auto file = files.write("one.txt", "abc");
  Http http;
  http.block = true;
  ContentInputWorker worker(http, config());
  ASSERT_TRUE(worker.start(command(file), {"session-one", "connection-one"}));
  ASSERT_TRUE(http.wait_put());
  EXPECT_TRUE(worker.busy());
  EXPECT_FALSE(worker.take());
  EXPECT_FALSE(worker.start(command(file), {"session-two", "connection-two"}));
  worker.cancel();
  http.release();
  auto result = take(worker);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->result, ContentUploadResult::Cancelled);
  EXPECT_TRUE(result->part.content_ref.empty());
  EXPECT_EQ(http.calls.load(), 2U);
}
TEST(ContentInputWorkerTest, CancelBeforeResultConsumptionDropsReference) {
  Files files;
  const auto file = files.write("one.txt", "abc");
  Http http;
  ContentInputWorker worker(http, config());
  ASSERT_TRUE(worker.start(command(file), {"session-one", "connection-one"}));
  ASSERT_TRUE(http.wait_put());
  worker.cancel();
  auto result = take(worker);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->result, ContentUploadResult::Cancelled);
  EXPECT_TRUE(result->part.content_ref.empty());
}
TEST(ContentInputWorkerTest, UnsupportedEmptyOversizeAndNonRegularNeverUpload) {
  Files files;
  Http http;
  ContentInputWorker worker(http, config());
  const auto big = files.write("big.txt", "a");
  std::filesystem::resize_file(big, 50ULL * 1024 * 1024 + 1);
  for (const auto &file : {files.write("empty.txt", ""),
                           files.write("one.exe", "abc"), big, files.root}) {
    ASSERT_TRUE(worker.start(command(file), {"session-one", "connection-one"}));
    auto result = take(worker);
    ASSERT_TRUE(result);
    EXPECT_EQ(result->result, ContentUploadResult::Invalid);
  }
  ASSERT_TRUE(worker.start(
      command(files.write("one.txt", "abc"), InputContentKind::Image),
      {"session-one", "connection-one"}));
  auto result = take(worker);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->result, ContentUploadResult::Invalid);
  EXPECT_EQ(http.calls.load(), 0U);
}
TEST(ContentInputWorkerTest, ExplicitImageUsesActualMimeAndBoundedStream) {
  Files files;
  Http http;
  ContentInputWorker worker(http, config());
  const auto file = files.write("one.PNG", std::string(131073, 'a'));
  ASSERT_TRUE(worker.start(command(file, InputContentKind::Image),
                           {"session-one", "connection-one"}));
  auto result = take(worker);
  ASSERT_TRUE(result);
  ASSERT_EQ(result->result, ContentUploadResult::Ready);
  EXPECT_EQ(result->part.kind, InputContentKind::Image);
  EXPECT_EQ(result->part.mime_type, "image/png");
  EXPECT_EQ(http.put.size(), 131073U);
}
TEST(ContentInputWorkerTest, PortExceptionsBecomeFixedFailure) {
  Files files;
  const auto file = files.write("one.txt", "abc");
  // 三个真实端口出口均须在无异常核心之前收敛；ASan同时核中间请求资源已释放。
  for (int stage = 0; stage < 3; ++stage) {
    SCOPED_TRACE(stage);
    Http http;
    http.throw_at = stage;
    ContentInputWorker worker(http, config());
    ASSERT_TRUE(worker.start(command(file),
                             {"session-one", "connection-one"}));
    auto result = take(worker);
    ASSERT_TRUE(result);
    EXPECT_EQ(result->result, ContentUploadResult::TransportFailed);
    EXPECT_TRUE(result->part.content_ref.empty());
    EXPECT_EQ(http.calls.load(), static_cast<unsigned>(stage + 1));
    EXPECT_FALSE(worker.busy());
  }
}
TEST(ContentInputWorkerTest,
     EventThreadRechecksOriginalSessionConnectionAndSelectedKind) {
  ContentInputCompletion result;
  result.original = {"session-one", "connection-one"};
  result.result = ContentUploadResult::Ready;
  result.part.kind = InputContentKind::Image;
  result.part.content_ref = "artifact_worker";
  EXPECT_TRUE(content_input_current_result(result, result.original, true));
  EXPECT_FALSE(content_input_current_result(result, result.original, false));
  EXPECT_FALSE(content_input_current_result(
      result, {"session-two", "connection-one"}, true));
  EXPECT_FALSE(content_input_current_result(
      result, {"session-one", "connection-two"}, true));
  EXPECT_FALSE(content_input_current_result(result, {}, true));
  result.result = ContentUploadResult::Cancelled;
  EXPECT_FALSE(content_input_current_result(result, result.original, true));
  result.result = ContentUploadResult::Ready;
  result.part.kind = InputContentKind::Text;
  EXPECT_FALSE(content_input_current_result(result, result.original, true));
}
TEST(ContentInputWorkerTest, InvalidSnapshotAndEmptyCommandNeverStart) {
  Files files;
  Http http;
  ContentInputWorker worker(http, config());
  const auto file = files.write("one.txt", "abc");
  EXPECT_FALSE(worker.start(command(file), {}));
  EXPECT_FALSE(
      worker.start(command(file), {std::string(65, 'a'), "connection"}));
  EXPECT_FALSE(worker.start(command(file), {"session", std::string(65, 'a')}));
  EXPECT_FALSE(worker.start({true, InputContentKind::File, {}},
                            {"session", "connection"}));
  EXPECT_FALSE(
      worker.start({true, InputContentKind::File, std::string("a\0b", 3)},
                   {"session", "connection"}));
  EXPECT_EQ(http.calls.load(), 0U);
  EXPECT_FALSE(worker.busy());
}
#ifndef _WIN32
TEST(ContentInputWorkerTest, SymlinkNeverOpensTarget) {
  Files files;
  Http http;
  ContentInputWorker worker(http, config());
  const auto file = files.write("original.txt", "abc");
  const auto link = files.root / "link.txt";
  std::filesystem::create_symlink(file, link);
  ASSERT_TRUE(worker.start(command(link), {"session", "connection"}));
  auto result = take(worker);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->result, ContentUploadResult::Invalid);
  EXPECT_EQ(http.calls.load(), 0U);
}
#endif
} // namespace
} // namespace ur::device
