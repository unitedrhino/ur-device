"""回放真实相机开关分支，UDP仅启停JPEG相机，不误启H264或变更媒体协议。"""
from pathlib import Path
import subprocess
import tempfile
import unittest
from .test_call_audio_decode import method

ROOT = Path(__file__).resolve().parents[2]


class CallCameraControlTest(unittest.TestCase):
    """替代硬件与信令出口，核对本地资源状态、恢复失败及两种传输分支。"""

    def test_actual_camera_toggle_uses_selected_capture_driver(self):
        """运行生产case，保留待处理控制、纯语音和无相机的边界行为。"""
        source = (ROOT / 'main/signaling/ur_signaling_call.cc').read_text()
        start = source.index('        case kUiToggleCamera:')
        actual = source[start:source.index('        default:', start)]
        code = r'''
#include <cassert>
#include <cstdint>
#include <string>
#define ESP_LOGW(...) ((void)0)
namespace ur::device {enum class HumanCallState{Available,Connected};}
struct Camera {
 int starts=0,stops=0;bool accepted=true;unsigned interval=120;
 void StopStream(){++stops;}
 bool StartCompressedStream(){++starts;return accepted;}
 void SetStreamFrameInterval(unsigned value){interval=value;}
};
struct Board {
 Camera*camera=nullptr;
 static Board& GetInstance(){static Board board;return board;}
 Camera* GetCamera(){return camera;}
};
struct Peer {bool udp=true,worker_ok=true;int starts=0,stops=0;bool IsUDP(){return udp;}
 bool StartUdpVideo(){++starts;return worker_ok;}void StopUdpVideo(){++stops;}};
struct Client {
 bool pending=false;int controls=0;bool paused=false;
 bool track_control_pending(){return pending;}
 void control_track(const char*id,bool p,std::uint64_t){assert(std::string(id)=="video-1");++controls;paused=p;}
};
struct Ui {
 enum{kUiToggleCamera};Client*client_;Peer*media_;bool voice_call_=false,camera_paused_=false,udp_camera_owned_=true;
 int h264_starts=0,h264_stops=0,notifications=0;
 void StartVideoUplink(){++h264_starts;}
 void StopVideoUplink(){++h264_stops;}
 void NotifyTrackState(){++notifications;}
 void Toggle(ur::device::HumanCallState state=ur::device::HumanCallState::Connected){std::uint64_t now=1;switch(kUiToggleCamera){CASE}}
};
int main(){
 Camera camera;Board::GetInstance().camera=&camera;Peer peer;Client client;Ui ui{&client,&peer};
 ui.Toggle();assert(ui.camera_paused_&&client.paused&&client.controls==1);
#if CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO
 assert(camera.stops==1&&peer.stops==1&&ui.h264_stops==0&&camera.interval==0&&"UDP pause must restore camera interval");
#endif
 ui.Toggle();assert(!ui.camera_paused_&&!client.paused&&client.controls==2);
#if CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO
 assert(camera.starts==1&&peer.starts==1&&ui.h264_starts==0&&camera.interval==40&&"UDP resume must restore call cadence");
 ui.Toggle();camera.accepted=false;const int controls=client.controls;ui.Toggle();
 assert(ui.camera_paused_&&client.controls==controls&&camera.interval==0&&"failed camera restart advertised active or changed cadence");
 Board::GetInstance().camera=nullptr;ui.Toggle();assert(ui.camera_paused_&&client.controls==controls);
 Board::GetInstance().camera=&camera;
 camera.accepted=true;peer.worker_ok=false;const int stops=camera.stops;ui.Toggle();
 assert(ui.camera_paused_&&client.controls==controls&&camera.stops==stops+1&&camera.interval==0&&"worker failure leaked camera or advertised active");
 // 未持有相机时暂停不得更改其他使用者的节拍或停止其流。
 Ui other{&client,&peer};other.udp_camera_owned_=false;camera.interval=77;const int other_stops=camera.stops;
 other.Toggle();assert(camera.stops==other_stops&&camera.interval==77);
#endif
 ui.voice_call_=true;const int guarded=client.controls;ui.Toggle();assert(client.controls==guarded);
 ui.voice_call_=false;client.pending=true;ui.Toggle();assert(client.controls==guarded);
 client.pending=false;ui.Toggle(ur::device::HumanCallState::Available);assert(client.controls==guarded);
 Client rtc_client;peer.udp=false;Ui rtc{&rtc_client,&peer};rtc.Toggle();rtc.Toggle();
#if CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO
 assert(rtc.h264_starts==1&&rtc.h264_stops==1);
#endif
}
'''.replace('CASE', actual)
        for enabled in (0, 1):
            with self.subTest(video=enabled), tempfile.TemporaryDirectory(prefix='watcher-camera-control-') as directory:
                binary = str(Path(directory) / 'camera')
                built = subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                        '-DCONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO=' + str(enabled),
                                        '-x', 'c++', '-', '-o', binary], input=code,
                                       text=True, capture_output=True, timeout=30)
                self.assertEqual(built.returncode, 0, built.stderr)
                result = subprocess.run([binary], text=True, capture_output=True, timeout=5)
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_udp_media_only_stops_its_owned_camera(self):
        """启动失败或其他功能占用相机时，结束通话不能停止其他相机流。"""
        source = (ROOT / 'main/signaling/ur_signaling_call.cc').read_text()
        begin = source.split('void UrSignalingCall::Impl::StartMedia()', 1)[1]
        end = source.split('void UrSignalingCall::Impl::StopMedia()', 1)[1]
        start = method(begin, 'if (!voice_call_ && media_ && media_->IsUDP())')
        stop = method(end, 'if (media_ && media_->IsUDP())')
        code = r'''
#include <cassert>
#define ESP_LOGW(...) ((void)0)
struct Camera {bool accepts=true;int starts=0,stops=0;unsigned interval=77;bool StartCompressedStream(){++starts;return accepts;}void SetStreamFrameInterval(unsigned value){interval=value;}void StopStream(){++stops;}};
struct Board {Camera* camera=nullptr;static Board& GetInstance(){static Board b;return b;}Camera* GetCamera(){return camera;}};
struct Peer {bool worker_ok=true;int starts=0,stops=0;bool IsUDP(){return true;}bool StartUdpVideo(){++starts;return worker_ok;}void StopUdpVideo(){++stops;}};
struct Ui {Peer*media_;bool voice_call_=false,camera_paused_=false,udp_camera_owned_=false;
 void Start(){START}void Stop(){STOP}};
int main(){Camera camera;Board::GetInstance().camera=&camera;Peer peer;
 // 相机由其他功能占用：不得停止已有流。
 {Ui ui{&peer};camera.accepts=false;ui.Start();ui.Stop();assert(ui.camera_paused_&&!ui.udp_camera_owned_&&camera.stops==0&&camera.interval==77);}
 // 任务创建失败：只回收本次成功启动的相机。
 {Ui ui{&peer};camera.accepts=true;peer.worker_ok=false;ui.Start();ui.Stop();assert(ui.camera_paused_&&!ui.udp_camera_owned_&&camera.stops==1&&camera.interval==77);}
 // 正常结束：视频任务先停止，相机只停止一次。
 {Ui ui{&peer};peer.worker_ok=true;ui.Start();assert(ui.udp_camera_owned_&&camera.interval==40);ui.Stop();assert(!ui.udp_camera_owned_&&camera.stops==2&&camera.interval==0);}
}
'''.replace('START', start).replace('STOP', stop)
        with tempfile.TemporaryDirectory(prefix='watcher-camera-ownership-') as directory:
            binary = str(Path(directory) / 'camera-ownership')
            built = subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-x', 'c++',
                                    '-', '-o', binary], input=code, text=True, capture_output=True,
                                   timeout=30)
            self.assertEqual(built.returncode, 0, built.stderr)
            result = subprocess.run([binary], text=True, capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)
