"""验证坦克相机缩放的裁剪、格式和缓冲边界。"""
from pathlib import Path
import subprocess
import tempfile
import unittest


class SparkBotVideoPixelsTest(unittest.TestCase):
    """使用带位置标记的像素执行生产缩放函数。"""

    def test_center_crop_and_bounds(self):
        root = Path(__file__).resolve().parents[2]
        code = r'''
#include "stream_video_pixels.h"
#include <cassert>
#include <cstdint>
using stream_video::ScaleRgb565ToSquare;
int main() {
    // 4x2 的源，每像素两字节；缩放到 2x2 时只保留中间两列。
    std::uint8_t src[]={0,100, 1,101, 2,102, 3,103,
                        4,104, 5,105, 6,106, 7,107};
    std::uint8_t out[8]={};
    assert(ScaleRgb565ToSquare(src,sizeof(src),4,2,out,sizeof(out),2));
    std::uint8_t want[]={1,101,2,102,5,105,6,106};
    for (int i=0;i<8;i++) assert(out[i]==want[i]);
    assert(!ScaleRgb565ToSquare(src,sizeof(src)-1,4,2,out,sizeof(out),2));
    assert(!ScaleRgb565ToSquare(src,sizeof(src),4,2,out,sizeof(out)-1,2));
    assert(!ScaleRgb565ToSquare(nullptr,sizeof(src),4,2,out,sizeof(out),2));
    assert(!ScaleRgb565ToSquare(src,sizeof(src),4,2,out,sizeof(out),0));
}
'''
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'pixels.cc'
            source.write_text(code)
            binary = Path(directory) / 'pixels'
            subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-I', str(root / 'main/boards/common'),
                            str(source), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()
