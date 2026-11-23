// 文件说明：人际设备接续的窄本地桥合同；不扩大通用桥DTO，不携带平台凭据或任意method。
#pragma once
#include "ur/device/endpoint_handoff_client.hpp"
#include <string>
#include <string_view>

namespace ur::device {
// EndpointBrowserDecode
// 区分其他原桥命令与本模块畸形输入；Invalid不得降级成通用命令。
enum class EndpointBrowserDecode { Other, Invalid, Ok };
// EndpointBrowserRequest
// 只声明本地呼叫/接续意图；当前会话必须再与HumanCall原腿核对。
struct EndpointBrowserRequest {
  std::string kind; // callDial/endpointPrepare/endpointCommit/endpointAbort。
  std::string session; // 除callDial外必填，不信任其授权含义。
  std::string target;  // callDial使用p:d，prepare使用device:p:d；其余必须缺省。
};
// decode_endpoint_browser
// 严格核重复键、字段和有界ID；失败清空，Other保留原桥分派。
EndpointBrowserDecode decode_endpoint_browser(std::string_view body,
                                              EndpointBrowserRequest &result);
// browser_endpoint_receipt
// 只序列化核心已核验投影；失败返回空，不把ACK或pending当实际媒体结果。
std::string browser_endpoint_receipt(const EndpointHandoffReceipt &receipt);
} // namespace ur::device
