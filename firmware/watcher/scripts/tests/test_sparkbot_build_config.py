"""检查正式构建入口的板型与分区隔离，不运行 Docker 或暴露真实凭据。"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class SparkBotBuildConfigTest(unittest.TestCase):
    """替换 Docker，验证生产脚本传递的配置与不支持形态的拒绝。"""

    def run_build(self, board, **flags):
        with tempfile.TemporaryDirectory() as directory:
            docker = Path(directory) / 'docker'
            docker.write_text('#!/bin/sh\nprintf "%s\\n" "$@"\n')
            docker.chmod(0o700)
            env = os.environ.copy()
            env.update(PATH=directory + os.pathsep + env.get('PATH', ''),
                       UR_PRODUCT_ID='test-id', UR_PRODUCT_SECRET='test-secret',
                       UR_PLATFORM_BASE_URL='https://example.invalid', UR_BOARD=board,
                       ENABLE_UR_AI_E2E_TEST='0', ENABLE_UR_AI_SIGNALING='0',
                       ENABLE_UR_AI_SIGNALING_DEVICE_AEC='0', ENABLE_UR_AI_SIGNALING_WEBRTC='0',
                       ENABLE_UR_AI_SIGNALING_WEBRTC_VIDEO='0', ENABLE_UR_UDP_TX_TRACE='0')
            env.update(flags)
            return subprocess.run(['bash', str(ROOT / 'build-watcher.sh')],
                                  env=env, text=True, capture_output=True)

    def test_board_config_and_partition_are_separate(self):
        tank = self.run_build('espressif/esp-sparkbot')
        self.assertEqual(tank.returncode, 0, tank.stderr)
        for value in ('UR_BOARD_NAME=esp-sparkbot', 'UR_APP_OFFSET=0x20000',
                      'UR_PARTITION_CSV=partitions/v2/16m.csv',
                      'UR_FLASHSIZE_CONFIG=CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y',
                      'UR_TICK_CONFIG=CONFIG_FREERTOS_HZ=100'):
            self.assertIn(value, tank.stdout)
        watcher = self.run_build('sensecap-watcher')
        self.assertEqual(watcher.returncode, 0, watcher.stderr)
        for value in ('UR_BOARD_NAME=sensecap-watcher', 'UR_APP_OFFSET=0x200000',
                      'UR_PARTITION_CSV=partitions/v2/32m-video.csv',
                      'UR_APP_PART_BYTES=6291456',
                      'UR_TICK_CONFIG=CONFIG_FREERTOS_HZ=1000'):
            self.assertIn(value, watcher.stdout)

    def test_tank_rejects_partial_signaling_profiles(self):
        for flags in ({'ENABLE_UR_AI_SIGNALING': '1'},
                      {'ENABLE_UR_AI_SIGNALING_DEVICE_AEC': '1'},
                      {'ENABLE_UR_AI_SIGNALING_WEBRTC': '1'},
                      {'ENABLE_UR_AI_SIGNALING_WEBRTC_VIDEO': '1'}):
            result = self.run_build('espressif/esp-sparkbot', **flags)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('必须同时启用', result.stderr)
            self.assertEqual(result.stdout, '')
        result = self.run_build('espressif/esp-sparkbot', ENABLE_UR_UDP_TX_TRACE='1')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('不允许启用 Wi-Fi 发送诊断', result.stderr)
        self.assertEqual(result.stdout, '')

    def test_tank_formal_signaling_profile_is_allowed_without_e2e(self):
        result = self.run_build(
            'espressif/esp-sparkbot',
            ENABLE_UR_AI_SIGNALING='1',
            ENABLE_UR_AI_SIGNALING_DEVICE_AEC='1',
            ENABLE_UR_AI_SIGNALING_WEBRTC='1',
            ENABLE_UR_AI_SIGNALING_WEBRTC_VIDEO='1',
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('UR_BOARD_NAME=esp-sparkbot', result.stdout)
        self.assertIn('ENABLE_UR_AI_E2E_TEST=0', result.stdout)
        self.assertIn('ENABLE_UR_AI_SIGNALING_WEBRTC_VIDEO=1', result.stdout)

    def test_makefile_selects_tank_aec_for_video_profile(self):
        tank = subprocess.run(
            ['make', '-n', 'build', 'BOARD=espressif/esp-sparkbot',
             'FLAVOR=webrtc-audio-video'],
            cwd=ROOT, text=True, capture_output=True, check=True)
        self.assertIn('ENABLE_UR_AI_SIGNALING_DEVICE_AEC=1', tank.stdout)
        watcher = subprocess.run(
            ['make', '-n', 'build', 'BOARD=sensecap-watcher',
             'FLAVOR=webrtc-audio-video'],
            cwd=ROOT, text=True, capture_output=True, check=True)
        self.assertIn('ENABLE_UR_AI_SIGNALING_DEVICE_AEC=0', watcher.stdout)

    def test_tank_signal_test_build_requires_complete_test_profile(self):
        flags = {'ENABLE_UR_AI_E2E_TEST': '1', 'ENABLE_UR_AI_SIGNALING': '1',
                 'ENABLE_UR_AI_SIGNALING_DEVICE_AEC': '1', 'ENABLE_UR_AI_SIGNALING_WEBRTC': '1',
                 'ENABLE_UR_AI_SIGNALING_WEBRTC_VIDEO': '1'}
        result = self.run_build('espressif/esp-sparkbot', **flags)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('UR_PARTITION_CSV=partitions/v2/16m.csv', result.stdout)
        self.assertIn('ENABLE_UR_AI_E2E_TEST=1', result.stdout)
        self.assertIn('ENABLE_UR_AI_SIGNALING_WEBRTC_VIDEO=1', result.stdout)
        for flag in ('ENABLE_UR_AI_SIGNALING', 'ENABLE_UR_AI_SIGNALING_DEVICE_AEC',
                     'ENABLE_UR_AI_SIGNALING_WEBRTC', 'ENABLE_UR_AI_SIGNALING_WEBRTC_VIDEO'):
            invalid = dict(flags)
            invalid[flag] = '0'
            result = self.run_build('espressif/esp-sparkbot', **invalid)
            self.assertNotEqual(result.returncode, 0, flag)
            self.assertEqual(result.stdout, '', flag)
        flags['ENABLE_UR_UDP_TX_TRACE'] = '1'
        result = self.run_build('espressif/esp-sparkbot', **flags)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, '')


    def test_ota_refuses_tank_or_mismatched_build(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            (work / 'Makefile').write_text((ROOT / 'Makefile').read_text())
            (work / 'sdkconfig').write_text('CONFIG_BOARD_TYPE_ESP_SPARKBOT=y\n')
            for extra in ([], ['BOARD=espressif/esp-sparkbot']):
                result = subprocess.run(['make', 'ota', 'SRC=1', *extra], cwd=work,
                                        text=True, capture_output=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('OTA', result.stdout)


if __name__ == '__main__':
    unittest.main()
