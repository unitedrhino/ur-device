"""编译真实 Watcher 显示邮箱入口，验证跨任务字幕与图片所有权，不模拟像素渲染。"""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class WatcherDisplayMailboxTest(unittest.TestCase):
    """测试代码从生产板实现提取，硬件图像仅替换为析构计数边界。"""

    def test_network_page_deduplicates_status_and_keeps_new_requests(self):
        """执行真实页面更新函数；终态重复不抢焦点，退出后不弹回，新请求仍展示。"""
        source = (ROOT / "main/boards/sensecap-watcher/sensecap_watcher.cc").read_text()
        begin = source.index("        void UpdateNetworkPage(")
        end = source.index("        // DrainUi", begin)
        code = r'''
#include <cassert>
#include <cstring>
#include <functional>
#include <string>
#include "ur_network.h"
#include "ui/menu_model.h"
uint32_t lv_tick_get() { return 10; }
struct Display {
    ur_network_status_t shown_network_{};
    std::string restored_network_request_;
    watcher::MenuModel menu_model_;
    std::function<void(watcher::MenuAction, int)> menu_action_;
    int renders = 0;
    void RenderMenu() { ++renders; }
METHOD
};
int main() {
    using namespace watcher;
    Display display;
    int visibility = -1;
    display.menu_action_ = [&](MenuAction action, int value) {
        assert(action == MenuAction::Visibility); visibility = value;
    };
    ur_network_status_t status{};
    display.UpdateNetworkPage(status);
    assert(display.renders == 0);
    std::strcpy(status.request_id, "0123456789abcdef");
    status.stage = 2;
    display.UpdateNetworkPage(status);
    assert(display.menu_model_.Page() == MenuPage::NetworkProgress && visibility == 1);
    display.menu_model_.Handle(MenuInput::Tick, 999999);
    assert(display.menu_model_.Page() == MenuPage::NetworkProgress);
    status.stage = 8; status.error = 5;
    display.UpdateNetworkPage(status);
    display.menu_model_.Handle(MenuInput::Next, 11);
    int renders = display.renders;
    display.UpdateNetworkPage(status);
    assert(display.renders == renders && display.menu_model_.Focus() == 1);
    display.menu_model_.Handle(MenuInput::Click, 12);
    assert(display.menu_model_.Page() == MenuPage::Network);
    display.UpdateNetworkPage(status);
    assert(display.renders == renders && display.menu_model_.Page() == MenuPage::Network);
    std::strcpy(status.request_id, "fedcba9876543210");
    display.UpdateNetworkPage(status);
    assert(display.menu_model_.Page() == MenuPage::NetworkResult && display.renders == renders + 1);
    status.blocked = true;
    display.UpdateNetworkPage(status);
    assert(display.menu_model_.Page() == MenuPage::NetworkProgress);
    status.blocked = false; status.stage = 7; status.error = 0;
    display.UpdateNetworkPage(status);
    assert(display.menu_model_.Page() == MenuPage::NetworkResult);
    display.menu_model_.RequestNetworkConfirmation(10);
    status.stage = 9;
    display.UpdateNetworkPage(status);
    assert(display.menu_model_.Page() == MenuPage::ConfirmNetwork);
}
'''.replace("METHOD", source[begin:end])
        compiler = shutil.which("g++") or shutil.which("clang++")
        self.assertIsNotNone(compiler, "需要主机 C++ 编译器")
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / "network-page-test")
            result = subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                                     "-I" + str(ROOT / "main/unitedrhino"),
                                     "-I" + str(ROOT / "main/boards/sensecap-watcher"),
                                     "-x", "c++", "-", "-o", binary], input=code,
                                    text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            for _ in range(10):
                result = subprocess.run([binary], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_latest_snapshot_and_image_ownership(self):
        """重复快照不追加，双角色隔离，图片替换/清除恰好释放一次。"""
        source = (ROOT / "main/boards/sensecap-watcher/sensecap_watcher.cc").read_text()
        begin = source.index("        struct UiPending {")
        end = source.index("\n        };", begin) + len("\n        };")
        state = source[begin:end]
        begin = source.index("        void SetChatMessage(")
        end = source.index("        // PostInput", begin)
        methods = source[begin:end].replace(" override", "")
        # 图像不涉及编码/硬件，析构计数用来检测异步移交后的泄漏与重复释放。
        code = r'''
#include <cassert>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include "ur_network.h"
#include "display/photo_ui_state.h"
ur_network_status_t network_snapshot{};
void ur_network_status(ur_network_status_t* out) { *out = network_snapshot; }
int destroyed = 0;
struct LvglImage { ~LvglImage() { ++destroyed; } };
struct Mailbox {
STATE
std::mutex ui_mutex_;
UiPending pending_ui_;
bool network_startup_known_ = false;
PhotoUiState photo_ui_;
METHODS
};
int main() {
    Mailbox box;
    box.SetStatus("待命");
    box.SetPhotoStage(1, PhotoUiStage::Capturing);
    assert(std::strcmp(box.photo_ui_.Text(), "正在拍照…") == 0);
    assert(box.pending_ui_.status == "待命");  // 显示阶段不篡改应用音频状态。
    box.SetPhotoStage(1, PhotoUiStage::Uploading);
    box.CancelPhotoStage();
    box.SetPhotoStage(1, PhotoUiStage::Waiting);
    assert(!box.photo_ui_.Active() && box.pending_ui_.status_bar_dirty);
    box.SetPhotoStage(2, PhotoUiStage::Capturing);
    box.SetPhotoStage(1, PhotoUiStage::UploadFailed);
    assert(std::strcmp(box.photo_ui_.Text(), "正在拍照…") == 0);
    box.SetPhotoStage(2, PhotoUiStage::Waiting);
    assert(std::strcmp(box.photo_ui_.Text(), "正在识图…") == 0);
    box.CancelPhotoStage();
    for (int i=0; i<1000; ++i) {
        box.SetChatMessage("user", "问题");
        box.SetChatMessage("assistant", "完整回复");
    }
    assert(box.pending_ui_.user == "问题");
    assert(box.pending_ui_.assistant == "完整回复");
    box.SetPreviewImage(std::make_unique<LvglImage>());
    box.SetStatus("等待识图");
    box.SetChatMessage("assistant", "新的完整回复");
    assert(box.pending_ui_.preview && destroyed == 0);
    box.SetPreviewImage(std::make_unique<LvglImage>());
    assert(destroyed == 1);
    auto snapshot = std::move(box.pending_ui_);
    box.pending_ui_ = Mailbox::UiPending{};
    assert(snapshot.preview && snapshot.preview_dirty);
    assert(!box.pending_ui_.preview_dirty);
    box.SetPreviewImage(nullptr);
    assert(box.pending_ui_.preview_dirty && !box.pending_ui_.preview);
    snapshot.preview.reset();
    assert(destroyed == 2);
    box.ClearChatMessages();
    assert(box.pending_ui_.user_dirty && box.pending_ui_.assistant_dirty);
    assert(box.pending_ui_.user.empty() && box.pending_ui_.assistant.empty());
    box.SetEmotion(nullptr);
    assert(box.pending_ui_.emotion == "neutral");
    network_snapshot.stage = 2;
    std::strcpy(network_snapshot.request_id, "0123456789abcdef");
    box.UpdateStatusBar();
    assert(!box.pending_ui_.network_dirty);
    box.SetNetworkStartup(false);
    box.UpdateStatusBar();
    assert(box.pending_ui_.network_dirty && box.pending_ui_.network.stage == 2);
    network_snapshot.stage = 5;
    box.UpdateStatusBar(true);
    assert(box.pending_ui_.network.stage == 5 && box.pending_ui_.update_all);
    assert(std::strcmp(box.pending_ui_.network.request_id, "0123456789abcdef") == 0);
    network_snapshot.error = 6;
    network_snapshot.blocked = true;
    box.UpdateStatusBar();
    assert(box.pending_ui_.network.blocked && box.pending_ui_.network.error == 6);
}
'''.replace("STATE", state).replace("METHODS", methods)
        compiler = shutil.which("g++") or shutil.which("clang++")
        self.assertIsNotNone(compiler, "需要主机 C++ 编译器")
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / "mailbox-test")
            subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            "-I" + str(ROOT / "main"),
                            "-I" + str(ROOT / "main/unitedrhino"),
                            "-x", "c++", "-", "-o", binary], input=code,
                           text=True, capture_output=True, check=True)
            for _ in range(10):
                subprocess.run([binary], check=True, capture_output=True)
