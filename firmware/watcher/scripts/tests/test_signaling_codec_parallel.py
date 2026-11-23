"""阻塞真实编码方法的DSP调用，验证下行解码仍可独立完成。"""
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def method(source, signature):
    """提取完整生产方法，测试不维护另一份编解码锁策略。"""
    start = source.index(signature)
    return source[start:source.index('\n}\n', start) + 3]


class CodecParallelTest(unittest.TestCase):
    """模拟硬件算法阻塞，不替换编解码入口及参数检查。"""

    def test_decode_progresses_while_encoder_is_busy(self):
        source = (ROOT / 'main/signaling/ur_signaling_opus.cc').read_text()
        methods = '\n'.join(method(source, signature) for signature in [
            'bool UrSignalingOpus::encode(', 'bool UrSignalingOpus::decode(',
        ])
        code = r'''
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>
using namespace std::chrono_literals;
constexpr int ESP_AUDIO_ERR_OK=0, ESP_AUDIO_DEC_RECOVERY_NONE=0;
struct esp_audio_enc_in_frame_t { uint8_t* buffer; uint32_t len; };
struct esp_audio_enc_out_frame_t { uint8_t* buffer; uint32_t len,encoded_bytes; };
struct esp_audio_dec_in_raw_t { uint8_t* buffer; uint32_t len,consumed; int frame_recover; };
struct esp_audio_dec_out_frame_t { uint8_t* buffer; uint32_t len,decoded_size; };
struct esp_audio_dec_info_t {};
static std::mutex control;
static std::condition_variable changed;
static bool encoding=false, release=false, decoded=false;
int64_t esp_timer_get_time() { return 0; }
int esp_opus_enc_process(void*,esp_audio_enc_in_frame_t*,esp_audio_enc_out_frame_t* out) {
    std::unique_lock<std::mutex> lock(control);
    encoding=true; changed.notify_all(); changed.wait(lock,[]{ return release; });
    out->encoded_bytes=3; return ESP_AUDIO_ERR_OK;
}
int esp_opus_dec_decode(void*,esp_audio_dec_in_raw_t*,esp_audio_dec_out_frame_t* out,esp_audio_dec_info_t*) {
    out->decoded_size=640; return ESP_AUDIO_ERR_OK;
}
struct UrSignalingOpus {
    std::mutex mutex_,encoder_mutex_,decoder_mutex_;
    void* encoder_=reinterpret_cast<void*>(1);
    void* decoder_=reinterpret_cast<void*>(2);
    int encoder_frame_samples_=320,encoder_outbuf_size_=128,decoder_frame_samples_=320;
    uint64_t encode_us_=0,decode_us_=0;
    unsigned encode_frames_=0,decode_frames_=0;
    bool encode(const std::vector<int16_t>&,std::vector<uint8_t>&);
    bool decode(const std::vector<uint8_t>&,std::vector<int16_t>&);
};
METHODS
int main() {
    UrSignalingOpus codec;
    std::thread encode([&]{std::vector<uint8_t> out; assert(codec.encode(std::vector<int16_t>(320),out));});
    {
        std::unique_lock<std::mutex> lock(control);
        assert(changed.wait_for(lock,1s,[]{return encoding;}));
    }
    std::thread decode([&]{
        std::vector<int16_t> out; assert(codec.decode({1,2,3},out) && out.size()==320);
        std::lock_guard<std::mutex> lock(control); decoded=true; changed.notify_all();
    });
    bool progressed;
    {
        std::unique_lock<std::mutex> lock(control);
        progressed=changed.wait_for(lock,200ms,[]{return decoded;});
        release=true; changed.notify_all();
    }
    encode.join(); decode.join();
    assert(progressed && "decoder waited for the busy encoder");
    std::vector<int16_t> pcm;
    assert(!codec.decode(std::vector<uint8_t>(1276),pcm));
    std::vector<uint8_t> opus;
    assert(!codec.encode(std::vector<int16_t>(319),opus));
}
'''.replace('METHODS', methods)
        with tempfile.TemporaryDirectory(prefix='watcher-codec-parallel-') as directory:
            binary = str(Path(directory) / 'codec-parallel')
            built = subprocess.run(['g++', '-std=c++17', '-pthread', '-x', 'c++', '-', '-o', binary],
                                   input=code, text=True, capture_output=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            result = subprocess.run([binary], text=True, capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
