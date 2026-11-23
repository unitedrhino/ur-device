"""执行实际相机 IO 包装，验证计时不改变传输结果、所有权与停流后的记录。"""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class CameraTransportTraceTest(unittest.TestCase):
    """透明转发、分配失败和定长诊断环均不得破坏驱动行为。"""

    def test_actual_io_trace_preserves_driver_and_bounds_diagnostics(self):
        header = (ROOT / "main/boards/sensecap-watcher/camera_transport_trace.h").read_text()
        body = header[header.index("// CameraTransportTrace 借用"):]
        code = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <vector>
using esp_err_t=int;
constexpr int ESP_OK=0,MALLOC_CAP_SPIRAM=1,MALLOC_CAP_8BIT=2;
int allocations=0,frees=0;
bool allocation_failed=false;
void* heap_caps_calloc(size_t count,size_t size,int) {
    ++allocations;return allocation_failed?nullptr:calloc(count,size);
}
void heap_caps_free(void* p) { if(p) ++frees;free(p); }
int64_t now=1000;
int64_t esp_timer_get_time() { return ++now; }
struct sscma_client_io_t {
    void* handle;
    esp_err_t (*del)(sscma_client_io_t*);
    esp_err_t (*write)(sscma_client_io_t*,const void*,size_t);
    esp_err_t (*read)(sscma_client_io_t*,void*,size_t);
    esp_err_t (*available)(sscma_client_io_t*,size_t*);
    esp_err_t (*flush)(sscma_client_io_t*);
};
// BODY
int main() {
    std::atomic<bool> streaming{true};
    int deletes=0;
    sscma_client_io_t inner{};inner.handle=&deletes;
    inner.del=[](auto* io){++*static_cast<int*>(io->handle);return 7;};
    inner.flush=[](auto*){return 6;};
    inner.write=[](auto*,const void* p,size_t n){assert(n==4 && !memcmp(p,"test",4));return 5;};
    inner.read=[](auto*,void* p,size_t n){memset(p,0xA5,n);now+=40000;return -9;};
    inner.available=[](auto*,size_t* n){*n=5000;now+=20000;return ESP_OK;};
    {
        CameraTransportTrace trace(&inner,&streaming);size_t size=0;
        assert(trace.handle==inner.handle);
        assert(trace.available(&trace,&size)==ESP_OK && size==5000);
        trace.Print(false);assert(allocations==0);
        allocation_failed=true;trace.Print(true);assert(allocations==1);
        trace.MarkFrame(now);trace.Print(false);
        allocation_failed=false;trace.Print(true);assert(allocations==2);
        char data[4]{};
        assert(trace.read(&trace,data,sizeof(data))==-9 && (uint8_t)data[0]==0xA5);
        assert(trace.write(&trace,"test",4)==5 && trace.flush(&trace)==6);
        for (int i=0;i<200;++i) trace.MarkFrame(100000+i);
        trace.Print(false);
        streaming=false;
        trace.Print(true);
        assert(trace.available(&trace,&size)==ESP_OK && size==5000);
        trace.MarkFrame(now);trace.Print(false);assert(allocations==2);
        // 包装的销毁不销毁借用的总线；显式 del 的原返回码也必须保留。
        assert(deletes==0 && trace.del(&trace)==7 && deletes==1);
    }
    assert(deletes==1 && frees==1);
}
'''.replace("// BODY", body)
        compiler = shutil.which("g++") or shutil.which("clang++")
        self.assertIsNotNone(compiler)
        output = ROOT.parents[1] / ".temp"
        output.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=output) as directory:
            binary = str(Path(directory) / "trace")
            result = subprocess.run([compiler,"-std=c++17","-Wall","-Wextra","-Werror",
                                     "-fsanitize=undefined","-x","c++","-","-o",binary],
                                    input=code,text=True,capture_output=True)
            self.assertEqual(result.returncode,0,result.stderr)
            result = subprocess.run([binary],capture_output=True,text=True,timeout=10)
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertIn("BEGIN count=128 enabled=1 reset=0",result.stdout)
            self.assertIn("BEGIN count=0 enabled=0 reset=1",result.stdout)
            self.assertIn("F,100072,100072,0,0",result.stdout)
            self.assertNotIn("F,100071,100071,0,0",result.stdout)
            self.assertTrue(result.stdout.rstrip().endswith("[UR_TRANSPORT_PERF] END"))
