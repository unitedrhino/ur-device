"""执行真实联网启动和超时函数，验证 Watcher 离线保留扫描，其他板型仍可自动配网。"""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_ur_network_ble import function

ROOT = Path(__file__).resolve().parents[2]


class WifiRetryTest(unittest.TestCase):
    """仅替换无线和定时器边界，不用主机回放代替真实路由器验收。"""

    def test_saved_wifi_offline_policy_ten_times(self):
        """已保存网络离线不退出 STA；空凭据和普通板型保持既有配网行为。"""
        source = (ROOT / "main/boards/common/wifi_board.cc").read_text()
        methods = "\n".join(function(source, name) for name in (
            "WifiBoard::TryWifiConnect", "WifiBoard::OnWifiConnectTimeout"))
        policy = function((ROOT / "main/boards/common/wifi_board.h").read_text(), "AllowAutomaticWifiConfig")
        watcher_policy = function((ROOT / "main/boards/sensecap-watcher/sensecap_watcher.cc").read_text(),
                                  "AllowAutomaticWifiConfig")
        connected_hook = function((ROOT / "main/boards/sensecap-watcher/sensecap_watcher.cc").read_text(),
                                  "OnNetworkEvent")
        code = r'''
// 无线与事件计数替身：真实生产分支必须决定是否启动热点或停止扫描。
#include <cassert>
#include <vector>
#include <string>
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define pdMS_TO_TICKS(x) (x)
static int timers=0, starts=0, stops=0, portals=0;
static int remembered=0, forwarded=0;
enum class NetworkEvent { Connected, Connecting, Disconnected };
bool ur_network_remember_connection(const char* ssid) {
 assert(std::string(ssid)=="known"); ++remembered; return true;
}
static constexpr int CONNECT_TIMEOUT_SEC=60;
void esp_timer_start_once(int,int) { ++timers; }
void vTaskDelay(int) {}
struct SsidManager {
 std::vector<int> entries;
 static SsidManager& GetInstance() { static SsidManager manager; return manager; }
 const auto& GetSsidList() const { return entries; }
};
struct WifiManager {
 static WifiManager& GetInstance() { static WifiManager manager; return manager; }
 void StartStation() { ++starts; }
 void StopStation() { ++stops; }
};
struct WifiBoard {
 int connect_timer_=0;
 // BASE_POLICY
 void StartWifiConfigMode() { ++portals; }
 virtual void OnNetworkEvent(NetworkEvent, const std::string& = "") { ++forwarded; }
 void TryWifiConnect();
 static void OnWifiConnectTimeout(void*);
};
struct Watcher : WifiBoard {
 // WATCHER_POLICY
 // CONNECTED_HOOK
};
// METHODS
int main() {
 Watcher watcher;
 watcher.OnNetworkEvent(NetworkEvent::Connecting, "known");
 watcher.OnNetworkEvent(NetworkEvent::Disconnected, "known");
 watcher.OnNetworkEvent(NetworkEvent::Connected, "");
 assert(remembered==0 && forwarded==3);
 watcher.OnNetworkEvent(NetworkEvent::Connected, "known");
 assert(remembered==1 && forwarded==4);
 SsidManager::GetInstance().entries={1,2};
 watcher.TryWifiConnect();
 assert(starts==1 && timers==0 && stops==0 && portals==0);
 // 已排队的超时回调也不能停止 Watcher 的重连。
 WifiBoard::OnWifiConnectTimeout(&watcher);
 assert(stops==0 && portals==0);
 WifiBoard other;
 other.TryWifiConnect();
 assert(starts==2 && timers==1);
 WifiBoard::OnWifiConnectTimeout(&other);
 assert(stops==1 && portals==1);
 SsidManager::GetInstance().entries.clear();
 watcher.TryWifiConnect();
 assert(starts==2 && portals==2);
}
'''.replace("// METHODS", methods).replace("// BASE_POLICY", policy).replace("// WATCHER_POLICY", watcher_policy).replace("// CONNECTED_HOOK", connected_hook)
        with tempfile.TemporaryDirectory(prefix="watcher-wifi-retry-") as directory:
            source_path = Path(directory) / "test.cc"
            binary = Path(directory) / "test"
            source_path.write_text(code)
            subprocess.run([shutil.which("g++") or "g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            str(source_path), "-o", str(binary)], check=True, capture_output=True)
            for _ in range(10):
                subprocess.run([str(binary)], check=True, capture_output=True)
