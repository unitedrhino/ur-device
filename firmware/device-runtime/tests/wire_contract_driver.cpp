// 端云交叉契约工具：只输出公开测试请求或校验结果，不连接网络或读取凭据。
#include <cJSON.h>
#include <iostream>
#include <string>
#include "ur/device/ai_control_wire.hpp"
#include "ur/device/ai_endpoint_invite_wire.hpp"
#include "ur/device/content_wire.hpp"
#include "ur/device/endpoint_wire.hpp"
#include "ur/device/focus_wire.hpp"
#include "ur/device/ipc_wire.hpp"
#include "ur/device/playback_wire.hpp"
#include "ur/device/record_playback_wire.hpp"
#include "ur/device/recording_wire.hpp"
#include "ur/device/shared_device_connection.hpp"
#include "ur/device/voice_wire.hpp"

// ContractIdleDriver只提供交叉契约的显式合成idle观测，不执行资源或冒充生产物理探针。
struct ContractIdleDriver final : ur::device::FocusDriver {
    std::uint64_t revision() const override { return 0; }
    std::optional<ur::device::FocusIdleObservation> initial_idle_observation() const override {
        return ur::device::FocusIdleObservation{{"microphone", "speaker"}, 0};
    }
    bool begin(const ur::device::FocusCommand&, std::uint64_t) override { return false; }
    std::optional<ur::device::FocusFact> take_fact(std::string_view, std::string_view) override {
        return {};
    }
    bool owns(const ur::device::FocusCommand&) const override { return false; }
    bool stop_scope(std::string_view, std::string_view, const std::vector<std::string>&) override {
        return false;
    }
};

// main --emit 输出真实 C++ 编码器产生的请求；--check 从 stdin 读取后端生成的响应。
int main(int argc, char** argv) {
    using namespace ur::device;
    if (argc != 2)
        return 2;
    const std::string mode = argv[1];
    if (mode == "--check-ai-endpoint-invite") {
        // 本模式仅供Go公开合成夹具核字段投影；不读取凭据、连接设备或接听邀请。
        std::string body;
        if (!std::getline(std::cin, body) || body.size() > 65536)
            return 1;
        VoiceCapability capability;
        capability.audio = {16000, 1, 20};
        capability.modes = {true, true, true};
        capability.ai_endpoint_handoff = true; // 仅公开互解夹具显式具备本地接听能力，不升级旧设备。
        AIEndpointInvite invite;
        if (decode_ai_endpoint_invite(WireRoute::Signaling, body, capability, true, invite) !=
            WireResult::Ok)
            return 1;
        std::cout << invite.source_session << ' ' << invite.handoff_id << ' '
                  << invite.envelope.session << ' ' << invite.media.input_track << ' '
                  << invite.media.output_track << '\n';
        return 0;
    }
    if (mode == "--emit-ai-control-sync") {
        // 输出原能力和原四字段同步；None无媒体代次，不把公开端代次当物理socket身份。
        for (const auto transport :
             {VoiceTransport::Udp, VoiceTransport::WebRTC, VoiceTransport::None}) {
            VoiceCapability capability;
            capability.transport = transport;
            if (transport == VoiceTransport::WebRTC)
                capability.audio = {48000, 1, 20};  // 原WebRTC适配仅声明真实48k格式。
            capability.session_sync = true;
            capability.text_input = transport == VoiceTransport::None;
            for (unsigned index = 0; index < 2; ++index) {
                Envelope command;
                const auto result =
                    index == 0 ? capability_command("control-cap", capability, command)
                               : ai_session_sync_command(
                                     "control-sync", "original-ai", 9007199254740991ULL, 0, {1, 1},
                                     transport == VoiceTransport::None ? 0 : 1, transport, command);
                std::string body;
                if (result != WireResult::Ok || encode_envelope(command, body) != DecodeResult::Ok)
                    return 1;
                std::cout << body << '\n';
            }
        }
        return 0;
    }
    if (mode == "--check-ai-control-snapshot-udp" || mode == "--check-ai-control-snapshot-webrtc" ||
        mode == "--check-ai-control-snapshot-none") {
        // 选择固定公开原轨，严格核Go实际全量快照；不安装媒体、采集意图或模型授权。
        VoiceMedia selected;
        selected.transport = mode == "--check-ai-control-snapshot-none"     ? VoiceTransport::None
                             : mode == "--check-ai-control-snapshot-webrtc" ? VoiceTransport::WebRTC
                                                                            : VoiceTransport::Udp;
        selected.input_track = "mic-1";
        selected.output_track = "speaker-1";
        std::string body;
        while (std::getline(std::cin, body)) {
            AIControlSnapshot snapshot;
            const bool accepted = body.size() <= 65536 &&
                                  decode_ai_control_snapshot(WireRoute::Signaling, body, selected,
                                                             snapshot) == WireResult::Ok;
            std::cout << (accepted ? "ok" : "rejected") << '\n';
        }
        return std::cin.bad() ? 1 : 0;
    }
    if (mode == "--check-ai-control-created") {
        // 成对新字段及旧端省略均由原创建解码器核验，不使用测试另造的宽松数字解析。
        std::string body;
        while (std::getline(std::cin, body)) {
            VoiceEvent event;
            const bool accepted =
                body.size() <= 65536 &&
                decode_voice_event(WireRoute::Signaling, body, event) == WireResult::Ok;
            std::cout << (accepted ? "ok" : "rejected") << '\n';
        }
        return std::cin.bad() ? 1 : 0;
    }
    if (mode == "--emit-human-passive") {
        // 原纯人工编码器声明语音与被动文字输出，不添加AI文字输入、AEC或交互模式。
        VoiceCapability capability;
        capability.audio = {48000, 1, 20};
        capability.text_input = false;
        ContractIdleDriver driver;  // 显式合成闲置仅用于跨语言契约，不冒充实际设备驱动。
        Envelope command;
        std::string bytes;
        if (handoff_capability_command("human-passive-cap", capability, driver, command, true) !=
                WireResult::Ok ||
            encode_envelope(command, bytes) != DecodeResult::Ok)
            return 1;
        std::cout << bytes << '\n';
        return 0;
    }
    if (mode == "--emit-human-endpoint-handoff") {
        // 显式互解夹具分别输出纯人与人/AI共存SDK新能力；原旧mode及物理资源事实保持不变。
        for (const bool human_only : {true, false}) {
            VoiceCapability capability;
            capability.audio = {48000, 1, 20};
            capability.modes = {true, true, true};
            capability.text_input = !human_only;
            ContractIdleDriver driver; // 只核实际编码器，不把合成baseline当物理闲置或已取得资源。
            Envelope command;
            std::string bytes;
            if (handoff_capability_command(human_only ? "human-endpoint-cap" : "shared-endpoint-cap",
                                           capability, driver, command, human_only, true) != WireResult::Ok ||
                encode_envelope(command, bytes) != DecodeResult::Ok)
                return 1;
            std::cout << bytes << '\n';
        }
        return 0;
    }
    if (mode == "--emit-ai-human-shared") {
        // 单一快照与原AI创建必须共同48k；编解码通过不等于真实闲置或人工已接听。
        VoiceCapability capability;
        capability.audio = {48000, 1, 20};
        capability.modes = {true, true, true};
        capability.text_input = true;
        ContractIdleDriver driver;
        Envelope command;
        std::string bytes;
        if (handoff_capability_command("shared-cap", capability, driver, command) !=
                WireResult::Ok ||
            encode_envelope(command, bytes) != DecodeResult::Ok)
            return 1;
        std::cout << bytes << '\n';
        if (create_voice_command("shared-create", capability, command) != WireResult::Ok ||
            encode_envelope(command, bytes) != DecodeResult::Ok)
            return 1;
        std::cout << bytes << '\n';
        return 0;
    }
    if (mode == "--emit-text-only") {
        // 五个原编码请求构成无媒体最小契约，公开夹具身份不能用于真实接入。
        VoiceCapability capability;
        capability.transport = VoiceTransport::None;
        capability.text_input = true;
        for (unsigned index = 0; index < 5; ++index) {
            Envelope command;
            WireResult status = WireResult::Invalid;
            if (index == 0)
                status = capability_command("text-cap", capability, command);
            if (index == 1)
                status = create_voice_command("text-create", capability, command);
            if (index == 2)
                status = text_input_command("text-input", "text-session", "input-1", "公开合成文字",
                                            false, command);
            if (index == 3)
                status = voice_control("text-cancel", "text-session", "respCancel", "reply-1", 0,
                                       command, 0, VoiceTransport::None);
            if (index == 4)
                status = voice_control("text-end", "text-session", "sessionEnd", "userStopped", 0,
                                       command, 0, VoiceTransport::None);
            std::string body;
            if (status != WireResult::Ok || encode_envelope(command, body) != DecodeResult::Ok)
                return 1;
            std::cout << body << '\n';
        }
        return 0;
    }
    if (mode == "--check-text-only") {
        // 路由只按公开信封确定；内容与输入ACK复用原message解码，不输出正文。
        std::string body;
        while (std::getline(std::cin, body)) {
            Envelope envelope;
            VoiceEvent voice;
            ContentEvent content;
            bool accepted = false;
            if (decode_envelope(body, envelope) == DecodeResult::Ok) {
                if (envelope.message_sequence)
                    accepted =
                        decode_content_event(WireRoute::Message, body, content) == WireResult::Ok;
                else {
                    auto* data = cJSON_Parse(envelope.payload.c_str());
                    const auto* request = cJSON_GetObjectItemCaseSensitive(data, "requestMethod");
                    const auto route = envelope.method == "requestAck" && cJSON_IsString(request) &&
                                               std::string_view(request->valuestring) == "inputSend"
                                           ? WireRoute::Message
                                           : WireRoute::Signaling;
                    cJSON_Delete(data);
                    accepted = decode_voice_event(route, body, voice) == WireResult::Ok;
                }
            }
            std::cout << (accepted ? "ok" : "rejected") << '\n';
        }
        return std::cin.bad() ? 1 : 0;
    }
    if (mode == "--check-ipc-snapshot") {
        // 固定公开夹具只核Go实际序列化与C++严格解析互解，不执行媒体或推导真实授权。
        IPCCapability capability;
        capability.revision = 3;
        capability.microphone = capability.conference_talk = true;
        capability.participant_roster = capability.participant_roster_sync = true;
        IPCInvite invite;
        invite.envelope.session = "device-leg";
        invite.audio_track = "mic-1";
        invite.speaker_track = "speaker-1";
        invite.endpoint_epoch = 1;
        invite.capability_revision = 3;
        std::string body;
        while (std::getline(std::cin, body)) {
            IPCRosterSnapshot snapshot;
            const bool accepted =
                body.size() <= 65536 &&
                decode_ipc_roster_snapshot(WireRoute::Signaling, body, invite, capability,
                                           snapshot) == WireResult::Ok;
            std::cout << (accepted ? "ok" : "rejected") << '\n';
        }
        return std::cin.bad() ? 1 : 0;
    }
    if (mode == "--check-record-invite" || mode == "--check-record-control" ||
        mode == "--check-record-state") {
        // 历史契约仅输出结论，不读取目录、文件或授权，更不记录原文和档案引用。
        std::string body;
        while (std::getline(std::cin, body)) {
            RecordPlaybackInvite invite;
            PlaybackCommand command;
            RecordPlaybackState state;
            WireResult result = WireResult::Invalid;
            if (mode == "--check-record-invite")
                result = decode_record_playback_invite(WireRoute::Signaling, body, invite);
            else if (mode == "--check-record-control")
                result = decode_record_playback_control(WireRoute::Signaling, body, command);
            else
                result = decode_record_playback_state(WireRoute::Signaling, body, state);
            std::cout << (result == WireResult::Ok ? "ok" : "rejected") << '\n';
        }
        return std::cin.bad() ? 1 : 0;
    }
    if (mode == "--emit-focus-capability" || mode == "--emit-focus-baseline") {
        // 三类宿主显式测试装配，不改变正式运行时能力或宣称驱动已经接通。
        for (unsigned index = 0; index < 3; ++index) {
            Envelope source, command;
            std::vector<std::string> group = {"speaker", "display"};
            WireResult result = WireResult::Invalid;
            if (index == 0) {
                PlaybackCapability capability;
                capability.audio_output = capability.video_output = true;
                result = playback_capability_command("file-focus-cap", capability, source);
            } else if (index == 1) {
                VoiceCapability capability;
                capability.file_audio_output = capability.file_video_output = true;
                result = capability_command("voice-focus-cap", capability, source);
            } else {
                result = ipc_capability_command("ipc-focus-cap", IPCCapability{}, source);
                group = {"camera:front"};
            }
            std::string body;
            if (result != WireResult::Ok ||
                add_focus_capability(source, group, command) != WireResult::Ok ||
                (mode == "--emit-focus-baseline" &&
                 add_focus_baseline(command, group,
                                    index == 0 ? 0 : (index == 1 ? 17 : 9007199254740991ULL),
                                    command) != WireResult::Ok) ||
                encode_envelope(command, body) != DecodeResult::Ok)
                return 1;
            std::cout << body << '\n';
        }
        return 0;
    }
    if (mode == "--check-focus-control" || mode == "--check-focus-state" ||
        mode == "--check-focus-reply") {
        std::string body;  // 原文仅进解析器，stdout不暴露内容/URL/身份。
        while (std::getline(std::cin, body)) {
            FocusCommand command;
            FocusState state;
            bool accepted = false;
            if (mode == "--check-focus-control")
                accepted =
                    decode_focus_control(WireRoute::Signaling, body, command) == WireResult::Ok;
            else if (mode == "--check-focus-state")
                accepted = decode_focus_state(WireRoute::Signaling, body, state) == WireResult::Ok;
            else {
                std::string reply;  // 成对读取原请求和原实际应答，缺少第二行明确失败。
                if (!std::getline(std::cin, reply))
                    return 1;
                accepted =
                    decode_focus_control(WireRoute::Signaling, body, command) == WireResult::Ok &&
                    decode_focus_state(WireRoute::Signaling, reply, state) == WireResult::Ok &&
                    focus_reply_matches(command, state);
            }
            std::cout << (accepted ? "ok" : "rejected") << '\n';
        }
        return std::cin.bad() ? 1 : 0;
    }
    if (mode == "--check-recording-state") {
        // 云录制事实与历史播放分别验证；只输出独立解析结论，不读取私有产物或输出原文。
        std::string body;
        while (std::getline(std::cin, body)) {
            RecordingStateFact state;
            const bool accepted = body.size() <= 65536 &&
                decode_recording_state(WireRoute::Signaling, body, state) == WireResult::Ok;
            std::cout << (accepted ? "ok" : "rejected") << '\n';
        }
        return std::cin.bad() ? 1 : 0;
    }
    if (mode == "--check-endpoint-control" || mode == "--check-endpoint-state") {
        std::string body;  // 原文仅进解析器，stdout不暴露会话、事务或目标端身份。
        while (std::getline(std::cin, body)) {
            EndpointCommand command;
            EndpointState state;
            bool accepted = false;
            if (mode == "--check-endpoint-control")
                accepted =
                    decode_endpoint_control(WireRoute::Signaling, body, command) == WireResult::Ok;
            else
                accepted =
                    decode_endpoint_state(WireRoute::Signaling, body, state) == WireResult::Ok;
            std::cout << (accepted ? "ok" : "rejected") << '\n';
        }
        return std::cin.bad() ? 1 : 0;
    }
    if (mode == "--emit-voice-playback") {
        // 输出完整交叉矩阵，每项两行原编码请求；不伪造平台交集或媒体事实。
        for (bool text : {false, true}) {
            for (const auto transport : {VoiceTransport::Udp, VoiceTransport::WebRTC}) {
                for (unsigned mask = 0; mask < 4; ++mask) {
                    VoiceCapability capability;
                    capability.text_input = text;
                    capability.transport = transport;
                    capability.modes = {true, true, true};
                    if (transport == VoiceTransport::WebRTC)
                        capability.audio = {48000, 1, 20};
                    capability.file_audio_output = (mask & 1) != 0;
                    capability.file_video_output = (mask & 2) != 0;
                    Envelope command;
                    std::string body;
                    if (capability_command("cap", capability, command) != WireResult::Ok ||
                        encode_envelope(command, body) != DecodeResult::Ok)
                        return 1;
                    std::cout << body << '\n';
                    if (create_voice_command("create", capability, command) != WireResult::Ok ||
                        encode_envelope(command, body) != DecodeResult::Ok)
                        return 1;
                    std::cout << body << '\n';
                }
            }
        }
        return 0;
    }
    if (mode == "--emit-playback") {
        PlaybackCapability capability;  // 仅显式测试装配，默认设备能力不变。
        capability.audio_output = capability.video_output = true;
        Envelope command;
        std::string body;
        if (playback_capability_command("file-cap", capability, command) != WireResult::Ok ||
            encode_envelope(command, body) != DecodeResult::Ok)
            return 1;
        std::cout << body << '\n';
        return 0;
    }
    if (mode == "--check-playback-invite") {
        std::string body;  // 邀请解析不读取授权源，不执行播放器。
        while (std::getline(std::cin, body)) {
            PlaybackInvite invite;
            const auto status = decode_playback_invite(WireRoute::Signaling, body, invite);
            std::cout << (status == WireResult::Ok ? "ok" : "rejected") << '\n';
        }
        return std::cin.bad() ? 1 : 0;
    }
    if (mode == "--check-playback") {
        std::string body;  // 只输出解析结论，不输出源引用或设备身份。
        while (std::getline(std::cin, body)) {
            PlaybackCommand command;
            const auto status = decode_playback_control(WireRoute::Signaling, body, command);
            std::cout << (status == WireResult::Ok ? "ok" : "rejected") << '\n';
        }
        return std::cin.bad() ? 1 : 0;
    }
    if (mode == "--emit-ipc-talk" || mode == "--check-ipc-talk") {
        IPCCapability capability;
        capability.microphone = capability.conference_talk = true;
        if (mode == "--emit-ipc-talk") {
            Envelope command;
            std::string body;
            if (ipc_capability_command("talk-cap", capability, command) != WireResult::Ok ||
                encode_envelope(command, body) != DecodeResult::Ok)
                return 1;
            std::cout << body << '\n';
            return 0;
        }
        std::string body;
        while (std::getline(std::cin, body)) {
            IPCTalkInvite invitation;
            const auto status =
                decode_ipc_talk_invite(WireRoute::Signaling, body, capability, invitation);
            std::cout << (status == WireResult::Ok ? "ok" : "rejected") << '\n';
        }
        return std::cin.bad() ? 1 : 0;
    }
    if (mode == "--emit-ipc") {
        Envelope command;
        std::string body;
        if (ipc_capability_command("ipc-cap", IPCCapability{}, command) != WireResult::Ok ||
            encode_envelope(command, body) != DecodeResult::Ok)
            return 1;
        std::cout << body << '\n';
        return 0;
    }
    if (mode == "--check-ipc") {
        std::string body;
        while (std::getline(std::cin, body)) {
            IPCInvite invitation;
            const auto status =
                decode_ipc_invite(WireRoute::Signaling, body, IPCCapability{}, invitation);
            std::cout << (status == WireResult::Ok ? "ok" : "rejected") << '\n';
        }
        return std::cin.bad() ? 1 : 0;
    }
    if (mode == "--emit") {
        VoiceCapability capability;
        capability.modes = {true, true, true};
        Envelope request;
        std::string body;
        if (capability_command("cap", capability, request) != WireResult::Ok ||
            encode_envelope(request, body) != DecodeResult::Ok)
            return 1;
        std::cout << body << '\n';
        if (create_voice_command("create", capability, request) != WireResult::Ok ||
            encode_envelope(request, body) != DecodeResult::Ok)
            return 1;
        std::cout << body << '\n';
        for (const auto* method : {"audioStart", "audioStop", "respCancel"}) {
            if (voice_control(method, "s", method, "object-1", 0, request,
                              std::string(method) == "audioStart" ? 128U : 0U) != WireResult::Ok ||
                encode_envelope(request, body) != DecodeResult::Ok)
                return 1;
            std::cout << body << '\n';
        }
        return 0;
    }
    if (mode == "--check") {
        std::string body;
        while (std::getline(std::cin, body)) {
            VoiceEvent result;
            const auto status = decode_voice_event(WireRoute::Signaling, body, result);
            std::cout << (status == WireResult::Ok ? "ok" : "rejected") << '\n';
        }
        return std::cin.bad() ? 1 : 0;
    }
    return 2;
}
