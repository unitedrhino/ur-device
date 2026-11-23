"""执行真实按钮读取边界，验证共享扩展器中断采样减少空闲I2C且保留边沿。"""
from pathlib import Path
import subprocess
import tempfile
import unittest
from .test_call_audio_decode import method

ROOT = Path(__file__).resolve().parents[2]


class WatcherButtonPollTest(unittest.TestCase):
    """替换GPIO/I2C时钟边界，按钮驱动的实际回调和板级方法保持原样。"""

    def test_irq_cache_shared_reads_fallback_and_failure(self):
        source = (ROOT / 'main/boards/sensecap-watcher/sensecap_watcher.cc').read_text()
        getter = method(source, 'uint8_t IoExpanderGetLevel(')
        knob = method(source, 'uint8_t ReadKnobButton(') if 'uint8_t ReadKnobButton(' in source else ''
        callback = method(source, 'btn_driver_->get_key_level = []')
        body = callback[callback.index('{') + 1:callback.rfind('}')]
        code = r'''
#include <cassert>
#include <cstdint>
#include <mutex>
#include <cstdio>
using std::uint32_t;using std::int64_t;
constexpr int ESP_OK=0,BSP_IO_EXPANDER_INT=2;
constexpr uint16_t DRV_IO_EXP_INPUT_MASK=0x20ff,BSP_KNOB_BTN=8;
static int64_t now_us=0;static int irq=1,reads=0,error=0;static uint32_t inputs=0x20ff;
int64_t esp_timer_get_time(){return now_us;}
int gpio_get_level(int){return irq;}
int esp_io_expander_get_level(void*,uint16_t,uint32_t* out){++reads;if(error)return -1;*out=inputs;irq=1;return 0;}
struct button_driver_t {};
struct Board {
 void* io_exp_handle=nullptr;static Board* instance_;
 std::mutex io_input_mutex_;uint32_t io_input_cache_=DRV_IO_EXP_INPUT_MASK;
 int64_t io_input_at_=-20000;
 unsigned io_input_reads_=0,knob_cache_hits_=0,io_input_failures_=0;
 @@GETTER@@
 @@KNOB@@
 static uint8_t Driver(button_driver_t* button_driver){(void)button_driver;@@CALLBACK@@}
};Board* Board::instance_=nullptr;
int main(){
 Board board;Board::instance_=&board;
 for(now_us=0;now_us<=100000;now_us+=5000)assert(Board::Driver(nullptr)==0);
 assert(reads<=6 && "idle knob polling still reads I2C at every 5ms tick");
 now_us=105000;inputs&=~BSP_KNOB_BTN;irq=0;assert(Board::Driver(nullptr)==1);
 now_us=110000;inputs|=BSP_KNOB_BTN;irq=0;assert(Board::Driver(nullptr)==0);
 // 其他消费者读取会清除共享INT，但最新快照仍须被按钮回调看到。
 now_us=115000;inputs&=~BSP_KNOB_BTN;irq=0;board.IoExpanderGetLevel(1);
 assert(irq==1 && Board::Driver(nullptr)==1);
 // IRQ缺失时最多20ms刷新；失败不得把未取得的输入当成按下。
 inputs|=BSP_KNOB_BTN;now_us=135000;assert(Board::Driver(nullptr)==0);
 irq=0;error=1;now_us=140000;assert(Board::Driver(nullptr)==0);
 error=0;inputs&=~BSP_KNOB_BTN;assert(Board::Driver(nullptr)==1);
 inputs|=BSP_KNOB_BTN;irq=0;assert(Board::Driver(nullptr)==0);
}
'''.replace('@@GETTER@@', getter).replace('@@KNOB@@', knob).replace('@@CALLBACK@@', body)
        with tempfile.TemporaryDirectory(prefix='watcher-button-poll-') as directory:
            binary = str(Path(directory) / 'poll')
            built = subprocess.run(['g++', '-std=c++17', '-pthread', '-Wall', '-Wextra',
                                    '-x', 'c++', '-', '-o', binary], input=code,
                                   text=True, capture_output=True, timeout=30)
            self.assertEqual(built.returncode, 0, built.stderr)
            result = subprocess.run([binary], text=True, capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
