"""执行生产人际媒体调度块：人际媒体让出高优先级，返回后恢复宿主调度。"""
from pathlib import Path
import subprocess
import tempfile
import unittest
from .test_call_audio_decode import method
ROOT=Path(__file__).resolve().parents[2]

class CallMediaPriorityTest(unittest.TestCase):
    """分别验证 UDP 与 RTC 调度及宿主优先级恢复。"""
    def test_actual_dispatch_restores_host_priority(self):
        source=(ROOT/'main/signaling/ur_signaling_call.cc').read_text().split('void UrSignalingCall::Impl::Poll(',1)[1]
        actual=method(source,'if (media_ != nullptr)')
        harness=r'''
#include <cassert>
unsigned priority=5;unsigned uxTaskPriorityGet(void*){return priority;}void vTaskPrioritySet(void*,unsigned p){priority=p;}
struct Media{bool udp=false;int observed=0;bool IsUDP(){return udp;}void Poll(){observed=priority;}};
struct Owner{Media*media_=nullptr;bool media_active_=true;void Poll(){ACTUAL}};
int main(){Media media;Owner owner{&media};
 // RTC 恢复此前有看门狗验证的低优先级；返回后不得影响宿主。
 priority=7;owner.Poll();assert(media.observed==2&&priority==7);
 // UDP 仍使用已验证的上行调度，不因 RTC 修复改变。
 media.udp=true;owner.Poll();assert(media.observed==5&&priority==7);
 // 本来较低的宿主优先级不应被提高，非通话状态不调整。
 priority=3;owner.Poll();assert(media.observed==3&&priority==3);
 priority=5;owner.Poll();assert(media.observed==5&&priority==5);
 priority=7;owner.media_active_=false;owner.Poll();assert(media.observed==7&&priority==7);}
'''.replace('ACTUAL',actual)
        output=ROOT.parents[1]/'.temp';output.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=output) as directory:
            binary=Path(directory)/'priority'
            for enabled in [1,0]:
                p=subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-DCONFIG_UR_SOFTWARE_AEC='+str(enabled),'-x','c++','-','-o',str(binary)],input=harness,text=True,capture_output=True)
                self.assertEqual(p.returncode,0,p.stderr)
                p=subprocess.run([str(binary)],capture_output=True)
                self.assertEqual(p.returncode,0,p.stderr.decode(errors='replace'))
if __name__=='__main__':unittest.main()
