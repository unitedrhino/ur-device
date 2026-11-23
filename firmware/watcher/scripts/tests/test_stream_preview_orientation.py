"""编译真实预览入口与绘制单元，验证方向、裁剪及原始图不被修改。"""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]  # 固件源码根目录。


class StreamPreviewOrientationTest(unittest.TestCase):
    """方向修正只作用于 Watcher 的低分辨率本地镜子。"""

    def test_actual_preview_preserves_source_and_corrects_horizontal_direction(self):
        """调用生产入口，校验仅标记本地镜子、不再复制原始帧。"""
        source = (ROOT / "main/display/lcd_display.cc").read_text()
        start = source.index("void LcdDisplay::SetStreamPreview(")
        method = source[start:source.index("// RecordPreviewFlush", start)]
        # 方法之后的板型条件由测试前置宏统一提供。
        method = method.removesuffix("#if CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER\n")
        code = r'''
#include <cassert>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <vector>
#define CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER 1
#define LV_IMAGE_HEADER_MAGIC 1
#define LV_COLOR_FORMAT_RGB565 1
#define LV_IMAGE_FLAGS_USER1 1
#define LV_DISPLAY_RENDER_MODE_PARTIAL 1
#define LV_DISPLAY_RENDER_MODE_FULL 2
#define LV_ALIGN_CENTER 1
#define LV_OBJ_FLAG_HIDDEN 1
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define ESP_LOGE(...) ((void)0)
static int registrations=0;
int64_t esp_timer_get_time() { static int64_t now=0; return ++now; }
struct Image {
    struct { int magic,cf,flags; uint16_t w,h,stride; } header;
    uint32_t data_size;
    const uint8_t* data;
};
void EnsureMirrorPreviewRenderer(const Image*) { ++registrations; }
struct Gif { void Stop() {} };
struct LcdDisplay {
    void* preview_image_=this;
    void* emoji_box_=nullptr;
    void* bottom_bar_=nullptr;
    void* preview_timer_=nullptr;
    void* display_=nullptr;
    Gif* gif_controller_=nullptr;
    bool preview_stream_active_=false, preview_fullscreen_=true, preview_remote_=false;
    int width_=412;
    Image stream_preview_dsc_{};
    uint32_t preview_sequence_=0;
    int64_t preview_received_us_=0, preview_decoded_us_=0;
    int64_t preview_published_us_=0, preview_render_started_us_=0;
    void SetStreamPreview(const uint8_t*,uint16_t,uint16_t,int64_t);
};
struct DisplayLockGuard {
    explicit DisplayLockGuard(LcdDisplay*) {}
    explicit operator bool() const { return true; }
};
void lv_image_set_src(void*,const Image*) {}
void lv_obj_set_size(void*,int,int) {}
void lv_image_set_antialias(void*,bool) {}
void lv_image_set_scale(void*,int) {}
void lv_display_set_render_mode(void*,int) {}
void lv_obj_align(void*,int,int,int) {}
void lv_obj_remove_flag(void*,int) {}
void lv_obj_add_flag(void*,int) {}
void esp_timer_stop(void*) {}
// METHOD
int main() {
    std::vector<uint8_t> source(240*240*2,0xAB);
    const auto original=source;
    LcdDisplay display;
    display.SetStreamPreview(source.data(),240,240,100);
    assert(display.stream_preview_dsc_.data==source.data() && source==original);
#if CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
    assert(display.stream_preview_dsc_.header.flags==LV_IMAGE_FLAGS_USER1);
    assert(registrations==1);
#else
    assert(display.stream_preview_dsc_.header.flags==0 && registrations==0);
#endif
    display.preview_fullscreen_=false;
    display.SetStreamPreview(source.data(),240,240,300);
    assert(display.stream_preview_dsc_.data==source.data());
    assert(display.stream_preview_dsc_.header.flags==0);
    display.preview_fullscreen_=true;
    display.preview_remote_=true;
    display.SetStreamPreview(source.data(),240,240,350);
    assert(display.stream_preview_dsc_.header.flags==0 && source==original);
    display.preview_remote_=false;
    std::vector<uint8_t> large(416*416*2);
    display.SetStreamPreview(large.data(),416,416,400);
    assert(display.stream_preview_dsc_.data==large.data());
    assert(display.stream_preview_dsc_.header.flags==0);
}
'''.replace("// METHOD", method)
        compiler = shutil.which("g++") or shutil.which("clang++")
        self.assertIsNotNone(compiler)
        output = ROOT.parents[1] / ".temp"
        output.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=output) as directory:
            binary = str(Path(directory) / "preview-orientation-test")
            for watcher in (1, 0):
                self.compile_and_run(compiler, binary, code.replace(
                    "#define CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER 1",
                    f"#define CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER {watcher}",
                ))

    def test_actual_renderer_clips_preserves_overlays_and_reuses_unit(self):
        """真实调度和像素函数覆盖局部脏区、奇数宽、非对齐、兼容路径及释放。"""
        source = (ROOT / "main/display/lcd_display.cc").read_text()
        start = source.index("// WatcherPreviewDrawUnit")
        methods = source[start:source.index("\n#endif\n\nLV_FONT_DECLARE", start)]
        code = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <vector>
constexpr int LV_DRAW_UNIT_IDLE=-1, LV_OPA_COVER=255, LV_BLEND_MODE_NORMAL=0;
constexpr int LV_COLOR_FORMAT_RGB565=1, LV_IMAGE_FLAGS_USER1=1;
constexpr int LV_DRAW_TASK_STATE_IN_PROGRESS=1, LV_DRAW_TASK_STATE_FINISHED=2;
constexpr int MALLOC_CAP_SPIRAM=1, MALLOC_CAP_8BIT=2, ESP_OK=0, ESP_ERR_NO_MEM=1;
#define ESP_ERROR_CHECK(result) do { if ((result)!=ESP_OK) throw std::runtime_error("no memory"); } while(0)
struct lv_area_t { int32_t x1,y1,x2,y2; };
bool lv_area_intersect(lv_area_t* out,const lv_area_t* a,const lv_area_t* b) {
    *out={std::max(a->x1,b->x1),std::max(a->y1,b->y1),std::min(a->x2,b->x2),std::min(a->y2,b->y2)};
    return out->x1<=out->x2 && out->y1<=out->y2;
}
int lv_area_get_width(const lv_area_t* a) { return a->x2-a->x1+1; }
int lv_area_get_height(const lv_area_t* a) { return a->y2-a->y1+1; }
struct Header { int w=240,h=240,stride=480,flags=0; };
struct lv_image_dsc_t { Header header; const uint8_t* data=nullptr; };
struct lv_draw_image_dsc_t {
    const void* src=nullptr;
    int opa=255,recolor_opa=0,rotation=0,scale_x=256,scale_y=256;
    int skew_x=0,skew_y=0,clip_radius=0,blend_mode=0;
    const void* bitmap_mask_src=nullptr;
    const void* colorkey=nullptr;
    bool tile=false;
};
struct lv_draw_task_t {
    lv_draw_image_dsc_t image;
    bool is_image=true;
    int preference_score=100,preferred_draw_unit_id=1,state=0;
    lv_area_t area{86,86,325,325},clip_area{86,86,325,325};
};
struct Buffer { Header header; uint8_t* data; };
struct lv_layer_t { int color_format=1; Buffer* draw_buf; lv_area_t buf_area; };
struct lv_draw_unit_t {
    int idx=2;
    const char* name=nullptr;
    int32_t (*evaluate_cb)(lv_draw_unit_t*,lv_draw_task_t*)=nullptr;
    int32_t (*dispatch_cb)(lv_draw_unit_t*,lv_layer_t*)=nullptr;
    int32_t (*delete_cb)(lv_draw_unit_t*)=nullptr;
};
static lv_draw_task_t* next_task=nullptr;
static int units=0,allocations=0,fallback_calls=0;
static bool fail_layer=false;
static std::vector<uint8_t> fallback_pixels;
void* lv_draw_create_unit(size_t size) { ++units; return calloc(1,size); }
lv_draw_image_dsc_t* lv_draw_task_get_image_dsc(lv_draw_task_t* t) { return t->is_image? &t->image:nullptr; }
lv_draw_task_t* lv_draw_get_next_available_task(lv_layer_t*,void*,int id) {
    return next_task && next_task->preferred_draw_unit_id==id && next_task->state==0? next_task:nullptr;
}
void* lv_draw_layer_alloc_buf(lv_layer_t* layer) { return fail_layer? nullptr:layer->draw_buf->data; }
void* lv_draw_layer_go_to_xy(lv_layer_t* layer,int x,int y) {
    return layer->draw_buf->data+y*layer->draw_buf->header.stride+x*2;
}
void lv_draw_dispatch_request() {}
void* heap_caps_aligned_alloc(size_t,size_t size,int) { ++allocations; return malloc(size); }
void heap_caps_free(void* p) { free(p); }
void lv_draw_sw_image(lv_draw_task_t*,const lv_draw_image_dsc_t* image,const lv_area_t*) {
    ++fallback_calls;
    const auto* source=static_cast<const lv_image_dsc_t*>(image->src);
    fallback_pixels.assign(source->data,source->data+240*240*2);
}
// METHODS
int main() {
    for (size_t offset: {0,1}) {
        std::vector<uint8_t> input(240*240*2+offset);
        for (size_t i=0;i<input.size();++i) input[i]=(i*17+i/480)%251;
        const auto original=input;
        lv_image_dsc_t source;
        source.data=input.data()+offset;
        source.header.flags=LV_IMAGE_FLAGS_USER1;
        EnsureMirrorPreviewRenderer(&source);
        auto* unit=watcher_preview_unit;
        unit->base.idx=2;
        assert(units==1);
        for (lv_area_t clip: {lv_area_t{86,86,325,325},{87,89,99,107},{310,100,325,115},{1,1,20,20}}) {
            for (lv_area_t bounds: {lv_area_t{0,0,411,411},{80,80,330,200}}) {
                const int stride=(bounds.x2-bounds.x1+1)*2+8;
                std::vector<uint8_t> pixels(stride*(bounds.y2-bounds.y1+1)+offset,0xAB);
                Buffer buffer; buffer.header.stride=stride; buffer.data=pixels.data()+offset;
                lv_layer_t layer{1,&buffer,bounds};
                lv_draw_task_t task;task.image.src=&source;task.clip_area=clip;
                next_task=&task;
                EvaluateMirrorPreview(&unit->base,&task);
                assert(task.preference_score==0 && task.preferred_draw_unit_id==2);
                assert(DispatchMirrorPreview(&unit->base,&layer)==1);
                assert(task.state==LV_DRAW_TASK_STATE_FINISHED && allocations==0);
                for (int y=bounds.y1;y<=bounds.y2;++y) {
                    for (int x=bounds.x1;x<=bounds.x2;++x) {
                        bool visible=x>=86 && x<=325 && y>=86 && y<=325 &&
                                     x>=clip.x1 && x<=clip.x2 && y>=clip.y1 && y<=clip.y2;
                        for (size_t byte=0;byte<2;++byte) {
                            auto actual=buffer.data[(y-bounds.y1)*stride+(x-bounds.x1)*2+byte];
                            auto expected=visible? source.data[((y-86)*240+239-(x-86))*2+byte]:0xAB;
                            assert(actual==expected);
                        }
                    }
                    for (int pad=(bounds.x2-bounds.x1+1)*2;pad<stride;++pad)
                        assert(buffer.data[(y-bounds.y1)*stride+pad]==0xAB);
                }
                assert(input==original);
            }
        }
        // 非镜子、未标记图像与文字不被接管，保证叠加 UI 仍按原任务顺序绘制。
        lv_draw_task_t other;other.image.src=&source;
        source.header.flags=0;EvaluateMirrorPreview(&unit->base,&other);
        assert(other.preferred_draw_unit_id==1);
        source.header.flags=LV_IMAGE_FLAGS_USER1;other.is_image=false;
        EvaluateMirrorPreview(&unit->base,&other);assert(other.preferred_draw_unit_id==1);
        // 层缓冲分配失败不推进任务；透明度和非 RGB565 格式沿用原软件绘制。
        Buffer buffer;std::vector<uint8_t> pixels(412*412*2);buffer.data=pixels.data();
        lv_layer_t layer{1,&buffer,{0,0,411,411}};
        lv_draw_task_t task;task.image.src=&source;next_task=&task;
        EvaluateMirrorPreview(&unit->base,&task);fail_layer=true;
        assert(DispatchMirrorPreview(&unit->base,&layer)==LV_DRAW_UNIT_IDLE && task.state==0);
        fail_layer=false;task.image.opa=128;
        assert(DispatchMirrorPreview(&unit->base,&layer)==1);
        for (size_t y=0;y<240;++y)
            for (size_t x=0;x<240;++x)
                for (size_t byte=0;byte<2;++byte)
                    assert(fallback_pixels[(y*240+x)*2+byte]==source.data[(y*240+239-x)*2+byte]);
        const int allocated=allocations;
        task.state=0;task.image.opa=255;layer.color_format=99;
        assert(DispatchMirrorPreview(&unit->base,&layer)==1 && allocations==allocated);
        assert(input==original);
        ReleaseMirrorPreviewRenderer(&source);
        assert(unit->source==nullptr && unit->fallback==nullptr);
        other.is_image=true;other.preferred_draw_unit_id=1;
        EvaluateMirrorPreview(&unit->base,&other);assert(other.preferred_draw_unit_id==1);
        // 下轮首次普通绘制不应分配兼容缓存。
        allocations=0;
    }
    assert(fallback_calls==4);
    auto* base=&watcher_preview_unit->base;
    base->delete_cb(base);free(base);assert(watcher_preview_unit==nullptr);
}
'''.replace("// METHODS", methods)
        compiler = shutil.which("g++") or shutil.which("clang++")
        self.assertIsNotNone(compiler)
        output = ROOT.parents[1] / ".temp"
        output.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=output) as directory:
            self.compile_and_run(compiler, str(Path(directory) / "preview-renderer-test"), code)

    def compile_and_run(self, compiler, binary, code):
        """编译指定板型宏并执行生产预览入口。"""
        compiled = subprocess.run(
            [compiler, "-x", "c++", "-std=c++17", "-O2", "-fsanitize=undefined", "-Wall", "-Wextra", "-Werror", "-", "-o", binary],
            input=code, text=True, capture_output=True,
        )
        self.assertEqual(compiled.returncode, 0, compiled.stderr)
        result = subprocess.run([binary], capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
