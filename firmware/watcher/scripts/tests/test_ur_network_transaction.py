"""编译真实换网事务核心，反复注入存储失败、旧回执和异常重启。"""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class NetworkTransactionTest(unittest.TestCase):
    """内存存储替身只验证状态合同，NVS/真实断电仍须设备测试。"""

    def test_wifi_recency_and_eviction_ten_times(self):
        """真实排序持久化及十槽淘汰，遍历 37 个提交失败边界并重复十次。"""
        compiler = shutil.which("g++") or shutil.which("clang++")
        self.assertIsNotNone(compiler, "需要主机 C++ 编译器")
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / "recency-test")
            subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            f"-I{ROOT / 'main/unitedrhino'}",
                            f"-I{ROOT / 'scripts/tests/fixtures/network_store'}",
                            str(ROOT / "scripts/tests/cpp/ur_wifi_recency_test.cc"),
                            "-o", binary], check=True, capture_output=True, text=True)
            for _ in range(10):
                subprocess.run([binary], check=True, capture_output=True)

    def test_production_c_bridge_ten_times(self):
        """链接实际桥接源文件，验证受控启动、连接代次及各终态。"""
        compiler = shutil.which("g++") or shutil.which("clang++")
        self.assertIsNotNone(compiler, "需要主机 C++ 编译器")
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / "bridge-test")
            subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
                            f"-I{ROOT / 'main/unitedrhino'}",
                            f"-I{ROOT / 'scripts/tests/fixtures/network_store'}",
                            str(ROOT / "main/unitedrhino/ur_network.cc"),
                            str(ROOT / "scripts/tests/cpp/ur_network_bridge_test.cc"),
                            "-o", binary], check=True, capture_output=True, text=True)
            for _ in range(10):
                for scenario in ("success", "cold", "unmarked", "timeout", "auth", "confirmation", "recency", "binding", "binding-cold"):
                    subprocess.run([binary, scenario], check=True, capture_output=True)

    def test_runtime_reboot_and_cloud_confirmation_ten_times(self):
        """回放完整持久化协调链及 37 个失败边界，不代替无线/平台 E2E。"""
        compiler = shutil.which("g++") or shutil.which("clang++")
        self.assertIsNotNone(compiler, "需要主机 C++ 编译器")
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / "runtime-test")
            subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            f"-I{ROOT / 'main/unitedrhino'}",
                            f"-I{ROOT / 'scripts/tests/fixtures/network_store'}",
                            str(ROOT / "scripts/tests/cpp/ur_network_runtime_test.cc"),
                            "-o", binary], check=True, capture_output=True, text=True)
            for _ in range(10):
                subprocess.run([binary], check=True, capture_output=True)

    def test_real_store_partial_writes_ten_times(self):
        """真实适配器在每个多键写入边界失败后可恢复，重复十次。"""
        compiler = shutil.which("g++") or shutil.which("clang++")
        self.assertIsNotNone(compiler, "需要主机 C++ 编译器")
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / "store-test")
            subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            f"-I{ROOT / 'main/unitedrhino'}",
                            f"-I{ROOT / 'scripts/tests/fixtures/network_store'}",
                            str(ROOT / "scripts/tests/cpp/ur_network_store_test.cc"),
                            "-o", binary], check=True, capture_output=True, text=True)
            for _ in range(10):
                subprocess.run([binary], check=True, capture_output=True)

    def test_transaction_faults_ten_times(self):
        """重启恢复、成功提交、凭据隔离与 90/180 秒边界重复十次。"""
        compiler = shutil.which("g++") or shutil.which("clang++")
        self.assertIsNotNone(compiler, "需要主机 C++ 编译器")
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / "network-test")
            subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            f"-I{ROOT / 'main/unitedrhino'}",
                            str(ROOT / "scripts/tests/cpp/ur_network_transaction_test.cc"),
                            "-o", binary], check=True, capture_output=True, text=True)
            for _ in range(10):
                subprocess.run([binary], check=True, capture_output=True)
