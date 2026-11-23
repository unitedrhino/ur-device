"""编译生产无线观察核心与适配器；SDK替身仅验证安全边界，不代替硬件校准。"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class UrUdpTxTraceTest(unittest.TestCase):
    """执行实际C++解析/队列及回调适配，覆盖合法、损坏、溢出、关闭和并发。"""

    @classmethod
    def setUpClass(cls):
        """只替换SDK头依赖，实际解析与适配函数不在测试中重写。"""
        compiler = shutil.which("g++") or shutil.which("clang++")
        if compiler is None:
            raise RuntimeError("需要主机C++编译器")
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / "wifi_tx_trace"
        driver = (ROOT / "main/protocols/ur_udp_wifi_trace.cc").read_text()
        for line in driver.splitlines():
            if line.startswith("#include <esp_") or line == "#include <freertos/FreeRTOS.h>":
                driver = driver.replace(line, "")
        fixture = (ROOT / "scripts/tests/cpp/ur_udp_tx_trace_test.cc").read_text()
        result = subprocess.run(
            [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
             f"-I{ROOT / 'main/protocols'}", str(ROOT / "main/protocols/ur_ai_runtime.cc"),
             str(ROOT / "main/protocols/ur_udp_tx_trace_state.cc"), "-x", "c++", "-",
             "-o", str(cls.binary)], input=fixture.replace("// PRODUCTION_DRIVER", driver),
            text=True, capture_output=True)
        if result.returncode:
            raise RuntimeError(result.stderr)

    def run_case(self, name):
        """同一生产逻辑用例固定十次，任一次失败保留实际stderr并使测试失败。"""
        for iteration in range(10):
            with self.subTest(case=name, iteration=iteration):
                result = subprocess.run([str(self.binary), name], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_valid(self):
        """帧解析成功后只保存数值，不借用原回调缓冲。"""
        self.run_case("valid")

    def test_driver_failure(self):
        """实际失败状态不能转换成成功或端到端确认。"""
        self.run_case("driver_failure")

    def test_truncation(self):
        """所有截断边界和空指针都不得越界读或伪造目标结果。"""
        self.run_case("truncation")

    def test_scope(self):
        """错误路由及端口不能污染当前会话。"""
        self.run_case("scope")

    def test_invalid_layout(self):
        """未知布局、协议、分片及超限只能计数，不能猜测载荷偏移。"""
        self.run_case("invalid_layout")

    def test_header_invalid(self):
        """同路由的非法类型或声明长度必须显式保留为无效头。"""
        self.run_case("header_invalid")

    def test_reject_reasons(self):
        """拒绝原因与长度范围独立计数，不越界读取超限或空缓冲。"""
        self.run_case("reject_reasons")

    def test_shape_candidates(self):
        """头类型只用于候选计数，不启用新解析器、不伪造路由匹配，开启后重置。"""
        self.run_case("shape_candidates")

    def test_options_padding(self):
        """合法IPv4选项与Ethernet填充不改变载荷长度。"""
        self.run_case("options_padding")

    def test_mac_layout(self):
        """标准MAC头长、QoS/四地址/HT及固定SNAP只记候选，不产生语音事件。"""
        self.run_case("mac_layout")

    def test_mac_truncation(self):
        """逐字节截断标准MAC、LLC和网络头，不越界读或确认完整IP/UDP。"""
        self.run_case("mac_truncation")

    def test_mac_rejects(self):
        """错误偏移、安全头、分片、无载荷和A-MSDU不扫描或展开猜测。"""
        self.run_case("mac_rejects")

    def test_mac_network_bounds(self):
        """LLC标记不等于合法IP/UDP；版本、长度、协议和分片分别门禁。"""
        self.run_case("mac_network_bounds")

    def test_overflow(self):
        """队列容量固定，溢出计数且原有顺序保留。"""
        self.run_case("overflow")

    def test_lifecycle(self):
        """关闭、迟到结果、新代际和无效开启不混用会话。"""
        self.run_case("lifecycle")

    def test_capture_complete(self):
        """交接前复制数值、交接后记录返回码，原缓冲失效也不影响结果。"""
        self.run_case("capture_complete")

    def test_capture_late(self):
        """交接返回跨越关闭或新代际时不污染队列，未返回数量保持可见。"""
        self.run_case("capture_late")

    def test_adapter(self):
        """执行生产SDK适配，验证回调内无日志及幂等注销。"""
        self.run_case("adapter")

    def test_registration_failure(self):
        """注册失败只停用观察且不注销没有取得的资源。"""
        self.run_case("registration_failure")

    def test_unregistration_failure(self):
        """SDK注销失败时仍通过静态停用状态隔离迟到回调。"""
        self.run_case("unregistration_failure")

    def test_concurrent(self):
        """真实互斥替身覆盖回调与worker并发读取，质量计数不丢失。"""
        self.run_case("concurrent")


class UrUdpTxTraceBuildTest(unittest.TestCase):
    """只检查诊断开关隔离和前置拒绝，不启动Docker或真实构建。"""

    def rejected_build(self, **values):
        """使用非秘密占位输入检查拒绝必须发生在Docker之前。"""
        environment = dict(os.environ, UR_PRODUCT_ID="test", UR_PRODUCT_SECRET="test-not-a-secret",
                           UR_PLATFORM_BASE_URL="http://invalid.example", ENABLE_UR_AI_E2E_TEST="0",
                           ENABLE_UR_UDP_TX_TRACE="1")
        environment.update(values)
        result = subprocess.run(["bash", str(ROOT / "build-watcher.sh")], env=environment,
                                capture_output=True, text=True, timeout=5)
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("docker:", result.stderr)
        self.assertNotIn("test-not-a-secret", result.stdout + result.stderr)
        return result

    def test_requires_diagnostic_build(self):
        """正式入口不得启用私有Wi-Fi回调。"""
        self.assertIn("必须开启语音测试入口", self.rejected_build().stderr)

    def test_requires_fixed_sdk(self):
        """非已核验镜像不能套用私有接口布局假设。"""
        self.assertIn("固定IDF6", self.rejected_build(ENABLE_UR_AI_E2E_TEST="1", IDF_IMAGE="other").stderr)

    def test_rejects_invalid_flag(self):
        """无效配置不得静默退化为成功构建。"""
        self.assertIn("仅支持0或1", self.rejected_build(ENABLE_UR_UDP_TX_TRACE="2").stderr)

    def test_default_off_and_source_isolation(self):
        """静态合同检查不替代IDF实际编译及正式镜像符号检查。"""
        script = (ROOT / "build-watcher.sh").read_text()
        self.assertIn('ENABLE_UR_UDP_TX_TRACE="${ENABLE_UR_UDP_TX_TRACE:-0}"', script)
        kconfig = (ROOT / "main/Kconfig.projbuild").read_text().split("config UR_UDP_TX_TRACE", 1)[1]
        kconfig = kconfig.split("config UR_PLATFORM_BASE_URL", 1)[0]
        self.assertIn("depends on UR_AI_E2E_TEST", kconfig)
        self.assertIn("default n", kconfig)
        cmake = (ROOT / "main/CMakeLists.txt").read_text()
        self.assertIn('if(CONFIG_UR_UDP_TX_TRACE)\n    list(APPEND SOURCES "protocols/ur_udp_tx_trace_state.cc"', cmake)
        self.assertEqual(script.count("python scripts/check_wifi_tx_trace_owner.py --idf-root"), 2)

    def test_generated_trace_gate_accepts_hidden_disabled_option(self):
        """执行实际收尾门禁：依赖关闭时Kconfig省略选项，仍须拒绝显式启用或缺失配置文件。"""
        script = (ROOT / "build-watcher.sh").read_text()
        start = script.rindex('if [ "$ENABLE_UR_UDP_TX_TRACE" = "1" ]; then')
        end = script.index('\nfi &&', start) + len('\nfi')
        gate = script[start:end]
        cases = ((False, '', True), (False, '# CONFIG_UR_UDP_TX_TRACE is not set\n', True),
                 (False, 'CONFIG_UR_UDP_TX_TRACE=y\n', False),
                 (False, 'CONFIG_UR_UDP_TX_TRACE=n\n', False), (False, None, False),
                 (True, 'CONFIG_UR_UDP_TX_TRACE=y\n', True), (True, '', False))
        for enabled, configuration, expected in cases:
            with self.subTest(enabled=enabled, configuration=configuration), tempfile.TemporaryDirectory() as directory:
                if configuration is not None:
                    (Path(directory) / 'sdkconfig').write_text(configuration)
                result = subprocess.run(['bash', '-c', gate], cwd=directory,
                    env=dict(os.environ, ENABLE_UR_UDP_TX_TRACE='1' if enabled else '0'),
                    capture_output=True, text=True, timeout=5)
                self.assertEqual(result.returncode == 0, expected, result.stdout + result.stderr)

    def test_owner_gate(self):
        """用临时源树验证任意其他组件引用导致拒绝，不修改真实受管组件。"""
        import importlib.util
        specification = importlib.util.spec_from_file_location("tx_owner", ROOT / "scripts/check_wifi_tx_trace_owner.py")
        module = importlib.util.module_from_spec(specification)
        specification.loader.exec_module(module)
        with tempfile.TemporaryDirectory() as directory:
            project = Path(directory) / "project"
            idf = Path(directory) / "idf"
            own = project / "main/protocols/ur_udp_wifi_trace.cc"
            own.parent.mkdir(parents=True)
            own.write_text("esp_wifi_set_tx_done_cb(own_callback);")
            (own.parent / "ur_udp_netif_trace.cc").write_text("__wrap_esp_netif_transmit;")
            with self.assertRaises(ValueError):
                module.find_other_owners(project, idf)
            (idf / "components/esp_wifi/src").mkdir(parents=True)
            (idf / "components/esp_netif").mkdir(parents=True)
            self.assertEqual(module.find_other_owners(project, idf), [])
            foreign = project / "managed_components/network/owner.cc"
            foreign.parent.mkdir(parents=True)
            foreign.write_text("auto register_fn = &esp_wifi_set_tx_done_cb;")
            self.assertEqual(module.find_other_owners(project, idf), [str(foreign)])
            sdk_owner = idf / "components/esp_netif/owner.c"
            sdk_owner.write_text("esp_wifi_set_tx_done_cb(other_callback);")
            self.assertEqual(module.find_other_owners(project, idf), sorted([str(foreign), str(sdk_owner)]))

    def test_owner_gate_non_utf8_source(self):
        """受管相机及字体源码可含非UTF8注释，检查不得漏检ASCII回调引用。"""
        import importlib.util
        specification = importlib.util.spec_from_file_location("tx_owner_bytes", ROOT / "scripts/check_wifi_tx_trace_owner.py")
        module = importlib.util.module_from_spec(specification)
        specification.loader.exec_module(module)
        with tempfile.TemporaryDirectory() as directory:
            project = Path(directory) / "project"
            idf = Path(directory) / "idf"
            own = project / "main/protocols/ur_udp_wifi_trace.cc"
            own.parent.mkdir(parents=True)
            own.write_bytes(b"esp_wifi_set_tx_done_cb(own_callback);")
            (own.parent / "ur_udp_netif_trace.cc").write_text("__wrap_esp_netif_transmit;")
            (idf / "components/esp_wifi/src").mkdir(parents=True)
            (idf / "components/esp_netif").mkdir(parents=True)
            foreign = project / "managed_components/camera/sensor.h"
            foreign.parent.mkdir(parents=True)
            foreign.write_bytes(b"// \xa8\xff\n#define SENSOR_REGISTER 1\n")
            self.assertEqual(module.find_other_owners(project, idf), [])
            foreign.write_bytes(b"// \xa8\xff\nauto register_fn = &esp_wifi_set_tx_done_cb;\n")
            self.assertEqual(module.find_other_owners(project, idf), [str(foreign)])


if __name__ == "__main__":
    unittest.main()
