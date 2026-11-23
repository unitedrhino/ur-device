"""复现 SPD2010 零长度发送导致的 IDF 6 触摸启动失败。"""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from prepare_touch_i2c import READ_OLD, READ_NEW, transform

# HARNESS 限定实际配置：无控制字节、无命令字节；IDF 6 不接受零长度 transmit_receive。
HARNESS = r'''
#include <stddef.h>
struct Touch { void *io; } device, *tp = &device;
int reads = 0;
int esp_lcd_panel_io_rx_param(void *io, int command, void *data, size_t length) {
    if (!data || !length) return 2;
    // 此配置中 command=0 进入零长度发送分支，command=-1 才进入纯接收。
    if (command != -1) return 1;
    ++reads;
    return 0;
}
int read_register(void) {
    unsigned char buffer[18];
    void *data_p = buffer;
    size_t len = sizeof(buffer);
    return CALL;
}
int main(void) { return read_register() || reads != 1; }
'''


class TouchI2CTest(unittest.TestCase):
    """执行真实驱动调用表达式；旧入口必须失败，新入口反复成功且拒绝依赖漂移。"""

    def test_read_contract(self):
        """从受管理驱动提取实际读调用，旧配置稳定失败，新配置连续十次通过。"""
        root = Path(__file__).resolve().parents[2]
        driver = root / 'managed_components/espressif__esp_lcd_touch_spd2010/esp_lcd_touch_spd2010.c'
        if driver.exists():
            source = driver.read_text()
            self.assertEqual(source.count(READ_OLD), 1)
            self.assertIn(READ_NEW, transform(source))
        with tempfile.TemporaryDirectory() as directory:
            for expression, expected in ((READ_OLD, 1), (transform(READ_OLD), 0)):
                binary = str(Path(directory) / 'touch')
                subprocess.run(['cc', '-x', 'c', '-', '-o', binary],
                               input=HARNESS.replace('CALL', expression), text=True, check=True,
                               capture_output=True)
                for _ in range(10):
                    self.assertEqual(subprocess.run([binary]).returncode, expected)

    def test_dependency_drift(self):
        """不存在、重复或已经修改的入口不得被构建生成器静默接受。"""
        for source in ('', READ_OLD * 2, READ_NEW, READ_OLD + READ_NEW):
            with self.assertRaises(ValueError):
                transform(source)

    def test_preserve_other_operations(self):
        """只修改读命令标志，不改变寄存器写、数据长度、延时及其他上游实现。"""
        before = 'write_register();\n' + READ_OLD + ';\ndelay();'
        self.assertEqual(transform(before), 'write_register();\n' + READ_NEW + ';\ndelay();')


if __name__ == '__main__':
    unittest.main()
