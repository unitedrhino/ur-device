// AI语音与无媒体离散输入契约；公共信封成功后仍须做路由、方法和字段级检查。
#pragma once
#include <array>
#include "ur/device/audio_codec.hpp"
#include "ur/device/envelope.hpp"
#include "ur/device/session.hpp"

namespace ur::device {
// VoiceTransport 是当前AI会话的实际媒体通道；None仅承载已选离散输入，不创建媒体，旧设备默认UDP。
enum class VoiceTransport { Udp, WebRTC, None };
// VoiceVisionProfile 是实际摄像头适配器提供的只读配置；从创建请求到终态必须稳定且不可变，
// ready只表示H264发布驱动已预检，不代表本人同意或允许取帧；不可借IPC/附件能力补造。
struct VoiceVisionProfile {
    std::string camera_id;                 // 已预检的原物理摄像头标识。
    std::string video_track = "vision-1";  // 本次创建唯一上行画面轨，与cameraId区分。
    unsigned max_publishers = 1;           // 真实驱动并发上限，范围1..4。
    bool ready = false;                    // 实际驱动可用；默认不得上报aiVision。
};
// vision_profile_binding 返回经严格标识校验的无碰撞内部快照；非法/未就绪返回空，不授取帧许可。
std::string vision_profile_binding(const VoiceVisionProfile* profile);
// VoiceVisionOperation只控制已协商上行画面；不增加轨、不携本人同意或获取其他源的权限。
enum class VoiceVisionOperation { Publish, Stop };
// VoiceVisionCommand保存严格原平台命令；公共token和eventSeq仍由当前会话逐条关联。
struct VoiceVisionCommand {
    Envelope envelope;                        // 原SIG下行请求，含规范化的三字段params。
    std::string track_id;                      // 原已选择的唯一上行画面轨。
    std::uint64_t media_revision = 0;          // 原媒体代次，必须为安全正整数。
    VoiceVisionOperation operation = VoiceVisionOperation::Stop; // 未验证前不默认开启。
};
// VoiceCapability描述真实语音端口或None离散输入模态；平台只能在声明范围内选择。
struct VoiceCapability {
    Capabilities modes;          // 半/全双工及真实 AEC 条件。
    AudioFormat audio;           // 唯一显式声明的编解码格式。
    std::uint64_t revision = 1;  // 当前连接的能力快照号。
    bool text_input = false;     // 明确支持主动文字输入；缺省只保留现有音频能力。
    VoiceTransport transport = VoiceTransport::Udp;  // 只声明真实可用的媒体端点。
    bool file_audio_output = false;                  // 实际文件声音驱动；不从AI扬声器能力自动推断。
    bool file_video_output = false;                  // 实际文件画面驱动；不授权摄像头或AI读取画面。
    bool live_audio_receive = false;  // 实际Opus48k mono20节目接收驱动，与AI16k输入独立。
    bool live_video_receive = false;  // 实际H264解码/可见节目末端，不是相机或文件输出声明。
    bool client_display_theme =
        false;                 // 已真实预检的固定本地主题端口；仅声明审核三字段，不提供schema。
    bool image_input = false;  // 宿主明确装配原件上传及图片输入端口，不从相机或节目能力推断。
    bool file_input = false;   // 宿主明确装配原件上传及文档输入端口，不授权任意本地文件。
    bool ai_vision = false;    // 仅实际专用Profile启用WebRTC摄像头会话，不推断AEC/附件支持。
    bool human_call = false; // 实际装配人际通话本地接听驱动。
    bool human_udp_av = false; // 实际装配固定Opus16/JPEG240人际UDP驱动。
    bool human_voice_call = false; // 显式装配人际纯语音，视频能力不隐含此支持。
    bool human_video_receive = false; // 独立远端接收能力，仍须本次接收轨授权。
    bool session_sync = false;  // 实际装配同一AI控制连接恢复后才显式声明；缺省维持原断连关闭。
    bool ai_endpoint_handoff = false; // 显式装配本地接听及候选双向焦点屏障，不能自动授旧设备接续能力。
    bool ai_endpoint_control_sync = false; // 目标端实际停整组、原租约释放及新连接重新取得全链装配后声明。
    bool udp_reply_start_receipt = false;  // 仅完整装配UDP原回复同步安装回执，缺省不改变旧行为。
};
// VoiceMedia是已鉴权sessionCreated的会话选择；仅语音含短期媒体参数，None无密钥或轨。
struct VoiceMedia {
    Mode mode = Mode::HalfDuplex;  // 仅语音有效；None不协商或使用交互模式。
    AudioFormat audio;             // 平台确认的采样格式。
    std::string host;              // UDP 已授权服务器，视觉选择不复用为摄像头标识。
    std::uint16_t port = 0;        // 非零端口。
    // 附件选择利用现有媒体字段之间的对齐空隙，不放大嵌入式会话对象预算。
    bool image_input = false;                        // 平台在原声明范围选择的图片输入。
    bool file_input = false;                         // 平台在原声明范围选择的文件输入。
    bool udp_reply_start_receipt = false;            // 平台显式选择原Start安装屏障，仅UDP有效。
    bool ai_vision = false;                          // 原专用创建匹配的三轨，视频必须初始paused。
    std::array<std::uint8_t, 16> key{};              // AES-128 密钥。
    std::array<std::uint8_t, 8> nonce{};             // 完整初始头的 nonce 区。
    std::string input_track;                         // 本端 sendonly 轨。
    std::string output_track;                        // 本端 recvonly 轨。
    bool text_input = false;                         // 平台确认的主动文字输入，不能从模型类型猜测。
    bool text_output = false;                        // 主动文字输入可选择的文本输出。
    bool audio_output = false;                       // 主动文字输入可选择的语音输出。
    VoiceTransport transport = VoiceTransport::Udp;  // 媒体协商后不可隐式切换。
};
// VoiceEvent 返回类型化字段；状态机仍须核对请求关联及当前会话/媒体代次。
struct VoiceEvent {
    Envelope envelope;                          // 已通过公共信封验证的元数据。
    std::string request_method;                 // 仅 requestAck 的原方法。
    std::string input_id;                       // ACK 或内容事件关联输入。
    std::string response_id;                    // 音频事件关联回复。
    std::string track_id;                       // 音频事件的下行轨。
    std::string state;                          // 已按方法验证的状态。
    std::string reason;                         // 稳定失败/结束原因。
    std::string scope;                          // sessionError 的影响范围，缺省 session。
    std::string tool_call_id;                   // scope=tool 的调用关联。
    bool retryable = false;                     // 仅 sessionError 有效，须由云端显式提供。
    std::uint64_t revision = 0;                 // 媒体代次或能力确认修订号。
    VoiceMedia media;                           // 只在成功 sessionCreated 时有效。
    std::array<std::uint8_t, 8> reply_nonce{};  // UDP 回复专属下行 nonce，不能使用上行 nonce。
    std::uint32_t first_packet_sequence = 0;  // respAudioStart 预告的首包序号，缺失时不得宣称完整。
    std::uint32_t last_packet_sequence = 0;   // respAudioDone 的实际末包，0代表未产生音频。
    bool udp_boundary = false;                // 是否完整携带UDP边界；WebRTC不得伪造。
    std::string answer_sdp;                   // 仅mediaAnswer的完整SDP，解析后不记录日志。
    std::uint64_t endpoint_epoch = 0;         // 仅协商sessionSync的创建应答返回，不能猜初值。
    std::uint64_t capability_revision = 0;    // 与本次已确认完整能力相同，不是MQTT内部连接epoch。
};
// UDPReplyStartReceipt只保存原安装事实关联；没有nonce、包序号、授权或播放完成信息。
struct UDPReplyStartReceipt {
    Envelope envelope;        // 原Start同token/session的反向应答，无事件水位，不产生ACK-of-ACK。
    std::string response_id;  // 原回复标识。
    std::string track_id;     // 原下行声音轨。
    std::uint64_t media_revision = 0;  // 原安全正媒体代次，不增代次。
};
// WireResult 区分错误格式、未知能力与路由不符，不返回原始报文片段。
enum class WireResult { Ok, Invalid, Unsupported, WrongRoute };
// WireRoute 为已校验 Topic 的业务路由，不把正文字符串误传成路由。
enum class WireRoute { Signaling, Message };
// reply_start_receipt只编码已实际安装的原Start；调用方须先完成同步HAL与Session安装。
WireResult reply_start_receipt(const VoiceEvent& start, Envelope& out);
// decode_reply_start_receipt独立解析SIG上行200三字段；不能接入普通下行或当作播放完成。
WireResult decode_reply_start_receipt(WireRoute route, std::string_view body,
                                      UDPReplyStartReceipt& out);
// decode_voice_vision_control仅解析SIG下行publish/stop；不能将trackState或message当作取帧许可。
WireResult decode_voice_vision_control(WireRoute route, std::string_view body,
                                      VoiceVisionCommand& out);
// voice_vision_receipt编码原token实际回执：200仅active/stopped，失败只reason；不带新事件水位。
WireResult voice_vision_receipt(const VoiceVisionCommand& command, int code,
                               std::string_view state, std::string_view reason, Envelope& out);
// capability_command生成当前Topic的真实能力；None的features显式为空，离散输入归独立模态。
WireResult capability_command(std::string token, const VoiceCapability& capability, Envelope& out,
                              const VoiceVisionProfile* vision = nullptr);
// create_voice_command按声明生成UDP/WebRTC语音或无媒体离散输入会话；不含设备身份或供应商名称。
WireResult create_voice_command(std::string token, const VoiceCapability& capability, Envelope& out,
                                const VoiceVisionProfile* vision = nullptr);
// media_offer_command 编码设备Peer已完成候选收集的SDP；仅WebRTC会话使用。
WireResult media_offer_command(std::string token, std::string session, std::string_view sdp,
                               Envelope& out);
// media_offer_command 的显式代次入口用于原会话媒体重协商；只编码安全正整数，
// 不授权恢复、推进状态或创建新会话，代次连续性仍由客户端和平台各自校验。
WireResult media_offer_command(std::string token, std::string session, std::string_view sdp,
                               std::uint64_t revision, Envelope& out);
// text_input_command 编码单轮文字；input_id重试保持不变，不在MQTT内携带媒体字节。
WireResult text_input_command(std::string token, std::string session, std::string input_id,
                              std::string_view text, bool audio, Envelope& out);
// voice_control 生成输入启停、定向取消、媒体就绪及结束命令；id为对象或结束原因。
// packet_sequence为audioStart下一发包序号、audioStop本段末包（空段0）；其他方法必须为0。
WireResult voice_control(std::string token, std::string session, std::string method, std::string id,
                         std::uint64_t revision, Envelope& out, std::uint32_t packet_sequence = 0,
                         VoiceTransport transport = VoiceTransport::Udp);
// decode_voice_event 只处理 signaling 下行音频控制；未知方法交其他显式适配器，不能默认为成功。
WireResult decode_voice_event(WireRoute route, std::string_view body, VoiceEvent& out,
                              const VoiceVisionProfile* vision = nullptr);
}  // namespace ur::device
