// 文件说明：AI控制断连恢复的固定期限及原请求关联；不触碰MQTT身份、不创建模型或驱动媒体。
#include "ur/device/ai_control_recovery.hpp"
#include <algorithm>

namespace ur::device {
namespace {
constexpr std::uint64_t maximum_integer = 9007199254740991ULL;  // MQTT整数统一上界。
// identifier 防止公共结构体直接传入无限长度或控制字符，完整JSON仍由wire入口检查。
bool identifier(std::string_view value, std::size_t limit) {
    return !value.empty() && value.size() <= limit &&
           std::none_of(value.begin(), value.end(),
                        [](unsigned char ch) { return ch < 32 || ch == 127; });
}
}  // namespace

// 构造保留平台原绑定；未协商或非法媒体不会被补成初始epoch=1。
AIControlRecovery::AIControlRecovery(std::string session, AIControlBinding binding,
                                     std::uint64_t media_revision, VoiceTransport transport)
    : session_(std::move(session)),
      binding_(binding),
      media_revision_(media_revision),
      transport_(transport) {
    if (identifier(session_, 64) && binding_.endpoint_epoch > 0 &&
        binding_.endpoint_epoch <= maximum_integer && binding_.capability_revision > 0 &&
        binding_.capability_revision <= maximum_integer && media_revision_ <= maximum_integer &&
        (transport_ == VoiceTransport::None ? media_revision_ == 0 : media_revision_ > 0) &&
        (transport_ == VoiceTransport::None || transport_ == VoiceTransport::Udp ||
         transport_ == VoiceTransport::WebRTC))
        phase_ = AIControlPhase::Bound;
}

// begin 只在原Bound时固定新窗口，重复失联观察只返回Duplicate，不刷新意图或水位。
// 保留 now/event_watermark/message_watermark 接口；now 固定单调恢复窗口，两类信令水位分别记录并核安全整数范围。
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
Result AIControlRecovery::begin(std::uint64_t now, std::uint64_t event_watermark,
                                std::uint64_t message_watermark, bool wants_capture) {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    if (phase_ == AIControlPhase::Failed)
        return Result::Stale;
    if (recovering())
        return expired(now) ? Result::Stale : Result::Duplicate;
    if (event_watermark > maximum_integer || message_watermark > maximum_integer)
        return Result::Invalid;
    started_ = observed_ = now;
    event_watermark_ = event_watermark;
    message_watermark_ = message_watermark;
    wants_capture_ = wants_capture;
    token_.clear();
    report_revision_ = 0;
    phase_ = AIControlPhase::Disconnected;
    return Result::Applied;
}

// expired 只有正在恢复时检查首次窗口；Bound由正常运行时的单调时钟检查保护。
bool AIControlRecovery::expired(std::uint64_t now) {
    if (phase_ == AIControlPhase::Failed)
        return true;
    if (!recovering())
        return false;
    if (now < observed_ || now < started_ || now - started_ >= 15000) {
        phase_ = AIControlPhase::Failed;
        wants_capture_ = false;
        return true;
    }
    observed_ = now;
    return false;
}

// reporting 新物理连接必须重新确认完整能力；重连风暴仍共享原15秒，不能直接发送旧输入。
// 保留 token/capability_revision/now 接口；token 固定当前能力关联，revision 核安全范围，now 核原单调恢复窗口。
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
Result AIControlRecovery::reporting(std::string token, std::uint64_t capability_revision,
                                    std::uint64_t now) {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    if (!recovering() || expired(now))
        return Result::Stale;
    if (!identifier(token, 128) || capability_revision == 0 ||
        capability_revision > maximum_integer)
        return Result::Invalid;
    token_ = std::move(token);
    report_revision_ = capability_revision;
    phase_ = AIControlPhase::Reporting;
    return Result::Applied;
}

// capability_ack 仅当前已发送能力精确ACK可推进；异步旧ACK不能消费同步请求槽。
Result AIControlRecovery::capability_ack(std::string_view token, std::uint64_t revision,
                                         std::uint64_t now) {
    if (!recovering() || expired(now) || phase_ != AIControlPhase::Reporting || token != token_ ||
        revision != report_revision_)
        return Result::Stale;
    token_.clear();
    phase_ = AIControlPhase::ReadyToSync;
    return Result::Applied;
}

// sync 编码原绑定与真实已应用水位，平台决定新epoch；该请求不能打开驱动。
Result AIControlRecovery::sync(std::string token, std::uint64_t now, Envelope& out) {
    out = {};
    if (!recovering() || expired(now) || phase_ != AIControlPhase::ReadyToSync)
        return Result::Stale;
    if (ai_session_sync_command(token, session_, event_watermark_, message_watermark_, binding_,
                                media_revision_, transport_, out) != WireResult::Ok)
        return Result::Invalid;
    token_ = std::move(token);
    phase_ = AIControlPhase::Syncing;
    return Result::Applied;
}

// accept 双时钟和完整scope通过后才替换公开绑定；旧输入ID/回复不是重放指令。
Result AIControlRecovery::accept(const AIControlSnapshot& snapshot, std::uint64_t now,
                                 std::uint64_t wall) {
    if (!recovering() || expired(now) || phase_ != AIControlPhase::Syncing ||
        snapshot.envelope.token != token_ || snapshot.envelope.session != session_ ||
        snapshot.envelope.method != "sessionSnapshot" || snapshot.envelope.request ||
        !snapshot.envelope.code || *snapshot.envelope.code != 200 ||
        !snapshot.envelope.event_sequence || snapshot.envelope.message_sequence ||
        *snapshot.envelope.event_sequence != snapshot.event_watermark || snapshot.revision == 0 ||
        snapshot.revision > maximum_integer || snapshot.event_watermark < event_watermark_ ||
        snapshot.event_watermark > maximum_integer ||
        snapshot.message_watermark < message_watermark_ ||
        snapshot.message_watermark > maximum_integer ||
        snapshot.media_revision != media_revision_ || wall == 0 || wall > maximum_integer ||
        snapshot.expires_at <= wall || snapshot.expires_at > maximum_integer)
        return Result::Stale;
    if (snapshot.ended) {
        // 已确认终态永久关闭恢复；不借更高epoch或历史resumeAllowed复活原模型会话。
        phase_ = AIControlPhase::Failed;
        wants_capture_ = false;
        return Result::Unsupported;
    }
    if (!snapshot.resume_allowed || snapshot.input_active ||
        binding_.endpoint_epoch >= maximum_integer ||
        snapshot.binding.endpoint_epoch != binding_.endpoint_epoch + 1 ||
        snapshot.binding.capability_revision != report_revision_)
        return Result::Stale;
    auto retired = retired_responses_;  // 整批核上限后替换，不能在半份非法快照时修改原墓碑。
    for (const auto& response : snapshot.active_responses) {
        if (!identifier(response, 128))
            return Result::Stale;
        if (std::find(retired.begin(), retired.end(), response) == retired.end())
            retired.push_back(response);
        if (retired.size() > 512) {
            phase_ = AIControlPhase::Failed;
            wants_capture_ = false;
            return Result::Busy;
        }
    }
    retired_responses_ = std::move(retired);
    binding_ = snapshot.binding;
    event_watermark_ = snapshot.event_watermark;
    message_watermark_ = snapshot.message_watermark;
    token_.clear();
    phase_ = AIControlPhase::Bound;
    return Result::Applied;
}

// retired_response 只有原完整快照确认的同会话ID才命中，不随重连清除。
bool AIControlRecovery::retired_response(std::string_view response) const {
    return std::find(retired_responses_.begin(), retired_responses_.end(), response) !=
           retired_responses_.end();
}

// advance_media_revision 原Peer恢复已获ACK后同步本地已知代次，后续控制同步不得借旧媒体revision。
Result AIControlRecovery::advance_media_revision(std::uint64_t revision) {
    if (phase_ != AIControlPhase::Bound || transport_ != VoiceTransport::WebRTC)
        return Result::Stale;
    if (revision == media_revision_)
        return Result::Duplicate;
    if (media_revision_ >= maximum_integer || revision != media_revision_ + 1)
        return Result::Invalid;
    media_revision_ = revision;
    return Result::Applied;
}
}  // namespace ur::device
