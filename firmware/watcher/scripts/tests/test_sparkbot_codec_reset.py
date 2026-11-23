"""编译生产 ES8311 复位逻辑，验证坦克热复位与其他板型默认行为。"""
from pathlib import Path
import subprocess
import tempfile
import unittest


class SparkBotCodecResetTest(unittest.TestCase):
    """校验板级完整复位不会改变其他板型原有复位顺序。"""

    def test_reset_sequence(self):
        root = Path(__file__).resolve().parents[2]
        source = (root / 'main/audio/codecs/es8311_audio_codec.cc').read_text()
        begin = source.index('void Es8311AudioCodec::ResetCodec(')
        end = source.index('\nEs8311AudioCodec::~Es8311AudioCodec()', begin)
        board = (root / 'main/boards/espressif/esp-sparkbot/esp_sparkbot_board.cc').read_text()
        self.assertIn('es8311_addr, use_mclk, false, true)', board)
        code = r'''
#include <cassert>
#include <cstdint>
#include <vector>
using esp_err_t=int;
std::vector<int> writes, waits;
struct Control { int (*write_reg)(Control*, int, int, uint8_t*, int); };
int write(Control*,int reg,int reglen,uint8_t *value,int len) {
 assert(reg==0 && reglen==1 && len==1);writes.push_back(*value);return 0;
}
#define ESP_ERROR_CHECK(x) assert((x)==0)
#define pdMS_TO_TICKS(x) (x)
#define ESP_LOGI(...) ((void)0)
void vTaskDelay(int delay) { waits.push_back(delay); }
struct Es8311AudioCodec { Control *ctrl_if_; void ResetCodec(bool full_reset); };
METHOD
int main() {
 Control ctrl{write}; Es8311AudioCodec codec{&ctrl};
 codec.ResetCodec(false);assert(writes==std::vector<int>{31} && waits==std::vector<int>{5});
 writes.clear();waits.clear();codec.ResetCodec(true);
 assert((writes==std::vector<int>{31,0}));assert((waits==std::vector<int>{20,20}));
}
'''.replace('METHOD', source[begin:end])
        with tempfile.TemporaryDirectory() as directory:
            driver = Path(directory) / 'test.cc'
            driver.write_text(code)
            binary = Path(directory) / 'test'
            result = subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                     str(driver), '-o', str(binary)], text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()
