// 屏幕通讯录传输实现：共享现有鉴权连接，每页四项，非法回复与超时均退回可重试状态。
#include "ur_signaling_contacts.h"
#include <cstring>
#include <memory>
#include <utility>
#include "cJSON.h"
#include "ur/device/envelope.hpp"
#include "ur/device/mqtt_port.hpp"

namespace {
// ReadText 读取有界UTF-8展示文字；公共信封已检查Unicode，长度按字节限制。
bool ReadText(const cJSON* object, const char* key, std::string& out, size_t maximum,
              bool empty = false) {
    const auto* value =
        cJSON_GetObjectItemCaseSensitive(object, key);  // 精确字段，避免大小写歧义。
    if (!cJSON_IsString(value) || std::strlen(value->valuestring) > maximum ||
        (!empty && value->valuestring[0] == '\0'))
        return false;
    out = value->valuestring;
    return true;
}
}  // namespace

UrSignalingContacts& UrSignalingContacts::Instance() {
    static UrSignalingContacts contacts;  // 生命周期覆盖板级显示和信令任务。
    return contacts;
}

void UrSignalingContacts::SetCallback(std::function<void(UrContactsResult)> callback) {
    std::lock_guard<std::mutex> lock(mutex_);  // 注册不访问网络或页面对象。
    callback_ = std::move(callback);
}

bool UrSignalingContacts::Query(uint32_t revision, uint32_t offset) {
    std::lock_guard<std::mutex> lock(mutex_);  // 按值保存，调用者不需要保留页面内存。
    if (!ready_ || offset > 10000 || offset % 4 != 0)
        return false;
    queued_ = Request{revision, offset};
    return true;
}

void UrSignalingContacts::Attach(ur::device::MqttPort* port, std::string identity,
                                 std::string instance) {
    port_ = port;
    identity_ = std::move(identity);
    instance_ = std::move(instance);
    std::lock_guard<std::mutex> lock(mutex_);  // 最后开放入口，页面看不到半装配宿主。
    ready_ = port_ != nullptr && !identity_.empty() && !instance_.empty();
}

void UrSignalingContacts::Detach() {
    std::optional<Request> queued;  // 尚未发送的页面也必须收到失败结果，避免永远停留加载中。
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ready_ = false;
        queued.swap(queued_);
    }
    Finish(false);
    if (queued) {
        in_flight_ = queued;
        Finish(false);
    }
    port_ = nullptr;
    identity_.clear();
}

void UrSignalingContacts::Finish(bool ok, std::vector<UrCallContact> contacts, bool more) {
    if (!in_flight_)
        return;
    UrContactsResult result{in_flight_->revision, in_flight_->offset, ok, more,
                            std::move(contacts)};  // 结果绑定原页面，不能用当前页面代次替代。
    in_flight_.reset();
    token_.clear();
    std::function<void(UrContactsResult)> callback;  // 锁外调用，接收者可安全投递下一次查询。
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callback = callback_;
    }
    if (callback)
        callback(std::move(result));
}

void UrSignalingContacts::Poll(uint64_t now) {
    std::optional<Request> request;  // 每轮最多消费一个最新请求，不在互斥区内发布。
    {
        std::lock_guard<std::mutex> lock(mutex_);
        request.swap(queued_);
    }
    if (request) {
        in_flight_ = request;
        token_ = instance_ + "-contacts-" + std::to_string(++sequence_);
        sent_at_ = now;
        ur::device::Envelope envelope;  // 无会话只读请求，不携带自报用户和企业。
        envelope.token = token_;
        envelope.method = "callContactsGet";
        envelope.payload = "{\"offset\":" + std::to_string(request->offset) + ",\"limit\":4}";
        std::string body;  // 编码失败和离线发布失败均通知原页面重试。
        if (!port_ || ur::device::encode_envelope(envelope, body) != ur::device::DecodeResult::Ok ||
            !port_->publish("$thing/up/signaling/" + identity_, body))
            Finish(false);
    }
    if (in_flight_ && now - sent_at_ >= 10000)
        Finish(false);
}

bool UrSignalingContacts::Handle(std::string_view topic, std::string_view body) {
    if (!in_flight_ || topic != "$thing/down/signaling/" + identity_ ||
        body.find(token_) == std::string_view::npos)
        return false;
    ur::device::Envelope envelope;  // 公共编解码器先拒绝重复键和非法信封。
    if (ur::device::decode_envelope(body, envelope) != ur::device::DecodeResult::Ok ||
        envelope.token != token_)
        return false;
    if (envelope.request || envelope.method != "requestAck" || envelope.code != 200 ||
        !envelope.session.empty()) {
        Finish(false);
        return true;
    }
    const std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(
        cJSON_Parse(envelope.payload.c_str()), cJSON_Delete);  // 借用节点只在本次解析期间有效。
    const auto* method =
        cJSON_GetObjectItemCaseSensitive(root.get(), "requestMethod");            // 原请求方法。
    const auto* result = cJSON_GetObjectItemCaseSensitive(root.get(), "result");  // 四项分页结果。
    const auto* offset = cJSON_GetObjectItemCaseSensitive(result, "offset");      // 原分页位置。
    const auto* more = cJSON_GetObjectItemCaseSensitive(result, "hasMore");       // 下一页标志。
    const auto* rows = cJSON_GetObjectItemCaseSensitive(result, "contacts");      // 本页设备端点。
    if (!cJSON_IsString(method) || std::strcmp(method->valuestring, "callContactsGet") != 0 ||
        !cJSON_IsObject(result) || !cJSON_IsNumber(offset) ||
        offset->valuedouble != in_flight_->offset || !cJSON_IsBool(more) || !cJSON_IsArray(rows) ||
        cJSON_GetArraySize(rows) > 4) {
        Finish(false);
        return true;
    }
    std::vector<UrCallContact> contacts;  // 严格限制为四项，不保留原JSON或额外账号字段。
    for (int index = 0; index < cJSON_GetArraySize(rows); ++index) {
        UrCallContact contact;  // 校验全部条目后才发布，拒绝部分结果冒充成功。
        const auto* row = cJSON_GetArrayItem(rows, index);
        if (!cJSON_IsObject(row) || !ReadText(row, "name", contact.name, 256) ||
            !ReadText(row, "target", contact.target, 128) ||
            !ReadText(row, "deviceName", contact.device_name, 256, true)) {
            Finish(false);
            return true;
        }
        contacts.push_back(std::move(contact));
    }
    if (cJSON_IsTrue(more) && contacts.size() != 4) {
        Finish(false);
        return true;
    }
    Finish(true, std::move(contacts), cJSON_IsTrue(more));
    return true;
}
