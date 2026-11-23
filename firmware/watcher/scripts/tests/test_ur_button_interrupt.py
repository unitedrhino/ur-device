"""编译并执行生产按键函数，验证无 audioDone 的取消流程能够恢复监听。"""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

WATCHER_ROOT = Path(__file__).resolve().parents[2]


class ButtonInterruptTest(unittest.TestCase):
    """只替换硬件边界，真实按键分支来自 application.cc。"""

    def test_cancel_without_terminal_restarts_listening(self):
        """取消无终态、继续监听、再次结束会话及空闲启动均可执行。"""
        source = (WATCHER_ROOT / "main/application.cc").read_text()
        start = source.index("void Application::EndConversation()")
        end = source.index("void Application::ContinueOpenAudioChannel", start)
        fixture = (WATCHER_ROOT / "scripts/tests/cpp/ur_button_interrupt_test.cc").read_text()
        code = fixture.replace("// PRODUCTION_BUTTON_HANDLER", source[start:end])
        compiler = shutil.which("g++") or shutil.which("clang++")
        self.assertIsNotNone(compiler, "需要主机 C++ 编译器")
        with tempfile.TemporaryDirectory() as temp:
            binary = str(Path(temp) / "button_test")
            for enabled in (0, 1):
                build = code.replace("#define CONFIG_UR_AI_SIGNALING_WEBRTC 1",
                                     f"#define CONFIG_UR_AI_SIGNALING_WEBRTC {enabled}")
                subprocess.run([compiler, "-x", "c++", "-std=c++17", "-Wall", "-Wextra",
                                "-Werror", "-", "-o", binary], input=build, text=True,
                               capture_output=True, check=True)
                subprocess.run([binary], check=True, capture_output=True)
