"""编译实际应用多轮入口及协议快照；不把重复创建独立会话冒充多轮。"""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class MultiTurnTest(unittest.TestCase):
    """以实际函数验证轮次关联、隔离标志和失败清理；硬件仍需真机回归。"""

    def test_actual_serial_command_selects_fixture_and_rejects_invalid_arguments(self):
        """编译实际串口lambda，验证缺省、轮数、拍照/控制选择及非法参数拒绝。"""
        source = (ROOT / "main/boards/sensecap-watcher/sensecap_watcher.cc").read_text()
        command = source.split("const esp_console_cmd_t ur_ai_e2e_cmd = {", 1)[1]
        handler = command.split(".func = ", 1)[1].split(",\n            .argtable", 1)[0]
        code = r'''
/* 实际命令只替换应用边界；非法参数不能启动音频会话。 */
#include <cassert>
#include <cstring>
#include <string>
#include <vector>
struct Application {
    int calls=0, turns=0;
    bool photo=false, control=false;
    static Application& GetInstance() { static Application app; return app; }
    bool RunUrAiE2eTest(int count, bool take_photo, bool control_volume) {
        ++calls; turns=count; photo=take_photo; control=control_volume; return true;
    }
};
int main() {
    auto handler = HANDLER;
    auto& app=Application::GetInstance();
    for (int repeat=0; repeat<10; ++repeat) {
        for (auto args : {std::vector<std::string>{"ur_ai_e2e"},
                          {"ur_ai_e2e", "3"}, {"ur_ai_e2e", "1", "photo"},
                          {"ur_ai_e2e", "5", "photo"}, {"ur_ai_e2e", "1", "control"}}) {
            std::vector<char*> argv;
            for (auto& arg : args) argv.push_back(arg.data());
            assert(handler(argv.size(), argv.data()) == 0);
            assert(app.turns == (args.size() == 1 ? 1 : args[1][0]-'0'));
            assert(app.photo == (args.size() == 3 && args[2] == "photo"));
            assert(app.control == (args.size() == 3 && args[2] == "control"));
        }
        const int calls=app.calls;
        for (auto args : {std::vector<std::string>{"ur_ai_e2e", "0"},
                          {"ur_ai_e2e", "6"}, {"ur_ai_e2e", "12"},
                          {"ur_ai_e2e", "photo"}, {"ur_ai_e2e", "1", "unknown"},
                          {"ur_ai_e2e", "1", "photo", "extra"}, {"ur_ai_e2e", "2", "control"}}) {
            std::vector<char*> argv;
            for (auto& arg : args) argv.push_back(arg.data());
            assert(handler(argv.size(), argv.data()) != 0);
            assert(app.calls == calls);
        }
    }
}
'''.replace("HANDLER", handler)
        compiler = shutil.which("g++") or shutil.which("clang++")
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / "serial-command-test")
            subprocess.run([compiler, "-x", "c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            "-", "-o", binary], input=code, text=True, capture_output=True, check=True)
            subprocess.run([binary], capture_output=True, check=True, timeout=10)

    def test_model_error_snapshot(self):
        """编译真实结果快照：错误即使包含目标物品且阶段齐全也必须被识别。"""
        header = (ROOT / "main/protocols/ur_ai_protocol.h").read_text()
        start = header.index("struct UrAiE2eResult {")
        result_type = header[start:header.index("\n};", start) + 3]
        source = (ROOT / "main/protocols/ur_ai_protocol.cc").read_text()
        start = source.index("UrAiE2eResult UrAiProtocol::WaitForE2eResult(")
        method = source[start:source.index("\n#endif", start)]
        code = r'''
// 仅替换事件组，错误判断与结果字段均来自生产代码。
#include <cassert>
#include <mutex>
#include <string>
using TickType_t = int;
using EventBits_t = int;
constexpr int kE2eSttDoneEvent=1, kE2eTextDoneEvent=2, kE2eFirstAudioEvent=4,
              kE2eAudioDoneEvent=8, kE2eResultMask=15, pdFALSE=0, pdTRUE=1;
int xEventGroupWaitBits(int, int mask, int, int, int) { return mask; }
// RESULT_TYPE
struct UrAiProtocol {
    mutable std::mutex state_mutex_;
    int event_group_=0;
    std::string stt_text_="音量", assistant_text_;
    UrAiE2eResult WaitForE2eResult(int, const std::string&, const std::string& = "音量") const;
};
// METHOD
int main() {
    UrAiProtocol protocol;
    for (int repeat=0; repeat<10; ++repeat) {
        protocol.assistant_text_="\n\n**AI 服务本次调用失败（error, status code: 402），玩具**";
        auto failed=protocol.WaitForE2eResult(1, "玩具");
        assert(failed.service_error && failed.text_done && failed.text_nonempty);
        assert(failed.expected_text_matched && failed.first_audio && failed.audio_done);
        protocol.assistant_text_="这是一个毛绒玩具。";
        assert(!protocol.WaitForE2eResult(1, "玩具").service_error);
        protocol.assistant_text_="错误码 402 表示请求失败。";
        assert(!protocol.WaitForE2eResult(1, "").service_error);
        protocol.assistant_text_.clear();
        auto empty=protocol.WaitForE2eResult(1, "");
        assert(!empty.service_error && !empty.text_nonempty);
        protocol.stt_text_="请拍照看看";
        assert(protocol.WaitForE2eResult(1, "", "拍照").stt_matches_fixture);
        assert(!protocol.WaitForE2eResult(1, "", "音量").stt_matches_fixture);
        assert(!protocol.WaitForE2eResult(1, "", "").stt_matches_fixture);
    }
}
'''.replace("// RESULT_TYPE", result_type).replace("// METHOD", method)
        compiler = shutil.which("g++") or shutil.which("clang++")
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / "model-error-test")
            subprocess.run([compiler, "-x", "c++", "-std=c++20", "-Wall", "-Wextra", "-Werror",
                            "-", "-o", binary], input=code, text=True, capture_output=True, check=True)
            subprocess.run([binary], capture_output=True, check=True, timeout=10)

    def test_same_session_new_tokens_and_failure_cleanup(self):
        application = (ROOT / "main/application.cc").read_text()
        start = application.index("bool Application::RunUrAiE2eTest(")
        method = application[start:application.index("\n#endif", start)]
        protocol = (ROOT / "main/protocols/ur_ai_protocol.cc").read_text()
        start = protocol.index("UrAiE2eTurn UrAiProtocol::GetE2eTurn() const")
        snapshot = protocol[start:protocol.index("\nbool UrAiProtocol::WaitForAudioUploadReady", start)]
        fixture = (ROOT / "scripts/tests/cpp/ur_ai_multiturn_test.cc").read_text()
        code = fixture.replace("// PRODUCTION_MULTITURN", method).replace("// PRODUCTION_READY_SNAPSHOT", snapshot)
        compiler = shutil.which("g++") or shutil.which("clang++")
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / "multiturn-test")
            subprocess.run([compiler, "-x", "c++", "-std=c++20", "-Wall", "-Wextra", "-Werror",
                            "-", "-o", binary], input=code, text=True, capture_output=True, check=True)
            subprocess.run([binary], capture_output=True, check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
