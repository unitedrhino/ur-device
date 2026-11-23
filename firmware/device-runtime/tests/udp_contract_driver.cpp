// Go/C++ 真实媒体互操作驱动：身份参数只经 stdin，使用原生 UDP、AES、Opus，不连接外部模型。
#include <chrono>
#include <iostream>
#include <thread>
#include "openssl_ctr.hpp"
#include "opus_codec.hpp"
#include "udp_socket.hpp"
#include "ur/device/udp_voice_media.hpp"

namespace ur::device {
// ContractPcm 生成可重复非静音帧并检查实际解码数据；不冒充麦克风或硬件 AEC。
class ContractPcm final : public DrainablePcmPort {
public:
    explicit ContractPcm(std::size_t samples) : samples_(samples) {}
    bool begin_capture(std::string_view) override {
        capture_ = true;
        return true;
    }
    void end_capture() override { capture_ = false; }
    PcmResult read_frame(std::vector<std::int16_t>& out) override {
        if (!capture_ || sent_ == 2)
            return PcmResult::Empty;
        ++sent_;
        out.resize(samples_);
        for (std::size_t i = 0; i < out.size(); ++i)
            out[i] = ((i / 16) % 2 == 0) ? 5000 : -5000;
        return PcmResult::Frame;
    }
    bool begin_playback(std::string_view, std::uint64_t epoch) override {
        epoch_ = epoch;
        return true;
    }
    bool write_frame(const std::vector<std::int16_t>& frame, std::uint64_t epoch) override {
        if (epoch != epoch_ || frame.size() != samples_)
            return false;
        ++decoded_;  // 测试即时消费并检查真实 PCM；不称为扬声器播放。
        return true;
    }
    void end_playback(std::uint64_t epoch) override {
        if (epoch == epoch_)
            epoch_ = 0;
    }
    bool playback_empty(std::uint64_t epoch) const override { return epoch == epoch_; }
    unsigned decoded() const { return decoded_; }

private:
    std::size_t samples_;      // 协商后单帧采样数量。
    unsigned sent_ = 0;        // 两帧持续上行的次数。
    unsigned decoded_ = 0;     // 真实 Opus 解码帧数。
    std::uint64_t epoch_ = 0;  // 当前播放资源。
    bool capture_ = false;     // 输入独立于输出状态。
};
// read_event 接收主测试提供的真实公开下行信封，错误不输出报文或凭据。
bool read_event(VoiceEvent& event) {
    std::string body;
    return static_cast<bool>(std::getline(std::cin, body)) &&
           decode_voice_event(WireRoute::Signaling, body, event) == WireResult::Ok;
}
// run 使用真实单调时钟推进网络；上层 Go 测试另有进程级10秒截止与准确清理。
int run() {
    VoiceEvent created;
    if (!read_event(created) || created.envelope.method != "sessionCreated")
        return 2;
    const auto format = created.media.audio;
    ContractPcm pcm(static_cast<std::size_t>(format.sample_rate * format.frame_ms / 1000));
    OpusCodec codec;
    UdpSocket socket;
    OpenSslCtr cipher;
    UdpVoiceMedia media(pcm, codec, socket, cipher);
    if (!media.configure(created.media))
        return 3;
    const auto begin = std::chrono::steady_clock::now();  // 本进程共用单调时基。
    const auto now = [&]() -> std::uint64_t {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                              std::chrono::steady_clock::now() - begin)
                                              .count());
    };
    while (!media.ready() && now() < 2000) {
        if (!media.poll(now()))
            return 4;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!media.ready())
        return 5;
    std::cout << "ready " << media.next_input_sequence() << std::endl;
    VoiceEvent start;
    if (!read_event(start) || start.envelope.method != "respAudioStart" ||
        !media.prepare_reply(start.reply_nonce, start.first_packet_sequence) ||
        !media.start_playback(start.response_id, 1) || !media.start_capture("input-contract"))
        return 6;
    while (pcm.decoded() < 2 && now() < 4000) {
        if (!media.poll(now()))
            return 7;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    media.stop_capture();
    if (pcm.decoded() != 2 || !media.timing().first_network_ms || !media.timing().first_playable_ms)
        return 8;
    std::cout << "decoded 2 non-silent" << std::endl;
    VoiceEvent done;
    if (!read_event(done) || done.envelope.method != "respAudioDone" ||
        !media.finish_reply(done.last_packet_sequence) || !media.poll(now()) ||
        media.drained_epoch() != 1 || media.missing_tail())
        return 9;
    media.stop_playback(1);
    media.close();
    std::cout << "closed" << std::endl;
    return 0;
}
}  // namespace ur::device
int main() { return ur::device::run(); }
