// LCD 界面：聊天、表情及相机预览；预览期间保留字幕内容但隐藏字幕层，避免遮挡。
#include "lcd_display.h"
#include "assets/lang_config.h"
#include "gif/lvgl_gif.h"
#include "lvgl_theme.h"
#include "settings.h"

#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_io.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <esp_psram.h>
#include <freertos/semphr.h>
#include <material_symbols.h>
#include <noto_emoji.h>
#include <src/misc/cache/lv_cache.h>
#include <src/draw/sw/lv_draw_sw.h>
#include <algorithm>
#include <cstring>
#include <vector>

#include "board.h"

#define TAG "LcdDisplay"

#if CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
#include "boards/sensecap-watcher/ui/skin_theme.h"
#include <src/draw/lv_draw_private.h>
#include <src/misc/lv_area_private.h>

// WatcherPreviewDrawUnit 接管标记过的 240×240 RGB565 图像，将简单复制或水平反转
// 合并到 LVGL 目标缓冲写入，避免通用图像混合器的逐像素状态判断。
struct WatcherPreviewDrawUnit {
    lv_draw_unit_t base;                     // 由 LVGL 分配和回收的绘制单元。
    const lv_image_dsc_t* source = nullptr;  // 当前显示实例的流描述，释放实例前解绑。
    uint8_t* fallback = nullptr;             // 透明、变换或截图格式的按需软件兼容缓存。
};
static WatcherPreviewDrawUnit* watcher_preview_unit = nullptr;  // 每个 LVGL 生命周期复用一个单元。

// IsDirectWatcherImage 判断图像是否满足直接写入条件；复杂变换继续走 LVGL 兼容路径。
static bool IsDirectWatcherImage(const lv_draw_image_dsc_t* image, const lv_area_t* area) {
    if (image == nullptr || image->src == nullptr || area == nullptr) {
        return false;
    }
    const auto* source = static_cast<const lv_image_dsc_t*>(image->src);
    return source->header.cf == LV_COLOR_FORMAT_RGB565 && source->header.w == 240 &&
           source->header.h == 240 && source->header.stride == 480 &&
           lv_area_get_width(area) == 240 && lv_area_get_height(area) == 240 &&
           image->opa == LV_OPA_COVER && image->recolor_opa == 0 && image->rotation == 0 &&
           image->scale_x == 256 && image->scale_y == 256 && image->skew_x == 0 &&
           image->skew_y == 0 && image->clip_radius == 0 && !image->bitmap_mask_src &&
           !image->colorkey && !image->tile && image->blend_mode == LV_BLEND_MODE_NORMAL;
}

// CopyMirrorPreviewRow 将源行的 pixels 个 RGB565 像素反向写入目标，不改变像素内字节序。
static void CopyMirrorPreviewRow(const uint8_t* source, uint8_t* target, size_t pixels) {
    if (((reinterpret_cast<uintptr_t>(source) | reinterpret_cast<uintptr_t>(target)) & 3) == 0 &&
        pixels % 2 == 0) {
        // 允许从字节存储按两像素读取；对齐检查避免未定义行为和小 memcpy 调用。
        using PixelPair = uint32_t __attribute__((__may_alias__));
        const auto* pairs = reinterpret_cast<const PixelPair*>(source);  // 反向读取的源像素组。
        auto* output = reinterpret_cast<PixelPair*>(target);             // 正向写入的目标像素组。
        for (size_t index = 0; index < pixels / 2; ++index) {
            const uint32_t pair = pairs[pixels / 2 - 1 - index];  // 一次读取两个相邻像素。
            output[index] = (pair << 16) | (pair >> 16);
        }
    } else {
        for (size_t index = 0; index < pixels; ++index) {
            target[index * 2] = source[(pixels - 1 - index) * 2];
            target[index * 2 + 1] = source[(pixels - 1 - index) * 2 + 1];
        }
    }
}

// EvaluateMirrorPreview 仅选择通话帧和本地镜子帧，其余图像和文字仍由原软件单元绘制。
static int32_t EvaluateMirrorPreview(lv_draw_unit_t* base, lv_draw_task_t* task) {
    const auto* image = lv_draw_task_get_image_dsc(task);  // 非图像任务返回空指针。
    const auto* source = image ? static_cast<const lv_image_dsc_t*>(image->src) : nullptr;
    if (image && source && (source->header.flags & (LV_IMAGE_FLAGS_USER1 | LV_IMAGE_FLAGS_USER2)) &&
        IsDirectWatcherImage(image, &task->area)) {
        task->preference_score = 0;
        task->preferred_draw_unit_id = base->idx;
    }
    return 0;
}

// DispatchMirrorPreview 按 LVGL 任务依赖顺序绘制镜子；局部裁剪后只写图像自身的有效区域。
static int32_t DispatchMirrorPreview(lv_draw_unit_t* base, lv_layer_t* layer) {
    auto* unit = reinterpret_cast<WatcherPreviewDrawUnit*>(base);  // 已由评估函数选择的单元。
    auto* task =
        lv_draw_get_next_available_task(layer, nullptr, base->idx);  // 避免越过重叠 UI 任务。
    if (!task || !lv_draw_layer_alloc_buf(layer))
        return LV_DRAW_UNIT_IDLE;
    task->state = LV_DRAW_TASK_STATE_IN_PROGRESS;
    const auto* image = lv_draw_task_get_image_dsc(task);  // 本任务的透明度、变换及图像源。
    const auto* source = image ? static_cast<const lv_image_dsc_t*>(image->src) : nullptr;
    if (image == nullptr || source == nullptr) {
        return LV_DRAW_UNIT_IDLE;
    }
    const bool mirror = (source->header.flags & LV_IMAGE_FLAGS_USER1) != 0;
    const bool direct = layer->color_format == LV_COLOR_FORMAT_RGB565 &&
                        IsDirectWatcherImage(image, &task->area);  // 标记图像可直接写入。
    if (direct) {
        lv_area_t area;  // 同时受图像范围、脏区与当前层缓冲边界限制的目标区域。
        if (lv_area_intersect(&area, &task->area, &task->clip_area) &&
            lv_area_intersect(&area, &area, &layer->buf_area)) {
            const size_t pixels = lv_area_get_width(&area);  // 本次实际可见的列数，允许奇数宽。
            auto* target = static_cast<uint8_t*>(
                lv_draw_layer_go_to_xy(layer, area.x1 - layer->buf_area.x1,
                                       area.y1 - layer->buf_area.y1));  // 首行目的像素。
            for (int32_t y = area.y1; y <= area.y2; ++y) {
                const uint8_t* row =
                    source->data + static_cast<size_t>(y - task->area.y1) * source->header.stride +
                    (mirror ? (source->header.w - 1 - (area.x2 - task->area.x1)) * 2
                            : (area.x1 - task->area.x1) * 2);
                if (mirror)
                    CopyMirrorPreviewRow(row, target, pixels);
                else
                    std::memcpy(target, row, pixels * 2);
                target += layer->draw_buf->header.stride;
            }
        }
    } else {
        if (!mirror) {
            // 远端图像若出现缩放、透明度或裁剪，保留 LVGL 原语义而不误用镜像缓存。
            lv_draw_sw_image(task, image, &task->area);
            task->state = LV_DRAW_TASK_STATE_FINISHED;
            lv_draw_dispatch_request();
            return 1;
        }
        // 非常规透明/变换/截图继续使用 LVGL 原绘制，按需校正源图以保留完整语义。
        if (!unit->fallback) {
            unit->fallback = static_cast<uint8_t*>(
                heap_caps_aligned_alloc(16, 240 * 240 * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            ESP_ERROR_CHECK(unit->fallback ? ESP_OK : ESP_ERR_NO_MEM);
        }
        for (size_t y = 0; y < 240; ++y)
            CopyMirrorPreviewRow(source->data + y * source->header.stride, unit->fallback + y * 480,
                                 240);
        lv_image_dsc_t mirrored = *source;  // 兼容路径的临时镜像描述，不改相机原始帧。
        mirrored.data = unit->fallback;
        mirrored.header.stride = 480;
        lv_draw_image_dsc_t compatible = *image;  // 透明度、变换与遮罩均沿用原绘制参数。
        compatible.src = &mirrored;
        lv_draw_sw_image(task, &compatible, &task->area);
    }
    task->state = LV_DRAW_TASK_STATE_FINISHED;
    lv_draw_dispatch_request();
    return 1;
}

// EnsureMirrorPreviewRenderer 在显示锁内注册一次，绑定当前描述；绘制单元由 LVGL 生命周期持有。
static void EnsureMirrorPreviewRenderer(const lv_image_dsc_t* source) {
    if (!watcher_preview_unit) {
        watcher_preview_unit = static_cast<WatcherPreviewDrawUnit*>(
            lv_draw_create_unit(sizeof(WatcherPreviewDrawUnit)));
        watcher_preview_unit->base.name = "WatcherMirror";
        watcher_preview_unit->base.evaluate_cb = EvaluateMirrorPreview;
        watcher_preview_unit->base.dispatch_cb = DispatchMirrorPreview;
        watcher_preview_unit->base.delete_cb = [](lv_draw_unit_t* base) -> int32_t {
            auto* unit =
                reinterpret_cast<WatcherPreviewDrawUnit*>(base);  // LVGL 正在回收的绘制单元。
            heap_caps_free(unit->fallback);
            watcher_preview_unit = nullptr;
            return 0;
        };
    }
    watcher_preview_unit->source = source;
}

// ReleaseMirrorPreviewRenderer 解绑即将释放的显示描述；保留 LVGL 所有的单元供下次显示实例复用。
static void ReleaseMirrorPreviewRenderer(const lv_image_dsc_t* source) {
    if (watcher_preview_unit && watcher_preview_unit->source == source) {
        watcher_preview_unit->source = nullptr;
        heap_caps_free(watcher_preview_unit->fallback);
        watcher_preview_unit->fallback = nullptr;
    }
}
#endif

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);
LV_FONT_DECLARE(BUILTIN_ICON_FONT);
LV_FONT_DECLARE(font_material_symbols_30_4);
LV_FONT_DECLARE(font_noto_emoji_30_4);

void LcdDisplay::InitializeLcdThemes() {
    auto text_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_TEXT_FONT);
    auto icon_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_ICON_FONT);
    auto large_icon_font = std::make_shared<LvglBuiltInFont>(&font_material_symbols_30_4);
    auto emoji_font = std::make_shared<LvglBuiltInFont>(&font_noto_emoji_30_4);

    // light theme
    auto light_theme = new LvglTheme("light");
    light_theme->set_background_color(lv_color_hex(0xFFFFFF));
    light_theme->set_text_color(lv_color_hex(0x000000));
    light_theme->set_chat_background_color(lv_color_hex(0xE0E0E0));
    light_theme->set_user_bubble_color(lv_color_hex(0x00FF00));
    light_theme->set_assistant_bubble_color(lv_color_hex(0xDDDDDD));
    light_theme->set_system_bubble_color(lv_color_hex(0xFFFFFF));
    light_theme->set_system_text_color(lv_color_hex(0x000000));
    light_theme->set_border_color(lv_color_hex(0x000000));
    light_theme->set_low_battery_color(lv_color_hex(0x000000));
    light_theme->set_text_font(text_font);
    light_theme->set_icon_font(icon_font);
    light_theme->set_large_icon_font(large_icon_font);
    light_theme->set_emoji_font(emoji_font);

    // dark theme
    auto dark_theme = new LvglTheme("dark");
    dark_theme->set_background_color(lv_color_hex(0x000000));
    dark_theme->set_text_color(lv_color_hex(0xFFFFFF));
    dark_theme->set_chat_background_color(lv_color_hex(0x1F1F1F));
    dark_theme->set_user_bubble_color(lv_color_hex(0x00FF00));
    dark_theme->set_assistant_bubble_color(lv_color_hex(0x222222));
    dark_theme->set_system_bubble_color(lv_color_hex(0x000000));
    dark_theme->set_system_text_color(lv_color_hex(0xFFFFFF));
    dark_theme->set_border_color(lv_color_hex(0xFFFFFF));
    dark_theme->set_low_battery_color(lv_color_hex(0xFF0000));
    dark_theme->set_text_font(text_font);
    dark_theme->set_icon_font(icon_font);
    dark_theme->set_large_icon_font(large_icon_font);
    dark_theme->set_emoji_font(emoji_font);

    auto& theme_manager = LvglThemeManager::GetInstance();
    theme_manager.RegisterTheme("light", light_theme);
    theme_manager.RegisterTheme("dark", dark_theme);
#if CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
    watcher::PrepareSkinThemes();
#endif
}

LcdDisplay::LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                       int height)
    : panel_io_(panel_io), panel_(panel) {
    width_ = width;
    height_ = height;

    // Initialize LCD themes
    InitializeLcdThemes();

    // Load theme from settings
#if CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
    // 首次升级自动收敛旧白底配置，后续启动使用已选皮肤，不依赖人工清NVS。
    Settings settings("display", true);
    const auto stored_theme = settings.GetString("theme", "dark");
    const int skin_version = settings.GetInt("watch_skin_v", 0);
    const int stored_skin = settings.GetInt("watch_skin", -1);
    const std::string theme_name = watcher::BootSkin(stored_theme, skin_version, stored_skin);
    const auto selected_skin = watcher::SkinIndex(theme_name);  // 主题名称与变体索引独立保存。
    const std::string storage_theme = watcher::kSkins[selected_skin].light ? "light" : "dark";
    if (storage_theme != stored_theme)
        settings.SetString("theme", storage_theme);
    if (stored_skin != static_cast<int>(selected_skin))
        settings.SetInt("watch_skin", static_cast<int>(selected_skin));
    if (skin_version < 1)
        settings.SetInt("watch_skin_v", 1);
#else
    Settings settings("display", false);
    std::string theme_name = settings.GetString("theme", "light");
#endif
    current_theme_ = LvglThemeManager::GetInstance().GetTheme(theme_name);

    // Create a timer to hide the preview image
    esp_timer_create_args_t preview_timer_args = {
        .callback =
            [](void* arg) {
                LcdDisplay* display = static_cast<LcdDisplay*>(arg);
                display->SetPreviewImage(nullptr);
            },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "preview_timer",
        .skip_unhandled_events = false,
    };
    esp_timer_create(&preview_timer_args, &preview_timer_);
}

SpiLcdDisplay::SpiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                             int width, int height, int offset_x, int offset_y, bool mirror_x,
                             bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {
    // draw white
    std::vector<uint16_t> buffer(width_, 0xFFFF);
    for (int y = 0; y < height_; y++) {
        esp_lcd_panel_draw_bitmap(panel_, 0, y, width_, y + 1, buffer.data());
    }

    // Set the display to on
    ESP_LOGI(TAG, "Turning display on");
    {
        esp_err_t __err = esp_lcd_panel_disp_on_off(panel_, true);
        if (__err == ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGW(TAG, "Panel does not support disp_on_off; assuming ON");
        } else {
            ESP_ERROR_CHECK(__err);
        }
    }

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

#if CONFIG_SPIRAM
    // lv image cache, currently only PNG is supported
    size_t psram_size_mb = esp_psram_get_size() / 1024 / 1024;
    if (psram_size_mb >= 8) {
#if CONFIG_LV_USE_BUILTIN_MALLOC
        // Base TLSF pool is CONFIG_LV_MEM_SIZE_KILOBYTES in PSRAM; grow it so
        // the 2MB decoded-image cache does not exhaust a 2MB-PSRAM-safe pool.
        // TLSF rejects any single pool larger than LV_MEM_SIZE when
        // LV_MEM_POOL_EXPAND_SIZE is 0 (block_size_max = 1 << ceil(log2(LV_MEM_SIZE))).
        const size_t extra_total = 2560 * 1024;
        const size_t chunk_size = LV_MEM_SIZE;
        size_t added = 0;
        while (added < extra_total) {
            size_t this_chunk = extra_total - added;
            if (this_chunk > chunk_size) {
                this_chunk = chunk_size;
            }
            void* pool = heap_caps_malloc(this_chunk, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (pool == nullptr || lv_mem_add_pool(pool, this_chunk) == nullptr) {
                if (pool != nullptr) {
                    heap_caps_free(pool);
                }
                ESP_LOGE(TAG, "Failed to add %uKB LVGL PSRAM pool (added %uKB)",
                         (unsigned)(this_chunk / 1024), (unsigned)(added / 1024));
                break;
            }
            added += this_chunk;
        }
        if (added == extra_total) {
            ESP_LOGI(TAG, "Added %uKB LVGL pool in PSRAM", (unsigned)(added / 1024));
        }
#endif
        lv_image_cache_resize(2 * 1024 * 1024, true);
        ESP_LOGI(TAG, "Use 2MB of PSRAM for image cache");
    } else if (psram_size_mb >= 2) {
        lv_image_cache_resize(512 * 1024, true);
        ESP_LOGI(TAG, "Use 512KB of PSRAM for image cache");
    }
#endif

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
#if CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
    // 显示与音频输出同级，低于音频输入优先级 8，兼顾预览响应和语音采集。
    port_cfg.task_priority = 4;
    port_cfg.task_max_sleep_ms = 5;
#else
    port_cfg.task_priority = 1;
#endif
#if CONFIG_SOC_CPU_CORES_NUM > 1
    // LVGL 绘制不占用 JPEG 解码所在的核，避免显示任务与远端画面解码互相抢占。
    port_cfg.task_affinity = 0;
#endif
    lvgl_port_init(&port_cfg);
#if CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
    // 先注册直接绘制单元；远端通话帧不经过本地镜子入口，也必须能被该单元接管。
    EnsureMirrorPreviewRenderer(nullptr);
#endif

    ESP_LOGI(TAG, "Adding LCD display");
    // Watcher使用内部DMA条带缓冲，绘制模式始终按条带容量刷新，避免要求整帧内存。
#if CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
    const bool full_frame = false;  // 通话远端240×240只刷新脏区，避免整帧渲染无关区域。
#if CONFIG_UR_SOFTWARE_AEC && CONFIG_UR_HUMAN_CALL_UDP_AV
    // 64KiB数据缓存与软件回采共用内部SRAM，12行双缓冲为相机DMA及AI重连留出约19KiB。
    constexpr int render_rows = 12;
#else
    // 其余Watcher构建保留24行双缓冲，远端240×240画面分10批刷新。
    constexpr int render_rows = 24;
#endif
#else
    const bool full_frame = false;  // 其余 SPI 板保持现有条带配置。
    constexpr int render_rows = 16;  // 其他 SPI 板保留原有16行绘制批次。
#endif
    // 注册显示与替换回调必须在同一把锁内，避免首帧已发送却更换完成回调。
    DisplayLockGuard initialization_lock(this);
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * (full_frame ? height_ : render_rows)),
        .double_buffer = true,
        .trans_size = 0,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = false,
        .rotation =
            {
                .swap_xy = swap_xy,
                .mirror_x = mirror_x,
                .mirror_y = mirror_y,
            },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags =
            {
                .buff_dma = !full_frame,
                .buff_spiram = full_frame,
                .sw_rotate = 0,
                .swap_bytes = !full_frame,
                .full_refresh = full_frame,
                .direct_mode = 0,
            },
    };

    display_ = lvgl_port_add_disp(&display_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

    if (full_frame) {
        for (auto& buffer : dma_flush_buf_) {
            buffer = static_cast<uint8_t*>(
                heap_caps_malloc(static_cast<size_t>(width_) * 16 * sizeof(uint16_t),
                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
            ESP_ERROR_CHECK(buffer ? ESP_OK : ESP_ERR_NO_MEM);
        }
        trans_sem_ = xSemaphoreCreateBinary();
        ESP_ERROR_CHECK(trans_sem_ ? ESP_OK : ESP_ERR_NO_MEM);
        // 每块完成只唤醒发送任务；只有整帧发送结束后才允许 LVGL 重用绘制缓冲。
        const esp_lcd_panel_io_callbacks_t callbacks = {
            .on_color_trans_done = [](esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*,
                                      void* context) -> bool {
                BaseType_t woken = pdFALSE;  // ISR 唤醒更高优先级任务时请求调度。
                xSemaphoreGiveFromISR(static_cast<SemaphoreHandle_t>(context), &woken);
                return woken == pdTRUE;
            },
        };
        ESP_ERROR_CHECK(
            esp_lcd_panel_io_register_event_callbacks(panel_io_, &callbacks, trans_sem_));
        lv_display_set_user_data(display_, this);
        lv_display_set_flush_cb(
            display_, [](lv_display_t* display, const lv_area_t* area, uint8_t* pixels) {
                static_cast<SpiLcdDisplay*>(lv_display_get_user_data(display))->Flush(area, pixels);
            });
#if CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
        // 渲染开始事件把定时等待与真正绘图分开，避免把整个间隔误判成刷新周期。
        lv_display_add_event_cb(
            display_,
            [](lv_event_t* event) {
                auto* display = static_cast<SpiLcdDisplay*>(lv_event_get_user_data(event));  // 本次渲染的显示实例。
                display->preview_render_started_us_ = esp_timer_get_time();
            },
            LV_EVENT_RENDER_START, this);
#endif
        ESP_LOGI(TAG, "Full-frame PSRAM rendering with pipelined internal DMA strips");
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }
}

// 析构时释放本显示实例的 DMA 发送资源。
SpiLcdDisplay::~SpiLcdDisplay() {
    for (auto& buffer : dma_flush_buf_) {
        if (buffer != nullptr) {
            heap_caps_free(buffer);
            buffer = nullptr;
        }
    }
    if (trans_sem_ != nullptr) {
        vSemaphoreDelete(static_cast<SemaphoreHandle_t>(trans_sem_));
        trans_sem_ = nullptr;
    }
}

// Flush 交替准备两个 DMA 缓冲，在上一块发送期间拷贝下一块；回收后才复用旧块。
// px_map 的行跨度来自当前 LVGL 缓冲，整帧完成后再释放该缓冲的绘制所有权。
void SpiLcdDisplay::Flush(const lv_area_t* area, uint8_t* px_map) {
    const int64_t started_us = esp_timer_get_time();  // 本帧开始向面板发送的时刻。
    int64_t copy_us = 0;                              // 累计像素拷贝和字节序转换耗时。
    int64_t submit_us = 0;                            // 累计窗口命令和数据提交耗时。
    int64_t wait_us = 0;                              // 累计等待 DMA 完成耗时。
    const int width = lv_area_get_width(area);        // 当前刷新的像素列数。
    const int height = lv_area_get_height(area);      // 当前刷新的像素行数。
    const lv_draw_buf_t* draw_buffer = lv_display_get_buf_active(display_);  // LVGL 活动缓冲。
    const size_t stride = draw_buffer->header.stride;  // 源图逐行字节跨度，可能包含对齐填充。
    const size_t row_bytes = static_cast<size_t>(width) * sizeof(uint16_t);  // 每行有效像素字节数。
    auto semaphore = static_cast<SemaphoreHandle_t>(trans_sem_);  // 仅消费当前条带的 DMA 完成通知。
    for (int row = 0; row < height; row += 16) {
        uint8_t* buffer = dma_flush_buf_[(row / 16) % 2];      // 与正在发送的上一块使用不同内存。
        const int64_t copy_started_us = esp_timer_get_time();  // 当前条带进入准备阶段。
        const int rows = std::min(16, height - row);           // 末尾不足一条带时仅发送有效行。
        for (int index = 0; index < rows; ++index) {
            memcpy(buffer + static_cast<size_t>(index) * row_bytes,
                   px_map + static_cast<size_t>(row + index) * stride, row_bytes);
        }
        lv_draw_sw_rgb565_swap(buffer, static_cast<uint32_t>(width) * rows);
        copy_us += esp_timer_get_time() - copy_started_us;
        // 下一块已准备好后才等待上一块，让拷贝与 DMA 并行；任意时刻仅一块在途。
        if (row != 0) {
            const int64_t wait_started_us = esp_timer_get_time();  // 上一块剩余发送等待。
            ESP_ERROR_CHECK(xSemaphoreTake(semaphore, pdMS_TO_TICKS(1000)) == pdTRUE
                                ? ESP_OK
                                : ESP_ERR_TIMEOUT);
            wait_us += esp_timer_get_time() - wait_started_us;
        }
        // 必须在提交前清除旧信号；提交后清除会丢失已完成的快速传输通知。
        xSemaphoreTake(semaphore, 0);
        const int64_t submit_started_us = esp_timer_get_time();  // 当前条带提交面板的时刻。
        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel_, area->x1, area->y1 + row, area->x2 + 1,
                                                  area->y1 + row + rows, buffer));
        submit_us += esp_timer_get_time() - submit_started_us;
    }
    // 最后一块没有下一次循环代为回收，须完成后才能把整帧缓冲交还 LVGL。
    const int64_t wait_started_us = esp_timer_get_time();  // 最后一块的剩余发送等待。
    ESP_ERROR_CHECK(xSemaphoreTake(semaphore, pdMS_TO_TICKS(1000)) == pdTRUE ? ESP_OK
                                                                             : ESP_ERR_TIMEOUT);
    wait_us += esp_timer_get_time() - wait_started_us;
#if CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
    // 局部刷新可能包含多个脏区，最后一块完成才代表本次预览呈现结束。
    if (lv_display_flush_is_last(display_)) {
        RecordPreviewFlush(started_us, esp_timer_get_time(), copy_us, submit_us, wait_us);
    }
#else
    (void)started_us;
    (void)copy_us;
    (void)submit_us;
    (void)wait_us;
#endif
    lv_display_flush_ready(display_);
}

// RGB LCD implementation
RgbLcdDisplay::RgbLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                             int width, int height, int offset_x, int offset_y, bool mirror_x,
                             bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {
    // draw white
    std::vector<uint16_t> buffer(width_, 0xFFFF);
    for (int y = 0; y < height_; y++) {
        esp_lcd_panel_draw_bitmap(panel_, 0, y, width_, y + 1, buffer.data());
    }

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
    port_cfg.timer_period_ms = 50;
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD display");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .buffer_size = static_cast<uint32_t>(width_ * 20),
        .double_buffer = true,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .rotation =
            {
                .swap_xy = swap_xy,
                .mirror_x = mirror_x,
                .mirror_y = mirror_y,
            },
        .flags =
            {
                .buff_dma = 1,
                .swap_bytes = 0,
                .full_refresh = 1,
                .direct_mode = 1,
            },
    };

    const lvgl_port_display_rgb_cfg_t rgb_cfg = {.flags = {
                                                     .bb_mode = true,
                                                     .avoid_tearing = true,
                                                 }};

    display_ = lvgl_port_add_disp_rgb(&display_cfg, &rgb_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add RGB display");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }
}

MipiLcdDisplay::MipiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                               int width, int height, int offset_x, int offset_y, bool mirror_x,
                               bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {
    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD display");
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = panel_io,
        .panel_handle = panel,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * 50),
        .double_buffer = false,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = false,
        /* Rotation values must be same as used in esp_lcd for initial settings of the screen */
        .rotation =
            {
                .swap_xy = swap_xy,
                .mirror_x = mirror_x,
                .mirror_y = mirror_y,
            },
        .flags =
            {
                .buff_dma = true,
                .buff_spiram = false,
                .sw_rotate = true,
            },
    };

    const lvgl_port_display_dsi_cfg_t dpi_cfg = {.flags = {
                                                     .avoid_tearing = false,
                                                 }};
    display_ = lvgl_port_add_disp_dsi(&disp_cfg, &dpi_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }
}

LcdDisplay::~LcdDisplay() {
    SetPreviewImage(nullptr);

    // Clean up GIF controller
    if (gif_controller_) {
        gif_controller_->Stop();
        gif_controller_.reset();
    }

    if (preview_timer_ != nullptr) {
        esp_timer_stop(preview_timer_);
        esp_timer_delete(preview_timer_);
    }

    {
#if CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
        // 删除图像和解绑描述使用同一显示锁，避免绘制单元读取已释放的实例。
        DisplayLockGuard lock(this);
        ReleaseMirrorPreviewRenderer(&stream_preview_dsc_);
#endif
        if (preview_image_ != nullptr)
            lv_obj_del(preview_image_);
    }
    if (chat_message_label_ != nullptr) {
        lv_obj_del(chat_message_label_);
    }
    if (emoji_label_ != nullptr) {
        lv_obj_del(emoji_label_);
    }
    if (emoji_image_ != nullptr) {
        lv_obj_del(emoji_image_);
    }
    if (emoji_box_ != nullptr) {
        lv_obj_del(emoji_box_);
    }
    if (content_ != nullptr) {
        lv_obj_del(content_);
    }
    if (bottom_bar_ != nullptr) {
        lv_obj_del(bottom_bar_);
    }
    if (status_bar_ != nullptr) {
        lv_obj_del(status_bar_);
    }
    if (top_bar_ != nullptr) {
        lv_obj_del(top_bar_);
    }
    if (side_bar_ != nullptr) {
        lv_obj_del(side_bar_);
    }
    if (container_ != nullptr) {
        lv_obj_del(container_);
    }
    if (display_ != nullptr) {
        lv_display_delete(display_);
    }

    if (panel_ != nullptr) {
        esp_lcd_panel_del(panel_);
    }
    if (panel_io_ != nullptr) {
        esp_lcd_panel_io_del(panel_io_);
    }
}

bool LcdDisplay::Lock(int timeout_ms) { return lvgl_port_lock(timeout_ms); }

void LcdDisplay::Unlock() { lvgl_port_unlock(); }

#if CONFIG_USE_WECHAT_MESSAGE_STYLE
void LcdDisplay::SetupUI() {
    // Prevent duplicate calls - if already called, return early
    if (setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() called multiple times, skipping duplicate call");
        return;
    }

    Display::SetupUI();  // Mark SetupUI as called
    DisplayLockGuard lock(this);

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);
    lv_obj_set_style_bg_color(screen, lvgl_theme->background_color(), 0);

    /* Container */
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(container_, 0, 0);
    lv_obj_set_flex_flow(container_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_pad_row(container_, 0, 0);
    lv_obj_set_style_bg_color(container_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_color(container_, lvgl_theme->border_color(), 0);

    /* Layer 1: Top bar - for status icons */
    top_bar_ = lv_obj_create(container_);
    lv_obj_set_size(top_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(top_bar_, 0, 0);
    lv_obj_set_style_bg_opa(top_bar_, LV_OPA_50, 0);  // 50% opacity background
    lv_obj_set_style_bg_color(top_bar_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_width(top_bar_, 0, 0);
    lv_obj_set_style_pad_all(top_bar_, 0, 0);
    lv_obj_set_style_pad_top(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_left(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_pad_right(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_flex_flow(top_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(top_bar_, LV_SCROLLBAR_MODE_OFF);

    // Left icon
    network_label_ = lv_label_create(top_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, icon_font, 0);
    lv_obj_set_style_text_color(network_label_, lvgl_theme->text_color(), 0);

    // Right icons container
    lv_obj_t* right_icons = lv_obj_create(top_bar_);
    lv_obj_set_size(right_icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right_icons, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_icons, 0, 0);
    lv_obj_set_style_pad_all(right_icons, 0, 0);
    lv_obj_set_flex_flow(right_icons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_icons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    mute_label_ = lv_label_create(right_icons);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, icon_font, 0);
    lv_obj_set_style_text_color(mute_label_, lvgl_theme->text_color(), 0);

    battery_label_ = lv_label_create(right_icons);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, icon_font, 0);
    lv_obj_set_style_text_color(battery_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_margin_left(battery_label_, lvgl_theme->spacing(2), 0);

    /* Layer 2: Status bar - for center text labels */
    status_bar_ = lv_obj_create(screen);
    lv_obj_set_size(status_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);  // Transparent background
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_style_pad_top(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_scrollbar_mode(status_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_layout(status_bar_, LV_LAYOUT_NONE, 0);  // Use absolute positioning
    lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, 0);        // Overlap with top_bar_

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(notification_label_, LV_HOR_RES * 0.8);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(notification_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_align(notification_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(status_label_, LV_HOR_RES * 0.8);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);
    lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 0);

    /* Content - Chat area */
    content_ = lv_obj_create(container_);
    lv_obj_set_style_radius(content_, 0, 0);
    lv_obj_set_width(content_, LV_HOR_RES);
    lv_obj_set_flex_grow(content_, 1);
    lv_obj_set_style_pad_all(content_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_border_width(content_, 0, 0);
    lv_obj_set_style_bg_color(content_, lvgl_theme->chat_background_color(),
                              0);  // Background for chat area

    // Enable scrolling for chat content
    lv_obj_set_scrollbar_mode(content_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(content_, LV_DIR_VER);

    // Create a flex container for chat messages
    lv_obj_set_flex_flow(content_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(content_, lvgl_theme->spacing(4), 0);  // Space between messages

    // We'll create chat messages dynamically in SetChatMessage
    chat_message_label_ = nullptr;

    low_battery_popup_ = lv_obj_create(screen);
    lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(low_battery_popup_, LV_HOR_RES * 0.9, text_font->line_height * 2);
    lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, -lvgl_theme->spacing(4));
    lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);
    lv_obj_set_style_radius(low_battery_popup_, lvgl_theme->spacing(4), 0);
    low_battery_label_ = lv_label_create(low_battery_popup_);
    lv_label_set_text(low_battery_label_, Lang::Strings::BATTERY_NEED_CHARGE);
    lv_obj_set_style_text_color(low_battery_label_, lv_color_white(), 0);
    lv_obj_center(low_battery_label_);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);

    emoji_image_ = lv_img_create(screen);
    lv_obj_align(emoji_image_, LV_ALIGN_TOP_MID, 0,
                 text_font->line_height + lvgl_theme->spacing(8));

    // Display AI logo while booting
    emoji_label_ = lv_label_create(screen);
    lv_obj_center(emoji_label_);
    lv_obj_set_style_text_font(emoji_label_, large_icon_font, 0);
    lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(emoji_label_, MATERIAL_SYMBOLS_ROBOT_2);
}
#if CONFIG_IDF_TARGET_ESP32P4
#define MAX_MESSAGES 40
#else
#define MAX_MESSAGES 20
#endif
void LcdDisplay::SetChatMessage(const char* role, const char* content) {
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "SetChatMessage('%s', '%s') called before SetupUI() - message will be lost!",
                 role, content);
    }
    DisplayLockGuard lock(this);
    if (content_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG,
                     "SetChatMessage('%s', '%s') failed: content_ is nullptr (SetupUI() was called "
                     "but container not created)",
                     role, content);
        }
        return;
    }

    // Check if message count exceeds limit
    uint32_t child_count = lv_obj_get_child_cnt(content_);
    if (child_count >= MAX_MESSAGES) {
        // Delete the oldest message (first child object)
        lv_obj_t* first_child = lv_obj_get_child(content_, 0);
        if (first_child != nullptr) {
            lv_obj_del(first_child);
            // Refresh child count after deletion
            child_count = lv_obj_get_child_cnt(content_);
        }
        // Scroll to the last message immediately (get last_child after deletion)
        if (child_count > 0) {
            lv_obj_t* last_child = lv_obj_get_child(content_, child_count - 1);
            if (last_child != nullptr && lv_obj_is_valid(last_child)) {
                lv_obj_scroll_to_view_recursive(last_child, LV_ANIM_OFF);
            }
        }
    }

    // Collapse system messages (if it's a system message, check if the last message is also a
    // system message)
    if (strcmp(role, "system") == 0) {
        // Refresh child count to get accurate count after potential deletion above
        child_count = lv_obj_get_child_cnt(content_);
        if (child_count > 0) {
            // Get the last message container
            lv_obj_t* last_container = lv_obj_get_child(content_, child_count - 1);
            if (last_container != nullptr && lv_obj_is_valid(last_container) &&
                lv_obj_get_child_cnt(last_container) > 0) {
                // Get the bubble inside the container
                lv_obj_t* last_bubble = lv_obj_get_child(last_container, 0);
                if (last_bubble != nullptr && lv_obj_is_valid(last_bubble)) {
                    // Check if bubble type is system message
                    void* bubble_type_ptr = lv_obj_get_user_data(last_bubble);
                    if (bubble_type_ptr != nullptr &&
                        strcmp((const char*)bubble_type_ptr, "system") == 0) {
                        // If the last message is also a system message, delete it
                        lv_obj_del(last_container);
                    }
                }
            }
        }
    } else {
        // Hide the centered AI logo
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }

    // Avoid empty message boxes
    if (strlen(content) == 0) {
        return;
    }

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);

    // Create a message bubble
    lv_obj_t* msg_bubble = lv_obj_create(content_);
    lv_obj_set_style_radius(msg_bubble, 8, 0);
    lv_obj_set_scrollbar_mode(msg_bubble, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_width(msg_bubble, 0, 0);
    lv_obj_set_style_pad_all(msg_bubble, lvgl_theme->spacing(4), 0);

    // Create the message text
    lv_obj_t* msg_text = lv_label_create(msg_bubble);
    lv_label_set_text(msg_text, content);

    // Calculate bubble width constraints
    lv_coord_t max_width = LV_HOR_RES * 85 / 100 - 16;  // 85% of screen width
    lv_coord_t min_width = 20;

    // Let LVGL calculate the natural text width first
    lv_obj_set_width(msg_text, LV_SIZE_CONTENT);
    lv_obj_update_layout(msg_text);
    lv_coord_t text_width = lv_obj_get_width(msg_text);

    // Ensure text width is not less than minimum width
    if (text_width < min_width) {
        text_width = min_width;
    }

    // Constrain to max width
    lv_coord_t bubble_width = (text_width < max_width) ? text_width : max_width;

    // Set message text width
    lv_obj_set_width(msg_text, bubble_width);
    lv_label_set_long_mode(msg_text, LV_LABEL_LONG_WRAP);

    // Set bubble width
    lv_obj_set_width(msg_bubble, bubble_width);
    lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);

    // Set alignment and style based on message role
    if (strcmp(role, "user") == 0) {
        // User messages are right-aligned with green background
        lv_obj_set_style_bg_color(msg_bubble, lvgl_theme->user_bubble_color(), 0);
        lv_obj_set_style_bg_opa(msg_bubble, LV_OPA_70, 0);
        // Set text color for contrast
        lv_obj_set_style_text_color(msg_text, lvgl_theme->text_color(), 0);

        // Set custom attribute to mark bubble type
        lv_obj_set_user_data(msg_bubble, (void*)"user");

        // Set appropriate width for content
        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);

        // Don't grow
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    } else if (strcmp(role, "assistant") == 0) {
        // Assistant messages are left-aligned with white background
        lv_obj_set_style_bg_color(msg_bubble, lvgl_theme->assistant_bubble_color(), 0);
        lv_obj_set_style_bg_opa(msg_bubble, LV_OPA_70, 0);
        // Set text color for contrast
        lv_obj_set_style_text_color(msg_text, lvgl_theme->text_color(), 0);

        // Set custom attribute to mark bubble type
        lv_obj_set_user_data(msg_bubble, (void*)"assistant");

        // Set appropriate width for content
        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);

        // Don't grow
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    } else if (strcmp(role, "system") == 0) {
        // System messages are center-aligned with light gray background
        lv_obj_set_style_bg_color(msg_bubble, lvgl_theme->system_bubble_color(), 0);
        lv_obj_set_style_bg_opa(msg_bubble, LV_OPA_70, 0);
        // Set text color for contrast
        lv_obj_set_style_text_color(msg_text, lvgl_theme->system_text_color(), 0);

        // Set custom attribute to mark bubble type
        lv_obj_set_user_data(msg_bubble, (void*)"system");

        // Set appropriate width for content
        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);

        // Don't grow
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    }

    // Create a full-width container for user messages to ensure right alignment
    if (strcmp(role, "user") == 0) {
        // Create a full-width container
        lv_obj_t* container = lv_obj_create(content_);
        lv_obj_set_width(container, LV_HOR_RES);
        lv_obj_set_height(container, LV_SIZE_CONTENT);

        // Make container transparent and borderless
        lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(container, 0, 0);
        lv_obj_set_style_pad_all(container, 0, 0);

        // Move the message bubble into this container
        lv_obj_set_parent(msg_bubble, container);

        // Right align the bubble in the container
        lv_obj_align(msg_bubble, LV_ALIGN_RIGHT_MID, -25, 0);

        // Auto-scroll to this container
        lv_obj_scroll_to_view_recursive(container, LV_ANIM_ON);
    } else if (strcmp(role, "system") == 0) {
        // Create full-width container for system messages to ensure center alignment
        lv_obj_t* container = lv_obj_create(content_);
        lv_obj_set_width(container, LV_HOR_RES);
        lv_obj_set_height(container, LV_SIZE_CONTENT);

        lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(container, 0, 0);
        lv_obj_set_style_pad_all(container, 0, 0);

        lv_obj_set_parent(msg_bubble, container);
        lv_obj_align(msg_bubble, LV_ALIGN_CENTER, 0, 0);
        lv_obj_scroll_to_view_recursive(container, LV_ANIM_ON);
    } else {
        // For assistant messages
        // Left align assistant messages
        lv_obj_align(msg_bubble, LV_ALIGN_LEFT_MID, 0, 0);

        // Auto-scroll to the message bubble
        lv_obj_scroll_to_view_recursive(msg_bubble, LV_ANIM_ON);
    }

    // Store reference to the latest message label
    chat_message_label_ = msg_text;
}

void LcdDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    DisplayLockGuard lock(this);
    if (content_ == nullptr) {
        return;
    }

    if (image == nullptr) {
        return;
    }

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    // Create a message bubble for image preview
    lv_obj_t* img_bubble = lv_obj_create(content_);
    lv_obj_set_style_radius(img_bubble, 8, 0);
    lv_obj_set_scrollbar_mode(img_bubble, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_width(img_bubble, 0, 0);
    lv_obj_set_style_pad_all(img_bubble, lvgl_theme->spacing(4), 0);

    // Set image bubble background color (similar to system message)
    lv_obj_set_style_bg_color(img_bubble, lvgl_theme->assistant_bubble_color(), 0);
    lv_obj_set_style_bg_opa(img_bubble, LV_OPA_70, 0);

    // Set custom attribute to mark bubble type
    lv_obj_set_user_data(img_bubble, (void*)"image");

    // Create the image object inside the bubble
    lv_obj_t* preview_image = lv_image_create(img_bubble);

    // Calculate appropriate size for the image
    lv_coord_t max_width = LV_HOR_RES * 70 / 100;   // 70% of screen width
    lv_coord_t max_height = LV_VER_RES * 50 / 100;  // 50% of screen height

    // Calculate zoom factor to fit within maximum dimensions
    auto img_dsc = image->image_dsc();
    lv_coord_t img_width = img_dsc->header.w;
    lv_coord_t img_height = img_dsc->header.h;
    if (img_width == 0 || img_height == 0) {
        img_width = max_width;
        img_height = max_height;
        ESP_LOGW(TAG, "Invalid image dimensions: %ld x %ld, using default dimensions: %ld x %ld",
                 img_width, img_height, max_width, max_height);
    }

    lv_coord_t zoom_w = (max_width * 256) / img_width;
    lv_coord_t zoom_h = (max_height * 256) / img_height;
    lv_coord_t zoom = (zoom_w < zoom_h) ? zoom_w : zoom_h;

    // Ensure zoom doesn't exceed 256 (100%)
    if (zoom > 256)
        zoom = 256;

    // Set image properties
    lv_image_set_src(preview_image, img_dsc);
    lv_image_set_scale(preview_image, zoom);

    // Add event handler to clean up LvglImage when image is deleted
    // We need to transfer ownership of the unique_ptr to the event callback
    LvglImage* raw_image = image.release();  // Release ownership of smart pointer
    lv_obj_add_event_cb(
        preview_image,
        [](lv_event_t* e) {
            LvglImage* img = (LvglImage*)lv_event_get_user_data(e);
            if (img != nullptr) {
                delete img;  // Properly release memory by deleting LvglImage object
            }
        },
        LV_EVENT_DELETE, (void*)raw_image);

    // Calculate actual scaled image dimensions
    lv_coord_t scaled_width = (img_width * zoom) / 256;
    lv_coord_t scaled_height = (img_height * zoom) / 256;

    // Set bubble size to be 16 pixels larger than the image (8 pixels on each side)
    lv_obj_set_width(img_bubble, scaled_width + 16);
    lv_obj_set_height(img_bubble, scaled_height + 16);

    // Don't grow in flex layout
    lv_obj_set_style_flex_grow(img_bubble, 0, 0);

    // Center the image within the bubble
    lv_obj_center(preview_image);

    // Left align the image bubble like assistant messages
    lv_obj_align(img_bubble, LV_ALIGN_LEFT_MID, 0, 0);

    // Auto-scroll to the image bubble
    lv_obj_scroll_to_view_recursive(img_bubble, LV_ANIM_ON);
}

void LcdDisplay::ClearChatMessages() {
    DisplayLockGuard lock(this);
    if (content_ == nullptr) {
        return;
    }

    // Use lv_obj_clean to delete all children of content_ (chat message bubbles)
    lv_obj_clean(content_);

    // Reset chat_message_label_ as it has been deleted
    chat_message_label_ = nullptr;

    // Show the centered AI logo (emoji_label_) again
    if (emoji_label_ != nullptr) {
        lv_obj_remove_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }

    ESP_LOGI(TAG, "Chat messages cleared");
}
#else
void LcdDisplay::SetupUI() {
    // Prevent duplicate calls - if already called, return early
    if (setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() called multiple times, skipping duplicate call");
        return;
    }

    Display::SetupUI();  // Mark SetupUI as called
    DisplayLockGuard lock(this);
    LvglTheme* lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);
    lv_obj_set_style_bg_color(screen, lvgl_theme->background_color(), 0);

    /* Container - used as background */
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(container_, 0, 0);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_bg_color(container_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_color(container_, lvgl_theme->border_color(), 0);

    /* Bottom layer: emoji_box_ - centered display */
    emoji_box_ = lv_obj_create(screen);
    lv_obj_set_size(emoji_box_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(emoji_box_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(emoji_box_, 0, 0);
    lv_obj_set_style_border_width(emoji_box_, 0, 0);
    lv_obj_align(emoji_box_, LV_ALIGN_CENTER, 0, 0);

    emoji_label_ = lv_label_create(emoji_box_);
    lv_obj_set_style_text_font(emoji_label_, large_icon_font, 0);
    lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(emoji_label_, MATERIAL_SYMBOLS_ROBOT_2);

    emoji_image_ = lv_img_create(emoji_box_);
    lv_obj_center(emoji_image_);
    lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);

    /* Middle layer: preview_image_ - centered display */
    preview_image_ = lv_image_create(screen);
    lv_obj_set_size(preview_image_, width_ / 2, height_ / 2);
    lv_obj_align(preview_image_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);

    /* Layer 1: Top bar - for status icons */
    top_bar_ = lv_obj_create(screen);
    lv_obj_set_size(top_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(top_bar_, 0, 0);
    lv_obj_set_style_bg_opa(top_bar_, LV_OPA_50, 0);  // 50% opacity background
    lv_obj_set_style_bg_color(top_bar_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_width(top_bar_, 0, 0);
    lv_obj_set_style_pad_all(top_bar_, 0, 0);
    lv_obj_set_style_pad_top(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_left(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_pad_right(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_flex_flow(top_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(top_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(top_bar_, LV_ALIGN_TOP_MID, 0, 0);

    // Left icon
    network_label_ = lv_label_create(top_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, icon_font, 0);
    lv_obj_set_style_text_color(network_label_, lvgl_theme->text_color(), 0);

    // Right icons container
    lv_obj_t* right_icons = lv_obj_create(top_bar_);
    lv_obj_set_size(right_icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right_icons, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_icons, 0, 0);
    lv_obj_set_style_pad_all(right_icons, 0, 0);
    lv_obj_set_flex_flow(right_icons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_icons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    mute_label_ = lv_label_create(right_icons);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, icon_font, 0);
    lv_obj_set_style_text_color(mute_label_, lvgl_theme->text_color(), 0);

    battery_label_ = lv_label_create(right_icons);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, icon_font, 0);
    lv_obj_set_style_text_color(battery_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_margin_left(battery_label_, lvgl_theme->spacing(2), 0);

    /* Layer 2: Status bar - for center text labels */
    status_bar_ = lv_obj_create(screen);
    lv_obj_set_size(status_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);  // Transparent background
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_style_pad_top(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_scrollbar_mode(status_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_layout(status_bar_, LV_LAYOUT_NONE, 0);  // Use absolute positioning
    lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, 0);        // Overlap with top_bar_

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(notification_label_, LV_HOR_RES * 0.75);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(notification_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_align(notification_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(status_label_, LV_HOR_RES * 0.75);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);
    lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 0);

#if CONFIG_USE_MULTILINE_CHAT_MESSAGE
    /* Bottom bar - auto height, grows upward with wrapped text */
    bottom_bar_ = lv_obj_create(screen);
    lv_obj_set_width(bottom_bar_, LV_HOR_RES);
    lv_obj_set_height(bottom_bar_, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(bottom_bar_, 0, 0);
    lv_obj_set_style_bg_color(bottom_bar_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_50, 0);
    lv_obj_set_style_text_color(bottom_bar_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_pad_all(bottom_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_border_width(bottom_bar_, 0, 0);
    lv_obj_set_scrollbar_mode(bottom_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, 0);

    /* chat_message_label_ placed in bottom_bar_, multiline wrapped display */
    chat_message_label_ = lv_label_create(bottom_bar_);
    lv_label_set_text(chat_message_label_, "");
    lv_obj_set_width(chat_message_label_, LV_HOR_RES - lvgl_theme->spacing(8));
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(chat_message_label_, lvgl_theme->text_color(), 0);
    lv_obj_align(chat_message_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);  // Hide until there is content
#else
    /* Top layer: Bottom bar - fixed height at bottom */
    bottom_bar_ = lv_obj_create(screen);
    lv_obj_set_size(bottom_bar_, LV_HOR_RES, text_font->line_height + lvgl_theme->spacing(8));
    lv_obj_set_style_radius(bottom_bar_, 0, 0);
    lv_obj_set_style_bg_color(bottom_bar_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_text_color(bottom_bar_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_pad_all(bottom_bar_, 0, 0);
    lv_obj_set_style_pad_left(bottom_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_pad_right(bottom_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_border_width(bottom_bar_, 0, 0);
    lv_obj_set_scrollbar_mode(bottom_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, 0);

    /* chat_message_label_ placed in bottom_bar_, single-line horizontal scroll */
    chat_message_label_ = lv_label_create(bottom_bar_);
    lv_label_set_text(chat_message_label_, "");
    lv_obj_set_width(chat_message_label_, LV_HOR_RES - lvgl_theme->spacing(8));
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(chat_message_label_, lvgl_theme->text_color(), 0);
    lv_obj_align(chat_message_label_, LV_ALIGN_CENTER, 0, 0);

    // Start scrolling after a delay (short text won't scroll)
    static lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_delay(&a, 1000);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_obj_set_style_anim(chat_message_label_, &a, LV_PART_MAIN);
    lv_obj_set_style_anim_duration(chat_message_label_, lv_anim_speed_clamped(60, 300, 60000),
                                   LV_PART_MAIN);
    lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);  // Hide until there is content
#endif

    low_battery_popup_ = lv_obj_create(screen);
    lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(low_battery_popup_, LV_HOR_RES * 0.9, text_font->line_height * 2);
    lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, -lvgl_theme->spacing(4));
    lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);
    lv_obj_set_style_radius(low_battery_popup_, lvgl_theme->spacing(4), 0);

    low_battery_label_ = lv_label_create(low_battery_popup_);
    lv_label_set_text(low_battery_label_, Lang::Strings::BATTERY_NEED_CHARGE);
    lv_obj_set_style_text_color(low_battery_label_, lv_color_white(), 0);
    lv_obj_center(low_battery_label_);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
}

// SetPreviewImage 接管当前帧；空指针结束预览并按用户设置恢复最新字幕。
void LcdDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    DisplayLockGuard lock(this);
    if (preview_image_ == nullptr) {
        ESP_LOGE(TAG, "Preview image is not initialized");
        return;
    }

    if (image == nullptr) {
        esp_timer_stop(preview_timer_);
        lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
        preview_image_cached_.reset();
        preview_stream_active_ = false;  // 流预览已退出，下次重新做首帧布局
        if (bottom_bar_ != nullptr && chat_message_label_ != nullptr) {
            const char* text = lv_label_get_text(chat_message_label_);
            if (!hide_subtitle_ && text != nullptr && text[0] != '\0') {
                lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
            }
        }
        if (gif_controller_) {
            gif_controller_->Start();
        }
        return;
    }

    // 普通照片预览恢复平滑缩放，低延迟流的最近邻选择不延续到单拍。
    lv_image_set_antialias(preview_image_, true);
    preview_image_cached_ = std::move(image);
    // 多行字幕位于预览上层且高度随文本增长，旧回复也可能完全挡住新照片。
    if (bottom_bar_ != nullptr) {
        lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
    }
    auto img_dsc = preview_image_cached_->image_dsc();
    lv_image_set_src(preview_image_, img_dsc);
    if (img_dsc->header.w > 0 && img_dsc->header.h > 0) {
        // 普通预览取半宽；镜子模式按屏宽铺满并居中。
        lv_image_set_scale(preview_image_, (preview_fullscreen_ ? 256 : 128) * width_ / img_dsc->header.w);
    }
    if (preview_fullscreen_) {
        lv_obj_align(preview_image_, LV_ALIGN_CENTER, 0, 0);
    }

    // Hide emoji_box_
    if (gif_controller_) {
        gif_controller_->Stop();
    }
    lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
    esp_timer_stop(preview_timer_);
    ESP_ERROR_CHECK(esp_timer_start_once(preview_timer_, PREVIEW_IMAGE_DURATION_MS * 1000));
}

// SetStreamPreview 流式预览直通：解码完成即持 LVGL 锁更新图像，
// 不经过 CustomLcdDisplay 的 pending 快照与 50ms 轮询，缩短显示延迟。
// data 由相机双缓冲持有；240 本地镜子在写入 LVGL 目标缓冲时校正方向，保留视频原始帧。
// 与官方 view_image_preview_flush 一致：稳态每帧只做 lv_image_set_src，
// 对象尺寸/缩放/隐藏层等布局操作只在进入流预览的首帧执行一次，
// 避免每帧触发布局重算导致整屏重绘。
void LcdDisplay::SetStreamPreview(const uint8_t* data, uint16_t w, uint16_t h,
                                  int64_t received_us) {
    if (data == nullptr || w == 0 || h == 0 || preview_image_ == nullptr) {
        return;
    }
    const int64_t decoded_us = esp_timer_get_time();  // 调用显示层时 JPEG 解码已经完成。
    DisplayLockGuard lock(this);
    if (!lock)
        return;
#if CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
    const bool mirror_preview =
        preview_fullscreen_ && !preview_remote_ && w == 240 && h == 240;  // 仅校正本地低延迟镜子。
    if (mirror_preview)
        EnsureMirrorPreviewRenderer(&stream_preview_dsc_);
    ++preview_sequence_;
    preview_received_us_ = received_us;
    preview_decoded_us_ = decoded_us;
    preview_published_us_ = esp_timer_get_time();
    preview_render_started_us_ = preview_published_us_;
#else
    (void)received_us;
    (void)decoded_us;
#endif
    stream_preview_dsc_ = {};
    stream_preview_dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
#if CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
    if (mirror_preview)
        stream_preview_dsc_.header.flags = LV_IMAGE_FLAGS_USER1;
#endif
    stream_preview_dsc_.header.cf = LV_COLOR_FORMAT_RGB565;
    stream_preview_dsc_.header.w = w;
    stream_preview_dsc_.header.h = h;
    stream_preview_dsc_.header.stride = static_cast<uint16_t>(w * 2);
    stream_preview_dsc_.data_size = static_cast<uint32_t>(w) * h * 2;
    stream_preview_dsc_.data = data;

    lv_image_set_src(preview_image_, &stream_preview_dsc_);
    if (preview_stream_active_) {
        return;
    }
    preview_stream_active_ = true;
    if (preview_fullscreen_) {
        // 240 图按原尺寸居中；避免全屏缩放占满 CPU，仅更新实际预览脏区。
        lv_obj_set_size(preview_image_, w, h);
        lv_image_set_antialias(preview_image_, true);
        lv_image_set_scale(preview_image_, 256);
#if CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
        // Watcher只分配24行绘制缓冲，即使输入为整屏尺寸也必须分条带刷新。
        lv_display_set_render_mode(display_, LV_DISPLAY_RENDER_MODE_PARTIAL);
#endif
        lv_obj_align(preview_image_, LV_ALIGN_CENTER, 0, 0);
    }
    lv_obj_remove_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
    if (gif_controller_) {
        gif_controller_->Stop();
    }
    lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    if (bottom_bar_ != nullptr) {
        lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
    }
    esp_timer_stop(preview_timer_);
}

#if CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
// RecordPreviewFlush 在新相机帧的最后脏区完成后记录呈现，避免把中途刷新当完成。
void LcdDisplay::RecordPreviewFlush(int64_t started_us, int64_t done_us, int64_t copy_us,
                                    int64_t submit_us, int64_t wait_us) {
    if (!preview_stream_active_ || preview_received_us_ == 0 ||
        preview_completed_sequence_ == preview_sequence_)
        return;
    preview_timings_[preview_timing_write_] = {
        preview_sequence_,
        static_cast<uint32_t>(preview_decoded_us_ - preview_received_us_),
        static_cast<uint32_t>(preview_published_us_ - preview_decoded_us_),
        static_cast<uint32_t>(started_us - preview_published_us_),
        static_cast<uint32_t>(copy_us),
        static_cast<uint32_t>(submit_us),
        static_cast<uint32_t>(wait_us),
        static_cast<uint32_t>(done_us - started_us),
        static_cast<uint32_t>(done_us - preview_received_us_),
        preview_completed_us_ ? static_cast<uint32_t>(done_us - preview_completed_us_) : 0,
        static_cast<uint32_t>(preview_render_started_us_ - preview_published_us_),
        static_cast<uint32_t>(started_us - preview_render_started_us_),
    };
    // 只用完整DMA耗时判断对端呈现，提交函数返回不能代表像素已经显示。
    if (preview_remote_) {
        remote_slow_frames_ = done_us - preview_received_us_ > 50000
            ? std::min<int>(remote_slow_frames_ + 1, 3) : 0;
        if (remote_slow_frames_ >= 3) remote_slow_.store(true);
    }
    preview_timing_write_ = (preview_timing_write_ + 1) % preview_timings_.size();
    preview_timing_count_ = std::min(preview_timing_count_ + 1, preview_timings_.size());
    preview_completed_sequence_ = preview_sequence_;
    preview_completed_us_ = done_us;
}

// PrintPreviewPerformance 把实时路径的定长样本复制后再输出，避免串口背压延长显示锁。
void LcdDisplay::PrintPreviewPerformance(bool reset) {
    std::vector<PreviewFrameTiming> samples;  // 离开显示锁后使用的独立样本快照。
    {
        DisplayLockGuard lock(this);
        if (!lock)
            return;
        if (reset) {
            preview_timing_write_ = preview_timing_count_ = 0;
            preview_completed_sequence_ = preview_sequence_;
            preview_completed_us_ = 0;
        } else {
            samples.reserve(preview_timing_count_);
            const size_t first =
                (preview_timing_write_ + preview_timings_.size() - preview_timing_count_) %
                preview_timings_.size();  // 最旧样本位置。
            for (size_t index = 0; index < preview_timing_count_; ++index)
                samples.push_back(preview_timings_[(first + index) % preview_timings_.size()]);
        }
    }
    printf("[UR_PREVIEW_PERF] BEGIN count=%u tick_hz=%u reset=%d\n",
           static_cast<unsigned>(samples.size()), static_cast<unsigned>(configTICK_RATE_HZ),
           static_cast<int>(reset));
    for (const auto& sample : samples) {
        printf("[UR_PREVIEW_PERF] %u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n",
               static_cast<unsigned>(sample.sequence), static_cast<unsigned>(sample.decode_us),
               static_cast<unsigned>(sample.lock_us), static_cast<unsigned>(sample.render_us),
               static_cast<unsigned>(sample.copy_us), static_cast<unsigned>(sample.submit_us),
               static_cast<unsigned>(sample.wait_us), static_cast<unsigned>(sample.flush_us),
               static_cast<unsigned>(sample.total_us), static_cast<unsigned>(sample.interval_us),
               static_cast<unsigned>(sample.schedule_us), static_cast<unsigned>(sample.draw_us));
    }
    printf("[UR_PREVIEW_PERF] END\n");
}
#endif

// SetPreviewFullscreen 镜子模式隐藏主页与状态栏，把预览图置于最前；退出时恢复容器。
// 进入时先准备完整视口；首帧再按实际尺寸选择 416 全屏或 240 居中局部刷新。
void LcdDisplay::SetPreviewFullscreen(bool on) {
    DisplayLockGuard lock(this);
    preview_fullscreen_ = on;
#if CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
    // 退出镜子仍保留条带模式，切换整帧模式会超过实际绘制缓冲并触发LVGL断言。
    if (!on && display_ != nullptr) {
        lv_display_set_render_mode(display_, LV_DISPLAY_RENDER_MODE_PARTIAL);
    }
#endif
    if (preview_image_ != nullptr) {
        lv_obj_set_size(preview_image_, on ? width_ : width_ / 2, on ? height_ : height_ / 2);
        lv_obj_align(preview_image_, LV_ALIGN_CENTER, 0, 0);
        if (on) lv_obj_move_foreground(preview_image_);
    }
    for (auto* obj : {status_bar_, top_bar_, content_, side_bar_}) {
        if (obj == nullptr) continue;
        if (on) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

// SetRemotePreview 由显示锁串行更新，远端画面保留原方向，本地镜子仍沿用既有校正。
void LcdDisplay::SetRemotePreview(bool enabled) {
    DisplayLockGuard lock(this);
    if (lock) {
        preview_remote_ = enabled;
#if CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
        remote_slow_frames_ = 0;
        remote_slow_.store(false);
#endif
    }
}

void LcdDisplay::SetChatMessage(const char* role, const char* content) {
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "SetChatMessage('%s', '%s') called before SetupUI() - message will be lost!",
                 role, content);
    }
    DisplayLockGuard lock(this);
    if (chat_message_label_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG,
                     "SetChatMessage('%s', '%s') failed: chat_message_label_ is nullptr (SetupUI() "
                     "was called but label not created)",
                     role, content);
        }
        return;
    }
    lv_anim_delete(chat_message_label_, nullptr);
    lv_label_set_text(chat_message_label_, content);
    // 预览期间仍保存新字幕，但不能让增量回复重新遮挡照片。
    if (bottom_bar_ != nullptr) {
        if (content == nullptr || content[0] == '\0' || preview_image_cached_ || hide_subtitle_) {
            lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        } else if (!hide_subtitle_) {
            lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        }
    }
#if CONFIG_USE_MULTILINE_CHAT_MESSAGE
    // Re-align bottom_bar_ after text change so it stays anchored to the bottom
    // as its height adapts to the wrapped content.
    if (bottom_bar_ != nullptr) {
        lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, 0);
    }
#endif
}

void LcdDisplay::ClearChatMessages() {
    DisplayLockGuard lock(this);
    // In non-wechat mode, just clear the chat message label and hide the bar
    if (chat_message_label_ != nullptr) {
        lv_label_set_text(chat_message_label_, "");
    }
    if (bottom_bar_ != nullptr) {
        lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
    }
}
#endif

void LcdDisplay::SetEmotion(const char* emotion) {
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "SetEmotion('%s') called before SetupUI() - emotion will not be displayed!",
                 emotion);
    }
    if (emoji_image_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG,
                     "SetEmotion('%s') failed: emoji_image_ is nullptr (SetupUI() was called but "
                     "emoji image not created)",
                     emotion);
        }
        return;
    }

    auto emoji_collection = static_cast<LvglTheme*>(current_theme_)->emoji_collection();
    auto image = emoji_collection != nullptr ? emoji_collection->GetEmojiImage(emotion) : nullptr;
    if (image == nullptr) {
        auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
        const char* utf8 = noto_emoji_get_utf8(emotion);
        const lv_font_t* emotion_font = lvgl_theme->emoji_font()->font();
        if (utf8 == nullptr) {
            utf8 = material_symbols_get_utf8(emotion);
            emotion_font = lvgl_theme->large_icon_font()->font();
        }
        if (utf8 != nullptr && emoji_label_ != nullptr) {
            DisplayLockGuard lock(this);
            if (gif_controller_) {
                gif_controller_->Stop();
                gif_controller_.reset();
            }
            lv_obj_set_style_text_font(emoji_label_, emotion_font, 0);
            lv_label_set_text(emoji_label_, utf8);
            lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    DisplayLockGuard lock(this);
    // Stop any running GIF animation in the same lock scope as setting new image
    // to prevent LVGL from accessing freed image data between operations
    if (gif_controller_) {
        gif_controller_->Stop();
        gif_controller_.reset();
    }
    if (image->IsGif()) {
        // Create new GIF controller
        gif_controller_ = std::make_unique<LvglGif>(image->image_dsc());

        if (gif_controller_->IsLoaded()) {
            // Set up frame update callback
            gif_controller_->SetFrameCallback(
                [this]() { lv_image_set_src(emoji_image_, gif_controller_->image_dsc()); });

            // Set initial frame and start animation
            lv_image_set_src(emoji_image_, gif_controller_->image_dsc());
            gif_controller_->Start();

            // Show GIF, hide others
            lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        } else {
            ESP_LOGE(TAG, "Failed to load GIF for emotion: %s", emotion);
            gif_controller_.reset();
        }
    } else {
        lv_image_set_src(emoji_image_, image->image_dsc());
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
    }

#if CONFIG_USE_WECHAT_MESSAGE_STYLE
    // In WeChat message style, if emotion is neutral, don't display it
    uint32_t child_count = lv_obj_get_child_cnt(content_);
    if (strcmp(emotion, "neutral") == 0 && child_count > 0) {
        // Stop GIF animation if running
        if (gif_controller_) {
            gif_controller_->Stop();
            gif_controller_.reset();
        }

        lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }
#endif
}

void LcdDisplay::SetTheme(Theme* theme) {
    DisplayLockGuard lock(this);

    auto lvgl_theme = static_cast<LvglTheme*>(theme);

    // Get the active screen
    lv_obj_t* screen = lv_screen_active();

    // Set font
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    if (text_font->line_height >= 40) {
        lv_obj_set_style_text_font(mute_label_, large_icon_font, 0);
        lv_obj_set_style_text_font(battery_label_, large_icon_font, 0);
        lv_obj_set_style_text_font(network_label_, large_icon_font, 0);
    } else {
        lv_obj_set_style_text_font(mute_label_, icon_font, 0);
        lv_obj_set_style_text_font(battery_label_, icon_font, 0);
        lv_obj_set_style_text_font(network_label_, icon_font, 0);
    }

    // Set parent text color
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);

    // Set background image
    if (lvgl_theme->background_image() != nullptr) {
        lv_obj_set_style_bg_image_src(container_, lvgl_theme->background_image()->image_dsc(), 0);
    } else {
        lv_obj_set_style_bg_image_src(container_, nullptr, 0);
        lv_obj_set_style_bg_color(container_, lvgl_theme->background_color(), 0);
    }

    // Update top bar background color with 50% opacity
    if (top_bar_ != nullptr) {
        lv_obj_set_style_bg_opa(top_bar_, LV_OPA_50, 0);
        lv_obj_set_style_bg_color(top_bar_, lvgl_theme->background_color(), 0);
    }

    // Update status bar elements
    lv_obj_set_style_text_color(network_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(status_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(notification_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(mute_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(battery_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);

    // If we have the chat message style, update all message bubbles
#if CONFIG_USE_WECHAT_MESSAGE_STYLE
    // Set content background opacity
    lv_obj_set_style_bg_opa(content_, LV_OPA_TRANSP, 0);

    // Iterate through all children of content (message containers or bubbles)
    uint32_t child_count = lv_obj_get_child_cnt(content_);
    for (uint32_t i = 0; i < child_count; i++) {
        lv_obj_t* obj = lv_obj_get_child(content_, i);
        if (obj == nullptr)
            continue;

        lv_obj_t* bubble = nullptr;

        // Check if this object is a container or bubble
        // If it's a container (user or system message), get its child as bubble
        // If it's a bubble (assistant message), use it directly
        if (lv_obj_get_child_cnt(obj) > 0) {
            // Might be a container, check if it's a user or system message container
            // User and system message containers are transparent
            lv_opa_t bg_opa = lv_obj_get_style_bg_opa(obj, LV_PART_MAIN);
            if (bg_opa == LV_OPA_TRANSP) {
                // This is a user or system message container
                bubble = lv_obj_get_child(obj, 0);
            } else {
                // This might be an assistant message bubble itself
                bubble = obj;
            }
        } else {
            // No child elements, might be other UI elements, skip
            continue;
        }

        if (bubble == nullptr)
            continue;

        // Use saved user data to identify bubble type
        void* bubble_type_ptr = lv_obj_get_user_data(bubble);
        if (bubble_type_ptr != nullptr) {
            const char* bubble_type = static_cast<const char*>(bubble_type_ptr);

            // Apply correct color based on bubble type
            if (strcmp(bubble_type, "user") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->user_bubble_color(), 0);
            } else if (strcmp(bubble_type, "assistant") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->assistant_bubble_color(), 0);
            } else if (strcmp(bubble_type, "system") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->system_bubble_color(), 0);
            } else if (strcmp(bubble_type, "image") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->system_bubble_color(), 0);
            }

            // Update border color
            lv_obj_set_style_border_color(bubble, lvgl_theme->border_color(), 0);

            // Update text color for the message
            if (lv_obj_get_child_cnt(bubble) > 0) {
                lv_obj_t* text = lv_obj_get_child(bubble, 0);
                if (text != nullptr) {
                    // Set text color based on bubble type
                    if (strcmp(bubble_type, "system") == 0) {
                        lv_obj_set_style_text_color(text, lvgl_theme->system_text_color(), 0);
                    } else {
                        lv_obj_set_style_text_color(text, lvgl_theme->text_color(), 0);
                    }
                }
            }
        } else {
            ESP_LOGW(TAG, "child[%lu] Bubble type is not found", i);
        }
    }
#else
    // Simple UI mode - just update the main chat message
    if (chat_message_label_ != nullptr) {
        lv_obj_set_style_text_color(chat_message_label_, lvgl_theme->text_color(), 0);
    }

    if (emoji_label_ != nullptr) {
        lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);
    }

    // Update bottom bar background color with 50% opacity
    if (bottom_bar_ != nullptr) {
        lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_50, 0);
        lv_obj_set_style_bg_color(bottom_bar_, lvgl_theme->background_color(), 0);
    }
#endif

    // Update low battery popup
    lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);

    // No errors occurred. Save theme to settings
    Display::SetTheme(lvgl_theme);
}

void LcdDisplay::SetHideSubtitle(bool hide) {
    DisplayLockGuard lock(this);
    hide_subtitle_ = hide;

    // Immediately update UI visibility based on the setting
    if (bottom_bar_ != nullptr) {
        if (hide || preview_image_cached_) {
            lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        } else {
            // Only show if there is actual content to display
            const char* text =
                (chat_message_label_ != nullptr) ? lv_label_get_text(chat_message_label_) : nullptr;
            if (text != nullptr && text[0] != '\0') {
                lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}
