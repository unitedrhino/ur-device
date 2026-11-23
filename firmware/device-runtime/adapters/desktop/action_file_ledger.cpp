// 文件说明：有界追加日志、设备身份绑定及异步fsync屏障；Windows当前明确不支持此新增适配。
// 启动校验全部原记录，持有独占锁；只接受完整校验和帧，损坏/截断/满额不删文件或重执行。
#include "action_file_ledger.hpp"
#include <cJSON.h>
#include <openssl/sha.h>
#include <array>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <thread>
#include <utility>
#include "environment.hpp"
#include "ur/device/json_parse.hpp"
#ifndef _WIN32
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#endif

namespace ur::device {
namespace {
// journal_bytes/entries/jobs 限制磁盘、恢复内存及worker排队，满额不自动淘汰幂等记录。
constexpr std::size_t journal_bytes = std::size_t{4} * 1024 * 1024;
constexpr std::size_t journal_entries = 256;
constexpr std::size_t journal_jobs = 8;
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
// print_json 收回cJSON缓冲，不借用调用者或解析树的生命周期。
std::string print_json(const cJSON* value) {
    auto* bytes = cJSON_PrintUnformatted(value);
    if (!bytes)
        return {};
    std::string result(bytes);
    cJSON_free(bytes);
    return result;
}
// request_wire 按现行action合同重新规范化，日志不保存云端自由扩展字段。
std::string request_wire(const ActionRequest& request) {
    std::string canonical_params;
    if (canonical_json_object(request.params, canonical_params) != DecodeResult::Ok)
        return {};
    Json root(cJSON_CreateObject(), cJSON_Delete);
    Json params(parse_json(canonical_params.c_str()), cJSON_Delete);
    if (!root || !params || !cJSON_IsObject(params.get()) ||
        !cJSON_AddStringToObject(root.get(), "method", "action") ||
        !cJSON_AddStringToObject(root.get(), "msgToken", request.token.c_str()) ||
        !cJSON_AddStringToObject(root.get(), "actionID", request.action.c_str()) ||
        !cJSON_AddItemToObject(root.get(), "params", params.get()))
        return {};
    // root接管params，返回指针仅记录转交，不改变日志正文或固化时序。
    [[maybe_unused]] auto* transferred_params = params.release();
    std::string canonical;
    ActionRequest checked;
    if (canonical_json_object(print_json(root.get()), canonical) != DecodeResult::Ok ||
        decode_action_request(canonical, checked) != DecodeResult::Ok ||
        checked.token != request.token || checked.action != request.action ||
        checked.params != request.params)
        return {};
    return canonical;
}
// reply_request 重新编码原终态核对全部字段，不接纳未知键、错关联或损坏后仍碰巧合法的投影。
bool reply_request(std::string_view body, ActionReply& reply) {
    std::string canonical;
    if (canonical_json_object(body, canonical) != DecodeResult::Ok)
        return false;
    Json root(parse_json(canonical.c_str()), cJSON_Delete);
    const auto field = [&](const char* key) {
        return cJSON_GetObjectItemCaseSensitive(root.get(), key);
    };
    const auto *token = field("msgToken"), *action = field("actionID"), *code = field("code"),
               *message = field("msg"), *data = field("data");
    if (!cJSON_IsString(token) || !cJSON_IsString(action) || !cJSON_IsString(message) ||
        !cJSON_IsNumber(code) || code->valuedouble != std::floor(code->valuedouble) ||
        code->valuedouble < 200 || code->valuedouble > 599 || !cJSON_IsObject(data))
        return false;
    reply = {token->valuestring, action->valuestring, static_cast<int>(code->valuedouble),
             message->valuestring, print_json(data)};
    std::string checked;
    return encode_action_reply(reply, checked) == DecodeResult::Ok && checked == canonical;
}
// frame 将种类、正文及长度一起校验；校验和只用于损坏检测，不是外部签名或授权。
std::string frame(unsigned char kind, const std::string& body) {
    if (body.empty() || body.size() > 65536)
        return {};
    const auto size = static_cast<std::uint32_t>(body.size() + 1);
    std::string result(4, '\0');
    for (unsigned index = 0; index < 4; ++index)
        result[index] = static_cast<char>((size >> (24 - index * 8)) & 255U);
    result += static_cast<char>(kind);
    result += body;
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    if (!SHA256(reinterpret_cast<const unsigned char*>(result.data()), result.size(),
                digest.data()))
        return {};
    result.append(reinterpret_cast<const char*>(digest.data()), digest.size());
    return result;
}
}  // namespace

// Implementation 所有公开访问只持短内存锁；worker释放锁后才write/fsync，不从线程回调核心。
struct ActionFileLedger::Implementation {
    struct Entry {
        ActionRequest request;   // 不可变原受理，不存设备密钥。
        std::string reply;       // 原终态正文，不向日志输出。
        bool accepted = false;   // 受理已完成fsync。
        bool completed = false;  // 终态已完成fsync。
    };
    struct Job {
        std::string token;  // 原token，关联固化结果。
        std::string bytes;  // 有限校验和帧。
        bool terminal;      // false受理、true终态。
    };
    mutable std::mutex mutex;           // 仅保护有限状态，绝不包围磁盘IO。
    std::condition_variable wake;       // 唤醒唯一文件worker。
    std::map<std::string, Entry> rows;  // 最多256条，正文总量受4MiB日志预算限制。
    std::deque<Job> jobs;               // 最多8个待写帧。
    std::thread worker;                 // 仅本实例创建和回收。
    std::size_t reserved = 0;           // 已写及已排队字节，防止并发突破预算。
    int descriptor = -1;                // 原独占私有文件，Windows始终-1。
    bool failed = true;                 // 启动或写盘失败默认保守拒绝。
    bool stopping = false;              // 只停止接纳，已排队原帧仍顺序固化。

    // append 只由启动线程或唯一worker调用；完整写入且fsync后才返回实际固化。
    bool append(const std::string& bytes) {
#ifndef _WIN32
        struct stat info{};
        if (::fstat(descriptor, &info) != 0 || info.st_nlink != 1)
            return false;
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const auto count = ::write(descriptor, bytes.data() + offset, bytes.size() - offset);
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
                return false;
            offset += static_cast<std::size_t>(count);
        }
        return ::fsync(descriptor) == 0;
#else
        (void)bytes;
        return false;
#endif
    }

    // restore 校验整份有限原日志；截断尾帧也失败，不擅自修剪以免丢失执行过的受理。
    // 保留既有调用签名兼容原宿主；日志完整性与设备身份绑定分别严校验。
    // NOLINTBEGIN(bugprone-easily-swappable-parameters)
    bool restore(const std::string& bytes, const std::string& identity) {
        // NOLINTEND(bugprone-easily-swappable-parameters)
        std::size_t offset = 0;
        bool bound = false;
        while (offset < bytes.size()) {
            if (bytes.size() - offset < 4)
                return false;
            std::uint32_t size = 0;
            for (unsigned index = 0; index < 4; ++index)
                size = (size << 8U) | static_cast<unsigned char>(bytes[offset + index]);
            if (size < 2 || size > 65537 || bytes.size() - offset < 4 + size + SHA256_DIGEST_LENGTH)
                return false;
            const auto kind = static_cast<unsigned char>(bytes[offset + 4]);
            const auto body = bytes.substr(offset + 5, size - 1);
            if (frame(kind, body) != bytes.substr(offset, 4 + size + SHA256_DIGEST_LENGTH))
                return false;
            offset += 4 + size + SHA256_DIGEST_LENGTH;
            if (!bound) {
                if (kind != 0 || body != "ur-action-journal-1:" + identity)
                    return false;
                bound = true;
            } else if (kind == 1) {
                ActionRequest request;
                if (rows.size() >= journal_entries ||
                    decode_action_request(body, request) != DecodeResult::Ok ||
                    request_wire(request) != body || rows.count(request.token))
                    return false;
                const auto token =
                    request.token;  // 在移动请求之前固定键，不能依赖函数参数求值顺序。
                rows.emplace(token, Entry{std::move(request), {}, true, false});
            } else if (kind == 2) {
                ActionReply reply;
                if (!reply_request(body, reply))
                    return false;
                const auto entry = rows.find(reply.token);
                if (entry == rows.end() || entry->second.request.action != reply.action ||
                    !entry->second.reply.empty())
                    return false;
                entry->second.reply = body;
                entry->second.completed = true;
            } else
                return false;
        }
        return bound;
    }

    // open 只接受当前用户的0600常规独占文件；新文件先绑定身份并固化父目录，再允许连接设备。
    bool open(const std::string& path, const std::string& identity) {
#ifndef _WIN32
        if (path.empty() || path.size() > 4096 || path.find('\0') != std::string::npos ||
            identity.empty() || identity.size() > 256 || identity.find('\0') != std::string::npos)
            return false;
        const std::filesystem::path target(path);
        if (!target.is_absolute() || target.filename().empty())
            return false;
        descriptor = ::open(
            path.c_str(), O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
        struct stat info{};
        if (descriptor < 0 || ::fstat(descriptor, &info) != 0 || !S_ISREG(info.st_mode) ||
            info.st_uid != ::geteuid() || (info.st_mode & 0777) != 0600 || info.st_nlink != 1 ||
            info.st_size < 0 || static_cast<std::uint64_t>(info.st_size) > journal_bytes ||
            ::flock(descriptor, LOCK_EX | LOCK_NB) != 0)
            return false;
        if (info.st_size == 0) {
            const auto header = frame(0, "ur-action-journal-1:" + identity);
            if (header.empty() || !append(header))
                return false;
            const auto parent = target.parent_path().string();
            const int directory =
                ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (directory < 0)
                return false;
            const bool durable = ::fsync(directory) == 0;
            ::close(directory);
            if (!durable)
                return false;
            reserved = header.size();
        } else {
            std::string content(static_cast<std::size_t>(info.st_size), '\0');
            std::size_t offset = 0;
            while (offset < content.size()) {
                const auto count = ::pread(descriptor, content.data() + offset,
                                           content.size() - offset, static_cast<off_t>(offset));
                if (count < 0 && errno == EINTR)
                    continue;
                if (count <= 0)
                    return false;
                offset += static_cast<std::size_t>(count);
            }
            if (!restore(content, identity))
                return false;
            reserved = content.size();
        }
        failed = false;
        worker = std::thread([this] { work(); });
        return true;
#else
        (void)path;
        (void)identity;
        return false;
#endif
    }

    // work 按原受理→终态FIFO写盘；任何失败锁存，绝不跳过坏帧继续制造可重放成功。
    void work() {
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait(lock, [&] { return stopping || failed || !jobs.empty(); });
                if (failed || (stopping && jobs.empty()))
                    return;
                job = std::move(jobs.front());
                jobs.pop_front();
            }
            bool stored = false;
            try {
                stored = append(job.bytes);
            } catch (...) {
                stored = false;
            }
            std::lock_guard<std::mutex> lock(mutex);
            if (!stored) {
                failed = true;
                return;
            }
            auto& entry = rows.at(job.token);
            if (job.terminal)
                entry.completed = true;
            else
                entry.accepted = true;
        }
    }

    ~Implementation() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
        }
        wake.notify_all();
        if (worker.joinable())
            worker.join();
#ifndef _WIN32
        if (descriptor >= 0)
            ::close(descriptor);
#endif
    }
};

// ActionFileLedger 绑定调用者指定的私有路径和设备身份；启动错误保留为不健康实例供宿主拒绝。
ActionFileLedger::ActionFileLedger(std::string path, std::string identity)
    : impl_(std::make_unique<Implementation>()) {
    try {
        impl_->open(path, identity);
    } catch (...) {
        impl_->failed = true;
    }
}
// ~ActionFileLedger 等待本实例排队任务并关闭原文件，不操作其他实例或删除历史。
ActionFileLedger::~ActionFileLedger() = default;
// healthy 返回原文件校验和worker错误的锁存状态，不表示某个动作已经固化。
bool ActionFileLedger::healthy() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return !impl_->failed && !impl_->stopping;
}
// recorded 查询原token是否已登记，包括尚待worker固化的受理。
bool ActionFileLedger::recorded(std::string_view token) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->rows.count(std::string(token)) != 0;
}
// find_request 返回原token不可变受理副本，未知token返回空值，不触发文件读取。
std::optional<ActionRequest> ActionFileLedger::find_request(std::string_view token) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto row = impl_->rows.find(std::string(token));
    if (row == impl_->rows.end())
        return std::nullopt;
    return row->second.request;
}
// find_reply 只返回已实际固化的原终态，排队中和未知token均为空值。
std::optional<std::string> ActionFileLedger::find_reply(std::string_view token) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto row = impl_->rows.find(std::string(token));
    if (row == impl_->rows.end() || !row->second.completed)
        return std::nullopt;
    return row->second.reply;
}
// record 校验并排队原受理；true不是执行许可，调用者仍须轮询persistence。
bool ActionFileLedger::record(const ActionRequest& request) {
    const auto encoded = request_wire(request), bytes = frame(1, encoded);
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->failed || impl_->stopping || bytes.empty() || impl_->rows.count(request.token) ||
        impl_->rows.size() >= journal_entries || impl_->jobs.size() >= journal_jobs ||
        impl_->reserved + bytes.size() > journal_bytes)
        return false;
    impl_->rows.emplace(request.token, Implementation::Entry{request, {}, false, false});
    impl_->jobs.push_back({request.token, bytes, false});
    impl_->reserved += bytes.size();
    impl_->wake.notify_one();
    return true;
}
// record_reply 校验原token及动作关联后排队唯一终态，false时不能发布成功或改写历史。
bool ActionFileLedger::record_reply(std::string_view token, std::string_view reply) {
    ActionReply checked;
    if (!reply_request(reply, checked) || checked.token != token)
        return false;
    const auto bytes = frame(2, std::string(reply));
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto row = impl_->rows.find(std::string(token));
    if (impl_->failed || impl_->stopping || bytes.empty() || row == impl_->rows.end() ||
        row->second.request.action != checked.action || !row->second.reply.empty() ||
        impl_->jobs.size() >= journal_jobs || impl_->reserved + bytes.size() > journal_bytes)
        return false;
    impl_->jobs.push_back({std::string(token), bytes, true});
    row->second.reply = std::string(reply);
    impl_->reserved += bytes.size();
    impl_->wake.notify_one();
    return true;
}
// persistence 查询受理或终态的真实固化屏障；存储失败或未知token返回Failed。
ActionPersistence ActionFileLedger::persistence(std::string_view token, bool terminal) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto row = impl_->rows.find(std::string(token));
    if (impl_->failed || row == impl_->rows.end())
        return ActionPersistence::Failed;
    return (terminal ? row->second.completed : row->second.accepted) ? ActionPersistence::Stored
                                                                     : ActionPersistence::Pending;
}
// configured_action_ledger 只使用显式环境路径；未配置返回空账本，已配置但无效返回false。
bool configured_action_ledger(const std::string& identity,
                              std::unique_ptr<ActionFileLedger>& ledger) {
    ledger.reset();
    const auto path = environment_value("UR_DEVICE_ACTION_LEDGER_PATH");
    if (!path)
        return true;
    ledger = std::make_unique<ActionFileLedger>(*path, identity);
    return ledger->healthy();
}
}  // namespace ur::device
