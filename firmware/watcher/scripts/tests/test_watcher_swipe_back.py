"""执行生产菜单模型，验证左滑返回的层级、安全门禁与页面代次。"""
import subprocess
import tempfile
import unittest
from pathlib import Path

# BOARD 指向实际菜单实现；测试不维护另一份返回状态机。
BOARD = Path(__file__).resolve().parents[2] / "main/boards/sensecap-watcher"


class WatcherSwipeBackTest(unittest.TestCase):
    """返回手势不确认危险动作、不取消活动换网、不把主页左滑变成长按。"""

    def test_back_safety_and_page_revision(self):
        """连续十轮执行当前模型，覆盖父页面、迟到手势和成功页完成动作。"""
        code = r'''
#include <cassert>
#include "ui/menu_model.h"
using namespace watcher;
// Back 模拟已识别手势的快照入口；实际LVGL识别另由布局回放测试覆盖。
MenuAction Back(MenuModel& model, bool asleep = false, bool busy = false) {
    return model.TouchBack(model.Revision(), 100, asleep, busy);
}
int main() {
    for (int round = 0; round < 10; ++round) {
        MenuModel model;
        assert(Back(model) == MenuAction::None && model.Page() == MenuPage::Home);
        model.Handle(MenuInput::LongPress, 1);
        const auto old_revision = model.Revision();
        model.TouchTarget(MenuTarget::Network, old_revision, 2);
        assert(model.Page() == MenuPage::Network);
        assert(model.TouchBack(old_revision, 3) == MenuAction::None);
        assert(model.Page() == MenuPage::Network);
        Back(model, true); Back(model, false, true);
        assert(model.Page() == MenuPage::Network);
        model.TouchItem(2, model.Revision(), 4);
        assert(model.Page() == MenuPage::ConfirmBind);
        assert(Back(model) == MenuAction::None && model.Page() == MenuPage::Network);
        Back(model);
        assert(model.Page() == MenuPage::Main);
        for (size_t index : {size_t(2), size_t(3), size_t(4)}) {
            model.TouchItem(index, model.Revision(), 101);
            Back(model);
            assert(model.Page() == MenuPage::Main);
        }
        model.TouchTarget(MenuTarget::Power, model.Revision(), 102);
        for (size_t index : {size_t(1), size_t(2)}) {
            model.TouchItem(index, model.Revision(), 103);
            assert(Back(model) == MenuAction::None && model.Page() == MenuPage::Power);
            assert(model.Focus() == 0);
        }
        Back(model); Back(model);
        assert(model.Page() == MenuPage::Home);
        model.RequestNetworkConfirmation(10);
        assert(Back(model) == MenuAction::CancelNetwork && model.Page() == MenuPage::Home);
        model.RequestNetworkConfirmation(10);
        assert(model.TouchBack(model.Revision(), 30010) == MenuAction::NetworkTimeout);
        model.ShowNetworkStatus(2, false, 10);
        assert(Back(model) == MenuAction::None && model.Page() == MenuPage::NetworkProgress);
        model.ShowNetworkStatus(7, false, 10);
        assert(Back(model, false, true) == MenuAction::None);
        assert(Back(model) == MenuAction::FinishNetwork);
        assert(Back(model) == MenuAction::None);
        model.Handle(MenuInput::NetworkDone, 101);
        assert(model.Page() == MenuPage::Home);
    }
}
'''
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / "swipe-back")
            subprocess.run(["g++", "-std=c++17", "-include", "initializer_list",
                            "-Wall", "-Wextra", "-Werror", "-I", str(BOARD),
                            "-x", "c++", "-", "-o", binary], input=code,
                           text=True, check=True, capture_output=True)
            subprocess.run([binary], check=True, capture_output=True)
