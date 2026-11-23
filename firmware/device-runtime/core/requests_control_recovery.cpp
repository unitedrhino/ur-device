// 文件说明：短控制断连只退休旧连接的在途发送，不抹除同一业务会话的请求幂等事实。
#include "ur/device/requests.hpp"

namespace ur::device {
// suspend_session 不取消远端已受理操作；保留原token，防止恢复后的计时器重放旧音频/文字输入。
void Requests::suspend_session(std::string_view session, std::uint64_t now) {
    if (session.empty())
        return;
    for (auto& entry : entries_) {
        if (entry.state != RequestState::Pending || entry.request.session != session ||
            entry.session_closed || now < entry.started)
            continue;
        entry.state = RequestState::Cancelled;
        entry.finished = now;
        // 不标session_closed：业务会话仍有效，幂等事实只能在正式结束后按原十分钟期限释放。
    }
}
}  // namespace ur::device
