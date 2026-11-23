"""使用 LVGL 行为替身编译真实页面管理器；不冒充设备动画或帧率验收。"""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / "main/boards/sensecap-watcher"


class WatcherPageManagerTest(unittest.TestCase):
    """连续运行生产页面管理逻辑，覆盖移植边界。"""

    def test_menu_model_repeated(self):
        """菜单业务状态机连续十次覆盖互斥、危险确认与超时边界。"""
        compiler = shutil.which("g++") or shutil.which("clang++")
        self.assertIsNotNone(compiler, "需要主机 C++ 编译器")
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / "watcher-menu-test")
            subprocess.run([
                compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                f"-I{BOARD}", str(ROOT / "scripts/tests/cpp/watcher_menu_model_test.cc"),
                "-o", binary,
            ], check=True, capture_output=True, text=True)
            for _ in range(10):
                result = subprocess.run([binary], check=True, capture_output=True, text=True)
                self.assertIn("PASS watcher menu model", result.stdout)

    def test_page_manager_repeated(self):
        """焦点恢复、输入互斥、初始化失败和时间回绕重复十次。"""
        compiler = shutil.which("g++") or shutil.which("clang++")
        self.assertIsNotNone(compiler, "需要主机 C++ 编译器")
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / "watcher-ui-test")
            subprocess.run([
                compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                f"-I{ROOT / 'scripts/tests/fixtures/watcher_ui'}", f"-I{BOARD}",
                str(BOARD / "watcher_page_manager.cc"),
                str(ROOT / "scripts/tests/cpp/watcher_page_manager_test.cc"),
                "-o", binary,
            ], check=True, capture_output=True, text=True)
            for _ in range(10):
                result = subprocess.run([binary], check=True, capture_output=True, text=True)
                self.assertIn("PASS watcher page manager", result.stdout)
