"""编译运行 Watcher 拍照上传纯逻辑，并连续复测关键门禁。"""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


WATCHER_ROOT = Path(__file__).resolve().parents[2]
RUNTIME_SOURCE = WATCHER_ROOT / "main/protocols/ur_photo_runtime.cc"
CPP_TEST = WATCHER_ROOT / "scripts/tests/cpp/ur_photo_runtime_test.cc"


class UrPhotoRuntimeTest(unittest.TestCase):
    """直接运行生产拍照核心，避免只用源码字符串断言。"""

    @classmethod
    def setUpClass(cls):
        """使用主机 C++17 编译器构建隔离测试程序。"""
        compiler = shutil.which("g++") or shutil.which("clang++")
        if compiler is None:
            raise RuntimeError("需要 g++ 或 clang++ 执行联犀拍照固件测试")
        cls._temporary_directory = tempfile.TemporaryDirectory()
        cls._binary = Path(cls._temporary_directory.name) / "ur_photo_runtime_test"
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
        """释放临时编译产物。"""
        cls._temporary_directory.cleanup()

    def test_photo_runtime_repeated(self):
        """连续 10 次覆盖 token、队列门禁、上传边界和凭据清零。"""
        for _ in range(10):
            result = subprocess.run(
                [str(self._binary)], check=True, text=True, capture_output=True
            )
            self.assertIn("PASS photo:", result.stdout)


if __name__ == "__main__":
    unittest.main()
