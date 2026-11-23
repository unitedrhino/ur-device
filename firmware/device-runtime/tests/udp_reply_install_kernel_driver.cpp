// 文件说明：安装确认组件驱动使用真实VoiceClient、UDP、AES与Opus，发布严格回执；软件PCM不代表扬声器、MQTT或模型验收。
#include "openssl_ctr.hpp"
#include "opus_codec.hpp"
#include "udp_socket.hpp"
#include "ur/device/udp_voice_media.hpp"
#include "ur/device/voice_client.hpp"
#include <chrono>
#include <cerrno>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>

namespace ur::device {
namespace {
// monotonic_ms读取真实调用时钟；不以媒体泵上一次poll参数代替raw-read或安装时刻。
std::uint64_t monotonic_ms() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
// RawSocket仅给当前实际UdpSocket附只读计数；不更改内核接收缓冲、不回送数据或解析nonce。
class RawSocket final : public DatagramPort {
public:
    bool tracking = false; // 探测完成后才把用户态回复读取计入本例。
    bool opened = false; // 原实际open/close调用状态，不代替独立进程退出证明。
    std::uint64_t reads = 0, first_ms = 0, last_ms = 0; // 实际成功用户态读取次数/首末时钟。
    bool open(std::string_view address, std::uint16_t port) override {
        opened = socket_.open(address, port);
        return opened;
    }
    bool set_peer(std::string_view address, std::uint16_t port) override {
        return socket_.set_peer(address, port);
    }
    std::uint16_t local_port() const override { return socket_.local_port(); }
    bool send(const std::vector<std::uint8_t>& bytes) override { return socket_.send(bytes); }
    ReceiveResult receive(std::vector<std::uint8_t>& bytes) override {
        const auto result = socket_.receive(bytes);
        if (tracking && result == ReceiveResult::Received) {
            const auto instant = monotonic_ms();
            if (reads == 0) first_ms = instant;
            last_ms = instant;
            ++reads;
        }
        return result;
    }
    void close() override { socket_.close(); opened = false; }
private:
    UdpSocket socket_; // 本实例唯一真实连接端口，生命周期覆盖上层媒体。
};
// ActualPcm生成原固定软件方波并立即消费真实解码PCM；非静音逐帧计算，不拿写入数量代替能量。
class ActualPcm final : public DrainablePcmPort {
public:
    explicit ActualPcm(std::size_t samples) : samples_(samples), explicit_ready_(samples == 320) {}
    // reply_install_ready仅证明本软件16kHz/20ms同步消费可用，不声明实体HAL或AEC。
    bool reply_install_ready() const override { return explicit_ready_; }
    std::uint64_t decoded = 0, voiced = 0, consumed = 0, plays = 0; // 实际解码接受/非静音/消费/启动次数。
    bool begin_capture(std::string_view) override { capturing_ = true; return true; }
    void end_capture() override { capturing_ = false; }
    PcmResult read_frame(std::vector<std::int16_t>& out) override {
        if (!capturing_ || sent_ >= 2) return PcmResult::Empty;
        ++sent_;
        out.resize(samples_);
        for (std::size_t i = 0; i < out.size(); ++i)
            out[i] = ((i / 16) % 2 == 0) ? 5000 : -5000;
        return PcmResult::Frame;
    }
    bool begin_playback(std::string_view, std::uint64_t epoch) override {
        if (epoch == 0 || epoch_ != 0) return false;
        epoch_ = epoch; ++plays; return true;
    }
    bool write_frame(const std::vector<std::int16_t>& frame, std::uint64_t epoch) override {
        if (epoch != epoch_ || frame.size() != samples_) return false;
        ++decoded;
        if (non_silent_pcm(frame)) ++voiced;
        // 本组件真实读取全部PCM完成能量检查后即时消费；不是浏览器或扬声器输出验收。
        consumed += frame.size();
        return true;
    }
    void end_playback(std::uint64_t epoch) override { if (epoch == epoch_) epoch_ = 0; }
    bool playback_empty(std::uint64_t epoch) const override { return epoch != 0 && epoch == epoch_; }
private:
    std::size_t samples_; // 原协商单帧样本数。
    bool explicit_ready_; // 只有原软件格式匹配才支持安装。
    unsigned sent_ = 0; // 仅两帧固定真实Opus上行供Go端复用，不运行外部音源。
    std::uint64_t epoch_ = 0; // 当前唯一播放代次。
    bool capturing_ = false; // 上行意图独立于开始回复安装。
};
// Lines在真实poll期间非阻塞读取有限stdin原信封；半行不阻塞媒体、不读取无界正文。
class Lines {
public:
    bool prepare() {
        const auto flags = fcntl(STDIN_FILENO, F_GETFL, 0);
        return flags >= 0 && fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) == 0;
    }
    bool poll() {
        char chunk[2048]; // 有界本地控制读取，不是媒体缓存。
        const auto count = ::read(STDIN_FILENO, chunk, sizeof(chunk));
        if (count < 0) return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
        if (count == 0) return false;
        if (bytes_.size() + static_cast<std::size_t>(count) > 65536) return false;
        bytes_.append(chunk, static_cast<std::size_t>(count));
        return true;
    }
    bool take(std::string& out) {
        const auto newline = bytes_.find('\n');
        if (newline == std::string::npos) return false;
        out = bytes_.substr(0, newline);
        bytes_.erase(0, newline + 1);
        return true;
    }
private:
    std::string bytes_; // 最多64KiB的组件stdin队列，禁止身份参数进入stdout。
};
// PrivatePublisher仅将原客户端发布字节送给父进程专属FD3，普通stdout不记录身份或nonce。
class PrivatePublisher final : public PublishPort {
public:
    // prepare设置非阻塞pipe；满时原客户端保留原字节重试，不阻塞媒体poll。
    bool prepare() {
        const auto flags = fcntl(3, F_GETFL, 0);
        return flags >= 0 && fcntl(3, F_SETFL, flags | O_NONBLOCK) == 0;
    }
    // publish限定一条不超过PIPE_BUF的原子写，不另造安装200或媒体状态。
    bool publish(std::string_view, std::string_view body) override {
        if (body.empty() || body.size() >= 4096 || body.find('\n') != std::string_view::npos)
            return false;
        std::string bytes(body);
        bytes.push_back('\n');
        return ::write(3, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size());
    }
};
// run保留原2秒探测/4秒组件及Go10秒预算；安装、取消、关闭只沿真实VoiceClient。
int run() {
    std::string body;
    VoiceEvent bootstrap;
    // 初行只提供同一原UDP配置与音频格式，不直接构造已建立Session或安装回复。
    if (!std::getline(std::cin, body) ||
        decode_voice_event(WireRoute::Signaling, body, bootstrap) != WireResult::Ok ||
        bootstrap.envelope.method != "sessionCreated" ||
        bootstrap.media.audio.sample_rate != 16000 || bootstrap.media.audio.channels != 1 ||
        bootstrap.media.audio.frame_ms != 20) return 2;
    ActualPcm pcm(320);
    OpusCodec codec;
    RawSocket socket;
    OpenSslCtr cipher;
    UdpVoiceMedia media(pcm, codec, socket, cipher);
    if (!media.enable_presignal_diagnostics() || !media.udp_reply_start_receipt_ready()) return 3;
    PrivatePublisher publisher;
    Lines lines;
    if (!publisher.prepare() || !lines.prepare()) return 7;
    VoiceCapability capability;
    capability.modes = {true, false, false}; // 软件输入不是实际AEC，仅声明受控halfDuplex。
    capability.audio = {16000, 1, 20};
    capability.udp_reply_start_receipt = true;
    VoiceClient client(publisher, media, "$thing/up/signaling/p/d",
                       "$thing/down/signaling/p/d", capability, "00112233445566778899aabbccddeeff");
    const auto boot = monotonic_ms();
    if (client.connect(boot) != Result::Applied) return 17;
    bool ready = false, input = false, reported = false, ended = false, closed = false;
    bool media_ready_sent = false; // 原请求发送一次后等待平台ACK，不重复宣告实际Ready。
    while (monotonic_ms() - boot < 4000) {
        if (!lines.poll()) return 8;
        if (lines.take(body)) {
            const auto now = monotonic_ms();
            if (body == R"({"localAction":"snapshot"})") {
                // 仅采样原持续poll后的状态，不安装、不取消、不授予任何发送资格。
                std::cout << "{\"phase\":\"snapshot\",\"rawReceived\":" << socket.reads
                          << ",\"decodedFrames\":" << pcm.decoded
                          << ",\"playing\":" << (client.playing() ? "true" : "false") << "}" << std::endl;
            } else if (body == R"({"localAction":"cancel"})") {
                // 仅该受控组件stdin动作触发原本地用户取消，不增加设备协议method。
                if (client.cancel_response(now) != Result::Applied) return 18;
                std::cout << "{\"phase\":\"cancelled\",\"decodedFrames\":" << pcm.decoded
                          << ",\"playing\":" << (client.playing() ? "true" : "false") << "}" << std::endl;
            } else if (body == R"({"localAction":"close"})") {
                client.disconnected(now);
                closed = true;
            } else {
                VoiceEvent event;
                if (decode_voice_event(WireRoute::Signaling, body, event) != WireResult::Ok) return 9;
                const auto before = monotonic_ms();
                const auto result = client.receive({"$thing/down/signaling/p/d", body}, now);
                if (result != Result::Applied && result != Result::Duplicate && result != Result::Stale)
                    return 19;
                if (event.envelope.method == "respAudioStart" && result == Result::Applied) {
                    const auto after = monotonic_ms();
                    // 仅观察原receive结果；200由实际prepare+Session.begin成功路径自行发布。
                    const auto snapshot = media.presignal_diagnostic();
                    if (!snapshot || !client.playing()) return 11;
                    reported = true;
                    std::cout << "{\"phase\":\"installed\",\"rawReceived\":" << socket.reads
                        << ",\"received\":" << snapshot->received << ",\"evicted\":" << snapshot->evicted
                        << ",\"retained\":" << snapshot->retained << ",\"installBeforeMs\":" << before
                        << ",\"installAfterMs\":" << after << "}" << std::endl;
                } else if (event.envelope.method == "respAudioDone" && result == Result::Applied) {
                    ended = true;
                }
            }
        }
        const auto now = monotonic_ms();
        if (client.media_negotiating() || client.state() == VoiceState::Established) {
            if (!media.poll(now)) return 14;
        }
        if (!media_ready_sent && client.media_negotiating() && media.ready()) {
            if (client.media_ready(now) != Result::Applied) return 20;
            media_ready_sent = true;
        }
        if (!ready && client.state() == VoiceState::Established) {
            ready = true;
            socket.tracking = true;
            std::cout << "{\"phase\":\"ready\",\"explicitSoftwareReady\":true,\"nextInputSequence\":"
                      << media.next_input_sequence() << "}" << std::endl;
            if (client.start_input(now) != Result::Applied) return 6;
            input = true;
        }
        if (!ready && now - boot >= 2000) return 5;
        client.tick(now);
        if (closed) {
            std::cout << "{\"phase\":\"closed\",\"socketOpen\":"
                      << (socket.opened ? "true" : "false") << ",\"decodedFrames\":" << pcm.decoded
                      << ",\"playing\":" << (client.playing() ? "true" : "false") << "}" << std::endl;
            return socket.opened ? 21 : 0;
        }
        if (ended && reported && input && media.drained_epoch() == client.playback_epoch()) {
            const auto epoch = client.playback_epoch();
            if (client.playback_drained(epoch) != Result::Applied) return 22;
            const auto final = media.take_stopped_timing(epoch);
            if (!final) return 15;
            if (client.end(now) != Result::Applied) return 23;
            std::cout << "{\"phase\":\"final\",\"rawReceived\":" << socket.reads
                << ",\"networkPackets\":" << final->network_packets << ",\"decodedFrames\":"
                << final->decoded_frames << ",\"nonSilentFrames\":" << pcm.voiced
                << ",\"consumedSamples\":" << pcm.consumed << ",\"missingPackets\":" << final->missing_packets
                << ",\"observedComplete\":" << (final->observed_complete ? "true" : "false")
                << ",\"socketOpen\":" << (socket.opened ? "true" : "false") << "}" << std::endl;
            return 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return 16;
}
} // namespace
} // namespace ur::device
int main() { return ur::device::run(); }
