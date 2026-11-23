"""回放信令宿主退出路径，避免编码通知或借用PCM在宿主任务销毁后继续运行。"""
from pathlib import Path
import subprocess
import tempfile
import unittest
from .test_call_audio_decode import method

ROOT = Path(__file__).resolve().parents[2]


class CallHostShutdownTest(unittest.TestCase):
    """保留生产退出/析构方法，仅替换FreeRTOS及运行时边界。"""

    def run_code(self, code):
        """编译并执行一个不访问真实设备的退出顺序回放。"""
        with tempfile.TemporaryDirectory(prefix='watcher-host-stop-') as directory:
            binary = str(Path(directory) / 'stop')
            built = subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                    '-x', 'c++', '-', '-o', binary], input=code,
                                   text=True, capture_output=True, timeout=30)
            self.assertEqual(built.returncode, 0, built.stderr)
            result = subprocess.run([binary], text=True, capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_host_exit_joins_human_media_before_pcm_owner(self):
        source = (ROOT / 'main/signaling/ur_signaling_protocol.cc').read_text()
        body = method(source, 'void SignalingProtocol::TaskLoop()')
        marker = '        WaitForNextPoll();\n    }\n'
        tail = body[body.rindex(marker) + len(marker):].rsplit('}', 1)[0]
        code = r'''
#include <atomic>
#include <cassert>
#include <memory>
#define CONFIG_UR_AI_SIGNALING_WEBRTC 1
bool encoder_alive=true,contacts_detached=false;unsigned detached=0;
// 通讯录也借用宿主发送口，先解除其回调再销毁通话与PCM宿主。
struct UrSignalingContacts {static UrSignalingContacts& Instance(){static UrSignalingContacts c;return c;}void Detach(){contacts_detached=true;}};
struct UrSignalingCall {static UrSignalingCall& Instance(){static UrSignalingCall c;return c;}void Detach(){assert(contacts_detached);encoder_alive=false;++detached;}};
struct Runtime {void close(unsigned){assert(!encoder_alive&&"host destroys PCM while human encoder can still notify/read it");}};
struct Audio {void ResumeDecoder(){assert(!encoder_alive);}};
struct Application{Audio a;static Application&GetInstance(){static Application a;return a;}Audio&GetAudioService(){return a;}};
unsigned MonotonicMs(){return 1;}
void xEventGroupSetBits(void*,unsigned){}
constexpr unsigned kSessionFinishedEvent=1;
struct SignalingProtocol {
 bool call_appearance_ready_=true;std::unique_ptr<Runtime>runtime_=std::make_unique<Runtime>();
 std::atomic<bool>session_active_{true},full_duplex_{true},runtime_started_{true};void*events_=nullptr;
 void Exit(){TAIL}
};
int main(){SignalingProtocol p;p.Exit();assert(detached==1&&!p.call_appearance_ready_&&!p.runtime_);}
'''.replace('TAIL', tail)
        self.run_code(code)

    def test_destructor_waits_for_actual_stop_and_retries_full_command_queue(self):
        source = (ROOT / 'main/signaling/ur_signaling_protocol.cc').read_text()
        actual = method(source, 'SignalingProtocol::~SignalingProtocol()')
        code = r'''
#include <atomic>
#include <cassert>
std::atomic<bool>running{true};unsigned elapsed=0,posts=0,deleted=0;
#define pdMS_TO_TICKS(x) (x)
void vTaskDelay(unsigned ticks){elapsed+=ticks;if(elapsed>=3000)running=false;}
void vQueueDelete(void*){assert(!running&&"two second timeout freed an active host queue");++deleted;}
void vEventGroupDelete(void*){assert(!running);++deleted;}
struct SignalingProtocol {
 enum class Command{kShutdown};void*task_=reinterpret_cast<void*>(1);void*commands_=task_;void*events_=task_;
 std::atomic<bool>&task_running_=running;static SignalingProtocol*instance_;
 bool PostCommand(Command){return ++posts>2;}
 ~SignalingProtocol();
};
SignalingProtocol* SignalingProtocol::instance_=nullptr;
ACTUAL
int main(){{SignalingProtocol p;SignalingProtocol::instance_=&p;}assert(elapsed>=3000&&posts>=3&&deleted==2&&SignalingProtocol::instance_==nullptr);}
'''.replace('ACTUAL', actual)
        self.run_code(code)
