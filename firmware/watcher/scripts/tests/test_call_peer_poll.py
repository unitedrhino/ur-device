"""验证生产select包装器的任务归属、连接阶段边界及调用者参数保护。"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'main/signaling'

class CallPeerPollTest(unittest.TestCase):
    """仅替换系统select和任务身份，包装器及RAII归属管理使用实际源码。"""
    def test_actual_select_scope_preserves_other_tasks_and_handshake(self):
        harness = r'''
#include "ur_signaling_call_poll.h"
#include <cassert>
#include <sys/select.h>
#include <thread>
thread_local int marker;thread_local long observed=-2;
extern "C" void* xTaskGetCurrentTaskHandle(){return &marker;}
extern "C" int __real_select(int,fd_set*,fd_set*,fd_set*,timeval* t){observed=t?t->tv_sec*1000000+t->tv_usec:-1;return 77;}
extern "C" int __wrap_select(int,fd_set*,fd_set*,fd_set*,timeval*);
void sample(long microseconds,long expected){timeval wait{0,microseconds};assert(__wrap_select(0,nullptr,nullptr,nullptr,&wait)==77);assert(observed==expected);assert(wait.tv_usec==microseconds);}
int main(){
 sample(1000,1000);
 {ur::device::CallPeerPollScope handshake(false);sample(1000,1000);}
 {ur::device::CallPeerPollScope connected(true);
  sample(1000,0);sample(50000,50000);sample(0,0);
  __wrap_select(0,nullptr,nullptr,nullptr,nullptr);assert(observed==-1);
  std::thread other([]{sample(1000,1000);});other.join();sample(1000,0);
  {ur::device::CallPeerPollScope nested(true);sample(1000,0);}sample(1000,0);
 }
 sample(1000,1000);
}
'''
        output = ROOT.parents[1] / '.temp'
        output.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=output) as folder:
            folder=Path(folder);headers=folder/'freertos';headers.mkdir()
            (headers/'FreeRTOS.h').write_text('#pragma once\nusing TaskHandle_t=void*;\n')
            (headers/'task.h').write_text('#pragma once\nextern "C" void* xTaskGetCurrentTaskHandle();\n')
            binary=folder/'poll'
            result=subprocess.run(['g++','-std=c++17','-pthread','-Wall','-Wextra','-Werror','-I',str(SOURCE),'-I',str(folder),'-x','c++','-',str(SOURCE/'ur_signaling_call_poll.cc'),'-o',str(binary)],input=harness,text=True,capture_output=True,timeout=30)
            self.assertEqual(result.returncode,0,result.stderr)
            result=subprocess.run([str(binary)],capture_output=True,timeout=5)
            self.assertEqual(result.returncode,0,result.stderr.decode(errors='replace'))

if __name__=='__main__':unittest.main()
