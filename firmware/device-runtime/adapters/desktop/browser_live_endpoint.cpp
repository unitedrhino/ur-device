// 文件说明：原MQTT借用端点管理单节目及紧凑墓碑；控制事实、发布背压、actual释放和新node就绪分别门控。
#include "ur/device/json_parse.hpp"
#include "browser_live_endpoint.hpp"
#include <cJSON.h>
#include <algorithm>
#include <limits>
namespace ur::device {
namespace {
constexpr std::size_t max_sessions = 32, max_controls = 32, max_bytes = std::size_t{128} * 1024;
constexpr std::size_t max_invite = 4096, max_control = 2048, max_reply = 2048;
// encoded 获取规范化原信封，不按JSON属性顺序误判幂等，也不忽略原eventSeq。
std::optional<std::string> encoded(const Envelope& message) {
    std::string body;
    if (encode_envelope(message, body) != DecodeResult::Ok)
        return {};
    return body;
}
}  // namespace
// BrowserLiveEndpoint 构造仅固定真实驱动/能力和有限原授权，不连接或开始播放。
BrowserLiveEndpoint::BrowserLiveEndpoint(PublishPort& mqtt, BrowserLiveFactory& factory,
                                         BrowserLiveReadiness readiness, std::string up,
                                         std::string down, std::uint64_t deadline)
    : mqtt_(mqtt),
      factory_(factory),
      readiness_(readiness),
      up_(std::move(up)),
      down_(std::move(down)),
      deadline_(deadline),
      publisher_(*this) {
    retired_.reserve(max_sessions);  // 紧凑墓碑容器一次有界预约，避免退役时隐含扩容预算。
}
// 析构仅请求退出；宿主必须先泵到released/settled，不能用对象销毁当实际资源证明。
BrowserLiveEndpoint::~BrowserLiveEndpoint() { close(clock_.value_or(0)); }
bool BrowserLiveEndpoint::matches(PublishPort& mqtt, std::string_view up,
                                  std::string_view down) const {
    return &mqtt == &mqtt_ && up == up_ && down == down_ && !up.empty() && up != down;
}
// observe 维持跨连接原时钟，回退或宿主期限失效永久禁止本次准入，不删除原事实。
bool BrowserLiveEndpoint::observe(std::uint64_t now) {
    if (!deadline_ || now >= deadline_ || (clock_ && now < *clock_) || !factory_.clock(now)) {
        close(now);
        return false;
    }
    clock_ = now;
    return true;
}
bool BrowserLiveEndpoint::begin_connection(std::uint64_t now) {
    if ((!readiness_.audio && !readiness_.video) || !released() || !settled() || !observe(now))
        return false;
    if (connection_ == std::numeric_limits<std::uint64_t>::max())
        return false;
    retire();
    ++connection_;  // 新连接不清事实，但原关闭许可不能跨连接复活。
    accepting_ = true;
    parent_closing_ = false;
    return true;
}
bool BrowserLiveEndpoint::owns(std::string_view session) const {
    if (current_ && current_->session == session)
        return true;
    return std::any_of(retired_.begin(), retired_.end(),
                       [&](const auto& record) { return record.session == session; });
}
// memory 包含所有紧凑记录容量；原当前端口/Client另受单实例与各自固定预算控制。
std::size_t BrowserLiveEndpoint::memory() const {
    std::size_t bytes = retired_.capacity() * sizeof(Record) + sizeof(Record);
    const auto count = [&](const Record& record) {
        bytes += record.session.capacity() + record.token.capacity() +
                 record.invitation.capacity() + record.decision.capacity() +
                 record.parent.capacity() + record.controls.capacity() * sizeof(Control);
        for (const auto& control : record.controls)
            bytes +=
                control.token.capacity() + control.original.capacity() + control.reply.capacity();
    };
    if (current_)
        count(*current_);
    for (const auto& record : retired_)
        count(record);
    return bytes;
}
// captured 原Client形成事实后保存一次字节；发布失败仍留原确定结果，不重复执行驱动。
bool BrowserLiveEndpoint::captured(std::string_view topic, std::string_view body) {
    if (topic != up_ || !current_)
        return false;
    Envelope message;
    if (decode_envelope(body, message) != DecodeResult::Ok || message.session != current_->session)
        return false;
    bool* delivered = nullptr;
    if (message.method == "sessionAccept" || message.method == "sessionReject") {
        if (message.token != current_->token || body.size() > max_reply)
            return false;
        if (current_->decision.empty())
            current_->decision = body;
        if (current_->decision != body)
            return false;
        delivered = &current_->decision_delivered;
    } else if (message.method == "trackState") {
        const auto found =
            std::find_if(current_->controls.begin(), current_->controls.end(),
                         [&](const auto& control) { return control.token == message.token; });
        if (found == current_->controls.end() || body.size() > max_reply)
            return false;
        if (found->reply.empty())
            found->reply = body;
        if (found->reply != body)
            return false;
        delivered = &found->delivered;
    }
    if (memory() > max_bytes)
        return false;
    const auto published = mqtt_.publish(topic, body);
    if (delivered && published)
        *delivered = true;
    return published;
}
bool BrowserLiveEndpoint::CapturePublisher::publish(std::string_view topic, std::string_view body) {
    return owner_.captured(topic, body);
}
// replay 只允许原Ended控制实际回执字节；原邀请决策已封，不重发accept或构造新接收端口。
bool BrowserLiveEndpoint::replay(const Record& record, const Envelope& message,
                                 std::string_view canonical) {
    if (message.method == "sessionInvite")
        return canonical == record.invitation;
    if (message.method != "trackControl")
        return true;  // 已终态ACK/旧answer仅隔离，不影响新腿。
    const auto control =
        std::find_if(record.controls.begin(), record.controls.end(),
                     [&](const auto& value) { return value.token == message.token; });
    return control != record.controls.end() && control->original == canonical &&
           !control->reply.empty() && mqtt_.publish(up_, control->reply);
}
// receive 先核全局原token/session墓碑，再严格准入新邀请；满额/背压不换token逃避旧事实。
bool BrowserLiveEndpoint::receive(std::string_view topic, std::string_view body, std::uint64_t now,
                                  std::uint64_t wall) {
    Envelope message;
    if (topic != down_ || !wall || decode_envelope(body, message) != DecodeResult::Ok)
        return false;
    const auto canonical = encoded(message);
    if (!canonical)
        return false;
    for (const auto& record : retired_) {
        if (record.session == message.session ||
            (message.method == "sessionInvite" && record.token == message.token))
            return record.session == message.session && replay(record, message, *canonical);
        if (message.method == "trackControl")
            for (const auto& control : record.controls)
                if (control.token == message.token)
                    return false;  // 原方法token不能换新session复用。
    }
    if (current_ && current_->session == message.session) {
        if (!client_)
            return false;
        bool added = false;
        if (message.method == "trackControl") {
            LiveTrackControl command;
            const auto* media = active_media();
            if (!media || decode_live_track_control(WireRoute::Signaling, body, media->binding(),
                                                    command) != WireResult::Ok)
                return false;
            const auto known =
                std::find_if(current_->controls.begin(), current_->controls.end(),
                             [&](const auto& control) { return control.token == message.token; });
            if (known != current_->controls.end()) {
                if (known->original != *canonical)
                    return false;
            } else {
                if (current_->controls.size() >= max_controls || canonical->size() > max_control ||
                    memory() + max_control + max_reply + 128 + 3 * std::string{}.capacity() >=
                        max_bytes)
                    return false;
                current_->controls.push_back({message.token, *canonical, {}, false});
                added = true;
            }
        }
        const auto accepted = client_->receive(topic, body, now, wall);
        if (!accepted && added)
            current_->controls.pop_back();
        if (accepted && client_->phase() == LivePhase::Established)
            current_->established = true;
        if (accepted && message.method == "sessionEnded") {
            terminal_ = true;
            if (!current_->platform_ended &&
                now <= std::numeric_limits<std::uint64_t>::max() - 600000) {
                current_->platform_ended = true;
                current_->retain_until = now + 600000;  // 原平台终态固定截止，重放不刷新。
            }
        }
        return accepted;
    }
    if (!accepting_ || parent_closing_ || !observe(now) || current_ || rearm_ ||
        message.method != "sessionInvite" || retired_.size() >= max_sessions ||
        canonical->size() > max_invite ||
        memory() + max_invite + max_reply + std::size_t{3} * 128 + 5 * std::string{}.capacity() +
                max_controls * sizeof(Control) >=
            max_bytes)
        return false;
    LiveInvite invitation;
    if (decode_live_invite(WireRoute::Signaling, body, invitation) != WireResult::Ok ||
        wall >= invitation.expires_at_ms)
        return false;
    auto media = factory_.create(invitation);
    if (!media || !dynamic_cast<BrowserLiveMediaPort*>(media.get()))
        return false;
    current_ = Record{message.session, message.token, *canonical, {}, {}, false};
    current_->controls.reserve(max_controls);  // 原控制最多32条，不在响应形成后再抢容器空间。
    const auto* concrete = dynamic_cast<BrowserLiveMediaPort*>(media.get());
    current_->parent = concrete->parent_session();
    current_->connection = connection_;
    current_->audio = invitation.audio;
    current_->video = invitation.video;
    media_ = std::move(media);
    client_ = std::make_unique<LiveClient>(publisher_, *media_, std::move(invitation), up_, down_,
                                           deadline_);
    terminal_ = connected_reported_ = false;
    return client_->receive(topic, body, now, wall);
}
BrowserLiveMediaPort* BrowserLiveEndpoint::active_media() {
    return dynamic_cast<BrowserLiveMediaPort*>(media_.get());
}
bool BrowserLiveEndpoint::receive_local(std::string_view body) {
    auto* media = active_media();
    if (!media || !media->receive(body))
        return false;
    std::string canonical;
    if (canonical_json_object(body, canonical) != DecodeResult::Ok)
        return false;
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(ur::device::parse_json(canonical.c_str()),
                                                         cJSON_Delete);
    const auto* kind = cJSON_GetObjectItemCaseSensitive(root.get(), "kind");
    // 实际Stopped只触发等待唯一原仲裁器的新false票据，未完成不得创建下一节目。
    if (cJSON_IsString(kind) && std::string_view(kind->valuestring) == "liveStopped" &&
        media->binding().audio)
        rearm_ = media->lane().lane();
    return true;
}
std::optional<BrowserLiveFocusScope> BrowserLiveEndpoint::scope() const {
    const auto* media = dynamic_cast<const BrowserLiveMediaPort*>(media_.get());
    if (!client_ || !media || !current_) {
        for (auto record = retired_.rbegin(); record != retired_.rend(); ++record)
            if (auto original = cleanup(*record, record->actual_released))
                return original;
        return {};
    }
    return BrowserLiveFocusScope{current_->session,
                                 media->binding().audio,
                                 media->binding().video,
                                 deadline_,
                                 media->authorization_valid(),
                                 client_->phase() == LivePhase::Established,
                                 current_->parent,
                                 current_->platform_ended,
                                 client_->released(),
                                 current_->retain_until};
}
// cleanup 只保关闭原lease的有限历史证据；父不再当前也不删除原绑定，但绝不借新父重新授权输出。
std::optional<BrowserLiveFocusScope> BrowserLiveEndpoint::cleanup(const Record& record,
                                                                  bool actual) const {
    if (!accepting_ || !clock_ || record.connection != connection_ || !record.established ||
        !record.platform_ended || !actual || !record.retain_until ||
        *clock_ >= record.retain_until || record.parent.empty())
        return {};
    return BrowserLiveFocusScope{
        record.session, record.audio,  record.video, deadline_, false,
        false,          record.parent, true,         true,      record.retain_until};
}
std::optional<BrowserLiveFocusScope> BrowserLiveEndpoint::release_scope(
    std::string_view session) const {
    if (current_ && current_->session == session)
        return cleanup(*current_, client_ && client_->released());
    for (const auto& record : retired_)
        if (record.session == session)
            return cleanup(record, record.actual_released);
    return {};
}
std::optional<std::string_view> BrowserLiveEndpoint::rearm_lane() const {
    return rearm_ ? std::optional<std::string_view>(*rearm_) : std::nullopt;
}
bool BrowserLiveEndpoint::rearmed(std::string_view lane) {
    if (!rearm_ || *rearm_ != lane || !released())
        return false;
    rearm_.reset();
    return true;
}
bool BrowserLiveEndpoint::released() const { return !client_ || client_->released(); }
bool BrowserLiveEndpoint::settled() const {
    return released() && (!parent_closing_ || !current_ || current_->platform_ended) &&
           (!current_ || (current_->decision_delivered &&
                          std::all_of(current_->controls.begin(), current_->controls.end(),
                                      [](const auto& control) {
                                          return !control.reply.empty() && control.delivered;
                                      })));
}
// retire 不要求新node就绪才记录原Released；新节目仍单独由rearm屏障挡住。
void BrowserLiveEndpoint::retire() {
    if (!current_ || !client_ || !terminal_ || !settled() || retired_.size() >= max_sessions)
        return;
    current_->actual_released = true;  // settled包括原全部实际资源退出和回执交付，非仅本地终态。
    retired_.push_back(std::move(*current_));
    current_.reset();
    client_.reset();
    media_.reset();  // 原Client先退出，Media之后释放唯一实际预约。
}
// tick 持续泵原actual关闭；尚未收到平台终态或原背压事实未交付时不丢当前关联。
void BrowserLiveEndpoint::tick(std::uint64_t now, std::uint64_t) {
    observe(now);
    auto* media = active_media();
    if (!media || !client_)
        return;
    media->poll(now);
    if (client_->phase() == LivePhase::Negotiating) {
        if (const auto offer = media->take_offer())
            if (!client_->offer_ready(*offer, now))
                client_->close(now);
        if (!connected_reported_ && media->connected())
            connected_reported_ = client_->media_connected(now);
    }
    client_->tick(now);
    retire();
}
void BrowserLiveEndpoint::close(std::uint64_t now) {
    accepting_ = false;
    if (client_) {
        client_->close(now);
        terminal_ = true;
    }  // 仅关闭/退役许可；不是平台sessionEnded证明，也不生成focusRelease或授权下一原租约。
}
// begin_parent_close
// 保原连接的有限关闭资格；只停媒体，不伪造平台childEnded或把旧lease当已Released。
void BrowserLiveEndpoint::begin_parent_close(std::uint64_t now) {
    if (accepting_)
        parent_closing_ = true;
    if (client_)
        client_->close(now);
    // terminal_仍只能由平台Ended更新；actualStopped先到也不能提前退役而丢失后来原终态证明。
}
}  // namespace ur::device
