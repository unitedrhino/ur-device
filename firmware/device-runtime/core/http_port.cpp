// HTTPS共享边界校验；不访问网络、时钟、凭据或平台TLS，不把URL解析器引入嵌入式核心。
#include "ur/device/http_port.hpp"

#include <algorithm>
#include <string_view>

namespace ur::device {
namespace {
// ascii_lower 只处理HTTP头的ASCII大小写，不依赖设备locale。
char ascii_lower(char ch) {
    return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch + ('a' - 'A')) : ch;
}
// header_token 拒绝分隔符和非ASCII头名，不能通过头名注入额外请求。
bool header_token(std::string_view value) {
    if (value.empty() || value.size() > 64)
        return false;
    for (char ch : value) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
            std::string_view("!#$%&'*+-.^_`|~").find(ch) != std::string_view::npos)
            continue;
        return false;
    }
    return true;
}
// same_header 原始值保持不变，仅在保留头/重复头判断时作ASCII不区分大小写比较。
bool same_header(std::string_view left, std::string_view right) {
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(),
                      [](char a, char b) { return ascii_lower(a) == ascii_lower(b); });
}
}  // namespace
// valid_http_request 在平台库复制原件/短期凭据前拒绝超预算和HTTP注入，不代替实际TLS授权。
bool valid_http_request(const HttpRequest& request) {
    constexpr std::uint64_t max_body = 50ULL * 1024 * 1024;  // 当前平台文件上传上限。
    if ((request.method != HttpMethod::Post && request.method != HttpMethod::Put) ||
        request.url.size() < 9 || request.url.size() > 8192 ||
        request.url.compare(0, 8, "https://") != 0 || request.body_size > max_body ||
        request.response_limit == 0 || request.response_limit > 65536 ||
        request.connect_timeout_ms == 0 || request.connect_timeout_ms > 30000 ||
        request.timeout_ms < request.connect_timeout_ms || request.timeout_ms > 120000 ||
        request.headers.size() > 16)
        return false;
    const std::string_view url(request.url);  // URL不复制到错误日志，查询中的签名仍为敏感数据。
    const auto end = url.find_first_of("/?#", 8);
    const auto authority = url.substr(8, end == std::string_view::npos ? end : end - 8);
    if (authority.empty() || authority.find('@') != std::string_view::npos ||
        url.find('#') != std::string_view::npos || url.find('\\') != std::string_view::npos)
        return false;
    for (char ch : url)
        if (static_cast<unsigned char>(ch) <= 32 || static_cast<unsigned char>(ch) >= 127)
            return false;
    std::size_t header_size = 0;  // 在复制到平台HTTP库之前限制短期凭据的总内存。
    for (std::size_t i = 0; i < request.headers.size(); ++i) {
        const auto& header = request.headers[i];
        if (!header_token(header.name) || header.value.size() > 4096)
            return false;
        for (char ch : header.value)
            if (static_cast<unsigned char>(ch) < 32 || ch == 127)
                return false;
        for (const auto* reserved : {"Host", "Content-Length", "Transfer-Encoding", "Connection",
                                     "Expect", "Proxy-Authorization", "Trailer", "TE"})
            if (same_header(header.name, reserved))
                return false;
        for (std::size_t j = 0; j < i; ++j)
            if (same_header(header.name, request.headers[j].name))
                return false;
        header_size += header.name.size() + header.value.size() + 4;
        if (header_size > 8192)
            return false;
    }
    return true;
}
}  // namespace ur::device
