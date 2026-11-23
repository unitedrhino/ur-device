// libmosquitto 桌面 MQTT 适配；Linux/Windows 使用同一库接口。
#pragma once
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include "thread_progress.hpp"
#include "ur/device/fixed_queue.hpp"
#include "ur/device/mqtt_port.hpp"
struct mosquitto;
struct mosquitto_message;

namespace ur::device {
// MosquittoPort 保留库异步建连，自持唯一网络线程；回调仅向有界队列交接，业务由单事件线程推进。
class MosquittoPort final : public MqttPort {
public:
    // progress仅显式装配只读原子观察；空值默认关闭，不创建线程或改变协议。
    explicit MosquittoPort(std::shared_ptr<ThreadProgress> progress = {})
        : progress_(std::move(progress)) {}
    ~MosquittoPort() override;
    MosquittoPort(const MosquittoPort&) = delete;
    MosquittoPort& operator=(const MosquittoPort&) = delete;
    bool connect(const MqttConfig& config) override;
    bool publish(std::string_view topic, std::string_view body) override;
    int subscribe(std::string_view topic) override;
    bool poll() override;
    std::optional<MqttEvent> take_event() override;
    // publications_drained 等事件线程实际消费全部PUBACK，不以publish排队成功替代。
    bool publications_drained() override;
    void disconnect() override;

private:
    // enqueue 必须持 mutex_；对不可丢控制消息实施有界背压，溢出进入失败态。
    void enqueue(MqttEvent event);
    // C 回调只投递类型化事件，不解析 AI 报文或执行设备动作。
    static void on_connect(mosquitto*, void*, int code);
    static void on_disconnect(mosquitto*, void*, int code);
    static void on_publish(mosquitto*, void*, int operation);
    static void on_subscribe(mosquitto*, void*, int operation, int count, const int* qos);
    static void on_message(mosquitto*, void*, const mosquitto_message* message);
    mosquitto* client_ = nullptr;  // 当前驱动拥有的连接。
    std::mutex mutex_;             // 库回调与设备事件线程之间的队列/状态锁。
    std::thread loop_thread_;      // 主线程持有唯一网络线程；自然退出后仍须join，再销毁库对象。
    std::chrono::steady_clock::time_point connect_deadline_;  // 主线程单调连接截止。
    FixedQueue<MqttEvent, 128> events_;         // 固定事件数及正文上限，禁止无限队列。
    bool connected_ = false;                    // 仅 CONNACK 成功时为真。
    bool failed_ = false;                       // 故障后不静默继续业务。
    bool overflow_ = false;                     // 队列溢出单独向调用者报告一次。
    unsigned pending_publish_ = 0;              // 已接受但业务线程尚未消费 PUBACK 的条数，最多 32。
    std::shared_ptr<ThreadProgress> progress_;  // 构造后不替换，回调结束前一直持有。
};
}  // namespace ur::device
