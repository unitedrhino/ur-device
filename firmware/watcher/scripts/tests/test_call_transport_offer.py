"""回放真实协商入口：UDP固定格式offer不得被WebRTC视频m-line守卫误拒。"""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

class CallTransportOfferTest(unittest.TestCase):
    """分别覆盖UDP、合法WebRTC语音/视频及缺视频的WebRTC视频，保留原SDP防护。"""
    def test_actual_negotiation_dispatches_selected_transport(self):
        source = (ROOT / 'main/signaling/ur_signaling_call.cc').read_text()
        actual = source.split('void UrSignalingCall::Impl::DriveNegotiation(std::uint64_t now) {', 1)[1].split('void UrSignalingCall::Impl::WatchIceDrop', 1)[0]
        harness = r'''
#include <cassert>
#include <cstdint>
#include <optional>
#include <string>
#define CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO 1
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
namespace ur::device { enum class HumanCallState { Negotiating, ConnectingMedia }; }
struct Client {
 bool ended=false, offered=false;
 ur::device::HumanCallState state(){return ur::device::HumanCallState::Negotiating;}
 void end(std::uint64_t){ended=true;}
 bool offer_ready(const std::string&,std::uint64_t){offered=true;return true;}
 bool media_connected(std::uint64_t){return true;}
};
struct Media {
 bool udp; std::string offer;
 std::optional<std::string> TakeOffer(){return offer;}
 bool IsUDP(){return udp;}
 bool connected(){return false;}
};
struct UrSignalingCall { struct Impl {
 Client* client_; Media* media_;
 bool offer_sent_=false,connected_reported_=false,media_active_=false,voice_call_=false;
 void StartMedia(){}
 void DriveNegotiation(std::uint64_t);
};};
void UrSignalingCall::Impl::DriveNegotiation(std::uint64_t now) {
''' + actual + r'''
int main(){
 for(int scenario=0;scenario<4;scenario++){
  Client client;Media media{scenario==0,scenario==3?"v=0\r\nm=audio 9 RTP/AVP 111\r\n":scenario==1?"v=0\r\nm=video 9 RTP/AVP 96\r\n":"{\"mediaRevision\":1,\"udpAv\":{}}"};
  UrSignalingCall::Impl owner{&client,&media};owner.voice_call_=scenario==3;owner.DriveNegotiation(1);
  assert(client.offered==(scenario!=2));assert(client.ended==(scenario==2));
 }
}
'''
        compiler = shutil.which('g++') or shutil.which('clang++')
        self.assertIsNotNone(compiler)
        output = ROOT.parents[1] / '.temp'
        output.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=output) as directory:
            binary = Path(directory) / 'offer'
            result = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-x', 'c++', '-', '-o', str(binary)],input=harness,text=True,capture_output=True)
            self.assertEqual(result.returncode,0,result.stderr)
            result = subprocess.run([str(binary)],capture_output=True)
            self.assertEqual(result.returncode,0,result.stderr.decode(errors='replace'))

    def test_actual_terminal_peer_state_clears_connected_cache(self):
        source = (ROOT / 'main/signaling/ur_signaling_call.cc').read_text()
        actual = source.split('    static void OnIceState(PeerConnectionState state, void* userdata) {', 1)[1].split('    /* PumpCapture', 1)[0]
        harness = r''' 
#include <cassert>
#define ESP_LOGI(...) ((void)0)
enum PeerConnectionState {PEER_CONNECTION_NEW, PEER_CONNECTION_CHECKING, PEER_CONNECTION_CONNECTED, PEER_CONNECTION_DISCONNECTED, PEER_CONNECTION_FAILED, PEER_CONNECTION_CLOSED};
enum class PeerState {kIdle,kConnecting,kConnected,kFailed};
struct PeerMedia { PeerState state_=PeerState::kConnected;
static void OnIceState(PeerConnectionState,void*);
};
void PeerMedia::OnIceState(PeerConnectionState state,void* userdata) {
''' + actual + r'''
int main(){
 for(auto state:{PEER_CONNECTION_DISCONNECTED,PEER_CONNECTION_FAILED,PEER_CONNECTION_CLOSED}) {
  PeerMedia media;PeerMedia::OnIceState(state,&media);assert(media.state_!=PeerState::kConnected);
 }
 PeerMedia media;media.state_=PeerState::kConnecting;PeerMedia::OnIceState(PEER_CONNECTION_CONNECTED,&media);assert(media.state_==PeerState::kConnected);
}
'''
        harness = '#include <initializer_list>\n' + harness
        compiler = shutil.which('g++') or shutil.which('clang++')
        output = ROOT.parents[1] / '.temp'
        output.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=output) as directory:
            binary = Path(directory) / 'state'
            result = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-x', 'c++', '-', '-o', str(binary)], input=harness, text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(binary)],capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr.decode(errors='replace'))

    def test_actual_poll_drains_burst_before_codec_work(self):
        """六包接收队列中，编码阻塞不能挤掉已按时到达的保活应答。"""
        source = (ROOT / 'main/signaling/ur_signaling_call.cc').read_text()
        actual = source.split('    void Poll() {',1)[1].split('    /* TakeOffer',1)[0]
        peer_poll = source.split('    void PollPeerOnce() {', 1)[1].split('    // EncodeCall', 1)[0]
        harness = r'''
#include <cassert>
#include <deque>
#include <algorithm>
#include <cstdint>
#include <sys/select.h>
#include "ur_signaling_call_poll.h"
#define CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO 0
enum class PeerState {kConnected,kFailed};
constexpr int PEER_CONNECTION_CONNECTED=2;
int delivered=0,decoded=0;
int clock_ms=0;bool rtp_added=false,stun_added=false,stun_acked=false,continuous=false;
std::deque<bool> mailbox(6,false);
void advance(int amount){
 for(int j=0;j<amount;++j){++clock_ms;
  if(continuous&&clock_ms%10==0&&mailbox.size()<6)mailbox.push_back(clock_ms==50);
  if(clock_ms==2&&!rtp_added){rtp_added=true;if(mailbox.size()<6)mailbox.push_back(false);}
  if(clock_ms==3&&!stun_added){stun_added=true;if(mailbox.size()<6)mailbox.push_back(true);}
 }
}
int64_t esp_timer_get_time(){return int64_t(clock_ms)*1000;}
extern "C" void* xTaskGetCurrentTaskHandle(){return reinterpret_cast<void*>(1);}
extern "C" int __real_select(int,fd_set*,fd_set*,fd_set*,timeval* t){if(!mailbox.empty())return 1;if(t->tv_usec)advance(20);return 0;}
extern "C" int __wrap_select(int,fd_set*,fd_set*,fd_set*,timeval*);
void peer_connection_loop(void*){timeval wait{0,1000};if(__wrap_select(0,nullptr,nullptr,nullptr,&wait)>0){stun_acked=stun_acked||mailbox.front();if(!mailbox.front())++delivered;mailbox.pop_front();advance(1);}}
int peer_connection_get_state(void*){return PEER_CONNECTION_CONNECTED;}
struct PeerMedia {
 bool udp_mode_=false;void* pc_=reinterpret_cast<void*>(1);PeerState state_=PeerState::kConnected;
 int captured=0;
 uint64_t udp_poll_at_us_=0,udp_poll_us_=0,udp_poll_max_us_=0,udp_poll_gap_max_us_=0;unsigned udp_poll_calls_=0;
 void PollUDP(){}
 void TraceAudio(){}
 void PollPeerOnce() {''' + peer_poll + r'''
 void PumpCapture(){++captured;advance(10);}
 void PumpPlayback(int limit=1){decoded+=std::min(limit,delivered-decoded);advance(10);}
 void Poll();
};
void PeerMedia::Poll(){''' + actual + r'''
int main(){
 PeerMedia media;media.Poll();assert(stun_acked);assert(media.captured>=8);
 // 空套接字的一次20ms等待后应立刻转去泵媒体，不能重复等待八次。
 clock_ms=100;mailbox.clear();PeerMedia idle;idle.Poll();assert(clock_ms-100<=210);
 // 编码期间持续到包，下一帧编码前必须继续接收，STUN不能一直等到整批结束。
 clock_ms=0;mailbox.assign(6,false);rtp_added=stun_added=true;stun_acked=false;continuous=true;
 PeerMedia streaming;streaming.Poll();assert(stun_acked);
 // 持续到包时，解码交付预算也必须跟上收包预算，不能只消化每批四包中的一帧。
 delivered=decoded=0;clock_ms=100;mailbox.assign(6,false);
 for(int i=0;i<12;++i)streaming.Poll();
 assert(delivered-decoded<=4);
}
'''
        compiler = shutil.which('g++') or shutil.which('clang++')
        output=ROOT.parents[1]/'.temp';output.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=output) as directory:
            binary=Path(directory)/'burst'
            headers=Path(directory)/'freertos';headers.mkdir()
            (headers/'FreeRTOS.h').write_text('#pragma once\nusing TaskHandle_t=void*;\n')
            (headers/'task.h').write_text('#pragma once\nextern "C" void* xTaskGetCurrentTaskHandle();\n')
            result=subprocess.run([compiler,'-std=c++17','-Wall','-Wextra','-Werror','-I',directory,'-I',str(ROOT/'main/signaling'),'-x','c++','-',str(ROOT/'main/signaling/ur_signaling_call_poll.cc'),'-o',str(binary)],input=harness,text=True,capture_output=True)
            self.assertEqual(result.returncode,0,result.stderr)
            result=subprocess.run([str(binary)],capture_output=True)
            self.assertEqual(result.returncode,0,result.stderr.decode(errors='replace'))

    def test_actual_udp_capture_drains_during_codec_burst(self):
        """真实八帧采集块每帧排网络队列，编码持续期间不能丢下行媒体。"""
        source = (ROOT / 'main/signaling/ur_signaling_call.cc').read_text()
        start = source.index('        if (!owner_.mic_paused_) {', source.index('    void PollUDP()'))
        end = source.index('        if (jpeg_output_', start)
        actual = source[start:end]
        harness = r'''#include <cassert>
#include <cstdint>
#include <memory>
#include <vector>
#define CONFIG_UR_SOFTWARE_AEC 1
#define ESP_LOGI(...) ((void)0)
static int64_t time_us=0;static bool costly=false;
int64_t esp_timer_get_time(){return time_us;}
namespace ur::device {enum class PcmResult{Frame,Empty};}
struct Owner{bool mic_paused_=false;uint64_t audio_capture_at_=0;};
struct Channel{bool audio(const std::vector<uint8_t>&,std::vector<uint8_t>&){return true;}};
struct PeerMedia{
 Owner owner_;std::unique_ptr<Channel> udp_channel_=std::make_unique<Channel>();int audio_up_=0;
 unsigned udp_decrypt_calls_=0;std::vector<int> downlink_;
 int queued=6,dropped=0,received=0,captured=0;
 void ReceiveUDP(int limit){while(limit--&&queued){--queued;++received;++udp_decrypt_calls_;}}
 ur::device::PcmResult ReadCapture(std::vector<int16_t>&){++captured;return ur::device::PcmResult::Frame;}
 bool EncodeCall(const std::vector<int16_t>&,std::vector<uint8_t>&){if(queued==6)++dropped;else++queued;return true;}
 bool SendCallUDP(const std::vector<uint8_t>&){return true;}
 bool QueueCallCapture(){++captured;if(queued==6)++dropped;else++queued;return false;}
 void PumpCallEncoded(){++audio_up_;if(costly)time_us+=50000;}
 void PumpPlayback(int){}
 void Capture(){const auto started=esp_timer_get_time();std::vector<int16_t> pcm;std::vector<uint8_t> audio,packet;
''' + actual + r'''}
};
int main(){PeerMedia media;media.Capture();assert(media.captured==8);assert(media.dropped==0);assert(media.received>=8);costly=true;PeerMedia busy;busy.Capture();assert(busy.captured==2 && "count-only outer loop ignored elapsed budget");}
'''
        output=ROOT.parents[1]/'.temp';output.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=output) as directory:
            binary=Path(directory)/'udp-fairness'
            result=subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-x','c++','-','-o',str(binary)],input=harness,text=True,capture_output=True)
            self.assertEqual(result.returncode,0,result.stderr)
            result=subprocess.run([str(binary)],capture_output=True)
            self.assertEqual(result.returncode,0,result.stderr.decode(errors='replace'))

if __name__ == '__main__':
    unittest.main()
