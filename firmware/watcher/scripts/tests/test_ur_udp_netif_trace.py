"""执行生产网络交接包装器与解析核心，SDK替身不能证明真机链路已校准。"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class UrUdpNetifTraceTest(unittest.TestCase):
    """验证两条交接路径、借用缓冲生命周期、代际、错误码和有界输出。"""

    @classmethod
    def setUpClass(cls):
        """直接编译生产包装器，替换ESP头依赖与硬件函数，不另写观察实现。"""
        compiler = shutil.which("g++") or shutil.which("clang++")
        if compiler is None:
            raise RuntimeError("需要主机C++编译器")
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / "netif_tx_trace"
        adapter = (ROOT / "main/protocols/ur_udp_netif_trace.cc").read_text()
        adapter = "\n".join(line for line in adapter.splitlines()
                            if not line.startswith("#include <esp_")
                            and line != "#include <freertos/FreeRTOS.h>")
        fixture = (ROOT / "scripts/tests/cpp/ur_udp_netif_trace_test.cc").read_text()
        result = subprocess.run(
            [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
             f"-I{ROOT / 'main/protocols'}", str(ROOT / "main/protocols/ur_ai_runtime.cc"),
             str(ROOT / "main/protocols/ur_udp_tx_trace_state.cc"), "-x", "c++", "-",
             "-o", str(cls.binary)], input=fixture.replace("// PRODUCTION_NETIF", adapter),
            text=True, capture_output=True)
        if result.returncode:
            raise RuntimeError(result.stderr)

    def run_case(self, name):
        """关键交接纯逻辑固定十次，失败立即保留真实stderr，不重跑覆盖。"""
        for iteration in range(10):
            with self.subTest(case=name, iteration=iteration):
                result = subprocess.run([str(self.binary), name], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_passthrough(self):
        """关闭或其他接口的全部参数、调用次数与原始返回码完全不变。"""
        self.run_case("passthrough")

    def test_results(self):
        """两条路径保留ESP_OK、驱动失败与负值，交接上下文没有日志。"""
        self.run_case("results")

    def test_consumed_buffer(self):
        """原函数消费或改写借用缓冲后仍只读取预先复制的数值。"""
        self.run_case("consumed_buffer")

    def test_late_return(self):
        """关闭/新代际之间返回的旧交接不借用会话或污染当前结果。"""
        self.run_case("late_return")

    def test_boundaries(self):
        """截断、空指针、错误路由和端口不伪造目标，也不阻止原函数。"""
        self.run_case("boundaries")

    def test_overflow(self):
        """固定32项队列溢出明确计数，不打印借用帧或静默补齐。"""
        self.run_case("overflow")

    def test_concurrent(self):
        """真实线程互斥核验两路径并发调用和worker有界排空。"""
        self.run_case("concurrent")

    def test_unavailable(self):
        """接口不存在、无效路由及幂等结束不创建驱动或接管其他接口。"""
        self.run_case("unavailable")

    def test_build_contract(self):
        """包装源及两个链接选项仅出现在原默认关闭的诊断分支。"""
        cmake = (ROOT / "main/CMakeLists.txt").read_text()
        source_branch = cmake.split("if(CONFIG_UR_UDP_TX_TRACE)", 1)[1].split("endif()", 1)[0]
        self.assertIn('"protocols/ur_udp_netif_trace.cc"', source_branch)
        link_branch = cmake.split("if(CONFIG_UR_UDP_TX_TRACE)", 2)[2].split("endif()", 1)[0]
        for name in ("esp_netif_transmit", "esp_netif_transmit_wrap"):
            self.assertIn(f'"-Wl,--wrap={name}"', link_branch)
        channel = (ROOT / "main/protocols/udp_audio_channel.cc").read_text()
        self.assertIn("UrUdpNetifTraceStart(", channel)
        self.assertIn("UrUdpNetifTraceStop();", channel)
        self.assertIn("UrUdpNetifTraceFlush();", (ROOT / "main/protocols/ur_ai_protocol.cc").read_text())
        self.assertIn("python scripts/check_udp_tx_trace_symbols.py --elf build/xiaozhi.elf",
                      (ROOT / "build-watcher.sh").read_text())

    def test_owner_gate(self):
        """其他C/C++包装器、CMake接管及扫描范围缺失均拒绝，包括非UTF8源码。"""
        import importlib.util
        spec = importlib.util.spec_from_file_location("netif_owner", ROOT / "scripts/check_wifi_tx_trace_owner.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        with tempfile.TemporaryDirectory() as directory:
            project = Path(directory) / "project"
            idf = Path(directory) / "idf"
            own = project / "main/protocols/ur_udp_wifi_trace.cc"
            own.parent.mkdir(parents=True)
            own.write_text("esp_wifi_set_tx_done_cb(own_callback);")
            (idf / "components/esp_wifi/src").mkdir(parents=True)
            (idf / "components/esp_netif").mkdir(parents=True)
            with self.assertRaisesRegex(ValueError, "netif"):
                module.find_other_owners(project, idf)
            (own.parent / "ur_udp_netif_trace.cc").write_text("__wrap_esp_netif_transmit; __wrap_esp_netif_transmit_wrap;")
            (project / "main/CMakeLists.txt").write_text("--wrap=esp_netif_transmit --wrap=esp_netif_transmit_wrap")
            self.assertEqual(module.find_other_owners(project, idf), [])
            foreign = project / "managed_components/driver/wrapper.cc"
            foreign.parent.mkdir(parents=True)
            foreign.write_bytes(b"// vendor comment \xff\n__wrap_esp_netif_transmit_wrap;")
            foreign_cmake = foreign.parent / "CMakeLists.txt"
            foreign_cmake.write_text("-Wl,--wrap,esp_netif_transmit")
            self.assertEqual(module.find_other_owners(project, idf), sorted([str(foreign), str(foreign_cmake)]))

    def test_elf_symbols(self):
        """实际ELF判定器拒绝缺符号、仅未定义引用及正式包误链接，不要求保留未调用分支。"""
        import importlib.util
        spec = importlib.util.spec_from_file_location("netif_symbols", ROOT / "scripts/check_udp_tx_trace_symbols.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        names = ("UrUdpNetifTraceStart(unsigned char const*, unsigned long, int)",
                 "UrUdpNetifTraceStop()", "UrUdpNetifTraceFlush()", "UrUdpWifiTraceStart()",
                 "__wrap_esp_netif_transmit_wrap")
        for _ in range(10):
            text = "\n".join("42000000 T " + name for name in names)
            module.validate_symbols(text, True)
            module.validate_symbols("42000000 T esp_netif_transmit_wrap", False)
            with self.assertRaises(ValueError):
                module.validate_symbols("", False)
            for omitted in names:
                missing = "\n".join("42000000 T " + name for name in names if name != omitted)
                with self.assertRaises(ValueError):
                    module.validate_symbols(missing + "\n         U " + omitted, True)
            for name in names:
                with self.assertRaises(ValueError):
                    module.validate_symbols("42000000 T " + name, False)


if __name__ == "__main__":
    unittest.main()
