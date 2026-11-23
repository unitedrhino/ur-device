"""执行生产AI媒体泵，验证突发收包、起播抗抖动、短回复和取消边界。"""
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def method(source, signature):
    """提取完整生产方法，不维护另一份播放状态机。"""
    start = source.index(signature)
    return source[start:source.index('\n}\n', start) + 3]


class SignalingPlaybackJitterTest(unittest.TestCase):
    """网络与扬声器只提供受控时钟，排队及门禁使用实际C++方法。"""

    def test_bursts_do_not_starve_playback_or_lose_frames(self):
        source = (ROOT / 'main/signaling/ur_signaling_webrtc_media.cc').read_text()
        constants = source.split('namespace {', 1)[1].split('}  // namespace', 1)[0]
        methods = '\n'.join(method(source, signature) for signature in [
            'bool WebRtcVoiceMedia::start_playback(', 'void WebRtcVoiceMedia::stop_playback(',
            'void WebRtcVoiceMedia::PumpPlayback()', 'bool WebRtcVoiceMedia::poll(',
        ])
        code = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <deque>
#include <string_view>
#include <vector>
int64_t clock_us=0;
int64_t esp_timer_get_time() { return clock_us; }
template<class... T> void log_all(T...) {}
#define ESP_LOGI(...) log_all(__VA_ARGS__)
#define ESP_LOGW(...) log_all(__VA_ARGS__)
CONSTANTS
struct PeerConnection { std::deque<int> packets; int calls=0; };
int peer_connection_loop(PeerConnection*);
struct Pcm {
    bool active=false,blocked=false;uint64_t epoch=0;
    std::deque<int> hardware;std::vector<int> submitted;
    bool begin_playback(std::string_view,uint64_t value) {
        if (active || !value) return false;
        active=true;epoch=value;return true;
    }
    void end_playback(uint64_t value) { if (value==epoch) { active=false;hardware.clear(); } }
    bool playback_backpressured(uint64_t) const { return blocked; }
    bool write_frame(const std::vector<int16_t>& samples,uint64_t value) {
        assert(active && value==epoch && !blocked);
        hardware.push_back(samples.front());submitted.push_back(samples.front());return true;
    }
    bool playback_empty(uint64_t) const { return hardware.empty(); }
};
struct Codec {
    int decoded=0;
    bool decode(const std::vector<uint8_t>& encoded,std::vector<int16_t>& samples) {
        ++decoded;samples.assign(320,encoded.front());return true;
    }
};
struct WebRtcVoiceMedia {
    enum class PeerState { Connected,Connecting };
    std::atomic<PeerState> state_{PeerState::Connected};
    Pcm pcm_;Codec codec_;PeerConnection peer;PeerConnection* pc_=&peer;
    bool playback_running_=false,playback_prefilled_=false;
    int64_t playback_first_frame_us_=-1;
    uint64_t playback_epoch_=0,drained_epoch_value_=0;unsigned playback_frames_=0;
    std::atomic<int> downlink_frames_{0};
    std::deque<std::vector<uint8_t>> downlink_;
    void PumpCapture() {} void PumpVideo() {}
    bool start_playback(std::string_view,uint64_t);void stop_playback(uint64_t);
    void PumpPlayback();bool poll(uint64_t);
};
WebRtcVoiceMedia* loop_owner=nullptr;
int peer_connection_loop(PeerConnection* peer) {
    ++peer->calls;
    if (!peer->packets.empty()) {
        const auto value=peer->packets.front();peer->packets.pop_front();
        loop_owner->downlink_.push_back({uint8_t(value)});++loop_owner->downlink_frames_;
    }
    return 0;
}
METHODS
int main() {
    // 首次突发空窗不能直接变成扬声器断流，30帧必须全部按序播放。
    {
        WebRtcVoiceMedia media;assert(media.start_playback("reply",1));
        int delivered=0,played=0,gaps=0;int64_t next_output=-1;
        for (int ms=0;ms<=1000;++ms) {
            clock_us=int64_t(ms)*1000;
            while (delivered<30) {
                const int arrival=delivered==0?0:delivered<=4?90:delivered*20;
                if (ms<arrival) break;
                media.downlink_.push_back({uint8_t(delivered++)});
            }
            if (ms%10==0) media.PumpPlayback();
            if (next_output<0 && !media.pcm_.hardware.empty()) next_output=clock_us;
            if (next_output>=0 && clock_us>=next_output && played<30) {
                if (media.pcm_.hardware.empty()) { ++gaps;next_output=clock_us+1000; }
                else {
                    assert(media.pcm_.hardware.front()==played);
                    media.pcm_.hardware.pop_front();++played;next_output=clock_us+20000;
                }
            }
        }
        assert(played==30 && gaps==0);
    }
    // 一帧短回复在100ms内放行，不能为攒够5帧永久静音。
    {
        clock_us=0;WebRtcVoiceMedia media;assert(media.start_playback("short",1));
        media.downlink_.push_back({1});media.PumpPlayback();assert(media.pcm_.submitted.empty());
        clock_us=100000;media.PumpPlayback();assert(media.pcm_.submitted.size()==1);
    }
    // 等待期间取消清掉旧帧，旧代次停止不得取消新回复。
    {
        clock_us=0;WebRtcVoiceMedia media;assert(media.start_playback("cancel",1));
        media.downlink_.push_back({1});media.PumpPlayback();media.stop_playback(1);
        assert(media.downlink_.empty());assert(media.start_playback("new",2));
        media.stop_playback(1);assert(media.playback_running_);
    }
    // 背压时不弹出或解码，恢复容量后仍保留原帧序列。
    {
        clock_us=0;WebRtcVoiceMedia media;assert(media.start_playback("busy",1));
        for(int i=0;i<5;++i) media.downlink_.push_back({uint8_t(i)});
        media.pcm_.blocked=true;media.PumpPlayback();
        assert(media.downlink_.size()==5 && media.codec_.decoded==0);
        media.pcm_.blocked=false;media.PumpPlayback();assert(media.pcm_.submitted.size()==4);
    }
    // 已连接才合并收包，空轮即停，单轮8包封顶，不加速ICE检查。
    {
        WebRtcVoiceMedia media;loop_owner=&media;media.peer.packets={1,2,3};media.poll(0);
        assert(media.peer.packets.empty() && media.peer.calls==4);
        media.peer.calls=0;media.peer.packets={1,2,3,4,5,6,7,8,9};media.poll(0);
        assert(media.peer.calls==8 && media.peer.packets.size()==1);
        media.peer.calls=0;media.state_.store(WebRtcVoiceMedia::PeerState::Connecting);media.poll(0);
        assert(media.peer.calls==1);
    }
}
'''.replace('CONSTANTS', constants).replace('METHODS', methods)
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / 'playback-jitter')
            built = subprocess.run(['g++', '-std=c++17', '-fsanitize=undefined',
                '-x', 'c++', '-', '-o', binary], input=code, text=True, capture_output=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            result = subprocess.run([binary], cwd=directory, text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
