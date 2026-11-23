"""编译真实相机DMA包装，校验任意流长度、内存边界、原IO错误和生命周期。"""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class CameraDmaIoTest(unittest.TestCase):
    """接收端刻意使用普通堆，底层只接受预留DMA指针，重现PSRAM直入SPI的不安全边界。"""

    def test_actual_stream_uses_reserved_aligned_dma_without_read_ahead(self):
        header = (ROOT / "main/boards/sensecap-watcher/camera_dma_io.h").read_text()
        body = header[header.index("class CameraDmaIo final"):]
        source = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>
using esp_err_t=int;
constexpr int ESP_OK=0,ESP_ERR_NO_MEM=-1,ESP_ERR_INVALID_ARG=-2;
constexpr int MALLOC_CAP_INTERNAL=1,MALLOC_CAP_DMA=2,MALLOC_CAP_8BIT=4;
bool fail_allocation=false; int allocations=0,frees=0,calls=0,fail_at=0,deletes=0;
uint8_t* reserved=nullptr; size_t consumed=0,total=0;
void* heap_caps_aligned_alloc(size_t align,size_t size,int caps) {
    assert(align==16 && size==4096 && caps==7); ++allocations;
    if(fail_allocation) return nullptr;
    reserved=static_cast<uint8_t*>(std::aligned_alloc(align,size));return reserved;
}
void heap_caps_free(void* p) {if(p){++frees;free(p);}}
struct sscma_client_io_t {
    void* handle=nullptr;
    int (*del)(sscma_client_io_t*)=nullptr;
    int (*write)(sscma_client_io_t*,const void*,size_t)=nullptr;
    int (*flush)(sscma_client_io_t*)=nullptr;
    int (*read)(sscma_client_io_t*,void*,size_t)=nullptr;
    int (*available)(sscma_client_io_t*,size_t*)=nullptr;
};
''' + body + r'''
int main() {
    sscma_client_io_t inner;inner.handle=&inner;
    inner.del=[](auto*){++deletes;return 13;};
    inner.flush=[](auto*){return 11;};
    inner.write=[](auto*,const void* p,size_t n){assert(n==4 && !memcmp(p,"test",4));return 12;};
    inner.available=[](auto*,size_t* size){*size=total-consumed;return ESP_OK;};
    inner.read=[](auto*,void* data,size_t size) {
        ++calls;if(calls==fail_at)return -17;
        assert(data==reserved && reinterpret_cast<uintptr_t>(data)%16==0);
        assert(size>0 && size<=4092 && (size%4==0 || size<=3));
        assert(consumed+size<=total);
        for(size_t i=0;i<size;++i) static_cast<uint8_t*>(data)[i]=(consumed+i)%251;
        consumed+=size;return ESP_OK;
    };
    {
        CameraDmaIo io(&inner);
        assert(io.handle==inner.handle && allocations==1);
        for(const size_t length:{1,2,3,4,4092,4095,4114,8190,65536}) {
            consumed=calls=0;total=length;
            std::vector<uint8_t> output(length+2,0xEE);
            assert(io.read(&io,output.data()+1,length)==ESP_OK);
            assert(consumed==length && allocations==1);
            assert(output.front()==0xEE && output.back()==0xEE);
            for(size_t i=0;i<length;++i)assert(output[i+1]==i%251);
            size_t available=1;assert(io.available(&io,&available)==0 && available==0);
        }
        std::vector<uint8_t> output(5000,0xEE);
        consumed=calls=0;total=5000;fail_at=2;
        assert(io.read(&io,output.data(),output.size())==-17 && consumed==4092);
        for(size_t i=4092;i<output.size();++i)assert(output[i]==0xEE);
        assert(io.read(&io,nullptr,0)==0 && io.read(&io,nullptr,1)==ESP_ERR_INVALID_ARG);
        assert(io.flush(&io)==11 && io.write(&io,"test",4)==12 && io.del(&io)==13);
        assert(deletes==1 && frees==0);
    }
    assert(frees==1);
    fail_allocation=true;calls=0;
    {CameraDmaIo io(&inner);uint8_t output;assert(io.read(&io,&output,1)==ESP_ERR_NO_MEM && calls==0);}
    assert(frees==1 && allocations==2);
}
'''
        compiler = shutil.which("g++") or shutil.which("clang++")
        self.assertIsNotNone(compiler)
        output = ROOT.parents[1] / ".temp"
        output.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=output) as directory:
            binary = Path(directory) / "dma-io"
            compiled = subprocess.run([
                compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=undefined,bounds", "-x", "c++", "-", "-o", str(binary),
            ], input=source, text=True, capture_output=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            completed = subprocess.run([str(binary)], capture_output=True)
            self.assertEqual(completed.returncode, 0, completed.stderr.decode(errors="replace"))
