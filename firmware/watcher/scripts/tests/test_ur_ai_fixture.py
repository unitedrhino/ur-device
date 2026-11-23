"""编译真实固件音频包解析器，验证查询/拍照/控制资源及损坏包拒绝边界。"""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


class AudioFixtureTest(unittest.TestCase):
    """替换链接资源和日志边界，生产构造器与 Next 原样参与测试。"""

    def test_real_assets_and_invalid_packages(self):
        """连续十次遍历真实样本；截断、空帧、过大帧和尾随数据均不得可读。"""
        volume = (ROOT / "main/testdata/ur_ai_query_volume.opuspack").read_bytes()
        photo = (ROOT / "main/testdata/ur_ai_take_photo.opuspack").read_bytes()
        control = (ROOT / "main/testdata/ur_ai_control_volume.opuspack").read_bytes()
        forward = (ROOT / "main/testdata/ur_ai_tank_forward.opuspack").read_bytes()
        compiler = shutil.which("g++") or shutil.which("clang++")
        self.assertIsNotNone(compiler)
        invalid = [photo[:8], photo[:-1], photo + b"extra",
                   photo[:12] + b"\x00\x00" + photo[14:],
                   photo[:12] + b"\xff\xff" + photo[14:],
                   photo[:10] + b"\x00\x00" + photo[12:]]
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory)
            (target / "esp_log.h").write_text(
                '#pragma once\ninline void FixtureLog(const char*, const char*, ...) {}\n'
                '#define ESP_LOGE(tag, ...) FixtureLog(tag, __VA_ARGS__)\n')
            # 链接符号与 IDF EMBED_FILES 一致，不用另一份解析器生成预期结果。
            driver = target / "driver.cc"
            driver.write_text('''
/* 测试三种资源游标独立且边界完整，不能用静态格式检查冒充实际解析。 */
#include "ur_ai_e2e_fixture.h"
#include <cassert>
int main(int argc, char**) {
    for (int repeat = 0; repeat < 10; ++repeat) {
        UrAiE2eFixture volume;
        UrAiE2eFixture photo(true);
        UrAiE2eFixture control(false, true);
        UrAiE2eFixture forward(false, false, true);
        UrAiE2eFixture conflict(true, true);
        UrAiE2eFixture forward_conflict(true, false, true);
        assert(forward.IsValid() && !forward_conflict.IsValid());
        assert(volume.IsValid());
        assert(control.IsValid());
        assert(!conflict.IsValid());
        UrAiE2eAudioFrame invalid_frame{};
        assert(!conflict.Next(&invalid_frame));
        assert(photo.IsValid() == (argc == 1));
        if (!photo.IsValid()) {
            UrAiE2eAudioFrame frame{};
            assert(!photo.Next(&frame));
        }
        for (auto* fixture : {&volume, &photo, &control, &forward}) {
            if (!fixture->IsValid()) continue;
            assert(fixture->sample_rate() == 16000 && fixture->frame_duration_ms() == 60);
            assert(fixture->frame_count() == (fixture == &forward ? 28 : (fixture == &photo ? 112 : (fixture == &control ? 40 : 29))));
            UrAiE2eAudioFrame frame{};
            assert(!fixture->Next(nullptr));
            int frames = 0;
            while (fixture->Next(&frame)) {
                assert(frame.data && frame.size > 0 && frame.size <= 4096);
                ++frames;
            }
            assert(frames == fixture->frame_count() && !fixture->Next(&frame));
        }
    }
}
'''.replace('#include <cassert>', '#include <cassert>\n#include <initializer_list>'))
            for index, payload in enumerate([photo] + invalid):
                with self.subTest(package=index):
                    assembly = target / "assets.S"
                    chunks = []
                    for name, data in (("query_volume", volume), ("take_photo", payload), ("control_volume", control), ("tank_forward", forward)):
                        chunks.append(
                            f'.section .rodata\n.global _binary_ur_ai_{name}_opuspack_start\n'
                            f'.global _binary_ur_ai_{name}_opuspack_end\n'
                            f'_binary_ur_ai_{name}_opuspack_start:\n.byte '
                            + ','.join(str(byte) for byte in data)
                            + f'\n_binary_ur_ai_{name}_opuspack_end:\n')
                    assembly.write_text(''.join(chunks) + '.section .note.GNU-stack,"",@progbits\n')
                    binary = target / "fixture-test"
                    subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                                    "-I", str(target), "-I", str(ROOT / "main/protocols"),
                                    str(ROOT / "main/protocols/ur_ai_e2e_fixture.cc"),
                                    str(driver), str(assembly), "-o", str(binary)],
                                   capture_output=True, check=True)
                    subprocess.run([str(binary)] + ([] if index == 0 else ["invalid"]),
                                   capture_output=True, check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
