// 宿主字体夹具：读取已锁定32位小端cbin布局，在64位LVGL中使用同一字形和字距数据。
#pragma once
#include <lvgl.h>
#include <cassert>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <vector>

// CbinHostFont 持有原Flash字体字节及重定位后的宿主描述；生命周期覆盖引用它的LVGL对象。
struct CbinHostFont {
    std::vector<uint8_t> bytes;                // 真实字体资源原始字节，创建后不再改变容量。
    std::vector<lv_font_fmt_txt_cmap_t> maps;  // 指针宽度转换后的字符映射。
    lv_font_fmt_txt_kern_classes_t kerning{};  // 目标资源使用字偶距分类，按原映射重定位。
    lv_font_fmt_txt_dsc_t description{};       // 字形格式与目标端相同。
    lv_font_t font{};                          // LVGL宿主运行接口。
    // U32、U16显式按小端读取，避免未对齐或宿主指针宽度影响字节布局。
    static uint32_t U32(const uint8_t* p) {
        return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
    }
    static uint16_t U16(const uint8_t* p) { return uint16_t(p[0]) | uint16_t(p[1]) << 8; }
    // Load 接受当前组件的分类字偶距、四位抗锯齿资源，变化时明确失败。
    void Load(const char* path) {
        std::ifstream file(path, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(file), {});
        assert(bytes.size() > 64);
        auto* raw = bytes.data();  // 下列偏移来自该组件32位LVGL字体格式。
        font.line_height = U32(raw + 12);
        font.base_line = U32(raw + 16);
        font.underline_position = int8_t(raw[21]);
        font.underline_thickness = raw[22];
        auto* descriptor = raw + U32(raw + 24);
        description.glyph_bitmap = descriptor + U32(descriptor);
        description.glyph_dsc =
            reinterpret_cast<const lv_font_fmt_txt_glyph_dsc_t*>(descriptor + U32(descriptor + 4));
        const uint16_t packed = U16(descriptor + 18);  // cmap_num/bpp/kern_classes/bitmap_format。
        description.cmap_num = packed & 511;
        description.bpp = (packed >> 9) & 15;
        description.bitmap_format = packed >> 14;
        description.stride = descriptor[20];
        assert(description.bpp == 4);
        description.kern_scale = U16(descriptor + 16);
        description.kern_classes = (packed >> 13) & 1;
        if (U32(descriptor + 12)) {
            assert(description.kern_classes);
            auto* kern = descriptor + U32(descriptor + 12);
            kerning.class_pair_values = reinterpret_cast<const int8_t*>(kern + U32(kern));
            kerning.left_class_mapping = kern + U32(kern + 4);
            kerning.right_class_mapping = kern + U32(kern + 8);
            kerning.left_class_cnt = kern[12];
            kerning.right_class_cnt = kern[13];
            description.kern_dsc = &kerning;
        }
        maps.resize(description.cmap_num);
        auto* map_bytes = descriptor + U32(descriptor + 8);
        for (size_t i = 0; i < maps.size(); ++i) {
            auto* item = map_bytes + i * 20;
            auto& map = maps[i];
            map.range_start = U32(item);
            map.range_length = U16(item + 4);
            map.glyph_id_start = U16(item + 6);
            map.unicode_list = U32(item + 8)
                                   ? reinterpret_cast<const uint16_t*>(map_bytes + U32(item + 8))
                                   : nullptr;
            map.glyph_id_ofs_list = U32(item + 12) ? map_bytes + U32(item + 12) : nullptr;
            map.list_length = U16(item + 16);
            map.type = static_cast<lv_font_fmt_txt_cmap_type_t>(item[18]);
        }
        description.cmaps = maps.data();
        font.dsc = &description;
        font.get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt;
        font.get_glyph_bitmap = lv_font_get_bitmap_fmt_txt;
    }
};
