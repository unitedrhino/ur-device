/* 统一信令 MQTT 端口：把联犀设备共享 MQTT 任务包装成可移植核心的 MqttPort。 */
#ifndef UR_SIGNALING_MQTT_PORT_H
#define UR_SIGNALING_MQTT_PORT_H

#include "ur/device/fixed_queue.hpp"
#include "ur/device/mqtt_port.hpp"

#include <atomic>
#include <string>

namespace ur::device {

/* UrSignalingMqttPort 由唯一信令事件线程驱动；连接生命周期归 ur_device 任务所有，
 * 本端口只负责订阅确认、QoS1 发布回执和下行报文交接。 */
class UrSignalingMqttPort final : public MqttPort {
public:
    UrSignalingMqttPort() = default;
    ~UrSignalingMqttPort() override = default;
    UrSignalingMqttPort(const UrSignalingMqttPort&) = delete;
    UrSignalingMqttPort& operator=(const UrSignalingMqttPort&) = delete;

    /* connect 复用共享设备连接；返回接受不代表已收到 CONNACK。 */
    bool connect(const MqttConfig& config) override;
    /* subscribe 提交精确主题订阅；SUBACK 以 Subscribed 事件回投并携带本地 MID。 */
    int subscribe(std::string_view topic) override;
    /* poll 仅推进本地状态；共享连接由联犀 MQTT 任务维护。 */
    bool poll() override;
    /* take_event 按到达顺序取事件；空时无值。 */
    std::optional<MqttEvent> take_event() override;
    /* publications_drained 表示全部在途 QoS1 发布均已收到 PUBACK。 */
    bool publications_drained() override;
    /* disconnect 只结束信令会话；不销毁共享的设备 MQTT 连接。 */
    void disconnect() override;

    /* publish 以 QoS1 提交信令上行；受理不代表送达。 */
    bool publish(std::string_view topic, std::string_view body) override;

private:
    /* pending_subscribes_ 保存已提交但未确认的本地 MID；MQTT 任务按序执行，
     * SUBACK 事件按同一顺序回投，取队首即可完成关联。 */
    FixedQueue<int, 8> pending_subscribes_;
    /* next_mid_ 生成本端口唯一的订阅标识；核心要求信令与内容订阅 MID 不同。 */
    int next_mid_ = 0;
    /* outstanding_publishes_ 统计已受理但未收到 PUBACK 的发布数。 */
    std::atomic<int> outstanding_publishes_{0};
};

}  // namespace ur::device

#endif /* UR_SIGNALING_MQTT_PORT_H */
