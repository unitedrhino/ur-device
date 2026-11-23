"""使用生产DialView与真实LVGL验证圆屏布局、实际按钮回调及截图。"""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest
from .test_watcher_font_lifetime import method

# ROOT固定工程，构建和截图全部进入任务临时目录。
ROOT = Path(__file__).resolve().parents[2]


class ContactsLvglTest(unittest.TestCase):
    """软件输入与截图不替代现场手指触摸和听感。"""

    def test_contacts_rows_and_dial_buttons(self):
        """表盘、设置、长名和两条分页均通过真实字体渲染。"""
        display_source = (ROOT / 'main/display/lcd_display.cc').read_text()
        start = display_source.index('void LcdDisplay::SetPreviewFullscreen(bool on)')
        fullscreen = display_source[start:display_source.index('// SetRemotePreview', start)]
        board_source = (ROOT / 'main/boards/sensecap-watcher/sensecap_watcher.cc').read_text()
        skin_colors = method(board_source, 'void ApplySkinColors()')
        lvgl = ROOT / 'managed_components/lvgl__lvgl'
        fonts = ROOT / 'managed_components/78__xiaozhi-fonts'
        code = r'''

#include "cbin_host_font.h"
#include "ui/dial_view.h"
#include "ui/page_manager.h"
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>
LV_FONT_DECLARE(font_noto_sans_basic_30_4);
LV_FONT_DECLARE(font_noto_sans_basic_20_4);
LV_FONT_DECLARE(font_material_symbols_30_4);
static std::vector<uint16_t> pixels(412 * 412); // 实际flush输出的屏幕像素。
static void Capture(lv_obj_t *screen, const char *name) {
  lv_obj_update_layout(screen);
  lv_screen_load(screen);
  lv_obj_invalidate(screen);
  lv_refr_now(nullptr);
  const char *folder = std::getenv("WATCHER_UI_SNAP_DIR");
  if (!folder)
    return;
  std::ofstream file(std::string(folder) + "/" + name + ".ppm",
                     std::ios::binary);
  file << "P6\n412 412\n255\n";
  for (auto pixel : pixels) {
    const unsigned char rgb[] = {
        static_cast<unsigned char>(((pixel >> 11) & 31) * 255 / 31),
        static_cast<unsigned char>(((pixel >> 5) & 63) * 255 / 63),
        static_cast<unsigned char>((pixel & 31) * 255 / 31)};
    file.write(reinterpret_cast<const char *>(rgb), 3);
  }
}
#define CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER 1
struct DisplayLockGuard { explicit DisplayLockGuard(void*) {} };
// 硬件锁为边界替身，生产退出方法作用于真实LVGL条带缓冲。
struct LcdDisplay {
  lv_display_t* display_=nullptr;
  bool preview_fullscreen_=false;
  int width_=412,height_=412;
  lv_obj_t *preview_image_=nullptr,*status_bar_=nullptr,*top_bar_=nullptr,*content_=nullptr,*side_bar_=nullptr;
  void SetPreviewFullscreen(bool on);
};
FULLSCREEN_METHOD
// 真实板级配色函数作用于真实LVGL对象，硬件主题名称只用边界替身。
struct SkinColorsHarness {
  struct ThemeName {const char* selected="dark"; const char* name() const {return selected;}} theme;
  ThemeName* current_theme_=&theme;
  struct PageRoot {lv_obj_t* screen=nullptr;} home_page_,menu_page_;
  watcher::MenuModel& menu_model_;
  watcher::DialView& dial_view_;
  lv_obj_t *container_=nullptr,*bottom_bar_=nullptr,*chat_message_label_=nullptr,
           *user_subtitle_=nullptr,*emoji_label_=nullptr,*home_menu_=nullptr;
  SkinColorsHarness(watcher::MenuModel& model,watcher::DialView& view):menu_model_(model),dial_view_(view){}
  SKIN_COLORS_METHOD
};
int main() {
  lv_init();
  auto *display = lv_display_create(412, 412);
  static uint16_t buffer[412 * 32];
  lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
  lv_display_set_buffers(display, buffer, nullptr, sizeof(buffer),
                         LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(
      display, [](lv_display_t *d, const lv_area_t *area, uint8_t *raw) {
        auto *data = reinterpret_cast<uint16_t *>(raw);
        for (int y = area->y1; y <= area->y2; ++y)
          for (int x = area->x1; x <= area->x2; ++x)
            pixels[y * 412 + x] = *data++;
        lv_display_flush_ready(d);
      });
  // 链接真实页面管理器，三套皮肤的双屏移动过渡不露出默认白底，快速返页延后执行。
  {
    watcher::PageManager pages;
    watcher::Page home, menu;
    home.screen=lv_obj_create(nullptr);
    menu.screen=lv_obj_create(nullptr);
    home.transition=LV_SCR_LOAD_ANIM_MOVE_RIGHT;
    menu.transition=LV_SCR_LOAD_ANIM_MOVE_LEFT;
    for (auto* root : {home.screen,menu.screen}) lv_obj_remove_flag(root,LV_OBJ_FLAG_SCROLLABLE);
    for (const auto& skin : watcher::kSkins) {
      for(auto* root : {home.screen,menu.screen})
        lv_obj_set_style_bg_color(root,lv_color_hex(skin.background),0);
      assert(pages.Open(home,lv_tick_get(),0));
      for(auto* target : {&menu,&home}) {
        assert(pages.Open(*target,lv_tick_get(),150));
        assert(!pages.Open(target==&menu?home:menu,lv_tick_get(),150));
        for(int frame=0;frame<6;++frame) {
          lv_tick_inc(30);
          lv_timer_handler();
          lv_refr_now(nullptr);
          assert(pixels[206*412+206]==lv_color_to_u16(lv_color_hex(skin.background)));
        }
        assert(lv_screen_active()==target->screen);
      }
    }
  }
  LcdDisplay board;
  board.display_=display;
  board.SetPreviewFullscreen(true);
  board.SetPreviewFullscreen(false);
  assert(lv_display_get_render_mode(display)==LV_DISPLAY_RENDER_MODE_PARTIAL);
  watcher::MenuModel model;
  watcher::DialView view;
  watcher::MenuAction clicked = watcher::MenuAction::None;
  view.Create(
      [&](watcher::MenuTarget target, uint32_t revision) {
        clicked = model.TouchTarget(target, revision, 100);
      },
      [&] { return model.Revision(); },
      [&](watcher::MenuInput input, uint32_t revision) {
        assert(revision == model.Revision());
        clicked = model.Handle(input, 100);
      });
  CbinHostFont primary, secondary;
  primary.Load("PRIMARY_FONT");
  secondary.Load("SECONDARY_FONT");
  lv_font_glyph_dsc_t glyph{};
  for (uint32_t letter : {uint32_t(U'通'), uint32_t(U'讯'), uint32_t(U'录'),
                          uint32_t(U'设'), uint32_t(U'置'), uint32_t(U'皮'), uint32_t(U'肤'), uint32_t(U'暖'), uint32_t(U'海')}) {
    assert(lv_font_get_glyph_dsc(&primary.font, &glyph, letter, 0) &&
           !glyph.is_placeholder && glyph.box_h > 10);
    assert(lv_font_get_glyph_dsc(&secondary.font, &glyph, letter, 0) &&
           !glyph.is_placeholder && glyph.box_h > 10);
  }
  view.SetFonts(&primary.font, &secondary.font, &font_material_symbols_30_4,
                &lv_font_montserrat_48);
  // 同一主题重复渲染不重绑字体；真正主题替换仍强制刷新隐藏页面。
  int font_style_changes = 0;
  lv_obj_add_event_cb(view.Screen(),[](lv_event_t* event) {
    ++*static_cast<int*>(lv_event_get_user_data(event));
  },LV_EVENT_STYLE_CHANGED,&font_style_changes);
  view.SetFonts(&primary.font,&secondary.font,&font_material_symbols_30_4,&lv_font_montserrat_48);
  assert(font_style_changes==0);
  view.SetFonts(&primary.font,&secondary.font,&font_material_symbols_30_4,&lv_font_montserrat_48,true);
  assert(font_style_changes>0);
  SkinColorsHarness colors(model,view);
  colors.home_page_.screen=lv_obj_create(nullptr);
  colors.menu_page_.screen=view.Screen();
  colors.container_=lv_obj_create(colors.home_page_.screen);
  colors.bottom_bar_=lv_obj_create(colors.container_);
  colors.chat_message_label_=lv_label_create(colors.bottom_bar_);
  colors.user_subtitle_=lv_label_create(colors.bottom_bar_);
  colors.emoji_label_=lv_label_create(colors.container_);
  colors.home_menu_=lv_button_create(colors.home_page_.screen);
  std::vector<UrCallContact> contacts;
  auto render = [&](const char *name) {
    view.Render(model, contacts, true, "网络连接失败", "Watcher 2.14.582");
    Capture(view.Screen(), name);
  };
  auto press = [&](watcher::MenuTarget target) {
    auto *button = view.ButtonFor(target);
    assert(button);
    lv_obj_send_event(button, LV_EVENT_PRESSED, nullptr);
    lv_obj_send_event(button, LV_EVENT_SHORT_CLICKED, nullptr);
  };
  view.UpdateClock("20:48");
  render("home");
  model.Handle(watcher::MenuInput::Next,1);
  render("home-turning");
  lv_tick_inc(200);lv_timer_handler();
  lv_obj_update_layout(view.Screen());
  assert(lv_obj_get_y(view.ButtonFor(watcher::MenuTarget::Directory))==164);
  Capture(view.Screen(),"home-turn-complete");
  // 连续转动会替换尚未结束的动画，最终坐标仍按最新焦点布局。
  for(int step=0;step<12;++step) {
    model.Handle(watcher::MenuInput::Next,2+step);
    view.Render(model,contacts,true,"网络连接失败","Watcher");
    lv_tick_inc(20);lv_timer_handler();
  }
  model.Handle(watcher::MenuInput::Previous,20);
  view.Render(model,contacts,true,"网络连接失败","Watcher");
  lv_tick_inc(200);lv_timer_handler();lv_obj_update_layout(view.Screen());
  assert(lv_obj_get_y(view.ButtonFor(static_cast<watcher::MenuTarget>(model.Focus())))==164);
  // 连续反向旋转时保留已绘制的当前位置，不能跳回固定起点。
  const auto previous_focus = static_cast<watcher::MenuTarget>(model.Focus());
  auto* moving_icon = view.ButtonFor(previous_focus);
  const int previous_y = lv_obj_get_y(moving_icon);
  model.Handle(watcher::MenuInput::Next,25);
  view.Render(model,contacts,true,"网络连接失败","Watcher");
  lv_obj_update_layout(view.Screen());
  assert(lv_obj_get_y(moving_icon)==previous_y);
  lv_tick_inc(50);lv_timer_handler();lv_obj_update_layout(view.Screen());
  const int mid_y = lv_obj_get_y(moving_icon);
  model.Handle(watcher::MenuInput::Previous,26);
  view.Render(model,contacts,true,"网络连接失败","Watcher");
  lv_obj_update_layout(view.Screen());
  assert(lv_obj_get_y(moving_icon)==mid_y);
  lv_tick_inc(200);lv_timer_handler();
  // 回到通讯录作为后续真实点击入口。
  while(model.Focus()!=1) model.Handle(watcher::MenuInput::Previous,30);
  view.Render(model,contacts,true,"网络连接失败","Watcher");
  lv_tick_inc(200);lv_timer_handler();
  assert(view.ButtonFor(watcher::MenuTarget::Chat));
  press(watcher::MenuTarget::Directory);
  assert(clicked == watcher::MenuAction::LoadContacts);
  contacts = {{"小王", "p:one", "客厅"},
              {"同名联系人很长很长很长很长", "p:two", "办公室"},
              {"小李", "p:three", "设备三"},
              {"小张", "p:four", "设备四"}};
  model.ApplyContacts(model.Revision(), 0, 4, true, true);
  render("contacts");
  assert(!view.ButtonFor(watcher::MenuTarget::Contact2));
  press(watcher::MenuTarget::NextPage);
  render("contacts-next");
  assert(!view.ButtonFor(watcher::MenuTarget::Contact0));
  press(watcher::MenuTarget::PreviousPage);
  render("contacts");
  press(watcher::MenuTarget::Contact1);
  render("contact-actions");
  assert(lv_obj_is_visible(view.ButtonFor(watcher::MenuTarget::Back)));
  lv_area_t back;
  lv_obj_get_coords(view.ButtonFor(watcher::MenuTarget::Back), &back);
  assert(back.x1 == 64 && back.y1 == 68);
  assert(pixels[100 * 412 + 88] != pixels[0]);
  // 同一屏幕再完整刷新应保留返回热区，不能只依赖缓存帧。
  render("contact-actions-repeat");
  press(watcher::MenuTarget::Video);
  assert(clicked == watcher::MenuAction::DialVideo);
  for (auto target : {watcher::MenuTarget::Voice, watcher::MenuTarget::Video}) {
    lv_area_t area;
    lv_obj_get_coords(view.ButtonFor(target), &area);
    assert(area.x1 >= 50 && area.x2 <= 362 && area.y1 >= 220 && area.y2 <= 356);
  }
  press(watcher::MenuTarget::Back);
  render("contacts-return");
  press(watcher::MenuTarget::Back);
  render("home-directory");
  model.Handle(watcher::MenuInput::LongPress, 100);
  render("settings");
  auto* network_button = view.ButtonFor(watcher::MenuTarget::Network);
  auto* volume_button = view.ButtonFor(watcher::MenuTarget::Volume);
  assert(network_button && volume_button);
  const int volume_before = lv_obj_get_y(volume_button);
  model.Handle(watcher::MenuInput::SwipeNext,101);
  view.Render(model,contacts,true,"网络连接失败","Watcher");
  auto* viewport = lv_obj_get_parent(volume_button);
  assert(viewport != view.Screen() && lv_anim_get(viewport,nullptr));
  assert(view.ButtonFor(watcher::MenuTarget::Volume)==volume_button);
  assert(lv_obj_get_y(volume_button)==volume_before);
  lv_tick_inc(60);lv_timer_handler();lv_obj_update_layout(view.Screen());
  const int scroll_mid = lv_obj_get_scroll_y(viewport);
  assert(scroll_mid>0 && scroll_mid<76);
  model.Handle(watcher::MenuInput::SwipePrevious,102);
  view.Render(model,contacts,true,"网络连接失败","Watcher");
  assert(lv_obj_get_scroll_y(viewport)==scroll_mid);
  lv_tick_inc(200);lv_timer_handler();lv_obj_update_layout(view.Screen());
  assert(lv_obj_get_scroll_y(viewport)==0);
  // 滚动末项仍居中，移动内容限制在列表视口，不遮住标题或返回按钮。
  for(int index=0;index<4;++index) {
    model.Handle(watcher::MenuInput::Next,103+index);
    view.Render(model,contacts,true,"网络连接失败","Watcher");
    lv_tick_inc(200);lv_timer_handler();
  }
  lv_obj_update_layout(view.Screen());
  lv_area_t power_bounds, header_bounds;
  lv_obj_get_coords(view.ButtonFor(watcher::MenuTarget::Power),&power_bounds);
  lv_obj_get_coords(view.ButtonFor(watcher::MenuTarget::Back),&header_bounds);
  assert(power_bounds.y1==206 && power_bounds.y2==269);
  assert(header_bounds.y2<130);
  assert(!view.ButtonFor(watcher::MenuTarget::Network));
  Capture(view.Screen(),"settings-last-motion");
  model.TouchTarget(watcher::MenuTarget::Volume,model.Revision(),110);
  render("volume");
  view.UpdateClock("21:00");
  Capture(view.Screen(), "volume-minute-boundary");
  bool percentage_visible = false;
  for (uint32_t index = 0; index < lv_obj_get_child_count(view.Screen()); ++index) {
    auto* object = lv_obj_get_child(view.Screen(), index);
    if (lv_obj_check_type(object, &lv_label_class) && std::string(lv_label_get_text(object)) == "50%") percentage_visible = true;
  }
  assert(percentage_visible);
  press(watcher::MenuTarget::Back);
  render("settings");
  press(watcher::MenuTarget::Network);
  render("network");
  model.RequestNetworkConfirmation(100);
  render("confirm-network");
  model.ShowNetworkStatus(8, false, 100);
  render("network-error");
  // 换肤重用原对象，所有可见页面采用同一套背景，选中按钮对比色同步更新。
  for (const auto& skin : watcher::kSkins) {
    model.ForceHome();
    colors.theme.selected=skin.id;
    colors.ApplySkinColors();
    assert(lv_color_eq(lv_obj_get_style_bg_color(colors.home_page_.screen,LV_PART_MAIN),lv_color_hex(skin.background)));
    assert(lv_color_eq(lv_obj_get_style_bg_color(colors.container_,LV_PART_MAIN),lv_color_hex(skin.background)));
    assert(lv_color_eq(lv_obj_get_style_bg_color(colors.bottom_bar_,LV_PART_MAIN),lv_color_hex(skin.surface)));
    assert(lv_color_eq(lv_obj_get_style_text_color(colors.user_subtitle_,LV_PART_MAIN),lv_color_hex(skin.text)));
    render((std::string("skin-home-")+skin.id).c_str());
    assert(lv_color_eq(lv_obj_get_style_bg_color(view.Screen(),LV_PART_MAIN),lv_color_hex(skin.background)));
    auto* stable_screen=view.Screen();
    model.TouchTarget(watcher::MenuTarget::Settings,model.Revision(),100);
    model.TouchTarget(watcher::MenuTarget::Skins,model.Revision(),100);
    render((std::string("skin-selector-")+skin.id).c_str());
    assert(view.Screen()==stable_screen);
    auto* selected=view.ButtonFor(static_cast<watcher::MenuTarget>(
        size_t(watcher::MenuTarget::SkinGreen)+watcher::SkinIndex(skin.id)));
    assert(selected && lv_obj_is_visible(selected));
    assert(lv_color_eq(lv_obj_get_style_bg_color(selected,LV_PART_MAIN),lv_color_hex(skin.accent)));
    assert(lv_color_eq(lv_obj_get_style_text_color(selected,LV_PART_MAIN),lv_color_hex(skin.on_accent)));
  }
  view.SetPalette(watcher::kSkins[0]);
  // 真正的指针输入从按钮中心完成按下/抬手，验证LVGL命中与生产回调。
  model.ForceHome();
  render("home-final");
  struct Pointer {
    bool down;
    lv_point_t point;
  } pointer{};
  auto *input = lv_indev_create();
  lv_indev_set_type(input, LV_INDEV_TYPE_POINTER);
  lv_indev_set_user_data(input, &pointer);
  lv_indev_set_read_cb(input, [](lv_indev_t *indev, lv_indev_data_t *data) {
    auto *p = static_cast<Pointer *>(lv_indev_get_user_data(indev));
    data->point = p->point;
    data->state = p->down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
  });
  // 真实指针从按钮上开始滑动，抬手不能顺带进入通讯录或其他快捷功能。
  auto swipe = [&](bool up) {
    pointer.point = {300, up ? 238 : 174};
    pointer.down = true;
    lv_indev_read(input);
    for (int step = 1; step <= 6; ++step) {
      pointer.point.y += up ? -16 : 16;
      lv_tick_inc(20);
      lv_indev_read(input);
    }
    pointer.down = false;
    lv_indev_read(input);
    lv_tick_inc(200);lv_timer_handler();
  };
  model = watcher::MenuModel{};
  render("swipe-home-before");
  swipe(true);
  assert(model.Page() == watcher::MenuPage::Home && model.Focus() == 1);
  render("swipe-home-after");
  swipe(false);
  assert(model.Page() == watcher::MenuPage::Home && model.Focus() == 0);
  model.TouchTarget(watcher::MenuTarget::Settings, model.Revision(), 100);
  render("swipe-settings-before");
  swipe(true);
  assert(model.Page() == watcher::MenuPage::Main && model.Focus() == 2);
  render("swipe-settings-after");
  swipe(false);
  assert(model.Page() == watcher::MenuPage::Main && model.Focus() == 1);
  model.ForceHome();
  render("home-pointer-tap");
  auto tap = [&](watcher::MenuTarget target) {
    lv_area_t area;
    lv_obj_get_coords(view.ButtonFor(target), &area);
    pointer.point = {(area.x1 + area.x2) / 2, (area.y1 + area.y2) / 2};
    pointer.down = true;lv_indev_read(input);lv_tick_inc(60);
    pointer.down = false;lv_indev_read(input);
  };
  // 真实点击设置并模拟AI结束，等待多轮定时器后页面仍是设置，不加载旧主页。
  swipe(false);
  render("home-settings-selected");
  lv_tick_inc(200);lv_timer_handler();
  tap(watcher::MenuTarget::Settings);
  assert(model.Page()==watcher::MenuPage::Main);
  render("settings-pointer-tap");
  model.SetConversationActive(false);
  for(int step=0;step<40;++step) {lv_tick_inc(50);lv_timer_handler();}
  assert(model.Page()==watcher::MenuPage::Main && lv_screen_active()==view.Screen());
  model.ForceHome();
  while(model.Focus()!=0) model.Handle(watcher::MenuInput::Previous,100);
  render("home-directory-tap");
  lv_tick_inc(200);lv_timer_handler();
  lv_area_t button;
  lv_obj_get_coords(view.ButtonFor(watcher::MenuTarget::Directory), &button);
  pointer.point = {static_cast<int32_t>((button.x1 + button.x2) / 2),
                   static_cast<int32_t>((button.y1 + button.y2) / 2)};
  pointer.down = true;
  lv_indev_read(input);
  lv_tick_inc(60);
  pointer.down = false;
  lv_indev_read(input);
  assert(model.Page() == watcher::MenuPage::Contacts);
  lv_indev_delete(input);
  lv_display_delete(display);
  lv_deinit();
}
'''
        code = code.replace('SKIN_COLORS_METHOD',skin_colors).replace('FULLSCREEN_METHOD', fullscreen).replace('PRIMARY_FONT', str(fonts / 'cbin/font_noto_sans_common_30_4.bin')).replace('SECONDARY_FONT', str(fonts / 'cbin/font_noto_sans_common_20_4.bin'))
        temporary = ROOT.parents[1] / '.temp'
        temporary.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='dial-lvgl-', dir=temporary) as directory:
            folder = Path(directory)
            (folder / 'dial.cc').write_text(code)
            (folder / 'lv_conf.h').write_text('#define LV_COLOR_DEPTH 16\n#define LV_USE_OS 0\n#define LV_FONT_MONTSERRAT_48 1\n#define LV_FONT_FMT_TXT_LARGE 1\n')
            sources = [fonts / 'src' / name for name in ['font_noto_sans_basic_30_4.c', 'font_noto_sans_basic_20_4.c', 'font_material_symbols_30_4.c']]
            (folder / 'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.16)
project(dial_screen C CXX)
set(CMAKE_CXX_STANDARD 17)
set(LV_BUILD_CONF_PATH "{folder / 'lv_conf.h'}" CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
add_subdirectory("{lvgl}" lvgl-build)
add_executable(dial dial.cc "{ROOT / 'main/boards/sensecap-watcher/watcher_page_manager.cc'}" {' '.join('"'+str(x)+'"' for x in sources)})
target_include_directories(dial PRIVATE "{ROOT / 'main/boards/sensecap-watcher'}" "{ROOT / 'main/signaling'}" "{fonts / 'include'}" "{ROOT / 'scripts/tests/support'}")
target_link_libraries(dial PRIVATE lvgl m)
''')
            for command in [['cmake', '-S', str(folder), '-B', str(folder / 'build')],
                            ['cmake', '--build', str(folder / 'build'), '--parallel', '4']]:
                result = subprocess.run(command, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout[-3000:] + result.stderr[-4000:])
            result = subprocess.run([str(folder / 'build/dial')], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
