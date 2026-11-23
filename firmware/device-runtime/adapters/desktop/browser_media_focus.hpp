// 文件说明：同一桌面宿主的AI与文件输出焦点许可；复用唯一实际驱动，不派生采集或设备身份权限。
#pragma once
#include <array>
#include "browser_playback_admission.hpp"
#include "browser_voice_focus.hpp"

namespace ur::device {
// BrowserFileEstablished 只查询共享核心收录的原子会话建立状态，不把fileReady当平台建立。
using BrowserFileEstablished = std::function<bool(std::string_view)>;
// BrowserMediaFocusAuthority 固定AI路径0及文件audio/video/audioVideo路径1/2/3。
class BrowserMediaFocusAuthority final : public FocusAuthority {
public:
    // 所有借用对象寿命须长于本授权器；deadline为独立宿主期限，不从控制续租。
    BrowserMediaFocusAuthority(OutputFocusDriver& driver, BrowserResolvedPlaybackAdmission& files,
                               std::uint64_t deadline, BrowserVoiceScope voice,
                               BrowserFileEstablished established);
    // authorize 只准入原已建立业务/完整输出；更换节目须旧租约已显式实际释放。
    std::optional<FocusPermit> authorize(const FocusCommand& command, std::uint64_t now,
                                         std::uint64_t wall) override;
    // valid 当前原业务、许可、期限或输出组失效立即拒绝，不复活终态路径。
    bool valid(const FocusPermit& permit, std::uint64_t now, std::uint64_t wall) const override;

private:
    // file_path 核对当前已建立原节目及独立内容许可，返回固定完整资源路径。
    std::optional<std::size_t> file_path(const FocusCommand& command, std::uint64_t now,
                                         std::uint64_t wall) const;
    OutputFocusDriver& driver_;                // 唯一全局水位与固定实际端口，不创建第二套仲裁。
    BrowserResolvedPlaybackAdmission& files_;  // 当前真实文件驱动，不持有跨替换裸指针。
    std::uint64_t deadline_;                   // 原独立宿主截止。
    BrowserVoiceFocusAuthority voice_;         // 保留现有AI范围与路径0准入规则。
    BrowserFileEstablished established_;       // C++核心事实，不从网页/报文自报。
    std::array<std::pair<std::string, std::string>, 3>
        scopes_;  // 当前原文件租约；更换仅由唯一驱动的实际释放栅栏允许。
};
}  // namespace ur::device
