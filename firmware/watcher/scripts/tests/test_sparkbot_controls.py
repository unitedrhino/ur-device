"""运行 IDF6 坦克生产控制时序，覆盖停车、灯效和触摸去抖。"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
RUNTIME = ROOT.parent / "device-runtime/include"


class SparkBotControlsTest(unittest.TestCase):
    """仅替换硬件边界，直接编译生产头文件或从应用中抽取实际回调。"""

    def compile_run(self, source):
        """编译并执行主机夹具，临时产物放任务临时目录。"""
        with tempfile.TemporaryDirectory(dir=os.environ.get("TMPDIR")) as directory:
            binary = str(Path(directory) / "tank-test")
            result = subprocess.run(["g++", "-x", "c++", "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror",
                                     "-I", str(RUNTIME), "-", "-o", binary],
                                    input=source, text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([binary], check=True, capture_output=True)

    def test_motion_light_and_touch(self):
        """低速、运动到期、跳舞抢占/停止/断连和触摸抖动必须保持确定性。"""
        self.compile_run(r'''
#include "ur/device/sparkbot_controls.hpp"
#include <cassert>
#include <cmath>
using namespace ur::device;
int main() {
    for (int mode=1; mode<=6; ++mode) assert(SparkBotLightCommand(mode) == '0' + mode + 2);
    assert(SparkBotLightCommand(0)=='8' && SparkBotLightCommand(7)==0);
    SparkBotMotion motion;
    motion.Start(0, 1, 1000, 3000);
    assert(std::abs(motion.Current(1100,1).y - 0.01f) < 0.0001f);
    assert(motion.Current(3999,100).y == 1);
    assert(motion.Current(4000,100).y == 0);
    motion.Dance(5000);
    assert(motion.Current(5000,50).y == 0.5f);
    assert(motion.Current(6000,100).x == 0.8f);
    motion.Stop();  // tankStop及断连均调用同一Stop；旧定时器不得恢复舞步。
    for (int now=6100; now<50000; now+=50) {
        assert(motion.Current(now,100).x == 0 && motion.Current(now,100).y == 0);
    }
    motion.Dance(50000);
    motion.Start(-1,0,50500,3000);
    assert(motion.Current(51500,20).x == -0.2f);
    assert(motion.Current(53500,100).x == 0);
    motion.Dance(60000);
    assert(motion.Current(80000,100).x == 0 && motion.Current(80000,100).y == 0);
    SparkBotTouchDebounce touch;
    assert(!touch.Update(true,100000));
    assert(!touch.Update(false,120000));
    assert(!touch.Update(true,140000));
    assert(touch.Update(true,200000));
    assert(!touch.Update(true,2000000));  // 长按只触发一次。
    assert(!touch.Update(false,2100000));
    assert(!touch.Update(false,2160000));
    assert(!touch.Update(true,2200000));
    assert(touch.Update(true,2260000));
}
''')

    def test_uart_commands_have_one_writer_and_stop_priority(self):
        """运行板级生产发送方法；灯光和运动分周期发送，停止优先且不会恢复舞步。"""
        source = (ROOT / "main/boards/espressif/esp-sparkbot/esp_sparkbot_board.cc").read_text()
        begin = source.index("    void UrWriteMotionLocked()")
        end = source.index("    // UrMotionStopCallback", begin)
        methods = source[begin:end]
        self.compile_run(r'''
#include "ur/device/sparkbot_controls.hpp"
#include <vector>
#include <string>
#include <mutex>
#include <cassert>
#include <cstdio>
#include <map>
struct Settings {
    inline static std::map<std::string,int> stored;
    inline static int writes=0;
    Settings(const std::string&, bool) {}
    int GetInt(const std::string& key,int fallback) { auto it=stored.find(key); return it==stored.end()?fallback:it->second; }
    void SetInt(const std::string& key,int value) { stored[key]=value; ++writes; }
};
using light_mode_t=int;
constexpr int LIGHT_MODE_SLEEP=8;
long long now_us=0;
long long esp_timer_get_time() { return now_us; }
struct Board {
    ur::device::SparkBotMotion ur_motion_;
    std::mutex ur_motion_mutex_;
    int ur_speed_=100, ur_pending_light_=3, light_mode_=3, ur_light_mode_=1;
    bool ur_motion_priority_=true;
    std::vector<std::string> sent;
    void SendUartMessage(const char* text) { sent.emplace_back(text); }
METHODS
};
int main() {
    Board b;
    b.UrStartDance();
    b.UrSetLight(6);
    assert(b.sent.empty());
    b.UrWriteMotionLocked();
    assert(b.sent.back()=="x0.00 y1.00");
    now_us=50000; b.UrWriteMotionLocked();
    assert(b.sent.back()=="w8");
    b.UrSetLight(2);
    b.UrStopChassis();
    now_us=100000; b.UrWriteMotionLocked();
    assert(b.sent.back()=="x0.00 y0.00");
    now_us=150000; b.UrWriteMotionLocked();
    assert(b.sent.back()=="w4");
    now_us=10000000; b.UrWriteMotionLocked();
    assert(b.sent.back()=="x0.00 y0.00");
    b.UrSetSpeed(37); b.UrSetLight(0);
    const int saved_writes=Settings::writes;
    b.UrSetSpeed(37); b.UrSetLight(0);
    assert(Settings::writes==saved_writes);
    Board restarted; restarted.RestoreChassisSettings();
    assert(restarted.ur_speed_==37 && restarted.ur_light_mode_==0 && restarted.ur_pending_light_==8);
    Settings::stored["speed"]=-5; Settings::stored["light-mode"]=99;
    restarted.RestoreChassisSettings();
    assert(restarted.ur_speed_==100 && restarted.ur_light_mode_==1);

}
'''.replace("METHODS", methods))

    def test_board_uses_shared_paths_and_preserves_other_defaults(self):
        """坦克使用同一灯效/舞步路径，公共音量不迁移用户设置。"""
        for relative in ("watcher/main/boards/espressif/esp-sparkbot",):
            source = (ROOT.parent / relative / "esp_sparkbot_board.cc").read_text()
            self.assertNotIn('SendUartMessage("d1")', source)
            self.assertIn("SparkBotLightCommand(mode)", source)
            self.assertGreaterEqual(source.count("UrSetLight(mode)"), 2)
            self.assertIn("esp_timer_start_periodic(ur_motion_timer_, 50000)", source)
            self.assertIn("output_volume_ = 20", source)
        common = (ROOT / "main/audio/audio_codec.cc").read_text()
        self.assertNotIn("vol20mig", common)
        self.assertIn("int output_volume_ = 70", (ROOT / "main/audio/audio_codec.h").read_text())

    def test_properties_snapshot_is_consistent_during_control(self):
        """并发改变成对设置时，实际属性上报不能读到跨次控制的混合值。"""
        source = (ROOT / "main/boards/espressif/esp-sparkbot/esp_sparkbot_board.cc").read_text()
        begin = source.index("    std::string GetUrPropertiesJson() override {")
        end = source.index("\n    /* 联犀 MQTT 断连", begin)
        self.compile_run(r'''
// 仅替换电池、网络和音量边界，实际属性上报方法保持不变。
#include <atomic>
#include <cassert>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
struct WifiManager {
 static WifiManager& GetInstance() { static WifiManager value;return value; }
 bool IsConnected() { return true; }
 int GetRssi() { return -50; }
};
struct Codec { int output_volume() { return 80; } };
struct Board {
 static Board& GetInstance() { static Board value;return value; }
 Codec* GetAudioCodec() { static Codec codec;return &codec; }
};
struct Properties { virtual std::string GetUrPropertiesJson()=0; };
struct ReportBoard : Properties {
 std::mutex ur_motion_mutex_;
 int ur_light_mode_=1, ur_speed_=10;
 void GetBatteryLevel(int& battery,bool& charging,bool& discharging) {
  battery=0;charging=false;discharging=false;
 }
METHOD
};
int main() {
 ReportBoard board;
 std::atomic<bool> ready{false}, stop{false};
 std::thread writer([&]() {
  ready.store(true);
  while(!stop.load()) {
   for(int mode=1;mode<=2;++mode) {
    std::lock_guard<std::mutex> lock(board.ur_motion_mutex_);
    board.ur_light_mode_=mode;
    std::this_thread::yield();
    board.ur_speed_=mode*10;
   }
  }
 });
 while(!ready.load()) std::this_thread::yield();
 for(int i=0;i<10000;++i) {
  const auto json=board.GetUrPropertiesJson();
  int mode=0,speed=0;
  assert(std::sscanf(json.c_str()+json.find("\"LightMode\""),"\"LightMode\":%d,\"Speed\":%d",&mode,&speed)==2);
  assert(speed==mode*10);
 }
 stop.store(true);writer.join();
}
'''.replace("METHOD", source[begin:end]))

    def test_report_info_matches_selected_hardware(self):
        """执行实际基础信息组包，坦克型号与其他板型分别保持正确且不破坏版本字段。"""
        source = (ROOT / "main/unitedrhino/ur_device.c").read_text()
        begin = source.index("static void ur_report_device_info(void)")
        end = source.index("/* ur_json_string", begin)
        for tank in (0, 1):
            self.compile_run(r'''
#include <string>
#include <cstdio>
#include <cassert>
#define CONFIG_BOARD_TYPE_ESP_SPARKBOT TANK
#define HAL_Snprintf snprintf
struct esp_app_desc_t { const char* version; };
esp_app_desc_t desc={"test-version"};
const esp_app_desc_t* esp_app_get_description() { return &desc; }
char s_report_info_token[64], s_device_name[]="test-device";
void* s_mqtt_client=nullptr;
std::string sent;
void ur_make_token(char* p, size_t n, const char* prefix) { snprintf(p,n,"%s-1",prefix); }
int ur_bridge_get_rssi() { return -40; }
int ur_publish_topic(void*, const char*, const char* message, int length) {
    sent.assign(message,length); return 0;
}
FUNCTION
int main() {
    ur_report_device_info();
    assert(sent.find("\"version\":\"test-version\"")!=std::string::npos);
#if CONFIG_BOARD_TYPE_ESP_SPARKBOT
    assert(sent.find("ESP-SparkBot Tank")!=std::string::npos);
    assert(sent.find("SenseCAP Watcher")==std::string::npos);
#else
    assert(sent.find("SenseCAP Watcher")!=std::string::npos);
#endif
}
'''.replace("TANK", str(tank)).replace("FUNCTION", source[begin:end]))


if __name__ == "__main__":
    unittest.main()
