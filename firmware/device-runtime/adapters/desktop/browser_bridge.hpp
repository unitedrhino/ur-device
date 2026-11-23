// 桌面浏览器桥：只传本地界面命令与PCM，不持有平台三元组，也不替代C++协议状态机。
#pragma once
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace ur::device {
struct ThreadProgress;  // 独立固定原子对象，HTTP不借用Runtime/业务驱动。
// valid_browser_media_origin
// 校验可信配置中的规范HTTPS源，拒绝本机字面地址和CSP注入；不替代内容授权/DNS核验。
bool valid_browser_media_origin(std::string_view origin);
// BrowserEventKind
// 区分连接事实、待处理正文与不可恢复背压，回调不直接进入设备核心。
enum class BrowserEventKind { Connected, Message, Closed, Failed };
// BrowserEvent 的正文拥有独立存储，只在设备主事件线程解析。
struct BrowserEvent {
    BrowserEventKind kind = BrowserEventKind::Failed;  // 网络事实，不等于平台会话状态。
    std::string body;  // 仅Message有正文；故障不包含凭据或第三方原始错误。
};
// BrowserBridge
// 绑定显式回环端口；同源校验加会话随机令牌，只允许一个已认证浏览器。
class BrowserBridge {
public:
    // port为1024至65535；token为64位小写十六进制。assets仅固定界面/测试媒体路径；
    // trusted_media_origin只有启用文件组件才可配置单个可信HTTPS源，不为旧页面开放跨源下载。
    BrowserBridge(unsigned port, std::string token, std::map<std::string, std::string> assets,
                  std::string trusted_media_origin = {},
                  std::shared_ptr<const ThreadProgress> progress = {});
    ~BrowserBridge();
    BrowserBridge(const BrowserBridge&) = delete;
    BrowserBridge& operator=(const BrowserBridge&) = delete;
    bool start();                        // 失败不回退公网地址或其他端口，不输出凭据。
    void stop();                         // 幂等关闭并等待网络线程，返回后没有回调。
    unsigned port() const;               // 返回已绑定端口，未启动时为0。
    std::optional<BrowserEvent> poll();  // 主线程一次消费一条有界事件。
    bool send(const std::string& body);  // 只发送有界文本；断连或背压明确失败。
private:
    struct Impl;                  // 第三方网络与线程依赖不进入核心或公开接口。
    std::unique_ptr<Impl> impl_;  // 网络对象比回调捕获的状态先停止。
};
}  // namespace ur::device
