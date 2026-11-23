"""对生产图像解码器做差分与边界测试，验证失败帧不越过目标容量。"""
import base64
import ctypes
import random
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class ImageBase64Test(unittest.TestCase):
    """随机二进制、48KiB上限、非法编码及哨兵共同检查真实查表实现。"""

    def test_actual_decoder_matches_standard_base64_and_rejects_invalid_input(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        self.assertIsNotNone(compiler)
        output = ROOT.parents[1] / ".temp"
        output.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=output) as directory:
            library = Path(directory) / "decode.so"
            source = '''#include "image_base64.h"
extern "C" bool decode(unsigned char* target, size_t capacity, size_t* written,
                       const unsigned char* source, size_t size) {
    return watcher::DecodeImageBase64(target, capacity, written, source, size);
}
'''
            result = subprocess.run([
                compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O2",
                "-fsanitize=undefined", "-shared", "-fPIC", "-x", "c++", "-",
                f"-I{ROOT / 'main/boards/sensecap-watcher'}", "-o", str(library),
            ], input=source, text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            decode = ctypes.CDLL(str(library)).decode
            decode.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t),
                               ctypes.c_void_p, ctypes.c_size_t]
            decode.restype = ctypes.c_bool

            def run(encoded, capacity):
                """目标首尾保留哨兵，失败不能暴露非零的成功长度。"""
                buffer = (ctypes.c_ubyte * (capacity + 2))(*([0xA5] * (capacity + 2)))
                written = ctypes.c_size_t(123456)
                ok = decode(ctypes.byref(buffer, 1), capacity, ctypes.byref(written),
                            encoded, len(encoded))
                self.assertEqual((buffer[0], buffer[-1]), (0xA5, 0xA5))
                self.assertLessEqual(written.value, capacity)
                if not ok:
                    self.assertEqual(written.value, 0)
                return ok, bytes(buffer[1:written.value + 1])

            rng = random.Random(8675)
            for size in list(range(257)) + [1023, 1024, 1025, 3672, 48 * 1024 - 1, 48 * 1024]:
                raw = bytes(rng.randrange(256) for _ in range(size))
                encoded = base64.b64encode(raw)
                for capacity in {size, size + 1, 48 * 1024}:
                    self.assertEqual(run(encoded, capacity), (True, raw))
                if size:
                    self.assertEqual(run(encoded, size - 1), (False, b""))
            for encoded in [b"=", b"A", b"AA", b"AAA", b"====", b"A===", b"AA=A",
                            b"Zg==AAAA", b"AAAA=AAA", b"Zh==", b"Zm9=", b"Zg==\n",
                            b"Zm 9v", b"Zm9v\r\n", b"____", b"----", b"AA\xffA", b"\0AAA"]:
                self.assertEqual(run(encoded, 48 * 1024), (False, b""), repr(encoded))
            # 每个字节值、四个字符位置都必须符合字母表与规范填充语义。
            for index in range(4):
                for value in range(256):
                    encoded = bytearray(b"AAAA")
                    encoded[index] = value
                    try:
                        expected = base64.b64decode(encoded, validate=True)
                        valid = base64.b64encode(expected) == encoded
                    except ValueError:
                        valid = False
                    ok, decoded = run(bytes(encoded), 3)
                    self.assertEqual(ok, valid, repr(encoded))
                    if valid:
                        self.assertEqual(decoded, expected)
            written = ctypes.c_size_t(5)
            self.assertFalse(decode(None, 3, ctypes.byref(written), b"AAAA", 4))
            self.assertEqual(written.value, 0)
            self.assertFalse(decode(None, 3, ctypes.byref(written), None, 4))
            self.assertFalse(decode(None, 3, None, b"AAAA", 4))
