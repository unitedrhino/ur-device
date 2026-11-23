#!/usr/bin/env python3
"""经串口验收 Watcher 真实拍照、按键取消与断网恢复；独立打断模式仍会拍照上传。"""
import argparse
import fcntl
import json
import os
import re
import time
from pathlib import Path

from run_ur_ai_e2e import _configure_serial, _read_available, _drain_failure_tail, FATAL_MARKERS

# 真实捕获、上传、图片输入和模型识别必须同时存在；ACK 不代表测试成功。
PHOTO_MARKERS = (
    "[UR_PHOTO_E2E] START source=physical_camera", "Capturing image...",
    "photo stage=preview_decoded",
    "Image upload completed size=", "method=inputSend image=1",
    "method=respTextDone", "method=firstAudio", "method=respAudioDone",
    "[UR_PHOTO_E2E] RESULT text_done=1 text_match=1 first_audio=1 audio_done=1 played=1",
    "[UR_PHOTO_E2E] PASS",
)


def evaluate_photo(text, flow_only=False):
    """默认要求识物命中；显式流程模式仅豁免物品词，仍要求完整结果及无运行错误。"""
    if flow_only:
        missing = [marker for marker in PHOTO_MARKERS[:-2] if marker not in text]
        result = re.search(r"\[UR_PHOTO_E2E\] RESULT text_done=1 text_match=[01] "
                           r"first_audio=1 audio_done=1 played=1(?=\r?\n|$)", text)
        terminal = re.search(r"\[UR_PHOTO_E2E\] (?:PASS|FAIL) reason=physical_photo_roundtrip(?=\r?\n|$)", text)
        if not result:
            missing.append("complete_photo_playback_result")
        if not terminal:
            missing.append("complete_photo_terminal")
        # 固件的综合 FAIL 可能仅因物品词未匹配；只有完整链路结果齐全才允许该特定终态。
        failures = re.findall(r"\[UR_PHOTO_E2E\] FAIL[^\r\n]*", text)
        # rc.6 将词未命中同时返回为控制台 0x1；只豁免完整终态之后唯一的该行。
        checked_text = text
        legacy_status = re.search(r"(?m)^Command returned non-zero error code: 0x1 \(ERROR\)\r?$", text)
        if (not missing and result and terminal and legacy_status
                and "text_match=0" in result.group()
                and terminal.group() == "[UR_PHOTO_E2E] FAIL reason=physical_photo_roundtrip"
                and result.end() < terminal.start() < legacy_status.start()
                and text.count("Command returned non-zero error code:") == 1):
            checked_text = text[:legacy_status.start()] + text[legacy_status.end():]
        fatal = next((m for m in FATAL_MARKERS if m in checked_text), "")
        if any(line != "[UR_PHOTO_E2E] FAIL reason=physical_photo_roundtrip" for line in failures):
            fatal = fatal or "unexpected_photo_failure"
        return {"passed": not missing and not fatal, "missing": missing, "fatal": fatal,
                "recognitionChecked": False}
    missing = [marker for marker in PHOTO_MARKERS if marker not in text]
    fatal = next((m for m in FATAL_MARKERS + ("[UR_PHOTO_E2E] FAIL",) if m in text), "")
    return {"passed": not missing and not fatal, "missing": missing, "fatal": fatal}


def has_fatal(text):
    """公共硬件用例失败门禁；方法齐全不能抵消崩溃、复位或串口命令失败。"""
    return any(marker in text for marker in FATAL_MARKERS + ("[UR_PHOTO_E2E] FAIL",))


def exchange(fd, command, terminal, timeout, log_path=None):
    """发送一个命令并采集至终态；独占串口，不用 DTR/RTS 干扰设备。"""
    os.write(fd, (command + "\r\n").encode())
    raw = bytearray()
    observed = set()
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        raw.extend(_read_available(fd, 0.25))
        text = raw.decode(errors="replace")
        if log_path is not None:
            log_path.write_text(text)
        for marker in PHOTO_MARKERS:
            if marker in text and marker not in observed:
                observed.add(marker)
                print(json.dumps({"stage": marker}), flush=True)
        if any(m in text for m in FATAL_MARKERS):
            raw.extend(_drain_failure_tail(fd))
            if log_path is not None:
                log_path.write_text(raw.decode(errors="replace"))
            break
        # 拍照终态要等完整行，避免串口分片把物品不匹配原因截成半句。
        completed = text.rsplit("\n", 1)[0] if "\n" in text else ""
        terminal_text = completed if terminal.startswith("[UR_PHOTO_E2E]") else text
        if terminal in terminal_text or "[UR_PHOTO_E2E] FAIL" in completed:
            if terminal.startswith("[UR_PHOTO_E2E]"):
                # 控制台返回值可能晚于业务终态一帧，必须归入本轮，不能污染下轮。
                raw.extend(_drain_failure_tail(fd))
                if log_path is not None:
                    log_path.write_text(raw.decode(errors="replace"))
            break
    return raw.decode(errors="replace")


def main():
    """运行拍照循环或独立按键打断，可先断连并要求恢复订阅；所有路径遇致命错误即失败。"""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--expect", help="拍照识物用例必填的现场物品词，例如杯；不发送给模型")
    parser.add_argument("--repeat", type=int, default=5)
    parser.add_argument("--flow-only", action="store_true", help="仅验真实拍照/上传/文字/语音/播放，不判物品名称；仍拒绝模型与运行错误")
    parser.add_argument("--reconnect", action="store_true")
    parser.add_argument("--interrupt", action="store_true", help="真实拍照播报时模拟单击打断，再验证下一轮")
    parser.add_argument("--interrupt-only", action="store_true",
                        help="只检查真实拍照播报时的按键取消/重新监听/退出；不判识物成功")
    parser.add_argument("--log-dir", type=Path, required=True)
    args = parser.parse_args()
    if args.flow_only and (args.interrupt_only or args.expect):
        parser.error("flow-only 不与 expect 或 interrupt-only 混用，避免混淆验收范围")
    if args.repeat < 1 or (not args.interrupt_only and not args.flow_only and (
            not args.expect or len(args.expect.encode()) > 64 or any(c.isspace() for c in args.expect))):
        parser.error("repeat 必须为正数；expect 为不含空白的 1–64 字节现场物品词")
    os.umask(0o077)
    args.log_dir.mkdir(parents=True, exist_ok=True)
    fd = os.open(args.port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        # 配置及排空串口前先抢占锁，避免读走正在进行的截图或升级确认日志。
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        _configure_serial(fd, 115200)
        # 清掉命令之前的陈旧日志，禁止上一轮成功污染本轮结果。
        while _read_available(fd, 0.1):
            pass
        if args.reconnect:
            text = exchange(fd, "ur_hw_test wifi-reconnect", "MQTT subscriptions restored", 75)
            (args.log_dir / "reconnect.log").write_text(text)
            passed = ("MQTT subscriptions restored" in text and "MQTT disconnected" in text
                      and not has_fatal(text))
            print(json.dumps({"case": "reconnect", "passed": passed}), flush=True)
            if not passed:
                return 1
        if args.interrupt or args.interrupt_only:
            # 异步入口不占用串口命令任务，首个真实音频到达后才能触发打断。
            before = exchange(fd, "ur_hw_test photo-start", "method=firstAudio", 90)
            interrupted = ""
            closed = ""
            if "method=firstAudio" in before and not has_fatal(before):
                interrupted = exchange(fd, "ur_hw_test press", "method=audioStarted", 25)
                if "method=audioStarted" in interrupted and not has_fatal(interrupted):
                    closed = exchange(fd, "ur_hw_test press", "State: listening -> idle", 15)
            (args.log_dir / "interrupt.log").write_text(before + interrupted + closed)
            passed = ("method=firstAudio" in before and "method=respCancel" in interrupted and
                      "method=audioStarted" in interrupted and "State: listening -> idle" in closed and
                      not has_fatal(before + interrupted + closed))
            print(json.dumps({"case": "button_interrupt", "passed": passed}), flush=True)
            if not passed:
                return 1
            if args.interrupt_only:
                return 0
        for index in range(args.repeat):
            # 占位词只进入设备本地比较，不发给模型；流程模式由完整链路结果单独判定。
            word = "__flow_only__" if args.flow_only else args.expect
            text = exchange(fd, "ur_hw_test photo-hex " + word.encode().hex(), "[UR_PHOTO_E2E] PASS", 215,
                            args.log_dir / f"photo-{index + 1}.log")
            result = evaluate_photo(text, flow_only=args.flow_only)
            print(json.dumps({"case": "photo", "round": index + 1, **result}), flush=True)
            if not result["passed"]:
                return 1
            time.sleep(2)
        return 0
    finally:
        os.close(fd)


if __name__ == "__main__":
    raise SystemExit(main())
