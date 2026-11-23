// 文件说明：可信内容/输出绑定与浏览器异步准备分离，准备完成才交给共享文件端点。
#pragma once
#include "browser_file_media.hpp"
#include "ur/device/playback_endpoint.hpp"

namespace ur::device {
// BrowserPlaybackAdmission 只消费宿主可信绑定；不把MQTT contentRef解析为自由URL。
// 原邀请可先到达并保持Pending，实际fileReady后才交付；不提供内容服务或焦点默认授权。
class BrowserPlaybackAdmission final : public PlaybackAdmissionPort {
public:
    BrowserPlaybackAdmission(BrowserFileBinding binding, BrowserFilePermission permission);
    // media 供唯一宿主事件线程泵入浏览器事实/取本地命令；不得绕过端点执行云端控制。
    BrowserFileMedia& media() { return *media_; }
    // authorize 严格匹配固定实例、内容和完整输出；准备等待不重建，原授权仅交付一次。
    PlaybackGrant authorize(const PlaybackInvite& invite, std::uint64_t now) override;
    // cancel 只关闭原固定准备资源；继续泵fileClosed后才能确认释放。
    bool cancel(std::string_view session, std::uint64_t now) override;

private:
    BrowserFileBinding binding_;               // 独立授权器给出的固定内容绑定，不来自模型URL。
    std::shared_ptr<BrowserFileMedia> media_;  // 宿主泵与端点驱动共持同一实际实例，不复制驱动。
    bool issued_ = false;                      // 同一绑定仅交付一次，端点负责原邀请幂等。
};

// BrowserFileResolution 将可信内容截止和原实例整组输出许可一并交给桌面适配，不提供默认授权。
struct BrowserFileResolution {
    BrowserFileBinding binding;        // 内容解析器固定的原会话/播放器/内容及独立截止。
    BrowserFilePermission permission;  // 捕获原实例的实际speaker/display租约，而非全局常开开关。
};
// BrowserFileResolver 只查询已准备的可信内容目录；不得在MQTT事件线程阻塞网络或下载媒体。
// 失败返回空值；URL、许可截止及输出组都不能从不可信邀请直接构造。
using BrowserFileResolver = std::function<std::optional<BrowserFileResolution>(
    const PlaybackInvite& invite, std::uint64_t now)>;

// BrowserResolvedPlaybackAdmission 按平台动态子会话惰性准备，复用固定适配与真实驱动。
// 一次只保留一个实际播放器；未知释放期间不解析第二个源，不能提前替换或续期。
class BrowserResolvedPlaybackAdmission final : public PlaybackAdmissionPort {
public:
    explicit BrowserResolvedPlaybackAdmission(BrowserFileResolver resolver);
    // active_media 返回当前实际驱动或空；指针仅在下一次准入前有效，宿主每轮重新取值。
    BrowserFileMedia* active_media();
    // authorize 原实例准备期间只轮询固定适配；完整释放后才能解析另一个独立实例。
    PlaybackGrant authorize(const PlaybackInvite& invite, std::uint64_t now) override;
    // cancel 只取消原实际准备，未知session表示从未分配资源，不影响其他合法实例。
    bool cancel(std::string_view session, std::uint64_t now) override;

private:
    // advance 时钟回退永久关闭当前适配；不能靠新邀请重置时钟并恢复输出。
    bool advance(std::uint64_t now);
    BrowserFileResolver resolver_;                      // 宿主可信目录入口，缺失时拒绝所有邀请。
    std::unique_ptr<BrowserPlaybackAdmission> active_;  // 先关闭再替换，原驱动租约仍独立持有。
    std::string session_;                 // 当前实际准备固定子会话，不接受父会话冒名。
    std::optional<std::uint64_t> clock_;  // 跨原实例替换仍保持同一个单调时钟。
    bool fenced_ = false;                 // 时钟故障后不可由新邀请解除。
};
}  // namespace ur::device
