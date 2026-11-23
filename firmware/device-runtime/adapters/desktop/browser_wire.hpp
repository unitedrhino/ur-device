// 本地界面桥消息：不复用云端Topic信封，设备身份和会话状态始终由C++持有。
#pragma once
#include "browser_pcm.hpp"
#include "ur/device/content_wire.hpp"
#include "ur/device/udp_audio.hpp"
#include "ur/device/reply_datagram.hpp"
#include "ur/device/voice_runtime.hpp"

namespace ur::device {
// BrowserRequest为已校验界面指令；字段只在对应kind有效，不直接驱动云端动作。
struct BrowserRequest {
    std::string kind;               // ready/textReady/控制/PCM回调/WebRTC媒体事实。
    std::string id;                 // 输入或回复标识。
    std::string text;               // sendText正文，仅交给已协商的C++会话状态机。
    std::string sdp;                // webrtcOffer完整SDP，只交给C++协议状态机。
    bool audio = false;             // sendText是否请求语音输出。
    AudioFormat format;             // ready声明的实际AudioContext格式。
    bool aec = false;               // 浏览器getSettings确认的实际AEC。
    std::uint64_t epoch = 0;        // played代次。
    std::uint64_t peer_revision = 0; // WebRTC本地Peer代次，不是页面自报的云端session身份。
    std::uint32_t ticket = 0;       // played完成帧水位。
    unsigned volume = 0;            // webrtcVolumeApplied已实际应用的浏览器增益。
    std::uint64_t gate_ticket = 0;  // outputGated的独立完整64位门控票据，不是played帧号。
    bool output_enabled = true;     // outputGated的实际门控状态，不控制麦克风。
    std::vector<std::int16_t> pcm;  // captured一帧有界PCM。
    std::string capture_session;    // 仅显式输入仲裁帧携带的原scope；不是平台设备身份或云端授权。
    std::uint64_t capture_generation = 0;  // 必须成对存在且为安全十进制字符串，不从当前页面补值。
};
// decode_browser_request拒绝畸形、重复键及未知命令，失败清空结果。
bool decode_browser_request(std::string_view body, BrowserRequest& result);
// BrowserResponseControl仅锁存原核心同会话/回复的内容终态，不拥有媒体或另建会话状态机。
class BrowserResponseControl {
public:
    // observe_terminal只接宿主已经核验的内容；不匹配原Core会话/回复或非终态不得退休控制目标。
    void observe_terminal(const ContentEvent& event, const VoiceClient& voice);
    // target只读Running下Established/Recovering原核心回复；Peer播放ID不能提供控制资格。
    std::string_view target(const VoiceClient& voice, RuntimeState runtime) const;
    // cancel_allowed核原Core目标且非空；成功仍须交原cancel_response判幂等/取消墓碑。
    bool cancel_allowed(std::string_view response_id, const VoiceClient& voice,
                        RuntimeState runtime) const;
private:
    std::string terminal_session_;   // 最多原128字节会话，终态仅抑制同会话的最近回复。
    std::string terminal_response_;  // 最多原128字节回复；新目标不受旧Done抑制。
};
// browser_response_control_state只追加已配对HAL状态的原核心目标；空串清除，不新增云端协议字段。
std::string browser_response_control_state(std::string_view state, std::string_view response_id);
// browser_command/frame返回独立JSON；epoch始终字符串，避免JavaScript丢失精度。
std::string browser_command(const BrowserAudioCommand& command);
std::string browser_frame(const PlaybackFrame& frame);
// browser_text编码已验证展示文本；可选response_id只来自原内容事件，空时保留旧JSON，非法非空返回空报文。
std::string browser_text(std::string_view text, std::string_view response_id = {});
// browser_tool_status传递工具标识/名称/状态及可选原response_id；空ID保留旧JSON，参数及结果正文不进入浏览器。
std::string browser_tool_status(const ContentEvent& event);
// browser_message_diagnostics
// 只编码最多32项固定观测；非法枚举/计数拒绝，不记录原标识或正文。
std::string browser_message_diagnostics(const MessageDiagnostics& diagnostic);
// browser_lifecycle_diagnostic 只编码三个一次锁存事实；非法枚举/计数拒绝，不含正文或身份。
std::string browser_lifecycle_diagnostic(const VoiceLifecycleDiagnostic& diagnostic);
// browser_runtime_failure_diagnostic
// 将实际runtime首因及同次UDP/PCM原因编码为固定JSON；
// 非法枚举或跨层组合返回空字符串，不包含原报文、身份、回复或媒体凭据。
std::string browser_runtime_failure_diagnostic(RuntimeFailure runtime,
                                               std::optional<UdpFailure> udp = {},
                                               PcmWriteFailure pcm = PcmWriteFailure::None);
// browser_client_failure_diagnostic 编码独立五字段闭集；非法枚举/来源组合拒绝，不输出原标识。
std::string browser_client_failure_diagnostic(const ClientFailureDiagnostic& diagnostic);
// browser_scope_failure_diagnostic
// 只编码候选Active首个原许可拒绝，不输出绑定或自由错误。
std::string browser_scope_failure_diagnostic(ScopeFailureCode code);
// browser_presignal_diagnostic仅编码固定计数/完整性，不包含回复标识或媒体，非法关系返回空。
std::string browser_presignal_diagnostic(const ReplyPreSignalDiagnostic& diagnostic);
// browser_completion关联回复、云端音频状态和本机接收/解码计数，媒体计数不承诺物理播放。
std::string browser_completion(std::string_view response_id, std::string_view audio_state,
                               const AudioTiming& timing);
// browser_media_final在停播时报告最终收包、可见缺包及解码事实；首包未到无法在现行报文中证明。
std::string browser_media_final(std::string_view response_id, const AudioTiming& timing);
// browser_reply_timing只投影原C++单调时钟，缺失为null，不用网页/WAV时钟补值或计算延迟。
// last_voice_sent_ms为最后实际发出有效语音帧，first_text_ms为原回复首文本；合法实时重叠保留原事实。
std::string browser_reply_timing(std::string_view response_id, const AudioTiming& timing,
                                std::optional<std::uint64_t> last_voice_sent_ms,
                                std::optional<std::uint64_t> first_text_ms);
}  // namespace ur::device
