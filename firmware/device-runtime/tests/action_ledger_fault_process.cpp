// 文件说明：Linux测试专用独立进程，链接包装仅本进程原账本write/fsync；不改生产实现或全局环境。
// 模块：固定故障包装、合成动作驱动、持锁/重放/固化错误回归；不输出设备身份或原报文。
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include "action_file_ledger.hpp"

#include <algorithm>

// 原内核端口由链接器解析，包装只进入此测试辅助程序，不加入生产库或安装包。
extern "C" ssize_t __real_write(int descriptor, const void* bytes, size_t count);
extern "C" int __real_fsync(int descriptor);
namespace {
// Fault 标识固定故障位置；None保证普通持锁和重开路径调用原系统接口。
enum class Fault { None, WriteAdmission, SyncAdmission, WriteTerminal, SyncTerminal, TornTerminal };
std::string fault_path;                   // 固定当前测试私有日志，启动worker后不再修改。
Fault fault = Fault::None;                // 只在创建账本/线程前设置，不接受自由syscall或FD。
thread_local unsigned frame_kind = 0;     // 当前worker写入帧：1受理，2终态。
thread_local bool target_frame = false;   // 仅固定的新测试动作帧受故障影响。
thread_local bool torn_terminal = false;  // 部分终态写成功后，后续同FD写必须失败。
// owned_fd
// 核对当前进程真实FD的完整绝对路径，不能误伤stdout、标记文件或其他账本。
bool owned_fd(int descriptor) {
    if (fault_path.empty() || fault_path.front() != '/')
        return false;
    std::array<char, 64> link{};
    std::array<char, 4097> actual{};
    const int size = std::snprintf(link.data(), link.size(), "/proc/self/fd/%d", descriptor);
    if (size <= 0 || static_cast<std::size_t>(size) >= link.size())
        return false;
    const auto length = ::readlink(link.data(), actual.data(), actual.size() - 1);
    if (length < 0)
        return false;
    actual[static_cast<std::size_t>(length)] = '\0';
    return fault_path == actual.data();
}
// configure_fault 固定模式和路径，必须先于ActionFileLedger创建及worker启动。
void configure_fault(const std::string& path, const std::string& mode) {
    fault_path = path;
    if (mode == "writeAdmission")
        fault = Fault::WriteAdmission;
    else if (mode == "syncAdmission")
        fault = Fault::SyncAdmission;
    else if (mode == "writeTerminal")
        fault = Fault::WriteTerminal;
    else if (mode == "syncTerminal")
        fault = Fault::SyncTerminal;
    else if (mode == "writeTerminalPartial")
        fault = Fault::TornTerminal;
}
// read_bytes
// 只读原测试日志，检查旧固化前缀不丢；不得打印正文或据此授予执行许可。
std::string read_bytes(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    return {(std::istreambuf_iterator<char>(stream)), {}};
}
}  // namespace
// __wrap_write
// 仅在原worker固定新动作帧返回有限ENOSPC；其他调用仍是原write，保留sanitizer检查。
extern "C" ssize_t __wrap_write(int descriptor, const void* bytes, size_t count) {
    if (fault != Fault::None && owned_fd(descriptor)) {
        if (torn_terminal) {
            errno = ENOSPC;
            return -1;
        }
        if (count >= 5) {
            frame_kind = static_cast<const unsigned char*>(bytes)[4];
            const auto* first = static_cast<const char*>(bytes);
            constexpr char token[] = "probe-new-action";
            target_frame = std::search(first, first + count, token, token + sizeof(token) - 1) !=
                           first + count;
            if (target_frame && fault == Fault::TornTerminal && frame_kind == 2) {
                torn_terminal = true;
                return __real_write(descriptor, bytes, count / 2);
            }
            if (target_frame && ((fault == Fault::WriteAdmission && frame_kind == 1) ||
                                 (fault == Fault::WriteTerminal && frame_kind == 2))) {
                errno = ENOSPC;
                return -1;
            }
        }
    }
    return __real_write(descriptor, bytes, count);
}
// __wrap_fsync
// 精确失败同线程刚写的新动作受理/终态；有限EIO不是断电或物理磁盘满证明。
extern "C" int __wrap_fsync(int descriptor) {
    if (target_frame && owned_fd(descriptor) &&
        ((fault == Fault::SyncAdmission && frame_kind == 1) ||
         (fault == Fault::SyncTerminal && frame_kind == 2))) {
        errno = EIO;
        return -1;
    }
    return __real_fsync(descriptor);
}

using namespace ur::device;
namespace {
// MarkerDriver
// 只在真实begin发生时写测试副作用标记；与账本固化完成是两个独立事实。
class MarkerDriver final : public ActionDriver {
public:
    explicit MarkerDriver(std::string path) : path_(std::move(path)) {}
    bool begin(const ActionRequest& request) override {
        ++begins;
        std::ofstream marker(path_, std::ios::app);
        marker << "effect\n";
        completion_ = ActionCompletion{request.token, 200, R"({"volume":40})"};
        return marker.good();
    }
    std::optional<ActionCompletion> take_completion() override {
        auto result = std::move(completion_);
        completion_.reset();
        return result;
    }
    bool cancel(std::string_view) override { return true; }
    unsigned begins = 0;

private:
    std::string path_;
    std::optional<ActionCompletion> completion_;
};
// wire 使用固定公开合成动作，不能接受环境传来的身份、参数或自由正文。
std::string wire(bool original) {
    return std::string(R"({"method":"action","msgToken":")") +
           (original ? "probe-original-action" : "probe-new-action") +
           R"(","actionID":"setVolume","params":{"volume":40}})";
}
// complete
// 等待实际原worker终态固化，等待有界且不会改变原ActionEngine五秒总截止。
bool complete(ActionEngine& engine, bool original, std::uint64_t& clock) {
    if (engine.submit(wire(original), ++clock).state != ActionAdmission::Accepted)
        return false;
    for (unsigned index = 0; index < 500; ++index) {
        engine.poll(++clock);
        if (engine.take_reply())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}
// inspect
// 重开原日志后只查询旧事实；没有原受理时不提交新动作，不制造“重执行”证据。
int inspect(const std::string& path, const std::string& marker, bool corrupt) {
    ActionFileLedger ledger(path, "probe-device");
    MarkerDriver driver(marker);
    ActionEngine engine(driver, 5000, &ledger);
    const auto original = engine.submit(wire(true), 1);
    const bool recorded = ledger.recorded("probe-new-action");
    auto next = ActionAdmission::Conflict;
    if (recorded)
        next = engine.submit(wire(false), 2).state;
    engine.poll(3);
    std::cout << "{\"healthy\":" << ledger.healthy()
              << ",\"originalReplay\":" << (original.state == ActionAdmission::Replay)
              << ",\"newRecorded\":" << recorded
              << ",\"newReplay\":" << (next == ActionAdmission::Replay)
              << ",\"newPending\":" << (next == ActionAdmission::Pending)
              << ",\"begins\":" << driver.begins << "}\n";
    if (corrupt)
        return !ledger.healthy() && driver.begins == 0 ? 0 : 4;
    return ledger.healthy() && original.state == ActionAdmission::Replay && driver.begins == 0 ? 0
                                                                                               : 4;
}
}  // namespace

// main
// 模式固定：持锁、锁拒绝、故障或重开；任何失败仅数字返回，不输出路径或任意异常。
int main(int argc, char** argv) {
    if (argc != 4)
        return 2;
    const std::string requested(argv[1]), path(argv[2]), marker(argv[3]);
    configure_fault(path, requested);
    const std::string mode = (requested == "writeAdmission" || requested == "syncAdmission")
                                 ? "faultAdmission"
                                 : ((requested == "writeTerminal" || requested == "syncTerminal" ||
                                     requested == "writeTerminalPartial")
                                        ? "faultTerminal"
                                        : requested);
    std::cout << std::boolalpha;
    if (mode == "inspect" || mode == "inspectCorrupt")
        return inspect(path, marker, mode == "inspectCorrupt");
    if (mode == "byteCapacityReplay") {
        // 整份合法日志先真实校验，再每8个原终态报告有限进度；不会把进度当整体成功。
        ActionFileLedger ledger(path, "probe-device");
        std::cout << "{\"opened\":" << ledger.healthy() << "}\n" << std::flush;
        if (!ledger.healthy())
            return 4;
        MarkerDriver driver(marker);
        ActionEngine engine(driver, 5000, &ledger);
        const std::string params = R"({"padding":")" + std::string(32U * 1024U, 'a') + R"("})";
        unsigned count = 0;
        for (; count < 256; ++count) {
            const auto token = "byte-budget-" + std::to_string(count);
            if (!ledger.recorded(token))
                break;
            const auto body = "{\"method\":\"action\",\"msgToken\":\"" + token +
                              "\",\"actionID\":\"setVolume\",\"params\":" + params + "}";
            if (engine.submit(body, count + 1).state != ActionAdmission::Replay)
                return 4;
            if ((count + 1) % 8 == 0)
                std::cout << "{\"replayed\":" << count + 1 << ",\"begins\":" << driver.begins
                          << "}\n"
                          << std::flush;
        }
        std::cout << "{\"healthy\":" << ledger.healthy() << ",\"replayed\":" << count
                  << ",\"begins\":" << driver.begins << "}\n";
        return ledger.healthy() && count > 0 && count < 256 && driver.begins == 0 ? 0 : 4;
    }
    if (mode != "hold" && mode != "holdIgnoreTerm" && mode != "locked" &&
        mode != "faultAdmission" && mode != "faultTerminal")
        return 2;
    ActionFileLedger ledger(path, "probe-device");
    if (mode == "locked") {
        std::cout << "{\"healthy\":" << ledger.healthy() << "}\n";
        return ledger.healthy() ? 3 : 0;
    }
    if (!ledger.healthy())
        return 3;
    MarkerDriver original(marker);
    ActionEngine original_engine(original, 5000, &ledger);
    std::uint64_t clock = 100;
    if (!complete(original_engine, true, clock) || original.begins != 1)
        return 4;
    if (mode == "hold" || mode == "holdIgnoreTerm") {
        // 只在辅助子进程忽略TERM，以验证父测试的有界升级；不修改宿主或其他测试的信号。
        if (mode == "holdIgnoreTerm" && ::signal(SIGTERM, SIG_IGN) == SIG_ERR)
            return 5;
        std::cout << "{\"ready\":true,\"originalStored\":true,\"originalBegins\":1}\n"
                  << std::flush;
        char released = '\0';
        if (!std::cin.get(released) || released != 'x')
            return 5;
        const auto replay = original_engine.submit(wire(true), ++clock);
        std::cout << "{\"healthy\":" << ledger.healthy()
                  << ",\"originalReplay\":" << (replay.state == ActionAdmission::Replay)
                  << ",\"originalBegins\":" << original.begins << "}\n";
        return ledger.healthy() && replay.state == ActionAdmission::Replay && original.begins == 1
                   ? 0
                   : 6;
    }
    const auto original_bytes = read_bytes(path);
    MarkerDriver next(marker);
    ActionEngine engine(next, 5000, &ledger);
    if (engine.submit(wire(false), ++clock).state != ActionAdmission::Accepted)
        return 4;
    bool published = false;
    for (unsigned index = 0; index < 500; ++index) {
        engine.poll(++clock);
        published = published || engine.take_reply().has_value();
        if (!ledger.healthy()) {
            engine.poll(++clock);
            published = published || engine.take_reply().has_value();
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const auto current_bytes = read_bytes(path);
    const bool retained = current_bytes.size() >= original_bytes.size() &&
                          current_bytes.compare(0, original_bytes.size(), original_bytes) == 0;
    const bool failed = ledger.persistence("probe-new-action", mode == "faultTerminal") ==
                        ActionPersistence::Failed;
    std::cout << "{\"healthy\":" << ledger.healthy() << ",\"persistenceFailed\":" << failed
              << ",\"publishedReply\":" << published << ",\"newBegins\":" << next.begins
              << ",\"originalBegins\":" << original.begins << ",\"priorFactsRetained\":" << retained
              << "}\n";
    const auto expected = mode == "faultAdmission" ? 0U : 1U;
    return !ledger.healthy() && failed && !published && retained && next.begins == expected ? 0 : 7;
}
