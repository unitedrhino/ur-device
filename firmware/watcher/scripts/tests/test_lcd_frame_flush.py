"""编译实际 LCD 刷新函数，回放 DMA 提前完成、异步完成及失败边界。"""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]  # 固件源码根目录，直接读取生产函数。


class LcdFrameFlushTest(unittest.TestCase):
    """生产函数必须逐块持有 DMA 源，完成通知不得被提交后的清空操作吞掉。"""

    def test_actual_timing_recorder_ignores_redraws_and_bounds_storage(self):
        """执行真实计时记录函数，验证分段边界、重复 UI 刷新与环形缓冲上限。"""
        header = (ROOT / "main/display/lcd_display.h").read_text()
        fields = header[header.index("    struct PreviewFrameTiming {"):]
        fields = fields[:fields.index("\n#endif")]
        source = (ROOT / "main/display/lcd_display.cc").read_text()
        method = source[source.index("void LcdDisplay::RecordPreviewFlush("):]
        method = method[:method.index("\n// PrintPreviewPerformance")]
        code = r'''
// 计时算法和字段均读取生产源码，仅显式提供显示锁已持有时的时间戳。
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstddef>
struct LcdDisplay {
    bool preview_stream_active_=false;
    bool preview_remote_=false;
    // FIELDS
};
// METHOD
int main() {
    LcdDisplay display;
    display.RecordPreviewFlush(300,500,40,30,130);
    assert(display.preview_timing_count_==0);
    display.preview_stream_active_=true;
    display.preview_received_us_=100;
    display.preview_decoded_us_=200;
    display.preview_published_us_=250;
    display.preview_sequence_=1;
    display.preview_render_started_us_=275;
    display.RecordPreviewFlush(300,500,40,30,130);
    const auto first=display.preview_timings_[0];
    assert(first.decode_us==100 && first.lock_us==50 && first.render_us==50);
    assert(first.schedule_us==25 && first.draw_us==25);
    assert(first.flush_us==200 && first.total_us==400 && first.interval_us==0);
    display.RecordPreviewFlush(600,700,40,30,30);
    assert(display.preview_timing_count_==1);
    for (uint32_t sequence=2; sequence<=201; ++sequence) {
        display.preview_sequence_=sequence;
        display.preview_received_us_=sequence*1000;
        display.preview_decoded_us_=sequence*1000+100;
        display.preview_published_us_=sequence*1000+150;
        display.preview_render_started_us_=sequence*1000+175;
        display.RecordPreviewFlush(sequence*1000+200,sequence*1000+400,40,30,130);
    }
    assert(display.preview_timing_count_==128);
    assert(display.preview_timing_write_==201%128);
    const auto latest=display.preview_timings_[(display.preview_timing_write_+127)%128];
    assert(latest.sequence==201 && latest.interval_us==1000);
    assert(latest.total_us==400 && latest.copy_us==40 && latest.wait_us==130);
    display.preview_remote_=true;
    display.preview_sequence_=202;
    display.preview_received_us_=202000000;
    display.preview_decoded_us_=202000100;
    display.preview_published_us_=202000200;
    display.preview_render_started_us_=202000300;
    display.RecordPreviewFlush(202001000,202050000,40,30,130);
    assert(!display.remote_slow_.load() && display.remote_slow_frames_==0);
    for (uint32_t n=203; n<=205; ++n) {
        display.preview_sequence_=n;
        display.preview_received_us_=n*1000000;
        display.preview_decoded_us_=display.preview_received_us_+100;
        display.preview_published_us_=display.preview_received_us_+200;
        display.preview_render_started_us_=display.preview_received_us_+300;
        display.RecordPreviewFlush(display.preview_received_us_+1000,
            display.preview_received_us_+50001,40,30,130);
        assert(display.remote_slow_.load()==(n==205));
    }
    display.preview_remote_=false;
    display.remote_slow_.store(false);
    display.remote_slow_frames_=0;
    ++display.preview_sequence_;
    display.RecordPreviewFlush(display.preview_received_us_+1000,
        display.preview_received_us_+300000,40,30,130);
    assert(!display.remote_slow_.load());
}
'''.replace("// FIELDS", fields).replace("// METHOD", method)
        compiler = shutil.which("g++") or shutil.which("clang++")
        self.assertIsNotNone(compiler)
        output = ROOT.parents[1] / ".temp"
        output.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=output) as directory:
            binary = str(Path(directory) / "lcd-timing-test")
            compiled = subprocess.run(
                [compiler, "-x", "c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-", "-o", binary],
                input=code, text=True, capture_output=True,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            result = subprocess.run([binary], capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))

    def test_actual_flush_preserves_completion_and_pixels(self):
        """覆盖首块即时完成、带填充行跨度、末尾短块及传输失败。"""
        source = (ROOT / "main/display/lcd_display.cc").read_text()
        start = source.index("void SpiLcdDisplay::Flush(")
        method = source[start:source.index("\n// RGB LCD implementation", start)]
        code = r'''
// 仅替换硬件和信号量边界，像素拷贝、交换、等待与释放均执行真实函数。
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>
using SemaphoreHandle_t = void*;
constexpr int ESP_OK=0, ESP_ERR_TIMEOUT=1, pdTRUE=1;
#define CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER 1
int64_t esp_timer_get_time() { static int64_t now=0; return ++now; }
#define pdMS_TO_TICKS(value) (value)
#define ESP_ERROR_CHECK(result) do { if ((result) != ESP_OK) throw std::runtime_error("DMA failed"); } while (0)
struct lv_area_t { int x1, y1, x2, y2; };
struct lv_draw_buf_t { struct { size_t stride; } header; };
struct Display { lv_draw_buf_t buffer{{20}}; bool ready=false,last=true; };
int timing_records=0;
int lv_area_get_width(const lv_area_t* area) { return area->x2-area->x1+1; }
int lv_area_get_height(const lv_area_t* area) { return area->y2-area->y1+1; }
lv_draw_buf_t* lv_display_get_buf_active(Display* display) { return &display->buffer; }
void lv_display_flush_ready(Display* display) { display->ready=true; }
bool lv_display_flush_is_last(Display* display) { return display->last; }
void lv_draw_sw_rgb565_swap(uint8_t* pixels, uint32_t count) {
    for (uint32_t i=0; i<count; ++i) std::swap(pixels[2*i],pixels[2*i+1]);
}
bool signal=false, pending=false, immediate=false, lose_completion=false, fail_draw=false;
int row=0, submissions=0, frame_width=8;
std::vector<uint8_t> expected, wire;
const uint8_t* dma=nullptr;
std::array<uint8_t*,2> staging{};
size_t dma_size=0;
void complete() {
    // 异步完成前，下一块应已准备完毕，且当前 DMA 源仍保持原像素。
    if (!immediate && row<35) {
        assert(staging[submissions%2][0]==0);
        assert(staging[submissions%2][1]==row);
    }
    // 在模拟 DMA 真正读取时核对数据，禁止下一块提前覆盖当前块。
    wire.insert(wire.end(),dma,dma+dma_size);
    assert(std::equal(wire.begin(),wire.end(),expected.begin()));
    pending=false;
    signal=!lose_completion;
}
int xSemaphoreTake(SemaphoreHandle_t, int ticks) {
    if (ticks && pending) complete();
    bool result=signal;
    signal=false;
    return result;
}
int esp_lcd_panel_draw_bitmap(int, int x1, int y1, int x2, int y2, const void* pixels) {
    assert(!pending && x1==4 && x2==4+frame_width && y1==7+row);
    if (fail_draw) return ESP_ERR_TIMEOUT;
    dma=static_cast<const uint8_t*>(pixels);
    dma_size=(x2-x1)*(y2-y1)*2;
    row+=y2-y1;
    ++submissions;
    pending=true;
    if (immediate) complete();
    return ESP_OK;
}
struct SpiLcdDisplay {
    Display* display_;
    int panel_=1;
    uint8_t* dma_flush_buf_[2];
    void* trans_sem_=nullptr;
    void Flush(const lv_area_t*,uint8_t*);
    void RecordPreviewFlush(int64_t,int64_t,int64_t,int64_t,int64_t) { ++timing_records; }
};
// METHOD
int main() {
    std::vector<uint8_t> pixels(35*20+1,0xee), bounce(8*16*2), second(8*16*2);
    staging={bounce.data(),second.data()};
    // 对齐偶数宽走融合路径；非对齐源和奇数宽覆盖安全回退及行尾填充。
    for (int width : {8,7}) for (size_t shift : {0U,1U}) {
        frame_width=width;
        expected.clear();
        for (int y=0;y<35;++y) for (int x=0;x<width;++x) {
            pixels[shift+y*20+x*2]=static_cast<uint8_t>(y);
            pixels[shift+y*20+x*2+1]=static_cast<uint8_t>(x);
            expected.push_back(static_cast<uint8_t>(x));
            expected.push_back(static_cast<uint8_t>(y));
        }
        lv_area_t area{4,7,4+width-1,41};
        for (bool early : {true,false}) for (bool last : {true,false}) {
            Display display;
            display.last=last; timing_records=0;
            SpiLcdDisplay lcd{&display,1,{bounce.data(),second.data()}};
            immediate=early; signal=true; row=0; submissions=0; wire.clear();
            lcd.Flush(&area,pixels.data()+shift);
            assert(display.ready && submissions==3 && wire==expected && !pending);
            assert(timing_records==(last ? 1:0));
        }
    }
    lv_area_t area{4,7,10,41};
    for (bool draw_error : {true,false}) {
        Display display;
        SpiLcdDisplay lcd{&display,1,{bounce.data(),second.data()}};
        immediate=false; signal=false; row=0; submissions=0; wire.clear();
        fail_draw=draw_error; lose_completion=!draw_error;
        bool rejected=false;
        try { lcd.Flush(&area,pixels.data()+1); }
        catch (const std::runtime_error&) { rejected=true; }
        assert(rejected && !display.ready);
    }
}
'''.replace("// METHOD", method)
        compiler = shutil.which("g++") or shutil.which("clang++")
        self.assertIsNotNone(compiler)
        output = ROOT.parents[1] / ".temp"
        output.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=output) as directory:
            binary = str(Path(directory) / "lcd-flush-test")
            compiled = subprocess.run(
                [compiler, "-x", "c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-", "-o", binary],
                input=code, text=True, capture_output=True,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            result = subprocess.run([binary], capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
