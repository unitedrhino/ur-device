"""回放真实终态投影和建立超时，销毁通话腿后不得残留活动控制层。"""
from pathlib import Path
import subprocess
import tempfile
import unittest
from .test_call_audio_decode import method

ROOT = Path(__file__).resolve().parents[2]


class CallTerminalUiTest(unittest.TestCase):
    """仅替换核心状态与UI回调边界，保留投影和看门狗的生产流程。"""

    def test_normal_and_forced_end_hide_before_teardown(self):
        source = (ROOT / 'main/signaling/ur_signaling_call.cc').read_text()
        actual = '\n'.join(method(source, signature) for signature in [
            'void UrSignalingCall::Impl::ProjectState(',
            'void UrSignalingCall::Impl::WatchEstablish(',
        ])
        code = r'''
#include <cassert>
#include <cstdint>
#include <vector>
#define ESP_LOGW(...) ((void)0)
constexpr std::uint64_t kEstablishWatchdogMs=45000;
struct Client {int value=1;int state(){return value;}void end(std::uint64_t){value=2;}};
struct UrSignalingCall {
 enum class UiState {Hidden,Active,Ended};
 struct Impl {
  Client storage;Client* client_=&storage;
  UiState ui_state_=UiState::Active;bool hidden_pending_=false,media_active_=false;
  bool offer_sent_=true,connected_reported_=true,mic_paused_=true,camera_paused_=true;
  std::uint64_t leg_started_ms_=100;int torn=0;std::vector<UiState> events;
  bool Busy(){return client_!=nullptr;}
  UiState MapState(int state){return state==1?UiState::Hidden:UiState::Ended;}
  void FireState(UiState state){events.push_back(state);}
  void TeardownLeg(){++torn;client_=nullptr;}
  void ProjectState();void WatchEstablish(std::uint64_t);
 };
};
PRODUCTION
int main(){
 using State=UrSignalingCall::UiState;
 UrSignalingCall::Impl normal;
 normal.ProjectState();normal.ProjectState();normal.ProjectState();
 assert((normal.events==std::vector<State>{State::Ended,State::Hidden}) && "terminal projection never reached Hidden");
 assert(normal.torn==1 && normal.client_==nullptr && !normal.hidden_pending_);
 // 已连通过的长通话关闭媒体后等待正常结束确认，不可重新视为建连超时。
 UrSignalingCall::Impl closing;closing.storage.value=2;closing.connected_reported_=true;
 closing.WatchEstablish(120100);
 assert(closing.torn==0 && closing.events.empty() && "established call ending was mistaken for failed establishment");
 UrSignalingCall::Impl timeout;timeout.connected_reported_=false;
 timeout.WatchEstablish(45099);assert(timeout.torn==0 && timeout.events.empty());
 timeout.media_active_=true;timeout.WatchEstablish(45100);assert(timeout.torn==0);
 timeout.media_active_=false;timeout.WatchEstablish(45100);
 assert(timeout.torn==1 && timeout.client_==nullptr);
 assert(!timeout.events.empty() && timeout.events.back()==State::Hidden && "watchdog destroyed leg without hiding call controls");
 assert(timeout.ui_state_==State::Hidden && !timeout.hidden_pending_);
 timeout.ProjectState();timeout.WatchEstablish(90000);assert(timeout.torn==1);
}
'''.replace('PRODUCTION', actual)
        with tempfile.TemporaryDirectory(prefix='watcher-terminal-ui-') as directory:
            binary = str(Path(directory) / 'terminal')
            built = subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                    '-x', 'c++', '-', '-o', binary], input=code,
                                   text=True, capture_output=True, timeout=30)
            self.assertEqual(built.returncode, 0, built.stderr)
            result = subprocess.run([binary], text=True, capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
