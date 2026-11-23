// 文件说明：严格私有上下文MSG编解码及有界接收；解析、留存、确认与主通话生命周期分离。
#include "ur/device/json_parse.hpp"
#include "ur/device/context_snapshot.hpp"
#include <cJSON.h>
#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <memory>
#include <utility>

namespace ur::device {
namespace {
// Json 管理解析临时树，失败时不向调用方交付半份私有正文。
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
// exact_keys 在公共重复键/UTF-8校验后限定精确对象字段，不接受大小写别名。
bool exact_keys(const cJSON* object, std::initializer_list<std::string_view> keys) {
    if (!cJSON_IsObject(object))
        return false;
    std::size_t count = 0;  // 原对象字段数须等于必填契约，不允许遗漏或扩权字段。
    for (const auto* item = object->child; item; item = item->next) {
        if (std::find(keys.begin(), keys.end(), std::string_view(item->string)) == keys.end())
            return false;
        ++count;
    }
    return count == keys.size();
}
// read_string 读取合法公共JSON中的有界字符串，标识拒绝控制字符，正文允许换行。
bool read_string(const cJSON* root, const char* key, std::string& out, std::size_t limit,
                 bool identifier = true) {
    const auto* value = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsString(value))
        return false;
    out = value->valuestring;
    return !out.empty() && out.size() <= limit &&
           (!identifier || std::none_of(out.begin(), out.end(),
                                        [](unsigned char c) { return c < 32 || c == 127; }));
}
// read_count 与后端JSON安全整数边界一致，不接受null、零、字符串或小数。
bool read_count(const cJSON* root, const char* key, std::uint64_t& out) {
    const auto* value = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) || value->valuedouble < 1 ||
        value->valuedouble > 9007199254740991.0 ||
        std::floor(value->valuedouble) != value->valuedouble)
        return false;
    out = static_cast<std::uint64_t>(value->valuedouble);
    return true;
}
// digest 只接纳规范小写SHA256，不以自由正文作为授权键。
bool digest(std::string_view value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](unsigned char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}
// space 匹配Go的Unicode空白集合，服务标识及正文共用精确边界。
bool space(std::uint32_t rune) {
    return (rune >= 9 && rune <= 13) || rune == 32 || rune == 0x85 || rune == 0xa0 ||
           rune == 0x1680 || (rune >= 0x2000 && rune <= 0x200a) || rune == 0x2028 ||
           rune == 0x2029 || rune == 0x202f || rune == 0x205f || rune == 0x3000;
}
// next_rune 仅解析公共入口已验证的UTF-8，推进offset后返回字符值。
std::uint32_t next_rune(std::string_view text, std::size_t& offset) {
    const auto lead = static_cast<unsigned char>(text[offset++]);  // 已验证UTF-8首字节。
    std::uint32_t rune = lead;  // 已验证完整字符值，不能把多字节空白当有效语音文本。
    unsigned trailing = 0;      // 当前字符剩余UTF-8字节数量。
    if (lead >= 0xf0) {
        rune &= 7;
        trailing = 3;
    } else if (lead >= 0xe0) {
        rune &= 15;
        trailing = 2;
    } else if (lead >= 0xc0) {
        rune &= 31;
        trailing = 1;
    }
    while (trailing--)
        rune = (rune << 6) | (static_cast<unsigned char>(text[offset++]) & 63);
    return rune;
}
// nonblank 完成文本不能只含Unicode空白，但不裁剪获授权原正文。
bool nonblank(std::string_view text) {
    for (std::size_t offset = 0; offset < text.size();) {
        if (!space(next_rune(text, offset)))
            return true;
    }
    return false;
}
// exact_service
// 对齐原目录/Consent：拒前后空白及路径/通配符，问号只作字面精确ID。
bool exact_service(std::string_view value) {
    if (value.empty() || value.find_first_of("/*+#") != std::string::npos)
        return false;
    std::size_t offset = 0;
    auto rune = next_rune(value, offset);
    if (space(rune))
        return false;
    while (offset < value.size())
        rune = next_rune(value, offset);
    return !space(rune);
}
// parse_turns
// 验证完整消息顺序、唯一ID与轮次；user可无assistant，assistant不能独立或连续。
bool parse_turns(const cJSON* array, ContextSnapshot& snapshot) {
    const int count = cJSON_GetArraySize(array);  // 单次私有留存最多64个完成消息。
    if (!cJSON_IsArray(array) || count < 1 || count > 64)
        return false;
    std::uint64_t previous = 0,
                  index = 0;  // 原消息与轮次水位，与MQTT messageSeq无关。
    unsigned users = 0;       // 最多32个授权user轮次。
    std::string role;         // assistant只允许紧跟同轮user。
    for (int n = 0; n < count; ++n) {
        const auto* item = cJSON_GetArrayItem(array, n);  // 本轮唯一完整消息对象。
        ContextSnapshotTurn turn;                         // 只在所有字段及关系通过后纳入原快照。
        if (!exact_keys(item, {"messageId", "sequence", "turnIndex", "role", "text"}) ||
            !read_string(item, "messageId", turn.message_id, 128) ||
            !read_count(item, "sequence", turn.sequence) ||
            !read_count(item, "turnIndex", turn.turn_index) ||
            !read_string(item, "role", turn.role, 9) ||
            !read_string(item, "text", turn.text, 16384, false) || !nonblank(turn.text) ||
            turn.sequence <= previous || turn.turn_index < snapshot.first_turn ||
            turn.turn_index > snapshot.last_turn ||
            std::any_of(snapshot.turns.begin(), snapshot.turns.end(),
                        [&](const auto& other) { return other.message_id == turn.message_id; }))
            return false;
        if (turn.role == "user") {
            if ((!users && turn.turn_index != snapshot.first_turn) ||
                (users && turn.turn_index != index + 1))
                return false;
            ++users;
        } else if (turn.role != "assistant" || role != "user" || turn.turn_index != index)
            return false;
        previous = turn.sequence;
        index = turn.turn_index;
        role = turn.role;
        snapshot.turns.push_back(std::move(turn));
    }
    return users > 0 && users <= 32 && index == snapshot.last_turn;
}
}  // namespace

WireResult decode_context_snapshot(WireRoute route, std::string_view body, ContextSnapshot& out) {
    out = {};
    if (route != WireRoute::Message)
        return WireResult::WrongRoute;
    ContextSnapshot snapshot;           // 私有临时值只在成功后整体提交。
    auto& message = snapshot.envelope;  // 已鉴权Topic不接受正文自报设备身份。
    if (decode_envelope(body, message) != DecodeResult::Ok || message.request || message.code ||
        message.event_sequence || !message.message_sequence || message.session.empty() ||
        message.session.size() > 64 || message.method != "contextSnapshot")
        return WireResult::Invalid;
    Json data(ur::device::parse_json(message.payload.c_str()),
              cJSON_Delete);  // 公共解析已校验重复键及深度。
    std::string scope;        // 原scope只能是completedText，不外推其他分享权限。
    if (!exact_keys(data.get(), {"deliveryId", "consentId", "scope", "sourceSessionId", "serviceId",
                                 "serviceRevision", "snapshotVersion", "snapshotDigest",
                                 "firstTurnIndex", "lastTurnIndex", "expiresAt", "turns"}) ||
        !read_string(data.get(), "deliveryId", snapshot.delivery_id, 64) ||
        !digest(snapshot.delivery_id) || message.token != snapshot.delivery_id ||
        !read_string(data.get(), "consentId", snapshot.consent_id, 64) ||
        !digest(snapshot.consent_id) ||
        !read_string(data.get(), "snapshotDigest", snapshot.snapshot_digest, 64) ||
        !digest(snapshot.snapshot_digest) || !read_string(data.get(), "scope", scope, 16) ||
        scope != "completedText" ||
        !read_string(data.get(), "sourceSessionId", snapshot.source_session_id, 64) ||
        !read_string(data.get(), "serviceId", snapshot.service_id, 128) ||
        !exact_service(snapshot.service_id) ||
        !read_string(data.get(), "serviceRevision", snapshot.service_revision, 128) ||
        !exact_service(snapshot.service_revision) ||
        !read_string(data.get(), "snapshotVersion", snapshot.snapshot_version, 128) ||
        !read_count(data.get(), "firstTurnIndex", snapshot.first_turn) ||
        !read_count(data.get(), "lastTurnIndex", snapshot.last_turn) ||
        snapshot.last_turn < snapshot.first_turn ||
        snapshot.last_turn - snapshot.first_turn >= 32 || snapshot.last_turn > 2147483647 ||
        !read_count(data.get(), "expiresAt", snapshot.expires_at) ||
        !parse_turns(cJSON_GetObjectItemCaseSensitive(data.get(), "turns"), snapshot))
        return WireResult::Invalid;
    out = std::move(snapshot);
    return WireResult::Ok;
}

WireResult decode_context_snapshot_ack(WireRoute route, std::string_view body,
                                       ContextSnapshotAck& out) {
    out = {};
    if (route != WireRoute::Message)
        return WireResult::WrongRoute;
    ContextSnapshotAck ack;        // 缺任一原关联字段都不能投影为已接收。
    auto& message = ack.envelope;  // ACK不是请求，不带messageSeq/eventSeq。
    if (decode_envelope(body, message) != DecodeResult::Ok || message.request || !message.code ||
        *message.code != 200 || message.event_sequence || message.message_sequence ||
        message.session.empty() || message.session.size() > 64 ||
        message.method != "contextSnapshotAck")
        return WireResult::Invalid;
    Json data(ur::device::parse_json(message.payload.c_str()),
              cJSON_Delete);  // 精确五键不夹带私有正文。
    std::string status;       // 仅实际留存received，不支持read或played推断。
    if (!exact_keys(data.get(),
                    {"deliveryId", "consentId", "snapshotVersion", "snapshotDigest", "status"}) ||
        !read_string(data.get(), "deliveryId", ack.delivery_id, 64) || !digest(ack.delivery_id) ||
        message.token != ack.delivery_id ||
        !read_string(data.get(), "consentId", ack.consent_id, 64) || !digest(ack.consent_id) ||
        !read_string(data.get(), "snapshotVersion", ack.snapshot_version, 128) ||
        !read_string(data.get(), "snapshotDigest", ack.snapshot_digest, 64) ||
        !digest(ack.snapshot_digest) || !read_string(data.get(), "status", status, 8) ||
        status != "received")
        return WireResult::Invalid;
    out = std::move(ack);
    return WireResult::Ok;
}

WireResult context_snapshot_ack(const ContextSnapshot& snapshot, Envelope& out) {
    out = {};
    std::string original;     // 生成器不信任调用方手工拼接的类型化对象，先重验原完整契约。
    ContextSnapshot checked;  // 只从已验证原wire取ACK身份，不能由可变字段覆盖。
    if (encode_envelope(snapshot.envelope, original) != DecodeResult::Ok ||
        decode_context_snapshot(WireRoute::Message, original, checked) != WireResult::Ok)
        return WireResult::Invalid;
    Json data(cJSON_CreateObject(), cJSON_Delete);  // 分配失败不交付部分ACK。
    if (!data || !cJSON_AddStringToObject(data.get(), "deliveryId", checked.delivery_id.c_str()) ||
        !cJSON_AddStringToObject(data.get(), "consentId", checked.consent_id.c_str()) ||
        !cJSON_AddStringToObject(data.get(), "snapshotVersion", checked.snapshot_version.c_str()) ||
        !cJSON_AddStringToObject(data.get(), "snapshotDigest", checked.snapshot_digest.c_str()) ||
        !cJSON_AddStringToObject(data.get(), "status", "received"))
        return WireResult::Invalid;
    auto* encoded = cJSON_PrintUnformatted(data.get());  // 仅本地固定回执，不泄露正文。
    if (!encoded)
        return WireResult::Invalid;
    Envelope ack;  // 无事件水位的标准MSG接收确认。
    ack.request = false;
    ack.token = checked.delivery_id;
    ack.session = checked.envelope.session;
    ack.method = "contextSnapshotAck";
    ack.code = 200;
    ack.payload = encoded;
    cJSON_free(encoded);
    std::string wire;             // 编码后的严格解码作为ACK生成最后边界。
    ContextSnapshotAck verified;  // 确認完整五键与原身份后才交付。
    if (encode_envelope(ack, wire) != DecodeResult::Ok ||
        decode_context_snapshot_ack(WireRoute::Message, wire, verified) != WireResult::Ok)
        return WireResult::Invalid;
    out = std::move(verified.envelope);
    return WireResult::Ok;
}

void ContextSnapshotInbox::expire(std::uint64_t wall_ms) {
    wall_highwater_ = std::max(wall_highwater_, wall_ms);
    if (retained_ && wall_highwater_ >= retained_->expires_at) {
        retained_.reset();
        canonical_wire_.clear();
        ack_ = {};
    }
}
void ContextSnapshotInbox::clear() {
    retained_.reset();
    canonical_wire_.clear();
    ack_ = {};
    message_highwater_ = wall_highwater_ = 0;
}
bool ContextSnapshotInbox::receive(std::string_view body, std::string_view session, bool connected,
                                   bool held, std::uint64_t wall_ms, Envelope& ack) {
    ack = {};
    const bool clock_valid =
        wall_ms > 0 && wall_ms >= wall_highwater_;  // 校时回退不新建留存或重ACK。
    expire(wall_ms);
    if (!clock_valid || !connected || held || session.empty())
        return false;
    ContextSnapshot candidate;  // 完整验证后才可能改变原留存。
    std::string canonical;      // 原完整语义wire作为幂等键，摘要字段不替代正文比对。
    if (decode_context_snapshot(WireRoute::Message, body, candidate) != WireResult::Ok ||
        candidate.envelope.session != session || candidate.expires_at <= wall_highwater_ ||
        encode_envelope(candidate.envelope, canonical) != DecodeResult::Ok)
        return false;
    if (retained_) {
        if (canonical != canonical_wire_)
            return false;
        ack = ack_;
        return true;
    }
    const auto sequence =
        candidate.envelope.message_sequence.value_or(0);  // 解析后仍在留存边界显式拒缺失水位。
    // 当前原转人工腿仅一份授权；清正文不撤销已消费墓碑，较高序号也不能重新授权。
    if (!sequence || message_highwater_ != 0)
        return false;
    Envelope receipt;  // 先完整生成固定ACK，随后同时提交正文/水位/ACK，发布失败不能丢原事实。
    if (context_snapshot_ack(candidate, receipt) != WireResult::Ok)
        return false;
    message_highwater_ = sequence;
    retained_ = std::move(candidate);
    canonical_wire_ = std::move(canonical);
    ack_ = std::move(receipt);
    ack = ack_;
    return true;
}
}  // namespace ur::device
