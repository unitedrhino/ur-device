// 附件上传：严格metadata→原件流式PUT→原租约确认；各失败出口丢弃敏感响应。
#include "ur/device/content_upload.hpp"
#include "ur/device/json_parse.hpp"

#include <cJSON.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <string_view>
#include <utility>

#include "ur/device/envelope.hpp"

namespace ur::device {
namespace {
using Json =
    std::unique_ptr<cJSON,
                    decltype(&cJSON_Delete)>; // 本次有界JSON树的唯一所有权。
// small_id 限制会话及素材引用的安全字符，RequestID另按设备HTTP专用约束校验。
bool small_id(std::string_view value, std::size_t limit) {
  return !value.empty() && value.size() <= limit &&
         std::all_of(value.begin(), value.end(), [](char ch) {
           return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                  (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' ||
                  ch == '.' || ch == ':';
         });
}
// kind_of 根据平台已有文件类型表校验metadata；名称不是原件格式证明。
std::string kind_of(const ContentUploadRequest &request) {
  if (!small_id(request.session_id, 64) || request.request_id.empty() ||
      request.request_id.size() > 64 ||
      !std::all_of(request.request_id.begin(), request.request_id.end(),
                   [](char ch) {
                     return (ch >= 'a' && ch <= 'z') ||
                            (ch >= 'A' && ch <= 'Z') ||
                            (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
                   }) ||
      request.name.empty() || request.name.size() > 255 ||
      request.name == "." || request.name == ".." ||
      request.name.find_first_of("/\\\r\n") != std::string::npos ||
      request.name.find('\0') != std::string::npos ||
      request.sha256.size() != 64 ||
      !std::all_of(request.sha256.begin(), request.sha256.end(),
                   [](char ch) {
                     return (ch >= '0' && ch <= '9') ||
                            (ch >= 'a' && ch <= 'f');
                   }) ||
      request.size == 0 || request.size > 50ULL * 1024 * 1024 ||
      request.timeout_ms == 0 || request.timeout_ms > 120000)
    return {};
  const auto dot = request.name.find_last_of('.');
  if (dot == std::string::npos)
    return {};
  std::string extension = request.name.substr(dot);
  for (auto &ch : extension)
    if (ch >= 'A' && ch <= 'Z')
      ch = static_cast<char>(ch + ('a' - 'A'));
  // 支持列表跟随现行AISVR deviceNameFormat，不自行扩音频/视频上传能力。
  constexpr std::array<std::pair<std::string_view, std::string_view>, 17>
      formats = {
          {{".png", "image/png"},
           {".jpg", "image/jpeg"},
           {".jpeg", "image/jpeg"},
           {".gif", "image/gif"},
           {".bmp", "image/bmp"},
           {".webp", "image/webp"},
           {".txt", "text/plain"},
           {".csv", "text/csv"},
           {".json", "application/json"},
           {".md", "text/markdown"},
           {".adoc", "text/asciidoc"},
           {".html", "text/html"},
           {".eml", "message/rfc822"},
           {".pdf", "application/pdf"},
           {".docx",
            "application/"
            "vnd.openxmlformats-officedocument.wordprocessingml.document"},
           {".pptx",
            "application/"
            "vnd.openxmlformats-officedocument.presentationml.presentation"},
           {".xlsx", "application/"
                     "vnd.openxmlformats-officedocument.spreadsheetml.sheet"}}};
  for (const auto &format : formats)
    if (extension == format.first && request.mime_type == format.second) {
      const bool image = request.mime_type.compare(0, 6, "image/") == 0;
      if (image && request.size > 20ULL * 1024 * 1024)
        return {};
      return image ? "image" : "file";
    }
  return {};
}
// metadata_request 只给平台固定路径附加Basic；OSS请求不经过此函数。
HttpRequest metadata_request(const ContentUploadRequest &in,
                             std::string_view suffix, std::uint32_t remaining) {
  HttpRequest out;
  out.url = in.api_origin + "/api/v1/things/device/edge/content/" +
            std::string(suffix);
  out.headers = {{"Authorization", in.authorization},
                 {"Content-Type", "application/json"}};
  out.timeout_ms = remaining;
  out.connect_timeout_ms = std::min<std::uint32_t>(10000, remaining);
  return out;
}
// StringSource 只借用本阶段metadata，不接触原件、文件系统或设备长期密钥。
class StringSource final : public HttpBodySource {
public:
  explicit StringSource(const std::string &value) : value_(value) {}
  bool read(std::uint8_t *buffer, std::size_t capacity,
            std::size_t &count) override {
    count = std::min(capacity, value_.size() - offset_);
    if (count)
      std::memcpy(buffer, value_.data() + offset_, count);
    offset_ += count;
    return true;
  }

private:
  const std::string &value_; // 当前perform返回前稳定的JSON字符串。
  std::size_t offset_ = 0;   // 顺序游标，不支持seek/自动重放。
};
// encoded_metadata 复用公共UTF-8/NUL/重复键/预算校验；非法名字不进入网络。
std::string encoded_metadata(const ContentUploadRequest &in, bool prepare,
                             const std::string &reference,
                             const std::string &receipt) {
  Json root(cJSON_CreateObject(), cJSON_Delete);
  if (!root ||
      !cJSON_AddStringToObject(root.get(), "sessionId", in.session_id.c_str()))
    return {};
  if (prepare) {
    if (!cJSON_AddStringToObject(root.get(), "requestId",
                                 in.request_id.c_str()) ||
        !cJSON_AddStringToObject(root.get(), "name", in.name.c_str()) ||
        !cJSON_AddStringToObject(root.get(), "mimeType",
                                 in.mime_type.c_str()) ||
        !cJSON_AddNumberToObject(root.get(), "size",
                                 static_cast<double>(in.size)) ||
        !cJSON_AddStringToObject(root.get(), "sha256", in.sha256.c_str()))
      return {};
  } else if (!cJSON_AddStringToObject(root.get(), "contentRef",
                                      reference.c_str()) ||
             !cJSON_AddStringToObject(root.get(), "uploadReceipt",
                                      receipt.c_str()))
    return {};
  char *raw = cJSON_PrintUnformatted(root.get());
  if (!raw)
    return {};
  std::string canonical;
  const auto result = canonical_json_object(raw, canonical);
  cJSON_free(raw);
  return result == DecodeResult::Ok ? canonical : std::string{};
}
// text 读取实际字段，不把null、超长或控制字符折叠成缺省。
bool text(const cJSON *object, const char *key, std::string &out,
          std::size_t limit, bool empty = false) {
  const auto *field = cJSON_GetObjectItemCaseSensitive(object, key);
  if (!cJSON_IsString(field))
    return false;
  out = field->valuestring;
  return (empty || !out.empty()) && out.size() <= limit &&
         std::none_of(out.begin(), out.end(),
                      [](unsigned char ch) { return ch < 32 || ch == 127; });
}
// number 限制公开毫秒截止及业务code为JSON安全整数。
bool number(const cJSON *object, const char *key, std::uint64_t &out) {
  const auto *field = cJSON_GetObjectItemCaseSensitive(object, key);
  if (!cJSON_IsNumber(field) || !std::isfinite(field->valuedouble) ||
      field->valuedouble < 0 || field->valuedouble > 9007199254740991.0 ||
      std::floor(field->valuedouble) != field->valuedouble)
    return false;
  out = static_cast<std::uint64_t>(field->valuedouble);
  return true;
}
// keys 拒绝成功metadata中的未定义字段，不默许私有租约/对象键进入设备合同。
bool keys(const cJSON *object,
          std::initializer_list<std::string_view> allowed) {
  for (auto *field = object->child; field; field = field->next)
    if (!field->string || std::find(allowed.begin(), allowed.end(),
                                    field->string) == allowed.end())
      return false;
  return true;
}
// failure_code 区分HTTP状态与平台REST业务码；只匹配既定分类，不回传错误正文。
ContentUploadResult failure_code(std::uint64_t code,
                                 std::string_view message = {}) {
  if (code == 401 || code == 403 || code == 100012)
    return ContentUploadResult::Denied;
  if (code == 409)
    return ContentUploadResult::Conflict;
  if (code == 422 || code == 100013)
    return ContentUploadResult::Unsupported;
  // Things以Parameter同时表示非法元数据和幂等冲突；仅固定消息允许区分后者。
  if (code == 100006)
    return message == "contentConflict" ? ContentUploadResult::Conflict
                                        : ContentUploadResult::Invalid;
  return ContentUploadResult::ServiceFailed;
}
// reply_tree 经公共校验后才消费code/data，不相信HTTP成功或仅有contentRef。
ContentUploadResult reply_tree(const HttpResponse &response, Json &root,
                               const cJSON *&data) {
  if (response.result != HttpResult::Ok || response.status < 200 ||
      response.status >= 300) {
    if (response.result == HttpResult::Cancelled)
      return ContentUploadResult::Cancelled;
    if (response.result == HttpResult::Timeout)
      return ContentUploadResult::Timeout;
    if (response.result == HttpResult::HttpFailed)
      return failure_code(response.status);
    return ContentUploadResult::TransportFailed;
  }
  std::string canonical;
  if (canonical_json_object(response.body, canonical) != DecodeResult::Ok)
    return ContentUploadResult::InvalidReply;
  root.reset(ur::device::parse_json(canonical.c_str()));
  std::uint64_t code = 0;
  if (!root || !number(root.get(), "code", code))
    return ContentUploadResult::InvalidReply;
  if (code != 200) {
    const auto *message = cJSON_GetObjectItemCaseSensitive(root.get(), "msg");
    return failure_code(code,
                        cJSON_IsString(message) ? message->valuestring : "");
  }
  data = cJSON_GetObjectItemCaseSensitive(root.get(), "data");
  return cJSON_IsObject(data) ? ContentUploadResult::Ready
                              : ContentUploadResult::InvalidReply;
}
// BusyGuard 确保网络、取消和格式失败均释放实例，不因异常结果永久占用worker。
struct BusyGuard {
  std::atomic_bool &busy; // 已成功占用的当前实例。
  ~BusyGuard() { busy.store(false); }
};
} // namespace

ContentUploadOutcome
ContentUploader::upload(const ContentUploadRequest &request,
                        HttpBodySource &source,
                        const std::atomic_bool &cancel) {
  ContentUploadOutcome out;
  if (busy_.exchange(true)) {
    out.result = ContentUploadResult::Busy;
    return out;
  }
  BusyGuard guard{busy_};
  const auto kind = kind_of(request);
  auto prepare = metadata_request(request, "prepare", request.timeout_ms);
  // 平台URL只来自可信origin；禁止地址携带路径/查询或percent编码的认证信息。
  if (kind.empty() || request.api_origin.compare(0, 8, "https://") != 0 ||
      request.api_origin.find_first_of("/?#%@", 8) != std::string::npos ||
      request.authorization.compare(0, 6, "Basic ") != 0 ||
      request.authorization.size() <= 6 || !valid_http_request(prepare))
    return out;
  const auto start = runtime_.monotonic_ms();
  const auto wall_start = runtime_.unix_ms();
  if (wall_start == 0 || wall_start > 9007199254620991ULL)
    return out;
  std::uint64_t expires = 0;      // 取得签名后只收紧，不能在Complete阶段续期。
  std::uint64_t previous = start; // 单调时钟倒退视为截止不可信，不能扩大预算。
  std::uint64_t observed_wall =
      wall_start; // 每阶段已核验的校时时钟水位，接收授权后也不容许回拨。
  // checkpoint 在每个阶段边界复核原session/连接及双时钟截止。
  const auto checkpoint = [&]() -> std::uint32_t {
    if (cancel.load()) {
      out.result = ContentUploadResult::Cancelled;
      return 0;
    }
    if (!runtime_.current()) {
      out.result = ContentUploadResult::Inactive;
      return 0;
    }
    const auto now = runtime_.monotonic_ms();
    const auto wall = runtime_.unix_ms();
    if (now < previous || now - start >= request.timeout_ms ||
        wall < observed_wall || (expires && wall >= expires)) {
      out.result = ContentUploadResult::Timeout;
      return 0;
    }
    previous = now;
    observed_wall = wall;
    auto remaining =
        static_cast<std::uint32_t>(request.timeout_ms - (now - start));
    if (expires)
      remaining = static_cast<std::uint32_t>(
          std::min<std::uint64_t>(remaining, expires - wall));
    return remaining;
  };
  const auto perform = [&](HttpRequest http, HttpBodySource &body,
                           std::uint64_t length) {
    http.body_size = length;
    const auto remaining = checkpoint();
    if (!remaining)
      return HttpResponse{};
    http.timeout_ms = remaining;
    http.connect_timeout_ms = std::min<std::uint32_t>(10000, remaining);
    return http_.perform(http, &body, cancel);
  };
  std::string body = encoded_metadata(request, true, {}, {});
  if (body.empty())
    return out;
  StringSource prepare_source(body);
  if (!checkpoint())
    return out;
  auto response = perform(prepare, prepare_source, body.size());
  if (!checkpoint())
    return out;
  out.transport_result = response.result;
  Json root(nullptr, cJSON_Delete);
  const cJSON *data = nullptr;
  out.result = reply_tree(response, root, data);
  if (out.result != ContentUploadResult::Ready)
    return out;
  std::string reference, type, mime, status, url, receipt;
  if (!keys(data, {"contentRef", "type", "mimeType", "status", "uploadUrl",
                   "uploadReceipt", "expiresAt"}) ||
      !text(data, "contentRef", reference, 64) || !small_id(reference, 64) ||
      reference.find_first_of(".:") != std::string::npos ||
      reference.size() <= 9 || reference.compare(0, 9, "artifact_") != 0 ||
      !text(data, "type", type, 8) || type != kind ||
      !text(data, "mimeType", mime, 128) || mime != request.mime_type ||
      !text(data, "status", status, 16) ||
      !text(data, "uploadUrl", url, 8192, true) ||
      !text(data, "uploadReceipt", receipt, 4096, true) ||
      !number(data, "expiresAt", expires)) {
    out.result = ContentUploadResult::InvalidReply;
    return out;
  }
  if (status == "ready") {
    if (!url.empty() || !receipt.empty() || expires != 0) {
      out.result = ContentUploadResult::InvalidReply;
      return out;
    }
  } else {
    // 平台在Prepare授权与持久化之后才签发；120秒限额按响应到达的可信校时核验，
    // 不能误用发请求前的起点拒绝合法慢响应。原单调总截止和绝对expires仍由checkpoint共同夹紧。
    if (status != "uploading" || url.empty() || receipt.empty() ||
        expires == 0 ||
        (expires > observed_wall && expires - observed_wall > 120000)) {
      out.result = ContentUploadResult::InvalidReply;
      return out;
    }
    if (!checkpoint())
      return out;
    HttpRequest put;
    put.method = HttpMethod::Put;
    put.url = url;
    put.headers = {{"Content-Type", mime}};
    put.body_size = request.size;
    put.response_limit = 1024; // 不消费OSS正文；错误正文不能通过结果回显。
    if (!valid_http_request(put)) {
      out.result = ContentUploadResult::InvalidReply;
      return out;
    }
    out.stage = ContentUploadStage::Put;
    response = perform(put, source, request.size);
    if (!checkpoint())
      return out;
    out.transport_result = response.result;
    if (response.result != HttpResult::Ok || response.status < 200 ||
        response.status >= 300) {
      out.result = response.result == HttpResult::Cancelled
                       ? ContentUploadResult::Cancelled
                   : response.result == HttpResult::Timeout
                       ? ContentUploadResult::Timeout
                       : ContentUploadResult::TransportFailed;
      return out;
    }
    out.stage = ContentUploadStage::Complete;
    body = encoded_metadata(request, false, reference, receipt);
    if (body.empty()) {
      out.result = ContentUploadResult::InvalidReply;
      return out;
    }
    StringSource complete_source(body);
    response =
        perform(metadata_request(request, "complete", request.timeout_ms),
                complete_source, body.size());
    if (!checkpoint())
      return out;
    out.transport_result = response.result;
    root.reset();
    data = nullptr;
    out.result = reply_tree(response, root, data);
    if (out.result != ContentUploadResult::Ready)
      return out;
    std::string completed_reference, completed_type, completed_mime;
    if (!keys(data, {"contentRef", "type", "mimeType", "status"}) ||
        !text(data, "contentRef", completed_reference, 64) ||
        completed_reference != reference ||
        !text(data, "type", completed_type, 8) || completed_type != type ||
        !text(data, "mimeType", completed_mime, 128) ||
        completed_mime != mime || !text(data, "status", status, 16) ||
        status != "ready") {
      out.result = ContentUploadResult::InvalidReply;
      return out;
    }
  }
  // 仅输出原件ready引用；宿主还须在事件线程检查最新能力/会话后发inputSend。
  out.content_ref = std::move(reference);
  out.type = std::move(type);
  out.mime_type = std::move(mime);
  out.result = ContentUploadResult::Ready;
  return out;
}
} // namespace ur::device
