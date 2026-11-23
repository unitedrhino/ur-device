"""验证语音模型试验不替换现有 WakeNet 唤醒，并拒绝缺失的模型。"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class SparkBotVoiceModelSelectionTest(unittest.TestCase):
    """抽取 AFE 生产分支，分别验证试验和默认构建的模型选择。"""

    def test_wakenet_is_preserved_only_in_explicit_test_profile(self):
        source = (ROOT / 'main/audio/engines/afe_audio_engine.cc').read_text()
        begin = source.index('#if CONFIG_UR_SPARKBOT_VOICE_MODEL_TEST')
        end = source.index('    if (use_multinet_for_wake) {', begin)
        actual = source[begin:end]
        code = r'''
#include <cassert>
#define ESP_LOGE(...) ((void)0)
int select(bool wn, bool mn) {
 const char* wakenet_model_name = wn ? "wn" : nullptr;
 const char* multinet_model_name = mn ? "mn" : nullptr;
''' + actual.replace('return false;', 'return -1;') + r'''
 return use_multinet_for_wake ? 1 : (wakenet_model_name ? 2 : 0);
}
int main() {
#if CONFIG_UR_SPARKBOT_VOICE_MODEL_TEST
 assert(select(true, true)==2);
 assert(select(false, true)==-1 && select(true, false)==-1 && select(false, false)==-1);
#else
 assert(select(true, true)==1 && select(false, true)==1);
 assert(select(true, false)==2 && select(false, false)==0);
#endif
}
'''
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / 'selection')
            for enabled in (0, 1):
                compiled = subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                           f'-DCONFIG_UR_SPARKBOT_VOICE_MODEL_TEST={enabled}',
                                           '-x', 'c++', '-', '-o', binary], input=code,
                                          text=True, capture_output=True)
                self.assertEqual(compiled.returncode, 0, compiled.stderr)
                subprocess.run([binary], check=True)


if __name__ == '__main__':
    unittest.main()
