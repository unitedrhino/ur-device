// 设备显示抽象：状态、对话与预览入口；可选图像回调时间用于呈现延迟诊断。
#ifndef DISPLAY_H
#define DISPLAY_H

#include "emoji_collection.h"
#include "text_glyph.h"
#include "photo_ui_state.h"

#ifndef CONFIG_USE_EMOTE_MESSAGE_STYLE
#define HAVE_LVGL 1
#include <lvgl.h>
#endif

#include <esp_log.h>
#include <esp_pm.h>
#include <esp_timer.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class LvglFont;
class LvglImage;

class Theme {
public:
    Theme(const std::string& name) : name_(name) {}
    virtual ~Theme() = default;

    inline std::string name() const { return name_; }
    // storage_name 返回兼容旧固件的持久化名称，颜色变体可使用所属基础主题名称。
    virtual std::string storage_name() const { return name_; }
    virtual std::shared_ptr<LvglFont> GetTextFont() const { return nullptr; }

private:
    std::string name_;
};

class Display {
public:
    Display();
    virtual ~Display();

    virtual void SetStatus(const char* status);
    // SetPhotoStage 投递本地拍照请求代次及阶段；非拍照界面默认忽略，不改变音频状态。
    virtual void SetPhotoStage(uint32_t generation, PhotoUiStage stage) { (void)generation; (void)stage; }
    // CancelPhotoStage 在退出、断线或真实播报开始后释放拍照提示，拒绝旧任务迟到更新。
    virtual void CancelPhotoStage() {}
    virtual void ShowNotification(const char* notification, int duration_ms = 3000);
    virtual void ShowNotification(const std::string& notification, int duration_ms = 3000);
    virtual void SetEmotion(const char* emotion);
    virtual void SetChatMessage(const char* role, const char* content);
    virtual void ClearChatMessages();
    virtual void SetTheme(Theme* theme);
    virtual Theme* GetTheme() { return current_theme_; }
    virtual void UpdateStatusBar(bool update_all = false);
    virtual void SetPowerSaveMode(bool on);
    virtual bool AddTextGlyphs(const std::vector<TextGlyph>& glyphs, uint8_t bpp) { return false; }
    virtual void ClearTextGlyphs() {}
    virtual void SetEmojiCollection(std::shared_ptr<EmojiCollection>) {}
    virtual void SetupUI() { setup_ui_called_ = true; }
    virtual bool IsMonochrome() const { return false; }
    virtual bool SupportsGuiOperations() const { return false; }
    virtual void SetHideSubtitle(bool hide) { (void)hide; }
    virtual bool InsertAnimDialog(const char* name, uint32_t duration_ms) {
        (void)name;
        (void)duration_ms;
        return false;
    }
    virtual bool MountAssets(const char* partition_label) {
        (void)partition_label;
        return false;
    }
    virtual void UnmountAssets() {}
    virtual bool GetAssetData(const std::string& name, const uint8_t*& data, size_t& size) {
        (void)name;
        (void)data;
        (void)size;
        return false;
    }
    virtual void LoadAssets() {}
    virtual void SetPreviewImage(std::unique_ptr<LvglImage> image);
    /* SetStreamPreview 流式预览直通：相机解码完成即持 LVGL 锁更新图像，
     * 不经过预览队列/轮询；data 由调用方双缓冲持有，本方法不接管所有权。 */
    // received_us 为收到相机图像回调的单调时钟；0 表示调用方不提供呈现计时。
    virtual void SetStreamPreview(const uint8_t* data, uint16_t w, uint16_t h,
                                  int64_t received_us = 0) {
        (void)data;
        (void)w;
        (void)h;
        (void)received_us;
    }
    virtual bool SetTextFont(std::shared_ptr<LvglFont> text_font) {
        (void)text_font;
        return false;
    }
    virtual bool SnapshotToJpeg(std::string& jpeg_data, int quality = 80) {
        (void)jpeg_data;
        (void)quality;
        return false;
    }

    inline int width() const { return width_; }
    inline int height() const { return height_; }
    inline bool IsSetupUICalled() const { return setup_ui_called_; }

protected:
    int width_ = 0;
    int height_ = 0;
    bool setup_ui_called_ = false;  // Track if SetupUI() has been called

    Theme* current_theme_ = nullptr;

    friend class DisplayLockGuard;
    virtual bool Lock(int timeout_ms = 0) = 0;
    virtual void Unlock() = 0;
};

class DisplayLockGuard {
public:
    explicit DisplayLockGuard(Display* display)
        : display_(display), locked_(display_->Lock(30000)) {
        if (!locked_) {
            ESP_LOGE("Display", "Failed to lock display");
        }
    }
    ~DisplayLockGuard() {
        if (locked_) {
            display_->Unlock();
        }
    }

    DisplayLockGuard(const DisplayLockGuard&) = delete;
    DisplayLockGuard& operator=(const DisplayLockGuard&) = delete;

    explicit operator bool() const { return locked_; }

private:
    Display* display_;
    bool locked_;
};

class NoDisplay : public Display {
private:
    virtual bool Lock(int timeout_ms = 0) override { return true; }
    virtual void Unlock() override {}
};

#endif
