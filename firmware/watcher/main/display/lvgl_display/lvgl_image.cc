#include "lvgl_image.h"
#include <cbin_font.h>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <cstring>

#define TAG "LvglImage"

LvglRawImage::LvglRawImage(void* data, size_t size) {
    bzero(&image_dsc_, sizeof(image_dsc_));
    image_dsc_.data_size = size;
    image_dsc_.data = static_cast<uint8_t*>(data);
    image_dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
    image_dsc_.header.cf = LV_COLOR_FORMAT_RAW_ALPHA;
    image_dsc_.header.w = 0;
    image_dsc_.header.h = 0;
}

bool LvglRawImage::IsGif() const {
    auto ptr = (const uint8_t*)image_dsc_.data;
    return ptr[0] == 'G' && ptr[1] == 'I' && ptr[2] == 'F';
}

LvglCBinImage::LvglCBinImage(void* data) {
    image_dsc_ = cbin_img_dsc_create(static_cast<uint8_t*>(data));
}

LvglCBinImage::~LvglCBinImage() {
    if (image_dsc_ != nullptr) {
        cbin_img_dsc_delete(image_dsc_);
    }
}

LvglAllocatedImage::LvglAllocatedImage(void* data, size_t size) {
    bzero(&image_dsc_, sizeof(image_dsc_));
    image_dsc_.data_size = size;
    image_dsc_.data = static_cast<uint8_t*>(data);
    capacity_ = size;

    if (lv_image_decoder_get_info(&image_dsc_, &image_dsc_.header) != LV_RESULT_OK) {
        ESP_LOGE(TAG, "Failed to get image info, data: %p size: %u", data, size);
        return;
    }
    valid_ = true;
}

LvglAllocatedImage::LvglAllocatedImage(void* data, size_t size, int width, int height, int stride,
                                       int color_format) {
    bzero(&image_dsc_, sizeof(image_dsc_));
    image_dsc_.data_size = size;
    image_dsc_.data = static_cast<uint8_t*>(data);
    capacity_ = size;
    image_dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
    image_dsc_.header.cf = color_format;
    image_dsc_.header.w = width;
    image_dsc_.header.h = height;
    image_dsc_.header.stride = stride;
    valid_ = data != nullptr && size > 0 && width > 0 && height > 0;
}

bool LvglAllocatedImage::CopyFrom(const void* data, size_t size, int width, int height, int stride,
                                  int color_format) {
    if (data == nullptr || size == 0 || width <= 0 || height <= 0 || stride <= 0) {
        return false;
    }
    if (capacity_ < size) {
        auto* next =
            static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (next == nullptr) {
            return false;
        }
        if (image_dsc_.data != nullptr) {
            heap_caps_free((void*)image_dsc_.data);
        }
        image_dsc_.data = next;
        capacity_ = size;
    }
    std::memcpy((void*)image_dsc_.data, data, size);
    image_dsc_.data_size = size;
    image_dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
    image_dsc_.header.cf = color_format;
    image_dsc_.header.w = width;
    image_dsc_.header.h = height;
    image_dsc_.header.stride = stride;
    valid_ = true;
    return true;
}

LvglAllocatedImage::~LvglAllocatedImage() {
    if (image_dsc_.data) {
        heap_caps_free((void*)image_dsc_.data);
        image_dsc_.data = nullptr;
    }
}
