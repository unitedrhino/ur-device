"""编译 SDK 分配适配器，验证大缓冲不会默认占用内部 DMA 内存。"""
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class SdkMemoryTest(unittest.TestCase):
    """覆盖分配阈值、PSRAM 成功和不足回退。"""

    def test_large_sdk_buffer_prefers_psram(self):
        """生产适配器分配顺序与请求长度必须符合约束。"""
        source = (ROOT / "main/unitedrhino/ur_sdk_memory.c").read_text().replace(
            "#include <esp_heap_caps.h>", ""
        )
        harness = r'''
#include <cassert>
#include <cstddef>
#include <cstdint>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
static int psram_calls = 0, fallback_calls = 0;
static bool available = true;
static uint32_t requested = 0;
static char external_buffer, internal_buffer;
void* heap_caps_malloc(uint32_t n, int caps) {
    assert(n == requested && caps == 3); ++psram_calls;
    return available ? &external_buffer : nullptr;
}
void* __real_HAL_Malloc(uint32_t n) {
    assert(n == requested); ++fallback_calls; return &internal_buffer;
}
'''
        harness += source
        harness += r'''
int main() {
    requested=16383;
    assert(__wrap_HAL_Malloc(requested) == &internal_buffer);
    assert(psram_calls == 0 && fallback_calls == 1);
    requested=16384;
    assert(__wrap_HAL_Malloc(requested) == &external_buffer);
    assert(psram_calls == 1 && fallback_calls == 1);
    requested=24000; available=false;
    assert(__wrap_HAL_Malloc(requested) == &internal_buffer);
    assert(psram_calls == 2 && fallback_calls == 2);
}
'''
        with tempfile.TemporaryDirectory() as temp:
            binary = str(Path(temp) / "sdk-memory")
            subprocess.run(["g++", "-std=c++17", "-x", "c++", "-", "-o", binary],
                           input=harness, text=True, check=True, capture_output=True)
            subprocess.run([binary], check=True)
        cmake = (ROOT / "main/CMakeLists.txt").read_text()
        self.assertIn("-Wl,--wrap=HAL_Malloc", cmake)
        self.assertIn('"unitedrhino/ur_sdk_memory.c"', cmake)
