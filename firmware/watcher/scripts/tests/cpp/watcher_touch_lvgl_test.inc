// 真实 LVGL 触点回放：硬件读数与队列为边界替身，页面、命中、回调和菜单消费来自生产代码。
namespace touch_e2e {
// TouchSample 只在主机回放提供触点，不能证明设备 I2C 或物理坐标正常。
struct TouchSample {
    bool pressed = false;
    bool valid = true;
    uint16_t x = 0, y = 0;
} sample;
using esp_lcd_touch_handle_t = void*;
// esp_lcd_touch_point_data_t 保留生产回调实际读取的坐标字段。
struct esp_lcd_touch_point_data_t { uint16_t x = 0, y = 0; };
// ESP_OK/pdTRUE 使用与设备侧一致的成功值，让失败路径不被替身反转。
constexpr int ESP_OK = 0, pdTRUE = 1;
// esp_lcd_touch_read_data 在不访问实际硬件的前提下提供通信成功或失败。
int esp_lcd_touch_read_data(void*) { return sample.valid ? ESP_OK : 1; }
// esp_lcd_touch_get_data 返回本次按下状态与坐标；释放时由真实LVGL保留上一触点。
int esp_lcd_touch_get_data(void*, esp_lcd_touch_point_data_t* point, uint8_t* count, int) {
    *count = sample.pressed ? 1 : 0;
    point->x = sample.x; point->y = sample.y;
    return ESP_OK;
}
// DisplayLockGuard 保留生产方法的锁边界；主机用例单线程，不模拟并发正确性。
struct DisplayLockGuard { explicit DisplayLockGuard(void*) {} };
// xQueueSend 复制事件到当前夹具队列，保留生产回调与显示消费之间的边界。
template<class Event> int xQueueSend(void* queue, const Event* event, int) {
    static_cast<std::deque<Event>*>(queue)->push_back(*event);
    return pdTRUE;
}
// xQueueReceive 取出一个快照；空队列返回失败而不是伪造一次点击。
template<class Event> int xQueueReceive(void* queue, Event* event, int) {
    auto* events = static_cast<std::deque<Event>*>(queue);
    if (events->empty()) return 0;
    *event = events->front(); events->pop_front(); return pdTRUE;
}
// CustomLcdDisplay 用实际页面及菜单方法构建对象，只有硬件和应用队列依赖为替身。
struct CustomLcdDisplay {
EVENT
    // events_/menu_events_ 是本夹具独占的有序队列，不模拟硬件队列拥塞或任务并发。
    std::deque<MenuEvent> events_;
    void* menu_events_ = &events_;
    // 页面及模型由真实LVGL与生产PageManager持有，测试结束按其生命周期释放。
    lv_display_t* display_ = nullptr;
    watcher::MenuModel menu_model_;
    watcher::PageManager pages_;
    watcher::Page home_page_, menu_page_;
    lv_obj_t* menu_title_ = nullptr;
    lv_obj_t* menu_list_ = nullptr;
    lv_obj_t* menu_labels_[7]{};
    lv_timer_t* menu_timer_ = nullptr;
    // 供电、睡眠和忙状态是当前场景的输入边界，不读真实GPIO或云端状态。
    bool charging_ = true, binding_flow_ = false;
    // 通话页占用屏幕期间冻结菜单渲染与输入；夹具按非通话态运行。
    bool call_ui_active_ = false;
    bool asleep_ = false, busy_ = false;
    LvglTheme theme;
    LvglTheme* current_theme_ = &theme;
    // 回调与触摸状态字段保持生产名称，便于直接编译未改写的采样及消费方法。
    std::function<void(watcher::MenuAction, int)> menu_action_;
    std::function<void(size_t, uint32_t)> touch_item_;
    std::function<bool()> touch_wake_;
    esp_lcd_touch_handle_t touch_handle_ = nullptr;
    watcher::TouchGate touch_gate_;
    uint32_t touch_revision_ = 0;
    struct { bool blocked = false; int stage = 0; uint8_t error = 0; } shown_network_;
    // 记录最终回调与业务动作，不能把这些替身状态当成设备硬件执行结果。
    size_t clicks_ = 0;
    watcher::MenuAction last_action_ = watcher::MenuAction::None;
    lv_obj_t* top_bar_ = nullptr;
    lv_obj_t* status_bar_ = nullptr;
    lv_obj_t* low_battery_popup_ = nullptr;
    lv_obj_t* network_label_ = nullptr;
    lv_obj_t* status_label_ = nullptr;
    lv_obj_t* notification_label_ = nullptr;
    lv_obj_t* mute_label_ = nullptr;
    lv_obj_t* battery_label_ = nullptr;
    // DrainUi 隔离不相关的网络/音频邮箱；触摸与菜单队列仍由真实PollMenu消费。
    void DrainUi() {}
METHODS
    // SetupOverlay 只替代基类对象创建，实际跨页层坐标与隐藏策略由生产片段执行。
    void SetupOverlay(lv_obj_t* home) {
        top_bar_ = lv_obj_create(home);
        status_bar_ = lv_obj_create(home);
        low_battery_popup_ = lv_obj_create(home);
        lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
        network_label_ = lv_label_create(top_bar_);
        mute_label_ = lv_label_create(top_bar_);
        battery_label_ = lv_label_create(top_bar_);
        status_label_ = lv_label_create(status_bar_);
        notification_label_ = lv_label_create(status_bar_);
OVERLAY
    }
    // Frame 用实际输入处理器执行按下/移动/抬手，再消费生产菜单队列。
    void Frame() {
        lv_tick_inc(20);
        lv_indev_read(lv_indev_get_next(nullptr));
        PollMenu();
        lv_obj_update_layout(lv_screen_active());
    }
    // Tap 按真实对象中心采样，不直接发送 SHORT_CLICKED 或调用 TouchItem。
    void Tap(lv_obj_t* object) {
        lv_obj_update_layout(lv_screen_active());
        lv_area_t area;
        lv_obj_get_coords(object, &area);
        assert(area.y1 >= 0 && area.y2 < 412);
        sample.x = (area.x1 + area.x2) / 2;
        sample.y = (area.y1 + area.y2) / 2;
        sample.pressed = true; Frame(); Frame();
        sample.pressed = false; Frame(); Frame();
    }
    // Swipe 通过真实LVGL指针移动识别方向，不直接调用返回或发送手势事件。
    void Swipe(bool left) {
        sample.y = 180; sample.x = left ? 280 : 130;
        sample.pressed = true; Frame();
        for (int step = 0; step < 5; ++step) {
            sample.x = left ? sample.x - 30 : sample.x + 30;
            Frame();
        }
        sample.pressed = false; Frame(); Frame();
    }
};
// RunSubtitleScroll 经生产触点读取和真实LVGL滚动验证长回复阅读；不直接设置滚动位置。
// drop_move 仅是负例的硬件采样故障，证明缺少移动时不能把点按算作滚动通过。
void RunSubtitleScroll(CustomLcdDisplay& board, bool drop_move) {
    for (int round = 0; round < 10; ++round) {
        // subtitles只隔离基类对象创建，布局、字体、视口及跟随条件均使用生产片段。
        ::SubtitleDisplay subtitles;
        subtitles.Setup();
        std::string text = "联犀：\n";
        for (int line = 0; line < 20; ++line) text += "这是完整的长回复文字。";
        subtitles.has_assistant_ = true;
        lv_label_set_text(subtitles.chat_message_label_, text.c_str());
        subtitles.RefreshSubtitleViewport(false, true);
        const int end_position = lv_obj_get_scroll_y(subtitles.bottom_bar_);
        assert(end_position > 144 && subtitles.Follow());

        // drag在字幕中央提供按下、移动及抬手，继续运行真实定时器等待惯性结束。
        auto drag = [&](bool down) {
            sample.x = 206; sample.y = down ? 170 : 260;
            sample.valid = true; sample.pressed = true; board.Frame();
            for (int step = 0; step < 6; ++step) {
                if (!drop_move) sample.y = down ? sample.y + 15 : sample.y - 15;
                board.Frame();
            }
            sample.pressed = false; board.Frame();
            for (int frame = 0; frame < 80; ++frame) {
                board.Frame();
                lv_timer_handler();
            }
            assert(board.menu_model_.Page() == watcher::MenuPage::Home);
        };
        drag(true);
        const int reading_position = lv_obj_get_scroll_y(subtitles.bottom_bar_);
        assert(reading_position < end_position);
        assert(!subtitles.Follow());
        // 读取真实跟随判断后提交增量，不能靠夹具强制false掩盖生产策略回归。
        const bool follow = subtitles.Follow();
        text += "新的增量不会打断阅读。";
        lv_label_set_text(subtitles.chat_message_label_, text.c_str());
        subtitles.RefreshSubtitleViewport(false, follow);
        assert(lv_obj_get_scroll_y(subtitles.bottom_bar_) == reading_position);
        drag(false);
        assert(lv_obj_get_scroll_y(subtitles.bottom_bar_) > reading_position);
        // 增量可能新增一行，单次相反拖动不保证正好回尾；用有界真实拖动继续查看。
        for (int attempt = 0; attempt < 12 && !subtitles.Follow(); ++attempt) drag(false);
        assert(subtitles.Follow());
        // 重新到达尾部后增量恢复自动跟随，最终文字不能留在不可见区域。
        const bool tail_follow = subtitles.Follow();
        text += "末尾回复也要完整展示。";
        lv_label_set_text(subtitles.chat_message_label_, text.c_str());
        subtitles.RefreshSubtitleViewport(false, tail_follow);
        assert(lv_obj_get_scroll_bottom(subtitles.bottom_bar_) <= 2);
        lv_obj_delete(subtitles.bottom_bar_);
    }
    std::cout << "PASS actual LVGL subtitle pointer scrolling rounds=10" << std::endl;
}
// Run 回放首页菜单、条目、设置按钮、唤醒吞键、通信故障和忙状态；危险动作不执行。
// drop_click 只用于负例：模拟回调后的事件丢失，证明本测试会拒绝“采样正常但菜单不动”。
void Run(lv_display_t* display, bool drop_click) {
    auto* previous = lv_screen_active();
    auto* home = lv_obj_create(nullptr);
    lv_screen_load(home);
    {
        CustomLcdDisplay board;
        board.display_ = display;
        board.menu_action_ = [&](watcher::MenuAction action, int) { board.last_action_ = action; };
        board.SetupMenu();
        board.SetupOverlay(home);
        board.AttachTouch(&board, [&] { return board.asleep_; }, [&](size_t index, uint32_t revision) {
            ++board.clicks_;
            if (!drop_click) board.PostTouch(index, revision, board.busy_, 25, 75, true);
        });
        auto* home_menu = lv_obj_get_child(home, 0);
        for (int round = 0; round < 10; ++round) {
            board.Tap(home_menu);
            assert(board.menu_model_.Page() == watcher::MenuPage::Main);
            assert(lv_obj_has_flag(board.status_bar_, LV_OBJ_FLAG_HIDDEN));
            board.Tap(board.menu_page_.objects[2]);
            assert(board.menu_model_.Page() == watcher::MenuPage::Network);
            board.Swipe(false);
            assert(board.menu_model_.Page() == watcher::MenuPage::Network);
            board.busy_ = true; board.Swipe(true); board.busy_ = false;
            assert(board.menu_model_.Page() == watcher::MenuPage::Network);
            board.asleep_ = true; board.Swipe(true); board.asleep_ = false;
            assert(board.menu_model_.Page() == watcher::MenuPage::Network);
            board.Swipe(true);
            assert(board.menu_model_.Page() == watcher::MenuPage::Main);
            lv_obj_scroll_to_view(board.menu_page_.objects[3], LV_ANIM_OFF);
            board.Tap(board.menu_page_.objects[3]);
            assert(board.menu_model_.Page() == watcher::MenuPage::Volume);
            board.Tap(board.menu_page_.objects[1]);
            assert(board.last_action_ == watcher::MenuAction::Volume && board.menu_model_.Volume() == 20);
            board.Tap(board.menu_page_.objects[2]);
            assert(board.menu_model_.Volume() == 25);
            board.Tap(board.menu_page_.objects[0]);
            assert(board.menu_model_.Page() == watcher::MenuPage::Main);
            lv_obj_scroll_to_view(board.menu_page_.objects[4], LV_ANIM_OFF);
            board.Tap(board.menu_page_.objects[4]);
            assert(board.menu_model_.Page() == watcher::MenuPage::Brightness);
            board.Tap(board.menu_page_.objects[1]);
            assert(board.last_action_ == watcher::MenuAction::Brightness && board.menu_model_.Brightness() == 70);
            board.Tap(board.menu_page_.objects[2]);
            assert(board.menu_model_.Brightness() == 75);
            board.Swipe(true);
            assert(board.menu_model_.Page() == watcher::MenuPage::Main);
            lv_obj_scroll_to_view(board.menu_page_.objects[6], LV_ANIM_OFF);
            board.Tap(board.menu_page_.objects[6]);
            assert(board.menu_model_.Page() == watcher::MenuPage::Power);
            board.Swipe(true);
            assert(board.menu_model_.Page() == watcher::MenuPage::Main);
            board.Swipe(true);
            assert(board.menu_model_.Page() == watcher::MenuPage::Home);
            assert(!lv_obj_has_flag(board.status_bar_, LV_OBJ_FLAG_HIDDEN));
            const size_t before = board.clicks_;
            board.asleep_ = true; board.Tap(home_menu); board.asleep_ = false;
            assert(board.clicks_ == before && board.menu_model_.Page() == watcher::MenuPage::Home);
            board.busy_ = true; board.Tap(home_menu); board.busy_ = false;
            assert(board.menu_model_.Page() == watcher::MenuPage::Home);
            // 未抬手的通信故障必须取消手势，不把恢复采样当作第二次点击。
            sample.pressed = true; sample.valid = false; board.Frame();
            sample.valid = true; board.Frame();
            sample.pressed = false; board.Frame();
            assert(board.menu_model_.Page() == watcher::MenuPage::Home);
        }
        RunSubtitleScroll(board, std::getenv("WATCHER_SUBTITLE_REPLAY_DROP_MOVE") != nullptr);
        lv_indev_delete(lv_indev_get_next(nullptr));
        lv_timer_delete(board.menu_timer_);
        lv_obj_delete(board.menu_page_.screen);
        lv_obj_delete(board.top_bar_);
        lv_obj_delete(board.status_bar_);
        lv_obj_delete(board.low_battery_popup_);
    }
    // 页面管理器先释放 group，再结束 LVGL；不能让假夹具掩盖页面生命周期错误。
    lv_screen_load(previous);
    lv_obj_delete(home);
    std::cout << "PASS actual LVGL touch routing rounds=10" << std::endl;
}
}  // namespace touch_e2e
