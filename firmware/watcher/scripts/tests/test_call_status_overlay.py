"""执行真实板级页面归属与提示层策略，通话不得叠加主页时间/小智提示。"""
from pathlib import Path
import subprocess
import tempfile
import unittest
from .test_call_audio_decode import method

ROOT = Path(__file__).resolve().parents[2]


class CallStatusOverlayTest(unittest.TestCase):
    """独立顶层不随切屏自动隐藏，须同时验证进入与退出后的恢复。"""

    def test_call_hides_home_status_and_restores_home_palette(self):
        source = (ROOT / 'main/boards/sensecap-watcher/sensecap_watcher.cc').read_text()
        actual = '\n'.join(method(source, signature) for signature in [
            'void ApplyPagePalette(', 'void SetCallUiActive(',
        ])
        code = r'''
#include <cassert>
#include <initializer_list>
#include <cstdint>
namespace watcher {enum class MenuPage {Home,Menu};}
struct lv_obj_t {bool hidden=false;int color=0;};
constexpr int LV_OBJ_FLAG_HIDDEN=1,LV_OPA_TRANSP=0,LV_SCR_LOAD_ANIM_FADE_IN=0;
int lv_color_hex(int value){return value;}void lv_obj_set_style_text_color(lv_obj_t*o,int c,int){o->color=c;}
void lv_obj_set_style_bg_opa(lv_obj_t*,int,int){}
void lv_obj_remove_flag(lv_obj_t*o,int){o->hidden=false;}void lv_obj_add_flag(lv_obj_t*o,int){o->hidden=true;}
uint32_t lv_tick_get(){return 1;}static lv_obj_t* active;
lv_obj_t* lv_display_get_screen_active(void*){return active;}void lv_screen_load_anim(lv_obj_t*o,int,int,int,bool){active=o;}
struct LvglTheme {int text_color()const{return 0x111111;}};
struct Model {watcher::MenuPage page=watcher::MenuPage::Home;watcher::MenuPage Page(){return page;}void ForceHome(){page=watcher::MenuPage::Home;}};
struct Pages {template<class T>void Open(T&,int,int){}};
struct Display {
 bool call_ui_active_=false;LvglTheme theme;void* current_theme_=&theme;void* display_=nullptr;
 Model menu_model_;Pages pages_;struct {lv_obj_t* screen=nullptr;}home_page_;
 lv_obj_t *network_label_=nullptr,*status_label_=nullptr,*notification_label_=nullptr,*mute_label_=nullptr,*battery_label_=nullptr,*top_bar_=nullptr,*status_bar_=nullptr;
 PRODUCTION
};
int main(){
 Display d;lv_obj_t status,network,home,call;d.status_bar_=&status;d.network_label_=&network;d.home_page_.screen=&home;active=&call;
 d.SetCallUiActive(true);
 assert(status.hidden && "home status layer still overlays call controls");
 assert(network.color==0xc3cfc2 && "home dark icons invisible on call background");
 d.ApplyPagePalette();assert(status.hidden);
 d.SetCallUiActive(false);assert(!status.hidden && active==&home && network.color==0x111111);
 d.menu_model_.page=watcher::MenuPage::Menu;d.ApplyPagePalette();assert(status.hidden);
}
'''.replace('PRODUCTION', actual)
        with tempfile.TemporaryDirectory(prefix='watcher-call-overlay-') as directory:
            binary = str(Path(directory) / 'overlay')
            built = subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                    '-x', 'c++', '-', '-o', binary], input=code,
                                   text=True, capture_output=True, timeout=30)
            self.assertEqual(built.returncode, 0, built.stderr)
            result = subprocess.run([binary], text=True, capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
