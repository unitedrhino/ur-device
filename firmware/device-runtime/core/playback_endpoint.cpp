// 文件说明：文件邀请严格路由、独立准入与有界终态保留；不持设备密钥或建立第二条网络连接。
#include "ur/device/playback_endpoint.hpp"
#include <cJSON.h>
#include <limits>

namespace ur::device {
namespace {
constexpr std::size_t retired_limit = 128;  // 十分钟内独立子会话身份保留上限。
constexpr std::size_t retired_bytes =
    65536;  // 墓碑对象/字符串容量预算，不含分配器及容器空余槽开销。
}  // namespace
bool PlaybackEndpoint::established(std::string_view session) const {
    if (closing_)
        return false;
    for (const auto& slot : slots_)
        if (slot.session_id == session && slot.session &&
            slot.session->phase() == PlaybackPhase::Active)
            return true;
    return false;
}
// PlaybackEndpoint 借用唯一发布端口和可信准入器；构造不连接网络或启动媒体。
PlaybackEndpoint::PlaybackEndpoint(PublishPort& mqtt, PlaybackAdmissionPort& admission,
                                   PlaybackCapability capability, std::string up, std::string down)
    : mqtt_(mqtt),
      admission_(admission),
      capability_(capability),
      up_(std::move(up)),
      down_(std::move(down)) {}
// 析构只请求本实例关闭，宿主正常路径须先确认实际释放再销毁。
PlaybackEndpoint::~PlaybackEndpoint() { close(clock_.value_or(0)); }

// owns 只查有界原实例，空会话不是可分派身份。
bool PlaybackEndpoint::owns(std::string_view session) const {
    if (session.empty())
        return false;
    for (const auto& slot : slots_)
        if (slot.session_id == session)
            return true;
    for (const auto& retired : retired_)
        if (retired.session == session)
            return true;
    return false;
}
// released 不把异步关闭请求当作释放；未准入的拒绝槽不持资源。
bool PlaybackEndpoint::released() const {
    for (const auto& slot : slots_)
        if (slot.preparing || slot.canceling || (slot.session && !slot.session->released()))
            return false;
    return true;
}
// settled 为正常父会话收尾提供独立门控，不能只看驱动实际关闭或逻辑Ended。
bool PlaybackEndpoint::settled() const {
    for (const auto& slot : slots_) {
        if (slot.session_id.empty())
            continue;
        if (slot.session) {
            if (!slot.session->release_submitted())
                return false;
        } else if (!slot.closed_at || slot.terminal_token.empty() || !slot.release_published) {
            return false;
        }
    }
    return true;
}
// begin_connection 保留旧实例幂等记录及待发布事实，不重建驱动或刷新原内容许可。
bool PlaybackEndpoint::begin_connection(std::uint64_t now) {
    if (!closing_ || !released() || (clock_ && now < *clock_))
        return false;
    clock_ = now;
    closing_ = false;
    return true;
}

// retire_slot 不用逻辑Ended推断释放；独立平台终态和全部原事实缺一不可，拒绝槽同样不能猜测。
PlaybackEndpoint::Slot* PlaybackEndpoint::retire_slot() {
    if (retired_.size() >= retired_limit)
        return nullptr;
    for (auto& slot : slots_) {
        if (!slot.closed_at || slot.preparing || slot.canceling || slot.rejection_pending)
            continue;
        std::optional<std::string> release;
        if (slot.session) {
            release = slot.session->retirement_release();
        } else if (!slot.terminal_token.empty() && slot.release_published &&
                   !slot.release_fact.empty()) {
            release = slot.release_fact;
        }
        if (!release)
            continue;
        Retired candidate{slot.session_id, slot.invitation, std::move(*release), *slot.closed_at,
                          0};
        candidate.bytes = sizeof(Retired) + candidate.session.capacity() +
                          candidate.invitation.capacity() + candidate.release.capacity() + 3;
        if (candidate.bytes > retired_bytes - retired_bytes_)
            continue;
        retired_bytes_ += candidate.bytes;
        retired_.push_back(std::move(candidate));
        slot.session.reset();  // 会话必须先于它借用的原驱动销毁；已实际释放不会触发新执行。
        slot.driver.reset();
        slot = Slot{};
        return &slot;
    }
    return nullptr;
}

// reject 固定原邀请token和子会话，不以播放事实或成功ACK代替设备拒绝。
bool PlaybackEndpoint::reject(const PlaybackInvite& invite, std::string_view reason, Slot* slot) {
    Envelope reply;
    reply.token = invite.envelope.token;
    reply.session = invite.envelope.session;
    reply.method = "sessionReject";
    reply.payload = std::string("{\"reason\":\"") + std::string(reason) + "\"}";
    std::string body;
    if (encode_envelope(reply, body) != DecodeResult::Ok)
        return false;
    if (slot)
        slot->rejection = body;
    const bool sent = mqtt_.publish(up_, body);
    if (slot)
        slot->rejection_pending = !sent;
    return sent;
}

// cancel_preparation
// 由已交付驱动或准备适配报告实际关闭；未知释放仍保持资源占用。
void PlaybackEndpoint::cancel_preparation(Slot& slot, std::uint64_t now) {
    slot.preparing = false;
    const bool released =
        slot.driver ? slot.driver->stop_all() : admission_.cancel(slot.session_id, now);
    slot.canceling = !released;
    if (released && !slot.closed_at) {
        slot.closed_at = now;
        slot.driver.reset();
    }
}

// publish_preparation_release 不制造已建立或已播放状态；与正式会话使用同一释放契约。
void PlaybackEndpoint::publish_preparation_release(Slot& slot) {
    if (!slot.closed_at || slot.terminal_token.empty() || slot.release_published)
        return;
    if (slot.release_fact.empty()) {
        PlaybackInvite invite;
        if (decode_playback_invite(WireRoute::Signaling, slot.invitation, invite) != WireResult::Ok)
            return;
        std::unique_ptr<cJSON, decltype(&cJSON_Delete)> data(cJSON_CreateObject(), cJSON_Delete);
        if (!data ||
            !cJSON_AddStringToObject(data.get(), "playbackId", invite.playback_id.c_str()) ||
            !cJSON_AddBoolToObject(data.get(), "released", true) ||
            !cJSON_AddRawToObject(data.get(), "endEventSeq",
                                  std::to_string(slot.terminal_sequence).c_str()))
            return;
        std::unique_ptr<char, decltype(&cJSON_free)> text(cJSON_PrintUnformatted(data.get()),
                                                          cJSON_free);
        if (!text)
            return;
        Envelope fact;
        fact.token = slot.terminal_token;
        fact.method = "playbackState";
        fact.session = slot.session_id;
        fact.request = false;
        fact.payload = text.get();
        if (encode_envelope(fact, slot.release_fact) != DecodeResult::Ok)
            return;
    }
    if (mqtt_.publish(up_, slot.release_fact))
        slot.release_published = true;
}

// resolve
// 保存原邀请的准备窗口及独立许可；pending不发接听，也不重分配内容或焦点。
bool PlaybackEndpoint::resolve(Slot& slot, PlaybackInvite invite, PlaybackGrant grant,
                               std::uint64_t now, std::uint64_t wall) {
    const bool valid_deadline = grant.deadline > now && (slot.source_deadline == 0 ||
                                                         grant.deadline <= slot.source_deadline);
    if (grant.pending && !grant.driver && valid_deadline && wall < invite.expires_at) {
        const auto remaining = invite.expires_at - wall;
        if (remaining <= 30000 && now <= std::numeric_limits<std::uint64_t>::max() - remaining) {
            slot.source_deadline = grant.deadline;
            if (!slot.preparing)
                slot.prepare_until = now + remaining;
            slot.preparing = true;
            return true;
        }
    } else if (!grant.pending && grant.driver && valid_deadline &&
               grant.driver->output_permitted()) {
        slot.preparing = false;
        slot.driver = std::move(grant.driver);
        slot.session = std::make_unique<PlaybackSession>(mqtt_, *slot.driver, std::move(invite),
                                                         capability_, up_, down_, grant.deadline);
        return slot.session->receive(down_, slot.invitation, now, wall);
    }
    slot.driver = std::move(grant.driver);  // 无效交付也不能析构后假报资源已释放。
    cancel_preparation(slot, now);
    return reject(invite, "userRejected", &slot);
}

// receive
// 先核对固定身份/严格邀请，再准入；已有实例只复用原绑定，不影响AI父会话。
bool PlaybackEndpoint::receive(std::string_view topic, std::string_view body, std::uint64_t now,
                               std::uint64_t wall) {
    if (topic != down_)
        return false;
    if (clock_ && now < *clock_) {
        close(now);
        return false;
    }
    clock_ = now;
    Envelope message;
    if (decode_envelope(body, message) != DecodeResult::Ok || message.session.empty())
        return false;
    // Ended后的旧控制原本就拒绝；墓碑保持相同语义，不借新邀请或新token复活已释放实例。
    for (const auto& retired : retired_)
        if (retired.session == message.session)
            return false;
    Slot* existing = nullptr;
    for (auto& slot : slots_)
        if (slot.session_id == message.session)
            existing = &slot;
    if (message.method != "sessionInvite") {
        if (!existing)
            return false;
        if (existing->session)
            return existing->session->receive(topic, body, now, wall);
        // 原准备/拒绝实例也接收终态；结束不能续权，先等待真实释放再回报原关联。
        VoiceEvent event;
        PlaybackInvite original;
        if (message.method != "sessionEnded" || !existing->terminal_token.empty() || message.code ||
            !message.event_sequence ||
            decode_voice_event(WireRoute::Signaling, body, event) != WireResult::Ok ||
            decode_playback_invite(WireRoute::Signaling, existing->invitation, original) !=
                WireResult::Ok ||
            !original.envelope.event_sequence.has_value() ||
            *message.event_sequence <= *original.envelope.event_sequence)
            return false;
        existing->terminal_token = message.token;
        existing->terminal_sequence = *message.event_sequence;
        existing->rejection_pending = false;
        if (!existing->closed_at)
            cancel_preparation(*existing, now);
        publish_preparation_release(*existing);
        return true;
    }

    PlaybackInvite invite;
    std::string canonical;
    if (decode_playback_invite(WireRoute::Signaling, body, invite) != WireResult::Ok ||
        encode_envelope(invite.envelope, canonical) != DecodeResult::Ok)
        return false;
    if (existing) {
        if (canonical != existing->invitation || !existing->terminal_token.empty())
            return false;
        if (!existing->rejection.empty()) {
            const bool sent = mqtt_.publish(up_, existing->rejection);
            existing->rejection_pending = !sent;
            return sent;
        }
        if (existing->preparing)
            return true;  // 原邀请仍在准备；不重授权、续期或提前接听。
        return existing->session && existing->session->receive(topic, body, now, wall);
    }
    if (closing_ || wall >= invite.expires_at || invite.expires_at - wall > 30000 ||
        validate_playback_outputs(capability_, invite) != WireResult::Ok)
        return false;
    Slot* available = nullptr;
    bool busy = false;
    for (auto& slot : slots_) {
        if (slot.session_id.empty() && !available)
            available = &slot;
        if (slot.preparing || slot.canceling || (slot.session && !slot.session->released()))
            busy = true;  // 逻辑Ended仍占用实际输出；不能抢先准入第二个播放器。
    }
    if (!available)
        available = retire_slot();
    if (!available)
        return false;  // 保留槽已满，不能发出无法保持幂等的拒绝后再准入同一邀请。
    available->session_id = invite.envelope.session;
    available->invitation = std::move(canonical);
    if (busy) {
        available->closed_at = now;
        return reject(invite, "resourceBusy", available);
    }
    auto grant = admission_.authorize(invite, now);
    return resolve(*available, std::move(invite), std::move(grant), now, wall);
}

// tick 保留未实际释放的驱动，固定终态满十分钟才回收近期关联。
void PlaybackEndpoint::tick(std::uint64_t now, std::uint64_t wall) {
    if (clock_ && now < *clock_)
        close(now);
    clock_ = now;
    for (auto& slot : slots_) {
        if (slot.canceling)
            cancel_preparation(slot, now);
        if (slot.preparing) {
            PlaybackInvite invite;
            if (decode_playback_invite(WireRoute::Signaling, slot.invitation, invite) !=
                WireResult::Ok) {
                cancel_preparation(slot, now);
            } else if (closing_ || now >= slot.prepare_until || now >= slot.source_deadline ||
                       wall >= invite.expires_at) {
                cancel_preparation(slot, now);
                reject(invite, "userRejected", &slot);
            } else {
                auto grant = admission_.authorize(invite, now);
                resolve(slot, std::move(invite), std::move(grant), now, wall);
            }
        }
        if (slot.rejection_pending && mqtt_.publish(up_, slot.rejection))
            slot.rejection_pending = false;
        publish_preparation_release(slot);
        if (slot.session) {
            slot.session->tick(now, wall);
            if (slot.session->released() && !slot.closed_at)
                slot.closed_at = now;
        }
        // 只有实际释放或固定拒绝才能回收；未知释放永不因保留计时而假报完成。
        const bool retained_fact_done =
            slot.session ? slot.session->release_submitted()
                         : (slot.terminal_token.empty() || slot.release_published);
        if (slot.closed_at && retained_fact_done && now >= *slot.closed_at &&
            now - *slot.closed_at >= 600000) {
            slot.session.reset();
            slot.driver.reset();
            slot = Slot{};
        }
    }
    for (auto iterator = retired_.begin(); iterator != retired_.end();) {
        if (now >= iterator->closed_at && now - iterator->closed_at >= 600000) {
            retired_bytes_ -= iterator->bytes;
            iterator = retired_.erase(iterator);
        } else {
            ++iterator;
        }
    }
}

// close 永久关闭本连接代次的新准入，仅切断文件实例，不断开共享网络。
void PlaybackEndpoint::close(std::uint64_t now) {
    closing_ = true;
    for (auto& slot : slots_)
        if (slot.session)
            slot.session->close(now);
        else if (slot.preparing || slot.canceling)
            cancel_preparation(slot, now);
}
}  // namespace ur::device
