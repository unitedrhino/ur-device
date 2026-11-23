"""执行生产皮肤目录与菜单状态机，验证迁移、配色对比度和迟到输入保护。"""
from pathlib import Path
import subprocess
import tempfile
import unittest

# BOARD定位板级源代码，测试直接复用皮肤数据和菜单逻辑。
BOARD = Path(__file__).resolve().parents[2] / 'main/boards/sensecap-watcher'


class WatcherSkinsTest(unittest.TestCase):
    """菜单选择不绕过页面代次，旧固件默认主题只迁移一次。"""

    def test_call_hints_follow_skin(self):
        """执行真实来电标签创建和换肤函数，浅色皮肤上的接听拒接说明仍可读。"""
        from .test_watcher_font_lifetime import method
        source = (BOARD / 'watcher_call_ui.cc').read_text()
        start = source.index('lv_color_hex(0xbdf2c9)')
        start = source.rfind('\n', 0, start) + 1
        creation = source[start:source.index('    // 通话控制容器', start)]
        refresh = method(source, 'void WatcherCallUi::RefreshTheme(').replace('WatcherCallUi::', '')
        hint = method(source, 'lv_obj_t* CreateHint(')
        code = r'''
#include "ui/skin_palette.h"
#include <cassert>
using watcher::SkinFor;
struct Font {const void* font() const {return this;}};
struct lv_obj_t {uint32_t color=0;};
using lv_color_t=uint32_t;
using lv_font_t=void;
uint32_t lv_color_hex(uint32_t value){return value;}
void lv_obj_set_style_bg_color(lv_obj_t*,uint32_t,int){}
void lv_obj_set_style_text_color(lv_obj_t* obj,uint32_t value,int){obj->color=value;}
void lv_obj_set_style_text_font(lv_obj_t*,const void*,int){}
void lv_obj_update_layout(lv_obj_t*){}
void lv_label_set_text(lv_obj_t*,const char*){}
lv_obj_t* lv_label_create(lv_obj_t*){return new lv_obj_t;}
void PlaceAt(lv_obj_t*,int,int){}
constexpr int kStatusY=1,kRingTitleY=2,kRingButtonSpan=3,kRingHintY=4;
struct LvglTheme {
 const char* id="light";Font resource;
 const char* name()const{return id;}
 Font* text_font()const{static Font resource;return &resource;}
 Font* large_icon_font()const{static Font resource;return &resource;}
};
struct Display {LvglTheme theme;LvglTheme* GetTheme(){return &theme;}};
HINT
struct WatcherCallUi {
 Display display;Display* display_=&display;
 lv_obj_t screen,ring;lv_obj_t* screen_=&screen;lv_obj_t* ring_box_=&ring;
 lv_obj_t *status_label_=nullptr,*ring_title_=nullptr,*accept_hint_=nullptr,*reject_hint_=nullptr,
          *accept_icon_=nullptr,*reject_icon_=nullptr,*hangup_icon_=nullptr,*mic_icon_=nullptr,*camera_icon_=nullptr;
 static WatcherCallUi& Instance(){static WatcherCallUi ui;return ui;}
 void BuildHints(){const lv_font_t* text_font=display.theme.text_font()->font(); CREATION}
 REFRESH
};
int main(){
 auto& ui=WatcherCallUi::Instance();ui.BuildHints();
 assert(ui.accept_hint_ && ui.reject_hint_ && "ring hints missing from skin refresh");
 for(const auto& skin:watcher::kSkins){
  ui.display.theme.id=skin.id;ui.RefreshTheme();
  assert(ui.accept_hint_->color==skin.text && ui.reject_hint_->color==skin.text);
 }
 delete ui.accept_hint_;delete ui.reject_hint_;
}
'''.replace('HINT', hint).replace('CREATION', creation).replace('REFRESH', refresh)
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / 'call-hints')
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-I', str(BOARD), '-x', 'c++', '-', '-o', binary],
                           input=code, text=True, check=True)
            result = subprocess.run([binary], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_skin_selection_and_migration(self):
        """保留旧动作编号，三套皮肤可往返选择且文字达到可读对比度。"""
        code = r'''
#include "ui/skin_palette.h"
#include "ui/menu_model.h"
#include <cassert>
#include <cmath>
#include <string_view>
using namespace watcher;
// Luminance按sRGB相对亮度验证皮肤中文字和底色的静态可读性。
double Luminance(uint32_t color) {
    double result=0;
    const double weights[]={0.2126,0.7152,0.0722};
    for(int channel=0;channel<3;++channel) {
        double value=((color>>(16-channel*8))&255)/255.0;
        value=value<=0.04045?value/12.92:std::pow((value+0.055)/1.055,2.4);
        result+=value*weights[channel];
    }
    return result;
}
// Contrast比较两种颜色的亮度比，不依赖具体屏幕截图压缩。
double Contrast(uint32_t a,uint32_t b) {
    const double first=Luminance(a),second=Luminance(b);
    return (std::max(first,second)+0.05)/(std::min(first,second)+0.05);
}
int main() {
    static_assert(size_t(MenuTarget::Voice)==24 && size_t(MenuTarget::Back)==32);
    assert(std::string_view(BootSkin("light",0))=="dark");
    assert(std::string_view(BootSkin("light",1))=="light");
    assert(std::string_view(BootSkin("watcher-blue",2))=="watcher-blue");
    assert(std::string_view(BootSkin("bad-old-name",1))=="dark");
    assert(std::string_view(BootSkin("dark",1,1))=="watcher-blue");
    assert(std::string_view(BootSkin("light",1,1))=="light");
    assert(std::string_view(BootSkin("dark",1,2))=="dark");
    MenuModel model;
    auto touch=[&](MenuTarget target) {
        return model.TouchTarget(target,model.Revision(),100);
    };
    touch(MenuTarget::Settings);
    assert(model.Count()==7);
    touch(MenuTarget::Skins);
    assert(model.Page()==MenuPage::Skins && model.Count()==4 && model.Focus()==1);
    for(size_t index=0;index<kSkins.size();++index) {
        const auto revision=model.Revision();
        const auto target=static_cast<MenuTarget>(size_t(MenuTarget::SkinGreen)+index);
        assert(model.TouchTarget(target,revision,100,true)==MenuAction::None);
        assert(model.TouchTarget(target,revision,100,false,true)==MenuAction::None);
        assert(touch(target)==MenuAction::Skin && model.Skin()==index);
        assert(model.TouchTarget(MenuTarget::SkinGreen,revision,101)==MenuAction::None);
        assert(model.Skin()==index);
        const auto& skin=kSkins[index];
        assert(Contrast(skin.background,skin.text)>=4.5);
        assert(Contrast(skin.accent,skin.on_accent)>=4.5);
        assert(Contrast(skin.background,skin.muted)>=4.5);
    }
    touch(MenuTarget::Back);
    model.SyncSkin(1);
    touch(MenuTarget::Skins);
    assert(model.Focus()==2);
    model.Handle(MenuInput::SwipeNext,100);
    assert(model.Focus()==3 && model.Skin()==1);
    assert(model.Handle(MenuInput::Click,100)==MenuAction::Skin && model.Skin()==2);
    touch(MenuTarget::Back);
    assert(model.Page()==MenuPage::Main);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / 'skins')
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I', str(BOARD),
                            '-x', 'c++', '-', '-o', binary], input=code, text=True, check=True)
            subprocess.run([binary], check=True)

    def test_registered_themes_reuse_updated_resources(self):
        """真实桥接函数重用主题对象，资源重载更新蓝色皮肤并恢复统一配色。"""
        header = r'''
#pragma once
#include <map>
#include <memory>
#include <string>
#include <cstdint>
inline uint32_t lv_color_hex(uint32_t color) { return color; }
struct Font { int version=0; };
// 夹具只替换系统主题容器，生产皮肤桥接函数保持原样。
struct LvglTheme {
    std::string label;
    explicit LvglTheme(const char* name):label(name) {}
    virtual ~LvglTheme()=default;
    std::string name() const {return label;}
    virtual std::string storage_name() const {return label;}
#define COLOR(name) uint32_t name##_{}; uint32_t name() const {return name##_;} void set_##name(uint32_t v){name##_=v;}
    COLOR(background_color) COLOR(text_color) COLOR(chat_background_color)
    COLOR(user_bubble_color) COLOR(assistant_bubble_color) COLOR(system_bubble_color)
    COLOR(system_text_color) COLOR(border_color) COLOR(low_battery_color)
#undef COLOR
#define RESOURCE(name,type) std::shared_ptr<type> name##_; auto name() const {return name##_;} void set_##name(std::shared_ptr<type> value){name##_ = value;}
    RESOURCE(text_font,Font) RESOURCE(icon_font,Font) RESOURCE(large_icon_font,Font) RESOURCE(emoji_font,Font)
    RESOURCE(background_image,int) RESOURCE(emoji_collection,int)
#undef RESOURCE
};
struct LvglThemeManager {
    std::map<std::string,LvglTheme*> themes;
    static auto& GetInstance(){static LvglThemeManager value;return value;}
    auto GetTheme(const char* name){auto it=themes.find(name);return it==themes.end()?nullptr:it->second;}
    void RegisterTheme(const char* name,LvglTheme* theme){themes[name]=theme;}
};
'''
        code = r'''
#include "ui/skin_theme.h"
#include <cassert>
int main() {
    auto& manager=LvglThemeManager::GetInstance();
    LvglTheme dark("dark"),light("light");
    manager.RegisterTheme("dark",&dark);
    manager.RegisterTheme("light",&light);
    auto first=std::make_shared<Font>();
    auto collection=std::make_shared<int>(1);
    for(auto* theme : {&dark,&light}) {
        theme->set_text_font(first);
        theme->set_icon_font(first);
        theme->set_large_icon_font(first);
        theme->set_emoji_font(first);
        theme->set_emoji_collection(collection);
    }
    watcher::PrepareSkinThemes();
    auto* blue=manager.GetTheme("watcher-blue");
    assert(blue && blue->text_font()==dark.text_font());
    assert(blue->name()=="watcher-blue" && blue->storage_name()=="dark");
    assert(blue->emoji_collection()==collection);
    auto next=std::make_shared<Font>();
    dark.set_text_font(next);
    light.set_text_font(next);
    dark.set_background_color(0xffffff);
    blue->set_background_image(std::make_shared<int>(2));
    for(int round=0;round<10;++round) watcher::PrepareSkinThemes();
    assert(manager.GetTheme("watcher-blue")==blue && manager.themes.size()==3);
    assert(blue->text_font()==next && blue->background_image()==nullptr);
    assert(dark.background_color()==watcher::kSkins[0].background);
    assert(light.text_color()==watcher::kSkins[2].text);
    assert(blue->text_color()==watcher::kSkins[1].text);
    delete blue;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            (folder / 'lvgl_theme.h').write_text(header)
            binary = str(folder / 'resources')
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-I', directory, '-I', str(BOARD), '-x', 'c++', '-', '-o', binary],
                           input=code, text=True, check=True)
            subprocess.run([binary], check=True)

    def test_theme_storage_name_keeps_legacy_default(self):
        """执行真实Theme和Display保存入口，旧深浅主题名称保持不变。"""
        from .test_watcher_font_lifetime import method
        root = BOARD.parents[1]
        source = (root / 'display/display.h').read_text()
        start = source.index('class Theme {')
        definition = source[start:source.index('\nclass Display', start)]
        save_theme = method((root / 'display/display.cc').read_text(),
                            'void Display::SetTheme(').replace('Display::', '')
        code = r'''
#include <map>
#include <string>
#include <memory>
#include <cassert>
class LvglFont;
THEME
struct Settings {
    static std::map<std::string,std::string> values;
    Settings(const char*,bool) {}
    void SetString(const char* key,const std::string& value) {values[key]=value;}
};
std::map<std::string,std::string> Settings::values;
struct Display {Theme* current_theme_=nullptr; SAVE};
struct Variant : Theme {
    Variant():Theme("watcher-blue") {}
    std::string storage_name() const override {return "dark";}
};
int main() {
    Display display;
    Theme light("light"),dark("dark");
    for(auto* theme : {&light,&dark}) {
        display.SetTheme(theme);
        assert(Settings::values["theme"]==theme->name());
        assert(display.current_theme_==theme);
    }
    Variant blue;
    display.SetTheme(&blue);
    assert(display.current_theme_->name()=="watcher-blue");
    assert(Settings::values["theme"]=="dark");
}
'''.replace('THEME', definition).replace('SAVE', save_theme)
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / 'theme-storage')
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-x', 'c++', '-', '-o', binary], input=code, text=True, check=True)
            subprocess.run([binary], check=True)
