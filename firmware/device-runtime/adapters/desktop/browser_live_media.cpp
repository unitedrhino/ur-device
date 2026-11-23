// 文件说明：节目接收回环事实桥；原源授权、临时输出mask、实际单轨操作及资源释放独立。
#include "ur/device/json_parse.hpp"
#include "browser_live_media.hpp"
#include <cJSON.h>
#include <charconv>
#include <limits>

namespace ur::device {
namespace {
// Json 本地事实树仅在事件线程解析，网页回调不会直接写共享核心。
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
// text 只读取严格字符串，重复键/非法JSON先由公共canonical检查拒绝。
std::string_view text(const cJSON* root, const char* key) {
    const auto* value = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsString(value) ? std::string_view(value->valuestring) : std::string_view{};
}
// integer 核安全uint整数；本地generation仍是原意图关联，不是授权来源。
std::optional<std::uint64_t> integer(const cJSON* root, const char* key) {
    const auto* value = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsNumber(value) || value->valuedouble < 1 || value->valuedouble > 9007199254740991.0)
        return {};
    const auto out = static_cast<std::uint64_t>(value->valuedouble);
    return static_cast<double>(out) == value->valuedouble ? std::optional<std::uint64_t>(out)
                                                          : std::nullopt;
}
// ticket 校验本地uint64原实际票据，拒绝浮点/前导零/溢出字符串。
std::optional<std::uint64_t> ticket(const cJSON* root) {
    const auto value = text(root, "gateTicket");
    std::uint64_t out = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), out);
    if (value.empty() || value.front() == '0' || parsed.ec != std::errc{} ||
        parsed.ptr != value.data() + value.size() || !out)
        return {};
    return out;
}
// serialized 仅编码内部有限参数，不手拼自由SDP或网页错误。
std::string serialized(const Json& root) {
    std::unique_ptr<char, decltype(&cJSON_free)> body(cJSON_PrintUnformatted(root.get()),
                                                      cJSON_free);
    return body ? body.get() : std::string{};
}
// base 只暴露本地实例和固定媒体代次，不向网页交MQTT身份/session/token。
Json base(const char* kind, const std::string& instance) {
    Json root(cJSON_CreateObject(), cJSON_Delete);
    if (!root || !cJSON_AddStringToObject(root.get(), "kind", kind) ||
        !cJSON_AddStringToObject(root.get(), "instance", instance.c_str()) ||
        !cJSON_AddNumberToObject(root.get(), "mediaRevision", 1))
        root.reset();
    return root;
}
// index 限定原两轨；不能因收到video帧就扩原audio-only许可。
std::optional<std::size_t> index(const LiveInvite& invite, std::string_view id) {
    if (invite.audio && id == "broadcast-out")
        return 0;
    if (invite.video && id == "broadcast-video-out")
        return 1;
    return {};
}
}  // namespace
// BrowserLiveMediaPort 构造不分配Peer、不输出；原父业务/期限由宿主可信状态固定。
BrowserLiveMediaPort::BrowserLiveMediaPort(LiveInvite invite, BrowserLiveLane& lane,
                                           BrowserLiveReadiness readiness, BrowserLiveScope parent,
                                           std::string parent_id, std::uint64_t lease,
                                           std::uint64_t instance, std::uint64_t now,
                                           std::shared_ptr<bool> reservation)
    : binding_(std::move(invite)),
      lane_(lane),
      readiness_(readiness),
      parent_(std::move(parent)),
      parent_id_(std::move(parent_id)),
      instance_(std::to_string(instance)),
      lease_(lease),
      now_(now),
      reservation_(std::move(reservation)) {
    std::string body;
    LiveInvite checked;
    failed_ =
        !instance || instance > 9007199254740991ULL || parent_id_.empty() || !parent_ || !lease ||
        now >= lease ||
        lane.lane() != (binding_.audio ? (binding_.video ? "audioVideo" : "audio") : "video") ||
        encode_envelope(binding_.envelope, body) != DecodeResult::Ok ||
        decode_live_invite(WireRoute::Signaling, body, checked) != WireResult::Ok ||
        checked.audio != binding_.audio || checked.video != binding_.video ||
        checked.expires_at_ms != binding_.expires_at_ms;
}
// ~BrowserLiveMediaPort 请求原独立关闭；宿主必须在实际Stopped后才销毁实例。
BrowserLiveMediaPort::~BrowserLiveMediaPort() {
    stop_all();
    if (reservation_ && stopped_)
        *reservation_ = false;
}
// prepare 只接受原绑定和独立有限期限，默认关闭末端必须实际确认，不借另一路idle。
LivePreparation BrowserLiveMediaPort::prepare(const LiveInvite& invite, std::uint64_t deadline) {
    std::string expected, actual;
    if (prepared_ || failed_ || closing_ || !deadline || deadline > lease_ || now_ >= deadline ||
        encode_envelope(binding_.envelope, expected) != DecodeResult::Ok ||
        encode_envelope(invite.envelope, actual) != DecodeResult::Ok || expected != actual ||
        invite.audio != binding_.audio || invite.video != binding_.video ||
        invite.expires_at_ms != binding_.expires_at_ms || !authorization_valid())
        return LivePreparation::Denied;
    if ((binding_.audio && !readiness_.audio) || (binding_.video && !readiness_.video))
        return LivePreparation::Unsupported;
    if (!lane_.idle())
        return LivePreparation::Busy;
    lease_ = deadline;
    prepared_ = true;
    return LivePreparation::Ready;
}
// enqueue 只交原有限内部命令，容量满拒绝，禁止删除旧原意图为新控制腾位。
bool BrowserLiveMediaPort::enqueue(std::string body) {
    return !body.empty() && commands_.push_back(std::move(body));
}
// begin_peer 默认关闭单轨输出，接收器不创建任何采集或相机轨。
bool BrowserLiveMediaPort::begin_peer() {
    if (!prepared_ || peer_ || closing_ || !authorization_valid())
        return false;
    auto root = base("liveBegin", instance_);
    if (!root || !cJSON_AddStringToObject(root.get(), "lane", lane_.lane().c_str()) ||
        !cJSON_AddBoolToObject(root.get(), "audio", binding_.audio) ||
        !cJSON_AddBoolToObject(root.get(), "video", binding_.video) || !enqueue(serialized(root)))
        return false;
    peer_ = true;
    return true;
}
// apply_answer 原完整answer仅受理一次；网络connected须独立真实事实。
bool BrowserLiveMediaPort::apply_answer(std::string_view sdp) {
    if (!peer_ || !offer_received_ || answer_ || closing_ || !authorization_valid() ||
        sdp.size() < 8 || sdp.size() > 49152 || sdp.substr(0, 5) != "v=0\r\n" ||
        sdp.find('\0') != std::string_view::npos)
        return false;
    auto root = base("liveAnswer", instance_);
    if (!root || !cJSON_AddStringToObject(root.get(), "sdp", std::string(sdp).c_str()) ||
        !enqueue(serialized(root)))
        return false;
    answer_ = true;
    return true;
}
// connected 仅当前Peer真实网络状态，不据此授予节目或末端输出。
bool BrowserLiveMediaPort::connected() const { return connected_ && !closing_ && !failed_; }
// authorization_valid 只核原有限业务许可，临时输出focus关闭不撤销节目。
bool BrowserLiveMediaPort::authorization_valid() const {
    const auto parent = parent_ ? parent_() : std::nullopt;
    return !failed_ && !closing_ && now_ < lease_ && parent && *parent == parent_id_;
}
// presentation_permitted 只查询当前原末端mask，仍不能替代Established或新轨意图。
bool BrowserLiveMediaPort::presentation_permitted(std::string_view track_id) const {
    return authorization_valid() && index(binding_, track_id) && lane_.permitted();
}
// begin_track 每轨只允许一个原操作，false无副作用；网页必须异步交实际结果。
bool BrowserLiveMediaPort::begin_track(const LiveTrackControl& command, std::uint64_t generation) {
    const auto track = index(binding_, command.track_id);
    if (!connected() || !authorization_valid() || !track || pending_[*track] || !generation ||
        generation > 9007199254740991ULL || command.media_revision != 1 ||
        command.envelope.session != binding_.envelope.session ||
        (command.operation != "pause" && command.operation != "resume" &&
         command.operation != "stop") ||
        (command.operation == "resume" && !presentation_permitted(command.track_id)))
        return false;
    auto root = base("liveTrackControl", instance_);
    const auto original_gate =
        command.operation == "resume" ? lane_.permitted_ticket() : std::nullopt;
    if (command.operation == "resume" && !original_gate)
        return false;
    if (!root || !cJSON_AddStringToObject(root.get(), "trackId", command.track_id.c_str()) ||
        !cJSON_AddStringToObject(root.get(), "operation", command.operation.c_str()) ||
        !cJSON_AddNumberToObject(root.get(), "generation", static_cast<double>(generation)) ||
        (original_gate && !cJSON_AddStringToObject(root.get(), "gateTicket",
                                                   std::to_string(*original_gate).c_str())) ||
        !enqueue(serialized(root)))
        return false;
    pending_[*track] = Operation{command, generation, original_gate};
    return true;
}
// take_fact 有界消费原实际结果，不把出队受理转换成成功事实。
std::optional<LiveMediaFact> BrowserLiveMediaPort::take_fact() {
    if (facts_.empty())
        return {};
    auto fact = std::move(*facts_.front());
    facts_.pop_front();
    return fact;
}
// stop_all 立即封原输出意图并保独立关闭命令；所有原资源实际退出后才返回true。
bool BrowserLiveMediaPort::stop_all() {
    if (!closing_) {
        closing_ = true;
        if (!peer_)
            stopped_ = true;
        else
            close_command_ = serialized(base("liveClose", instance_));
    }
    return stopped_;
}
// poll 同步宿主时间，回退或当前源许可失效必须实际收尾，不自行恢复节目。
bool BrowserLiveMediaPort::poll(std::uint64_t now) {
    if (now < now_)
        failed_ = true;
    else
        now_ = now;
    if (!closing_ && !authorization_valid())
        stop_all();
    return !failed_;
}
// take_command 关闭独占优先；已关闭原实例不再发尚未执行的旧播放/answer意图。
std::optional<std::string> BrowserLiveMediaPort::take_command() {
    if (close_command_) {
        auto value = std::move(close_command_);
        close_command_.reset();
        return value;
    }
    if (closing_ || commands_.empty())
        return {};
    auto value = std::move(*commands_.front());
    commands_.pop_front();
    return value;
}
// take_offer 只取一次当前原Peer实际完整SDP，不保存第二个可复活旧会话的offer。
std::optional<std::string> BrowserLiveMediaPort::take_offer() {
    if (closing_)
        return {};
    auto value = std::move(offer_);
    offer_.reset();
    return value;
}
// receive 精确关联本地实例/原媒体代次/原operation；终态只能接实际Stopped，不接迟到active。
bool BrowserLiveMediaPort::receive(std::string_view body) {
    std::string canonical;
    if (canonical_json_object(body, canonical) != DecodeResult::Ok)
        return false;
    Json root(ur::device::parse_json(canonical.c_str()), cJSON_Delete);
    if (!root || text(root.get(), "instance") != instance_ ||
        integer(root.get(), "mediaRevision") != 1)
        return false;
    const auto kind = text(root.get(), "kind");
    const auto count = cJSON_GetArraySize(root.get());
    if (kind == "liveStopped") {
        if (!closing_ || count != 8)
            return false;
        for (const auto* name : {"peerClosed", "tracksStopped", "outputsDetached",
                                 "callbacksStopped", "transformsStopped"})
            if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root.get(), name)))
                return false;
        stopped_ = true;
        connected_ = false;
        return true;
    }
    if (closing_ || failed_ || !peer_)
        return false;
    if (kind == "liveOffer") {
        const auto sdp = text(root.get(), "sdp");
        if (count != 4 || offer_received_ || answer_ || sdp.size() < 8 || sdp.size() > 49152 ||
            sdp.substr(0, 5) != "v=0\r\n" || sdp.find('\0') != std::string_view::npos)
            return false;
        offer_ = std::string(sdp);
        offer_received_ = true;
        return true;
    }
    if (kind == "liveConnected") {
        if (count != 3 || !answer_)
            return false;
        connected_ = true;
        return true;
    }
    if (kind == "liveFailed") {
        if (count != 3)
            return false;
        failed_ = true;
        connected_ = false;
        stop_all();
        return true;
    }
    if (kind != "liveTrackFact" || (count != 7 && count != 8) || !connected() ||
        !authorization_valid())
        return false;
    const auto track = index(binding_, text(root.get(), "trackId"));
    const auto generation = integer(root.get(), "generation"), code = integer(root.get(), "code");
    if (!track)
        return false;
    const auto& pending = pending_[*track];
    if (!pending.has_value() || !generation || *generation != pending->generation || !code ||
        *code > 599 || facts_.size() >= facts_.capacity())
        return false;
    const auto& operation = pending->command.operation;
    const auto state = text(root.get(), "state");
    LiveMediaFact fact{*generation, 1, std::string(text(root.get(), "trackId")), std::string(state),
                       static_cast<int>(*code)};
    if (*code == 200) {
        const auto expected = operation == "resume"  ? "active"
                              : operation == "pause" ? "paused"
                                                     : "stopped";
        if (state != expected)
            return false;
        // 形成actual携原已确认true票据；Bridge晚交的临时mask变化不抹除合法原事实。
        if (state == "active") {
            if (count != 8 || !pending->gate_ticket || ticket(root.get()) != pending->gate_ticket)
                return false;
        } else if (count != 7)
            return false;
    } else {
        if (count != 7 || *code < 400 || !state.empty() ||
            cJSON_GetObjectItemCaseSensitive(root.get(), "state"))
            return false;
        const auto failure = text(root.get(), "failure");
        if (failure == "outputBusy" && *code == 409)
            fact.failure = LiveMediaFailure::OutputBusy;
        else if (failure != "mediaFailed")
            return false;
    }
    if (!facts_.push_back(std::move(fact)))
        return false;
    pending_[*track].reset();
    return true;
}
// BrowserLiveFactory 配置严格固定原三组和父业务查询，不从页面任意选择物理组。
BrowserLiveFactory::BrowserLiveFactory(std::array<BrowserLiveLane*, 3> lanes,
                                       BrowserLiveReadiness readiness, BrowserLiveScope parent,
                                       std::uint64_t deadline)
    : lanes_(lanes), readiness_(readiness), parent_(std::move(parent)), deadline_(deadline) {
    failed_ = !parent_ || !deadline || !lanes_[0] || !lanes_[1] || !lanes_[2] ||
              lanes_[0]->lane() != "audio" || lanes_[1]->lane() != "video" ||
              lanes_[2]->lane() != "audioVideo";
}
// clock 原宿主同步高水位，回退不可通过新邀请重置。
bool BrowserLiveFactory::clock(std::uint64_t now) {
    if ((now_ && now < *now_) || now >= deadline_)
        failed_ = true;
    if (!failed_)
        now_ = now;
    return !failed_;
}
// create 固定原C++当前父业务、本地实例及完整原组；任何缺口都不回退其他媒体权限。
std::unique_ptr<LiveMediaPort> BrowserLiveFactory::create(const LiveInvite& invite) {
    const auto parent = parent_ ? parent_() : std::nullopt;
    if (failed_ || !now_ || !parent || parent->empty() || *reservation_ ||
        serial_ >= 9007199254740991ULL)
        return {};
    const auto path = invite.audio ? (invite.video ? 2U : 0U) : 1U;
    auto port =
        std::make_unique<BrowserLiveMediaPort>(invite, *lanes_[path], readiness_, parent_, *parent,
                                               deadline_, ++serial_, *now_, reservation_);
    *reservation_ = true;
    return port;
}
}  // namespace ur::device
