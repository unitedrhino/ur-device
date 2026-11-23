// Watcher圆屏视图：表盘、设置与通讯录复用固定对象池，业务动作只通过稳定目标回调。
#pragma once
#include <lvgl.h>
#include <material_symbols.h>
#include <algorithm>
#include <functional>
#include <string>
#include <vector>
#include "menu_model.h"
#include "skin_palette.h"
#include "ur_signaling_contacts.h"

namespace watcher {
// DialView 只在显示任务使用；字体由板级主题持有，联系人仅在Render期间借用。
class DialView {
public:
    // Create 创建独立屏幕；pressed_revision读取按下代次，on_swipe将上下滑动交给宿主门禁。
    void Create(std::function<void(MenuTarget, uint32_t)> on_click,
                std::function<uint32_t()> pressed_revision,
                std::function<void(MenuInput, uint32_t)> on_swipe = {}) {
        on_click_ = std::move(on_click);
        pressed_revision_ = std::move(pressed_revision);
        on_swipe_ = std::move(on_swipe);
        screen_ = lv_obj_create(nullptr);
        lv_obj_remove_flag(screen_, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_add_event_cb(
            screen_,
            [](lv_event_t* event) {
                auto* self = static_cast<DialView*>(
                    lv_event_get_user_data(event));  // 显示任务内的视图实例。
                auto* input = lv_indev_active();     // 使用实际指针方向，抬手后禁止补发点击。
                if (!input || !self->on_swipe_)
                    return;
                const auto direction =
                    lv_indev_get_gesture_dir(input);  // 只消费上下滑，左滑由宿主返回。
                if (direction != LV_DIR_TOP && direction != LV_DIR_BOTTOM)
                    return;
                lv_indev_wait_release(input);
                self->on_swipe_(
                    direction == LV_DIR_TOP ? MenuInput::SwipeNext : MenuInput::SwipePrevious,
                    self->pressed_revision_());
            },
            LV_EVENT_GESTURE, this);
        list_ = lv_obj_create(screen_);
        lv_obj_remove_style_all(list_);
        lv_obj_remove_flag(list_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(list_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(list_, LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_set_pos(list_, 58, 130);
        lv_obj_set_size(list_, 296, 216);
        lv_obj_set_style_pad_bottom(list_, 76, 0);  // 末项也能滚到中央，保留底部空白。
        lv_obj_set_scrollbar_mode(list_, LV_SCROLLBAR_MODE_OFF);
        lv_obj_add_event_cb(
            list_,
            [](lv_event_t* event) {
                auto* animation =
                    static_cast<lv_anim_t*>(lv_event_get_param(event));  // 原生滚动动画。
                if (animation)
                    lv_anim_set_duration(animation, 150);
            },
            LV_EVENT_SCROLL_BEGIN, nullptr);
        title_ = lv_label_create(screen_);
        detail_ = lv_label_create(screen_);
        clock_ = lv_label_create(screen_);
        progress_ = lv_bar_create(screen_);

        for (size_t index = 0; index < 9; ++index) {
            auto& slot = slots_[index];  // 事件上下文与固定对象池同寿命。
            slot.owner = this;
            slot.button = lv_button_create(screen_);
            lv_obj_remove_flag(slot.button, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(slot.button, LV_OBJ_FLAG_GESTURE_BUBBLE);
            lv_obj_set_style_pad_all(slot.button, 0, 0);
            lv_obj_set_style_border_width(slot.button, 0, 0);
            lv_obj_set_style_shadow_width(slot.button, 0, 0);
            slot.label = lv_label_create(slot.button);
            slot.secondary = lv_label_create(slot.button);
            slot.icon = lv_label_create(slot.button);
            lv_obj_add_event_cb(
                slot.button,
                [](lv_event_t* event) {
                    auto* slot = static_cast<Slot*>(
                        lv_event_get_user_data(event));  // 固定槽，不从文字推导目标。
                    if (lv_event_get_code(event) == LV_EVENT_PRESSED) {
                        slot->pressed_target = slot->target;
                        slot->revision = slot->owner->pressed_revision_();
                    } else if (lv_event_get_code(event) == LV_EVENT_SHORT_CLICKED &&
                               slot->pressed_target == slot->target) {
                        slot->owner->on_click_(slot->target, slot->revision);
                    }
                },
                LV_EVENT_ALL, &slot);
        }
        SetPalette(kSkins[0]);
    }
    // SetPalette 只更新已有对象的颜色，同一皮肤重复渲染不重绑样式或字体。
    void SetPalette(const SkinPalette& skin) {
        if (palette_ready_ && palette_ == &skin)
            return;
        palette_ = &skin;
        palette_ready_ = true;
        lv_obj_set_style_bg_color(screen_, lv_color_hex(skin.background), 0);
        lv_obj_set_style_text_color(screen_, lv_color_hex(skin.text), 0);
        lv_obj_set_style_text_color(title_, lv_color_hex(last_page_ == MenuPage::Home ? skin.accent : skin.text), 0);
        lv_obj_set_style_bg_color(progress_, lv_color_hex(skin.surface), 0);
        lv_obj_set_style_bg_color(progress_, lv_color_hex(skin.accent), LV_PART_INDICATOR);
        for (auto& slot : slots_) {
            lv_obj_set_style_bg_color(slot.button, lv_color_hex(slot.selected ? skin.accent : skin.surface), 0);
            lv_obj_set_style_text_color(slot.button, lv_color_hex(slot.selected ? skin.on_accent : skin.text), 0);
        }
    }
    lv_obj_t* Screen() const { return screen_; }  // 借用屏幕，由显示宿主统一生命周期管理。
    // ButtonFor 暴露可见目标供真实LVGL输入回归和焦点组借用，不转移所有权。
    lv_obj_t* ButtonFor(MenuTarget target) const {
        for (const auto& slot : slots_)
            if (slot.target == target && lv_obj_is_visible(slot.button))
                return slot.button;
        return nullptr;
    }
    // SetFonts 在主题释放前重绑所有对象；force用于主题替换，同一主题滚动不重复刷新样式。
    void SetFonts(const lv_font_t* primary, const lv_font_t* secondary, const lv_font_t* icons,
                  const lv_font_t* clock, bool force = false) {
        if (!force && primary_ == primary && secondary_ == secondary && icons_ == icons &&
            clock_font_ == clock)
            return;
        primary_ = primary;
        secondary_ = secondary;
        icons_ = icons;
        clock_font_ = clock;
        lv_obj_set_style_text_font(screen_, primary_, 0);
        lv_obj_set_style_text_color(screen_, lv_color_hex(palette_->text), 0);
        lv_obj_set_style_text_font(detail_, secondary_, 0);
        lv_obj_set_style_text_font(clock_, clock_font_, 0);
        for (auto& slot : slots_) {
            lv_obj_set_style_text_font(slot.label, primary_, 0);
            lv_obj_set_style_text_font(slot.secondary, secondary_, 0);
            lv_obj_set_style_text_font(slot.icon, icons_, 0);
        }
    }
    // UpdateClock 仅在分钟文字变化时提交，空闲轮询不产生持续重绘。
    void UpdateClock(const char* value) {
        if (clock_text_ == value)
            return;
        clock_text_ = value;
        if (last_page_ == MenuPage::Home)
            lv_label_set_text(clock_, value);
    }
    // Render 按页面铺排；contacts固定最多四项，network_title由原事务状态提供。
    void Render(const MenuModel& model, const std::vector<UrCallContact>& contacts, bool charging,
                const std::string& network_title, const std::string& version) {
        const auto page = model.Page();  // 页面与焦点只在显示任务读取。
        const bool animate =
            last_page_ == MenuPage::Home && page == MenuPage::Home && last_focus_ != model.Focus();
        const bool list_page = page == MenuPage::Main || page == MenuPage::Network ||
                               page == MenuPage::Power || page == MenuPage::Skins;  // 列表保持固定行，只滚动裁剪视口。
        for (auto& slot : slots_) {
            slot.motion_y = lv_obj_get_y(slot.button);
            slot.motion_visible =
                last_page_ == page && !lv_obj_has_flag(slot.button, LV_OBJ_FLAG_HIDDEN);
            lv_anim_delete(slot.button, nullptr);
            lv_obj_add_flag(slot.button, LV_OBJ_FLAG_HIDDEN);
            if (!list_page && lv_obj_get_parent(slot.button) != screen_)
                lv_obj_set_parent(slot.button, screen_);
        }
        if (list_page) {
            lv_obj_remove_flag(list_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_scroll_to_y(list_, 0, LV_ANIM_OFF);
            lv_obj_add_flag(list_, LV_OBJ_FLAG_HIDDEN);
        }
        for (auto* object : {title_, detail_, clock_, progress_})
            lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
        if (page == MenuPage::Home) {
            const char* names[] = {"AI 对话", "通讯录", "拍照", "镜子",
                                   "设置"};  // 表盘语义与稳定目标同序。
            const char* icons[] = {MATERIAL_SYMBOLS_CHAT_BUBBLE, MATERIAL_SYMBOLS_PERSON,
                                   MATERIAL_SYMBOLS_PHOTO_CAMERA, MATERIAL_SYMBOLS_IMAGE,
                                   MATERIAL_SYMBOLS_SETTINGS};
            Text(clock_, clock_text_.c_str(), 48, 108, 190, 64);
            Text(title_, names[model.Focus()], 48, 204, 180, 90);
            lv_obj_set_style_text_align(clock_, LV_TEXT_ALIGN_LEFT, 0);
            lv_obj_set_style_text_align(title_, LV_TEXT_ALIGN_LEFT, 0);
            lv_obj_set_style_text_color(title_, lv_color_hex(palette_->accent), 0);
            for (int delta = -1; delta <= 1; ++delta) {
                const size_t index = (model.Focus() + 5 + delta) % 5;  // 只呈现当前项和相邻两项。
                const bool selected = delta == 0;
                const int target_y =
                    66 + (delta + 1) * 98;  // LVGL延迟布局，动画终点必须使用目标坐标。
                auto* button = Button(index, static_cast<MenuTarget>(index), "",
                                      selected ? 266 : 254, target_y, selected ? 88 : 72,
                                      selected ? 88 : 72, selected, icons[index]);
                if (animate) {
                    lv_anim_t animation;  // 动画只改变可见图标位置，期间业务焦点已同步。
                    lv_anim_init(&animation);
                    lv_anim_set_var(&animation, button);
                    const int direction = (model.Focus() + 5 - last_focus_) % 5 <= 2
                                              ? 1
                                              : -1;    // 环形相邻焦点的滚动方向。
                    const auto& slot = slots_[index];  // 连续旋转保留屏幕当前位置，避免反向跳帧。
                    lv_anim_set_values(
                        &animation, slot.motion_visible ? slot.motion_y : target_y + direction * 98,
                        target_y);
                    lv_anim_set_duration(&animation, 150);
                    lv_anim_set_path_cb(&animation, lv_anim_path_ease_out);
                    lv_anim_set_exec_cb(&animation, [](void* object, int32_t y) {
                        lv_obj_set_y(static_cast<lv_obj_t*>(object), y);
                    });
                    lv_anim_start(&animation);
                }
            }
        } else if (page == MenuPage::Contacts) {
            Header("通讯录", model.Focus() == 0);
            if (!model.ContactsLoading())
                Button(1, MenuTarget::Refresh, "", 290, 68, 48, 48, model.Focus() == 1,
                       MATERIAL_SYMBOLS_REFRESH);
            if (model.ContactsLoading() || model.ContactsFailed() || contacts.empty()) {
                Text(detail_,
                     model.ContactsLoading()  ? "正在加载"
                     : model.ContactsFailed() ? "加载失败，请重试"
                                              : "暂无可拨打联系人",
                     66, 180, 280, 76);
                if (model.ContactsFailed())
                    Button(2, MenuTarget::Refresh, "重试", 126, 264, 160, 58, false);
            } else {
                for (size_t row = 0; row < 2 && model.ContactsSlice() + row < contacts.size();
                     ++row) {
                    const size_t index =
                        model.ContactsSlice() + row;  // 本批次真实下标，不按姓名定位。
                    Button(2 + row, static_cast<MenuTarget>(size_t(MenuTarget::Contact0) + index),
                           contacts[index].name.c_str(), 58, 132 + row * 94, 296, 82,
                           model.Focus() == index + 2);
                    auto& slot = slots_[2 + row];
                    const bool separate = !contacts[index].device_name.empty() &&
                                          contacts[index].device_name != contacts[index].name;
                    Text(slot.label, contacts[index].name.c_str(), 16,
                         separate ? 3 : (82 - lv_font_get_line_height(primary_)) / 2, 264,
                         lv_font_get_line_height(primary_));
                    if (separate)
                        Text(slot.secondary, contacts[index].device_name.c_str(), 16, 47, 264,
                             lv_font_get_line_height(secondary_));
                }
                if (model.ContactsOffset() || model.ContactsSlice())
                    Button(4, MenuTarget::PreviousPage, "", 126, 330, 64, 48, false,
                           MATERIAL_SYMBOLS_ARROW_BACK);
                if (model.ContactsSlice() + 2 < contacts.size() || model.ContactsMore())
                    Button(5, MenuTarget::NextPage, "", 222, 330, 64, 48, false,
                           MATERIAL_SYMBOLS_ARROW_FORWARD);
            }
        } else if (page == MenuPage::Contact) {
            Header("联系人", model.Focus() == 0);
            Text(title_,
                 model.SelectedContact() < contacts.size()
                     ? contacts[model.SelectedContact()].name.c_str()
                     : "联系人",
                 72, 124, 268, 88);
            lv_label_set_long_mode(title_, LV_LABEL_LONG_WRAP);
            lv_obj_set_height(title_, 2 * lv_font_get_line_height(primary_));
            Button(1, MenuTarget::Voice, "语音通话", 86, 234, 112, 112, model.Focus() == 1,
                   MATERIAL_SYMBOLS_PHONE);
            Button(2, MenuTarget::Video, "视频通话", 214, 234, 112, 112, model.Focus() == 2,
                   MATERIAL_SYMBOLS_PHOTO_CAMERA);
        } else if (page == MenuPage::Volume || page == MenuPage::Brightness) {
            Header(page == MenuPage::Volume ? "音量" : "亮度", model.Focus() == 0);
            const std::string value =
                std::to_string(page == MenuPage::Volume ? model.Volume() : model.Brightness()) +
                "%";  // 硬件值的展示快照。
            Text(clock_, value.c_str(), 96, 140, 220, 66);
            lv_obj_set_pos(progress_, 86, 230);
            lv_obj_set_size(progress_, 240, 12);
            lv_bar_set_value(progress_,
                             page == MenuPage::Volume ? model.Volume() : model.Brightness(),
                             LV_ANIM_OFF);
            lv_obj_remove_flag(progress_, LV_OBJ_FLAG_HIDDEN);
            Button(1, MenuTarget::Decrease, "-", 100, 274, 88, 64, false);
            Button(2, MenuTarget::Increase, "+", 224, 274, 88, 64, false);
        } else {
            const char* title = "设置";  // 其余页面最多三项，当前焦点所在窗口复用相同对象。
            const char* labels[7] = {"返回表盘", "网络", "音量", "亮度", "设备信息", "电源", "皮肤"};
            switch (page) {
                case MenuPage::Skins:
                    title = "皮肤";
                    labels[0] = "返回设置";
                    for (size_t index = 0; index < kSkins.size(); ++index)
                        labels[index + 1] = kSkins[index].title;
                    break;
                case MenuPage::Network:
                    title = "网络与绑定";
                    labels[0] = "返回设置";
                    labels[1] = "更换 Wi-Fi";
                    labels[2] = "配网并绑定";
                    break;
                case MenuPage::Power:
                    title = "电源";
                    labels[0] = "返回设置";
                    labels[1] = charging ? "重启设备" : "关闭电源";
                    labels[2] = "恢复出厂";
                    break;
                case MenuPage::ConfirmPower:
                    title = charging ? "确认重启？" : "确认关机？";
                    labels[0] = "取消";
                    labels[1] = "确认";
                    break;
                case MenuPage::ConfirmReset:
                    title = "清除绑定与网络？";
                    labels[0] = "取消";
                    labels[1] = "确认恢复出厂";
                    break;
                case MenuPage::ConfirmBind:
                    title = "用手机添加设备？";
                    labels[0] = "取消";
                    labels[1] = "开始配网绑定";
                    break;
                case MenuPage::ConfirmNetwork:
                    title = "连接新的 Wi-Fi？";
                    labels[0] = "暂时不更换";
                    labels[1] = "开始 · 保留原网络";
                    break;
                case MenuPage::Info:
                    title = "设备信息";
                    labels[0] = "返回设置";
                    break;
                case MenuPage::NetworkProgress:
                    title = "网络设置";
                    labels[0] = "单击取消";
                    labels[1] = "请在手机完成操作";
                    labels[2] = "原绑定保留";
                    break;
                case MenuPage::NetworkResult:
                    title = "网络设置";
                    labels[0] = model.Count() == 1 ? "完成并返回" : "返回设置";
                    labels[1] = "返回网络设置";
                    break;
                default:
                    break;
            }
            Header(title, model.TargetAt(model.Focus()) == MenuTarget::Back);
            if (page == MenuPage::ConfirmPower || page == MenuPage::ConfirmReset ||
                page == MenuPage::ConfirmBind || page == MenuPage::ConfirmNetwork) {
                Header("确认操作");
                Text(detail_,
                     page == MenuPage::ConfirmReset ? "清除绑定和网络\n并恢复出厂设置？" : title,
                     64, 130, 284, 88);
                Button(1, MenuTarget::Cancel, "取消", 68, 230, 276, 56, model.Focus() == 0);
                Button(2, MenuTarget::Confirm, page == MenuPage::ConfirmReset ? "恢复出厂" : "确认",
                       68, 298, 276, 56, model.Focus() == 1);
            } else if (page == MenuPage::Info || page == MenuPage::NetworkProgress ||
                       page == MenuPage::NetworkResult) {
                Text(detail_, page == MenuPage::Info ? version.c_str() : network_title.c_str(), 64,
                     132, 284, 116);
                const size_t count =
                    page == MenuPage::NetworkProgress
                        ? 0
                        : model.Count();  // 活动配网由原事务输入取消，不把说明文案当作按钮。
                for (size_t index = 0; index < count; ++index)
                    Button(index + 1, model.TargetAt(index), labels[index], 68, 264 + index * 58,
                           276, 52, model.Focus() == index);
            } else {
                const int center =
                    std::max(1, static_cast<int>(model.Focus()));  // 返回独立于列表。
                for (size_t index = 1; index < model.Count(); ++index) {
                    auto* button = slots_[index + 1].button;  // 固定条目对象在移动中保持动作身份。
                    if (lv_obj_get_parent(button) != list_)
                        lv_obj_set_parent(button, list_);
                    Button(index + 1, model.TargetAt(index), labels[index], 0, index * 76, 296, 64,
                           model.Focus() == index);
                }
                lv_obj_update_layout(list_);
                if (last_page_ != page || last_focus_ != model.Focus())
                    lv_obj_scroll_to_y(list_, (center - 1) * 76,
                                       last_page_ == page ? LV_ANIM_ON : LV_ANIM_OFF);
            }
        }
        if (page != MenuPage::Home)
            lv_obj_set_style_text_color(title_, lv_color_hex(palette_->text), 0);
        // LVGL记录对象自身的脏区；避免每次选择都将整屏标脏，挤占动画刷新时间。
        lv_obj_update_layout(screen_);
        last_page_ = page;
        last_focus_ = model.Focus();
    }

private:
    // Slot 将点击上下文绑定到固定对象，复用槽位时旧按下目标不能触发新动作。
    struct Slot {
        DialView* owner = nullptr;  // 视图生命周期覆盖全部槽位。
        lv_obj_t *button = nullptr, *label = nullptr, *secondary = nullptr,
                 *icon = nullptr;  // 固定UI对象。
        MenuTarget target = MenuTarget::Back,
                   pressed_target = MenuTarget::Back;  // 当前动作与按下快照。
        uint32_t revision = 0;                         // 按下时的页面代次。
        int motion_y = 0;                              // 重启动画前的实际位置。
        bool selected = false;  // 换肤时复用选中状态，无需重建按钮。
        bool motion_visible = false;                   // 上一帧已显示的图标接续原位置。
    };
    // Text 限制文本到指定矩形，布局测量使用真实字体；标签不消费按钮的触摸。
    static void Text(lv_obj_t* object, const char* text, int x, int y, int width, int height) {
        lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(object, x, y);
        lv_obj_set_size(object, width, height);
        lv_label_set_long_mode(object, LV_LABEL_LONG_DOT);
        lv_label_set_text(object, text);
        lv_obj_set_style_text_align(object, LV_TEXT_ALIGN_CENTER, 0);
    }
    // Header 为各页保留统一标题和返回热区，返回不占用列表的一条数据。
    void Header(const char* title, bool selected = false) {
        Text(title_, title, 122, 72, 168, 50);
        Button(0, MenuTarget::Back, "", 64, 68, 48, 48, selected, MATERIAL_SYMBOLS_ARROW_BACK);
    }
    // Button 配置固定槽位，selected负责焦点颜色，glyph为可选Material图标。
    lv_obj_t* Button(size_t index, MenuTarget target, const char* text, int x, int y, int width,
                     int height, bool selected, const char* glyph = nullptr) {
        auto& slot = slots_[index];  // 调用者仅使用0..8固定槽位。
        slot.target = target;
        slot.selected = selected;
        lv_obj_remove_flag(slot.button, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(slot.button, x, y);
        lv_obj_set_size(slot.button, width, height);
        lv_obj_set_style_radius(slot.button, glyph ? LV_RADIUS_CIRCLE : 20, 0);
        lv_obj_set_style_bg_color(slot.button, lv_color_hex(selected ? palette_->accent : palette_->surface), 0);
        lv_obj_set_style_text_color(slot.button, lv_color_hex(selected ? palette_->on_accent : palette_->text), 0);
        lv_obj_add_flag(slot.secondary, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(slot.icon, LV_OBJ_FLAG_HIDDEN);
        const bool caption = glyph && text[0];  // 通话按钮图标和文字均在同一个热区内。
        Text(slot.label, text, 8,
             caption ? height - 42 : (height - lv_font_get_line_height(primary_)) / 2, width - 16,
             lv_font_get_line_height(primary_));
        if (caption)
            lv_obj_set_style_text_font(slot.label, secondary_, 0);
        else
            lv_obj_set_style_text_font(slot.label, primary_, 0);
        if (glyph) {
            Text(slot.icon, glyph, 0, caption ? 12 : (height - lv_font_get_line_height(icons_)) / 2,
                 width, lv_font_get_line_height(icons_));
        }
        return slot.button;
    }
    lv_obj_t *screen_ = nullptr, *title_ = nullptr, *detail_ = nullptr, *clock_ = nullptr,
             *progress_ = nullptr, *list_ = nullptr;  // 视图根、固定文字和可裁剪列表。
    const SkinPalette* palette_ = &kSkins[0];  // 皮肤目录进程期固定，指针不会悬空。
    bool palette_ready_ = false;               // 初次创建时需要完整设置颜色。
    Slot slots_[9]{};  // 足够表盘五个功能和分页控件，不随滚动申请新对象。
    const lv_font_t *primary_ = nullptr, *secondary_ = nullptr, *icons_ = nullptr,
                    *clock_font_ = nullptr;               // 字体借用。
    std::function<void(MenuTarget, uint32_t)> on_click_;  // 仅投递原板级输入门禁。
    std::function<void(MenuInput, uint32_t)> on_swipe_;  // 原始页面代次随手势投递有界输入队列。
    std::function<uint32_t()> pressed_revision_;          // 读取触摸按下代次。
    MenuPage last_page_ = MenuPage::Info;                 // 首次表盘显示不播放切换动画。
    size_t last_focus_ = 0;                               // 只在焦点改变时动画。
    std::string clock_text_ = "--:--";                    // 未校时不展示错误时间。
};
}  // namespace watcher
