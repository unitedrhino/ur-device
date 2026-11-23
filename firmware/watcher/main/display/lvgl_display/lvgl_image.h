#pragma once

#include <lvgl.h>

// Wrap around lv_img_dsc_t
class LvglImage {
public:
    virtual const lv_img_dsc_t* image_dsc() const = 0;
    virtual bool IsGif() const { return false; }
    virtual ~LvglImage() = default;
};

class LvglRawImage : public LvglImage {
public:
    LvglRawImage(void* data, size_t size);
    virtual const lv_img_dsc_t* image_dsc() const override { return &image_dsc_; }
    virtual bool IsGif() const;

private:
    lv_img_dsc_t image_dsc_{};
};

class LvglCBinImage : public LvglImage {
public:
    LvglCBinImage(void* data);
    virtual ~LvglCBinImage();
    virtual const lv_img_dsc_t* image_dsc() const override { return image_dsc_; }

private:
    lv_img_dsc_t* image_dsc_ = nullptr;
};

class LvglAllocatedImage : public LvglImage {
public:
    /* LvglAllocatedImage 创建空的可复用描述；CopyFrom 首次使用时分配缓冲。 */
    LvglAllocatedImage() = default;
    LvglAllocatedImage(void* data, size_t size);
    LvglAllocatedImage(void* data, size_t size, int width, int height, int stride,
                       int color_format);
    virtual ~LvglAllocatedImage();
    virtual const lv_img_dsc_t* image_dsc() const override { return &image_dsc_; }
    bool IsValid() const { return valid_; }
    // SetFlags 标记图像所属的显示路径，供 Watcher 的直接绘制单元跳过通用混合器。
    void SetFlags(uint16_t flags) { image_dsc_.header.flags = flags; }
    /* CopyFrom 将外部图像拷贝到自有缓冲；容量足够时不重新分配。 */
    bool CopyFrom(const void* data, size_t size, int width, int height, int stride,
                  int color_format);

private:
    lv_img_dsc_t image_dsc_;
    bool valid_ = false;
    size_t capacity_ = 0;
};

// LvglBorrowedImage 只引用外部缓冲，不接管所有权：析构不释放 data。
// 用于相机双缓冲零拷贝预览，调用方保证缓冲在下一帧替换前有效。
class LvglBorrowedImage : public LvglImage {
public:
    LvglBorrowedImage(const void* data, int width, int height, int stride, int color_format) {
        image_dsc_ = {};
        image_dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
        image_dsc_.header.cf = color_format;
        image_dsc_.header.w = width;
        image_dsc_.header.h = height;
        image_dsc_.header.stride = stride;
        image_dsc_.data_size = static_cast<uint32_t>(stride) * height;
        image_dsc_.data = static_cast<const uint8_t*>(data);
    }
    virtual const lv_img_dsc_t* image_dsc() const override { return &image_dsc_; }

private:
    lv_img_dsc_t image_dsc_{};
};
