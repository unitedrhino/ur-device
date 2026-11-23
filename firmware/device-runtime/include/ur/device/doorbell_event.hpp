// 文件说明：门铃按钮仅上报原生物模型事件；确认、重试和连接隔离复用请求核心，不管理通话媒体。
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include "ur/device/requests.hpp"

namespace ur::device {
// DoorbellEventConfig 来自宿主明确配置和已鉴权设备身份；缺配置默认关闭，事件不自选被叫。
struct DoorbellEventConfig {
    bool enabled = false;     // 仅显式启用才允许发送，不能由浏览器报文启用。
    std::string product_id;   // 已鉴权产品标识，用于固定物模型Topic。
    std::string device_name;  // 已鉴权设备标识，不从事件params读取。
    std::string event_id;     // 产品物模型中已配置的门铃事件标识，不使用默认事件名。
};
// DoorbellPress 是一次固定原意图；新按压必须使用新token，重试必须保留原timestamp和params。
struct DoorbellPress {
    std::string token;            // 原物理按压关联键，最多128字节，不由本组件生成或替换。
    std::uint64_t timestamp = 0;  // 原按压墙钟毫秒，正数且不超过JSON安全整数上限。
    std::string params = "{}";    // 本阶段只支持无参门铃物模型，禁止自报用户/相机/媒体地址。
};
// DoorbellEvent 单事件线程驱动，共用宿主MQTT发布端口；不订阅、不重连、不启动音视频。
class DoorbellEvent {
public:
    // DoorbellEvent 固定鉴权Topic和事件；非法或未启用配置只能关闭，不能静默猜测默认值。
    DoorbellEvent(PublishPort& mqtt, DoorbellEventConfig config);
    DoorbellEvent(const DoorbellEvent&) = delete;
    DoorbellEvent& operator=(const DoorbellEvent&) = delete;
    // enabled 返回静态配置是否可用，不代表MQTT连接、事件确认或门铃通话已建立。
    bool enabled() const { return enabled_; }
    // begin_connection 仅由宿主在原鉴权连接及事件下行SUBACK就绪后调用；不恢复上次未知事件。
    bool begin_connection(std::uint64_t now);
    // submit 使用原意图并尝试QoS1发布；Accepted只是本地保留，不代表Broker或平台业务成功。
    SubmitResult submit(const DoorbellPress& press, std::uint64_t now);
    // receive 仅接收固定下行eventReply；现平台可省略eventID，携带时必须与配置严格相同。
    bool receive(std::string_view topic, std::string_view body, std::uint64_t now);
    // tick 用单调毫秒推进原字节10/20秒重试和30秒结果未知，断连后不得继续发布。
    void tick(std::uint64_t now);
    // cancel 取消当前连接事件的本地等待；不能撤销已上报事实或声称远端未执行。
    void cancel(std::uint64_t now);
    // disconnect 取消原连接等待并隔离迟到回复；宿主仍负责释放唯一MQTT连接。
    void disconnect(std::uint64_t now);
    // state 返回保留的事件请求事实；Succeeded仅表示事件ACK，不表示振铃或媒体接通。
    RequestState state(std::string_view token) const;
    // reply_topic 返回宿主应精确订阅的下行Topic；配置不可用时为空。
    std::string_view reply_topic() const { return down_; }

private:
    // observe 验证单调时钟；回退立即取消当前等待并关闭准入，不补发未知事件。
    bool observe(std::uint64_t now);
    Requests requests_;                   // 复用已有有界关联、原字节重试和终态保留，不复制状态机。
    std::string event_, up_, down_;       // 仅保存固定原物模型事件与设备路由，不保存身份凭据。
    std::string connection_;              // 本地连接作用域，绝不写入物模型报文。
    std::uint64_t generation_ = 0;        // 单实例连接代次，溢出不能回绕为旧连接身份。
    std::optional<std::uint64_t> clock_;  // 宿主唯一单调时钟，墙钟不参与重试预算。
    bool enabled_ = false;                // 非法配置和默认缺失配置均不启用。
    bool connected_ = false;              // 原鉴权连接/SUBACK已就绪，非发布或业务ACK状态。
};
}  // namespace ur::device
