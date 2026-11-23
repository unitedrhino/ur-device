"""回放实际菜单与镜子任务，覆盖重复进入、调试入口一致性和根屏幕生命周期。"""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / "main/boards/sensecap-watcher"


class WatcherMirrorTest(unittest.TestCase):
    """真实LVGL检查页面切换，任务边界替身验证启停不会重叠。"""

    def test_double_click_only_exits_mirror(self):
        """镜子双击退出且不拍照；唤醒、忙碌、普通菜单仍遵循原门禁。"""
        code = r'''
#include <cassert>
#include "ui/menu_model.h"
int main() {
    using namespace watcher;
    MenuModel model;
    model.Handle(MenuInput::MirrorOn, 1);
    assert(model.Handle(MenuInput::Click, 2) == MenuAction::None);
    assert(model.Handle(MenuInput::DoubleClick, 3, true) == MenuAction::None);
    assert(model.Handle(MenuInput::DoubleClick, 4, false, true) == MenuAction::None);
    assert(model.Page() == MenuPage::Mirror);
    assert(model.Handle(MenuInput::DoubleClick, 5) == MenuAction::Mirror);
    assert(model.Page() == MenuPage::Home);
    assert(model.Handle(MenuInput::DoubleClick, 6) == MenuAction::Photo);
    model.Handle(MenuInput::LongPress, 7);
    assert(model.Handle(MenuInput::DoubleClick, 8) == MenuAction::None);
    assert(model.Page() == MenuPage::Main);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / "mirror-double-click")
            subprocess.run(["g++", "-std=c++17", "-include", "initializer_list",
                            "-I", str(BOARD), "-x", "c++", "-", "-o", binary],
                           input=code, text=True, capture_output=True, check=True)
            result = subprocess.run([binary], capture_output=True, text=True, cwd=directory)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_actual_menu_reenters_mirror_without_hiding_root_screen(self):
        source = (BOARD / "sensecap_watcher.cc").read_text()
        render = source[source.index("        void RenderMenu()"):source.index("        // PollMenu")]
        setup_begin = source.index("            menu_page_.screen = lv_obj_create(nullptr);")
        setup = source[setup_begin:source.index("            pages_.Open(home_page_", setup_begin)]
        button_begin = source.index("        void SetMenuButtonVisible(")
        button = source[button_begin:source.index("        // OnMenuAction", button_begin)]
        value_begin = source.index("                    const int value =")
        value = source[value_begin:source.index("                    menu_action_(action, value);", value_begin)]
        touch_begin = source.index("        void SetupMirrorExitTouch()")
        touch = source[touch_begin:source.index("        // SetupMenu", touch_begin)]
        code = r'''
#include <algorithm>
#include <cassert>
#include <functional>
#include <string>
#include "lvgl.h"
#include "ui/menu_model.h"
#include "ui/page_manager.h"
#define ESP_LOGI(...) ((void)0)
struct Font { const lv_font_t* font() { return LV_FONT_DEFAULT; } };
struct LvglTheme { Font font_; Font* text_font() { return &font_; } };
struct Description { const char* version="test"; };
const Description* esp_app_get_description() { static Description d; return &d; }
struct CustomLcdDisplay {
    watcher::MenuModel menu_model_;
    watcher::PageManager pages_;
    watcher::Page home_page_,menu_page_;
    LvglTheme theme;
    LvglTheme* current_theme_=&theme;
    lv_obj_t* home_menu_=nullptr;
    lv_obj_t* container_=nullptr;
    lv_obj_t* preview_image_=nullptr;
    lv_obj_t* menu_title_=nullptr;
    lv_obj_t* menu_list_=nullptr;
    lv_obj_t* menu_labels_[8]{};
    std::function<void(size_t,uint32_t)> touch_item_;
    uint32_t touch_revision_=0;
    uint32_t mirror_touch_revision_=0;
    bool mirror_touch_pending_=false;
    uint32_t mirror_touch_started_=0;
    lv_point_t mirror_touch_point_{};
    bool call_ui_active_=false,charging_=false,binding_flow_=false;
    struct { bool blocked=false; uint8_t stage=0,error=0; } shown_network_;
    void ApplyPagePalette() {}
    void Setup() {
        home_page_.screen=lv_screen_active();
        // 还原生产主页中可命中触摸的全屏背景，避免测试把空白区错误地当作根屏幕。
        container_=lv_obj_create(home_page_.screen);
        lv_obj_set_size(container_,412,412);
        lv_obj_set_style_border_width(container_,0,0);
        lv_obj_set_style_pad_all(container_,0,0);
        preview_image_=lv_image_create(home_page_.screen);
        lv_obj_set_size(preview_image_,240,240);
        lv_obj_center(preview_image_);
        SetupMirrorExitTouch();
        home_menu_=lv_button_create(home_page_.screen);
SETUP
        pages_.Open(home_page_,lv_tick_get(),0);
    }
BUTTON
TOUCH
RENDER
    int Value(watcher::MenuAction action) {
VALUE
        return value;
    }
};
int main() {
    using namespace watcher;
    lv_init(); auto* display=lv_display_create(412,412);
    CustomLcdDisplay board; board.Setup();
    uint32_t now=100;
    for (int round=0;round<100;++round) {
        auto& model=board.menu_model_;
        assert(model.Handle(MenuInput::MirrorOn,++now)==MenuAction::Mirror);
        assert(board.Value(MenuAction::Mirror)==1);
        assert(model.Handle(MenuInput::MirrorOn,++now)==MenuAction::None);
        board.RenderMenu();
        assert(lv_screen_active()==board.home_page_.screen);
        assert(!lv_obj_has_flag(board.menu_page_.screen,LV_OBJ_FLAG_HIDDEN));
        assert(lv_obj_has_flag(board.home_menu_,LV_OBJ_FLAG_HIDDEN));
        now+=120001;
        assert(model.Handle(MenuInput::Tick,now)==MenuAction::None && model.Page()==MenuPage::Mirror);
        assert(model.Handle(MenuInput::Back,++now)==MenuAction::Mirror);
        assert(board.Value(MenuAction::Mirror)==0);
        board.RenderMenu();
        assert(!lv_obj_has_flag(board.home_menu_,LV_OBJ_FLAG_HIDDEN));
        assert(model.Handle(MenuInput::LongPress,++now)==MenuAction::CloseChat);
        board.RenderMenu();
        assert(lv_screen_active()==board.menu_page_.screen);
        assert(model.TouchItem(7,model.Revision(),++now)==MenuAction::Mirror);
        assert(board.Value(MenuAction::Mirror)==1);
        board.RenderMenu();
        assert(lv_screen_active()==board.home_page_.screen);
        assert(model.Handle(MenuInput::MirrorOff,++now)==MenuAction::Mirror);
        assert(model.Handle(MenuInput::MirrorOff,++now)==MenuAction::None);
        board.RenderMenu();
        assert(!lv_obj_has_flag(board.home_menu_,LV_OBJ_FLAG_HIDDEN));
    }
    assert(board.menu_model_.Handle(MenuInput::MirrorOn,++now,false,true)==MenuAction::None);
    std::puts("PASS mirror menus rounds=100");
    // 通过真实LVGL指针采样执行生产回调，覆盖画面和周围空白区；不直接发送双击事件。
    auto* pointer=lv_indev_create();
    lv_indev_set_type(pointer,LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(pointer,display);
    lv_indev_set_mode(pointer,LV_INDEV_MODE_EVENT);
    struct Contact { int x=0,y=0; bool down=false; } contact;
    lv_indev_set_user_data(pointer,&contact);
    lv_indev_set_read_cb(pointer,[](lv_indev_t* input,lv_indev_data_t* data) {
        auto* value=static_cast<Contact*>(lv_indev_get_user_data(input));
        data->point={value->x,value->y};
        data->state=value->down?LV_INDEV_STATE_PRESSED:LV_INDEV_STATE_RELEASED;
    });
    bool busy=false;
    int exits=0;
    board.touch_item_=[&](size_t index,uint32_t revision) {
        assert(index==TouchBackIndex);
        const auto action=board.menu_model_.TouchBack(revision,lv_tick_get(),false,busy);
        if (action==MenuAction::Mirror) ++exits;
        else assert(action==MenuAction::None);
        board.RenderMenu();
    };
    auto sample=[&](int x,int y,bool down,unsigned elapsed=30) {
        if (down && !contact.down) board.touch_revision_=board.menu_model_.Revision();
        contact={x,y,down};lv_tick_inc(elapsed);lv_indev_read(pointer);
    };
    auto tap=[&](int x,int y) { sample(x,y,true);sample(x,y,false); };
    auto open=[&]() {
        lv_tick_inc(700);
        board.menu_model_.Handle(MenuInput::MirrorOn,lv_tick_get());
        board.RenderMenu();lv_obj_update_layout(board.home_page_.screen);
    };
    for (int round=0;round<10;++round) {
        for (int coordinate : {206,70}) {
            open();
            tap(coordinate,206);
            assert(board.menu_model_.Page()==MenuPage::Mirror);
            tap(coordinate,206);
            assert(board.menu_model_.Page()==MenuPage::Home);
        }
    }
    assert(exits==20);
    // 实体两次点按允许有小幅位置偏差，不能套用LVGL默认10像素的双击距离。
    open();tap(206,206);tap(226,206);
    assert(board.menu_model_.Page()==MenuPage::Home);
    // 距离太远的两次点击不能被放宽容差拼成双击。
    open();tap(150,206);tap(250,206);
    assert(board.menu_model_.Page()==MenuPage::Mirror);
    board.menu_model_.Handle(MenuInput::MirrorOff,lv_tick_get());board.RenderMenu();
    open();busy=true;tap(206,206);tap(206,206);
    assert(board.menu_model_.Page()==MenuPage::Mirror && exits==21);
    busy=false;lv_tick_inc(700);
    tap(206,206);lv_tick_inc(700);tap(206,206);
    assert(board.menu_model_.Page()==MenuPage::Mirror);
    tap(206,206);assert(board.menu_model_.Page()==MenuPage::Home);
    // 第一次点击后退出重进，不能将两个页面的单击拼接为退出手势。
    open();tap(206,206);
    board.menu_model_.Handle(MenuInput::MirrorOff,lv_tick_get());board.RenderMenu();
    board.menu_model_.Handle(MenuInput::MirrorOn,lv_tick_get());board.RenderMenu();
    tap(206,206);assert(board.menu_model_.Page()==MenuPage::Mirror);
    // 长按与拖动不能提供双击所需的短点击。
    lv_tick_inc(700);sample(206,206,true);sample(206,206,true,700);sample(206,206,false);
    tap(206,206);assert(board.menu_model_.Page()==MenuPage::Mirror);
    lv_tick_inc(700);sample(206,206,true);sample(310,206,true);sample(310,206,false);
    assert(board.menu_model_.Page()==MenuPage::Mirror);
    lv_indev_delete(pointer);
    std::puts("PASS actual LVGL mirror double tap rounds=10");
    // 页面管理器先于显示销毁，避免测试退出时组仍引用已删除的对象。
    (void)display;
}
'''.replace("SETUP", setup).replace("BUTTON", button).replace("TOUCH", touch).replace("RENDER", render).replace("VALUE", value)
        output = ROOT.parents[1] / ".temp"
        output.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=output) as directory:
            folder = Path(directory)
            (folder / "main.cc").write_text(code)
            (folder / "lv_conf.h").write_text("#define LV_COLOR_DEPTH 16\n#define LV_USE_OS 0\n")
            (folder / "CMakeLists.txt").write_text(f'''cmake_minimum_required(VERSION 3.16)
project(watcher_mirror_test C CXX)
set(CMAKE_CXX_STANDARD 17)
set(LV_BUILD_CONF_PATH "{folder / 'lv_conf.h'}" CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
add_subdirectory("{ROOT / 'managed_components/lvgl__lvgl'}" lvgl-build)
add_executable(mirror main.cc "{BOARD / 'watcher_page_manager.cc'}")
target_include_directories(mirror PRIVATE "{BOARD}")
target_link_libraries(mirror PRIVATE lvgl m)
''')
            for command in [["cmake", "-S", str(folder), "-B", str(folder / "build")],
                            ["cmake", "--build", str(folder / "build"), "--parallel", "4"],
                            [str(folder / "build/mirror")]]:
                result = subprocess.run(command, capture_output=True, text=True, timeout=300)
                self.assertEqual(result.returncode, 0, result.stdout[-4000:] + result.stderr[-4000:])

    def test_actual_worker_serializes_stop_cleanup_and_restart(self):
        source = (BOARD / "sensecap_watcher.cc").read_text()
        begin = source.index("static std::atomic<bool> g_mirror_on")
        methods = source[begin:source.index('#include "lvgl_theme.h"', begin)]
        code = r'''
#include <atomic>
#include <cassert>
#include <cstdio>
#define ESP_LOGW(...) ((void)0)
using TaskHandle_t=void*;
constexpr int pdTRUE=1,pdPASS=1,portMAX_DELAY=-1;
int pdMS_TO_TICKS(int n) { return n; }
void vTaskDelay(int) {}
bool SetMirrorEnabled(struct Camera*,bool);
struct Camera {
    int starts=0,stops=0,frames=0;
    bool running=false;
    void StartStream() { assert(!running); running=true; ++starts; }
    bool Capture() {
        assert(running);++frames;
        if (frames==1) { SetMirrorEnabled(this,false);SetMirrorEnabled(this,true); }
        else SetMirrorEnabled(this,false);
        return true;
    }
    void StopStream() {
        assert(running);running=false;++stops;
        if (stops==1) SetMirrorEnabled(this,true);
    }
};
struct LcdDisplay {
    int clears=0;
    bool fullscreen=false;
    void SetPreviewFullscreen(bool on) { fullscreen=on; }
    void SetPreviewImage(void*) { ++clears; }
};
LcdDisplay display;
struct Board {
    static Board& GetInstance() { static Board board;return board; }
    LcdDisplay* GetDisplay() { return &display; }
};
int creates=0,notifies=0,takes=0;
bool fail_create=false;
constexpr int MALLOC_CAP_SPIRAM=1,MALLOC_CAP_8BIT=2;
// 校验生产入口为非 DMA 镜子栈选择外部内存，同时保留创建失败及重复启停语义。
int xTaskCreateWithCaps(void(*)(void*),const char*,int stack,void*,int priority,
                        TaskHandle_t* task,int caps) {
    assert(stack==8192 && priority==3 && caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    ++creates;if (fail_create) return 0; *task=&display;return pdPASS;
}
void xTaskNotifyGive(TaskHandle_t) { ++notifies; }
struct Finished {};
int ulTaskNotifyTake(int,int) { if (++takes>2) throw Finished{}; return 1; }
// METHODS
int main() {
    Camera camera;
    assert(!SetMirrorEnabled(nullptr,true) && !g_mirror_on.load());
    fail_create=true;
    assert(!SetMirrorEnabled(&camera,true) && !g_mirror_on.load() && !g_mirror_task);
    fail_create=false;
    assert(SetMirrorEnabled(&camera,true));
    assert(SetMirrorEnabled(&camera,true));
    assert(creates==2);
    try { UrMirrorTask(&camera); } catch (const Finished&) {}
    assert(camera.starts==2 && camera.stops==2 && camera.frames==3);
    assert(!camera.running && !display.fullscreen && display.clears==2);
    assert(!g_mirror_on.load() && g_mirror_task && creates==2);
    assert(SetMirrorEnabled(&camera,false));
    std::puts("PASS serialized mirror worker");
}
'''.replace("// METHODS", methods).replace("bool SetMirrorEnabled(struct Camera*,bool);",
                                          "static bool SetMirrorEnabled(struct Camera*,bool);")
        compiler = shutil.which("g++") or shutil.which("clang++")
        self.assertIsNotNone(compiler)
        output = ROOT.parents[1] / ".temp"
        output.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=output) as directory:
            binary = str(Path(directory) / "worker")
            result = subprocess.run([compiler, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
                                     "-fsanitize=undefined", "-x", "c++", "-", "-o", binary],
                                    input=code, text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([binary], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
