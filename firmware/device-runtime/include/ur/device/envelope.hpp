// 新 signaling/message 公共信封编解码；只验证公共结构，不代表方法已支持。
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ur::device {
// Envelope 不含设备自报身份；Topic 鉴权和方法分派由接入层完成。
struct Envelope {
    std::string token;                              // 非空 msgToken，最多 128 字节。
    std::string method;                             // 非空 method，最多 64 字节。
    std::string session;                            // 可选 sessionId，是否必填由方法契约决定。
    bool request = true;                            // 请求使用 params，否则使用 data。
    std::string payload = "{}";                     // 独立 JSON 对象，规范化为排序键后紧凑形式。
    std::optional<int> code;                        // 应答或 sessionError 的状态码。
    std::optional<std::uint64_t> event_sequence;    // 仅信令事件水位。
    std::optional<std::uint64_t> message_sequence;  // 仅内容事件水位。
};
// DecodeResult 返回稳定错误分类，不返回包含凭据的原文片段。
enum class DecodeResult { Ok, TooLarge, InvalidJson, InvalidEnvelope };
// canonical_json_object 为不同Topic复用有界JSON安全校验；不赋予对象任何方法或执行权限。
DecodeResult canonical_json_object(std::string_view json, std::string& canonical);
// decode_envelope 失败清空目标；拒绝重复键、过深对象、尾随数据和非法公共字段。
DecodeResult decode_envelope(std::string_view json, Envelope& envelope);
// encode_envelope 复用同一校验，失败清空输出；不添加 version 或隐式身份字段。
DecodeResult encode_envelope(const Envelope& envelope, std::string& json);
}  // namespace ur::device
