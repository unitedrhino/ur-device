"""执行真实SDP边界函数，防止多网卡候选写穿libpeer固定数组。"""
import ctypes
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class AVProbeSDPTest(unittest.TestCase):
    """覆盖崩溃关联的候选数量、行缓冲和IPv4限制，不替换实际边界函数。"""

    def test_actual_sdp_guard_rejects_parser_overflow_inputs(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        self.assertIsNotNone(compiler)
        production = (ROOT / "main/boards/sensecap-watcher/ur_av_probe.cc").read_text()
        function = production.split("bool ValidProbeSDP(", 1)[1].split("// RunRTC", 1)[0]
        source = '''#include <string_view>
#include <string>
#include <vector>
#include <arpa/inet.h>
#define lwip_inet_pton inet_pton
bool ValidProbeSDP(''' + function + '''
extern "C" bool validate(const char* data, size_t size) {
    return ValidProbeSDP(std::string_view(data, size));
}
'''
        output = ROOT.parents[1] / ".temp"
        output.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=output) as directory:
            library = Path(directory) / "sdp.so"
            result = subprocess.run([
                compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-O2",
                "-fsanitize=undefined", "-shared", "-fPIC", "-x", "c++", "-", "-o", str(library),
            ], input=source, text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            validate = ctypes.CDLL(str(library)).validate
            validate.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
            validate.restype = ctypes.c_bool

            def check(text, expected):
                """按实际字节长度调用，包含NUL时不能被C字符串截断掩盖。"""
                self.assertEqual(validate(text, len(text)), expected)

            candidate = b"a=candidate:1234 1 udp 2122260223 192.168.1.2 50000 typ host\r\n"
            check(b"v=0\r\n" + candidate, True)
            check(candidate * 8, True)
            check(candidate * 9, False)
            check(candidate * 12, False)
            check(b"v=0\r\n" + b"x" * 256 + b"\r\n" + candidate, False)
            check(candidate.replace(b"1234", b"9" * 33), False)
            check(candidate.replace(b"192.168.1.2", b"::1"), False)
            check(candidate.replace(b"udp", b"tcp"), False)
            check(candidate + b"truncated", False)
            check(candidate + b"\x00\r\n", False)
            check(b"v=0\r\n", False)
            check(candidate + b"x" * 16384, False)
