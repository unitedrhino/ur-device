// 文件说明：privacyMode原物模型report／reportReply编解码与连接围栏；不发信令或直接操作媒体。
#include "ur/device/privacy_property.hpp"
#include "ur/device/envelope.hpp"
#include "ur/device/json_parse.hpp"
#include <algorithm>
#include <cJSON.h>
#include <cmath>
#include <limits>
#include <memory>
#include <utility>

namespace ur::device {
namespace {
// JsonPtr 保证每个临时解析树只由当前事件线程释放，不保存原自由错误正文。
using JsonPtr = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
// safe_integer 限制报告墙钟与JSON精确整数的共同范围。
constexpr std::uint64_t safe_integer = 9007199254740991ULL;
// route_id 拒绝分隔符、通配符和非原生身份字符，不接受正文扩张Topic。
bool route_id(std::string_view value) {
    return !value.empty() && value.size() <= 64 &&
           std::all_of(value.begin(), value.end(), [](unsigned char byte) {
               return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                      (byte >= '0' && byte <= '9') || byte == '-' || byte == '_';
           });
}
// token_id 校验原请求键，非法输入不自动替换成新的报告。
bool token_id(std::string_view value) {
    return !value.empty() && value.size() <= 128 &&
           std::none_of(value.begin(), value.end(),
                        [](unsigned char byte) { return byte < 32 || byte == 127; });
}
// encode_report
// 只编码已固定的privacyMode数值，原墙钟精确保留，失败不留下半份正文。
bool encode_report(const PrivacyReport &report, std::string &output) {
    if (!token_id(report.token) || report.timestamp == 0 || report.timestamp > safe_integer)
        return false;
    JsonPtr root(cJSON_CreateObject(), cJSON_Delete);
    JsonPtr params(cJSON_CreateObject(), cJSON_Delete);
    const auto stamp = std::to_string(report.timestamp);  // 原墙钟字面量，不经浮点打印舍入。
    JsonPtr time(cJSON_CreateRaw(stamp.c_str()), cJSON_Delete);
    if (!root || !params || !time || !cJSON_AddStringToObject(root.get(), "method", "report") ||
        !cJSON_AddStringToObject(root.get(), "msgToken", report.token.c_str()) ||
        !cJSON_AddNumberToObject(params.get(), "privacyMode", report.enabled ? 1 : 0) ||
        !cJSON_AddItemToObject(root.get(), "timestamp", time.get()))
        return false;
    // 根对象接管墙钟字面量，返回指针不再拥有对象；失败仍由临时所有者收敛。
    [[maybe_unused]] auto* transferred_time = time.release();
    if (!cJSON_AddItemToObject(root.get(), "params", params.get()))
        return false;
    // 根对象接管params，返回指针仅记录转交，不再次释放。
    [[maybe_unused]] auto* transferred_params = params.release();
    auto *bytes = cJSON_PrintUnformatted(root.get());  // 原编码器生成正文，不拼接外部JSON。
    if (!bytes)
        return false;
    const std::string raw(bytes);  // 仅本地编码暂存，不输出日志或透传网页。
    cJSON_free(bytes);
    return canonical_json_object(raw, output) == DecodeResult::Ok;
}
// decode_reply
// 严格读取当前平台CommonMsg，只确认属性处理，不能解析为会话或媒体完成。
bool decode_reply(std::string_view body, std::string &token, int &code) {
    std::string canonical;  // 公共解析器拒绝重复键、非法Unicode、深度和长度越界。
    if (canonical_json_object(body, canonical) != DecodeResult::Ok)
        return false;
    JsonPtr root(parse_json(canonical.c_str()), cJSON_Delete);
    if (!root)
        return false;
    for (auto *field = root->child; field; field = field->next) {
        const std::string_view name(field->string);  // 字段名已由公共解析器完成合法性校验。
        if (name != "method" && name != "msgToken" && name != "code" && name != "msg" &&
            name != "timestamp" && name != "data")
            return false;
    }
    const auto *method = cJSON_GetObjectItemCaseSensitive(root.get(), "method");
    const auto *key = cJSON_GetObjectItemCaseSensitive(root.get(), "msgToken");
    const auto *status = cJSON_GetObjectItemCaseSensitive(root.get(), "code");
    const auto *message = cJSON_GetObjectItemCaseSensitive(root.get(), "msg");
    const auto *stamp = cJSON_GetObjectItemCaseSensitive(root.get(), "timestamp");
    if (!cJSON_IsString(method) || std::string_view(method->valuestring) != "reportReply" ||
        !cJSON_IsString(key) || !token_id(key->valuestring) || !cJSON_IsNumber(status) ||
        status->valuedouble < 1 || status->valuedouble > std::numeric_limits<int>::max() ||
        std::floor(status->valuedouble) != status->valuedouble ||
        (message &&
         (!cJSON_IsString(message) || std::string_view(message->valuestring).size() > 256)) ||
        (stamp && (!cJSON_IsNumber(stamp) || stamp->valuedouble < 0 ||
                   stamp->valuedouble > static_cast<double>(safe_integer) ||
                   std::floor(stamp->valuedouble) != stamp->valuedouble)))
        return false;
    token = key->valuestring;
    code = static_cast<int>(status->valuedouble);
    return true;
}
}  // namespace

// PrivacyProperty 只构造原设备路由，不注册物模型、读取密钥或打开任何媒体。
PrivacyProperty::PrivacyProperty(PublishPort &mqtt, PrivacyPropertyConfig config)
    : requests_(mqtt) {
    enabled_ = config.enabled && route_id(config.product_id) && route_id(config.device_name);
    if (enabled_) {
        up_ = "$thing/up/property/" + config.product_id + "/" + config.device_name;
        down_ = "$thing/down/property/" + config.product_id + "/" + config.device_name;
    }
}

// observe 在最后有效时间退休回退连接，不用墙钟重置重试期限。
bool PrivacyProperty::observe(std::uint64_t now) {
    if (clock_ && now < *clock_) {
        disconnect(*clock_);
        return false;
    }
    clock_ = now;
    return true;
}

// begin_connection 不让重复SUBACK取消当前报告，新的真实连接使用独立本地作用域。
bool PrivacyProperty::begin_connection(std::uint64_t now) {
    if (!enabled_ || !observe(now) || connected_ ||
        generation_ == std::numeric_limits<std::uint64_t>::max())
        return false;
    connection_ = "privacy-" + std::to_string(++generation_);
    connected_ = true;
    return true;
}

// submit 先核原意图后交原请求核心保留字节，不为超时或背压生成新token。
SubmitResult PrivacyProperty::submit(const PrivacyReport &report, std::uint64_t now) {
    if (!enabled_ || !observe(now) || !connected_)
        return SubmitResult::Invalid;
    std::string body;  // 仅当前原报告的规范编码，重试后由Requests保留。
    if (!encode_report(report, body))
        return SubmitResult::Invalid;
    if (!current_.empty() && current_ != report.token && state(current_) == RequestState::Pending)
        return SubmitResult::Full;
    const auto result =
        requests_.submit({report.token, connection_, "report", up_, std::move(body)}, now);
    if (result == SubmitResult::Accepted)
        current_ = report.token;
    return result;
}

// receive 不把其它属性控制或会话终态混入报告关联；系统错误码统一映射本地失败。
// 保留既有topic/body接口；原topic身份与body请求关联各自严格校验，交换参数仍拒绝。
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
bool PrivacyProperty::receive(std::string_view topic, std::string_view body, std::uint64_t now) {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    if (!enabled_ || topic != down_ || !observe(now) || !connected_)
        return false;
    std::string token;  // 仅用于查原请求，不向网页或日志公开。
    int code = 0;       // 平台真实业务码，不假设仅使用HTTP状态码。
    if (!decode_reply(body, token, code))
        return false;
    return requests_.acknowledge({token, connection_, "report", code == 200 ? 200 : 500, up_}, now);
}

// tick 断连等待已经取消，只回收过期墓碑，绝不自动解除隐私或重新准入媒体。
void PrivacyProperty::tick(std::uint64_t now) {
    if (observe(now))
        requests_.tick(now);
}

// disconnect 重复关闭不改变成功事实，迟到回执不能推进已退休连接。
void PrivacyProperty::disconnect(std::uint64_t now) {
    const auto final_now = clock_ && now < *clock_ ? *clock_ : now;  // 取消不得回退原计时。
    if (!connection_.empty())
        requests_.cancel_session(connection_, final_now);
    connected_ = false;
    clock_ = final_now;
}

// state 只查询原物模型report记录，不恢复摄像头或暗示平台媒体授权。
RequestState PrivacyProperty::state(std::string_view token) const {
    return requests_.state({up_, "report", token});
}
}  // namespace ur::device
