// 流式HTTPS上传：真实TLS验证、单次目标、固定资源上限及取消；任何失败均不返回敏感正文。
#include "curl_http_port.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <memory>
#include <utility>

namespace ur::device {
namespace {
// CurlGlobal
// 由C++线程安全静态初始化创建；不在单个上传结束时破坏其他worker的HTTP库。
struct CurlGlobal {
    CURLcode result = curl_global_init(CURL_GLOBAL_DEFAULT);  // 唯一进程级初始化结果。
    ~CurlGlobal() {
        if (result == CURLE_OK)
            curl_global_cleanup();
    }
};
// BusyLease 只归本次请求持有，所有返回路径均释放端口占用。
struct BusyLease {
    std::atomic_bool& busy;  // 已被本次请求成功取得的原实例标志。
    ~BusyLease() { busy.store(false); }
};
// Transfer 只借用当前worker存活的源/取消标志，记录实际字节而不是声明成功。
struct Transfer {
    HttpBodySource* source;          // 为空仅允许零长度body。
    const std::atomic_bool& cancel;  // 调用者拥有，须存活至perform返回。
    std::uint64_t expected;          // 原声明精确长度。
    std::size_t limit;               // 2xx正文保留上限。
    std::uint64_t read = 0;          // 已从原件实际取得的字节。
    bool source_failed = false;      // 提前EOF/非法count/源失败不能变成HTTP成功。
    bool response_large = false;     // 超大正文终止传输，不返回截断JSON。
    std::string response;            // 限额内临时正文，失败即丢弃。
};
// read_body 仅顺序取精确原件，返回ABORT而不是提前EOF以避免对端等待声明长度。
std::size_t read_body(char* buffer, std::size_t size, std::size_t count, void* context) {
    auto& transfer = *static_cast<Transfer*>(context);
    if (transfer.cancel.load())
        return CURL_READFUNC_ABORT;
    if (size != 1 || count == 0 || transfer.read > transfer.expected) {
        transfer.source_failed = true;
        return CURL_READFUNC_ABORT;
    }
    const auto capacity =
        static_cast<std::size_t>(std::min<std::uint64_t>(count, transfer.expected - transfer.read));
    if (capacity == 0)
        return 0;
    std::size_t produced = 0;  // 由source写入，超预算和无进展均为明确失败。
    if (!transfer.source ||
        !transfer.source->read(reinterpret_cast<std::uint8_t*>(buffer), capacity, produced) ||
        produced == 0 || produced > capacity) {
        transfer.source_failed = true;
        return CURL_READFUNC_ABORT;
    }
    transfer.read += produced;
    return produced;
}
// write_body 有界接收，取消/超限时终止，不把片段当作完整业务应答。
std::size_t write_body(char* buffer, std::size_t size, std::size_t count, void* context) {
    auto& transfer = *static_cast<Transfer*>(context);
    if (transfer.cancel.load())
        return 0;
    if (size != 1 || count > transfer.limit - transfer.response.size()) {
        transfer.response_large = true;
        return 0;
    }
    transfer.response.append(buffer, count);
    return count;
}
// progress 可由另一线程取消；不依赖媒体事件循环或轮询长阻塞源。
int progress(void* context, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return static_cast<Transfer*>(context)->cancel.load() ? 1 : 0;
}
// valid_url 使用库的结构化解析再核无用户信息/fragment，不把原地址拼进错误正文。
bool valid_url(const std::string& source) {
    const auto url =
        std::unique_ptr<CURLU, decltype(&curl_url_cleanup)>(curl_url(), curl_url_cleanup);
    if (!url || curl_url_set(url.get(), CURLUPART_URL, source.c_str(), 0) != CURLUE_OK)
        return false;
    char* part = nullptr;  // 每次成功读取后释放，不能把userinfo遗漏为普通host。
    if (curl_url_get(url.get(), CURLUPART_HOST, &part, 0) != CURLUE_OK)
        return false;
    const bool host_present = part && *part;
    curl_free(part);
    if (!host_present)
        return false;
    for (const auto kind : {CURLUPART_USER, CURLUPART_PASSWORD, CURLUPART_FRAGMENT}) {
        part = nullptr;
        const auto result = curl_url_get(url.get(), kind, &part, 0);
        curl_free(part);
        if (result == CURLUE_OK)
            return false;
    }
    return true;
}
}  // namespace
// 构造仅保存独立信任链路径，不发网络、不复制设备身份或提前创建HTTP句柄。
CurlHttpPort::CurlHttpPort(std::string ca_file) : ca_file_(std::move(ca_file)) {}
// perform 对当前worker执行一次完整传输；返回固定分类/状态及有界成功正文，不重放请求。
HttpResponse CurlHttpPort::perform(const HttpRequest& request, HttpBodySource* source,
                                   const std::atomic_bool& cancel) {
    if (!valid_http_request(request) || (request.body_size && !source))
        return {};
    if (cancel.load())
        return {HttpResult::Cancelled, 0, {}};
    if (busy_.exchange(true))
        return {HttpResult::Busy, 0, {}};
    const BusyLease lease{busy_};    // 出错/超时/取消都释放原实例，不保留请求或认证信息。
    static const CurlGlobal global;  // curl_global_init先于所有libcurl API及并发worker。
    if (global.result != CURLE_OK)
        return {HttpResult::TransportFailed, 0, {}};
    if (!valid_url(request.url))
        return {};
    const auto handle =
        std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>(curl_easy_init(), curl_easy_cleanup);
    if (!handle)
        return {HttpResult::TransportFailed, 0, {}};
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(nullptr,
                                                                        curl_slist_free_all);
    // append_header 不丢弃OOM前原链表，已分配的头由本次请求RAII清理。
    const auto append_header = [&](const std::string& value) {
        auto* appended = curl_slist_append(headers.get(), value.c_str());
        if (!appended)
            return false;
        // append返回的新头接管原链表，释放的返回指针不拥有对象，紧随reset不重复释放。
        [[maybe_unused]] auto* transferred_headers = headers.release();
        headers.reset(appended);
        return true;
    };
    for (const auto& header : request.headers)
        if (!append_header(header.name + ": " + header.value))
            return {HttpResult::TransportFailed, 0, {}};
    if (!append_header("Expect:"))  // 无隐式100-continue等待；保留头不可由调用者覆写。
        return {HttpResult::TransportFailed, 0, {}};
    Transfer transfer{source, cancel, request.body_size, request.response_limit, 0, false,
                      false,  {}};
    transfer.response.reserve(std::min<std::size_t>(request.response_limit, 4096));
    // 每个安全选项的失败都阻止发网；TLS/协议限定不得因库版本或适配差异静默省略。
    const auto set = [&](CURLoption option, auto value) {
        return curl_easy_setopt(handle.get(), option, value) == CURLE_OK;
    };
    if (!set(CURLOPT_URL, request.url.c_str()) || !set(CURLOPT_PROTOCOLS_STR, "https") ||
        !set(CURLOPT_REDIR_PROTOCOLS_STR, "https") || !set(CURLOPT_FOLLOWLOCATION, 0L) ||
        !set(CURLOPT_PROXY, "") || !set(CURLOPT_NETRC, static_cast<long>(CURL_NETRC_IGNORED)) ||
        !set(CURLOPT_SSL_VERIFYPEER, 1L) || !set(CURLOPT_SSL_VERIFYHOST, 2L) ||
        !set(CURLOPT_SSLVERSION, static_cast<long>(CURL_SSLVERSION_TLSv1_2)) ||
        !set(CURLOPT_NOSIGNAL, 1L) || !set(CURLOPT_VERBOSE, 0L) ||
        !set(CURLOPT_HTTP_VERSION, static_cast<long>(CURL_HTTP_VERSION_1_1)) ||
        !set(CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(request.connect_timeout_ms)) ||
        !set(CURLOPT_TIMEOUT_MS, static_cast<long>(request.timeout_ms)) ||
        !set(CURLOPT_HTTPHEADER, headers.get()) || !set(CURLOPT_READFUNCTION, read_body) ||
        !set(CURLOPT_READDATA, &transfer) || !set(CURLOPT_WRITEFUNCTION, write_body) ||
        !set(CURLOPT_WRITEDATA, &transfer) || !set(CURLOPT_NOPROGRESS, 0L) ||
        !set(CURLOPT_XFERINFOFUNCTION, progress) || !set(CURLOPT_XFERINFODATA, &transfer) ||
        (!ca_file_.empty() && !set(CURLOPT_CAINFO, ca_file_.c_str())))
        return {HttpResult::TransportFailed, 0, {}};
    if (request.method == HttpMethod::Post) {
        if (!set(CURLOPT_POST, 1L) ||
            !set(CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body_size)))
            return {HttpResult::TransportFailed, 0, {}};
    } else if (!set(CURLOPT_UPLOAD, 1L) ||
               !set(CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(request.body_size))) {
        return {HttpResult::TransportFailed, 0, {}};
    }
    const auto result = curl_easy_perform(handle.get());  // 仅此同步worker阻塞，媒体/MQTT独立推进。
    long code = 0;  // HTTP状态保留便于上层分类，错误正文和签名地址永不返回。
    curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &code);
    const auto status = code > 0 && code <= 599 ? static_cast<unsigned int>(code) : 0U;
    if (cancel.load())
        return {HttpResult::Cancelled, status, {}};
    if (transfer.source_failed)
        return {HttpResult::SourceFailed, status, {}};
    if (transfer.response_large)
        return {HttpResult::ResponseTooLarge, status, {}};
    if (result == CURLE_OPERATION_TIMEDOUT)
        return {HttpResult::Timeout, status, {}};
    if (result == CURLE_PEER_FAILED_VERIFICATION || result == CURLE_SSL_CONNECT_ERROR ||
        result == CURLE_SSL_CACERT_BADFILE)
        return {HttpResult::TlsFailed, status, {}};
    if (result != CURLE_OK)
        return {HttpResult::TransportFailed, status, {}};
    if (status < 200 || status >= 300)
        return {HttpResult::HttpFailed, status, {}};
    curl_off_t uploaded = 0;  // 2xx也必须证明精确原件确已上传，不能接受对端提前应答。
    if (curl_easy_getinfo(handle.get(), CURLINFO_SIZE_UPLOAD_T, &uploaded) != CURLE_OK ||
        uploaded < 0 || static_cast<std::uint64_t>(uploaded) != request.body_size ||
        transfer.read != request.body_size)
        return {HttpResult::SourceFailed, status, {}};
    return {HttpResult::Ok, status, std::move(transfer.response)};
}
}  // namespace ur::device
