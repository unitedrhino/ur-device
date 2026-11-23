"""回放AI上行生产媒体泵，确保16k麦克风帧直接编码并保留暂停及有界发送。"""
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class NativeCaptureTest(unittest.TestCase):
    """硬件替身检查样本与顺序，媒体泵使用原函数。"""

    def test_capture_encodes_native_frames(self):
        source = (ROOT / 'main/signaling/ur_signaling_webrtc_media.cc').read_text()
        start = source.index('void WebRtcVoiceMedia::PumpCapture()')
        body = source[start:source.index('\n}\n', start) + 3]
        protocol = (ROOT / 'main/signaling/ur_signaling_protocol.cc').read_text()
        first = protocol.index('ur::device::AudioFormat format;')
        config = protocol[first:protocol.index('        /* ESP32', first)]
        code = r'''
#include <cassert>
#include <cstdint>
#include <deque>
#include <vector>
#define CONFIG_UR_AI_SIGNALING_WEBRTC 1
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
using esp_ae_sample_t=uint8_t;
constexpr int kIn16kSamples=320, kUp48kSamples=960;
static int resamples=0,sent=0;
int esp_ae_rate_cvt_process(void*,esp_ae_sample_t*,int,esp_ae_sample_t*,uint32_t*) { ++resamples; return 0; }
int peer_connection_send_audio(void*,uint8_t*,size_t) { ++sent; return 0; }
enum class PcmResult { Frame, Empty };
struct Pcm {
    std::deque<int> frames{1,2,3,4,5};
    int pausedDrains=0;
    void DrainCaptureWhilePaused() { ++pausedDrains; }
    PcmResult read_frame(std::vector<int16_t>& frame) {
        if (frames.empty()) return PcmResult::Empty;
        frame.assign(320,frames.front()); frames.pop_front(); return PcmResult::Frame;
    }
};
struct Codec {
    std::vector<int> encoded;
    bool encode(const std::vector<int16_t>& frame,std::vector<uint8_t>& opus) {
        assert(frame.size()==320);
        encoded.push_back(frame.front()); opus={1}; return true;
    }
};
struct WebRtcVoiceMedia {
    bool capture_paused_=false,capture_running_=true;
    void *rate_cvt_=nullptr,*pc_=nullptr;
    Pcm pcm_; Codec codec_; int rtp_sequence_base_=1;
    void PumpCapture();
};
namespace ur::device { struct AudioFormat { int sample_rate=0,channels=0,frame_ms=0; }; }
METHOD
int main() {
    CONFIGURATION
    assert(format.sample_rate==16000 && format.channels==1 && format.frame_ms==20);
    WebRtcVoiceMedia media;
    media.PumpCapture();
    assert(resamples==0 && sent==3 && media.pcm_.frames.size()==2);
    media.PumpCapture();
    assert(sent==5 && media.rtp_sequence_base_==6);
    assert((media.codec_.encoded==std::vector<int>{1,2,3,4,5}));
    media.capture_paused_=true; media.PumpCapture();
    assert(media.pcm_.pausedDrains==1 && sent==5);
    media.capture_paused_=false; media.capture_running_=false;
    media.pcm_.frames.push_back(6); media.PumpCapture();
    assert(sent==5 && media.pcm_.frames.size()==1);
}
'''.replace('METHOD', body).replace('CONFIGURATION', config)
        with tempfile.TemporaryDirectory(prefix='watcher-native-capture-') as directory:
            binary = str(Path(directory) / 'native-capture')
            built = subprocess.run(['g++', '-std=c++17', '-x', 'c++', '-', '-o', binary],
                                   input=code, text=True, capture_output=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            result = subprocess.run([binary], text=True, capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
