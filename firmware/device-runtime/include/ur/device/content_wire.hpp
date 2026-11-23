// AI 内容报文的类型化边界；展示通知不授权执行设备动作，不驱动音频播放。
#pragma once
#include <vector>
#include "ur/device/voice_wire.hpp"

namespace ur::device {
// InputContentKind 只开放本轮已实现的文字/图片/文件；音视频原件不能被当成已支持输入。
enum class InputContentKind { Text, Image, File };
// InputContentPart 保存原有序离散内容，媒体只含已上传artifact_引用，不持有原件或签名URL。
struct InputContentPart {
    InputContentKind kind = InputContentKind::Text;  // 当前实现的明确模态。
    std::string text;                                // 仅Text有效，可包含正常换行。
    std::string content_ref;                         // Image/File实际Ready原引用，非对象键。
    std::string mime_type;                           // 当前附件格式表中的实际MIME。
};
// content_input_command 编码现行message/inputSend；1至16段及整帧64KiB由公共信封限制。
// 成功只证明语法，上传归属、撤权和模型能力仍由原云端会话复核；失败清空输出。
WireResult content_input_command(std::string token, std::string session, std::string input_id,
                                 const std::vector<InputContentPart>& parts, bool audio,
                                 Envelope& out);
// ContentEvent 保留单条不可变内容事实，不在核心累计整段对话或执行工具参数。
struct ContentEvent {
    Envelope envelope;                // 已校验 message 路由信封，包含会话及 messageSeq。
    std::string response_id;          // 内容归属回复。
    std::string input_id;             // 有确切关联时的输入段；不能从当前开麦状态推测。
    std::string utterance_id;         // 平台识别的语音条目，可与持续采集段形成多对一关系。
    std::string text;                 // 增量或最终文本，允许空最终结果和换行。
    std::string emoji;                // 白名单UI表情，不驱动物理执行器。
    std::string emotion;              // neutral/happy/sad/angry/surprised。
    std::string tool_call_id;         // 仅工具通知有效。
    std::string tool_name;            // 仅工具开始通知有效。
    std::string object_json;          // 工具参数或脱敏结果对象；仅展示，禁止执行。
    std::string status;               // 工具或回复终态，按方法分别校验枚举。
    std::string reason;               // 非正常回复终态的稳定原因。
    std::string audio_state;          // 聚合终态的网络发送状态，不是播放状态。
    std::uint64_t content_index = 0;  // 文本段编号，从零开始。
    std::uint64_t content_count = 0;  // 聚合终态的最终段数。
};
// decode_content_event 只支持第一阶段 AI
// 展示消息；未实现的客户端工具执行显式拒绝。
// 失败清空输出，不输出原报文或凭据；调用方仍须核对设备 Topic、sessionId
// 和消息水位。
WireResult decode_content_event(WireRoute route, std::string_view body, ContentEvent& out);
}  // namespace ur::device
