"""执行生产接听门控，验证语音模型退出与旧来电隔离。"""
from pathlib import Path
import subprocess
import tempfile
import unittest
from .test_call_audio_decode import method

ROOT = Path(__file__).resolve().parents[2]


class CallVoiceAcceptGateTest(unittest.TestCase):
    """替换应用调度与音频服务边界，保留真实门控及 UI 分支。"""

    def test_wake_is_rearmed_without_starting_ai_while_guarded(self):
        source = (ROOT / 'main/application.cc').read_text()
        method_body = method(source, 'void Application::HandleWakeWordDetectedEvent(')
        begin = method_body.index('#if CONFIG_UR_SPARKBOT_VOICE_MODEL_TEST')
        end = method_body.index('#endif', begin) + len('#endif')
        guard = method_body[begin:end]
        code = r'''
#include <cassert>
#define CONFIG_UR_SPARKBOT_VOICE_MODEL_TEST 1
struct UrSignalingCall {bool active=false;static UrSignalingCall& Instance(){static UrSignalingCall call;return call;}bool VoiceAnswerGuardActive(){return active;}};
struct Audio {int rearms=0;void EnableWakeWordDetection(bool value){assert(value);++rearms;}};
struct Application {Audio audio_service_;int ai_starts=0;void Handle();};
void Application::Handle(){
''' + guard + r'''
 ++ai_starts;
}
int main(){Application app;UrSignalingCall::Instance().active=true;app.Handle();assert(app.ai_starts==0&&app.audio_service_.rearms==1);UrSignalingCall::Instance().active=false;app.Handle();assert(app.ai_starts==1);}
'''
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / 'wake')
            result = subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                     '-x', 'c++', '-', '-o', binary], input=code,
                                    text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([binary], check=True)

    def test_accept_waits_and_stale_results_are_rejected(self):
        source = (ROOT / 'main/signaling/ur_signaling_call.cc').read_text()
        actual = '\n'.join(method(source, sig) for sig in (
            'void UrSignalingCall::Impl::StartVoiceAnswerTest(',
            'void UrSignalingCall::Impl::StopVoiceAnswerTest(',
            'void UrSignalingCall::Impl::AdvanceVoiceAnswerAccept(',
            'void UrSignalingCall::Impl::AdvanceVoiceAnswerGuard(',
        ))
        apply = method(source, 'void UrSignalingCall::Impl::ApplyUiCommand(')
        start = apply.index('    const auto state = client_->state();')
        end = apply.index('        case kUiToggleMic:', start)
        actual += '\nvoid UrSignalingCall::Impl::ApplyUiCommand(int command,std::uint64_t now) {\n' + apply[start:end] + '        default: break;\n    }\n}\n'
        delivery = method(source, 'bool UrSignalingCall::Impl::DeliverInvite(')
        pending_start = delivery.index('    if (pending_accept_ &&')
        pending_end = delivery.index('    return true;', pending_start)
        actual += '\nvoid UrSignalingCall::Impl::DeliverPending(std::uint64_t now) {\n' + delivery[pending_start:pending_end] + '}\n'
        code = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <deque>
#include <functional>
#define CONFIG_UR_SPARKBOT_VOICE_MODEL_TEST 1
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
enum { kDeviceStateIdle=1 };
namespace ur::device {enum class HumanCallState {Ringing,Ending,Connected};}
struct CallVoiceWorker {enum class Outcome {Recognized,Cancelled,Unavailable};};
struct Audio {bool busy=false;int starts=0,stops=0;std::function<void(std::uint32_t,CallVoiceWorker::Outcome)> done;
 bool BeginCallVoiceCapture(std::uint32_t,std::function<void(std::uint32_t,CallVoiceWorker::Outcome)> f){++starts;busy=true;done=f;return true;}
 void StopCallVoiceCapture(){++stops;}
 bool IsCallVoiceCaptureBusy()const{return busy;}
};
struct Application {Audio audio;std::deque<std::function<void()>> jobs;int state=kDeviceStateIdle;
 static Application& GetInstance(){static Application a;return a;}
 void Schedule(std::function<void()> f){jobs.push_back(std::move(f));}
 Audio& GetAudioService(){return audio;}int GetDeviceState()const{return state;}
 void run(){auto f=std::move(jobs.front());jobs.pop_front();f();}
 void drain(){while(!jobs.empty())run();}
};
struct Client {ur::device::HumanCallState s=ur::device::HumanCallState::Ringing;int accepted=0,rejected=0;
 auto state()const{return s;}bool accept(std::uint64_t){++accepted;s=ur::device::HumanCallState::Connected;return true;}
 bool reject(std::uint64_t){++rejected;s=ur::device::HumanCallState::Ending;return true;}
 bool end(std::uint64_t){s=ur::device::HumanCallState::Ending;return true;}
};
struct UrSignalingCall {struct Impl {
 enum {kUiAccept=1,kUiReject=2,kUiHangUp=3};
 std::atomic<bool> voice_guard_{false},voice_allowed_{false};std::atomic<unsigned> voice_app_jobs_{0};
 bool pending_accept_=false;bool voice_accept_pending_=false,voice_guard_end_pending_=false;std::uint32_t voice_accept_generation_=0;std::atomic<std::uint32_t> ui_generation_{7};
 Client storage;Client* client_=&storage;int queued=0;std::uint32_t queued_generation=0;
 bool Busy()const{return client_!=nullptr;}
 void PostUiForGeneration(int command,std::uint32_t g){queued=command;queued_generation=g;}
 void StartVoiceAnswerTest();void StopVoiceAnswerTest(bool);void AdvanceVoiceAnswerGuard();void AdvanceVoiceAnswerAccept(std::uint64_t);void ApplyUiCommand(int,std::uint64_t);void DeliverPending(std::uint64_t);
};};
''' + actual + r'''
int main(){
 using Impl=UrSignalingCall::Impl;auto& app=Application::GetInstance();
 Impl early;early.StartVoiceAnswerTest();early.ApplyUiCommand(Impl::kUiAccept,1);
 early.AdvanceVoiceAnswerAccept(1);assert(early.storage.accepted==0);
 app.drain();early.AdvanceVoiceAnswerAccept(2);assert(early.storage.accepted==1&&app.audio.starts==0);
 Impl active;active.StartVoiceAnswerTest();app.drain();assert(app.audio.starts==1&&app.audio.busy);
 active.ApplyUiCommand(Impl::kUiAccept,3);app.drain();active.AdvanceVoiceAnswerAccept(4);assert(active.storage.accepted==0);
 app.audio.busy=false;active.AdvanceVoiceAnswerAccept(5);assert(active.storage.accepted==1);
 Impl cancelled;cancelled.StartVoiceAnswerTest();app.drain();auto old=app.audio.done;
 cancelled.ApplyUiCommand(Impl::kUiReject,6);app.audio.busy=false;app.drain();
 int before_stale=app.audio.stops;cancelled.ui_generation_=8;old(7,CallVoiceWorker::Outcome::Recognized);app.drain();
 assert(app.audio.stops==before_stale);
 assert(cancelled.storage.rejected==1&&cancelled.queued_generation==7&&cancelled.storage.accepted==0);
 Impl pending;pending.pending_accept_=true;app.audio.busy=true;pending.DeliverPending(6);
 app.drain();pending.AdvanceVoiceAnswerAccept(6);assert(pending.storage.accepted==0);
 app.audio.busy=false;pending.AdvanceVoiceAnswerAccept(6);assert(pending.storage.accepted==1);
 Impl gone;gone.StartVoiceAnswerTest();app.drain();gone.ApplyUiCommand(Impl::kUiAccept,7);app.drain();
 gone.storage.s=ur::device::HumanCallState::Ending;app.audio.busy=false;gone.AdvanceVoiceAnswerAccept(8);
 assert(gone.storage.accepted==0&&!gone.voice_accept_pending_);
 Impl end;end.StartVoiceAnswerTest();app.drain();end.StopVoiceAnswerTest(true);app.drain();
 end.AdvanceVoiceAnswerGuard();assert(end.voice_guard_); // 模型未释放时继续阻止 AI。
 app.audio.busy=false;end.AdvanceVoiceAnswerGuard();assert(!end.voice_guard_);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / 'gate')
            result = subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                     '-x', 'c++', '-', '-o', binary], input=code,
                                    text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([binary], text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
