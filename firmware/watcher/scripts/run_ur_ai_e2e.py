#!/usr/bin/env python3
"""在直连设备串口上重复执行联犀语音闭环测试并校验完整阶段。"""

from __future__ import annotations

import argparse
import fcntl
import json
import os
import re
import select
import sys
import termios
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path


REQUIRED_MARKERS = (
    "[UR_AI_E2E] START",
    "method=sessionCreated",
    "method=audioStarted",
    "[UR_AI_E2E] INPUT_DONE",
    "method=respSttDone",
    "method=respTextDone",
    "method=firstAudio",
    "method=respAudioDone",
    "[UR_AI_E2E] RESULT stt_done=1 stt_nonempty=1 stt_match=1 text_done=1 "
    "text_nonempty=1 first_audio=1 audio_done=1",
    "[UR_AI_E2E] PASS reason=voice_roundtrip_complete",
)

# 成功阶段不能抵消崩溃或 AES 丢帧；本门禁同时供语音、拍照和按键验收复用。
FATAL_MARKERS = (
    "[UR_AI_E2E] FAIL",
    "assert failed:",
    "Guru Meditation Error",
    "Backtrace:",
    "CORRUPT HEAP",
    "abort() was called",
    "Failed to resample output audio",
    "Rebooting...",
    "rst:0x",
    "Command returned non-zero error code",
    "MQTT Recv buffer not enough",
    "AES-CTR operation failed:",
    "esp-aes: Failed to allocate memory",
)

TIMING_PATTERNS = {
    "audio_stop_elapsed_ms": re.compile(r"method=audioStop elapsed=(\d+)ms"),
    "stt_done_elapsed_ms": re.compile(r"method=respSttDone[^\r\n]*elapsed=(\d+)ms"),
    "first_audio_elapsed_ms": re.compile(r"method=firstAudio[^\r\n]*elapsed=(\d+)ms"),
    "text_done_elapsed_ms": re.compile(r"method=respTextDone[^\r\n]*elapsed=(\d+)ms"),
    "audio_done_elapsed_ms": re.compile(r"method=respAudioDone[^\r\n]*elapsed=(\d+)ms"),
    # 各方法的 elapsed 计时原点并不相同；跨阶段门禁必须使用统一的设备 uptime。
    "audio_stop_uptime_ms": re.compile(r"I \((\d+)\)[^\r\n]*method=audioStop"),
    "stt_done_uptime_ms": re.compile(r"I \((\d+)\)[^\r\n]*method=respSttDone"),
    "first_audio_uptime_ms": re.compile(r"I \((\d+)\)[^\r\n]*method=firstAudio"),
    "text_done_uptime_ms": re.compile(r"I \((\d+)\)[^\r\n]*method=respTextDone"),
}

# 拍照必须经过真实相机和 MQTT 回执；模型口头回答不能代替动作执行。
PHOTO_MARKERS = (
    "ur_photo: stage=captured",
    "photo stage=upload_start",
    "ur_photo: stage=complete code=200",
    "[UR_VOICE_PHOTO_E2E] action_reply code=200 published=1",
)


@dataclass(frozen=True)
class RunEvaluation:
    """RunEvaluation 保存单轮测试结论、缺失阶段和可比较延迟。"""

    passed: bool
    reason: str
    missing: tuple[str, ...]
    timings_ms: dict[str, int]


def _extract_timings(transcript: str) -> dict[str, int]:
    """_extract_timings 从脱敏方法日志中提取阶段时间并计算验收延迟。"""

    timings: dict[str, int] = {}
    for name, pattern in TIMING_PATTERNS.items():
        match = pattern.search(transcript)
        if match:
            timings[name] = int(match.group(1))
    if "audio_stop_uptime_ms" in timings and "first_audio_uptime_ms" in timings:
        timings["audio_stop_to_first_audio_ms"] = (
            timings["first_audio_uptime_ms"] - timings["audio_stop_uptime_ms"]
        )
    if "stt_done_uptime_ms" in timings and "text_done_uptime_ms" in timings:
        timings["stt_done_to_text_done_ms"] = (
            timings["text_done_uptime_ms"] - timings["stt_done_uptime_ms"]
        )
    return timings


def evaluate_transcript(
    transcript: str,
    max_first_audio_ms: int = 8000,
    max_text_done_ms: int = 15000,
    required_markers: tuple[str, ...] = REQUIRED_MARKERS,
) -> RunEvaluation:
    """evaluate_transcript 校验完整闭环、崩溃标记和 TASK-135 延迟门禁。"""

    fatal = next((marker for marker in FATAL_MARKERS if marker in transcript), "")
    missing = tuple(marker for marker in required_markers if marker not in transcript)
    timings = _extract_timings(transcript)
    if fatal:
        return RunEvaluation(False, f"fatal:{fatal}", missing, timings)
    if missing:
        return RunEvaluation(False, "missing_stage", missing, timings)

    first_audio_latency = timings.get("audio_stop_to_first_audio_ms")
    text_done_latency = timings.get("stt_done_to_text_done_ms")
    if first_audio_latency is None or first_audio_latency < 0:
        return RunEvaluation(False, "invalid_first_audio_timing", (), timings)
    if first_audio_latency >= max_first_audio_ms:
        return RunEvaluation(False, "first_audio_timeout", (), timings)
    if text_done_latency is None or text_done_latency < 0:
        return RunEvaluation(False, "invalid_text_done_timing", (), timings)
    if text_done_latency >= max_text_done_ms:
        return RunEvaluation(False, "text_done_timeout", (), timings)
    return RunEvaluation(True, "voice_roundtrip_complete", (), timings)


def evaluate_multiturn_transcript(
    transcript: str, turns: int, max_first_audio_ms: int = 8000, max_text_done_ms: int = 15000,
) -> RunEvaluation:
    """逐轮校验真实阶段及延迟，拒绝重复独立 session、缺轮或拿上一轮成功补足下一轮。"""
    fatal = next((marker for marker in FATAL_MARKERS if marker in transcript), "")
    if fatal:
        return RunEvaluation(False, f"fatal:{fatal}", (), {})
    boundaries = list(re.finditer(r"\[UR_AI_E2E\] TURN index=(\d+) total=(\d+)", transcript))
    if not 2 <= turns <= 5 or [(int(m[1]), int(m[2])) for m in boundaries] != [
        (index, turns) for index in range(1, turns + 1)
    ]:
        return RunEvaluation(False, "invalid_turn_sequence", (), {})
    if (transcript.count("method=sessionCreated") != 1 or
            transcript.count("method=audioStarted") < turns or
            transcript.count("method=respAudioDone") < turns or
            transcript.count(REQUIRED_MARKERS[-1]) != 1):
        return RunEvaluation(False, "invalid_session_lifecycle", (), {})
    timings: dict[str, int] = {}
    # 首轮创建会话；后续就绪依赖固件检查完整 session 和新的 audioStart token。
    # audioDone 事件位先于工作线程的日志，日志可能落入下一段；逐轮 RESULT 已锁定本轮终态。
    per_turn = tuple(marker for marker in REQUIRED_MARKERS[3:-1] if marker != "method=respAudioDone")
    for index, boundary in enumerate(boundaries, 1):
        end = boundaries[index].start() if index < turns else len(transcript)
        segment = transcript[boundary.start():end]
        required = per_turn + (
            f"[UR_AI_E2E] TURN_READY index={index} same_session=1 new_audio_token=1",
            f"[UR_AI_E2E] TURN_PASS index={index}",
        )
        result = evaluate_transcript(segment, max_first_audio_ms, max_text_done_ms, required)
        timings.update({f"turn_{index}_{key}": value for key, value in result.timings_ms.items()})
        if not result.passed:
            return RunEvaluation(False, f"turn_{index}:{result.reason}", result.missing, timings)
    return RunEvaluation(True, "same_session_multiturn_complete", (), timings)


def evaluate_fixture_transcript(
    transcript: str, fixture: str, turns: int = 1,
    max_first_audio_ms: int = 8000, max_text_done_ms: int = 15000,
) -> RunEvaluation:
    """校验所选样本与实际拍照或控制阶段；旧轮或其他样本不能补足本轮证据。"""
    result = (evaluate_multiturn_transcript(transcript, turns, max_first_audio_ms, max_text_done_ms)
              if turns > 1 else evaluate_transcript(transcript, max_first_audio_ms, max_text_done_ms))
    if not result.passed:
        return result
    starts = re.findall(r"\[UR_AI_E2E\] START fixture=(\w+)", transcript)
    if fixture not in ("query_volume", "take_photo", "control_volume", "tank_forward") or starts != [fixture]:
        return RunEvaluation(False, "fixture_mismatch", (), result.timings_ms)
    if fixture == "tank_forward":
        marker = "[UR_VOICE_MOTION_E2E] action=tankForward"
        segment = transcript[transcript.index("[UR_AI_E2E] START"):]
        if turns != 1 or segment.count(marker) != 1 or segment.index(marker) > segment.index(REQUIRED_MARKERS[-1]):
            return RunEvaluation(False, "missing_motion_action", (marker,), result.timings_ms)
    if fixture == "control_volume":
        marker = "[UR_VOICE_CONTROL_E2E] before=55 target=40 after=40 changed=1"
        segment = transcript[transcript.index("[UR_AI_E2E] START"):]
        if turns != 1 or segment.count(marker) != 1 or segment.index(marker) > segment.index(REQUIRED_MARKERS[-1]):
            return RunEvaluation(False, "missing_control_change", (marker,), result.timings_ms)
    if fixture == "take_photo":
        boundaries = list(re.finditer(r"\[UR_AI_E2E\] TURN index=\d+ total=\d+", transcript))
        segments = ([transcript[boundary.start():boundaries[index + 1].start()
                     if index + 1 < len(boundaries) else len(transcript)]
                     for index, boundary in enumerate(boundaries)] if turns > 1 else
                    [transcript[transcript.index("[UR_AI_E2E] START"):]])
        for index, segment in enumerate(segments, 1):
            cursor = 0
            for marker in PHOTO_MARKERS:
                position = segment.find(marker, cursor)
                if position < 0:
                    return RunEvaluation(False, f"turn_{index}:missing_photo_stage",
                                         (marker,), result.timings_ms)
                cursor = position + len(marker)
    return result


def _test_command(turns: int, fixture: str) -> str:
    """构造兼容旧固件的缺省命令；拍照/控制显式选择真实音频资源。"""
    if fixture == "tank_forward":
        return f"ur_ai_e2e {turns} forward"
    if fixture == "take_photo":
        return f"ur_ai_e2e {turns} photo"
    if fixture == "control_volume":
        return f"ur_ai_e2e {turns} control"
    return "ur_ai_e2e" if turns == 1 else f"ur_ai_e2e {turns}"


def _configure_serial(fd: int, baud: int) -> None:
    """_configure_serial 将设备配置为无回显原始串口。"""

    baud_constant = getattr(termios, f"B{baud}", None)
    if baud_constant is None:
        raise ValueError(f"不支持串口波特率: {baud}")
    attributes = termios.tcgetattr(fd)
    attributes[0] = 0
    attributes[1] = 0
    attributes[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    attributes[3] = 0
    attributes[4] = baud_constant
    attributes[5] = baud_constant
    attributes[6][termios.VMIN] = 0
    attributes[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attributes)


def _read_available(fd: int, timeout: float) -> bytes:
    """_read_available 在限定时间内读取当前可用字节。"""

    readable, _, _ = select.select([fd], [], [], timeout)
    if not readable:
        return b""
    try:
        return os.read(fd, 8192)
    except BlockingIOError:
        return b""


def _drain_failure_tail(fd: int) -> bytes:
    """失败仍读取三秒串口尾部，保留随后打印的寄存器、Backtrace 与重启原因。"""
    tail = bytearray()
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        tail.extend(_read_available(fd, 0.25))
    return bytes(tail)


def _run_once(
    fd: int,
    timeout_seconds: float,
    max_first_audio_ms: int,
    max_text_done_ms: int,
    turns: int = 1,
    fixture: str = "query_volume",
) -> tuple[bytes, RunEvaluation]:
    """_run_once 发送一次测试命令并等待 PASS、失败或超时。"""

    termios.tcflush(fd, termios.TCIFLUSH)
    command = _test_command(turns, fixture)
    os.write(fd, (command + "\r\n").encode())
    started = time.monotonic()
    payload = bytearray()
    while time.monotonic() - started < timeout_seconds:
        chunk = _read_available(fd, 0.25)
        if chunk:
            payload.extend(chunk)
            transcript = payload.decode("utf-8", errors="replace")
            if any(marker in transcript for marker in FATAL_MARKERS):
                payload.extend(_drain_failure_tail(fd))
                break
            if REQUIRED_MARKERS[-1] in transcript:
                # PASS 之前应已打印所有方法阶段；短暂读取尾部状态切换日志。
                payload.extend(_read_available(fd, 0.5))
                break
    transcript = payload.decode("utf-8", errors="replace")
    return bytes(payload), evaluate_fixture_transcript(
        transcript, fixture, turns, max_first_audio_ms, max_text_done_ms)


def _default_log_dir() -> Path:
    """_default_log_dir 返回仓库统一临时目录，避免日志散落源码树。"""

    repository_root = Path(__file__).resolve().parents[3]
    return repository_root / ".temp" / "device-firmware" / "ur-ai-e2e"


def _parse_args(argv: list[str]) -> argparse.Namespace:
    """_parse_args 解析串口、循环次数、超时和验收阈值。"""

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="已核对归属的设备串口，例如 /dev/ttyACM1")
    parser.add_argument("--baud", type=int, default=115200, help="串口波特率")
    parser.add_argument("--repeat", type=int, default=1, help="连续执行轮数")
    parser.add_argument("--turns", type=int, choices=range(1, 6), default=1,
                        help="每个独立 session 内的轮数；多轮须使用支持轮数参数的测试固件")
    parser.add_argument("--fixture", choices=("query_volume", "take_photo", "control_volume", "tank_forward"), default="query_volume",
                        help="真实音频样本；音量控制须预置55；坦克前进须另验底盘接收和停车")
    parser.add_argument("--timeout", type=float, default=None,
                        help="每轮总预算秒数；缺省音量75/拍照150，不改变首帧与文字延迟门禁")
    parser.add_argument("--settle", type=float, default=2, help="轮次间等待秒数")
    parser.add_argument(
        "--max-first-audio-ms",
        type=int,
        default=8000,
        help="audioStop 到首个 TTS 音频的上限（严格小于）",
    )
    parser.add_argument(
        "--max-text-done-ms",
        type=int,
        default=15000,
        help="STTDone 到 TextDone 的上限（严格小于）",
    )
    parser.add_argument("--log-dir", type=Path, default=_default_log_dir(), help="原始日志目录")
    args = parser.parse_args(argv)
    # 拍照长回复总时长包括实际播放，不能拿总等待预算代替首帧延迟要求。
    if args.timeout is None:
        args.timeout = 150 if args.fixture == "take_photo" else 75
    if args.repeat <= 0 or args.timeout <= 0 or args.settle < 0:
        parser.error("repeat/timeout 必须为正数，settle 不能为负数")
    if args.fixture in ("control_volume", "tank_forward") and (args.turns != 1 or args.repeat != 1):
        parser.error("控制用例只允许单轮单次；音量控制每次必须经平台重新准备音量55")
    return args


def main(argv: list[str] | None = None) -> int:
    """main 重复执行真机测试，输出机器可读摘要并以退出码表示总结果。"""

    args = _parse_args(sys.argv[1:] if argv is None else argv)
    # 串口原日志可能带网络与会话信息，不能依赖调用者的默认文件权限。
    os.umask(0o077)
    args.log_dir.mkdir(parents=True, exist_ok=True)
    fd = os.open(args.port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        # 与截图和硬件测试共享串口锁，未取得所有权时不改配置、不消费日志。
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        _configure_serial(fd, args.baud)
        all_passed = True
        run_id = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        for index in range(1, args.repeat + 1):
            log_path = args.log_dir / f"{run_id}-round-{index:02d}.log"
            # 发命令前排他创建日志；同名冲突不能覆盖旧失败，更不能先操作设备再报冲突。
            with log_path.open("xb") as log_file:
                raw, evaluation = _run_once(
                    fd,
                    args.timeout * args.turns,
                    args.max_first_audio_ms,
                    args.max_text_done_ms,
                    args.turns,
                    args.fixture,
                )
                log_file.write(raw)
            summary = {
                "round": index,
                "turns": args.turns,
                "fixture": args.fixture,
                "passed": evaluation.passed,
                "reason": evaluation.reason,
                "missing": list(evaluation.missing),
                "timingsMs": evaluation.timings_ms,
                "log": str(log_path),
            }
            print(json.dumps(summary, ensure_ascii=False), flush=True)
            all_passed = all_passed and evaluation.passed
            if not evaluation.passed:
                break
            if index < args.repeat:
                time.sleep(args.settle)
        return 0 if all_passed else 1
    finally:
        os.close(fd)


if __name__ == "__main__":
    raise SystemExit(main())
