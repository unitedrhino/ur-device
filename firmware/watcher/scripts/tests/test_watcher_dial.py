"""执行生产表盘状态机，覆盖语义输入、分页、活动对话和危险确认。"""
from pathlib import Path
import subprocess
import tempfile
import unittest

# BOARD指向当前板级实现，测试直接使用生产状态机。
BOARD = Path(__file__).resolve().parents[2] / 'main/boards/sensecap-watcher'


class WatcherDialTest(unittest.TestCase):
    """表盘输入不能串入通话或旧通讯录页面。"""

    def test_navigation_and_safety(self):
        """五个快捷功能、两条分页、返回选择及确认默认取消。"""
        source = r'''

#include "ui/menu_model.h"
#include <cassert>
using namespace watcher;
int main() {
  MenuModel model;
  auto touch = [&](MenuTarget target) {
    return model.TouchTarget(target, model.Revision(), 100);
  };
  assert(model.Focus() == 0 && model.Page() == MenuPage::Home);
  assert(model.Handle(MenuInput::Next, 1) == MenuAction::None &&
         model.Focus() == 1);
  assert(model.Volume() == 50);
  assert(model.Handle(MenuInput::Click, 2) == MenuAction::LoadContacts);
  auto revision = model.Revision();
  assert(model.ApplyContacts(revision, 0, 4, true, true));
  assert(touch(MenuTarget::Contact2) == MenuAction::None &&
         model.Page() == MenuPage::Contacts);
  touch(MenuTarget::NextPage);
  assert(model.ContactsSlice() == 2);
  revision = model.Revision();
  touch(MenuTarget::Contact3);
  assert(model.SelectedContact() == 3 && model.Page() == MenuPage::Contact);
  assert(model.TouchTarget(MenuTarget::Video, revision, 100) ==
         MenuAction::None);
  assert(touch(MenuTarget::Video) == MenuAction::DialVideo);
  touch(MenuTarget::Back);
  assert(model.Focus() == 5 && model.ContactsSlice() == 2);
  assert(touch(MenuTarget::NextPage) == MenuAction::LoadContacts);
  assert(model.ApplyContacts(model.Revision(), 4, 1, false, true));
  assert(touch(MenuTarget::PreviousPage) == MenuAction::LoadContacts);
  assert(model.ApplyContacts(model.Revision(), 0, 4, true, true) &&
         model.ContactsSlice() == 2);
  touch(MenuTarget::Back);
  assert(model.Page() == MenuPage::Home && model.Focus() == 1);
  model.SetConversationActive(true);
  assert(model.Handle(MenuInput::Next, 100) == MenuAction::Volume &&
         model.Volume() == 45);
  touch(MenuTarget::Settings);
  assert(model.Page() == MenuPage::Main);
  touch(MenuTarget::Volume);
  touch(MenuTarget::Decrease);
  assert(model.Volume() == 40);
  touch(MenuTarget::Increase);
  assert(model.Volume() == 45);
  touch(MenuTarget::Back);
  touch(MenuTarget::Power);
  touch(MenuTarget::Reset);
  assert(model.Focus() == 0 && model.Page() == MenuPage::ConfirmReset);
  assert(model.TouchTarget(MenuTarget::Confirm, model.Revision(), 100, true) ==
         MenuAction::None);
  assert(model.TouchTarget(MenuTarget::Confirm, model.Revision(), 100, false,
                           true) == MenuAction::None);
  model.Handle(MenuInput::Click, 100);
  assert(model.Page() == MenuPage::Power);
  model.RequestNetworkConfirmation(0);
  revision = model.Revision();
  assert(model.TouchTarget(MenuTarget::Confirm, revision, 30000) ==
         MenuAction::NetworkTimeout);
  model.SetConversationActive(false);
  assert(touch(MenuTarget::Directory) == MenuAction::LoadContacts);
  revision = model.Revision();
  touch(MenuTarget::Back);
  assert(!model.ApplyContacts(revision, 0, 4, false, true));
  model.Handle(MenuInput::LongPress, 1);
  assert(model.Page() == MenuPage::Main);
  // 滑动不改变活动对话的音量，也不能选中危险操作确认。
  model = MenuModel{};
  model.SetConversationActive(true);
  assert(model.Handle(MenuInput::SwipeNext,100) == MenuAction::None && model.Volume()==50);
  touch(MenuTarget::Settings);
  auto settings_revision = model.Revision();
  model.SetConversationActive(false);
  assert(model.Page()==MenuPage::Main && model.Revision()==settings_revision);
  model.Handle(MenuInput::SwipeNext,100);
  assert(model.Focus()==2 && model.Page()==MenuPage::Main);
  touch(MenuTarget::Power);
  touch(MenuTarget::Reset);
  model.Handle(MenuInput::SwipeNext,100);
  assert(model.Focus()==0 && model.Page()==MenuPage::ConfirmReset);
  model = MenuModel{};
  model.Handle(MenuInput::SwipeNext,100,true);
  assert(model.Focus()==0);
  model = MenuModel{};
  assert(touch(MenuTarget::Mirror) == MenuAction::Mirror);
  assert(touch(MenuTarget::Back) == MenuAction::Mirror);
  assert(model.Page() == MenuPage::Home);
  assert(touch(MenuTarget::Photo) == MenuAction::Photo && model.Focus() == 2);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            executable = str(Path(directory) / 'dial')
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I', str(BOARD),
                            '-x', 'c++', '-', '-o', executable], input=source, text=True, check=True)
            subprocess.run([executable], check=True)
