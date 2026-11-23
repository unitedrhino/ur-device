"""执行生产信令任务创建片段，检查目标组合、其他路径和失败后的状态回收。"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class SignalingTaskLaneTest(unittest.TestCase):
    """替换FreeRTOS创建边界，不另写生产条件分支或失败恢复。"""

    def test_creation_failure_and_configuration_paths(self):
        source = (ROOT / 'main/signaling/ur_signaling_protocol.cc').read_text()
        start = source.index('    task_running_.store(true);')
        end = source.index('    return true;', start) + len('    return true;')
        actual = source[start:end]
        code = r'''
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#define ESP_LOGE(...) ((void)0)
constexpr int pdPASS=1,MALLOC_CAP_SPIRAM=1,MALLOC_CAP_8BIT=2;
using TaskHandle_t=void*;
// Created保存传入的创建参数；失败不得遗留可被后续关闭误用的任务句柄。
struct Created {int core=-1,priority=0;size_t stack=0;void* context=nullptr;};
static Created created;
static bool reject=false;
int xTaskCreateWithCaps(void(*)(void*),const char*,size_t stack,void* context,int priority,TaskHandle_t* handle,int){
    created={-1,priority,stack,context};
    if(reject)return 0;
    *handle=reinterpret_cast<void*>(1);return pdPASS;
}
int xTaskCreatePinnedToCoreWithCaps(void(*)(void*),const char*,size_t stack,void* context,int priority,TaskHandle_t* handle,int core,int){
    created={core,priority,stack,context};
    if(reject)return 0;
    *handle=reinterpret_cast<void*>(1);return pdPASS;
}
struct Signaling {
    std::atomic<bool> task_running_{false};TaskHandle_t task_=nullptr;
    static void TaskEntry(void*){}
    bool Create(){PRODUCTION}
};
int main(){
    Signaling signaling;
    reject=true;assert(!signaling.Create());
    assert(!signaling.task_running_.load() && signaling.task_==nullptr);
    reject=false;assert(signaling.Create());
    assert(signaling.task_running_.load() && signaling.task_!=nullptr);
    // 生产信令任务当前使用6级优先级，确保媒体突发先于低级编解码任务被排空。
    assert(created.context==&signaling && created.stack==32768 && created.priority==6);
    const int expected=CONFIG_UR_HUMAN_CALL_UDP_AV && CONFIG_UR_SOFTWARE_AEC && CONFIG_FREERTOS_NUMBER_OF_CORES>1 ? 1 : -1;
    assert(created.core==expected && "wrong task creation path");
}
'''.replace('PRODUCTION', actual)
        with tempfile.TemporaryDirectory(prefix='watcher-task-lane-') as directory:
            binary = str(Path(directory) / 'lane')
            for udp, aec, cores in [(1, 1, 2), (0, 1, 2), (1, 0, 2), (1, 1, 1)]:
                with self.subTest(udp=udp, aec=aec, cores=cores):
                    built = subprocess.run([
                        'g++', '-std=c++17', '-Wall', '-Wextra',
                        f'-DCONFIG_UR_HUMAN_CALL_UDP_AV={udp}',
                        f'-DCONFIG_UR_SOFTWARE_AEC={aec}',
                        f'-DCONFIG_FREERTOS_NUMBER_OF_CORES={cores}',
                        '-x', 'c++', '-', '-o', binary,
                    ], input=code, text=True, capture_output=True, timeout=30)
                    self.assertEqual(built.returncode, 0, built.stderr)
                    result = subprocess.run([binary], text=True, capture_output=True, timeout=5)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
