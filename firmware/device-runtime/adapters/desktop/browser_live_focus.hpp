// 文件说明：AI与实时节目共享唯一输出仲裁；节目原授权与临时完整组mask独立。
#pragma once
#include <array>
#include "browser_voice_focus.hpp"

namespace ur::device {
// BrowserLiveFocusScope 由原C++端点读取，不允许网页填session或把Connected替代Established。
struct BrowserLiveFocusScope {
    std::string session;                           // 原不可变节目会话。
    bool audio = false, video = false;             // 原完整接收轨组，不允许请求子集。
    std::uint64_t deadline = 0;                    // 原独立有限授权截止，不因focus控制延长。
    bool authorized = false, established = false;  // 当前源/父业务及平台建立事实，均需成立。
    std::string parent_session{};  // 仅C++原父绑定；节目关闭不能借新父或设备主人扩大权限。
    bool platform_ended = false,
         actual_released = false;    // 平台终态与实际退出分别记，localclose不代替。
    std::uint64_t retain_until = 0;  // 固定Ended+十分钟关闭资格，非输出/源授权期限。
};
// BrowserLiveFocusQuery 只读原设备核心/实际端口，不承担第二套全局仲裁或授权缓存。
using BrowserLiveFocusQuery = std::function<std::optional<BrowserLiveFocusScope>()>;
// BrowserLiveReleaseQuery 只按原会话查紧凑关闭证明；新当前节目不得遮蔽旧原租约的合法释放。
using BrowserLiveReleaseQuery =
    std::function<std::optional<BrowserLiveFocusScope>(std::string_view)>;
// BrowserLiveFocusAuthority 固定AI0与节目audio1/video2/AV3；不提升文件或采集权限。
class BrowserLiveFocusAuthority final : public FocusAuthority {
public:
    // driver为唯一原仲裁器，voice/current须读取原C++事实，deadline是独立宿主期限。
    BrowserLiveFocusAuthority(OutputFocusDriver& driver, std::uint64_t deadline,
                              BrowserVoiceScope voice, BrowserLiveFocusQuery current,
                              BrowserLiveReleaseQuery release = {});
    // authorize 只为当前已建立原节目完整组绑定；换节目须唯一驱动已实际释放旧租约。
    std::optional<FocusPermit> authorize(const FocusCommand& command, std::uint64_t now,
                                         std::uint64_t wall) override;
    // valid 持续核原业务与有限期限；临时mask关闭不成为源撤权或自动新播放意图。
    bool valid(const FocusPermit& permit, std::uint64_t now, std::uint64_t wall) const override;
    // closed_release_valid
    // 只允许原平台Ended/actualReleased/历史原父同连接墓碑授有限关闭资格，非正常输出权。
    bool closed_release_valid(const FocusPermit& permit, std::uint64_t now,
                              std::uint64_t wall) const override;

private:
    // path 只求原完整资源组，不从控制消息扩展音轨、显示或节目来源。
    std::optional<std::size_t> path(const FocusCommand& command, std::uint64_t now,
                                    std::uint64_t wall) const;
    // scope_for 原release可求已终态关闭证明；所有开启/暂停仍只查当前Established原许可。
    std::optional<BrowserLiveFocusScope> scope_for(const FocusCommand& command) const;
    OutputFocusDriver& driver_;         // 唯一全局实际水位，与AI共享。
    std::uint64_t deadline_;            // 宿主有限期限，不可由恢复焦点续期。
    BrowserVoiceFocusAuthority voice_;  // AI路径沿原规则，不复制单双工策略。
    BrowserLiveFocusQuery current_;     // 当前C++真实原业务，不缓存网页身份。
    BrowserLiveReleaseQuery release_;   // 可选原会话关闭查找，默认旧行为不扩大准入。
    std::array<std::pair<std::string, std::string>, 3>
        scopes_;  // 原节目/焦点，不复活已释放旧身份。
};
}  // namespace ur::device
