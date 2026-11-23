// LCD 显示接口：主题、预览与 SPI/RGB/MIPI 面板；SPI 完成通知约束 DMA 缓冲复用。
#ifndef LCD_DISPLAY_H
#define LCD_DISPLAY_H

#include "gif/lvgl_gif.h"
#include "lvgl_display.h"

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <atomic>
#include <array>
#include <memory>

#define PREVIEW_IMAGE_DURATION_MS 5000

class LcdDisplay : public LvglDisplay {
protected:
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;

    lv_draw_buf_t draw_buf_;
    lv_obj_t* top_bar_ = nullptr;
    lv_obj_t* status_bar_ = nullptr;
    lv_obj_t* content_ = nullptr;
    lv_obj_t* container_ = nullptr;
    lv_obj_t* side_bar_ = nullptr;
    lv_obj_t* bottom_bar_ = nullptr;
    lv_obj_t* preview_image_ = nullptr;
    lv_obj_t* emoji_label_ = nullptr;
    lv_obj_t* emoji_image_ = nullptr;
    std::unique_ptr<LvglGif> gif_controller_ = nullptr;
    lv_obj_t* emoji_box_ = nullptr;
    lv_obj_t* chat_message_label_ = nullptr;
    esp_timer_handle_t preview_timer_ = nullptr;
    std::unique_ptr<LvglImage> preview_image_cached_ = nullptr;
    bool hide_subtitle_ = false;  // Control whether to hide chat messages/subtitles
    bool preview_fullscreen_ = false;     // 镜子模式：预览图全屏并隐藏主页/状态栏
    bool preview_stream_active_ = false;  // 流式预览已进入首帧布局，稳态只更新时间源
    bool preview_remote_ = false;  // 对端视频保持其方向，不套用本地镜子的左右校正。
    lv_img_dsc_t stream_preview_dsc_{};   // 流式预览图像描述，引用相机帧，由镜子绘制路径校正方向。

#if CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
    // PreviewFrameTiming 记录一帧从图像回调到 DMA 完成的分段耗时，单位均为微秒。
    struct PreviewFrameTiming {
        uint32_t sequence;     // 已交付显示层的图像序号。
        uint32_t decode_us;    // 图像回调进入至解码结束的耗时。
        uint32_t lock_us;      // 解码结束至取得显示锁的耗时。
        uint32_t render_us;    // 图像交付至末区 flush，含排队、绘制及此前脏区发送。
        uint32_t copy_us;      // 末个脏区内部 DMA 拷贝和换序耗时。
        uint32_t submit_us;    // 末个脏区窗口命令和数据提交耗时。
        uint32_t wait_us;      // 末个脏区等待 DMA 完成的耗时。
        uint32_t flush_us;     // 末个脏区 flush 的完整耗时。
        uint32_t total_us;     // 图像回调进入至本帧 DMA 完成的耗时。
        uint32_t interval_us;  // 与上一帧 DMA 完成时刻的间隔。
        uint32_t schedule_us;  // 图像交付至 LVGL 开始渲染的等待耗时。
        uint32_t draw_us;      // 渲染开始至末区 flush，含此前脏区发送。
    };
    std::array<PreviewFrameTiming, 128> preview_timings_{};  // 仅保留最近 128 个完整呈现样本。
    size_t preview_timing_write_ = 0;                        // 下一个环形缓冲写入位置。
    size_t preview_timing_count_ = 0;                        // 当前可导出的样本数。
    uint32_t preview_sequence_ = 0;                          // 当前交付给 LVGL 的图像序号。
    uint32_t preview_completed_sequence_ = 0;  // 最近完成 DMA 的图像序号，防止 UI 重绘重复计数。
    int64_t preview_received_us_ = 0;          // 当前图像回调进入时刻。
    int64_t preview_decoded_us_ = 0;           // 当前图像解码结束时刻。
    int64_t preview_published_us_ = 0;         // 当前图像取得显示锁并交付的时刻。
    int64_t preview_render_started_us_ = 0;    // 当前图像开始 LVGL 渲染的时刻。
    int64_t preview_completed_us_ = 0;         // 上一幅图像完成 DMA 的时刻。
    uint8_t remote_slow_frames_ = 0;  // 对端视频连续超过50ms的完整DMA帧数，仅显示锁内更新。
    std::atomic<bool> remote_slow_{false};  // 网络/解码任务无锁读取的本次会话停显事实。
    // RecordPreviewFlush 在显示锁内记录完整呈现，不在实时刷新路径打印日志。
    void RecordPreviewFlush(int64_t started_us, int64_t done_us, int64_t copy_us, int64_t submit_us,
                            int64_t wait_us);
#endif

    void InitializeLcdThemes();
    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;

protected:
    // Add protected constructor
    LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
               int height);

public:
    ~LcdDisplay();
    virtual void SetEmotion(const char* emotion) override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void ClearChatMessages() override;
    virtual void SetPreviewImage(std::unique_ptr<LvglImage> image) override;
    // SetStreamPreview 流式预览直通：持 LVGL 锁更新图像，不走队列与轮询。
    void SetStreamPreview(const uint8_t* data, uint16_t w, uint16_t h,
                          int64_t received_us = 0) override;
    // SetRemotePreview 标记对端视频呈现作用域；调用者须与本地镜子互斥。
    void SetRemotePreview(bool enabled);
    // RemotePreviewSlow 返回本次对端视频是否连续三帧完整呈现超过50ms，不等待显示锁。
    bool RemotePreviewSlow() const {
#if CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
        return remote_slow_.load();
#else
        return false;
#endif
    }
#if CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER
    // PrintPreviewPerformance 在锁外导出最近样本；reset 为 true 时仅清空计数。
    void PrintPreviewPerformance(bool reset);
#endif
    // SetPreviewFullscreen 切换镜子层：隐藏主页/状态栏，实际预览尺寸由流首帧决定。
    void SetPreviewFullscreen(bool on);
    virtual void SetupUI() override;
    // Add theme switching function
    virtual void SetTheme(Theme* theme) override;

    // Set whether to hide chat messages/subtitles
    virtual void SetHideSubtitle(bool hide) override;
};

// SPI LCD display
class SpiLcdDisplay : public LcdDisplay {
public:
    SpiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                  int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                  bool swap_xy);
    // 析构时释放 SPI 刷新专用的 DMA 缓冲和完成信号量。
    ~SpiLcdDisplay();
    // Flush 将 area 区域的 px_map 像素分块发送；等待 DMA 完成后通知 LVGL 可重用缓冲。
    void Flush(const lv_area_t* area, uint8_t* px_map);

private:
    uint8_t* dma_flush_buf_[2] = {nullptr,
                                  nullptr};  // 两块内部 DMA 缓冲，准备下一块与发送当前块并行。
    void* trans_sem_ = nullptr;         // 每块 DMA 发送完成信号量（on_color_trans_done）
};

// RGB LCD display
class RgbLcdDisplay : public LcdDisplay {
public:
    RgbLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                  int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                  bool swap_xy);
};

// MIPI LCD display
class MipiLcdDisplay : public LcdDisplay {
public:
    MipiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                   int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                   bool swap_xy);
};

#endif  // LCD_DISPLAY_H
