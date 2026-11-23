// 文件说明：闭集故障注入测试端口；不公开网络入口，不补造授权或正常协议可达性。
#pragma once
#include "ur/device/voice_client.hpp"

namespace ur::device {
// 专用friend只模拟正常构造/解码不可能产生的缺对象/缺字段，供防御性guard回归。
class VoiceClientOptionalTestPort {
public:
    // 先关闭原Session后移除对象；返回固定恢复分支结果，不以异常标记开启驱动。
    static Result recovering_without_session(VoiceClient& client) {
        if (client.session_)
            client.session_->close();
        client.session_.reset();
        client.state_ = VoiceState::Recovering;
        client.recovery_.disconnected_acked = true;
        client.media_reported_ = client.media_acked_ = true;
        client.want_capture_ = client.recovery_.input_acked = true;
        return client.establish();
    }
    // 直接交给原私有方法；调用者只删除已合法解码命令的可选序号，不补新字段。
    static Result vision_control(VoiceClient& client, const VoiceVisionCommand& command,
                                 std::uint64_t now) {
        return client.vision_control(command, now);
    }
    // 只读原SIG水位，核异常未消费序号；不提供设置水位的端口。
    static std::uint64_t event_sequence(const VoiceClient& client) {
        return client.event_seq_;
    }
};
}  // namespace ur::device
