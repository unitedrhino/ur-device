// 文件说明：可信固定MP4档案的有界本地桥；只从真实网页事实确认档案/Peer，不持网络设备身份。
#include "ur/device/json_parse.hpp"
#include "browser_record_playback.hpp"
#include <cJSON.h>
#include <algorithm>
#include <charconv>
#include <memory>
#include <utility>

namespace ur::device {
namespace {
// safe_integer 与平台JSON整数上限一致，uint64操作generation另用十进制字符串。
constexpr std::uint64_t safe_integer = 9007199254740991ULL;
// Json 释放所有临时JSON树，错误不返回素材路径或原payload。
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
// id 检查可信配置中的非空有限标识，不把转义／控制字符当目录分隔符。
bool id(std::string_view text, std::size_t maximum) {
    return !text.empty() && text.size() <= maximum &&
           std::none_of(text.begin(), text.end(),
                        [](unsigned char ch) { return ch < 32 || ch == 127; });
}
// text 返回精确字符串字段，缺失／数字／null均不自动转换。
std::string text(const cJSON* object, const char* key) {
    const auto* field = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(field) ? field->valuestring : std::string{};
}
// number 只接受JSON非负安全整数，positive用于媒体代次。
bool number(const cJSON* object, const char* key, std::uint64_t& result, bool positive = false) {
    const auto* field = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(field) || !(field->valuedouble >= (positive ? 1 : 0)) ||
        field->valuedouble > static_cast<double>(safe_integer))
        return false;
    result = static_cast<std::uint64_t>(field->valuedouble);
    return static_cast<double>(result) == field->valuedouble;
}
// generation 原uint64字符串无前导零，不通过JS浮点处理操作身份。
bool generation(std::string_view value, std::uint64_t& result) {
    if (value.empty() || value.front() == '0' || value.size() > 20)
        return false;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && result != 0;
}
// add_uint 输出精确十进制安全整数，不通过浮点近似位置。
bool add_uint(cJSON* object, const char* key, std::uint64_t value) {
    return cJSON_AddRawToObject(object, key, std::to_string(value).c_str()) != nullptr;
}
// mime 固定首驱动格式，不把VP8/AAC素材映射成H264/Opus档案能力。
const char* mime(bool audio) {
    return audio ? "video/mp4; codecs=\"avc1.42E01E,opus\"" : "video/mp4; codecs=\"avc1.42E01E\"";
}
// sdp 仅做完整候选SDP基本格式检查，实际方向和codec由浏览器／媒体模块分别验证。
bool sdp(std::string_view value) {
    return value.size() >= 8 && value.size() <= 49152 && value.substr(0, 5) == "v=0\r\n" &&
           value.find('\0') == std::string_view::npos;
}
}  // namespace

// 构造仅核验部署清单和排队能力探针，不读取档案、不创建媒体资源。
BrowserRecordPlaybackPort::BrowserRecordPlaybackPort(BrowserRecordAsset asset,
                                                     BrowserRecordPermission permission)
    : asset_(std::move(asset)), permission_(std::move(permission)) {
    const auto prefix = std::string_view("/record-assets/");
    const bool route =
        asset_.route.size() > prefix.size() && asset_.route.size() <= 128 &&
        asset_.route.substr(0, prefix.size()) == prefix &&
        asset_.route.find("..") == std::string::npos &&
        std::all_of(asset_.route.begin() + static_cast<std::ptrdiff_t>(prefix.size()),
                    asset_.route.end(), [](unsigned char ch) {
                        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                               (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.';
                    });
    configured_ = route && permission_ && id(asset_.camera_id, 64) && id(asset_.recording_id, 64) &&
                  id(asset_.recording_version, 64) && id(asset_.content_ref, 512) &&
                  asset_.duration_ms > 0 && asset_.duration_ms <= safe_integer &&
                  asset_.max_bytes > 0 && asset_.max_bytes <= 67108864;
    if (configured_)
        enqueue(command("recordProbe"));
}
// 析构请求整组停止；宿主应在销毁前取得原实际关闭事实，析构不制造released。
BrowserRecordPlaybackPort::~BrowserRecordPlaybackPort() { stop_all(); }

// enqueue在本地桥背压时失败关闭，不丢意图仍继续发布。
bool BrowserRecordPlaybackPort::enqueue(std::string body) {
    if (body.empty() || body.size() > 65536 || !commands_.push_back(std::move(body))) {
        failed_ = true;
        return false;
    }
    return true;
}
// command构造固定白名单结构；本地m/g只关联原核心意图，不添加MQTT权限字段。
std::string BrowserRecordPlaybackPort::command(const char* kind, std::uint64_t revision,
                                               const PlaybackCommand* control,
                                               std::string_view value) const {
    Json object(cJSON_CreateObject(), cJSON_Delete);
    if (!object || !cJSON_AddStringToObject(object.get(), "kind", kind))
        return {};
    const std::string_view type(kind);
    if (type == "recordProbe") {
        if (!cJSON_AddStringToObject(object.get(), "mime", mime(asset_.audio)) ||
            !cJSON_AddBoolToObject(object.get(), "audio", asset_.audio))
            return {};
    } else {
        if (!binding_ ||
            !cJSON_AddStringToObject(object.get(), "sessionId",
                                     binding_->envelope.session.c_str()) ||
            !cJSON_AddStringToObject(object.get(), "playbackId", binding_->playback_id.c_str()))
            return {};
        if (type == "recordBind") {
            if (!cJSON_AddStringToObject(object.get(), "assetRoute", asset_.route.c_str()) ||
                !cJSON_AddStringToObject(object.get(), "mime", mime(asset_.audio)) ||
                !cJSON_AddBoolToObject(object.get(), "audio", asset_.audio) ||
                !add_uint(object.get(), "durationMs", asset_.duration_ms) ||
                !add_uint(object.get(), "maxBytes", asset_.max_bytes))
                return {};
        } else if (type == "recordClose") {
            if (!cJSON_AddStringToObject(object.get(), "releaseId", "1"))
                return {};
        } else {
            if (!add_uint(object.get(), "mediaRevision", revision))
                return {};
            if (type == "recordControl" || type == "recordRestore") {
                if (!cJSON_AddStringToObject(object.get(), "generation",
                                             value.empty() ? std::to_string(generation_).c_str()
                                                           : std::string(value).c_str()))
                    return {};
                if (control) {
                    if (!cJSON_AddStringToObject(object.get(), "operation",
                                                 control->operation.c_str()) ||
                        (control->position_ms &&
                         !add_uint(object.get(), "positionMs", *control->position_ms)))
                        return {};
                }
            }
        }
    }
    std::unique_ptr<char, decltype(&cJSON_free)> raw(cJSON_PrintUnformatted(object.get()),
                                                     cJSON_free);
    return raw ? raw.get() : std::string{};
}
// tick以宿主单调时钟切断过期／倒退，不从页面活动刷新许可。
void BrowserRecordPlaybackPort::tick(std::uint64_t now) {
    if ((clock_ && now < *clock_) || (binding_ && now >= deadline_))
        failed_ = true;
    clock_ = now;
    if (failed_ || (binding_ && !valid()))
        stop_all();
}
// ready仅反映配置与实际格式探针，不代表任何会话已获媒体许可。
bool BrowserRecordPlaybackPort::ready() const {
    return configured_ && format_ready_ && !failed_ && !closing_;
}
// valid在每次媒体动作前重核原有限本地许可，缺时钟默认拒绝。
bool BrowserRecordPlaybackPort::valid() const {
    return ready() && clock_ && (!binding_ || *clock_ < deadline_) && permission_(*clock_);
}
// prepare精确绑定固定清单四元组和整份clip，回报Ready仍不是已loaded。
RecordPreparation BrowserRecordPlaybackPort::prepare(const RecordPlaybackInvite& binding,
                                                     std::uint64_t deadline) {
    if (!configured_ || !format_ready_)
        return RecordPreparation::Unsupported;
    if (failed_ || closing_ || binding_)
        return RecordPreparation::Unavailable;
    if (!clock_ || !permission_(*clock_) || deadline <= *clock_)
        return RecordPreparation::Denied;
    if (binding.camera_id != asset_.camera_id || binding.recording_id != asset_.recording_id ||
        binding.recording_version != asset_.recording_version ||
        binding.content_ref != asset_.content_ref)
        return RecordPreparation::Denied;
    if (binding.clip.start_ms != 0 || binding.clip.end_ms != asset_.duration_ms ||
        binding.audio != asset_.audio)
        return RecordPreparation::Unsupported;
    binding_ = binding;
    deadline_ = deadline;
    if (!enqueue(command("recordBind"))) {
        stop_all();
        return RecordPreparation::Unavailable;
    }
    return RecordPreparation::Ready;
}
// begin把原控制转为有界本地意图；seek只受理原m指定的唯一下一m，不自主增长。
bool BrowserRecordPlaybackPort::begin(const PlaybackCommand& control, std::uint64_t value,
                                      std::uint64_t next_revision) {
    if (!binding_ || !valid() ||
        (pending_ && !(operation_ == "seek" && control.operation == "stop")) || value == 0 ||
        value <= generation_ || control.envelope.session != binding_->envelope.session ||
        control.playback_id != binding_->playback_id || !control.media_revision)
        return false;
    const auto& operation = control.operation;
    if (operation == "load") {
        if (loaded_ || control.content_ref != asset_.content_ref || *control.media_revision != 1 ||
            next_revision != 1)
            return false;
    } else if (!loaded_ || (operation != "play" && operation != "pause" && operation != "seek" &&
                            operation != "stop")) {
        return false;
    }
    if (operation == "seek") {
        if (*control.media_revision != current_revision_ || current_revision_ == safe_integer ||
            next_revision != current_revision_ + 1 || !control.position_ms ||
            *control.position_ms > asset_.duration_ms)
            return false;
    } else if (next_revision != current_revision_ || *control.media_revision != current_revision_)
        return false;
    if (operation == "play" && !publishing_permitted(current_revision_))
        return false;
    const auto before = generation_;
    generation_ = value;
    if (!enqueue(command("recordControl", next_revision, &control))) {
        generation_ = before;
        stop_all();
        return false;
    }
    current_revision_ = next_revision;
    operation_ = operation;
    pending_ = true;
    fact_queued_ = false;
    return true;
}
// take_fact交出原实际事实才解除在途屏障，旧generation不能解除当前操作。
std::optional<RecordPlaybackFact> BrowserRecordPlaybackPort::take_fact() {
    if (facts_.empty())
        return {};
    auto fact = std::move(*facts_.front());
    facts_.pop_front();
    if (fact.generation == generation_) {
        pending_ = false;
        fact_queued_ = false;
    }
    return fact;
}
// begin_peer必须等原代次实际Stopped；档案loaded不代表当前Peer已连通。
bool BrowserRecordPlaybackPort::begin_peer(std::uint64_t revision) {
    if (!binding_ || !valid() || !loaded_ || revision != current_revision_ ||
        peers_.count(revision))
        return false;
    for (const auto& peer : peers_)
        if (!peer.second.stopped)
            return false;
    peers_.clear();
    if (!enqueue(command("recordPeerBegin", revision))) {
        stop_all();
        return false;
    }
    peers_.emplace(revision, Peer{});
    return true;
}
// apply_answer只向原Peer交完整应答，实际Connected由独立页面回执确认。
bool BrowserRecordPlaybackPort::apply_answer(std::uint64_t revision, std::string_view description) {
    auto peer = peers_.find(revision);
    if (!valid() || peer == peers_.end() || peer->second.closing || peer->second.answered ||
        !sdp(description))
        return false;
    Json object(cJSON_CreateObject(), cJSON_Delete);
    if (!object || !binding_ || !cJSON_AddStringToObject(object.get(), "kind", "recordAnswer") ||
        !cJSON_AddStringToObject(object.get(), "sessionId", binding_->envelope.session.c_str()) ||
        !cJSON_AddStringToObject(object.get(), "playbackId", binding_->playback_id.c_str()) ||
        !add_uint(object.get(), "mediaRevision", revision) ||
        !cJSON_AddStringToObject(object.get(), "sdp", std::string(description).c_str()))
        return false;
    std::unique_ptr<char, decltype(&cJSON_free)> raw(cJSON_PrintUnformatted(object.get()),
                                                     cJSON_free);
    if (!raw || !enqueue(raw.get())) {
        stop_all();
        return false;
    }
    peer->second.answered = true;
    return true;
}
// connected同时检查原实际Peer与当前许可，不把answer排队当连通。
bool BrowserRecordPlaybackPort::connected(std::uint64_t revision) const {
    const auto peer = peers_.find(revision);
    return valid() && peer != peers_.end() && peer->second.connected && !peer->second.closing;
}
// close_peer立即撤回本地发布许可，实际旧回调退出仍须Stopped事实。
void BrowserRecordPlaybackPort::close_peer(std::uint64_t revision) {
    auto peer = peers_.find(revision);
    if (peer == peers_.end() || peer->second.closing)
        return;
    peer->second.closing = true;
    peer->second.connected = false;
    if (!enqueue(command("recordPeerClose", revision)))
        stop_all();
}
// peer_stopped只返回原Peer已收实际退出，不以不存在或超时推断已停。
bool BrowserRecordPlaybackPort::peer_stopped(std::uint64_t revision) const {
    const auto peer = peers_.find(revision);
    return peer != peers_.end() && peer->second.stopped;
}
// restore_intent仅接核心Established后原loaded/paused/playing意图，不自主恢复播放。
bool BrowserRecordPlaybackPort::restore_intent(std::uint64_t revision, std::string_view state,
                                               std::uint64_t value) {
    if (!valid() || pending_ || revision != current_revision_ || value != generation_ ||
        !connected(revision) || (state != "loaded" && state != "paused" && state != "playing") ||
        (state == "playing" && !publishing_permitted(revision)))
        return false;
    PlaybackCommand control;
    control.operation = std::string(state);
    if (!enqueue(command("recordRestore", revision, &control))) {
        stop_all();
        return false;
    }
    pending_ = true;
    fact_queued_ = false;
    return true;
}
// publishing_permitted仍非平台Established，由核心与此原驱动屏障共同限制play。
bool BrowserRecordPlaybackPort::publishing_permitted(std::uint64_t revision) const {
    return loaded_ && revision == current_revision_ && connected(revision);
}
// stop_all保持未知直到原Bind可能建立的所有资源实际关闭，不靠关闭排队造事实。
bool BrowserRecordPlaybackPort::stop_all() {
    if (!binding_ || !bound_sent_) {
        // Bind尚未交给宿主时网页无原绑定，不能要求它确认从未存在的媒体资源。
        commands_.clear();
        closing_ = released_ = true;
        return true;
    }
    if (!closing_) {
        closing_ = true;
        for (auto& peer : peers_) {
            peer.second.closing = true;
            peer.second.connected = false;
        }
        commands_.clear();  // 未发送的load/play在终态后不能执行，只保留本实例关闭意图。
        enqueue(command("recordClose"));
    }
    return released_;
}

// receive严格验原本地身份/结构/代次；旧回调只忽略，不能复活权限或推进新Actual。
bool BrowserRecordPlaybackPort::receive(std::string_view body) {
    std::string canonical;
    if (canonical_json_object(body, canonical) != DecodeResult::Ok)
        return false;
    Json object(ur::device::parse_json(canonical.c_str()), cJSON_Delete);
    if (!object)
        return false;
    const auto kind = text(object.get(), "kind");
    if (kind == "recordBridgeReady") {
        const auto* supported = cJSON_GetObjectItemCaseSensitive(object.get(), "supported");
        if (cJSON_GetArraySize(object.get()) != 3 ||
            text(object.get(), "mime") != mime(asset_.audio) || !cJSON_IsBool(supported))
            return false;
        if (closing_)
            return true;
        format_ready_ = cJSON_IsTrue(supported);
        return true;
    }
    if (!binding_ || text(object.get(), "sessionId") != binding_->envelope.session ||
        text(object.get(), "playbackId") != binding_->playback_id)
        return false;
    if (kind == "recordClosed") {
        const auto* success = cJSON_GetObjectItemCaseSensitive(object.get(), "success");
        if (cJSON_GetArraySize(object.get()) != 5 || text(object.get(), "releaseId") != "1" ||
            !cJSON_IsBool(success) || !closing_)
            return false;
        if (cJSON_IsTrue(success)) {
            released_ = true;
            for (auto& peer : peers_)
                peer.second.stopped = true;
        }
        return true;
    }
    std::uint64_t revision = 0;
    if (!number(object.get(), "mediaRevision", revision, true))
        return false;
    if (kind == "recordPeerStopped") {
        auto peer = peers_.find(revision);
        if (cJSON_GetArraySize(object.get()) != 4 || peer == peers_.end() || !peer->second.closing)
            return false;
        peer->second.stopped = true;
        return true;
    }
    if (kind == "recordFact") {
        RecordPlaybackFact fact;
        fact.media_revision = revision;
        if (!generation(text(object.get(), "generation"), fact.generation))
            return false;
        std::uint64_t code = 0;
        if (!number(object.get(), "code", code) || (code != 200 && (code < 400 || code > 599)))
            return false;
        fact.code = static_cast<int>(code);
        if (code == 200) {
            const auto* spontaneous = cJSON_GetObjectItemCaseSensitive(object.get(), "spontaneous");
            const auto* duration = cJSON_GetObjectItemCaseSensitive(object.get(), "durationMs");
            fact.state = text(object.get(), "state");
            if (cJSON_GetArraySize(object.get()) != (duration ? 10 : 9) ||
                !cJSON_IsBool(spontaneous) ||
                (fact.state != "loaded" && fact.state != "playing" && fact.state != "paused" &&
                 fact.state != "seeking" && fact.state != "stopped" && fact.state != "ended" &&
                 fact.state != "failed") ||
                !number(object.get(), "positionMs", fact.position_ms) ||
                fact.position_ms > asset_.duration_ms)
                return false;
            fact.spontaneous = cJSON_IsTrue(spontaneous);
            if (duration) {
                std::uint64_t value = 0;
                if (!number(object.get(), "durationMs", value) || value != asset_.duration_ms)
                    return false;
                fact.duration_ms = value;
            }
        } else if (cJSON_GetArraySize(object.get()) != 6)
            return false;
        if (fact.generation != generation_ || revision != current_revision_ || closing_)
            return true;
        if (canonical == last_fact_)
            return true;
        if (fact_queued_ || (!fact.spontaneous && !pending_))
            return false;
        if (!facts_.push_back(std::move(fact))) {
            failed_ = true;
            stop_all();
            return false;
        }
        last_fact_ = canonical;
        fact_queued_ = true;
        if (code == 200 && text(object.get(), "state") == "loaded")
            loaded_ = true;
        return true;
    }
    if (kind != "recordOffer" && kind != "recordConnected" && kind != "recordFailure")
        return false;
    auto peer = peers_.find(revision);
    if (revision != current_revision_ || peer == peers_.end() || peer->second.closing || closing_)
        return true;
    BrowserRecordEvent event;
    event.media_revision = revision;
    if (kind == "recordOffer") {
        event.kind = BrowserRecordEvent::Kind::Offer;
        event.sdp = text(object.get(), "sdp");
        if (cJSON_GetArraySize(object.get()) != 5 || !sdp(event.sdp))
            return false;
    } else if (kind == "recordConnected") {
        if (cJSON_GetArraySize(object.get()) != 4 || !peer->second.answered)
            return false;
        if (peer->second.connected)
            return true;
        peer->second.connected = true;
        event.kind = BrowserRecordEvent::Kind::Connected;
    } else if (kind == "recordFailure") {
        if (cJSON_GetArraySize(object.get()) != 4)
            return false;
        event.kind = BrowserRecordEvent::Kind::Failed;
        failed_ = true;
    } else
        return false;
    if (!events_.push_back(std::move(event))) {
        failed_ = true;
        stop_all();
        return false;
    }
    return true;
}
// take_command把有界原字节交宿主，Bind交付后保守等待实际整组关闭。
std::optional<std::string> BrowserRecordPlaybackPort::take_command() {
    if (commands_.empty())
        return {};
    auto body = std::move(*commands_.front());
    commands_.pop_front();
    // 仅宿主取出原Bind后进入实际关闭屏障；未知投递状态仍按可能已建资源处理。
    if (body.find("\"kind\":\"recordBind\"") != std::string::npos)
        bound_sent_ = true;
    return body;
}
// take_event交出当前原Peer事实，不含用户身份或档案路径。
std::optional<BrowserRecordEvent> BrowserRecordPlaybackPort::take_event() {
    if (events_.empty())
        return {};
    auto event = std::move(*events_.front());
    events_.pop_front();
    return event;
}
}  // namespace ur::device
