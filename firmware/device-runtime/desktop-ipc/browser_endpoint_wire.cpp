// 文件说明：本地端切换命令严格解析与有限状态投影；不操作MQTT、授权或HAL。
#include "browser_endpoint_wire.hpp"
#include "ur/device/json_parse.hpp"
#include <algorithm>
#include <cJSON.h>
#include <memory>

namespace ur::device {
namespace {
using Json =
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)>; // 所有失败出口释放参数树。
// text 对可选或必填标识采用一致边界，排除控制字符与过长自由正文。
bool text(std::string_view value, std::size_t maximum, bool required = true) {
  return (!required || !value.empty()) && value.size() <= maximum &&
         std::none_of(value.begin(), value.end(),
                      [](unsigned char ch) { return ch < 32 || ch == 127; });
}
// field 只读取字符串，数值与null不得隐式转成当前会话或设备标识。
std::string field(const cJSON *object, const char *key) {
  const auto *value = cJSON_GetObjectItemCaseSensitive(object, key);
  return cJSON_IsString(value) ? value->valuestring : "";
}
// device_id 与正式设备方向保持规范两段身份；空段、分隔符注入和多余前缀均拒绝。
bool device_id(std::string_view value) {
  const auto split = value.find(':');
  return text(value, 128) && split != std::string_view::npos && split > 0 &&
         split + 1 < value.size() &&
         value.find(':', split + 1) == std::string_view::npos &&
         value.find('/') == std::string_view::npos &&
         value.find('\\') == std::string_view::npos;
}
} // namespace

EndpointBrowserDecode decode_endpoint_browser(std::string_view body,
                                              EndpointBrowserRequest &result) {
  result = {};
  std::string canonical;
  if (canonical_json_object(body, canonical) != DecodeResult::Ok)
    return EndpointBrowserDecode::Invalid;
  Json root(parse_json(canonical.c_str()), cJSON_Delete);
  if (!root)
    return EndpointBrowserDecode::Invalid;
  EndpointBrowserRequest request;
  request.kind = field(root.get(), "kind");
  if (request.kind != "callDial" && request.kind != "endpointPrepare" &&
      request.kind != "endpointCommit" && request.kind != "endpointAbort")
    return EndpointBrowserDecode::Other;
  const auto count = cJSON_GetArraySize(root.get());
  if (request.kind == "callDial") {
    request.target = field(root.get(), "target");
    if (count != 2 || !device_id(request.target))
      return EndpointBrowserDecode::Invalid;
  } else {
    request.session = field(root.get(), "sessionId");
    if (!text(request.session, 64))
      return EndpointBrowserDecode::Invalid;
    if (request.kind == "endpointPrepare") {
      request.target = field(root.get(), "target");
      if (count != 3 || request.target.rfind("device:", 0) != 0 ||
          !text(request.target, 128) ||
          !device_id(std::string_view(request.target).substr(7)))
        return EndpointBrowserDecode::Invalid;
    } else if (count != 2) {
      return EndpointBrowserDecode::Invalid;
    }
  }
  result = std::move(request);
  return EndpointBrowserDecode::Ok;
}

std::string browser_endpoint_receipt(const EndpointHandoffReceipt &receipt) {
  // 尚未绑定原腿时没有可投影的接续事实；不能在浏览器配对前触发桥发送失败。
  if (!text(receipt.session, 64) || !text(receipt.handoff, 128, false) ||
      !text(receipt.candidate, 128, false) ||
      !text(receipt.token, 128, false) ||
      receipt.revision > 9007199254740991ULL ||
      (receipt.code && (receipt.code < 200 || receipt.code > 599)))
    return {};
  if (receipt.state != "" && receipt.state != "preparing" &&
      receipt.state != "ready" && receipt.state != "committed" &&
      receipt.state != "aborted" && receipt.state != "failed")
    return {};
  if (receipt.operation != "" && receipt.operation != "prepare" &&
      receipt.operation != "commit" && receipt.operation != "abort")
    return {};
  if (receipt.reason != "" && receipt.reason != "handoffTimeout" &&
      receipt.reason != "userCancelled" && receipt.reason != "mediaFailed" &&
      receipt.reason != "permissionRevoked" &&
      receipt.reason != "sourceChanged")
    return {};
  Json object(cJSON_CreateObject(), cJSON_Delete);
  if (!object ||
      !cJSON_AddStringToObject(object.get(), "kind", "endpointStatus") ||
      !cJSON_AddStringToObject(object.get(), "sessionId",
                               receipt.session.c_str()) ||
      !cJSON_AddStringToObject(object.get(), "handoffId",
                               receipt.handoff.c_str()) ||
      !cJSON_AddStringToObject(object.get(), "state",
                               receipt.state.empty() ? "idle"
                                                     : receipt.state.c_str()) ||
      !cJSON_AddStringToObject(object.get(), "candidateSessionId",
                               receipt.candidate.c_str()) ||
      !cJSON_AddStringToObject(object.get(), "reason",
                               receipt.reason.c_str()) ||
      !cJSON_AddStringToObject(object.get(), "operation",
                               receipt.operation.c_str()) ||
      !cJSON_AddStringToObject(object.get(), "msgToken",
                               receipt.token.c_str()) ||
      !cJSON_AddNumberToObject(object.get(), "revision",
                               static_cast<double>(receipt.revision)) ||
      !cJSON_AddNumberToObject(object.get(), "code", receipt.code) ||
      !cJSON_AddBoolToObject(object.get(), "pending", receipt.pending) ||
      !cJSON_AddBoolToObject(object.get(), "unknown", receipt.unknown))
    return {};
  std::unique_ptr<char, decltype(&cJSON_free)> encoded(
      cJSON_PrintUnformatted(object.get()), cJSON_free);
  return encoded ? std::string(encoded.get()) : std::string{};
}
} // namespace ur::device
