// ESP-IDF编译/链接探针：只验证共享协议代码可链接，没有I2S、Wi-Fi、摄像头或实际固件功能。
#include <cstdlib>
#include "ur/device/buffered_pcm.hpp"
#include "ur/device/udp_voice_media.hpp"
#include "ur/device/capture_gate.hpp"
#include "ur/device/doorbell_event.hpp"
#include "ur/device/live_client.hpp"
#include "ur/device/playback_engine.hpp"
#include "ur/device/playback_wire.hpp"
#include "ur/device/record_playback_client.hpp"
#include "ur/device/record_playback_wire.hpp"
#include "ur/device/recording_wire.hpp"
#include "ur/device/voice_runtime.hpp"
#include "ur/device/voice_wire.hpp"

namespace {
// CompileMqttPort 只链接无媒体运行时的平台接口，拒绝网络且不伪造已连接、订阅或发布事实。
class CompileMqttPort final : public ur::device::MqttPort {
public:
    // connect 拒绝所有探针连接；不读取认证字段或访问Wi-Fi/TCP。
    bool connect(const ur::device::MqttConfig&) override { return false; }
    // subscribe 没有连接，不生成订阅MID或成功应答。
    int subscribe(std::string_view) override { return 0; }
    // publish 不发送信令、正文或任何媒体，不把探针当成真实网络适配器。
    bool publish(std::string_view, std::string_view) override { return false; }
    // poll 没有网络资源，不能生成可推进会话的事实。
    bool poll() override { return false; }
    // take_event 不注入Connected/Established或其它合成事件。
    std::optional<ur::device::MqttEvent> take_event() override { return std::nullopt; }
    // publications_drained 没有有效连接，不能伪造网络确认。
    bool publications_drained() override { return false; }
    // disconnect 无资源，只验证运行时关闭入口可链接。
    void disconnect() override {}
};

// CompilePublisher 拒绝实际发布，仅使门铃事件共享核心参与编译和链接，不提供MQTT驱动。
class CompilePublisher final : public ur::device::PublishPort {
public:
    // publish 拒绝所有本地探针报文，不建立连接或向平台发送事件。
    bool publish(std::string_view, std::string_view) override { return false; }
};

// CompilePlayer 只用于链接可移植驱动接口；不启动文件、输出或任何实体硬件。
class CompilePlayer final : public ur::device::PlaybackDriver {
public:
    // begin 拒绝实际工作，探针不能被当成已支持播放器的设备驱动。
    bool begin(const ur::device::PlaybackCommand&, std::uint64_t) override { return false; }
    // take_fact 没有真实媒体，不能伪造成功事实。
    std::optional<ur::device::PlaybackFact> take_fact() override { return std::nullopt; }
    // output_permitted 探针无speaker/display许可。
    bool output_permitted() const override { return false; }
    // stop_all 没有输出资源，只验证关闭分支可链接。
    bool stop_all() override { return true; }
};

// CompileRecordPort 链接历史档案的独立驱动契约；无档案、Peer或硬件，不制造实际状态。
class CompileRecordPort final : public ur::device::RecordPlaybackPort {
public:
    // prepare 明确不支持档案，编译探针不能授予档案读取许可。
    ur::device::RecordPreparation prepare(const ur::device::RecordPlaybackInvite&,
                                          std::uint64_t) override {
        return ur::device::RecordPreparation::Unsupported;
    }
    // begin 拒绝媒体副作用，不生成loaded或播放事实。
    bool begin(const ur::device::PlaybackCommand&, std::uint64_t, std::uint64_t) override {
        return false;
    }
    // take_fact 没有驱动资源，不回报实际状态。
    std::optional<ur::device::RecordPlaybackFact> take_fact() override { return std::nullopt; }
    // begin_peer 不在编译探针建立WebRTC资源。
    bool begin_peer(std::uint64_t) override { return false; }
    // apply_answer 探针没有Peer，不能接纳任何SDP。
    bool apply_answer(std::uint64_t, std::string_view) override { return false; }
    // connected 无真实ICE/DTLS，始终未连通。
    bool connected(std::uint64_t) const override { return false; }
    // close_peer 无资源，只确保关闭入口参与编译。
    void close_peer(std::uint64_t) override {}
    // peer_stopped 没有Peer或发布回调，全部已退出。
    bool peer_stopped(std::uint64_t) const override { return true; }
    // restore_intent 不恢复任何播放意图。
    bool restore_intent(std::uint64_t, std::string_view, std::uint64_t) override { return false; }
    // publishing_permitted 没有档案授权或媒体许可。
    bool publishing_permitted(std::uint64_t) const override { return false; }
    // stop_all 无资源，检查幂等释放契约可链接。
    bool stop_all() override { return true; }
};
// CompileLivePort 只链接独立接收核心；没有真实节目、解码器或硬件，不生成媒体成功事实。
class CompileLivePort final : public ur::device::LiveMediaPort {
public:
    // prepare 明确不支持真实接收，编译通过不代表Opus/H264驱动存在。
    ur::device::LivePreparation prepare(const ur::device::LiveInvite&, std::uint64_t) override {
        return ur::device::LivePreparation::Unsupported;
    }
    // begin_peer 不分配媒体连接。
    bool begin_peer() override { return false; }
    // apply_answer 不接纳任何真实SDP。
    bool apply_answer(std::string_view) override { return false; }
    // connected 没有真实ICE/DTLS，始终未连通。
    bool connected() const override { return false; }
    // authorization_valid 没有节目许可，不能因编译探针授信。
    bool authorization_valid() const override { return false; }
    // begin_track 不形成实际控制副作用。
    bool begin_track(const ur::device::LiveTrackControl&, std::uint64_t) override { return false; }
    // take_fact 没有媒体资源，不能假报active/paused/stopped。
    std::optional<ur::device::LiveMediaFact> take_fact() override { return std::nullopt; }
    // presentation_permitted 没有输出许可。
    bool presentation_permitted(std::string_view) const override { return false; }
    // stop_all 无资源，只验证关闭契约可以链接。
    bool stop_all() override { return true; }
};
}  // namespace

// 除组件内逐翻译单元守护外，应用入口也验证实际标准，防止只改目标属性却被SDK覆盖。
static_assert(__cplusplus == 201703L, "ESP编译探针必须实际使用C++17");

// app_main 为IDF链接入口；构建任务不烧录/运行这个程序。
extern "C" void app_main() {
    // 云端录制仅核可移植构造/解析符号真实链接，不创建取流、录制文件或同意授权。
    ur::device::RecordingControlCommand recording_control;
    recording_control.operation = "stop";
    recording_control.expected_revision = 1;
    recording_control.recording_id = "compile-recording";
    ur::device::Envelope recording_request;
    ur::device::RecordingStateFact recording_state;
    if (ur::device::recording_control_command(recording_control, "compile-recording-stop",
                                              "compile-session", recording_request) !=
            ur::device::WireResult::Ok ||
        ur::device::decode_recording_state(ur::device::WireRoute::Signaling,
                                           R"({"msgToken":"compile-recording-stop","method":"recordingState","sessionId":"compile-session","code":200,"data":{"recordingId":"compile-recording","revision":1,"state":"finalizing"}})",
                                           recording_state) != ur::device::WireResult::Ok)
        std::abort();
    ur::device::BufferedPcm compile_pcm(
        {16000, 1, 20});  // 无实际I2S/硬件Ready，编译探针不得虚报安装支持。
    if (compile_pcm.reply_install_ready())
        std::abort();
    ur::device::VoiceEvent compile_start;  // 仅链接严格合同，不能创建媒体许可或发送回执。
    const auto start_bytes =
        R"({"msgToken":"compile-start","method":"respAudioStart","sessionId":"compile-session","eventSeq":1,"data":{"respId":"compile-reply","trackId":"compile-speaker","mediaRevision":1,"udpNonce":"0000000000000063","firstPacketSeq":128}})";
    ur::device::Envelope compile_receipt;              // 保存原Start关联，不发布到网络。
    std::string receipt_bytes;                         // 严格编码结果，仅用于本地链接探针。
    ur::device::UDPReplyStartReceipt compile_decoded;  // 回读闭集字段，不制造已安装事实。
    if (ur::device::decode_voice_event(ur::device::WireRoute::Signaling, start_bytes,
                                       compile_start) != ur::device::WireResult::Ok ||
        ur::device::reply_start_receipt(compile_start, compile_receipt) !=
            ur::device::WireResult::Ok ||
        ur::device::encode_envelope(compile_receipt, receipt_bytes) !=
            ur::device::DecodeResult::Ok ||
        ur::device::decode_reply_start_receipt(ur::device::WireRoute::Signaling, receipt_bytes,
                                               compile_decoded) != ur::device::WireResult::Ok)
        std::abort();
    CompileMqttPort text_network;  // None只借用网络接口，不创建空音频驱动、Peer或UDP。
    ur::device::VoiceCapability text_capability;
    text_capability.transport = ur::device::VoiceTransport::None;
    text_capability.text_input = true;
    ur::device::VoiceRuntime text_runtime(text_network, "compile-product/compile-device",
                                          text_capability, "compile-text-boot");
    const ur::device::MqttConfig text_config;  // 无真实地址、凭据或硬件配置。
    // 只引用合法失败及释放路径；编译/链接不代表已建立文字会话或完成ESP32驱动验收。
    if (text_runtime.start(text_config, 0) != ur::device::Result::DriverFailed)
        std::abort();
    text_runtime.poll(1);
    if (text_runtime.send_text("compile-text", false, 1) != ur::device::Result::Stale)
        std::abort();
    text_runtime.close(2);
    if (text_runtime.state() != ur::device::RuntimeState::Idle)
        std::abort();
    CompilePublisher publisher;  // 无网络端口，只验证门铃编码、关联和关闭路径可链接。
    ur::device::DoorbellEvent event(publisher, {true, "compile-product", "compile-device", "bell"});
    if (!event.enabled() || !event.begin_connection(0) ||
        event.submit({"compile-press", 1, "{}"}, 0) != ur::device::SubmitResult::Accepted)
        std::abort();
    event.tick(0);
    if (!event.receive(event.reply_topic(),
                       R"({"method":"eventReply","msgToken":"compile-press","code":200})", 1) ||
        event.state("compile-press") != ur::device::RequestState::Succeeded)
        std::abort();
    event.cancel(1);
    event.disconnect(1);
    ur::device::CaptureGate capture(1);  // 有界预约器探针，没有I2S、麦克风、扬声器或相机。
    const auto lease = capture.acquire(true, true, true);
    if (!lease || capture.available(true, true, true) || !capture.release(*lease))
        std::abort();
    CompilePlayer driver;  // 显式引用构造、提交、轮询和析构，不只编译未引用静态库。
    ur::device::PlaybackEngine engine(driver, "compile-child", "p", 1000);
    if (engine
            .submit(
                R"({"msgToken":"compile-load","method":"playbackControl","sessionId":"compile-child","eventSeq":1,"params":{"operation":"load","playbackId":"p","expectedRevision":0,"contentRef":"compile:opaque"}})",
                0)
            .admission != ur::device::PlaybackAdmission::Accepted)
        std::abort();
    engine.poll(0);
    if (!engine.take_reply())
        std::abort();
    ur::device::PlaybackCommand playback;  // 真正链接新增入口，不以未引用静态库冒充编译可用。
    if (ur::device::decode_playback_control(
            ur::device::WireRoute::Signaling,
            R"({"msgToken":"compile-player","method":"playbackControl","sessionId":"player","eventSeq":1,"params":{"operation":"play","playbackId":"p","expectedRevision":0}})",
            playback) != ur::device::WireResult::Ok)
        std::abort();
    ur::device::Envelope message;
    // 历史探针只链接真实解析器，不读取档案或启动媒体；文件默认与历史正代次入口保持分开。
    ur::device::PlaybackCommand record_command;
    ur::device::RecordPlaybackInvite record_invite;
    ur::device::RecordPlaybackState record_state;
    CompileRecordPort record_port;  // 无效绑定探针只能保持终态，不能隐式进入历史会话。
    ur::device::RecordPlaybackClient record_client(publisher, record_port, record_invite,
                                                   "compile-up", "compile-down", 1000);
    record_client.tick(0);
    record_client.close(1);
    if (record_client.phase() != ur::device::RecordPlaybackPhase::Ended)
        std::abort();
    CompileLivePort live_port;  // 实际引用新核心，而不是仅构建未链接的静态对象。
    ur::device::LiveInvite live_invite;
    ur::device::LiveClient live_client(publisher, live_port, live_invite, "compile-up",
                                       "compile-down", 1000);
    live_client.tick(0);
    live_client.close(1);
    if (live_client.phase() != ur::device::LivePhase::Ended ||
        ur::device::decode_live_invite(ur::device::WireRoute::Signaling, "{}", live_invite) ==
            ur::device::WireResult::Ok)
        std::abort();
    if (ur::device::decode_record_playback_invite(ur::device::WireRoute::Signaling, "{}",
                                                  record_invite) == ur::device::WireResult::Ok ||
        ur::device::decode_record_playback_state(ur::device::WireRoute::Signaling, "{}",
                                                 record_state) == ur::device::WireResult::Ok ||
        ur::device::decode_record_playback_control(
            ur::device::WireRoute::Signaling,
            R"({"msgToken":"compile-record","method":"playbackControl","sessionId":"record-player","eventSeq":1,"params":{"operation":"play","playbackId":"p","expectedRevision":0,"mediaRevision":1}})",
            record_command) != ur::device::WireResult::Ok)
        std::abort();
    const ur::device::VoiceCapability capability{{true, false, false}, {16000, 1, 20}, 1};
    if (ur::device::capability_command("compile-probe", capability, message) !=
        ur::device::WireResult::Ok)
        std::abort();
    std::string encoded;
    if (ur::device::encode_envelope(message, encoded) != ur::device::DecodeResult::Ok)
        std::abort();
}
