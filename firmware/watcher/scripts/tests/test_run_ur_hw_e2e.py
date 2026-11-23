"""验证真机拍照判定器不会把 ACK、模拟回复或残缺链路计为成功。"""
import sys
import fcntl
import os
import pty
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from run_ur_hw_e2e import PHOTO_MARKERS, evaluate_photo
import run_ur_hw_e2e as HW
from run_ur_ai_e2e import _drain_failure_tail


class SerialOwnershipTest(unittest.TestCase):
    """用真实伪终端验证其他取证进程持锁时不会发送设备命令。"""

    def test_busy_serial_rejected_before_device_command(self):
        """占用串口必须立即失败，而不是发送拍照命令后等待回复。"""
        self.check_busy_serial('run_ur_hw_e2e.py', ['--flow-only'])

    def test_voice_runner_respects_same_serial_lock(self):
        """语音入口也遵守同一把锁，不干扰相机和截图取证。"""
        self.check_busy_serial('run_ur_ai_e2e.py', [])

    def check_busy_serial(self, script, extra_args):
        """运行指定生产脚本，断言持锁时退出且没有任何串口写入。"""
        master, slave = pty.openpty()
        try:
            fcntl.flock(slave, fcntl.LOCK_EX | fcntl.LOCK_NB)
            with tempfile.TemporaryDirectory() as directory:
                try:
                    result = subprocess.run([
                        sys.executable, str(Path(HW.__file__).with_name(script)), '--port', os.ttyname(slave),
                        *extra_args, '--repeat', '1', '--log-dir', directory,
                    ], capture_output=True, text=True, timeout=3)
                except subprocess.TimeoutExpired:
                    self.fail('串口已占用，测试入口仍发送命令并等待设备')
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('BlockingIOError', result.stderr)
                os.set_blocking(master, False)
                with self.assertRaises(BlockingIOError):
                    os.read(master, 1024)
        finally:
            os.close(slave)
            os.close(master)


class PhotoEvaluationTest(unittest.TestCase):
    """逐项移除关键阶段并注入异常，覆盖自动验收的失败口径。"""

    def setUp(self):
        """纯判定测试使用替身文件描述符，真实串口锁另由伪终端用例覆盖。"""
        serial_lock = patch.object(HW.fcntl, 'flock')
        serial_lock.start()
        self.addCleanup(serial_lock.stop)

    def test_complete(self):
        """真实捕获、识物与播放全部完成才通过。"""
        self.assertTrue(evaluate_photo("\n".join(PHOTO_MARKERS))["passed"])

    def test_flow_only_keeps_all_transport_and_playback_gates(self):
        """显式流程验收仅免除物品词命中，不免除模型错误、完整播放及任何真实阶段。"""
        flow = "\n".join(PHOTO_MARKERS[:-1]).replace('text_match=1', 'text_match=0')
        flow += '\n[UR_PHOTO_E2E] FAIL reason=physical_photo_roundtrip\n'
        result = evaluate_photo(flow, flow_only=True)
        self.assertTrue(result['passed'])
        self.assertFalse(result['recognitionChecked'])
        self.assertFalse(evaluate_photo(flow)['passed'])
        legacy_status = 'Command returned non-zero error code: 0x1 (ERROR)\r\n'
        self.assertTrue(evaluate_photo(flow + legacy_status, flow_only=True)['passed'])
        self.assertFalse(evaluate_photo(legacy_status + flow, flow_only=True)['passed'])
        self.assertFalse(evaluate_photo(flow + legacy_status * 2, flow_only=True)['passed'])
        self.assertFalse(evaluate_photo(flow + legacy_status.replace('0x1', '0x2'), flow_only=True)['passed'])
        self.assertFalse(evaluate_photo(flow.replace('text_match=0', 'text_match=1') + legacy_status,
                                       flow_only=True)['passed'])
        for marker in PHOTO_MARKERS[:-2]:
            self.assertFalse(evaluate_photo(flow.replace(marker, ''), flow_only=True)['passed'])
        for field in ('text_done', 'first_audio', 'audio_done', 'played'):
            self.assertFalse(evaluate_photo(flow.replace(field + '=1', field + '=0'), flow_only=True)['passed'])
        for bad in ('model_service_error', 'invalid_input_or_not_idle', 'physical_photo_ro'):
            self.assertFalse(evaluate_photo(flow.replace('physical_photo_roundtrip', bad), flow_only=True)['passed'])
        for error in ('Guru Meditation Error', 'AES-CTR operation failed: -132', 'rst:0xc',
                      '[UR_PHOTO_E2E] FAIL reason=model_service_error'):
            self.assertFalse(evaluate_photo(flow + error, flow_only=True)['passed'])

    def test_flow_only_cli_repeats_without_object_word(self):
        """真实 CLI 分支允许无 expect，但模型错误仍使循环立即停止。"""
        flow = '\n'.join(PHOTO_MARKERS[:-1]).replace('text_match=1', 'text_match=0')
        flow += '\n[UR_PHOTO_E2E] FAIL reason=physical_photo_roundtrip\n'
        for reply, expected, calls in ((flow, 0, 2), (flow + '[UR_PHOTO_E2E] FAIL reason=model_service_error', 1, 1)):
            with self.subTest(expected=expected), tempfile.TemporaryDirectory() as directory, \
                    patch.object(sys, 'argv', ['runner', '--port', '/dev/test', '--flow-only', '--repeat', '2', '--log-dir', directory]), \
                    patch.object(HW.os, 'open', return_value=7), patch.object(HW.os, 'close'), \
                    patch.object(HW, '_configure_serial'), patch.object(HW, '_read_available', return_value=b''), \
                    patch.object(HW.time, 'sleep'), patch.object(HW, 'exchange', return_value=reply) as exchange:
                self.assertEqual(HW.main(), expected)
                self.assertEqual(exchange.call_count, calls)

    def test_photo_terminal_line_must_not_be_truncated(self):
        """真实串口可在 FAIL 中途分片，必须保留完整原因行后才结束采集。"""
        with patch.object(HW.os, 'write'), patch.object(HW, '_read_available', side_effect=[
            b'[UR_PHOTO_E2E] FAIL reason=physical_photo_ro', b'undtrip\r\n',
        ]), patch.object(HW.time, 'monotonic', side_effect=[0, 1, 2, 3]), \
                patch.object(HW, '_drain_failure_tail', return_value=b'Command returned non-zero error code: 0x1 (ERROR)\r\n'):
            text = HW.exchange(7, 'test', '[UR_PHOTO_E2E] PASS', 10)
            self.assertIn('reason=physical_photo_roundtrip\r\n', text)
            self.assertIn('Command returned non-zero error code: 0x1 (ERROR)', text)

    def test_failure_tail_keeps_backtrace(self):
        """崩溃首行与调用栈分片到达时，采集器仍保留后续诊断。"""
        with patch("run_ur_ai_e2e.time.monotonic", side_effect=[0, 1, 2, 4]), patch(
            "run_ur_ai_e2e._read_available", side_effect=[b"Backtrace: 0x403", b"\nrst:0x3"]
        ):
            self.assertEqual(_drain_failure_tail(0), b"Backtrace: 0x403\nrst:0x3")

    def test_hex_word_decoder(self):
        """编译生产测试入口解码器，覆盖中文、ASCII、空值、奇数、非法/NUL与超限。"""
        root = Path(__file__).resolve().parents[2]
        code = r'''
#include "ur_hw_test_input.h"
#include <cassert>
int main() {
 assert(UrDecodeTestWord("e69daf") == "杯");
 assert(UrDecodeTestWord("637570") == "cup");
 for(auto value : {"", "1", "zz", "00", "630070"}) assert(UrDecodeTestWord(value).empty());
 assert(UrDecodeTestWord(std::string(130,'1')).empty());
 assert(UrDecodeTestWord(std::string(128,'1')).size()==64);
}
'''
        with tempfile.TemporaryDirectory() as temp:
            binary = str(Path(temp) / "hex-word")
            subprocess.run([shutil.which("g++"), "-std=c++17", "-x", "c++", "-I" + str(root / "main/protocols"),
                            "-", "-o", binary], input=code, text=True, check=True)
            subprocess.run([binary], check=True)

    def test_missing_every_stage(self):
        """缺少任何一个阶段均失败，包括相机与图片内容断言。"""
        for marker in PHOTO_MARKERS:
            with self.subTest(marker=marker):
                self.assertFalse(evaluate_photo("\n".join(m for m in PHOTO_MARKERS if m != marker))["passed"])

    def test_fatal_and_wrong_object(self):
        """有成功标记但发生崩溃或识物不匹配仍失败。"""
        complete = "\n".join(PHOTO_MARKERS)
        for error in ("Guru Meditation Error", "[UR_PHOTO_E2E] FAIL", "Rebooting...",
                      "MQTT Recv buffer not enough"):
            self.assertFalse(evaluate_photo(complete + error)["passed"])
        self.assertFalse(evaluate_photo(complete.replace("text_match=1", "text_match=0"))["passed"])

    def test_interrupt_fatal_must_stop_before_following_photo(self):
        """即使打断方法齐全，出现崩溃也须立即失败，禁止后续照片 PASS 掩盖它。"""
        with tempfile.TemporaryDirectory() as directory, patch.object(sys, "argv", [
            "runner", "--port", "/dev/test", "--expect", "杯", "--repeat", "1",
            "--interrupt", "--log-dir", directory,
        ]), patch.object(HW.os, "open", return_value=7), patch.object(HW.os, "close"), \
                patch.object(HW, "_configure_serial"), patch.object(HW, "_read_available", return_value=b""), \
                patch.object(HW.time, "sleep"), patch.object(HW, "exchange", side_effect=[
                    "method=firstAudio", "method=respCancel method=audioStarted Guru Meditation Error",
                    "State: listening -> idle", "\n".join(PHOTO_MARKERS),
                ]) as exchange:
            self.assertEqual(HW.main(), 1)
            self.assertLessEqual(exchange.call_count, 3)

    def test_aes_loss_rejects_photo_and_shared_hardware_gate(self):
        """图片回复和按键共用的门禁也须拒绝 AES 丢帧，不能只要求收到首帧与终态。"""
        complete = "\n".join(PHOTO_MARKERS)
        for error in ("AES-CTR operation failed: -132", "esp-aes: Failed to allocate memory"):
            with self.subTest(error=error):
                self.assertFalse(evaluate_photo(complete + error)["passed"])
                self.assertTrue(HW.has_fatal(error))

    def test_interrupt_only_has_no_object_assertion_or_following_photo(self):
        """独立用例只验证首帧、取消、新监听及退出；每阶段异常均失败并停止动作。"""
        stages = ["method=firstAudio", "method=respCancel method=audioStarted", "State: listening -> idle"]
        cases = [(stages, 0)]
        for index in range(3):
            fatal = stages.copy()
            fatal[index] += " Guru Meditation Error"
            cases.append((fatal, 1))
            missing = stages.copy()
            missing[index] = "[UR_HW_TEST] ACK"
            cases.append((missing, 1))
        for replies, expected in cases:
            with self.subTest(replies=replies), tempfile.TemporaryDirectory() as directory, \
                    patch.object(sys, "argv", ["runner", "--port", "/dev/test", "--interrupt-only",
                                                "--log-dir", directory]), \
                    patch.object(HW.os, "open", return_value=7), patch.object(HW.os, "close"), \
                    patch.object(HW, "_configure_serial"), patch.object(HW, "_read_available", return_value=b""), \
                    patch.object(HW, "exchange", side_effect=replies) as exchange:
                self.assertEqual(HW.main(), expected)
                self.assertLessEqual(exchange.call_count, 3)
                self.assertTrue(all("photo-hex" not in call.args[1] for call in exchange.call_args_list))

    def test_reconnect_fatal_prevents_camera_start(self):
        """断线恢复方法齐全但随后复位，不得继续相机测试。"""
        with tempfile.TemporaryDirectory() as directory, patch.object(sys, "argv", [
            "runner", "--port", "/dev/test", "--reconnect", "--interrupt-only", "--log-dir", directory,
        ]), patch.object(HW.os, "open", return_value=7), patch.object(HW.os, "close"), \
                patch.object(HW, "_configure_serial"), patch.object(HW, "_read_available", return_value=b""), \
                patch.object(HW, "exchange", return_value="MQTT disconnected MQTT subscriptions restored rst:0xc") as exchange:
            self.assertEqual(HW.main(), 1)
            self.assertEqual(exchange.call_count, 1)
