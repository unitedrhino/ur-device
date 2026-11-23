// Windows/Linux共享libcurl
// HTTPS适配；库句柄及TLS策略仅存在于桌面驱动，不进入核心。
#pragma once
#include "ur/device/http_port.hpp"

namespace ur::device {
// CurlHttpPort
// 同步worker端口；默认使用系统CA，可指定独立CA文件但不能关闭证书/主机名校验。
class CurlHttpPort final : public HttpPort {
   public:
    // ca_file为空使用系统信任链；自有测试CA只在本任务隔离HTTPS端点使用。
    explicit CurlHttpPort(std::string ca_file = {});
    HttpResponse perform(const HttpRequest& request, HttpBodySource* source,
                         const std::atomic_bool& cancel) override;

   private:
    std::string ca_file_;           // 本实例不可变信任链路径，不持有平台身份。
    std::atomic_bool busy_{false};  // 拒绝并发重入，不把两个上传的原件或认证头混用。
};
}  // namespace ur::device
