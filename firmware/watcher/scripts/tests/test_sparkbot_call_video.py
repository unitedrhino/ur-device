"""用可控主任务与显示替身验证坦克通话帧交接。"""
from pathlib import Path
import subprocess
import tempfile
import unittest


class SparkBotCallVideoTest(unittest.TestCase):
    """检查最新帧、停流代次和迟到清屏。"""

    def test_frame_lifecycle(self):
        root = Path(__file__).resolve().parents[2]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'application.h').write_text(r'''
#pragma once
#include <functional>
#include <deque>
class Application { public:
 std::deque<std::function<void()>> queue;
 static Application& GetInstance(){static Application a;return a;}
 void Schedule(std::function<void()> f){queue.push_back(f);}
 void step(){auto f=queue.front();queue.pop_front();f();}
};
''')
            (path / 'display.h').write_text(r'''
#pragma once
#include <cstdint>
struct lv_obj_t { bool hidden=true;const void* source=nullptr; };
inline lv_obj_t object;
inline lv_obj_t* lv_screen_active(){return &object;}
inline lv_obj_t* lv_image_create(lv_obj_t*){return &object;}
inline void lv_obj_center(lv_obj_t*){}
inline void lv_image_set_src(lv_obj_t* o,const void* p){o->source=p;}
#define LV_OBJ_FLAG_HIDDEN 1
#define LV_COLOR_FORMAT_RGB565 1
inline void lv_obj_add_flag(lv_obj_t* o,int){o->hidden=true;}
inline void lv_obj_remove_flag(lv_obj_t* o,int){o->hidden=false;}
inline void lv_obj_move_foreground(lv_obj_t*){}
class Display {};
class DisplayLockGuard { public: explicit DisplayLockGuard(Display*){} explicit operator bool()const{return true;} };
''')
            (path / 'lvgl_image.h').write_text(r'''
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
inline std::function<void()> during_copy;
struct LvglAllocatedImage {
 uint8_t byte=0;
 bool CopyFrom(const void* p,size_t,int,int,int,int){byte=*static_cast<const uint8_t*>(p);if(during_copy)during_copy();return true;}
 const void* image_dsc()const{return &byte;}
};
''')
            (path / 'test.cc').write_text(r'''
#include <cassert>
#include "sparkbot_call_video.h"
int main() {
 Display display;sparkbot::CallVideoView view(&display);auto& app=Application::GetInstance();
 uint8_t first=1,second=2;
 view.SetActive(true);view.Submit(&first,1,1);view.Submit(&second,1,1);
 assert(app.queue.size()==1);app.step();assert(!object.hidden && *static_cast<const uint8_t*>(object.source)==2);
 view.Submit(&first,1,1);view.SetActive(false);app.step();app.step();assert(object.hidden && !object.source);
 view.SetActive(true);view.Submit(nullptr,0,0);view.Submit(&second,1,1);
 app.step();app.step();assert(!object.hidden && *static_cast<const uint8_t*>(object.source)==2);
 during_copy=[&]{view.SetActive(false);};view.Submit(&first,1,1);during_copy={};
 assert(app.queue.size()==1);app.step();assert(object.hidden && !object.source);
}
''')
            binary = path / 'test'
            built = subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                    '-I', str(path), '-I', str(root / 'main/boards/espressif/esp-sparkbot'),
                                    str(path / 'test.cc'), '-o', str(binary)], capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()
