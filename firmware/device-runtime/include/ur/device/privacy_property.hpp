// 文件说明：原物模型privacyMode上报；复用有界请求核心，不管理摄像头或统一会话权限。
#pragma once
#include "ur/device/requests.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ur::device {
// PrivacyPropertyConfig
// 必须由宿主核对已注册物模型并显式启用，设备身份来自原鉴权连接。
struct PrivacyPropertyConfig {
    bool enabled = false;     // 缺省关闭，不能因收到浏览器意图隐式启用。
    std::string product_id;   // 固定原产品标识，不从报告正文提取。
    std::string device_name;  // 固定原设备标识，不接受跨设备路由。
};
// PrivacyReport
// 保存原报告意图；相同token重试必须保留时间与数值，false不恢复旧媒体。
struct PrivacyReport {
    std::string token;            // 当前连接唯一请求键，不由组件自动生成或替换。
    std::uint64_t timestamp = 0;  // 原墙钟毫秒，正数且满足JSON安全整数范围。
    bool enabled = false;         // 原物模型bool编码为0／1，不自报资源、用户或会话。
};
// PrivacyProperty
// 由唯一设备事件线程驱动；属性ACK只确认云端处理，不证明实际采集停止。
class PrivacyProperty {
public:
    // PrivacyProperty 借用原QoS1发布端口，非法或未启用配置不能创建可用出口。
    PrivacyProperty(PublishPort &mqtt, PrivacyPropertyConfig config);
    PrivacyProperty(const PrivacyProperty &) = delete;
    PrivacyProperty &operator=(const PrivacyProperty &) = delete;
    // enabled 返回静态配置是否可用，不表示连接、物模型注册或平台权限已确认。
    bool enabled() const { return enabled_; }
    // begin_connection 仅在原鉴权连接及属性下行SUBACK成功后准入；重复就绪不换代。
    bool begin_connection(std::uint64_t now);
    // submit
    // 编码固定privacyMode原意图；Accepted仅表示本地保留，不能投影为隐私停流完成。
    SubmitResult submit(const PrivacyReport &report, std::uint64_t now);
    // receive
    // 只接受固定下行reportReply及原token，平台业务错误映射为本地失败而非未知超时。
    bool receive(std::string_view topic, std::string_view body, std::uint64_t now);
    // tick 使用同一单调毫秒时钟推进10／20秒原字节重试及30秒结果未知。
    void tick(std::uint64_t now);
    // disconnect 退休原连接所有等待，不补发false、不自动解除隐私或恢复媒体。
    void disconnect(std::uint64_t now);
    // state 查询原报告保留事实；Succeeded不能替代媒体HAL停止票据。
    RequestState state(std::string_view token) const;
    // reply_topic 返回宿主应精确订阅的原属性路由，配置关闭时为空。
    std::string_view reply_topic() const { return down_; }

private:
    // observe 对单调时钟回退立即关闭准入，原等待在最后有效时间取消。
    bool observe(std::uint64_t now);
    Requests requests_;  // 原在途／近期终态容量与关联，不复制重试状态机。
    std::string up_, down_,
        connection_;                      // 原物模型路由及本地连接作用域，不含凭据。
    std::string current_;                 // 一次只受理一个在途隐私值，避免前值重试与新值并行覆盖。
    std::optional<std::uint64_t> clock_;  // 唯一宿主单调时钟，墙钟仅进入报告正文。
    std::uint64_t generation_ = 0;        // 本地连接代次，溢出拒绝而不复用旧作用域。
    bool enabled_ = false;                // 静态配置的严格校验结果。
    bool connected_ = false;              // 原鉴权及属性SUBACK已完成，不推导媒体授权。
};
}  // namespace ur::device
