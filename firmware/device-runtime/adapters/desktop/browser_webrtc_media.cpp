// 文件说明：WebRTC媒体只经受控回环桥交换SDP和播放控制，不让网页拥有云端设备凭据。
#include "browser_webrtc_media.hpp"
#include <cJSON.h>
#include <limits>
#include <memory>
#include "ur/device/envelope.hpp"
#include "ur/device/json_parse.hpp"
#include "browser_input_ownership_wire.hpp"

namespace ur::device {
namespace {
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
// valid_sdp拒绝空和畸形候选文本；完整语法仍由浏览器与Pion检查。
bool valid_sdp(std::string_view sdp) {
    return sdp.size() >= 8 && sdp.size() <= 49152 && sdp.substr(0, 5) == "v=0\r\n" &&
           sdp.find('\0') == std::string_view::npos;
}
// decimal只接规范十进制原字符串，避免浮点精度或溢出改变控制关联。
bool decimal(const cJSON* value, std::uint64_t& result) {
    if (!cJSON_IsString(value) || !value->valuestring || value->valuestring[0] < '1' ||
        value->valuestring[0] > '9')
        return false;
    result = 0;
    for (const char* cursor = value->valuestring; *cursor; ++cursor) {
        if (*cursor < '0' || *cursor > '9' ||
            result > (std::numeric_limits<std::uint64_t>::max() - (*cursor - '0')) / 10)
            return false;
        result = result * 10 + (*cursor - '0');
    }
    return true;
}
// same_text只比较原字段，不把缺省、null或数字当合法原标识。
bool same_text(const cJSON* object, const char* key, std::string_view expected) {
    const auto* value = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(value) && value->valuestring && value->valuestring == expected;
}
}  // namespace
bool BrowserWebRTCMedia::command(std::string_view kind, std::string_view id, std::uint64_t epoch,
                                 std::string_view sdp) {
    Json value(cJSON_CreateObject(), &cJSON_Delete);
    const std::string type(kind), object(id), description(sdp);
    if (!value || !cJSON_AddStringToObject(value.get(), "kind", type.c_str()) ||
        (!id.empty() && !cJSON_AddStringToObject(value.get(), "id", object.c_str())) ||
        (kind == "webrtcVolume" &&
         !cJSON_AddNumberToObject(value.get(), "volume", static_cast<double>(epoch))) ||
        (kind != "webrtcVolume" && epoch != 0 &&
         !cJSON_AddStringToObject(value.get(), "epoch", std::to_string(epoch).c_str())) ||
        (!sdp.empty() && !cJSON_AddStringToObject(value.get(), "sdp", description.c_str()))) {
        failed_ = true;
        return false;
    }
    // 音量是物理speaker原令牌；其余Peer操作均绑定原核心已授代次。
    if (kind != "webrtcVolume" &&
        !cJSON_AddStringToObject(value.get(), "peerRevision", std::to_string(peer_revision_).c_str())) {
        failed_ = true;
        return false;
    }
    if ((kind == "captureStart" || kind == "captureStop") && !capture_session_.empty() &&
        (!cJSON_AddStringToObject(value.get(), "sessionId", capture_session_.c_str()) ||
         !cJSON_AddStringToObject(value.get(), "routeGeneration",
                                  std::to_string(capture_generation_).c_str()))) {
        failed_ = true;
        return false;
    }
    if (kind == "webrtcConfigureVision" &&
        (!vision_ ||
         !cJSON_AddStringToObject(value.get(), "cameraId", vision_->camera_id.c_str()) ||
         !cJSON_AddStringToObject(value.get(), "videoTrack", vision_->video_track.c_str()) ||
         !cJSON_AddStringToObject(value.get(), "codec", "H264") ||
         !cJSON_AddStringToObject(value.get(), "state", "paused"))) {
        failed_ = true;
        return false;
    }
    char* raw = cJSON_PrintUnformatted(value.get());
    if (!raw) {
        failed_ = true;
        return false;
    }
    std::string body(raw);
    cJSON_free(raw);
    if (kind == "webrtcConfigure" || kind == "webrtcConfigureVision" || kind == "webrtcRecover")
        peer_command_ = body;
    if (kind == "captureStart")
        capture_start_command_ = body;
    if (body.size() > 65536 || !commands_.push_back(std::move(body))) {
        failed_ = true;
        return false;
    }
    return true;
}
bool BrowserWebRTCMedia::configure(const VoiceMedia& media) {
    if (media.ai_vision || vision_configured_ || configured_ || failed_ ||
        media.transport != VoiceTransport::WebRTC || media.audio.sample_rate != 48000 ||
        media.audio.channels != 1 || media.audio.frame_ms != 20 || !media.host.empty() ||
        media.port != 0)
        return false;
    peer_revision_ = 1;
    disconnected_ = peer_command_sent_ = false;
    configured_ = command("webrtcConfigure");
    return configured_;
}
// bind_capture_route不安装输入许可，必须是尚无输入的同一已连接候选Peer。
bool BrowserWebRTCMedia::bind_capture_route(std::string_view session, std::uint64_t generation) {
    if (!ready() || !input_.empty() || !capture_session_.empty() || capture_closing_ ||
        browser_input_ownership_command({std::string(session), generation, false}, 1).empty())
        return false;
    capture_session_ = session;
    capture_generation_ = generation;
    return true;
}
// fence_capture_route只关闭原路由，错session/代次不影响其他端；排队失败不伪造停止事实。
bool BrowserWebRTCMedia::fence_capture_route(std::string_view session, std::uint64_t generation) {
    if (session.empty() || session != capture_session_ || generation != capture_generation_)
        return false;
    if (!input_.empty()) {
        if (!command("captureStop", input_))
            return false;
        input_.clear();
    }
    capture_permitted_ = false;
    return true;
}
// enable_capture_route并不替用户产生新输入；终态后原路由永久不能重开。
bool BrowserWebRTCMedia::enable_capture_route(std::string_view session, std::uint64_t generation) {
    if (!ready() || capture_closing_ || session.empty() || session != capture_session_ ||
        generation != capture_generation_)
        return false;
    capture_permitted_ = true;
    return true;
}
// finish_capture_route_close由原完整HAL实际释放驱动；不得把调用受理当作浏览器已停Peer。
bool BrowserWebRTCMedia::finish_capture_route_close(std::string_view session,
                                                  std::uint64_t generation) {
    if (session.empty() || session != capture_session_ || generation != capture_generation_ ||
        capture_permitted_ || !input_.empty())
        return false;
    if (!capture_closing_)
        return true;
    if (!command("webrtcStop"))
        return false;
    capture_closing_ = false;
    return true;
}
// enable_vision不打开摄像头；明确的ready来自宿主实际端口预检，不是本函数自行认定。
bool BrowserWebRTCMedia::enable_vision(const VoiceVisionProfile& profile) {
    if ((vision_preflight_ && vision_preflight_result_ != std::optional<bool>(true)) || vision_ ||
        configured_ || failed_ || profile.max_publishers != 1 || profile.camera_id.size() > 64 ||
        profile.video_track.size() > 64 || vision_profile_binding(&profile).empty())
        return false;
    vision_ = profile;
    return true;
}
bool BrowserWebRTCMedia::prepare_vision_preflight(const VoiceVisionProfile& profile,
                                                  std::string_view device_id) {
    auto candidate = profile;
    candidate.ready = true;  // 仅供结构validator，不保存为能力或交给Core。
    if (profile.ready || configured_ || failed_ || vision_ || vision_preflight_ ||
        profile.max_publishers != 1 || profile.camera_id.size() > 64 ||
        profile.video_track.size() > 64 || vision_profile_binding(&candidate).empty() ||
        device_id.empty() || device_id.size() > 256)
        return false;
    for (const auto byte : device_id) {
        if (static_cast<unsigned char>(byte) < 32 || byte == 127)
            return false;
    }
    Json value(cJSON_CreateObject(), &cJSON_Delete);
    if (!value || !cJSON_AddStringToObject(value.get(), "deviceId", std::string(device_id).c_str()))
        return false;
    char* raw = cJSON_PrintUnformatted(value.get());
    if (!raw)
        return false;
    std::string encoded(raw), canonical;
    cJSON_free(raw);
    if (canonical_json_object(encoded, canonical) != DecodeResult::Ok)
        return false;
    vision_preflight_ = profile;
    vision_device_id_ = std::string(device_id);
    return true;
}
bool BrowserWebRTCMedia::receive_vision_ready(std::string_view json) {
    std::string canonical;
    if (!vision_preflight_ || failed_ || configured_ || json.size() > 4096 ||
        canonical_json_object(json, canonical) != DecodeResult::Ok)
        return false;
    Json value(parse_json(canonical.c_str()), &cJSON_Delete);
    if (!value || cJSON_GetArraySize(value.get()) != 5 ||
        !same_text(value.get(), "kind", "webrtcVisionReady") ||
        !same_text(value.get(), "cameraId", vision_preflight_->camera_id) ||
        !same_text(value.get(), "videoTrack", vision_preflight_->video_track) ||
        !same_text(value.get(), "deviceId", vision_device_id_))
        return false;
    const bool supported = same_text(value.get(), "state", "ready");
    if (!supported && !same_text(value.get(), "state", "unsupported"))
        return false;
    if (vision_preflight_result_)
        return *vision_preflight_result_ == supported;
    vision_preflight_result_ = supported;
    if (!supported)
        return true;  // 原普通音频仍可用，不能用默认相机填补未知视觉能力。
    auto profile = *vision_preflight_;
    profile.ready = true;
    return enable_vision(profile);
}
const VoiceVisionProfile* BrowserWebRTCMedia::vision_profile() const {
    return vision_ ? &*vision_ : nullptr;
}
// configure_vision固定原平台选择；只有后续原Core控制可取帧，网络connected不会取得摄像头。
bool BrowserWebRTCMedia::configure_vision(const VoiceMedia& media,
                                          const VoiceVisionProfile& profile) {
    if (!vision_ || vision_configured_ || configured_ || failed_ || !media.ai_vision ||
        media.transport != VoiceTransport::WebRTC || media.audio.sample_rate != 48000 ||
        media.audio.channels != 1 || media.audio.frame_ms != 20 || !media.host.empty() ||
        media.port != 0 || media.input_track != "mic-1" || media.output_track != "speaker-1" ||
        vision_profile_binding(&profile).empty() ||
        vision_profile_binding(&profile) != vision_profile_binding(&*vision_))
        return false;
    peer_revision_ = 1;
    disconnected_ = peer_command_sent_ = false;
    configured_ = command("webrtcConfigureVision");
    vision_configured_ = configured_;
    return configured_;
}
std::string BrowserWebRTCMedia::vision_command(const VoiceVisionControl& control,
                                               bool cancel) const {
    Json value(cJSON_CreateObject(), &cJSON_Delete);
    if (!value ||
        !cJSON_AddStringToObject(value.get(), "kind",
                                 cancel ? "webrtcVisionCancel" : "webrtcVisionControl") ||
        !cJSON_AddStringToObject(value.get(), "generation",
                                 std::to_string(control.generation).c_str()) ||
        !cJSON_AddStringToObject(value.get(), "sessionId", control.session_id.c_str()) ||
        !cJSON_AddStringToObject(value.get(), "cameraId", control.camera_id.c_str()) ||
        !cJSON_AddStringToObject(value.get(), "trackId", control.track_id.c_str()) ||
        !cJSON_AddStringToObject(value.get(), "mediaRevision",
                                 std::to_string(control.media_revision).c_str()) ||
        !cJSON_AddStringToObject(value.get(), "peerRevision",
                                 std::to_string(peer_revision_).c_str()) ||
        (!cancel && !cJSON_AddStringToObject(
                        value.get(), "operation",
                        control.operation == VoiceVisionOperation::Publish ? "publish" : "stop")))
        return {};
    char* raw = cJSON_PrintUnformatted(value.get());
    if (!raw)
        return {};
    std::string result(raw);
    cJSON_free(raw);
    std::string canonical;
    if (canonical_json_object(result, canonical) != DecodeResult::Ok)
        return {};
    return result;
}
VoiceVisionRequestResult BrowserWebRTCMedia::begin_vision_control(
    const VoiceVisionControl& control) {
    if (!vision_configured_ || !vision_ || !ready() || disconnected_ || control.generation == 0 ||
        control.session_id.empty() || control.session_id.size() > 64 ||
        control.session_id.find('\0') != std::string::npos ||
        control.camera_id != vision_->camera_id || control.track_id != vision_->video_track ||
        control.media_revision == 0 || control.media_revision > 9007199254740991ULL ||
        (control.operation != VoiceVisionOperation::Publish &&
         control.operation != VoiceVisionOperation::Stop))
        return VoiceVisionRequestResult::Unsupported;
    for (const auto byte : control.session_id) {
        if (static_cast<unsigned char>(byte) < 32 || byte == 127)
            return VoiceVisionRequestResult::Unsupported;
    }
    if (vision_control_) {
        const auto& original = *vision_control_;
        if (control.session_id != original.session_id ||
            control.media_revision != original.media_revision ||
            control.generation < original.generation)
            return VoiceVisionRequestResult::Unsupported;
        if (control.generation == original.generation)
            return !vision_cancelled_ && control.operation == original.operation
                       ? VoiceVisionRequestResult::Accepted
                       : VoiceVisionRequestResult::Unsupported;
        if (control.operation == VoiceVisionOperation::Publish &&
            (!vision_settled_ || vision_cancelled_ || vision_active_ || vision_uncertain_))
            return VoiceVisionRequestResult::Busy;
    }
    auto body = vision_command(control, false);
    if (body.empty())
        return VoiceVisionRequestResult::Unsupported;
    if (vision_facts_.full() || !commands_.push_back(std::string(body)))
        return VoiceVisionRequestResult::Busy;
    vision_control_ = control;
    vision_command_ = std::move(body);
    vision_sent_ = vision_settled_ = vision_cancelled_ = false;
    vision_last_fact_.reset();
    return VoiceVisionRequestResult::Accepted;
}
std::optional<VoiceVisionFact> BrowserWebRTCMedia::take_vision_fact() {
    auto* fact = vision_facts_.front();
    if (!fact)
        return {};
    auto result = *fact;
    vision_facts_.pop_front();
    return result;
}
void BrowserWebRTCMedia::cancel_vision_control(std::uint64_t generation) {
    if (!vision_control_ || generation != vision_control_->generation || vision_cancelled_)
        return;
    vision_cancelled_ = true;
    auto body = vision_command(*vision_control_, true);
    if (body.empty() || !commands_.push_back(std::move(body)))
        close();  // 不能交付取消时退休原Peer，不继续持有未知摄像头意图。
}
bool BrowserWebRTCMedia::receive_vision_actual(std::string_view json) {
    std::string canonical;
    if (json.size() > 4096 || canonical_json_object(json, canonical) != DecodeResult::Ok)
        return false;
    Json value(parse_json(canonical.c_str()), &cJSON_Delete);
    if (!value || cJSON_GetArraySize(value.get()) != 9 ||
        !same_text(value.get(), "kind", "webrtcVisionActual"))
        return false;
    constexpr const char* fields[] = {"kind",         "generation", "sessionId",
                                      "cameraId",     "trackId",    "mediaRevision",
                                      "peerRevision", "state",      "error"};
    for (const cJSON* item = value->child; item; item = item->next) {
        bool allowed = false;
        for (const auto* field : fields)
            allowed = allowed || (item->string && std::string_view(item->string) == field);
        if (!allowed)
            return false;
    }
    std::uint64_t generation = 0, media = 0, peer = 0;
    if (!decimal(cJSON_GetObjectItemCaseSensitive(value.get(), "generation"), generation) ||
        !decimal(cJSON_GetObjectItemCaseSensitive(value.get(), "mediaRevision"), media) ||
        media > 9007199254740991ULL ||
        !decimal(cJSON_GetObjectItemCaseSensitive(value.get(), "peerRevision"), peer) ||
        !vision_control_ || !vision_ ||
        !same_text(value.get(), "sessionId", vision_control_->session_id) ||
        !same_text(value.get(), "cameraId", vision_->camera_id) ||
        !same_text(value.get(), "trackId", vision_->video_track) ||
        media != vision_control_->media_revision)
        return false;
    VoiceVisionFact fact{generation, VoiceVisionState::Failed, VoiceVisionFailure::None};
    if (same_text(value.get(), "state", "active"))
        fact.state = VoiceVisionState::Active;
    else if (same_text(value.get(), "state", "stopped"))
        fact.state = VoiceVisionState::Stopped;
    else if (!same_text(value.get(), "state", "failed"))
        return false;
    const char* errors[] = {"", "unsupported", "busy", "mediaFailed", "timeout", "cancelled"};
    unsigned error = 0;
    while (error < 6 && !same_text(value.get(), "error", errors[error]))
        ++error;
    if (error == 6 || (fact.state == VoiceVisionState::Failed) != (error != 0))
        return false;
    constexpr VoiceVisionFailure failures[] = {
        VoiceVisionFailure::None,    VoiceVisionFailure::Unsupported,
        VoiceVisionFailure::Busy,    VoiceVisionFailure::MediaFailed,
        VoiceVisionFailure::Timeout, VoiceVisionFailure::Cancelled};
    fact.failure = failures[error];
    if (generation > vision_control_->generation || peer > peer_revision_)
        return false;
    if (generation < vision_control_->generation || peer < peer_revision_ || vision_cancelled_ ||
        !configured_ || disconnected_)
        return true;  // 退休事实只丢弃，不能重开轨或为后继控制提供确认。
    if (generation != vision_control_->generation || peer != peer_revision_ || !ready() ||
        !vision_sent_ ||
        (fact.state == VoiceVisionState::Active &&
         vision_control_->operation != VoiceVisionOperation::Publish) ||
        (fact.state == VoiceVisionState::Stopped &&
         vision_control_->operation != VoiceVisionOperation::Stop))
        return false;
    if (vision_last_fact_)
        return fact.state == vision_last_fact_->state && fact.failure == vision_last_fact_->failure;
    if (!vision_facts_.push_back(std::move(fact)))
        return false;
    vision_last_fact_ = fact;
    vision_settled_ = true;
    vision_uncertain_ = fact.state == VoiceVisionState::Failed;
    if (fact.state != VoiceVisionState::Failed)
        vision_active_ = fact.state == VoiceVisionState::Active;
    return true;
}
bool BrowserWebRTCMedia::offer_received(std::string_view sdp, std::uint64_t peer_revision) {
    if (peer_revision && peer_revision < peer_revision_)
        return true;
    if (peer_revision != peer_revision_ || !configured_ || failed_ || !peer_command_sent_ ||
        disconnected_ || offer_ || offer_taken_ || !valid_sdp(sdp))
        return false;
    offer_ = std::string(sdp);
    return true;
}
std::optional<std::string> BrowserWebRTCMedia::take_offer() {
    auto value = std::move(offer_);
    offer_.reset();
    if (value)
        offer_taken_ = true;
    return value;
}
bool BrowserWebRTCMedia::apply_answer(std::string_view sdp) {
    if (!configured_ || failed_ || !offer_taken_ || answered_ || !valid_sdp(sdp))
        return false;
    answered_ = command("webrtcAnswer", {}, 0, sdp);
    return answered_;
}
bool BrowserWebRTCMedia::connected_received(std::uint64_t peer_revision) {
    if (peer_revision && peer_revision < peer_revision_)
        return true;
    if (peer_revision != peer_revision_ || !configured_ || !answered_ || failed_ ||
        disconnected_ || connected_)
        return false;
    connected_ = true;
    return true;
}
// disconnected_received只失效当前网络意图；物理末端票据与原音量回执仍归共享宿主。
bool BrowserWebRTCMedia::disconnected_received(std::uint64_t peer_revision) {
    if (peer_revision && peer_revision < peer_revision_)
        return true;
    if (peer_revision != peer_revision_ || !configured_ || failed_ || !answered_ ||
        (!connected_ && !disconnected_))
        return false;
    connected_ = false;
    disconnected_ = true;
    input_.clear();
    response_.clear();
    epoch_ = 0;
    drained_.reset();
    return true;
}
// begin_recovery不自主提升云端代次，仅执行VoiceClient已确认的原相邻恢复。
bool BrowserWebRTCMedia::begin_recovery(std::uint64_t next_revision) {
    if (vision_configured_ || !configured_ || failed_ || !disconnected_ ||
        peer_revision_ == std::numeric_limits<std::uint64_t>::max() ||
        next_revision != peer_revision_ + 1)
        return false;
    peer_revision_ = next_revision;
    disconnected_ = peer_command_sent_ = offer_taken_ = answered_ = connected_ = false;
    offer_.reset();
    return command("webrtcRecover");
}
bool BrowserWebRTCMedia::poll(std::uint64_t now) {
    if (output_gate_ready_) {
        if ((gate_clock_ && now < *gate_clock_) || (gate_pending_ && now >= gate_deadline_))
            failed_ = true;
        gate_clock_ = now;
    }
    return !failed_;
}
// enable_output_gate只固定本实例实际端口；不重置原票据或重新取得输出资格。
bool BrowserWebRTCMedia::enable_output_gate() {
    if (failed_ || configured_ || output_gate_ready_)
        return false;
    output_gate_ready_ = true;
    return true;
}
std::optional<std::uint64_t> BrowserWebRTCMedia::request_output_gate(bool enabled,
                                                                     std::uint64_t now) {
    if (!output_gate_ready_ || !poll(now) || gate_pending_ || (enabled && !ready()) ||
        gate_ticket_ == std::numeric_limits<std::uint64_t>::max() ||
        now > std::numeric_limits<std::uint64_t>::max() - 2000)
        return {};
    const auto ticket = gate_ticket_ + 1;
    const auto body = "{\"kind\":\"playbackGate\",\"ticket\":\"" + std::to_string(ticket) +
                      "\",\"enabled\":" + (enabled ? "true" : "false") + "}";
    if (!commands_.push_back(std::string(body))) {
        failed_ = true;
        return {};
    }
    gate_ticket_ = ticket;
    gate_pending_ = true;
    gate_sent_ = false;
    gate_enabled_ = enabled;
    gate_deadline_ = now + 2000;
    gate_command_ = body;
    return ticket;
}
bool BrowserWebRTCMedia::output_gate_confirmed(std::uint64_t ticket, bool enabled) const {
    return output_gate_ready_ && !failed_ && !gate_pending_ && ticket != 0 &&
           ticket == gate_confirmed_ && enabled == gate_enabled_;
}
bool BrowserWebRTCMedia::output_gated(std::uint64_t ticket, bool enabled, std::uint64_t now) {
    if (!output_gate_ready_ || !poll(now))
        return false;
    if (ticket && ticket < gate_ticket_)
        return true;  // 原退休票据只能丢弃，不覆盖当前打开或停止事实。
    if (!ticket || ticket != gate_ticket_ || enabled != gate_enabled_ ||
        (gate_pending_ && !gate_sent_)) {
        failed_ = true;
        return false;
    }
    if (!gate_pending_)
        return ticket == gate_confirmed_;
    gate_pending_ = false;
    gate_confirmed_ = ticket;
    gate_command_.clear();
    return true;
}
bool BrowserWebRTCMedia::prepare_reply(const std::array<std::uint8_t, 8>&,
                                       std::uint32_t first_sequence) {
    return ready() && first_sequence == 0 && response_.empty();
}
bool BrowserWebRTCMedia::finish_reply(std::uint32_t last_sequence) {
    return ready() && last_sequence == 0 && !response_.empty() && epoch_ != 0 &&
           command("webrtcFinish", {}, epoch_);
}
bool BrowserWebRTCMedia::start_capture(std::string_view input_id) {
    if (!ready() || !input_.empty() || input_id.empty() || input_id.size() > 128 ||
        (!capture_session_.empty() && (!capture_permitted_ || capture_closing_)))
        return false;
    input_ = std::string(input_id);
    return command("captureStart", input_);
}
void BrowserWebRTCMedia::stop_capture() {
    if (input_.empty())
        return;
    command("captureStop", input_);
    input_.clear();
}
bool BrowserWebRTCMedia::start_playback(std::string_view response_id, std::uint64_t epoch) {
    if (!ready() || !response_.empty() || response_id.empty() || response_id.size() > 128 ||
        epoch == 0)
        return false;
    response_ = std::string(response_id);
    epoch_ = epoch;
    drained_.reset();
    return command("playbackStart", response_, epoch_);
}
void BrowserWebRTCMedia::stop_playback(std::uint64_t epoch) {
    if (epoch == 0 || epoch != epoch_ || response_.empty())
        return;
    command("playbackStop", response_, epoch_);
    response_.clear();
    epoch_ = 0;
    drained_.reset();
}
bool BrowserWebRTCMedia::drained_received(std::uint64_t epoch, std::uint64_t peer_revision) {
    if (peer_revision && peer_revision < peer_revision_)
        return true;
    if (peer_revision != peer_revision_ || !ready() || response_.empty() || epoch == 0 ||
        epoch != epoch_ || drained_)
        return false;
    drained_ = epoch;
    return true;
}
bool BrowserWebRTCMedia::set_volume(unsigned percent) {
    (void)percent;
    return false;  // 跨进程增益不可同步确认，必须使用带标识的异步接口。
}
bool BrowserWebRTCMedia::request_volume(std::string_view token, unsigned percent) {
    if (!ready() || volume_pending_ || volume_confirmed_ || token.empty() || token.size() > 128 ||
        token.find('\0') != std::string_view::npos || percent > 100 ||
        !command("webrtcVolume", token, percent))
        return false;
    volume_pending_ = VolumeApplied{std::string(token), percent};
    return true;
}
bool BrowserWebRTCMedia::volume_applied(std::string_view token, unsigned percent) {
    if (!volume_pending_ || volume_confirmed_ || volume_pending_->token != token ||
        volume_pending_->percent != percent)
        return false;
    volume_ = percent;
    volume_confirmed_ = std::move(volume_pending_);
    volume_pending_.reset();
    return true;
}
std::optional<VolumeApplied> BrowserWebRTCMedia::take_volume_applied() {
    auto result = std::move(volume_confirmed_);
    volume_confirmed_.reset();
    return result;
}
std::optional<std::string> BrowserWebRTCMedia::take_command() {
    std::string value;  // 原关闭之前尚未交付的开麦意图必须退休，不能在关闭后短暂开麦再停。
    for (;;) {
        auto* next = commands_.front();
        if (!next)
            return {};
        value = std::move(*next);
        commands_.pop_front();
        if (!capture_session_.empty() && !capture_permitted_ && value == capture_start_command_)
            continue;
        break;
    }
    if (gate_pending_ && value == gate_command_)
        gate_sent_ = true;
    if (!peer_command_.empty() && value == peer_command_)
        peer_command_sent_ = true;
    if (!vision_command_.empty() && value == vision_command_)
        vision_sent_ = true;
    return value;
}
void BrowserWebRTCMedia::close() {
    vision_cancelled_ = true;
    if (vision_configured_)
        commands_.clear();  // 视觉Peer退休时先撤旧未交付取帧命令，确保原停止不被队列背压丢弃。
    if (configured_) {
        if (capture_session_.empty())
            command("webrtcStop");
        else {
            // 原Sender与输出票据仍需活Peer完成false事实，宿主在HAL settled后再停网络腿。
            stop_capture();
            capture_permitted_ = false;
            capture_closing_ = true;
        }
    }
    configured_ = offer_taken_ = answered_ = connected_ = disconnected_ =
        peer_command_sent_ = false;
    offer_.reset();
    peer_command_.clear();
    drained_.reset();
    input_.clear();
    response_.clear();
    epoch_ = 0;
    volume_pending_.reset();
    volume_confirmed_.reset();
}
}  // namespace ur::device
