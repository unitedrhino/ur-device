"""执行真实来电分派，验证 AI 让位期间第二个来电不会覆盖首个邀请。"""
from pathlib import Path
import subprocess
import tempfile
import unittest
from .test_call_audio_decode import method

ROOT = Path(__file__).resolve().parents[2]


class PendingInviteBusyTest(unittest.TestCase):
    """仅替换信令和媒体边界，调用生产 Handle 实现。"""

    def test_pending_invite_is_not_replaced(self):
        source = (ROOT / 'main/signaling/ur_signaling_call.cc').read_text()
        handler = method(source, 'bool UrSignalingCall::Impl::Handle(')
        retire = method(source, 'void UrSignalingCall::Impl::RetirePending(')
        code = r'''
#include <cassert>
#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include "ur_signaling_pending_tombstones.h"
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
template <class... T> void log_w(T...) {}
#define ESP_LOGW(...) log_w(__VA_ARGS__)
constexpr const char* kTag="test";
namespace ur { namespace device {
enum class HumanCallState { Idle, Available, Failed, Ringing };
enum class DecodeResult { Ok };
struct Envelope { std::string method, token, session, payload; bool request=true;
 std::optional<std::uint64_t> event_sequence=1;std::optional<int> code; };
DecodeResult decode_envelope(std::string_view body, Envelope& out) {
 out.method="sessionInvite";out.token=std::string(body);out.session=std::string(body);
 if (body=="sessionEnded-first") {out.method="sessionEnded";out.request=false;out.session="humanCall-first";out.event_sequence=2;}
 if (body=="sessionEnded-fourth") {out.method="sessionEnded";out.request=false;out.session="humanCall-fourth";out.event_sequence=2;}
 if (body=="sessionEnded-other") {out.method="sessionEnded";out.request=false;out.session="other";out.event_sequence=2;}
 if (body=="sessionEnded-old") {out.method="sessionEnded";out.request=false;out.session="humanCall-first";out.event_sequence=1;}
 if (body=="sessionEnded-request") {out.method="sessionEnded";out.request=true;out.session="humanCall-first";out.event_sequence=2;}
 if (body=="sessionEnded-code") {out.method="sessionEnded";out.request=false;out.session="humanCall-first";out.event_sequence=2;out.code=200;}
 if (body=="humanCall-altered") {out.token="humanCall-first";out.session="different";}
 if (body=="humanCall-same-session") {out.session="humanCall-first";}
 out.payload="original";
 if (body=="humanCall-changed-payload") {out.token="humanCall-first";out.session="humanCall-first";out.payload="changed";}
 if (body=="humanCall-changed-seq") {out.token="humanCall-first";out.session="humanCall-first";out.event_sequence=2;}
 return DecodeResult::Ok;
}
bool human_call_invitation(std::string_view body) { return body.find("humanCall")!=std::string_view::npos; }
bool human_call_terminal_matches(const Envelope& e,std::string_view session,std::uint64_t prev) {
 return e.method=="sessionEnded"&&!e.request&&!e.code&&e.session==session&&e.event_sequence&&*e.event_sequence>prev;
}
std::optional<std::uint64_t> human_call_pending_deadline(const Envelope& e) {
 if (e.token=="humanCall-invalid") return {};
 if (e.token=="humanCall-expired") return 1;
 if (e.token=="humanCall-late") return 2000;
 return 1000;
}
} }
struct Client {
 bool owns_shared_create_reply(const ur::device::Envelope&) const{return false;}
 std::string id; ur::device::HumanCallState state() {return ur::device::HumanCallState::Ringing;}
 const std::string& session_id() {return id;}
 bool receive_shared(std::string_view,std::string_view,std::uint64_t,std::uint64_t){return true;}
 struct Facts {int failure=0;}; Facts failure_facts(){return {};}
};
const std::string kEmptySession;
unsigned esp_get_free_heap_size(){return 0;}
unsigned esp_get_free_internal_heap_size(){return 0;}
struct UrSignalingCall { struct Impl {
 struct Pending { std::string token, session, body; std::uint64_t expires; std::size_t retained_bytes; };
 void *port_=this,*pcm_=this,*codec_=this;Client* client_=nullptr;
 std::optional<Pending> pending_invite_;bool host_busy=true,pending_accept_=false;
 std::string identity_="p/d"; PendingCallTombstones pending_tombstones_;
 std::uint32_t ui_generation_=0;
 std::atomic<bool> dial_queued_{false};
 int yield_requests=0,delivered=0,rejected=0;std::string rejected_token,rejected_reason;
 bool HostBusy(){return host_busy;} void TeardownLeg(){}
 void RetirePending(PendingCallTombstones::Decision decision);
 void StartYieldWait(const ur::device::Envelope& e,std::string_view body,std::uint64_t,std::uint64_t expiry){pending_invite_=Pending{e.token,e.session,std::string(body),expiry,e.token.size()+e.session.size()+e.payload.size()};++yield_requests;}
 void DeliverInvite(std::string_view,std::uint64_t,std::uint64_t){++delivered;pending_invite_.reset();}
 bool EnsureLeg(std::uint64_t){return true;}
 void RejectInvite(const ur::device::Envelope& e,const char* reason){++rejected;rejected_token=e.token;rejected_reason=reason;}
 bool Handle(std::string_view,std::string_view,std::uint64_t,std::uint64_t);
}; };
// PRODUCTION
int main(){
 UrSignalingCall::Impl invalid;
 assert(!invalid.Handle("wrong-topic","humanCall-first",1,1));
 assert(!invalid.pending_invite_);
 assert(invalid.Handle("$thing/down/signaling/p/d","humanCall-invalid",1,10));
 assert(invalid.Handle("$thing/down/signaling/p/d","humanCall-expired",1,10));
 assert(invalid.yield_requests==0 && !invalid.pending_invite_);
 UrSignalingCall::Impl call;
 assert(call.Handle("$thing/down/signaling/p/d","humanCall-first",1,1));
 assert(call.pending_invite_ && call.pending_invite_->token=="humanCall-first");
 assert(call.yield_requests==1);
 assert(call.Handle("$thing/down/signaling/p/d","humanCall-first",2,2));
 assert(call.yield_requests==1 && call.rejected==0);
 assert(call.Handle("$thing/down/signaling/p/d","humanCall-altered",2,2));
 assert(call.Handle("$thing/down/signaling/p/d","humanCall-same-session",2,2));
 assert(call.Handle("$thing/down/signaling/p/d","humanCall-changed-payload",2,2));
 assert(call.Handle("$thing/down/signaling/p/d","humanCall-changed-seq",2,2));
 assert(call.rejected==0 && call.yield_requests==1);
 assert(call.Handle("$thing/down/signaling/p/d","humanCall-second",3,3));
 assert(call.rejected==1 && call.rejected_token=="humanCall-second" && call.rejected_reason=="busy");
 assert(call.pending_invite_ && call.pending_invite_->token=="humanCall-first");
 call.host_busy=false;
 assert(call.Handle("$thing/down/signaling/p/d","humanCall-third",4,4));
 assert(call.rejected==2 && call.delivered==0);
 assert(call.pending_invite_ && call.pending_invite_->token=="humanCall-first");
 // 相同忙线请求重投维持忙线，不改变原等待邀请。
 assert(call.Handle("$thing/down/signaling/p/d","humanCall-second",4,4));
 assert(call.rejected==3 && call.pending_invite_);
 call.pending_accept_=true;
 assert(!call.Handle("$thing/down/signaling/p/d","sessionEnded-other",5,5));
 assert(!call.Handle("$thing/down/signaling/p/d","sessionEnded-old",5,5));
 assert(!call.Handle("$thing/down/signaling/p/d","sessionEnded-request",5,5));
 assert(!call.Handle("$thing/down/signaling/p/d","sessionEnded-code",5,5));
 assert(!call.Handle("wrong-topic","sessionEnded-first",5,5));
 assert(call.pending_invite_ && call.pending_accept_);
 assert(call.Handle("$thing/down/signaling/p/d","sessionEnded-first",5,5));
 assert(!call.pending_invite_ && !call.pending_accept_);
 assert(call.Handle("$thing/down/signaling/p/d","humanCall-first",6,6));
 assert(call.Handle("$thing/down/signaling/p/d","humanCall-changed-payload",6,6));
 assert(call.delivered==0 && !call.pending_invite_);
 // 剩余槽为当前等待预留；并发拒接溢出时进入截止屏障，不能复活。
 call.host_busy=true;
 assert(call.Handle("$thing/down/signaling/p/d","humanCall-fourth",6,6));
 assert(call.pending_invite_);
 assert(call.Handle("$thing/down/signaling/p/d","humanCall-fifth",6,6));
 assert(call.Handle("$thing/down/signaling/p/d","sessionEnded-fourth",6,6));
 assert(!call.pending_invite_);
 call.host_busy=false;
 assert(call.Handle("$thing/down/signaling/p/d","humanCall-fifth",7,7));
 assert(call.delivered==0 && !call.pending_invite_);
 // 已过原截止才回收，时钟回拨不能让旧邀请复活。
 assert(call.Handle("$thing/down/signaling/p/d","humanCall-first",7,999));
 assert(call.delivered==0);
 assert(call.Handle("$thing/down/signaling/p/d","humanCall-first",8,10));
 assert(call.delivered==0);
 assert(call.Handle("$thing/down/signaling/p/d","humanCall-first",9,1000));
 assert(call.delivered==0);
 assert(call.Handle("$thing/down/signaling/p/d","humanCall-late",10,1000));
 assert(call.delivered==1);
}
'''.replace('// PRODUCTION', retire + '\n' + handler)
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / 'pending_invite')
            subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-I', str(ROOT / 'main/signaling'), '-x', 'c++', '-', '-o', binary], input=code, text=True, check=True)
            subprocess.run([binary], check=True)


if __name__ == '__main__':
    unittest.main()
