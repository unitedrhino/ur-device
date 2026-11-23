// 固定授权文件子会话协调器；宿主先完成内容/输出资源绑定，本层不授予下载或焦点权限。
#pragma once
#include <memory>
#include "ur/device/playback_engine.hpp"
#include "ur/device/requests.hpp"

namespace ur::device {
// PlaybackPhase 分开接听、平台建立和实际播放，不把MQTT确认等同媒体完成。
enum class PlaybackPhase { Prepared, Accepting, Active, Ended };
// PlaybackSession 仅持有一个宿主已授权的固定邀请/驱动，不负责选择内容或分配其它设备资源。
// 唯一事件线程驱动；deadline是宿主授权的单调期限，不能取邀请墙钟截止充当播放授权。
class PlaybackSession {
public:
    PlaybackSession(PublishPort& mqtt, PlaybackDriver& driver, PlaybackInvite binding,
                    PlaybackCapability capability, std::string up, std::string down,
                    std::uint64_t deadline);
    ~PlaybackSession();
    PlaybackSession(const PlaybackSession&) = delete;
    PlaybackSession& operator=(const PlaybackSession&) = delete;
    // receive 只接精确Topic和固定邀请；wall用于邀请期限，now用于本地生命周期。
    bool receive(std::string_view topic, std::string_view body, std::uint64_t now,
                 std::uint64_t wall);
    // tick 处理原文重试、授权/时钟失效及实际驱动事实，发布背压时保留原回报。
    void tick(std::uint64_t now, std::uint64_t wall);
    // close 切断本实例并取消等待；不清理其他AI/IPC输出，实际释放由驱动另行确认。
    void close(std::uint64_t now);
    PlaybackPhase phase() const { return phase_; }  // 逻辑阶段，不是实际播放器状态。
    // released 仅在关闭后实际驱动确认整组输出释放时为true；宿主必须继续tick到确认或明确记录未知。
    bool released() const { return released_; }
    // release_submitted 原平台终态及实际关闭后，本地发布端口已接纳事实；不是Core持久化确认。
    bool release_submitted() const {
        return released_ && release_published_ && !pending_fact_ &&
               (!engine_ || engine_->replies_drained());
    }
    // retirement_release 仅完整交付所有原事实后导出固定释放字节；不携带驱动或可恢复许可。
    std::optional<std::string> retirement_release() const;

private:
    bool observe(std::uint64_t now);  // 倒退或本地授权截止立即切断。
    void activate();                  // 必须同时有原接听ACK和独立平台建立事件。
    void publish_release();           // 原终态与实际释放齐备才发布独立事实，背压保留原字节。
    // publish_pending 每轮最多交付一条原事实；结束后同样保留背压和固定原关联。
    bool publish_pending();
    PublishPort& mqtt_;                        // 实际MQTT端口，发布成功仍不是设备执行确认。
    PlaybackDriver& driver_;                   // 宿主固定内容/输出许可驱动，生命周期长于本对象。
    PlaybackInvite binding_;                   // 不可变授权邀请，不接受模型替换内容引用。
    PlaybackCapability capability_;            // 真实文件输出能力。
    Requests requests_;                        // 只关联原邀请接听，不把媒体事实注册成请求。
    std::string up_, down_;                    // 精确鉴权Topic，不能由媒体控制正文指定。
    std::uint64_t deadline_;                   // 独立本地资源授权的单调截止。
    std::uint64_t sequence_ = 0;               // 已确认的信令水位，迟到结束不能覆盖后续控制。
    std::optional<std::uint64_t> clock_;       // 同一事件线程的最后单调时间。
    std::unique_ptr<PlaybackEngine> engine_;   // 只在邀请接纳后构建，不启动采集。
    std::optional<std::string> pending_fact_;  // MQTT背压时保留唯一原事实字节。
    std::string terminal_token_;  // 首次成功接收的原平台终态，不能由驱动或本地关闭猜测。
    std::optional<std::string> release_fact_;  // 独立整组释放事实，编码后不因重试改变token或水位。
    PlaybackPhase phase_ = PlaybackPhase::Prepared;  // 未确认资源前不能执行控制。
    bool acknowledged_ = false;                      // 原token接听ACK已经核验。
    bool established_ = false;                       // 独立平台建立事实已经核验。
    bool released_ = false;                          // 独立实际释放确认，不从逻辑Ended或断连推断。
    bool release_published_ = false;                 // 仅表示本地发布接纳，不是Core持久化确认。
};
}  // namespace ur::device
