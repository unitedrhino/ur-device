"""编译执行生产 UDP 发送函数，验证测试取证不改变行为或泄露音频/凭据。"""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class UdpSendEvidenceTest(unittest.TestCase):
    """只替换套接字与加密硬件边界，保留生产发送和帧头构造。"""

    def run_configuration(self, enabled):
        """编译指定开关，并连续运行十次发送成功/失败/短返回与无效输入。"""
        compiler = shutil.which("g++") or shutil.which("clang++")
        self.assertIsNotNone(compiler, "需要主机 C++ 编译器")
        source = (ROOT / "main/protocols/udp_audio_channel.cc").read_text()
        start = source.index("bool UdpAudioChannel::SendPayload(")
        end = source.index("bool UdpAudioChannel::IsOpen()", start)
        fixture = (ROOT / "scripts/tests/cpp/udp_send_evidence_test.cc").read_text()
        code = fixture.replace("// PRODUCTION_SEND_PAYLOAD", source[start:end])
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "udp_send_test"
            built = subprocess.run(
                [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                 f"-DCONFIG_UR_AI_E2E_TEST={int(enabled)}", f"-I{ROOT / 'main/protocols'}",
                 str(ROOT / "main/protocols/ur_ai_runtime.cc"), "-x", "c++", "-", "-o", str(binary)],
                input=code, text=True, capture_output=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            if not enabled:
                self.assertNotIn(b"[UR_UDP_E2E]", binary.read_bytes(), "正式开关应排除取证字符串")
            for _ in range(10):
                result = subprocess.run([str(binary)], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_test_build_numeric_evidence(self):
        """测试包逐次记录真实序号/长度/发送返回值，不记录音频、密钥或nonce。"""
        self.run_configuration(True)

    def test_formal_build_without_evidence(self):
        """正式包没有测试日志，发送返回值与序号语义保持不变。"""
        self.run_configuration(False)


if __name__ == "__main__":
    unittest.main()
