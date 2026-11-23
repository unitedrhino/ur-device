// MQTT 原生适配：TLS 校验证书，QoS1，禁 retained，连接结束后必须重新协商。
#include "mosquitto_port.hpp"
#include <mosquitto.h>
#include <utility>

namespace ur::device {
namespace {
// Library 在进程中初始化一次，最后释放；不会为每个连接重复改变全局库状态。
struct Library {
    const int status = mosquitto_lib_init();  // 全局初始化的真实结果。
    ~Library() {
        if (status == MOSQ_ERR_SUCCESS)
            mosquitto_lib_cleanup();
    }
};
// valid_topic 拒绝通配订阅及 NUL，设备不得订阅其他设备的泛化 Topic。
bool valid_topic(std::string_view topic) {
    return !topic.empty() && topic.size() <= 512 &&
           topic.find_first_of("+#") == std::string_view::npos &&
           topic.find('\0') == std::string_view::npos;
}
}  // namespace
MosquittoPort::~MosquittoPort() { disconnect(); }
bool MosquittoPort::connect(const MqttConfig& config) {
    disconnect();
    events_.clear();
    failed_ = overflow_ = false;
    static const Library library;
    if (library.status != MOSQ_ERR_SUCCESS || config.host.empty() || config.client_id.empty() ||
        config.port == 0 || config.connect_timeout_ms == 0 || config.connect_timeout_ms > 30000 ||
        config.host.find('\0') != std::string::npos ||
        config.client_id.find('\0') != std::string::npos ||
        config.username.find('\0') != std::string::npos ||
        config.password.find('\0') != std::string::npos ||
        config.ca_file.find('\0') != std::string::npos)
        return false;
    if (config.ca_file.empty() && config.host != "127.0.0.1" && config.host != "::1")
        return false;
    client_ = mosquitto_new(config.client_id.c_str(), true, this);
    if (!client_)
        return false;
    mosquitto_connect_callback_set(client_, on_connect);
    mosquitto_disconnect_callback_set(client_, on_disconnect);
    mosquitto_publish_callback_set(client_, on_publish);
    mosquitto_subscribe_callback_set(client_, on_subscribe);
    mosquitto_message_callback_set(client_, on_message);
    bool configured = mosquitto_max_inflight_messages_set(client_, 32) == MOSQ_ERR_SUCCESS;
    if (!config.username.empty())
        configured =
            configured && mosquitto_username_pw_set(client_, config.username.c_str(),
                                                    config.password.c_str()) == MOSQ_ERR_SUCCESS;
    if (!config.ca_file.empty())
        configured = configured && mosquitto_tls_set(client_, config.ca_file.c_str(), nullptr,
                                                     nullptr, nullptr, nullptr) == MOSQ_ERR_SUCCESS;
    connect_deadline_ =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(config.connect_timeout_ms);
    if (!configured ||
        mosquitto_connect_async(client_, config.host.c_str(), config.port, 30) !=
            MOSQ_ERR_SUCCESS ||
        mosquitto_threaded_set(client_, true) != MOSQ_ERR_SUCCESS) {
        disconnect();
        return false;
    }
    try {
        // 原async成功后才启动唯一网络线程，保留keepalive30秒的原loop参数和库多线程锁。
        // 自持线程句柄不受库自然退出时清空threaded标志影响，关闭必须建立实际join同步。
        loop_thread_ =
            std::thread([client = client_] { (void)mosquitto_loop_forever(client, 30000, 1); });
    } catch (...) {
        // 线程创建失败尚无joinable句柄；归还原连接与队列，公开connect仍只返回false。
        disconnect();
        return false;
    }
    return true;
}
// publications_drained 仅读取受锁保护的网络事实；断连或失败不声称消息已送到Broker。
bool MosquittoPort::publications_drained() {
    std::lock_guard<std::mutex> lock(mutex_);
    return connected_ && !failed_ && pending_publish_ == 0;
}

bool MosquittoPort::publish(std::string_view topic, std::string_view body) {
    if (!client_ || !valid_topic(topic) || body.empty() || body.size() > 65536)
        return false;
    const std::string owned(topic);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!connected_ || failed_ || pending_publish_ >= 32)
            return false;
        ++pending_publish_;
    }
    if (mosquitto_publish(client_, nullptr, owned.c_str(), static_cast<int>(body.size()),
                          body.data(), 1, false) != MOSQ_ERR_SUCCESS) {
        std::lock_guard<std::mutex> lock(mutex_);
        --pending_publish_;
        return false;
    }
    return true;
}
int MosquittoPort::subscribe(std::string_view topic) {
    if (!client_ || !valid_topic(topic))
        return 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!connected_ || failed_)
            return 0;
    }
    int operation = 0;
    const std::string owned(topic);
    return mosquitto_subscribe(client_, &operation, owned.c_str(), 1) == MOSQ_ERR_SUCCESS
               ? operation
               : 0;
}
bool MosquittoPort::poll() {
    if (progress_)
        progress_->mqtt_poll_entered.increment();
    ProgressExit observed_exit(progress_ ? &progress_->mqtt_poll_exited : nullptr);
    std::lock_guard<std::mutex> lock(mutex_);
    if (!client_ || failed_)
        return false;
    if (!connected_ && std::chrono::steady_clock::now() >= connect_deadline_) {
        failed_ = true;
        enqueue({MqttEvent::Kind::Failed, 0, MOSQ_ERR_TIMEOUT, {}, {}});
    }
    return !failed_;
}
void MosquittoPort::enqueue(MqttEvent event) {
    // Message仅由网络回调入队；其它事件也可能来自宿主poll，不能混写同一计数槽。
    const bool message = event.kind == MqttEvent::Kind::Message;
    const auto route = message ? ThreadProgress::route(event.topic) : ProgressRoute::Other;
    if (!events_.push_back(std::move(event))) {
        if (message && progress_)
            progress_->enqueue_overflow.increment();
        overflow_ = failed_ = true;
        connected_ = false;
    } else if (message && progress_)
        progress_->for_route(route).enqueued.increment();
}
std::optional<MqttEvent> MosquittoPort::take_event() {
    if (progress_)
        progress_->take_entered.increment();
    ProgressExit observed_exit(progress_ ? &progress_->take_exited : nullptr);
    std::lock_guard<std::mutex> lock(mutex_);
    if (overflow_) {
        overflow_ = false;
        events_.clear();
        return MqttEvent{MqttEvent::Kind::Failed, 0, MOSQ_ERR_NOMEM, {}, {}};
    }
    auto* next = events_.front();
    if (!next)
        return std::nullopt;
    auto event = std::move(*next);
    events_.pop_front();
    if (event.kind == MqttEvent::Kind::Message && progress_)
        progress_->for_route(ThreadProgress::route(event.topic)).taken.increment();
    // 直到业务线程取到 PUBACK 事件才释放容量，避免它不消费事件却无限堆积确认。
    if (event.kind == MqttEvent::Kind::Published && pending_publish_ > 0)
        --pending_publish_;
    return event;
}
void MosquittoPort::disconnect() {
    // 不持状态锁等待网络线程；否则其 disconnect 回调无法取锁而相互等待。
    if (client_) {
        mosquitto_disconnect(client_);
        // 原disconnect唤醒同一库循环；即使网络线程已自然退出，仍join后才能销毁内部互斥量。
        // 不强制cancel、不持状态锁等待，也不依赖库threaded标志猜测线程已经回收。
        if (loop_thread_.joinable())
            loop_thread_.join();
        mosquitto_destroy(client_);
    }
    client_ = nullptr;
    std::lock_guard<std::mutex> lock(mutex_);
    connected_ = false;
    pending_publish_ = 0;
    events_.clear();
}
void MosquittoPort::on_connect(mosquitto* client, void* context, int code) {
    auto& self = *static_cast<MosquittoPort*>(context);
    std::lock_guard<std::mutex> lock(self.mutex_);
    if (self.failed_) {
        mosquitto_disconnect(client);
        return;
    }
    self.connected_ = code == 0;
    self.failed_ = code != 0;
    self.enqueue(
        {code == 0 ? MqttEvent::Kind::Connected : MqttEvent::Kind::Failed, 0, code, {}, {}});
}
void MosquittoPort::on_disconnect(mosquitto* client, void* context, int code) {
    auto& self = *static_cast<MosquittoPort*>(context);
    std::lock_guard<std::mutex> lock(self.mutex_);
    self.connected_ = false;
    self.failed_ = true;
    self.enqueue({MqttEvent::Kind::Disconnected, 0, code, {}, {}});
    // 禁止库自行恢复已失效连接；重连必须由设备重新协商能力和媒体。
    if (code != 0)
        mosquitto_disconnect(client);
}
void MosquittoPort::on_publish(mosquitto*, void* context, int operation) {
    auto& self = *static_cast<MosquittoPort*>(context);
    std::lock_guard<std::mutex> lock(self.mutex_);
    self.enqueue({MqttEvent::Kind::Published, operation, 0, {}, {}});
}
// libmosquitto 的 C 回调 ABI 固定相邻 int 参数，不能改为业务结构体。
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void MosquittoPort::on_subscribe(mosquitto*, void* context, int operation, int count,
                                 const int* qos) {
    auto& self = *static_cast<MosquittoPort*>(context);
    std::lock_guard<std::mutex> lock(self.mutex_);
    const bool success = count == 1 && qos && qos[0] == 1;
    if (!success)
        self.failed_ = true;
    self.enqueue({success ? MqttEvent::Kind::Subscribed : MqttEvent::Kind::Failed,
                  operation,
                  success ? 0 : 128,
                  {},
                  {}});
}
void MosquittoPort::on_message(mosquitto*, void* context, const mosquitto_message* message) {
    auto& self = *static_cast<MosquittoPort*>(context);
    // 入口在业务互斥锁前可见，HTTP只读独立原子，不等待此锁或回调完成。
    if (self.progress_) {
        self.progress_->callbacks_entered.increment();
        self.progress_->callback(CallbackStage::Entered);
    }
    ProgressExit observed_exit(self.progress_ ? &self.progress_->callbacks_exited : nullptr,
                               self.progress_ ? &self.progress_->callback_stage : nullptr,
                               static_cast<std::uint32_t>(CallbackStage::Exited));
    std::lock_guard<std::mutex> lock(self.mutex_);
    if (self.progress_) {
        self.progress_->callbacks_locked.increment();
        self.progress_->callback(CallbackStage::Locked);
    }
    if (!message || !message->topic || !valid_topic(message->topic) || message->payloadlen <= 0 ||
        message->payloadlen > 65536 || !message->payload || message->retain) {
        if (self.progress_) {
            self.progress_->rejected.increment();
            self.progress_->callback(CallbackStage::Rejected);
        }
        self.failed_ = true;
        self.enqueue({MqttEvent::Kind::Failed, 0, MOSQ_ERR_PROTOCOL, {}, {}});
        return;
    }
    if (self.progress_)
        self.progress_->for_route(ThreadProgress::route(message->topic)).valid.increment();
    self.enqueue({MqttEvent::Kind::Message, message->mid, 0, message->topic,
                  std::string(static_cast<const char*>(message->payload),
                              static_cast<std::size_t>(message->payloadlen))});
    if (self.progress_)
        self.progress_->callback(CallbackStage::Enqueued);
}
}  // namespace ur::device
