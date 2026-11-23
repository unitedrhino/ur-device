// 文件说明：Linux真实私有日志、排他锁及独立进程回归；不接设备或收费模型。
#include "action_file_ledger.hpp"
#include <gtest/gtest.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <thread>
#include "environment.hpp"
extern char** environ;  // posix_spawn沿用测试环境，不输出或另存任何凭据。

namespace ur::device {
namespace {
// LedgerProbeProcess
// 用精确子PID和私有IPC保持独立账本进程，失败时只终止自身子进程。
class LedgerProbeProcess {
public:
    // 资源唯一归本实例，不可复制子PID或描述符后重复回收。
    LedgerProbeProcess() = default;
    LedgerProbeProcess(const LedgerProbeProcess&) = delete;
    LedgerProbeProcess& operator=(const LedgerProbeProcess&) = delete;
    // start
    // 只启动CMake固定测试产物，模式/路径沿当前用例传入，不使用shell或全局环境注入。
    bool start(const char* mode, std::string& file, std::string& marker) {
        int input[2]{-1, -1}, output[2]{-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, input) != 0)
            return false;
        if (::pipe2(output, O_CLOEXEC) != 0) {
            ::close(input[0]);
            ::close(input[1]);
            return false;
        }
        posix_spawn_file_actions_t actions{};
        if (::posix_spawn_file_actions_init(&actions) != 0) {
            for (const int descriptor : {input[0], input[1], output[0], output[1]})
                ::close(descriptor);
            return false;
        }
        const bool configured =
            ::posix_spawn_file_actions_adddup2(&actions, input[0], STDIN_FILENO) == 0 &&
            ::posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO) == 0;
        char* arguments[]{const_cast<char*>(UR_ACTION_LEDGER_FAULT_HELPER), const_cast<char*>(mode),
                          file.data(), marker.data(), nullptr};
        const int error = configured ? ::posix_spawn(&pid_, UR_ACTION_LEDGER_FAULT_HELPER, &actions,
                                                     nullptr, arguments, environ)
                                     : EINVAL;
        ::posix_spawn_file_actions_destroy(&actions);
        ::close(input[0]);
        ::close(output[1]);
        if (error != 0) {
            ::close(input[1]);
            ::close(output[0]);
            pid_ = -1;
            return false;
        }
        input_ = input[1];
        output_ = output[0];
        return true;
    }
    // line 默认三秒读唯一有限JSON行；容量恢复可传专用预算，不延长原动作执行截止。
    std::optional<std::string> line(std::chrono::milliseconds budget = std::chrono::seconds(3)) {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        std::string result;
        while (result.size() < 2048) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       deadline - std::chrono::steady_clock::now())
                                       .count();
            if (remaining <= 0)
                return std::nullopt;
            pollfd descriptor{output_, POLLIN, 0};
            const auto ready = ::poll(&descriptor, 1, static_cast<int>(remaining));
            if (ready < 0 && errno == EINTR)
                continue;
            if (ready <= 0)
                return std::nullopt;
            char byte = '\0';
            const auto count = ::read(output_, &byte, 1);
            if (count < 0 && errno == EINTR)
                continue;
            if (count != 1)
                return std::nullopt;
            if (byte == '\n')
                return result;
            result += byte;
        }
        return std::nullopt;
    }
    // release 只通知原持锁子进程退出；不是放弃账本或执行一个新的设备动作。
    bool release() { return ::send(input_, "x", 1, MSG_NOSIGNAL) == 1; }
    // finish 在本机单调截止内回收原子PID，超时保留所有权供析构精确终止；信号不算成功。
    int finish(std::chrono::milliseconds budget = std::chrono::seconds(3)) {
        if (pid_ <= 0)
            return -1;
        const auto deadline = std::chrono::steady_clock::now() + budget;
        int status = 0;
        while (true) {
            const auto waited = ::waitpid(pid_, &status, WNOHANG);
            if (waited == pid_) {
                pid_ = -1;
                return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            }
            if (waited < 0 && errno != EINTR) {
                // 已被回收的PID不可再发信号；其他错误保留原所有权，不能冒认退出成功。
                if (errno == ECHILD)
                    pid_ = -1;
                return -1;
            }
            if (std::chrono::steady_clock::now() >= deadline)
                return -1;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    // ~LedgerProbeProcess
    // 仅在测试提前失败时终止本对象的子进程，关闭本对象的原管道。
    ~LedgerProbeProcess() {
        if (pid_ > 0) {
            ::kill(pid_, SIGTERM);
            (void)finish(std::chrono::milliseconds(100));
            if (pid_ > 0) {
                ::kill(pid_, SIGKILL);
                (void)finish(std::chrono::seconds(1));
            }
            EXPECT_LE(pid_, 0) << "本测试子进程未在TERM/KILL截止内回收";
        }
        if (input_ >= 0)
            ::close(input_);
        if (output_ >= 0)
            ::close(output_);
    }

private:
    pid_t pid_ = -1;   // 唯一本对象启动的进程，不能指向其他任务。
    int input_ = -1;   // 原持锁进程的退出通知管道。
    int output_ = -1;  // 固定布尔/次数JSON出口，不收集任意错误正文。
};
// FileLedgerTest 只创建和清理自己mkdtemp取得的任务临时目录。
class FileLedgerTest : public testing::Test {
protected:
    // SetUp 为当前用例新建唯一目录，路径不来自环境或其他测试身份。
    void SetUp() override {
        const auto base = std::filesystem::current_path() / ".temp";
        std::filesystem::create_directories(base);
        auto pattern = (base / "action-journal-XXXXXX").string();
        const auto result = ::mkdtemp(pattern.data());
        ASSERT_NE(result, nullptr);
        directory = result;
        file = (directory / "actions.bin").string();
        marker = (directory / "effects.txt").string();
    }
    // TearDown 仅回收本用例持有的精确临时目录，不触碰设备账本或共享缓存。
    void TearDown() override {
        if (!directory.empty() && directory.filename().string().rfind("action-journal-", 0) == 0)
            std::filesystem::remove_all(directory);
    }
    // stored 等待真实IO完成，最长一秒；不以sleep时间本身当固化证据。
    bool stored(ActionFileLedger& ledger, bool terminal = false) {
        for (unsigned count = 0; count < 500; ++count) {
            const auto state = ledger.persistence("original-action", terminal);
            if (state != ActionPersistence::Pending)
                return state == ActionPersistence::Stored;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    }
    // child 启动CMake指定的唯一测试产物，不经shell拼接路径或自由命令。
    int child(const char* mode) {
        char* arguments[] = {const_cast<char*>(UR_ACTION_LEDGER_HELPER), const_cast<char*>(mode),
                             file.data(), marker.data(), nullptr};
        pid_t pid = -1;
        if (::posix_spawn(&pid, UR_ACTION_LEDGER_HELPER, nullptr, nullptr, arguments, environ) != 0)
            return -1;
        int result = 0;
        if (::waitpid(pid, &result, 0) != pid || !WIFEXITED(result))
            return -1;
        return WEXITSTATUS(result);
    }
    // read_bytes 以固定4KiB块读取到真实EOF，比较完整私有字节；不seek、裁剪或改变固化断言。
    // 缺文件仍返回empty，append显式长度保留二进制NUL，最后不足完整块仍追加真实尾部。
    std::string read_bytes(const std::string& path) {
        std::ifstream stream(path, std::ios::binary);  // 同一打开源读到EOF，不猜文件长度。
        std::array<char, 4096> block{};  // 固定有界读取块，不按文件大小加载第二份临时缓存。
        std::string result;  // 原断言仍比较整份文件，没有缩减字节范围。
        while (stream) {
            stream.read(block.data(), static_cast<std::streamsize>(block.size()));
            const auto count = stream.gcount();  // 最后一块不足4KiB仍保留真实读取长度。
            if (count > 0)
                result.append(block.data(), static_cast<std::size_t>(count));
        }
        return result;
    }
    // legacy_read_bytes 保留原逐字符istreambuf实现，仅用于同一文件的逐字回归对照。
    std::string legacy_read_bytes(const std::string& path) {
        std::ifstream stream(path, std::ios::binary);
        return {(std::istreambuf_iterator<char>(stream)), {}};
    }
    // compare_reader_bytes 写入本用例合成二进制，核两真实reader与预期逐字一致并记录本机阶段耗时。
    // 文件仍由SetUp/TearDown精确拥有；计时只描述文件读取，不代表网络延迟或生产持久化速度。
    void compare_reader_bytes(const std::string& expected) {
        std::ofstream output(file, std::ios::binary);  // 只写本用例唯一私有文件。
        ASSERT_TRUE(output.good());
        output.write(expected.data(), static_cast<std::streamsize>(expected.size()));
        output.close();
        ASSERT_FALSE(output.fail());
        const auto block_started = std::chrono::steady_clock::now();  // 新reader单阶段单调起点。
        const auto actual = read_bytes(file);  // 完整新读取结果，不输出字节正文。
        const auto block_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - block_started).count();  // 仅真实读阶段耗时。
        const auto legacy_started = std::chrono::steady_clock::now();  // 原reader独立阶段，不共享续期截止。
        const auto legacy = legacy_read_bytes(file);  // 实际执行原程序实现，不用预期替代对照。
        const auto legacy_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - legacy_started).count();  // 原逐字符reader真实耗时。
        EXPECT_EQ(actual, expected);
        EXPECT_EQ(legacy, expected);
        EXPECT_EQ(actual, legacy);
        RecordProperty("blockReaderBytes", expected.size());
        RecordProperty("blockReaderElapsedMs", block_elapsed);
        RecordProperty("legacyReaderElapsedMs", legacy_elapsed);
    }
    // fault_child 精确启动新的辅助进程并核全部固定事实，不能仅断言“不崩溃”。
    void fault_child(const char* mode, const std::string& expected) {
        LedgerProbeProcess process;
        ASSERT_TRUE(process.start(mode, file, marker));
        ASSERT_EQ(process.line(), expected);
        ASSERT_EQ(process.finish(), 0);
    }
    // worker_fault
    // 验证原worker失败、实际begin、旧事实前缀、独立重开和原日志不修剪。
    void worker_fault(const char* mode, bool admission, bool recorded, bool replay, bool corrupt) {
        const std::string actual =
            std::string(
                R"({"healthy":false,"persistenceFailed":true,"publishedReply":false,"newBegins":)") +
            (admission ? "0" : "1") + R"(,"originalBegins":1,"priorFactsRetained":true})";
        ASSERT_NO_FATAL_FAILURE(fault_child(mode, actual));
        const auto original_file = read_bytes(file);
        const auto original_effects = read_bytes(marker);
        const std::string restored =
            std::string(R"({"healthy":)") + (corrupt ? "false" : "true") + R"(,"originalReplay":)" +
            (corrupt ? "false" : "true") + R"(,"newRecorded":)" + (recorded ? "true" : "false") +
            R"(,"newReplay":)" + (replay ? "true" : "false") + R"(,"newPending":)" +
            (recorded && !replay ? "true" : "false") + R"(,"begins":0})";
        ASSERT_NO_FATAL_FAILURE(fault_child(corrupt ? "inspectCorrupt" : "inspect", restored));
        EXPECT_EQ(read_bytes(file), original_file);
        EXPECT_EQ(read_bytes(marker), original_effects);
        EXPECT_EQ(original_effects, admission ? "effect\n" : "effect\neffect\n");
    }
    std::filesystem::path directory;  // 本实例唯一拥有的目录。
    std::string file;                 // 显式0600日志路径。
    std::string marker;               // 软件驱动的可观察副作用。
    const ActionRequest request{"original-action", "setVolume", R"({"volume":40})"};
};

// 空文件与不存在文件都沿原empty合同，不把打不开文件升级为新的测试语义。
TEST_F(FileLedgerTest, BlockReaderEmptyAndMissingMatchLegacy) {
    ASSERT_FALSE(std::filesystem::exists(file));
    EXPECT_EQ(read_bytes(file), legacy_read_bytes(file));
    EXPECT_TRUE(read_bytes(file).empty());
    compare_reader_bytes("");
}
// 全部256种字节重复四遍，包括NUL；不能用C字符串结束规则截断结果。
TEST_F(FileLedgerTest, BlockReaderBinaryNulMatchesLegacy) {
    std::string expected;  // 合成1024字节，覆盖所有二进制值。
    for (unsigned index = 0; index < 1024; ++index)
        expected.push_back(static_cast<char>(index % 256));
    compare_reader_bytes(expected);
}
// 跨越两个完整块并留下17字节尾块，核边界及短读尾部不丢失。
TEST_F(FileLedgerTest, BlockReaderCrossBlockMatchesLegacy) {
    std::string expected;  // 合成两块加17字节，长度不能被块边界整除。
    for (unsigned index = 0; index < 2U * 4096U + 17U; ++index)
        expected.push_back(static_cast<char>(index % 251));
    compare_reader_bytes(expected);
}
// 4MiB加17字节全量比对，不seek/缩样本；原CTest120秒仍约束整例执行。
TEST_F(FileLedgerTest, BlockReaderLargeFileMatchesLegacy) {
    std::string expected(4U * 1024U * 1024U + 17U, '\0');  // 原容量级合成大文件，包含二进制NUL。
    for (std::size_t index = 0; index < expected.size(); ++index)
        expected[index] = static_cast<char>(index % 251);
    compare_reader_bytes(expected);
}

TEST_F(FileLedgerTest, RealFsyncRestoreAndExclusiveDeviceBinding) {
    std::string expected;
    ASSERT_EQ(encode_action_reply(
                  {request.token, request.action, 200, "success", R"({"volume":40})"}, expected),
              DecodeResult::Ok);
    {
        ActionFileLedger ledger(file, "fixture-device");
        ASSERT_TRUE(ledger.healthy());
        ActionFileLedger second(file, "fixture-device");
        EXPECT_FALSE(second.healthy());
        ASSERT_TRUE(ledger.record(request));
        ASSERT_TRUE(stored(ledger));
        EXPECT_FALSE(ledger.record(request));
        ASSERT_TRUE(ledger.record_reply(request.token, expected));
        ASSERT_TRUE(stored(ledger, true));
        EXPECT_FALSE(ledger.record_reply(request.token, expected));
    }
    {
        ActionFileLedger other(file, "different-device");
        EXPECT_FALSE(other.healthy());
    }
    ActionFileLedger restored(file, "fixture-device");
    ASSERT_TRUE(restored.healthy());
    ASSERT_TRUE(restored.find_request(request.token));
    EXPECT_EQ(restored.find_request(request.token)->params, request.params);
    EXPECT_EQ(restored.find_reply(request.token), expected);
}

TEST_F(FileLedgerTest, CompletedIndependentProcessesReplayAndRejectChangedParameters) {
    ASSERT_EQ(child("complete"), 0);
    ASSERT_EQ(child("completedReplay"), 0);
    ASSERT_EQ(child("conflict"), 0);
    std::ifstream stream(marker);
    const std::string effects((std::istreambuf_iterator<char>(stream)), {});
    EXPECT_EQ(effects, "effect\n");
}

TEST_F(FileLedgerTest, CapacityAfterCompletedWorkRetainsDeduplicationAcrossRepeatedRestarts) {
    ASSERT_EQ(child("fillCapacity"), 0);
    const auto original_size = std::filesystem::file_size(file);
    // 两次独立重启各重投首/中/尾300次，并拒绝同Token改参及第257个新动作。
    ASSERT_EQ(child("replayCapacity"), 0);
    ASSERT_EQ(child("replayCapacity"), 0);
    EXPECT_EQ(std::filesystem::file_size(file), original_size);
    std::ifstream stream(marker);
    unsigned count = 0;
    for (std::string line; std::getline(stream, line); ++count)
        EXPECT_EQ(line, "effect");
    EXPECT_EQ(count, 256U);  // 观察真实begin副作用，不从重放成功倒推动作次数。
}

TEST_F(FileLedgerTest, AbruptAdmissionOnlyExitDoesNotExecuteOnNewProcess) {
    ASSERT_EQ(child("crashAfterAdmission"), 0);
    ASSERT_EQ(child("pendingReplay"), 0);
    ASSERT_EQ(child("conflict"), 0);
    EXPECT_FALSE(std::filesystem::exists(marker));
}

TEST_F(FileLedgerTest, CorruptionAndTruncatedTailFailClosedWithoutRepairOrTruncation) {
    {
        ActionFileLedger ledger(file, "fixture-device");
        ASSERT_TRUE(ledger.healthy());
    }
    const auto original = std::filesystem::file_size(file);
    {
        std::ofstream stream(file, std::ios::app | std::ios::binary);
        stream.put('x');
    }
    {
        ActionFileLedger ledger(file, "fixture-device");
        EXPECT_FALSE(ledger.healthy());
        EXPECT_FALSE(ledger.record(request));
    }
    EXPECT_EQ(std::filesystem::file_size(file), original + 1);
    // 仅修改本测试新建文件，模拟完整帧内容损坏；长度仍不变，不能靠JSON碰巧合法继续。
    {
        std::fstream stream(file, std::ios::in | std::ios::out | std::ios::binary);
        stream.seekp(8);
        stream.put('!');
    }
    ActionFileLedger ledger(file, "fixture-device");
    EXPECT_FALSE(ledger.healthy());
    EXPECT_EQ(std::filesystem::file_size(file), original + 1);
}

TEST_F(FileLedgerTest, UnsafeFilesAndInvalidRequestsAreExplicitlyRejected) {
    {
        ActionFileLedger ledger(file, "fixture-device");
        ASSERT_TRUE(ledger.healthy());
    }
    const auto link = (directory / "link.bin").string();
    std::filesystem::create_symlink(file, link);
    ActionFileLedger symlink(link, "fixture-device");
    EXPECT_FALSE(symlink.healthy());
    ASSERT_EQ(::chmod(file.c_str(), 0644), 0);
    {
        ActionFileLedger unsafe(file, "fixture-device");
        EXPECT_FALSE(unsafe.healthy());
    }
    ASSERT_EQ(::chmod(file.c_str(), 0600), 0);
    ActionFileLedger ledger(file, "fixture-device");
    ASSERT_TRUE(ledger.healthy());
    EXPECT_FALSE(ledger.record({"bad", "setVolume", R"({"x":1,"x":2})"}));
    EXPECT_FALSE(ledger.record_reply(request.token, R"({"unexpected":true})"));
    EXPECT_EQ(ledger.persistence("missing", false), ActionPersistence::Failed);
}

TEST_F(FileLedgerTest, ExplicitConfigurationNeverSilentlyFallsBackToMemory) {
    // RestoreEnvironment只恢复本用例进入前的原值，不污染其他用例或桌面宿主。
    struct RestoreEnvironment {
        std::optional<std::string> value = environment_value("UR_DEVICE_ACTION_LEDGER_PATH");
        ~RestoreEnvironment() {
            if (value)
                ::setenv("UR_DEVICE_ACTION_LEDGER_PATH", value->c_str(), 1);
            else
                ::unsetenv("UR_DEVICE_ACTION_LEDGER_PATH");
        }
    } restore;
    std::unique_ptr<ActionFileLedger> ledger;
    ASSERT_EQ(::unsetenv("UR_DEVICE_ACTION_LEDGER_PATH"), 0);
    ASSERT_TRUE(configured_action_ledger("fixture-device", ledger));
    EXPECT_FALSE(ledger);
    ASSERT_EQ(::setenv("UR_DEVICE_ACTION_LEDGER_PATH", "", 1), 0);
    EXPECT_FALSE(configured_action_ledger("fixture-device", ledger));
    ASSERT_TRUE(ledger);
    EXPECT_FALSE(ledger->healthy());
    ASSERT_EQ(::setenv("UR_DEVICE_ACTION_LEDGER_PATH", file.c_str(), 1), 0);
    ASSERT_TRUE(configured_action_ledger("fixture-device", ledger));
    EXPECT_TRUE(ledger->healthy());
}

TEST_F(FileLedgerTest, RemovedOriginalFileLatchesFailureAndCannotReportStored) {
    ActionFileLedger ledger(file, "fixture-device");
    ASSERT_TRUE(ledger.healthy());
    ASSERT_TRUE(ledger.record(request));
    ASSERT_TRUE(stored(ledger));
    ASSERT_TRUE(std::filesystem::remove(file));  // 本测试创建的文件，模拟原inode被撤除。
    std::string reply;
    ASSERT_EQ(encode_action_reply({request.token, request.action, 200, "success", "{}"}, reply),
              DecodeResult::Ok);
    ASSERT_TRUE(ledger.record_reply(request.token, reply));
    EXPECT_FALSE(stored(ledger, true));
    EXPECT_FALSE(ledger.healthy());
    EXPECT_FALSE(ledger.find_reply(request.token));
    EXPECT_FALSE(ledger.record({"another", "setVolume", R"({"volume":40})"}));
}

TEST_F(FileLedgerTest, EntryLimitRejectsFreshWorkButRetainsOriginalReplay) {
    ActionFileLedger ledger(file, "fixture-device");
    ASSERT_TRUE(ledger.healthy());
    ASSERT_TRUE(ledger.record(request));
    ASSERT_TRUE(stored(ledger));
    for (unsigned index = 1; index < 256; ++index) {
        const auto token = "bounded-" + std::to_string(index);
        ASSERT_TRUE(ledger.record({token, request.action, request.params}));
        bool complete = false;
        for (unsigned attempt = 0; attempt < 500; ++attempt) {
            const auto state = ledger.persistence(token, false);
            if (state != ActionPersistence::Pending) {
                complete = state == ActionPersistence::Stored;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        ASSERT_TRUE(complete);
    }
    EXPECT_FALSE(ledger.record({"overflow", request.action, request.params}));
    EXPECT_TRUE(ledger.recorded(request.token));
    EXPECT_EQ(ledger.find_request(request.token)->params, request.params);
}

TEST_F(FileLedgerTest, OversizeJournalIsRejectedWithoutTruncation) {
    {
        ActionFileLedger ledger(file, "fixture-device");
        ASSERT_TRUE(ledger.healthy());
    }
    constexpr auto oversized = 4U * 1024U * 1024U + 1U;
    std::filesystem::resize_file(file, oversized);  // 仅本测试拥有的合成日志。
    ActionFileLedger ledger(file, "fixture-device");
    EXPECT_FALSE(ledger.healthy());
    EXPECT_FALSE(ledger.record(request));
    EXPECT_EQ(std::filesystem::file_size(file), oversized);
}
// 两个进程同时存活，拒绝方不得改变文件；原持有者及后继独立进程均只重放原已固化事实。
TEST_F(FileLedgerTest, ConcurrentIndependentProcessesKeepExclusiveLockAndOriginalFacts) {
    LedgerProbeProcess holder;
    ASSERT_TRUE(holder.start("hold", file, marker));
    ASSERT_EQ(holder.line(), R"({"ready":true,"originalStored":true,"originalBegins":1})");
    const auto original_file = read_bytes(file);
    ASSERT_NO_FATAL_FAILURE(fault_child("locked", R"({"healthy":false})"));
    EXPECT_EQ(read_bytes(file), original_file);
    ASSERT_TRUE(holder.release());
    ASSERT_EQ(holder.line(), R"({"healthy":true,"originalReplay":true,"originalBegins":1})");
    ASSERT_EQ(holder.finish(), 0);
    ASSERT_NO_FATAL_FAILURE(fault_child(
        "inspect",
        R"({"healthy":true,"originalReplay":true,"newRecorded":false,"newReplay":false,"newPending":false,"begins":0})"));
    EXPECT_EQ(read_bytes(file), original_file);
    EXPECT_EQ(read_bytes(marker), "effect\n");
}
// 有限write错误只作用于原受理帧；未固化不能调用驱动，重开没有新受理也不制造重执行。
TEST_F(FileLedgerTest, WorkerAdmissionWriteErrorNeverStartsDevice) {
    worker_fault("writeAdmission", true, false, false, false);
}
// 有限fsync错误在受理完整写入后发生；不能用完整内存登记或重开可读数据替代原固化屏障。
TEST_F(FileLedgerTest, WorkerAdmissionFsyncErrorNeverStartsDevice) {
    worker_fault("syncAdmission", true, true, false, false);
}
// 实际驱动已完成但终态write失败，只能未知；重开原受理不能再调用设备副作用。
TEST_F(FileLedgerTest, WorkerTerminalWriteErrorNeverPublishesSuccessOrRepeatsDevice) {
    worker_fault("writeTerminal", false, true, false, false);
}
// fsync错误未发布成功；独立进程重开完整原帧可重放真实完成，但不证明断电持久。
TEST_F(FileLedgerTest, WorkerTerminalFsyncErrorRetainsOriginalCompletionWithoutRepeatingDevice) {
    worker_fault("syncTerminal", false, true, true, false);
}
// 部分终态帧写入后失败必须拒绝恢复，不修剪旧事实或再次执行原动作。
TEST_F(FileLedgerTest, TornTerminalWriteErrorNeverRepairsOrReexecutes) {
    worker_fault("writeTerminalPartial", false, true, false, true);
}
// 辅助进程拒绝TERM时仍必须有界KILL并回收；原固化事实在强制退出后保持可重放。
TEST_F(FileLedgerTest, ProbeExitTimeoutRetainsOwnershipAndReapsOnlyItsChild) {
    {
        LedgerProbeProcess holder;
        ASSERT_TRUE(holder.start("holdIgnoreTerm", file, marker));
        ASSERT_EQ(holder.line(), R"({"ready":true,"originalStored":true,"originalBegins":1})");
        const auto started = std::chrono::steady_clock::now();
        EXPECT_EQ(holder.finish(std::chrono::milliseconds(20)), -1);
        EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(1));
    }
    ASSERT_NO_FATAL_FAILURE(fault_child(
        "inspect",
        R"({"healthy":true,"originalReplay":true,"newRecorded":false,"newReplay":false,"newPending":false,"begins":0})"));
    EXPECT_EQ(read_bytes(marker), "effect\n");
}
// 已回收进程的重复finish不能等待其他子进程或冒认第二次成功。
TEST_F(FileLedgerTest, ProbeRepeatedFinishDoesNotWaitOrReuseExitedPid) {
    LedgerProbeProcess process;
    ASSERT_TRUE(process.start("hold", file, marker));
    ASSERT_TRUE(process.line());
    ASSERT_TRUE(process.release());
    ASSERT_TRUE(process.line());
    ASSERT_EQ(process.finish(), 0);
    const auto started = std::chrono::steady_clock::now();
    EXPECT_EQ(process.finish(), -1);
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::milliseconds(100));
}
// 使用合法较大params逐条实际write/fsync达到字节预算；不是预填零字节或条数上限。
TEST_F(FileLedgerTest, RuntimeByteBudgetRefusesFreshWorkAndRetainsTerminalReplay) {
    constexpr std::size_t limit = 4U * 1024U * 1024U;
    const std::string params = R"({"padding":")" + std::string(32U * 1024U, 'a') + R"("})";
    unsigned count = 0;
    std::string original;
    {
        ActionFileLedger ledger(file, "probe-device");
        ASSERT_TRUE(ledger.healthy());
        ActionPersistence observed = ActionPersistence::Pending;  // 固定枚举区分IO失败与等待截止。
        const auto construct_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
        // 整份合法容量构造只有一次绝对验证截止；重帧/每次轮询均不续期，生产动作预算不变。
        // 每次只排一个帧，真实worker固化完才继续；不会误测八帧排队上限。
        const auto persisted = [&](const std::string& token, bool terminal) {
            while (std::chrono::steady_clock::now() < construct_deadline) {
                observed = ledger.persistence(token, terminal);
                if (observed != ActionPersistence::Pending)
                    return observed == ActionPersistence::Stored;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            return false;
        };
        for (; count < 256; ++count) {
            ASSERT_LT(std::chrono::steady_clock::now(), construct_deadline);
            const ActionRequest next{"byte-budget-" + std::to_string(count), "setVolume", params};
            const auto before = read_bytes(file);
            if (!ledger.record(next)) {
                EXPECT_FALSE(ledger.recorded(next.token));
                EXPECT_FALSE(ledger.find_request(next.token));
                EXPECT_EQ(read_bytes(file), before);
                break;
            }
            ASSERT_TRUE(persisted(next.token, false))
                << "entry=" << count << ",persistence=" << static_cast<int>(observed)
                << ",healthy=" << ledger.healthy();
            std::string reply;
            ASSERT_EQ(encode_action_reply({next.token, next.action, 200, "success", "{}"}, reply),
                      DecodeResult::Ok);
            ASSERT_TRUE(ledger.record_reply(next.token, reply));
            ASSERT_TRUE(persisted(next.token, true))
                << "entry=" << count << ",persistence=" << static_cast<int>(observed)
                << ",healthy=" << ledger.healthy();
            EXPECT_LE(std::filesystem::file_size(file), limit);
        }
        ASSERT_GT(count, 0U);
        ASSERT_LT(count, 256U);
        EXPECT_TRUE(ledger.healthy());
        original = read_bytes(file);
        EXPECT_GT(original.size(), limit - 33U * 1024U);
        EXPECT_TRUE(ledger.find_reply("byte-budget-0"));
        EXPECT_TRUE(ledger.find_reply("byte-budget-" + std::to_string(count - 1)));
    }
    // 容量恢复需校验整份4MiB日志，独立于原动作5秒截止；固定总预算不能随progress续期。
    LedgerProbeProcess restored;
    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + std::chrono::seconds(90);
    const auto remaining = [&](std::chrono::milliseconds maximum) {
        const auto value = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        return std::max(std::chrono::milliseconds::zero(), std::min(value, maximum));
    };
    ASSERT_TRUE(restored.start("byteCapacityReplay", file, marker));
    ASSERT_EQ(restored.line(remaining(std::chrono::seconds(30))), R"({"opened":true})");
    RecordProperty("capacityRestoreElapsedMs",
                   std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - started)
                       .count());
    // 每8条原终态均实际Replay后才允许一个严格递增的progress，不能据progress判整轮成功。
    for (unsigned replayed = 8; replayed <= count; replayed += 8) {
        ASSERT_EQ(restored.line(remaining(std::chrono::seconds(3))),
                  "{\"replayed\":" + std::to_string(replayed) + ",\"begins\":0}");
    }
    ASSERT_EQ(restored.line(remaining(std::chrono::seconds(3))),
              "{\"healthy\":true,\"replayed\":" + std::to_string(count) + ",\"begins\":0}");
    ASSERT_EQ(restored.finish(remaining(std::chrono::seconds(3))), 0);
    EXPECT_LT(std::chrono::steady_clock::now(), deadline);
    RecordProperty("capacityRetainedEntries", count);
    RecordProperty("capacityJournalBytes", original.size());
    EXPECT_EQ(read_bytes(file), original);
    EXPECT_FALSE(std::filesystem::exists(marker));
}
}  // namespace
}  // namespace ur::device
