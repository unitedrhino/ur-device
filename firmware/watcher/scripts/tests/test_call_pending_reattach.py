"""验证宿主解绑时保留等待邀请终态，同设备重连仍防重投。"""
from pathlib import Path
import subprocess
import tempfile
import unittest
from .test_call_audio_decode import method

ROOT = Path(__file__).resolve().parents[2]


class PendingReattachTest(unittest.TestCase):
    def test_detach_retains_same_identity_only(self):
        source = (ROOT / 'main/signaling/ur_signaling_call.cc').read_text()
        functions = '\n'.join(method(source, signature) for signature in (
            'void UrSignalingCall::Impl::Attach(',
            'void UrSignalingCall::Impl::Detach(',
            'void UrSignalingCall::Impl::RetirePending(',
        ))
        code = r'''
#include "ur_signaling_pending_tombstones.h"
#include <cassert>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#define CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO 0
#define pdTRUE 1
#define ESP_LOGW(...) ((void)0)
namespace ur { namespace device {
struct UrSignalingMqttPort {};
enum class DecodeResult {Ok};
struct Envelope {std::string token,session,payload;};
DecodeResult decode_envelope(std::string_view,Envelope& e){e.token="token";e.session="session";e.payload="payload";return DecodeResult::Ok;}
} }
std::uint64_t MonotonicMs(){return 1;}
int xQueueReceive(void*,void*,int){return 0;}
struct Client {void end(std::uint64_t){}};
struct UrSignalingCall {
 enum class UiState {Hidden};
 struct Impl {
  struct QueuedUiCommand {int command;std::uint32_t generation;};
  std::uint32_t ui_generation_=0;
  struct PendingInvite {std::string token,session,body;std::uint64_t expires=0;};
  ur::device::UrSignalingMqttPort* port_=nullptr;std::string identity_,instance_;
  std::string pending_tombstones_identity_;
  PendingCallTombstones pending_tombstones_;
  std::optional<PendingInvite> pending_invite_;
  std::unique_ptr<Client> client_,media_;
  void *ui_queue_=nullptr,*pcm_=nullptr,*codec_=nullptr;
  std::function<bool()> host_busy_,host_yield_;
  bool pending_accept_=false,hidden_pending_=false;UiState ui_state_=UiState::Hidden;
  int rejects=0;
  std::optional<int> pending_dial_;std::atomic<bool> dial_queued_{false};
  std::atomic<bool> dial_ready_{false};
  bool EnsureUiQueue(){return true;}
  void StopMedia(){} void TeardownLeg(){} void FireState(UiState){}
  void RejectInvite(const ur::device::Envelope&,const char*){++rejects;}
  void Attach(ur::device::UrSignalingMqttPort*,const std::string&);
  void Detach();
  void RetirePending(PendingCallTombstones::Decision);
 };
};
// PRODUCTION
int main(){
 using Decision=PendingCallTombstones::Decision;
 ur::device::UrSignalingMqttPort port;UrSignalingCall::Impl app;
 app.Attach(&port,"device-a");app.pending_invite_=UrSignalingCall::Impl::PendingInvite{"token","session","body",1000};
 app.pending_accept_=true;app.Detach();
 assert(app.rejects==1 && !app.pending_invite_ && !app.pending_accept_);
 app.Attach(&port,"device-a");
 assert(app.pending_tombstones_.Find("token","session","payload")==Decision::Busy);
 app.Detach();app.Attach(&port,"device-b");
 assert(app.pending_tombstones_.Find("token","session","payload")==Decision::None);
}
'''.replace('// PRODUCTION', functions)
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / 'reattach')
            subprocess.run(['c++', '-x', 'c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-I', str(ROOT / 'main/signaling'), '-', '-o', binary],
                           input=code, text=True, check=True)
            subprocess.run([binary], check=True)


if __name__ == '__main__':
    unittest.main()
