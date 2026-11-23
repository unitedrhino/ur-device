// 文件说明：实际浏览器驱动适配共享端点所有权；不新增设备报文、下载源或音视频采集。
#include "browser_playback_admission.hpp"

namespace ur::device {
namespace {
// matches 确保完整输出与原内容绑定完全相同，不能按音频子集降级视频或提升采集权限。
bool matches(const BrowserFileBinding& binding, const PlaybackInvite& invite) {
    if (invite.envelope.session != binding.session || invite.playback_id != binding.playback_id ||
        invite.content_ref != binding.content_ref)
        return false;
    const bool video = binding.mime == "video/mp4" || binding.mime == "video/webm";
    bool expected_audio = false, expected_video = false;
    for (const auto& modality : invite.output_modalities) {
        if (modality == "audio" && !expected_audio)
            expected_audio = true;
        else if (modality == "video" && !expected_video)
            expected_video = true;
        else
            return false;
    }
    return expected_audio == binding.audio && expected_video == video;
}
// FileLease 保持端点会话引用期间实际驱动有效；媒体事实和执行操作始终交给同一实例。
class FileLease final : public PlaybackDriver {
public:
    explicit FileLease(std::shared_ptr<BrowserFileMedia> media) : media_(std::move(media)) {}
    // begin 仅转交已经过共享核心关联检查的控制，不重新选源或分配输出。
    bool begin(const PlaybackCommand& command, std::uint64_t generation) override {
        return media_->begin(command, generation);
    }
    // take_fact 读取真实驱动队列，不把初始化或网络确认作为播放事实。
    std::optional<PlaybackFact> take_fact() override { return media_->take_fact(); }
    // output_permitted 使用原整组资源许可与内容截止，不提供默认true。
    bool output_permitted() const override { return media_->output_permitted(); }
    // stop_all 等待浏览器实际关闭事实，不把命令受理当释放完成。
    bool stop_all() override { return media_->stop_all(); }

private:
    std::shared_ptr<BrowserFileMedia> media_;  // 与宿主事件泵共持原固定驱动。
};
}  // namespace

// BrowserPlaybackAdmission 先构造固定媒体绑定；不会在构造或准入时执行load/play。
BrowserPlaybackAdmission::BrowserPlaybackAdmission(BrowserFileBinding binding,
                                                   BrowserFilePermission permission)
    : binding_(std::move(binding)),
      media_(std::make_shared<BrowserFileMedia>(binding_, std::move(permission))) {}

// authorize 匹配完整音视频输出，不以摄像头能力或邀请截止替代文件授权。
PlaybackGrant BrowserPlaybackAdmission::authorize(const PlaybackInvite& invite, std::uint64_t now) {
    if (issued_ || now >= binding_.expires_at || !media_->tick(now) ||
        !media_->preparation_permitted() || !matches(binding_, invite))
        return {};
    if (!media_->ready())
        return {nullptr, binding_.expires_at,
                true};  // 固定原准备；核心轮询期间不能另建或刷新期限。
    issued_ = true;
    return {std::make_unique<FileLease>(media_), binding_.expires_at};
}

// cancel 按固定实例关闭准备资源；不停止AI或其他节目，不把请求受理当释放完成。
bool BrowserPlaybackAdmission::cancel(std::string_view session, std::uint64_t now) {
    if (session != binding_.session)
        return true;  // 此固定适配从未为其他实例准备资源，不能关闭当前合法实例。
    media_->tick(now);
    return media_->stop_all();
}

// BrowserResolvedPlaybackAdmission 构造时不创建浏览器资源或声明文件能力。
BrowserResolvedPlaybackAdmission::BrowserResolvedPlaybackAdmission(BrowserFileResolver resolver)
    : resolver_(std::move(resolver)) {}

// active_media 提供当前原实例事件泵，不把尚未解析的邀请当作已准备资源。
BrowserFileMedia* BrowserResolvedPlaybackAdmission::active_media() {
    return active_ ? &active_->media() : nullptr;
}

// advance 保留跨会话单调栅栏；回退时关闭原实例，但实际释放仍须由宿主继续泵回执。
bool BrowserResolvedPlaybackAdmission::advance(std::uint64_t now) {
    if (clock_ && now < *clock_) {
        fenced_ = true;
        if (active_)
            active_->cancel(session_, now);
    }
    if (!fenced_)
        clock_ = now;
    return !fenced_;
}

// authorize 先守住原资源与身份，再解析一次独立许可；原准备重复不调用解析器或延长截止。
PlaybackGrant BrowserResolvedPlaybackAdmission::authorize(const PlaybackInvite& invite,
                                                          std::uint64_t now) {
    if (!advance(now) || !resolver_)
        return {};
    if (active_ && invite.envelope.session == session_)
        return active_->authorize(invite, now);
    if (active_ && !active_->media().quiescent())
        return {};
    auto resolved = resolver_(invite, now);
    if (!resolved || !resolved->permission || resolved->binding.expires_at <= now ||
        !matches(resolved->binding, invite))
        return {};
    const bool video =
        resolved->binding.mime == "video/mp4" || resolved->binding.mime == "video/webm";
    if (!resolved->permission(resolved->binding.audio, video, now))
        return {};
    auto prepared = std::make_unique<BrowserPlaybackAdmission>(std::move(resolved->binding),
                                                               std::move(resolved->permission));
    auto grant = prepared->authorize(invite, now);
    // 构造或源校验失败不得交出准备实例；只有真实驱动待就绪才留下唯一事件泵。
    if (!grant.pending && !grant.driver)
        return {};
    active_ = std::move(prepared);
    session_ = invite.envelope.session;
    return grant;
}

// cancel 即使时钟故障也继续关闭原资源；不以永久栅栏或空解析结果假报已有资源释放。
bool BrowserResolvedPlaybackAdmission::cancel(std::string_view session, std::uint64_t now) {
    advance(now);
    return !active_ || session != session_ || active_->cancel(session, now);
}
}  // namespace ur::device
