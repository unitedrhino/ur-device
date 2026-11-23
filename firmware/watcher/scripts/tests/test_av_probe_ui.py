"""回放真实RTC清屏片段，验证网络任务不等待显示锁，延迟清理不覆盖新帧或新会话。"""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class AVProbeUITest(unittest.TestCase):
    """显示锁计数只允许在调度后的UI工作中增加，网络媒体推进必须直接返回。"""

    def test_actual_stale_cleanup_defers_display_lock_and_expires(self):
        production = (ROOT / "main/boards/sensecap-watcher/ur_av_probe.cc").read_text()
        block = production.split("            auto last_display =", 1)[1].split(
            "            if (state.video_disabled.load()", 1)[0]
        source = '''#include <atomic>
#include <memory>
#include <functional>
#include <vector>
#include <cassert>
#include <cstdint>
int locks = 0;
struct LcdDisplay { int cleared = 0; bool slow = false; bool RemotePreviewSlow() const { return slow; } void SetPreviewImage(void*) { ++cleared; } };
struct DisplayLockGuard {
    explicit DisplayLockGuard(LcdDisplay*) { ++locks; }
    explicit operator bool() const { return true; }
};
struct Application {
    std::vector<std::function<void()>> jobs;
    static Application& GetInstance() { static Application app; return app; }
    void Schedule(std::function<void()> job) { jobs.push_back(job); }
};
struct RtcProbeState {
    LcdDisplay* display;
    std::atomic<bool> video_disabled{false};
    std::shared_ptr<std::atomic<uint64_t>> last_display_ms =
        std::make_shared<std::atomic<uint64_t>>(100);
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);
};
void cleanup(RtcProbeState& state, uint64_t now) {
    auto last_display =''' + block + '''
}
int main() {
    LcdDisplay display;
    RtcProbeState state{&display};
    auto& jobs = Application::GetInstance().jobs;
    cleanup(state, 599); assert(jobs.empty() && locks == 0);
    cleanup(state, 600); assert(jobs.size() == 1 && locks == 0);
    state.last_display_ms->store(601); jobs[0]();
    assert(display.cleared == 0 && locks == 0);
    cleanup(state, 1101); assert(jobs.size() == 2 && locks == 0);
    jobs[1](); assert(display.cleared == 1 && locks == 1);
    state.last_display_ms->store(1200); cleanup(state, 1700);
    state.alive->store(false); jobs[2]();
    assert(display.cleared == 1 && locks == 1);
    display.slow = true; cleanup(state, 1800);
    assert(state.video_disabled.load());
}
'''
        compiler = shutil.which("g++") or shutil.which("clang++")
        self.assertIsNotNone(compiler)
        output = ROOT.parents[1] / ".temp"
        output.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=output) as directory:
            binary = Path(directory) / "ui"
            result = subprocess.run([
                compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-O2",
                "-fsanitize=undefined", "-x", "c++", "-", "-o", str(binary),
            ], input=source, text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(binary)], check=True)
