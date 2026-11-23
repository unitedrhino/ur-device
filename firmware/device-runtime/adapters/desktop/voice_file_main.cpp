// 无硬件C++设备进程：真实MQTT/UDP收发单段WAV，输出解码音频，不替代桌面麦克风/AEC验收。
#include <openssl/rand.h>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include "action_file_ledger.hpp"
#include "device_credentials.hpp"
#include "environment.hpp"
#include "file_actions.hpp"
#include "file_pcm.hpp"
#include "mosquitto_port.hpp"
#include "openssl_ctr.hpp"
#include "opus_codec.hpp"
#include "udp_socket.hpp"
#include "ur/device/udp_voice_media.hpp"
#include "ur/device/voice_runtime.hpp"

namespace ur::device {
namespace {
// environment 不记录环境变量内容；凭据只进入原生设备进程，不进入UI或结果文件。
std::string environment(const char* name) { return environment_value(name).value_or(""); }
// milliseconds 返回本进程统一的单调时间轴，与Unix签名过期时间严格分开。
std::uint64_t milliseconds() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}
// instance_id 使用系统密码随机源，进程重启不能复用协议幂等标识。
std::string instance_id() {
    std::array<unsigned char, 16> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
        return {};
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (const auto byte : bytes) {
        result += hex[byte >> 4];
        result += hex[byte & 15];
    }
    return result;
}
// configure
// 仅在显式提供设备三元组时签名；不使用仓库示例账号或自动发现环境凭据。
bool configure(MqttConfig& config, std::string& identity, const std::string& instance) {
    const auto product = environment("UR_DEVICE_PRODUCT");
    const auto device = environment("UR_DEVICE_NAME");
    const auto secret = environment("UR_DEVICE_SECRET");
    const auto expires = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count() +
                         3600;
    if (expires <= 0)
        return false;
    const auto auth =
        device_credentials(product, device, secret, instance, static_cast<std::uint64_t>(expires));
    if (!auth)
        return false;
    identity = product + "/" + device;
    config.host = environment("UR_MQTT_HOST");
    config.ca_file = environment("UR_MQTT_CA");
    const auto port = environment("UR_MQTT_PORT");
    unsigned number = 0;
    const auto parsed = std::from_chars(port.data(), port.data() + port.size(), number);
    if (config.host.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != port.data() + port.size() || number == 0 || number > 65535)
        return false;
    config.port = static_cast<std::uint16_t>(number);
    config.client_id = auth->client_id;
    config.username = auth->username;
    config.password = auth->password;
    return true;
}
// FilePaths 通过具名字段区分只读输入和必须新建的输出目录。
struct FilePaths {
    std::filesystem::path source;       // 只读语料。
    std::filesystem::path destination;  // 新输出目录。
    unsigned rounds = 1;                // 同一会话内重复固定语料，1至30轮。
    bool text = false;                  // 显式--text-file输入；不把文字延迟混入语音末帧延迟。
};
// print_delay
// 输出同一时钟的真实差值；缺基点或回复与输入重叠时为null，禁止无符号下溢或伪造零。
void print_delay(const char* key, std::optional<std::uint64_t> point,
                 std::optional<std::uint64_t> baseline) {
    std::cout << ",\"" << key << "\":";
    if (point && baseline && *point >= *baseline)
        std::cout << *point - *baseline;
    else
        std::cout << "null";
}
// run_file 返回0表示本次文件联调全部完成，非0不计入成功样本。
int run_file(const FilePaths& paths) {
    WaveAudio corpus;
    std::string text;  // 只在本地只读文件和请求缓存中保留，不写诊断日志。
    std::ifstream input(paths.source, std::ios::binary);
    if (paths.text && input) {
        text.resize(60001);
        input.read(text.data(), static_cast<std::streamsize>(text.size()));
        text.resize(static_cast<std::size_t>(input.gcount()));
        corpus.sample_rate = 16000;
        Envelope checked;
        if (text_input_command("check", "check", "check", text, true, checked) != WireResult::Ok) {
            std::cerr << "invalid_input_text\n";
            return 2;
        }
    } else if (!input || !read_wave(input, corpus) ||
               (corpus.sample_rate != 16000 && corpus.sample_rate != 24000)) {
        std::cerr << "invalid_input_wave\n";
        return 2;
    }
    MqttConfig config;
    std::string identity;
    const auto instance = instance_id();
    if (instance.empty() || !configure(config, identity, instance)) {
        std::cerr << "invalid_device_environment\n";
        return 2;
    }
    // 只创建调用者明确指定的新目录；从不覆盖既有音频或跟随文件名中的响应ID。
    std::error_code error;
    if (!std::filesystem::create_directory(paths.destination, error) || error) {
        std::cerr << "output_directory_must_be_new\n";
        return 2;
    }
    const AudioFormat format{static_cast<int>(corpus.sample_rate), 1,
                             corpus.sample_rate == 16000 ? 20 : 60};
    FilePcm pcm(format);
    OpusCodec codec;
    UdpSocket udp;
    OpenSslCtr cipher;
    UdpVoiceMedia media(pcm, codec, udp, cipher);
    MosquittoPort mqtt;
    FileActions action_driver(pcm);  // 明确的软件物模型适配，不冒充硬件控制。
    std::unique_ptr<ActionFileLedger> action_ledger;  // 文件IO不进入语音事件线程。
    if (!configured_action_ledger(identity, action_ledger)) {
        std::cerr << "action_ledger_unavailable\n";
        return 2;
    }
    ActionEngine actions(action_driver, 5000, action_ledger.get());
    // 文件驱动没有AEC，不能通过开关虚假声明fullDuplex；两种模式的核心策略由同一runtime实现。
    VoiceRuntime runtime(mqtt, media, identity,
                         VoiceCapability{{true, false, false}, format, 1, paths.text}, instance,
                         &actions);
    const auto began = milliseconds();
    if (runtime.start(config, began) != Result::Applied) {
        std::cerr << "connection_start_failed\n";
        return 1;
    }
    bool started = false;
    bool stopped = false;
    bool had_audio = false;
    bool sent_end = false;
    bool saved = false;
    unsigned finished_rounds = 0;                 // 仅在音频排空及同回复聚合终态到齐时推进。
    std::optional<std::uint64_t> text_submitted;  // 文字请求发布基点，不是模型受理或语音末帧。
    auto round_began = began;                     // 每轮独立超时，不因已完成轮次延长失败轮次等待。
    std::string saved_response;  // WAV与聚合终态必须属于同一回复，不能拿前一回复的文件冒充新回复。
    std::optional<std::uint64_t> first_text;  // 当前轮次首次有正文的回复文本到达，不是识别文本。
    std::string text_response;                // 首文本所归属的回复，必须与本轮保存音频一致。
    std::string
        completed_response;  // 聚合终态与音频排空必须属于同一回复，跨Topic到达顺序不作假设。
    auto previous = VoiceState::Idle;
    bool previous_capture = false;        // 仅记录实际采集状态跃迁，不以请求发送推断开麦成功。
    auto previous_volume = pcm.volume();  // 观察实际软件播放增益，不能把模型回复文本当作动作完成。
    std::cout << "{\"event\":\"softwareVolume\",\"value\":" << previous_volume
              << ",\"elapsedMs\":0}\n";
    int status = 1;
    std::uint64_t previous_actions =
        0;  // 记录实际驱动完成次数，查询同值也可验证执行，不依赖音量变化。
    while (milliseconds() - round_began < 180000) {
        const auto now = milliseconds();
        if (!pcm.poll(now))
            break;
        runtime.poll(now);
        if (action_driver.completed_count() != previous_actions) {
            previous_actions = action_driver.completed_count();
            std::cout << "{\"event\":\"softwareActionCompleted\",\"count\":" << previous_actions
                      << ",\"code\":" << action_driver.last_code() << ",\"volume\":" << pcm.volume()
                      << ",\"elapsedMs\":" << now - began << "}\n";
        }
        if (pcm.volume() != previous_volume) {
            previous_volume = pcm.volume();
            std::cout << "{\"event\":\"softwareVolume\",\"value\":" << previous_volume
                      << ",\"elapsedMs\":" << now - began << "}\n";
        }
        const auto state = runtime.voice().state();
        if (runtime.voice().capturing() != previous_capture) {
            previous_capture = runtime.voice().capturing();
            std::cout << "{\"event\":\"captureState\",\"active\":"
                      << (previous_capture ? "true" : "false") << ",\"elapsedMs\":" << now - began
                      << "}\n";
        }
        if (state != previous) {
            std::cout << "{\"event\":\"voiceState\",\"state\":" << static_cast<int>(state)
                      << ",\"elapsedMs\":" << now - began << "}\n";
            previous = state;
        }
        // 内容只输出事件类型和长度，避免把真实用户对话写入默认调试日志。
        while (auto content = runtime.take_content()) {
            std::cout << "{\"event\":\"content\",\"method\":\"" << content->envelope.method
                      << "\",\"textBytes\":" << content->text.size() << "}\n";
            if (content->envelope.method == "respDone" && content->status == "completed")
                completed_response = content->response_id;
            if (!first_text && !content->text.empty() &&
                (content->envelope.method == "respTextDelta" ||
                 content->envelope.method == "respTextDone")) {
                first_text = now;
                text_response = content->response_id;
            }
        }
        if (runtime.state() == RuntimeState::Failed || state == VoiceState::Failed)
            break;
        // 云端主动终止并已关闭媒体时，本轮明确失败；不能继续空等到三分钟语料截止。
        if (!sent_end && runtime.state() == RuntimeState::Idle)
            break;
        if (!started && state == VoiceState::Established) {
            if (paths.text) {
                if (runtime.send_text(text, true, now) != Result::Applied)
                    break;
                text_submitted = now;
                stopped = true;
            } else if (!pcm.queue_input(corpus) || runtime.start_input(now) != Result::Applied)
                break;
            started = true;
        }
        if (started && !stopped && pcm.input_complete()) {
            // 半双工收到云端回复时核心可能已自动停采；重复停止是已达成意图，不是设备故障。
            const auto result = runtime.stop_input(now);
            if (result != Result::Applied && result != Result::Duplicate)
                break;
            stopped = true;
        }
        had_audio = had_audio || runtime.voice().playing();
        // 本入口是固定语料的一问一答评测；多回复须另做聚合，不能悄悄把两个回复混成一个成功样本。
        if (saved && pcm.response_id() != saved_response)
            break;
        if (had_audio && !runtime.voice().playing() && !saved) {
            const auto filename = paths.rounds == 1
                                      ? std::string("reply.wav")
                                      : "reply-" + std::to_string(finished_rounds + 1) + ".wav";
            std::ofstream output(paths.destination / filename, std::ios::binary);
            if (!output || !write_wave(output, pcm.rendered()))
                break;
            output.close();
            const auto timing = media.timing();  // 固定回复测量快照，分别检查两种首音事实。
            if (!output || media.missing_tail() || timing.missing_packets != 0 ||
                timing.decoded_frames != timing.network_packets || !timing.first_network_ms ||
                !timing.first_playable_ms)
                break;
            saved = true;
            saved_response = pcm.response_id();
            std::cout << "{\"event\":\"virtualPlaybackSaved\",\"samples\":"
                      << pcm.rendered().samples.size() << ",\"round\":" << finished_rounds + 1
                      << ",\"firstNetworkMs\":" << *timing.first_network_ms - began
                      << ",\"firstPlayableMs\":" << *timing.first_playable_ms - began << "}\n";
        }
        if (saved && completed_response == saved_response && stopped && !sent_end) {
            const auto timing = media.timing();
            const auto last_voice = paths.text ? text_submitted : media.last_non_silent_sent_ms();
            std::cout << "{\"event\":\"roundCompleted\",\"round\":" << finished_rounds + 1
                      << ",\"latencyBasis\":\""
                      << (paths.text ? "textSubmitted" : "lastNonSilentSentFrame") << "\"";
            print_delay(paths.text ? "inputSubmittedMs" : "lastNonSilentSentMs", last_voice, began);
            print_delay("firstTextLatencyMs",
                        text_response == saved_response ? first_text : std::nullopt, last_voice);
            print_delay("firstNetworkLatencyMs", timing.first_network_ms, last_voice);
            print_delay("firstPlayableLatencyMs", timing.first_playable_ms, last_voice);
            std::cout << "}\n";
            ++finished_rounds;
            if (finished_rounds < paths.rounds) {
                started = stopped = had_audio = saved = false;
                saved_response.clear();
                completed_response.clear();
                first_text.reset();
                text_response.clear();
                round_began = now;
                continue;
            }
            if (runtime.end(now) != Result::Applied)
                break;
            sent_end = true;
        }
        if (sent_end && runtime.state() == RuntimeState::Idle) {
            status = 0;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    runtime.close(milliseconds());
    // 关闭会清空会话状态，但不能丢失经过关联验证的拒绝；自由文本不进入默认日志。
    if (const auto& rejected = runtime.voice().last_rejection()) {
        std::cout << "{\"event\":\"requestRejected\",\"method\":\"" << rejected->method
                  << "\",\"code\":" << rejected->code << "}\n";
    }
    std::cout << "{\"event\":\"finished\",\"ok\":" << (status == 0 ? "true" : "false") << "}\n";
    return status;
}
}  // namespace
}  // namespace ur::device

// main
// 只接受显式输入与新输出目录，不自动连接测试环境；异常消息不包含路径或凭据。
int main(int argc, char** argv) {
    const bool text = argc > 1 && std::string_view(argv[1]) == "--text-file";
    const int offset = text ? 1 : 0;  // 参数偏移只区分两种显式输入，不猜文件扩展名。
    if (argc != 3 + offset && argc != 4 + offset) {
        std::cerr << "usage: device_voice_file INPUT.wav NEW_OUTPUT_DIRECTORY "
                     "[ROUNDS:1..30]\n"
                     "   or: device_voice_file --text-file INPUT.txt NEW_OUTPUT_DIRECTORY "
                     "[ROUNDS:1..30]\n"
                     "env: UR_DEVICE_PRODUCT UR_DEVICE_NAME UR_DEVICE_SECRET "
                     "UR_MQTT_HOST "
                     "UR_MQTT_PORT UR_MQTT_CA\n";
        return 2;
    }
    unsigned rounds = 1;  // 未传入时保持单轮入口及reply.wav文件名兼容。
    if (argc == 4 + offset) {
        const std::string value(argv[3 + offset]);
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), rounds);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || rounds == 0 ||
            rounds > 30) {
            std::cerr << "invalid_round_count\n";
            return 2;
        }
    }
    try {
        return ur::device::run_file({argv[1 + offset], argv[2 + offset], rounds, text});
    } catch (...) {
        std::cerr << "device_process_failed\n";
        return 1;
    }
}
