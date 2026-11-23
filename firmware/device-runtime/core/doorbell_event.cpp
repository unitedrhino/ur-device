// 文件说明：门铃事件编码和ACK严格校验；原物模型大小写、Topic和错误码独立于统一信令。
#include "ur/device/json_parse.hpp"
#include "ur/device/doorbell_event.hpp"
#include <cJSON.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <utility>
#include "ur/device/envelope.hpp"

namespace ur::device {
namespace {
// JsonPtr 释放临时物模型解析树，不把平台自由错误或原报文写入日志。
using JsonPtr = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
// safe_integer 是墙钟毫秒和JSON数值可精确表示的共同上限。
constexpr std::uint64_t safe_integer = 9007199254740991ULL;
// route_id 拒绝Topic分隔符、通配符和空标识，不能借自报身份扩大原设备路由。
bool route_id(std::string_view value) {
    return !value.empty() && value.size() <= 64 &&
           std::all_of(value.begin(), value.end(), [](unsigned char byte) {
               return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                      (byte >= '0' && byte <= '9') || byte == '-' || byte == '_';
           });
}
// token_id 验证原关联键；不替换非法token或生成另一个业务意图。
bool token_id(std::string_view value) {
    return !value.empty() && value.size() <= 128 &&
           std::none_of(value.begin(), value.end(),
                        [](unsigned char byte) { return byte < 32 || byte == 127; });
}
// encoded_press 仅生成无参物模型eventPost，timestamp随原意图固定且不携带信令会话字段。
bool encoded_press(const DoorbellPress& press, const std::string& event, std::string& output) {
    std::string params;
    if (!token_id(press.token) || press.timestamp == 0 || press.timestamp > safe_integer ||
        canonical_json_object(press.params, params) != DecodeResult::Ok || params != "{}")
        return false;
    JsonPtr root(cJSON_CreateObject(), cJSON_Delete);
    JsonPtr arguments(cJSON_CreateObject(), cJSON_Delete);
    const auto timestamp = std::to_string(press.timestamp);
    JsonPtr time(cJSON_CreateRaw(timestamp.c_str()), cJSON_Delete);
    if (!root || !arguments || !time ||
        !cJSON_AddStringToObject(root.get(), "method", "eventPost") ||
        !cJSON_AddStringToObject(root.get(), "msgToken", press.token.c_str()) ||
        !cJSON_AddStringToObject(root.get(), "eventID", event.c_str()) ||
        !cJSON_AddItemToObject(root.get(), "timestamp", time.get()))
        return false;
    // 根对象接管精确墙钟字面量；返回指针不再拥有对象，避免默认打印四舍五入。
    [[maybe_unused]] auto* transferred_time = time.release();
    if (!cJSON_AddItemToObject(root.get(), "params", arguments.get()))
        return false;
    // 挂接后的params由根对象释放，返回指针仅记录转交；失败仍由临时所有者负责。
    [[maybe_unused]] auto* transferred_arguments = arguments.release();
    auto* bytes = cJSON_PrintUnformatted(root.get());
    if (!bytes)
        return false;
    const std::string raw(bytes);
    cJSON_free(bytes);
    return canonical_json_object(raw, output) == DecodeResult::Ok;
}
// decoded_reply 只解析原物模型ACK；CommonMsg实际可无eventID，系统错误码不是HTTP状态码。
bool decoded_reply(std::string_view body, const std::string& event, std::string& token, int& code) {
    std::string canonical;
    if (canonical_json_object(body, canonical) != DecodeResult::Ok)
        return false;
    JsonPtr root(ur::device::parse_json(canonical.c_str()), cJSON_Delete);
    if (!root)
        return false;
    for (auto* field = root->child; field; field = field->next) {
        const std::string_view key(field->string);
        if (key != "method" && key != "msgToken" && key != "code" && key != "eventID" &&
            key != "msg" && key != "data" && key != "timestamp")
            return false;
    }
    const auto* method = cJSON_GetObjectItemCaseSensitive(root.get(), "method");
    const auto* correlation = cJSON_GetObjectItemCaseSensitive(root.get(), "msgToken");
    const auto* status = cJSON_GetObjectItemCaseSensitive(root.get(), "code");
    const auto* identifier = cJSON_GetObjectItemCaseSensitive(root.get(), "eventID");
    const auto* message = cJSON_GetObjectItemCaseSensitive(root.get(), "msg");
    const auto* timestamp = cJSON_GetObjectItemCaseSensitive(root.get(), "timestamp");
    if (!cJSON_IsString(method) || std::string_view(method->valuestring) != "eventReply" ||
        !cJSON_IsString(correlation) || !token_id(correlation->valuestring) ||
        !cJSON_IsNumber(status) || status->valuedouble < 1 ||
        status->valuedouble > std::numeric_limits<int>::max() ||
        std::floor(status->valuedouble) != status->valuedouble ||
        (identifier && (!cJSON_IsString(identifier) || identifier->valuestring != event)) ||
        (message &&
         (!cJSON_IsString(message) || std::string_view(message->valuestring).size() > 256)) ||
        (timestamp && (!cJSON_IsNumber(timestamp) || timestamp->valuedouble < 0 ||
                       timestamp->valuedouble > static_cast<double>(safe_integer) ||
                       std::floor(timestamp->valuedouble) != timestamp->valuedouble)))
        return false;
    token = correlation->valuestring;
    code = static_cast<int>(status->valuedouble);
    return true;
}
}  // namespace

// DoorbellEvent 固定原鉴权设备路由；未启用或配置非法时不创建可用事件出口。
DoorbellEvent::DoorbellEvent(PublishPort& mqtt, DoorbellEventConfig config) : requests_(mqtt) {
    enabled_ = config.enabled && route_id(config.product_id) && route_id(config.device_name) &&
               route_id(config.event_id);
    if (enabled_) {
        event_ = std::move(config.event_id);
        up_ = "$thing/up/event/" + config.product_id + "/" + config.device_name;
        down_ = "$thing/down/event/" + config.product_id + "/" + config.device_name;
    }
}

// observe 拒绝倒退时间，取消事实保留在最后有效单调时间，不用墙钟补偿。
bool DoorbellEvent::observe(std::uint64_t now) {
    if (clock_ && now < *clock_) {
        disconnect(*clock_);
        return false;
    }
    clock_ = now;
    return true;
}

// begin_connection 为宿主已验证的连接分配本地作用域，重复就绪不重置原事件等待。
bool DoorbellEvent::begin_connection(std::uint64_t now) {
    if (!enabled_ || !observe(now) || generation_ == std::numeric_limits<std::uint64_t>::max())
        return false;
    if (connected_)
        return false;  // 重复就绪通知不换代，不取消仍等待的原事件。
    connection_ = "doorbell-" + std::to_string(++generation_);
    connected_ = true;
    return true;
}

// submit 先核验固定原意图，再交已有请求核心保留和发布；不会生成新的token。
SubmitResult DoorbellEvent::submit(const DoorbellPress& press, std::uint64_t now) {
    if (!enabled_ || !observe(now) || !connected_)
        return SubmitResult::Invalid;
    std::string body;
    if (!encoded_press(press, event_, body))
        return SubmitResult::Invalid;
    return requests_.submit({press.token, connection_, "eventPost", up_, std::move(body)}, now);
}

// receive 用精确下行Topic、method和原token关联，成功也只确认物模型事件。
// 保留既有topic/body接口；不同作用域在函数内分别校验，交换参数不能消耗原请求。
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
bool DoorbellEvent::receive(std::string_view topic, std::string_view body, std::uint64_t now) {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    if (!enabled_ || topic != down_ || !observe(now) || !connected_)
        return false;
    std::string token;
    int code = 0;
    if (!decoded_reply(body, event_, token, code))
        return false;
    // Requests仅接受HTTP范围的本地分类；不修改线上原错误码，也不把事件成功映射为通话建立。
    return requests_.acknowledge({token, connection_, "eventPost", code == 200 ? 200 : 500, up_},
                                 now);
}

// tick 在有界请求核心中推进原计时，取消过的连接只到期回收，不隐式重连或补发。
void DoorbellEvent::tick(std::uint64_t now) {
    if (observe(now))
        requests_.tick(now);  // 断连时已取消所有在途，tick只到期回收墓碑，不会自动恢复原连接。
}

// cancel 取消当前连接的所有本地等待，已完成事实及原token仍保留。
void DoorbellEvent::cancel(std::uint64_t now) {
    if (observe(now) && !connection_.empty())
        requests_.cancel_session(connection_, now);
}

// disconnect 使用最后有效计时隔离原连接，重复关闭不改变原请求的执行事实。
void DoorbellEvent::disconnect(std::uint64_t now) {
    const auto final_now = clock_ && now < *clock_ ? *clock_ : now;
    if (!connection_.empty())
        requests_.cancel_session(connection_, final_now);
    connected_ = false;
    clock_ = final_now;
}

// state 按原物模型路由和事件方法查询本地记录，不投影为人际通话或媒体状态。
RequestState DoorbellEvent::state(std::string_view token) const {
    return requests_.state({up_, "eventPost", token});
}
}  // namespace ur::device
