// Watcher 菜单回归：互斥输入、默认取消、返回层级、限值、超时和唤醒吞键。
#include "ui/menu_model.h"
#include "ui/touch_gate.h"
#include <cassert>
#include <cstdio>
#include <limits>
#include <initializer_list>
#include <cstring>

// main 对真实菜单模型执行确定性事件序列，不替代 LVGL/按键驱动真机验证。
int main() {
    using namespace watcher;
    MenuModel menu;
    // 活动AI对话旋钮调节音量；唤醒和忙碌输入不得修改音量，边界限制为百分比。
    menu.SetConversationActive(true);
    menu.SyncValues(50, 50);
    assert(menu.Handle(MenuInput::Next, 0, true) == MenuAction::None);
    assert(menu.Handle(MenuInput::Next, 0, false, true) == MenuAction::None);
    assert(menu.Volume() == 50);
    assert(menu.Handle(MenuInput::Next, 0) == MenuAction::Volume && menu.Volume() == 45);
    assert(menu.Handle(MenuInput::Previous, 0) == MenuAction::Volume && menu.Volume() == 50);
    menu.SyncValues(98, 50);
    assert(menu.Handle(MenuInput::Previous, 0) == MenuAction::Volume && menu.Volume() == 100);
    menu.SyncValues(2, 50);
    assert(menu.Handle(MenuInput::Next, 0) == MenuAction::Volume && menu.Volume() == 0);
    menu.SyncValues(50, 50);
    assert(menu.Handle(MenuInput::Click, 0, true) == MenuAction::None);
    assert(menu.Handle(MenuInput::DoubleClick, 0, false, true) == MenuAction::None);
    assert(menu.Handle(MenuInput::Click, 1) == MenuAction::Chat);
    assert(menu.Handle(MenuInput::DoubleClick, 2) == MenuAction::Photo);
    assert(menu.Handle(MenuInput::LongPress, 3) == MenuAction::CloseChat);
    assert(menu.Handle(MenuInput::DoubleClick, 4) == MenuAction::None);
    assert(menu.Page() == MenuPage::Main);
    menu.TouchTarget(MenuTarget::Power, menu.Revision(), 5);
    assert(menu.Page() == MenuPage::Power);
    menu.Handle(MenuInput::Next, 7);
    menu.Handle(MenuInput::Next, 8);
    menu.Handle(MenuInput::Click, 9);
    assert(menu.Page() == MenuPage::ConfirmReset && menu.Focus() == 0);
    assert(menu.Handle(MenuInput::Click, 10) == MenuAction::None);
    assert(menu.Page() == MenuPage::Power);
    menu.Handle(MenuInput::Next, 11);
    menu.Handle(MenuInput::Click, 12);
    assert(menu.Page() == MenuPage::ConfirmPower && menu.Focus() == 0);
    menu.Handle(MenuInput::Next, 13);
    assert(menu.Handle(MenuInput::Click, 14) == MenuAction::Power);
    assert(menu.Page() == MenuPage::Home);
    // 每个事务阶段都禁止普通输入和菜单超时；结果只能回主页或进入设置，不自动重试。
    for (uint8_t stage = 1; stage <= 6; ++stage) {
        menu.ShowNetworkStatus(stage, false, 100001);
        assert(menu.Page() == MenuPage::NetworkProgress && menu.Count() == 3);
        for (auto input : {MenuInput::Tick, MenuInput::Click, MenuInput::DoubleClick,
                           MenuInput::LongPress, MenuInput::Next, MenuInput::Previous}) {
            assert(menu.Handle(input, 400001) == MenuAction::None);
            assert(menu.Page() == MenuPage::NetworkProgress);
        }
    }
    menu.ShowNetworkStatus(7, false, 400000);
    assert(menu.Count() == 1);
    assert(menu.TouchItem(0, menu.Revision(), 400001) == MenuAction::FinishNetwork);
    assert(menu.Handle(MenuInput::Click, 400002) == MenuAction::None);
    assert(menu.Handle(MenuInput::LongPress, 400003) == MenuAction::None);
    assert(menu.Handle(MenuInput::NetworkDone, 400003) == MenuAction::None);
    assert(menu.Page() == MenuPage::Home);
    menu.ShowNetworkStatus(7, false, 400004);
    assert(menu.TouchItem(0, menu.Revision() - 1, 400005) == MenuAction::None);
    assert(menu.TouchItem(0, menu.Revision(), 400006, true) == MenuAction::None);
    assert(menu.TouchItem(0, menu.Revision(), 400007, false, true) == MenuAction::None);
    assert(menu.Handle(MenuInput::DoubleClick, 400008) == MenuAction::None);
    assert(menu.Handle(MenuInput::LongPress, 400009) == MenuAction::FinishNetwork);
    for (uint8_t stage : {8}) {
        menu.ShowNetworkStatus(stage, false, 400002);
        assert(menu.Page() == MenuPage::NetworkResult && menu.Focus() == 0 && menu.Count() == 2);
        assert(menu.Handle(MenuInput::DoubleClick, 400003) == MenuAction::None);
        menu.Handle(MenuInput::Next, 400004);
        assert(menu.Handle(MenuInput::Click, 400005) == MenuAction::None);
        assert(menu.Page() == MenuPage::Network);
        menu.ShowNetworkStatus(stage, false, 400006);
        menu.Handle(MenuInput::Click, 400007);
        assert(menu.Page() == MenuPage::Home);
    }
    menu.ShowNetworkStatus(8, true, 400008);
    assert(menu.Page() == MenuPage::NetworkProgress);
    assert(std::strcmp(NetworkErrorText(1), "重启中断，保留原网络") == 0);
    assert(std::strcmp(NetworkErrorText(2), "等待超时，保留原网络") == 0);
    assert(std::strcmp(NetworkErrorText(3), "已取消，保留原网络") == 0);
    assert(std::strcmp(NetworkErrorText(4), "平台鉴权失败") == 0);
    assert(std::strcmp(NetworkErrorText(5), "网络连接失败") == 0);
    assert(std::strcmp(NetworkErrorText(6), "配置存储失败") == 0);
    assert(std::strcmp(NetworkErrorText(255), NetworkErrorText(0)) == 0);
    menu.RequestNetworkConfirmation(400009);
    menu.ShowNetworkStatus(9, false, 400010);
    assert(menu.Page() == MenuPage::ConfirmNetwork);
    menu = MenuModel{};
    menu.Handle(MenuInput::LongPress, 15);
    menu.TouchTarget(MenuTarget::Volume, menu.Revision(), 17);
    menu.SyncValues(100, 0);
    assert(menu.Handle(MenuInput::Next, 18) == MenuAction::Volume);
    assert(menu.Volume() == 95);
    menu.Handle(MenuInput::Previous, 19);
    assert(menu.Volume() == 100);
    menu.Handle(MenuInput::LongPress, 20);
    assert(menu.Page() == MenuPage::Main);
    menu.Handle(MenuInput::Tick, 120020, false, true);
    assert(menu.Page() == MenuPage::Main);
    menu.Handle(MenuInput::Tick, 120020);
    assert(menu.Page() == MenuPage::Home);
    menu.Handle(MenuInput::LongPress, std::numeric_limits<uint32_t>::max() - 100);
    menu.Handle(MenuInput::Tick, 200);
    assert(menu.Page() == MenuPage::Main);
    menu.Handle(MenuInput::Tick, 120000);
    assert(menu.Page() == MenuPage::Home);
    menu.RequestNetworkConfirmation(40000);
    assert(menu.Page() == MenuPage::ConfirmNetwork && menu.Focus() == 0);
    assert(menu.Handle(MenuInput::DoubleClick, 40001) == MenuAction::None);
    assert(menu.Handle(MenuInput::Click, 40002) == MenuAction::CancelNetwork);
    menu.RequestNetworkConfirmation(50000);
    menu.Handle(MenuInput::Next, 50001);
    assert(menu.Handle(MenuInput::Click, 50002) == MenuAction::ConfirmNetwork);
    menu.RequestNetworkConfirmation(60000);
    assert(menu.Handle(MenuInput::LongPress, 60001) == MenuAction::CancelNetwork);
    menu.RequestNetworkConfirmation(70000);
    menu.Handle(MenuInput::Next, 99999);
    // 迟到确认不能抢在超时处理之前生效，旋转不延长请求窗口。
    assert(menu.Handle(MenuInput::Click, 100000) == MenuAction::NetworkTimeout);
    assert(menu.Page() == MenuPage::Home);
    std::puts("PASS watcher menu model");
    // 触摸与滚轮共用模型；同页选择直达，危险确认、过期与迟到输入不能绕过保护。
    menu.Handle(MenuInput::LongPress, 110000);
    auto revision = menu.Revision();
    assert(menu.TouchItem(5, revision, 110001, true) == MenuAction::None);
    assert(menu.TouchItem(5, revision, 110001, false, true) == MenuAction::None);
    assert(menu.Page() == MenuPage::Main && menu.Focus() == 1);
    assert(menu.TouchItem(7, revision, 110001) == MenuAction::None);
    menu.TouchItem(5, revision, 110002);
    assert(menu.Page() == MenuPage::Power);
    menu.TouchItem(2, menu.Revision(), 110003);
    assert(menu.Page() == MenuPage::ConfirmReset && menu.Focus() == 0);
    assert(menu.TouchItem(1, revision, 110004) == MenuAction::None);
    assert(menu.TouchItem(0, menu.Revision(), 110005) == MenuAction::None);
    assert(menu.Page() == MenuPage::Power);
    menu.TouchItem(2, menu.Revision(), 110006);
    assert(menu.TouchItem(1, menu.Revision(), 110007) == MenuAction::Reset);
    menu.RequestNetworkConfirmation(120000);
    menu.TouchActivity(149999);
    assert(menu.TouchItem(1, menu.Revision(), 150000) == MenuAction::NetworkTimeout);
    menu.ShowNetworkStatus(2, false, 150001);
    assert(menu.TouchItem(2, menu.Revision(), 150002) == MenuAction::None);
    menu = MenuModel{};
    menu.Handle(MenuInput::LongPress, 160000);
    assert(menu.TouchItem(5, menu.Revision(), 280000) == MenuAction::None);
    assert(menu.Page() == MenuPage::Home);
    menu.Handle(MenuInput::LongPress, 200000);
    menu.TouchActivity(229999);
    menu.TouchItem(5, menu.Revision(), 230000);
    assert(menu.Page() == MenuPage::Power);
    std::puts("PASS watcher touch menu guards");
    // 唤醒后持续接触不能变成业务按下；通信故障必须等真实抬手才恢复。
    TouchGate touch;
    assert(!touch.Sample(true, false, false));
    assert(!touch.Sample(true, true, true));
    assert(touch.Down());
    assert(!touch.Sample(true, true, false));
    assert(!touch.Sample(true, false, false));
    assert(!touch.Down());
    assert(touch.Sample(true, true, false));
    assert(touch.Sample(true, true, false));
    assert(!touch.Sample(false, false, false));
    assert(touch.Down());
    assert(!touch.Sample(true, true, false));
    assert(!touch.Sample(true, false, false));
    assert(touch.Sample(true, true, false));
    assert(!touch.Sample(true, false, false));
    assert(!touch.Sample(false, false, false));
    assert(!touch.Sample(true, true, false));
    assert(!touch.Sample(true, false, false));
    assert(touch.Sample(true, true, false));
    std::puts("PASS watcher touch gesture gate");
    // 首页触摸只打开菜单；绑定默认取消，音量/亮度可触摸且沿用范围限制。
    menu = MenuModel{};
    assert(menu.TouchTarget(MenuTarget::Settings, menu.Revision(), 1) == MenuAction::CloseChat);
    menu.TouchTarget(MenuTarget::Network, menu.Revision(), 2);
    assert(menu.Count() == 3);
    menu.TouchItem(2, menu.Revision(), 3);
    assert(menu.Page() == MenuPage::ConfirmBind && menu.Focus() == 0);
    assert(menu.Handle(MenuInput::Click, 4) == MenuAction::None);
    assert(menu.Page() == MenuPage::Network);
    menu.TouchItem(2, menu.Revision(), 5);
    assert(menu.TouchItem(1, menu.Revision(), 6) == MenuAction::ProvisionBind);
    menu.TouchTarget(MenuTarget::Settings, menu.Revision(), 7);
    menu.TouchTarget(MenuTarget::Volume, menu.Revision(), 8);
    assert(menu.TouchItem(1, menu.Revision(), 9) == MenuAction::Volume);
    assert(menu.Volume() == 45);
    assert(menu.TouchItem(2, menu.Revision(), 10) == MenuAction::Volume);
    assert(menu.Volume() == 50);
    menu.TouchItem(0, menu.Revision(), 11);
    menu.TouchTarget(MenuTarget::Brightness, menu.Revision(), 12);
    assert(menu.TouchItem(2, menu.Revision(), 13) == MenuAction::Brightness);
    assert(menu.Brightness() == 55);
    // 亮度右滚增加、左滚减小；音量保持原方向，触屏加减文字语义不反转。
    assert(menu.Handle(MenuInput::Next, 14) == MenuAction::Brightness);
    assert(menu.Brightness() == 60);
    assert(menu.Handle(MenuInput::Previous, 15) == MenuAction::Brightness);
    assert(menu.Brightness() == 55);
    assert(menu.TouchItem(1, menu.Revision(), 16) == MenuAction::Brightness);
    assert(menu.Brightness() == 50);
    assert(menu.TouchItem(2, menu.Revision(), 17) == MenuAction::Brightness);
    assert(menu.Brightness() == 55);
    assert(menu.Handle(MenuInput::Next, 18, true) == MenuAction::None);
    assert(menu.Handle(MenuInput::Next, 19, false, true) == MenuAction::None);
    assert(menu.Brightness() == 55);
    for (int step = 0; step < 30; ++step) menu.Handle(MenuInput::Previous, 20 + step);
    assert(menu.Brightness() == 0);
    for (int step = 0; step < 30; ++step) menu.Handle(MenuInput::Next, 60 + step);
    assert(menu.Brightness() == 100);
}
