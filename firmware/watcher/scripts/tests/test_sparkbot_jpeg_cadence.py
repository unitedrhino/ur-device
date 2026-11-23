"""验证坦克下行画面限频的首帧、间隔和时间回退。"""
from pathlib import Path
import subprocess
import tempfile
import unittest


class SparkBotJpegCadenceTest(unittest.TestCase):
    """使用生产节拍器验证画面持续刷新而非永久停显。"""

    def test_cadence(self):
        root = Path(__file__).resolve().parents[2]
        code = r'''
#include <cassert>
#include "sparkbot_jpeg_cadence.h"
int main() {
 sparkbot::JpegCadence cadence;
 assert(cadence.Ready(0));cadence.Mark(0);
 assert(!cadence.Ready(1) && !cadence.Ready(499));
 assert(cadence.Ready(500));cadence.Mark(500);
 assert(!cadence.Ready(999) && cadence.Ready(1000));
 assert(cadence.Ready(20));cadence.Mark(20);
 assert(!cadence.Ready(519) && cadence.Ready(520));
}
'''
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'cadence'
            built = subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                    '-I', str(root / 'main/boards/espressif/esp-sparkbot'),
                                    '-x', 'c++', '-', '-o', str(binary)], input=code, text=True,
                                   capture_output=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()
