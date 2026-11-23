#!/usr/bin/env python3
"""采集测试固件真实 LVGL 截图；只通过本地串口，不调用云端或保存串口对话日志。"""
import argparse
import fcntl
import os
import re
import time
import zlib
from pathlib import Path

from run_ur_ai_e2e import _configure_serial, _read_available, FATAL_MARKERS


def exchange(fd, operation):
    """发送白名单输入并等待本次 ACK，失败或超时不继续后续操作。"""
    if operation not in {"state", "next", "previous", "long-press", "press"}:
        raise ValueError("unsupported UI operation")
    _read_available(fd, 0.1)
    os.write(fd, f"ur_hw_test {operation}\n".encode())
    text = ""
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        text += _read_available(fd, 0.1).decode("utf-8", errors="replace")
        # 串口可能恰好在 ACK 前缀后分片；完整行到达前状态字段仍不可信。
        completed = text.rsplit("\n", 1)[0] if "\n" in text else ""
        if f"[UR_HW_TEST] ACK op={operation} " in completed:
            return text
    raise TimeoutError("UI command ACK timed out")


def read_state(fd, include_brightness=False):
    """只接受队列已消费的硬件快照；可选回读背光，保持原音量探针结果兼容。"""
    for _ in range(20):
        text = exchange(fd, "state")
        page = re.search(r"\[UR_UI\] page=(\d+) focus=(\d+) revision=(\d+) pending=(\d+)", text)
        values = re.search(r"\[UR_UI\] volume=(\d+) brightness=(\d+)", text)
        if page and values and int(page[4]) == 0:
            state = {"page": int(page[1]), "focus": int(page[2]), "volume": int(values[1])}
            if include_brightness:
                state["brightness"] = int(values[2])
            return state
    raise TimeoutError("UI did not settle")


def verify_knob(fd):
    """真实板级输入验证主页音量及菜单焦点互斥，结束恢复原音量和主页。"""
    original = read_state(fd)
    if original["page"] != 0:
        raise RuntimeError("knob E2E requires home page; refusing to interrupt active setup")
    # 长按只唤醒/开菜单，不会启动对话；允许第一次唤醒吞键。
    for _ in range(2):
        exchange(fd, "long-press")
        menu = read_state(fd)
        if menu["page"] == 1:
            break
    if menu["page"] != 1:
        raise RuntimeError("menu did not open")
    exchange(fd, "next")
    moved = read_state(fd)
    if moved != {"page": 1, "focus": 1, "volume": original["volume"]}:
        raise RuntimeError("menu knob changed volume or did not move focus")
    exchange(fd, "long-press")
    if read_state(fd)["page"] != 0:
        raise RuntimeError("menu did not return home")
    before = original["volume"]
    # 与实体滚轮一致：Next 为右滚减小，Previous 为左滚增加，菜单焦点语义不变。
    first, second, expected = ("next", "previous", before - 5) if before > 95 else ("previous", "next", before + 5)
    exchange(fd, first)
    try:
        if read_state(fd)["volume"] != expected:
            raise RuntimeError("home knob did not change hardware volume")
    finally:
        # 只有实际发生预期变化才发送反向输入，避免首步失败时再改坏现场音量。
        if read_state(fd)["volume"] == expected:
            exchange(fd, second)
    if read_state(fd)["volume"] != before:
        raise RuntimeError("volume did not restore")
    print("PASS knob home_volume=5 menu_focus=1 restored=1")


def verify_brightness(fd):
    """回放亮度子页右增左减，等待真实背光渐变完成，结束恢复数值及主页。"""
    original = read_state(fd, include_brightness=True)
    if original["page"] != 0:
        raise RuntimeError("brightness E2E requires home page")
    for _ in range(2):
        exchange(fd, "long-press")
        if read_state(fd)["page"] == 1:
            break
    menu = read_state(fd)
    if menu["page"] != 1 or menu["focus"] != 0:
        raise RuntimeError("brightness menu did not open at initial focus")
    for _ in range(4):
        exchange(fd, "next")
    if read_state(fd)["focus"] != 4:
        raise RuntimeError("brightness menu focus mismatch")
    exchange(fd, "press")
    if read_state(fd)["page"] != 4:
        raise RuntimeError("brightness page did not open")
    before = original["brightness"]
    first, second, expected = ("next", "previous", before + 5) if before <= 95 else ("previous", "next", before - 5)

    def wait_value(expected_value):
        """渐变期间不能把目标 ACK 当作实际背光完成，读到准确值后才通过。"""
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            state = read_state(fd, include_brightness=True)
            if state["page"] != 4 or state["volume"] != original["volume"]:
                raise RuntimeError("brightness probe changed page or volume")
            if state["brightness"] == expected_value:
                return
            time.sleep(0.05)
        raise TimeoutError("hardware brightness did not reach expected value")

    exchange(fd, first)
    try:
        wait_value(expected)
    finally:
        # 未到预期值时不盲目反向修改；失败保留现场状态以定位，而非声称已恢复。
        if read_state(fd, include_brightness=True)["brightness"] == expected:
            exchange(fd, second)
    wait_value(before)
    exchange(fd, "long-press")
    exchange(fd, "long-press")
    if read_state(fd)["page"] != 0:
        raise RuntimeError("brightness probe did not return home")
    print("PASS brightness right_increase=1 left_decrease=1 restored=1")


def decode_snapshot(text):
    """验证唯一传输 ID、连续偏移、大小、CRC 和 JPEG 标记，缺片或混帧全部拒绝。"""
    if any(marker in text for marker in FATAL_MARKERS):
        raise ValueError("device fault during screenshot")
    frames = re.findall(r"\[UR_SCREEN\] ([^\r\n]+)", text)
    if not frames or not frames[0].startswith("BEGIN "):
        raise ValueError("missing screenshot header")
    header = frames[0].split()
    if len(header) != 4:
        raise ValueError("invalid screenshot header")
    _, identity, size_text, crc_text = header
    size = int(size_text)
    if not 0 < size <= 256 * 1024 or not re.fullmatch(r"[0-9a-f]{8}", identity):
        raise ValueError("invalid screenshot size or id")
    data = bytearray()
    for frame in frames[1:-1]:
        fields = frame.split()
        if len(fields) != 4 or fields[0] != "DATA" or fields[1] != identity:
            raise ValueError("mixed screenshot frames")
        part = bytes.fromhex(fields[3])
        if int(fields[2]) != len(data) or not 0 < len(part) <= 128 or len(data) + len(part) > size:
            raise ValueError("invalid screenshot offset or chunk")
        data.extend(part)
    if frames[-1] != "END " + identity or len(data) != size:
        raise ValueError(f"incomplete screenshot: received={len(data)} expected={size} "
                         f"end_matches={frames[-1] == 'END ' + identity}")
    if zlib.crc32(data) != int(crc_text, 16):
        raise ValueError("screenshot CRC mismatch")
    if not data.startswith(b"\xff\xd8") or not data.endswith(b"\xff\xd9"):
        raise ValueError("invalid JPEG markers")
    return bytes(data)


def snapshot_complete(text):
    """串口可在 END 标识或 ID 中间分片，只有完整结束行到达才允许解析。"""
    return re.search(r"\[UR_SCREEN\] END [0-9a-f]{8}\r?\n", text) is not None


def main():
    """独占串口采集单次截图，输出目录受限；传输失败不保存部分图片。"""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--verify-knob", action="store_true", help="截图前验证滚轮并恢复原音量；要求初始主页")
    parser.add_argument("--verify-brightness", action="store_true", help="截图前验证亮度右增左减并恢复原值；要求初始主页")
    args = parser.parse_args()
    os.umask(0o077)
    fd = os.open(args.port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        _configure_serial(fd, 115200)
        _read_available(fd, 0.2)
        if args.verify_knob:
            verify_knob(fd)
        if args.verify_brightness:
            verify_brightness(fd)
        os.write(fd, b"ur_hw_test snapshot\n")
        deadline = time.monotonic() + 45
        text = ""
        while time.monotonic() < deadline:
            text += _read_available(fd, 0.25).decode("utf-8", errors="replace")
            if any(marker in text for marker in FATAL_MARKERS):
                raise RuntimeError("device fault during screenshot; no image saved")
            if "[UR_SCREEN] FAIL" in text:
                raise RuntimeError("device screenshot failed")
            if snapshot_complete(text):
                data = decode_snapshot(text)
                args.output.parent.mkdir(parents=True, exist_ok=True)
                # 独占创建避免误覆盖已有取证，输出不含原始串口日志。
                with args.output.open("xb") as output:
                    output.write(data)
                stages = re.findall(r"snapshot stage=([a-z_]+) stack_free=(\d+)", text)
                if stages:
                    print("snapshot_stack_min=" + str(min(int(free) for _, free in stages)) +
                          " stages=" + ",".join(stage for stage, _ in stages))
                print(f"PASS screenshot bytes={len(data)} output={args.output}")
                return
        raise TimeoutError("screenshot transfer timed out")
    finally:
        os.close(fd)


if __name__ == "__main__":
    main()
