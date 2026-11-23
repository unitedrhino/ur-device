// 文件说明：桌面宿主只读媒体源目录；仅解析固定内容引用，不从邀请派生URL、输出或授权期限。
#pragma once
#include "browser_playback_admission.hpp"

namespace ur::device {
// BrowserFileParent 返回C++已验证的当前AI父会话；未建立、结束或撤权时返回空，不采信网页自报。
using BrowserFileParent = std::function<std::optional<std::string>()>;
// decode_browser_file_catalog 校验宿主配置JSON并将固定Unix截止换算到单调时钟；失败清空条目。
bool decode_browser_file_catalog(std::string_view json, std::uint64_t now, std::uint64_t wall,
                                 std::vector<BrowserFileBinding>& entries);
// BrowserFileCatalog 条目为宿主预先授权的来源/完整输出/单调截止，session和playback_id须为空。
// 平台仍负责用户/设备内容授权；本目录仅给设备固定源及独立许可，不是焦点取得证明。
class BrowserFileCatalog {
public:
    // 构造时深持有最多128个独立引用，非法项/重复引用/多源域使整个目录不可用。
    BrowserFileCatalog(std::vector<BrowserFileBinding> entries, BrowserFileParent parent);
    // valid 返回启动配置是否完整合法，不代表节目已加载、播放或具有实际输出焦点。
    bool valid() const { return valid_; }
    // resolve 固定原子会话及实例并捕获原AI父许可；不下载或阻塞事件线程，不延长目录截止。
    std::optional<BrowserFileResolution> resolve(const PlaybackInvite& invite, std::uint64_t now);

private:
    std::vector<BrowserFileBinding> entries_;  // 原配置副本，只读；不按控制报文更新URL或期限。
    BrowserFileParent parent_;                 // 当前已验证C++业务状态，寿命由宿主保证。
    std::optional<std::uint64_t> clock_;       // 目录跨实例保持单调栅栏，回退永久拒绝。
    bool valid_ = false;                       // 整体配置有效性，不跳过错误项继续广告能力。
    bool fenced_ = false;                      // 时钟失效后不能靠新邀请复活目录。
};
}  // namespace ur::device
