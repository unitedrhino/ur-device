"""验证 Watcher 链接拦截旧 HAL 的 Token 后断连，而不是修改受管理 SDK。"""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class TokenLifetimeTest(unittest.TestCase):
    """真实 C 链接回归：Token 回调不得提前调用 SDK 射频释放。"""

    def test_token_does_not_close_gatt(self):
        """提取生产 wrapper 并通过 GNU --wrap 验证实际调用仍保留连接。"""
        source = (ROOT / 'main/unitedrhino/ur_ble_provision.c').read_text()
        wrapper = re.search(r'void __wrap_ble_combo_release_radio_after_token\(void\)\s*\{[^}]*\}', source)
        self.assertIsNotNone(wrapper)
        self.assertIn('--wrap=ble_combo_release_radio_after_token', (ROOT / 'CMakeLists.txt').read_text())
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'wrapper.c').write_text('#define ESP_LOGI(...) ((void)0)\n' + wrapper.group())
            (path / 'caller.c').write_text('void ble_combo_release_radio_after_token(void); int main(void) { ble_combo_release_radio_after_token(); return 0; }')
            (path / 'sdk.c').write_text('#include <stdlib.h>\nvoid ble_combo_release_radio_after_token(void) { abort(); }')
            subprocess.run(['cc', str(path / 'wrapper.c'), str(path / 'caller.c'), str(path / 'sdk.c'),
                            '-Wl,--wrap=ble_combo_release_radio_after_token', '-o', str(path / 'test')], check=True)
            subprocess.run([str(path / 'test')], check=True)
