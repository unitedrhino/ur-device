// 有界流式HTTPS端口；平台TLS/套接字实现不进入设备核心，耗时调用仅在上传worker执行。
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ur::device {
// HttpMethod
// 仅开放设备附件准备/确认所需POST及私有原件直传所需PUT，不支持自动重放。
enum class HttpMethod { Post, Put };
// HttpResult 使用稳定分类，不返回供应商异常正文、认证头或带签名的URL。
enum class HttpResult {
    Ok,
    Invalid,
    Busy,
    Cancelled,
    Timeout,
    TlsFailed,
    SourceFailed,
    TransportFailed,
    ResponseTooLarge,
    HttpFailed
};
// HttpHeader 持有worker作用域内的短期头；私有OSS请求不得继承平台Authorization。
struct HttpHeader {
    std::string name;   // 合法HTTP token；不允许重复或覆写传输层保留头。
    std::string value;  // 不含CR/LF/NUL；不得输出到日志或浏览器。
};
// HttpRequest 明确单次传输边界；无跳转、无环境代理、无关闭TLS校验的配置项。
struct HttpRequest {
    HttpMethod method = HttpMethod::Post;      // 本次单次请求方法。
    std::string url;                           // 完整HTTPS地址，禁止userinfo和fragment。
    std::vector<HttpHeader> headers;           // 当前目标专属头，最多16项及8KiB总量。
    std::uint64_t body_size = 0;               // 精确原件长度，最多50MiB，不依赖整包RAM。
    std::size_t response_limit = 65536;        // 成功响应预算，1～64KiB。
    std::uint32_t connect_timeout_ms = 10000;  // DNS/TCP/TLS建连截止，1～30000ms。
    std::uint32_t timeout_ms = 30000;          // 本次总截止，建连以上且不超过120000ms。
};
// HttpBodySource
// 顺序提供原件；false为读取失败，提前EOF拒绝，不允许自动seek或重传。
class HttpBodySource {
   public:
    virtual ~HttpBodySource() = default;
    // read 将不超过capacity的字节写入buffer并设置count；成功count=0表示真实EOF。
    virtual bool read(std::uint8_t* buffer, std::size_t capacity, std::size_t& count) = 0;
};
// HttpResponse 失败不保留响应正文，HTTP状态不是平台业务code或附件ready证明。
struct HttpResponse {
    HttpResult result = HttpResult::Invalid;  // 稳定结果；只有Ok允许读取body。
    unsigned int status = 0;                  // 实际HTTP状态，未取得响应为0。
    std::string body;                         // 有界的2xx正文，仍需上层JSON/业务契约校验。
};
// valid_http_request
// 校验共享资源/报文边界；平台实现还必须严格解析URL和验证TLS。
bool valid_http_request(const HttpRequest& request);
// HttpPort
// 同实例一次只允许一个worker调用；cancel须存活到perform返回，禁止在MQTT回调调用。
class HttpPort {
   public:
    virtual ~HttpPort() = default;
    // perform
    // 单次流式传输；空body允许source=nullptr，取消不隐式重试或提交附件确认。
    virtual HttpResponse perform(const HttpRequest& request, HttpBodySource* source,
                                 const std::atomic_bool& cancel) = 0;
};
}  // namespace ur::device
