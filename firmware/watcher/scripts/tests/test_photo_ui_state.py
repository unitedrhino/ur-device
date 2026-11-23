"""执行拍照显示生产状态机，覆盖请求关联、取消、迟到和失败，不能替代显示接线实测。"""
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class PhotoUiStateTest(unittest.TestCase):
    """不替换状态机实现；仅在宿主进程提供确定性事件序列。"""

    def test_production_render_override_and_restore(self):
        """执行真实 DrainUi 状态段，覆盖空闲刷新、取消恢复和重复刷新无额外布局。"""
        source = (ROOT / 'main/boards/sensecap-watcher/sensecap_watcher.cc').read_text()
        begin = source.index('        void DrainUi() {')
        end = source.index('            if (update.notification_dirty)', begin)
        method = source[begin:end] + '\n        }\n'
        code = r'''
#include <cassert>
#include <mutex>
#include <string>
#include <utility>
#include "display/photo_ui_state.h"
std::string rendered;
int writes = 0;
void lv_label_set_text(int*, const char* text) { rendered = text; ++writes; }
struct SpiLcdDisplay {
    void SetStatus(const char* text) { lv_label_set_text(nullptr, text); }
    void UpdateStatusBar(bool) { lv_label_set_text(nullptr, "17:00"); }
};
struct Display : SpiLcdDisplay {
    struct UiPending {
        bool status_dirty = false, network_dirty = false, status_bar_dirty = false, update_all = false;
        std::string status, restored_network_request;
        int network = 0;
    };
    UiPending pending_ui_;
    std::mutex ui_mutex_;
    PhotoUiState photo_ui_;
    std::string photo_status_, normal_status_, restored_network_request_;
    int* status_label_ = nullptr;
    void UpdateNetworkPage(int) {}
METHOD
};
int main() {
    Display display;
    display.pending_ui_.status = "待命";
    display.pending_ui_.status_dirty = true;
    display.photo_ui_.Begin(1);
    display.DrainUi();
    assert(rendered == "正在拍照…");
    const auto count = writes;
    display.DrainUi();
    assert(writes == count);
    display.photo_ui_.Advance(1, PhotoUiStage::Uploading);
    display.pending_ui_.status_bar_dirty = true;
    display.DrainUi();
    assert(rendered == "正在上传…");
    display.photo_ui_.Advance(1, PhotoUiStage::Waiting);
    display.DrainUi();
    assert(rendered == "正在识图…");
    display.photo_ui_.Cancel();
    display.DrainUi();
    assert(rendered == "待命");
    display.photo_ui_.Advance(1, PhotoUiStage::Waiting);
    display.DrainUi();
    assert(rendered == "待命");
    display.photo_ui_.Begin(2);
    display.DrainUi();
    display.photo_ui_.Cancel();
    display.pending_ui_.status = "正在播报";
    display.pending_ui_.status_dirty = true;
    display.DrainUi();
    assert(rendered == "正在播报");
}
'''.replace('METHOD', method)
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / 'photo-render')
            result = subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                     '-I', str(ROOT / 'main'), '-x', 'c++', '-', '-o', binary],
                                    input=code, text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            for _ in range(10):
                result = subprocess.run([binary], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_request_lifecycle_and_late_events(self):
        """完整正向流程及失败/取消后拒绝旧代次，重复十次验证同一生产类。"""
        source = r'''
#include <cassert>
#include <cstring>
#include "display/photo_ui_state.h"
int main() {
    PhotoUiState ui;
    assert(!ui.Begin(0));
    assert(!ui.Active() && std::strcmp(ui.Text(), "") == 0);
    assert(!ui.Advance(1, PhotoUiStage::Waiting));
    assert(ui.Begin(1));
    assert(std::strcmp(ui.Text(), "正在拍照…") == 0);
    assert(ui.Advance(1, PhotoUiStage::Uploading));
    assert(std::strcmp(ui.Text(), "正在上传…") == 0);
    assert(!ui.Advance(1, PhotoUiStage::Uploading));
    assert(ui.Advance(1, PhotoUiStage::Waiting));
    assert(std::strcmp(ui.Text(), "正在识图…") == 0);
    assert(!ui.Advance(1, PhotoUiStage::Uploading));
    assert(!ui.Begin(1));
    assert(!ui.Finish(2));
    assert(ui.Finish(1));
    assert(!ui.Advance(1, PhotoUiStage::UploadFailed));
    assert(!ui.Begin(1));
    assert(std::strcmp(ui.Text(), "") == 0);
    for (unsigned id = 2; id < 12; ++id) {
        assert(ui.Begin(id));
        assert(!ui.Advance(id - 1, PhotoUiStage::Waiting));
        assert(!ui.Finish(id - 1));
        ui.Cancel();
        assert(!ui.Advance(id, PhotoUiStage::Waiting));
        assert(!ui.Begin(id));
        assert(!ui.Active() && std::strcmp(ui.Text(), "") == 0);
    }
    unsigned id = 12;
    for (auto failure : {PhotoUiStage::CaptureFailed, PhotoUiStage::UploadFailed, PhotoUiStage::Unavailable}) {
        assert(ui.Begin(id));
        assert(ui.Advance(id, failure));
        assert(!ui.Active() && std::strlen(ui.Text()) > 0);
        assert(!ui.Advance(id, PhotoUiStage::Waiting));
        assert(!ui.Advance(id, PhotoUiStage::Unavailable));
        assert(ui.Finish(id));
        assert(std::strcmp(ui.Text(), "") == 0);
        ++id;
    }
    PhotoUiState wrap;
    assert(wrap.Begin(UINT32_MAX));
    wrap.Cancel();
    assert(wrap.Begin(1));
    assert(!wrap.Begin(UINT32_MAX));
    assert(!wrap.Advance(UINT32_MAX, PhotoUiStage::Waiting));
}
'''
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / 'photo-ui')
            result = subprocess.run(['g++', '-std=c++17', '-include', 'initializer_list',
                                     '-Wall', '-Wextra', '-Werror', '-I', str(ROOT / 'main'),
                                     '-x', 'c++', '-', '-o', binary], input=source,
                                    text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            for _ in range(10):
                result = subprocess.run([binary], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
