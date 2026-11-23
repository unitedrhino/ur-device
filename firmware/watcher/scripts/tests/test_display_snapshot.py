"""执行截图合成生产代码，覆盖顶层透明度、字节序、步长与坏缓冲；不冒充实屏截图。"""
import subprocess
import tempfile
import unittest
import sys
import zlib
from unittest.mock import patch
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
from run_watcher_ui_probe import decode_snapshot
import run_watcher_ui_probe as probe


class DisplaySnapshotTest(unittest.TestCase):
    """用确定性像素验证屏幕截图合成。"""

    def test_ui_ack_waits_for_complete_line(self):
        """逐字节分割 ACK，不能在状态字段尚未到达时把前缀当成完整应答。"""
        frame = b'[UR_HW_TEST] ACK op=state state=3 mqtt=1\r\n'
        for split in range(1, len(frame)):
            with self.subTest(split=split), patch.object(probe.os, 'write'), \
                    patch.object(probe, '_read_available', side_effect=[b'', frame[:split], frame[split:]]):
                self.assertEqual(probe.exchange(123, 'state'), frame.decode())

    def test_end_record_may_be_split_at_every_byte(self):
        """逐字节送入结束记录，禁止只见 END 前缀就提前解析尚未收全的图片。"""
        frame = "[UR_SCREEN] END 12345678\r\n"
        for length in range(len(frame)):
            self.assertFalse(probe.snapshot_complete(frame[:length]))
        self.assertTrue(probe.snapshot_complete(frame))
        self.assertTrue(probe.snapshot_complete(frame.replace("\r", "")))
        self.assertFalse(probe.snapshot_complete("[UR_SCREEN] END 123\n"))

    def test_production_capture_keeps_native_rgb565(self):
        """执行截图生产函数：顶层合成后 JPEG 入口必须得到原生小端像素，失败释放全部缓冲。"""
        source = (ROOT / "main/display/lvgl_display/lvgl_display.cc").read_text()
        method = source[source.index("bool LvglDisplay::SnapshotToJpeg("):]
        harness = r'''
#include <cassert>
#include <cstring>
#include <string>
#include "snapshot_composite.h"
#define CONFIG_LV_USE_SNAPSHOT 1
#define ESP_LOGE(...) ((void)0)
enum { LV_COLOR_FORMAT_RGB565, LV_COLOR_FORMAT_ARGB8888, V4L2_PIX_FMT_RGB565 };
struct lv_obj_t { int type; } screen{0}, top{1}, system_layer{2};
struct Header { size_t w, h, stride; };
struct Buffer { Header header; uint8_t* data; size_t data_size; };
int allocated = 0, freed = 0, fail_layer = -1;
bool encoding_ok = true;
struct LvglDisplay { void* display_ = nullptr; bool SnapshotToJpeg(std::string&, int); };
struct DisplayLockGuard { explicit DisplayLockGuard(void*) {} };
void TraceSnapshotStage(const char*) {}
lv_obj_t* lv_display_get_screen_active(void*) { return &screen; }
lv_obj_t* lv_display_get_layer_top(void*) { return &top; }
lv_obj_t* lv_display_get_layer_sys(void*) { return &system_layer; }
size_t lv_obj_get_child_count(lv_obj_t* obj) { return obj == &top ? 1 : 0; }
Buffer* lv_snapshot_take(lv_obj_t* obj, int) {
    if (obj->type == fail_layer) return nullptr;
    auto* buf = new Buffer{{2, 1, 8}, new uint8_t[8]{}, 8};
    if (obj == &screen) { buf->data[1] = 248; buf->data[2] = 224; buf->data[3] = 7; }
    ++allocated; return buf;
}
void lv_draw_buf_destroy(Buffer* buf) { delete[] buf->data; delete buf; ++freed; }
using lv_draw_buf_t = Buffer;
bool image_to_jpeg_cb(uint8_t* data, size_t size, size_t w, size_t h, int format, int,
                     size_t (*callback)(void*, size_t, const void*, size_t), void* arg) {
    assert(size == 4 && w == 2 && h == 1 && format == V4L2_PIX_FMT_RGB565);
    assert(data[0] == 0 && data[1] == 248 && data[2] == 224 && data[3] == 7);
    callback(arg, 0, "encoded", 7);
    return encoding_ok;
}
METHOD
int main() {
    LvglDisplay display;
    std::string jpeg = "old";
    assert(display.SnapshotToJpeg(jpeg, 75) && jpeg == "encoded");
    assert(allocated == 2 && freed == 2);
    encoding_ok = false;
    assert(!display.SnapshotToJpeg(jpeg, 75) && jpeg.empty() && allocated == freed);
    fail_layer = 1; jpeg = "old";
    assert(!display.SnapshotToJpeg(jpeg, 75) && jpeg.empty() && allocated == freed);
    fail_layer = 0; jpeg = "old";
    assert(!display.SnapshotToJpeg(jpeg, 75) && jpeg.empty() && allocated == freed);
}
'''.replace("METHOD", method)
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / "snapshot-runtime")
            subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Wno-unused-parameter",
                            "-I" + str(ROOT / "main/display/lvgl_display"),
                            "-x", "c++", "-", "-o", binary], input=harness,
                           text=True, capture_output=True, check=True)
            subprocess.run([binary], check=True, capture_output=True)

    def test_knob_probe_restores_volume(self):
        """采集器必须检查实际变化；成功恢复音量，失败不盲目追加反向输入。"""
        for initial in (0, 50, 98, 100):
            state = {"page": 0, "focus": 0, "volume": initial}

            def command(_fd, operation):
                if operation == "long-press":
                    state.update(page=1 - state["page"], focus=0)
                elif state["page"] == 1:
                    state["focus"] += 1
                else:
                    state["volume"] += -5 if operation == "next" else 5

            with patch.object(probe, "read_state", side_effect=lambda _fd: dict(state)), \
                    patch.object(probe, "exchange", side_effect=command):
                probe.verify_knob(1)
            self.assertEqual(state, {"page": 0, "focus": 0, "volume": initial})
        with patch.object(probe, "read_state", return_value={"page": 10, "focus": 0, "volume": 50}), \
                patch.object(probe, "exchange") as command, self.assertRaises(RuntimeError):
            probe.verify_knob(1)
        command.assert_not_called()

    def test_knob_probe_rejects_ack_without_volume_change(self):
        """硬件只 ACK 而音量不变必须失败，不能反向旋转进一步改变现场。"""
        state = {"page": 0, "focus": 0, "volume": 50}
        operations = []

        def command(_fd, operation):
            operations.append(operation)
            if operation == "long-press":
                state.update(page=1 - state["page"], focus=0)
            elif state["page"] == 1:
                state["focus"] += 1

        with patch.object(probe, "read_state", side_effect=lambda _fd: dict(state)), \
                patch.object(probe, "exchange", side_effect=command), self.assertRaises(RuntimeError):
            probe.verify_knob(1)
        self.assertEqual(state["volume"], 50)
        self.assertEqual(operations, ["long-press", "next", "long-press", "previous"])

    def test_brightness_probe_restores_hardware_value(self):
        """亮度从上下限及中间值执行相反两格，验证方向且不更改音量。"""
        for initial in (0, 50, 98, 100):
            state = {"page": 0, "focus": 0, "volume": 35, "brightness": initial}

            def command(_fd, operation):
                if operation == "long-press":
                    state.update(page={0: 1, 1: 0, 4: 1}[state["page"]], focus=0)
                elif operation == "press":
                    state["page"] = 4
                elif state["page"] == 1:
                    state["focus"] += 1
                else:
                    state["brightness"] += 5 if operation == "next" else -5

            with patch.object(probe, "read_state", side_effect=lambda _fd, **_kw: dict(state)), \
                    patch.object(probe, "exchange", side_effect=command):
                probe.verify_brightness(1)
            self.assertEqual(state, {"page": 0, "focus": 0, "volume": 35, "brightness": initial})

    def test_brightness_probe_rejects_active_network(self):
        """活动配网页不得被亮度验收入口打断。"""
        with patch.object(probe, "read_state", return_value={"page": 10}), \
                patch.object(probe, "exchange") as command, self.assertRaises(RuntimeError):
            probe.verify_brightness(1)
        command.assert_not_called()

    def test_brightness_probe_rejects_ack_without_change(self):
        """背光不变时即使串口ACK成功也必须失败，不盲目追加反向旋转。"""
        state = {"page": 0, "focus": 0, "volume": 35, "brightness": 50}
        operations = []

        def command(_fd, operation):
            operations.append(operation)
            if operation == "long-press":
                state.update(page=1, focus=0)
            elif operation == "press":
                state["page"] = 4
            elif state["page"] == 1:
                state["focus"] += 1

        with patch.object(probe, "read_state", side_effect=lambda _fd, **_kw: dict(state)), \
                patch.object(probe, "exchange", side_effect=command), \
                patch.object(probe.time, "monotonic", side_effect=[0, 1, 6]), \
                patch.object(probe.time, "sleep"), self.assertRaises(TimeoutError):
            probe.verify_brightness(1)
        self.assertEqual(state["brightness"], 50)
        self.assertEqual(operations, ["long-press", "next", "next", "next", "next", "press", "next"])

    def test_transport_validation(self):
        """混入普通日志不破坏截图，丢片/重复/乱序/异 ID/CRC 及伪图片不得通过。"""
        jpeg = b"\xff\xd8" + bytes(range(256)) + b"\xff\xd9"
        lines = [f"[UR_SCREEN] BEGIN 12345678 {len(jpeg)} {zlib.crc32(jpeg):08x}"]
        lines += [f"[UR_SCREEN] DATA 12345678 {offset} {jpeg[offset:offset + 128].hex()}"
                  for offset in range(0, len(jpeg), 128)]
        lines += ["[UR_SCREEN] END 12345678"]
        self.assertEqual(decode_snapshot("\nnormal log\n".join(lines)), jpeg)
        with self.assertRaises(ValueError):
            decode_snapshot("\n".join(lines) + "\nGuru Meditation Error")
        cases = [lines[:-1], lines[:1] + lines[2:], lines[:2] + lines[1:],
                 lines[:1] + [lines[2], lines[1]] + lines[3:],
                 [lines[0]] + [line.replace("12345678", "87654321") for line in lines[1:]],
                 [lines[0].rsplit(" ", 1)[0] + " 00000000"] + lines[1:],
                 [lines[0].replace(str(len(jpeg)), "262145")] + lines[1:]]
        for malformed in cases:
            with self.subTest(frames=malformed), self.assertRaises(ValueError):
                decode_snapshot("\n".join(malformed))
        fake = b"abcd"
        with self.assertRaises(ValueError):
            decode_snapshot(f"[UR_SCREEN] BEGIN 12345678 4 {zlib.crc32(fake):08x}\n"
                            "[UR_SCREEN] DATA 12345678 0 61626364\n[UR_SCREEN] END 12345678")

    def test_composite(self):
        """透明保持、全不透明覆盖、半透明混合、行填充和输入拒绝重复十次。"""
        harness = r'''
#include "snapshot_composite.h"
#include <cassert>
int main() {
    uint8_t dst[] = {0, 248, 99, 99, 224, 7, 99, 99};
    uint8_t src[] = {255, 0, 0, 0, 88, 88, 88, 88, 255, 0, 0, 255, 88, 88, 88, 88};
    using display_snapshot::Composite;
    assert(Composite(dst, sizeof(dst), 4, src, sizeof(src), 8, 1, 2));
    assert(dst[0] == 0 && dst[1] == 248 && dst[4] == 31 && dst[5] == 0);
    assert(dst[2] == 99 && dst[3] == 99 && dst[6] == 99 && dst[7] == 99);
    src[3] = 128;
    assert(Composite(dst, 4, 4, src, 8, 8, 1, 1));
    assert(dst[0] == 16 && dst[1] == 120);
    assert(!Composite(dst, 1, 4, src, 8, 8, 1, 1));
    assert(!Composite(dst, 4, 0, src, 8, 8, 1, 1));
    assert(!Composite(dst, 4, 4, src, 8, 3, 1, 1));
    assert(!Composite(nullptr, 4, 4, src, 8, 8, 1, 1));
    assert(!Composite(dst, 4, 4, src, 8, 8, SIZE_MAX, 1));
    assert(dst[0] == 16 && dst[1] == 120);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / "snapshot-test")
            subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            "-I" + str(ROOT / "main/display/lvgl_display"),
                            "-x", "c++", "-", "-o", binary], input=harness,
                           text=True, capture_output=True, check=True)
            for _ in range(10):
                subprocess.run([binary], check=True, capture_output=True)
