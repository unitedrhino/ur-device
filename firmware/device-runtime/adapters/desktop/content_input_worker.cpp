// 桌面附件实现：先固定原文件句柄，以64KiB缓冲计算SHA256，再原句柄回卷流式PUT；最终Complete校验归平台。
#include "content_input_worker.hpp"
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace ur::device {
namespace {
// FileCloser只关闭本任务已打开原件，不重新按路径读取或删除文件。
struct FileCloser {
  void operator()(FILE *file) const {
    if (file)
      std::fclose(file);
  }
};
// Source按原长度顺序读取同一原件，取消立即停止；长度变化/IO错误不生成Ready引用。
class Source final : public HttpBodySource {
public:
  Source(FILE *file, std::uint64_t size, const std::atomic_bool &cancel)
      : file_(file), remaining_(size), cancel_(cancel) {}
  bool read(std::uint8_t *buffer, std::size_t capacity,
            std::size_t &count) override {
    count = 0;
    if (cancel_.load())
      return false;
    if (!remaining_)
      return true;
    const auto wanted =
        static_cast<std::size_t>(std::min<std::uint64_t>(remaining_, capacity));
    count = std::fread(buffer, 1, wanted, file_);
    remaining_ -= count;
    return count == wanted && !std::ferror(file_);
  }

private:
  FILE *file_;                     // 借用任务唯一原件句柄。
  std::uint64_t remaining_;        // 不允许读出原stat长度之外的内容。
  const std::atomic_bool &cancel_; // 原事件线程撤销许可。
};
// Runtime只看本任务atomic许可及系统时钟，不访问VoiceRuntime或宿主可变字符串。
class Runtime final : public ContentUploadRuntime {
public:
  explicit Runtime(const std::atomic_bool &current) : current_(current) {}
  std::uint64_t monotonic_ms() const override {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }
  std::uint64_t unix_ms() const override {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
  }
  bool current() const override { return current_.load(); }

private:
  const std::atomic_bool &current_; // 只读单向撤销，不重新授予旧任务。
};
// SafeHttp把桌面端口异常隔离在原perform调用内；异常不能穿过无异常核心而跳过请求资源析构。
class SafeHttp final : public HttpPort {
public:
  // SafeHttp借用当前worker独占端口，不复制认证或新建网络资源。
  explicit SafeHttp(HttpPort &http) : http_(http) {}
  // perform原样转发请求/源/取消；异常仅返回固定传输失败，不保留异常正文或响应。
  HttpResponse perform(const HttpRequest &request, HttpBodySource *source,
                       const std::atomic_bool &cancel) noexcept override {
    try {
      return http_.perform(request, source, cancel);
    } catch (...) {
      return {HttpResult::TransportFailed, 0, {}};
    }
  }

private:
  HttpPort &http_; // 原端口寿命覆盖单次上传，不改变其预算或身份。
};
// mime只接受上传器当前公开格式表；kind须与用户显式命令一致，不按内容猜模态。
std::string mime(const std::string &name, InputContentKind kind) {
  const auto dot = name.find_last_of('.');
  if (dot == std::string::npos)
    return {};
  auto ext = name.substr(dot);
  for (auto &c : ext)
    if (c >= 'A' && c <= 'Z')
      c = static_cast<char>(c + ('a' - 'A'));
  struct Format {
    const char *ext;
    const char *mime;
    InputContentKind kind;
  };
  static constexpr Format formats[] = {
      {".png", "image/png", InputContentKind::Image},
      {".jpg", "image/jpeg", InputContentKind::Image},
      {".jpeg", "image/jpeg", InputContentKind::Image},
      {".gif", "image/gif", InputContentKind::Image},
      {".bmp", "image/bmp", InputContentKind::Image},
      {".webp", "image/webp", InputContentKind::Image},
      {".txt", "text/plain", InputContentKind::File},
      {".csv", "text/csv", InputContentKind::File},
      {".json", "application/json", InputContentKind::File},
      {".md", "text/markdown", InputContentKind::File},
      {".adoc", "text/asciidoc", InputContentKind::File},
      {".html", "text/html", InputContentKind::File},
      {".eml", "message/rfc822", InputContentKind::File},
      {".pdf", "application/pdf", InputContentKind::File},
      {".docx",
       "application/"
       "vnd.openxmlformats-officedocument.wordprocessingml.document",
       InputContentKind::File},
      {".pptx",
       "application/"
       "vnd.openxmlformats-officedocument.presentationml.presentation",
       InputContentKind::File},
      {".xlsx",
       "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
       InputContentKind::File}};
  for (const auto &format : formats)
    if (ext == format.ext && kind == format.kind)
      return format.mime;
  return {};
}
// open_file只返回已打开的普通文件；Linux拒绝软链/FIFO，Windows用Unicode路径和二进制读。
std::unique_ptr<FILE, FileCloser> open_file(const std::filesystem::path &path,
                                            std::uint64_t &size) {
#ifdef _WIN32
  const int fd = _wopen(path.c_str(), _O_RDONLY | _O_BINARY | _O_NOINHERIT);
  if (fd < 0)
    return {};
  struct _stat64 st{};
  if (_fstat64(fd, &st) != 0 || (st.st_mode & _S_IFMT) != _S_IFREG ||
      st.st_size <= 0) {
    _close(fd);
    return {};
  }
  FILE *raw = _fdopen(fd, "rb");
  if (!raw)
    _close(fd);
#else
  const int fd =
      ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0)
    return {};
  struct stat st{};
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
    ::close(fd);
    return {};
  }
  FILE *raw = fdopen(fd, "rb");
  if (!raw)
    ::close(fd);
#endif
  if (!raw)
    return {};
  size = static_cast<std::uint64_t>(st.st_size);
  return std::unique_ptr<FILE, FileCloser>(raw);
}
// hash_file增量SHA256核原size与EOF；取消/短读/增长/回卷失败均拒绝，不缓存完整原件。
std::string hash_file(FILE *file, std::uint64_t size,
                      const std::atomic_bool &cancel,
                      std::chrono::steady_clock::time_point deadline) {
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(),
                                                              EVP_MD_CTX_free);
  if (!ctx || EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1)
    return {};
  std::array<unsigned char, 65536> buffer{};
  while (size) {
    if (cancel.load() || std::chrono::steady_clock::now() >= deadline)
      return {};
    const auto wanted =
        static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), size));
    const auto count = std::fread(buffer.data(), 1, wanted, file);
    if (count != wanted ||
        EVP_DigestUpdate(ctx.get(), buffer.data(), count) != 1)
      return {};
    size -= count;
  }
  if (std::fgetc(file) != EOF || std::ferror(file) || cancel.load() ||
      std::fseek(file, 0, SEEK_SET) != 0)
    return {};
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned length = 0;
  if (EVP_DigestFinal_ex(ctx.get(), digest.data(), &length) != 1 ||
      length != 32)
    return {};
  constexpr char hex[] = "0123456789abcdef";
  std::string out;
  for (unsigned i = 0; i < length; ++i) {
    out += hex[digest[i] >> 4];
    out += hex[digest[i] & 15];
  }
  return out;
}
// random_id是设备本次新上传意图的随机32hex标识，不用路径/时间/旧请求推导。
std::string random_id() {
  std::array<unsigned char, 16> bytes{};
  if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
    return {};
  constexpr char hex[] = "0123456789abcdef";
  std::string out;
  for (auto byte : bytes) {
    out += hex[byte >> 4];
    out += hex[byte & 15];
  }
  return out;
}
} // namespace
ContentInputCommand parse_content_input(const std::string &line) {
  ContentInputCommand out;
  for (const auto &pair : {std::make_pair("/image", InputContentKind::Image),
                           std::make_pair("/file", InputContentKind::File)}) {
    const std::string prefix = pair.first;
    if (line == prefix ||
        line.compare(0, prefix.size() + 1, prefix + " ") == 0) {
      out.attachment = true;
      out.kind = pair.second;
      if (line.size() > prefix.size())
        out.path = line.substr(prefix.size() + 1);
      break;
    }
  }
  return out;
}
bool content_input_configured(const ContentInputConfig &config) {
  if (config.origin.compare(0, 8, "https://") != 0 ||
      config.origin.find_first_of("/?#%@", 8) != std::string::npos ||
      config.authorization.compare(0, 6, "Basic ") != 0 ||
      config.authorization.size() <= 6)
    return false;
  HttpRequest request;
  request.url = config.origin + "/api/v1/things/device/edge/content/prepare";
  request.headers = {{"Authorization", config.authorization}};
  return valid_http_request(request);
}
std::string content_input_basic(const std::string &username,
                                const std::string &password) {
  if (username.empty() || password.empty() ||
      username.find(':') != std::string::npos || username.size() > 2048 ||
      password.size() > 2048)
    return {};
  const auto bytes = username + ":" + password;
  std::string out(4 * ((bytes.size() + 2) / 3), '\0');
  const int length =
      EVP_EncodeBlock(reinterpret_cast<unsigned char *>(&out[0]),
                      reinterpret_cast<const unsigned char *>(bytes.data()),
                      static_cast<int>(bytes.size()));
  if (length <= 0)
    return {};
  out.resize(static_cast<std::size_t>(length));
  return "Basic " + out;
}
bool content_input_current_result(const ContentInputCompletion &result,
                                  const ContentInputSnapshot &current,
                                  bool supported) {
  return supported && result.result == ContentUploadResult::Ready &&
         !current.session.empty() && !current.connection.empty() &&
         result.original.session == current.session &&
         result.original.connection == current.connection &&
         (result.part.kind == InputContentKind::Image ||
          result.part.kind == InputContentKind::File) &&
         !result.part.content_ref.empty();
}
ContentInputWorker::ContentInputWorker(HttpPort &http,
                                       ContentInputConfig config)
    : http_(http), config_(std::move(config)) {}
ContentInputWorker::~ContentInputWorker() {
  cancel();
  if (thread_.joinable())
    thread_.join();
}
bool ContentInputWorker::start(ContentInputCommand command,
                               ContentInputSnapshot original) {
  if (!content_input_configured(config_) || !command.attachment ||
      command.path.empty() || command.path.size() > 32768 ||
      command.path.find('\0') != std::string::npos ||
      (command.kind != InputContentKind::Image &&
       command.kind != InputContentKind::File) ||
      original.session.empty() || original.session.size() > 64 ||
      original.connection.empty() || original.connection.size() > 64 ||
      busy_.exchange(true))
    return false;
  cancel_.store(false);
  current_.store(true);
  finished_.store(false);
  try {
    thread_ = std::thread(&ContentInputWorker::execute, this,
                          std::move(command), std::move(original));
  } catch (...) {
    busy_.store(false);
    current_.store(false);
    return false;
  }
  return true;
}
void ContentInputWorker::cancel() {
  current_.store(false);
  cancel_.store(true);
}
std::optional<ContentInputCompletion> ContentInputWorker::take() {
  if (!finished_.load())
    return std::nullopt;
  if (thread_.joinable())
    thread_.join();
  std::lock_guard<std::mutex> lock(mutex_);
  auto out = std::move(completion_);
  completion_.reset();
  finished_.store(false);
  busy_.store(false);
  if (out && cancel_.load()) {
    out->result = ContentUploadResult::Cancelled;
    out->part = {};
  }
  return out;
}
void ContentInputWorker::execute(ContentInputCommand command,
                                 ContentInputSnapshot original) noexcept {
  ContentInputCompletion out;
  out.original = std::move(original);
  try {
    const auto path = std::filesystem::u8path(command.path);
    ContentUploadRequest request;
    request.name = path.filename().u8string();
    request.mime_type = mime(request.name, command.kind);
    std::uint64_t size = 0;
    auto file = request.mime_type.empty() ? std::unique_ptr<FILE, FileCloser>{}
                                          : open_file(path, size);
    const auto maximum = command.kind == InputContentKind::Image
                             ? 20ULL * 1024 * 1024
                             : 50ULL * 1024 * 1024;
    if (file && size <= maximum) {
      const auto hash_start = std::chrono::steady_clock::now();
      const auto deadline =
          hash_start + std::chrono::milliseconds(request.timeout_ms);
      request.sha256 = hash_file(file.get(), size, cancel_, deadline);
      const auto elapsed =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - hash_start)
              .count();
      if (elapsed >= request.timeout_ms) {
        out.result = ContentUploadResult::Timeout;
      } else if (!request.sha256.empty()) {
        request.timeout_ms -= static_cast<std::uint32_t>(elapsed);
        request.api_origin = config_.origin;
        request.authorization = config_.authorization;
        request.session_id = out.original.session;
        request.request_id = random_id();
        request.size = size;
        Source source(file.get(), size, cancel_);
        Runtime runtime(current_);
        SafeHttp http(http_);
        ContentUploader uploader(http, runtime);
        const auto uploaded = uploader.upload(request, source, cancel_);
        out.result = uploaded.result;
        if (uploaded.result == ContentUploadResult::Ready && current_.load() &&
            !cancel_.load()) {
          out.part.kind = command.kind;
          out.part.content_ref = uploaded.content_ref;
          out.part.mime_type = uploaded.mime_type;
        }
      }
    }
  } catch (...) {
    out.result = ContentUploadResult::TransportFailed;
  }
  if (cancel_.load()) {
    out.result = ContentUploadResult::Cancelled;
    out.part = {};
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    completion_ = std::move(out);
  }
  finished_.store(true);
}
} // namespace ur::device
