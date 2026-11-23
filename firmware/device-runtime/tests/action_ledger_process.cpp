// 文件说明：Linux跨进程账本回归的独立C++进程；文件标记只是软件动作副作用，不代表硬件。
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <thread>
#include "action_file_ledger.hpp"

namespace {
using namespace ur::device;
// MarkerDriver 仅在实际begin时追加合成标记，重启重投若重复执行会产生第二行。
class MarkerDriver final : public ActionDriver {
public:
    // MarkerDriver 借测试路径记录真正执行，不从请求或重放结果反推执行次数。
    explicit MarkerDriver(std::string path) : path_(std::move(path)) {}
    // begin 真正追加唯一副作用标记，返回文件写入事实，不把接纳冒充成功。
    bool begin(const ActionRequest& request) override {
        ++begins;
        std::ofstream file(path_, std::ios::app);
        file << "effect\n";
        completed_ = ActionCompletion{request.token, 200, R"({"volume":40})"};
        return file.good();
    }
    // take_completion 单次交付原动作完成事实，不在新的进程中重新生成旧结果。
    std::optional<ActionCompletion> take_completion() override {
        auto result = std::move(completed_);
        completed_.reset();
        return result;
    }
    // cancel 软件副作用已同步结束，不存在后续驱动任务。
    bool cancel(std::string_view) override { return true; }
    unsigned begins = 0;  // 真正提交到软件驱动的次数。
private:
    std::string path_;                           // 本测试私有标记文件。
    std::optional<ActionCompletion> completed_;  // 唯一实际完成事实。
};
// wait_stored 只在显式真实文件测试等待，核心状态测试使用可控时钟而不等待磁盘。
bool wait_stored(ActionFileLedger& ledger, const std::string& token) {
    for (unsigned count = 0; count < 500; ++count) {
        const auto state = ledger.persistence(token, false);
        if (state != ActionPersistence::Pending)
            return state == ActionPersistence::Stored;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}
// action_json 生成有确定标识的容量回归动作，参数变更仅用于验证冲突拒绝。
std::string action_json(unsigned index, bool changed = false) {
    return R"({"method":"action","msgToken":"capacity-)" + std::to_string(index) +
           R"(","actionID":"setVolume","params":{"volume":)" + (changed ? "50" : "40") + "}}";
}
// capacity_round 首进程执行并固化256个不同动作；重启进程反复重放首中尾及参数冲突。
// 满容量后不得为接纳新动作遗忘旧事实，也不得让重投再次触发真实软件驱动。
bool capacity_round(ActionEngine& engine, MarkerDriver& driver, bool fill) {
    std::uint64_t now = 100;
    // 只输出合成用例阶段与序号；保留真实磁盘超时，不用增加等待或重试掩盖失败。
    const auto failed = [](const char* phase, unsigned index) {
        std::fprintf(stderr, "capacity_phase=%s index=%u\n", phase, index);
        return false;
    };
    if (fill) {
        for (unsigned index = 0; index < 256; ++index) {
            if (engine.submit(action_json(index), ++now).state != ActionAdmission::Accepted)
                return failed("admission", index);
            bool completed = false;
            for (unsigned attempt = 0; attempt < 500; ++attempt) {
                engine.poll(++now);
                if (engine.take_reply()) {
                    completed = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            if (!completed)
                return failed("durableReplyDeadline", index);
        }
    }
    for (unsigned round = 0; round < 100; ++round) {
        for (unsigned index : {0U, 127U, 255U}) {
            const auto replay = engine.submit(action_json(index), ++now);
            if (replay.state != ActionAdmission::Replay || replay.reply.empty() ||
                engine.submit(action_json(index, true), ++now).state != ActionAdmission::Conflict)
                return failed("replayOrConflict", index);
            engine.poll(++now);
            if (engine.take_reply())
                return failed("extraReply", index);  // 重放不能生成新驱动完成事件。
        }
    }
    if (engine.submit(action_json(256), ++now).state != ActionAdmission::Closed)
        return failed("capacityAdmission", 256);
    engine.poll(++now);
    // 拒绝新工作后仍须可重放原事实，不能用关闭执行器抹掉安全性。
    if (engine.submit(action_json(0), ++now).state != ActionAdmission::Replay ||
        driver.begins != (fill ? 256U : 0U))
        return failed("originalReplayOrEffects", 0);
    return true;
}
}  // namespace

// main 每个模式是全新进程，只使用公开合成动作与显式测试文件路径。
int main(int argc, char** argv) {
    if (argc != 4)
        return 2;
    const std::string mode(argv[1]), file(argv[2]), marker(argv[3]);
    ActionFileLedger ledger(file, "fixture-device");
    if (!ledger.healthy())
        return 3;
    const ActionRequest action{"original-action", "setVolume", R"({"volume":40})"};
    if (mode == "crashAfterAdmission") {
        if (!ledger.record(action) || !wait_stored(ledger, action.token))
            return 4;
        std::_Exit(0);  // 模拟受理已落盘但进程未生成终态，不能由析构偷偷补终态。
    }
    MarkerDriver driver(marker);
    ActionEngine engine(driver, 5000, &ledger);
    if (mode == "fillCapacity" || mode == "replayCapacity")
        return capacity_round(engine, driver, mode == "fillCapacity") ? 0 : 9;
    const auto params = mode == "conflict" ? R"({"volume":50})" : action.params;
    const auto request =
        std::string(
            R"({"method":"action","msgToken":"original-action","actionID":"setVolume","params":)") +
        params + "}";
    const auto submitted = engine.submit(request, 100);
    if (mode == "completedReplay" || mode == "pendingReplay" || mode == "conflict") {
        const auto expected = mode == "completedReplay" ? ActionAdmission::Replay
                              : mode == "pendingReplay" ? ActionAdmission::Pending
                                                        : ActionAdmission::Conflict;
        engine.poll(101);
        return submitted.state == expected && driver.begins == 0 ? 0 : 5;
    }
    if (mode != "complete" || submitted.state != ActionAdmission::Accepted)
        return 6;
    for (unsigned count = 0; count < 500; ++count) {
        engine.poll(101 + count);
        if (engine.take_reply())
            return driver.begins == 1 ? 0 : 7;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return 8;
}
