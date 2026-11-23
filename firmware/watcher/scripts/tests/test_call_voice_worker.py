"""执行真实识别工作线程，替换 RTOS 和模型以验证隔离及取消屏障。"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class CallVoiceWorkerTest(unittest.TestCase):
    """覆盖识别、取消、模型失败和输入丢帧，不以替身结果宣称真实识别。"""

    def test_worker_lifecycle_and_fail_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'freertos').mkdir()
            (root / 'freertos/FreeRTOS.h').write_text('''#pragma once
#define pdPASS 1
#define portMAX_DELAY -1
#define CONFIG_FREERTOS_NUMBER_OF_CORES 2
using TaskHandle_t=void*;using SemaphoreHandle_t=void*;
''')
            (root / 'freertos/task.h').write_text('#pragma once\n#include "FreeRTOS.h"\nvoid vTaskDelay(int);\n')
            (root / 'freertos/semphr.h').write_text('''#pragma once
#include "FreeRTOS.h"
SemaphoreHandle_t xSemaphoreCreateBinary();void xSemaphoreGive(SemaphoreHandle_t);int xSemaphoreTake(SemaphoreHandle_t,int);void vSemaphoreDelete(SemaphoreHandle_t);
''')
            (root / 'freertos/idf_additions.h').write_text('''#pragma once
#include "FreeRTOS.h"
int xTaskCreatePinnedToCoreWithCaps(void(*)(void*),const char*,int,void*,int,TaskHandle_t*,int,int);void vTaskDeleteWithCaps(void*);
''')
            (root / 'esp_heap_caps.h').write_text('#pragma once\n#include <cstddef>\n#define MALLOC_CAP_SPIRAM 1\n#define MALLOC_CAP_8BIT 2\nstd::size_t heap_caps_get_free_size(int);\n')
            (root / 'esp_log.h').write_text('#pragma once\n#define ESP_LOGI(...) ((void)0)\n')
            (root / 'model_path.h').write_text('#pragma once\nstruct srmodel_list_t{};\n')
            (root / 'esp_mn_iface.h').write_text('#pragma once\nstruct esp_mn_iface_t{};struct model_iface_data_t{};\n')
            code = r'''
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include "call_voice_worker.h"
#include "call_voice_recognizer.h"
using namespace std::chrono_literals;
struct Sem {std::mutex m;std::condition_variable cv;bool signaled=false;};
SemaphoreHandle_t xSemaphoreCreateBinary(){return new Sem;}
void xSemaphoreGive(SemaphoreHandle_t p){auto* s=static_cast<Sem*>(p);{std::lock_guard<std::mutex> l(s->m);s->signaled=true;}s->cv.notify_all();}
int xSemaphoreTake(SemaphoreHandle_t p,int){auto* s=static_cast<Sem*>(p);std::unique_lock<std::mutex> l(s->m);s->cv.wait(l,[&]{return s->signaled;});return 1;}
void vSemaphoreDelete(SemaphoreHandle_t p){delete static_cast<Sem*>(p);}
int xTaskCreatePinnedToCoreWithCaps(void(*fn)(void*),const char*,int,void* p,int,TaskHandle_t* h,int,int){*h=p;std::thread([fn,p]{fn(p);}).detach();return 1;}
void vTaskDeleteWithCaps(void*){} void vTaskDelay(int){std::this_thread::sleep_for(1ms);}
std::atomic<bool> fail_start{false},hold_start{false},in_start{false},hold_feed{false},in_feed{false},released{true};
std::atomic<std::size_t> psram{4*1024*1024},after_load{1024*1024};std::size_t heap_caps_get_free_size(int){return psram.load();}
std::atomic<bool> CallVoiceRecognizer::owned_{false};
CallVoiceRecognizer::~CallVoiceRecognizer(){Stop();}
bool CallVoiceRecognizer::Start(srmodel_list_t*){in_start=true;while(hold_start)std::this_thread::sleep_for(1ms);in_start=false;if(fail_start)return false;owns_=true;released=false;psram=after_load.load();return true;}
bool CallVoiceRecognizer::Feed(const int16_t* data,std::size_t n){in_feed=true;while(hold_feed)std::this_thread::sleep_for(1ms);in_feed=false;return n && data[0]==42;}
void CallVoiceRecognizer::Stop(){if(owns_){owns_=false;released=true;psram=4*1024*1024;}}
struct Observer {std::mutex m;std::condition_variable cv;int n=0;std::uint32_t generation=0;CallVoiceWorker::Outcome outcome=CallVoiceWorker::Outcome::Unavailable;
 void record(std::uint32_t g,CallVoiceWorker::Outcome o){assert(released);{std::lock_guard<std::mutex> l(m);++n;generation=g;outcome=o;}cv.notify_all();}
 void wait(int value){std::unique_lock<std::mutex> l(m);assert(cv.wait_for(l,3s,[&]{return n>=value;}));}
};
template<class F>void wait_until(F f){for(int i=0;i<3000 && !f();++i)std::this_thread::sleep_for(1ms);assert(f());}
int main(){
 srmodel_list_t models;Observer observer;CallVoiceWorker worker;assert(worker.Initialize());
 auto callback=[&](auto g,auto o){observer.record(g,o);};
 assert(worker.Begin(&models,7,callback));wait_until([&]{return worker.Ready();});
 int16_t audio[1]={42};assert(!worker.Push(6,audio,1));assert(worker.Push(7,audio,1));observer.wait(1);
 assert(observer.generation==7 && observer.outcome==CallVoiceWorker::Outcome::Recognized && !worker.Busy());
 assert(worker.Begin(&models,8,callback));wait_until([&]{return worker.Ready();});
 hold_feed=true;assert(worker.Push(8,audio,1));wait_until([&]{return in_feed.load();});worker.RequestStop();
 assert(worker.Busy());hold_feed=false;observer.wait(2);assert(observer.outcome==CallVoiceWorker::Outcome::Cancelled);
 assert(worker.Begin(&models,9,callback));wait_until([&]{return worker.Ready();});
 assert(!worker.Push(9,audio,1025));observer.wait(3);assert(observer.outcome==CallVoiceWorker::Outcome::Unavailable);
 assert(worker.Begin(&models,10,callback));wait_until([&]{return worker.Ready();});
 hold_feed=true;assert(worker.Push(10,audio,1));wait_until([&]{return in_feed.load();});
 for(int i=0;i<8;++i)assert(worker.Push(10,audio,1));
 assert(!worker.Push(10,audio,1));hold_feed=false;observer.wait(4);
 assert(observer.outcome==CallVoiceWorker::Outcome::Unavailable);
 hold_start=true;assert(worker.Begin(&models,11,callback));wait_until([&]{return in_start.load();});
 worker.RequestStop();assert(worker.Busy());hold_start=false;observer.wait(5);
 assert(observer.outcome==CallVoiceWorker::Outcome::Cancelled && !worker.Ready());
 psram=2*1024*1024;assert(worker.Begin(&models,12,callback));observer.wait(6);
 assert(observer.outcome==CallVoiceWorker::Outcome::Unavailable && released);
 psram=4*1024*1024;after_load=245268;assert(worker.Begin(&models,13,callback));observer.wait(7);
 assert(observer.outcome==CallVoiceWorker::Outcome::Unavailable && released);
 after_load=1024*1024;fail_start=true;assert(worker.Begin(&models,14,callback));observer.wait(8);assert(observer.outcome==CallVoiceWorker::Outcome::Unavailable);
}
'''
            binary = root / 'worker'
            result = subprocess.run(['c++', '-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror',
                                     '-I', str(root), '-I', str(ROOT / 'main/audio/call_voice'),
                                     str(ROOT / 'main/audio/call_voice/call_voice_worker.cc'),
                                     '-x', 'c++', '-', '-o', str(binary)], input=code,
                                    text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(binary)], text=True, capture_output=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
