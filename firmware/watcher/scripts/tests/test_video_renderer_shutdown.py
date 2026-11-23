"""回放真实下行解码器Stop，覆盖慢帧超过旧三秒上限后仍在使用缓冲的边界。"""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]

class VideoRendererShutdownTest(unittest.TestCase):
    """硬件释放必须晚于工作线程真实退出，不能把定时等待结束当作退出证明。"""
    def test_actual_decoder_stop_waits_before_releasing(self):
        production=(ROOT/"main/signaling/ur_signaling_call_video.cc").read_text()
        stop=production.split("    void Stop() override {",1)[1].split("    /* OnNalu",1)[0]
        source=r'''
#include <atomic>
#include <cassert>
#include <functional>
#include <mutex>
using TaskHandle_t=void*;
constexpr int kStopWaitMs=3000;
bool worker_live=true,released=false;int elapsed=0;
void vTaskDelay(int);
#define pdMS_TO_TICKS(x) (x)
void LogHeapDiag(const char*) {}
struct Decoder { void close(Decoder*){assert(!worker_live);} void del(Decoder*){assert(!worker_live);released=true;} };
struct CallVideoRendererImpl {
 std::mutex control_mutex_,callback_mutex_;std::atomic<bool> running_{true};
 std::atomic<void*> task_{reinterpret_cast<void*>(1)}; Decoder* decoder_=nullptr;
 std::function<void()> on_frame_;
 void DrainQueue(){assert(!worker_live);} void FreeOutputBuffers(){assert(!worker_live);}
 void Stop();
};
CallVideoRendererImpl* owner=nullptr;
void vTaskDelay(int ticks){assert(!released);elapsed+=ticks;if(elapsed>=3600){worker_live=false;owner->task_.store(nullptr);}}
void CallVideoRendererImpl::Stop(){
'''+stop+r'''
int main(){CallVideoRendererImpl renderer;Decoder decoder;owner=&renderer;renderer.decoder_=&decoder;renderer.Stop();assert(released && !worker_live && elapsed>=3600);renderer.Stop();}
'''
        compiler=shutil.which("g++") or shutil.which("clang++")
        self.assertIsNotNone(compiler)
        output=ROOT.parents[1]/".temp";output.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=output) as directory:
            binary=Path(directory)/"stop"
            result=subprocess.run([compiler,"-std=c++17","-Wall","-Wextra","-Werror","-fsanitize=undefined","-x","c++","-","-o",str(binary)],input=source,text=True,capture_output=True)
            self.assertEqual(result.returncode,0,result.stderr)
            result=subprocess.run([str(binary)],capture_output=True)
            self.assertEqual(result.returncode,0,result.stderr.decode(errors="replace"))
