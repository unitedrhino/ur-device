"""Watcher 联犀语音串口 E2E runner 的主机侧单元测试。"""

import importlib.util
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch


SCRIPT = Path(__file__).resolve().parents[1] / "run_ur_ai_e2e.py"
SPEC = importlib.util.spec_from_file_location("run_ur_ai_e2e", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


def successful_transcript(
    first_audio_uptime_ms: int = 3000,
    text_done_uptime_ms: int = 5000,
) -> str:
    """successful_transcript 生成包含乱序响应阶段的完整脱敏日志。"""

    return f"""
I (100) Application: [UR_AI_E2E] START fixture=query_volume frames=29 sample_rate=16000 frame_ms=60
I (200) UrAiProtocol: method=sessionCreated session=12345678
I (300) UrAiProtocol: method=audioStarted elapsed=100ms
I (2000) UrAiProtocol: method=audioStop elapsed=1800ms
[UR_AI_E2E] INPUT_DONE frames=29
I (2400) UrAiProtocol: method=respCreated resp=resp_123 elapsed=500ms
I (2500) UrAiProtocol: method=respSttDone resp=resp_123 elapsed=600ms
I (2800) UrAiProtocol: method=respAudioStart resp=resp_123 elapsed=2000ms
I ({first_audio_uptime_ms}) UrAiProtocol: method=firstAudio resp=resp_123 elapsed=1600ms
I ({text_done_uptime_ms}) UrAiProtocol: method=respTextDone resp=resp_123 elapsed=3100ms
[UR_AI_E2E] RESULT stt_done=1 stt_nonempty=1 stt_match=1 text_done=1 text_nonempty=1 first_audio=1 audio_done=1
I (10000) UrAiProtocol: method=respAudioDone resp=resp_123 elapsed=9000ms
[UR_AI_E2E] PASS reason=voice_roundtrip_complete
"""


def successful_multiturn(turns=3):
    """组合一个会话的逐轮方法记录；每轮重新给出阶段，禁止复用上轮终态。"""
    parts = []
    for index in range(1, turns + 1):
        text = successful_transcript()
        text = text.replace("[UR_AI_E2E] INPUT_DONE", (
            f"[UR_AI_E2E] TURN_READY index={index} same_session=1 new_audio_token=1\n"
            "[UR_AI_E2E] INPUT_DONE"))
        if index > 1:
            text = "\n".join(line for line in text.splitlines() if
                             "method=sessionCreated" not in line and "[UR_AI_E2E] START" not in line)
        text = text.replace("[UR_AI_E2E] PASS reason=voice_roundtrip_complete",
                            f"[UR_AI_E2E] TURN_PASS index={index}")
        parts.append(f"[UR_AI_E2E] TURN index={index} total={turns}\n" + text)
    return "\n".join(parts) + "\n[UR_AI_E2E] PASS reason=voice_roundtrip_complete\n"


class TankForwardFixtureTest(unittest.TestCase):
    """语音回复完整也不能替代前进行为入口执行，旧轮或重复动作必须拒绝。"""

    def test_requires_current_forward_action(self):
        """无动作、旧动作和重复动作均失败；仅本轮单次前进记录可通过。"""
        base = successful_transcript().replace("fixture=query_volume", "fixture=tank_forward")
        marker = "[UR_VOICE_MOTION_E2E] action=tankForward\n"
        self.assertFalse(MODULE.evaluate_fixture_transcript(base, "tank_forward").passed)
        self.assertFalse(MODULE.evaluate_fixture_transcript(marker + base, "tank_forward").passed)
        valid = base.replace("[UR_AI_E2E] RESULT", marker + "[UR_AI_E2E] RESULT")
        self.assertTrue(MODULE.evaluate_fixture_transcript(valid, "tank_forward").passed)
        self.assertFalse(MODULE.evaluate_fixture_transcript(valid.replace(marker, marker * 2), "tank_forward").passed)


class LogPrivacyTest(unittest.TestCase):
    """LogPrivacyTest 使用真实文件系统验证权限与证据保护，不连接物理串口。"""

    def run_mock_serial(self, directory, payload, passed=True):
        """运行实际CLI保存分支，仅替换串口读写，返回真实退出码。"""
        result = MODULE.RunEvaluation(passed, 'fixture_result', (), {})
        with patch.object(MODULE.os, 'open', return_value=7), \
                patch.object(MODULE.os, 'close'), patch.object(MODULE.fcntl, 'flock'), \
                patch.object(MODULE, '_configure_serial'), \
                patch.object(MODULE, '_run_once', return_value=(payload, result)) as serial_run, \
                patch.object(MODULE, 'datetime') as clock:
            clock.now.return_value.strftime.return_value = 'fixed'
            try:
                return MODULE.main(['--port', '/dev/mock', '--log-dir', str(directory)])
            finally:
                self.serial_call_count = serial_run.call_count

    def test_logs_private_under_permissive_umask(self):
        """调用者umask002时，新目录和成功/失败日志仍须仅当前用户可读。"""
        previous = os.umask(0o002)
        try:
            with tempfile.TemporaryDirectory() as folder:
                for passed in (True, False):
                    os.umask(0o002)
                    directory = Path(folder) / str(passed)
                    self.assertEqual(self.run_mock_serial(directory, b'evidence', passed), 0 if passed else 1)
                    self.assertEqual(directory.stat().st_mode & 0o777, 0o700)
                    log = directory / 'fixed-round-01.log'
                    self.assertEqual(log.stat().st_mode & 0o777, 0o600)
                    self.assertEqual(log.read_bytes(), b'evidence')
        finally:
            os.umask(previous)

    def test_same_name_does_not_overwrite_evidence(self):
        """同秒同目录再次运行须拒绝覆盖，旧失败或成功原日志保持逐字节不变。"""
        previous = os.umask(0o002)
        try:
            with tempfile.TemporaryDirectory() as folder:
                directory = Path(folder) / 'logs'
                self.run_mock_serial(directory, b'original-failed-evidence', False)
                with self.assertRaises(FileExistsError):
                    self.run_mock_serial(directory, b'replacement', True)
                self.assertEqual(self.serial_call_count, 0)
                self.assertEqual((directory / 'fixed-round-01.log').read_bytes(), b'original-failed-evidence')
        finally:
            os.umask(previous)


class EvaluateTranscriptTest(unittest.TestCase):
    """EvaluateTranscriptTest 覆盖成功、缺阶段、崩溃和延迟门禁。"""

    def test_fixture_command_and_arguments(self):
        """缺省保持旧命令，拍照必须显式选择且 CLI 拒绝未知样本。"""
        self.assertEqual(MODULE._parse_args(["--port", "/dev/test"]).fixture, "query_volume")
        self.assertEqual(MODULE._test_command(1, "query_volume"), "ur_ai_e2e")
        self.assertEqual(MODULE._test_command(3, "query_volume"), "ur_ai_e2e 3")
        self.assertEqual(MODULE._test_command(1, "take_photo"), "ur_ai_e2e 1 photo")
        self.assertEqual(MODULE._test_command(3, "take_photo"), "ur_ai_e2e 3 photo")
        self.assertEqual(MODULE._test_command(1, "control_volume"), "ur_ai_e2e 1 control")
        self.assertEqual(MODULE._parse_args(["--port", "/dev/test", "--fixture", "control_volume"]).timeout, 75)
        with self.assertRaises(SystemExit):
            MODULE._parse_args(["--port", "/dev/test", "--fixture", "control_volume", "--turns", "2"])
        with self.assertRaises(SystemExit):
            MODULE._parse_args(["--port", "/dev/test", "--fixture", "control_volume", "--repeat", "2"])
        self.assertEqual(MODULE._parse_args(["--port", "/dev/test"]).timeout, 75)
        self.assertEqual(MODULE._parse_args(["--port", "/dev/test", "--fixture", "take_photo"]).timeout, 150)
        self.assertEqual(MODULE._parse_args(["--port", "/dev/test", "--fixture", "take_photo",
                                          "--timeout", "90"]).timeout, 90)
        with self.assertRaises(SystemExit):
            MODULE._parse_args(["--port", "/dev/test", "--fixture", "fake"])

    def test_voice_photo_requires_real_capture_upload_and_action_reply(self):
        """口头回答、上传失败、发布失败、错样本及旧轮证据不能通过语音拍照。"""
        audio = successful_transcript().replace("fixture=query_volume", "fixture=take_photo")
        stages = "\n".join(MODULE.PHOTO_MARKERS) + "\n"
        photo = audio.replace("method=respCreated", stages + "method=respCreated")
        self.assertTrue(MODULE.evaluate_fixture_transcript(photo, "take_photo").passed)
        for marker in MODULE.PHOTO_MARKERS:
            self.assertFalse(MODULE.evaluate_fixture_transcript(
                photo.replace(marker, "missing"), "take_photo").passed, marker)
        for invalid in (audio, photo.replace("published=1", "published=0"),
                        photo.replace("stage=complete code=200", "stage=complete code=502"),
                        stages + audio, photo.replace(stages, "\n".join(reversed(MODULE.PHOTO_MARKERS))),
                        photo.replace("fixture=take_photo", "fixture=query_volume")):
            self.assertFalse(MODULE.evaluate_fixture_transcript(invalid, "take_photo").passed)

    def test_voice_control_requires_changed_hardware_volume(self):
        """设置测试不能只凭正常语音或旧音量通过，必须有本轮准备值与实际改变证据。"""
        audio = successful_transcript().replace("fixture=query_volume", "fixture=control_volume")
        marker = "[UR_VOICE_CONTROL_E2E] before=55 target=40 after=40 changed=1"
        complete = audio.replace("[UR_AI_E2E] PASS", marker + "\n[UR_AI_E2E] PASS")
        self.assertTrue(MODULE.evaluate_fixture_transcript(complete, "control_volume").passed)
        for invalid in (audio, marker + audio, complete.replace("after=40", "after=55"),
                        complete.replace("before=55", "before=40"), complete.replace("changed=1", "changed=0"),
                        complete.replace("target=40", "target=35")):
            self.assertFalse(MODULE.evaluate_fixture_transcript(invalid, "control_volume").passed)
        self.assertFalse(MODULE.evaluate_fixture_transcript(complete, "control_volume", 2).passed)

    def test_multiturn_photo_checks_each_turn_without_reusing_prior_capture(self):
        """三轮拍照分别要求完整阶段；首轮多份记录不能抵消最后一轮缺相机。"""
        text = successful_multiturn().replace("fixture=query_volume", "fixture=take_photo")
        stages = "\n".join(MODULE.PHOTO_MARKERS) + "\n"
        text = text.replace("method=respCreated", stages + "method=respCreated")
        self.assertTrue(MODULE.evaluate_fixture_transcript(text, "take_photo", 3).passed)
        prefix, tail = text.split("[UR_AI_E2E] TURN index=3", 1)
        invalid = prefix + "[UR_AI_E2E] TURN index=3" + tail.replace(stages, "")
        result = MODULE.evaluate_fixture_transcript(invalid, "take_photo", 3)
        self.assertFalse(result.passed)
        self.assertEqual(result.reason, "turn_3:missing_photo_stage")

    def test_complete_round_passes_and_reports_comparable_latency(self):
        result = MODULE.evaluate_transcript(successful_transcript())
        self.assertTrue(result.passed)
        self.assertEqual(result.timings_ms["audio_stop_to_first_audio_ms"], 1000)
        self.assertEqual(result.timings_ms["stt_done_to_text_done_ms"], 2500)

    def test_response_stage_order_may_interleave(self):
        transcript = successful_transcript().replace(
            "I (2500) UrAiProtocol: method=respSttDone resp=resp_123 elapsed=600ms\n"
            "I (2800) UrAiProtocol: method=respAudioStart resp=resp_123 elapsed=2000ms",
            "I (2800) UrAiProtocol: method=respAudioStart resp=resp_123 elapsed=2000ms\n"
            "I (2500) UrAiProtocol: method=respSttDone resp=resp_123 elapsed=600ms",
        )
        self.assertTrue(MODULE.evaluate_transcript(transcript).passed)

    def test_missing_terminal_stage_fails(self):
        result = MODULE.evaluate_transcript(
            successful_transcript().replace("method=respAudioDone", "method=audioStillRunning")
        )
        self.assertFalse(result.passed)
        self.assertEqual(result.reason, "missing_stage")
        self.assertIn("method=respAudioDone", result.missing)

    def test_heap_assertion_fails_even_if_pass_marker_exists(self):
        result = MODULE.evaluate_transcript(successful_transcript() + "\nassert failed: heap\n")
        self.assertFalse(result.passed)
        self.assertEqual(result.reason, "fatal:assert failed:")

    def test_resampler_overflow_signal_fails_before_heap_assertion(self):
        """重采样容量异常即判失败，不能等后续堆断言才暴露。"""
        transcript = successful_transcript() + (
            "\nE AudioService: Failed to resample output audio, "
            "error=0 actual=1633 capacity=672\n"
        )
        result = MODULE.evaluate_transcript(transcript)
        self.assertFalse(result.passed)
        self.assertEqual(result.reason, "fatal:Failed to resample output audio")

    def test_latency_gate_is_strict(self):
        result = MODULE.evaluate_transcript(successful_transcript(first_audio_uptime_ms=10000))
        self.assertFalse(result.passed)
        self.assertEqual(result.reason, "first_audio_timeout")

    def test_aes_frame_loss_fails_complete_single_and_multi_turns(self):
        """AES 失败会丢音频帧，即使后续阶段和 PASS 齐全也不能证明播放流畅。"""
        for error in ("AES-CTR operation failed: -132", "esp-aes: Failed to allocate memory"):
            with self.subTest(error=error):
                self.assertFalse(MODULE.evaluate_transcript(successful_transcript() + error).passed)
                self.assertFalse(MODULE.evaluate_multiturn_transcript(
                    successful_multiturn() + error, 3).passed)

    def test_three_turns_require_one_session_and_complete_each_turn(self):
        """同 session 三轮均独立有阶段和时延；重复独立单轮不能冒充多轮。"""
        result = MODULE.evaluate_multiturn_transcript(successful_multiturn(), 3)
        self.assertTrue(result.passed)
        self.assertEqual(result.timings_ms["turn_3_audio_stop_to_first_audio_ms"], 1000)
        self.assertFalse(MODULE.evaluate_multiturn_transcript(successful_transcript() * 3, 3).passed)
        rebuilt = successful_multiturn() + "\nmethod=sessionCreated session=another\n"
        self.assertEqual(MODULE.evaluate_multiturn_transcript(rebuilt, 3).reason,
                         "invalid_session_lifecycle")

    def test_later_turn_missing_stage_or_stale_token_fails(self):
        """首轮成功不能抵消后续缺语音、缺新 token 门禁或错误的轮次编号。"""
        prefix, tail = successful_multiturn().split("[UR_AI_E2E] TURN index=3", 1)
        for marker in ("method=firstAudio", "new_audio_token=1", "stt_match=1", "TURN_PASS index=3"):
            invalid = prefix + "[UR_AI_E2E] TURN index=3" + tail.replace(marker, "missing")
            self.assertFalse(MODULE.evaluate_multiturn_transcript(invalid, 3).passed, marker)
        invalid = successful_multiturn().replace("TURN index=2 total=3", "TURN index=1 total=3")
        self.assertEqual(MODULE.evaluate_multiturn_transcript(invalid, 3).reason,
                         "invalid_turn_sequence")

    def test_multiturn_checks_late_latency_and_fatal_after_completion(self):
        """最后一轮超门限或 PASS 后复位仍失败，不能只检查首轮延迟。"""
        prefix, tail = successful_multiturn().split("[UR_AI_E2E] TURN index=3", 1)
        invalid = prefix + "[UR_AI_E2E] TURN index=3" + tail.replace(
            "I (3000) UrAiProtocol: method=firstAudio", "I (10000) UrAiProtocol: method=firstAudio")
        self.assertEqual(MODULE.evaluate_multiturn_transcript(invalid, 3).reason,
                         "turn_3:first_audio_timeout")
        self.assertFalse(MODULE.evaluate_multiturn_transcript(successful_multiturn() + "rst:0xc", 3).passed)

    def test_multiturn_argument_bounds(self):
        """轮数有界，缺省保留原单轮命令语义。"""
        self.assertEqual(MODULE._parse_args(["--port", "/dev/test"]).turns, 1)
        self.assertEqual(MODULE._parse_args(["--port", "/dev/test", "--turns", "3"]).turns, 3)
        for turns in (0, 6):
            self.assertFalse(MODULE.evaluate_multiturn_transcript(successful_multiturn(), turns).passed)

    def test_audio_done_log_can_follow_next_turn_boundary(self):
        """事件位先置位、日志后打印的跨任务顺序不能被误判缺轮，逐轮 RESULT 仍必须完整。"""
        text = successful_multiturn()
        line = "I (10000) UrAiProtocol: method=respAudioDone resp=resp_123 elapsed=9000ms"
        text = text.replace(line, "", 1).replace("TURN index=2 total=3\n", "TURN index=2 total=3\n" + line + "\n")
        self.assertTrue(MODULE.evaluate_multiturn_transcript(text, 3).passed)
        self.assertFalse(MODULE.evaluate_multiturn_transcript(text.replace(line, "", 1), 3).passed)


if __name__ == "__main__":
    unittest.main()
