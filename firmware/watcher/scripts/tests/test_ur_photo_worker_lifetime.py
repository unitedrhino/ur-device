"""编译真实拍照 worker，验证自删任务前已经释放 C++ 局部资源。"""
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class PhotoWorkerLifetimeTest(unittest.TestCase):
    """模拟 RTOS 删除边界，检查成功和各失败出口的资源生命周期。"""

    def test_resources_released_before_task_delete(self):
        """自删任务不会展开 C++ 栈，所有图片请求资源必须提前析构。"""
        source = (ROOT / "main/unitedrhino/ur_bridge.cc").read_text()
        worker = source.split("enum class UrPhotoRequestKind", 1)[1].split("#endif", 1)[0]
        harness = r'''
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <mutex>
#include <new>
#include <string>
#include "ur_photo_runtime.h"
#include "display/photo_ui_state.h"
static PhotoUiStage shown_stage;
static unsigned shown_generation;
class TestDisplay {
public:
    void SetPhotoStage(uint32_t generation, PhotoUiStage stage) {
        shown_generation = generation; shown_stage = stage;
    }
};
static long live_allocations = 0, deletion_allocations = -1;
void* operator new(size_t n) { auto p = std::malloc(n); if (!p) throw std::bad_alloc(); ++live_allocations; return p; }
void operator delete(void* p) noexcept { if(p) { --live_allocations; std::free(p); } }
void operator delete(void* p, size_t) noexcept { ::operator delete(p); }
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define CONFIG_UR_PLATFORM_BASE_URL "http://example.test:7777"
static int mode = 0;
static std::mutex s_photo_mutex;
static UrPhotoGate s_photo_gate;
static void vTaskDeleteWithCaps(void*) { deletion_allocations = live_allocations; }
static bool ur_device_photo_complete(const char*, int, const char*, const char*) { return true; }
static bool ur_device_copy_http_auth(char* u, size_t, char* p, size_t) {
    std::strcpy(u,"test-user"); std::strcpy(p,"test-proof"); return mode != 2;
}
class TestCamera {
public:
    bool Capture() { return mode != 1; }
    std::expected<std::string,std::string> UploadToPlatform(const std::string&, const char*, const char*) {
        if(mode == 3) return std::unexpected("upload failure with non-SSO error text");
        return "https://objects.example.test/camera/captured-image.jpg";
    }
};
class Board {
public:
    static Board& GetInstance() { static Board instance; return instance; }
    TestCamera* GetCamera() { static TestCamera camera; return &camera; }
    TestDisplay* GetDisplay() { static TestDisplay display; return &display; }
};
class Application {
public:
    static Application& GetInstance() { static Application instance; return instance; }
    std::string GetUrAiUploadUrl() { return mode == 4 ? "invalid" : "/api/v1/device/camera/upload"; }
    void CompleteUrPhotoInput(int, const char*) {}
};
'''
        harness += "enum class UrPhotoRequestKind" + worker
        harness += r'''
int main() {
    for(mode=0; mode<5; ++mode) {
        for(auto kind : {UrPhotoRequestKind::kAction, UrPhotoRequestKind::kImageInput}) {
            const auto baseline = live_allocations;
            auto* request = new UrPhotoRequest{kind, "non-SSO-token-owned-by-photo-worker", 42};
            deletion_allocations = -1;
            ur_photo_worker(request);
            assert(deletion_allocations == baseline);
            assert(live_allocations == baseline);
            assert(shown_generation == 42);
            assert(shown_stage == (mode == 0 ? PhotoUiStage::Waiting :
                mode == 1 ? PhotoUiStage::CaptureFailed : mode == 3 ? PhotoUiStage::UploadFailed :
                PhotoUiStage::Unavailable));
        }
    }
}
'''
        with tempfile.TemporaryDirectory() as temp:
            binary = str(Path(temp) / "photo-worker")
            subprocess.run(["g++", "-std=c++23", "-I" + str(ROOT / "main/protocols"),
                            "-I" + str(ROOT / "main"),
                            "-x", "c++", "-", str(ROOT / "main/protocols/ur_photo_runtime.cc"),
                            "-o", binary], input=harness, text=True, check=True, capture_output=True)
            subprocess.run([binary], check=True, capture_output=True)
