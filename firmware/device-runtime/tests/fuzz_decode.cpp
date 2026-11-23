// libFuzzer 入口：不可信 JSON/UDP 不得崩溃；密码替身只为覆盖包边界检查。
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include "ur/device/action_wire.hpp"
#include "ur/device/ai_control_wire.hpp"
#include "ur/device/ai_endpoint_invite_wire.hpp"
#include "ur/device/client_tool.hpp"
#include "ur/device/content_wire.hpp"
#include "ur/device/content_upload.hpp"
#include "ur/device/context_snapshot.hpp"
#include "ur/device/envelope.hpp"
#include "ur/device/focus_wire.hpp"
#include "ur/device/ipc_wire.hpp"
#include "ur/device/live_wire.hpp"
#include "ur/device/participant_roster.hpp"
#include "ur/device/playback_wire.hpp"
#include "ur/device/record_playback_wire.hpp"
#include "ur/device/privacy_property.hpp"
#include "ur/device/udp_channel.hpp"
#include "ur/device/voice_wire.hpp"

namespace {
// PropertyPublish 不创建网络或保存正文，只允许正式属性解析器进入原token在途状态。
class PropertyPublish final : public ur::device::PublishPort {
public:
    bool publish(std::string_view, std::string_view) override { return true; }
};
// FuzzCipher 不做密码验收，只让任意有效包能进入解码后的生命周期。
class FuzzCipher final : public ur::device::AesCtrPort {
public:
    bool transform(const std::array<std::uint8_t, 16>&, const std::uint8_t* in, std::uint8_t* out,
                   std::size_t size) override {
        std::copy_n(in, size, out);
        return true;
    }
};
// UploadClock只提供固定软件时间，不签发会话授权或启动真实设备。
class UploadClock final : public ur::device::ContentUploadRuntime {
public:
    std::uint64_t monotonic_ms() const override { return 500; }
    std::uint64_t unix_ms() const override { return 1700000000000ULL; }
    bool current() const override { return true; }
};
// UploadReply将任意原字节送入正式上传响应解析，绝不连接URL或读取身份凭据。
class UploadReply final : public ur::device::HttpPort {
public:
    explicit UploadReply(std::string_view body) : body_(body) {}
    ur::device::HttpResponse perform(const ur::device::HttpRequest&,
                                    ur::device::HttpBodySource*, const std::atomic_bool&) override {
        return {ur::device::HttpResult::Ok, 200, body_};
    }
private:
    std::string body_;  // 本轮有界模糊字节副本，退出即释放，不留存日志。
};
// UploadBytes只覆盖响应驱动的读取边界；不接设备文件、摄像头或网络原件。
class UploadBytes final : public ur::device::HttpBodySource {
public:
    bool read(std::uint8_t* out, std::size_t capacity, std::size_t& count) override {
        count = !used_ && capacity ? 1 : 0;
        if (count)
            *out = 'a';
        used_ = true;
        return true;
    }
private:
    bool used_ = false;  // 合成单字节来源只能消费一次。
};
}  // namespace
// LLVMFuzzerTestOneInput 使用相同生产入口解析字节，不记录原始内容。
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    ur::device::ActionRequest action;
    // 邀请为固定显式夹具，只验证不可信轨控制解析，不开放网络或真实媒体权限。
    ur::device::IPCInvite ipc;
    ipc.envelope.session = "fuzz-ipc";
    ipc.audio_track = "mic-1";
    ipc.speaker_track = "speaker-1";
    ur::device::IPCTrackCommand track;
    ur::device::decode_ipc_track(ur::device::WireRoute::Signaling,
                                 std::string_view(reinterpret_cast<const char*>(data), size), ipc,
                                 track);
    ur::device::PlaybackCommand playback;
    // 历史三类严格解析共用不可信原字节，不读取可信清单或执行真实读取／媒体。
    const std::string_view record_body(reinterpret_cast<const char*>(data), size);
    // AI短断网只验证不可信公开绑定及完整快照；绝不创建socket、模型或采集许可。
    ur::device::AIControlBinding ai_binding;
    ur::device::decode_ai_control_created_binding(record_body, ai_binding);
    ur::device::AIControlSnapshot ai_snapshot;
    ur::device::VoiceMedia ai_media;
    ai_media.transport = ur::device::VoiceTransport::Udp;
    ai_media.input_track = "mic-1";
    ai_media.output_track = "speaker-1";
    ur::device::decode_ai_control_snapshot(ur::device::WireRoute::Signaling, record_body, ai_media,
                                           ai_snapshot);
    ai_media.transport = ur::device::VoiceTransport::None;
    ur::device::decode_ai_control_snapshot(ur::device::WireRoute::Signaling, record_body, ai_media,
                                           ai_snapshot);
    // 候选接续仅进入纯邀请解析；固定软件能力不能创建held媒体、接听或迁移模型。
    ur::device::VoiceCapability invite_capability;
    invite_capability.audio = {16000, 1, 20};
    invite_capability.modes = {true, true, true};
    invite_capability.ai_endpoint_handoff = true; // 仅使纯parser到达严格邀请字段，不执行候选生命周期。
    ur::device::AIEndpointInvite ai_invite;
    ur::device::decode_ai_endpoint_invite(ur::device::WireRoute::Signaling, record_body,
                                          invite_capability, true, ai_invite);
    ur::device::decode_ai_endpoint_invite(ur::device::WireRoute::Message, record_body,
                                          invite_capability, true, ai_invite);
    // 属性回执也经同一严格JSON入口；原token夹具不赋予摄像头或会话权限。
    PropertyPublish property_publish;
    ur::device::PrivacyProperty privacy(property_publish, {true, "product", "device"});
    privacy.begin_connection(1);
    privacy.submit({"fuzz-privacy", 1700000000000ULL, true}, 2);
    privacy.receive("$thing/down/property/product/device", record_body, 3);
    // 新附件解析同样接不可信code/data/期限/类型；synthetic端口不会执行任何HTTP副作用。
    UploadClock upload_clock;
    UploadReply upload_reply(record_body);
    UploadBytes upload_bytes;
    ur::device::ContentUploadRequest upload_request;
    upload_request.api_origin = "https://platform.invalid";
    upload_request.authorization = "Basic c3ludGhldGljOnByaXZhdGU=";
    upload_request.session_id = "fuzz-upload";
    upload_request.request_id = "fuzz-01";
    upload_request.name = "sample.txt";
    upload_request.mime_type = "text/plain";
    upload_request.sha256.assign(64, 'a');
    upload_request.size = 1;
    std::atomic_bool upload_cancel{false};  // 固定解析夹具，不模拟真实取消或模型授权。
    ur::device::ContentUploader uploader(upload_reply, upload_clock);
    uploader.upload(upload_request, upload_bytes, upload_cancel);
    // 私有文本仅走生产严格解析，不留存、打印或授予实际通话权限。
    ur::device::ContextSnapshot context_snapshot;
    ur::device::ContextSnapshotAck context_ack;
    ur::device::decode_context_snapshot(ur::device::WireRoute::Message, record_body,
                                        context_snapshot);
    ur::device::decode_context_snapshot_ack(ur::device::WireRoute::Message, record_body,
                                            context_ack);
    // 共享名册只投影不可信完整条目；重复或畸形输入不创建会议、网络或媒体权限。
    ur::device::ParticipantRoster roster;
    roster.apply(record_body);
    roster.apply(record_body);
    roster.replace(record_body);  // 全量纯入口也须独立检查UTF-8、重复成员、逻辑预算与原子替换。
    ur::device::IPCCapability sync_capability;  // 固定解析夹具，不调用网络或创建真实许可。
    sync_capability.microphone = sync_capability.conference_talk = true;
    sync_capability.participant_roster = sync_capability.participant_roster_sync = true;
    ipc.endpoint_epoch = ipc.capability_revision = 1;
    ur::device::IPCRosterSnapshot snapshot;
    ur::device::decode_ipc_roster_snapshot(ur::device::WireRoute::Signaling, record_body, ipc,
                                           sync_capability, snapshot);
    // 客户端工具仅解析不可信独立Execute，不创建scope、显示驱动或执行副作用。
    ur::device::ClientToolCommand client_tool;
    ur::device::decode_client_tool_execute(ur::device::WireRoute::Message, record_body,
                                           client_tool);
    // 同字节在错误通道也必须拒绝，不能将信令或展示事件解释成执行授权。
    ur::device::decode_client_tool_execute(ur::device::WireRoute::Signaling, record_body,
                                           client_tool);
    // 持续节目严格解析与已有安全边界一起验证，不打开源、采集或输出驱动。
    ur::device::LiveInvite live_invite;
    ur::device::LiveTrackControl live_control;
    ur::device::LiveTrackState live_state;
    ur::device::decode_live_invite(ur::device::WireRoute::Signaling, record_body, live_invite);
    ur::device::decode_live_track_control(ur::device::WireRoute::Signaling, record_body,
                                          live_invite, live_control);
    ur::device::decode_live_track_state(ur::device::WireRoute::Signaling, record_body, live_state);
    ur::device::RecordPlaybackInvite record_invite;
    ur::device::RecordPlaybackState record_state;
    ur::device::decode_record_playback_invite(ur::device::WireRoute::Signaling, record_body,
                                              record_invite);
    ur::device::decode_record_playback_control(ur::device::WireRoute::Signaling, record_body,
                                               playback);
    ur::device::decode_record_playback_state(ur::device::WireRoute::Signaling, record_body,
                                             record_state);
    ur::device::FocusCommand focus;      // 不可信焦点请求不得直接执行资源驱动。
    ur::device::FocusState focus_state;  // 任意应答仍须严格校验完整组及原请求关联。
    ur::device::decode_focus_control(ur::device::WireRoute::Signaling,
                                     std::string_view(reinterpret_cast<const char*>(data), size),
                                     focus);
    ur::device::decode_focus_state(ur::device::WireRoute::Signaling,
                                   std::string_view(reinterpret_cast<const char*>(data), size),
                                   focus_state);
    ur::device::decode_playback_control(ur::device::WireRoute::Signaling,
                                        std::string_view(reinterpret_cast<const char*>(data), size),
                                        playback);
    ur::device::decode_action_request(std::string_view(reinterpret_cast<const char*>(data), size),
                                      action);
    ur::device::Envelope envelope;
    if (ur::device::decode_envelope(std::string_view(reinterpret_cast<const char*>(data), size),
                                    envelope) == ur::device::DecodeResult::Ok) {
        std::string encoded;
        ur::device::encode_envelope(envelope, encoded);
    }
    FuzzCipher cipher;
    ur::device::ContentEvent content;
    ur::device::decode_content_event(ur::device::WireRoute::Message,
                                     std::string_view(reinterpret_cast<const char*>(data), size),
                                     content);
    ur::device::VoiceEvent event;
    ur::device::decode_voice_event(ur::device::WireRoute::Signaling,
                                   std::string_view(reinterpret_cast<const char*>(data), size),
                                   event);
    // 安装回执单独解析原三字段200；模糊字节不能创建回复、授予焦点或伪造HAL成功。
    ur::device::UDPReplyStartReceipt receipt;
    ur::device::decode_reply_start_receipt(ur::device::WireRoute::Signaling, record_body, receipt);
    ur::device::decode_reply_start_receipt(ur::device::WireRoute::Message, record_body, receipt);
    ur::device::UdpChannel channel(cipher, {});
    std::vector<std::uint8_t> decoded;
    channel.decode(data, size, decoded);
    channel.decode(data, size, decoded);
    ur::device::UdpChannel signaling_channel(cipher, {}, 128);
    signaling_channel.decode(data, size, decoded);
    return 0;
}
