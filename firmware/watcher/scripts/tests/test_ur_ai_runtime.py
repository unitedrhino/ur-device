"""编译运行固件共用的联犀 AI 协议核心单元与时序 E2E 测试。"""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


WATCHER_ROOT = Path(__file__).resolve().parents[2]
RUNTIME_SOURCE = WATCHER_ROOT / "main/protocols/ur_ai_runtime.cc"
CPP_TEST = WATCHER_ROOT / "scripts/tests/cpp/ur_ai_runtime_test.cc"


class UrAiRuntimeTest(unittest.TestCase):
    """运行生产协议核心，避免仅靠源码字符串断言形成假覆盖。"""

    @classmethod
    def setUpClass(cls):
        """使用主机 C++17 编译器构建一次测试程序，供两个套件复用。"""
        compiler = shutil.which("g++") or shutil.which("clang++")
        if compiler is None:
            raise RuntimeError("需要 g++ 或 clang++ 执行联犀 AI 固件运行时测试")
        cls._temporary_directory = tempfile.TemporaryDirectory()
        cls._binary = Path(cls._temporary_directory.name) / "ur_ai_runtime_test"
        subprocess.run(
            [
                compiler,
                "-std=c++17",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{WATCHER_ROOT / 'main/protocols'}",
                str(RUNTIME_SOURCE),
                str(CPP_TEST),
                "-o",
                str(cls._binary),
            ],
            check=True,
            text=True,
            capture_output=True,
        )

    @classmethod
    def tearDownClass(cls):
        """释放测试编译产物，不在源码目录留下临时文件。"""
        cls._temporary_directory.cleanup()

    def run_suite(self, suite):
        """运行指定套件，并要求输出清晰的通过标记。"""
        result = subprocess.run(
            [str(self._binary), suite],
            check=True,
            text=True,
            capture_output=True,
        )
        self.assertIn(f"PASS {suite}:", result.stdout)

    def test_production_protocol_unit_suite(self):
        """覆盖表情、关联、终态去重和 UDP 头的运行时边界。"""
        self.run_suite("unit")

    def test_devicesim_trace_e2e_suite(self):
        """回放预热、多轮、乱序、重复、打断和断线旧消息场景。"""
        self.run_suite("e2e")


if __name__ == "__main__":
    unittest.main()
