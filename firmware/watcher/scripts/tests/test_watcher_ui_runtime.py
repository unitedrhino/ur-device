"""运行真实亮度桥接与息屏判断，验证菜单状态不会脱离平台上报。"""
import subprocess
import re
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class WatcherUiRuntimeTest(unittest.TestCase):
    """用最小硬件替身执行生产函数，覆盖背光可选与拍照活动边界。"""

    def test_restored_network_success_does_not_block_chat(self):
        """执行真实页面刷新：旧成功记录不占据主页，新事务成功和异常仍显示。"""
        source = (ROOT / "main/boards/sensecap-watcher/sensecap_watcher.cc").read_text()
        start = source.index("        void UpdateNetworkPage(")
        end = source.index("\n        // DrainUi", start)
        harness = r'''
#include <cassert>
#include <cstring>
#include <functional>
#include <string>
struct ur_network_status_t { int stage=0, error=0; bool blocked=false; char request_id[64]{}; };
namespace watcher { enum class MenuAction { Visibility }; enum class MenuPage { Home, Result }; }
unsigned lv_tick_get() { return 1; }
struct Model {
    int shown=0; watcher::MenuPage page=watcher::MenuPage::Home;
    void ShowNetworkStatus(int, bool, unsigned) { ++shown; page=watcher::MenuPage::Result; }
    watcher::MenuPage Page() { return page; }
};
struct Display {
    ur_network_status_t shown_network_{};
    std::string restored_network_request_="previous-success";
    Model menu_model_;
    std::function<void(watcher::MenuAction,int)> menu_action_;
    int renders=0;
    void RenderMenu() { ++renders; }
METHOD
};
int main() {
    Display display;
    int blocked=0;
    display.menu_action_=[&](watcher::MenuAction,int value) { blocked=value; };
    ur_network_status_t status{}; status.stage=7;
    std::strcpy(status.request_id,"previous-success");
    display.UpdateNetworkPage(status);
    assert(display.menu_model_.shown==0 && blocked==0 && display.renders==0);
    display.UpdateNetworkPage(status);
    assert(display.menu_model_.shown==0);
    std::strcpy(status.request_id,"new-request"); status.stage=1;
    display.UpdateNetworkPage(status); assert(display.menu_model_.shown==1 && blocked==1);
    status.stage=7; display.UpdateNetworkPage(status); assert(display.menu_model_.shown==2);
    display.UpdateNetworkPage(status); assert(display.menu_model_.shown==2);
    Display error; status.blocked=true; std::strcpy(status.request_id,"previous-success");
    error.UpdateNetworkPage(status); assert(error.menu_model_.shown==1);
    Display recovery; status.blocked=false; status.stage=8;
    recovery.UpdateNetworkPage(status); assert(recovery.menu_model_.shown==1);
    Display live; live.restored_network_request_.clear(); status.stage=7;
    live.UpdateNetworkPage(status); assert(live.menu_model_.shown==1);
}
'''.replace("METHOD", source[start:end])
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / "restored-network-ui")
            subprocess.run(["g++", "-std=c++17", "-x", "c++", "-", "-o", binary],
                           input=harness, text=True, capture_output=True, check=True)
            for _ in range(10):
                subprocess.run([binary], check=True, capture_output=True)

    def test_network_startup_snapshot_precedes_refresh(self):
        """真实启动与状态栏方法保证初始化前不渲染旧结果，独占换网不吞掉成功页。"""
        source = (ROOT / "main/boards/sensecap-watcher/sensecap_watcher.cc").read_text()
        start = source.index("        void SetNetworkStartup(")
        end = source.index("        // PostInput", start)
        harness = r'''
#include <cassert>
#include <cstring>
#include <mutex>
#include <string>
struct ur_network_status_t { int stage=7; bool blocked=false; char request_id[64]="saved"; } current;
void ur_network_status(ur_network_status_t* result) { *result=current; }
struct Display {
    std::mutex ui_mutex_;
    bool network_startup_known_=false;
    struct Pending {
        ur_network_status_t network;
        bool network_dirty=false, status_bar_dirty=false, update_all=false;
        std::string restored_network_request;
    } pending_ui_;
METHODS
};
int main() {
    Display restored;
    restored.UpdateStatusBar(); assert(!restored.pending_ui_.network_dirty);
    restored.SetNetworkStartup(true);
    assert(restored.pending_ui_.network_dirty && restored.pending_ui_.restored_network_request=="saved");
    restored.UpdateStatusBar(true);
    assert(restored.pending_ui_.restored_network_request=="saved" && restored.pending_ui_.update_all);
    Display active; active.SetNetworkStartup(false);
    assert(active.pending_ui_.network_dirty && active.pending_ui_.restored_network_request.empty());
    current.blocked=true;
    Display blocked; blocked.SetNetworkStartup(true);
    assert(blocked.pending_ui_.network_dirty && blocked.pending_ui_.restored_network_request.empty());
    current.blocked=false; current.stage=8;
    Display failure; failure.SetNetworkStartup(true);
    assert(failure.pending_ui_.network_dirty && failure.pending_ui_.restored_network_request.empty());
}
'''.replace("METHODS", source[start:end].replace(" override", ""))
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / "network-startup")
            subprocess.run(["g++", "-std=c++17", "-pthread", "-x", "c++", "-", "-o", binary],
                           input=harness, text=True, capture_output=True, check=True)
            for _ in range(10):
                subprocess.run([binary], check=True, capture_output=True)

    def test_snapshot_console_stack_only_in_test_firmware(self):
        """执行真实控制台配置片段：测试截图有渲染栈预算，正式固件不额外预留内存。"""
        source = (ROOT / "main/boards/sensecap-watcher/sensecap_watcher.cc").read_text()
        start = source.index("        esp_console_repl_config_t repl_config =")
        end = source.index("        repl_config.max_cmdline_length", start)
        harness = r'''
#include <cassert>
struct esp_console_repl_config_t { int task_stack_size; };
#define ESP_CONSOLE_REPL_CONFIG_DEFAULT() esp_console_repl_config_t{4096}
int main() {
CONFIGURATION
    assert(repl_config.task_stack_size == (CONFIG_UR_AI_E2E_TEST ? 12 * 1024 : 4096));
}
'''.replace("CONFIGURATION", source[start:end])
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / "console-stack")
            for enabled in (0, 1):
                subprocess.run(["g++", "-std=c++17", f"-DCONFIG_UR_AI_E2E_TEST={enabled}",
                                "-x", "c++", "-", "-o", binary], input=harness,
                               text=True, capture_output=True, check=True)
                subprocess.run([binary], check=True, capture_output=True)

    def test_home_layout_regions_and_knob_routing(self):
        """检查实际布局参数的区域交集，并防止对话字幕再截走旋钮事件；不替代实屏截图。"""
        source = (ROOT / "main/boards/sensecap-watcher/sensecap_watcher.cc").read_text()
        status_y = int(re.search(r"lv_obj_align\(status_bar_, LV_ALIGN_TOP_MID, 0, (\d+)\)", source)[1])
        status_height = int(re.findall(r"lv_obj_set_size\(status_bar_, 260, (\d+)\)", source)[-1])
        subtitle_y = int(re.search(r"lv_obj_align\(bottom_bar_, LV_ALIGN_TOP_MID, 0, (\d+)\)", source)[1])
        subtitle_height = int(re.search(r"lv_obj_set_size\(bottom_bar_, 288, (\d+)\)", source)[1])
        button_height = int(re.search(r"lv_obj_set_size\(home_menu_, 96, (\d+)\)", source)[1])
        button_offset = int(re.search(r"lv_obj_align\(home_menu_, LV_ALIGN_BOTTOM_MID, 0, (-?\d+)\)", source)[1])
        self.assertLessEqual(subtitle_y + subtitle_height + 4, status_y)
        self.assertLessEqual(status_y + status_height + 4, 412 + button_offset - button_height)
        poll = source.split("void PollMenu()", 1)[1].split("void SetupMenu()", 1)[0]
        self.assertNotIn("lv_obj_scroll_by", poll)
        self.assertIn("menu_model_.Handle(event.input", poll)

    def test_home_text_slots_fit_actual_watcher_font_height(self):
        """Watcher 的内置 30px 字体行高实际为 43px；文字槽必须容纳行高而非名义字号。"""
        source = (ROOT / "main/boards/sensecap-watcher/sensecap_watcher.cc").read_text()
        status_height = int(re.findall(r"lv_obj_set_size\(status_bar_, 260, (\d+)\)", source)[-1])
        menu_height = int(re.search(r"lv_obj_set_size\(home_menu_, 96, (\d+)\)", source)[1])
        for name, height in (("状态提示", status_height), ("主页菜单", menu_height)):
            self.assertGreaterEqual(height, 43 + 4, name + "应容纳实际行高及上下留白")

    def test_saved_network_touch_gate(self):
        """执行触摸入口真实门禁表达式，成功页可触摸但活动事务、故障和 OTA 不放行。"""
        source = (ROOT / "main/boards/sensecap-watcher/sensecap_watcher.cc").read_text()
        gate = source.split("display_->AttachTouch(touch_", 1)[1].split("bool busy = ", 1)[1].split(";", 1)[0]
        harness = r'''
#include <atomic>
#include <cassert>
struct State { int stage = 0; bool blocked = false; } network;
struct Ota { int state = 0; } ota;
std::atomic<bool> network_flow_{true};
bool busy() { return EXPRESSION; }
int main() {
    for (int stage = 1; stage <= 6; ++stage) { network.stage = stage; assert(busy()); }
    network.stage = 7; assert(!busy());
    network.blocked = true; assert(busy()); network.blocked = false;
    ota.state = 1; assert(busy()); ota.state = 2; assert(busy());
    ota.state = 0; assert(!busy());
}
'''.replace("EXPRESSION", gate)
        with tempfile.TemporaryDirectory() as temporary:
            binary = str(Path(temporary) / "network-touch")
            subprocess.run(["g++", "-std=c++17", "-x", "c++", "-", "-o", binary],
                           input=harness, text=True, capture_output=True, check=True)
            for _ in range(10):
                subprocess.run([binary], check=True, capture_output=True)

    def test_serial_input_uses_board_dispatch(self):
        """运行真实测试命令映射，禁止直接触发聊天或将未知命令变为危险输入。"""
        source = (ROOT / "main/boards/sensecap-watcher/sensecap_watcher.cc").read_text()
        begin = source.index("    bool DispatchTestInput(")
        end = source.index("\n#endif", begin)
        harness = r'''
#include <cassert>
#include <cstring>
#include "ui/menu_model.h"
struct Watcher {
    int count = 0;
    watcher::MenuInput last = watcher::MenuInput::Tick;
    void DispatchInput(watcher::MenuInput input) { ++count; last = input; }
METHOD
};
int main() {
    Watcher board;
    using watcher::MenuInput;
    const char* names[] = {"press", "double-press", "long-press", "next", "previous"};
    const MenuInput inputs[] = {MenuInput::Click, MenuInput::DoubleClick,
        MenuInput::LongPress, MenuInput::Next, MenuInput::Previous};
    for (int i = 0; i < 5; ++i) {
        assert(board.DispatchTestInput(names[i]));
        assert(board.count == i + 1 && board.last == inputs[i]);
    }
    assert(!board.DispatchTestInput("reset"));
    assert(!board.DispatchTestInput(""));
    assert(!board.DispatchTestInput(nullptr));
    assert(board.count == 5);
}
'''.replace("METHOD", source[begin:end])
        with tempfile.TemporaryDirectory() as temporary:
            binary = str(Path(temporary) / "serial-input")
            subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            "-I" + str(ROOT / "main/boards/sensecap-watcher"),
                            "-x", "c++", "-", "-o", binary], input=harness,
                           text=True, capture_output=True, check=True)
            for _ in range(10):
                subprocess.run([binary], check=True, capture_output=True)
        command = source.split("const esp_console_cmd_t ur_hw_test_cmd =", 1)[1].split(
            "ESP_ERROR_CHECK(esp_console_cmd_register(&ur_hw_test_cmd))", 1)[0]
        self.assertIn("DispatchTestInput(argv[1])", command)
        self.assertNotIn("app.ToggleChatState()", command)

    def test_storage_fault_button_recovery_before_and_during_network_boot(self):
        """执行真实按钮分发：受理和独占阶段均可重启恢复，唤醒及双击不能误重启。"""
        source = (ROOT / "main/boards/sensecap-watcher/sensecap_watcher.cc").read_text()
        begin = source.index("    void DispatchInput(")
        end = source.index("    // ExecuteMenuAction", begin)
        harness = r'''
#include <atomic>
#include <cassert>
#include <functional>
#include <initializer_list>
#include "ur_network.h"
#include "ui/menu_model.h"
int rebooted = 0, failed = 0, posted = 0;
ur_network_status_t snapshot{};
void ur_network_status(ur_network_status_t* status) { *status = snapshot; }
int ur_network_fail(uint8_t error) { assert(error == UR_NETWORK_ERROR_CANCELLED); ++failed; return UR_NETWORK_ROLLBACK; }
void esp_restart() { ++rebooted; }
struct ur_ota_status_t { int state = 0; };
void ur_bridge_get_ota_status(ur_ota_status_t*) {}
bool ur_bridge_photo_busy() { return false; }
int BSP_PWR_VBUS_IN_DET = 0;
int IoExpanderGetLevel(int) { return 0; }
struct Application {
    static Application& GetInstance() { static Application app; return app; }
    void Schedule(std::function<void()> task) { task(); }
    void Reboot() { ++rebooted; }
};
struct Display {
    void ShowNotification(const char*) {}
    uint32_t InputEpoch() const {return 7;}
    uint32_t InputRevision() const {return 9;}
    void PostInput(watcher::MenuInput, bool, bool, int, int, bool, uint32_t epoch, uint32_t revision) {
        assert(epoch==7 && revision==9); ++posted;
    }
};
struct Power { void WakeUp() {} };
struct Audio { int output_volume() { return 50; } };
struct Light { int brightness() { return 75; } };
struct Watcher {
    std::atomic<bool> screen_asleep_{false}, network_flow_{false};
    Display display; Display* display_ = &display;
    Power power; Power* power_save_timer_ = &power;
    Audio audio; Light light;
    Audio* GetAudioCodec() { return &audio; }
    Light* GetBacklight() { return &light; }
METHOD
};
int main() {
    using namespace watcher;
    Watcher board;
    snapshot.blocked = true;
    for (bool flow : {false, true}) {
        board.network_flow_ = flow;
        for (auto input : {MenuInput::Click, MenuInput::LongPress, MenuInput::DoubleClick,
                           MenuInput::Next, MenuInput::Previous}) {
            int previous = rebooted;
            board.screen_asleep_ = true;
            board.DispatchInput(input);
            assert(rebooted == previous && posted == 0 && failed == 0);
            board.screen_asleep_ = false;
            board.DispatchInput(input);
            assert(rebooted == previous + (input == MenuInput::Click || input == MenuInput::LongPress));
            assert(posted == 0 && failed == 0);
        }
    }
    snapshot.blocked = false;
    board.DispatchInput(MenuInput::Click);
    assert(failed == 1 && posted == 0);
    // 已保存的网络不可再取消：输入交给结果页，不误报存储失败。
    snapshot.stage = 7;
    board.DispatchInput(MenuInput::Click);
    assert(failed == 1 && posted == 1);
    board.network_flow_ = false;
    board.DispatchInput(MenuInput::Click);
    assert(posted == 2);
}
'''.replace("METHOD", source[begin:end])
        with tempfile.TemporaryDirectory() as temporary:
            binary = str(Path(temporary) / "network-buttons")
            result = subprocess.run(["g++", "-std=c++17", "-DCONFIG_UR_AI=1", "-Wall", "-Wextra", "-Werror",
                                     "-I" + str(ROOT / "main/unitedrhino"),
                                     "-I" + str(ROOT / "main/boards/sensecap-watcher"),
                                     "-x", "c++", "-", "-o", binary], input=harness,
                                    text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            for _ in range(10):
                result = subprocess.run([binary], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_actual_brightness_and_photo_sleep_guard(self):
        bridge = (ROOT / "main/unitedrhino/ur_bridge.cc").read_text()
        getter = "int ur_bridge_get_brightness(void)" + bridge.split("int ur_bridge_get_brightness(void)", 1)[1].split("void ur_bridge_set_brightness", 1)[0]
        application = (ROOT / "main/application.cc").read_text()
        sleep = "bool Application::CanEnterSleepMode()" + application.split("bool Application::CanEnterSleepMode()", 1)[1].split("void Application::RegisterMcpBroadcastCallback", 1)[0]
        harness = r'''
#include <cassert>
static int s_brightness = 80;
static bool photo_busy = false;
bool ur_bridge_photo_busy() { return photo_busy; }
struct Backlight { int value = 75; int brightness() const { return value; } };
struct Board {
    Backlight light; bool present = true;
    static Board& GetInstance() { static Board board; return board; }
    Backlight* GetBacklight() { return present ? &light : nullptr; }
};
enum { kDeviceStateIdle, kDeviceStateSpeaking };
struct Protocol { bool open = false; bool IsAudioChannelOpened() { return open; } };
struct Audio { bool idle = true; bool IsIdle() { return idle; } };
struct Application {
    int state = kDeviceStateIdle; Protocol* protocol_ = nullptr; Audio audio_service_;
    int GetDeviceState() { return state; }
    bool CanEnterSleepMode();
};
'''
        harness += getter + sleep + r'''
int main() {
    auto& board = Board::GetInstance();
    assert(ur_bridge_get_brightness() == 75);
    board.light.value = 40;
    assert(ur_bridge_get_brightness() == 40);
    board.present = false;
    assert(ur_bridge_get_brightness() == 0);
    Application app;
    assert(app.CanEnterSleepMode());
    photo_busy = true;
#if CONFIG_UR_AI
    assert(!app.CanEnterSleepMode());
#else
    assert(app.CanEnterSleepMode());
#endif
    photo_busy = false;
    app.state = kDeviceStateSpeaking; assert(!app.CanEnterSleepMode());
    app.state = kDeviceStateIdle; app.audio_service_.idle = false;
    assert(!app.CanEnterSleepMode());
    app.audio_service_.idle = true; Protocol protocol; protocol.open = true;
    app.protocol_ = &protocol; assert(!app.CanEnterSleepMode());
    protocol.open = false; assert(app.CanEnterSleepMode());
}
'''
        with tempfile.TemporaryDirectory() as temporary:
            for enabled in (0, 1):
                binary = str(Path(temporary) / f"ui-{enabled}")
                subprocess.run(["g++", "-std=c++17", f"-DCONFIG_UR_AI={enabled}",
                                "-x", "c++", "-", "-o", binary], input=harness,
                               text=True, check=True, capture_output=True)
                for _ in range(10):
                    result = subprocess.run([binary], capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stderr)
