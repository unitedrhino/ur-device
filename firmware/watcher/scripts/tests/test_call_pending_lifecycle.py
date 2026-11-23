"""执行让位等待的生产生命周期，验证终态记录及后续邀请许可。"""
from pathlib import Path
import subprocess
import tempfile
import unittest
from .test_call_audio_decode import method

ROOT = Path(__file__).resolve().parents[2]


class PendingLifecycleTest(unittest.TestCase):
    def test_yield_failure_timeout_and_delivery(self):
        source = (ROOT / 'main/signaling/ur_signaling_call.cc').read_text()
        functions = '\n'.join(method(source, signature) for signature in (
            'void UrSignalingCall::Impl::RetirePending(',
            'bool UrSignalingCall::Impl::EnsureUiQueue(',
            'void UrSignalingCall::Impl::PostUi(',
            'void UrSignalingCall::Impl::PostUiForGeneration(',
            'void UrSignalingCall::Impl::DrainUiCommands(',
            'void UrSignalingCall::Impl::StartYieldWait(',
            'void UrSignalingCall::Impl::AdvanceYieldWait(',
        ))
        apply = method(source, 'void UrSignalingCall::Impl::ApplyUiCommand(')
        apply = apply[:apply.index('    const auto state = client_->state();')] + '    (void)now;\n}\n'
        functions += '\n' + apply
        code = r'''
#include "ur_signaling_pending_tombstones.h"
#include <cassert>
#include <optional>
#include <string>
#include <string_view>
#include <deque>
#include <vector>
#include <cstring>
#include <atomic>
constexpr int kUiQueueLength=8, MALLOC_CAP_SPIRAM=1, MALLOC_CAP_8BIT=2, pdTRUE=1;
struct Queue {unsigned size;std::deque<std::vector<char>> commands;};
bool fail_alloc=false;
Queue* xQueueCreateWithCaps(int, unsigned size, int){return fail_alloc?nullptr:new Queue{size,{}};}
int xQueueSend(Queue* q,const void* item,int){auto* p=static_cast<const char*>(item);q->commands.emplace_back(p,p+q->size);return 1;}
int xQueueReceive(Queue* q,void* item,int){if(q->commands.empty())return 0;std::memcpy(item,q->commands.front().data(),q->size);q->commands.pop_front();return pdTRUE;}
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
constexpr std::uint64_t kYieldTimeoutMs=10000,kYieldCheckMs=500;
namespace ur { namespace device {
enum class DecodeResult {Ok};
struct Envelope {std::string token,session,payload;};
DecodeResult decode_envelope(std::string_view,Envelope& e){e.token="token";e.session="session";e.payload="payload";return DecodeResult::Ok;}
} }
struct UrSignalingCall {struct Impl {
 enum {kUiAccept=1,kUiReject=2,kUiHangUp=3,kUiDial=6};
 enum class UiState {Hidden,Connecting,Ended};
 UiState ui_state_=UiState::Hidden;
 bool voice_call_=false;
 struct QueuedUiCommand {int command;std::uint32_t generation;bool voice_only=false;};
 std::optional<QueuedUiCommand> pending_dial_;std::atomic<bool> dial_queued_{false};
 std::uint64_t dial_requested_at_=0;
 bool Busy(){return client_!=nullptr;}void FireState(UiState){}
 struct PendingInvite {std::string token,session,body;std::uint64_t expires=0,started=0,last_check=0;std::size_t retained_bytes=0;};
 std::optional<PendingInvite> pending_invite_;PendingCallTombstones pending_tombstones_;
 Queue* ui_queue_=nullptr;std::atomic<std::uint32_t> ui_generation_{0}; bool pending_accept_=false;void* client_=nullptr;bool host_busy=true,yield_result=true;
 int rejects=0,delivered=0,yields=0;
 void RejectInvite(const ur::device::Envelope&,const char*){++rejects;}
 bool RequestHostYield(){++yields;return yield_result;}
 bool HostBusy(){return host_busy;}
 void DeliverInvite(std::string_view,std::uint64_t,std::uint64_t){++delivered;}
 bool EnsureUiQueue();void PostUi(int);void PostUiForGeneration(int,std::uint32_t);void DrainUiCommands(std::uint64_t);
 void RetirePending(PendingCallTombstones::Decision);
 void StartYieldWait(const ur::device::Envelope&,std::string_view,std::uint64_t,std::uint64_t);
 void AdvanceYieldWait(std::uint64_t,std::uint64_t);
 void ApplyUiCommand(int,std::uint64_t);
};};
// PRODUCTION
int main(){
 using Decision=PendingCallTombstones::Decision;
 ur::device::Envelope invite{"token","session","payload"};
 UrSignalingCall::Impl denied;denied.yield_result=false;
 denied.StartYieldWait(invite,"body",100,1000);
 assert(denied.rejects==1 && !denied.pending_invite_);
 assert(denied.pending_tombstones_.Find("token","session","payload")==Decision::Busy);
 UrSignalingCall::Impl first;
 first.StartYieldWait(invite,"body",100,1000);
 first.PostUi(UrSignalingCall::Impl::kUiAccept);first.DrainUiCommands(101);
 assert(first.pending_accept_ && "首次让位时接听命令丢失");
 UrSignalingCall::Impl stale;
 stale.StartYieldWait(invite,"body",100,1000);
 stale.PostUi(UrSignalingCall::Impl::kUiAccept);
 const auto old_generation=stale.ui_generation_.load();
 stale.RetirePending(Decision::Cancelled);
 // 投递方已经取到旧代次但在终态之后才入队，也不得越过会话边界。
 UrSignalingCall::Impl::QueuedUiCommand late{UrSignalingCall::Impl::kUiReject,old_generation};
 stale.PostUiForGeneration(late.command,late.generation);
 ur::device::Envelope newer{"token2","session2","payload2"};
 stale.StartYieldWait(newer,"body",102,1000);
 stale.DrainUiCommands(103);
 assert(stale.pending_invite_ && !stale.pending_accept_ && "旧邀请的排队命令影响了新邀请");
 UrSignalingCall::Impl exhausted;fail_alloc=true;
 exhausted.StartYieldWait(invite,"body",100,1000);fail_alloc=false;
 assert(exhausted.rejects==1 && !exhausted.pending_invite_ && exhausted.yields==0);
 assert(exhausted.pending_tombstones_.Find("token","session","payload")==Decision::Busy);
 UrSignalingCall::Impl decline;
 decline.StartYieldWait(invite,"body",100,1000);
 decline.PostUi(UrSignalingCall::Impl::kUiAccept);decline.DrainUiCommands(101);
 assert(decline.pending_invite_ && decline.pending_accept_);
 decline.PostUi(UrSignalingCall::Impl::kUiReject);decline.DrainUiCommands(102);
 assert(!decline.pending_invite_ && !decline.pending_accept_ && decline.rejects==1);
 assert(decline.pending_tombstones_.Find("token","session","payload")==Decision::Declined);
 UrSignalingCall::Impl timeout;
 timeout.StartYieldWait(invite,"body",100,1000);timeout.pending_accept_=true;
 timeout.AdvanceYieldWait(10100,200);
 assert(timeout.rejects==1 && !timeout.pending_invite_ && !timeout.pending_accept_);
 assert(timeout.pending_tombstones_.Find("token","session","payload")==Decision::Busy);
 UrSignalingCall::Impl ready;
 ready.StartYieldWait(invite,"body",100,1000);ready.AdvanceYieldWait(600,200);
 assert(ready.pending_invite_ && ready.yields==2 && ready.delivered==0);
 ready.host_busy=false;ready.AdvanceYieldWait(1100,200);
 assert(!ready.pending_invite_ && ready.delivered==1);
 assert(ready.pending_tombstones_.Find("token","session","payload")==Decision::None);
 UrSignalingCall::Impl overlap;
 overlap.StartYieldWait(invite,"body",100,1000);overlap.pending_accept_=true;overlap.client_=&overlap;
 overlap.AdvanceYieldWait(200,200);
 assert(!overlap.pending_invite_ && !overlap.pending_accept_);
 assert(overlap.pending_tombstones_.Find("token","session","payload")==Decision::Busy);
}
'''.replace('// PRODUCTION', functions)
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / 'lifecycle')
            subprocess.run(['c++', '-x', 'c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-I', str(ROOT / 'main/signaling'), '-', '-o', binary],
                           input=code, text=True, check=True)
            subprocess.run([binary], check=True)


if __name__ == '__main__':
    unittest.main()
