"""回放真实定位助手，LVGL延迟布局时圆形按钮仍须按实际尺寸居中。"""
from pathlib import Path
import subprocess
import tempfile
import unittest
from .test_call_audio_decode import method

ROOT = Path(__file__).resolve().parents[2]


class CallButtonLayoutTest(unittest.TestCase):
    """尺寸仅在布局更新后可读，复现真机按钮相对提示文字偏半径的问题。"""

    def test_deferred_layout_before_centering(self):
        source = (ROOT / 'main/boards/sensecap-watcher/watcher_call_ui.cc').read_text()
        actual = method(source, 'void PlaceAt(')
        code = r'''
#include <cassert>
constexpr int kScreenSize=412;
struct lv_obj_t {int requested_w,requested_h;int w=0,h=0,x=0,y=0;};
void lv_obj_update_layout(lv_obj_t* o){o->w=o->requested_w;o->h=o->requested_h;}
int lv_obj_get_width(lv_obj_t* o){return o->w;}int lv_obj_get_height(lv_obj_t* o){return o->h;}
void lv_obj_set_pos(lv_obj_t* o,int x,int y){o->x=x;o->y=y;}
PRODUCTION
int main(){
 for(int side:{-1,1}){
  lv_obj_t button{96,96};PlaceAt(&button,side*86,12);
  assert(button.x+48==206+side*86 && button.y+48==218 && "pending LVGL size shifted incoming-call button");
 }
 lv_obj_t label{60,30};lv_obj_update_layout(&label);PlaceAt(&label,86,78);
 assert(label.x+30==292 && label.y+15==284);
 lv_obj_t hangup{76,76};PlaceAt(&hangup,0,118);assert(hangup.x+38==206 && hangup.y+38==324);
}
'''.replace('PRODUCTION', actual).replace('#include <cassert>', '#include <cassert>\n#include <initializer_list>')
        with tempfile.TemporaryDirectory(prefix='watcher-button-layout-') as directory:
            binary = str(Path(directory) / 'layout')
            built = subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                    '-x', 'c++', '-', '-o', binary], input=code,
                                   text=True, capture_output=True, timeout=30)
            self.assertEqual(built.returncode, 0, built.stderr)
            result = subprocess.run([binary], text=True, capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
