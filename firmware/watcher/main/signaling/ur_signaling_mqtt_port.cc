#include "ur_signaling_mqtt_port.h"

#include <esp_log.h>

#include <string_view>

#include "ur/device/connect_trace.hpp"
#include "ur_device.h"

namespace ur::device {
namespace {
constexpr const char* kTag = "ur_sig_mqtt";
constexpr const char* kConnectTag = "ur_conn_mqtt";
}  // namespace

bool UrSignalingMqttPort::connect(const MqttConfig& config) {
    /* 连接由联犀设备任务维护；这里只透传身份用于日志对账，不保存凭据。 */
    ESP_LOGI(kTag, "attach shared device mqtt identity=%s", config.client_id.c_str());
    /* 排空启动前的陈旧事件，避免历史 Connected/Disconnected 误杀新会话。 */
    ur_signaling_event_t stale;
    while (ur_signaling_take_event(&stale)) {
        ur_signaling_free_event(&stale);
    }
    while (!pending_subscribes_.empty()) {
        pending_subscribes_.pop_front();
    }
    /* Connected 事件只在 MQTT 重连时推送；共享连接长期在线时按需补投，
     * 否则 runtime 卡在 Connecting 等待永不出现的事件（真机会话建立超时根因）。 */
    if (!ur_signaling_inject_connected()) {
        ESP_LOGW(kTag, "shared device mqtt offline; runtime waits for reconnect event");
    }
    return true;
}

int UrSignalingMqttPort::subscribe(std::string_view topic) {
    std::string topic_copy(topic);
    if (!ur_signaling_subscribe(topic_copy.c_str())) {
        ESP_LOGW(kConnectTag, "[connect-ms] stage=subscribe_failed t=%llu topic=%.*s",
                 UR_CONNECT_MS(), static_cast<int>(topic.size()), topic.data());
        return 0;
    }
    /* 核心要求每个订阅拿到唯一非零 MID；SUBACK 事件按提交顺序关联。 */
    next_mid_ = next_mid_ >= 0x7FFF ? 1 : next_mid_ + 1;
    int assigned = next_mid_;
    if (!pending_subscribes_.push_back(std::move(assigned))) {
        return 0;
    }
    UR_CONNECT_TRACE(kConnectTag, "stage=subscribe_sent mid=%d t=%llu topic=%.*s", next_mid_,
                     UR_CONNECT_MS(), static_cast<int>(topic.size()), topic.data());
    return next_mid_;
}

bool UrSignalingMqttPort::poll() {
    /* 网络推进在联犀 MQTT 任务；这里仅确认队列仍在服务。 */
    return true;
}

std::optional<MqttEvent> UrSignalingMqttPort::take_event() {
    ur_signaling_event_t event;
    if (!ur_signaling_take_event(&event)) {
        return std::nullopt;
    }
    MqttEvent mapped;
    switch (event.kind) {
        case UR_SIGNALING_EVENT_CONNECTED:
            mapped.kind = MqttEvent::Kind::Connected;
            break;
        case UR_SIGNALING_EVENT_DISCONNECTED:
            mapped.kind = MqttEvent::Kind::Disconnected;
            break;
        case UR_SIGNALING_EVENT_SUBSCRIBED:
            mapped.kind = MqttEvent::Kind::Subscribed;
            break;
        case UR_SIGNALING_EVENT_PUBLISHED:
            mapped.kind = MqttEvent::Kind::Published;
            break;
        case UR_SIGNALING_EVENT_MESSAGE:
            mapped.kind = MqttEvent::Kind::Message;
            break;
        case UR_SIGNALING_EVENT_FAILED:
            mapped.kind = MqttEvent::Kind::Failed;
            break;
        default:
            ur_signaling_free_event(&event);
            return std::nullopt;
    }
    mapped.code = event.code;
    mapped.topic = event.topic;
    if (event.payload != nullptr && event.length > 0) {
        /* 调试日志：信令事件低频全量，消息流高频截断，避免 UART 阻塞拖垮 MQTT。 */
        if (mapped.kind == MqttEvent::Kind::Message) {
            const bool signaling = mapped.topic.find("/signaling/") != std::string_view::npos;
            const unsigned limit = signaling ? 900 : 60;
            const int shown = static_cast<int>(
                event.length < limit ? event.length : limit);
            ESP_LOGI(kTag, "down event topic=%.*s len=%u body=%.*s",
                     static_cast<int>(mapped.topic.size()), mapped.topic.data(),
                     static_cast<unsigned>(event.length), shown,
                     static_cast<const char*>(event.payload));
        }
        mapped.body.assign(event.payload, event.length);
    }
    if (mapped.kind == MqttEvent::Kind::Subscribed) {
        /* MQTT 任务按命令队列顺序执行订阅，SUBACK 按同序回投；取队首关联 MID。 */
        int mid = 0;
        const int* head = pending_subscribes_.front();
        if (head != nullptr) {
            mid = *head;
            pending_subscribes_.pop_front();
        }
        mapped.operation = mid;
        UR_CONNECT_TRACE(kConnectTag, "stage=suback mid=%d code=%d t=%llu", mid, mapped.code,
                         UR_CONNECT_MS());
    } else if (mapped.kind == MqttEvent::Kind::Published) {
        mapped.operation = event.packet_id;
        if (mapped.code == 0 && outstanding_publishes_.load() > 0) {
            outstanding_publishes_.fetch_sub(1);
        }
    }
    if (mapped.kind == MqttEvent::Kind::Failed || mapped.kind == MqttEvent::Kind::Disconnected) {
        ESP_LOGW(kTag, "mqtt event kind=%d code=%d", static_cast<int>(mapped.kind), mapped.code);
    }
    ur_signaling_free_event(&event);
    return mapped;
}

bool UrSignalingMqttPort::publications_drained() {
    return outstanding_publishes_.load() == 0;
}

void UrSignalingMqttPort::disconnect() {
    /* 共享连接供物模型/OTA 继续使用；只清空在途状态等待下一会话。 */
    outstanding_publishes_.store(0);
    pending_subscribes_.clear();
}

bool UrSignalingMqttPort::publish(std::string_view topic, std::string_view body) {
    std::string topic_copy(topic);
    std::string body_copy(body);
    if (!ur_signaling_publish(topic_copy.c_str(), body_copy.data(), body_copy.size())) {
        return false;
    }
    outstanding_publishes_.fetch_add(1);
    return true;
}

}  // namespace ur::device
