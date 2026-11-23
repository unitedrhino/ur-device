// 物模型动作线协议，与signaling/message信封分开；解析成功不代表驱动已执行。
#pragma once
#include "ur/device/envelope.hpp"

namespace ur::device {
// ActionRequest 保存不可变请求，用规范化参数比较同Token重投，设备身份只取MQTT Topic。
struct ActionRequest {
    std::string token;          // 平台请求关联，回复原样保留。
    std::string action;         // 大小写敏感物模型标识。
    std::string params = "{}";  // 参数对象，具体类型/范围由动作驱动校验。
};
// ActionReply 是驱动完成事实；200只能在已执行成功后使用。
struct ActionReply {
    std::string token;        // 原请求Token，不能换成新生成的标识。
    std::string action;       // 原请求行为标识。
    int code = 500;           // 默认失败，避免缺失驱动结果时伪造成功。
    std::string message;      // 对设备公开的稳定说明，不填底层错误/凭据。
    std::string data = "{}";  // 实际输出对象，不直接回显输入冒充执行结果。
};
// decode_action_request 只解码action；接受无参数动作及旧actionId拼写，但禁止双字段歧义。
DecodeResult decode_action_request(std::string_view json, ActionRequest& request);
// encode_action_reply 生成现行actionReply，不添加新信令sessionId/序号/version。
DecodeResult encode_action_reply(const ActionReply& reply, std::string& json);
}  // namespace ur::device
