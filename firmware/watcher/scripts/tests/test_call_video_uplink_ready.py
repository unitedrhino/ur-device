"""执行生产视频准入逻辑，验证 JPEG 和 H264 分别使用各自的实际能力。"""
from pathlib import Path
import subprocess
import tempfile
import unittest


class CallVideoUplinkReadyTest(unittest.TestCase):
    """两种通话媒体的资源判据不能互相冒充。"""

    def test_transport_readiness(self):
        root = Path(__file__).resolve().parents[2]
        source = (root / 'main/signaling/ur_signaling_call.cc').read_text()
        begin = source.index('    bool VideoUplinkReady() const {')
        end = source.index('\n#else\n    /* 纯音频构建', begin)
        method = source[begin:end]
        program = r'''
#include <cassert>
#include <functional>
struct Camera { bool ready=false;bool SupportsCompressedStream()const{return ready;} };
struct Board { Camera* camera=nullptr;static Board& GetInstance(){static Board b;return b;} Camera* GetCamera(){return camera;} };
struct Call { void* video_=nullptr;std::function<bool()> video_starter_;
METHOD
};
int main(){
 Call call;Camera camera;auto& board=Board::GetInstance();
 assert(!call.VideoUplinkReady());board.camera=&camera;
 call.video_=&camera;call.video_starter_=[]{return true;};
#if CONFIG_UR_HUMAN_CALL_UDP_AV
 assert(!call.VideoUplinkReady());camera.ready=true;assert(call.VideoUplinkReady());
 call.video_=nullptr;call.video_starter_={};assert(call.VideoUplinkReady());
 board.camera=nullptr;assert(!call.VideoUplinkReady());
#else
 assert(call.VideoUplinkReady());call.video_starter_={};assert(!call.VideoUplinkReady());
#endif
}
'''.replace('METHOD', method)
        with tempfile.TemporaryDirectory() as directory:
            driver = Path(directory) / 'test.cc'
            driver.write_text(program)
            for udp in (0, 1):
                binary = Path(directory) / str(udp)
                built = subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                        f'-DCONFIG_UR_HUMAN_CALL_UDP_AV={udp}', str(driver),
                                        '-o', str(binary)], capture_output=True, text=True)
                self.assertEqual(built.returncode, 0, built.stderr)
                subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()
