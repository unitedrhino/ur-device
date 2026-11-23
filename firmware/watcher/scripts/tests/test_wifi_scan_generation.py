"""测试 Wi-Fi 构建适配的锚点保护与真实依赖输入，不写 managed_components。"""
import importlib.util
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("wifi_scan_generation", ROOT / "scripts/prepare_wifi_scan_order.py")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class WifiScanGenerationTest(unittest.TestCase):
    """生成器失败必须阻止构建，避免上游升级后静默丢失最近网络策略。"""

    def test_real_scan_does_not_hide_new_network(self):
        """执行实际生成的 StartScan：多个网络跨信道时不能只扫描旧缓存信道。"""
        vendor = ROOT / "managed_components/78__esp-wifi-connect/wifi_station.cc"
        if not vendor.exists():
            self.skipTest("需要已安装依赖进行生产扫描回放")
        generated = MODULE.transform(vendor.read_text())
        start = generated.index("void WifiStation::StartScan() {")
        method = generated[start:generated.index("\nvoid WifiStation::HandleScanResult", start)]
        code = r'''
// SDK 替身只记录扫描范围；扫描策略与排序直接编译生产实现。
#include <cassert>
#include <vector>
#include "ur_wifi_scan_order.h"
static uint8_t saved_count=2;
extern "C" uint8_t ur_network_copy_wifi_order(char ssids[10][33]) {
    std::strcpy(ssids[0],"new-network"); std::strcpy(ssids[1],"old-network");
    return saved_count;
}
struct SsidManager {
    static SsidManager& GetInstance() { static SsidManager manager; return manager; }
    std::vector<uint8_t> GetSavedChannels() { return {6}; }
};
using esp_err_t=int;
constexpr int ESP_OK=0;
#define ESP_IDF_VERSION 6
#define ESP_IDF_VERSION_VAL(a,b,c) 1
#define CHANNEL_TO_BIT_NUMBER(channel) (channel)
#define CHANNEL_TO_BIT(channel) (1U << (channel))
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
struct wifi_scan_config_t {
    uint8_t channel=0;
    struct { uint32_t ghz_2_channels=0, ghz_5_channels=0; } channel_bitmap;
};
static bool full_scan=false;
int esp_wifi_scan_start(wifi_scan_config_t* config,bool) { full_scan=config==nullptr; return 0; }
struct WifiStation {
    bool use_saved_channels_scan_=true, last_scan_used_saved_channels_=false;
    void StartScan();
};
// METHOD
int main() {
    for (int repeat=0;repeat<10;++repeat) {
        saved_count=2;
        WifiStation multiple; multiple.StartScan();
        assert(full_scan && !multiple.last_scan_used_saved_channels_);
        // 完整扫描才会同时发现新热点；后续必须优先较弱的新网络。
        struct AP { char ssid[33]; int rssi; } aps[]={{"old-network",-29},{"new-network",-65}};
        watcher::PrioritizeWifiScan(aps,2);
        assert(std::strcmp(aps[0].ssid,"new-network")==0);
        saved_count=1;
        WifiStation single; single.StartScan();
        assert(!full_scan && single.last_scan_used_saved_channels_);
        single.use_saved_channels_scan_=false; single.StartScan();
        assert(full_scan);
    }
}
'''.replace("// METHOD", method)
        compiler = shutil.which("g++") or shutil.which("clang++")
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / "scan-test")
            subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            "-I" + str(ROOT / "main/unitedrhino"), "-x", "c++", "-", "-o", binary],
                           input=code, text=True, capture_output=True, check=True)
            subprocess.run([binary], capture_output=True, check=True, timeout=10)

    def test_cmake_replaces_only_station_source(self):
        """运行真实 CMake 适配，确认原文件不再参与编译而其他源保留；不代替 IDF 构建。"""
        cmake = shutil.which("cmake")
        self.assertIsNotNone(cmake, "需要 CMake 验证构建适配")
        with tempfile.TemporaryDirectory(prefix="watcher-scan-cmake-") as directory:
            root = Path(directory)
            (root / "scripts").symlink_to(ROOT / "scripts", target_is_directory=True)
            (root / "main").symlink_to(ROOT / "main", target_is_directory=True)
            vendor = root / "vendor"
            vendor.mkdir()
            original = "// 保留上游头\nvoid WifiStation::StartScan() {\n" + MODULE.SCAN_ANCHOR + "}\n}\nvoid WifiStation::HandleScanResult() {\n" + MODULE.ANCHOR + "}\n"
            (vendor / "wifi_station.cc").write_text(original)
            (vendor / "other.cc").write_text("// 不相关构建源\n")
            (root / "CMakeLists.txt").write_text(f'''
cmake_minimum_required(VERSION 3.16)
project(scan_adapter LANGUAGES CXX)
add_library(wifi STATIC vendor/wifi_station.cc vendor/other.cc)
function(idf_component_get_property output component property)
  if(property STREQUAL "COMPONENT_LIB")
    set(${{output}} wifi PARENT_SCOPE)
  else()
    set(${{output}} "{vendor}" PARENT_SCOPE)
  endif()
endfunction()
function(idf_build_get_property output property)
  set(${{output}} "{sys.executable}" PARENT_SCOPE)
endfunction()
include("{ROOT / 'cmake/watcher_wifi_order.cmake'}")
get_target_property(actual wifi SOURCES)
file(WRITE "${{CMAKE_BINARY_DIR}}/sources.txt" "${{actual}}")
''')
            build = root / "build"
            subprocess.run([cmake, "-S", str(root), "-B", str(build)], check=True, capture_output=True)
            sources = (build / "sources.txt").read_text().split(";")
            self.assertEqual(sources, [str(build / "watcher_wifi/wifi_station.cc"), "vendor/other.cc"])
            self.assertIn(MODULE.INSERTION, (build / "watcher_wifi/wifi_station.cc").read_text())
            self.assertEqual((vendor / "wifi_station.cc").read_text(), original)

    def test_anchor_and_reentry(self):
        """仅适配扫描与排序；重复生成、重复锚点和位置变化均拒绝。"""
        source = "// 上游许可占位，必须保留\nvoid WifiStation::StartScan() {\n" + MODULE.SCAN_ANCHOR + "}\n}\nvoid WifiStation::HandleScanResult() {\n" + MODULE.ANCHOR + "}\n"
        generated = MODULE.transform(source)
        self.assertIn(source.splitlines()[0], generated)
        self.assertEqual(generated.count(MODULE.INSERTION), 1)
        self.assertEqual(generated.replace('#include "ur_wifi_scan_order.h"\n', '').replace(MODULE.INSERTION, '').replace(MODULE.SCAN_REPLACEMENT, MODULE.SCAN_ANCHOR), source)
        for invalid in (generated, source + MODULE.ANCHOR, source.replace(MODULE.ANCHOR, ''),
                        MODULE.ANCHOR + "void WifiStation::HandleScanResult() {}",
                        source.replace(MODULE.SCAN_ANCHOR, ''), source + MODULE.SCAN_ANCHOR):
            with self.assertRaises(ValueError):
                MODULE.transform(invalid)

    def test_installed_dependency(self):
        """已安装依赖时验证真实源插入点，不把伪造源作为当前 SDK 兼容证据。"""
        source_path = ROOT / "managed_components/78__esp-wifi-connect/wifi_station.cc"
        if not source_path.exists():
            self.skipTest("未安装 ESP-IDF 依赖，需构建阶段验证真实输入")
        before = source_path.read_bytes()
        result = MODULE.transform(before.decode())
        self.assertLess(result.index("// sort by rssi descending"), result.index(MODULE.INSERTION))
        self.assertLess(result.index(MODULE.INSERTION), result.index("connect_queue_.push_back(record)"))
        self.assertEqual(source_path.read_bytes(), before)
