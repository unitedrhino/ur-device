"""回放生产状态桥和页面类型渲染，语音来电不得显示为视频或留下摄像头按钮。"""
from pathlib import Path
import subprocess
import tempfile
import unittest
from .test_call_audio_decode import method

ROOT = Path(__file__).resolve().parents[2]

class CallUiKindTest(unittest.TestCase):
    """类型只从严格邀请产生的核心状态读取，界面不重新猜测媒体权限。"""
    def compile_run(self, harness):
        """编译执行真实方法片段，HAL和显示API只记录可见性与顺序。"""
        output=ROOT.parents[1]/'.temp';output.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=output) as directory:
            binary=Path(directory)/'kind'
            p=subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-x','c++','-','-o',str(binary)],input=harness,text=True,capture_output=True)
            self.assertEqual(p.returncode,0,p.stderr)
            p=subprocess.run([str(binary)],capture_output=True)
            self.assertEqual(p.returncode,0,p.stderr.decode(errors='replace'))

    def test_actual_state_bridge_reports_trusted_type_before_state(self):
        source=(ROOT/'main/signaling/ur_signaling_call.cc').read_text()
        actual=method(source,'void UrSignalingCall::Impl::FireState(')
        self.compile_run(r'''
#include <cassert>
#include <functional>
#include <vector>
#include <atomic>
#include <cstdint>
#define pdTRUE 1
#define pdMS_TO_TICKS(x) x
int xSemaphoreTake(void*,int){return 1;}void xSemaphoreGive(void*){}
namespace ur::device{enum class HumanCallMode{Video,Voice};}
struct Client{ur::device::HumanCallMode kind=ur::device::HumanCallMode::Voice;ur::device::HumanCallMode mode(){return kind;}};
struct UrSignalingCall{enum class UiState{Ringing,Active};struct Impl{
 void* hooks_mutex_=this;Client* client_=nullptr;std::atomic<std::uint32_t> ui_generation_{42};
 struct Hooks{std::function<void(UiState)>on_state;std::function<void(UiState,std::uint32_t)>on_state_generation;std::function<void(bool)>on_call_kind;}hooks_;
 void FireState(UiState);
};};
''' + actual + r'''
int main(){Client client;UrSignalingCall::Impl owner;owner.client_=&client;std::vector<int> order;
 owner.hooks_.on_call_kind=[&](bool voice){order.push_back(voice?1:2);};
 owner.hooks_.on_state=[&](auto){order.push_back(3);};
 owner.hooks_.on_state_generation=[&](auto,auto generation){assert(generation==42);order.push_back(4);};
 owner.FireState(UrSignalingCall::UiState::Ringing);assert((order==std::vector<int>{1,3,4}));
 order.clear();client.kind=ur::device::HumanCallMode::Video;owner.FireState(UrSignalingCall::UiState::Active);assert((order==std::vector<int>{2,3,4}));
 owner.hooks_mutex_=nullptr;order.clear();owner.FireState(UrSignalingCall::UiState::Active);assert(order.empty());
}
''')

    def test_actual_kind_render_hides_camera_and_keeps_voice_controls(self):
        source=(ROOT/'main/boards/sensecap-watcher/watcher_call_ui.cc').read_text()
        self.assertIn('void WatcherCallUi::ApplyCallKind(',source)
        actual=method(source,'void WatcherCallUi::ApplyCallKind(')
        self.compile_run(r'''
#include <cassert>
#include <string>
struct Object{bool hidden=false;std::string text;int x=0;};
constexpr int LV_OBJ_FLAG_HIDDEN=1,kTrackButtonSpan=96,kControlY=118,kRingTitleY=-96;
void lv_label_set_text(Object*o,const char*s){o->text=s;}void lv_obj_update_layout(Object*){}
void PlaceAt(Object*o,int x,int){o->x=x;}
void lv_obj_add_flag(Object*o,int){o->hidden=true;}void lv_obj_remove_flag(Object*o,int){o->hidden=false;}
struct WatcherCallUi{bool voice_call_=false;Object*ring_title_=nullptr,*camera_btn_=nullptr,*mic_btn_=nullptr,*hangup_btn_=nullptr;void ApplyCallKind(bool);};
''' + actual + r'''
int main(){WatcherCallUi ui;ui.ApplyCallKind(true);assert(ui.voice_call_);
 Object title,camera,mic,hangup;ui.ring_title_=&title;ui.camera_btn_=&camera;ui.mic_btn_=&mic;ui.hangup_btn_=&hangup;
 ui.ApplyCallKind(true);assert(title.text=="语音来电"&&camera.hidden&&!mic.hidden&&!hangup.hidden);assert(mic.x<0&&hangup.x>0);
 ui.ApplyCallKind(false);assert(title.text=="视频来电"&&!camera.hidden);assert(mic.x==-kTrackButtonSpan&&hangup.x==0);
}
''')

if __name__=='__main__':unittest.main()
